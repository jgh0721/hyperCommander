#include "stdafx.h"
#include "nativeFileElevation.hpp"
#include "permissionService.hpp"
#include "storageProviders.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeStorageRevision.hpp"
#include "fileSecurityRecovery.hpp"
#include <condition_variable>
#include <atomic>
namespace hc::permissions {
namespace {
using namespace storage;
using namespace operations;
constexpr DWORD frameLimit = 2 * 1024 * 1024;
QString Text(const std::string& S) { return QString::fromStdString(S); }
QString Path(QString P){P=QDir::fromNativeSeparators(P);if(P.startsWith("//?/",Qt::CaseInsensitive))P=P.mid(4);return QDir::cleanPath(P); }
std::string String(const QJsonValue& V) { return V.toString().toStdString(); }
Error Failure(DWORD Code, const char* Stage) {
    return {Code==ERROR_CANCELLED?ErrorCategory::Cancelled:Code==ERROR_ACCESS_DENIED?ErrorCategory::AccessDenied:
        Code==ERROR_FILE_INVALID?ErrorCategory::StaleRevision:ErrorCategory::Unavailable,
        std::string(Stage)+":"+std::to_string(Code)};
}
QJsonObject EncodeError(const Error& E) { return {{"category",int(E.category_)},{"code",Text(E.code_)}}; }
Error DecodeError(const QJsonObject& E) { return {ErrorCategory(E["category"].toInt()),String(E["code"])}; }
QJsonObject Snapshot(const EntrySnapshot& E) {
    return {{"path",Path(Text(E.locator_.root_))},{"revision",E.id_.revision_?Text(E.id_.revision_->token_):QString()},{"directory",E.directory_}};
}
QString Sid(HANDLE Process=GetCurrentProcess()) {
    HANDLE Raw=nullptr; if(!OpenProcessToken(Process,TOKEN_QUERY,&Raw))return {};
    filetools::Handle Token(Raw); DWORD Size=0;GetTokenInformation(Raw,TokenUser,nullptr,0,&Size);
    QByteArray Buffer(Size,0);if(!GetTokenInformation(Raw,TokenUser,Buffer.data(),Size,&Size))return {};
    LPWSTR RawSid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Buffer.data())->User.Sid,&RawSid))return {};
    const auto Result=QString::fromWCharArray(RawSid);LocalFree(RawSid);return Result;
}
QString ProcessCreated(HANDLE Process) {
    FILETIME Created{},Exited{},Kernel{},User{};
    return GetProcessTimes(Process,&Created,&Exited,&Kernel,&User)?QString::number(native::FileTimeTicks(Created)):QString();
}
bool Local(const QString& Raw) {
    const auto P=Path(Raw);
    // Do not pass UNC, mapped network drives, virtual providers or device names
    // across the privilege boundary. The native provider owns normal SMB access.
    if(!filetools::LocalFolder(QFileInfo(P).absolutePath()) || QDir::fromNativeSeparators(P).split('/').contains("..") || P.count(':')!=1 || P.indexOf(':')!=1)return false;
    const auto Drive=QDir::toNativeSeparators(P.left(3)).toStdWString();
    const auto Kind=GetDriveTypeW(Drive.c_str());return Kind==DRIVE_FIXED||Kind==DRIVE_REMOVABLE||Kind==DRIVE_RAMDISK;
}
// All frames are bounded and overlapped. A dead helper, closed main process or
// unanswered UAC cannot hold a scheduler worker indefinitely.
bool Io(HANDLE Pipe, bool Write, QByteArray& Bytes, DWORD Timeout) {
    filetools::Handle Event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED Ov{};Ov.hEvent=Event.value_;DWORD Count=0;
    const BOOL Done=Write?WriteFile(Pipe,Bytes.constData(),DWORD(Bytes.size()),&Count,&Ov):ReadFile(Pipe,Bytes.data(),DWORD(Bytes.size()),&Count,&Ov);
    if(!Done) {
        if(GetLastError()!=ERROR_IO_PENDING)return false;
        if(WaitForSingleObject(Event.value_,Timeout)!=WAIT_OBJECT_0) {CancelIoEx(Pipe,&Ov);GetOverlappedResult(Pipe,&Ov,&Count,TRUE);return false;}
        if(!GetOverlappedResult(Pipe,&Ov,&Count,FALSE))return false;
    }
    if(Write)return Count==Bytes.size();Bytes.resize(Count);return Count>0;
}
bool Send(HANDLE Pipe,QJsonObject Object,const QString& Nonce,DWORD Timeout=5000) {
    Object["nonce"]=Nonce;auto Bytes=QJsonDocument(Object).toJson(QJsonDocument::Compact);
    return Bytes.size()<=frameLimit&&Io(Pipe,true,Bytes,Timeout);
}
std::optional<QJsonObject> Receive(HANDLE Pipe,const QString& Nonce,DWORD Timeout=5000) {
    QByteArray Bytes(frameLimit,0);if(!Io(Pipe,false,Bytes,Timeout))return {};
    QJsonParseError E;const auto Doc=QJsonDocument::fromJson(Bytes,&E);
    if(E.error!=QJsonParseError::NoError||!Doc.isObject()||Doc.object()["nonce"].toString()!=Nonce)return {};
    return Doc.object();
}
QJsonObject Recovery(const WriteRecoveryRecord& R) {
    return {{"transaction",Text(R.transaction_)},{"destination",Path(Text(R.destination_.root_))},{"stage",Text(R.stage_.root_)},
        {"backup",Text(R.backup_.root_)},{"stageId",Text(R.stageIdentity_)},{"originalId",Text(R.originalIdentity_)},{"phase",int(R.phase_)}};
}
WriteRecoveryRecord Recovery(const QJsonObject& R) {
    return {String(R["transaction"]),{LocatorKind::Local,String(R["destination"])},{LocatorKind::Local,String(R["stage"])},
        {LocatorKind::Local,String(R["backup"])},String(R["stageId"]),String(R["originalId"]),WritePhase(R["phase"].toInt())};
}
Result<EntrySnapshot> Verify(const QJsonObject& E,std::shared_ptr<IStorageProvider> Reader,OperationContext Context) {
    const auto Path=E["path"].toString();if(!Local(Path)||E["revision"].toString().isEmpty())return Failure(ERROR_INVALID_PARAMETER,"elevation-source");
    auto Resolved=Reader->Resolve({LocatorKind::Local,Path.toStdString()},Context).get();if(!Resolved.HasValue())return Resolved.Failure();
    auto Actual=Reader->Stat(Resolved.Value().id_,Context).get();if(!Actual.HasValue())return Actual.Failure();
    if((!Actual.Value().id_.revision_ || Actual.Value().id_.revision_->token_!=String(E["revision"]))||Actual.Value().directory_!=E["directory"].toBool())return Failure(ERROR_FILE_INVALID,"elevation-source-changed");
    if(Actual.Value().attributes_&FILE_ATTRIBUTE_REPARSE_POINT)return Failure(ERROR_FILE_INVALID,"elevation-reparse");
    return Actual.Value();
}
QJsonObject EncodeStatus(const FileElevationStatus& S) {
    QJsonObject R{{"kind","status"},{"bytes",QString::number(S.bytesDone_)},{"ready",S.readyToCommit_},{"finished",S.finished_},
        {"published",S.destinationCommitted_},{"removed",S.sourceRemoved_},{"detail",Text(S.detail_)}};
    if(S.error_)R["error"]=EncodeError(*S.error_);if(S.destination_)R["destination"]=Snapshot(*S.destination_);
    QJsonArray Cleanup;for(const auto& E:S.cleanupErrors_)Cleanup.append(EncodeError(E));R["cleanup"]=Cleanup;return R;
}
struct SessionState {
    std::mutex mutex_;std::condition_variable changed_;FileElevationStatus status_;
    std::atomic<bool> cancelled_{false},commit_{false},done_{false},checkpointSeen_{false};
    OperationContext context_;FileElevationLauncher launcher_;FileElevationRequest original_;QJsonObject request_;QString nonce_,helper_;HANDLE pipe_=INVALID_HANDLE_VALUE;
    ~SessionState(){if(pipe_!=INVALID_HANDLE_VALUE)CloseHandle(pipe_);}
};
void Complete(const std::shared_ptr<SessionState>& S,Error E) {
    std::lock_guard Lock(S->mutex_);S->status_.finished_=true;S->status_.error_=std::move(E);S->done_=true;S->changed_.notify_all();
}
void Run(const std::shared_ptr<SessionState>& S) {
    const auto Payload=QJsonDocument(S->request_).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals);
    if(Payload.size()>24000){Complete(S,Failure(ERROR_BUFFER_OVERFLOW,"elevation-payload"));return;}
    const auto Arguments="--elevated-job "+QString::fromLatin1(Payload);
    SHELLEXECUTEINFOW Info{};Info.cbSize=sizeof(Info);Info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    Info.lpVerb=L"runas";Info.lpFile=reinterpret_cast<LPCWSTR>(S->helper_.utf16());Info.lpParameters=reinterpret_cast<LPCWSTR>(Arguments.utf16());Info.nShow=SW_HIDE;
    if(S->launcher_) {auto Launched=S->launcher_(S->helper_,Arguments);if(!Launched.HasValue()){Complete(S,Launched.Failure());return;}Info.hProcess=Launched.Value();}
    else if(!ShellExecuteExW(&Info)){Complete(S,Failure(GetLastError(),"elevation-launch"));return;}
    filetools::Handle Process(Info.hProcess);if(!Info.hProcess){Complete(S,Failure(ERROR_INVALID_HANDLE,"elevation-launch"));return;}
    filetools::Handle Event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED Ov{};Ov.hEvent=Event.value_;
    bool Connected=ConnectNamedPipe(S->pipe_,&Ov)!=FALSE;const auto Code=Connected?ERROR_SUCCESS:GetLastError();
    if(Code==ERROR_PIPE_CONNECTED)Connected=true;
    const auto Deadline=std::chrono::steady_clock::now()+std::chrono::seconds(125);
    if(Code==ERROR_IO_PENDING)while(!Connected&&!S->cancelled_&&std::chrono::steady_clock::now()<Deadline) {
        if(WaitForSingleObject(Event.value_,50)==WAIT_OBJECT_0){DWORD Count=0;Connected=GetOverlappedResult(S->pipe_,&Ov,&Count,FALSE)!=FALSE;break;}
        if(WaitForSingleObject(Process.value_,0)==WAIT_OBJECT_0)break;
    }
    if(!Connected){CancelIoEx(S->pipe_,&Ov);DWORD Count=0;if(Code==ERROR_IO_PENDING)GetOverlappedResult(S->pipe_,&Ov,&Count,TRUE);
        DWORD ExitCode=STILL_ACTIVE;GetExitCodeProcess(Process.value_,&ExitCode);
        Complete(S,Failure(S->cancelled_?ERROR_CANCELLED:ExitCode!=STILL_ACTIVE?ExitCode:ERROR_TIMEOUT,"elevation-connect"));return;}
    ULONG Peer=0;if(!GetNamedPipeClientProcessId(S->pipe_,&Peer)||Peer!=GetProcessId(Process.value_)){Complete(S,Failure(ERROR_ACCESS_DENIED,"elevation-peer"));return;}
    DWORD Mode=PIPE_READMODE_MESSAGE;SetNamedPipeHandleState(S->pipe_,&Mode,nullptr,nullptr);
    std::shared_ptr<IReadStream> Input;bool NeedsData=false,Eof=false,CommitChecked=false;
    std::uint64_t ReadBytes=0;const auto ReadStarted=std::chrono::steady_clock::now();
    bool Unknown=false;const auto End=std::chrono::steady_clock::now()+std::chrono::minutes(15);
    while(std::chrono::steady_clock::now()<End) {
        const bool Cancel=S->cancelled_||S->context_.Check({}).has_value();
        const bool Paused=!S->commit_&&S->context_.pauseRequested_&&S->context_.pauseRequested_();
        QJsonObject Command{{"kind","control"},{"cancel",Cancel&&!S->commit_},{"pause",Paused},{"commit",S->commit_.load()}};
        const auto SourceCheck=[&]()->std::optional<Error>{
            auto C=S->context_;if(S->commit_){C.stopToken_={};C.deadline_=std::chrono::steady_clock::time_point::max();}
            auto Fresh=S->original_.sourceReader_->Stat(S->original_.source_->id_,C).get();
            if(!Fresh.HasValue())return Fresh.Failure();
            if(Fresh.Value().locator_!=S->original_.source_->locator_||Fresh.Value().id_.key_!=S->original_.source_->id_.key_||Fresh.Value().size_!=S->original_.source_->size_)return Failure(ERROR_FILE_INVALID,"elevation-provider-source-changed");return {};
        };
        if(NeedsData&&!Paused&&!Cancel&&!Eof) {
            std::optional<Error> Error;
            if(!Input) {Error=SourceCheck();if(!Error){auto ReadContext=S->context_;ReadContext.denyConcurrentMutation_=true;auto Opened=S->original_.sourceReader_->OpenRead(S->original_.source_->id_,ReadContext).get();if(Opened.HasValue())Input=Opened.Value();else Error=Opened.Failure();}}
            if(!Error) {
                auto Chunk=Input->Read(std::min<std::size_t>(S->context_.maxReadBytes_,1024*1024),S->context_).get();
                if(!Chunk.HasValue())Error=Chunk.Failure();else {
                    ReadBytes+=Chunk.Value().bytes_.size();Eof=Chunk.Value().eof_;
                    if(ReadBytes>S->original_.maxBytes_||ReadBytes>S->original_.source_->size_)Error=Failure(ERROR_FILE_TOO_LARGE,"elevation-provider-size");
                    else if(Eof&&(ReadBytes!=S->original_.source_->size_))Error=Failure(ERROR_FILE_INVALID,"elevation-provider-size");
                    else {if(Eof)Error=SourceCheck();if(!Error){Command["data"]=QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),qsizetype(Chunk.Value().bytes_.size())).toBase64());Command["eof"]=Eof;}}
                }
            }
            if(Error)Command["sourceError"]=EncodeError(*Error);
        }
        if(S->commit_&&S->request_["streamed"].toBool()&&!CommitChecked){CommitChecked=true;if(auto Error=SourceCheck()){Command["sourceError"]=EncodeError(*Error);Command["commit"]=false;}}
        if(!Send(S->pipe_,Command,S->nonce_))break;
        auto Message=Receive(S->pipe_,S->nonce_,30000);if(!Message)break;
        while((*Message)["kind"].toString()=="checkpoint") {
            S->checkpointSeen_=true;auto Record=Recovery((*Message)["record"].toObject());const auto Seq=(*Message)["sequence"].isString()?std::optional<std::uint64_t>((*Message)["sequence"].toString().toULongLong()):std::nullopt;
            Result<std::uint64_t> Saved=S->context_.writeCheckpoint_?S->context_.writeCheckpoint_(Record,Seq):Result<std::uint64_t>(Failure(ERROR_NOT_SUPPORTED,"elevation-journal-missing"));
            QJsonObject Reply{{"kind","checkpointAck"},{"ok",Saved.HasValue()}};if(Saved.HasValue())Reply["sequence"]=QString::number(Saved.Value());else Reply["error"]=EncodeError(Saved.Failure());
            if(!Send(S->pipe_,Reply,S->nonce_))break;Message=Receive(S->pipe_,S->nonce_,30000);if(!Message)break;
        }
        if(!Message||(*Message)["kind"].toString()!="status")break;
        NeedsData=(*Message)["needsData"].toBool();
        FileElevationStatus Status;Status.bytesDone_=(*Message)["bytes"].toString().toULongLong();Status.readyToCommit_=(*Message)["ready"].toBool();Status.finished_=(*Message)["finished"].toBool();
        Status.destinationCommitted_=(*Message)["published"].toBool();Status.sourceRemoved_=(*Message)["removed"].toBool();Status.detail_=String((*Message)["detail"]);
        if((*Message).contains("error"))Status.error_=DecodeError((*Message)["error"].toObject());
        for(const auto& E:(*Message)["cleanup"].toArray())Status.cleanupErrors_.push_back(DecodeError(E.toObject()));
        if((*Message).contains("destination")) {auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Out=Verify((*Message)["destination"].toObject(),Reader,{});if(Out.HasValue())Status.destination_=Out.Value();else if(!Status.error_)Status.error_=Out.Failure();}
        if(Status.finished_) {
            const auto Record=QJsonObject{{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"operation",QStringList{QObject::tr("복사"),QObject::tr("삭제"),QObject::tr("휴지통"),QObject::tr("이름 변경"),QObject::tr("폴더 생성"),QObject::tr("파일 생성"),QObject::tr("소유권·권한 변경"),QObject::tr("원래 권한 복원")}.value(S->request_["action"].toInt())},
                {"target",S->request_["destination"]},{"jobId",S->request_["jobId"]},{"code",Status.error_?1:0},{"error",Status.error_?Text(Status.error_->code_):QString()},{"detail",Text(Status.detail_)}};
            if(!AppendHistory(Record))Status.cleanupErrors_.push_back(Failure(ERROR_WRITE_FAULT,"permission-history-save-failed"));
            {std::lock_guard Lock(S->mutex_);S->status_=Status;S->done_=true;S->changed_.notify_all();}return;
        }
        {std::lock_guard Lock(S->mutex_);S->status_=Status;S->changed_.notify_all();}
        if(NeedsData&&S->original_.bytesPerSecondLimit_) {
            const auto Until=ReadStarted+std::chrono::milliseconds(std::int64_t((ReadBytes/S->original_.bytesPerSecondLimit_)*1000+(ReadBytes%S->original_.bytesPerSecondLimit_)*1000/S->original_.bytesPerSecondLimit_));
            while(std::chrono::steady_clock::now()<Until&&!S->cancelled_)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else if(!NeedsData)std::this_thread::sleep_for(std::chrono::milliseconds(Status.readyToCommit_||Paused?40:5));
    }
    Unknown=S->commit_;Complete(S,Failure(ERROR_TIMEOUT,Unknown?"elevation-outcome-unknown-inspect-before-retry":"elevation-transport"));
}
class Session final:public IFileElevationSession {
public:
    explicit Session(std::shared_ptr<SessionState> State):state_(std::move(State)){}
    Result<FileElevationStatus> Poll() override {std::unique_lock Lock(state_->mutex_);if(state_->commit_&&!state_->done_)state_->changed_.wait_for(Lock,std::chrono::milliseconds(50));return state_->status_;}
    Result<Unit> Commit() override {std::lock_guard Lock(state_->mutex_);if(!state_->status_.readyToCommit_||state_->status_.finished_)return Failure(ERROR_INVALID_STATE,"elevation-commit");state_->commit_=true;return Unit{};}
    void Cancel() override {state_->cancelled_=true;}
    std::vector<Error> Close() override {if(!state_->done_)Cancel();std::unique_lock Lock(state_->mutex_);state_->changed_.wait_for(Lock,std::chrono::seconds(2),[this]{return state_->done_.load();});if(!state_->done_&&state_->checkpointSeen_)return {Error{ErrorCategory::Unavailable,"elevation-cleanup-pending"}};return {};}
private:std::shared_ptr<SessionState> state_;
};
class IncomingCopy final:public IJobTask {
public:
    IncomingCopy(std::shared_ptr<IWritableStorage> Writer,Locator Destination,QJsonObject Request,QJsonObject& Input)
        :writer_(std::move(Writer)),destination_(std::move(Destination)),request_(std::move(Request)),input_(Input){}
    bool NeedsData()const{return phase_==1;}
    JobStep Advance(JobStepContext C)override {
        auto Fail=[](Error E){JobStep S{JobState::Failed};S.error_=std::move(E);return S;};
        JobStep S{JobState::Running};S.progress_=JobProgress{bytes_,request_["size"].toString().toULongLong(),destination_.root_};
        if(phase_==0) {
            DestinationExpectation Expected;Expected.exists_=request_["expectedExists"].toBool();if(Expected.exists_)Expected.revision_=ResourceRevision{String(request_["expectedRevision"])};
            C.operation_.expectedWriteBytes_=request_["size"].toString().toULongLong();CopyOptions Options;Options.metadata_=request_["metadata"].toBool();Options.verify_=request_["verify"].toBool();
            auto Open=writer_->BeginCopy(destination_,Expected,Options,C.operation_);if(!Open.HasValue())return Fail(Open.Failure());session_=std::move(Open.Value());phase_=1;
        } else if(phase_==1&&input_.contains("data")) {
            auto Data=QByteArray::fromBase64(input_["data"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
            if(Data.size()>1024*1024||bytes_+Data.size()>request_["size"].toString().toULongLong()||bytes_+Data.size()>request_["bytesLimit"].toString().toULongLong())return Fail(Failure(ERROR_FILE_TOO_LARGE,"elevation-incoming-budget"));
            const auto Span=std::span(reinterpret_cast<const std::byte*>(Data.constData()),std::size_t(Data.size()));
            if(!Span.empty()){auto Wrote=session_->Append(Span,C.operation_);if(!Wrote.HasValue())return Fail(Wrote.Failure());if(request_["verify"].toBool()){auto Hash=session_->RecordSourceBytes(Span,C.operation_);if(!Hash.HasValue())return Fail(Hash.Failure());}}
            bytes_+=Data.size();if(input_["eof"].toBool()){if(bytes_!=request_["size"].toString().toULongLong())return Fail(Failure(ERROR_FILE_INVALID,"elevation-incoming-size"));phase_=2;}
        } else if(phase_==2) {
            if(request_["metadata"].toBool()){auto Set=session_->SetMetadata({request_["modified"].toString().toLongLong(),std::uint32_t(request_["attributes"].toInt())},C.operation_);if(!Set.HasValue())return Fail(Set.Failure());}
            auto Extras=session_->AdvanceCopyExtras(0,C.operation_);if(!Extras.HasValue())return Fail(Extras.Failure());
            phase_=3;
        } else if(phase_==3) {
            if(request_["verify"].toBool()){auto Checked=session_->VerifyContent(C.operation_);if(!Checked.HasValue())return Fail(Checked.Failure());if(!Checked.Value())return S;}
            phase_=4;S.next_=JobState::Committing;
        } else if(phase_==4&&C.committing_) {
            auto Done=session_->Commit(C.operation_);if(!Done.HasValue())return Fail(Done.Failure());published_=Done.Value().destinationCommitted_;cleanup_=Done.Value().cleanupErrors_;
            S.next_=JobState::Completed;S.item_=ItemOutcome{destination_.root_,ItemOutcome::Status::Completed,{},published_};
        }
        S.progress_->bytesDone_=bytes_;return S;
    }
    std::vector<Error> Finalize(JobState)override {if(session_&&!published_){auto Abort=session_->Abort();if(!Abort.HasValue())cleanup_.push_back(Abort.Failure());}return cleanup_;}
private:
    std::shared_ptr<IWritableStorage> writer_;Locator destination_;QJsonObject request_;QJsonObject& input_;
    std::unique_ptr<IWriteSession> session_;std::uint64_t bytes_=0;int phase_=0;bool published_=false;std::vector<Error> cleanup_;
};
class Service final:public IFileElevationService {
public:
    explicit Service(FileElevationLauncher Launcher):launcher_(std::move(Launcher)){}
    bool CanExecute(const FileElevationRequest& R) const override {
        if (R.action_ == FileElevationAction::Metadata && (!R.source_ || !R.metadataPatch_.Valid()
            || R.metadataPatch_.Empty() || R.destination_ != R.source_->locator_ || !R.source_->id_.revision_)) return false;
        if(R.jobId_.empty()||R.destination_.kind_!=LocatorKind::Local||!R.destination_.nesting_.empty()||!Local(Text(R.destination_.root_)))return false;
        if(R.source_&&(R.source_->locator_.kind_!=LocatorKind::Local||!Local(Text(R.source_->locator_.root_)))) {
            // Provider credentials and handles stay in the main process. Only
            // captured bytes enter the authenticated, bounded local writer pipe.
            if(R.action_!=FileElevationAction::Copy||R.source_->directory_||!R.sourceReader_||R.copyOptions_.acl_||R.copyOptions_.streams_)return false;
        }
        if(R.requiresPublishedDestination_&&!R.publishedDestination_)return false;
        return R.content_.size()<8192;
    }
    Result<std::unique_ptr<IFileElevationSession>> Begin(FileElevationRequest R,OperationContext Context) override {
        if(!CanExecute(R)||(!launcher_&&!QFileInfo::exists(HelperPath())))return Failure(ERROR_NOT_SUPPORTED,"elevation-unavailable");
        auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);
        QJsonObject Object{{"version",2},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"jobId",Text(R.jobId_)},{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"pid",int(GetCurrentProcessId())},{"created",ProcessCreated(GetCurrentProcess())},{"action",int(R.action_)},{"destination",Path(Text(R.destination_.root_))},
            {"expectedExists",R.expected_.exists_},{"expectedRevision",R.expected_.revision_?Text(R.expected_.revision_->token_):QString()},
            {"writeCheckpoints",bool(Context.writeCheckpoint_)},{"readOnly",R.allowReadOnlyRemoval_},{"flushFileContents",Context.flushFileContents_},{"requirePublished",R.requiresPublishedDestination_},{"bytesLimit",QString::number(R.maxBytes_)},{"rate",QString::number(R.bytesPerSecondLimit_)},
            {"metadata",R.copyOptions_.metadata_},{"verify",R.copyOptions_.verify_},{"acl",R.copyOptions_.acl_},{"streams",R.copyOptions_.streams_},
            {"content",QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(R.content_.data()),qsizetype(R.content_.size())).toBase64())}};
        const bool Streamed=R.source_&&(R.source_->locator_.kind_!=LocatorKind::Local||!Local(Text(R.source_->locator_.root_)));
        Object["streamed"]=Streamed;
        if (R.action_ == FileElevationAction::Metadata) {
            Object["setMask"] = int(R.metadataPatch_.setMask_); Object["clearMask"] = int(R.metadataPatch_.clearMask_);
            if (R.metadataPatch_.modifiedUtcMs_) Object["mtime"] = QString::number(*R.metadataPatch_.modifiedUtcMs_);
            Object["source"] = Snapshot(*R.source_);
            Object["sourceAttributes"] = int(R.source_->attributes_);
            // A protected entry may only be readable by the directory helper.
            // Require its captured file-ID revision; the elevated process verifies
            // it again and pins parents before opening the mutation handle.
            const auto& Revision = R.source_->id_.revision_->token_;
            if (!Revision.starts_with("directory:") && Revision.find(":identity:") == std::string::npos)
                return Failure(ERROR_FILE_INVALID, "metadata-identity-required");
        }
        if(Streamed) {Object["size"]=QString::number(R.source_->size_);Object["modified"]=QString::number(R.source_->modifiedUtcMs_);Object["attributes"]=int(R.source_->attributes_);}
        if(R.source_&&!Streamed && R.action_ != FileElevationAction::Metadata) {
            // Stat verifies the caller's revision and upgrades cheap listings to
            // a file-ID revision before the user sees UAC.
            auto Fresh=Writer->Inspect(R.source_->locator_,Context);if(!Fresh.HasValue())return Fresh.Failure();if(!Fresh.Value())return Failure(ERROR_FILE_NOT_FOUND,"elevation-source");
            const auto Native=filetools::NativePath(Text(R.source_->locator_.root_));filetools::Handle H(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            BY_HANDLE_FILE_INFORMATION Info{};if(H.value_==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(H.value_,&Info))return Failure(GetLastError(),"elevation-source");
            if((!R.source_->id_.revision_ || !native::MatchesRevision(*R.source_->id_.revision_,Info)))return Failure(ERROR_FILE_INVALID,"elevation-source-changed");
            Object["source"]=Snapshot(*Fresh.Value());Object["sourceIdentity"]=QString::fromLatin1(filetools::Identity(H.value_));
        }
        const auto BackupFolder=QDir(settings::Paths().directory_).filePath("security-backups");
        if(R.action_==FileElevationAction::SecurityChange||R.action_==FileElevationAction::SecurityRestore) {
            if(!R.source_||!QDir().mkpath(BackupFolder))return Failure(ERROR_WRITE_FAULT,"security-backup-folder");
            Object["callerSid"]=Sid();Object["backupFolder"]=BackupFolder;Object["grant"]=R.grantAccess_;Object["backupPath"]=Text(R.backupPath_);Object["backupIdentity"]=Text(R.backupIdentity_);Object["securityDigest"]=Text(R.securityDigest_);
        }
        if(R.publishedDestination_)Object["published"]=Snapshot(*R.publishedDestination_);
        QJsonArray Parents;for(const auto& P: R.action_ == FileElevationAction::Metadata ? QStringList{} : QStringList{QFileInfo(Path(Text(R.destination_.root_))).absolutePath(),R.source_&&!Streamed?QFileInfo(Path(Text(R.source_->locator_.root_))).absolutePath():QString(),Object["backupFolder"].toString()}) {
            if(P.isEmpty())continue;const auto Identity=filetools::Inspect(P).identity_;if(Identity.isEmpty())return Failure(ERROR_FILE_INVALID,"elevation-parent");Parents.append(QJsonObject{{"path",P},{"identity",QString::fromLatin1(Identity)}});
        }
        Object["parents"]=Parents;auto State=std::make_shared<SessionState>();State->context_=Context;State->original_=R;State->launcher_=launcher_;State->nonce_=Object["id"].toString();State->helper_=QDir::toNativeSeparators(HelperPath());
        const auto PipeName="\\\\.\\pipe\\HyperCommander-"+State->nonce_;Object["pipe"]=PipeName;
        const auto Owner=Sid();if(Owner.isEmpty())return Failure(ERROR_ACCESS_DENIED,"elevation-caller");
        PSECURITY_DESCRIPTOR Descriptor=nullptr;const auto Sddl="D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+Owner+")";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()),SDDL_REVISION_1,&Descriptor,nullptr))return Failure(GetLastError(),"elevation-pipe-security");
        SECURITY_ATTRIBUTES Attributes{sizeof(Attributes),Descriptor,FALSE};
        State->pipe_=CreateNamedPipeW(reinterpret_cast<LPCWSTR>(PipeName.utf16()),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,frameLimit,frameLimit,5000,&Attributes);LocalFree(Descriptor);
        if(State->pipe_==INVALID_HANDLE_VALUE)return Failure(GetLastError(),"elevation-pipe");State->request_=Object;
        // ShellExecute can wait on the secure desktop. A shared detached worker
        // owns the pipe; a late approval sees a cancelled/disconnected request.
        std::thread([State]{try {Run(State);}catch(...){Complete(State,Failure(ERROR_GEN_FAILURE,"elevation-worker"));}}).detach();
        return std::unique_ptr<IFileElevationSession>(new Session(State));
    }
