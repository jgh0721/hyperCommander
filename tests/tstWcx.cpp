#include <QtTest>
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/wcxMutation.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"

using namespace hc::storage;
using namespace hc::operations;
namespace
{
    QString Fixture() { return QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64"; }
    bool Put(const QString& Path, QByteArray Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    Locator Location(const QString& Path, std::string Entry = "/") { return {LocatorKind::Archive, Path.toStdString(), {{"wcx", std::move(Entry)}}}; }
    QByteArray Bytes(const ReadChunk& Chunk) { return QByteArray(reinterpret_cast<const char*>(Chunk.bytes_.data()), Chunk.bytes_.size()); }
    int __stdcall Continue(wchar_t*, int) { return 1; }
    QByteArray FileBytes(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    std::shared_ptr<IContentSource> Content(const QString& Path)
    {
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Reader->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get();
        return Entry.HasValue() ? std::make_shared<ProviderContentSource>(Reader, Entry.Value()) : nullptr;
    }
    class FailedDestination final : public IWritableStorage
    {
        class Session final : public IWriteSession
        {
        public:
            Session(std::unique_ptr<IWriteSession> Inner, bool CommitFailure) : inner_(std::move(Inner)), commitFailure_(CommitFailure) {}
            Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override {
                const auto Wrote = inner_->Append(Bytes, Context); if (!Wrote.HasValue()) return Wrote;
                return commitFailure_ ? Result<Unit>(Unit{}) : Result<Unit>(Error{ErrorCategory::Unavailable, "injected-archive-disk-full"});
            }
            Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) override { return inner_->SetMetadata(Metadata, Context); }
            Result<WriteReceipt> Commit(OperationContext) override { return Error{ErrorCategory::Unavailable, "injected-archive-commit-failure"}; }
            Result<Unit> Abort() override { return inner_->Abort(); }
        private:
            std::unique_ptr<IWriteSession> inner_;
            bool commitFailure_;
        };
    public:
        explicit FailedDestination(bool CommitFailure) : inner_(MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local))), commitFailure_(CommitFailure) {}
        ProviderId Id() const override { return inner_->Id(); }
        Result<SupportDecision> Supports(Mutation Op, Locator At, OperationContext Context) override { return inner_->Supports(Op, At, Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return inner_->Inspect(At, Context); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override {
            auto Result = inner_->BeginWrite(At, Expected, Context); if (!Result.HasValue()) return Result.Failure();
            return std::unique_ptr<IWriteSession>(new Session(std::move(Result.Value()), commitFailure_));
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { return inner_->MakeDirectory(At, Context); }
        Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override { return inner_->Remove(Entry, Context); }
        Result<Unit> Rename(EntrySnapshot Entry, Locator At, OperationContext Context) override { return inner_->Rename(Entry, At, Context); }
    private:
        std::shared_ptr<IWritableStorage> inner_;
        bool commitFailure_;
    };
}
class WcxTests : public QObject
{
    Q_OBJECT
private slots:
    void mutationReplaceAndRemoveLastImplicitChild()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "changed"));
        auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        auto Alpha = Reader->Resolve(Location(Path, "/alpha.txt"), {}).get(); QVERIFY(Alpha.HasValue());
        auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/alpha.txt", {}, Content(Input), {}, {true, Alpha.Value().id_.revision_}}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit({}).HasValue());
        Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        Alpha = Reader->Resolve(Location(Path, "/alpha.txt"), {}).get(); QVERIFY(Alpha.HasValue()); QCOMPARE(Alpha.Value().size_, 7u);
        auto Child = Reader->Resolve(Location(Path, "/sub/한글.txt"), {}).get(); QVERIFY(Child.HasValue());
        Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
        Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Alpha.Value().id_},
            {ArchiveChangeKind::Remove, "/sub/한글.txt", {}, {}, Child.Value().id_}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit({}).HasValue());
        Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        auto Cursor = Reader->Enumerate(Location(Path), {}).get(); QVERIFY(Cursor.HasValue()); const auto Page = Cursor.Value()->Next(100, {}).get();
        QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.empty());
    }
    void mutationDestinationFailures()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Original = FileBytes(Path); const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "new-content"));
        for (bool CommitFailure : {false, true})
        {
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString(), {}, {}, std::make_shared<FailedDestination>(CommitFailure));
            QVERIFY(Batch.HasValue()); const auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.bin", {}, Content(Input)}}, {});
            if (CommitFailure) { QVERIFY(Prepared.HasValue()); const auto Failed = Batch.Value()->Commit({}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().code_, "injected-archive-commit-failure"); }
            else { QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, "injected-archive-disk-full"); }
            QCOMPARE(FileBytes(Path), Original); QVERIFY(Batch.Value()->Abort().empty());
            QCOMPARE(QDir(Directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
        }
    }
    void mutationPathAndBudgetGuards()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "new-content"));
        const auto Original = FileBytes(Path);
        for (const auto& Name : {"/../escape", "/CON.txt", "/bad:stream", "/bad.", "/alpha.txt"})
        {
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
            const auto Failed = Batch.Value()->Prepare({{ArchiveChangeKind::Put, Name, {}, Content(Input)}}, {}); QVERIFY(!Failed.HasValue());
            QCOMPARE(FileBytes(Path), Original);
        }
        for (int Kind = 0; Kind < 3; ++Kind)
        {
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
            ArchiveMutationLimits Limits; if (Kind == 0) Limits.maxInputBytes_ = 10; if (Kind == 1) Limits.maxArchiveBytes_ = 20;
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString(), Limits); QVERIFY(Batch.HasValue());
            std::stop_source Stop; OperationContext Context; if (Kind == 2) { Stop.request_stop(); Context.stopToken_ = Stop.get_token(); }
            const auto Failed = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.bin", {}, Content(Input)}}, Context); QVERIFY(!Failed.HasValue());
            QCOMPARE(Failed.Failure().category_, Kind == 2 ? ErrorCategory::Cancelled : ErrorCategory::BudgetExceeded); QCOMPARE(FileBytes(Path), Original);
        }
    }
    void mutationBatchPublishesOnlyAfterCommit()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Original = FileBytes(Path); const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "new-content"));
        auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        const auto Alpha = Reader->Resolve(Location(Path, "/alpha.txt"), {}).get(); QVERIFY(Alpha.HasValue());
        const auto Folder = Reader->Resolve(Location(Path, "/sub"), {}).get(); QVERIFY(Folder.HasValue());
        auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
        std::vector<ArchiveChange> Changes;
        Changes.push_back({ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Alpha.Value().id_});
        Changes.push_back({ArchiveChangeKind::Rename, "/sub", "/renamed", {}, Folder.Value().id_});
        Changes.push_back({ArchiveChangeKind::Put, "/added.bin", {}, Content(Input)});
        Changes.push_back({ArchiveChangeKind::MakeDirectory, "/empty"});
        const auto Prepared = Batch.Value()->Prepare(std::move(Changes), {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
        QCOMPARE(FileBytes(Path), Original);
        const auto Committed = Batch.Value()->Commit({}); QVERIFY(Committed.HasValue()); QVERIFY(Committed.Value().cleanupErrors_.empty());
        QVERIFY(!Batch.Value()->Commit({}).HasValue()); QVERIFY(Batch.Value()->Abort().empty());
        auto Updated = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        QVERIFY(!Updated->Resolve(Location(Path, "/alpha.txt"), {}).get().HasValue());
        QVERIFY(!Updated->Resolve(Location(Path, "/sub"), {}).get().HasValue());
        QVERIFY(Updated->Resolve(Location(Path, "/empty"), {}).get().HasValue());
        for (const auto& [Name, Expected] : std::vector<std::pair<std::string, QByteArray>>{{"/added.bin", "new-content"}, {"/renamed/한글.txt", "한글!\r\n"}})
        {
            const auto Entry = Updated->Resolve(Location(Path, Name), {}).get(); QVERIFY(Entry.HasValue());
            auto Stream = Updated->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
            auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Bytes(Read.Value()), Expected);
        }
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue()); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
    }
    void mutationFailuresPreserveOriginal_data()
    {
        QTest::addColumn<QByteArray>("Mode");
        QTest::newRow("pack-error-after-write") << QByteArray("P");
        QTest::newRow("delete-error-after-write") << QByteArray("X");
        QTest::newRow("integrity-test-failure") << QByteArray("K");
    }
    void mutationFailuresPreserveOriginal()
    {
        QFETCH(QByteArray, Mode); QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01" + Mode));
        const auto Original = FileBytes(Path); const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "new-content"));
        auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        const auto Alpha = Reader->Resolve(Location(Path, "/alpha.txt"), {}).get(); QVERIFY(Alpha.HasValue());
        auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
        const auto Failed = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.bin", {}, Content(Input)},
            {ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Alpha.Value().id_}}, {});
        QVERIFY(!Failed.HasValue()); QCOMPARE(FileBytes(Path), Original); QVERIFY(!Batch.Value()->Commit({}).HasValue()); QVERIFY(Batch.Value()->Abort().empty());
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue()); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
    }
    void mutationAbortCancelAndStaleCommit()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Input = Directory.filePath("new.bin"); QVERIFY(Put(Input, "new-content"));
        const auto Original = FileBytes(Path);
        for (int Mode = 0; Mode < 3; ++Mode)
        {
            QVERIFY(Put(Path, Original));
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
            const auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.bin", {}, Content(Input)}}, {});
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
            if (Mode == 0) QVERIFY(Batch.Value()->Abort().empty());
            if (Mode == 1) { std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
                auto Cancelled = Batch.Value()->Commit(Context); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled); }
            if (Mode == 2) { QVERIFY(Put(Path, "HCWCX01changed")); const auto Failed = Batch.Value()->Commit({}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().category_, ErrorCategory::StaleRevision); }
            QVERIFY(Batch.Value()->Abort().empty()); QCOMPARE(FileBytes(Path), Mode == 2 ? QByteArray("HCWCX01changed") : Original);
        }
        QVERIFY(QDir(Directory.path()).entryList({"*.hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void unicodeReadCopyAndLifetime()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        auto Cursor = Provider->Enumerate(Location(Path), {}).get(); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
        const auto Page = Cursor.Value()->Next(10, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), 2u);
        auto Entry = Provider->Resolve(Location(Path, "/sub/한글.txt"), {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        const auto Chunk = Stream.Value()->Read(32, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("한글!\r\n"));
        QVERIFY(Stream.Value()->Seek(0, {}).get().HasValue());
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue());
        auto Other = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        QVERIFY(!Other->Stat(Entry.Value().id_, {}).get().HasValue());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).size() == 1);
        Stream.Value().reset(); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
        const auto Destination = Directory.filePath("out"); QVERIFY(QDir().mkdir(Destination));
        auto Native = MakeNativeReadProvider(LocatorKind::Local);
        FileOperationRequest Request; Request.destination_ = {LocatorKind::Local, Destination.toStdString()};
        Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Native);
        auto Folder = Provider->Resolve(Location(Path, "/sub"), {}).get(); QVERIFY(Folder.HasValue());
        Request.sources_.push_back({Folder.Value(), Provider, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Job = SubmitFileOperation(Scheduler, std::move(Request)); QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Completed);
        QFile Copied(Destination + "/sub/한글.txt"); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("한글!\r\n"));
        QVERIFY(Put(Path, "changed archive"));
        const auto Stale = Provider->Stat(Entry.Value().id_, {}).get(); QVERIFY(!Stale.HasValue()); QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
    }
    void failures_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("MountFailure");
        QTest::newRow("traversal") << QByteArray("T") << true;
        QTest::newRow("duplicate") << QByteArray("D") << true;
        QTest::newRow("crc") << QByteArray("C") << false;
        QTest::newRow("short-output") << QByteArray("S") << false;
        QTest::newRow("split-volume") << QByteArray("V") << false;
    }
    void failures()
    {
        QFETCH(QByteArray, Mode); QFETCH(bool, MountFailure);
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01" + Mode));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        const auto Entry = Provider->Resolve(Location(Path, "/alpha.txt"), {}).get();
        if (MountFailure) QVERIFY(!Entry.HasValue());
        else { QVERIFY(Entry.HasValue()); const auto Read = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(!Read.HasValue()); }
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue());
        QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
        QVERIFY(!QFile::exists(Directory.filePath("escape.txt")));
    }
    void cancellationAndEntryBudget()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        const auto Cancelled = Provider->Enumerate(Location(Path), Context).get(); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        Context = {}; Context.maxEnumerationEntries_ = 1;
        const auto Bounded = Provider->Enumerate(Location(Path), Context).get(); QVERIFY(!Bounded.HasValue()); QCOMPARE(Bounded.Failure().category_, ErrorCategory::BudgetExceeded);
        QVERIFY(Provider->Enumerate(Location(Path), {}).get().HasValue());
    }
    void realSevenZipPlugin()
    {
        const auto Plugin = qEnvironmentVariable("HC_WCX_PLUGIN"); if (Plugin.isEmpty()) QSKIP("Set HC_WCX_PLUGIN to the separately obtained x64 7zip WCX binary");
        QTemporaryDir Directory; QVERIFY(QDir().mkdir(Directory.filePath("sub")));
        QVERIFY(Put(Directory.filePath("alpha.txt"), "alpha")); QVERIFY(Put(Directory.filePath("sub/한글.txt"), "한글!\r\n"));
        const auto Module = LoadLibraryExW(QDir::toNativeSeparators(Plugin).toStdWString().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        QVERIFY(Module); const auto Cleanup = qScopeGuard([&] { FreeLibrary(Module); });
        const auto Pack = reinterpret_cast<int (__stdcall*)(wchar_t*, wchar_t*, wchar_t*, wchar_t*, int)>(GetProcAddress(Module, "PackFilesW"));
        const auto Progress = reinterpret_cast<void (__stdcall*)(HANDLE, tProcessDataProcW)>(GetProcAddress(Module, "SetProcessDataProcW"));
        QVERIFY(Pack); QVERIFY(Progress); Progress(nullptr, &Continue);
        auto Archive = QDir::toNativeSeparators(Directory.filePath("real.7z")).toStdWString();
        auto Source = QDir::toNativeSeparators(Directory.path() + "/").toStdWString();
        wchar_t List[] = L"alpha.txt\0sub\\한글.txt\0";
        QCOMPARE(Pack(Archive.data(), nullptr, Source.data(), List, PK_PACK_SAVE_PATHS), 0);
        auto Provider = MakeWcxReadProvider(Plugin.toStdString(), Location(QString::fromStdWString(Archive)), Directory.path().toStdString());
        auto Entry = Provider->Resolve(Location(QString::fromStdWString(Archive), "/sub/한글.txt"), {}).get();
        QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Chunk = Stream.Value()->Read(32, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("한글!\r\n"));
        const auto Mutate = [&](const QString& ArchivePath) {
            auto Read = MakeWcxReadProvider(Plugin.toStdString(), Location(ArchivePath), Directory.path().toStdString());
            auto Alpha = Read->Resolve(Location(ArchivePath, "/alpha.txt"), {}).get(); QVERIFY(Alpha.HasValue());
            auto Folder = Read->Resolve(Location(ArchivePath, "/sub"), {}).get(); QVERIFY(Folder.HasValue());
            const auto Original = FileBytes(ArchivePath);
            auto Batch = BeginWcxMutation(Plugin.toStdString(), Location(ArchivePath), Read, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
            const auto Prepared = Batch.Value()->Prepare({
                {ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Alpha.Value().id_},
                {ArchiveChangeKind::Rename, "/sub", "/renamed", {}, Folder.Value().id_},
                {ArchiveChangeKind::Put, "/new.txt", {}, Content(Directory.filePath("alpha.txt"))},
                {ArchiveChangeKind::MakeDirectory, "/empty"}}, {});
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
            QCOMPARE(FileBytes(ArchivePath), Original);
            auto Committed = Batch.Value()->Commit({}); QVERIFY(Committed.HasValue()); QVERIFY(Committed.Value().cleanupErrors_.empty());
            auto Updated = MakeWcxReadProvider(Plugin.toStdString(), Location(ArchivePath), Directory.path().toStdString());
            QVERIFY(Updated->Resolve(Location(ArchivePath, "/empty"), {}).get().HasValue());
            QVERIFY(!Updated->Resolve(Location(ArchivePath, "/alpha.txt"), {}).get().HasValue());
            auto Renamed = Updated->Resolve(Location(ArchivePath, "/renamed/한글.txt"), {}).get(); QVERIFY(Renamed.HasValue());
            auto Content = Updated->OpenRead(Renamed.Value().id_, {}).get(); QVERIFY(Content.HasValue());
            auto BytesRead = Content.Value()->Read(32, {}).get(); QVERIFY(BytesRead.HasValue()); QCOMPARE(Bytes(BytesRead.Value()), QByteArray("한글!\r\n"));
            auto Cursor = Updated->Enumerate(Location(ArchivePath), {}).get(); QVERIFY(Cursor.HasValue());
            auto Page = Cursor.Value()->Next(100, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_);
            std::vector<ArchiveChange> RemoveAll;
            for (const auto& Item : Page.Value().entries_) RemoveAll.push_back({ArchiveChangeKind::Remove, Item.locator_.nesting_[0].entry_, {}, {}, Item.id_});
            auto EmptyBatch = BeginWcxMutation(Plugin.toStdString(), Location(ArchivePath), Updated, Directory.path().toStdString()); QVERIFY(EmptyBatch.HasValue());
            auto Emptied = EmptyBatch.Value()->Prepare(std::move(RemoveAll), {}); QVERIFY2(Emptied.HasValue(), Emptied.HasValue() ? "" : Emptied.Failure().code_.c_str());
            QVERIFY(EmptyBatch.Value()->Commit({}).HasValue());
            // A successful pack must still respect the read/materialization
            // expansion policy before its output can replace the original.
            const auto Compressible = Directory.filePath("compressible.bin"); QVERIFY(Put(Compressible, QByteArray(1024 * 1024, 'x')));
            const auto EmptyBytes = FileBytes(ArchivePath);
            auto EmptyReader = MakeWcxReadProvider(Plugin.toStdString(), Location(ArchivePath), Directory.path().toStdString());
            auto Limited = BeginWcxMutation(Plugin.toStdString(), Location(ArchivePath), EmptyReader, Directory.path().toStdString()); QVERIFY(Limited.HasValue());
            auto Expansion = Limited.Value()->Prepare({{ArchiveChangeKind::Put, "/compressible.bin", {}, ::Content(Compressible)}}, {});
            QVERIFY(!Expansion.HasValue()); QCOMPARE(Expansion.Failure().category_, ErrorCategory::BudgetExceeded); QCOMPARE(FileBytes(ArchivePath), EmptyBytes);
        };
        const auto SmbRoot = qEnvironmentVariable("HC_WCX_SMB_ROOT");
        if (!SmbRoot.isEmpty())
        {
            QTemporaryDir Remote(QDir(SmbRoot).filePath("hc-wcx-XXXXXX")); QVERIFY(Remote.isValid());
            const auto RemoteArchive = Remote.filePath("real.7z"); QVERIFY(QFile::copy(QString::fromStdWString(Archive), RemoteArchive));
            auto RemoteProvider = MakeWcxReadProvider(Plugin.toStdString(), Location(RemoteArchive), Directory.path().toStdString());
            auto RemoteEntry = RemoteProvider->Resolve(Location(RemoteArchive, "/sub/한글.txt"), {}).get();
            QVERIFY2(RemoteEntry.HasValue(), RemoteEntry.HasValue() ? "" : RemoteEntry.Failure().code_.c_str());
            auto RemoteStream = RemoteProvider->OpenRead(RemoteEntry.Value().id_, {}).get();
            QVERIFY2(RemoteStream.HasValue(), RemoteStream.HasValue() ? "" : RemoteStream.Failure().code_.c_str());
            auto RemoteChunk = RemoteStream.Value()->Read(32, {}).get(); QVERIFY(RemoteChunk.HasValue()); QCOMPARE(Bytes(RemoteChunk.Value()), QByteArray("한글!\r\n"));
            Mutate(RemoteArchive);
        }
        Mutate(QString::fromStdWString(Archive));
    }
};
QTEST_GUILESS_MAIN(WcxTests)
#include "tstWcx.moc"
