#include <QtTest>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include "manifestFile.hpp"
#include "commonLibs/plugins/archiveCreation.hpp"

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
    void archiveCreationSeparatesReadFormats()
    {
        auto Plugin = Manifest(); Plugin.format_ = PluginFormat::SevenZip;
        Plugin.extensions_.insert(Extension::ArchiveWrite); Plugin.permissions_.insert(Permission::WriteContent);
        ArchiveCreatorDescriptor Creator{"builtin.7zip", "7-Zip", ArchiveCreatorKind::BuiltinSevenZip,
            {Plugin.sha256_, Plugin}, ArchiveConfigurationKind::BuiltinDialog,
            {{"7z", "7z", ArchiveSettingsKind::SevenZip, ArchiveCreationEvidence::BuiltinAdapter},
             {"zip", "zip", ArchiveSettingsKind::SevenZip, ArchiveCreationEvidence::BuiltinAdapter}}};
        QVERIFY(ValidateArchiveCreator(Creator).HasValue());
        QVERIFY(ValidateArchiveCreateSelection(Creator, Creator.identity_, "7z").HasValue());
        QVERIFY(ValidateArchiveCreateSelection(Creator, Creator.identity_, "zip").HasValue());
        // A readable format and a declared write extension do not prove creation.
        QCOMPARE(ValidateArchiveCreateSelection(Creator, Creator.identity_, "rar").Failure().category_, ErrorCategory::Unsupported);
        Creator.formats_.push_back({"rar", "rar", ArchiveSettingsKind::SevenZip, ArchiveCreationEvidence::BuiltinAdapter});
        QCOMPARE(ValidateArchiveCreator(Creator).Failure().code_, std::string("archive-create-format-unsupported"));
    }
    void archiveCreationRequiresFormatEvidence()
    {
        auto Plugin = Manifest(); Plugin.extensions_.insert(Extension::ArchiveWrite);
        Plugin.permissions_.insert(Permission::WriteContent);
        ArchiveCreatorDescriptor Creator{"test.wcx", "WCX", ArchiveCreatorKind::Wcx,
            {Plugin.sha256_, Plugin}, ArchiveConfigurationKind::None, {}};
        QVERIFY(ValidateArchiveCreator(Creator).HasValue());
        QVERIFY(!ValidateArchiveCreateSelection(Creator, Creator.identity_, "zip").HasValue());
        Creator.formats_.push_back({"zip", "zip"});
        QCOMPARE(ValidateArchiveCreator(Creator).Failure().code_, std::string("archive-create-format-unverified"));
        Creator.formats_.front().evidence_ = ArchiveCreationEvidence::VerifiedPackage;
        QVERIFY(ValidateArchiveCreateSelection(Creator, Creator.identity_, "zip").HasValue());
        Creator.formats_.front().settings_ = ArchiveSettingsKind::SevenZip;
        QCOMPARE(ValidateArchiveCreator(Creator).Failure().code_, std::string("archive-create-settings-kind-mismatch"));
        Creator.formats_.front().settings_ = ArchiveSettingsKind::External;
        Creator.formats_.front().optionsVersion_ = 1;
        QVERIFY(!ValidateArchiveCreator(Creator).HasValue());
    }
    void archiveCreationBindsWholePackageIdentity()
    {
        auto Plugin = Manifest(); Plugin.extensions_.insert(Extension::ArchiveWrite);
        Plugin.permissions_.insert(Permission::WriteContent);
        Plugin.dependencies_["codec.dll"] = std::string(64, 'b');
        ArchiveCreatorDescriptor Creator{"test.wcx", "WCX", ArchiveCreatorKind::Wcx,
            {Plugin.sha256_, Plugin}, ArchiveConfigurationKind::None,
            {{"zip", "zip", ArchiveSettingsKind::External, ArchiveCreationEvidence::VerifiedPackage}}};
        const auto Original = Creator.identity_;
        auto Changed = Original; Changed.plugin_->dependencies_["codec.dll"] = std::string(64, 'c');
        QCOMPARE(ValidateArchiveCreateSelection(Creator, Changed, "zip").Failure().category_, ErrorCategory::StaleRevision);
        Changed = Original; Changed.plugin_->permissions_.insert(Permission::UserInterface);
        QVERIFY(!ValidateArchiveCreateSelection(Creator, Changed, "zip").HasValue());
        Changed = Original; Changed.plugin_->helpers_["helper.exe"] = std::string(64, 'd');
        QVERIFY(!ValidateArchiveCreateSelection(Creator, Changed, "zip").HasValue());
        Changed = Original; Changed.engineSha256_ = std::string(64, 'e');
        QVERIFY(!ValidateArchiveCreateSelection(Creator, Changed, "zip").HasValue());
        QVERIFY(ValidateArchiveCreateSelection(Creator, Original, "zip").HasValue());
    }
    void archiveCreationDopusUsesStorageDeclaration()
    {
        auto Plugin = Manifest(); Plugin.format_ = PluginFormat::DopusVfs;
        Plugin.extensions_ = {Extension::StorageRead, Extension::StorageWrite};
        Plugin.permissions_.insert(Permission::WriteContent);
        ArchiveCreatorDescriptor Creator{"test.dopus", "DOpus", ArchiveCreatorKind::DopusVfs,
            {Plugin.sha256_, Plugin}, ArchiveConfigurationKind::None, {}};
        // Generic ADD/storage-write support is insufficient for new archives.
        QVERIFY(!ValidateArchiveCreateSelection(Creator, Creator.identity_, "zip").HasValue());
        Creator.formats_.push_back({"zip", "zip", ArchiveSettingsKind::External, ArchiveCreationEvidence::VerifiedPackage});
        QVERIFY(ValidateArchiveCreateSelection(Creator, Creator.identity_, "zip").HasValue());
        Creator.configuration_ = ArchiveConfigurationKind::PluginDialog;
        QCOMPARE(ValidateArchiveCreator(Creator).Failure().code_, std::string("archive-creator-ui-not-declared"));
        Creator.identity_.plugin_->permissions_.insert(Permission::UserInterface);
        QVERIFY(ValidateArchiveCreator(Creator).HasValue());
    }
    void archiveCreationIsoAndSnapshotContract()
    {
        ArchiveCreatorDescriptor Creator{"builtin.iso", "ISO", ArchiveCreatorKind::BuiltinIso,
            {std::string(64, 'a'), {}}, ArchiveConfigurationKind::BuiltinDialog,
            {{"iso", "iso", ArchiveSettingsKind::Iso, ArchiveCreationEvidence::BuiltinAdapter}}};
        QVERIFY(ValidateArchiveCreateSelection(Creator, Creator.identity_, "iso").HasValue());
        ArchiveCreateRequest Request; Request.creator_ = Creator; Request.format_ = "iso";
        Request.schedule_ = ArchiveCreateSchedule::Queued;
        IsoCreateSettings Settings; Settings.shareDuplicateContents_ = true; Request.settings_ = Settings;
        const auto Accepted = Request;
        std::get<IsoCreateSettings>(Request.settings_).volumeLabel_ = "CHANGED";
        Request.creator_.identity_.engineSha256_ = std::string(64, 'b');
        QCOMPARE(std::get<IsoCreateSettings>(Accepted.settings_).volumeLabel_, std::string("HYPERCOMMANDER"));
        QVERIFY(std::get<IsoCreateSettings>(Accepted.settings_).shareDuplicateContents_);
        QCOMPARE(Accepted.creator_.identity_, Creator.identity_);
        // Invalid dormant root-omission state never affects combined/flat output.
        ArchiveCompositionOptions Composition; Composition.omitSelectedFolderRoot_ = true;
        QVERIFY(!Composition.OmitSelectedFolderRoot()); Composition.separateArchives_ = true;
        QVERIFY(Composition.OmitSelectedFolderRoot()); Composition.preservePaths_ = false;
        QVERIFY(!Composition.OmitSelectedFolderRoot());
    }
    void archiveCreationRejectsMalformedCatalog()
    {
        ArchiveCreatorDescriptor Creator{"builtin.iso", "ISO", ArchiveCreatorKind::BuiltinIso,
            {std::string(64, 'a'), {}}, ArchiveConfigurationKind::BuiltinDialog,
            {{"iso", "iso", ArchiveSettingsKind::Iso, ArchiveCreationEvidence::BuiltinAdapter}}};
        Creator.formats_.push_back(Creator.formats_.front()); QVERIFY(!ValidateArchiveCreator(Creator).HasValue());
        Creator.formats_.pop_back(); Creator.formats_.front().extension_ = "../iso";
        QVERIFY(!ValidateArchiveCreator(Creator).HasValue());
        Creator.formats_.front().extension_ = "iso"; Creator.identity_.engineSha256_ = "unbound";
        QVERIFY(!ValidateArchiveCreator(Creator).HasValue());
    }
    void total7zipGeneratedPermissions()
    {
        QTemporaryDir Package; const auto Binary = Package.filePath("Total7zip.wcx64"); QVERIFY(Put(Binary, Pe()));
        auto ReadOnly = Manifest(); ReadOnly.binary_ = "Total7zip.wcx64"; ReadOnly.compatibility_ = "total7zip";
        const auto Read = InspectPluginBinary(Binary, PluginFormat::Wcx, ReadOnly); QVERIFY(Read.HasValue());
        QVERIFY(Read.Value().manifest_.helpers_.empty()); QVERIFY(!Read.Value().manifest_.permissions_.contains(Permission::WriteContent));
        QVERIFY(!InspectPluginBinary(Binary, PluginFormat::Wcx).HasValue());
        QVERIFY(QDir().mkpath(Package.filePath("64"))); auto Exe = Pe(); Exe[87] = '\0';
        QVERIFY(Put(Package.filePath("64/7z.dll"), Pe())); QVERIFY(Put(Package.filePath("64/7zG.exe"), Exe));
          QVERIFY(Put(Package.filePath("7z.dll"), Pe(0x14c, 0x10b)));
          const auto Write = InspectPluginBinary(Binary, PluginFormat::Wcx); QVERIFY(Write.HasValue());
        QVERIFY(ArchiveWritePolicy(Write.Value().manifest_).HasValue()); QVERIFY(Write.Value().manifest_.permissions_.contains(Permission::ChildProcess));
    }
    void generatedFormats_data()
    {
        QTest::addColumn<int>("Format");
        QTest::newRow("wcx") << int(PluginFormat::Wcx); QTest::newRow("wlx") << int(PluginFormat::Wlx);
        QTest::newRow("wfx") << int(PluginFormat::Wfx); QTest::newRow("wdx") << int(PluginFormat::Wdx);
        QTest::newRow("dopus-viewer") << int(PluginFormat::DopusViewer); QTest::newRow("dopus-vfs") << int(PluginFormat::DopusVfs);
    }
    void generatedFormats()
    {
        QFETCH(int, Format); QTemporaryDir Package, Config;
        QVERIFY(Put(Package.filePath("plugin.dll"), Pe()));
        const auto Checked = InspectPluginBinary(Package.filePath("plugin.dll"), PluginFormat(Format)); QVERIFY(Checked.HasValue());
        const auto Saved = SavePluginManifest(Checked.Value(), Config.path()); QVERIFY(Saved.HasValue());
        QCOMPARE(InspectManifestFile(Saved.Value()).Value().manifest_, Checked.Value().manifest_);
    }
    void generatedPackageAndUpdate()
    {
        QTemporaryDir Package, Config; QVERIFY(Package.isValid()); QVERIFY(Config.isValid());
        const auto Binary = Package.filePath("viewer.wlx64"); const auto Dll = Pe();
        QVERIFY(Put(Binary, Dll)); QVERIFY(Put(Package.filePath("support.dll"), Dll));
        auto First = InspectPluginBinary(Binary, PluginFormat::Wlx); QVERIFY2(First.HasValue(), First.HasValue() ? "" : First.Failure().code_.c_str());
        QCOMPARE(First.Value().manifest_.dependencies_.size(), 1u);
        QVERIFY(!First.Value().manifest_.packageRoot_.empty());
        auto Saved = SavePluginManifest(First.Value(), Config.path()); QVERIFY(Saved.HasValue());
        QVERIFY(QFileInfo(Saved.Value()).absolutePath() == Config.path());
        QVERIFY(InspectManifestFile(Saved.Value()).HasValue());
        PluginApproval Approval{First.Value().manifest_, true, false, First.Value().manifest_.permissions_};
        HostProfile Host; Host.formats_ = {PluginFormat::Wlx}; Host.extensions_ = {Extension::Viewer}; Host.supportsIsolation_ = true;
        QVERIFY(Admit(First.Value().manifest_, Approval, Host).HasValue());
        QVERIFY(Put(Binary, Dll + "version-two"));
        QCOMPARE(InspectManifestFile(Saved.Value()).Failure().code_, std::string("plugin-binary-hash-mismatch"));
        QVERIFY(PluginPackageChanged("plugin-binary-hash-mismatch"));
        QVERIFY(!SavePluginManifest(First.Value(), Config.path()).HasValue()); // Changed while the approval dialog was open.
        auto Next = InspectPluginBinary(Binary, PluginFormat::Wlx, First.Value().manifest_); QVERIFY(Next.HasValue());
        QCOMPARE(Next.Value().manifest_.id_, First.Value().manifest_.id_);
        QVERIFY(!Admit(Next.Value().manifest_, Approval, Host).HasValue());
        const auto NextPath = SavePluginManifest(Next.Value(), Config.path()); QVERIFY(NextPath.HasValue()); QVERIFY(NextPath.Value() != Saved.Value());
        QVERIFY(QFileInfo::exists(Saved.Value())); // Cancelling a settings draft cannot overwrite the committed declaration.
        QVERIFY(InspectManifestFile(NextPath.Value()).HasValue());
        QVERIFY(Put(Package.filePath("new.dll"), Dll));
        QCOMPARE(InspectManifestFile(NextPath.Value()).Failure().code_, std::string("plugin-unreviewed-adjacent-dll"));
        QVERIFY(InspectPluginBinary(Binary, PluginFormat::Wlx, Next.Value().manifest_).HasValue());
        QVERIFY(QFile::remove(Package.filePath("support.dll")));
        const auto Removed = InspectPluginBinary(Binary, PluginFormat::Wlx, Next.Value().manifest_); QVERIFY(Removed.HasValue());
        QVERIFY(!Removed.Value().manifest_.dependencies_.contains("support.dll"));
        auto Json = QJsonDocument::fromJson(EncodeManifest(Next.Value().manifest_)).object(); Json["packageRoot"] = "../escape";
        QVERIFY(!ParseManifest(QJsonDocument(Json).toJson()).HasValue());
        QVERIFY(Put(Binary, Pe(0x14c, 0x10b))); QVERIFY(!InspectPluginBinary(Binary, PluginFormat::Wlx).HasValue());
        OperationContext Context; std::stop_source Stop; Stop.request_stop(); Context.stopToken_ = Stop.get_token();
        QVERIFY(!InspectPluginBinary(Binary, PluginFormat::Wlx, {}, Context).HasValue());
    }
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