private:FileElevationLauncher launcher_;
};
} // namespace
std::shared_ptr<IFileElevationService> MakeFileElevationService(FileElevationLauncher Launcher){return std::make_shared<Service>(std::move(Launcher));}
DWORD ExecuteFileElevation(const QJsonObject& R) {
    const auto Nonce=R["id"].toString(),PipeName=R["pipe"].toString();const auto Time=QDateTime::fromString(R["utc"].toString(),Qt::ISODateWithMs);
    const auto Age=Time.msecsTo(QDateTime::currentDateTimeUtc());
    if(R["version"].toInt()!=2||QUuid(Nonce).isNull()||R["jobId"].toString().isEmpty()||!Time.isValid()
        ||PipeName!="\\\\.\\pipe\\HyperCommander-"+Nonce||!Local(R["destination"].toString()))return ERROR_INVALID_PARAMETER;
    if(Age< -5000||Age>120000)return ERROR_TIMEOUT;
    const int ActionValue=R["action"].toInt(-1);if(ActionValue<0||ActionValue>int(FileElevationAction::Metadata))return ERROR_NOT_SUPPORTED;
    const auto Action=FileElevationAction(ActionValue);filetools::Handle Parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,DWORD(R["pid"].toInt())));
    if(!Parent.value_||ProcessCreated(Parent.value_)!=R["created"].toString())return ERROR_INVALID_OWNER;
    if((Action==FileElevationAction::SecurityChange||Action==FileElevationAction::SecurityRestore)&&R["callerSid"].toString()!=Sid(Parent.value_))return ERROR_INVALID_OWNER;
    if(!WaitNamedPipeW(reinterpret_cast<LPCWSTR>(PipeName.utf16()),5000)&&GetLastError()!=ERROR_SEM_TIMEOUT)return GetLastError();
    filetools::Handle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(PipeName.utf16()),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if(Pipe.value_==INVALID_HANDLE_VALUE)return GetLastError();ULONG Peer=0;if(!GetNamedPipeServerProcessId(Pipe.value_,&Peer)||Peer!=DWORD(R["pid"].toInt()))return ERROR_ACCESS_DENIED;
    DWORD Mode=PIPE_READMODE_MESSAGE;SetNamedPipeHandleState(Pipe.value_,&Mode,nullptr,nullptr);
    FileElevationStatus Status;std::vector<std::unique_ptr<filetools::Handle>> Pins;DWORD PinError=0;
    for(const auto& Value:R["parents"].toArray()) {
        const auto P=Value.toObject();if(!Local(P["path"].toString())){PinError=ERROR_INVALID_PARAMETER;break;}
        const auto PinFailure=filetools::PinNativeParents(P["path"].toString(),Pins);if(!PinFailure.isEmpty()){PinError=ERROR_FILE_INVALID;break;}
        if(filetools::Inspect(P["path"].toString()).identity_!=P["identity"].toString().toLatin1()){PinError=ERROR_FILE_INVALID;break;}
    }
    QSet<QString> Required{QFileInfo(R["destination"].toString()).absolutePath().toCaseFolded()};
    if(R.contains("source"))Required.insert(QFileInfo(R["source"].toObject()["path"].toString()).absolutePath().toCaseFolded());
    if(Action==FileElevationAction::SecurityChange||Action==FileElevationAction::SecurityRestore)Required.insert(R["backupFolder"].toString().toCaseFolded());
    for(const auto& P:R["parents"].toArray())Required.remove(P.toObject()["path"].toString().toCaseFolded());
    MetadataPatch Patch;
    if (Action == FileElevationAction::Metadata) {
        bool Parsed = true;
        Patch.setMask_ = std::uint32_t(R["setMask"].toInt(-1)); Patch.clearMask_ = std::uint32_t(R["clearMask"].toInt(-1));
        if (R.contains("mtime")) Patch.modifiedUtcMs_ = R["mtime"].toString().toLongLong(&Parsed);
        const auto SourceObject = R["source"].toObject(); const auto Revision = SourceObject["revision"].toString();
        if (!Parsed || !Patch.Valid() || Patch.Empty() || !Local(SourceObject["path"].toString())
            || Path(SourceObject["path"].toString()) != Path(R["destination"].toString())
            || (!Revision.startsWith("directory:") && !Revision.contains(":identity:"))) return ERROR_INVALID_PARAMETER;
        if (!filetools::PinNativeParents(QFileInfo(SourceObject["path"].toString()).absolutePath(), Pins).isEmpty()) PinError = ERROR_FILE_INVALID;
        Required.clear(); // Leaf file-ID is verified below; parents remain pinned.
    }
    if(!Required.isEmpty())PinError=ERROR_INVALID_PARAMETER;
    auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);OperationContext Context;
    Context.allowReadOnlyRemoval_=R["readOnly"].toBool();Context.flushFileContents_=R["flushFileContents"].toBool(true);std::stop_source Stop;Context.stopToken_=Stop.get_token();
    // Follow the caller persistence policy; absent fields retain legacy protocol behavior.
    if(R["writeCheckpoints"].toBool(true))Context.writeCheckpoint_=[&](const WriteRecoveryRecord& Record,std::optional<std::uint64_t> Sequence)->Result<std::uint64_t>{
        QJsonObject Frame{{"kind","checkpoint"},{"record",Recovery(Record)}};if(Sequence)Frame["sequence"]=QString::number(*Sequence);
        if(!Send(Pipe.value_,Frame,Nonce))return Failure(ERROR_BROKEN_PIPE,"elevation-checkpoint");auto Reply=Receive(Pipe.value_,Nonce,30000);
        if(!Reply||(*Reply)["kind"]!="checkpointAck")return Failure(ERROR_BROKEN_PIPE,"elevation-checkpoint");
        if(!(*Reply)["ok"].toBool())return DecodeError((*Reply)["error"].toObject());return (*Reply)["sequence"].toString().toULongLong();
    };
    std::optional<EntrySnapshot> Source;std::unique_ptr<IJobTask> Task;QJsonObject Input;IncomingCopy* Incoming=nullptr;
    if(PinError)Status.error_=Failure(PinError,"elevation-parent");
    if(!Status.error_&&R.contains("source")){auto Checked=Verify(R["source"].toObject(),Reader,Context);if(Checked.HasValue())Source=Checked.Value();else Status.error_=Checked.Failure();}
    if (!Status.error_ && Action == FileElevationAction::Metadata && (!Source || Source->attributes_ != std::uint32_t(R["sourceAttributes"].toInt())))
        Status.error_ = Failure(ERROR_FILE_INVALID, "metadata-attributes-changed");
    const Locator Destination{LocatorKind::Local,String(R["destination"])};
    if(!Status.error_&&Action==FileElevationAction::Copy&&R["streamed"].toBool()) {
        if(R.contains("source")||R["acl"].toBool()||R["streams"].toBool())Status.error_=Failure(ERROR_NOT_SUPPORTED,"elevation-provider-extras");
        else {auto Copy=std::make_unique<IncomingCopy>(Writer,Destination,R,Input);Incoming=Copy.get();Task=std::move(Copy);}
    } else if(!Status.error_&&Action==FileElevationAction::Copy) {
        if(!Source||Source->directory_)Status.error_=Failure(ERROR_NOT_SUPPORTED,"elevation-copy");else {
            FileOperationRequest Request;Request.sources_={{*Source,Reader,{}}};Request.destination_={LocatorKind::Local,QFileInfo(Text(Destination.root_)).absolutePath().toStdString()};
            Request.destinationName_=QFileInfo(Text(Destination.root_)).fileName().toStdString();Request.destinationWriter_=Writer;Request.conflict_=ConflictPolicy::Replace;
            DestinationExpectation Expected;Expected.exists_=R["expectedExists"].toBool();if(Expected.exists_)Expected.revision_=ResourceRevision{String(R["expectedRevision"])};Request.approvedDestination_=Expected;
            Request.maxBytes_=R["bytesLimit"].toString().toULongLong();Request.bytesPerSecondLimit_=R["rate"].toString().toULongLong();
            Request.copyOptions_={R["metadata"].toBool(),R["verify"].toBool(),R["acl"].toBool(),R["streams"].toBool()};
            auto Prepared=PrepareFileOperation(Request,Context);if(Prepared.HasValue())Task=std::move(Prepared.Value().task_);else Status.error_=Prepared.Failure();
        }
    } else if(!Status.error_&&Action==FileElevationAction::CreateContent) {
        const auto Data=QByteArray::fromBase64(R["content"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);
        if(Data.size()>8192)Status.error_=Failure(ERROR_INVALID_PARAMETER,"elevation-content");else Task=std::make_unique<filetools::CreateTask>(Writer,Destination,Data);
    } else if(!Status.error_&&(Action==FileElevationAction::Remove||Action==FileElevationAction::Recycle||Action==FileElevationAction::Rename||Action==FileElevationAction::SecurityChange||Action==FileElevationAction::SecurityRestore)&&!Source)Status.error_=Failure(ERROR_INVALID_PARAMETER,"elevation-source-required");
    auto NextAdvance=std::chrono::steady_clock::time_point::min();
    bool Gate=false;JobState Final=JobState::Failed;const auto End=std::chrono::steady_clock::now()+std::chrono::minutes(15);
    while(std::chrono::steady_clock::now()<End&&WaitForSingleObject(Parent.value_,0)!=WAIT_OBJECT_0) {
        auto Command=Receive(Pipe.value_,Nonce,30000);if(!Command||(*Command)["kind"]!="control")break;
        Input=*Command;
        if(Input.contains("sourceError")){Status.error_=DecodeError(Input["sourceError"].toObject());Final=JobState::Failed;}
        if(Status.error_)Status.finished_=true;
        if((*Command)["cancel"].toBool()&&!Gate){Stop.request_stop();Status.error_=Failure(ERROR_CANCELLED,"elevation-cancelled");Status.finished_=true;Final=JobState::Cancelled;}
        if(!Status.finished_&&!(*Command)["pause"].toBool()&&std::chrono::steady_clock::now()>=NextAdvance) {
            if(Status.readyToCommit_&&(*Command)["commit"].toBool()){Gate=true;Context.stopToken_={};}
            if(!Status.readyToCommit_||Gate) {
                if(Task) {
                    auto Step=Task->Advance({String(R["jobId"]),Context,{},0,Gate});NextAdvance=std::chrono::steady_clock::now()+Step.deferFor_;if(Step.progress_)Status.bytesDone_=Step.progress_->bytesDone_;
                    if(Step.item_){Status.destinationCommitted_|=Step.item_->destinationCommitted_;Status.sourceRemoved_|=Step.item_->sourceRemoved_;}
                    if(Step.next_==JobState::Committing)Status.readyToCommit_=true;
                    if(IsTerminal(Step.next_)){Status.finished_=true;Status.error_=Step.error_;Final=Step.next_;}
                    if(Step.next_==JobState::WaitingForUser){Status.finished_=true;Status.error_=Failure(ERROR_FILE_INVALID,"elevation-unapproved-interaction");}
                } else if(!Gate)Status.readyToCommit_=true;
                else {
                    std::unique_ptr<filetools::Handle> PublishedLock;
                    if(R["requirePublished"].toBool()) {
                        auto Checked=Verify(R["published"].toObject(),Reader,Context);if(!Checked.HasValue())Status.error_=Checked.Failure();
                        else {
                            const auto P=filetools::NativePath(R["published"].toObject()["path"].toString());
                            PublishedLock=std::make_unique<filetools::Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(P.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
                            BY_HANDLE_FILE_INFORMATION Info{};
                            if(PublishedLock->value_==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(PublishedLock->value_,&Info))Status.error_=Failure(GetLastError(),"elevation-published-lock");
                            else if(!Checked.Value().id_.revision_||!native::MatchesRevision(*Checked.Value().id_.revision_,Info))Status.error_=Failure(ERROR_FILE_INVALID,"elevation-published-changed");
                        }
                    }
                    if(!Status.error_&&Source){auto Checked=Verify(R["source"].toObject(),Reader,Context);if(!Checked.HasValue())Status.error_=Checked.Failure();}
                    if(!Status.error_) {
                        if(Action==FileElevationAction::Metadata){auto Done=Writer->PatchMetadata(*Source,Patch,Context);if(!Done.HasValue())Status.error_=Done.Failure();else Status.destinationCommitted_=true;}
                        else if(Action==FileElevationAction::MakeDirectory){auto Done=Writer->MakeDirectory(Destination,Context);if(!Done.HasValue())Status.error_=Done.Failure();else Status.destinationCommitted_=true;}
                        else if(Action==FileElevationAction::Rename){auto Done=Writer->Rename(*Source,Destination,Context);if(!Done.HasValue())Status.error_=Done.Failure();else Status.destinationCommitted_=Status.sourceRemoved_=true;}
                        else if(Action==FileElevationAction::Recycle){auto Done=Writer->Recycle(*Source,Context);if(!Done.HasValue())Status.error_=Done.Failure();else {Status.sourceRemoved_=true;Status.cleanupErrors_=Done.Value().cleanupErrors_;}}
                        else if(Action==FileElevationAction::SecurityChange||Action==FileElevationAction::SecurityRestore){
                            const auto Result=security::Execute(Text(Source->locator_.root_),R["sourceIdentity"].toString(),R["callerSid"].toString(),R["backupFolder"].toString(),R["grant"].toBool(),Action==FileElevationAction::SecurityRestore,R["backupPath"].toString(),R["backupIdentity"].toString(),R["securityDigest"].toString(),R["jobId"].toString());
                            Status.detail_=QJsonDocument(Result.backup_).toJson(QJsonDocument::Compact).toStdString();
                            if(Result.code_)Status.error_=Failure(Result.code_,"security-change");
                            if(Result.statusCode_)Status.cleanupErrors_.push_back(Failure(Result.statusCode_,"security-status-save-failed"));
                            if(Result.rollbackCode_)Status.cleanupErrors_.push_back(Failure(Result.rollbackCode_,"security-rollback-failed"));
                            Status.destinationCommitted_=!Result.code_;
                        }
                        else {auto Done=Writer->Remove(*Source,Context);if(!Done.HasValue())Status.error_=Done.Failure();else Status.sourceRemoved_=true;}
                    }
                    Status.finished_=true;Final=Status.error_?JobState::Failed:JobState::Completed;
                }
            }
        }
        if(Status.finished_) {
            if(Task){Status.cleanupErrors_=Task->Finalize(Final);Task.reset();}
            if(Status.destinationCommitted_ && (Action==FileElevationAction::Copy || Action==FileElevationAction::Rename || Action==FileElevationAction::CreateContent || Action==FileElevationAction::MakeDirectory)){auto Output=Writer->Inspect(Destination,{});if(Output.HasValue()&&Output.Value())Status.destination_=*Output.Value();}
        }
        auto Frame=EncodeStatus(Status);Frame["needsData"]=Incoming&&!Status.finished_&&Incoming->NeedsData();
        if(!Send(Pipe.value_,Frame,Nonce))break;if(Status.finished_)return Status.error_?ERROR_GEN_FAILURE:ERROR_SUCCESS;
    }
    Stop.request_stop();if(Task)Task->Finalize(JobState::Cancelled);return ERROR_TIMEOUT;
}
namespace {
QJsonObject ListingEntry(const EntrySnapshot& E)
{
    QJsonObject Hints; for (const auto& [Key, Value] : E.hints_) Hints[Text(Key)] = Text(Value);
    return {{"hints", Hints}, {"name", Text(E.name_)}, {"path", Text(E.locator_.root_)}, {"provider", Text(E.id_.providerId_.value_)},
        {"key", Text(E.id_.key_)}, {"scope", int(E.id_.scope_)}, {"revision", E.id_.revision_ ? Text(E.id_.revision_->token_) : QString()},
        {"directory", E.directory_}, {"size", QString::number(E.size_)}, {"attributes", double(E.attributes_)},
        {"created", QString::number(E.createdUtcMs_)}, {"modified", QString::number(E.modifiedUtcMs_)}};
}
class DirectorySnapshotCursor final : public IEntryCursor {
public:
    explicit DirectorySnapshotCursor(std::vector<EntrySnapshot> Entries) : entries_(std::move(Entries)) {}
    Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override {
        if (const auto Error = Context.Check({"local"})) return Ready<EntryPage>(*Error);
        if (!MaxItems) return Ready<EntryPage>(Failure(ERROR_INVALID_PARAMETER, "directory-page-size"));
        EntryPage Page;
        const auto Count = std::min(MaxItems, entries_.size() - offset_);
        for (std::size_t Index = 0; Index < Count; ++Index) Page.entries_.push_back(std::move(entries_[offset_++]));
        Page.end_ = offset_ == entries_.size(); return Ready<EntryPage>(std::move(Page));
    }
private:
    std::vector<EntrySnapshot> entries_;
    std::size_t offset_ = 0;
};
bool InDirectoryScope(const QString& Scope, const QString& Candidate)
{
    const auto At = Path(Candidate); auto Prefix = Scope; if (!Prefix.endsWith('/')) Prefix += '/';
    return !At.contains(QChar::Null) && (At.compare(Scope, Qt::CaseInsensitive) == 0 || At.startsWith(Prefix, Qt::CaseInsensitive));
}
class ElevatedDirectorySession final : public DirectoryElevationSession {
public:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    QString nonce_, path_, root_;
    std::shared_ptr<IStorageProvider> reader_;
    bool ended_ = false;
    std::timed_mutex mutex_;
    std::atomic<bool> broken_ = false;
    ~ElevatedDirectorySession() override {
        if (pipe_ != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(pipe_); CloseHandle(pipe_); }
        if (process_) { WaitForSingleObject(process_, 1000); CloseHandle(process_); }
    }
    bool Contains(const QString& At) const override { return !broken_ && InDirectoryScope(root_, At); }
    Result<std::unique_ptr<IEntryCursor>> Enumerate(const QString& At, OperationContext Context, std::shared_ptr<IStorageProvider> Reader) override {
        if (const auto Error = Context.Check({"local"})) return *Error;
        if (!Contains(At)) return Failure(ERROR_ACCESS_DENIED, "directory-outside-approved-scope");
        std::unique_lock Lock(mutex_, std::defer_lock);
        while (!Lock.try_lock_for(std::chrono::milliseconds(50))) if (const auto Error = Context.Check({"local"})) return *Error;
        path_ = Path(At); reader_ = Reader ? std::move(Reader) : MakeNativeReadProvider(LocatorKind::Local); ended_ = false;
        if (!Send(pipe_, {{"kind", "open"}, {"path", path_}}, nonce_)) { broken_ = true; return Failure(ERROR_BROKEN_PIPE, "directory-open"); }
        const auto Ready = Receive(pipe_, nonce_);
        if (!Ready || (*Ready)["kind"] != "ready") { broken_ = true; return Failure(ERROR_BROKEN_PIPE, "directory-open-reply"); }
        std::vector<EntrySnapshot> Entries;
        while (!ended_) {
            auto Page = next(128, Context); if (!Page.HasValue()) return Page.Failure();
            if (Entries.size() + Page.Value().entries_.size() > Context.EnumerationLimit()) return Failure(ERROR_BUFFER_OVERFLOW, "directory-entry-limit");
            for (auto& Entry : Page.Value().entries_) Entries.push_back(std::move(Entry));
        }
        return std::unique_ptr<IEntryCursor>(new DirectorySnapshotCursor(std::move(Entries)));
    }
private:
    Result<EntryPage> next(std::size_t MaxItems, OperationContext Context) {
        if (const auto E = Context.Check({"local"})) return *E;
        if (!MaxItems) return Failure(ERROR_INVALID_PARAMETER, "directory-page-size");
        if (ended_) return EntryPage{{}, {}, true};
        const auto Limit = int(std::min<std::size_t>(MaxItems, 128));
        if (!Send(pipe_, {{"kind", "next"}, {"limit", Limit}}, nonce_)) { broken_ = true; return Failure(ERROR_BROKEN_PIPE, "directory-send"); }
        const auto Reply = Receive(pipe_, nonce_, 30000);
        if (const auto E = Context.Check({"local"})) return *E;
        if (!Reply) { broken_ = true; return Failure(ERROR_BROKEN_PIPE, "directory-receive"); }
        if (Reply->contains("error")) { ended_ = true; return DecodeError((*Reply)["error"].toObject()); }
        if ((*Reply)["kind"] != "page" || !(*Reply)["entries"].isArray() || !(*Reply)["end"].isBool())
            return Failure(ERROR_INVALID_DATA, "directory-protocol");
        const auto Rows = (*Reply)["entries"].toArray();
        if (Rows.size() > Limit || (Rows.isEmpty() && !(*Reply)["end"].toBool())) return Failure(ERROR_INVALID_DATA, "directory-page");
        EntryPage Page; Page.end_ = (*Reply)["end"].toBool();
        for (const auto& Value : Rows) {
            const auto Row = Value.toObject(); const auto Name = Row["name"].toString();
            if (Name.isEmpty() || Name == "." || Name == ".." || Name.contains('/') || Name.contains('\\') || Name.contains(':')
                || Path(Row["path"].toString()).compare(QDir(path_).filePath(Name), Qt::CaseInsensitive)
                || Row["provider"] != "local" || Row["key"].toString().isEmpty()) return Failure(ERROR_INVALID_DATA, "directory-entry");
            EntrySnapshot Entry;
            Entry.name_ = Name.toStdString(); Entry.locator_ = {LocatorKind::Local, String(Row["path"])};
            Entry.id_ = {{"local"}, String(Row["key"]), IdentityScope(Row["scope"].toInt()), {}};
            if (!Row["revision"].toString().isEmpty()) Entry.id_.revision_ = ResourceRevision{String(Row["revision"])};
            Entry.directory_ = Row["directory"].toBool(); Entry.size_ = Row["size"].toString().toULongLong();
            Entry.attributes_ = std::uint32_t(Row["attributes"].toDouble());
            Entry.createdUtcMs_ = Row["created"].toString().toLongLong(); Entry.modifiedUtcMs_ = Row["modified"].toString().toLongLong();
            const auto Hints = Row["hints"].toObject();
            for (auto It = Hints.begin(); It != Hints.end(); ++It) Entry.hints_[It.key().toStdString()] = It.value().toString().toStdString();
            // Resource IDs are session-local. Rebind metadata to the caller's
            // normal provider; file reads and mutations do not inherit elevation.
            auto Bound = BindNativeSnapshot(*reader_, std::move(Entry));
            if (!Bound.HasValue()) return Bound.Failure();
            Page.entries_.push_back(std::move(Bound.Value()));
        }
        ended_ = Page.end_;
        return Page;
    }
};
}

Result<std::shared_ptr<DirectoryElevationSession>> OpenElevatedDirectorySession(const QString& RequestedPath, OperationContext Context, FileElevationLauncher Launcher)
{
    if (const auto E = Context.Check({"local"})) return *E;
    const auto Target = Path(RequestedPath);
    if (!Local(Target) || Target.contains(QChar::Null)) return Failure(ERROR_NOT_SUPPORTED, "directory-local-only");
    auto Cursor = std::make_shared<ElevatedDirectorySession>(); Cursor->root_ = Target;
    Cursor->nonce_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto PipeName = "\\\\.\\pipe\\HyperCommander-" + Cursor->nonce_;
    const auto Owner = Sid(); if (Owner.isEmpty()) return Failure(ERROR_ACCESS_DENIED, "directory-owner");
    const auto Sddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + Owner + ")";
    PSECURITY_DESCRIPTOR Descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()), SDDL_REVISION_1, &Descriptor, nullptr))
        return Failure(GetLastError(), "directory-pipe-security");
    SECURITY_ATTRIBUTES Attributes{sizeof(Attributes), Descriptor, FALSE};
    Cursor->pipe_ = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(PipeName.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, frameLimit, frameLimit, 5000, &Attributes);
    LocalFree(Descriptor);
    if (Cursor->pipe_ == INVALID_HANDLE_VALUE) return Failure(GetLastError(), "directory-pipe");
    const QJsonObject Request{{"version", 1}, {"id", Cursor->nonce_}, {"pipe", PipeName}, {"path", Target}, {"callerSid", Owner},
        {"pid", int(GetCurrentProcessId())}, {"created", ProcessCreated(GetCurrentProcess())}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    const auto Payload = QJsonDocument(Request).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (Payload.size() > 24000) return Failure(ERROR_BUFFER_OVERFLOW, "directory-payload");
    const auto Arguments = "--elevated-list " + QString::fromLatin1(Payload);
    const auto Helper = QDir::toNativeSeparators(HelperPath());
    if (Launcher) {
        auto Launched = Launcher(Helper, Arguments); if (!Launched.HasValue()) return Launched.Failure(); Cursor->process_ = Launched.Value();
    } else {
        const auto Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW Info{}; Info.cbSize = sizeof(Info); Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        Info.lpVerb = L"runas"; Info.lpFile = reinterpret_cast<LPCWSTR>(Helper.utf16());
        Info.lpParameters = reinterpret_cast<LPCWSTR>(Arguments.utf16()); Info.nShow = SW_HIDE;
        const auto Started = ShellExecuteExW(&Info); const auto Code = GetLastError();
        if (SUCCEEDED(Com)) CoUninitialize();
        if (!Started) return Failure(Code, "directory-uac"); Cursor->process_ = Info.hProcess;
    }
    if (!Cursor->process_) return Failure(ERROR_INVALID_HANDLE, "directory-helper");
    filetools::Handle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED Ov{}; Ov.hEvent = Event.value_;
    bool Connected = ConnectNamedPipe(Cursor->pipe_, &Ov) != FALSE; const auto Code = Connected ? ERROR_SUCCESS : GetLastError();
    if (Code == ERROR_PIPE_CONNECTED) Connected = true;
    const auto Deadline = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(125));
    if (Code == ERROR_IO_PENDING) while (!Connected && !Context.Check({"local"}) && std::chrono::steady_clock::now() < Deadline) {
        if (WaitForSingleObject(Event.value_, 50) == WAIT_OBJECT_0) { DWORD Count = 0; Connected = GetOverlappedResult(Cursor->pipe_, &Ov, &Count, FALSE) != FALSE; break; }
        if (WaitForSingleObject(Cursor->process_, 0) == WAIT_OBJECT_0) break;
    }
    if (!Connected) {
        CancelIoEx(Cursor->pipe_, &Ov); DWORD Count = 0;
        if (Code == ERROR_IO_PENDING) GetOverlappedResult(Cursor->pipe_, &Ov, &Count, TRUE);
        if (const auto E = Context.Check({"local"})) return *E;
        DWORD ExitCode = STILL_ACTIVE; GetExitCodeProcess(Cursor->process_, &ExitCode);
        return Failure(ExitCode == STILL_ACTIVE ? ERROR_TIMEOUT : ExitCode, "directory-connect");
    }
    ULONG Peer = 0;
    if (!GetNamedPipeClientProcessId(Cursor->pipe_, &Peer) || Peer != GetProcessId(Cursor->process_)) return Failure(ERROR_ACCESS_DENIED, "directory-peer");
    return std::shared_ptr<DirectoryElevationSession>(std::move(Cursor));
}

