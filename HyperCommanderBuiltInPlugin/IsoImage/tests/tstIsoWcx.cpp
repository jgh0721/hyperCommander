#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QtCore/qt_windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"

using namespace hc::plugins;
using namespace hc::storage;
namespace {
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
}
class IsoWcxTests : public QObject {
    Q_OBJECT
private slots:
    void windowsHeaderPaths()
    {
        QTemporaryDir Directory; QVERIFY(QDir().mkpath(Directory.filePath("한글")));
        QVERIFY(Put(Directory.filePath("한글/test.txt"), "nested"));
        const auto Options = BuiltinIsoOptions(); const auto Archive = Directory.filePath("image.iso");
        QVERIFY(MakeApprovedWcxBackend(Options)->Update({Archive.toStdString(), Directory.path().toStdString(), {"한글/test.txt"}}, {}).HasValue());
        // Check the raw ABI independently of a consumer's path normalization:
        // TC displays slash-separated children at the archive root.
        QLibrary Library(Options.binaryPath_); QVERIFY2(Library.load(), qPrintable(Library.errorString()));
        const auto Open = reinterpret_cast<HANDLE (__stdcall*)(tOpenArchiveDataW*)>(Library.resolve("OpenArchiveW"));
        const auto Header = reinterpret_cast<int (__stdcall*)(HANDLE, tHeaderDataExW*)>(Library.resolve("ReadHeaderExW"));
        const auto Process = reinterpret_cast<int (__stdcall*)(HANDLE, int, wchar_t*, wchar_t*)>(Library.resolve("ProcessFileW"));
        const auto Close = reinterpret_cast<int (__stdcall*)(HANDLE)>(Library.resolve("CloseArchive"));
        QVERIFY(Open && Header && Process && Close);
        auto Path = Archive.toStdWString(); tOpenArchiveDataW Data{}; Data.ArcName = Path.data();
        const auto Handle = Open(&Data); QVERIFY(Handle); const auto Guard = qScopeGuard([&] { Close(Handle); });
        QStringList Names; int Status = 0;
        for (;;) {
            tHeaderDataExW Entry{}; Status = Header(Handle, &Entry); if (Status != 0) break;
            Names.push_back(QString::fromWCharArray(Entry.FileName));
            QCOMPARE(Process(Handle, PK_SKIP, nullptr, nullptr), 0);
        }
        QCOMPARE(Status, E_END_ARCHIVE); QVERIFY(Names.contains(QStringLiteral("한글\\test.txt")));
        for (const auto& Name : Names) QVERIFY(!Name.contains('/'));
    }
    void externalBridgeImage()
    {
        const auto Archive = qEnvironmentVariable("HC_ISO_REFERENCE_IMAGE");
        const auto Work = qEnvironmentVariable("HC_ISO_REFERENCE_WORK");
        if (Archive.isEmpty() || Work.isEmpty()) QSKIP("Set HC_ISO_REFERENCE_IMAGE and HC_ISO_REFERENCE_WORK for the large external ISO comparison");
        const auto Inventory = QJsonDocument::fromJson(Read(QDir(Work).filePath("inventory.json"))).object(); QVERIFY(!Inventory.isEmpty());
        auto Options = BuiltinIsoOptions(); Options.limits_.requestMs_ = 60000;
        auto Backend = MakeApprovedWcxBackend(Options);
        auto Listing = Backend->Enumerate(Archive.toStdString(), true, {});
        QVERIFY2(Listing.HasValue(), Listing.HasValue() ? "" : Listing.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) {
            auto Page = Listing.Value()->Next(128, {}); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            Headers.insert(Headers.end(), Page.Value().entries_.begin(), Page.Value().entries_.end());
            if (Page.Value().end_) break;
        }
        Listing.Value().reset(); QCOMPARE(Headers.size(), size_t(Inventory.size()));
        const auto Destination = QDir(Work).filePath("wcx-extracted"); QVERIFY(!QFileInfo::exists(Destination)); QVERIFY(QDir().mkpath(Destination));
        uint64_t Bytes = 0; size_t Files = 0;
        for (const auto& Header : Headers) {
            const auto Name = QString::fromStdString(Header.name_).replace('\\', '/'); QVERIFY(Inventory.contains(Name));
            const auto Expected = Inventory[Name].toObject(); QCOMPARE(Header.directory_, Expected["directory"].toBool());
            if (Header.directory_) continue;
            QCOMPARE(Header.size_, uint64_t(Expected["bytes"].toInteger()));
            const auto Output = QDir(Destination).filePath(QString::number(Files++));
            auto Extracted = Backend->Extract(Archive.toStdString(), Header, Output.toStdString(), {});
            QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str());
            QFile File(Output); QVERIFY(File.open(QIODevice::ReadOnly)); QCryptographicHash Hash(QCryptographicHash::Sha256); QVERIFY(Hash.addData(&File));
            QCOMPARE(QString::fromLatin1(Hash.result().toHex()), Expected["sha256"].toString()); Bytes += Header.size_;
        }
        qInfo() << "External bridge hosted WCX enumeration/read/extraction" << Files << "files" << Bytes << "bytes, all SHA-256 matched";
    }
    void configurationProfiles()
    {
        QTemporaryDir Directory; QVERIFY(Put(Directory.filePath("SOURCE.TXT"), "profile"));
        for (int Profile = 0; Profile < 4; ++Profile) {
            auto Options = BuiltinIsoOptions(); Options.defaultIniPath_ = Directory.filePath("pkplugin.ini");
            QVERIFY(Put(Options.defaultIniPath_, "[IsoImage]\r\nProfile=" + QByteArray::number(Profile) + "\r\nVolumeLabel=PROFILE_TEST\r\n"));
            const auto Target = Directory.filePath(QString("profile-%1.iso").arg(Profile));
            const auto Result = MakeApprovedWcxBackend(Options)->Update({Target.toStdString(), Directory.path().toStdString(), {"SOURCE.TXT"}}, {});
            QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
            const auto Bytes = Read(Target); QCOMPARE(Bytes.mid(16 * 2048 + 40, 12), QByteArray("PROFILE_TEST"));
            QCOMPARE(uint8_t(Bytes[17 * 2048]), Profile == 0 || Profile == 3 ? uint8_t(255) : uint8_t(2));
            QCOMPARE(Bytes.mid((Profile == 3 ? 19 : 20) * 2048 + 1, 5) == QByteArray("NSR02"), Profile >= 2);
        }
    }
    void externalBridgeWrite()
    {
        const auto Work = qEnvironmentVariable("HC_ISO_REFERENCE_WORK");
        if (Work.isEmpty()) QSKIP("Set HC_ISO_REFERENCE_WORK for large WCX creation and rewrite");
        const auto Inventory = QJsonDocument::fromJson(Read(QDir(Work).filePath("inventory.json"))).object(); QVERIFY(!Inventory.isEmpty());
        auto Options = BuiltinIsoOptions(); Options.defaultIniPath_ = QDir(Work).filePath("wcx-profile.ini");
        QVERIFY(Put(Options.defaultIniPath_, "[IsoImage]\r\nProfile=3\r\nVolumeLabel=CCCOMA_X64FRE_KO-KR_DV9\r\n"));
        auto Backend = MakeApprovedWcxBackend(Options);
        const auto Target = QDir(Work).filePath("wcx-created.iso"); QVERIFY(!QFileInfo::exists(Target));
        ArchiveUpdate Request{Target.toStdString(), QDir(Work).filePath("extracted").toStdString()}; Request.maxOutputBytes_ = 128ull * 1024 * 1024 * 1024;
        for (auto I = Inventory.begin(); I != Inventory.end(); ++I) Request.add_.push_back((I.key() + (I.value().toObject()["directory"].toBool() ? "/" : "")).toStdString());
        auto Created = Backend->Update(Request, {}); QVERIFY2(Created.HasValue(), Created.HasValue() ? "" : Created.Failure().code_.c_str());
        QVERIFY(QFileInfo(Target).size() > 4ll * 1024 * 1024 * 1024);
        auto Inspected = InspectBuiltinIso(Target, true, {}); QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        QCOMPARE(Inspected.Value().files_ + Inspected.Value().directories_, uint64_t(Inventory.size()));
        QCOMPARE(Inspected.Value().udfRevision_, 0x0102u); QVERIFY(!Inspected.Value().namespaces_.contains("Joliet"));
        QCOMPARE(Inspected.Value().volumeLabel_, QString("CCCOMA_X64FRE_KO-KR_DV9"));
        const auto BeforeSize = QFileInfo(Target).size();
        const auto Marker = QString("hc-iso-validation-marker.txt"); QVERIFY(Put(QDir(Work).filePath(Marker), "rewrite"));
        ArchiveUpdate Changed{Target.toStdString(), Work.toStdString(), {Marker.toStdString()}, {}, Request.maxOutputBytes_};
        QVERIFY(Backend->Update(Changed, {}).HasValue());
        Inspected = InspectBuiltinIso(Target, true, {}); QVERIFY(Inspected.HasValue()); QCOMPARE(Inspected.Value().files_ + Inspected.Value().directories_, uint64_t(Inventory.size() + 1));
        Changed.add_.clear(); Changed.remove_ = {Marker.toStdString()}; QVERIFY(Backend->Update(Changed, {}).HasValue());
        QCOMPARE(QFileInfo(Target).size(), BeforeSize);
        qInfo() << "Large hosted WCX create/add/delete passed" << BeforeSize << "bytes";
    }
    void bootMetadataPreventsRewrite()
    {
        QTemporaryDir Directory; auto Options = BuiltinIsoOptions(); Options.defaultIniPath_ = Directory.filePath("profile.ini");
        QVERIFY(Put(Options.defaultIniPath_, "[IsoImage]\r\nProfile=0\r\n")); QVERIFY(Put(Directory.filePath("source.txt"), "preserve"));
        auto Backend = MakeApprovedWcxBackend(Options); const auto Target = Directory.filePath("boot.iso");
        ArchiveUpdate Request{Target.toStdString(), Directory.path().toStdString(), {"source.txt"}};
        QVERIFY(Backend->Update(Request, {}).HasValue());
        auto Bytes = Read(Target); Bytes.replace(18 * 2048, 2048, Bytes.mid(17 * 2048, 2048)); Bytes[17 * 2048] = 0;
        QVERIFY(Put(Target, Bytes)); QVERIFY(!Backend->Update(Request, {}).HasValue()); QCOMPARE(Read(Target), Bytes);
        auto Inspection = InspectBuiltinIso(Target, true, {}); QVERIFY(Inspection.HasValue()); QCOMPARE(Inspection.Value().files_, 1ull);
    }
    void hostedRoundTrip()
    {
        QTemporaryDir Directory; auto Options = BuiltinIsoOptions(); QVERIFY(Options.approval_.approved_);
        QVERIFY(!Options.approval_.granted_.contains(Permission::ChildProcess));
        auto Backend = MakeApprovedWcxBackend(Options);
        const auto Archive = Directory.filePath("image.iso");
        QVERIFY(QDir().mkpath(Directory.filePath("한글/empty")));
        QMap<QString, QByteArray> Expected{{"alpha.txt", "alpha"}, {"한글/test.txt", QByteArray(200000, 'z')}, {"zero", {}}};
        ArchiveUpdate Request{Archive.toStdString(), Directory.path().toStdString()}; Request.add_.push_back("한글/empty/");
        for (auto I = Expected.cbegin(); I != Expected.cend(); ++I) { QVERIFY(Put(Directory.filePath(I.key()), I.value())); Request.add_.push_back(I.key().toStdString()); }
        auto Updated = Backend->Update(Request, {}); QVERIFY2(Updated.HasValue(), Updated.HasValue() ? "" : Updated.Failure().code_.c_str());
        const auto Verify = [&]() -> bool {
            auto Cursor = Backend->Enumerate(Archive.toStdString(), true, {}); if (!Cursor.HasValue()) { qWarning() << Cursor.Failure().code_; return false; }
            std::vector<ArchiveHeader> Headers;
            for (;;) { auto Page = Cursor.Value()->Next(2, {}); if (!Page.HasValue()) { qWarning() << Page.Failure().code_; return false; } Headers.insert(Headers.end(), Page.Value().entries_.begin(), Page.Value().entries_.end()); if (Page.Value().end_) break; }
            Cursor.Value().reset(); QSet<QString> Seen;
            for (const auto& Header : Headers) {
                if (Header.directory_) continue; const auto Name = QString::fromStdString(Header.name_).replace('\\', '/'); if (!Expected.contains(Name)) { qWarning() << "Unexpected file" << Name; return false; }
                const auto Output = Directory.filePath("extract-" + QString::number(Seen.size())); QFile::remove(Output);
                const auto Result = Backend->Extract(Archive.toStdString(), Header, Output.toStdString(), {});
                if (!Result.HasValue()) { qWarning() << Result.Failure().code_; return false; }
                if (Read(Output) != Expected[Name]) { qWarning() << "Payload mismatch" << Name; return false; } Seen.insert(Name);
            }
            if (Seen.size() != Expected.size()) qWarning() << "Missing entries" << Seen << Expected.keys();
            return Seen.size() == Expected.size();
        };
        QVERIFY(Verify());
        Expected["alpha.txt"] = "replacement"; QVERIFY(Put(Directory.filePath("alpha.txt"), Expected["alpha.txt"]));
        QVERIFY(Backend->Update({Archive.toStdString(), Directory.path().toStdString(), {"alpha.txt"}}, {}).HasValue()); QVERIFY(Verify());
        QVERIFY(Backend->Update({Archive.toStdString(), Directory.path().toStdString(), {}, {"한글/test.txt"}}, {}).HasValue()); Expected.remove("한글/test.txt"); QVERIFY(Verify());
        const auto Before = Read(Archive); std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        QVERIFY(!Backend->Update(Request, Context).HasValue()); QCOMPARE(Read(Archive), Before);
        const auto Artifact = qEnvironmentVariable("HC_ISO_TEST_ARTIFACT"); if (!Artifact.isEmpty()) QVERIFY(QFile::copy(Archive, Artifact));
    }
    void compiledApproval()
    {
        auto Options = BuiltinIsoOptions(); auto Inspected = InspectManifestFile(Options.manifestPath_); QVERIFY(Inspected.HasValue());
        QVERIFY(Inspected.Value().manifest_ == Options.approval_.reviewed_);
        Options.approval_.reviewed_.version_ = "tampered";
        QTemporaryDir Directory; QVERIFY(!MakeApprovedWcxBackend(Options)->Enumerate(Directory.filePath("no.iso").toStdString(), false, {}).HasValue());
    }
};
QTEST_GUILESS_MAIN(IsoWcxTests)
#include "tstIsoWcx.moc"
