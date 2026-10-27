#include <QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "UniqueLibs/qtItemOutcomeStore.hpp"
#include "UniqueLibs/qtJobJournal.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/operations/writeRecovery.hpp"
#include <Windows.h>
#include <QLockFile>

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
    void deferredReceiptRecoveryRetainsMetadataWithoutReplay()
    {
        class Task final : public IJobTask {
        public:
            explicit Task(std::atomic<int>& Calls) : calls_(Calls) {}
            JobStep Advance(JobStepContext) override { ++calls_; return JobStep{JobState::Completed}; }
            std::vector<Error> Finalize(JobState) override { return {}; }
        private: std::atomic<int>& calls_;
        };
        QTemporaryDir Directory; auto Journal = MakeFileJobJournal(Directory.path()); std::atomic<int> Calls{0};
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 16, Journal);
        JobRequest Request; Request.deferred_ = true; Request.metadata_ = {"압축 작업", "source", "target", "format=7z", {}};
        auto Handle = Scheduler.Submit(Request, std::make_unique<Task>(Calls)); QVERIFY(Handle.HasValue());
        QTRY_VERIFY(QFile::exists(Directory.filePath(QString::fromStdString(Handle.Value().id_) + ".json")));
        auto Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value().size(), size_t(1));
        const auto Snapshot = Recovered.Value().front().snapshot_; QCOMPARE(Snapshot.metadata_.label_, std::string("압축 작업")); QCOMPARE(Snapshot.metadata_.options_, std::string("format=7z"));
        QVERIFY(Snapshot.submittedUtcMs_ > 0); QVERIFY(Snapshot.queueOrder_ > 0); QCOMPARE(Snapshot.state_, JobState::Failed); QVERIFY(!Snapshot.deferred_); QCOMPARE(Snapshot.error_->code_, std::string("job-interrupted")); QCOMPARE(Calls.load(), 0);
        QVERIFY(Scheduler.Cancel(Handle.Value().id_).HasValue()); QTRY_VERIFY(Handle.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QCOMPARE(Calls.load(), 0);
    }
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
    void derivedIndexModesAreWireCompatible()
    {
        QTemporaryDir Directory;
        for (const bool Buffered:{false,true}) {
            const auto Path=Directory.filePath(Buffered?"buffered.items":"legacy.items");
            { auto Store=OpenDurableOutcomes(Path,Buffered); QVERIFY(Store.HasValue());
              for(uint64_t Row=0;Row<8300;++Row) { ItemOutcome Item; Item.item_=std::to_string(Row); QVERIFY(Store.Value()->Append(Item).HasValue()); }
              for(uint64_t Row:{0ull,4095ull,4096ull,8191ull,8192ull,8299ull}) {
                  ItemOutcome Item; Item.item_="changed-"+std::to_string(Row); Item.sourceRemoved_=true; QVERIFY(Store.Value()->Replace(Row,Item).HasValue());
              }
              for(uint64_t Row:{0ull,4095ull,4096ull,8191ull,8192ull,8299ull}) {
                  auto Page=Store.Value()->Read(Row,1); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().items_[0].item_,"changed-"+std::to_string(Row)); QVERIFY(Page.Value().items_[0].sourceRemoved_);
              } }
            { auto Other=OpenDurableOutcomes(Path,!Buffered); QVERIFY(Other.HasValue()); QCOMPARE(Other.Value()->Count(),8300ull);
              for(uint64_t Row:{0ull,4095ull,4096ull,8191ull,8192ull,8299ull}) {
                  auto Page=Other.Value()->Read(Row,1); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().items_[0].item_,"changed-"+std::to_string(Row));
              }
              auto Untouched=Other.Value()->Read(5000,1); QVERIFY(Untouched.HasValue()); QCOMPARE(Untouched.Value().items_[0].item_,std::string("5000")); }
        }
        QCOMPARE(Read(Directory.filePath("legacy.items")),Read(Directory.filePath("buffered.items")));
    }
    void optionalRecoveryPersistence()
    {
        QTemporaryDir Directory; const auto JournalPath=Directory.filePath("journal");
        auto Journal=MakeFileJobJournal(JournalPath,false,true,false);
        JobResult Off; Off.snapshot_.id_="hc-job-r22-off"; Off.snapshot_.state_=JobState::Running;
        QVERIFY(Journal->Checkpoint(Off).HasValue());
        auto Items=Journal->OpenItems(Off.snapshot_.id_); QVERIFY(Items.HasValue() && Items.Value()); Off.outcomes_=Items.Value();
        auto Writes=Journal->OpenWrites(Off.snapshot_.id_); QVERIFY(Writes.HasValue()); QVERIFY(!Writes.Value());
        ItemOutcome Item; Item.item_="file.txt"; Item.destinationCommitted_=true; Item.sourceRemoved_=true;
        Item.status_=ItemOutcome::Status::Completed; Item.source_=EntrySnapshot{}; Item.source_->name_="original.txt";
        QVERIFY(Items.Value()->Append(Item).HasValue()); Item.status_=ItemOutcome::Status::Failed; QVERIFY(Items.Value()->Replace(0,Item).HasValue());
        auto Page=Items.Value()->Read(0,1); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().items_[0].status_,ItemOutcome::Status::Failed);
        QVERIFY(Page.Value().items_[0].sourceRemoved_); QCOMPARE(Page.Value().items_[0].source_->name_,std::string("original.txt"));
        SetFileJobPersistence(Journal,true,true); QVERIFY(Journal->Checkpoint(Off).HasValue());
        QVERIFY(!QDir(JournalPath).exists()); QVERIFY(!Journal->OpenWrites(Off.snapshot_.id_).Value());
        JobResult On; On.snapshot_.id_="hc-job-r22-on"; On.snapshot_.state_=JobState::Running;
        QVERIFY(Journal->Checkpoint(On).HasValue()); auto OnItems=Journal->OpenItems(On.snapshot_.id_); QVERIFY(OnItems.HasValue()); On.outcomes_=OnItems.Value();
        auto OnWrites=Journal->OpenWrites(On.snapshot_.id_); QVERIFY(OnWrites.HasValue() && OnWrites.Value()); On.writes_=OnWrites.Value();
        QVERIFY(On.outcomes_->Append(Item).HasValue()); QVERIFY(On.writes_->Append(Item).HasValue()); QVERIFY(Journal->Checkpoint(On).HasValue());
        const auto Before=Read(Directory.filePath("journal/hc-job-r22-on.json"));
        SetFileJobPersistence(Journal,false,false); On.snapshot_.progress_.bytesDone_=123;
        QVERIFY(Journal->Checkpoint(On).HasValue()); QVERIFY(Read(Directory.filePath("journal/hc-job-r22-on.json"))!=Before);
        auto Hidden=Journal->Recover(); QVERIFY(Hidden.HasValue()); QVERIFY(Hidden.Value().empty());
        JobResult Next; Next.snapshot_.id_="hc-job-r22-next"; QVERIFY(Journal->Checkpoint(Next).HasValue());
        QVERIFY(!QFile::exists(Directory.filePath("journal/hc-job-r22-next.json")));
        On=JobResult{}; OnItems.Value().reset(); OnWrites.Value().reset(); Journal.reset();
        auto Enabled=MakeFileJobJournal(JournalPath,false,true,true)->Recover(); QVERIFY(Enabled.HasValue()); QCOMPARE(Enabled.Value().size(),size_t(1));
        QCOMPARE(Enabled.Value()[0].snapshot_.id_,std::string("hc-job-r22-on")); QCOMPARE(Enabled.Value()[0].writes_->Count(),1ull);
    }
    void jobPresentationRoundTrip()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto Journal = MakeFileJobJournal(Directory.path(), true);
        JobResult Result; auto& Snapshot = Result.snapshot_; Snapshot.id_ = "hc-job-presentation";
        Snapshot.state_ = JobState::Completed; Snapshot.metadata_.kind_ = JobKind::ArchiveSeparate;
        Snapshot.metadata_.source_ = "D:/source"; Snapshot.metadata_.destination_ = "E:/target/output.7z";
        Snapshot.progress_.source_ = "D:/source/file.txt"; Snapshot.progress_.destination_ = "E:/target/output.7z";
        Snapshot.progress_.phase_ = JobPhase::SavingOutput; Snapshot.progress_.itemsDone_ = 7;
        Snapshot.progress_.outputIndex_ = 2; Snapshot.progress_.outputCount_ = 4; Snapshot.progress_.activeFiles_ = 3;
        QVERIFY(Journal->Checkpoint(Result).HasValue());
        auto Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value().size(), size_t(1));
        const auto& Copy = Recovered.Value().front().snapshot_;
        QCOMPARE(Copy.metadata_.kind_, JobKind::ArchiveSeparate); QCOMPARE(Copy.progress_.phase_, JobPhase::SavingOutput);
        QCOMPARE(Copy.progress_.source_, Snapshot.progress_.source_); QCOMPARE(Copy.progress_.destination_, Snapshot.progress_.destination_);
        QCOMPARE(Copy.progress_.itemsDone_.value(), 7ull); QCOMPARE(Copy.progress_.outputIndex_.value(), 2ull);
        QCOMPARE(Copy.progress_.outputCount_.value(), 4ull); QCOMPARE(Copy.progress_.activeFiles_, 3u);
    }
    void copyWithoutRecoveryRetainsSessionResults()
    {
        QTemporaryDir Directory; QVERIFY(QDir().mkdir(Directory.filePath("source"))); QVERIFY(QDir().mkdir(Directory.filePath("target")));
        QVERIFY(Put(Directory.filePath("source/file.txt"),"new data")); QVERIFY(Put(Directory.filePath("target/file.txt"),"old data"));
        auto Reader=MakeNativeReadProvider(LocatorKind::Local); auto Source=Reader->Resolve({LocatorKind::Local,Directory.filePath("source/file.txt").toStdString()},{}).get(); QVERIFY(Source.HasValue());
        FileOperationRequest Request; Request.sources_={{Source.Value(),Reader}}; Request.selectionRoot_={LocatorKind::Local,Directory.filePath("source").toStdString()};
        Request.destination_={LocatorKind::Local,Directory.filePath("target").toStdString()}; Request.conflict_=ConflictPolicy::Replace;
        Request.destinationWriter_=MakeNativeWritableStorage(LocatorKind::Local,Reader);
        auto Journal=MakeFileJobJournal(Directory.filePath("journal"),false,true,false);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(),1,16,Journal);
        auto Job=SubmitFileOperation(Scheduler,Request); QVERIFY(Job.HasValue()); auto Done=Job.Value().completion_.get();
        QCOMPARE(Done.snapshot_.state_,JobState::Completed); QCOMPARE(Done.snapshot_.completedItems_,1ull); QVERIFY(!Done.writes_);
        QVERIFY(Done.outcomes_); QCOMPARE(Done.outcomes_->Count(),1ull); auto Page=Done.outcomes_->Read(0,1); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().items_[0].destinationCommitted_);
        QCOMPARE(Read(Directory.filePath("target/file.txt")),QByteArray("new data")); QVERIFY(!QDir(Directory.filePath("journal")).exists());
        QCOMPARE(QDir(Directory.filePath("target")).entryList(QDir::Files|QDir::Hidden),QStringList{"file.txt"});
    }
    void optionalProgressPersistence()
    {
        QTemporaryDir Directory; auto Journal=MakeFileJobJournal(Directory.path());
        JobResult Record; Record.snapshot_.id_="hc-job-r20-off"; Record.snapshot_.state_=JobState::Running;
        auto Items=Journal->OpenItems(Record.snapshot_.id_); QVERIFY(Items.HasValue()); Record.outcomes_=Items.Value();
        auto Writes=Journal->OpenWrites(Record.snapshot_.id_); QVERIFY(Writes.HasValue()); Record.writes_=Writes.Value();
        QVERIFY(Journal->Checkpoint(Record).HasValue()); const auto Path=Directory.filePath("hc-job-r20-off.json"); const auto Before=Read(Path);
        auto Reader=MakeNativeReadProvider(LocatorKind::Local); const Locator At{LocatorKind::Local,Path.toStdString()}; auto BeforeEntry=Reader->Resolve(At,{}).get(); QVERIFY(BeforeEntry.HasValue());
        JobResult Other; Other.snapshot_.id_="hc-job-r20-other"; Other.snapshot_.state_=JobState::Running;
        QVERIFY(Journal->Checkpoint(Other).HasValue());
        Record.snapshot_.progress_.bytesDone_=123; Record.snapshot_.progress_.currentItem_="a.txt";
        Record.snapshot_.completedItems_=1; QVERIFY(Journal->Checkpoint(Record).HasValue()); QCOMPARE(Read(Path),Before);
        auto AfterEntry=Reader->Resolve(At,{}).get(); QVERIFY(AfterEntry.HasValue()); QCOMPARE(AfterEntry.Value().id_,BeforeEntry.Value().id_);
        ItemOutcome Item; Item.item_="a.txt"; Item.destinationCommitted_=true; Item.status_=ItemOutcome::Status::Completed;
        QVERIFY(Record.outcomes_->Append(Item).HasValue()); QVERIFY(Record.writes_->Append(Item).HasValue());
        QVERIFY(Journal->Forget(Other.snapshot_.id_).HasValue());
        auto Recovered=Journal->Recover(); QVERIFY2(Recovered.HasValue(),Recovered.HasValue()?"":Recovered.Failure().code_.c_str());
        QCOMPARE(Recovered.Value()[0].snapshot_.state_,JobState::Failed); QCOMPARE(Recovered.Value()[0].snapshot_.completedItems_,1ull);
        QCOMPARE(Recovered.Value()[0].writes_->Count(),1ull); QCOMPARE(Recovered.Value()[0].snapshot_.progress_.bytesDone_,0ull);
        SetFileJobProgressPersistence(Journal,true); Record.snapshot_.progress_.bytesDone_=456;
        QVERIFY(Journal->Checkpoint(Record).HasValue()); QCOMPARE(Read(Path),Before); // Existing job remains off.
        Record.snapshot_.state_=JobState::Committing; QVERIFY(Journal->Checkpoint(Record).HasValue()); QVERIFY(Read(Path)!=Before);
        Record.snapshot_.state_=JobState::Completed; QVERIFY(Journal->Checkpoint(Record).HasValue());
        Recovered=Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value()[0].snapshot_.progress_.bytesDone_,456ull);
        Record=JobResult{}; Record.snapshot_.id_="hc-job-r20-on"; Record.snapshot_.state_=JobState::Running; QVERIFY(Journal->Checkpoint(Record).HasValue());
        const auto OnPath=Directory.filePath("hc-job-r20-on.json"); const auto OnBefore=Read(OnPath);
        SetFileJobProgressPersistence(Journal,false); Record.snapshot_.progress_.bytesDone_=789;
        QVERIFY(Journal->Checkpoint(Record).HasValue()); QVERIFY(Read(OnPath)!=OnBefore); // Existing job remains on.
        const auto Object=QJsonDocument::fromJson(Read(OnPath)).object(); QCOMPARE(Object["bytes"].toString(),QString("789"));
        Record.snapshot_.id_="hc-job-r20-next"; QVERIFY(Journal->Checkpoint(Record).HasValue());
        QVERIFY(!QJsonDocument::fromJson(Read(Directory.filePath("hc-job-r20-next.json"))).object().contains("bytes"));
        Recovered.Value().clear(); Items.Value().reset(); Writes.Value().reset(); Journal.reset();
        auto Restart=MakeFileJobJournal(Directory.path())->Recover(); QVERIFY2(Restart.HasValue(),Restart.HasValue()?"":Restart.Failure().code_.c_str());
        const auto Off=std::ranges::find_if(Restart.Value(),[](const JobResult& Value){return Value.snapshot_.id_=="hc-job-r20-off";});
        QVERIFY(Off!=Restart.Value().end()); QCOMPARE(Off->snapshot_.completedItems_,1ull); QCOMPARE(Off->writes_->Count(),1ull);
        QCOMPARE(Off->snapshot_.progress_.bytesDone_,456ull);
    }
    void deleteProgressJournalRoundTrip()
    {
        QTemporaryDir Directory; auto Journal = MakeFileJobJournal(Directory.path(), true); JobResult Record;
        Record.snapshot_.id_ = "hc-job-delete-count"; Record.snapshot_.state_ = JobState::Preparing;
        Record.snapshot_.progress_.discoveredItems_ = 10000000000ull;
        Record.snapshot_.progress_.discoveredFiles_ = 9000000000ull; Record.snapshot_.progress_.deletedFiles_ = 0;
        QVERIFY(Journal->Checkpoint(Record).HasValue());
        auto Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value().size(), size_t(1));
        const auto& Progress = Recovered.Value()[0].snapshot_.progress_;
        QVERIFY(!Progress.plannedItems_); QCOMPARE(Progress.discoveredItems_.value_or(0), 10000000000ull);
        QCOMPARE(Progress.discoveredFiles_.value_or(0), 9000000000ull); QVERIFY(Progress.deletedFiles_.has_value()); QCOMPARE(*Progress.deletedFiles_, 0ull);
        Record.snapshot_.state_ = JobState::Completed; Record.snapshot_.progress_.deletedFiles_ = 9000000000ull;
        Record.snapshot_.progress_.plannedItems_ = 10000000000ull; QVERIFY(Journal->Checkpoint(Record).HasValue());
        Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(*Recovered.Value()[0].snapshot_.progress_.deletedFiles_, 9000000000ull);
        QCOMPARE(*Recovered.Value()[0].snapshot_.progress_.plannedItems_, 10000000000ull);
    }
    void manifestDeleteValidationAndCounts()
    {
        class Manifest final : public ISelectionManifest {
        public:
            std::shared_ptr<IStorageProvider> provider_; std::vector<EntrySnapshot> entries_;
            uint64_t Count() const override { return entries_.size(); }
            std::shared_ptr<IStorageProvider> Provider() const override { return provider_; }
            Result<EntryPage> Read(uint64_t Offset, size_t Count, const OperationContext&) const override {
                EntryPage Page; const auto End = std::min<uint64_t>(entries_.size(), Offset + Count);
                for (auto I = Offset; I < End; ++I) Page.entries_.push_back(entries_[I]);
                Page.end_ = End == entries_.size(); return Page;
            }
        };
        for (const bool Duplicate : {true, false}) for (const bool Streaming : {false, true}) {
            QTemporaryDir Directory; QVERIFY(Directory.isValid()); QVERIFY(Put(Directory.filePath("a"), "a")); QVERIFY(Put(Directory.filePath("b"), "b"));
            auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
            auto Selection = std::make_shared<Manifest>(); Selection->provider_ = Reader;
            for (const auto* Name : {"a", "b"}) {
                auto Item = Reader->Resolve({LocatorKind::Local, QDir::toNativeSeparators(Directory.filePath(Name)).toStdString()}, {}).get();
                QVERIFY(Item.HasValue()); Selection->entries_.push_back(Item.Value());
            }
            if (Duplicate) Selection->entries_.back() = Selection->entries_.front();
            FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete; Request.deleteWhileListing_ = Streaming;
            Request.selection_ = Selection; Request.selectionRoot_ = {LocatorKind::Local, QDir::toNativeSeparators(Directory.path()).toStdString()};
            Request.selectionWriter_ = Writer; Request.confirmDelete_ = false;
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Job = SubmitFileOperation(Scheduler, Request); QVERIFY(Job.HasValue());
            const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, Duplicate ? JobState::Failed : JobState::Completed);
            if (Duplicate) { QVERIFY(QFileInfo::exists(Directory.filePath("a"))); QVERIFY(QFileInfo::exists(Directory.filePath("b"))); }
            else { QCOMPARE(Done.snapshot_.completedItems_, 2ull); QCOMPARE(Done.snapshot_.progress_.deletedFiles_.value_or(99), 2ull); QCOMPARE(Done.snapshot_.progress_.plannedItems_.value_or(99), 2ull); }
        }
    }
    void identicalCheckpointsAndExternalChanges()
    {
        QTemporaryDir Directory;auto Journal=MakeFileJobJournal(Directory.path(),true);JobResult Record;Record.snapshot_.id_="hc-job-r18-1";Record.snapshot_.state_=JobState::Preparing;
        QVERIFY(Journal->Checkpoint(Record).HasValue());const auto Path=Directory.filePath("hc-job-r18-1.json");auto Reader=MakeNativeReadProvider(LocatorKind::Local);
        const Locator At{LocatorKind::Local,Path.toStdString()};auto Before=Reader->Resolve(At,{}).get();QVERIFY(Before.HasValue());
        for(int I=0;I<1000;++I)QVERIFY(Journal->Checkpoint(Record).HasValue());auto After=Reader->Resolve(At,{}).get();QVERIFY(After.HasValue());QCOMPARE(After.Value().id_,Before.Value().id_);
        QLockFile Lock(Directory.filePath("writer.lock"));QVERIFY(Lock.tryLock());QVERIFY(!Journal->Checkpoint(Record).HasValue());Lock.unlock();
        QFile Corrupt(Path);QVERIFY(Corrupt.open(QIODevice::WriteOnly|QIODevice::Truncate));Corrupt.write("bad");Corrupt.close();QVERIFY(Journal->Checkpoint(Record).HasValue());QVERIFY(Journal->Recover().HasValue());
        QVERIFY(QFile::remove(Path));QVERIFY(Journal->Checkpoint(Record).HasValue());QVERIFY(QFileInfo::exists(Path));
        Record.snapshot_.progress_.bytesDone_=123;QVERIFY(Journal->Checkpoint(Record).HasValue());auto Recovered=Journal->Recover();QVERIFY(Recovered.HasValue());QCOMPARE(Recovered.Value()[0].snapshot_.progress_.bytesDone_,123ull);
    }
    void transientCheckpointSharingViolation()
    {
        QTemporaryDir Directory;auto Journal=MakeFileJobJournal(Directory.path(),true);JobResult Record;Record.snapshot_.id_="hc-job-r18-sharing";QVERIFY(Journal->Checkpoint(Record).HasValue());
        const auto Path=QDir::toNativeSeparators(Directory.filePath("hc-job-r18-sharing.json"));
        const auto Handle=CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);QVERIFY(Handle!=INVALID_HANDLE_VALUE);
        std::thread Reader([Handle]{std::this_thread::sleep_for(std::chrono::milliseconds(70));CloseHandle(Handle);});
        Record.snapshot_.progress_.bytesDone_=123;auto Done=Journal->Checkpoint(Record);Reader.join();QVERIFY(Done.HasValue());
        auto Recovered=Journal->Recover();QVERIFY(Recovered.HasValue());QCOMPARE(Recovered.Value()[0].snapshot_.progress_.bytesDone_,123ull);
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
