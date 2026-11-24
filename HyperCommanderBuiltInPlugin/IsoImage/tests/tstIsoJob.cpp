#include <QtTest>
#include <QTemporaryDir>
#include "HyperCommanderBuiltInPlugin/IsoImage/isoJob.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/isoProvider.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/isoRewrite.hpp"
#include <Windows.h>

using namespace hc::storage;
using namespace hc::operations;
namespace {
bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
}
class IsoJobTests : public QObject {
    Q_OBJECT
private slots:
    void extractMetadata_data()
    {
        QTest::addColumn<unsigned>("Profile");
        for (unsigned Profile = 0; Profile <= 3; ++Profile) QTest::newRow(QByteArray::number(Profile)) << Profile;
    }
    void extractMetadata()
    {
        QFETCH(unsigned, Profile);
        QTemporaryDir Directory;
        const auto Input = Directory.filePath("payload");
        QVERIFY(Put(Input, "contents"));
        constexpr int64_t Modified = 1666666666123LL;
        hc::iso::RewriteRequest Build; Build.profile_ = Profile;
        Build.sources_ = {{"", "FOLDER", true, {Modified, 0x27, 0x27, 1}},
            {Input.toStdString(), "FOLDER/FILE", false, {Modified + 1, 0x27, 0x27, 1}}};
        auto Image = hc::iso::PrepareRewrite(Build).get();
        QVERIFY2(Image.HasValue(), Image.HasValue() ? "" : Image.Failure().code_.c_str());
        Locator Root{LocatorKind::Archive, Image.Value()->Path().toStdString(), {{"wcx", "/"}}};
        auto Provider = hc::iso::MakeReadProvider(Root);
        Root.nesting_.back().entry_ = "/FOLDER";
        auto Entry = Provider->Resolve(Root, {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Copy; Copy.sources_ = {{Entry.Value(), Provider, {}}};
        const auto Share = qEnvironmentVariable("HC_TEST_ARCHIVE_OUTPUT_ROOT");
        QTemporaryDir Output(QDir(Share.isEmpty() ? Directory.path() : Share).filePath("iso-metadata-XXXXXX"));
        QVERIFY(Output.isValid());
        const auto Kind = Share.isEmpty() ? LocatorKind::Local : LocatorKind::Smb;
        Copy.destination_ = {Kind, Output.path().toStdString()};
        Copy.destinationWriter_ = MakeNativeWritableStorage(Kind, MakeNativeReadProvider(Kind));
        const auto Folder = Output.filePath("FOLDER"), File = Folder + "/FILE";
        const auto Reset = qScopeGuard([&] {
            SetFileAttributesW(reinterpret_cast<LPCWSTR>(File.utf16()), FILE_ATTRIBUTE_NORMAL);
            SetFileAttributesW(reinterpret_cast<LPCWSTR>(Folder.utf16()), FILE_ATTRIBUTE_NORMAL);
        });
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Job = SubmitFileOperation(Scheduler, Copy); QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
        const auto Result = Job.Value().completion_.get();
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "extract");
        QCOMPARE(Read(File), QByteArray("contents"));
        for (const auto& Path : {File, Folder}) {
            const auto Flags = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Path.utf16()));
            QVERIFY(Flags != INVALID_FILE_ATTRIBUTES); QCOMPARE(Flags & 0x27u, 0x27u);
            QCOMPARE(QFileInfo(Path).lastModified().toMSecsSinceEpoch(), Modified + (Path == File));
        }
    }
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
        QCOMPARE(Page.Value().entries_[0].modifiedUtcMs_, Entry.Value().modifiedUtcMs_);
        QCOMPARE(Page.Value().entries_[0].attributes_ & 0x27u, Entry.Value().attributes_ & 0x27u);
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
