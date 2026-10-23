#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/fileBatchOperations.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <QtTest>
#include "UniqueLibs/nativeFileElevation.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/fileSecurityRecovery.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <ShlObj.h>
#include <wrl/client.h>
#include <QScopeGuard>
#include <atomic>
#include <thread>

using namespace hc::storage;
using namespace hc::operations;

namespace
{
    enum class WriteFault { Remove, Append, Metadata, Commit, AppendAndCleanup, Directory, Corrupt };
    class FaultSession final : public IWriteSession
    {
    public:
        FaultSession(std::unique_ptr<IWriteSession> Session, WriteFault Fault) : session_(std::move(Session)), fault_(Fault) {}
        Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
        {
            if (fault_ == WriteFault::Append || fault_ == WriteFault::AppendAndCleanup)
            {
                const auto Partial = session_->Append(Bytes.first((std::max)(std::size_t{1}, Bytes.size() / 2)), Context);
                if (!Partial.HasValue()) return Partial.Failure();
                return Error{ErrorCategory::Unavailable, "injected-disk-full"};
            }
            if (fault_ == WriteFault::Corrupt && !Bytes.empty()) {
                std::vector<std::byte> Changed(Bytes.begin(), Bytes.end()); Changed.front() ^= std::byte{1};
                return session_->Append(Changed, Context);
            }
            return session_->Append(Bytes, Context);
        }
        Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) override
        {
            if (fault_ == WriteFault::Metadata) return Error{ErrorCategory::AccessDenied, "injected-metadata"};
            return session_->SetMetadata(std::move(Metadata), Context);
        }
        Result<Unit> ConfigureCopy(EntrySnapshot Source, CopyOptions Options, OperationContext Context) override { return session_->ConfigureCopy(std::move(Source), Options, Context); }
        Result<Unit> RecordSourceBytes(std::span<const std::byte> Bytes, OperationContext Context) override { return session_->RecordSourceBytes(Bytes, Context); }
        Result<CopyExtraProgress> AdvanceCopyExtras(std::uint64_t Remaining, OperationContext Context) override { return session_->AdvanceCopyExtras(Remaining, Context); }
        Result<bool> VerifyContent(OperationContext Context) override { return session_->VerifyContent(Context); }
        Result<WriteReceipt> Commit(OperationContext Context) override
        {
            if (fault_ == WriteFault::Commit) return Error{ErrorCategory::Unavailable, "injected-commit"};
            return session_->Commit(Context);
        }
        Result<Unit> Abort() override
        {
            const auto Cleaned = session_->Abort();
            if (!Cleaned.HasValue()) return Cleaned.Failure();
            if (fault_ == WriteFault::AppendAndCleanup) return Error{ErrorCategory::AccessDenied, "injected-cleanup"};
            return Unit{};
        }
    private:
        std::unique_ptr<IWriteSession> session_;
        WriteFault fault_;
    };
    struct ParallelProbe
    {
        std::atomic<int> active_{0}, peak_{0}, calls_{0}, commits_{0};
        std::atomic<bool> wrongCommitThread_{false}, hold_{false};
        std::thread::id owner_ = std::this_thread::get_id();
        bool fail_ = false;
        std::string failName_;
    };
    class ParallelSession final : public IWriteSession
    {
    public:
        ParallelSession(std::unique_ptr<IWriteSession> Inner, std::shared_ptr<ParallelProbe> Probe)
            : inner_(std::move(Inner)), probe_(std::move(Probe)) {}
        Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
        {
            ++probe_->calls_; const auto Active = ++probe_->active_; auto Peak = probe_->peak_.load();
            while (Active > Peak && !probe_->peak_.compare_exchange_weak(Peak, Active)) {}
            const auto Exit = qScopeGuard([&] { --probe_->active_; });
            do {
                if (const auto Error = Context.Check({})) return *Error;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } while (probe_->hold_);
            if (probe_->fail_ || (!probe_->failName_.empty() && probe_->failName_ == name_)) return Error{ErrorCategory::Unavailable, "parallel-disk-full"};
            return inner_->Append(Bytes, Context);
        }
        Result<Unit> SetMetadata(FileMetadata Value, OperationContext Context) override { return inner_->SetMetadata(Value, Context); }
        Result<Unit> ConfigureCopy(EntrySnapshot Source, CopyOptions Options, OperationContext Context) override { name_ = Source.name_; return inner_->ConfigureCopy(std::move(Source), Options, Context); }
        Result<Unit> RecordSourceBytes(std::span<const std::byte> Bytes, OperationContext Context) override { return inner_->RecordSourceBytes(Bytes, Context); }
        Result<CopyExtraProgress> AdvanceCopyExtras(std::uint64_t Left, OperationContext Context) override { return inner_->AdvanceCopyExtras(Left, Context); }
        Result<bool> VerifyContent(OperationContext Context) override { return inner_->VerifyContent(Context); }
        Result<WriteReceipt> Commit(OperationContext Context) override
        {
            ++probe_->commits_; if (std::this_thread::get_id() != probe_->owner_) probe_->wrongCommitThread_ = true;
            return inner_->Commit(Context);
        }
        Result<Unit> Abort() override { return inner_->Abort(); }
    private:
        std::unique_ptr<IWriteSession> inner_;
        std::shared_ptr<ParallelProbe> probe_;
        std::string name_;
    };
    class ParallelWriter final : public IWritableStorage
    {
    public:
        ParallelWriter(std::shared_ptr<IWritableStorage> Inner, std::shared_ptr<ParallelProbe> Probe)
            : inner_(std::move(Inner)), probe_(std::move(Probe)) {}
        ProviderId Id() const override { return inner_->Id(); }
        bool SupportsParallelCopies() const override { return true; }
        CopyCapabilities CopySupport() const override { return inner_->CopySupport(); }
        Result<SupportDecision> Supports(Mutation Kind, Locator At, OperationContext Context) override { return inner_->Supports(Kind, At, Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return inner_->Inspect(At, Context); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override { return BeginCopy(At, Expected, {}, Context); }
        Result<std::unique_ptr<IWriteSession>> BeginCopy(Locator At, DestinationExpectation Expected, CopyOptions Options, OperationContext Context) override
        {
            auto Made = inner_->BeginCopy(At, Expected, Options, Context); if (!Made.HasValue()) return Made.Failure();
            return std::unique_ptr<IWriteSession>(new ParallelSession(std::move(Made.Value()), probe_));
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { return inner_->MakeDirectory(At, Context); }
        Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override { return inner_->Remove(Entry, Context); }
        Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override { return inner_->Rename(Entry, To, Context); }
    private:
        std::shared_ptr<IWritableStorage> inner_;
        std::shared_ptr<ParallelProbe> probe_;
    };
    class CopySelection final : public ISelectionManifest
    {
    public:
        std::vector<EntrySnapshot> entries_;
        std::shared_ptr<IStorageProvider> reader_;
        mutable std::size_t largestPage_ = 0;
        std::uint64_t Count() const override { return entries_.size(); }
        std::shared_ptr<IStorageProvider> Provider() const override { return reader_; }
        Result<EntryPage> Read(std::uint64_t Offset, std::size_t Count, const OperationContext&) const override
        {
            largestPage_ = std::max(largestPage_, Count); EntryPage Page;
            const auto End = std::min<std::uint64_t>(entries_.size(), Offset + Count);
            Page.entries_.assign(entries_.begin() + Offset, entries_.begin() + End); Page.end_ = End == entries_.size(); return Page;
        }
    };
    class ImmediateExecutor final : public hc::commands::IExecutor
    {
    public:
        bool Post(std::function<void()> Work) override { Work(); return true; }
    };
    class FailedRemoveWriter final : public IWritableStorage
    {
    public:
        explicit FailedRemoveWriter(std::shared_ptr<IWritableStorage> Writer, WriteFault Fault = WriteFault::Remove) : writer_(std::move(Writer)), fault_(Fault) {}
        ProviderId Id() const override { return writer_->Id(); }
        Result<SupportDecision> Supports(Mutation Operation, Locator At, OperationContext Context) override { return writer_->Supports(Operation, std::move(At), Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return writer_->Inspect(std::move(At), Context); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override
        {
            auto Session = writer_->BeginWrite(std::move(At), std::move(Expected), Context);
            if (!Session.HasValue()) return Session.Failure();
            return std::unique_ptr<IWriteSession>(new FaultSession(std::move(Session.Value()), fault_));
        }
        CopyCapabilities CopySupport() const override { return writer_->CopySupport(); }
        Result<std::unique_ptr<IWriteSession>> BeginCopy(Locator At, DestinationExpectation Expected, CopyOptions Options, OperationContext Context) override
        {
            auto Session = writer_->BeginCopy(std::move(At), std::move(Expected), Options, Context);
            if (!Session.HasValue()) return Session.Failure();
            return std::unique_ptr<IWriteSession>(new FaultSession(std::move(Session.Value()), fault_));
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { if (fault_ == WriteFault::Directory && made_++ == 1) return Error{ErrorCategory::AccessDenied, "injected-parent-create"}; return writer_->MakeDirectory(std::move(At), Context); }
        Result<Unit> Remove(EntrySnapshot, OperationContext) override { return Error{ErrorCategory::AccessDenied, "injected-source-delete"}; }
        Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override { return writer_->Rename(std::move(Entry), std::move(To), Context); }
    private:
        std::shared_ptr<IWritableStorage> writer_;
        WriteFault fault_; int made_ = 0;
    };
    class DeniedContentWriter final : public IWritableStorage
    {
    public:
        DeniedContentWriter(std::shared_ptr<IWritableStorage> Writer, QString Name = {}) : writer_(std::move(Writer)), name_(std::move(Name)) {}
        ProviderId Id() const override { return writer_->Id(); }
        Result<SupportDecision> Supports(Mutation What, Locator At, OperationContext Context) override { return writer_->Supports(What, std::move(At), Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return writer_->Inspect(std::move(At), Context); }
        CopyCapabilities CopySupport() const override { return writer_->CopySupport(); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override
        { return writer_->BeginWrite(std::move(At), std::move(Expected), Context); }
        Result<std::unique_ptr<IWriteSession>> BeginCopy(Locator At, DestinationExpectation Expected, CopyOptions Options, OperationContext Context) override
        {
            if (name_.isEmpty() || QString::fromStdString(At.root_).endsWith(name_)) return Error{ErrorCategory::AccessDenied, "fixture-copy-denied"};
            return writer_->BeginCopy(std::move(At), std::move(Expected), Options, Context);
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { return writer_->MakeDirectory(std::move(At), Context); }
        Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override { return writer_->Remove(std::move(Entry), Context); }
        Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override { return writer_->Rename(std::move(Entry),std::move(To),Context); }
    private:
        std::shared_ptr<IWritableStorage> writer_;
        QString name_;
    };
    struct ElevationFixtureState
    {
        std::atomic_bool hold_{false}, cancelled_{false}, decline_{false};
        std::mutex mutex_;
        std::vector<FileElevationRequest> requests_;
        std::function<bool()> paused_;
    };
    class FixtureElevationSession final : public IFileElevationSession
    {
    public:
        FixtureElevationSession(FileElevationRequest Request, std::shared_ptr<IStorageProvider> Reader,
            std::shared_ptr<IWritableStorage> Writer, std::shared_ptr<ElevationFixtureState> State)
            : request_(std::move(Request)), reader_(std::move(Reader)), writer_(std::move(Writer)), state_(std::move(State))
        {
            if (request_.action_ == FileElevationAction::Copy) {
                FileOperationRequest Child; Child.sources_ = {{*request_.source_,reader_,{}}};
                Child.destination_ = request_.destination_;
                const auto Path = QString::fromStdString(Child.destination_.root_);
                Child.destination_.root_ = QDir::toNativeSeparators(QFileInfo(Path).absolutePath()).toStdString();
                Child.destinationName_ = QFileInfo(Path).fileName().toStdString(); Child.destinationWriter_ = writer_;
                Child.conflict_ = ConflictPolicy::Replace; Child.approvedDestination_ = request_.expected_;
                auto Prepared = PrepareFileOperation(std::move(Child));
                if (Prepared.HasValue()) child_ = std::move(Prepared.Value().task_); else status_.error_ = Prepared.Failure();
            }
        }
        Result<FileElevationStatus> Poll() override
        {
            if (state_->decline_) { status_.error_ = Error{ErrorCategory::Cancelled,"elevation-uac-declined"}; status_.finished_ = true; return status_; }
            if (status_.finished_ || state_->hold_) return status_;
            if (status_.error_) { status_.finished_=true; return status_; }
            if (child_) {
                if (status_.readyToCommit_ && !gate_) return status_;
                JobStepContext Context; Context.jobId_ = request_.jobId_; Context.committing_ = gate_;
                auto Step = child_->Advance(Context);
                if (Step.progress_) status_.bytesDone_ = Step.progress_->bytesDone_;
                if (Step.next_ == JobState::Committing) status_.readyToCommit_ = true;
                if (IsTerminal(Step.next_)) {
                    status_.finished_ = true; status_.error_ = Step.error_; status_.cleanupErrors_ = child_->Finalize(Step.next_); child_.reset();
                    if (Step.item_) { status_.destinationCommitted_ = Step.item_->destinationCommitted_; status_.sourceRemoved_ = Step.item_->sourceRemoved_; }
                    auto Output = writer_->Inspect(request_.destination_, {}); if (Output.HasValue() && Output.Value()) status_.destination_ = *Output.Value();
                }
            } else if (!gate_) status_.readyToCommit_ = true;
            else {
                auto Valid = reader_->Stat(request_.source_->id_, {}).get();
                if (!Valid.HasValue()) status_.error_ = Valid.Failure();
                else if (request_.requiresPublishedDestination_ && (!request_.publishedDestination_ || !reader_->Stat(request_.publishedDestination_->id_, {}).get().HasValue()))
                    status_.error_ = Error{ErrorCategory::StaleRevision,"elevation-published-output-changed"};
                else {
                    auto Removed = writer_->Remove(*request_.source_,{});
                    if (!Removed.HasValue()) status_.error_ = Removed.Failure(); else status_.sourceRemoved_ = true;
                }
                status_.finished_ = true;
            }
            return status_;
        }
        Result<Unit> Commit() override { if (!status_.readyToCommit_) return Error{ErrorCategory::InvalidRequest,"fixture-not-ready"}; gate_=true; return Unit{}; }
        void Cancel() override { state_->cancelled_ = true; }
        std::vector<Error> Close() override { if(child_) { auto Errors=child_->Finalize(JobState::Cancelled); child_.reset(); return Errors; } return {}; }
    private:
        FileElevationRequest request_;
        std::shared_ptr<IStorageProvider> reader_;
        std::shared_ptr<IWritableStorage> writer_;
        std::shared_ptr<ElevationFixtureState> state_;
        std::unique_ptr<IJobTask> child_;
        FileElevationStatus status_;
        bool gate_ = false;
    };
    class FixtureElevationService final : public IFileElevationService
    {
    public:
        FixtureElevationService(std::shared_ptr<IStorageProvider> Reader, std::shared_ptr<IWritableStorage> Writer)
            : reader_(std::move(Reader)), writer_(std::move(Writer)), state_(std::make_shared<ElevationFixtureState>()) {}
        bool CanExecute(const FileElevationRequest& Request) const override
        { return !Request.jobId_.empty() && Request.source_ && Request.source_->locator_.kind_ == LocatorKind::Local
            && (Request.action_ == FileElevationAction::Copy || Request.action_ == FileElevationAction::Remove); }
        Result<std::unique_ptr<IFileElevationSession>> Begin(FileElevationRequest Request, OperationContext Context) override
        {
            { std::lock_guard Lock(state_->mutex_); state_->requests_.push_back(Request); state_->paused_=Context.pauseRequested_; }
            return std::unique_ptr<IFileElevationSession>(new FixtureElevationSession(std::move(Request),reader_,writer_,state_));
        }
        std::vector<FileElevationRequest> Requests() const { std::lock_guard Lock(state_->mutex_); return state_->requests_; }
        bool Paused() const { std::function<bool()> Query; {std::lock_guard Lock(state_->mutex_); Query=state_->paused_;} return Query && Query(); }
        std::shared_ptr<ElevationFixtureState> state_;
    private:
        std::shared_ptr<IStorageProvider> reader_;
        std::shared_ptr<IWritableStorage> writer_;
    };

}

class RemoteFixtureReader final:public IStorageProvider {
public:
    RemoteFixtureReader(std::shared_ptr<IStorageProvider> Inner,LocatorKind Kind):inner_(std::move(Inner)),kind_(Kind){}
    ProviderId Id()const override{return inner_->Id();}
    Task<SupportDecision> Supports(Operation R,OperationContext C)override{return inner_->Supports(R,C);}
    Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator L,OperationContext C)override{return inner_->Enumerate(L,C);}
    Task<EntrySnapshot> Stat(ResourceId I,OperationContext C)override {
        auto R=inner_->Stat(I,C).get();if(R.HasValue())R.Value().locator_.kind_=kind_;return Ready<EntrySnapshot>(std::move(R));
    }
    Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId I,OperationContext C)override{return inner_->OpenRead(I,C);}
private:std::shared_ptr<IStorageProvider> inner_;LocatorKind kind_;
};
static OperationContext ElevationContext() {
    OperationContext C;const auto Journal=QDir(hc::settings::Paths().directory_).filePath("write-"+QUuid::createUuid().toString(QUuid::Id128)+".json");
    C.writeCheckpoint_=[Journal](const WriteRecoveryRecord& R,std::optional<std::uint64_t> Row)->Result<std::uint64_t>{
        QJsonObject Record{{"phase",int(R.phase_)},{"transaction",QString::fromStdString(R.transaction_)},{"stage",QString::fromStdString(R.stage_.root_)},{"destination",QString::fromStdString(R.destination_.root_)},{"backup",QString::fromStdString(R.backup_.root_)}};
        const auto Bytes=QJsonDocument(Record).toJson();QSaveFile File(Journal);if(!File.open(QIODevice::WriteOnly)||File.write(Bytes)!=Bytes.size()||!File.commit())return Error{ErrorCategory::Unavailable,"fixture-checkpoint-save"};return Row.value_or(0);
    };return C;
}
static hc::permissions::FileElevationLauncher FixtureLauncher(std::function<void()> Before={},bool ExitEarly=false) {
    return [Before=std::move(Before),ExitEarly](const QString&,const QString& Arguments)->Result<HANDLE>{
        if(Before)Before();const auto Exe=QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const auto Mode = Arguments.startsWith("--elevated-list ") ? "--directory-fixture " : "--elevation-fixture ";
        const auto Command='"'+Exe+"\" "+(ExitEarly?QString("--elevation-fixture-exit"):QString(Mode)+Arguments.section(' ',1));
        std::wstring Writable=Command.toStdWString();STARTUPINFOW Startup{};Startup.cb=sizeof(Startup);PROCESS_INFORMATION Process{};
        if(!CreateProcessW(reinterpret_cast<LPCWSTR>(Exe.utf16()),Writable.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process))return Error{ErrorCategory::Unavailable,"fixture-launch:"+std::to_string(GetLastError())};
        CloseHandle(Process.hThread);return Process.hProcess;
    };
}
class WriteTests : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> root_;
    std::shared_ptr<IStorageProvider> reader_;
    std::shared_ptr<IWritableStorage> writer_;
    LocatorKind kind_ = LocatorKind::Local;
    Locator at(const QString& Name) const { return {kind_, QDir::toNativeSeparators(root_->filePath(Name)).toStdString()}; }
    bool write(const QString& Name, const QByteArray& Bytes)
    {
        QFile File(root_->filePath(Name));
        return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
    }
    QByteArray read(const QString& Name)
    {
        QFile File(root_->filePath(Name));
        if (!File.open(QIODevice::ReadOnly)) return {};
        return File.readAll();
    }
    Result<Unit> append(IWriteSession& Session, const QByteArray& Bytes, OperationContext Context = {})
    { return Session.Append({reinterpret_cast<const std::byte*>(Bytes.constData()), static_cast<std::size_t>(Bytes.size())}, Context); }
    QStringList artifacts()
    { return QDir(root_->path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden | QDir::System); }
private slots:
    void parallelCopy_data()
    {
        QTest::addColumn<int>("Mode");
        QTest::newRow("serial-commit-parallel-transfer") << 0;
        QTest::newRow("cancel-joins-workers") << 1;
        QTest::newRow("late-destination-preserved") << 2;
        QTest::newRow("overwrite-answers") << 3;
        QTest::newRow("failed-transfer-preserves-destinations") << 4;
        QTest::newRow("verified-copy") << 5;
        QTest::newRow("byte-budget-before-writes") << 6;
        QTest::newRow("pause-before-publish") << 7;
        QTest::newRow("eight-worker-cap") << 8;
        QTest::newRow("same-name-selected-roots") << 9;
        QTest::newRow("journal-keeps-serial-path") << 10;
        QTest::newRow("throttle-keeps-serial-path") << 11;
        QTest::newRow("alternate-streams-keep-serial-path") << 12;
        QTest::newRow("deadline-joins-workers") << 13;
    }
    void parallelCopy()
    {
        QFETCH(int, Mode);
        if (kind_ != LocatorKind::Local) QSKIP("Local parallel-copy contract");
        const QString From = QString("parallel-from-%1").arg(Mode), To = QString("parallel-to-%1").arg(Mode);
        QVERIFY(QDir().mkpath(root_->filePath(From))); QVERIFY(QDir().mkpath(root_->filePath(To)));
        auto Probe = std::make_shared<ParallelProbe>(); Probe->fail_ = Mode == 4;
        Probe->hold_ = Mode == 1 || Mode == 7 || Mode == 13;
        FileOperationRequest Request; Request.destination_ = at(To);
        Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, Probe);
        Request.copyOptions_.verify_ = Mode == 5;
        if (Mode == 4) Request.conflict_ = ConflictPolicy::Replace;
        if (Mode == 11) Request.bytesPerSecondLimit_ = 64 * 1024 * 1024;
        Request.copyOptions_.streams_ = Mode == 12;
        for (int I = 0; I < 24; ++I) {
            const auto Name = Mode == 9 ? QString("same.bin") : QString::number(I).rightJustified(2, '0') + ".bin";
            const auto SourceFolder = Mode == 9 ? From + '/' + QString::number(I) : From;
            QVERIFY(QDir().mkpath(root_->filePath(SourceFolder)));
            QVERIFY(write(SourceFolder + '/' + Name, QByteArray(4096, char(I))));
            auto Entry = reader_->Resolve(at(SourceFolder + '/' + Name), {}).get(); QVERIFY(Entry.HasValue());
            Request.sources_.push_back({Entry.Value(), reader_, {}});
            if (Mode == 3 || Mode == 4) QVERIFY(write(To + '/' + Name, "original"));
        }
        if (Mode == 6) Request.maxBytes_ = 4096;
        OperationContext Context; Context.fileCopyWorkers_ = Mode == 8 ? 100 : 4;
        Context.flushFileContents_ = false;
        std::uint64_t Checkpoints = 0;
        if (Mode == 10) Context.writeCheckpoint_ = [&](const WriteRecoveryRecord&, std::optional<std::uint64_t>) -> Result<std::uint64_t> { return ++Checkpoints; };
        std::atomic<bool> Paused{false}; Context.pauseRequested_ = [&] { return Paused.load(); };
        std::stop_source Stop; Context.stopToken_ = Stop.get_token();
        if (Mode == 13) Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        auto Prepared = PrepareFileOperation(std::move(Request), Context); QVERIFY(Prepared.HasValue());
        auto Task = std::move(Prepared.Value().task_);
        JobStepContext StepContext; StepContext.operation_ = Context;
        JobState State = JobState::Preparing; int Outcomes = 0, Skipped = 0, Questions = 0;
        bool Injected = false; std::uint64_t Previous = 0; std::string FailureCode;
        const auto Start = std::chrono::steady_clock::now();
        while (!IsTerminal(State) && std::chrono::steady_clock::now() - Start < std::chrono::seconds(10)) {
            auto Step = Task->Advance(StepContext); StepContext.answer_.reset(); State = Step.next_;
            if (Step.error_) FailureCode = Step.error_->code_;
            if (Step.progress_) { QVERIFY(Step.progress_->bytesDone_ >= Previous); Previous = Step.progress_->bytesDone_; }
            if (Step.item_) { ++Outcomes; Skipped += Step.item_->status_ == ItemOutcome::Status::Skipped; }
            if (Mode == 1 && Probe->active_ > 0) { Stop.request_stop(); Injected = true; }
            if (Mode == 7 && Probe->active_ > 0 && !Injected) {
                Paused = true; Probe->hold_ = false; std::this_thread::sleep_for(std::chrono::milliseconds(15));
                QCOMPARE(Probe->commits_.load(), 0); Paused = false; Injected = true;
            }
            if (State == JobState::Committing && Mode == 2 && !Injected) { QVERIFY(write(To + "/00.bin", "late")); Injected = true; }
            if (State == JobState::WaitingForUser) {
                ++Questions; QVERIFY(Step.interaction_);
                StepContext.answer_ = InteractionAnswer{"", Questions == 1 ? InteractionChoice::Skip : InteractionChoice::Replace};
            }
            StepContext.committing_ = State == JobState::Committing;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        QVERIFY(IsTerminal(State)); const auto Errors = Task->Finalize(State); QVERIFY(Errors.empty()); Task.reset();
        QCOMPARE(Probe->active_.load(), 0); QVERIFY(!Probe->wrongCommitThread_);
        if (Mode == 0 || Mode == 3 || Mode == 5 || Mode == 7 || (Mode >= 8 && Mode <= 12)) {
            QCOMPARE(State, JobState::Completed); QCOMPARE(Outcomes, 24);
            if (Mode != 3 && Mode != 9 && Mode < 10) QVERIFY(Probe->peak_ > 1);
            if (Mode >= 10) QCOMPARE(Probe->peak_.load(), 1);
            QVERIFY(Probe->peak_ <= (Mode == 8 ? 8 : 4));
            for (int I = 0; I < 24 && Mode != 9; ++I) {
                const auto Name = QString::number(I).rightJustified(2, '0') + ".bin";
                QCOMPARE(read(To + '/' + Name), Mode == 3 && I == 0 ? QByteArray("original") : QByteArray(4096, char(I)));
            }
            if (Mode == 3) { QCOMPARE(Questions, 24); QCOMPARE(Skipped, 1); }
            if (Mode == 9) { QCOMPARE(Questions, 23); QCOMPARE(Skipped, 1); QCOMPARE(read(To + "/same.bin"), QByteArray(4096, char(23))); }
            if (Mode == 10) QVERIFY(Checkpoints > 0);
        } else {
            QCOMPARE(State, JobState::Failed); QCOMPARE(Probe->commits_.load(), Mode == 2 ? 1 : 0);
            if (Mode == 2) QCOMPARE(read(To + "/00.bin"), QByteArray("late"));
            if (Mode == 4) for (int I = 0; I < 24; ++I) QCOMPARE(read(To + '/' + QString::number(I).rightJustified(2, '0') + ".bin"), QByteArray("original"));
            if (Mode == 13) QCOMPARE(FailureCode, std::string("deadline-exceeded"));
        }
        QCOMPARE(QDir(root_->filePath(To)).entryList({".hc-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void parallelRenameAffectsPending()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local parallel overwrite decisions");
        QVERIFY(QDir().mkpath(root_->filePath("parallel-rename/from"))); QVERIFY(QDir().mkpath(root_->filePath("parallel-rename/to")));
        auto Probe = std::make_shared<ParallelProbe>();
        FileOperationRequest Request; Request.destination_ = at("parallel-rename/to"); Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, Probe);
        for (const auto Name : {"a", "b", "c"}) {
            const auto File = QString("parallel-rename/from/%1.bin").arg(Name); QVERIFY(write(File, QByteArray(4096, Name[0])));
            auto Entry = reader_->Resolve(at(File), {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back({Entry.Value(), reader_, {}});
        }
        QVERIFY(write("parallel-rename/to/a.bin", "original"));
        OperationContext Context; Context.fileCopyWorkers_ = 3; Context.flushFileContents_ = false;
        auto Prepared = PrepareFileOperation(std::move(Request), Context); QVERIFY(Prepared.HasValue());
        auto Task = std::move(Prepared.Value().task_); JobStepContext StepContext; StepContext.operation_ = Context;
        JobState State = JobState::Preparing; int Questions = 0, Completed = 0, Skipped = 0; std::uint64_t Previous = 0;
        const auto Start = std::chrono::steady_clock::now();
        while (!IsTerminal(State) && std::chrono::steady_clock::now() - Start < std::chrono::seconds(10)) {
            auto Step = Task->Advance(StepContext); State = Step.next_; StepContext.answer_.reset();
            if (Step.progress_) { QVERIFY(Step.progress_->bytesDone_ >= Previous); Previous = Step.progress_->bytesDone_; }
            if (Step.item_) { Completed += Step.item_->status_ == ItemOutcome::Status::Completed; Skipped += Step.item_->status_ == ItemOutcome::Status::Skipped; }
            if (State == JobState::WaitingForUser) {
                ++Questions;
                if (Questions == 1) {
                    QTRY_VERIFY_WITH_TIMEOUT(Probe->calls_ >= 2, 5000);
                    StepContext.answer_ = InteractionAnswer{"", InteractionChoice::Rename, "b.bin"};
                } else StepContext.answer_ = InteractionAnswer{"", InteractionChoice::Skip};
            }
            StepContext.committing_ = State == JobState::Committing; std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        const auto Errors = Task->Finalize(State); Task.reset();
        QCOMPARE(State, JobState::Completed); QCOMPARE(Questions, 2); QCOMPARE(Completed, 2); QCOMPARE(Skipped, 1); QVERIFY(Errors.empty());
        QCOMPARE(read("parallel-rename/to/a.bin"), QByteArray("original")); QCOMPARE(read("parallel-rename/to/b.bin"), QByteArray(4096, 'a'));
        QCOMPARE(read("parallel-rename/to/c.bin"), QByteArray(4096, 'c'));
        QCOMPARE(QDir(root_->filePath("parallel-rename/to")).entryList({".hc-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void parallelSelection_data()
    {
        QTest::addColumn<int>("Mode");
        QTest::newRow("manifest-pages") << 0;
        QTest::newRow("stale-selected-file") << 1;
        QTest::newRow("failed-file-keeps-next-selections") << 2;
    }
    void parallelSelection()
    {
        QFETCH(int, Mode);
        if (kind_ != LocatorKind::Local) QSKIP("Local selected files");
        const auto Prefix = QString("selection-copy-%1").arg(Mode);
        QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/from"))); QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/to")));
        auto Selection = std::make_shared<CopySelection>(); Selection->reader_ = reader_;
        for (int I = 0; I < 72; ++I) {
            const auto Name = Prefix + "/from/" + QString::number(I) + ".bin";
            QVERIFY(write(Name, QByteArray(4096, char(I)))); auto Entry = reader_->Resolve(at(Name), {}).get(); QVERIFY(Entry.HasValue());
            Selection->entries_.push_back(Entry.Value());
        }
        if (Mode == 1) QVERIFY(write(Prefix + "/from/7.bin", "changed"));
        if (Mode == 2) QVERIFY(write(Prefix + "/to/7.bin", "original"));
        auto Probe = std::make_shared<ParallelProbe>(); if (Mode == 2) Probe->failName_ = "7.bin";
        FileOperationRequest Request; Request.selection_ = Selection; Request.selectionRoot_ = at(Prefix + "/from");
        Request.destination_ = at(Prefix + "/to"); Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, Probe); Request.conflict_ = ConflictPolicy::Replace;
        OperationContext Context; Context.fileCopyWorkers_ = 4; Context.flushFileContents_ = false;
        auto Prepared = PrepareFileOperation(std::move(Request), Context); QVERIFY(Prepared.HasValue());
        auto Task = std::move(Prepared.Value().task_); JobStepContext StepContext; StepContext.operation_ = Context;
        JobState State = JobState::Preparing; int Completed = 0, Failed = 0; std::uint64_t Previous = 0;
        const auto Start = std::chrono::steady_clock::now();
        while (!IsTerminal(State) && std::chrono::steady_clock::now() - Start < std::chrono::seconds(10)) {
            auto Step = Task->Advance(StepContext); State = Step.next_;
            if (Step.item_) { Completed += Step.item_->status_ == ItemOutcome::Status::Completed; Failed += Step.item_->status_ == ItemOutcome::Status::Failed; }
            if (Step.progress_) { QVERIFY(Step.progress_->bytesDone_ >= Previous); Previous = Step.progress_->bytesDone_; }
            StepContext.committing_ = State == JobState::Committing; std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        QCOMPARE(State, JobState::Completed); QVERIFY(Task->Finalize(State).empty()); Task.reset();
        QCOMPARE(Completed, Mode == 0 ? 72 : 71); QCOMPARE(Failed, Mode == 0 ? 0 : 1);
        QCOMPARE(Selection->largestPage_, std::size_t(64)); QVERIFY(Probe->peak_ > 1); QVERIFY(!Probe->wrongCommitThread_);
        for (int I = 0; I < 72; ++I) {
            const auto Name = Prefix + "/to/" + QString::number(I) + ".bin";
            if (I == 7 && Mode == 1) QVERIFY(!QFileInfo::exists(root_->filePath(Name)));
            else QCOMPARE(read(Name), I == 7 && Mode == 2 ? QByteArray("original") : QByteArray(4096, char(I)));
        }
        QCOMPARE(QDir(root_->filePath(Prefix + "/to")).entryList({".hc-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void parallelGlobalBudget()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local worker budget");
        QVERIFY(QDir().mkpath(root_->filePath("global-copy-from")));
        std::vector<OperationSource> Sources;
        for (int I = 0; I < 8; ++I) {
            const auto Name = "global-copy-from/" + QString::number(I) + ".bin";
            QVERIFY(write(Name, QByteArray(4096, char(I))));
            auto Entry = reader_->Resolve(at(Name), {}).get(); QVERIFY(Entry.HasValue()); Sources.push_back({Entry.Value(), reader_, {}});
        }
        auto Probe = std::make_shared<ParallelProbe>(); Probe->hold_ = true;
        OperationContext Context; Context.fileCopyWorkers_ = 8; Context.flushFileContents_ = false;
        std::stop_source Stop; Context.stopToken_ = Stop.get_token();
        std::vector<std::unique_ptr<IJobTask>> Tasks;
        for (int I = 0; I < 5; ++I) {
            const auto Folder = "global-copy-to/" + QString::number(I); QVERIFY(QDir().mkpath(root_->filePath(Folder)));
            FileOperationRequest Request; Request.sources_ = Sources; Request.destination_ = at(Folder);
            Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, Probe);
            auto Prepared = PrepareFileOperation(std::move(Request), Context); QVERIFY(Prepared.HasValue()); Tasks.push_back(std::move(Prepared.Value().task_));
        }
        JobStepContext StepContext; StepContext.operation_ = Context;
        const auto Start = std::chrono::steady_clock::now();
        while (Probe->active_ < 32 && std::chrono::steady_clock::now() - Start < std::chrono::seconds(5)) {
            for (int I = 0; I < 4; ++I) {
                const auto Step = Tasks[I]->Advance(StepContext); QVERIFY(!IsTerminal(Step.next_));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto Active = Probe->active_.load();
        // Eight planning steps, then planning completion and a denied slot.
        for (int I = 0; I < 10; ++I) Tasks[4]->Advance(StepContext);
        const auto Peak = Probe->peak_.load(); const auto Committed = Probe->commits_.load();
        Stop.request_stop();
        std::vector<Error> Errors;
        for (auto& Task : Tasks) { auto Found = Task->Finalize(JobState::Cancelled); Errors.insert(Errors.end(), Found.begin(), Found.end()); }
        Tasks.clear();
        QCOMPARE(Active, 32); QCOMPARE(Peak, 32); QCOMPARE(Committed, 0); QCOMPARE(Probe->active_.load(), 0); QVERIFY(Errors.empty());
        for (int I = 0; I < 5; ++I) QCOMPARE(QDir(root_->filePath("global-copy-to/" + QString::number(I))).entryList(QDir::Files | QDir::Hidden).size(), 0);
    }
    void parallelSchedulerControls_data()
    {
        QTest::addColumn<bool>("SharedLane");
        QTest::addColumn<bool>("Ui");
        QTest::newRow("retain-volume-lane") << false << false;
        QTest::newRow("two-admitted-jobs") << true << false;
        QTest::newRow("ui-retain-volume-lane") << false << true;
        QTest::newRow("ui-two-admitted-jobs") << true << true;
    }
    void parallelSchedulerControls()
    {
        QFETCH(bool, SharedLane); QFETCH(bool, Ui);
        if (kind_ != LocatorKind::Local) QSKIP("Local scheduler controls");
        const auto Prefix = QString(SharedLane ? "parallel-shared-%1" : "parallel-lane-%1").arg(Ui);
        QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/from")));
        QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/first"))); QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/second")));
        FileOperationRequest Request;
        for (int I = 0; I < 24; ++I) {
            const auto Name = Prefix + "/from/" + QString::number(I) + ".bin"; QVERIFY(write(Name, QByteArray(4096, char(I))));
            auto Entry = reader_->Resolve(at(Name), {}).get(); QVERIFY(Entry.HasValue()); Request.sources_.push_back({Entry.Value(), reader_, {}});
        }
        auto FirstProbe = std::make_shared<ParallelProbe>(); FirstProbe->hold_ = true;
        auto SecondProbe = std::make_shared<ParallelProbe>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2);
        if (SharedLane) QVERIFY(Scheduler.SetLaneLimit(FileOperationLane(at(Prefix)), 2).HasValue());
        OperationContext Context; Context.fileCopyWorkers_ = 4; Context.flushFileContents_ = false;
        Context.providerInteractions_ = Ui;
        Request.destination_ = at(Prefix + "/first"); Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, FirstProbe);
        auto First = SubmitFileOperation(Scheduler, Request, Context); QVERIFY(First.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(FirstProbe->active_ == 4, 5000);
        Request.destination_ = at(Prefix + "/second"); Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, SecondProbe);
        auto Second = SubmitFileOperation(Scheduler, Request, Context); QVERIFY(Second.HasValue());
        QVERIFY(Scheduler.Pause(First.Value().id_, true).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(First.Value().id_).Value().state_ == JobState::Paused, 5000);
        FirstProbe->hold_ = false;
        QTest::qWait(20); QCOMPARE(FirstProbe->commits_.load(), 0);
        if (SharedLane) QCOMPARE(Second.Value().completion_.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        else { QCOMPARE(Second.Value().completion_.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout); QCOMPARE(SecondProbe->calls_.load(), 0); }
        QVERIFY(Scheduler.Cancel(First.Value().id_).HasValue());
        QCOMPARE(First.Value().completion_.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        const auto Cancelled = First.Value().completion_.get(); QCOMPARE(Cancelled.snapshot_.state_, JobState::Cancelled); QVERIFY(Cancelled.cleanupErrors_.empty());
        QCOMPARE(FirstProbe->active_.load(), 0); QCOMPARE(FirstProbe->commits_.load(), 0);
        QCOMPARE(Second.Value().completion_.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        const auto Done = Second.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Completed); QCOMPARE(Done.snapshot_.completedItems_, std::uint64_t(24));
        QCOMPARE(Done.snapshot_.progress_.bytesDone_, std::uint64_t(24 * 4096)); QVERIFY(Done.cleanupErrors_.empty());
        for (int I = 0; I < 24; ++I) QCOMPARE(read(Prefix + "/second/" + QString::number(I) + ".bin"), QByteArray(4096, char(I)));
        QCOMPARE(QDir(root_->filePath(Prefix + "/first")).entryList(QDir::Files | QDir::Hidden).size(), 0);
    }
    void nativeReadAhead_data()
    {
        QTest::addColumn<int>("MiB"); QTest::addColumn<int>("Mode");
        for (int MiB : {1, 4, 8}) {
            QTest::newRow(qPrintable(QString("tail-%1").arg(MiB))) << MiB << 0;
            QTest::newRow(qPrintable(QString("cancel-%1").arg(MiB))) << MiB << 1;
            QTest::newRow(qPrintable(QString("disk-full-%1").arg(MiB))) << MiB << 2;
            QTest::newRow(qPrintable(QString("verify-%1").arg(MiB))) << MiB << 3;
        }
    }
    void nativeReadAhead()
    {
        QFETCH(int, MiB); QFETCH(int, Mode);
        if (kind_ != LocatorKind::Local) QSKIP("Native pipeline contract");
        const QByteArray Content(9 * 1024 * 1024 + 37, 'p');
        QVERIFY(write("pipeline.bin", Content)); QVERIFY(QDir().mkpath(root_->filePath("pipeline-target")));
        QVERIFY(write("pipeline-target/pipeline.bin", "original"));
        auto Source = reader_->Resolve(at("pipeline.bin"), {}).get(); QVERIFY(Source.HasValue());
        auto Probe = std::make_shared<ParallelProbe>(); Probe->fail_ = Mode == 2;
        FileOperationRequest Request; Request.sources_ = {{Source.Value(), reader_, {}}};
        Request.destination_ = at("pipeline-target"); Request.conflict_ = ConflictPolicy::Replace;
        Request.destinationWriter_ = std::make_shared<ParallelWriter>(writer_, Probe); Request.copyOptions_.verify_ = Mode == 3;
        OperationContext Context; Context.directContent_ = false; Context.nativeReadAhead_ = true;
        Context.maxReadBytes_ = MiB * 1024 * 1024; Context.flushFileContents_ = false;
        Context.nativeCopyBufferBytes_ = Context.maxReadBytes_;
        std::stop_source Stop; Context.stopToken_ = Stop.get_token();
        auto Prepared = PrepareFileOperation(std::move(Request), Context); QVERIFY(Prepared.HasValue());
        auto Task = std::move(Prepared.Value().task_); JobStepContext StepContext; StepContext.operation_ = Context;
        JobState State = JobState::Preparing;
        for (int StepNo = 0; StepNo < 1000 && !IsTerminal(State); ++StepNo) {
            auto Step = Task->Advance(StepContext); State = Step.next_;
            if (Mode == 1 && Probe->calls_ > 0) Stop.request_stop();
            StepContext.committing_ = State == JobState::Committing;
        }
        QVERIFY(IsTerminal(State)); QVERIFY(Task->Finalize(State).empty()); Task.reset();
        QCOMPARE(Probe->active_.load(), 0);
        if (Mode == 0 || Mode == 3) { QCOMPARE(State, JobState::Completed); QCOMPARE(read("pipeline-target/pipeline.bin"), Content); }
        else { QCOMPARE(State, JobState::Failed); QCOMPARE(read("pipeline-target/pipeline.bin"), QByteArray("original")); }
        QVERIFY(!Probe->wrongCommitThread_);
        QCOMPARE(QDir(root_->filePath("pipeline-target")).entryList({".hc-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void producedContent_data()
    {
        QTest::addColumn<int>("Mode");
        QTest::newRow("success") << 0; QTest::newRow("short-output") << 1;
        QTest::newRow("late-error") << 2; QTest::newRow("cancelled-output") << 3;
        QTest::newRow("hardlink-rejected") << 4; QTest::newRow("readonly-output") << 5;
        QTest::newRow("take-success") << 6; QTest::newRow("take-short") << 7;
        QTest::newRow("take-late-error") << 8; QTest::newRow("take-cancelled") << 9;
        QTest::newRow("take-hardlink-rejected") << 10; QTest::newRow("take-readonly") << 11;
    }
    void producedContent()
    {
        QFETCH(int, Mode); const auto Scenario = Mode % 6; QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        const Locator Target{LocatorKind::Local, Directory.filePath("target.bin").toStdString()};
        QFile Existing(QString::fromStdString(Target.root_)); QVERIFY(Existing.open(QIODevice::WriteOnly));
        QCOMPARE(Existing.write("prior!!"), qint64(7)); Existing.close();
        auto Before = Writer->Inspect(Target, {}); QVERIFY(Before.HasValue() && Before.Value());
        OperationContext Context; Context.expectedWriteBytes_ = 7;
        std::stop_source Stop; Context.stopToken_ = Stop.get_token();
        auto Session = Writer->BeginCopy(Target, {true, Before.Value()->id_.revision_}, {}, Context); QVERIFY(Session.HasValue());
        QVERIFY(Session.Value()->ConfigureCopy({}, {}, Context).HasValue()); QVERIFY(Session.Value()->CanProduceContent(Context));
        ContentProducer Producer;
        Producer.produce_ = [&](const std::string& Output, OperationContext) -> Result<Unit> {
            const auto Path = QString::fromStdString(Output);
            if (Scenario == 4) {
                if (!CreateHardLinkW(Path.toStdWString().c_str(), QString::fromStdString(Target.root_).toStdWString().c_str(), nullptr))
                    return Error{ErrorCategory::Unavailable, "fixture-hardlink"};
            } else {
                QFile File(Path); if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return Error{ErrorCategory::Unavailable, "fixture-open"};
                if (File.write(Scenario == 1 ? QByteArray("short") : QByteArray("payload")) < 0) return Error{ErrorCategory::Unavailable, "fixture-write"};
                File.close();
                if (Scenario == 5 && !SetFileAttributesW(Path.toStdWString().c_str(), FILE_ATTRIBUTE_READONLY)) return Error{ErrorCategory::Unavailable, "fixture-readonly"};
            }
            if (Scenario == 2) return Error{ErrorCategory::Unavailable, "fixture-crc-after-write"};
            if (Scenario == 3) Stop.request_stop();
            return Unit{};
        };
        if (Mode >= 6) {
            Producer.take_ = [Generate = std::move(Producer.produce_)](const std::string& Parent, OperationContext Context) -> Result<ProducedContent> {
                auto Lease = std::make_shared<QTemporaryDir>(QDir(QString::fromStdString(Parent)).filePath("take-XXXXXX"));
                if (!Lease->isValid()) return Error{ErrorCategory::Unavailable, "fixture-lease"};
                const auto Output = Lease->filePath("payload").toStdString(); auto Generated = Generate(Output, Context);
                if (!Generated.HasValue()) return Generated.Failure();
                return ProducedContent{Output, std::move(Lease)};
            };
        }
        auto Produced = Session.Value()->ProduceContent(Producer, Context);
        const bool Success = Scenario == 0 || Scenario == 5;
        QCOMPARE(Produced.HasValue(), Success);
        if (Success) { QVERIFY(Session.Value()->AdvanceCopyExtras(UINT64_MAX, Context).HasValue()); QVERIFY(Session.Value()->Commit(Context).HasValue()); }
        QVERIFY(Session.Value()->Abort().HasValue()); Session.Value().reset();
        QVERIFY(Existing.open(QIODevice::ReadOnly)); QCOMPARE(Existing.readAll(), Success ? QByteArray("payload") : QByteArray("prior!!")); Existing.close();
        QCOMPARE(QDir(Directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System), QStringList{"target.bin"});
    }
    void producedContentVerificationFallback()
    {
        QTemporaryDir Directory; auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        OperationContext Context; Context.expectedWriteBytes_ = 7; CopyOptions Options; Options.verify_ = true;
        auto Session = Writer->BeginCopy({LocatorKind::Local, Directory.filePath("verified").toStdString()}, {}, Options, Context);
        QVERIFY(Session.HasValue()); QVERIFY(Session.Value()->ConfigureCopy({}, Options, Context).HasValue());
        QVERIFY(!Session.Value()->CanProduceContent(Context)); QVERIFY(Session.Value()->Abort().HasValue());
    }
    void producedContentDestinationInheritance()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); const auto Parent = Directory.filePath("output"); QVERIFY(QDir().mkpath(Parent));
        const auto Path = QDir::toNativeSeparators(Parent).toStdWString();
        PSID Owner = nullptr; PSECURITY_DESCRIPTOR Original = nullptr; LPWSTR OwnerText = nullptr;
        QCOMPARE(GetNamedSecurityInfoW(Path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &Owner, nullptr, nullptr, nullptr, &Original), DWORD(ERROR_SUCCESS));
        const auto FreeOriginal = qScopeGuard([&] { LocalFree(Original); });
        QVERIFY(ConvertSidToStringSidW(Owner, &OwnerText)); const auto Sid = QString::fromWCharArray(OwnerText); LocalFree(OwnerText);
        const auto Sddl = ("D:P(A;OICI;FA;;;" + Sid + ")(A;OINP;FR;;;BU)").toStdWString();
        PSECURITY_DESCRIPTOR Policy = nullptr; QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Policy, nullptr));
        const auto FreePolicy = qScopeGuard([&] { LocalFree(Policy); });
        PACL Acl = nullptr; BOOL Present = FALSE, Defaulted = FALSE; QVERIFY(GetSecurityDescriptorDacl(Policy, &Present, &Acl, &Defaulted));
        QCOMPARE(SetNamedSecurityInfoW(const_cast<wchar_t*>(Path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, Acl, nullptr), DWORD(ERROR_SUCCESS));
        const auto DaclText = [](const QString& At) {
            PSECURITY_DESCRIPTOR Sd = nullptr; LPWSTR Text = nullptr; QString Value;
            if (GetNamedSecurityInfoW(At.toStdWString().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &Sd) == ERROR_SUCCESS) {
                if (ConvertSecurityDescriptorToStringSecurityDescriptorW(Sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &Text, nullptr)) { Value = QString::fromWCharArray(Text); LocalFree(Text); } LocalFree(Sd);
            } return Value;
        };
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        OperationContext Context; Context.expectedWriteBytes_ = 7; Context.producingContent_ = true;
        auto Session = Writer->BeginCopy({LocatorKind::Local, QDir(Parent).filePath("target").toStdString()}, {}, {}, Context); QVERIFY(Session.HasValue());
        QVERIFY(Session.Value()->ConfigureCopy({}, {}, Context).HasValue());
        const auto Stages = QDir(Parent).entryList(QDir::Files | QDir::Hidden | QDir::System); QCOMPARE(Stages.size(), 1);
        const auto Expected = DaclText(QDir(Parent).filePath(Stages.front())); QVERIFY(!Expected.isEmpty());
        ContentProducer Producer; Producer.produce_ = [](const std::string& Output, OperationContext) -> Result<Unit> {
            QFile File(QString::fromStdString(Output)); if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly) || File.write("payload") != 7)
                return Error{ErrorCategory::Unavailable, "fixture-producer-write"}; return Unit{};
        };
        QVERIFY(Session.Value()->ProduceContent(Producer, Context).HasValue());
        QVERIFY(Session.Value()->AdvanceCopyExtras(UINT64_MAX, Context).HasValue()); QVERIFY(Session.Value()->Commit(Context).HasValue());
        QCOMPARE(DaclText(QDir(Parent).filePath("target")), Expected);
    }
    void producedContentCooperativeCancellation_data()
    {
        QTest::addColumn<bool>("Ui"); QTest::addColumn<bool>("Paused"); QTest::addColumn<bool>("DirectFinalize");
        QTest::newRow("scheduler") << false << false << false;
        QTest::newRow("paused-scheduler") << false << true << false;
        QTest::newRow("ui-wrapper") << true << false << false;
        QTest::newRow("paused-ui-wrapper") << true << true << false;
        QTest::newRow("direct-finalization") << false << false << true;
    }
    void producedContentCooperativeCancellation()
    {
        QFETCH(bool, Ui); QFETCH(bool, Paused); QFETCH(bool, DirectFinalize);
        struct SlowProvider final : IStorageProvider {
            std::shared_ptr<IStorageProvider> source_ = MakeNativeReadProvider(LocatorKind::Local);
            std::atomic<bool> entered_ = false, finished_ = false;
            std::atomic<bool> emergency_ = false;
            ProviderId Id() const override { return source_->Id(); }
            Task<SupportDecision> Supports(Operation Request, OperationContext Context) override { return source_->Supports(Request, Context); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override { return source_->Enumerate(std::move(At), Context); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Context) override { return source_->Stat(std::move(Id), Context); }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override { return source_->Resolve(std::move(At), Context); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Context) override { return source_->OpenRead(std::move(Id), Context); }
            Task<std::optional<ContentProducer>> PrepareContent(ResourceId, OperationContext) override {
                ContentProducer Producer;
                Producer.produce_ = [this](const std::string& Output, OperationContext Context) -> Result<Unit> {
                    QFile File(QString::fromStdString(Output));
                    if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly) || File.write(QByteArray(512, 'p')) != 512)
                        return Error{ErrorCategory::Unavailable, "fixture-producer-open"};
                    entered_ = true; if (Context.progress_) Context.progress_(512);
                    while (!Context.Check(Id()) && !emergency_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    File.close(); finished_ = true; return Context.Check(Id()).value_or(Error{ErrorCategory::Cancelled, "fixture-emergency"});
                };
                return Ready<std::optional<ContentProducer>>(std::optional<ContentProducer>{std::move(Producer)});
            }
        };
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Source = Directory.filePath("source.bin"); QFile File(Source);
        QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(QByteArray(2 * 1024 * 1024, 's')), qint64(2 * 1024 * 1024)); File.close();
        QVERIFY(QDir().mkpath(Directory.filePath("output")));
        QFile Old(Directory.filePath("output/source.bin")); QVERIFY(Old.open(QIODevice::WriteOnly)); QCOMPARE(Old.write("existing"), qint64(8)); Old.close();
        auto Reader = std::make_shared<SlowProvider>(); auto Entry = Reader->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Request; Request.sources_ = {{Entry.Value(), Reader, {}}};
        Request.destination_ = {LocatorKind::Local, Directory.filePath("output").toStdString()};
        Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Reader->source_); Request.conflict_ = ConflictPolicy::Replace;
        if (DirectFinalize) {
            auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue()); auto Task = std::move(Prepared.Value().task_);
            for (int I = 0; I < 100 && !Reader->entered_; ++I) { Task->Advance({}); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            QVERIFY(Reader->entered_);
            auto Cleanup = std::async(std::launch::async, [&] { return Task->Finalize(JobState::Cancelled); });
            const bool Stopped = Cleanup.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            if (!Stopped) Reader->emergency_ = true;
            QVERIFY(Cleanup.wait_for(std::chrono::seconds(5)) == std::future_status::ready); QVERIFY(Cleanup.get().empty()); Task.reset();
            QVERIFY2(Stopped, "Finalize did not cancel its owned background producer");
            QVERIFY(Old.open(QIODevice::ReadOnly)); QCOMPARE(Old.readAll(), QByteArray("existing")); return;
        }
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        OperationContext Context; Context.providerInteractions_ = Ui;
        auto Job = SubmitFileOperation(Scheduler, Request, Context); QVERIFY(Job.HasValue());
        QTRY_VERIFY(Reader->entered_.load());
        QTRY_VERIFY(Scheduler.Snapshot(Job.Value().id_).Value().progress_.bytesDone_ >= 512);
        if (Paused) { QVERIFY(Scheduler.Pause(Job.Value().id_, true).HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Job.Value().id_).Value().state_, JobState::Paused); }
        QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue());
        const bool Stopped = Job.Value().completion_.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!Stopped) Reader->emergency_ = true;
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Cancelled);
        QVERIFY2(Stopped, "Cancellation was disconnected after an Advance returned");
        QVERIFY(Reader->finished_.load()); QVERIFY(Done.cleanupErrors_.empty());
        QVERIFY(Old.open(QIODevice::ReadOnly)); QCOMPARE(Old.readAll(), QByteArray("existing")); Old.close();
        QCOMPARE(QDir(Directory.filePath("output")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System), QStringList{"source.bin"});
    }
    void elevatedDirectoryListing()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        for (int Index = 0; Index < 301; ++Index) {
            QFile File(Directory.filePath(QString::number(Index) + QStringLiteral("-한글.txt")));
            QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("abc"), 3);
        }
        QVERIFY(QDir(Directory.path()).mkdir("nested"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        int Launches = 0; HANDLE Observed = nullptr;
        const auto CloseObserved = qScopeGuard([&] { if (Observed) CloseHandle(Observed); });
        auto Session = hc::permissions::OpenElevatedDirectorySession(Directory.path(), {},
            [&, Launch = FixtureLauncher()](const QString& Exe, const QString& Args) -> Result<HANDLE> {
                ++Launches; auto Started = Launch(Exe, Args);
                if (Started.HasValue()) DuplicateHandle(GetCurrentProcess(), Started.Value(), GetCurrentProcess(), &Observed, SYNCHRONIZE, FALSE, 0);
                return Started;
            });
        QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
        auto Open = Session.Value()->Enumerate(Directory.path(), {}, Reader);
        QVERIFY2(Open.HasValue(), Open.HasValue() ? "" : Open.Failure().code_.c_str());
        int Count = 0; bool End = false;
        while (!End) {
            auto Page = Open.Value()->Next(17, {}).get();
            QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            QVERIFY(Page.Value().entries_.size() <= 17);
            for (const auto& Entry : Page.Value().entries_) {
                auto Stat = Reader->Stat(Entry.id_, {}).get();
                QVERIFY2(Stat.HasValue(), Stat.HasValue() ? "" : Stat.Failure().code_.c_str());
                QCOMPARE(Stat.Value().locator_, Entry.locator_);
                if (!Entry.directory_) QCOMPARE(Entry.size_, 3u);
                ++Count;
            }
            End = Page.Value().end_;
        }
        QCOMPARE(Count, 302);
        QVERIFY(Observed); QCOMPARE(WaitForSingleObject(Observed, 0), DWORD(WAIT_TIMEOUT));
        auto Refreshed = Session.Value()->Enumerate(Directory.path(), {}, Reader); QVERIFY(Refreshed.HasValue());
        auto Child = Session.Value()->Enumerate(Directory.filePath("nested"), {}, Reader); QVERIFY(Child.HasValue());
        auto Empty = Child.Value()->Next(17, {}).get(); QVERIFY(Empty.HasValue()); QVERIFY(Empty.Value().end_); QVERIFY(Empty.Value().entries_.empty());
        QVERIFY(!Session.Value()->Enumerate(QFileInfo(Directory.path()).absolutePath(), {}, Reader).HasValue());
        QCOMPARE(Launches, 1);
        Session.Value().reset(); QCOMPARE(WaitForSingleObject(Observed, 2000), DWORD(WAIT_OBJECT_0));
        Open = hc::permissions::EnumerateElevatedDirectory(Directory.filePath("missing"), {}, FixtureLauncher());
        QVERIFY(!Open.HasValue()); QCOMPARE(Open.Failure().category_, ErrorCategory::NotFound);
        bool Launched = false; std::stop_source Stop; Stop.request_stop(); OperationContext Cancelled; Cancelled.stopToken_ = Stop.get_token();
        QVERIFY(!hc::permissions::EnumerateElevatedDirectory(Directory.path(), Cancelled, FixtureLauncher([&] { Launched = true; })).HasValue());
        QVERIFY(!Launched);
        QVERIFY(!hc::permissions::EnumerateElevatedDirectory("//server/share", {}, FixtureLauncher([&] { Launched = true; })).HasValue());
        QVERIFY(!Launched);
        QVERIFY(!hc::permissions::EnumerateElevatedDirectory(Directory.path(), {}, FixtureLauncher({}, true)).HasValue());
    }
    void elevatedDirectoryRealUac()
    {
        if (!qEnvironmentVariableIsSet("HC_DIRECTORY_REAL_UAC")) QSKIP("Opt-in Windows UAC listing test");
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("read-only.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("listing"); File.close();
        const auto Native = QDir::toNativeSeparators(Directory.path()).toStdWString();
        HANDLE Handle = CreateFileW(Native.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
        PACL Original = nullptr; PSECURITY_DESCRIPTOR OriginalDescriptor = nullptr;
        QCOMPARE(GetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Original, nullptr, &OriginalDescriptor), DWORD(ERROR_SUCCESS));
        const auto Restore = qScopeGuard([&] {
            SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, Original, nullptr);
            LocalFree(OriginalDescriptor);
        });
        PSECURITY_DESCRIPTOR Descriptor = nullptr;
        QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;BA)(A;;FA;;;SY)", SDDL_REVISION_1, &Descriptor, nullptr));
        const auto Free = qScopeGuard([&] { LocalFree(Descriptor); });
        BOOL Present = FALSE, Defaulted = FALSE; PACL Acl = nullptr; QVERIFY(GetSecurityDescriptorDacl(Descriptor, &Present, &Acl, &Defaulted));
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, Acl, nullptr), DWORD(ERROR_SUCCESS));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        if (!hc::permissions::Elevated()) QVERIFY(!Reader->Enumerate({LocatorKind::Local, Directory.path().toStdString()}, {}).get().HasValue());
        auto Session = hc::permissions::OpenElevatedDirectorySession(Directory.path(), {});
        QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
        auto Open = Session.Value()->Enumerate(Directory.path(), {}, Reader);
        QVERIFY2(Open.HasValue(), Open.HasValue() ? "" : Open.Failure().code_.c_str());
        auto Page = Open.Value()->Next(16, {}).get();
        QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
        QCOMPARE(Page.Value().entries_.size(), 1u); QCOMPARE(Page.Value().entries_.front().name_, std::string("read-only.txt"));
        auto Refreshed = Session.Value()->Enumerate(Directory.path(), {}, Reader); QVERIFY(Refreshed.HasValue());
    }
    void initTestCase()
    {
        const auto Remote = qEnvironmentVariable("HC_MUTATION_ROOT");
        if (!Remote.isEmpty())
        {
            QVERIFY(Remote.compare("\\\\DEVTESTER-QTS\\Public", Qt::CaseInsensitive) == 0
                || Remote.compare("\\\\DEVTESTER-QTB4\\Pubs", Qt::CaseInsensitive) == 0);
            kind_ = LocatorKind::Smb;
            root_ = std::make_unique<QTemporaryDir>(QDir(Remote).filePath("hc-write-test-XXXXXX"));
        }
        else root_ = std::make_unique<QTemporaryDir>();
        QVERIFY(root_->isValid());
        reader_ = MakeNativeReadProvider(kind_);
        writer_ = MakeNativeWritableStorage(kind_, reader_);
        qInfo().noquote() << "Owned mutation fixture:" << root_->path();
    }
    void smbReadSourceCopiesToLocal_data()
    {
        QTest::addColumn<bool>("Verify");
        QTest::newRow("metadata") << false;
        QTest::newRow("metadata-sha256") << true;
    }
    void smbReadSourceCopiesToLocal()
    {
        QFETCH(bool, Verify);
        const auto SourcePath = qEnvironmentVariable("HC_SMB_READ_SOURCE");
        if (SourcePath.isEmpty()) QSKIP("Set HC_SMB_READ_SOURCE to opt in to a read-only SMB source copy.");
        QVERIFY(SourcePath.startsWith("\\\\"));
        // The remote path is only read. All output and staging artifacts belong
        // to this local temporary directory; never reuse HC_MUTATION_ROOT here.
        QTemporaryDir Destination; QVERIFY(Destination.isValid());
        QVERIFY(!Destination.path().startsWith("//") && !Destination.path().startsWith("\\\\"));
        auto SourceReader = MakeNativeReadProvider(LocatorKind::Smb);
        auto Source = SourceReader->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(SourcePath).toStdString()}, {}).get();
        QVERIFY2(Source.HasValue(), Source.HasValue() ? "" : Source.Failure().code_.c_str());
        QVERIFY(!Source.Value().directory_);
        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
        auto LocalWriter = MakeNativeWritableStorage(LocatorKind::Local, LocalReader);
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        Request.sources_ = {{Source.Value(), SourceReader, {}}};
        Request.destination_ = {LocatorKind::Local, QDir::toNativeSeparators(Destination.path()).toStdString()};
        Request.destinationWriter_ = LocalWriter;
        Request.copyOptions_.verify_ = Verify;
        auto Submitted = SubmitFileOperation(Scheduler, Request); QVERIFY(Submitted.HasValue());
        const auto Ready = Submitted.Value().completion_.wait_for(std::chrono::minutes(10));
        if (Ready != std::future_status::ready) Scheduler.Cancel(Submitted.Value().id_);
        QVERIFY(Ready == std::future_status::ready);
        const auto& Outcome = Submitted.Value().completion_.get();
        QVERIFY2(Outcome.snapshot_.state_ == JobState::Completed,
            Outcome.snapshot_.error_ ? Outcome.snapshot_.error_->code_.c_str() : "SMB copy did not complete");
        QCOMPARE(Outcome.snapshot_.completedItems_, 1u);
        QCOMPARE(Outcome.snapshot_.progress_.bytesDone_, Source.Value().size_);
        QCOMPARE(std::uint64_t(QFileInfo(Destination.filePath(QString::fromStdString(Source.Value().name_))).size()), Source.Value().size_);
        QVERIFY(QDir(Destination.path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden | QDir::System).isEmpty());
        qInfo() << "Read-only SMB source copied to owned local output:" << SourcePath << "bytes:" << Source.Value().size_ << "SHA-256:" << Verify;
    }
    void r10CopyElevationPreservesCompletedItems()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Elevation is intentionally local-only.");
        QVERIFY(writer_->MakeDirectory(at("r10-copy-src"),{}).HasValue()); QVERIFY(writer_->MakeDirectory(at("r10-copy-dst"),{}).HasValue());
        QVERIFY(write("r10-copy-src/a.txt","alpha")); QVERIFY(write("r10-copy-src/b.txt","beta"));
        auto Service = std::make_shared<FixtureElevationService>(reader_,writer_);
        FileOperationRequest Request; Request.destination_ = at("r10-copy-dst");
        Request.destinationWriter_ = std::make_shared<DeniedContentWriter>(writer_,"b.txt"); Request.elevation_ = Service;
        for (const auto& Name : {"a.txt","b.txt"}) { auto Source=writer_->Inspect(at(QString("r10-copy-src/")+Name),{}); QVERIFY(Source.HasValue()&&Source.Value()); Request.sources_.push_back({*Source.Value(),reader_,{}}); }
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Started=SubmitFileOperation(Scheduler,Request); QVERIFY(Started.HasValue()); const auto Id=Started.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_,JobState::WaitingForUser);
        QCOMPARE(read("r10-copy-dst/a.txt"),QByteArray("alpha")); QVERIFY(!QFileInfo::exists(root_->filePath("r10-copy-dst/b.txt")));
        // A completed item may change independently; it must never be copied again.
        QVERIFY(write("r10-copy-src/a.txt","changed after completion"));
        auto Question=Scheduler.Snapshot(Id).Value().interaction_; QVERIFY(Question); QCOMPARE(Question->kind_,InteractionKind::Elevation);
        QVERIFY(Scheduler.Reply(Id,{Question->id_,InteractionChoice::Elevate,{}}).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);
        const auto& Result=Started.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_==JobState::Completed,Result.snapshot_.error_?Result.snapshot_.error_->code_.c_str():"copy not completed");
        QCOMPARE(Result.snapshot_.completedItems_,2u); QCOMPARE(Result.snapshot_.progress_.bytesDone_,9u);
        QCOMPARE(read("r10-copy-dst/a.txt"),QByteArray("alpha")); QCOMPARE(read("r10-copy-dst/b.txt"),QByteArray("beta"));
        const auto Requests=Service->Requests(); QCOMPARE(Requests.size(),1u); QCOMPARE(Requests.front().jobId_,Id); QCOMPARE(Requests.front().source_->name_,std::string("b.txt"));
        QVERIFY(Result.cleanupErrors_.empty());
    }
    void r10MoveElevatesRemovalAndReplacesPartialOutcomes()
    {
        if(kind_!=LocatorKind::Local)QSKIP("Elevation is intentionally local-only.");
        QVERIFY(writer_->MakeDirectory(at("r10-move-src"),{}).HasValue()); QVERIFY(writer_->MakeDirectory(at("r10-move-dst"),{}).HasValue());
        QVERIFY(write("r10-move-src/a.txt","alpha")); QVERIFY(write("r10-move-src/b.txt","beta"));
        auto Service=std::make_shared<FixtureElevationService>(reader_,writer_);
        FileOperationRequest Request; Request.operation_=hc::operations::FileOperation::Move; Request.destination_=at("r10-move-dst"); Request.destinationWriter_=writer_; Request.elevation_=Service;
        auto Denied=std::make_shared<FailedRemoveWriter>(writer_);
        for(const auto& Name:{"a.txt","b.txt"}) {auto Source=writer_->Inspect(at(QString("r10-move-src/")+Name),{});QVERIFY(Source.HasValue()&&Source.Value());Request.sources_.push_back({*Source.Value(),reader_,Denied});}
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started=SubmitFileOperation(Scheduler,Request);QVERIFY(Started.HasValue());const auto Id=Started.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_,JobState::WaitingForUser);
        QCOMPARE(Scheduler.Snapshot(Id).Value().failedItems_,1u); QCOMPARE(read("r10-move-dst/a.txt"),QByteArray("alpha")); QCOMPARE(read("r10-move-src/a.txt"),QByteArray("alpha"));
        auto Question=Scheduler.Snapshot(Id).Value().interaction_;QVERIFY(Question);QVERIFY(Scheduler.Reply(Id,{Question->id_,InteractionChoice::ElevateAll,{}}).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);
        const auto& Result=Started.Value().completion_.get();QVERIFY2(Result.snapshot_.state_==JobState::Completed,Result.snapshot_.error_?Result.snapshot_.error_->code_.c_str():"move not completed");
        QCOMPARE(Result.items_.size(),2u);QCOMPARE(Result.snapshot_.failedItems_,0u);QCOMPARE(Result.snapshot_.completedItems_,2u);
        const auto Requests=Service->Requests();QCOMPARE(Requests.size(),2u);
        for(const auto& R:Requests){QCOMPARE(R.action_,FileElevationAction::Remove);QVERIFY(R.requiresPublishedDestination_&&R.publishedDestination_);QCOMPARE(R.jobId_,Id);}
        for(const auto& Item:Result.items_){QVERIFY(Item.destinationCommitted_&&Item.sourceRemoved_);QCOMPARE(Item.status_,ItemOutcome::Status::Completed);}
        QVERIFY(!QFileInfo::exists(root_->filePath("r10-move-src/a.txt")));QVERIFY(!QFileInfo::exists(root_->filePath("r10-move-src/b.txt")));
    }
    void r10PermissionCopyOnlyPreservesSource()
    {
        if(kind_!=LocatorKind::Local)QSKIP("Elevation is intentionally local-only.");
        QVERIFY(write("r10-keep.txt","keep"));QVERIFY(writer_->MakeDirectory(at("r10-keep-dst"),{}).HasValue());
        auto Source=writer_->Inspect(at("r10-keep.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());
        auto Service=std::make_shared<FixtureElevationService>(reader_,writer_);FileOperationRequest Request;Request.operation_=hc::operations::FileOperation::Move;
        Request.sources_={{*Source.Value(),reader_,std::make_shared<FailedRemoveWriter>(writer_)}};Request.destination_=at("r10-keep-dst");Request.destinationWriter_=writer_;Request.elevation_=Service;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Started=SubmitFileOperation(Scheduler,Request);QVERIFY(Started.HasValue());const auto Id=Started.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_,JobState::WaitingForUser);const auto Q=Scheduler.Snapshot(Id).Value().interaction_;QVERIFY(Q);
        QVERIFY(Scheduler.Reply(Id,{Q->id_,InteractionChoice::CopyOnly,{}}).HasValue());QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);
        const auto& Result=Started.Value().completion_.get();QCOMPARE(Result.snapshot_.state_,JobState::Completed);QCOMPARE(Result.snapshot_.failedItems_,0u);
        QCOMPARE(read("r10-keep.txt"),QByteArray("keep"));QCOMPARE(read("r10-keep-dst/r10-keep.txt"),QByteArray("keep"));
        QCOMPARE(Result.items_.size(),1u);QVERIFY(Result.items_.front().destinationCommitted_&&!Result.items_.front().sourceRemoved_);QVERIFY(Service->Requests().empty());
    }
    void r10ApprovalDoesNotCrossJobsAndCancellationClosesHelper()
    {
        if(kind_!=LocatorKind::Local)QSKIP("Elevation is intentionally local-only.");
        QVERIFY(write("r10-cancel.txt","cancel"));QVERIFY(writer_->MakeDirectory(at("r10-cancel-dst"),{}).HasValue());
        auto Source=writer_->Inspect(at("r10-cancel.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());
        auto Service=std::make_shared<FixtureElevationService>(reader_,writer_);Service->state_->hold_=true;
        FileOperationRequest Request;Request.sources_={{*Source.Value(),reader_,{}}};Request.destination_=at("r10-cancel-dst");Request.destinationWriter_=std::make_shared<DeniedContentWriter>(writer_);Request.elevation_=Service;Request.elevationApproval_=std::make_shared<FileElevationApproval>();
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto First=SubmitFileOperation(Scheduler,Request);QVERIFY(First.HasValue());const auto Id=First.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_,JobState::WaitingForUser);const auto Q=Scheduler.Snapshot(Id).Value().interaction_;QVERIFY(Q);
        QVERIFY(Scheduler.Reply(Id,{Q->id_,InteractionChoice::ElevateAll,{}}).HasValue());QTRY_COMPARE(Service->Requests().size(),1u);
        QVERIFY(Scheduler.Pause(Id,true).HasValue());QTRY_VERIFY(Service->Paused());QVERIFY(Scheduler.Pause(Id,false).HasValue());QTRY_VERIFY(!Service->Paused());
        QVERIFY(Scheduler.Cancel(Id).HasValue());QVERIFY(First.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);QCOMPARE(First.Value().completion_.get().snapshot_.state_,JobState::Cancelled);QVERIFY(Service->state_->cancelled_);
        QVERIFY(!QFileInfo::exists(root_->filePath("r10-cancel-dst/r10-cancel.txt")));QCOMPARE(read("r10-cancel.txt"),QByteArray("cancel"));
        auto Second=SubmitFileOperation(Scheduler,Request);QVERIFY(Second.HasValue());QTRY_COMPARE(Scheduler.Snapshot(Second.Value().id_).Value().state_,JobState::WaitingForUser);
        QCOMPARE(Service->Requests().size(),1u);QVERIFY(Scheduler.Cancel(Second.Value().id_).HasValue());
    }
    void r10NativeHelperCopyAndProviderStreams_data() {
        QTest::addColumn<int>("Kind");QTest::newRow("local")<<int(LocatorKind::Local);QTest::newRow("smb-reader")<<int(LocatorKind::Smb);QTest::newRow("plugin-reader")<<int(LocatorKind::Plugin);
    }
    void r10NativeHelperCopyAndProviderStreams() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QFETCH(int,Kind);
        const auto Suffix=QString::number(Kind);const auto Name="r10-native-"+Suffix+".bin",Folder="r10-native-dst-"+Suffix;
        QByteArray Data(2*1024*1024+17,'x');Data[8]='\0';QVERIFY(write(Name,Data));QVERIFY(writer_->MakeDirectory(at(Folder),{}).HasValue());
        auto Source=writer_->Inspect(at(Name),{});QVERIFY(Source.HasValue()&&Source.Value());
        std::shared_ptr<IStorageProvider> Input=reader_;auto Entry=*Source.Value();
        if(Kind!=int(LocatorKind::Local)){Entry.locator_.kind_=LocatorKind(Kind);Input=std::make_shared<RemoteFixtureReader>(reader_,LocatorKind(Kind));}
        const bool RealUac=qEnvironmentVariableIntValue("HC_R10_REAL_UAC")==1;
        hc::filetools::Handle ProtectedFolder(INVALID_HANDLE_VALUE);
        QString OriginalFolder;
        const auto RestoreFolder=qScopeGuard([&]{if(ProtectedFolder.value_!=INVALID_HANDLE_VALUE&&!OriginalFolder.isEmpty())hc::permissions::security::Restore(ProtectedFolder.value_,OriginalFolder);});
        if(RealUac) {
            const auto Native=hc::filetools::NativePath(root_->filePath(Folder));
            ProtectedFolder.value_=CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),READ_CONTROL|WRITE_DAC|WRITE_OWNER,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            QVERIFY(ProtectedFolder.value_!=INVALID_HANDLE_VALUE);DWORD Code=0;OriginalFolder=hc::permissions::security::Descriptor(ProtectedFolder.value_,Code);QCOMPARE(Code,DWORD(ERROR_SUCCESS));
            PSECURITY_DESCRIPTOR OriginalSd=nullptr;PSID Owner=nullptr;BOOL Default=FALSE;LPWSTR OwnerText=nullptr;
            QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(OriginalFolder.utf16()),SDDL_REVISION_1,&OriginalSd,nullptr));
            QVERIFY(GetSecurityDescriptorOwner(OriginalSd,&Owner,&Default));QVERIFY(ConvertSidToStringSidW(Owner,&OwnerText));
            const auto Sddl="D:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;FR;;;"+QString::fromWCharArray(OwnerText)+")";LocalFree(OwnerText);LocalFree(OriginalSd);
            PSECURITY_DESCRIPTOR Restricted=nullptr;QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()),SDDL_REVISION_1,&Restricted,nullptr));
            const auto RestrictedCleanup=qScopeGuard([&]{LocalFree(Restricted);});
            PACL RestrictedDacl=nullptr;BOOL Present=FALSE;QVERIFY(GetSecurityDescriptorDacl(Restricted,&Present,&RestrictedDacl,&Default));
            QCOMPARE(SetSecurityInfo(ProtectedFolder.value_,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,RestrictedDacl,nullptr),DWORD(ERROR_SUCCESS));
            const auto Probe=hc::filetools::NativePath(root_->filePath(Folder+"/must-be-denied.tmp"));
            hc::filetools::Handle Denied(CreateFileW(reinterpret_cast<LPCWSTR>(Probe.utf16()),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));
            QVERIFY(Denied.value_==INVALID_HANDLE_VALUE);QCOMPARE(GetLastError(),DWORD(ERROR_ACCESS_DENIED));
        }
        auto Service=hc::permissions::MakeFileElevationService(RealUac?hc::permissions::FileElevationLauncher{}:FixtureLauncher());
        FileOperationRequest Request;Request.sources_={{Entry,Input,{}}};Request.destination_=at(Folder);Request.destinationWriter_=RealUac?writer_:std::make_shared<DeniedContentWriter>(writer_);Request.copyOptions_.verify_=true;Request.elevation_=Service;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Job=SubmitFileOperation(Scheduler,Request,ElevationContext());QVERIFY(Job.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Job.Value().id_).Value().state_,JobState::WaitingForUser);
        const auto Question=Scheduler.Snapshot(Job.Value().id_).Value().interaction_;QVERIFY(Question);
        QVERIFY(Scheduler.Reply(Job.Value().id_,{Question->id_,InteractionChoice::Elevate,{}}).HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(RealUac?180:15))==std::future_status::ready);
        const auto& Result=Job.Value().completion_.get();QVERIFY2(Result.snapshot_.state_==JobState::Completed,Result.snapshot_.error_?Result.snapshot_.error_->code_.c_str():"cleanup failure");
        QCOMPARE(read(Folder+'/'+Name),Data);QCOMPARE(Result.snapshot_.progress_.bytesDone_,std::uint64_t(Data.size()));QVERIFY(Result.cleanupErrors_.empty());
    }
    void r10NativeHelperRejectsChangedAndDeadRequests_data() {
        QTest::addColumn<int>("Change");QTest::newRow("source")<<0;QTest::newRow("destination")<<1;QTest::newRow("helper-exit")<<2;
    }
    void r10NativeHelperRejectsChangedAndDeadRequests() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QFETCH(int,Change);
        const auto Name="r10-revalidate-"+QString::number(Change)+".txt",Folder="r10-revalidate-dst-"+QString::number(Change);
        QVERIFY(write(Name,"original"));QVERIFY(writer_->MakeDirectory(at(Folder),{}).HasValue());auto Source=writer_->Inspect(at(Name),{});QVERIFY(Source.HasValue()&&Source.Value());
        const auto Before=[&,Change]{if(Change==0){QFile F(root_->filePath(Name));if(F.open(QIODevice::WriteOnly))F.write("changed while awaiting UAC");}else if(Change==1){QFile F(root_->filePath(Folder+'/'+Name));if(F.open(QIODevice::WriteOnly))F.write("concurrent target");}};
        auto Service=hc::permissions::MakeFileElevationService(FixtureLauncher(Before,Change==2));
        FileElevationRequest Request;Request.source_=*Source.Value();Request.destination_=at(Folder+'/'+Name);Request.sourceReader_=reader_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());JobRequest Options;Options.operation_=ElevationContext();auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,Service));QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(10))==std::future_status::ready);const auto& Result=Job.Value().completion_.get();QCOMPARE(Result.snapshot_.state_,JobState::Failed);
        QVERIFY(Result.snapshot_.error_);if(Change!=2)QVERIFY2(Result.snapshot_.error_->category_==ErrorCategory::StaleRevision,Result.snapshot_.error_->code_.c_str());
        if(Change==1)QCOMPARE(read(Folder+'/'+Name),QByteArray("concurrent target"));else QVERIFY(!QFileInfo::exists(root_->filePath(Folder+'/'+Name)));
    }
    void r10NativeCancellationDuringLaunchCannotPublish() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QVERIFY(write("r10-late.txt","late approval"));QVERIFY(writer_->MakeDirectory(at("r10-late-dst"),{}).HasValue());
        auto Source=writer_->Inspect(at("r10-late.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());std::atomic<bool> Started=false,Release=false;
        auto Service=hc::permissions::MakeFileElevationService(FixtureLauncher([&]{Started=true;while(!Release)std::this_thread::sleep_for(std::chrono::milliseconds(10));}));
        FileElevationRequest Request;Request.source_=*Source.Value();Request.sourceReader_=reader_;Request.destination_=at("r10-late-dst/r10-late.txt");
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());JobRequest Options;Options.operation_=ElevationContext();auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,Service));QVERIFY(Job.HasValue());QTRY_VERIFY(Started);
        QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue());Release=true;QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(6))==std::future_status::ready);
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_,JobState::Cancelled);QTest::qWait(300);QVERIFY(!QFileInfo::exists(root_->filePath("r10-late-dst/r10-late.txt")));
    }
    void r10RemoteMoveOffersCopyOnlyWithoutLocalElevation() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QVERIFY(write("r10-remote-move.txt","remote data"));QVERIFY(writer_->MakeDirectory(at("r10-remote-move-dst"),{}).HasValue());
        auto Source=writer_->Inspect(at("r10-remote-move.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());auto Entry=*Source.Value();Entry.locator_.kind_=LocatorKind::Smb;
        FileOperationRequest Request;Request.operation_=hc::operations::FileOperation::Move;Request.sources_={{Entry,std::make_shared<RemoteFixtureReader>(reader_,LocatorKind::Smb),std::make_shared<FailedRemoveWriter>(writer_)}};Request.destination_=at("r10-remote-move-dst");Request.destinationWriter_=writer_;Request.elevation_=hc::permissions::MakeFileElevationService(FixtureLauncher());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Job=SubmitFileOperation(Scheduler,Request);QVERIFY(Job.HasValue());QTRY_COMPARE(Scheduler.Snapshot(Job.Value().id_).Value().state_,JobState::WaitingForUser);
        const auto Question=Scheduler.Snapshot(Job.Value().id_).Value().interaction_;QVERIFY(Question);QVERIFY(std::find(Question->choices_.begin(),Question->choices_.end(),InteractionChoice::Elevate)==Question->choices_.end());
        QVERIFY(Scheduler.Reply(Job.Value().id_,{Question->id_,InteractionChoice::CopyOnly,{}}).HasValue());QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);QCOMPARE(Job.Value().completion_.get().snapshot_.state_,JobState::Completed);
        QCOMPARE(read("r10-remote-move.txt"),QByteArray("remote data"));QCOMPARE(read("r10-remote-move-dst/r10-remote-move.txt"),QByteArray("remote data"));
    }
    void r10LiveSmbToLocalElevationTransport() {
        const auto Remote=qEnvironmentVariable("HC_SMB_READ_SOURCE");if(Remote.isEmpty())QSKIP("Explicit read-only live SMB source required; UAC itself is not part of this fixture.");
        if(kind_!=LocatorKind::Local)QSKIP("Local destination required.");auto Reader=MakeNativeReadProvider(LocatorKind::Smb);
        auto Resolved=Reader->Resolve({LocatorKind::Smb,Remote.toStdString()},{}).get();QVERIFY(Resolved.HasValue());auto Source=Reader->Stat(Resolved.Value().id_,{}).get();QVERIFY(Source.HasValue());
        FileElevationRequest Request;Request.source_=Source.Value();Request.sourceReader_=Reader;Request.destination_=at("r10-live-smb-copy.mp4");Request.copyOptions_.verify_=true;
        JobRequest Options;Options.operation_=ElevationContext();JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,hc::permissions::MakeFileElevationService(FixtureLauncher())));QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::minutes(10))==std::future_status::ready);const auto& Result=Job.Value().completion_.get();QVERIFY2(Result.snapshot_.state_==JobState::Completed,Result.snapshot_.error_?Result.snapshot_.error_->code_.c_str():"cleanup failure");
        QCOMPARE(std::uint64_t(QFileInfo(root_->filePath("r10-live-smb-copy.mp4")).size()),Source.Value().size_);QVERIFY(Result.cleanupErrors_.empty());
    }
    void r10NativeJournalFailureLeavesExistingDestination() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QVERIFY(write("r10-journal-src.txt","new bytes"));QVERIFY(write("r10-journal-dst.txt","existing target"));
        auto Source=writer_->Inspect(at("r10-journal-src.txt"),{}),Destination=writer_->Inspect(at("r10-journal-dst.txt"),{});QVERIFY(Source.HasValue()&&Source.Value()&&Destination.HasValue()&&Destination.Value());
        FileElevationRequest Request;Request.source_=*Source.Value();Request.destination_=at("r10-journal-dst.txt");Request.expected_={true,Destination.Value()->id_.revision_};
        JobRequest Options;Options.operation_=ElevationContext();const auto Save=Options.operation_.writeCheckpoint_;
        Options.operation_.writeCheckpoint_=[Save](const WriteRecoveryRecord& Record,std::optional<std::uint64_t> Row)->Result<std::uint64_t>{if(Record.phase_==WritePhase::Prepared)return Error{ErrorCategory::Unavailable,"fixture-journal-denied"};return Save(Record,Row);};
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,hc::permissions::MakeFileElevationService(FixtureLauncher())));QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(8))==std::future_status::ready);QCOMPARE(Job.Value().completion_.get().snapshot_.state_,JobState::Failed);
        QCOMPARE(read("r10-journal-dst.txt"),QByteArray("existing target"));QCOMPARE(read("r10-journal-src.txt"),QByteArray("new bytes"));QVERIFY(artifacts().isEmpty());
    }
    void r10NativeOtherActions_data() {
        QTest::addColumn<int>("Action");for(auto A:{FileElevationAction::MakeDirectory,FileElevationAction::CreateContent,FileElevationAction::Rename,FileElevationAction::Remove})QTest::newRow(std::to_string(int(A)).c_str())<<int(A);
    }
    void r10NativeOtherActions() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QFETCH(int,Action);const auto Name="r10-action-"+QString::number(Action);
        FileElevationRequest Request;Request.action_=FileElevationAction(Action);Request.destination_=at(Name);
        if(Request.action_==FileElevationAction::CreateContent)Request.content_={std::byte{'a'},std::byte{'b'}};
        if(Request.action_==FileElevationAction::Rename||Request.action_==FileElevationAction::Remove){QVERIFY(write(Name+"-src","original"));auto Source=writer_->Inspect(at(Name+"-src"),{});QVERIFY(Source.HasValue()&&Source.Value());Request.source_=*Source.Value();if(Request.action_==FileElevationAction::Remove)Request.destination_=Request.source_->locator_;}
        JobRequest Options;Options.operation_=ElevationContext();JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,hc::permissions::MakeFileElevationService(FixtureLauncher())));QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(8))==std::future_status::ready);const auto& Result=Job.Value().completion_.get();QVERIFY2(Result.snapshot_.state_==JobState::Completed,Result.snapshot_.error_?Result.snapshot_.error_->code_.c_str():"cleanup failure");
        if(Request.action_==FileElevationAction::MakeDirectory)QVERIFY(QFileInfo(root_->filePath(Name)).isDir());
        else if(Request.action_==FileElevationAction::CreateContent)QCOMPARE(read(Name),QByteArray("ab"));
        else if(Request.action_==FileElevationAction::Rename){QCOMPARE(read(Name),QByteArray("original"));QVERIFY(!QFileInfo::exists(root_->filePath(Name+"-src")));}
        else QVERIFY(!QFileInfo::exists(root_->filePath(Name+"-src")));
    }
    void r10SecurityBackupAndRestore() {
        if(kind_!=LocatorKind::Local)QSKIP("Local fixture required.");QVERIFY(write("r10-security.txt","security fixture"));const auto Path=root_->filePath("r10-security.txt");
        const auto Native=hc::filetools::NativePath(Path);hc::filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
        DWORD Code=0;const auto Original=hc::permissions::security::Descriptor(File.value_,Code);QCOMPARE(Code,DWORD(ERROR_SUCCESS));
        const bool RealUac=qEnvironmentVariableIntValue("HC_R10_REAL_UAC")==1;
        const auto Execute=[&](FileElevationRequest Request) {JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());JobRequest Options;Options.operation_=ElevationContext();auto Job=Scheduler.Submit(Options,std::make_unique<FileElevationJob>(Request,hc::permissions::MakeFileElevationService(RealUac?hc::permissions::FileElevationLauncher{}:FixtureLauncher())));if(!Job.HasValue())return JobResult{};if(Job.Value().completion_.wait_for(std::chrono::seconds(RealUac?180:10))!=std::future_status::ready)return JobResult{};return Job.Value().completion_.get();};
        auto Source=writer_->Inspect(at("r10-security.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());FileElevationRequest Request;Request.source_=*Source.Value();Request.destination_=at("r10-security.txt");Request.action_=FileElevationAction::SecurityChange;Request.grantAccess_=true;
        const auto Changed=Execute(Request);
        if(Changed.snapshot_.error_&&Changed.snapshot_.error_->code_=="security-change:1314")QSKIP("Actual security changes require SeRestorePrivilege; UAC account approval remains manual acceptance.");
        QVERIFY2(Changed.snapshot_.state_==JobState::Completed,Changed.snapshot_.error_?Changed.snapshot_.error_->code_.c_str():"no result");QVERIFY(!Changed.items_.empty());
        QVERIFY(Changed.items_.front().source_);const auto Backup=QJsonDocument::fromJson(QByteArray::fromStdString(Changed.items_.front().source_->hints_.at("securityBackup"))).object();QCOMPARE(Backup["state"].toString(),QString("applied"));
        QVERIFY(QFileInfo::exists(Backup["backupPath"].toString()));
        QFile OriginalBackup(Backup["backupPath"].toString());QVERIFY(OriginalBackup.open(QIODevice::ReadOnly));const auto BackupBytes=OriginalBackup.readAll();OriginalBackup.close();
        QCOMPARE(QJsonDocument::fromJson(BackupBytes).object()["state"].toString(),QString("prepared"));QVERIFY(QFileInfo::exists(Backup["statusPath"].toString()));
        Request.action_=FileElevationAction::SecurityRestore;Request.grantAccess_=false;Request.backupPath_=Backup["backupPath"].toString().toStdString();Request.backupIdentity_=Backup["backupIdentity"].toString().toStdString();Request.securityDigest_=Backup["appliedDigest"].toString().toStdString();
        const auto Restored=Execute(Request);QVERIFY2(Restored.snapshot_.state_==JobState::Completed,Restored.snapshot_.error_?Restored.snapshot_.error_->code_.c_str():"no result");
        QVERIFY(OriginalBackup.open(QIODevice::ReadOnly));QCOMPARE(OriginalBackup.readAll(),BackupBytes);OriginalBackup.close();
        // The documented file API converts legacy inheritance to AI. Compare
        // every owner/group/ACE/protection bit, allowing only that marker.
        const auto Normalize=[](QString Descriptor){return Descriptor.replace("D:PAI","D:P").replace("D:AI","D:");};
        QCOMPARE(Normalize(hc::permissions::security::Descriptor(File.value_,Code)),Normalize(Original));QCOMPARE(Code,DWORD(ERROR_SUCCESS));
    }
    void r10ApprovalExpiresAndBindsScope()
    {
        auto Now=std::chrono::steady_clock::now(); FileElevationApproval Approval([&Now] { return Now; });
        Approval.Choose("job-one","selected-root",InteractionChoice::Elevate);
        QVERIFY(Approval.Decision("job-one","selected-root"));
        QVERIFY(!Approval.Decision("job-two","selected-root")); QVERIFY(!Approval.Decision("job-one","other-root"));
        Now+=std::chrono::minutes(2); QVERIFY(!Approval.Decision("job-one","selected-root"));
    }
    void r10ApprovalRevalidatesSourceAndDestination_data()
    {
        QTest::addColumn<bool>("DestinationChanged");QTest::newRow("source-changed")<<false;QTest::newRow("destination-appeared")<<true;
    }
    void r10ApprovalRevalidatesSourceAndDestination()
    {
        if(kind_!=LocatorKind::Local)QSKIP("Elevation is intentionally local-only.");QFETCH(bool,DestinationChanged);
        QVERIFY(write("r10-change.txt","original"));if (!QFileInfo::exists(root_->filePath("r10-change-dst"))) QVERIFY(writer_->MakeDirectory(at("r10-change-dst"),{}).HasValue());
        auto Source=writer_->Inspect(at("r10-change.txt"),{});QVERIFY(Source.HasValue()&&Source.Value());
        auto Service=std::make_shared<FixtureElevationService>(reader_,writer_);FileOperationRequest Request;Request.sources_={{*Source.Value(),reader_,{}}};Request.destination_=at("r10-change-dst");Request.destinationWriter_=std::make_shared<DeniedContentWriter>(writer_);Request.elevation_=Service;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());auto Started=SubmitFileOperation(Scheduler,Request);QVERIFY(Started.HasValue());const auto Id=Started.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_,JobState::WaitingForUser);
        if(DestinationChanged)QVERIFY(write("r10-change-dst/r10-change.txt","concurrent destination"));else QVERIFY(write("r10-change.txt","changed before approval"));
        const auto Q=Scheduler.Snapshot(Id).Value().interaction_;QVERIFY(Q);QVERIFY(Scheduler.Reply(Id,{Q->id_,InteractionChoice::Elevate,{}}).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4))==std::future_status::ready);const auto& Result=Started.Value().completion_.get();QCOMPARE(Result.snapshot_.state_,JobState::Failed);QVERIFY(Result.snapshot_.error_);QCOMPARE(Result.snapshot_.error_->category_,ErrorCategory::StaleRevision);
        if(DestinationChanged)QCOMPARE(read("r10-change-dst/r10-change.txt"),QByteArray("concurrent destination"));else QVERIFY(!QFileInfo::exists(root_->filePath("r10-change-dst/r10-change.txt")));
        QFile::remove(root_->filePath("r10-change-dst/r10-change.txt"));
    }
    void r9CreateTemplatesThroughScheduler()
    {
        using namespace hc::filetools;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        const Locator Parent{kind_,root_->path().toStdString()};
        QCOMPARE(FileOperationLane({LocatorKind::Local,"C:/folder"}),std::string("volume:c:"));
        QCOMPARE(FileOperationLane({LocatorKind::Smb,"//SERVER/SHARE/folder"}),std::string("share:\\\\server\\share"));
        for (int Type = 0; Type < 8; ++Type) {
            const auto Name = "r9_" + QString::number(Type) + ".txt";
            auto Prepared = PrepareCreate(writer_, Parent, Name, Type, true); QVERIFY(Prepared.HasValue());
            auto Started = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Started.HasValue());
            QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
            const auto Result = Started.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "missing error"); QCOMPARE(Result.snapshot_.state_,JobState::Completed); QCOMPARE(Result.items_.size(),std::size_t(1)); QVERIFY(Result.items_[0].destinationCommitted_);
            const auto Expected = (Type ? QByteArray::fromHex("efbbbf") : QByteArray()) + Templates()[Type].text_.toUtf8(); QCOMPARE(read(Name), Expected);
            Prepared = PrepareCreate(writer_, Parent, Name, Type, false); QVERIFY(Prepared.HasValue());
            Started = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Started.HasValue());
            QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready); QCOMPARE(Started.Value().completion_.get().snapshot_.state_,JobState::Failed); QCOMPARE(read(Name), Expected);
        }
        QVERIFY(!PrepareCreate(writer_, Parent, "../escape",0,false).HasValue()); QVERIFY(!PrepareCreate(writer_, Parent,"bad.txt",8,false).HasValue()); QVERIFY(!PrepareCreate({},Parent,"bad.txt",0,false).HasValue());
    }
    void r9CreateCancellationAndWriteFailure()
    {
        using namespace hc::filetools;
        const Locator Parent{kind_,root_->path().toStdString()};
        auto Prepared = PrepareCreate(writer_,Parent,"r9-cancel.txt",6,true); QVERIFY(Prepared.HasValue());
        JobStepContext Context;
        for (int I = 0; I < 3; ++I) QCOMPARE(Prepared.Value().task_->Advance(Context).next_,JobState::Running);
        QVERIFY(!QFileInfo::exists(root_->filePath("r9-cancel.txt"))); QVERIFY(Prepared.Value().task_->Finalize(JobState::Cancelled).empty()); QVERIFY(artifacts().isEmpty());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        for (const auto Fault : {WriteFault::Append,WriteFault::Commit}) {
            Prepared = PrepareCreate(std::make_shared<FailedRemoveWriter>(writer_,Fault),Parent,"r9-failure.txt",6,true); QVERIFY(Prepared.HasValue());
            auto Started = Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Started.HasValue());
            QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready); QCOMPARE(Started.Value().completion_.get().snapshot_.state_,JobState::Failed);
            QVERIFY(!QFileInfo::exists(root_->filePath("r9-failure.txt"))); QVERIFY(artifacts().isEmpty());
        }
        Prepared = PrepareCreate(writer_,Parent,"r9-deferred.txt",0,false); QVERIFY(Prepared.HasValue()); Prepared.Value().request_.deferred_ = true;
        auto Started = Scheduler.Submit(std::move(Prepared.Value().request_),std::move(Prepared.Value().task_)); QVERIFY(Started.HasValue()); QVERIFY(Scheduler.Cancel(Started.Value().id_).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready); QCOMPARE(Started.Value().completion_.get().snapshot_.state_,JobState::Cancelled); QVERIFY(!QFileInfo::exists(root_->filePath("r9-deferred.txt")));
    }
    void r8RecursiveFilterAndMove()
    {
        QVERIFY(QDir().mkpath(root_->filePath("r8-tree/empty"))); QVERIFY(QDir().mkpath(root_->filePath("r8-target")));
        QVERIFY(write("r8-tree/한글.TXT", "included")); QVERIFY(write("r8-tree/keep.bin", "excluded"));
        auto Entry = reader_->Resolve(at("r8-tree"), {}).get(); QVERIFY(Entry.HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        FileOperationRequest Request; Request.sources_ = {{Entry.Value(), reader_, writer_}}; Request.destination_ = at("r8-target"); Request.destinationWriter_ = writer_;
        Request.includePattern_ = "??.txt"; Request.copyOptions_.verify_ = true;
        auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Completed); QCOMPARE(Result.snapshot_.skippedItems_, uint64_t(1));
        QCOMPARE(read("r8-target/r8-tree/한글.TXT"), QByteArray("included")); QVERIFY(QDir(root_->filePath("r8-target/r8-tree/empty")).exists());
        QVERIFY(!QFile::exists(root_->filePath("r8-target/r8-tree/keep.bin")));
        QVERIFY(QDir().mkpath(root_->filePath("r8-moved"))); Request.operation_ = hc::operations::FileOperation::Move; Request.destination_ = at("r8-moved"); Request.includePattern_ = "*.TXT;*.log";
        Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Completed);
        QVERIFY(!QFile::exists(root_->filePath("r8-tree/한글.TXT"))); QCOMPARE(read("r8-tree/keep.bin"), QByteArray("excluded"));
        QVERIFY(QDir(root_->filePath("r8-tree")).exists()); QCOMPARE(read("r8-moved/r8-tree/한글.TXT"), QByteArray("included"));
    }
    void r8HashMismatchPreservesMove()
    {
        QVERIFY(write("r8-hash.txt", "immutable source")); QVERIFY(QDir().mkpath(root_->filePath("r8-hash-target"))); QVERIFY(write("r8-hash-target/r8-hash.txt", "old target"));
        auto Entry = reader_->Resolve(at("r8-hash.txt"), {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move; Request.sources_ = {{Entry.Value(), reader_, writer_}};
        Request.destination_ = at("r8-hash-target"); Request.destinationWriter_ = std::make_shared<FailedRemoveWriter>(writer_, WriteFault::Corrupt); Request.conflict_ = ConflictPolicy::Replace; Request.copyOptions_.verify_ = true;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Result.snapshot_.error_->code_, std::string("copy-hash-mismatch"));
        QVERIFY(!Result.items_.front().destinationCommitted_ && !Result.items_.front().sourceRemoved_);
        QCOMPARE(read("r8-hash.txt"), QByteArray("immutable source")); QCOMPARE(read("r8-hash-target/r8-hash.txt"), QByteArray("old target"));
        QVERIFY(QDir(root_->filePath("r8-hash-target")).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void r8NativeStreamsAndDacl()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS fixture required");
        QVERIFY(write("r8-extras.txt", "main content")); QVERIFY(write("r8-extras.txt:notes", QByteArray(8193, 'a'))); QVERIFY(write("r8-extras.txt:한글", "unicode stream"));
        const auto Path = QDir::toNativeSeparators(root_->filePath("r8-extras.txt")).toStdWString();
        PACL Dacl = nullptr; PSECURITY_DESCRIPTOR Descriptor = nullptr;
        QCOMPARE(GetNamedSecurityInfoW(Path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Descriptor), DWORD(ERROR_SUCCESS));
        const auto Free = qScopeGuard([&] { LocalFree(Descriptor); });
        QCOMPARE(SetNamedSecurityInfoW(const_cast<wchar_t*>(Path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, Dacl, nullptr), DWORD(ERROR_SUCCESS));
        const auto DaclText = [](const std::wstring& At) { PSECURITY_DESCRIPTOR Sd = nullptr; LPWSTR Text = nullptr; QString Value;
            if (GetNamedSecurityInfoW(At.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &Sd) == ERROR_SUCCESS) {
                if (ConvertSecurityDescriptorToStringSecurityDescriptorW(Sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &Text, nullptr)) { Value = QString::fromWCharArray(Text); LocalFree(Text); } LocalFree(Sd); } return Value; };
        const auto ExpectedDacl = DaclText(Path); QVERIFY(!ExpectedDacl.isEmpty());
        QVERIFY(QDir().mkpath(root_->filePath("r8-extras-target"))); auto Entry = reader_->Resolve(at("r8-extras.txt"), {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Request; Request.sources_ = {{Entry.Value(), reader_, writer_}}; Request.destination_ = at("r8-extras-target"); Request.destinationWriter_ = writer_; Request.copyOptions_ = {true, true, true, true};
        OperationContext Context; Context.maxReadBytes_ = 1024;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request, Context); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "copy incomplete");
        QCOMPARE(read("r8-extras-target/r8-extras.txt"), QByteArray("main content")); QCOMPARE(read("r8-extras-target/r8-extras.txt:notes"), QByteArray(8193, 'a')); QCOMPARE(read("r8-extras-target/r8-extras.txt:한글"), QByteArray("unicode stream"));
        QCOMPARE(DaclText(QDir::toNativeSeparators(root_->filePath("r8-extras-target/r8-extras.txt")).toStdWString()), ExpectedDacl);
        QCOMPARE(Result.snapshot_.progress_.bytesDone_, uint64_t(12 + 8193 + 14)); QVERIFY(!Result.snapshot_.progress_.bytesTotal_);
    }
    void r8CopyPolicyValidationAndMetadata()
    {
        QVERIFY(!IsValidCopyPattern("*.txt;")); QVERIFY(!IsValidCopyPattern("../*")); QVERIFY(!IsValidCopyPattern(std::string("*\0*", 3))); QVERIFY(IsValidCopyPattern("*.txt; *.bin"));
        QVERIFY(write("r8-policy.txt", "policy")); auto Source = reader_->Resolve(at("r8-policy.txt"), {}).get(); QVERIFY(Source.HasValue());
        const auto Path = QDir::toNativeSeparators(root_->filePath("r8-policy.txt")).toStdWString();
        HANDLE File = CreateFileW(Path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(File != INVALID_HANDLE_VALUE);
        const uint64_t Value = (1700000000000LL + 11644473600000LL) * 10000; FILETIME Time{DWORD(Value), DWORD(Value >> 32)};
        QVERIFY(SetFileTime(File, nullptr, nullptr, &Time)); CloseHandle(File); Source = reader_->Resolve(at("r8-policy.txt"), {}).get(); QVERIFY(Source.HasValue());
        QVERIFY(QDir().mkpath(root_->filePath("r8-policy-target"))); FileOperationRequest Request; Request.sources_ = {{Source.Value(), reader_, writer_}}; Request.destination_ = at("r8-policy-target"); Request.destinationWriter_ = writer_; Request.copyOptions_.metadata_ = false;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Completed); auto Target = writer_->Inspect(at("r8-policy-target/r8-policy.txt"), {}); QVERIFY(Target.HasValue() && Target.Value()); QVERIFY(Target.Value()->modifiedUtcMs_ != Source.Value().modifiedUtcMs_);
        class Unsupported final : public IWritableStorage { public: ProviderId Id() const override { return {"unsupported"}; }
            Result<SupportDecision> Supports(Mutation, Locator, OperationContext) override { return SupportDecision{}; }
            Result<std::optional<EntrySnapshot>> Inspect(Locator, OperationContext) override { return std::optional<EntrySnapshot>{}; }
            Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator, DestinationExpectation, OperationContext) override { return Error{ErrorCategory::Unsupported, "unavailable"}; }
            Result<Unit> MakeDirectory(Locator, OperationContext) override { return Error{ErrorCategory::Unsupported, "unavailable"}; }
            Result<Unit> Remove(EntrySnapshot, OperationContext) override { return Error{ErrorCategory::Unsupported, "unavailable"}; }
            Result<Unit> Rename(EntrySnapshot, Locator, OperationContext) override { return Error{ErrorCategory::Unsupported, "unavailable"}; } };
        Request.destinationWriter_ = std::make_shared<Unsupported>(); Request.copyOptions_.verify_ = true;
        auto Prepared = PrepareFileOperation(Request); QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, std::string("copy-options-unavailable"));
        Request.destinationWriter_ = writer_; Request.includePattern_ = "*.txt;"; Prepared = PrepareFileOperation(Request); QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, std::string("invalid-copy-options"));
        Request.includePattern_ = "*"; Request.bytesPerSecondLimit_ = (1ull << 40) + 1; QVERIFY(!PrepareFileOperation(Request).HasValue());
    }
    void r8AdsBudgetFailurePreservesSource()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS fixture required");
        QVERIFY(write("r8-ads-budget.txt", "main")); QVERIFY(write("r8-ads-budget.txt:large", QByteArray(2048, 'b'))); QVERIFY(QDir().mkpath(root_->filePath("r8-ads-budget-target")));
        auto Source = reader_->Resolve(at("r8-ads-budget.txt"), {}).get(); QVERIFY(Source.HasValue());
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move; Request.sources_ = {{Source.Value(), reader_, writer_}}; Request.destination_ = at("r8-ads-budget-target"); Request.destinationWriter_ = writer_; Request.copyOptions_.streams_ = true; Request.maxBytes_ = 1024;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Result.snapshot_.error_->code_, std::string("copy-ads-byte-budget"));
        QCOMPARE(read("r8-ads-budget.txt:large"), QByteArray(2048, 'b')); QVERIFY(!QFile::exists(root_->filePath("r8-ads-budget-target/r8-ads-budget.txt"))); QVERIFY(Result.cleanupErrors_.empty());
    }
    void r8AdsHashDetectsStageCorruption()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS fixture required");
        QVERIFY(write("r8-ads-corrupt.txt", "main")); QVERIFY(write("r8-ads-corrupt.txt:notes", "original stream"));
        auto Source = reader_->Resolve(at("r8-ads-corrupt.txt"), {}).get(); QVERIFY(Source.HasValue());
        OperationContext Context; Context.maxReadBytes_ = 1024; Locator Stage;
        Context.writeCheckpoint_ = [&](const WriteRecoveryRecord& Record, std::optional<std::size_t>) -> Result<std::size_t> { Stage = Record.stage_; return std::size_t{0}; };
        auto Session = writer_->BeginCopy(at("r8-ads-corrupt-output.txt"), {}, {true, true, false, true}, Context); QVERIFY(Session.HasValue());
        QVERIFY(Session.Value()->ConfigureCopy(Source.Value(), {true, true, false, true}, Context).HasValue());
        const QByteArray Main("main"); QVERIFY(Session.Value()->RecordSourceBytes({reinterpret_cast<const std::byte*>(Main.constData()), std::size_t(Main.size())}, Context).HasValue()); QVERIFY(append(*Session.Value(), Main, Context).HasValue());
        bool Done = false; for (int I = 0; I < 32 && !Done; ++I) { auto Step = Session.Value()->AdvanceCopyExtras(1024, Context); QVERIFY(Step.HasValue()); Done = Step.Value().done_; } QVERIFY(Done);
        const auto Stream = QString::fromStdString(Stage.root_).toStdWString() + L":notes";
        HANDLE File = CreateFileW(Stream.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(File != INVALID_HANDLE_VALUE);
        DWORD Written = 0; const bool Corrupted = WriteFile(File, "X", 1, &Written, nullptr); CloseHandle(File); QVERIFY(Corrupted && Written == 1);
        std::optional<Error> Failure; for (int I = 0; I < 32 && !Failure; ++I) { auto Step = Session.Value()->VerifyContent(Context); if (!Step.HasValue()) Failure = Step.Failure(); else QVERIFY(!Step.Value()); }
        QVERIFY(Failure); QCOMPARE(Failure->code_, std::string("copy-ads-hash-mismatch")); QVERIFY(Session.Value()->Abort().HasValue());
        QCOMPARE(read("r8-ads-corrupt.txt:notes"), QByteArray("original stream")); QVERIFY(!QFile::exists(root_->filePath("r8-ads-corrupt-output.txt"))); QVERIFY(artifacts().isEmpty());
    }
    void r8HardLinksCopyAsIndependentFiles()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS fixture required");
        QVERIFY(write("r8-hard-original.txt", "hardlink content"));
        const auto Original = QDir::toNativeSeparators(root_->filePath("r8-hard-original.txt")).toStdWString(), Link = QDir::toNativeSeparators(root_->filePath("r8-hard-link.txt")).toStdWString();
        QVERIFY(CreateHardLinkW(Link.c_str(), Original.c_str(), nullptr)); QVERIFY(QDir().mkdir(root_->filePath("r8-hard-target")));
        auto A = reader_->Resolve(at("r8-hard-original.txt"), {}).get(), B = reader_->Resolve(at("r8-hard-link.txt"), {}).get(); QVERIFY(A.HasValue() && B.HasValue());
        FileOperationRequest Request; Request.sources_ = {{A.Value(), reader_, writer_}, {B.Value(), reader_, writer_}}; Request.destination_ = at("r8-hard-target"); Request.destinationWriter_ = writer_; Request.copyOptions_.verify_ = true;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(3)) == std::future_status::ready); QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("r8-hard-target/r8-hard-link.txt"), QByteArray("hardlink content")); QVERIFY(write("r8-hard-target/r8-hard-original.txt", "changed target")); QCOMPARE(read("r8-hard-target/r8-hard-link.txt"), QByteArray("hardlink content")); QCOMPARE(read("r8-hard-original.txt"), QByteArray("hardlink content"));
    }
    void r8SymbolicLinkFailsBeforeCopy()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local symbolic link fixture required");
        QVERIFY(write("r8-symlink-original.txt", "symlink content"));
        const auto Original = QDir::toNativeSeparators(root_->filePath("r8-symlink-original.txt")).toStdWString(), Link = QDir::toNativeSeparators(root_->filePath("r8-symlink.txt")).toStdWString();
        if (!CreateSymbolicLinkW(Link.c_str(), Original.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) { QVERIFY(GetLastError() == ERROR_PRIVILEGE_NOT_HELD); QSKIP("Windows developer mode or symbolic-link privilege unavailable"); }
        QVERIFY(QDir().mkdir(root_->filePath("r8-symlink-target"))); auto Source = reader_->Resolve(at("r8-symlink.txt"), {}).get(); QVERIFY(Source.HasValue());
        FileOperationRequest Request; Request.sources_ = {{Source.Value(), reader_, writer_}}; Request.destination_ = at("r8-symlink-target"); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Result.snapshot_.error_->code_, std::string("reparse-entry-denied"));
        QCOMPARE(read("r8-symlink-original.txt"), QByteArray("symlink content")); QVERIFY(!QFile::exists(root_->filePath("r8-symlink-target/r8-symlink.txt"))); QVERIFY(artifacts().isEmpty());
    }
    void r8PacingReleasesWorkerAndCancels()
    {
        QVERIFY(write("r8-paced.txt", QByteArray(32768, 'p'))); QVERIFY(QDir().mkpath(root_->filePath("r8-paced-target")));
        auto Entry = reader_->Resolve(at("r8-paced.txt"), {}).get(); QVERIFY(Entry.HasValue());
        FileOperationRequest Request; Request.sources_ = {{Entry.Value(), reader_, writer_}}; Request.destination_ = at("r8-paced-target"); Request.destinationWriter_ = writer_; Request.bytesPerSecondLimit_ = 16384;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1); QElapsedTimer Clock; Clock.start();
        auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(6)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Completed); QVERIFY(Clock.elapsed() >= 1900);
        QVERIFY(write("r8-paced-cancel.txt", QByteArray(65536, 'q'))); Entry = reader_->Resolve(at("r8-paced-cancel.txt"), {}).get(); QVERIFY(Entry.HasValue()); Request.sources_ = {{Entry.Value(), reader_, writer_}};
        Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QTRY_VERIFY(Scheduler.Snapshot(Started.Value().id_).Value().progress_.bytesDone_ > 0);
        class Probe final : public IJobTask { public: JobStep Advance(JobStepContext) override { return JobStep{JobState::Completed}; } std::vector<Error> Finalize(JobState) override { return {}; } };
        auto Other = Scheduler.Submit({}, std::make_unique<Probe>()); QVERIFY(Other.HasValue());
        // A paced job retains its global admission slot. Pause releases admission;
        // the same worker must then execute another task without a blocking sleep.
        QVERIFY(Scheduler.Pause(Started.Value().id_, true).HasValue());
        QVERIFY(Other.Value().completion_.wait_for(std::chrono::seconds(1)) == std::future_status::ready); QCOMPARE(Other.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(Scheduler.Cancel(Started.Value().id_).HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QVERIFY(!QFile::exists(root_->filePath("r8-paced-target/r8-paced-cancel.txt"))); QCOMPARE(read("r8-paced-cancel.txt"), QByteArray(65536, 'q'));
    }
    void stagedCommitAndMetadata()
    {
        auto Session = writer_->BeginWrite(at("한글 staged.txt"), {}, {});
        QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
        QVERIFY(append(*Session.Value(), "first ").HasValue());
        QVERIFY(append(*Session.Value(), "second").HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("한글 staged.txt")));
        QVERIFY(Session.Value()->SetMetadata({1700000000123LL, FILE_ATTRIBUTE_ARCHIVE}, {}).HasValue());
        auto Result = Session.Value()->Commit({});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        QVERIFY(Result.Value().cleanupErrors_.empty());
        QCOMPARE(read("한글 staged.txt"), QByteArray("first second"));
        auto Entry = writer_->Inspect(at("한글 staged.txt"), {});
        QVERIFY(Entry.HasValue() && Entry.Value());
        QCOMPARE(Entry.Value()->modifiedUtcMs_, 1700000000123LL);
        auto Stream = reader_->OpenRead(Entry.Value()->id_, {}).get();
        QVERIFY(Stream.HasValue());
        auto Bytes = Stream.Value()->Read(64, {}).get();
        QVERIFY(Bytes.HasValue());
        QCOMPARE(Bytes.Value().bytes_.size(), std::size_t{12});
        QVERIFY(Session.Value()->Abort().HasValue());
        QCOMPARE(read("한글 staged.txt"), QByteArray("first second"));
        QVERIFY(artifacts().isEmpty());
    }
    void destinationAttributesAreVerified()
    {
        constexpr DWORD Mask = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY;
        for (const auto& Name : {QString("ordinary-attributes.txt"), QString(".dot-attributes.txt")})
        {
            for (const DWORD Attributes : {DWORD(FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN),
                DWORD(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)})
            {
                const auto Path = QDir::toNativeSeparators(root_->filePath(Name)).toStdWString();
                const auto Remove = qScopeGuard([&] { SetFileAttributesW(Path.c_str(), FILE_ATTRIBUTE_NORMAL); QFile::remove(QString::fromStdWString(Path)); });
                auto Session = writer_->BeginWrite(at(Name), {}, {});
                QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
                QVERIFY(append(*Session.Value(), "attribute fixture").HasValue());
                QVERIFY(Session.Value()->SetMetadata({1700000000123LL, Attributes}, {}).HasValue());
                auto Receipt = Session.Value()->Commit({});
                QVERIFY2(Receipt.HasValue(), Receipt.HasValue() ? "" : Receipt.Failure().code_.c_str());
                QCOMPARE(read(Name), QByteArray("attribute fixture"));
                const auto Actual = GetFileAttributesW(Path.c_str());
                QVERIFY(Actual != INVALID_FILE_ATTRIBUTES);
                const auto Difference = (Actual ^ Attributes) & Mask;
                qInfo() << Name << "requested" << Attributes << "actual" << Actual;
                if (kind_ == LocatorKind::Local) QCOMPARE(Difference, DWORD(0));
                if (!Difference) QVERIFY(Receipt.Value().cleanupErrors_.empty());
                else
                {
                    QCOMPARE(Receipt.Value().cleanupErrors_.size(), std::size_t(1));
                    QCOMPARE(Receipt.Value().cleanupErrors_.front().code_, Difference == FILE_ATTRIBUTE_HIDDEN && (Actual & FILE_ATTRIBUTE_HIDDEN)
                        ? std::string("destination-added-hidden-attribute") : std::string("destination-attributes-mismatch"));
                }
                QVERIFY(Session.Value()->Abort().HasValue());
                QCOMPARE(read(Name), QByteArray("attribute fixture"));
                QVERIFY(artifacts().isEmpty());
            }
        }
    }
    void coordinatorDotFileMetadata()
    {
        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
        auto LocalWriter = MakeNativeWritableStorage(LocatorKind::Local, LocalReader);
        for (const auto Operation : {hc::operations::FileOperation::Copy, hc::operations::FileOperation::Move})
        {
            QTemporaryDir SourceRoot; QVERIFY(SourceRoot.isValid());
            const auto Name = Operation == hc::operations::FileOperation::Copy ? ".copy-attributes.txt" : ".move-attributes.txt";
            const auto SourcePath = QDir::toNativeSeparators(SourceRoot.filePath(Name));
            { QFile File(SourcePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("dot file fixture"), 16); }
            QVERIFY(SetFileAttributesW(SourcePath.toStdWString().c_str(), FILE_ATTRIBUTE_ARCHIVE));
            auto Source = LocalWriter->Inspect({LocatorKind::Local, SourcePath.toStdString()}, {});
            QVERIFY(Source.HasValue() && Source.Value());
            QCOMPARE(Source.Value()->attributes_, std::uint32_t(FILE_ATTRIBUTE_ARCHIVE));
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
            FileOperationRequest Request;
            Request.operation_ = Operation;
            Request.sources_ = {{*Source.Value(), LocalReader, LocalWriter}};
            Request.destination_ = at(""); Request.destinationWriter_ = writer_;
            auto Submitted = SubmitFileOperation(Scheduler, Request); QVERIFY(Submitted.HasValue());
            QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
            const auto& Result = Submitted.Value().completion_.get();
            QCOMPARE(read(Name), QByteArray("dot file fixture"));
            const auto Actual = GetFileAttributesW(QDir::toNativeSeparators(root_->filePath(Name)).toStdWString().c_str());
            QVERIFY(Actual != INVALID_FILE_ATTRIBUTES);
            QCOMPARE(Result.items_.size(), std::size_t(1));
            QVERIFY(Result.items_.front().destinationCommitted_);
            if (Actual & FILE_ATTRIBUTE_HIDDEN)
            {
                QCOMPARE(Result.snapshot_.state_, JobState::Failed);
                QVERIFY(Result.snapshot_.error_);
                QVERIFY(Result.items_.front().error_);
                QCOMPARE(Result.items_.front().error_->code_, std::string("destination-added-hidden-attribute"));
                QVERIFY(QFileInfo::exists(SourcePath));
                QVERIFY(!Result.items_.front().sourceRemoved_);
            }
            else
            {
                QCOMPARE(Result.snapshot_.state_, JobState::Completed);
                QCOMPARE(QFileInfo::exists(SourcePath), Operation == hc::operations::FileOperation::Copy);
            }
        }
    }
    void overwriteRevalidatesDestination()
    {
        QVERIFY(write("replace.txt", "original"));
        const auto Old = writer_->Inspect(at("replace.txt"), {});
        QVERIFY(Old.HasValue() && Old.Value());
        auto Session = writer_->BeginWrite(at("replace.txt"), {true, Old.Value()->id_.revision_}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "replacement").HasValue());
        QCOMPARE(read("replace.txt"), QByteArray("original"));
        auto Committed = Session.Value()->Commit({});
        QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
        QVERIFY(Committed.Value().cleanupErrors_.empty());
        QCOMPARE(read("replace.txt"), QByteArray("replacement"));
        QVERIFY(artifacts().isEmpty());

        auto Stale = writer_->BeginWrite(at("replace.txt"), {true, Old.Value()->id_.revision_}, {});
        QVERIFY(Stale.HasValue());
        QVERIFY(append(*Stale.Value(), "must not win").HasValue());
        auto Rejected = Stale.Value()->Commit({});
        QVERIFY(!Rejected.HasValue());
        QCOMPARE(Rejected.Failure().category_, ErrorCategory::StaleRevision);
        QVERIFY(Stale.Value()->Abort().HasValue());
        QCOMPARE(read("replace.txt"), QByteArray("replacement"));
        QVERIFY(artifacts().isEmpty());
    }
    void newDestinationCannotReplaceConcurrentFile()
    {
        auto Session = writer_->BeginWrite(at("race.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "stage").HasValue());
        QVERIFY(write("race.txt", "concurrent"));
        QVERIFY(!Session.Value()->Commit({}).HasValue());
        QVERIFY(Session.Value()->Abort().HasValue());
        QCOMPARE(read("race.txt"), QByteArray("concurrent"));
        QVERIFY(artifacts().isEmpty());
    }
    void cancelAndAbortLeaveNoStage()
    {
        auto Session = writer_->BeginWrite(at("cancel.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "partial").HasValue());
        std::stop_source Stop;
        Stop.request_stop();
        OperationContext Context;
        Context.stopToken_ = Stop.get_token();
        QCOMPARE(append(*Session.Value(), "more", Context).Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Session.Value()->Commit(Context).Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("cancel.txt")));
        QVERIFY(artifacts().isEmpty());
        auto Empty = writer_->BeginWrite(at("empty.txt"), {}, {});
        QVERIFY(Empty.HasValue());
        QVERIFY(Empty.Value()->Commit({}).HasValue());
        QVERIFY(QFileInfo::exists(root_->filePath("empty.txt")));
        QCOMPARE(QFileInfo(root_->filePath("empty.txt")).size(), 0);
    }
    void mkdirRenameRemoveAndStaleSource()
    {
        QVERIFY(writer_->MakeDirectory(at("directory"), {}).HasValue());
        QVERIFY(!writer_->MakeDirectory(at("directory"), {}).HasValue());
        QVERIFY(write("directory/source.txt", "source"));
        auto Source = writer_->Inspect(at("directory/source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        QVERIFY(writer_->Rename(*Source.Value(), at("directory/renamed.txt"), {}).HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("directory/source.txt")));
        QCOMPARE(read("directory/renamed.txt"), QByteArray("source"));
        auto Renamed = writer_->Inspect(at("directory/renamed.txt"), {});
        QVERIFY(Renamed.HasValue() && Renamed.Value());
        QVERIFY(write("directory/renamed.txt", "changed source"));
        QCOMPARE(writer_->Remove(*Renamed.Value(), {}).Failure().category_, ErrorCategory::StaleRevision);
        auto Directory = writer_->Inspect(at("directory"), {});
        QVERIFY(Directory.HasValue() && Directory.Value());
        QVERIFY(!writer_->Remove(*Directory.Value(), {}).HasValue());
        Renamed = writer_->Inspect(at("directory/renamed.txt"), {});
        QVERIFY(writer_->Remove(*Renamed.Value(), {}).HasValue());
        Directory = writer_->Inspect(at("directory"), {});
        QVERIFY(writer_->Remove(*Directory.Value(), {}).HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("directory")));
    }
    void replacedDirectoryAndConcurrentSourceWritesAreRejected()
    {
        QVERIFY(writer_->MakeDirectory(at("identity"), {}).HasValue());
        auto Original = writer_->Inspect(at("identity"), {});
        QVERIFY(Original.HasValue() && Original.Value());
        QVERIFY(writer_->Rename(*Original.Value(), at("identity-old"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("identity"), {}).HasValue());
        QCOMPARE(writer_->Remove(*Original.Value(), {}).Failure().category_, ErrorCategory::StaleRevision);
        QVERIFY(QFileInfo::exists(root_->filePath("identity")));
        QVERIFY(write("locked.txt", "stable bytes"));
        auto File = writer_->Inspect(at("locked.txt"), {});
        QVERIFY(File.HasValue() && File.Value());
        OperationContext Context; Context.denyConcurrentMutation_ = true;
        auto Stream = reader_->OpenRead(File.Value()->id_, Context).get();
        QVERIFY(Stream.HasValue());
        QVERIFY(!write("locked.txt", "torn bytes"));
        Stream.Value().reset();
        QCOMPARE(read("locked.txt"), QByteArray("stable bytes"));
        QVERIFY(write("locked.txt", "after close"));
    }
    void invalidNamesAndRootMutations()
    {
        for (const auto& Name : {"../escape", "CON.txt", "bad:stream", "trailing.", "trailing ", "a/../b", "a//b", "bad*name"})
            QVERIFY2(!writer_->BeginWrite(at(Name), {}, {}).HasValue(), Name);
        Locator Invalid{kind_, "relative.txt"};
        QVERIFY(!writer_->BeginWrite(Invalid, {}, {}).HasValue());
        auto Missing = writer_->Inspect(at("missing.txt"), {});
        QVERIFY(Missing.HasValue() && !Missing.Value());
        QVERIFY(artifacts().isEmpty());
    }
    void pinnedParentCannotBeRenamed()
    {
        QVERIFY(writer_->MakeDirectory(at("pinned"), {}).HasValue());
        auto Session = writer_->BeginWrite(at("pinned/file.txt"), {}, {});
        QVERIFY(Session.HasValue());
        const auto From = QDir::toNativeSeparators(root_->filePath("pinned")).toStdWString();
        const auto To = QDir::toNativeSeparators(root_->filePath("pinned-moved")).toStdWString();
        QVERIFY(!MoveFileExW(From.c_str(), To.c_str(), 0));
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(MoveFileExW(From.c_str(), To.c_str(), 0));
    }
    void junctionTraversalIsRejected()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS junction fixture; SMB reparse support depends on server");
        QVERIFY(writer_->MakeDirectory(at("junction-target"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("junction"), {}).HasValue());
        QVERIFY(write("junction-target/sentinel.txt", "preserve"));
        const auto Link = QDir::toNativeSeparators(root_->filePath("junction")).toStdWString();
        const auto Target = QDir::toNativeSeparators(root_->filePath("junction-target")).toStdWString();
        const auto Substitute = std::wstring(L"\\??\\") + Target;
        struct JunctionData
        {
            DWORD tag_;
            WORD size_, reserved_, substituteOffset_, substituteLength_, printOffset_, printLength_;
            wchar_t paths_[1];
        };
        const auto PathBytes = (Substitute.size() + Target.size() + 2) * sizeof(wchar_t);
        std::vector<std::byte> Buffer(16 + PathBytes);
        auto* Data = reinterpret_cast<JunctionData*>(Buffer.data());
        Data->tag_ = IO_REPARSE_TAG_MOUNT_POINT;
        Data->size_ = static_cast<WORD>(8 + PathBytes);
        Data->substituteLength_ = static_cast<WORD>(Substitute.size() * sizeof(wchar_t));
        Data->printOffset_ = Data->substituteLength_ + sizeof(wchar_t);
        Data->printLength_ = static_cast<WORD>(Target.size() * sizeof(wchar_t));
        memcpy(Data->paths_, Substitute.c_str(), Data->substituteLength_ + sizeof(wchar_t));
        memcpy(reinterpret_cast<std::byte*>(Data->paths_) + Data->printOffset_, Target.c_str(), Data->printLength_ + sizeof(wchar_t));
        const auto Handle = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0;
        const bool Set = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer.data(), static_cast<DWORD>(Buffer.size()), nullptr, 0, &Returned, nullptr);
        CloseHandle(Handle);
        QVERIFY(Set);
        const auto Cleanup = qScopeGuard([&] { RemoveDirectoryW(Link.c_str()); });
        auto Blocked = writer_->BeginWrite(at("junction/escaped.txt"), {}, {});
        QVERIFY(!Blocked.HasValue());
        QCOMPARE(Blocked.Failure().code_, std::string("reparse-parent-denied"));
        QVERIFY(!writer_->Inspect(at("junction"), {}).HasValue());
        auto Junction = reader_->Resolve(at("junction"), {}).get(); QVERIFY(Junction.HasValue());
        for (const auto Mode : {DeleteMode::Permanent, DeleteMode::Recycle}) for (const bool Streaming : {false, true}) {
            FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete;
            Request.sources_ = {{Junction.Value(), reader_, writer_}}; Request.deleteMode_ = Mode;
            Request.deleteWhileListing_ = Streaming; Request.confirmDelete_ = false;
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); auto Job = SubmitFileOperation(Scheduler, Request); QVERIFY(Job.HasValue());
            const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Failed);
            QCOMPARE(Done.snapshot_.error_->code_, std::string("reparse-entry-denied"));
            QCOMPARE(read("junction-target/sentinel.txt"), QByteArray("preserve"));
        }
        QCOMPARE(read("junction-target/sentinel.txt"), QByteArray("preserve"));
        QVERIFY(!QFileInfo::exists(root_->filePath("junction-target/escaped.txt")));
    }
    void longPaths()
    {
        QString Path;
        for (int Index = 0; Index < 5; ++Index)
        {
            Path += (Path.isEmpty() ? "" : "/") + QString(55, QChar('a' + Index));
            QVERIFY(writer_->MakeDirectory(at(Path), {}).HasValue());
        }
        auto Session = writer_->BeginWrite(at(Path + "/long.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "long-path-data").HasValue());
        QVERIFY(Session.Value()->Commit({}).HasValue());
        auto Entry = writer_->Inspect(at(Path + "/long.txt"), {});
        QVERIFY(Entry.HasValue() && Entry.Value());
        QVERIFY(writer_->Remove(*Entry.Value(), {}).HasValue());
        for (int Index = 0; Index < 5; ++Index)
        {
            Entry = writer_->Inspect(at(Path), {});
            QVERIFY(Entry.HasValue() && Entry.Value());
            QVERIFY(writer_->Remove(*Entry.Value(), {}).HasValue());
            Path = Path.left(Path.lastIndexOf('/'));
        }
    }
    void nestedDirectoryPlanAndNamedMove()
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::MakeDirectory; Request.createParents_ = true;
        Request.destination_ = at("r6-parent/inner/final"); Request.destinationWriter_ = writer_;
        auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "nested mkdir failed");
        QVERIFY(QFileInfo(root_->filePath("r6-parent/inner/final")).isDir()); QCOMPARE(Result.snapshot_.completedItems_, 3ULL); QCOMPARE(Result.snapshot_.progress_.plannedItems_.value_or(0), 3ULL);
        Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(write("r6-parent/block", "keep")); Request.destination_ = at("r6-parent/block/child"); Started = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Failed); QCOMPARE(read("r6-parent/block"), QByteArray("keep"));
        QVERIFY(write("r6-move.txt", "move payload")); const auto Entry = writer_->Inspect(at("r6-move.txt"), {}); QVERIFY(Entry.HasValue() && Entry.Value());
        Request.operation_ = hc::operations::FileOperation::Move; Request.sources_ = {{*Entry.Value(), reader_, writer_}}; Request.destination_ = at("r6-destination/new"); Request.destinationName_ = "renamed.txt";
        Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Moved = Started.Value().completion_.get(); QVERIFY2(Moved.snapshot_.state_ == JobState::Completed, Moved.snapshot_.error_ ? Moved.snapshot_.error_->code_.c_str() : "named move failed");
        QVERIFY(!QFileInfo::exists(root_->filePath("r6-move.txt"))); QCOMPARE(read("r6-destination/new/renamed.txt"), QByteArray("move payload"));
        Request.destinationName_ = "../escape"; QVERIFY(!PrepareFileOperation(Request).HasValue());
    }
    void partialDirectoryFailureAndCancel()
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); FileOperationRequest Request;
        Request.operation_ = hc::operations::FileOperation::MakeDirectory; Request.createParents_ = true;
        Request.destination_ = at("r6-partial/inner/final"); Request.destinationWriter_ = std::make_shared<FailedRemoveWriter>(writer_, WriteFault::Directory);
        auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Result.snapshot_.completedItems_, 1ULL);
        QVERIFY(QFileInfo(root_->filePath("r6-partial")).isDir()); QVERIFY(!QFileInfo::exists(root_->filePath("r6-partial/inner")));
        Request.destination_ = at("r6-cancel/inner/final"); Request.destinationWriter_ = writer_; const auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue());
        auto& Task = *Prepared.Value().task_; std::stop_source Stop; JobStepContext Context; Context.operation_.stopToken_ = Stop.get_token();
        bool Created = false;
        for (int I = 0; I < 20; ++I) { const auto Step = Task.Advance(Context); Context.committing_ = Step.next_ == JobState::Committing;
            if (Step.item_) { Created = true; Stop.request_stop(); break; } }
        QVERIFY(Created); const auto Cancel = Task.Advance(Context); QVERIFY(Cancel.error_); QCOMPARE(Cancel.error_->category_, ErrorCategory::Cancelled);
        QVERIFY(Task.Finalize(JobState::Cancelled).empty()); QVERIFY(QFileInfo(root_->filePath("r6-cancel")).isDir()); QVERIFY(!QFileInfo::exists(root_->filePath("r6-cancel/inner")));
    }
    void readonlyDeletePoliciesAndCancellation()
    {
        QVERIFY(write("r6-readonly-a.txt", "a")); QVERIFY(write("r6-readonly-b.txt", "b"));
        const auto A = root_->filePath("r6-readonly-a.txt"), B = root_->filePath("r6-readonly-b.txt");
        QVERIFY(SetFileAttributesW(A.toStdWString().c_str(), FILE_ATTRIBUTE_READONLY)); QVERIFY(SetFileAttributesW(B.toStdWString().c_str(), FILE_ATTRIBUTE_READONLY));
        const auto Clean = qScopeGuard([&] { SetFileAttributesW(A.toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL); SetFileAttributesW(B.toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL); });
        const auto EA = writer_->Inspect(at("r6-readonly-a.txt"), {}), EB = writer_->Inspect(at("r6-readonly-b.txt"), {}); QVERIFY(EA.HasValue() && EA.Value() && EB.HasValue() && EB.Value());
        QVERIFY(!writer_->Remove(*EA.Value(), {}).HasValue());
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>()); FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete; Request.confirmDelete_ = false;
        Request.sources_ = {{*EA.Value(), reader_, writer_}, {*EB.Value(), reader_, writer_}}; Request.readOnly_ = ReadOnlyPolicy::Skip;
        auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Started.Value().completion_.get().snapshot_.skippedItems_, 2ULL); QVERIFY(QFileInfo::exists(A) && QFileInfo::exists(B));
        Request.readOnly_ = ReadOnlyPolicy::Ask; Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Started.Value().id_).Value().state_, JobState::WaitingForUser); QVERIFY(Scheduler.Cancel(Started.Value().id_).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready); QCOMPARE(Started.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QVERIFY(QFileInfo::exists(A) && QFileInfo::exists(B));
        Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); QTRY_COMPARE(Scheduler.Snapshot(Started.Value().id_).Value().state_, JobState::WaitingForUser);
        const auto Question = *Scheduler.Snapshot(Started.Value().id_).Value().interaction_; QCOMPARE(Question.kind_, InteractionKind::ReadOnlyRemoval);
        QVERIFY(Scheduler.Reply(Started.Value().id_, {Question.id_, InteractionChoice::ConfirmAll}).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready); const auto Removed = Started.Value().completion_.get();
        QVERIFY2(Removed.snapshot_.state_ == JobState::Completed, Removed.snapshot_.error_ ? Removed.snapshot_.error_->code_.c_str() : "readonly deletion failed"); QVERIFY(!QFileInfo::exists(A) && !QFileInfo::exists(B));
    }
    void recycleAndShellRestore()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Recycle is local fixed volume only");
        using Microsoft::WRL::ComPtr;
        const auto Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); QVERIFY(SUCCEEDED(Com)); const auto Apartment = qScopeGuard([] { CoUninitialize(); });
        const auto Name = "r6-recycle-" + QUuid::createUuid().toString(QUuid::Id128) + ".txt"; QVERIFY(write(Name, "recycle payload"));
        const auto Entry = writer_->Inspect(at(Name), {}); QVERIFY(Entry.HasValue() && Entry.Value());
        std::stop_source Stop; Stop.request_stop(); OperationContext Cancelled; Cancelled.stopToken_ = Stop.get_token(); QVERIFY(!writer_->Recycle(*Entry.Value(), Cancelled).HasValue()); QVERIFY(QFileInfo::exists(root_->filePath(Name)));
        const auto Recycled = writer_->Recycle(*Entry.Value(), {}); QVERIFY2(Recycled.HasValue(), Recycled.HasValue() ? "" : Recycled.Failure().code_.c_str());
        QVERIFY(!QFileInfo::exists(root_->filePath(Name))); QVERIFY(QFileInfo::exists(QString::fromStdString(Recycled.Value().destination_.root_)));
        ComPtr<IShellItem> Item, Parent, Destination, Restored; ComPtr<ITransferSource> Transfer;
        auto Hr = SHCreateItemFromParsingName(QString::fromStdString(Recycled.Value().destination_.root_).toStdWString().c_str(), nullptr, IID_PPV_ARGS(&Item));
        if (SUCCEEDED(Hr)) Hr = Item->GetParent(&Parent); if (SUCCEEDED(Hr)) Hr = Parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&Transfer));
        if (SUCCEEDED(Hr)) Hr = SHCreateItemFromParsingName(QDir::toNativeSeparators(root_->path()).toStdWString().c_str(), nullptr, IID_PPV_ARGS(&Destination));
        if (SUCCEEDED(Hr)) Hr = Transfer->MoveItem(Item.Get(), Destination.Get(), Name.toStdWString().c_str(), TSF_NORMAL, &Restored);
        QVERIFY2(SUCCEEDED(Hr), qPrintable(QString("Shell restore HRESULT %1").arg(quint32(Hr), 8, 16, QLatin1Char('0')))); QCOMPARE(read(Name), QByteArray("recycle payload"));
    }
    void streamingBeginsBeforeWholeTreeScan()
    {
        class CountingProvider final : public IStorageProvider {
        public:
            explicit CountingProvider(std::shared_ptr<IStorageProvider> Reader) : reader_(std::move(Reader)) {}
            ProviderId Id() const override { return reader_->Id(); }
            Task<SupportDecision> Supports(Operation Op, OperationContext Context) override { return reader_->Supports(Op, Context); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override { ++enumerations_; return reader_->Enumerate(At, Context); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Context) override { return reader_->Stat(Id, Context); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Context) override { return reader_->OpenRead(Id, Context); }
            int enumerations_ = 0;
        private:
            std::shared_ptr<IStorageProvider> reader_;
        };
        QVERIFY(writer_->MakeDirectory(at("scan-source"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("scan-source/nested"), {}).HasValue());
        QVERIFY(write("scan-source/nested/payload", "payload"));
        const auto Source = writer_->Inspect(at("scan-source"), {}); QVERIFY(Source.HasValue() && Source.Value());
        for (const bool Prescan : {true, false}) {
            const auto Destination = Prescan ? QString("scan-eager") : QString("scan-stream");
            QVERIFY(writer_->MakeDirectory(at(Destination), {}).HasValue());
            auto Reader = std::make_shared<CountingProvider>(reader_);
            FileOperationRequest Request; Request.calculateSizeBefore_ = Prescan;
            Request.sources_ = {{*Source.Value(), Reader, {}}}; Request.destination_ = at(Destination); Request.destinationWriter_ = writer_;
            auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue());
            bool Created = false; JobStepContext Context; Context.jobId_ = "scan-test"; Context.committing_ = true;
            for (int StepIndex = 0; StepIndex < 100 && !Created; ++StepIndex) {
                const auto Step = Prepared.Value().task_->Advance(Context);
                QVERIFY2(Step.next_ != JobState::Failed, Step.error_ ? Step.error_->code_.c_str() : "unexpected failure");
                Created = QFileInfo::exists(root_->filePath(Destination + "/scan-source"));
            }
            QVERIFY(Created); QCOMPARE(Reader->enumerations_, Prescan ? 2 : 1);
            QVERIFY(Prepared.Value().task_->Finalize(JobState::Cancelled).empty());
        }
    }
    void deleteWhileListing_data()
    {
        QTest::addColumn<bool>("Streaming"); QTest::addColumn<bool>("Prescan"); QTest::addColumn<bool>("Cancel");
        for (const bool Streaming : {false, true}) for (const bool Prescan : {false, true}) for (const bool Cancel : {false, true})
            QTest::newRow(qPrintable(QString("stream-%1-size-%2-cancel-%3").arg(Streaming).arg(Prescan).arg(Cancel))) << Streaming << Prescan << Cancel;
    }
    void deleteWhileListing()
    {
        QFETCH(bool, Streaming); QFETCH(bool, Prescan); QFETCH(bool, Cancel);
        struct Probe { int opened_ = 0, exhausted_ = 0, alive_ = 0; };
        class Cursor final : public IEntryCursor {
        public:
            Cursor(std::unique_ptr<IEntryCursor> Inner, std::shared_ptr<Probe> Probe) : inner_(std::move(Inner)), probe_(std::move(Probe)) { ++probe_->opened_; ++probe_->alive_; }
            ~Cursor() override { --probe_->alive_; }
            Task<EntryPage> Next(size_t Count, OperationContext Context) override {
                return std::async(std::launch::deferred, [this, Count, Context]() -> Result<EntryPage> {
                    auto Page = inner_->Next(Count, Context).get();
                    if (Page.HasValue() && Page.Value().end_ && !ended_) { ended_ = true; ++probe_->exhausted_; }
                    return Page;
                });
            }
        private: std::unique_ptr<IEntryCursor> inner_; std::shared_ptr<Probe> probe_; bool ended_ = false;
        };
        class Provider final : public IStorageProvider {
        public:
            Provider(std::shared_ptr<IStorageProvider> Inner, std::shared_ptr<Probe> Probe) : inner_(std::move(Inner)), probe_(std::move(Probe)) {}
            ProviderId Id() const override { return inner_->Id(); }
            Task<SupportDecision> Supports(Operation Op, OperationContext C) override { return inner_->Supports(Op, C); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext C) override { return inner_->Stat(Id, C); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext C) override { return inner_->OpenRead(Id, C); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext C) override {
                return std::async(std::launch::deferred, [this, At, C]() -> Result<std::unique_ptr<IEntryCursor>> {
                    auto Inner = inner_->Enumerate(At, C).get(); if (!Inner.HasValue()) return Inner.Failure();
                    return std::unique_ptr<IEntryCursor>(new Cursor(std::move(Inner.Value()), probe_));
                });
            }
        private: std::shared_ptr<IStorageProvider> inner_; std::shared_ptr<Probe> probe_;
        };
        const auto Prefix = QString("delete-%1-%2-%3").arg(Streaming).arg(Prescan).arg(Cancel);
        QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/nested")));
        for (const auto* Name : {"a", "b", "nested/c"}) QVERIFY(write(Prefix + "/" + Name, "payload"));
        auto Source = writer_->Inspect(at(Prefix), {}); QVERIFY(Source.HasValue() && Source.Value());
        auto Probe = std::make_shared<struct Probe>(); auto Reader = std::make_shared<Provider>(reader_, Probe);
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete;
        Request.deleteWhileListing_ = Streaming; Request.calculateSizeBefore_ = Prescan; Request.confirmDelete_ = true;
        Request.sources_ = {{*Source.Value(), Reader, writer_}};
        // Duplicate and ancestor selections are rejected before any mutation.
        auto Duplicate = Request; Duplicate.deleteWhileListing_ = true; Duplicate.sources_.push_back(Duplicate.sources_.front());
        QVERIFY(!PrepareFileOperation(Duplicate).HasValue());
        auto Child = writer_->Inspect(at(Prefix + "/a"), {}); QVERIFY(Child.HasValue() && Child.Value());
        Duplicate.sources_.back() = {*Child.Value(), Reader, writer_}; QVERIFY(!PrepareFileOperation(Duplicate).HasValue());
        auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue());
        JobStepContext Context; Context.jobId_ = "delete-listing"; Context.committing_ = true; Context.operation_.maxPageItems_ = 1;
        std::stop_source Stop; Context.operation_.stopToken_ = Stop.get_token();
        bool Confirmed = false, Removed = false, Completed = false; JobProgress Last;
        for (int I = 0; I < 1000 && !Completed; ++I) {
            auto Step = Prepared.Value().task_->Advance(Context); Context.answer_.reset();
            if (Step.progress_) Last = *Step.progress_;
            if (Step.next_ == JobState::WaitingForUser) {
                QVERIFY(!Confirmed); QCOMPARE(Step.interaction_->kind_, InteractionKind::DeleteConfirmation);
                QVERIFY(QFileInfo::exists(root_->filePath(Prefix + "/a")));
                QVERIFY(QFileInfo::exists(root_->filePath(Prefix + "/nested/c")));
                if (Streaming) QCOMPARE(Probe->opened_, 0);
                Context.answer_ = InteractionAnswer{"", InteractionChoice::Confirm}; Confirmed = true;
            }
            QVERIFY2(Step.next_ != JobState::Failed, Step.error_ ? Step.error_->code_.c_str() : "delete failed");
            if (Step.item_ && Step.item_->sourceRemoved_ && Step.item_->source_ && !Step.item_->source_->directory_ && !Removed) {
                Removed = true; QVERIFY(Confirmed);
                QCOMPARE(Probe->opened_ > Probe->exhausted_, Streaming);
                if (Streaming) QVERIFY(!Last.plannedItems_);
                if (Cancel) {
                    Stop.request_stop(); auto Cancelled = Prepared.Value().task_->Advance(Context);
                    QCOMPARE(Cancelled.next_, JobState::Failed); QCOMPARE(Cancelled.error_->category_, ErrorCategory::Cancelled);
                    QVERIFY(Prepared.Value().task_->Finalize(JobState::Cancelled).empty()); QCOMPARE(Probe->alive_, 0);
                    QCOMPARE(QDir(root_->filePath(Prefix)).entryList(QDir::Files).size() + QDir(root_->filePath(Prefix + "/nested")).entryList(QDir::Files).size(), 2);
                    return;
                }
            }
            Completed = Step.next_ == JobState::Completed;
        }
        QVERIFY(Confirmed && Removed && Completed); QVERIFY(!QFileInfo::exists(root_->filePath(Prefix)));
        QCOMPARE(Last.deletedFiles_.value_or(99), 3ull); QCOMPARE(Last.discoveredFiles_.value_or(99), 3ull);
        QCOMPARE(Last.discoveredItems_.value_or(99), 5ull); QCOMPARE(Last.plannedItems_.value_or(99), 5ull);
        QVERIFY(Prepared.Value().task_->Finalize(JobState::Completed).empty()); QCOMPARE(Probe->alive_, 0);
    }
    void deleteDirectoryAfterCreationTimeChange_data()
    {
        QTest::addColumn<bool>("Streaming");
        QTest::newRow("prescan") << false; QTest::newRow("streaming") << true;
    }
    void deleteDirectoryAfterCreationTimeChange()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local deterministic timestamp fixture.");
        QFETCH(bool, Streaming);
        const QString Name = Streaming ? "delete-time-streaming" : "delete-time-prescan";
        QVERIFY(QDir().mkpath(root_->filePath(Name)));
        for (int I = 0; I < 37; ++I) QVERIFY(write(Name + "/" + QString::number(I), "payload"));
        auto Source = writer_->Inspect(at(Name), {}); QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete;
        Request.sources_ = {{*Source.Value(), reader_, writer_}}; Request.confirmDelete_ = false;
        Request.deleteWhileListing_ = Streaming; Request.calculateSizeBefore_ = !Streaming;
        auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue());
        bool Changed = false, Completed = false; JobStepContext Context; Context.committing_ = true;
        for (int I = 0; I < 1000 && !Completed; ++I) {
            const auto Step = Prepared.Value().task_->Advance(Context);
            QVERIFY2(Step.next_ != JobState::Failed, Step.error_ ? Step.error_->code_.c_str() : "delete failed");
            if (!Changed && Step.item_ && Step.item_->sourceRemoved_ && Step.item_->source_ && !Step.item_->source_->directory_) {
                const auto Native = QDir::toNativeSeparators(root_->filePath(Name)).toStdWString();
                const auto Handle = CreateFileW(Native.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                QVERIFY(Handle != INVALID_HANDLE_VALUE); const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
                BY_HANDLE_FILE_INFORMATION Info{}; QVERIFY(GetFileInformationByHandle(Handle, &Info));
                const auto Ticks = ((std::uint64_t(Info.ftCreationTime.dwHighDateTime) << 32) | Info.ftCreationTime.dwLowDateTime) + 10000000;
                const FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)}; QVERIFY(SetFileTime(Handle, &Time, nullptr, nullptr)); Changed = true;
            }
            Completed = Step.next_ == JobState::Completed;
        }
        QVERIFY(Changed && Completed); QVERIFY(!QFileInfo::exists(root_->filePath(Name)));
        QVERIFY(Prepared.Value().task_->Finalize(JobState::Completed).empty());
    }
    void nativeDeleteEnumerationYieldsBeforeFolderEnd()
    {
        const auto Prefix = QString("native-delete-pages"); QVERIFY(QDir().mkpath(root_->filePath(Prefix)));
        for (int I = 0; I < 8; ++I) QVERIFY(write(Prefix + "/" + QString::number(I), "payload"));
        OperationContext Context; Context.freshEnumerationMetadata_ = true; Context.incrementalEnumeration_ = true;
        Context.maxPageItems_ = 1; Context.maxEnumerationEntries_ = 2;
        auto Cursor = reader_->Enumerate(at(Prefix), Context).get(); QVERIFY(Cursor.HasValue());
        for (int I = 0; I < 2; ++I) { auto Page = Cursor.Value()->Next(1, Context).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1)); QVERIFY(!Page.Value().end_); }
        auto Budget = Cursor.Value()->Next(1, Context).get(); QVERIFY(!Budget.HasValue()); QCOMPARE(Budget.Failure().category_, ErrorCategory::BudgetExceeded);
        std::stop_source Stop; Context.stopToken_ = Stop.get_token(); Stop.request_stop();
        auto Cancelled = Cursor.Value()->Next(1, Context).get(); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(QDir(root_->filePath(Prefix)).entryList(QDir::Files).size(), 8);
    }
    void incrementalRecycleKeepsSelectedRootsAtomic()
    {
        class Provider final : public IStorageProvider {
        public:
            explicit Provider(std::shared_ptr<IStorageProvider> Reader) : reader_(std::move(Reader)) {}
            ProviderId Id() const override { return reader_->Id(); }
            Task<SupportDecision> Supports(Operation Op, OperationContext C) override { return reader_->Supports(Op, C); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext C) override { return reader_->Stat(Id, C); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext C) override { return reader_->OpenRead(Id, C); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext C) override { paths_.push_back(At.root_); return reader_->Enumerate(At, C); }
            std::vector<std::string> paths_;
        private: std::shared_ptr<IStorageProvider> reader_;
        };
        class Bin final : public IWritableStorage {
        public:
            Bin(std::shared_ptr<IWritableStorage> Writer, Locator At) : writer_(std::move(Writer)), at_(std::move(At)) {}
            ProviderId Id() const override { return writer_->Id(); }
            Result<SupportDecision> Supports(Mutation M, Locator At, OperationContext C) override { return writer_->Supports(M, At, C); }
            Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext C) override { return writer_->Inspect(At, C); }
            Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation E, OperationContext C) override { return writer_->BeginWrite(At, E, C); }
            Result<Unit> MakeDirectory(Locator At, OperationContext C) override { return writer_->MakeDirectory(At, C); }
            Result<Unit> Remove(EntrySnapshot, OperationContext) override { ++removes_; return Error{ErrorCategory::Internal, "permanent-fallback-forbidden"}; }
            Result<Unit> Rename(EntrySnapshot E, Locator At, OperationContext C) override { return writer_->Rename(E, At, C); }
            Result<WriteReceipt> Recycle(EntrySnapshot E, OperationContext C) override {
                ++recycles_; auto To = at_; To.root_ += "\\" + E.name_;
                auto Moved = writer_->Rename(E, To, C); if (!Moved.HasValue()) return Moved.Failure(); return WriteReceipt{To};
            }
            int removes_ = 0, recycles_ = 0;
        private: std::shared_ptr<IWritableStorage> writer_; Locator at_;
        };
        for (const bool Streaming : {false, true}) {
            const auto Prefix = QString("recycle-listing-%1").arg(Streaming);
            QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/a/nested"))); QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/b")));
            QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/bin")));
            for (const auto* Name : {"a/one", "a/nested/two", "b/three"}) QVERIFY(write(Prefix + "/" + Name, "payload"));
            const auto ReadOnly = root_->filePath(Prefix + "/b/three").toStdWString(); QVERIFY(SetFileAttributesW(ReadOnly.c_str(), FILE_ATTRIBUTE_READONLY));
            const auto Clear = qScopeGuard([&] { SetFileAttributesW(ReadOnly.c_str(), FILE_ATTRIBUTE_NORMAL); });
            auto Reader = std::make_shared<Provider>(reader_); auto Writer = std::make_shared<Bin>(writer_, at(Prefix + "/bin"));
            FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Delete; Request.deleteMode_ = DeleteMode::Recycle;
            Request.confirmDelete_ = false; Request.deleteWhileListing_ = Streaming; Request.readOnly_ = ReadOnlyPolicy::Skip;
            for (const auto* Name : {"a", "b"}) { auto E = writer_->Inspect(at(Prefix + "/" + Name), {}); QVERIFY(E.HasValue() && E.Value()); Request.sources_.push_back({*E.Value(), Reader, Writer}); }
            auto Prepared = PrepareFileOperation(Request); QVERIFY(Prepared.HasValue()); JobStepContext C; C.committing_ = true; C.operation_.maxPageItems_ = 1;
            bool First = false, Completed = false; JobProgress Last;
            for (int I = 0; I < 1000 && !Completed; ++I) {
                auto Step = Prepared.Value().task_->Advance(C); if (Step.progress_) Last = *Step.progress_;
                QVERIFY2(Step.next_ != JobState::Failed, Step.error_ ? Step.error_->code_.c_str() : "recycle failed");
                if (Writer->recycles_ && !First) {
                    First = true; QCOMPARE(Reader->paths_.size(), Streaming ? size_t(2) : size_t(3));
                    QVERIFY(QFileInfo::exists(root_->filePath(Prefix + "/bin/a/nested/two")));
                    QVERIFY(QFileInfo::exists(root_->filePath(Prefix + "/b/three")));
                    if (Streaming) QVERIFY(!Last.plannedItems_);
                }
                Completed = Step.next_ == JobState::Completed;
            }
            QVERIFY(First && Completed); QCOMPARE(Writer->recycles_, 1); QCOMPARE(Writer->removes_, 0);
            QCOMPARE(Last.discoveredFiles_.value_or(99), 3ull); QCOMPARE(Last.deletedFiles_.value_or(99), 2ull);
            QCOMPARE(Last.plannedItems_.value_or(99), 2ull); QVERIFY(Prepared.Value().task_->Finalize(JobState::Completed).empty());
        }
    }
    void moveDirectorySharingConflict_data()
    {
        QTest::addColumn<bool>("Prescan"); QTest::addColumn<QString>("Action");
        for (const bool Prescan : {true, false})
            for (const auto* Action : {"release", "persistent", "cancel", "replace", "new-child"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(Prescan ? "prescan" : "streaming", Action))) << Prescan << QString(Action);
    }
    void moveDirectorySharingConflict()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Native local handle-sharing fixture required.");
        QFETCH(bool, Prescan); QFETCH(QString, Action);
        const auto Prefix = QString("sharing-%1-%2").arg(Prescan).arg(Action);
        const auto SourceName = Prefix + "/source", TargetName = Prefix + "/target";
        QVERIFY(QDir().mkpath(root_->filePath(SourceName + "/empty")));
        QVERIFY(QDir().mkpath(root_->filePath(TargetName)));
        QVERIFY(write(SourceName + "/payload.txt", "move payload"));
        const auto Source = writer_->Inspect(at(SourceName), {}); QVERIFY(Source.HasValue() && Source.Value());
        const auto Native = QDir::toNativeSeparators(root_->filePath(SourceName)).toStdWString();
        HANDLE Lock = CreateFileW(Native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Lock != INVALID_HANDLE_VALUE);
        const auto Unlock = qScopeGuard([&] { if (Lock != INVALID_HANDLE_VALUE) CloseHandle(Lock); });
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move;
        Request.calculateSizeBefore_ = Prescan; Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at(TargetName); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        const auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); const auto Id = Started.Value().id_;
        QTRY_COMPARE_WITH_TIMEOUT(Scheduler.Snapshot(Id).Value().failedItems_, 1ULL, 1000);
        QCOMPARE(read(TargetName + "/source/payload.txt"), QByteArray("move payload"));
        QVERIFY(!QFileInfo::exists(root_->filePath(SourceName + "/payload.txt")));
        QVERIFY(QDir(root_->filePath(TargetName + "/source/empty")).exists());
        if (Action != "persistent") {
            QVERIFY(Scheduler.Pause(Id, true).HasValue());
            QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_, JobState::Paused);
            if (Action == "cancel") QVERIFY(Scheduler.Cancel(Id).HasValue());
            else {
                CloseHandle(Lock); Lock = INVALID_HANDLE_VALUE;
                if (Action == "replace") {
                    QVERIFY(QDir().rename(root_->filePath(SourceName), root_->filePath(Prefix + "/original")));
                    QVERIFY(QDir().mkpath(root_->filePath(SourceName)));
                }
                if (Action == "new-child") QVERIFY(write(SourceName + "/late.txt", "keep me"));
                QVERIFY(QFileInfo::exists(root_->filePath(SourceName)));
                QCOMPARE(Scheduler.Snapshot(Id).Value().state_, JobState::Paused);
                QVERIFY(Scheduler.Pause(Id, false).HasValue());
            }
        }
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get();
        QCOMPARE(Result.items_.size(), std::size_t(3));
        const auto Folder = std::ranges::find_if(Result.items_, [&](const auto& Item) {
            return Item.source_ && Item.source_->id_ == Source.Value()->id_;
        });
        QVERIFY(Folder != Result.items_.end()); QVERIFY(Folder->destinationCommitted_);
        if (Action == "release") {
            QCOMPARE(Result.snapshot_.state_, JobState::Completed); QCOMPARE(Result.snapshot_.failedItems_, 0ULL);
            QVERIFY(Folder->sourceRemoved_); QVERIFY(!QFileInfo::exists(root_->filePath(SourceName)));
        } else {
            QCOMPARE(Result.snapshot_.state_, Action == "cancel" ? JobState::Cancelled : JobState::Failed);
            QVERIFY(!Folder->sourceRemoved_); QVERIFY(QFileInfo::exists(root_->filePath(SourceName)));
            QVERIFY(Folder->error_);
            QCOMPARE(Folder->error_->code_, Action == "replace" ? std::string("revision-changed")
                : Action == "new-child" ? std::string("win32:145") : std::string("win32:32"));
            if (Action == "new-child") QCOMPARE(read(SourceName + "/late.txt"), QByteArray("keep me"));
        }
        QCOMPARE(read(TargetName + "/source/payload.txt"), QByteArray("move payload"));
    }
    void readonlyMovePolicies_data()
    {
        QTest::addColumn<bool>("Directory"); QTest::addColumn<int>("Policy");
        QTest::addColumn<bool>("ConfirmAll");
        for (const bool Directory : {false, true}) {
            for (const auto Policy : {ReadOnlyPolicy::Ask, ReadOnlyPolicy::Allow, ReadOnlyPolicy::Skip})
                QTest::newRow(qPrintable(QString("%1-%2").arg(Directory ? "directory" : "file").arg(int(Policy)))) << Directory << int(Policy) << false;
            QTest::newRow(Directory ? "directory-confirm-all" : "file-confirm-all") << Directory << int(ReadOnlyPolicy::Ask) << true;
        }
    }
    void readonlyMovePolicies()
    {
        QFETCH(bool, Directory); QFETCH(int, Policy); QFETCH(bool, ConfirmAll);
        const auto Prefix = QString("readonly-move-%1-%2-%3").arg(Directory).arg(Policy).arg(ConfirmAll);
        QVERIFY(QDir().mkpath(root_->filePath(Prefix + "/target")));
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move;
        Request.readOnly_ = ReadOnlyPolicy(Policy); Request.destination_ = at(Prefix + "/target"); Request.destinationWriter_ = writer_;
        std::vector<std::wstring> Paths;
        const auto Restore = qScopeGuard([&] { for (const auto& Path : Paths) {
            const auto Attr = GetFileAttributesW(Path.c_str());
            if (Attr != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(Path.c_str(), Attr & ~FILE_ATTRIBUTE_READONLY);
        } });
        for (const auto* Name : {"a", "b"}) {
            const auto SourceName = Prefix + '/' + Name;
            if (Directory) { QVERIFY(QDir().mkpath(root_->filePath(SourceName))); QVERIFY(write(SourceName + "/payload", "data")); }
            else QVERIFY(write(SourceName, "data"));
            const auto Native = QDir::toNativeSeparators(root_->filePath(SourceName)).toStdWString(); Paths.push_back(Native);
            Paths.push_back(QDir::toNativeSeparators(root_->filePath(Prefix + "/target/" + Name)).toStdWString());
            QVERIFY(SetFileAttributesW(Native.c_str(), GetFileAttributesW(Native.c_str()) | FILE_ATTRIBUTE_READONLY));
            const auto Entry = writer_->Inspect(at(SourceName), {}); QVERIFY(Entry.HasValue() && Entry.Value());
            Request.sources_.push_back({*Entry.Value(), reader_, writer_});
        }
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        const auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); const auto Id = Started.Value().id_;
        if (ReadOnlyPolicy(Policy) == ReadOnlyPolicy::Ask) {
            for (int I = 0; I < (ConfirmAll ? 1 : 2); ++I) {
                QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_, JobState::WaitingForUser);
                const auto Question = Scheduler.Snapshot(Id).Value().interaction_; QVERIFY(Question);
                QCOMPARE(Question->kind_, InteractionKind::ReadOnlyRemoval);
                QVERIFY(Scheduler.Reply(Id, {Question->id_, ConfirmAll ? InteractionChoice::ConfirmAll : InteractionChoice::Confirm}).HasValue());
                if (!ConfirmAll && I == 0) QTRY_VERIFY(!Scheduler.Snapshot(Id).Value().interaction_ || Scheduler.Snapshot(Id).Value().interaction_->id_ != Question->id_);
            }
        }
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Completed);
        QCOMPARE(Result.snapshot_.skippedItems_, ReadOnlyPolicy(Policy) == ReadOnlyPolicy::Skip ? 2ULL : 0ULL);
        for (const auto* Name : {"a", "b"}) {
            const auto SourceName = Prefix + '/' + Name, DestinationName = Prefix + "/target/" + Name;
            QCOMPARE(QFileInfo::exists(root_->filePath(SourceName)), ReadOnlyPolicy(Policy) == ReadOnlyPolicy::Skip);
            if (ReadOnlyPolicy(Policy) != ReadOnlyPolicy::Skip || Directory)
                QCOMPARE(read(DestinationName + (Directory ? "/payload" : "")), QByteArray("data"));
        }
    }
    void directoryMoveElevationRetainsCommittedDestination()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Elevation is intentionally local-only.");
        QVERIFY(QDir().mkpath(root_->filePath("move-elevation/source")));
        QVERIFY(QDir().mkpath(root_->filePath("move-elevation/target")));
        const auto Source = writer_->Inspect(at("move-elevation/source"), {}); QVERIFY(Source.HasValue() && Source.Value());
        auto Service = std::make_shared<FixtureElevationService>(reader_, writer_);
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, std::make_shared<FailedRemoveWriter>(writer_)}};
        Request.destination_ = at("move-elevation/target"); Request.destinationWriter_ = writer_; Request.elevation_ = Service;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        const auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue()); const auto Id = Started.Value().id_;
        QTRY_COMPARE(Scheduler.Snapshot(Id).Value().state_, JobState::WaitingForUser);
        const auto Question = Scheduler.Snapshot(Id).Value().interaction_; QVERIFY(Question);
        QCOMPARE(Question->kind_, InteractionKind::Elevation);
        QVERIFY(Scheduler.Reply(Id, {Question->id_, InteractionChoice::Elevate}).HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Completed); QCOMPARE(Result.snapshot_.failedItems_, 0ULL);
        QCOMPARE(Result.items_.size(), std::size_t(1));
        QVERIFY(Result.items_.front().destinationCommitted_ && Result.items_.front().sourceRemoved_);
        const auto Requests = Service->Requests(); QCOMPARE(Requests.size(), std::size_t(1));
        QCOMPARE(Requests.front().action_, FileElevationAction::Remove);
        QVERIFY(!Requests.front().requiresPublishedDestination_);
        QVERIFY(!QFileInfo::exists(root_->filePath("move-elevation/source")));
        QVERIFY(QDir(root_->filePath("move-elevation/target/source")).exists());
    }
    void moveWithStreamsReleasesSource()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS alternate stream fixture required.");
        QVERIFY(QDir().mkpath(root_->filePath("move-streams-target")));
        QVERIFY(write("move-streams.txt", "main")); QVERIFY(write("move-streams.txt:notes", "alternate"));
        const auto Source = writer_->Inspect(at("move-streams.txt"), {}); QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request; Request.operation_ = hc::operations::FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, writer_}}; Request.destination_ = at("move-streams-target"); Request.destinationWriter_ = writer_;
        Request.copyOptions_.streams_ = true; Request.copyOptions_.verify_ = true;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        const auto Started = SubmitFileOperation(Scheduler, Request); QVERIFY(Started.HasValue());
        QVERIFY(Started.Value().completion_.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
        const auto Result = Started.Value().completion_.get();
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "move failed");
        QVERIFY(!QFileInfo::exists(root_->filePath("move-streams.txt")));
        QCOMPARE(read("move-streams-target/move-streams.txt"), QByteArray("main"));
        QCOMPARE(read("move-streams-target/move-streams.txt:notes"), QByteArray("alternate"));
    }
    void coordinatorCopyMoveAndConfirmedDeleteTree_data()
    {
        QTest::addColumn<bool>("Prescan"); QTest::newRow("prescan") << true; QTest::newRow("streaming") << false;
    }
    void coordinatorCopyMoveAndConfirmedDeleteTree()
    {
        QFETCH(bool, Prescan);
        auto OriginalRoot = std::move(root_);
        const auto Restore = qScopeGuard([&] { root_ = std::move(OriginalRoot); });
        root_ = std::make_unique<QTemporaryDir>(OriginalRoot->filePath("plan-XXXXXX")); QVERIFY(root_->isValid());
        QVERIFY(writer_->MakeDirectory(at("tree-source"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-source/inner"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-copy"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-move"), {}).HasValue());
        QVERIFY(write("tree-source/a.txt", "alpha"));
        QVERIFY(write("tree-source/inner/b.txt", "beta"));
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        auto Source = writer_->Inspect(at("tree-source"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("tree-copy");
        Request.calculateSizeBefore_ = Prescan;
        Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Copied = Submitted.Value().completion_.get();
        if (Copied.snapshot_.error_ && Copied.snapshot_.error_->resource_)
        {
            auto Resource = *Copied.snapshot_.error_->resource_;
            const auto ExpectedRevision = Resource.revision_;
            Resource.revision_.reset();
            const auto Fresh = reader_->Stat(Resource, {}).get();
            qInfo().noquote() << "Copy failure resource:" << QString::fromStdString(Resource.key_)
                << "expected:" << (ExpectedRevision ? QString::fromStdString(ExpectedRevision->token_) : QString())
                << "fresh:" << (Fresh.HasValue() && Fresh.Value().id_.revision_ ? QString::fromStdString(Fresh.Value().id_.revision_->token_) : QString())
                << "items:" << Copied.items_.size() << "current:" << QString::fromStdString(Copied.snapshot_.progress_.currentItem_);
        }
        QVERIFY2(Copied.snapshot_.state_ == JobState::Completed, Copied.snapshot_.error_ ? Copied.snapshot_.error_->code_.c_str() : "copy did not complete");
        QCOMPARE(read("tree-copy/tree-source/a.txt"), QByteArray("alpha"));
        QCOMPARE(read("tree-copy/tree-source/inner/b.txt"), QByteArray("beta"));
        QCOMPARE(Copied.snapshot_.progress_.bytesDone_, 9u);
        if (Prescan) QCOMPARE(Copied.snapshot_.progress_.plannedItems_.value_or(0), Copied.snapshot_.completedItems_);
        else QVERIFY(!Copied.snapshot_.progress_.bytesTotal_);
        Request.operation_ = hc::operations::FileOperation::Move;
        Source = writer_->Inspect(at("tree-copy/tree-source"), {});
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("tree-move");
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Moved = Submitted.Value().completion_.get();
        if (Moved.snapshot_.error_ && Moved.snapshot_.error_->resource_)
        {
            auto Resource = *Moved.snapshot_.error_->resource_;
            const auto ExpectedRevision = Resource.revision_;
            Resource.revision_.reset();
            const auto Fresh = reader_->Stat(Resource, {}).get();
            qInfo().noquote() << "Move failure resource:" << QString::fromStdString(Resource.key_)
                << "expected:" << (ExpectedRevision ? QString::fromStdString(ExpectedRevision->token_) : QString())
                << "fresh:" << (Fresh.HasValue() && Fresh.Value().id_.revision_ ? QString::fromStdString(Fresh.Value().id_.revision_->token_) : QString())
                << "items:" << Moved.items_.size() << "committed:" << (!Moved.items_.empty() && Moved.items_.back().destinationCommitted_);
        }
        QVERIFY2(Moved.snapshot_.state_ == JobState::Completed, Moved.snapshot_.error_ ? Moved.snapshot_.error_->code_.c_str() : "move did not complete");
        if (Prescan) QCOMPARE(Moved.snapshot_.progress_.plannedItems_.value_or(0), Moved.snapshot_.completedItems_);
        QVERIFY(!QFileInfo::exists(root_->filePath("tree-copy/tree-source")));
        QCOMPARE(read("tree-move/tree-source/inner/b.txt"), QByteArray("beta"));
        Request.operation_ = hc::operations::FileOperation::Delete;
        Source = writer_->Inspect(at("tree-move/tree-source"), {});
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.confirmDelete_ = Prescan;
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        if (Prescan) {
            QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
            QVERIFY(QFileInfo::exists(root_->filePath("tree-move/tree-source/a.txt")));
            auto Prompt = *Scheduler.Snapshot(Submitted.Value().id_).Value().interaction_;
            QCOMPARE(Prompt.kind_, InteractionKind::DeleteConfirmation);
            QVERIFY(Scheduler.Reply(Submitted.Value().id_, {Prompt.id_, InteractionChoice::Confirm}).HasValue());
        }
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Deleted = Submitted.Value().completion_.get();
        QVERIFY2(Deleted.snapshot_.state_ == JobState::Completed, Deleted.snapshot_.error_ ? Deleted.snapshot_.error_->code_.c_str() : "delete did not complete");
        QVERIFY(!QFileInfo::exists(root_->filePath("tree-move/tree-source")));
        QVERIFY(artifacts().isEmpty());
    }
    void coordinatorMkdirRenameAndPreflightBudget()
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        Request.operation_ = hc::operations::FileOperation::MakeDirectory;
        Request.destination_ = at("coordinator-directory"); Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(write("coordinator-directory/from.txt", "rename payload"));
        auto Source = writer_->Inspect(at("coordinator-directory/from.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.operation_ = hc::operations::FileOperation::Rename;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("coordinator-directory/to.txt");
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("coordinator-directory/to.txt"), QByteArray("rename payload"));
        QVERIFY(!QFileInfo::exists(root_->filePath("coordinator-directory/from.txt")));
        Source = writer_->Inspect(at("coordinator-directory/to.txt"), {});
        Request.operation_ = hc::operations::FileOperation::Copy;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at(""); Request.maxBytes_ = 2;
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.error_->category_, ErrorCategory::BudgetExceeded);
        QVERIFY(!QFileInfo::exists(root_->filePath("to.txt")));
    }
    void coordinatorConflictRenameAndCancel()
    {
        QVERIFY(writer_->MakeDirectory(at("conflict-target"), {}).HasValue());
        QVERIFY(write("conflict-source.txt", "new contents"));
        QVERIFY(write("conflict-target/conflict-source.txt", "old contents"));
        auto Hub = std::make_shared<hc::commands::EventHub>();
        JobScheduler Scheduler(Hub, 1);
        FileOperationRequest Request;
        auto Source = writer_->Inspect(at("conflict-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("conflict-target");
        Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
        const auto Prompt = *Scheduler.Snapshot(Submitted.Value().id_).Value().interaction_;
        QVERIFY(Scheduler.Reply(Submitted.Value().id_, {Prompt.id_, InteractionChoice::Rename, "chosen.txt"}).HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("conflict-target/chosen.txt"), QByteArray("new contents"));
        QCOMPARE(read("conflict-target/conflict-source.txt"), QByteArray("old contents"));

        std::atomic<bool> Cancelled{false};
        auto Subscription = Hub->Subscribe("job.progress", std::make_shared<ImmediateExecutor>(), [&](const auto& Event) {
            if (std::get<std::int64_t>(Event.payload_.at("bytes")) > 0 && !Cancelled.exchange(true)) Scheduler.Cancel(Event.causationId_);
        });
        Request.conflict_ = ConflictPolicy::Replace;
        OperationContext Context;
        Context.maxReadBytes_ = 2;
        Submitted = SubmitFileOperation(Scheduler, Request, Context);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QVERIFY(Cancelled.load());
        QCOMPARE(read("conflict-target/conflict-source.txt"), QByteArray("old contents"));
        QCOMPARE(read("conflict-source.txt"), QByteArray("new contents"));
        QVERIFY(QDir(root_->filePath("conflict-target")).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void coordinatorPreservesCommittedCopyWhenSourceDeleteFails()
    {
        QVERIFY(writer_->MakeDirectory(at("partial-target"), {}).HasValue());
        QVERIFY(write("partial-source.txt", "preserve both"));
        auto Source = writer_->Inspect(at("partial-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request;
        Request.operation_ = hc::operations::FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, std::make_shared<FailedRemoveWriter>(writer_)}};
        Request.destination_ = at("partial-target"); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Result = Submitted.Value().completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Failed);
        QCOMPARE(Result.snapshot_.error_->code_, std::string("injected-source-delete"));
        QVERIFY(Result.items_.at(0).destinationCommitted_);
        QVERIFY(!Result.items_.at(0).sourceRemoved_);
        QCOMPARE(read("partial-source.txt"), QByteArray("preserve both"));
        QCOMPARE(read("partial-target/partial-source.txt"), QByteArray("preserve both"));
        Request.destination_ = at("");
        QVERIFY(!SubmitFileOperation(Scheduler, Request).HasValue());
    }
    void coordinatorFaultMatrixPreservesExistingDestination()
    {
        QVERIFY(writer_->MakeDirectory(at("fault-target"), {}).HasValue());
        QVERIFY(write("fault-source.txt", "immutable original source"));
        auto Source = writer_->Inspect(at("fault-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        for (const auto Fault : {WriteFault::Append, WriteFault::Metadata, WriteFault::Commit, WriteFault::AppendAndCleanup})
        {
            QVERIFY(write("fault-target/fault-source.txt", "existing destination"));
            FileOperationRequest Request;
            Request.sources_ = {{*Source.Value(), reader_, writer_}};
            Request.destination_ = at("fault-target");
            Request.destinationWriter_ = std::make_shared<FailedRemoveWriter>(writer_, Fault);
            Request.conflict_ = ConflictPolicy::Replace;
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
            auto Submitted = SubmitFileOperation(Scheduler, Request);
            QVERIFY(Submitted.HasValue());
            QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            const auto& Result = Submitted.Value().completion_.get();
            QCOMPARE(Result.snapshot_.state_, JobState::Failed);
            QCOMPARE(Result.cleanupErrors_.empty(), Fault != WriteFault::AppendAndCleanup);
            QCOMPARE(read("fault-source.txt"), QByteArray("immutable original source"));
            QCOMPARE(read("fault-target/fault-source.txt"), QByteArray("existing destination"));
            QVERIFY(QDir(root_->filePath("fault-target")).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
        }
    }
    void crossProviderLocalToSmbAndBack()
    {
        if (kind_ != LocatorKind::Smb) QSKIP("Run explicitly against DEVTESTER-QTS for cross-provider validation");
        QTemporaryDir Local;
        QVERIFY(Local.isValid());
        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
        auto LocalWriter = MakeNativeWritableStorage(LocatorKind::Local, LocalReader);
        const auto LocalAt = [&](const QString& Name) { return Locator{LocatorKind::Local, QDir::toNativeSeparators(Local.filePath(Name)).toStdString()}; };
        QByteArray Payload(65536, '\0');
        for (qsizetype Index = 0; Index < Payload.size(); ++Index) Payload[Index] = static_cast<char>(Index % 251);
        QFile SourceFile(Local.filePath("cross.bin"));
        QVERIFY(SourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(SourceFile.write(Payload), Payload.size());
        SourceFile.close();
        auto Source = LocalWriter->Inspect(LocalAt("cross.bin"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request;
        Request.sources_ = {{*Source.Value(), LocalReader, LocalWriter}};
        Request.destination_ = at(""); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("cross.bin"), Payload);
        QVERIFY(LocalWriter->MakeDirectory(LocalAt("returned"), {}).HasValue());
        Source = writer_->Inspect(at("cross.bin"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.operation_ = hc::operations::FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = LocalAt("returned"); Request.destinationWriter_ = LocalWriter;
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(!QFileInfo::exists(root_->filePath("cross.bin")));
        QFile Returned(Local.filePath("returned/cross.bin"));
        QVERIFY(Returned.open(QIODevice::ReadOnly));
        QCOMPARE(Returned.readAll(), Payload);
    }
    void cleanupTestCase()
    {
        QVERIFY(artifacts().isEmpty());
        writer_.reset();
        reader_.reset();
        QVERIFY(root_->remove());
    }
};
int main(int Argc,char** Argv) {
    QTemporaryDir Config(QDir(QFileInfo(QString::fromLocal8Bit(Argv[0])).absolutePath()).filePath("r10-test-settings-XXXXXX"));
    if(!Config.isValid())return 2;
    std::vector<QByteArray> Args;for(int I=0;I<Argc;++I)Args.emplace_back(Argv[I]);Args.emplace_back("--config-dir="+Config.path().toUtf8());
    std::vector<char*> Raw;for(auto& A:Args)Raw.push_back(A.data());int AppArgc=int(Raw.size());Raw.push_back(nullptr);
    QCoreApplication App(AppArgc,Raw.data());QStandardPaths::setTestModeEnabled(true);
    if(Argc==2&&QString::fromLocal8Bit(Argv[1])=="--elevation-fixture-exit")return 0;
    if(Argc==3&&QString::fromLocal8Bit(Argv[1])=="--directory-fixture") {
        const auto Bytes = QByteArray::fromBase64(QByteArray(Argv[2]), QByteArray::Base64UrlEncoding);
        return int(hc::permissions::ExecuteDirectoryElevation(QJsonDocument::fromJson(Bytes).object()));
    }
    if(Argc==3&&QString::fromLocal8Bit(Argv[1])=="--elevation-fixture") {
        // Test executable only: the production administrator main always checks
        // TokenElevation and has no fixture mode. All targets here are test owned.
        const auto Bytes=QByteArray::fromBase64(QByteArray(Argv[2]),QByteArray::Base64UrlEncoding);
        return int(hc::permissions::ExecuteFileElevation(QJsonDocument::fromJson(Bytes).object()));
    }
    WriteTests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstWrites.moc"
