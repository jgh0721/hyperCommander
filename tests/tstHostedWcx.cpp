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
