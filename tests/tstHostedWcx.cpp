#include <QtTest>
#include <QCryptographicHash>
#include <QJsonDocument>
#include "pluginHost/hostedArchiveBackend.hpp"
#include "pluginHost/approvedWcxBackend.hpp"
#include "pluginHost/archiveCodec.hpp"
#include "pluginHost/pipeChannel.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/wcxBackend.hpp"
#include "UniqueLibs/wcxMutation.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include <Windows.h>
#include <thread>
#include <TlHelp32.h>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    Locator At(const QString& Path, std::string Entry = "/") { return {LocatorKind::Archive, Path.toStdString(), {{"wcx", std::move(Entry)}}}; }
    std::shared_ptr<IContentSource> Content(const QString& Path)
    {
        auto Provider = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Provider->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get();
        return Entry.HasValue() ? std::make_shared<ProviderContentSource>(Provider, Entry.Value()) : nullptr;
    }
    Result<std::shared_ptr<PluginProcess>> Start(const QTemporaryDir& Directory, bool Write = true, unsigned RequestMs = 3000, QString Source = {}, bool Helper = false, unsigned WriteMs = 0)
    {
        const auto Binary = Directory.filePath("fixture.wcx64");
        if (Source.isEmpty()) Source = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        if (!QFile::copy(Source, Binary)) return Error{ErrorCategory::Internal, "copy-fixture"};
        PluginManifest Manifest; Manifest.id_ = "hc.test.hosted-wcx"; Manifest.version_ = "1"; Manifest.binary_ = "fixture.wcx64";
        Manifest.sha256_ = QCryptographicHash::hash(Read(Binary), QCryptographicHash::Sha256).toHex().toStdString();
        Manifest.extensions_ = {Extension::ArchiveRead}; Manifest.permissions_ = {Permission::ReadContent};
        if (Write) { Manifest.extensions_.insert(Extension::ArchiveWrite); Manifest.permissions_.insert(Permission::WriteContent); }
        if (Helper)
        {
            if (!QDir().mkdir(Directory.filePath("64")) || !QFile::copy(Binary, Directory.filePath("64/7z.dll"))
                || !QFile::copy(QCoreApplication::applicationDirPath() + "/hc_plugin_helper_fixture.exe", Directory.filePath("64/7zG.exe"))) return Error{ErrorCategory::Internal, "copy-helper"};
            Manifest.compatibility_ = "total7zip"; Manifest.permissions_.insert(Permission::ChildProcess); Manifest.permissions_.insert(Permission::UserInterface);
            Manifest.dependencies_["64/7z.dll"] = Manifest.sha256_;
            Manifest.helpers_["64/7zG.exe"] = QCryptographicHash::hash(Read(Directory.filePath("64/7zG.exe")), QCryptographicHash::Sha256).toHex().toStdString();
        }
        PluginApproval Approval; Approval.reviewed_ = Manifest; Approval.approved_ = true; Approval.granted_ = Manifest.permissions_;
        if (!Put(Directory.filePath("plugin.json"), EncodeManifest(Manifest))) return Error{ErrorCategory::Internal, "write-manifest"};
        HostLimits Limits; Limits.requestMs_ = RequestMs; Limits.archiveWriteMs_ = WriteMs ? WriteMs : RequestMs;
        auto Started = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Directory.filePath("plugin.json"), Approval, Limits);
        if (!Started.HasValue()) return Started.Failure(); return std::shared_ptr<PluginProcess>(std::move(Started.Value()));
    }
    QByteArray Archive(int Count)
    {
        QByteArray Bytes("HCWCX02\0", 8);
        const auto Number = [&](quint32 Value) { Bytes.append(reinterpret_cast<const char*>(&Value), 4); };
        Number(Count);
        for (int Index = 0; Index < Count; ++Index)
        {
            const auto Name = QString("한글-%1.txt").arg(Index); Number(Name.size()); Number(1); Number(0);
            Bytes.append(reinterpret_cast<const char*>(Name.utf16()), Name.size() * 2); Bytes.append('a');
        }
        return Bytes;
    }
}
class HostedWcxTests : public QObject
{
    Q_OBJECT
private slots:
    void nestedHostedMutation()
    {
        for (const char Mode : {char(0), 'P', 'J'}) {
            QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
            const auto Path = Directory.filePath("outer.hct"); QByteArray Original("HCWCX02", 7); Original += Mode;
            const QString Name("inner.hct"); const auto Number = [&](quint32 Value) { Original.append(reinterpret_cast<const char*>(&Value), 4); };
            Number(1); Number(Name.size()); Number(7); Number(0); Original.append(reinterpret_cast<const char*>(Name.utf16()), Name.size() * 2); Original += "HCWCX01";
            QVERIFY(Put(Path, Original)); auto Root = At(Path, "/inner.hct"); Root.nesting_.push_back({"wcx", "/"});
            auto Backend = MakeHostedArchiveBackend(Started.Value()); auto Reader = MakeWcxReadProvider({}, Root, Directory.path().toStdString(), Backend);
            auto Session = BeginWcxMutation({}, Root, Reader, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
            auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::MakeDirectory, "/created"}}, {}); QCOMPARE(Read(Path), Original);
            if (Mode) { QVERIFY(!Prepared.HasValue()); QVERIFY(Session.Value()->Abort().empty()); QCOMPARE(Read(Path), Original); }
            else { QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Session.Value()->Commit({}).HasValue());
                auto Fresh = MakeWcxReadProvider({}, Root, Directory.path().toStdString(), Backend); Root.nesting_.back().entry_ = "/created";
                auto Added = Fresh->Resolve(Root, {}).get(); QVERIFY(Added.HasValue()); QVERIFY(Added.Value().directory_); }
        }
    }
    void nestedHostedJointMutation()
    {
        for (const char Mode : {char(0), 'P', 'J'}) {
            QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
            const auto Path = Directory.filePath("joint.hct"); QByteArray Original("HCWCX02", 7); Original += Mode;
            const auto Number = [&](quint32 Value) { Original.append(reinterpret_cast<const char*>(&Value), 4); }; Number(2);
            for (const QString Name : {QString("source.hct"), QString("target.hct")}) {
                Number(Name.size()); Number(7); Number(0); Original.append(reinterpret_cast<const char*>(Name.utf16()), Name.size() * 2); Original += "HCWCX01";
            }
            QVERIFY(Put(Path, Original));
            auto SourceRoot = At(Path, "/source.hct"); SourceRoot.nesting_.push_back({"wcx", "/"});
            auto TargetRoot = At(Path, "/target.hct"); TargetRoot.nesting_.push_back({"wcx", "/"});
            auto Backend = MakeHostedArchiveBackend(Started.Value());
            auto Source = MakeWcxReadProvider({}, SourceRoot, Directory.path().toStdString(), Backend);
            auto Target = MakeWcxReadProvider({}, TargetRoot, Directory.path().toStdString(), Backend);
            auto Alpha = SourceRoot; Alpha.nesting_.back().entry_ = "/alpha.txt";
            auto Entry = Source->Resolve(Alpha, {}).get(); QVERIFY(Entry.HasValue());
            auto Removal = BeginWcxMutation({}, SourceRoot, Source, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Removal.HasValue());
            auto Session = BeginWcxMutation({}, TargetRoot, Target, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
            auto Prepared = Session.Value()->PrepareTogether({{ArchiveChangeKind::Put, "/moved.txt", {}, std::make_shared<ProviderContentSource>(Source, Entry.Value())}},
                std::move(Removal.Value()), {{ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Entry.Value().id_}}, {});
            QCOMPARE(Read(Path), Original);
            if (Mode) { QVERIFY(!Prepared.HasValue()); QVERIFY(Session.Value()->Abort().empty()); QCOMPARE(Read(Path), Original); }
            else {
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Session.Value()->Commit({}).HasValue());
                QVERIFY(!MakeWcxReadProvider({}, SourceRoot, Directory.path().toStdString(), Backend)->Resolve(Alpha, {}).get().HasValue());
                Alpha = TargetRoot; Alpha.nesting_.back().entry_ = "/moved.txt";
                QVERIFY(MakeWcxReadProvider({}, TargetRoot, Directory.path().toStdString(), Backend)->Resolve(Alpha, {}).get().HasValue());
            }
        }
    }
    void encryptedMutationPreservesEncryption()
    {
        for (const char Mode : {'Q', 'R'}) {
            QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
            const auto Path = Directory.filePath("encrypted.hct"); const auto Original = QByteArray("HCWCX01") + Mode; QVERIFY(Put(Path, Original));
            auto Backend = MakeHostedArchiveBackend(Started.Value()); auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
            OperationContext Context; Context.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<Password> { return Password("비밀-pass"); };
            auto Entry = Provider->Resolve(At(Path, "/alpha.txt"), Context).get(); QVERIFY(Entry.HasValue());
            auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Session.HasValue());
            auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Rename, "/alpha.txt", "/renamed.txt", {}, Entry.Value().id_}}, Context);
            QCOMPARE(Read(Path), Original);
            if (Mode == 'R') { QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, std::string("archive-output-encryption-lost")); }
            else { QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Session.Value()->Commit(Context).HasValue());
                auto Fresh = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend); auto Renamed = Fresh->Resolve(At(Path, "/renamed.txt"), Context).get();
                QVERIFY(Renamed.HasValue()); QVERIFY(Renamed.Value().hints_.contains("encrypted")); }
        }
    }
    void passwordCallbacks()
    {
        for (const bool Hosted : {false, true}) for (const char Mode : {'U', 'Q'}) {
            QTemporaryDir Directory; const auto Path = Directory.filePath("encrypted.hct"); QVERIFY(Put(Path, QByteArray("HCWCX01") + Mode));
            std::shared_ptr<IArchiveBackend> Backend; std::shared_ptr<PluginProcess> Host;
            if (Hosted) { auto Started = Start(Directory); QVERIFY(Started.HasValue()); Host = Started.Value(); Backend = MakeHostedArchiveBackend(Host); }
            else Backend = MakeInProcessWcxBackend((QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64").toStdString());
            unsigned Prompts = 0; OperationContext Context;
            Context.passwordPrompt_ = [&](const std::string&, unsigned Attempt, const OperationContext&) -> Result<Password> {
                ++Prompts; return Password(Attempt == 1 ? "wrong" : "비밀-pass");
            };
            auto Cursor = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
            auto Page = Cursor.Value()->Next(128, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            QCOMPARE(Page.Value().entries_.size(), 2); QVERIFY(Prompts >= 2); QCOMPARE(Read(Path), QByteArray("HCWCX01") + Mode);
        }
    }
    void passwordCancellationAndRetryLimit()
    {
        for (const int Mode : {0, 1, 2, 3}) {
            QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
            const auto Path = Directory.filePath("encrypted.hct"); QVERIFY(Put(Path, "HCWCX01U"));
            auto Backend = MakeHostedArchiveBackend(Started.Value()); OperationContext Context; unsigned Prompts = 0;
            if (Mode) Context.passwordPrompt_ = [&](const std::string&, unsigned, const OperationContext& Call) -> Result<Password> {
                ++Prompts;
                if (Mode == 1) return Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}};
                if (Mode == 3) { while (!Call.Check({"wcx"})) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return *Call.Check({"wcx"}); }
                return Password("wrong");
            };
            if (Mode == 3) Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
            auto Result = Backend->Enumerate(Path.toStdString(), false, Context); QVERIFY(!Result.HasValue());
            if (Mode == 0) QCOMPARE(Result.Failure().code_, std::string("archive-password-required"));
            if (Mode == 1) QCOMPARE(Result.Failure().category_, ErrorCategory::Cancelled);
            if (Mode == 2) { QCOMPARE(Result.Failure().code_, std::string("archive-password-attempt-limit")); QCOMPARE(Prompts, 3u); }
            QCOMPARE(Read(Path), QByteArray("HCWCX01U"));
        }
    }
    void inProcessWriteBudget()
    {
        QTemporaryDir Directory; auto Started = Start(Directory, true, 300, {}, false, 5000); QVERIFY(Started.HasValue());
        const auto Path = Directory.filePath("slow.hct"); QVERIFY(Put(Path, "HCWCX01L")); QVERIFY(Put(Directory.filePath("added.txt"), "payload"));
        auto Backend = MakeHostedArchiveBackend(Started.Value()); QElapsedTimer Timer; Timer.start();
        auto Result = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {"added.txt"}}, {});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str()); QVERIFY(Timer.elapsed() >= 800);
        QVERIFY(Started.Value()->Ping().HasValue());
    }
    void compatibilityPackage()
    {
        const auto ManifestPath = qEnvironmentVariable("HC_WCX_MANIFEST");
        if (ManifestPath.isEmpty()) QSKIP("Set HC_WCX_MANIFEST for an explicitly selected compatibility package");
        auto Inspected = InspectManifestFile(ManifestPath); QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_}; QTemporaryDir Directory;
        HostLimits Limits; Limits.requestMs_ = 15000;
        auto Started = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", ManifestPath, Approval, Limits, {}, Directory.filePath("pkplugin.ini"));
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::shared_ptr<PluginProcess>(std::move(Started.Value())); auto Backend = MakeHostedArchiveBackend(Host);
        auto Probe = Host->Probe(); QVERIFY(Probe.HasValue()); qInfo() << "Compatibility probe" << Probe.Value();
        auto Path = qEnvironmentVariable("HC_WCX_SOURCE");
        const bool Create = Path.isEmpty();
        QMap<QString, QByteArray> ExpectedFiles;
        if (Create)
        {
            Path = Directory.filePath("created." + qEnvironmentVariable("HC_WCX_EXTENSION", "sqx"));
            ExpectedFiles.insert("alpha.txt", "compatibility archive content");
            ExpectedFiles.insert("한글.txt", "Unicode content");
            ExpectedFiles.insert("empty.txt", {});
            QByteArray Repeated;
            for (int Index = 0; Index < 8192; ++Index) Repeated += QByteArray::number(Index) + " SQX repeated match content 0123456789\r\n";
            ExpectedFiles.insert("nested/repeated.txt", Repeated);
            QByteArray Random(256 * 1024, Qt::Uninitialized); QRandomGenerator Generator(0x535158);
            for (auto& Byte : Random) Byte = char(Generator.generate() & 0xff);
            ExpectedFiles.insert("nested/random.bin", Random);
            QVERIFY(QDir().mkpath(Directory.filePath("nested")));
            std::vector<std::string> Names;
            for (auto It = ExpectedFiles.cbegin(); It != ExpectedFiles.cend(); ++It)
            { QVERIFY(Put(Directory.filePath(It.key()), It.value())); Names.push_back(It.key().toStdString()); }
            const auto Created = Backend->Update({Path.toStdString(), Directory.path().toStdString(), Names}, {});
            QVERIFY2(Created.HasValue(), Created.HasValue() ? "" : Created.Failure().code_.c_str());
        }
        auto Cursor = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
        auto Page = Cursor.Value()->Next(256, {}); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); QVERIFY(!Page.Value().entries_.empty());
        const auto Entry = Page.Value().entries_.front(); qInfo() << "Archive entries" << Page.Value().entries_.size(); Cursor.Value().reset();
        for (const auto& Header : Page.Value().entries_) qInfo() << QString::fromStdString(Header.name_) << Header.size_ << Header.packedSize_;
        const auto Output = Directory.filePath("extracted.txt");
        const auto Extracted = Backend->Extract(Path.toStdString(), Entry, Output.toStdString(), {});
        QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str());
        if (!Create) QVERIFY(QFileInfo(Output).size() > 0);
        if (Create)
        {
            const auto VerifyContents = [&]() -> bool {
                auto Listing = Backend->Enumerate(Path.toStdString(), true, {});
                if (!Listing.HasValue()) { qWarning() << Listing.Failure().code_; return false; }
                std::vector<ArchiveHeader> Headers;
                for (;;)
                {
                    auto Next = Listing.Value()->Next(2, {});
                    if (!Next.HasValue()) { qWarning() << Next.Failure().code_; return false; }
                    Headers.insert(Headers.end(), Next.Value().entries_.begin(), Next.Value().entries_.end());
                    if (Next.Value().end_) break;
                }
                Listing.Value().reset(); QSet<QString> Seen;
                for (const auto& Header : Headers)
                {
                    if (Header.directory_) continue;
                    auto Name = QString::fromStdString(Header.name_); Name.replace('\\', '/');
                    if (!ExpectedFiles.contains(Name) || Seen.contains(Name)) { qWarning() << "Unexpected entry" << Name; return false; }
                    Seen.insert(Name);
                    // Extract to a fresh staging path. Destination replacement
                    // belongs to the host's commit layer, not a plugin dialog.
                    if (QFile::exists(Output) && !QFile::remove(Output)) return false;
                    const auto Extract = Backend->Extract(Path.toStdString(), Header, Output.toStdString(), {});
                    if (!Extract.HasValue()) { qWarning() << Name << Extract.Failure().code_; return false; }
                    if (Read(Output) != ExpectedFiles[Name]) { qWarning() << "Payload mismatch" << Name; return false; }
                }
                if (Seen.size() != ExpectedFiles.size()) qWarning() << "Missing entries" << Seen << ExpectedFiles.keys();
                return Seen.size() == ExpectedFiles.size();
            };
            QVERIFY(VerifyContents());
            ExpectedFiles["alpha.txt"] = "updated SQX payload";
            QVERIFY(Put(Directory.filePath("alpha.txt"), ExpectedFiles["alpha.txt"]));
            const auto Updated = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {"alpha.txt"}}, {});
            QVERIFY2(Updated.HasValue(), Updated.HasValue() ? "" : Updated.Failure().code_.c_str());
            QVERIFY(VerifyContents());
            const auto Deleted = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {}, {"한글.txt"}}, {});
            QVERIFY2(Deleted.HasValue(), Deleted.HasValue() ? "" : Deleted.Failure().code_.c_str());
            ExpectedFiles.remove("한글.txt"); QVERIFY(VerifyContents());
            qInfo() << "Create, paged list, test, all payloads, replacement and deletion verified";

            if (!Inspected.Value().manifest_.helpers_.empty())
            {
                qInfo() << "Job processes" << Host->Snapshot().activeProcesses_ << Host->Snapshot().totalProcesses_;
                QVERIFY(Host->Snapshot().totalProcesses_ >= 4);
                const auto Before = Read(Path);
                auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
                const auto NewFile = Directory.filePath("new file.txt"); QVERIFY(Put(NewFile, "transaction payload"));
                auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, {}, {}, Backend);
                QVERIFY(Session.HasValue());
                auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Put, "/new file.txt", {}, Content(NewFile)}}, {});
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
                QCOMPARE(Read(Path), Before); QVERIFY(Session.Value()->Commit({}).HasValue());
                ExpectedFiles["new file.txt"] = "transaction payload"; QVERIFY(VerifyContents());
                qInfo() << "Prepared archive verified; original preserved until commit";
            }
            const auto Artifact = qEnvironmentVariable("HC_WCX_CREATED_ARCHIVE");
            if (!Artifact.isEmpty()) QVERIFY(QFile::copy(Path, Artifact));
            const auto ExpectedDirectory = qEnvironmentVariable("HC_WCX_EXPECTED_DIRECTORY");
            if (!ExpectedDirectory.isEmpty()) for (auto It = ExpectedFiles.cbegin(); It != ExpectedFiles.cend(); ++It)
            {
                const auto File = QDir(ExpectedDirectory).filePath(It.key()); QVERIFY(QDir().mkpath(QFileInfo(File).absolutePath())); QVERIFY(Put(File, It.value()));
            }
            // Damage payload bytes while keeping readable headers. PK_TEST must
            // surface the deferred CloseArchive error without a modal dialog.
            auto DamagedBytes = Read(Path); QVERIFY(DamagedBytes.size() > 4096);
            DamagedBytes[DamagedBytes.size() / 2] ^= char(0x80);
            const auto Damaged = Directory.filePath("damaged.sqx"); QVERIFY(Put(Damaged, DamagedBytes));
            auto DamagedHeaders = Backend->Enumerate(Damaged.toStdString(), false, {}); QVERIFY(DamagedHeaders.HasValue());
            auto Readable = DamagedHeaders.Value()->Next(256, {}); QVERIFY(Readable.HasValue()); QVERIFY(Readable.Value().end_);
            DamagedHeaders.Value().reset();
            auto Integrity = Backend->Enumerate(Damaged.toStdString(), true, {}); QVERIFY(Integrity.HasValue());
            auto Rejected = Integrity.Value()->Next(256, {}); QVERIFY(!Rejected.HasValue());
            qInfo() << "Corrupt payload rejected:" << Rejected.Failure().code_;
            Integrity.Value().reset(); QVERIFY(Host->Ping().HasValue());
            const auto Corrupt = Directory.filePath("corrupt.sqx"); QVERIFY(Put(Corrupt, Read(Path).left(16)));
            auto Broken = Backend->Enumerate(Corrupt.toStdString(), false, {});
            if (Broken.HasValue()) QVERIFY(!Broken.Value()->Next(256, {}).HasValue());
        }
        else if (qEnvironmentVariableIsSet("HC_WCX_CHECK_HELPER_DENIAL"))
        {
            const auto Before = Read(Path); QVERIFY(Put(Directory.filePath("alpha.txt"), "helper policy probe"));
            const auto Attempt = Backend->Update({Directory.filePath("helper.7z").toStdString(), Directory.path().toStdString(), {"alpha.txt"}}, {});
            QVERIFY(!Attempt.HasValue()); QCOMPARE(Attempt.Failure().code_, std::string("plugin-helper-required")); QVERIFY(Host->Ping().HasValue()); qInfo() << "Helper-dependent write unavailable:" << QString::fromStdString(Attempt.Failure().code_);
            QCOMPARE(Read(Path), Before);
        }
        Host->Close(); QVERIFY(!Host->Snapshot().running_);
        QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void compatibilityPassword()
    {
        const auto ManifestPath = qEnvironmentVariable("HC_WCX_PASSWORD_MANIFEST");
        const auto Path = qEnvironmentVariable("HC_WCX_PASSWORD_SOURCE");
        const auto Mode = qEnvironmentVariable("HC_WCX_PASSWORD_MODE", "read");
        if (ManifestPath.isEmpty() || Path.isEmpty()) QSKIP("Set explicit password compatibility manifest and owned fixture path");
        const bool Write = Mode.startsWith("write");
        QVERIFY(Mode == "create" || Mode == "read" || Mode == "cancel" || Mode == "timeout" || Mode == "wrong" || Mode == "dismiss"
            || Mode == "write" || Mode == "write-plain" || Mode == "write-cancel" || Mode == "write-wrong" || Mode == "write-timeout");
        auto Inspected = InspectManifestFile(ManifestPath); QVERIFY(Inspected.HasValue());
        PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_};
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); HostLimits Limits; Limits.requestMs_ = 60000;
        auto Started = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", ManifestPath, Approval, Limits, {}, Directory.filePath("pkplugin.ini"));
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::shared_ptr<PluginProcess>(std::move(Started.Value())); auto Backend = MakeHostedArchiveBackend(Host);
        struct NativePromptState { DWORD pid_; bool wrong_, dismiss_, wait_, plain_, writeOnly_; std::atomic<unsigned> handled_{0}, visible_{0};
            std::wstring helper_; std::vector<DWORD> children_; };
        NativePromptState Native{Host->Snapshot().processId_, Mode == "wrong" || Mode == "write-wrong", Mode == "dismiss" || Mode == "write-cancel",
            Mode == "cancel" || Mode == "timeout" || Mode == "write-timeout", Mode == "write-plain", Mode == "write-cancel" || Mode == "write-timeout"};
        if (Inspected.Value().manifest_.helpers_.contains("64/7zG.exe"))
            Native.helper_ = QDir::toNativeSeparators(QFileInfo(ManifestPath).dir().absoluteFilePath("64/7zG.exe")).toStdWString();
        std::jthread NativeInput;
        if (qEnvironmentVariableIntValue("HC_WCX_PASSWORD_AUTOMATE") == 1)
            NativeInput = std::jthread([&](std::stop_token Token) {
                while (!Token.stop_requested()) {
                    Native.children_.clear();
                    if (!Native.helper_.empty()) {
                        const auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                        if (Snapshot != INVALID_HANDLE_VALUE) {
                            PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
                            if (Process32FirstW(Snapshot, &Entry)) do {
                                if (Entry.th32ParentProcessID != Native.pid_) continue;
                                const auto Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID);
                                if (!Process) continue;
                                wchar_t Image[32768]{}; DWORD Length = DWORD(std::size(Image));
                                if (QueryFullProcessImageNameW(Process, 0, Image, &Length) && _wcsicmp(Image, Native.helper_.c_str()) == 0)
                                    Native.children_.push_back(Entry.th32ProcessID);
                                CloseHandle(Process);
                            } while (Process32NextW(Snapshot, &Entry));
                            CloseHandle(Snapshot);
                        }
                    }
                    EnumWindows([](HWND Window, LPARAM Parameter) -> BOOL {
                        auto& State = *reinterpret_cast<NativePromptState*>(Parameter); DWORD Pid = 0; GetWindowThreadProcessId(Window, &Pid);
                        if ((Pid != State.pid_ && std::ranges::find(State.children_, Pid) == State.children_.end()) || !IsWindowVisible(Window)) return TRUE;
                        std::vector<HWND> Fields;
                        EnumChildWindows(Window, [](HWND Child, LPARAM Parameter) -> BOOL {
                            wchar_t Class[64]{}; GetClassNameW(Child, Class, 64);
                            if (_wcsicmp(Class, L"Edit") == 0 && (GetWindowLongPtrW(Child, GWL_STYLE) & ES_PASSWORD) && IsWindowVisible(Child))
                                reinterpret_cast<std::vector<HWND>*>(Parameter)->push_back(Child);
                            return TRUE;
                        }, reinterpret_cast<LPARAM>(&Fields));
                        if (Fields.empty()) return TRUE;
                        const auto CreateSqx = GetDlgItem(Window, 1709) != nullptr;
                        const auto ReadSqx = GetDlgItem(Window, 1603) && GetDlgItem(Window, 2000);
                        const auto Packing = CreateSqx || Fields.size() > 1;
                        ++State.visible_; if (State.wait_ && (!State.writeOnly_ || Packing)) return TRUE;
                        const auto Plain = State.plain_ && Packing;
                        DWORD_PTR Result = 0;
                        // TotalSQX reopens its input after a wrong password.
                        // Submit one wrong value, then cancel the retry.
                        const auto Dismiss = (State.dismiss_ && (!State.writeOnly_ || Packing)) || (State.wrong_ && State.handled_.load() != 0);
                        if (!Dismiss) for (auto Field : Fields)
                            if (!SendMessageTimeoutW(Field, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(Plain ? L"" : State.wrong_ ? L"wrong-test-password" : L"hc-test-only-2026"), SMTO_ABORTIFHUNG, 500, &Result)) return TRUE;
                        // Only the test host or its approved direct helper's
                        // visible dialog is touched; no global keys/credentials.
                        // TotalSQX uses its own resource IDs for the encryption
                        // dialog; Total7zip uses the standard OK/Cancel IDs.
                        const auto Command = CreateSqx ? (Dismiss ? 1711 : State.plain_ ? 1710 : 1709) : ReadSqx ? (Dismiss ? 2001 : 2000) : (Dismiss ? IDCANCEL : IDOK);
                        if (SendMessageTimeoutW(Window, WM_COMMAND, Command, 0, SMTO_ABORTIFHUNG, 500, &Result)) ++State.handled_;
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&Native));
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
        const QByteArray Payload("HC encrypted compatibility payload\r\n");
        std::atomic<unsigned> Prompts = 0; std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        Context.passwordPrompt_ = [&](const std::string&, unsigned, const OperationContext&) -> Result<Password> { ++Prompts;
            if (Mode == "dismiss" || Mode == "write-cancel") return Error{ErrorCategory::Cancelled, "test-password-cancelled"};
            return Password(Mode == "wrong" || Mode == "write-wrong" ? "wrong-test-password" : "hc-test-only-2026"); };
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        if (Write) {
            // Each run owns a copy. Never publish over the supplied corpus.
            const auto Original = Read(Path); QVERIFY(!Original.isEmpty());
            const auto Working = Directory.filePath("write." + QFileInfo(Path).suffix()); QVERIFY(Put(Working, Original));
            auto Root = At(Working);
            auto Provider = MakeWcxReadProvider({}, Root, Directory.path().toStdString(), Backend);
            auto Secret = Provider->Resolve(At(Working, "/secret.txt"), Context).get(); QVERIFY(Secret.HasValue());
            QVERIFY(Secret.Value().hints_.contains("encrypted"));
            const auto Input = Directory.filePath("new.txt"); QVERIFY(Put(Input, Payload));
            auto InputContent = Content(Input); QVERIFY(InputContent);
            const auto Replacement = Directory.filePath("replacement.txt"); QVERIFY(Put(Replacement, "changed encrypted payload\r\n"));
            auto ReplacementContent = Content(Replacement); QVERIFY(ReplacementContent);
            if (Mode == "write-timeout") Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            const std::vector<std::vector<ArchiveChange>> Batches = Mode == "write" ? std::vector<std::vector<ArchiveChange>>{
                {{ArchiveChangeKind::Rename, "/secret.txt", "/renamed.txt", {}, Secret.Value().id_}},
                {{ArchiveChangeKind::Put, "/renamed.txt", {}, ReplacementContent}},
                {{ArchiveChangeKind::Put, "/secret.txt", {}, InputContent}},
                {{ArchiveChangeKind::Remove, "/renamed.txt"}}}
                : std::vector<std::vector<ArchiveChange>>{{Mode == "write-wrong"
                    ? ArchiveChange{ArchiveChangeKind::Rename, "/secret.txt", "/renamed.txt", {}, Secret.Value().id_}
                    : ArchiveChange{ArchiveChangeKind::Put, "/new.txt", {}, InputContent}}};
            for (auto Batch : Batches) {
                const auto Before = Read(Working);
                for (auto& Change : Batch) {
                    auto Existing = Provider->Resolve(At(Working, Change.path_), Context).get();
                    if (Existing.HasValue()) {
                        if (Change.kind_ == ArchiveChangeKind::Put) Change.destination_ = {true, Existing.Value().id_.revision_};
                        else Change.source_ = Existing.Value().id_;
                    }
                }
                auto Session = BeginWcxMutation({}, Root, Provider, Directory.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Session.HasValue());
                auto Prepared = Session.Value()->Prepare(Batch, Context);
                QCOMPARE(Read(Working), Before); QCOMPARE(Read(Path), Original);
                if (Mode != "write") {
                    QVERIFY2(!Prepared.HasValue(), "An unencrypted or cancelled write must not become a candidate");
                    qInfo() << "Encrypted write rejected" << Prepared.Failure().code_;
                    if (Mode == "write-plain") QCOMPARE(Prepared.Failure().code_, std::string("archive-output-encryption-lost"));
                    if (Mode == "write-cancel") QCOMPARE(Prepared.Failure().code_, std::string("wcx:21"));
                    if (Mode == "write-timeout") QCOMPARE(Prepared.Failure().code_, std::string("deadline-exceeded"));
                    QVERIFY(Session.Value()->Abort().empty()); QCOMPARE(Read(Working), Before);
                    return;
                }
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
                auto Committed = Session.Value()->Commit(Context); QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
                Provider = MakeWcxReadProvider({}, Root, Directory.path().toStdString(), Backend);
                auto Listing = Backend->Enumerate(Working.toStdString(), true, Context); QVERIFY(Listing.HasValue());
                auto Page = Listing.Value()->Next(256, Context); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_);
                for (const auto& Header : Page.Value().entries_) if (!Header.directory_) QVERIFY(Header.encrypted_);
            }
            auto Final = Backend->Enumerate(Working.toStdString(), false, Context); QVERIFY(Final.HasValue());
            auto Page = Final.Value()->Next(256, Context); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), 1);
            QCOMPARE(Page.Value().entries_.front().name_, std::string("secret.txt"));
            const auto Output = Directory.filePath("verify.txt");
            auto Extracted = Backend->Extract(Working.toStdString(), Page.Value().entries_.front(), Output.toStdString(), Context);
            QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(Read(Output), Payload);
            const auto Saved = qEnvironmentVariable("HC_WCX_CREATED_ARCHIVE");
            if (!Saved.isEmpty()) { QVERIFY(!QFile::exists(Saved)); QVERIFY(QFile::copy(Working, Saved)); }
            qInfo() << "Encrypted rename, replace, add, delete and payload verification passed";
            return;
        }
        if (Mode == "create") {
            QVERIFY(!QFile::exists(Path)); QVERIFY(Put(Directory.filePath("secret.txt"), Payload));
            ArchiveUpdate Update{Path.toStdString(), Directory.path().toStdString(), {"secret.txt"}}; Update.encrypt_ = true;
            auto Created = Backend->Update(Update, Context); QVERIFY2(Created.HasValue(), Created.HasValue() ? "" : Created.Failure().code_.c_str());
        }
        const auto Original = Read(Path); QVERIFY(!Original.isEmpty());
        auto Cursor = Backend->Enumerate(Path.toStdString(), false, Context); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
        auto Page = Cursor.Value()->Next(256, Context); QVERIFY(Page.HasValue()); Cursor.Value().reset();
        const auto Entry = std::ranges::find_if(Page.Value().entries_, [](const auto& Header) { return Header.name_ == "secret.txt"; });
        QVERIFY(Entry != Page.Value().entries_.end()); qInfo() << "Plugin encrypted header marker" << Entry->encrypted_;
        std::jthread Cancellation;
        if (Mode == "cancel") Cancellation = std::jthread([&](std::stop_token Token) {
            for (int Index = 0; Index < 200 && !Token.stop_requested(); ++Index) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!Token.stop_requested()) Stop.request_stop();
        });
        if (Mode == "timeout") Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        const auto Output = Directory.filePath("secret.txt.out");
        auto Extracted = Backend->Extract(Path.toStdString(), *Entry, Output.toStdString(), Context);
        Cancellation.request_stop(); NativeInput.request_stop(); if (NativeInput.joinable()) NativeInput.join();
        QCOMPARE(Read(Path), Original); qInfo() << "Crypt callback requests" << Prompts.load();
        qInfo() << "Native password dialog observations" << Native.visible_.load() << "submitted" << Native.handled_.load();
        if (Mode == "cancel" || Mode == "timeout" || Mode == "wrong" || Mode == "dismiss") {
            QVERIFY(!Extracted.HasValue());
            if (Mode == "wrong" || Mode == "dismiss") QVERIFY(Host->Snapshot().running_);
            // Raw WCX can leave a partial output on failure. The product owns
            // it through a materialization lease; this direct ABI test owns it
            // through Directory and discards it without publishing any bytes.
            qInfo() << "Discarded staging bytes" << QFileInfo(Output).size();
            if (QFile::exists(Output)) QVERIFY(QFile::remove(Output));
            QVERIFY(!QFile::exists(Output));
            qInfo() << "Bounded native password wait result" << Extracted.Failure().code_;
        } else {
            QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(Read(Output), Payload);
        }
    }
    void approvedBackendRechecksEachOperation()
    {
        QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
        Started.Value()->Close(); Started.Value().reset();
        auto Inspected = InspectManifestFile(Directory.filePath("plugin.json")); QVERIFY(Inspected.HasValue());
        ApprovedWcxOptions Options; Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.manifestPath_ = Directory.filePath("plugin.json"); Options.binaryPath_ = Directory.filePath("fixture.wcx64");
        Options.approval_.reviewed_ = Inspected.Value().manifest_; Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
        const auto Path = Directory.filePath("approved.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Backend = MakeApprovedWcxBackend(Options);
        const auto Native = QDir::toNativeSeparators(Options.binaryPath_).toStdWString();
        QVERIFY(!GetModuleHandleW(Native.c_str()));
        ArchiveHeader Header;
        {
            auto Cursor = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Cursor.HasValue());
            auto Page = Cursor.Value()->Next(128, {}); QVERIFY(Page.HasValue());
            QCOMPARE(Page.Value().entries_.size(), 2u); Header = Page.Value().entries_.front();
        }
        QVERIFY(Backend->Extract(Path.toStdString(), Header, Directory.filePath("output").toStdString(), {}).HasValue());
        QCOMPARE(Read(Directory.filePath("output")), QByteArray("alpha"));
        QVERIFY(Backend->Update({Path.toStdString(), Directory.path().toStdString(), {}, {"alpha.txt"}}, {}).HasValue());
        QVERIFY(!GetModuleHandleW(Native.c_str()));
        // A backend retained by a mount must not retain an old approval after a manifest edit.
        auto Changed = Options.approval_.reviewed_; Changed.version_ = "2";
        QVERIFY(Put(Options.manifestPath_, EncodeManifest(Changed)));
        auto Denied = Backend->Enumerate(Path.toStdString(), false, {});
        QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().category_, ErrorCategory::AccessDenied);
    }
    void approvedBackendRejectsUnreviewedAndWrongBinary()
    {
        ApprovedWcxOptions Empty; auto Unreviewed = MakeApprovedWcxBackend(Empty);
        auto Denied = Unreviewed->Enumerate("missing", false, {});
        QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("plugin-review-required"));
        QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue()); Started.Value()->Close();
        auto Inspected = InspectManifestFile(Directory.filePath("plugin.json")); QVERIFY(Inspected.HasValue());
        ApprovedWcxOptions Options; Options.manifestPath_ = Directory.filePath("plugin.json");
        Options.binaryPath_ = Directory.filePath("another.wcx64"); Options.approval_.approved_ = true;
        Options.approval_.reviewed_ = Inspected.Value().manifest_; Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
        QVERIFY(QFile::copy(Directory.filePath("fixture.wcx64"), Options.binaryPath_));
        auto Wrong = MakeApprovedWcxBackend(Options)->Enumerate("missing", false, {});
        QVERIFY(!Wrong.HasValue()); QCOMPARE(Wrong.Failure().code_, std::string("plugin-configured-binary-mismatch"));
    }
    void hostedProviderDoesNotWaitForLegacyMutex()
    {
        QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
        const auto Path = Directory.filePath("parallel.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), MakeHostedArchiveBackend(Started.Value()));
        std::unique_lock Legacy(WcxCallMutex());
        auto Pending = Provider->Resolve(At(Path, "/alpha.txt"), {});
        const auto State = Pending.wait_for(std::chrono::seconds(3));
        // Always release before waiting/destruction, including a regression failure.
        Legacy.unlock(); auto Resolved = Pending.get();
        QCOMPARE(State, std::future_status::ready); QVERIFY(Resolved.HasValue());
    }
    void legacyMutexWaitCanBeCancelled()
    {
        auto Backend = MakeInProcessWcxBackend("missing.wcx64");
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        std::unique_lock Legacy(WcxCallMutex());
        auto Pending = std::async(std::launch::async, [&] { return Backend->Enumerate("missing", false, Context); });
        Stop.request_stop(); const auto State = Pending.wait_for(std::chrono::seconds(1));
        Legacy.unlock(); auto Result = Pending.get();
        QCOMPARE(State, std::future_status::ready); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::Cancelled);
    }
    void legacyModuleEndsWithCall()
    {
        QTemporaryDir Directory; const auto Binary = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        auto Backend = MakeInProcessWcxBackend(Binary.toStdString()); const auto Native = QDir::toNativeSeparators(Binary).toStdWString();
        QVERIFY(!GetModuleHandleW(Native.c_str())); const auto Path = Directory.filePath("a.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Cursor = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Cursor.HasValue()); QVERIFY(GetModuleHandleW(Native.c_str()));
        auto Page = Cursor.Value()->Next(128, {}); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_); QCOMPARE(Page.Value().entries_.size(), 2u);
        Cursor.Value().reset(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        QVERIFY(Backend->Extract(Path.toStdString(), Page.Value().entries_[0], Directory.filePath("output").toStdString(), {}).HasValue());
        QCOMPARE(Read(Directory.filePath("output")), QByteArray("alpha")); QVERIFY(!GetModuleHandleW(Native.c_str()));
        QVERIFY(Backend->Update({Path.toStdString(), Directory.path().toStdString()}, {}).HasValue()); QVERIFY(!GetModuleHandleW(Native.c_str()));
    }
    void pagingAndCleanup()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); auto Started = Start(Directory); QVERIFY(Started.HasValue());
        auto Backend = MakeHostedArchiveBackend(Started.Value()); const auto Path = Directory.filePath("목록.hct"); QVERIFY(Put(Path, Archive(300)));
        auto Cursor = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Cursor.HasValue());
        std::size_t Count = 0; unsigned Pages = 0;
        for (;;)
        {
            auto Page = Cursor.Value()->Next(256, {}); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            QVERIFY(Page.Value().entries_.size() <= 128); Count += Page.Value().entries_.size(); ++Pages;
            if (Page.Value().end_) break;
        }
        QCOMPARE(Count, 300u); QCOMPARE(Pages, 3u);
        // Early cursor destruction must release the child's 16-slot quota.
        for (int Index = 0; Index < 20; ++Index) { auto Early = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Early.HasValue()); }
        OperationContext Limited; Limited.maxEnumerationEntries_ = 2;
        auto Small = Backend->Enumerate(Path.toStdString(), false, Limited); QVERIFY(Small.HasValue());
        auto Exceeded = Small.Value()->Next(128, Limited); QVERIFY(!Exceeded.HasValue()); QCOMPARE(Exceeded.Failure().category_, ErrorCategory::BudgetExceeded);
        QVERIFY(Started.Value()->Ping().HasValue());
    }
    void unicodeReadAndStaleHeader()
    {
        QTemporaryDir Directory(QDir::tempPath() + "/호스트 읽기 XXXXXX"); auto Started = Start(Directory); QVERIFY(Started.HasValue());
        auto Backend = MakeHostedArchiveBackend(Started.Value()); const auto Path = Directory.filePath("원본.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        auto Entry = Provider->Resolve(At(Path, "/sub/한글.txt"), {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), QByteArray("한글!\r\n"));
        ArchiveHeader Wrong; Wrong.name_ = "alpha.txt"; Wrong.size_ = 99;
        auto Failed = Backend->Extract(Path.toStdString(), Wrong, Directory.filePath("bad").toStdString(), {});
        QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().category_, ErrorCategory::StaleRevision); QVERIFY(!QFile::exists(Directory.filePath("bad")));
        // The materialized stream remains usable after the host is closed.
        Started.Value()->Close(); QVERIFY(Stream.Value()->Seek(0, {}).get().HasValue()); QVERIFY(Stream.Value()->Read(100, {}).get().HasValue());
    }
    void batchCommitAndAbort_data()
    { QTest::addColumn<bool>("Commit"); QTest::newRow("commit") << true; QTest::newRow("abort") << false; }
    void batchCommitAndAbort()
    {
        QFETCH(bool, Commit); QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue());
        auto Backend = MakeHostedArchiveBackend(Started.Value()); const auto Path = Directory.filePath("batch.hct"), Input = Directory.filePath("input.txt");
        QVERIFY(Put(Path, "HCWCX01")); QVERIFY(Put(Input, "new-content")); const auto Original = Read(Path);
        auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        auto Alpha = Provider->Resolve(At(Path, "/alpha.txt"), {}).get(), Folder = Provider->Resolve(At(Path, "/sub"), {}).get(); QVERIFY(Alpha.HasValue() && Folder.HasValue());
        auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Alpha.Value().id_},
            {ArchiveChangeKind::Rename, "/sub", "/renamed", {}, Folder.Value().id_}, {ArchiveChangeKind::Put, "/added.txt", {}, Content(Input)},
            {ArchiveChangeKind::MakeDirectory, "/empty"}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(Read(Path), Original);
        if (Commit)
        {
            auto Receipt = Session.Value()->Commit({}); QVERIFY(Receipt.HasValue()); QVERIFY(Receipt.Value().cleanupErrors_.empty());
            auto Fresh = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
            QVERIFY(!Fresh->Resolve(At(Path, "/alpha.txt"), {}).get().HasValue()); QVERIFY(!Fresh->Resolve(At(Path, "/sub"), {}).get().HasValue());
            for (const auto& Name : {"/added.txt", "/renamed/한글.txt", "/empty"}) QVERIFY(Fresh->Resolve(At(Path, Name), {}).get().HasValue());
        }
        QVERIFY(Session.Value()->Abort().empty()); if (!Commit) QCOMPARE(Read(Path), Original);
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue()); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
    }
    void failurePreservesOriginal_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("Cancel");
        QTest::newRow("pack-error") << QByteArray("P") << false;
        QTest::newRow("integrity-error") << QByteArray("K") << false;
        QTest::newRow("crash-after-stage-write") << QByteArray("J") << false;
        QTest::newRow("hang-after-stage-write") << QByteArray("H") << false;
        QTest::newRow("cancel-after-stage-write") << QByteArray("H") << true;
    }
    void failurePreservesOriginal()
    {
        QFETCH(QByteArray, Mode); QFETCH(bool, Cancel); QTemporaryDir Directory; auto Started = Start(Directory, true, 700); QVERIFY(Started.HasValue());
        auto Backend = MakeHostedArchiveBackend(Started.Value()); const auto Path = Directory.filePath("batch.hct"), Input = Directory.filePath("input.txt");
        QVERIFY(Put(Path, "HCWCX01" + Mode)); QVERIFY(Put(Input, "new-content")); const auto Original = Read(Path);
        auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        const auto SignalName = L"Local\\hc-wcx-stage-" + std::to_wstring(Started.Value()->Snapshot().processId_);
        WinHandle Staged(CreateEventW(nullptr, TRUE, FALSE, SignalName.c_str())); QVERIFY(Staged.Valid());
        std::jthread Canceller([&] { if (Cancel) { WaitForSingleObject(Staged.Get(), 3000); Stop.request_stop(); } });
        QElapsedTimer Timer; Timer.start(); auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Put, "/new.txt", {}, Content(Input)}}, Context);
        QVERIFY(!Prepared.HasValue()); QVERIFY(Timer.elapsed() < 4000); QCOMPARE(Read(Path), Original);
        if (Cancel) QCOMPARE(Prepared.Failure().category_, ErrorCategory::Cancelled);
        if (Mode == "H" || Mode == "J") {
            QCOMPARE(WaitForSingleObject(Staged.Get(), 0), DWORD(WAIT_OBJECT_0));
            QTRY_VERIFY_WITH_TIMEOUT(!Started.Value()->Snapshot().running_, 2000); QVERIFY(Started.Value()->Snapshot().failure_.has_value());
            if (Mode == "J") QCOMPARE(Started.Value()->Snapshot().exitCode_.value(), DWORD(EXCEPTION_ACCESS_VIOLATION));
        }
        else QVERIFY(Started.Value()->Ping().HasValue());
        QVERIFY(!Session.Value()->Commit({}).HasValue()); QVERIFY(Session.Value()->Abort().empty());
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue()); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
    }
    void helperParentDeath()
    {
        QTemporaryDir Directory; auto Setup = Start(Directory, true, 3000, {}, true); QVERIFY(Setup.HasValue()); Setup.Value()->Close(); Setup.Value().reset();
        const auto Path = Directory.filePath("parent.hct"); QVERIFY(Put(Path, "HCWCX01E")); QVERIFY(Put(Directory.filePath("new.txt"), "new"));
        QProcess Owner; Owner.start(QCoreApplication::applicationDirPath() + "/hc_plugin_owner_fixture.exe",
            {QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Directory.filePath("plugin.json"), Path});
        QVERIFY(Owner.waitForStarted(5000)); QVERIFY(Owner.waitForReadyRead(5000));
        bool Parsed = false; const auto Pid = Owner.readLine().trimmed().toUInt(&Parsed); QVERIFY(Parsed && Pid);
        WinHandle Host(OpenProcess(SYNCHRONIZE, FALSE, Pid)); QVERIFY(Host.Valid()); WinHandle Child;
        const auto FindChild = [&] {
            WinHandle Processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)); PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
            if (Processes.Valid() && Process32FirstW(Processes.Get(), &Entry)) do {
                if (Entry.th32ParentProcessID == Pid && _wcsicmp(Entry.szExeFile, L"7zG.exe") == 0)
                { Child.Reset(OpenProcess(SYNCHRONIZE, FALSE, Entry.th32ProcessID)); break; }
            } while (Process32NextW(Processes.Get(), &Entry));
            return Child.Valid();
        };
        QTRY_VERIFY_WITH_TIMEOUT(FindChild(), 3000); Owner.kill(); QVERIFY(Owner.waitForFinished(3000));
        QCOMPARE(WaitForSingleObject(Host.Get(), 3000), DWORD(WAIT_OBJECT_0)); QCOMPARE(WaitForSingleObject(Child.Get(), 3000), DWORD(WAIT_OBJECT_0));
    }
    void helperLifetime_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("Cancel");
        QTest::newRow("completion") << QByteArray("F") << false;
        QTest::newRow("timeout") << QByteArray("E") << false;
        QTest::newRow("cancel") << QByteArray("E") << true;
        QTest::newRow("host-crash") << QByteArray("G") << false;
    }
    void helperLifetime()
    {
        QFETCH(QByteArray, Mode); QFETCH(bool, Cancel);
        QTemporaryDir Directory; auto Started = Start(Directory, true, Mode == "F" ? 500 : 2000, {}, true, 2000);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = Started.Value(); const auto Pid = Host->Snapshot().processId_;
        const auto EventName = L"Local\\hc-wcx-helper-" + std::to_wstring(Pid);
        WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, EventName.c_str())); QVERIFY(Event.Valid());
        // Pinning prevents replacement while a reviewed host can launch the EXE.
        QFile Helper(Directory.filePath("64/7zG.exe")); QVERIFY(!Helper.open(QIODevice::WriteOnly));
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        WinHandle Child;
        std::jthread Observer([&] {
            if (WaitForSingleObject(Event.Get(), 3000) != WAIT_OBJECT_0) return;
            WinHandle Processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)); PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
            if (Processes.Valid() && Process32FirstW(Processes.Get(), &Entry)) do {
                if (Entry.th32ParentProcessID == Pid && _wcsicmp(Entry.szExeFile, L"7zG.exe") == 0)
                { Child.Reset(OpenProcess(SYNCHRONIZE, FALSE, Entry.th32ProcessID)); break; }
            } while (Process32NextW(Processes.Get(), &Entry));
            if (Cancel) Stop.request_stop();
        });
        const auto Path = Directory.filePath("original.hct"); const auto Original = QByteArray("HCWCX01") + Mode; QVERIFY(Put(Path, Original));
        const auto Input = Directory.filePath("new.txt"); QVERIFY(Put(Input, "helper update"));
        auto Backend = MakeHostedArchiveBackend(Host); auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        QElapsedTimer Timer; Timer.start();
        auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Put, "/new.txt", {}, Content(Input)}}, Context);
        Observer.join(); QVERIFY(Child.Valid()); QCOMPARE(Read(Path), Original);
        if (Mode == "F") { QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Timer.elapsed() >= 650); QVERIFY(Session.Value()->Abort().empty()); }
        else { QVERIFY(!Prepared.HasValue()); if (Cancel) QCOMPARE(Prepared.Failure().category_, ErrorCategory::Cancelled); QVERIFY(!Session.Value()->Commit({}).HasValue()); }
        QVERIFY(Timer.elapsed() < 5000); Host->Close(); QCOMPARE(WaitForSingleObject(Child.Get(), 2000), DWORD(WAIT_OBJECT_0));
        QVERIFY(!Host->Snapshot().running_); QCOMPARE(Read(Path), Original);
        Host.reset(); Started.Value().reset(); Backend.reset(); Provider.reset(); Session.Value().reset();
        QVERIFY(Helper.open(QIODevice::WriteOnly));
    }
    void writeApprovalAndChunking()
    {
        QTemporaryDir DeniedDirectory; auto Denied = Start(DeniedDirectory, false); QVERIFY(Denied.HasValue());
        auto ReadOnly = MakeHostedArchiveBackend(Denied.Value());
        auto Failure = ReadOnly->Update({DeniedDirectory.filePath("a").toStdString(), DeniedDirectory.path().toStdString()}, {});
        QVERIFY(!Failure.HasValue()); QCOMPARE(Failure.Failure().category_, ErrorCategory::AccessDenied); QVERIFY(Denied.Value()->Ping().HasValue());
        QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue()); auto Backend = MakeHostedArchiveBackend(Started.Value());
        const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        ArchiveUpdate Update{Path.toStdString(), Directory.path().toStdString()};
        // A delete list larger than one pipe frame is split into verified chunks.
        for (int Index = 0; Index < 1500; ++Index) Update.remove_.push_back(std::string(900, 'x') + std::to_string(Index));
        QVERIFY(QJsonDocument(EncodeArchiveUpdate(Update)).toJson(QJsonDocument::Compact).size() > WirePayloadLimit);
        auto Applied = Backend->Update(std::move(Update), {}); QVERIFY2(Applied.HasValue(), Applied.HasValue() ? "" : Applied.Failure().code_.c_str());
        auto Cursor = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Cursor.HasValue()); auto Page = Cursor.Value()->Next(128, {});
        QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), 2u);
    }
    void invalidUploadsAreRecoverable()
    {
        QTemporaryDir Directory; auto Started = Start(Directory); QVERIFY(Started.HasValue()); auto Host = Started.Value(); const auto Context = EncodeContext({});
        auto Unknown = Host->ArchiveCall(WireKind::ArchiveNext, {{"cursor", "999"}, {"count", "1"}, {"context", Context}});
        QVERIFY(!Unknown.HasValue()); QCOMPARE(Unknown.Failure().category_, ErrorCategory::InvalidRequest);
        auto Begun = Host->ArchiveCall(WireKind::ArchiveUpdateBegin, {{"size", "3"}, {"sha256", QString(64, '0')}, {"context", Context}}); QVERIFY(Begun.HasValue());
        auto Upload = Begun.Value()["upload"];
        auto BadOffset = Host->ArchiveCall(WireKind::ArchiveUpdateData, {{"upload", Upload}, {"offset", "1"}, {"data", "YWJj"}, {"context", Context}}); QVERIFY(!BadOffset.HasValue());
        auto Wrote = Host->ArchiveCall(WireKind::ArchiveUpdateData, {{"upload", Upload}, {"offset", "0"}, {"data", "YWJj"}, {"context", Context}}); QVERIFY(Wrote.HasValue());
        auto Applied = Host->ArchiveCall(WireKind::ArchiveUpdateApply, {{"upload", Upload}, {"context", Context}}); QVERIFY(!Applied.HasValue());
        QCOMPARE(Applied.Failure().code_, std::string("host-archive-update-integrity"));
        auto Backend = MakeHostedArchiveBackend(Host); const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        QVERIFY(Backend->Update({Path.toStdString(), Directory.path().toStdString()}, {}).HasValue()); QVERIFY(Host->Ping().HasValue());
    }
    void realSevenZipPlugin()
    {
        const auto Plugin = qEnvironmentVariable("HC_WCX_PLUGIN"); if (Plugin.isEmpty()) QSKIP("Set HC_WCX_PLUGIN to the separately obtained x64 7zip WCX binary");
        QTemporaryDir Directory; auto Started = Start(Directory, true, 10000, Plugin); QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Backend = MakeHostedArchiveBackend(Started.Value()); QVERIFY(QDir().mkdir(Directory.filePath("sub")));
        QVERIFY(Put(Directory.filePath("alpha.txt"), "alpha")); QVERIFY(Put(Directory.filePath("sub/한글.txt"), "한글!\r\n"));
        const auto Path = Directory.filePath("real.7z");
        auto Created = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {"alpha.txt", "sub/한글.txt"}}, {});
        QVERIFY2(Created.HasValue(), Created.HasValue() ? "" : Created.Failure().code_.c_str());
        auto Provider = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        auto Entry = Provider->Resolve(At(Path, "/sub/한글.txt"), {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue()); auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), QByteArray("한글!\r\n"));
        const auto Original = Read(Path); auto Session = BeginWcxMutation({}, At(Path), Provider, Directory.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        auto Prepared = Session.Value()->Prepare({{ArchiveChangeKind::Rename, "/sub/한글.txt", "/changed.txt", {}, Entry.Value().id_},
            {ArchiveChangeKind::Put, "/new.txt", {}, Content(Directory.filePath("alpha.txt"))}, {ArchiveChangeKind::MakeDirectory, "/empty"}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(Read(Path), Original); QVERIFY(Session.Value()->Commit({}).HasValue());
        auto Fresh = MakeWcxReadProvider({}, At(Path), Directory.path().toStdString(), Backend);
        for (const auto& Name : {"/changed.txt", "/new.txt", "/empty"}) QVERIFY(Fresh->Resolve(At(Path, Name), {}).get().HasValue());
        QVERIFY(!Fresh->Resolve(At(Path, "/sub/한글.txt"), {}).get().HasValue());
        Started.Value()->Close(); QVERIFY(!Started.Value()->Snapshot().forcedTermination_);
    }
};
QTEST_GUILESS_MAIN(HostedWcxTests)
#include "tstHostedWcx.moc"
