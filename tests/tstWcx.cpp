#include <QtTest>
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/wcxMutation.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/moveOperation.hpp"
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
    QByteArray NestedFixture(const QByteArray& Inner, char Mode = 0)
    {
        QByteArray Data("HCWCX02", 7); Data += Mode;
        const auto Number = [&](quint32 Value) { Data.append(reinterpret_cast<const char*>(&Value), 4); };
        Number(2);
        for (const auto& Pair : {qMakePair(QString("inner.hct"), Inner), qMakePair(QString("sibling.txt"), QByteArray("keep"))}) {
            Number(Pair.first.size()); Number(Pair.second.size()); Number(0);
            Data.append(reinterpret_cast<const char*>(Pair.first.utf16()), Pair.first.size() * 2); Data += Pair.second;
        }
        return Data;
    }
    std::shared_ptr<IContentSource> Content(const QString& Path)
    {
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Reader->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get();
        return Entry.HasValue() ? std::make_shared<ProviderContentSource>(Reader, Entry.Value()) : nullptr;
    }
    ArchiveOperationRequest ArchiveRequest(const QString& Path, const QString& TemporaryBase)
    {
        ArchiveOperationRequest Request;
        Request.archive_ = Request.destination_ = Location(Path);
        Request.reader_ = MakeWcxReadProvider(Fixture().toStdString(), Request.archive_, TemporaryBase.toStdString());
        Request.begin_ = [Plugin = Fixture().toStdString(), Root = Request.archive_, Reader = Request.reader_, Base = TemporaryBase.toStdString()](OperationContext Context) {
            return BeginWcxMutation(Plugin, Root, Reader, Base, {}, Context);
        };
        return Request;
    }
    class Selection final : public ISelectionManifest {
    public:
        Selection(std::shared_ptr<IStorageProvider> Reader, std::vector<EntrySnapshot> Entries) : reader_(std::move(Reader)), entries_(std::move(Entries)) {}
        uint64_t Count() const override { return entries_.size(); }
        std::shared_ptr<IStorageProvider> Provider() const override { return reader_; }
        Result<EntryPage> Read(uint64_t Offset, size_t Count, const OperationContext& Context) const override {
            if (const auto Stop = Context.Check(reader_->Id())) return *Stop;
            if (Offset > entries_.size()) return Error{ErrorCategory::InvalidRequest, "test-selection-offset"};
            const auto Last = Offset + std::min<uint64_t>(Count, entries_.size() - Offset);
            return EntryPage{{entries_.begin() + Offset, entries_.begin() + Last}, {}, Last == entries_.size()};
        }
    private:
        std::shared_ptr<IStorageProvider> reader_;
        std::vector<EntrySnapshot> entries_;
    };
    struct BatchGate
    {
        std::promise<void> prepared_;
        std::atomic<bool> release_ = false;
        std::atomic<int> commits_ = 0;
        std::function<void()> afterCommit_;
        bool throwAbort_ = false;
    };
    class GatedBatch final : public IArchiveMutationSession
    {
    public:
        GatedBatch(std::unique_ptr<IArchiveMutationSession> Inner, std::shared_ptr<BatchGate> Gate) : inner_(std::move(Inner)), gate_(std::move(Gate)) {}
        Result<Unit> Prepare(std::vector<ArchiveChange> Changes, OperationContext Context) override
        {
            auto Prepared = inner_->Prepare(std::move(Changes), Context);
            gate_->prepared_.set_value();
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!gate_->release_ && !Context.stopToken_.stop_requested() && std::chrono::steady_clock::now() < Deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return Prepared;
        }
        Result<WriteReceipt> Commit(OperationContext Context) override
        {
            ++gate_->commits_; auto Receipt = inner_->Commit(Context);
            if (Receipt.HasValue() && gate_->afterCommit_) gate_->afterCommit_();
            return Receipt;
        }
        std::vector<Error> Abort() override
        {
            auto Errors = inner_->Abort();
            if (gate_->throwAbort_) { gate_->throwAbort_ = false; throw std::runtime_error("injected-finalize-exception"); }
            return Errors;
        }
    private:
        std::unique_ptr<IArchiveMutationSession> inner_;
        std::shared_ptr<BatchGate> gate_;
    };
    void GateRequest(ArchiveOperationRequest& Request, const std::shared_ptr<BatchGate>& Gate)
    {
        Request.begin_ = [Begin = Request.begin_, Gate](OperationContext Context) -> Result<std::unique_ptr<IArchiveMutationSession>> {
            auto Batch = Begin(Context); if (!Batch.HasValue()) return Batch.Failure();
            return std::unique_ptr<IArchiveMutationSession>(new GatedBatch(std::move(Batch.Value()), Gate));
        };
    }
    ArchiveMoveEndpoint MoveEndpoint(const ArchiveOperationRequest& Request)
    { return {Request.archive_, Request.reader_, Request.begin_}; }
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
        explicit FailedDestination(bool CommitFailure, bool RemoveFailure = false, LocatorKind Kind = LocatorKind::Local)
            : inner_(MakeNativeWritableStorage(Kind, MakeNativeReadProvider(Kind))), commitFailure_(CommitFailure), removeFailure_(RemoveFailure) {}
        ProviderId Id() const override { return inner_->Id(); }
        Result<SupportDecision> Supports(Mutation Op, Locator At, OperationContext Context) override { return inner_->Supports(Op, At, Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return inner_->Inspect(At, Context); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override {
            auto Result = inner_->BeginWrite(At, Expected, Context); if (!Result.HasValue()) return Result.Failure();
            return std::unique_ptr<IWriteSession>(new Session(std::move(Result.Value()), commitFailure_));
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { return inner_->MakeDirectory(At, Context); }
        Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override { return removeFailure_ ? Result<Unit>(Error{ErrorCategory::Unavailable, "injected-source-remove-failure"}) : inner_->Remove(Entry, Context); }
        Result<Unit> Rename(EntrySnapshot Entry, Locator At, OperationContext Context) override { return inner_->Rename(Entry, At, Context); }
    private:
        std::shared_ptr<IWritableStorage> inner_;
        bool commitFailure_;
        bool removeFailure_;
    };
}
class WcxTests : public QObject
{
    Q_OBJECT
private slots:
    void manifestArchiveCopyAndDelete()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("target.hct"); QVERIFY(Put(Path, "HCWCX01"));
        QVERIFY(QDir().mkdir(Directory.filePath("input"))); std::vector<EntrySnapshot> Entries;
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        const Locator Root{LocatorKind::Local, QDir::toNativeSeparators(Directory.filePath("input")).toStdString()};
        for (const auto* Name : {"one.bin", "two.bin"}) {
            const auto File = Directory.filePath(QString("input/") + Name); QVERIFY(Put(File, Name));
            auto Entry = Reader->Resolve({LocatorKind::Local, File.toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Entries.push_back(Entry.Value());
        }
        auto Request = ArchiveRequest(Path, Directory.path()); Request.selectionRoot_ = Root;
        Request.selection_ = std::make_shared<Selection>(Reader, Entries);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Job = SubmitArchiveOperation(Scheduler, Request); QVERIFY(Job.HasValue());
        auto Copied = Job.Value().completion_.get(); QVERIFY2(Copied.snapshot_.state_ == JobState::Completed, Copied.snapshot_.error_ ? Copied.snapshot_.error_->code_.c_str() : "incomplete");
        auto Delete = ArchiveRequest(Path, Directory.path()); Delete.operation_ = FileOperation::Delete; Delete.confirmDelete_ = false; Entries.clear();
        for (const auto* Name : {"/one.bin", "/two.bin"}) { auto Entry = Delete.reader_->Resolve(Location(Path, Name), {}).get(); QVERIFY(Entry.HasValue()); Entries.push_back(Entry.Value()); }
        Delete.selectionRoot_ = Delete.archive_; Delete.selection_ = std::make_shared<Selection>(Delete.reader_, Entries);
        Job = SubmitArchiveOperation(Scheduler, Delete); QVERIFY(Job.HasValue()); QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Completed);
        auto Fresh = ArchiveRequest(Path, Directory.path()); QVERIFY(!Fresh.reader_->Resolve(Location(Path, "/one.bin"), {}).get().HasValue());
    }
    void metadataBeyondOldByteLimit()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("large.hct"); QVERIFY(Put(Path, "HCWCX01B"));
        auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        auto Entry = Reader->Resolve(Location(Path, "/alpha.txt"), {}).get(); QVERIFY(Entry.HasValue()); QCOMPARE(Entry.Value().size_, 64ull * 1024 * 1024 * 1024 + 1);
    }
    void nestedMutation()
    {
        for (int Depth : {2, 3}) {
            QTemporaryDir Directory; const auto Path = Directory.filePath("outer.hct");
            QByteArray Original("HCWCX01"); auto Root = Location(Path);
            for (int Level = 1; Level < Depth; ++Level) { Original = NestedFixture(Original); Root.nesting_.back().entry_ = "/inner.hct"; Root.nesting_.push_back({"wcx", "/"}); }
            QVERIFY(Put(Path, Original)); QVERIFY(Put(Directory.filePath("new.txt"), "new payload"));
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Root, Directory.path().toStdString());
            auto Alpha = Root; Alpha.nesting_.back().entry_ = "/alpha.txt";
            auto Entry = Reader->Resolve(Alpha, {}).get(); QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str());
            std::vector<WriteRecoveryRecord> Records; OperationContext Context;
            Context.writeCheckpoint_ = [&](const WriteRecoveryRecord& Record, std::optional<uint64_t> Row) -> Result<uint64_t> {
                if (Row) Records.at(*Row) = Record;
                else { Records.push_back(Record); Row = Records.size() - 1; }
                return *Row;
            };
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Root, Reader, Directory.path().toStdString(), {}, Context); QVERIFY2(Batch.HasValue(), Batch.HasValue() ? "" : Batch.Failure().code_.c_str());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/alpha.txt", "/renamed.txt", {}, Entry.Value().id_},
                {ArchiveChangeKind::Put, "/new.txt", {}, Content(Directory.filePath("new.txt"))}}, Context);
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(FileBytes(Path), Original);
            auto Committed = Batch.Value()->Commit(Context); QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
            QCOMPARE(Records.size(), size_t(1)); QCOMPARE(Records[0].phase_, WritePhase::Finished);
            QCOMPARE(QDir::fromNativeSeparators(QString::fromStdString(Records[0].destination_.root_)), Path);
            QVERIFY(!Batch.Value()->Commit({}).HasValue()); QVERIFY(!Reader->Stat(Entry.Value().id_, {}).get().HasValue());
            auto Fresh = MakeWcxReadProvider(Fixture().toStdString(), Root, Directory.path().toStdString()); auto Added = Root; Added.nesting_.back().entry_ = "/new.txt";
            auto New = Fresh->Resolve(Added, {}).get(); QVERIFY(New.HasValue()); auto Stream = Fresh->OpenRead(New.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
            auto Chunk = Stream.Value()->Read(1024, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("new payload"));
            QVERIFY(!Fresh->Resolve(Alpha, {}).get().HasValue());
            for (int Level = 1; Level < Depth; ++Level) {
                auto Parent = Root; Parent.nesting_.resize(Level); Parent.nesting_.back().entry_ = "/sibling.txt";
                auto ParentRoot = Parent; ParentRoot.nesting_.back().entry_ = "/";
                auto Provider = MakeWcxReadProvider(Fixture().toStdString(), ParentRoot, Directory.path().toStdString());
                auto Sibling = Provider->Resolve(Parent, {}).get(); QVERIFY(Sibling.HasValue()); auto Read = Provider->OpenRead(Sibling.Value().id_, {}).get(); QVERIFY(Read.HasValue());
                auto Keep = Read.Value()->Read(1024, {}).get(); QVERIFY(Keep.HasValue()); QCOMPARE(Bytes(Keep.Value()), QByteArray("keep"));
            }
        }
    }
    void nestedFailurePreservesRoot()
    {
        for (int Failure : {0, 1, 2, 3, 4, 5}) {
            QTemporaryDir Directory; const auto Path = Directory.filePath("outer.hct"); const auto Original = NestedFixture("HCWCX01", Failure == 4 ? 'P' : 0);
            QVERIFY(Put(Path, Original)); QVERIFY(Put(Directory.filePath("new.txt"), "new"));
            auto Root = Location(Path, "/inner.hct"); Root.nesting_.push_back({"wcx", "/"});
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Root, Directory.path().toStdString());
            std::shared_ptr<IWritableStorage> Writer; if (Failure <= 1) Writer = std::make_shared<FailedDestination>(Failure == 1);
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Root, Reader, Directory.path().toStdString(), {}, {}, Writer); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.txt", {}, Content(Directory.filePath("new.txt"))}}, {});
            if (Failure == 0 || Failure == 4) QVERIFY(!Prepared.HasValue());
            else {
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
                if (Failure == 2) { std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token(); QVERIFY(!Batch.Value()->Commit(Context).HasValue()); }
                else if (Failure == 3) { QVERIFY(Put(Path, "external change")); QVERIFY(!Batch.Value()->Commit({}).HasValue()); }
                else if (Failure == 1) QVERIFY(!Batch.Value()->Commit({}).HasValue());
            }
            QVERIFY(Batch.Value()->Abort().empty()); QCOMPARE(FileBytes(Path), Failure == 3 ? QByteArray("external change") : Original);
        }
    }
    void nestedBranchMove_data()
    {
        QTest::addColumn<int>("Depth"); QTest::addColumn<bool>("Skip");
        QTest::addColumn<int>("Layout");
        for (int Depth : {1, 2, 3, 7}) for (bool Skip : {false, true})
            QTest::newRow(qPrintable(QString("depth-%1-%2").arg(Depth).arg(Skip ? "skip" : "replace"))) << Depth << Skip << 0;
        QTest::newRow("shared-nested-parent") << 2 << false << 1;
        QTest::newRow("move-to-ancestor") << 3 << false << 2;
        QTest::newRow("skip-preserves-directory") << 1 << true << 3;
    }
    void nestedJointFailurePreservesRoot()
    {
        for (int Failure : {0, 1, 2, 3, 4, 5})
        {
            QTemporaryDir Directory;
            const auto Share = qEnvironmentVariable("HC_WCX_SMB_ROOT");
            QTemporaryDir Data(Share.isEmpty() ? QDir::tempPath() + "/hc-joint-XXXXXX" : Share + "/hc-joint-XXXXXX");
            QVERIFY(Data.isValid()); const auto Path = Data.filePath("outer.hct");
            auto Original = NestedFixture("HCWCX01", Failure == 4 ? 'P' : 0);
            const quint32 Count = 3; std::memcpy(Original.data() + 8, &Count, 4);
            const QString TargetName("target.hct");
            const auto Number = [&](quint32 Value) { Original.append(reinterpret_cast<const char*>(&Value), 4); };
            Number(TargetName.size()); Number(7); Number(0); Original.append(reinterpret_cast<const char*>(TargetName.utf16()), TargetName.size() * 2); Original += "HCWCX01";
            QVERIFY(Put(Path, Original));
            auto SourceRoot = Location(Path, "/inner.hct"); SourceRoot.nesting_.push_back({"wcx", "/"});
            auto TargetRoot = Location(Path, "/target.hct"); TargetRoot.nesting_.push_back({"wcx", "/"});
            auto Source = MakeWcxReadProvider(Fixture().toStdString(), SourceRoot, Directory.path().toStdString());
            auto Target = MakeWcxReadProvider(Fixture().toStdString(), TargetRoot, Directory.path().toStdString());
            auto Alpha = SourceRoot; Alpha.nesting_.back().entry_ = "/alpha.txt"; auto Entry = Source->Resolve(Alpha, {}).get(); QVERIFY(Entry.HasValue());
            auto Removal = BeginWcxMutation(Fixture().toStdString(), SourceRoot, Source, Directory.path().toStdString()); QVERIFY(Removal.HasValue());
            std::shared_ptr<IWritableStorage> Writer; if (Failure < 2) Writer = std::make_shared<FailedDestination>(Failure == 1, false, Share.isEmpty() ? LocatorKind::Local : LocatorKind::Smb);
            auto Batch = BeginWcxMutation(Fixture().toStdString(), TargetRoot, Target, Directory.path().toStdString(), {}, {}, Writer); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->PrepareTogether({{ArchiveChangeKind::Put, "/moved.txt", {}, std::make_shared<ProviderContentSource>(Source, Entry.Value())}},
                std::move(Removal.Value()), {{ArchiveChangeKind::Remove, "/alpha.txt", {}, {}, Entry.Value().id_}}, {});
            QCOMPARE(FileBytes(Path), Original);
            if (Failure == 0 || Failure == 4) QVERIFY(!Prepared.HasValue());
            else
            {
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
                if (Failure == 1) QVERIFY(!Batch.Value()->Commit({}).HasValue());
                if (Failure == 2) { std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token(); QVERIFY(!Batch.Value()->Commit(Context).HasValue()); }
                if (Failure == 3) { QVERIFY(Put(Path, "external change")); QVERIFY(!Batch.Value()->Commit({}).HasValue()); }
            }
            QVERIFY(Batch.Value()->Abort().empty()); QCOMPARE(FileBytes(Path), Failure == 3 ? QByteArray("external change") : Original);
        }
    }
    void nestedBranchMove()
    {
        QFETCH(int, Depth); QFETCH(bool, Skip); QFETCH(int, Layout);
        const auto Share = qEnvironmentVariable("HC_WCX_SMB_ROOT");
        QTemporaryDir Directory;
        QTemporaryDir Data(Share.isEmpty() ? QDir::tempPath() + "/hc-branch-XXXXXX" : Share + "/hc-branch-XXXXXX");
        QVERIFY(Directory.isValid()); QVERIFY(Data.isValid()); const auto Path = Data.filePath("outer.hct");
        auto SourceBytes = QByteArray("HCWCX01");
        for (int Index = 1; Index < Depth; ++Index) SourceBytes = NestedFixture(SourceBytes);
        // Different depths force the deeper branch to be lifted first.
        auto OuterBytes = NestedFixture(SourceBytes); const quint32 Count = 3; std::memcpy(OuterBytes.data() + 8, &Count, 4);
        const QString TargetName("target.hct");
        const auto Number = [&](quint32 Value) { OuterBytes.append(reinterpret_cast<const char*>(&Value), 4); };
        Number(TargetName.size()); Number(7); Number(0); OuterBytes.append(reinterpret_cast<const char*>(TargetName.utf16()), TargetName.size() * 2); OuterBytes += "HCWCX01";
        auto SourceRoot = Location(Path, "/inner.hct"); SourceRoot.nesting_.push_back({"wcx", "/"});
        for (int Index = 1; Index < Depth; ++Index) { SourceRoot.nesting_.back().entry_ = "/inner.hct"; SourceRoot.nesting_.push_back({"wcx", "/"}); }
        auto TargetRoot = Location(Path, "/target.hct"); TargetRoot.nesting_.push_back({"wcx", "/"});
        if (Layout == 1) {
            OuterBytes = NestedFixture(OuterBytes);
            SourceRoot.nesting_.insert(SourceRoot.nesting_.begin(), {"wcx", "/inner.hct"});
            TargetRoot.nesting_.insert(TargetRoot.nesting_.begin(), {"wcx", "/inner.hct"});
        }
        QVERIFY(Put(Path, OuterBytes));
        if (Layout == 2) TargetRoot = Location(Path);
        const auto Endpoint = [&](Locator Root) {
            ArchiveMoveEndpoint Value; Value.root_ = Root;
            Value.reader_ = MakeWcxReadProvider(Fixture().toStdString(), Root, Directory.path().toStdString());
            Value.begin_ = [Root, Reader = Value.reader_, Base = Directory.path().toStdString()](OperationContext Context) {
                return BeginWcxMutation(Fixture().toStdString(), Root, Reader, Base, {}, Context);
            }; return Value;
        };
        if (Layout == 3) {
            QVERIFY(Put(Directory.filePath("extra.txt"), "extra"));
            auto Source = Endpoint(SourceRoot); auto Additional = Source.begin_({});
            QVERIFY2(Additional.HasValue(), Additional.HasValue() ? "" : Additional.Failure().code_.c_str());
            QVERIFY(Additional.Value()->Prepare({{ArchiveChangeKind::Put, "/sub/extra.txt", {}, Content(Directory.filePath("extra.txt"))}}, {}).HasValue());
            QVERIFY(Additional.Value()->Commit({}).HasValue());
        }
        MoveOperationRequest Request; Request.sourceArchive_ = Endpoint(SourceRoot); Request.destinationArchive_ = Endpoint(TargetRoot);
        Request.sourceReader_ = Request.sourceArchive_->reader_; Request.destination_ = TargetRoot;
        Request.conflict_ = Skip ? ConflictPolicy::Skip : ConflictPolicy::Replace;
        for (const auto* Name : {"/alpha.txt", "/sub"}) {
            auto At = SourceRoot; At.nesting_.back().entry_ = Name; auto Entry = Request.sourceReader_->Resolve(At, {}).get();
            QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str()); Request.sources_.push_back(Entry.Value());
        }
        const auto Original = FileBytes(Path);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        if (Depth >= 2) { Request.selectionRoot_ = SourceRoot; Request.selection_ = std::make_shared<Selection>(Request.sourceReader_, std::move(Request.sources_)); Request.sources_.clear(); }
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY2(Submitted.HasValue(), Submitted.HasValue() ? "" : Submitted.Failure().code_.c_str());
        QTRY_VERIFY_WITH_TIMEOUT(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 30000);
        const auto Result = Submitted.Value().completion_.get();
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "not completed");
        auto FreshSource = Endpoint(SourceRoot).reader_; auto Alpha = SourceRoot; Alpha.nesting_.back().entry_ = "/alpha.txt";
        QCOMPARE(FreshSource->Resolve(Alpha, {}).get().HasValue(), Skip);
        auto FreshTarget = Endpoint(TargetRoot).reader_; Alpha = TargetRoot; Alpha.nesting_.back().entry_ = "/alpha.txt";
        auto Copied = FreshTarget->Resolve(Alpha, {}).get(); QVERIFY(Copied.HasValue());
        auto Stream = FreshTarget->OpenRead(Copied.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
        auto Read = Stream.Value()->Read(1024, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Bytes(Read.Value()), QByteArray("alpha"));
        if (Layout == 3) {
            auto Child = SourceRoot; Child.nesting_.back().entry_ = "/sub/extra.txt"; QVERIFY(!FreshSource->Resolve(Child, {}).get().HasValue());
            Child.nesting_.back().entry_ = "/sub/한글.txt"; QVERIFY(FreshSource->Resolve(Child, {}).get().HasValue());
            Child = TargetRoot; Child.nesting_.back().entry_ = "/sub/extra.txt"; QVERIFY(FreshTarget->Resolve(Child, {}).get().HasValue());
            for (const auto& Item : Result.items_) if (Item.source_ && Item.source_->directory_) QVERIFY(!Item.sourceRemoved_);
        }
        else if (Skip) QCOMPARE(FileBytes(Path), Original);
        else for (const auto& Item : Result.items_) QVERIFY(Item.destinationCommitted_ && Item.sourceRemoved_);
    }
    void crossProviderMove_data()
    {
        QTest::addColumn<bool>("SourceArchive"); QTest::addColumn<bool>("DestinationArchive"); QTest::addColumn<bool>("Skip");
        for (bool Source : {false, true}) for (bool Destination : {false, true}) for (bool Skip : {false, true})
        {
            const auto Name = QByteArray(Source ? "archive" : "native") + "-to-" + (Destination ? "archive" : "native") + (Skip ? "-skip" : "-replace");
            QTest::newRow(Name.constData()) << Source << Destination << Skip;
        }
    }
    void crossProviderMove()
    {
        QFETCH(bool, SourceArchive); QFETCH(bool, DestinationArchive); QFETCH(bool, Skip);
        QTemporaryDir Directory; const auto Source = Directory.filePath(SourceArchive ? "source.hct" : "source");
        const auto Destination = Directory.filePath(DestinationArchive ? "destination.hct" : "destination");
        MoveOperationRequest Request; Request.conflict_ = Skip ? ConflictPolicy::Skip : ConflictPolicy::Replace;
        if (SourceArchive)
        {
            QVERIFY(Put(Source, "HCWCX01")); Request.sourceArchive_ = MoveEndpoint(ArchiveRequest(Source, Directory.path()));
            Request.sourceReader_ = Request.sourceArchive_->reader_;
        }
        else
        {
            QVERIFY(QDir().mkpath(Source + "/sub")); QVERIFY(Put(Source + "/alpha.txt", "alpha")); QVERIFY(Put(Source + "/sub/한글.txt", "한글!\r\n"));
            Request.sourceReader_ = MakeNativeReadProvider(LocatorKind::Local);
            Request.sourceWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Request.sourceReader_);
        }
        if (DestinationArchive)
        {
            QVERIFY(Put(Destination, "HCWCX01")); Request.destinationArchive_ = MoveEndpoint(ArchiveRequest(Destination, Directory.path()));
            Request.destination_ = Location(Destination);
        }
        else
        {
            QVERIFY(QDir().mkdir(Destination)); QVERIFY(Put(Destination + "/alpha.txt", "existing"));
            Request.destination_ = {LocatorKind::Local, Destination.toStdString()};
            Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
        }
        for (const auto& Name : {"/alpha.txt", "/sub"})
        {
            auto At = SourceArchive ? Location(Source, Name) : Locator{LocatorKind::Local, (Source + Name).toStdString()};
            auto Entry = Request.sourceReader_->Resolve(At, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value());
        }
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue()); const auto Handle = Submitted.Value();
        QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Result = Handle.completion_.get();
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "incomplete");
        QCOMPARE(Result.items_.size(), 3u); QVERIFY(Result.cleanupErrors_.empty());
        const auto Skipped = Skip ? DestinationArchive ? 3u : 1u : 0u;
        QCOMPARE(Result.snapshot_.skippedItems_, Skipped); QCOMPARE(Result.snapshot_.completedItems_, 3u - Skipped);
        for (const auto& Item : Result.items_)
            if (Item.status_ == ItemOutcome::Status::Completed) { QVERIFY(Item.destinationCommitted_); QVERIFY(Item.sourceRemoved_); }
            else QVERIFY(!Item.sourceRemoved_);
        auto UpdatedSource = SourceArchive ? ArchiveRequest(Source, Directory.path()).reader_ : MakeNativeReadProvider(LocatorKind::Local);
        auto UpdatedDestination = DestinationArchive ? ArchiveRequest(Destination, Directory.path()).reader_ : MakeNativeReadProvider(LocatorKind::Local);
        for (const auto& Name : {"/alpha.txt", "/sub/한글.txt"})
        {
            auto From = SourceArchive ? Location(Source, Name) : Locator{LocatorKind::Local, (Source + Name).toStdString()};
            QCOMPARE(UpdatedSource->Resolve(From, {}).get().HasValue(), Skip && (DestinationArchive || std::string(Name) == "/alpha.txt"));
            auto To = DestinationArchive ? Location(Destination, Name) : Locator{LocatorKind::Local, (Destination + Name).toStdString()};
            auto Entry = UpdatedDestination->Resolve(To, {}).get(); QVERIFY(Entry.HasValue());
            auto Stream = UpdatedDestination->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
            auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue());
            QCOMPARE(Bytes(Read.Value()), std::string(Name) == "/alpha.txt" ? QByteArray(Skip && !DestinationArchive ? "existing" : "alpha") : QByteArray("한글!\r\n"));
        }
    }
    void moveArchiveRemovalCancellation_data()
    {
        QTest::addColumn<bool>("CancelBeforeCommit");
        QTest::newRow("cancel-after-copy-before-source-removal") << true;
        QTest::newRow("late-cancel-retains-all-removal-results") << false;
    }
    void moveArchiveRemovalCancellation()
    {
        QFETCH(bool, CancelBeforeCommit);
        QTemporaryDir Directory; const auto Source = Directory.filePath("source.hct"); QVERIFY(Put(Source, "HCWCX01"));
        const auto Original = FileBytes(Source); const auto Destination = Directory.filePath("destination"); QVERIFY(QDir().mkdir(Destination));
        auto Archive = ArchiveRequest(Source, Directory.path()); auto Gate = std::make_shared<BatchGate>();
        auto Prepared = Gate->prepared_.get_future(); GateRequest(Archive, Gate);
        MoveOperationRequest Request; Request.sourceArchive_ = MoveEndpoint(Archive); Request.sourceReader_ = Archive.reader_;
        Request.destination_ = {LocatorKind::Local, Destination.toStdString()};
        Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
        for (const auto& Name : {"/alpha.txt", "/sub"})
        { auto Entry = Request.sourceReader_->Resolve(Location(Source, Name), {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value()); }
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue()); const auto Handle = Submitted.Value();
        QTRY_VERIFY(Prepared.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(FileBytes(Source), Original); QCOMPARE(FileBytes(Destination + "/alpha.txt"), "alpha");
        if (CancelBeforeCommit) QVERIFY(Scheduler.Cancel(Handle.id_).HasValue());
        else Gate->afterCommit_ = [&] { Scheduler.Cancel(Handle.id_); };
        Gate->release_ = true;
        QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Result = Handle.completion_.get(); QCOMPARE(Result.snapshot_.state_, CancelBeforeCommit ? JobState::Cancelled : JobState::Completed);
        QCOMPARE(Result.items_.size(), 3u); QCOMPARE(Result.snapshot_.completedItems_, 3u);
        for (const auto& Item : Result.items_) { QVERIFY(Item.destinationCommitted_); QCOMPARE(Item.sourceRemoved_, !CancelBeforeCommit); }
        if (CancelBeforeCommit) QCOMPARE(FileBytes(Source), Original);
        QVERIFY(Result.cleanupErrors_.empty());
    }
    void moveRemovalFailuresRetainCopies_data()
    {
        QTest::addColumn<bool>("SourceArchive"); QTest::newRow("native-remove-failure") << false; QTest::newRow("archive-commit-failure") << true;
    }
    void moveSourceRevisionAfterDestinationCommit()
    {
        QTemporaryDir Directory; const auto Source = Directory.filePath("new.txt"); QVERIFY(Put(Source, "original"));
        const auto Destination = Directory.filePath("destination.hct"); QVERIFY(Put(Destination, "HCWCX01"));
        auto Archive = ArchiveRequest(Destination, Directory.path()); auto Gate = std::make_shared<BatchGate>();
        Gate->release_ = true; std::atomic<bool> Changed = false;
        Gate->afterCommit_ = [&] { Changed = Put(Source, "new revision must remain"); }; GateRequest(Archive, Gate);
        MoveOperationRequest Request; Request.destinationArchive_ = MoveEndpoint(Archive); Request.destination_ = Location(Destination);
        Request.sourceReader_ = MakeNativeReadProvider(LocatorKind::Local); Request.sourceWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Request.sourceReader_);
        auto Entry = Request.sourceReader_->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue());
        QTRY_VERIFY(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Changed); const auto Result = Submitted.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed);
        QCOMPARE(Result.items_.size(), 1u); QVERIFY(Result.items_[0].destinationCommitted_); QVERIFY(!Result.items_[0].sourceRemoved_);
        QCOMPARE(FileBytes(Source), "new revision must remain");
        auto Reader = ArchiveRequest(Destination, Directory.path()).reader_;
        auto Copied = Reader->Resolve(Location(Destination, "/new.txt"), {}).get(); QVERIFY(Copied.HasValue());
        auto Stream = Reader->OpenRead(Copied.Value().id_, {}).get(); QVERIFY(Stream.HasValue()); auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue());
        QCOMPARE(Bytes(Read.Value()), "original");
    }
    void moveCopyFinalizeExceptionPreservesCommittedOutcome()
    {
        QTemporaryDir Directory; const auto Source = Directory.filePath("new.txt"); QVERIFY(Put(Source, "original"));
        const auto Destination = Directory.filePath("destination.hct"); QVERIFY(Put(Destination, "HCWCX01"));
        auto Archive = ArchiveRequest(Destination, Directory.path()); auto Gate = std::make_shared<BatchGate>();
        Gate->release_ = true; Gate->throwAbort_ = true; GateRequest(Archive, Gate);
        MoveOperationRequest Request; Request.destinationArchive_ = MoveEndpoint(Archive); Request.destination_ = Location(Destination);
        Request.sourceReader_ = MakeNativeReadProvider(LocatorKind::Local); Request.sourceWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Request.sourceReader_);
        const auto Entry = Request.sourceReader_->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue());
        QTRY_VERIFY(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Result = Submitted.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed);
        QCOMPARE(Result.items_.size(), 1u); QVERIFY(Result.items_[0].destinationCommitted_); QVERIFY(!Result.items_[0].sourceRemoved_);
        QCOMPARE(Result.cleanupErrors_.size(), 1u); QCOMPARE(Result.cleanupErrors_[0].code_, "copy-finalize-exception");
        QCOMPARE(FileBytes(Source), "original");
        auto Reader = ArchiveRequest(Destination, Directory.path()).reader_; auto Copied = Reader->Resolve(Location(Destination, "/new.txt"), {}).get(); QVERIFY(Copied.HasValue());
        auto Stream = Reader->OpenRead(Copied.Value().id_, {}).get(); QVERIFY(Stream.HasValue()); auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue());
        QCOMPARE(Bytes(Read.Value()), "original");
    }
    void moveRejectsDestinationInsideNativeSource()
    {
        QTemporaryDir Directory; const auto Destination = Directory.filePath("destination.hct"); QVERIFY(Put(Destination, "HCWCX01"));
        const auto Original = FileBytes(Destination);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        for (const auto& Source : {Destination, Directory.path()})
        {
            MoveOperationRequest Request; Request.destinationArchive_ = MoveEndpoint(ArchiveRequest(Destination, Directory.path())); Request.destination_ = Location(Destination);
            Request.sourceReader_ = MakeNativeReadProvider(LocatorKind::Local); Request.sourceWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Request.sourceReader_);
            const auto Entry = Request.sourceReader_->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value());
            const auto Rejected = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(!Rejected.HasValue());
            QCOMPARE(Rejected.Failure().code_, "destination-inside-source"); QCOMPARE(FileBytes(Destination), Original);
        }
        QVERIFY(Scheduler.Snapshots().empty());
    }
    void moveRemovalFailuresRetainCopies()
    {
        QFETCH(bool, SourceArchive);
        QTemporaryDir Directory; const auto Source = Directory.filePath(SourceArchive ? "source.hct" : "alpha.txt");
        const auto Destination = Directory.filePath("destination.hct"); QVERIFY(Put(Destination, "HCWCX01"));
        MoveOperationRequest Request; Request.conflict_ = ConflictPolicy::Replace;
        Request.destinationArchive_ = MoveEndpoint(ArchiveRequest(Destination, Directory.path())); Request.destination_ = Location(Destination);
        QVERIFY(Put(Source, SourceArchive ? "HCWCX01" : "changed")); const auto Original = FileBytes(Source);
        if (SourceArchive)
        {
            auto Archive = ArchiveRequest(Source, Directory.path());
            Archive.begin_ = [Plugin = Fixture().toStdString(), Root = Archive.archive_, Reader = Archive.reader_, Base = Directory.path().toStdString()](OperationContext Context) {
                return BeginWcxMutation(Plugin, Root, Reader, Base, {}, Context, std::make_shared<FailedDestination>(true));
            };
            Request.sourceArchive_ = MoveEndpoint(Archive); Request.sourceReader_ = Archive.reader_;
        }
        else { Request.sourceReader_ = MakeNativeReadProvider(LocatorKind::Local); Request.sourceWriter_ = std::make_shared<FailedDestination>(true, true); }
        auto Entry = Request.sourceReader_->Resolve(SourceArchive ? Location(Source, "/alpha.txt") : Locator{LocatorKind::Local, Source.toStdString()}, {}).get();
        QVERIFY(Entry.HasValue()); Request.sources_.push_back(Entry.Value());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue());
        QTRY_VERIFY(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Result = Submitted.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Result.items_.size(), 1u);
        QVERIFY(Result.items_[0].destinationCommitted_); QVERIFY(!Result.items_[0].sourceRemoved_); QCOMPARE(FileBytes(Source), Original);
        auto Reader = ArchiveRequest(Destination, Directory.path()).reader_; auto Copied = Reader->Resolve(Location(Destination, "/alpha.txt"), {}).get(); QVERIFY(Copied.HasValue());
        auto Stream = Reader->OpenRead(Copied.Value().id_, {}).get(); QVERIFY(Stream.HasValue()); auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue());
        QCOMPARE(Bytes(Read.Value()), SourceArchive ? "alpha" : "changed"); QVERIFY(Result.cleanupErrors_.empty());
    }
    void archiveJobCommitBoundary_data()
    {
        QTest::addColumn<bool>("CancelBeforeCommit");
        QTest::newRow("pause-and-cancel-before-commit") << true;
        QTest::newRow("cancel-after-commit-reports-entire-batch") << false;
    }
    void archiveJobCommitBoundary()
    {
        QFETCH(bool, CancelBeforeCommit);
        QTemporaryDir Directory; const auto Path = Directory.filePath("jobs.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto Original = FileBytes(Path);
        auto Request = ArchiveRequest(Path, Directory.path()); Request.operation_ = FileOperation::Delete; Request.confirmDelete_ = false;
        for (const auto& Name : {"/alpha.txt", "/sub"})
        {
            auto Entry = Request.reader_->Resolve(Location(Path, Name), {}).get(); QVERIFY(Entry.HasValue());
            Request.sources_.push_back({Entry.Value(), Request.reader_, {}});
        }
        auto Gate = std::make_shared<BatchGate>(); auto Prepared = Gate->prepared_.get_future(); GateRequest(Request, Gate);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitArchiveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue()); const auto Handle = Submitted.Value();
        QTRY_VERIFY(Prepared.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(FileBytes(Path), Original); QCOMPARE(Scheduler.Snapshot(Handle.id_).Value().completedItems_, 0u);
        if (CancelBeforeCommit)
        {
            QVERIFY(Scheduler.Pause(Handle.id_, true).HasValue()); Gate->release_ = true;
            QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::Paused);
            QCOMPARE(FileBytes(Path), Original); QVERIFY(Scheduler.Cancel(Handle.id_).HasValue());
        }
        else
        {
            Gate->afterCommit_ = [&] { Scheduler.Cancel(Handle.id_); };
            Gate->release_ = true;
        }
        QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Result = Handle.completion_.get(); QVERIFY(Result.cleanupErrors_.empty());
        QCOMPARE(Result.snapshot_.state_, CancelBeforeCommit ? JobState::Cancelled : JobState::Completed);
        QCOMPARE(Gate->commits_.load(), CancelBeforeCommit ? 0 : 1);
        QCOMPARE(Result.items_.size(), CancelBeforeCommit ? 0u : 2u);
        if (CancelBeforeCommit) QCOMPARE(FileBytes(Path), Original);
        else
        {
            for (const auto& Item : Result.items_) { QCOMPARE(Item.status_, ItemOutcome::Status::Completed); QVERIFY(Item.sourceRemoved_); }
            auto Reader = ArchiveRequest(Path, Directory.path()).reader_;
            auto Cursor = Reader->Enumerate(Location(Path), {}).get(); QVERIFY(Cursor.HasValue());
            auto Page = Cursor.Value()->Next(100, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.empty());
        }
    }
    void archiveJobRecursiveConflictRename()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("jobs.hct"); QVERIFY(Put(Path, "HCWCX01"));
        const auto SourcePath = Directory.filePath("alpha.txt"); QVERIFY(QDir().mkdir(SourcePath));
        QVERIFY(Put(SourcePath + "/child.txt", "child-content"));
        auto Source = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Source->Resolve({LocatorKind::Local, SourcePath.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Request = ArchiveRequest(Path, Directory.path()); Request.sources_.push_back({Entry.Value(), Source, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Submitted = SubmitArchiveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue()); const auto Handle = Submitted.Value();
        QTRY_COMPARE(Scheduler.Snapshot(Handle.id_).Value().state_, JobState::WaitingForUser);
        auto Interaction = Scheduler.Snapshot(Handle.id_).Value().interaction_; QVERIFY(Interaction);
        QVERIFY(Scheduler.Reply(Handle.id_, {Interaction->id_, InteractionChoice::Rename, "renamed"}).HasValue());
        QTRY_VERIFY(Handle.completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Result = Handle.completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "archive job did not complete"); QCOMPARE(Result.items_.size(), 2u);
        for (const auto& Item : Result.items_) QVERIFY(Item.destinationCommitted_);
        auto Reader = ArchiveRequest(Path, Directory.path()).reader_;
        auto Child = Reader->Resolve(Location(Path, "/renamed/child.txt"), {}).get(); QVERIFY(Child.HasValue());
        auto Stream = Reader->OpenRead(Child.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
        auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Bytes(Read.Value()), "child-content");
        QVERIFY(Reader->Resolve(Location(Path, "/alpha.txt"), {}).get().HasValue());
        QVERIFY(QFile::exists(SourcePath + "/child.txt"));
    }
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
    void mutationWithoutHeaderChecksum()
    {
        for (const auto Mode : {'0', '!'}) {
            QTemporaryDir Directory; const auto Path = Directory.filePath("batch.hct");
            const auto Original = QByteArray("HCWCX01") + Mode; QVERIFY(Put(Path, Original));
            const auto Input = Directory.filePath("source"); QVERIFY(Put(Input, "verified bytes"));
            auto Reader = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
            auto Batch = BeginWcxMutation(Fixture().toStdString(), Location(Path), Reader, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Put, "/new.txt", {}, Content(Input)}}, {});
            if (Mode == '!') {
                QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, "archive-output-content-mismatch");
                QCOMPARE(FileBytes(Path), Original); QVERIFY(Batch.Value()->Abort().empty());
            } else {
                QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
                QVERIFY(Batch.Value()->Commit({}).HasValue());
                auto Fresh = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
                auto Entry = Fresh->Resolve(Location(Path, "/new.txt"), {}).get(); QVERIFY(Entry.HasValue());
                auto Stream = Fresh->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
                auto Read = Stream.Value()->Read(100, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Bytes(Read.Value()), "verified bytes");
            }
        }
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
        {
            auto Outer = QDir::toNativeSeparators(Directory.filePath("outer.7z")).toStdWString(); wchar_t NestedList[] = L"real.7z\0";
            Progress(nullptr, &Continue); QCOMPARE(Pack(Outer.data(), nullptr, Source.data(), NestedList, PK_PACK_SAVE_PATHS), 0);
            auto Root = Location(QString::fromStdWString(Outer), "/real.7z"); Root.nesting_.push_back({"wcx", "/"});
            auto Nested = MakeWcxReadProvider(Plugin.toStdString(), Root, Directory.path().toStdString());
            auto Alpha = Root; Alpha.nesting_.back().entry_ = "/alpha.txt"; auto Selected = Nested->Resolve(Alpha, {}).get();
            QVERIFY2(Selected.HasValue(), Selected.HasValue() ? "" : Selected.Failure().code_.c_str());
            const auto Before = FileBytes(QString::fromStdWString(Outer));
            auto Batch = BeginWcxMutation(Plugin.toStdString(), Root, Nested, Directory.path().toStdString()); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/alpha.txt", "/nested-renamed.txt", {}, Selected.Value().id_}}, {});
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(FileBytes(QString::fromStdWString(Outer)), Before);
            QVERIFY(Batch.Value()->Commit({}).HasValue());
            auto Fresh = MakeWcxReadProvider(Plugin.toStdString(), Root, Directory.path().toStdString()); Alpha.nesting_.back().entry_ = "/nested-renamed.txt";
            auto Renamed = Fresh->Resolve(Alpha, {}).get(); QVERIFY(Renamed.HasValue()); auto Read = Fresh->OpenRead(Renamed.Value().id_, {}).get(); QVERIFY(Read.HasValue());
            auto Content = Read.Value()->Read(32, {}).get(); QVERIFY(Content.HasValue()); QCOMPARE(Bytes(Content.Value()), QByteArray("alpha"));
        }
        const auto JointMove = [&](const QString& Base) {
            const auto OuterPath = QDir(Base).filePath("joint.7z");
            QVERIFY(QFile::copy(QString::fromStdWString(Archive), Directory.filePath("target.7z")));
            auto Outer = QDir::toNativeSeparators(OuterPath).toStdWString(); wchar_t Members[] = L"real.7z\0target.7z\0";
            Progress(nullptr, &Continue); QCOMPARE(Pack(Outer.data(), nullptr, Source.data(), Members, PK_PACK_SAVE_PATHS), 0);
            QVERIFY(QFile::remove(Directory.filePath("target.7z")));
            const auto Endpoint = [&](const char* Member) {
                ArchiveMoveEndpoint Value; Value.root_ = Location(OuterPath, Member); Value.root_.nesting_.push_back({"wcx", "/"});
                Value.reader_ = MakeWcxReadProvider(Plugin.toStdString(), Value.root_, Directory.path().toStdString());
                Value.begin_ = [Root = Value.root_, Reader = Value.reader_, Base = Directory.path().toStdString(), Binary = Plugin.toStdString()](OperationContext Context) {
                    return BeginWcxMutation(Binary, Root, Reader, Base, {}, Context);
                }; return Value;
            };
            MoveOperationRequest Request; Request.sourceArchive_ = Endpoint("/real.7z"); Request.destinationArchive_ = Endpoint("/target.7z");
            Request.sourceReader_ = Request.sourceArchive_->reader_; Request.destination_ = Request.destinationArchive_->root_; Request.conflict_ = ConflictPolicy::Replace;
            auto Alpha = Request.sourceArchive_->root_; Alpha.nesting_.back().entry_ = "/alpha.txt";
            auto Entry = Request.sourceReader_->Resolve(Alpha, {}).get(); QVERIFY(Entry.HasValue()); Request.sources_ = {Entry.Value()};
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1); auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue());
            QTRY_VERIFY_WITH_TIMEOUT(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 30000);
            const auto Result = Submitted.Value().completion_.get();
            QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "not completed");
            QVERIFY(!Endpoint("/real.7z").reader_->Resolve(Alpha, {}).get().HasValue());
            auto Target = Endpoint("/target.7z"); Alpha = Target.root_; Alpha.nesting_.back().entry_ = "/alpha.txt";
            Entry = Target.reader_->Resolve(Alpha, {}).get(); QVERIFY(Entry.HasValue());
            auto Read = Target.reader_->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Read.HasValue());
            auto Chunk = Read.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("alpha"));
            for (const auto& Item : Result.items_) QVERIFY(Item.destinationCommitted_ && Item.sourceRemoved_);
        };
        JointMove(Directory.path());
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
        const auto ExerciseMoves = [&](const QString& Base) {
            const auto First = QDir(Base).filePath("move-source.7z"), Second = QDir(Base).filePath("move-target.7z"), Native = QDir(Base).filePath("move-files");
            QVERIFY(QFile::copy(QString::fromStdWString(Archive), First)); QVERIFY(QFile::copy(QString::fromStdWString(Archive), Second));
            QVERIFY(QDir().mkdir(Native)); QVERIFY(Put(Native + "/round.txt", "roundtrip"));
            const auto NativeKind = QDir::toNativeSeparators(Base).startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local;
            const auto Endpoint = [&](const QString& Path) {
                ArchiveMoveEndpoint Value{Location(Path), MakeWcxReadProvider(Plugin.toStdString(), Location(Path), Directory.path().toStdString())};
                Value.begin_ = [Binary = Plugin.toStdString(), Root = Value.root_, Reader = Value.reader_, Temporary = Directory.path().toStdString()](OperationContext Context) {
                    return BeginWcxMutation(Binary, Root, Reader, Temporary, {}, Context);
                };
                return Value;
            };
            for (int Stage = 0; Stage < 3; ++Stage)
            {
                MoveOperationRequest Request;
                if (Stage == 0) { Request.sourceReader_ = MakeNativeReadProvider(NativeKind); Request.sourceWriter_ = MakeNativeWritableStorage(NativeKind, Request.sourceReader_); }
                else { Request.sourceArchive_ = Endpoint(Stage == 1 ? First : Second); Request.sourceReader_ = Request.sourceArchive_->reader_; }
                if (Stage < 2) { Request.destinationArchive_ = Endpoint(Stage == 0 ? First : Second); Request.destination_ = Request.destinationArchive_->root_; }
                else { Request.destination_ = {NativeKind, Native.toStdString()}; Request.destinationWriter_ = MakeNativeWritableStorage(NativeKind, MakeNativeReadProvider(NativeKind)); }
                const auto From = Stage == 0 ? Locator{NativeKind, (Native + "/round.txt").toStdString()} : Location(Stage == 1 ? First : Second, "/round.txt");
                const auto Selected = Request.sourceReader_->Resolve(From, {}).get(); QVERIFY(Selected.HasValue()); Request.sources_.push_back(Selected.Value());
                JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
                auto Submitted = SubmitMoveOperation(Scheduler, std::move(Request)); QVERIFY(Submitted.HasValue());
                QTRY_VERIFY(Submitted.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
                const auto Result = Submitted.Value().completion_.get();
                QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "move incomplete");
                QCOMPARE(Result.items_.size(), 1u); QVERIFY(Result.items_[0].destinationCommitted_); QVERIFY(Result.items_[0].sourceRemoved_);
                if (Stage == 0) QVERIFY(!QFile::exists(Native + "/round.txt"));
                else QVERIFY(!Endpoint(Stage == 1 ? First : Second).reader_->Resolve(From, {}).get().HasValue());
            }
            QCOMPARE(FileBytes(Native + "/round.txt"), "roundtrip");
        };
        ExerciseMoves(Directory.path());
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
            ExerciseMoves(Remote.path());
            JointMove(Remote.path());
        }
        Mutate(QString::fromStdWString(Archive));
    }
};
QTEST_GUILESS_MAIN(WcxTests)
#include "tstWcx.moc"
