#include <QtTest>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include "manifestFile.hpp"

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    PluginManifest Manifest()
    {
        PluginManifest Value; Value.id_ = "hc.test.wcx"; Value.version_ = "1.0.0";
        Value.binary_ = "sample.wcx64"; Value.sha256_ = std::string(64, 'a');
        Value.extensions_ = {Extension::ArchiveRead}; Value.permissions_ = {Permission::ReadContent}; return Value;
    }
    QByteArray Pe(std::uint16_t Machine = 0x8664, std::uint16_t Magic = 0x20b)
    {
        QByteArray Bytes(512, '\0');
        const auto Put16 = [&](int At, std::uint16_t Value) { Bytes[At] = static_cast<char>(Value); Bytes[At + 1] = static_cast<char>(Value >> 8); };
        Put16(0, 0x5a4d); Put16(0x3c, 64); Put16(64, 0x4550); Put16(68, Machine);
        Put16(70, 1); Put16(84, 240); Put16(86, 0x2002); Put16(88, Magic); return Bytes;
    }
    auto Inspect(const QByteArray& Bytes) { return InspectPe(std::span(reinterpret_cast<const std::byte*>(Bytes.constData()), Bytes.size())); }
    bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
}
class PluginManifestTests : public QObject
{
    Q_OBJECT
private slots:
    void formatRoundTrip_data()
    {
        QTest::addColumn<int>("Format"); QTest::addColumn<int>("Capability");
        QTest::newRow("wcx") << int(PluginFormat::Wcx) << int(Extension::ArchiveRead);
        QTest::newRow("wlx") << int(PluginFormat::Wlx) << int(Extension::Viewer);
        QTest::newRow("wfx") << int(PluginFormat::Wfx) << int(Extension::StorageRead);
        QTest::newRow("wdx") << int(PluginFormat::Wdx) << int(Extension::Metadata);
    }
    void formatRoundTrip()
    {
        QFETCH(int, Format); QFETCH(int, Capability);
        auto Value = Manifest(); Value.format_ = PluginFormat(Format); Value.extensions_ = {Extension(Capability)};
        if (Value.format_ == PluginFormat::Wlx) Value.permissions_.insert(Permission::UserInterface);
        const auto Encoded = EncodeManifest(Value); auto Parsed = ParseManifest(Encoded); QVERIFY(Parsed.HasValue());
        QCOMPARE(EncodeManifest(Parsed.Value()), Encoded);
        Parsed.Value().extensions_.insert(Value.format_ == PluginFormat::Wcx ? Extension::Viewer : Extension::ArchiveWrite);
        QVERIFY(!Validate(Parsed.Value()).HasValue());
    }
    void malformedJson()
    {
        const auto Valid = EncodeManifest(Manifest());
        for (const auto Bad : {QByteArray{}, QByteArray("[]"), Valid.left(Valid.size() / 2), QByteArray(65537, ' ')}) QVERIFY(!ParseManifest(Bad).HasValue());
        const auto Change = [&](const QString& Key, QJsonValue Value) {
            auto Object = QJsonDocument::fromJson(Valid).object(); Object[Key] = Value; return ParseManifest(QJsonDocument(Object).toJson());
        };
        for (const auto Value : {QJsonValue(1.5), QJsonValue(-1), QJsonValue("1"), QJsonValue(4294967296.0), QJsonValue(true)}) QVERIFY(!Change("schemaVersion", Value).HasValue());
        QVERIFY(!Change("trusted", true).HasValue()); QVERIFY(!Change("format", "WCX").HasValue()); QVERIFY(!Change("permissions", QJsonArray{"content.read", "content.read"}).HasValue());
        QVERIFY(!Change("permissions", QJsonArray{"admin"}).HasValue()); QVERIFY(!Change("extensions", QJsonArray{}).HasValue());
        auto Duplicate = Valid; Duplicate.insert(Duplicate.indexOf('{') + 1, "\"id\":\"other\",");
        QCOMPARE(ParseManifest(Duplicate).Failure().code_, std::string("plugin-manifest-duplicate-key"));
        Duplicate = Valid; Duplicate.insert(Duplicate.indexOf('{') + 1, "\"\\u0069d\":\"other\",");
        QCOMPARE(ParseManifest(Duplicate).Failure().code_, std::string("plugin-manifest-duplicate-key"));
        Duplicate = Valid; Duplicate.replace("\"major\": 1", "\"major\": 1, \"major\": 2");
        QCOMPARE(ParseManifest(Duplicate).Failure().code_, std::string("plugin-manifest-duplicate-key"));
    }
    void pathAndPermissionValidation()
    {
        for (const auto Path : {"../x.wcx64", "/x.wcx64", "C:/x.wcx64", "x\\x.wcx64", "x//x.wcx64", "x/./x.wcx64", "x/../x.wcx64", "NUL.wcx64", "COM1.wcx64", "x /x.wcx64", "x.wcx64 ", "x.wcx64:stream", "x?.wcx64"})
        { auto Value = Manifest(); Value.binary_ = Path; QVERIFY2(!Validate(Value).HasValue(), Path); }
        auto Value = Manifest(); Value.binary_ = "하위/검증.wcx64"; QVERIFY(Validate(Value).HasValue());
        Value.binary_ = std::string("x\0.wcx64", 8); QVERIFY(!Validate(Value).HasValue());
        Value = Manifest(); Value.extensions_.insert(Extension::ArchiveWrite); QVERIFY(!Validate(Value).HasValue());
        Value.permissions_.insert(Permission::WriteContent); QVERIFY(Validate(Value).HasValue());
        Value.permissions_.insert(Permission(999)); QVERIFY(!Validate(Value).HasValue());
        Value = Manifest(); Value.sha256_[0] = 'A'; QVERIFY(!Validate(Value).HasValue());
    }
    void versionNegotiation()
    {
        const auto Matched = Negotiate({1, 2, 5}, {1, 3, 9}); QVERIFY(Matched.HasValue()); QCOMPARE(Matched.Value(), (ProtocolVersion{1, 5}));
        QVERIFY(!Negotiate({2, 0, 0}, {1, 0, 9}).HasValue()); QVERIFY(!Negotiate({1, 2, 5}, {1, 6, 7}).HasValue());
        QVERIFY(!Negotiate({1, 5, 2}, {1, 0, 9}).HasValue()); QVERIFY(!Negotiate({0, 0, 0}, {0, 0, 0}).HasValue());
        QCOMPARE(Negotiate({1, UINT32_MAX, UINT32_MAX}, {1, 0, UINT32_MAX}).Value().minor_, UINT32_MAX);
    }
    void admissionDoesNotTrustManifest()
    {
        auto Value = Manifest(); PluginApproval Approval;
        HostProfile Host; Host.formats_ = {PluginFormat::Wcx}; Host.extensions_ = {Extension::ArchiveRead}; Host.supportsIsolation_ = true;
        QVERIFY(!Admit(Value, Approval, Host).HasValue());
        Approval.reviewed_ = Value; Approval.approved_ = true;
        QCOMPARE(Admit(Value, Approval, Host).Failure().code_, std::string("plugin-permission-denied"));
        Approval.granted_ = Value.permissions_; QVERIFY(Admit(Value, Approval, Host).HasValue());
        auto Changed = Value; Changed.sha256_[0] = 'b'; QVERIFY(!Admit(Changed, Approval, Host).HasValue());
        Changed = Value; Changed.permissions_.insert(Permission::Network); Approval.granted_.insert(Permission::Network); QVERIFY(!Admit(Changed, Approval, Host).HasValue());
        Changed = Value; Changed.protocol_.maximumMinor_ = 1; QVERIFY(!Admit(Changed, Approval, Host).HasValue());
        Host.architecture_ = Architecture::X86; QVERIFY(!Admit(Value, Approval, Host).HasValue()); Host.architecture_ = Architecture::X64;
        Host.supportsIsolation_ = false; QVERIFY(!Admit(Value, Approval, Host).HasValue());
        Value.execution_ = ExecutionMode::TrustedInProcess; Approval.reviewed_ = Value; Host.supportsInProcess_ = true;
        QVERIFY(!Admit(Value, Approval, Host).HasValue()); Approval.allowInProcess_ = true; QVERIFY(Admit(Value, Approval, Host).HasValue());
        Host.extensions_.clear(); QVERIFY(!Admit(Value, Approval, Host).HasValue());
    }
    void helperApprovalAndPeKinds()
    {
        auto Value = Manifest(); Value.compatibility_ = "total7zip";
        Value.extensions_.insert(Extension::ArchiveWrite); Value.permissions_.insert(Permission::WriteContent);
        QVERIFY(!Validate(Value).HasValue());
        Value.helpers_["64/7zG.exe"] = std::string(64, 'b'); Value.dependencies_["64/7z.dll"] = std::string(64, 'c');
        Value.permissions_.insert(Permission::ChildProcess); Value.permissions_.insert(Permission::UserInterface);
        QVERIFY(Validate(Value).HasValue()); auto Parsed = ParseManifest(EncodeManifest(Value));
        QVERIFY(Parsed.HasValue()); QVERIFY(Parsed.Value() == Value);
        PluginApproval Approval{Value, true, false, Value.permissions_}; HostProfile Host;
        Host.formats_ = {PluginFormat::Wcx}; Host.extensions_ = Value.extensions_; Host.supportsIsolation_ = true;
        QVERIFY(Admit(Value, Approval, Host).HasValue());
        auto Changed = Value; Changed.helpers_.begin()->second[0] = 'd'; QVERIFY(!Admit(Changed, Approval, Host).HasValue());
        Approval.granted_.erase(Permission::ChildProcess); QVERIFY(!Admit(Value, Approval, Host).HasValue());
        Changed = Value; Changed.permissions_.erase(Permission::ChildProcess); QVERIFY(!Validate(Changed).HasValue());
        Changed = Value; Changed.execution_ = ExecutionMode::TrustedInProcess; QVERIFY(!Validate(Changed).HasValue());
        Changed = Value; Changed.helpers_["../other.exe"] = std::string(64, 'a'); QVERIFY(!Validate(Changed).HasValue());
        auto Dll = Pe(); auto Exe = Dll; Exe[87] = '\0';
        auto Span = [](const QByteArray& Bytes) { return std::span(reinterpret_cast<const std::byte*>(Bytes.constData()), Bytes.size()); };
        QVERIFY(InspectPe(Span(Exe), true).HasValue()); QVERIFY(!InspectPe(Span(Dll), true).HasValue()); QVERIFY(!InspectPe(Span(Exe)).HasValue());
        QTemporaryDir Directory; QVERIFY(QDir().mkpath(Directory.filePath("64")));
        QVERIFY(Put(Directory.filePath(Value.binary_.c_str()), Dll)); QVERIFY(Put(Directory.filePath("64/7z.dll"), Dll)); QVERIFY(Put(Directory.filePath("64/7zG.exe"), Exe));
        Value.sha256_ = QCryptographicHash::hash(Dll, QCryptographicHash::Sha256).toHex().toStdString();
        Value.dependencies_["64/7z.dll"] = Value.sha256_;
        Value.helpers_["64/7zG.exe"] = QCryptographicHash::hash(Exe, QCryptographicHash::Sha256).toHex().toStdString();
        const auto Path = Directory.filePath("plugin.json"); QVERIFY(Put(Path, EncodeManifest(Value))); QVERIFY(InspectManifestFile(Path).HasValue());
        QVERIFY(Put(Directory.filePath("64/other.dll"), Dll)); QCOMPARE(InspectManifestFile(Path).Failure().code_, std::string("plugin-unreviewed-helper-dll"));
        QVERIFY(QFile::remove(Directory.filePath("64/other.dll")));
        QVERIFY(Put(Directory.filePath("64/7zG.exe"), Exe + "changed")); QCOMPARE(InspectManifestFile(Path).Failure().code_, std::string("plugin-binary-hash-mismatch"));
        QVERIFY(QFile::remove(Directory.filePath("64/7zG.exe"))); QVERIFY(!InspectManifestFile(Path).HasValue());
    }
    void peHeaderBounds()
    {
        const auto Bytes = Pe(); QCOMPARE(Inspect(Bytes).Value(), Architecture::X64);
        QCOMPARE(Inspect(Pe(0x14c, 0x10b)).Value(), Architecture::X86); QCOMPARE(Inspect(Pe(0xaa64)).Value(), Architecture::Arm64);
        for (int Length = 0; Length < 368; ++Length) QVERIFY(!Inspect(Bytes.left(Length)).HasValue());
        auto Broken = Bytes; Broken[0x3c] = '\xff'; Broken[0x3d] = '\xff'; Broken[0x3e] = '\xff'; Broken[0x3f] = '\xff'; QVERIFY(!Inspect(Broken).HasValue());
        Broken = Bytes; Broken[87] = '\0'; QVERIFY(!Inspect(Broken).HasValue());
        Broken = Bytes; Broken[70] = char(97); QVERIFY(!Inspect(Broken).HasValue());
        QVERIFY(!Inspect(Pe(0x8664, 0x10b)).HasValue()); QVERIFY(!Inspect(Pe(0x1234)).HasValue());
    }
    void actualFixtureAndCli()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(QDir(Directory.path()).mkdir("하위"));
        const auto Binary = Directory.filePath("하위/검증.wcx64");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64", Binary));
        QFile Input(Binary); QVERIFY(Input.open(QIODevice::ReadOnly)); const auto Bytes = Input.readAll(); Input.close();
        auto Value = Manifest(); Value.binary_ = "하위/검증.wcx64"; Value.sha256_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex().toStdString();
        const auto Path = Directory.filePath("plugin.json"); QVERIFY(Put(Path, EncodeManifest(Value)));
        const auto Checked = InspectManifestFile(Path); QVERIFY2(Checked.HasValue(), Checked.HasValue() ? "" : Checked.Failure().code_.c_str());
        QCOMPARE(Checked.Value().binaryPath_, QFileInfo(Binary).canonicalFilePath());
        QProcess Tool; Tool.start(QCoreApplication::applicationDirPath() + "/hc-plugin-inspect.exe", {Path}); QVERIFY(Tool.waitForFinished(5000)); QCOMPARE(Tool.exitCode(), 0);
        const auto Report = QJsonDocument::fromJson(Tool.readAllStandardOutput()).object(); QCOMPARE(Report["loaded"].toBool(true), false); QCOMPARE(Report["authorized"].toBool(true), false);
        QCOMPARE(Report["sha256"].toString().toStdString(), Value.sha256_);
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        QCOMPARE(InspectManifestFile(Path, Context).Failure().category_, ErrorCategory::Cancelled);
        Context = {}; Context.deadline_ = std::chrono::steady_clock::now(); QVERIFY(!InspectManifestFile(Path, Context).HasValue());
        Value.architecture_ = Architecture::X86; QVERIFY(Put(Path, EncodeManifest(Value)));
        QCOMPARE(InspectManifestFile(Path).Failure().code_, std::string("plugin-binary-architecture-mismatch"));
        Value.architecture_ = Architecture::X64; QVERIFY(Put(Path, EncodeManifest(Value))); QVERIFY(Put(Binary, Bytes + "changed"));
        QCOMPARE(InspectManifestFile(Path).Failure().code_, std::string("plugin-binary-hash-mismatch"));
        Tool.start(QCoreApplication::applicationDirPath() + "/hc-plugin-inspect.exe", {Path}); QVERIFY(Tool.waitForFinished(5000)); QCOMPARE(Tool.exitCode(), 1);
        QVERIFY(Tool.readAllStandardError().contains("plugin-binary-hash-mismatch"));
    }
};
QTEST_GUILESS_MAIN(PluginManifestTests)
#include "tstPluginManifest.moc"
