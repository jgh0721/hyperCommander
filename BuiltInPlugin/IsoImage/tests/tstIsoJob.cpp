#include <QtTest>
#include <QTemporaryDir>
#include "BuiltInPlugin/IsoImage/isoJob.hpp"
#include "BuiltInPlugin/IsoImage/builtinIso.hpp"
#include "UniqueLibs/storageProviders.hpp"

using namespace hc::storage;
using namespace hc::operations;
namespace {
bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
}
class IsoJobTests : public QObject {
    Q_OBJECT
private slots:
    void schedulerCommitAndCancel()
    {
        QTemporaryDir Directory; const auto Source = Directory.filePath("한글.txt"), Target = Directory.filePath("result.iso"); QVERIFY(Put(Source, QByteArray(4097, 'a')));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Reader->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        hc::iso::CreateRequest Request; Request.sources_.push_back({Entry.Value(), Reader, {}}); Request.destination_ = {LocatorKind::Local, Target.toStdString()};
        auto Prepared = hc::iso::PrepareCreate(Request); QVERIFY(Prepared.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Submitted.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Submitted.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        const auto Result = Submitted.Value().completion_.get();
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "unexpected job state");
        QVERIFY(Result.cleanupErrors_.empty()); QVERIFY(QFileInfo(Target).size() > 256 * 2048);
        auto Backend = hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinIsoOptions()); auto Cursor = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(10, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1)); QCOMPARE(Page.Value().entries_[0].name_, std::string("한글.txt")); Cursor.Value().reset();
        const auto Original = Read(Target);
        auto Again = hc::iso::PrepareCreate(Request); QVERIFY(Again.HasValue());
        auto Pending = Scheduler.Submit(std::move(Again.Value().request_), std::move(Again.Value().task_)); QVERIFY(Pending.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Pending.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        QCOMPARE(Read(Target), Original); QVERIFY(Scheduler.Cancel(Pending.Value().id_).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Pending.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 5000);
        QCOMPARE(Pending.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(Read(Target), Original);
        auto Replace = hc::iso::PrepareCreate(Request); QVERIFY(Replace.HasValue());
        auto Replacement = Scheduler.Submit(std::move(Replace.Value().request_), std::move(Replace.Value().task_)); QVERIFY(Replacement.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Replacement.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        const auto Waiting = Scheduler.Snapshot(Replacement.Value().id_).Value();
        QVERIFY(Scheduler.Reply(Replacement.Value().id_, {Waiting.interaction_->id_, InteractionChoice::Replace, {}}).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Replacement.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        const auto Replaced = Replacement.Value().completion_.get();
        QVERIFY2(Replaced.snapshot_.state_ == JobState::Completed, Replaced.snapshot_.error_ ? Replaced.snapshot_.error_->code_.c_str() : "replacement failed");
        QCOMPARE(Read(Target), Original);
    }
};
QTEST_GUILESS_MAIN(IsoJobTests)
#include "tstIsoJob.moc"
