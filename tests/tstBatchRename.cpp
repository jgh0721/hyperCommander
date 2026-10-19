#include <QtTest>
#include <QProcess>
#include <QScopeGuard>
#include "UniqueLibs/batchRenameJournal.hpp"
#include "UniqueLibs/qtJobJournal.hpp"

using namespace hc::filetools;
using namespace hc::operations;
using namespace hc::storage;
namespace {
QList<RenameRow> Fixture(const QString& Folder, bool Swap = false) {
    QList<RenameRow> Rows;
    for (int I = 0; I < 3; ++I) {
        const auto Source = QDir(Folder).filePath(QString("original%1.txt").arg(I));
        QFile File(Source); if (!File.open(QIODevice::WriteOnly)) return {}; File.write(QByteArray::number(I)); File.close();
        Rows.append({Inspect(Source), QDir(Folder).filePath(Swap ? QString("original%1.txt").arg((I+1)%3) : QString("renamed%1.txt").arg(I)), {}});
    }
    return Rows;
}
QString IdOf(const PreparedJob& Job) { return QString::fromStdString(Job.request_.metadata_.options_).mid(QString("rename-record=").size()); }
JobResult Run(JobScheduler& Scheduler, PreparedJob Job) {
    auto Submitted = Scheduler.Submit(std::move(Job.request_), std::move(Job.task_));
    if (!Submitted.HasValue()) return {};
    if (Submitted.Value().completion_.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return {};
    return Submitted.Value().completion_.get();
}
bool Contents(const QString& Path, int I) { QFile File(Path); return File.open(QIODevice::ReadOnly) && File.readAll() == QByteArray::number(I); }
class PacedTask final : public IJobTask {
public:
    explicit PacedTask(std::unique_ptr<IJobTask> Inner) : inner_(std::move(Inner)) {}
    JobStep Advance(JobStepContext Context) override { auto Step = inner_->Advance(std::move(Context)); if (Step.next_ == JobState::Running) Step.deferFor_ = std::chrono::milliseconds(30); return Step; }
    std::vector<Error> Finalize(JobState State) override { return inner_->Finalize(State); }
private:
    std::unique_ptr<IJobTask> inner_;
};
class LaneMarker final : public IJobTask {
public:
    explicit LaneMarker(std::atomic<bool>& Seen) : seen_(Seen) {}
    JobStep Advance(JobStepContext) override { seen_ = true; JobStep Step; Step.next_ = JobState::Completed; return Step; }
    std::vector<Error> Finalize(JobState) override { return {}; }
private:
    std::atomic<bool>& seen_;
};
}
class BatchRenameTests : public QObject {
    Q_OBJECT
private slots:
    void sharingViolationWaitsBeforeAnyRename() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); QCOMPARE(Rows.size(),3);
        const auto Path = NativePath(Rows[1].source_.path_);
        HANDLE Held = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(Held!=INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&]{if(Held!=INVALID_HANDLE_VALUE)CloseHandle(Held);});
        auto Prepared=PrepareBatchRename(Journal.path(),Rows);QVERIFY(Prepared.HasValue());auto Job=std::move(Prepared.Value());
        const auto RecordId=IdOf(Job);JobStepContext Context;Context.jobId_="owned-rename-lock";Context.operation_.providerInteractions_=true;
        auto Cleanup=qScopeGuard([&]{Job.task_->Finalize(JobState::Cancelled);});
        JobStep Step;
        for(int I=0;I<200;++I){Step=Job.task_->Advance(Context);Context.committing_=Step.next_==JobState::Committing;if(Step.next_==JobState::WaitingForUser||IsTerminal(Step.next_))break;}
        QCOMPARE(Step.next_,JobState::WaitingForUser);QVERIFY(Step.interaction_);QVERIFY(Step.interaction_->reason_.starts_with("file-in-use:"));
        for(const auto& Row:Rows){QVERIFY(QFile::exists(Row.source_.path_));QVERIFY(!QFile::exists(Row.target_));}
        CloseHandle(Held);Held=INVALID_HANDLE_VALUE;Context.answer_=InteractionAnswer{"",InteractionChoice::Retry,{}};
        for(int I=0;I<300;++I){Step=Job.task_->Advance(Context);Context.answer_.reset();Context.committing_=Step.next_==JobState::Committing;if(IsTerminal(Step.next_))break;}
        QCOMPARE(Step.next_,JobState::Completed);Job.task_->Finalize(JobState::Completed);Cleanup.dismiss();
        for(int I=0;I<Rows.size();++I)QVERIFY(Contents(Rows[I].target_,I));
        const auto Record=ReadRenameRecord(Journal.path(),RecordId);QVERIFY(Record.HasValue());QCOMPARE(Record.Value().state_,RenameState::Completed);
    }
    void queueLaneAndDurableUndo() {
        QTemporaryDir Data, Journal, General; QVERIFY(Data.isValid()); const auto Rows = Fixture(Data.path(), true); QCOMPARE(Rows.size(), 3);
        auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Id = IdOf(Prepared.Value());
        QCOMPARE(Prepared.Value().request_.resourceLanes_.size(), std::size_t(1)); QVERIFY(Prepared.Value().request_.resourceLanes_.front().starts_with("volume:"));
        Prepared.Value().request_.deferred_ = true;
        {
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2, 1024, MakeFileJobJournal(General.path()));
            auto Started = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Started.HasValue());
            QCOMPARE(Started.Value().completion_.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
            QVERIFY(Contents(Rows[0].source_.path_, 0)); QVERIFY(Scheduler.Control({Started.Value().id_}, JobControl::Start).HasValue());
            QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
            const auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Completed); QCOMPARE(Result.snapshot_.completedItems_, std::uint64_t(3));
        }
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].target_, I));
        auto Saved = ReadRenameRecord(Journal.path(), Id); QVERIFY(Saved.HasValue()); QCOMPARE(Saved.Value().state_, RenameState::Completed);
        auto History = ReadRenameHistory(Journal.path()); QVERIFY(History.HasValue()); QCOMPARE(History.Value().size(), 1); QCOMPARE(History.Value().first().count_, 3);
        JobScheduler Restart(std::make_shared<hc::commands::EventHub>());
        auto Undo = PrepareRenameUndo(Journal.path(), Id); QVERIFY(Undo.HasValue()); const auto UndoId = IdOf(Undo.Value());
        QCOMPARE(Run(Restart, std::move(Undo.Value())).snapshot_.state_, JobState::Completed);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].source_.path_, I));
        QVERIFY(!PrepareRenameUndo(Journal.path(), Id).HasValue());
        auto Redo = PrepareRenameUndo(Journal.path(), UndoId); QVERIFY(Redo.HasValue());
        QCOMPARE(Run(Restart, std::move(Redo.Value())).snapshot_.state_, JobState::Completed);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].target_, I));
    }
    void cancelDuringMutationRestores() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue());
        const auto Id = IdOf(Prepared.Value()); auto Record = ReadRenameRecord(Journal.path(), Id); QVERIFY(Record.HasValue());
        auto Task = std::move(Prepared.Value().task_); JobStepContext Context; Context.jobId_ = "hc-job-fixture";
        for (int I = 0; I < 30; ++I) {
            const auto Step = Task->Advance(Context); Context.committing_ = Step.next_ == JobState::Committing;
            if (QFileInfo::exists(Record.Value().temporary_[0])) break;
        }
        QVERIFY(QFileInfo::exists(Record.Value().temporary_[0])); QVERIFY(Task->Finalize(JobState::Cancelled).empty()); Task.reset();
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].source_.path_, I));
        QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, RenameState::Restored);
        QVERIFY(QDir(Data.path()).entryList({".hc-rename-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void crashRecovery_data() { QTest::addColumn<bool>("Restore"); QTest::addColumn<bool>("TargetPhase"); QTest::newRow("resume-temp") << false << false; QTest::newRow("restore-temp") << true << false; QTest::newRow("resume-target") << false << true; QTest::newRow("restore-target") << true << true; }
    void crashRecovery() {
        QFETCH(bool, Restore); QFETCH(bool, TargetPhase); QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path(), true);
        auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Id = IdOf(Prepared.Value()); Prepared.Value().task_.reset();
        QProcess Child; Child.start(QCoreApplication::applicationFilePath(), {"--crash", Journal.path(), Id, TargetPhase ? "target" : "temporary"});
        QVERIFY(Child.waitForFinished(30000)); QCOMPARE(Child.exitCode(), 17);
        auto Record = ReadRenameRecord(Journal.path(), Id); QVERIFY(Record.HasValue()); QCOMPARE(Record.Value().state_, RenameState::Running);
        // No implicit restart replay; inspection leaves the crash state intact.
        QVERIFY(ReadRenameHistory(Journal.path()).HasValue()); QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, RenameState::Running);
        auto Recovery = PrepareRenameRecovery(Journal.path(), Id, Restore); QVERIFY(Recovery.HasValue());
        JobScheduler Restart(std::make_shared<hc::commands::EventHub>()); const auto Result = Run(Restart, std::move(Recovery.Value()));
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "not completed");
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Restore ? Rows[I].source_.path_ : Rows[I].target_, I));
        QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, Restore ? RenameState::Restored : RenameState::Completed);
        QVERIFY(QDir(Data.path()).entryList({".hc-rename-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void replacementAndCollisionArePreserved() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Id = IdOf(Prepared.Value());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); QCOMPARE(Run(Scheduler, std::move(Prepared.Value())).snapshot_.state_, JobState::Completed);
        QFile Occupant(Rows[0].source_.path_); QVERIFY(Occupant.open(QIODevice::WriteOnly)); Occupant.write("foreign"); Occupant.close();
        auto Undo = PrepareRenameUndo(Journal.path(), Id); QVERIFY(Undo.HasValue()); const auto Failed = Run(Scheduler, std::move(Undo.Value()));
        QCOMPARE(Failed.snapshot_.state_, JobState::Failed);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].target_, I));
        QVERIFY(Occupant.open(QIODevice::ReadOnly)); QCOMPARE(Occupant.readAll(), QByteArray("foreign")); Occupant.close();
        QVERIFY(QFile::remove(Rows[1].target_)); QFile Replacement(Rows[1].target_); QVERIFY(Replacement.open(QIODevice::WriteOnly)); Replacement.write("replacement"); Replacement.close();
        Undo = PrepareRenameUndo(Journal.path(), Id); QVERIFY(Undo.HasValue()); QCOMPARE(Run(Scheduler, std::move(Undo.Value())).snapshot_.state_, JobState::Failed);
        QVERIFY(Replacement.open(QIODevice::ReadOnly)); QCOMPARE(Replacement.readAll(), QByteArray("replacement"));
    }
    void malformedOrUnavailableJournalDoesNotMutate() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); QFile Blocked(Journal.filePath("blocked")); QVERIFY(Blocked.open(QIODevice::WriteOnly)); Blocked.close();
        QVERIFY(!PrepareBatchRename(Blocked.fileName(), Rows).HasValue());
        auto Invalid = Rows; Invalid[0].target_ = QDir(Data.path()).filePath("../escape.txt"); QVERIFY(!PrepareBatchRename(Journal.path(), Invalid).HasValue());
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].source_.path_, I));
        QVERIFY(ReadRenameHistory(Journal.path()).HasValue()); QCOMPARE(ReadRenameHistory(Journal.path()).Value().size(), 0);
    }
    void cancelledQueueAndStaleRecovery() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Id = IdOf(Prepared.Value());
        Prepared.Value().request_.deferred_ = true; JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Submitted.HasValue());
        QVERIFY(Scheduler.Cancel(Submitted.Value().id_).HasValue()); QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, RenameState::Restored);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].source_.path_, I));
        Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Next = IdOf(Prepared.Value());
        auto Stale = PrepareRenameRecovery(Journal.path(), Next, true); QVERIFY(Stale.HasValue());
        QCOMPARE(Run(Scheduler, std::move(Prepared.Value())).snapshot_.state_, JobState::Completed);
        QCOMPARE(Run(Scheduler, std::move(Stale.Value())).snapshot_.state_, JobState::Failed);
        QCOMPARE(ReadRenameRecord(Journal.path(), Next).Value().state_, RenameState::Completed);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].target_, I));
    }
    void receiptGapAndPartialTail() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue()); const auto Id = IdOf(Prepared.Value()); Prepared.Value().task_.reset();
        auto Record = ReadRenameRecord(Journal.path(), Id); QVERIFY(Record.HasValue());
        QFile Log(QDir(Journal.path()).filePath(Id + ".log")); QVERIFY(Log.open(QIODevice::WriteOnly));
        Log.write("{\"event\":\"state\",\"state\":1}\n{\"event\":\"intent\",\"index\":0,\"destination\":\"temporary\"}\n{\"event\":"); Log.close();
        const auto Native = NativePath(Rows[0].source_.path_); Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        QVERIFY(File.value_ != INVALID_HANDLE_VALUE); QCOMPARE(RenameHandle(File.value_, Record.Value().temporary_[0]), DWORD(ERROR_SUCCESS)); CloseHandle(File.value_); File.value_ = INVALID_HANDLE_VALUE;
        auto Recovery = PrepareRenameRecovery(Journal.path(), Id, false); QVERIFY(Recovery.HasValue()); JobScheduler Restart(std::make_shared<hc::commands::EventHub>());
        QCOMPARE(Run(Restart, std::move(Recovery.Value())).snapshot_.state_, JobState::Completed);
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].target_, I));
        QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, RenameState::Completed);
    }
    void sharedLanePauseAndSchedulerCancellation() {
        QTemporaryDir Data, Journal; const auto Rows = Fixture(Data.path()); auto Prepared = PrepareBatchRename(Journal.path(), Rows); QVERIFY(Prepared.HasValue());
        const auto Id = IdOf(Prepared.Value()); const auto Lane = Prepared.Value().request_.resourceLanes_; auto Record = ReadRenameRecord(Journal.path(), Id); QVERIFY(Record.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2); Prepared.Value().task_ = std::make_unique<PacedTask>(std::move(Prepared.Value().task_));
        auto Rename = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Rename.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Record.Value().temporary_[0]), 5000);
        QVERIFY(Scheduler.Pause(Rename.Value().id_, true).HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Rename.Value().id_).Value().state_, JobState::Paused);
        std::atomic<bool> Seen = false; JobRequest Other; Other.resourceLanes_ = Lane; auto Marker = Scheduler.Submit(std::move(Other), std::make_unique<LaneMarker>(Seen)); QVERIFY(Marker.HasValue());
        QTest::qWait(100); QVERIFY(!Seen.load()); QVERIFY(QFileInfo::exists(Record.Value().temporary_[0]));
        QVERIFY(Scheduler.Cancel(Rename.Value().id_).HasValue()); QVERIFY(Rename.Value().completion_.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        QCOMPARE(Rename.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QVERIFY(Marker.Value().completion_.wait_for(std::chrono::seconds(10)) == std::future_status::ready); QVERIFY(Seen.load());
        for (int I = 0; I < 3; ++I) QVERIFY(Contents(Rows[I].source_.path_, I));
        QCOMPARE(ReadRenameRecord(Journal.path(), Id).Value().state_, RenameState::Restored);
    }
};
int main(int Argc, char** Argv) {
    QCoreApplication App(Argc, Argv);
    const auto Args = App.arguments();
    if (Args.size() == 5 && Args[1] == "--crash") {
        auto Record = ReadRenameRecord(Args[2], Args[3]); if (!Record.HasValue()) return 3;
        auto Prepared = PrepareRenameRecovery(Args[2], Args[3], false); if (!Prepared.HasValue()) return 4;
        auto Task = std::move(Prepared.Value().task_); JobStepContext Context; Context.jobId_ = "hc-job-crash-fixture";
        for (int I = 0; I < 100; ++I) {
            const auto Step = Task->Advance(Context); if (Step.next_ == JobState::Failed) return 5;
            Context.committing_ = Step.next_ == JobState::Committing;
            const auto Path = Args[4] == "target" ? Record.Value().rows_[0].target_ : Record.Value().temporary_[0];
            if (Inspect(Path).identity_ == Record.Value().rows_[0].source_.identity_) ExitProcess(17);
        }
        return 6;
    }
    BatchRenameTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstBatchRename.moc"