Result<std::unique_ptr<IEntryCursor>> EnumerateElevatedDirectory(const QString& Target, OperationContext Context, FileElevationLauncher Launcher, std::shared_ptr<IStorageProvider> Reader)
{
    auto Session = OpenElevatedDirectorySession(Target, Context, std::move(Launcher));
    if (!Session.HasValue()) return Session.Failure();
    return Session.Value()->Enumerate(Target, Context, std::move(Reader));
}

DWORD ExecuteDirectoryElevation(const QJsonObject& R)
{
    const auto Nonce = R["id"].toString(), PipeName = R["pipe"].toString(), Target = R["path"].toString();
    const auto Time = QDateTime::fromString(R["utc"].toString(), Qt::ISODateWithMs);
    const auto Age = Time.msecsTo(QDateTime::currentDateTimeUtc());
    if (R["version"].toInt() != 1 || QUuid(Nonce).isNull() || !Time.isValid() || !Local(Target) || Target.contains(QChar::Null)
        || PipeName != "\\\\.\\pipe\\HyperCommander-" + Nonce) return ERROR_INVALID_PARAMETER;
    if (Age < -5000 || Age > 120000) return ERROR_TIMEOUT;
    filetools::Handle Parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(R["pid"].toInt())));
    if (!Parent.value_ || ProcessCreated(Parent.value_) != R["created"].toString() || Sid(Parent.value_) != R["callerSid"].toString()) return ERROR_INVALID_OWNER;
    if (!WaitNamedPipeW(reinterpret_cast<LPCWSTR>(PipeName.utf16()), 5000) && GetLastError() != ERROR_SEM_TIMEOUT) return GetLastError();
    filetools::Handle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(PipeName.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (Pipe.value_ == INVALID_HANDLE_VALUE) return GetLastError(); ULONG Peer = 0;
    if (!GetNamedPipeServerProcessId(Pipe.value_, &Peer) || Peer != DWORD(R["pid"].toInt())) return ERROR_ACCESS_DENIED;
    DWORD Mode = PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(Pipe.value_, &Mode, nullptr, nullptr);
    OperationContext Context; Context.freshEnumerationMetadata_ = true; Context.allowUnavailableChildMetadata_ = true;
    std::stop_source Stop; Context.stopToken_ = Stop.get_token();
    // The pipe is the tab's lease: disconnect or parent death ends the session,
    // including an in-flight native enumeration. Idle browsing has no timeout.
    std::jthread Monitor([&](std::stop_token Token) {
        while (!Token.stop_requested()) {
            if (WaitForSingleObject(Parent.value_, 50) == WAIT_OBJECT_0 || !PeekNamedPipe(Pipe.value_, nullptr, 0, nullptr, nullptr, nullptr)) {
                Stop.request_stop(); break;
            }
        }
    });
    auto Reader = MakeNativeReadProvider(LocatorKind::Local);
    std::unique_ptr<IEntryCursor> Cursor;
    std::vector<std::unique_ptr<filetools::Handle>> ScopePins, Pins;
    QString Current;
    while (!Stop.stop_requested()) {
        DWORD Available = 0;
        if (!PeekNamedPipe(Pipe.value_, nullptr, 0, nullptr, &Available, nullptr)) break;
        if (!Available) { Sleep(50); continue; }
        const auto Command = Receive(Pipe.value_, Nonce, 5000);
        if (!Command) break;
        if ((*Command)["kind"] == "open") {
            const auto Requested = (*Command)["path"].toString();
            if (!InDirectoryScope(Path(Target), Requested)) return ERROR_ACCESS_DENIED;
            Current = Path(Requested); Cursor.reset(); Pins.clear();
            Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            if (!Send(Pipe.value_, {{"kind", "ready"}}, Nonce)) break;
            continue;
        }
        if ((*Command)["kind"] != "next" || Current.isEmpty()) return ERROR_INVALID_PARAMETER;
        const auto Limit = (*Command)["limit"].toInt(); if (Limit < 1 || Limit > 128) return ERROR_INVALID_PARAMETER;
        const auto Report = [&](const Error& Error) {
            Cursor.reset(); Pins.clear(); Current.clear();
            return Send(Pipe.value_, {{"error", EncodeError(Error)}}, Nonce);
        };
        if (!Cursor) {
            const auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(filetools::NativePath(Current).utf16()));
            if (Attributes == INVALID_FILE_ATTRIBUTES) {
                const auto Code = GetLastError();
                auto Error = Failure(Code, "win32");
                if (Code == ERROR_FILE_NOT_FOUND || Code == ERROR_PATH_NOT_FOUND) Error.category_ = ErrorCategory::NotFound;
                if (!Report(Error)) break; continue;
            }
            if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (!Report(Failure(ERROR_NOT_SUPPORTED, "directory-reparse"))) break; continue;
            }
            // Keep the approved root stable and reject junction escapes from
            // that subtree. Child directory pins last only for one listing.
            const auto ScopeError = ScopePins.empty() ? filetools::PinNativeParents(Target, ScopePins) : QString();
            if (!ScopeError.isEmpty()) ScopePins.clear();
            const auto PinError = ScopeError.isEmpty() ? filetools::PinNativeParents(Current, Pins) : ScopeError;
            if (!PinError.isEmpty()) {
                if (!Report(Failure(ERROR_ACCESS_DENIED, "directory-parents"))) break; continue;
            }
            auto Open = Reader->Enumerate({LocatorKind::Local, Current.toStdString()}, Context).get();
            if (!Open.HasValue()) { if (!Report(Open.Failure())) break; continue; }
            Cursor = std::move(Open.Value());
        }
        auto Page = Cursor->Next(std::size_t(Limit), Context).get();
        if (!Page.HasValue()) { if (!Report(Page.Failure())) break; continue; }
        QJsonArray Rows; for (const auto& Entry : Page.Value().entries_) Rows.append(ListingEntry(Entry));
        if (!Send(Pipe.value_, {{"kind", "page"}, {"entries", Rows}, {"end", Page.Value().end_}}, Nonce)) return ERROR_BROKEN_PIPE;
        if (Page.Value().end_) { Cursor.reset(); Pins.clear(); Current.clear(); }
    }
    return ERROR_SUCCESS;
}
} // namespace hc::permissions
