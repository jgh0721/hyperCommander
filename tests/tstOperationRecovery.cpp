#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include "UniqueLibs/qtItemOutcomeStore.hpp"
#include "UniqueLibs/qtJobJournal.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/operations/writeRecovery.hpp"
#include <Windows.h>

using namespace hc::storage;
using namespace hc::operations;
namespace {
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
class Selection final : public ISelectionManifest {
public:
    Selection(std::shared_ptr<ISelectionManifest> Index, std::shared_ptr<IStorageProvider> Provider) : index_(std::move(Index)), provider_(std::move(Provider)) {}
    uint64_t Count() const override { return index_->Count(); }
    Result<EntryPage> Read(uint64_t Offset, size_t Count, const OperationContext& Context) const override { return index_->Read(Offset, Count, Context); }
    std::shared_ptr<IStorageProvider> Provider() const override { return provider_; }
private:
    std::shared_ptr<ISelectionManifest> index_;
    std::shared_ptr<IStorageProvider> provider_;
};
int CrashWriter(const QString& Directory, int Phase)
{
    auto Store = OpenDurableOutcomes(QDir(Directory).filePath("writes.items")); if (!Store.HasValue()) return 2;
    auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
    const Locator Target{LocatorKind::Local, QDir(Directory).filePath("target").toStdString()};
    auto Original = Writer->Inspect(Target, {}); if (!Original.HasValue() || !Original.Value()) return 3;
    OperationContext Context; Context.expectedWriteBytes_ = 11;
    Context.writeCheckpoint_ = [Store = Store.Value(), Phase](const WriteRecoveryRecord& Record, std::optional<uint64_t> Row) -> Result<uint64_t> {
        // Stop after a rename but before its next durable checkpoint.
        if (Phase == 10 + int(Record.phase_)) TerminateProcess(GetCurrentProcess(), 77);
        auto Item = EncodeWriteRecovery(Record);
        Result<uint64_t> Saved = Row ? Result<uint64_t>(*Row) : Store->Append(Item);
        if (Row) { auto Replaced = Store->Replace(*Row, Item); if (!Replaced.HasValue()) return Replaced.Failure(); }
        if (!Saved.HasValue()) return Saved.Failure();
        if (int(Record.phase_) == Phase) TerminateProcess(GetCurrentProcess(), 77);
        return Saved;
    };
    auto Session = Writer->BeginWrite(Target, {true, Original.Value()->id_.revision_}, Context); if (!Session.HasValue()) return 4;
    const QByteArray Payload("replacement");
    auto Written = Session.Value()->Append({reinterpret_cast<const std::byte*>(Payload.constData()), size_t(Payload.size())}, Context);
    if (!Written.HasValue()) return 5;
    return Session.Value()->Commit(Context).HasValue() ? 0 : 6;
}
}
class RecoveryTests : public QObject {
    Q_OBJECT
private slots:
    void durableResultsAndTailRecovery()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("results.items");
        auto Store = OpenDurableOutcomes(Path); QVERIFY(Store.HasValue());
        for (uint64_t Row = 0; Row < 10001; ++Row) {
            auto Added = Store.Value()->Append({std::to_string(Row), ItemOutcome::Status::Completed, {}, true, false});
            QVERIFY(Added.HasValue()); QCOMPARE(Added.Value(), Row);
        }
        QVERIFY(Store.Value()->Replace(9999, {"9999", ItemOutcome::Status::Failed, Error{ErrorCategory::Unavailable, "partial"}, true, false}).HasValue());
        Store.Value().reset();
        QFile Tail(Path); QVERIFY(Tail.open(QIODevice::Append)); Tail.write("partial-frame"); Tail.close();
        auto Reopened = OpenDurableOutcomes(Path); QVERIFY(Reopened.HasValue()); QCOMPARE(Reopened.Value()->Count(), 10001ull);
        auto Last = Reopened.Value()->Read(9999, 20); QVERIFY(Last.HasValue()); QCOMPARE(Last.Value().items_.size(), size_t(2)); QVERIFY(Last.Value().end_);
        QCOMPARE(Last.Value().items_[0].status_, ItemOutcome::Status::Failed); QVERIFY(Last.Value().items_[0].destinationCommitted_);
        QCOMPARE(Last.Value().items_[1].item_, "10000");
        Reopened.Value().reset();
        QFile Corrupt(Path); QVERIFY(Corrupt.open(QIODevice::ReadWrite)); QVERIFY(Corrupt.seek(52));
        auto Byte = Corrupt.read(1); QVERIFY(Byte.size() == 1); Byte[0] ^= 1;
        QVERIFY(Corrupt.seek(52)); QCOMPARE(Corrupt.write(Byte), qint64(1)); Corrupt.close();
        auto Rejected = OpenDurableOutcomes(Path); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, "outcome-log-checksum");
    }
    void schedulerPagedResults()
    {
        QTemporaryDir Directory; auto Journal = MakeFileJobJournal(Directory.path());
        class Task final : public IJobTask {
        public:
            JobStep Advance(JobStepContext) override {
                JobStep Step{JobState::Running};
                if (row_ < 1001) { Step.item_ = ItemOutcome{std::to_string(row_++), ItemOutcome::Status::Completed, {}, true, false}; return Step; }
                Step.next_ = JobState::Completed; Step.item_ = ItemOutcome{"999", ItemOutcome::Status::Completed, {}, true, true}; Step.replaceItem_ = 999; return Step;
            }
            std::vector<Error> Finalize(JobState) override { return {}; }
        private: uint64_t row_ = 0;
        };
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 16, Journal);
        auto Job = Scheduler.Submit({}, std::make_unique<Task>()); QVERIFY(Job.HasValue());
        const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Completed);
        QCOMPARE(Done.items_.size(), size_t(200)); QCOMPARE(Done.outcomes_->Count(), 1001ull); QCOMPARE(Done.snapshot_.completedItems_, 1001ull);
        auto Page = Done.outcomes_->Read(999, 2); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().items_[0].sourceRemoved_);
        auto Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value().size(), size_t(1));
        QCOMPARE(Recovered.Value()[0].outcomes_->Count(), 1001ull);
        QVERIFY(QFileInfo(Directory.filePath(QString::fromStdString(Job.Value().id_) + ".json")).size() < 4096);
        QVERIFY(Journal->Forget(Job.Value().id_).HasValue()); QVERIFY(Journal->Recover().Value().empty());
    }
    void crashRecovery_data()
    {
        QTest::addColumn<int>("Phase");
        for (int Phase = 0; Phase <= int(WritePhase::Finished); ++Phase) QTest::newRow(qPrintable(QString::number(Phase))) << Phase;
        QTest::newRow("backup-renamed-before-checkpoint") << 12;
        QTest::newRow("stage-published-before-checkpoint") << 13;
    }
    void crashRecovery()
    {
        QFETCH(int, Phase); QTemporaryDir Directory; const auto Target = Directory.filePath("target"); QVERIFY(Put(Target, "original"));
        QProcess Child; Child.start(QCoreApplication::applicationFilePath(), {"--crash-writer", Directory.path(), QString::number(Phase)});
        QVERIFY(Child.waitForFinished(30000)); QCOMPARE(Child.exitCode(), 77);
        auto Store = OpenDurableOutcomes(Directory.filePath("writes.items")); QVERIFY(Store.HasValue()); QCOMPARE(Store.Value()->Count(), 1ull);
        auto Page = Store.Value()->Read(0, 1); QVERIFY(Page.HasValue());
        auto Record = DecodeWriteRecovery(Page.Value().items_[0]); QVERIFY(Record.HasValue()); QCOMPARE(int(Record.Value().phase_), Phase >= 10 ? Phase - 11 : Phase);
        if (Phase == int(WritePhase::BackedUp) || Phase == 12) {
            QVERIFY(!QFile::exists(Target)); auto Changed = Record.Value(); Changed.originalIdentity_ += "bad";
            QVERIFY(!RecoverNativeWrite(Changed).HasValue()); QVERIFY(!QFile::exists(Target));
        }
        auto Recovered = RecoverNativeWrite(Record.Value()); QVERIFY2(Recovered.HasValue(), Recovered.HasValue() ? "" : Recovered.Failure().code_.c_str());
        const bool Published = Phase == 13 || (Phase < 10 && Phase >= int(WritePhase::Published));
        QCOMPARE(Recovered.Value().destinationCommitted_, Published); QCOMPARE(Read(Target), Published ? QByteArray("replacement") : QByteArray("original"));
        auto Again = RecoverNativeWrite(Record.Value()); QVERIFY(Again.HasValue()); QCOMPARE(Read(Target), Published ? QByteArray("replacement") : QByteArray("original"));
        for (const auto& Artifact : Recovered.Value().preservedArtifacts_) QVERIFY(QFile::exists(QString::fromStdString(Artifact.root_)));
    }
    void reservationFailurePreservesOriginal()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("target"); QVERIFY(Put(Path, "original"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        Locator At{LocatorKind::Local, Path.toStdString()}; auto Original = Writer->Inspect(At, {}); QVERIFY(Original.HasValue());
        OperationContext Context; Context.expectedWriteBytes_ = uint64_t(INT64_MAX) - 65535;
        auto Session = Writer->BeginWrite(At, {true, Original.Value()->id_.revision_}, Context); QVERIFY(!Session.HasValue());
        QCOMPARE(Read(Path), QByteArray("original")); QCOMPARE(QDir(Directory.path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden).size(), 0);
    }
    void checkpointFailure_data()
    {
        QTest::addColumn<bool>("Published");
        QTest::newRow("backup-record-failure") << false;
        QTest::newRow("published-record-failure") << true;
    }
    void checkpointFailure()
    {
        QFETCH(bool, Published); QTemporaryDir Directory; const auto Path = Directory.filePath("target"); QVERIFY(Put(Path, "original"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        Locator At{LocatorKind::Local, Path.toStdString()}; auto Original = Writer->Inspect(At, {}); QVERIFY(Original.HasValue());
        WriteRecoveryRecord Durable; OperationContext Context;
        Context.writeCheckpoint_ = [&](const WriteRecoveryRecord& Record, std::optional<uint64_t>) -> Result<uint64_t> {
            if (Record.phase_ == (Published ? WritePhase::Published : WritePhase::BackedUp)) return Error{ErrorCategory::Unavailable, "injected-checkpoint-failure"};
            Durable = Record; return uint64_t(0);
        };
        auto Session = Writer->BeginWrite(At, {true, Original.Value()->id_.revision_}, Context); QVERIFY(Session.HasValue());
        const QByteArray Data("replacement");
        QVERIFY(Session.Value()->Append({reinterpret_cast<const std::byte*>(Data.constData()), size_t(Data.size())}, Context).HasValue());
        auto Receipt = Session.Value()->Commit(Context); QCOMPARE(Receipt.HasValue(), Published);
        if (Published) { QVERIFY(!Receipt.Value().cleanupErrors_.empty()); QCOMPARE(Receipt.Value().preservedArtifacts_.size(), size_t(1)); }
        QVERIFY(Session.Value()->Abort().HasValue()); Session.Value().reset();
        QCOMPARE(Read(Path), Published ? Data : QByteArray("original"));
        if (Published) { auto Recovered = RecoverNativeWrite(Durable); QVERIFY(Recovered.HasValue()); QVERIFY(Recovered.Value().destinationCommitted_); }
    }
    void cancelPreservesOriginalAndJournal()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("target"); QVERIFY(Put(Path, "original"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        Locator At{LocatorKind::Local, Path.toStdString()}; auto Original = Writer->Inspect(At, {}); QVERIFY(Original.HasValue());
        auto Store = OpenDurableOutcomes(Directory.filePath("writes.items")); QVERIFY(Store.HasValue());
        OperationContext Context;
        Context.writeCheckpoint_ = [Store = Store.Value()](const WriteRecoveryRecord& Record, std::optional<uint64_t> Row) -> Result<uint64_t> {
            if (!Row) return Store->Append(EncodeWriteRecovery(Record));
            auto Saved = Store->Replace(*Row, EncodeWriteRecovery(Record)); if (!Saved.HasValue()) return Saved.Failure(); return *Row;
        };
        auto Session = Writer->BeginWrite(At, {true, Original.Value()->id_.revision_}, Context); QVERIFY(Session.HasValue());
        std::stop_source Stop; Context.stopToken_ = Stop.get_token(); Stop.request_stop();
        auto Receipt = Session.Value()->Commit(Context); QVERIFY(!Receipt.HasValue()); QCOMPARE(Receipt.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(Session.Value()->Abort().HasValue()); QCOMPARE(Read(Path), QByteArray("original"));
        QCOMPARE(DecodeWriteRecovery(Store.Value()->Read(0, 1).Value().items_[0]).Value().phase_, WritePhase::Aborted);
        QCOMPARE(QDir(Directory.path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden).size(), 0);
    }
    void manifestBatchAndPartialFailure()
    {
        QTemporaryDir Directory; QVERIFY(QDir().mkdir(Directory.filePath("source"))); QVERIFY(QDir().mkdir(Directory.filePath("target")));
        for (int I = 0; I < 205; ++I) QVERIFY(Put(Directory.filePath("source/" + QString::number(I)), "payload"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        Locator Source{LocatorKind::Local, QDir::toNativeSeparators(Directory.filePath("source")).toStdString()};
        auto Cursor = Reader->Enumerate(Source, {}).get(); QVERIFY(Cursor.HasValue());
        auto Index = PagedEntryIndex::Build(*Cursor.Value(), {}); QVERIFY(Index.HasValue());
        auto All = Index.Value()->SelectAll(); QVERIFY(All.HasValue());
        auto Manifest = std::make_shared<Selection>(All.Value(), Reader);
        // A stale selected source fails independently; the remaining roots run.
        QVERIFY(QFile::remove(Directory.filePath("source/100")));
        FileOperationRequest Request; Request.selection_ = Manifest; Request.selectionRoot_ = Source;
        Request.destination_ = {LocatorKind::Local, Directory.filePath("target").toStdString()};
        Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        auto Journal = MakeFileJobJournal(Directory.filePath("journal")); JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2, 16, Journal);
        auto Job = SubmitFileOperation(Scheduler, Request); QVERIFY(Job.HasValue());
        auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Failed);
        QCOMPARE(Done.snapshot_.failedItems_, 1ull); QCOMPARE(Done.snapshot_.completedItems_, 204ull);
        QCOMPARE(Done.outcomes_->Count(), 205ull); QCOMPARE(QDir(Directory.filePath("target")).entryList(QDir::Files).size(), 204);
        QCOMPARE(Done.writes_->Count(), 204ull);
    }
};
int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    if (App.arguments().size() == 4 && App.arguments()[1] == "--crash-writer") return CrashWriter(App.arguments()[2], App.arguments()[3].toInt());
    RecoveryTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstOperationRecovery.moc"
