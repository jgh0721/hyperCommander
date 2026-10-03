#define NOMINMAX
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include "BuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "BuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include "BuiltInPlugin/SevenZip/sevenZipJob.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "pluginHost/archiveCodec.hpp"
#include <Windows.h>

using namespace hc::storage;
using namespace hc::plugins;
namespace {
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
OperationContext PasswordContext(const QString& Password) {
    OperationContext Context; Context.passwordPrompt_ = [Password](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> {
        return hc::storage::Password(Password.toStdString());
    }; return Context;
}
}
class SevenZipTests : public QObject {
    Q_OBJECT
    QJsonArray cases_;
    QString root_;
private slots:
    void creation_data() {
        QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Solid"); QTest::addColumn<bool>("Encrypt"); QTest::addColumn<unsigned>("Level");
        for (const auto Format : {QString("7z"), QString("zip")}) for (const auto Level : {0u, 5u, 9u})
            QTest::newRow(qPrintable(Format + QString::number(Level))) << Format << false << false << Level;
        QTest::newRow("solid") << QString("7z") << true << false << 5u;
        QTest::newRow("7z-aes") << QString("7z") << true << true << 5u;
        QTest::newRow("zip-aes") << QString("zip") << false << true << 5u;
    }
    void creation() {
        QFETCH(QString, Format); QFETCH(bool, Solid); QFETCH(bool, Encrypt); QFETCH(unsigned, Level);
        QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input"))); QVERIFY(QDir().mkdir(Dir.filePath("input/empty")));
        const auto Data = QByteArray(2 * 1024 * 1024, 'z'); QVERIFY(Put(Dir.filePath("input/한글.txt"), Data)); QVERIFY(Put(Dir.filePath("input/zero"), {})); QVERIFY(Put(Dir.filePath("input/second"), "second payload"));
        const auto Target = Dir.filePath("result." + Format);
        ArchiveUpdate Update{Target.toStdString(), Dir.filePath("input").toStdString(), {"한글.txt", "zero", "empty/", "second"}};
        Update.encrypt_ = Encrypt; Update.creation_ = ArchiveCreationOptions{Level, Solid};
        const auto Wire = EncodeArchiveUpdate(Update); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().creation_->level_, Level);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Context = PasswordContext("secret");
        auto Done = Backend->Update(Update, Context); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, Context); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        auto Page = Listed.Value()->Next(16, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); QCOMPARE(Page.Value().entries_.size(), size_t(4));
        Listed.Value().reset(); bool Found = false;
        for (const auto& H : Page.Value().entries_) if (H.name_ == "한글.txt") {
            Found = true; QCOMPARE(H.encrypted_, Encrypt); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), Context).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Data);
        }
        QVERIFY(Found); QVERIFY(!GetModuleHandleW(L"7z.dll"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()) + "." + Format), Read(Target))); }
    }
    void updateAndFailureAtomicity_data() { QTest::addColumn<QString>("Format"); QTest::newRow("7z") << QString("7z"); QTest::newRow("zip") << QString("zip"); }
    void updateAndFailureAtomicity() {
        QFETCH(QString, Format); QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input")));
        QVERIFY(Put(Dir.filePath("input/keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("input/remove"), "remove"));
        const auto Target = Dir.filePath("result." + Format); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.filePath("input").toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, false};
        auto Made = Backend->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
        const auto Before = Read(Target); QVERIFY(Put(Dir.filePath("input/new"), "new payload"));
        ArchiveUpdate Change{Target.toStdString(), Dir.filePath("input").toStdString(), {"new"}, {"remove"}};
        auto Limited = Change; Limited.maxOutputBytes_ = 10; auto Failed = Backend->Update(Limited, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Before);
        std::stop_source Stop; Stop.request_stop(); OperationContext Cancel; Cancel.stopToken_ = Stop.get_token(); QVERIFY(!Backend->Update(Change, Cancel).HasValue()); QCOMPARE(Read(Target), Before);
        auto Bad = Change; Bad.add_ = {"../escape"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        Bad = Change; Bad.add_ = {"KEEP"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(2)); List.Value().reset();
        for (const auto& H : Page.Value().entries_) { QVERIFY(H.name_ != "remove"); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Read(Dir.filePath("input/" + QString::fromStdString(H.name_)))); }
        ArchiveUpdate Empty{Target.toStdString(), Dir.filePath("input").toStdString(), {}, {"new", "keep"}};
        Done = Backend->Update(Empty, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.empty());
    }
    void unsupportedUpdatesPreserveOriginal() {
        QTemporaryDir Dir; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        QVERIFY(Put(Dir.filePath("new"), "new"));
        for (const auto* Name : {"encrypted.7z", "aes.zip", "test_read_format_rar5_compressed.rar", "split.7z.001"}) {
            const auto Source = QDir(root_).filePath(Name); if (!QFileInfo::exists(Source)) continue;
            const auto Target = Dir.filePath(Name); QVERIFY(QFile::copy(Source, Target)); const auto Before = Read(Target);
            auto Result = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, PasswordContext("secret"));
            QVERIFY(!Result.HasValue()); QCOMPARE(Read(Target), Before);
        }
    }
    void solidUpdatePreservation() {
        QTemporaryDir Dir; QVERIFY(Put(Dir.filePath("keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("remove"), "remove")); QVERIFY(Put(Dir.filePath("new"), "new"));
        const auto Target = Dir.filePath("solid.7z"); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, true};
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        // Appending preserves the original block and its stored dictionary.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        const auto Before = Read(Target);
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"remove"}}, {});
        QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), Before);
    }
    void zipCommentsAndExtraFields() {
        QTemporaryDir Dir; const auto Target = Dir.filePath("comments.zip");
        // Stored ZIP with archive/file comments and an opaque 0xCAFE extra field
        // in both headers. This fixture is independent of the SDK writer.
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAAgAa2VlcC50eHT+ygQAQUJDRHByZXNlcnZlZCBwYXlsb2FkUEsBAhQAFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAgACAANAAAAAAAgAAAAAAAAAGtlZXAudHh0/soEAEFCQ0RlbnRyeSBjb21tZW50UEsFBgAAAAABAAEASwAAAD8AAAAPAGFyY2hpdmUgY29tbWVudA==");
        QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("new"), "new")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        const auto After = Read(Target); QVERIFY(After.endsWith("archive comment")); QVERIFY(After.contains("entry comment"));
        QCOMPARE(After.count(QByteArray::fromHex("feca040041424344")), 2); QVERIFY(After.contains("preserved payload"));
        // The generic rename request cannot express auxiliary metadata transfer.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}, {"keep.txt", "new"}}, {});
        QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), After);
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("comments.zip"), After)); }
    }
    void schedulerPublication() {
        using namespace hc::operations;
        QTemporaryDir Dir; const auto Source = Dir.filePath("source"), Target = Dir.filePath("result.zip"); QVERIFY(Put(Source, QByteArray(1024, 'a')));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Reader->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        hc::sevenzip::CreateRequest Request; Request.sources_.push_back({Entry.Value(), Reader, {}}); Request.destination_ = {LocatorKind::Local, Target.toStdString()}; Request.format_ = "zip";
        const auto Folder = Dir.filePath("folder"); QVERIFY(QDir().mkdir(Folder)); QVERIFY(Put(Folder + "/child", "child"));
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Folder).utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Ticks = (uint64_t(1577934246000LL) + 11644473600000ULL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
        const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle); QVERIFY(Saved);
        auto FolderEntry = Reader->Resolve({LocatorKind::Local, Folder.toStdString()}, {}).get(); QVERIFY(FolderEntry.HasValue()); Request.sources_.push_back({FolderEntry.Value(), Reader, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue());
        auto Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        auto Result = Job.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "job state"); QVERIFY(Result.cleanupErrors_.empty());
        const auto Before = Read(Target); QVERIFY(!Before.isEmpty());
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); bool DirectorySeen = false;
        for (const auto& Header : Page.Value().entries_) if (Header.directory_) { QCOMPARE(Header.modifiedUtcMs_, FolderEntry.Value().modifiedUtcMs_); DirectorySeen = true; }
        QVERIFY(DirectorySeen); Listed.Value().reset();
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 5000); QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(Read(Target), Before);
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        const auto Waiting = Scheduler.Snapshot(Job.Value().id_).Value(); QVERIFY(Put(Target, "external change"));
        QVERIFY(Scheduler.Reply(Job.Value().id_, {Waiting.interaction_->id_, InteractionChoice::Replace, {}}).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        Result = Job.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Read(Target), QByteArray("external change"));
    }
    void initTestCase() {
        const auto Path = qEnvironmentVariable("HC_SEVENZIP_CORPUS");
        QVERIFY2(QFileInfo::exists(Path), "Build hc_sevenzip_corpus or set HC_SEVENZIP_CORPUS to the generated corpus.json");
        cases_ = QJsonDocument::fromJson(Read(Path))["cases"].toArray(); QVERIFY(cases_.size() >= 17); root_ = QFileInfo(Path).absolutePath();
    }
    void corpus_data() {
        QTest::addColumn<int>("Row");
        for (int Index = 0; Index < cases_.size(); ++Index) QTest::newRow(QFileInfo(cases_[Index].toObject()["archive"].toString()).fileName().toUtf8()) << Index;
    }
    void corpus() {
        QFETCH(int, Row); const auto Case = cases_[Row].toObject(); const auto Path = Case["archive"].toString();
        QTemporaryDir Output; auto Options = BuiltinSevenZipOptions(); auto Context = PasswordContext(Case["password"].toString());
        auto Backend = MakeApprovedWcxBackend(Options); auto Listed = Backend->Enumerate(Path.toStdString(), false, Context);
        QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(2, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            for (const auto& Header : Page.Value().entries_) Headers.push_back(Header); if (Page.Value().end_) break; }
        Listed.Value().reset(); QVERIFY(!Headers.empty());
        size_t Files = 0;
        for (const auto& Header : Headers) {
            if (Header.directory_) continue;
            const auto PathTo = Output.filePath("selected.bin"); auto Done = Backend->Extract(Path.toStdString(), Header, PathTo.toStdString(), Context);
            if (Header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) { QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); continue; }
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Expected = QDir(Case["expected"].toString()).filePath(QString::fromStdString(Header.name_));
            QVERIFY2(QFileInfo::exists(Expected), qPrintable(Expected)); QCOMPARE(Read(PathTo), Read(Expected)); ++Files;
        }
        QVERIFY(Files > 0);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY(Tested.HasValue());
        for (;;) { auto Page = Tested.Value()->Next(64, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); if (Page.Value().end_) break; }
        QVERIFY(!Backend->Update({Path.toStdString(), Output.path().toStdString()}, Context).HasValue());
        QVERIFY(!GetModuleHandleW(L"7z.dll")); // The application never loads archive engine code.
    }
    void errorsCancellationAndPolicy() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        const auto Plain = QDir(root_).filePath("plain.7z").toStdString(); const auto Encrypted = QDir(root_).filePath("encrypted.7z").toStdString();
        auto Required = Backend->Enumerate(Encrypted, false, {}); QVERIFY(!Required.HasValue()); QCOMPARE(Required.Failure().category_, ErrorCategory::AccessDenied);
        auto Wrong = Backend->Enumerate(Encrypted, false, PasswordContext("wrong")); QVERIFY(!Wrong.HasValue());
        auto Declined = PasswordContext(""); Declined.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> { return Error{ErrorCategory::Cancelled, "test-cancel-password"}; };
        auto Cancelled = Backend->Enumerate(Encrypted, false, Declined); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        auto Correct = Backend->Enumerate(Encrypted, false, PasswordContext("secret")); QVERIFY(Correct.HasValue()); Correct.Value().reset();
        std::stop_source Stop; Stop.request_stop(); OperationContext Stopped; Stopped.stopToken_ = Stop.get_token();
        auto Early = Backend->Enumerate(Plain, false, Stopped); QVERIFY(!Early.HasValue()); QCOMPARE(Early.Failure().category_, ErrorCategory::Cancelled);
        OperationContext Limited; Limited.maxEnumerationEntries_ = 1; QVERIFY(!Backend->Enumerate(Plain, false, Limited).HasValue());
        QTemporaryDir Directory; const auto Broken = Directory.filePath("broken.7z"); QVERIFY(Put(Broken, Read(QString::fromStdString(Plain)).left(48)));
        auto Bad = Backend->Enumerate(Broken.toStdString(), true, {});
        if (Bad.HasValue()) QVERIFY(!Bad.Value()->Next(256, {}).HasValue());
        auto Listed = Backend->Enumerate(Plain, false, {}); QVERIFY(Listed.HasValue()); std::optional<ArchiveHeader> Chosen;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue()); for (const auto& Item : Page.Value().entries_) if (Item.name_ == "sub/zeros.bin") Chosen = Item; if (Page.Value().end_) break; }
        QVERIFY(Chosen.has_value()); Listed.Value().reset();
        OperationContext Automatic; Automatic.workload_.intent_ = WorkloadIntent::Automatic; Automatic.workload_.automaticFileBytes_ = 1024;
        auto Blocked = Backend->Extract(Plain, *Chosen, Directory.filePath("auto").toStdString(), Automatic); QVERIFY(!Blocked.HasValue()); QCOMPARE(Blocked.Failure().category_, ErrorCategory::BudgetExceeded);
        std::stop_source During; OperationContext Active; Active.stopToken_ = During.get_token(); Active.progress_ = [&](uint64_t Done) { if (Done > 1024 * 1024) During.request_stop(); };
        // Drive the native callback synchronously: the 8 MiB fixture can finish
        // before the host's 50 ms progress notification interval on fast disks.
        const auto Module = LoadLibraryExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(BuiltinSevenZipOptions().binaryPath_).utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        QVERIFY(Module); auto Unload = qScopeGuard([&] { FreeLibrary(Module); }); auto Native = MakeSevenZipBackend(Module);
        auto Interrupted = Native->Extract(Plain, *Chosen, Directory.filePath("cancelled").toStdString(), Active); QVERIFY(!Interrupted.HasValue());
        QCOMPARE(Interrupted.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!QFileInfo::exists(Directory.filePath("cancelled")));
    }
    void corruptedPayload() {
        QTemporaryDir Directory; auto Bytes = Read(QDir(root_).filePath("plain.7z")); QVERIFY(Bytes.size() > 128);
        // Damage the packed payload while preserving the start and next headers.
        Bytes[64] = char(static_cast<unsigned char>(Bytes[64]) ^ 0x80);
        const auto Path = Directory.filePath("corrupt.7z"); QVERIFY(Put(Path, Bytes));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listed = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Listed.HasValue());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue());
            for (const auto& Header : Page.Value().entries_) if (!Header.directory_) Headers.push_back(Header);
            if (Page.Value().end_) break; }
        Listed.Value().reset(); bool Failed = false;
        for (const auto& Header : Headers) {
            const auto Output = Directory.filePath("output.bin");
            auto Done = Backend->Extract(Path.toStdString(), Header, Output.toStdString(), {});
            if (!Done.HasValue()) { Failed = true; QVERIFY(!QFileInfo::exists(Output)); }
        }
        QVERIFY(Failed);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, {}); QVERIFY(Tested.HasValue());
        QVERIFY(!Tested.Value()->Next(128, {}).HasValue());
    }
    void commonProviderAndSplitMissing() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); QTemporaryDir Cache;
        const auto Path = QDir(root_).filePath("solid.7z"); Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}};
        auto Provider = MakeWcxReadProvider({}, Root, Cache.path().toStdString(), Backend);
        auto At = Root; At.nesting_.back().entry_ = "/payload.bin"; auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        QVERIFY(Stream.Value()->CanSeek()); QVERIFY(Stream.Value()->Seek(100, {}).get().HasValue()); auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), Read(QDir(root_).filePath("input/payload.bin")).mid(100, 100));
        QTemporaryDir Missing;
        for (const auto* Name : {"split.7z.001", "test_read_format_rar5_multiarchive.part01.rar"}) {
            QVERIFY(QFile::copy(QDir(root_).filePath(Name), Missing.filePath(Name)));
            auto Bad = Backend->Enumerate(Missing.filePath(Name).toStdString(), true, {});
            if (Bad.HasValue()) { bool Failed = false; for (;;) { auto Page = Bad.Value()->Next(128, {}); if (!Page.HasValue()) { Failed = true; break; } if (Page.Value().end_) break; } QVERIFY(Failed); }
        }
    }
};
QTEST_GUILESS_MAIN(SevenZipTests)
#include "tstSevenZip.moc"
