#include "hostedViewerSession.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include "UniqueLibs/nativeStorageRevision.hpp"
#include "documentViewerClient.hpp"
#include "imageArchiveBroker.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/document/imagePerformance.hpp"
#include "UniqueLibs/document/archiveDraftDirectory.hpp"
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonDocument>
#include <QDateTime>
#include <QStringDecoder>
#include "wlxDetection.hpp"
#include "pluginApplicationRule.hpp"
#include <qt_windows.h>
#include "externalLibs/TotalCommander/tc_listplug.h"
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <thread>
#include <future>
#include <cstring>

namespace hc::plugins
{
    using namespace storage;
    using namespace viewer;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
        QJsonObject ReadingSource(const EntrySnapshot& Entry){
            if((Entry.locator_.kind_==LocatorKind::Local || Entry.locator_.kind_==LocatorKind::Smb))return {{"path",QString::fromUtf8(Entry.locator_.root_)}};
            if(Entry.locator_.kind_==LocatorKind::Archive&&Entry.locator_.nesting_.size()==1)return {{"path",QString::fromUtf8(Entry.locator_.root_)},{"entry",QString::fromUtf8(Entry.locator_.nesting_.front().entry_)}};
            return {};
        }
        bool IsDirectBook(const EntrySnapshot& Entry)
        {
            return (Entry.locator_.kind_==LocatorKind::Local || Entry.locator_.kind_==LocatorKind::Smb) && !Entry.directory_
                && QStringList{"zip","cbz","rar","cbr","7z","cb7"}.contains(QFileInfo(QString::fromUtf8(Entry.locator_.root_)).suffix().toLower());
        }
        bool IsDirectNative(const EntrySnapshot& Entry)
        {
            if((Entry.locator_.kind_!=LocatorKind::Local && Entry.locator_.kind_!=LocatorKind::Smb) || Entry.directory_ || (Entry.attributes_&FILE_ATTRIBUTE_REPARSE_POINT))return false;
            const auto Path=QString::fromUtf8(Entry.locator_.root_);
            // All native inputs, including SMB and redirected RDP, share the verified direct-read path.
            return QFileInfo(Path).isAbsolute();
        }
        // Only the approved built-in reader may borrow the original archive.
        // Keep a read-only handle without write/delete sharing for the complete
        // viewer lifetime; neither a copy nor a full archive hash is needed.
        class SourceLease final : public IMaterializedFileLease
        {
        public:
            SourceLease(EntrySnapshot Entry,HANDLE Pin):entry_(std::move(Entry)),pin_(Pin){}
            ~SourceLease()override{CloseHandle(pin_);}
            std::string LocalPath()const override{return entry_.locator_.root_;}
            EntrySnapshot Describe()const override{return entry_;}
            std::size_t Depth()const override{return 0;}
        private:
            EntrySnapshot entry_;HANDLE pin_;
        };
        Result<std::shared_ptr<IMaterializedFileLease>> BorrowBook(const std::shared_ptr<IContentSource>& Source,OperationContext Context)
        {
            const auto Entry=Source->Describe();const auto Path=QString::fromUtf8(Entry.locator_.root_);
            if(!IsDirectBook(Entry)||!QFileInfo(Path).isAbsolute())return Invalid("viewer-book-path");
            const auto Pin=CreateFileW(reinterpret_cast<LPCWSTR>(hc::storage::native::FilePath(Path).utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(Pin==INVALID_HANDLE_VALUE)return Error{ErrorCategory::Unavailable,"viewer-book-pin:"+std::to_string(GetLastError())};
            std::shared_ptr<IMaterializedFileLease> Lease=std::make_shared<SourceLease>(Entry,Pin);
            auto Verified=Source->Verify(Context).get();if(!Verified.HasValue())return Verified.Failure();
            return Lease;
        }
        Result<std::shared_ptr<IMaterializedFileLease>> BorrowNative(const std::shared_ptr<IContentSource>& Source,OperationContext Context)
        {
            hc::document::ImageTraceSpan Trace("viewer.direct-native",QString::fromUtf8(Source->Describe().locator_.root_));
            const auto Entry=Source->Describe();if(!IsDirectNative(Entry))return Invalid("viewer-native-path");
            const auto Path=QString::fromUtf8(Entry.locator_.root_);
            // The built-in image reader owns a verified byte snapshot after load.
            // Preserve external edits/renames, like a direct HyperEditor open;
            // ReadDocumentFile checks changes during reading and saves check SHA-256.
            const auto Pin=CreateFileW(reinterpret_cast<LPCWSTR>(hc::storage::native::FilePath(Path).utf16()),GENERIC_READ,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(Pin==INVALID_HANDLE_VALUE)return Error{ErrorCategory::Unavailable,"viewer-native-open:"+std::to_string(GetLastError())};
            std::shared_ptr<IMaterializedFileLease> Lease=std::make_shared<SourceLease>(Entry,Pin);
            const auto Verified=Source->Verify(Context).get();if(!Verified.HasValue())return Verified.Failure();
            return Lease;
        }
        Result<QByteArray> ExtensionBytes(const EntrySnapshot& Entry)
        {
            QStringDecoder Decoder(QStringDecoder::Utf8); const QString Name = Decoder(Entry.name_);
            if (Decoder.hasError() || Name.contains(QChar::Null)) return Invalid("wlx-detect-name-encoding");
            const auto Text = QFileInfo(Name).suffix().toStdWString();
            if (Text.empty()) return QByteArray{};
            if (Text.size() > 32767) return Invalid("wlx-detect-extension");
            const bool Utf8 = GetACP() == CP_UTF8; BOOL Substituted = FALSE;
            const auto Flags = Utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
            const auto Size = WideCharToMultiByte(CP_ACP, Flags, Text.c_str(), -1, nullptr, 0, nullptr, Utf8 ? nullptr : &Substituted);
            if (!Size || Substituted) return Error{ErrorCategory::Unsupported, "wlx-detect-extension-encoding", {"wlx"}};
            QByteArray Bytes(Size, '\0'); Substituted = FALSE;
            if (!WideCharToMultiByte(CP_ACP, Flags, Text.c_str(), -1, Bytes.data(), Size, nullptr, Utf8 ? nullptr : &Substituted) || Substituted)
                return Error{ErrorCategory::Unsupported, "wlx-detect-extension-encoding", {"wlx"}};
            Bytes.chop(1); return Bytes;
        }
        Result<QByteArray> ReadDetectionPrefix(IContentSource& Source, const EntrySnapshot& Entry, OperationContext Context, std::size_t Maximum = 8192)
        {
            const auto Required = static_cast<std::size_t>(std::min<std::uint64_t>(Entry.size_, Maximum));
            if (const auto* Provider = dynamic_cast<ProviderContentSource*>(&Source)) {
                const auto Cached = Provider->Prefix();
                if (Cached.size() >= Required) {
                    auto Verified = Source.Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
                    return QByteArray(reinterpret_cast<const char*>(Cached.data()), qsizetype(Required));
                }
            }
            if (Required > Context.maxReadBytes_) return Error{ErrorCategory::BudgetExceeded, "wlx-detect-read-budget", Entry.id_.providerId_};
            QByteArray Bytes; std::shared_ptr<IReadStream> PrefixStream;
            if (Required)
            {
                auto Opened = Source.OpenRead(Context).get(); if (!Opened.HasValue()) return Opened.Failure();
                if (!Opened.Value()) return Invalid("wlx-detect-stream-required");
                PrefixStream = Opened.Value();
                while (static_cast<std::size_t>(Bytes.size()) < Required)
                {
                    if (const auto Stop = Context.Check(Entry.id_.providerId_)) return *Stop;
                    const auto Remaining = Required - static_cast<std::size_t>(Bytes.size());
                    auto Chunk = Opened.Value()->Read(Remaining, Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                    if (Chunk.Value().bytes_.size() > Remaining) return Invalid("wlx-detect-read-overflow");
                    if (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_) return Invalid("wlx-detect-read-no-progress");
                    Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), static_cast<qsizetype>(Chunk.Value().bytes_.size()));
                    if (Chunk.Value().eof_ && static_cast<std::size_t>(Bytes.size()) < Required)
                        return Error{ErrorCategory::StaleRevision, "wlx-detect-premature-eof", Entry.id_.providerId_, Entry.id_};
                }
            }
            auto Verified = Source.Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
            if (auto* Provider = dynamic_cast<ProviderContentSource*>(&Source)) {
                const auto* Data = reinterpret_cast<const std::byte*>(Bytes.constData());
                Provider->RememberPrefix({Data, Data + Bytes.size()}, PrefixStream);
            }
            return Bytes;
        }
        QJsonObject ReadPrefix(const std::shared_ptr<IContentSource>& Source)
        {
            const auto Provider = std::dynamic_pointer_cast<ProviderContentSource>(Source);
            if (!Provider || !IsDirectNative(Provider->Describe())) return {};
            const auto Bytes = Provider->Prefix(); if (Bytes.empty()) return {};
            return {{"sourceId", QString::fromStdString(Serialize(Provider->Describe().id_))},
                {"sourceLocator", QString::fromStdString(Serialize(Provider->Describe().locator_))},
                {"bytes", QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(Bytes.data()), qsizetype(Bytes.size())).toBase64())}};
        }
        Result<QByteArray> DraftHash(const QString& Path)
        {
            // Refuse inspection while any other handle can still write the draft.
            const auto Pin = CreateFileW(reinterpret_cast<LPCWSTR>(native::FilePath(Path).utf16()), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (Pin == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "archive-draft-busy"};
            QFile File(Path); QCryptographicHash Hash(QCryptographicHash::Sha256);
            const bool Read = File.open(QIODevice::ReadOnly) && Hash.addData(&File);
            File.close(); CloseHandle(Pin);
            if (!Read) return Error{ErrorCategory::Unavailable, "archive-draft-read"};
            return Hash.result();
        }
    }
    struct HostedViewerSession::State
    {
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::stop_source stop_;
        PluginViewerDetails details_;
        std::function<HostSnapshot()> observation_;
        std::function<void()> forceStop_;
        bool closed_ = false, cancelled_ = false, commands_ = false, search_ = false, focus_ = false;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> size_;
        std::optional<QJsonObject> inputBinding_;
        struct Command { std::uint64_t sequence_; int command_, parameter_; QString text_; QJsonObject document_; std::vector<HostedViewerSession::DocumentInput> inputs_; };
        std::deque<Command> queue_;
        std::uint64_t nextCommand_ = 1;
        void stop(bool Close)
        {
            { std::lock_guard Lock(mutex_); closed_ |= Close; cancelled_ |= !Close; }
            stop_.request_stop(); changed_.notify_all();
        }
    };
    struct PluginViewerService::Task
    {
        std::shared_ptr<HostedViewerSession::State> state_;
        std::jthread worker_;
    };
    HostedViewerSession::HostedViewerSession(std::shared_ptr<State> State) : state_(std::move(State)) {}
    HostedViewerSession::~HostedViewerSession() { Close(); }
    ViewerSnapshot HostedViewerSession::Snapshot() const { std::lock_guard Lock(state_->mutex_); return state_->details_.viewer_; }
    PluginViewerDetails HostedViewerSession::Details() const
    {
        PluginViewerDetails Value; std::function<HostSnapshot()> Observation;
        { std::lock_guard Lock(state_->mutex_); Value = state_->details_; Observation = state_->observation_; }
        if (Observation) { auto Host = Observation(); if (Host.processId_) Value.host_ = std::move(Host); }
        const auto Runtime = Value.host_.runtime_;
        Value.uiSuspect_ = Runtime["probeFailures"].toInt() > 0;
        Value.waitingResponsive_ = (Runtime["workState"] == "Running" || Runtime["workState"] == "Queued")
            && Runtime["workAgeMs"].toInteger() >= 2000 && !Value.uiSuspect_;
        Value.resultUnknown_ = Runtime["workState"] == "Lost" || (Value.commandError_ && Value.commandError_->userAction_ == "result-unknown");
        return Value;
    }
    std::vector<std::string> HostedViewerSession::Commands() const
    {
        std::lock_guard Lock(state_->mutex_); auto Commands = state_->commands_ ? std::vector<std::string>{"copy", "wrap", "select-all", "set-percent"} : std::vector<std::string>{};
        if (state_->search_) Commands.push_back("find"); return Commands;
    }
    void HostedViewerSession::Cancel() { state_->stop(false); }
    void HostedViewerSession::Close() { state_->stop(true); }
    void HostedViewerSession::ForceStop()
    {
        std::function<void()> Stop;
        { std::lock_guard Lock(state_->mutex_); Stop = state_->forceStop_; }
        if (Stop) Stop();
    }
    Result<std::uint64_t> HostedViewerSession::CloseArchive(int Decision)
    {
        std::lock_guard Lock(state_->mutex_);
        if (Decision < 0 || Decision > 2 || !state_->details_.externalArchive_ || state_->stop_.stop_requested()
            || state_->queue_.size() >= 16) return Invalid("archive-close-unavailable");
        const auto Sequence = state_->nextCommand_++;
        state_->queue_.push_back({Sequence, 8, Decision}); state_->changed_.notify_one(); return Sequence;
    }
    Result<Unit> HostedViewerSession::Resize(std::uint32_t Width, std::uint32_t Height)
    {
        if (Width > 32768 || Height > 32768) return Invalid("host-viewer-size");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->size_ = {Width, Height}; state_->changed_.notify_one(); return Unit{};
    }
    Result<Unit> HostedViewerSession::Focus()
    {
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->focus_ = true; state_->changed_.notify_one(); return Unit{};
    }
    Result<Unit> HostedViewerSession::SetReturnKey(unsigned Key, unsigned Modifiers, unsigned ReturnMessage, unsigned InteractionMessage)
    {
        if (Key > 255 || Modifiers > 15 || ReturnMessage < 0xc000 || ReturnMessage > 0xffff
            || InteractionMessage < 0xc000 || InteractionMessage > 0xffff) return Invalid("host-viewer-input-binding");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested()) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->inputBinding_ = QJsonObject{{"configureOnly", true}, {"returnKey", int(Key)}, {"returnModifiers", int(Modifiers)},
            {"returnMessage", int(ReturnMessage)}, {"interactionMessage", int(InteractionMessage)}};
        state_->changed_.notify_one(); return Unit{};
    }
    Result<std::uint64_t> HostedViewerSession::SendCommand(int Command, int Parameter)
    {
        if (Command < 1 || Command > 4 || Parameter < 0 || Parameter > (Command == 4 ? 100 : 511)) return Invalid("host-viewer-command-fields");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready) return Error{ErrorCategory::Unavailable, "viewer-not-ready", {"wlx"}};
        if (!state_->commands_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"wlx"}};
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "viewer-command-budget", {"wlx"}};
        const auto Sequence = state_->nextCommand_++; state_->queue_.push_back({Sequence, Command, Parameter});
        state_->changed_.notify_one(); return Sequence;
    }
    Result<std::uint64_t> HostedViewerSession::Search(QString Text, int Flags)
    {
        if (Text.isEmpty() || Text.size() > 4096 || Text.contains(QChar::Null) || Flags < 0 || Flags > 15) return Invalid("host-viewer-search-fields");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready) return Error{ErrorCategory::Unavailable, "viewer-not-ready", {"wlx"}};
        if (!state_->search_) return Error{ErrorCategory::Unsupported, "host-viewer-search-unavailable", {"wlx"}};
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "viewer-command-budget", {"wlx"}};
        const auto Sequence = state_->nextCommand_++; state_->queue_.push_back({Sequence, 5, Flags, std::move(Text)}); state_->changed_.notify_one(); return Sequence;
    }
    Result<std::uint64_t> HostedViewerSession::SendDocument(QJsonObject Request)
    {
        std::lock_guard Lock(state_->mutex_);
        if (!state_->details_.documentChannel_ || state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready)
            return Error{ErrorCategory::Unavailable, "document-channel-not-ready"};
        if (Request.isEmpty() || QJsonDocument(Request).toJson(QJsonDocument::Compact).size() > document::HostPayloadLimit / 2 || state_->queue_.size() >= 16)
            return Error{ErrorCategory::BudgetExceeded, "document-command-budget"};
        const auto Sequence = state_->nextCommand_++;
        state_->queue_.push_back({Sequence, 6, 0, {}, std::move(Request)}); state_->changed_.notify_one(); return Sequence;
    }
    Result<std::uint64_t> HostedViewerSession::OpenDocuments(std::vector<DocumentInput> Inputs, bool ReplacePrepared)
    {
        std::lock_guard Lock(state_->mutex_);
        if (!state_->details_.documentChannel_ || state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready)
            return Error{ErrorCategory::Unavailable, "document-channel-not-ready"};
        if (Inputs.empty() || state_->queue_.size() >= 16
            || std::any_of(Inputs.begin(), Inputs.end(), [](const auto& Item) { return !Item.source_; }))
            return Invalid("document-open-inputs");
        const auto Sequence = state_->nextCommand_++;
        state_->queue_.push_back({Sequence, 7, ReplacePrepared ? 1 : 0, {}, {}, std::move(Inputs)}); state_->changed_.notify_one(); return Sequence;
    }
    PluginViewerService::PluginViewerService(MaterializerFactory Factory, std::size_t MaxSessions)
        : factory_(std::move(Factory)), maxSessions_(MaxSessions) {}
    PluginViewerService::~PluginViewerService() { CancelAll(); tasks_.clear(); }
    Result<std::shared_ptr<HostedViewerSession>> PluginViewerService::Prepare(PluginViewerOptions Options,
        QString ParentWindow, std::uint32_t Width, std::uint32_t Height, OperationContext Context)
    { return open({std::move(Options)}, {}, {}, std::move(ParentWindow), Width, Height, Context, true); }
    Result<std::shared_ptr<HostedViewerSession>> PluginViewerService::Open(PluginViewerOptions Options,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, std::uint32_t Width, std::uint32_t Height, OperationContext Context)
    { return open({std::move(Options)}, {}, std::move(Source), std::move(ParentWindow), Width, Height, Context); }
    Result<std::shared_ptr<HostedViewerSession>> PluginViewerService::OpenAutomatic(std::vector<PluginViewerOptions> Candidates,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, std::uint32_t Width, std::uint32_t Height,
        PluginViewerSelectionPolicy Policy, OperationContext Context)
    { return open(std::move(Candidates), Policy, std::move(Source), std::move(ParentWindow), Width, Height, Context); }
    Result<std::shared_ptr<HostedViewerSession>> PluginViewerService::open(std::vector<PluginViewerOptions> Candidates,
        std::optional<PluginViewerSelectionPolicy> Policy, std::shared_ptr<IContentSource> Source, QString ParentWindow,
        std::uint32_t Width, std::uint32_t Height, OperationContext Context, bool Preparing)
    {
        if (!factory_ || (!Source && !Preparing) || ParentWindow.isEmpty() || Width > 32768 || Height > 32768 || Candidates.size() > 1000
            || std::any_of(Candidates.begin(), Candidates.end(), [](const auto& Item) { return Item.flags_ < 0 || Item.flags_ > 511; }))
            return Invalid("invalid-wlx-session-request");
        if (!maxSessions_ || maxSessions_ > 16) return Invalid("invalid-wlx-session-budget");
        const bool BuiltinViewer=Candidates.size()==1&&Candidates.front().approval_.reviewed_.id_=="builtin.hyperviewer"
            &&Candidates.front().approval_.reviewed_.compatibility_=="hyperviewer-v1";
        if (Preparing && (!BuiltinViewer || Source || Policy)) return Invalid("viewer-prepare-builtin-required");
        Collect(); if (!BuiltinViewer&&tasks_.size() >= maxSessions_) return Error{ErrorCategory::BudgetExceeded, "viewer-session-budget", {"wlx"}};
        auto State = std::make_shared<HostedViewerSession::State>(); State->size_ = {Width, Height};
        State->details_.viewer_.id_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        State->details_.viewer_.provider_ = Policy ? "plugin-viewer.automatic"
            : std::string(Candidates.front().approval_.reviewed_.format_ == PluginFormat::DopusViewer ? "dopus-viewer." : "wlx.") + Candidates.front().approval_.reviewed_.id_;
        auto Session = std::shared_ptr<HostedViewerSession>(new HostedViewerSession(State));
        auto Task = std::make_unique<PluginViewerService::Task>(); Task->state_ = State;
        // Retain the task before starting it: allocation failure while appending
        // must not join an already-running viewer that has never been cancelled.
        tasks_.push_back(std::move(Task));
        try
        {
            tasks_.back()->worker_ = std::jthread([State, Candidates = std::move(Candidates), Policy, Source = std::move(Source), ParentWindow = std::move(ParentWindow), Factory = factory_, Context]() mutable {
                run(State, std::move(Candidates), Policy, std::move(Source), std::move(ParentWindow), std::move(Factory), Context);
                std::lock_guard Lock(State->mutex_); State->details_.idle_ = true;
            });
        }
        catch (const std::system_error&) { tasks_.pop_back(); return Error{ErrorCategory::Unavailable, "viewer-worker-unavailable", {"wlx"}}; }
        return Session;
    }
    void PluginViewerService::CancelAll() { for (auto& Task : tasks_) Task->state_->stop(false); }
    bool PluginViewerService::IsIdle() const
    {
        for (const auto& Task : tasks_) { std::lock_guard Lock(Task->state_->mutex_); if (!Task->state_->details_.idle_) return false; }
        return true;
    }
    void PluginViewerService::Collect()
    {
        std::erase_if(tasks_, [](const auto& Task) { std::lock_guard Lock(Task->state_->mutex_); return Task->state_->details_.idle_; });
    }
    void PluginViewerService::run(const std::shared_ptr<HostedViewerSession::State>& State, std::vector<PluginViewerOptions> Candidates,
        std::optional<PluginViewerSelectionPolicy> Policy,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, MaterializerFactory Factory, OperationContext Context)
    {
        std::stop_callback External(Context.stopToken_, [&] { State->stop(false); });
        Context.stopToken_ = State->stop_.get_token();
        const auto UiContext = Context;
        const auto PreparationStage=[State](const QString& Stage){std::lock_guard Lock(State->mutex_);State->details_.preparationStage_=Stage;State->details_.preparationProgress_.reset();};
        // Progress belongs to one input preparation, never to renderer startup.
        const auto PreparationContext = [State](OperationContext Calls, const EntrySnapshot& Entry) {
            Calls.progress_ = [State, Entry, Observer = Calls.progress_](std::uint64_t Done) {
                { std::lock_guard Lock(State->mutex_);
                  State->details_.preparationProgress_ = OperationContext::Progress{Done, 0, 0, Entry.size_, Entry.name_}; }
                if (Observer) Observer(Done);
            };
            Calls.reportProgress_ = [State, Observer = Calls.reportProgress_](const OperationContext::Progress& Progress) {
                { std::lock_guard Lock(State->mutex_); State->details_.preparationProgress_ = Progress; }
                if (Observer) Observer(Progress);
            };
            return Calls;
        };
        const auto ClearPreparation = [State] {
            std::lock_guard Lock(State->mutex_); State->details_.preparationProgress_.reset();
        };
        std::shared_ptr<IMaterializer> Materializer;
        std::shared_ptr<IMaterializedFileLease> Lease;
        std::unique_ptr<ImageArchiveBroker> ArchiveBroker;
        std::unique_ptr<PluginProcess> Host;
        std::unique_ptr<DocumentViewerClient> Document;
        bool NoMatch = false, ArchiveIdle = false;
        bool DirectInputAllowed=false;
        bool DirectOriginal = false;
        std::shared_ptr<hc::document::ArchiveDraftDirectory> ExternalDraft;
        QString ExternalPath;
        QByteArray ExternalBaseline;
        ProviderContentSource::ArchiveWriteback ExternalWriteback;
        const auto CloseHost = [&] {
            if (!Host) { ArchiveBroker.reset(); return; }
            Host->Close(); ArchiveBroker.reset(); Document.reset(); const auto Final = Host->Snapshot();
            { std::lock_guard Lock(State->mutex_); State->details_.host_ = Final; State->observation_ = {}; }
            Host.reset();
        };
        const auto Work = [&]() -> Result<Unit> {
            if (const auto Stop = Context.Check({"wlx"})) return *Stop;
            if (Policy)
            {
                Context.deadline_ = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(30));
                Context.denyConcurrentMutation_ = true;
            }
            const auto UiCall = [&](auto&& Function) {
                const auto Started = std::chrono::steady_clock::now();
                auto Value = Function(UiContext);
                if constexpr (!std::is_same_v<decltype(Value), Result<std::unique_ptr<PluginProcess>>>) {
                    if (!Value.HasValue() && Value.Failure().code_ == "host-caller-detached" && Host && !State->stop_.stop_requested()) {
                        // The session owns the input lease after this caller's
                        // absolute wait ended. Rejoin its terminal slot before
                        // retiring a failed open; never kill a responsive SDK
                        // merely because the opening subscriber timed out.
                        OperationContext Wait; Wait.stopToken_ = State->stop_.get_token();
                        auto Terminal = Host->AwaitPending(Wait);
                        Value = Terminal.HasValue() ? decltype(Value)(Error{ErrorCategory::Unavailable, "host-caller-wait-expired", {"plugin-host"}, {}, false, "completed-after-detach"})
                            : decltype(Value)(Terminal.Failure());
                    }
                }
                // The automatic-selection budget measures input preparation.
                // UI calls receive the original absolute caller deadline; a
                // responsive modal does not consume the preparation allowance.
                if (Policy && Context.deadline_ != std::chrono::steady_clock::time_point::max())
                    Context.deadline_ = std::min(UiContext.deadline_, Context.deadline_ + (std::chrono::steady_clock::now() - Started));
                return Value;
            };
            std::optional<EntrySnapshot> Entry;
            WlxDetectionInput Detection; bool PrefixReady = false, ExtensionReady = false, FatalContent = false;
            const auto Materialize = [&]() -> Result<Unit> {
                hc::document::ImageTraceSpan Trace("viewer.prepare-input",QString::fromUtf8(Source->Describe().locator_.root_));
                if (Lease) return Unit{};
                PreparationStage("materialize");
                if (DirectOriginal) {
                    auto Verified = Source->Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
                    const auto Original = Source->Describe();
                    const auto Path = QString::fromUtf8(Original.locator_.root_);
                    // Keep identity/lifetime without denying the plugin's save or atomic replacement.
                    // This path is restricted to explicitly approved local native inputs below.
                    const auto Pin = CreateFileW(reinterpret_cast<LPCWSTR>(native::FilePath(Path).utf16()), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (Pin == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "viewer-original-open:" + std::to_string(GetLastError())};
                    auto Borrowed = std::make_shared<SourceLease>(Original, Pin);
                    BY_HANDLE_FILE_INFORMATION Info{};
                    if (!GetFileInformationByHandle(Pin, &Info) || !Original.id_.revision_
                        || (Info.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT))
                        || !native::MatchesRevision(*Original.id_.revision_, Info, Pin))
                        return Error{ErrorCategory::StaleRevision, "viewer-original-changed"};
                    Lease = std::move(Borrowed);
                    return Unit{};
                }
                if(DirectInputAllowed && IsDirectBook(Source->Describe())){
                    auto Borrowed=BorrowBook(Source,Context);if(!Borrowed.HasValue())return Borrowed.Failure();
                    Lease=std::move(Borrowed.Value());
                    if(ArchiveBroker)ArchiveBroker->Allow(QString::fromStdString(Lease->LocalPath()));
                    return Unit{};
                }
                if(DirectInputAllowed && IsDirectNative(Source->Describe())){
                    auto Borrowed=BorrowNative(Source,Context);if(!Borrowed.HasValue())return Borrowed.Failure();
                    Lease=std::move(Borrowed.Value());return Unit{};
                }
                auto Created = Factory(); if (!Created.HasValue()) return Created.Failure(); Materializer = std::move(Created.Value());
                if (!Materializer) return Invalid("viewer-materializer-required");
                MaterializationRequest Input{Source, {}, {}};
                QByteArray PrivatePrefix = Detection.prefix_;
                if (!PrefixReady) {
                    auto Prefix = ReadDetectionPrefix(*Source, Source->Describe(), PreparationContext(Context, Source->Describe()), std::min<std::size_t>(20, Context.maxReadBytes_));
                    if (!Prefix.HasValue()) return Prefix.Failure(); PrivatePrefix = std::move(Prefix.Value());
                }
                // Readonly WAL connections may leave an empty WAL and a shared
                // memory index. Declare these disposable caches only for our
                // generated private copy, never for a borrowed source path.
                // A nonempty WAL or larger/unknown output is preserved by the
                // lease owner, rather than discarding a possible transaction.
                if (PrivatePrefix.size() >= 20 && std::memcmp(PrivatePrefix.constData(), "SQLite format 3\0", 16) == 0
                    && PrivatePrefix[18] == 2 && PrivatePrefix[19] == 2)
                    Input.auxiliaries_ = {{"-wal", 0}, {"-shm", 32768}};
                auto Materialized = MaterializePrivateContent(*Materializer, std::move(Input), PreparationContext(Context, Source->Describe())).get();
                ClearPreparation();
                if (!Materialized.HasValue()) return Materialized.Failure(); Lease = std::move(Materialized.Value());
                if (!Lease) return Invalid("viewer-content-lease-required");
                if(ArchiveBroker)ArchiveBroker->Allow(QString::fromStdString(Lease->LocalPath()));
                return Unit{};
            };
            const auto Attempt = [&](PluginViewerOptions& Options, std::size_t Index) -> Result<bool> {
                hc::document::ImageTraceSpan Trace("viewer.open");
                if (const auto Stop = Context.Check({"wlx"})) return *Stop;
                if (!Options.approval_.approved_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wlx"}};
                auto Inspected = InspectManifestFile(Options.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
                const bool Dopus = Inspected.Value().manifest_.format_ == PluginFormat::DopusViewer;
                if ((!Dopus && Inspected.Value().manifest_.format_ != PluginFormat::Wlx) || !Inspected.Value().manifest_.extensions_.contains(Extension::Viewer) || Options.approval_.reviewed_ != Inspected.Value().manifest_)
                    return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wlx"}};
                if (Options.binaryPath_.isEmpty() || QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
                    return Error{ErrorCategory::AccessDenied, "plugin-configured-binary-mismatch", {"wlx"}};
                HostProfile Profile; Profile.formats_ = {PluginFormat::Wlx, PluginFormat::DopusViewer}; Profile.extensions_ = {Extension::Viewer, Extension::Metadata, Extension::ContentConvert}; Profile.supportsIsolation_ = true;
                auto Admission = Admit(Inspected.Value().manifest_, Options.approval_, Profile); if (!Admission.HasValue()) return Admission.Failure();
                if (!Source) {
                    // Preparation owns a process/window/channel, never an input lease or document.
                    // Keep the same inspected manifest and admission checks as a real open.
                    if (!Dopus || Inspected.Value().manifest_.id_ != "builtin.hyperviewer"
                        || Inspected.Value().manifest_.compatibility_ != "hyperviewer-v1") return Invalid("viewer-prepare-builtin-required");
                    auto Broker = ImageArchiveBroker::Start(); if (!Broker.HasValue()) return Broker.Failure();
                    ArchiveBroker = std::move(Broker.Value());
                    PreparationStage("host");
                    auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_,
                        Context, Options.defaultIniPath_, ArchiveBroker->Endpoint());
                    if (!Started.HasValue()) return Started.Failure(); Host = std::move(Started.Value());
                    ArchiveBroker->BindClient(Host->Snapshot().processId_);
                    auto Probe = UiCall([&](OperationContext Calls) { return Host->Probe(Calls); }); if (!Probe.HasValue()) return Probe.Failure();
                    std::pair<std::uint32_t, std::uint32_t> Size;
                    { std::lock_guard Lock(State->mutex_); Size = *State->size_; }
                    auto Opened = UiCall([&](OperationContext Calls) { return Host->ViewerCall(WireKind::ViewerOpen, {{"parent", ParentWindow}, {"file", ""},
                        {"width", int(Size.first)}, {"height", int(Size.second)}, {"flags", Options.flags_},
                        {"preview", false}, {"prepareOnly", true}}, Calls); });
                    if (!Opened.HasValue()) return Opened.Failure();
                    Document = std::make_unique<DocumentViewerClient>(*Host);
                    auto Hello = UiCall([&](OperationContext Calls) { return Document->Hello(Calls); }); if (!Hello.HasValue()) return Hello.Failure();
                    auto Prepared = UiCall([&](OperationContext Calls) { return Document->Execute({{"action", "prepareViewer"}}, Calls); });
                    if (!Prepared.HasValue()) return Prepared.Failure();
                    auto Current = UiCall([&](OperationContext Calls) { return Document->Call("DocumentState", {}, {}, {}, Calls); });
                    if (!Current.HasValue()) return Current.Failure();
                    if (!Current.Value()["documents"].toArray().isEmpty()) return Invalid("viewer-prepare-has-documents");
                    std::lock_guard Lock(State->mutex_);
                    State->details_.host_ = Host->Snapshot();
                    State->observation_ = Host->Observe();
                    State->forceStop_ = Host->ForceStopSource();
                    if (State->size_ && *State->size_ == Size) State->size_.reset();
                    State->details_.window_ = Opened.Value()["window"].toString();
                    State->details_.child_ = Opened.Value()["viewer"].toString();
                    State->details_.documentChannel_ = true;
                    State->details_.documentState_ = Current.Value();
                    State->details_.viewer_.status_ = ViewerStatus::Ready;
                    State->details_.selectedCandidate_ = Index;
                    return true;
                }
                if (!Entry)
                {
                    Entry = Source->Describe();
                    { std::lock_guard Lock(State->mutex_); State->details_.viewer_.source_ = *Entry; }
                    if (Entry->directory_) { FatalContent = true; return Error{ErrorCategory::Unsupported, "viewer-directory", {"wlx"}}; }
                    Detection.size_ = Entry->size_; Detection.force_ = Policy && Policy->force_;
                    if (Policy && Policy->builtinProbe_ && !Policy->force_)
                    {
                        auto Prefix = ReadDetectionPrefix(*Source, *Entry, Context);
                        if (!Prefix.HasValue()) { FatalContent = true; return Prefix.Failure(); }
                        Detection.prefix_ = std::move(Prefix.Value()); PrefixReady = true;
                        Policy->builtinMultimedia_ = Policy->builtinProbe_(*Entry,
                            {reinterpret_cast<const std::byte*>(Detection.prefix_.constData()), static_cast<std::size_t>(Detection.prefix_.size())});
                    }
                }
                auto Application = PluginApplicationRule::Parse(Options.applicationRule_); if (!Application.HasValue()) return Application.Failure();
                std::optional<QByteArray> ApplicationPrefix;
                if (Application.Value().NeedsPrefix()) {
                    if (!PrefixReady) { auto Prefix = ReadDetectionPrefix(*Source, *Entry, Context); if (!Prefix.HasValue()) { FatalContent = true; return Prefix.Failure(); } Detection.prefix_ = std::move(Prefix.Value()); PrefixReady = true; }
                    ApplicationPrefix = Detection.prefix_;
                }
                auto Applies = Application.Value().Matches(QString::fromStdString(Entry->name_), Entry->size_, ApplicationPrefix);
                if (!Applies.HasValue()) return Applies.Failure();
                if (!Applies.Value()) return false;
                DirectInputAllowed=Inspected.Value().manifest_.compatibility_=="hyperviewer-v1" && Inspected.Value().manifest_.id_=="builtin.hyperviewer";
                const bool PreviousDirectOriginal = DirectOriginal;
                DirectOriginal = !DirectInputAllowed && Options.approval_.granted_.contains(Permission::WriteContent)
                    && Entry->locator_.kind_ == LocatorKind::Local && !Entry->directory_
                    && !(Entry->attributes_ & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT))
                    && QFileInfo(QString::fromUtf8(Entry->locator_.root_)).isAbsolute();
                // Only the native provider can vouch for a directly writable original.
                if (DirectOriginal) {
                    const auto Path = native::FilePath(QString::fromUtf8(Entry->locator_.root_)).toStdWString();
                    wchar_t Root[MAX_PATH]{};
                    const auto Drive = GetVolumePathNameW(Path.c_str(), Root, MAX_PATH) ? GetDriveTypeW(Root) : DRIVE_UNKNOWN;
                    auto Native = MakeNativeReadProvider(LocatorKind::Local);
                    auto Resolved = Native->Resolve(Entry->locator_, Context).get();
                    DirectOriginal = (Drive == DRIVE_FIXED || Drive == DRIVE_REMOVABLE) && Resolved.HasValue()
                        && Native->Id() == Entry->id_.providerId_ && Entry->id_.revision_.has_value()
                        && !(Resolved.Value().attributes_ & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_REPARSE_POINT));
                }
                if (PreviousDirectOriginal != DirectOriginal || (!DirectInputAllowed && !DirectOriginal && dynamic_cast<SourceLease*>(Lease.get()))) Lease.reset();
                if (!Policy && !DirectInputAllowed)
                {
                    auto Prepared = Materialize(); if (!Prepared.HasValue()) return Prepared.Failure();
                }
                if(Inspected.Value().manifest_.compatibility_=="hyperviewer-v1"&&Inspected.Value().manifest_.id_=="builtin.hyperviewer"){
                    auto Broker=ImageArchiveBroker::Start();if(!Broker.HasValue())return Broker.Failure();ArchiveBroker=std::move(Broker.Value());
                    const auto Origin=ReadingSource(*Entry);if(!Origin.isEmpty())ArchiveBroker->Allow(Origin["path"].toString());if(Lease)ArchiveBroker->Allow(QString::fromStdString(Lease->LocalPath()));
                }
                // Only the built-in handler is already selected without an SDK detect call.
                // Its input preparation can overlap process/DLL startup on this worker.
                std::future<Result<Unit>> InputPreparation;
                if (DirectInputAllowed) InputPreparation = std::async(std::launch::async, Materialize);
                const auto PreparedInput = [&]() -> Result<Unit> { return InputPreparation.valid() ? InputPreparation.get() : Materialize(); };
                PreparationStage("host");
                auto Started = UiCall([&](OperationContext Calls){hc::document::ImageTraceSpan Stage("viewer.host-start");return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_, Calls, Options.defaultIniPath_,ArchiveBroker?ArchiveBroker->Endpoint():QJsonObject{});});
                if (!Started.HasValue()) return Started.Failure(); Host = std::move(Started.Value());
                const auto StartedState = Host->Snapshot();if(ArchiveBroker)ArchiveBroker->BindClient(StartedState.processId_);
                { std::lock_guard Lock(State->mutex_); State->details_.host_ = StartedState; State->observation_ = Host->Observe(); State->forceStop_ = Host->ForceStopSource(); }
                auto Probe = UiCall([&](OperationContext Calls){hc::document::ImageTraceSpan Stage("viewer.plugin-probe");return Host->Probe(Calls);}); if (!Probe.HasValue()) return Probe.Failure();
                if ((Policy || Options.detectionOverride_) && !Dopus)
                {
                    if (Probe.Value()["ansiCodePage"].toInt(-1) != static_cast<int>(GetACP()))
                        return Error{ErrorCategory::Unsupported, "wlx-detect-code-page", {"wlx"}};
                    auto Verified = Source->Verify(Context).get();
                    if (!Verified.HasValue()) { FatalContent = true; return Verified.Failure(); }
                    const auto Encoded = Probe.Value()["detectStringBytes"].toString().toLatin1();
                    const auto Decoded = QByteArray::fromBase64Encoding(Encoded, QByteArray::AbortOnBase64DecodingErrors);
                    if (!Decoded) return Invalid("wlx-detect-base64");
                    // ListLoad's SDK contract tries plugins with no definition,
                    // and proceeds to the next plugin if ListLoad returns NULL.
                    WlxDetectionMatch Match{true, false};
                    const auto Definition = Options.detectionOverride_ ? Options.detectionOverride_->toLocal8Bit() : Decoded.decoded;
                    if (Options.detectionOverride_ && QString::fromLocal8Bit(Definition) != *Options.detectionOverride_)
                        return Invalid("wlx-detect-encoding");
                    if ((Options.detectionOverride_ || Probe.Value()["detectAvailable"].toBool()) && !Definition.trimmed().isEmpty())
                    {
                        auto Expression = WlxDetectionExpression::Parse(Definition); if (!Expression.HasValue()) return Expression.Failure();
                        if (Expression.Value().NeedsExtension() && !ExtensionReady)
                        {
                            auto Extension = ExtensionBytes(*Entry); if (!Extension.HasValue()) return Extension.Failure();
                            Detection.extension_ = std::move(Extension.Value()); ExtensionReady = true;
                        }
                        if (Expression.Value().NeedsPrefix() && !PrefixReady)
                        {
                            auto Prefix = ReadDetectionPrefix(*Source, *Entry, Context);
                            if (!Prefix.HasValue()) { FatalContent = true; return Prefix.Failure(); }
                            Detection.prefix_ = std::move(Prefix.Value()); PrefixReady = true;
                        }
                        auto Evaluated = Expression.Value().Evaluate(Detection); if (!Evaluated.HasValue()) return Evaluated.Failure();
                        Match = Evaluated.Value();
                    }
                    if (Match.matches_ != true || (Policy && Policy->builtinMultimedia_ && !Policy->force_ && !Match.multimedia_)) return false;
                    auto Prepared = PreparedInput();
                    if (!Prepared.HasValue()) { FatalContent = true; return Prepared.Failure(); }
                    if (Policy) Options.flags_ = (Options.flags_ & ~lcp_forceshow) | (Policy->force_ ? lcp_forceshow : 0);
                }
                if (Policy && Dopus)
                {
                    if (Policy->builtinMultimedia_ && !Policy->force_ && !(Probe.Value()["flags"].toInteger() & (1u << 15))) return false;
                    auto Prepared = PreparedInput(); if (!Prepared.HasValue()) { FatalContent = true; return Prepared.Failure(); }
                }
                if (InputPreparation.valid()) { auto Prepared = PreparedInput(); if (!Prepared.HasValue()) return Prepared.Failure(); }
                const auto ArchiveSource = std::dynamic_pointer_cast<ProviderContentSource>(Source);
                if (!DirectInputAllowed && ArchiveSource && ArchiveSource->GetArchiveWriteback()
                    && Options.approval_.granted_.contains(Permission::WriteContent)) {
                    ExternalDraft = std::make_shared<hc::document::ArchiveDraftDirectory>(hc::document::ArchiveDraftRoot());
                    if (!ExternalDraft->isValid() || !QDir(ExternalDraft->path()).mkdir("content")) return Invalid("archive-draft-directory");
                    ExternalPath = ExternalDraft->filePath("content/" + QFileInfo(QString::fromUtf8(Entry->name_)).fileName());
                    if (!QFile::copy(QString::fromStdString(Lease->LocalPath()), ExternalPath)
                        || !QFile::setPermissions(ExternalPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return Invalid("archive-draft-copy");
                    auto Hash = DraftHash(ExternalPath); if (!Hash.HasValue()) return Hash.Failure();
                    ExternalBaseline = Hash.Value(); ExternalWriteback = ArchiveSource->GetArchiveWriteback();
                    // Preserve before granting the plugin write access, including crashes/forced shutdown.
                    ExternalDraft->setAutoRemove(false);
                    // The plugin may write at any time, so the copy is pending from the start; the recovery
                    // list drops a leftover whose content still equals baselineSha256.
                    const auto& Locator = Entry->locator_;
                    hc::document::WriteArchiveDraftManifest(ExternalDraft->path(), QJsonObject{{"schemaVersion", 2},
                        {"purpose", "external-archive-preview-edit"}, {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                        {"owner", QString::fromStdString(State->details_.viewer_.id_)}, {"draft", ExternalPath},
                        {"source", QString::fromStdString(Serialize(Locator))}, {"retention", "until-recovered-or-explicitly-discarded"},
                        {"name", QFileInfo(ExternalPath).fileName()}, {"archivePath", Locator.kind_ == LocatorKind::Archive ? QString::fromUtf8(Locator.root_) : QString{}},
                        {"entryPath", Locator.nesting_.empty() ? QString{} : QString::fromUtf8(Locator.nesting_.back().entry_)}, {"nesting", int(Locator.nesting_.size())},
                        {"baselineSha256", QString::fromLatin1(ExternalBaseline.toHex())}, {"pending", true}});
                    std::lock_guard Lock(State->mutex_); State->details_.externalArchive_ = true; State->details_.externalArchiveDraft_ = ExternalPath;
                }
                std::pair<std::uint32_t, std::uint32_t> Size;
                { std::lock_guard Lock(State->mutex_); Size = *State->size_; }
                PreparationStage("content");
                QJsonObject OpenRequest{{"parent", ParentWindow}, {"file", ExternalDraft ? ExternalPath : QString::fromStdString(Lease->LocalPath())},
                    {"width", static_cast<int>(Size.first)}, {"height", static_cast<int>(Size.second)}, {"flags", Options.flags_}, {"preview", Options.purpose_ == ViewerPurpose::QuickPreview}};
                if (DirectInputAllowed) { const auto Prefix = ReadPrefix(Source); if (!Prefix.isEmpty()) OpenRequest["readPrefix"] = Prefix; }
                auto Opened = UiCall([&](OperationContext Calls){ return Host->ViewerCall(WireKind::ViewerOpen, OpenRequest, Calls); });
                if (!Opened.HasValue())
                {
                    if (Policy && Opened.Failure().code_ == "host-viewer-load-rejected") return false;
                    return Opened.Failure();
                }
                if (Inspected.Value().manifest_.compatibility_ == "hyperviewer-v1") {
                    Document = std::make_unique<DocumentViewerClient>(*Host);
                    auto Ready = UiCall([&](OperationContext Calls){ return Document->Hello(Calls); }); if (!Ready.HasValue()) return Ready.Failure();
                    { std::lock_guard Lock(State->mutex_);
                      State->details_.documentChannel_ = true;
                      State->details_.window_ = Opened.Value()["window"].toString();
                      State->details_.child_ = Opened.Value()["viewer"].toString(); }
                    // Ready negotiates the channel; the asynchronous SDK Load
                    // can still be parsing. Bind only after its slot is stable.
                    auto Deadline = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(15));
                    auto WaitTrace=std::make_unique<hc::document::ImageTraceSpan>("viewer.initial-content");
                    for (;;) {
                        const auto PollStarted = std::chrono::steady_clock::now();
                        auto Current = UiCall([&](OperationContext Calls) { return Document->Call("DocumentState", {}, {}, {}, Calls); }); if (!Current.HasValue()) return Current.Failure();
                        Deadline = std::min(Context.deadline_, Deadline + (std::chrono::steady_clock::now() - PollStarted));
                        { std::lock_guard Lock(State->mutex_); State->details_.documentState_=Current.Value(); }
                        const auto Slots = Current.Value()["documents"].toArray();
                        // An empty helper is idle before the queued SDK load starts.
                        // Wait for that specific slot, otherwise its delayed load can
                        // overwrite the original-provider binding with a read-only slot.
                        if (Slots.size() == 1) {
                            const auto Slot = Slots[0].toObject();
                            if (Slot["hostSlot"].toBool() && QFileInfo(Slot["path"].toString()).absoluteFilePath().compare(
                                QFileInfo(QString::fromStdString(Lease->LocalPath())).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
                                if (!Slot["error"].toString().isEmpty()) return Error{ErrorCategory::Unavailable, Slot["error"].toString().toStdString()};
                                // EPUB needs the presented native surface to finish its first frame.
                                // Its immutable input and matching hash are stable before rendering.
                                const bool EpubSource = Slot["view"].toObject()["viewId"] == "epub" && Slot["sourceReady"].toBool()
                                    && !Slot["canSave"].toBool() && !Slot["canSaveAs"].toBool();
                                const bool ImageSource=Slot["imagePresentationReady"].toBool() && !Slot["loading"].toBool()
                                    && Slot["lifecycle"].toString()=="ready";
                                if (EpubSource || ImageSource || (Current.Value()["idle"].toBool() && !Slot["loading"].toBool() && Slot["lifecycle"].toString() == "ready")) break;
                            }
                        }
                        if (auto Stop = Context.Check({"document"})) return *Stop;
                        if (std::chrono::steady_clock::now() >= Deadline) return Error{ErrorCategory::Unavailable, "document-initial-slot-timeout"};
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    WaitTrace.reset();
                    const bool DirectBook=DirectInputAllowed && IsDirectBook(*Entry);
                    auto Writer = DirectBook?std::shared_ptr<IWritableStorage>{}:Options.writer_;
                    if (!DirectBook && !Writer && (Entry->locator_.kind_ == LocatorKind::Local || Entry->locator_.kind_ == LocatorKind::Smb) && !(Entry->attributes_ & 1u))
                        Writer = MakeNativeWritableStorage(Entry->locator_.kind_, MakeNativeReadProvider(Entry->locator_.kind_));
                    PreparationStage("verify");
                    // The approved reader already hashed the exact snapshot while loading it.
                    // Re-reading an SMB/RDP input here doubles first-open network traffic.
                    QByteArray Hash;
                    const auto Loaded = UiCall([&](OperationContext Calls) { return Document->Call("DocumentState", {}, {}, {}, Calls); });
                    if (!Loaded.HasValue()) return Loaded.Failure();
                    for (const auto& Item : Loaded.Value()["documents"].toArray())
                        if (Item.toObject()["hostSlot"].toBool()) Hash = QByteArray::fromHex(Item.toObject()["inputSha256"].toString().toLatin1());
                    if (!DirectBook && Hash.size() != 32) return Invalid("document-input-hash-required");
                    const auto ArchiveSource = std::dynamic_pointer_cast<ProviderContentSource>(Source);
                    const auto Writeback = ArchiveSource ? ArchiveSource->GetArchiveWriteback() : ProviderContentSource::ArchiveWriteback{};
                    const auto Token = Document->Bind(*Entry, Writer, Lease, Hash, Writeback);
                    if (Token.isEmpty()) return Invalid("document-source-binding");
                    auto Bound = UiCall([&](OperationContext Calls) { return Document->Call("OpenDocuments", {{"documents", QJsonArray{QJsonObject{{"path", QString::fromStdString(Lease->LocalPath())},
                        {"displayName", QString::fromUtf8(Entry->name_)}, {"readingSource",ReadingSource(*Entry)}, {"sourceToken", Token}, {"broker", bool(Writer) || bool(Writeback)}, {"hostSlot", true}, {"bindExistingHost", true}, {"readOnlyBook",DirectBook}, {"sha256", QString::fromLatin1(Hash.toHex())}}}}}, {}, {}, Calls); });
                    if (!Bound.HasValue()) return Bound.Failure();
                    std::lock_guard Lock(State->mutex_); State->details_.documentChannel_ = true;
                }
                const auto HostState = Host->Snapshot();
                {
                    std::lock_guard Lock(State->mutex_); State->details_.host_ = HostState;
                    if (State->size_ && *State->size_ == Size) State->size_.reset();
                    State->details_.window_ = Opened.Value()["window"].toString(); State->details_.child_ = Opened.Value()["viewer"].toString();
                    State->details_.viewer_.bytesRead_ = Lease->Describe().size_; State->details_.viewer_.status_ = ViewerStatus::Ready;
                    State->details_.viewer_.provider_ = std::string(Dopus ? "dopus-viewer." : "wlx.") + Options.approval_.reviewed_.id_;
                    State->details_.selectedCandidate_ = Index; State->commands_ = Probe.Value()["commands"].toBool();
                    State->details_.directOriginal_ = DirectOriginal;
                    State->search_ = Probe.Value()["search"].toBool();
                }
                return true;
            };
            bool Selected = false;
            for (std::size_t Index = 0; Index < Candidates.size(); ++Index)
            {
                auto Tried = Attempt(Candidates[Index], Index);
                if (Tried.HasValue() && Tried.Value()) { Selected = true; break; }
                CloseHost();
                if (ExternalDraft) {
                    const auto Hash = DraftHash(ExternalPath);
                    if (!Hash.HasValue() || Hash.Value() != ExternalBaseline)
                        return Error{ErrorCategory::Unavailable, "archive-plugin-rejected-edited-draft:" + ExternalPath.toStdString()};
                    ExternalDraft->setAutoRemove(true); ExternalDraft.reset(); ExternalPath.clear(); ExternalWriteback = {};
                    std::lock_guard Lock(State->mutex_); State->details_.externalArchive_ = false; State->details_.externalArchiveDraft_.clear();
                }
                if (!Tried.HasValue())
                {
                    if (!Policy || FatalContent) return Tried.Failure();
                    std::lock_guard Lock(State->mutex_); State->details_.candidateFailures_.push_back({Index, Tried.Failure()});
                }
                if (const auto Stop = Context.Check({"wlx"})) return *Stop;
            }
            if (!Selected)
            {
                std::lock_guard Lock(State->mutex_);
                if (!State->details_.candidateFailures_.empty()) return State->details_.candidateFailures_.front().error_;
                NoMatch = true; return Unit{};
            }
            // Document bindings own each input until its tab closes. Retaining
            // the initial lease/source here would pin a closed ZIP (or its outer
            // archive) for the entire reusable viewer process lifetime.
            // Ordinary WLX viewers still need their session-scoped input lease.
            if (Document) { Lease.reset(); Source.reset(); }
            auto HostState = Host->Snapshot();
            OperationContext Calls; Calls.stopToken_ = Context.stopToken_;
            while (!State->stop_.stop_requested())
            {
                std::optional<std::pair<std::uint32_t, std::uint32_t>> Resize;
                std::optional<QJsonObject> InputBinding;
                std::optional<HostedViewerSession::State::Command> Command; bool Focus = false;
                {
                    std::unique_lock Lock(State->mutex_);
                    State->changed_.wait_for(Lock, std::chrono::milliseconds(50), [&] { return State->stop_.stop_requested() || State->size_ || State->focus_ || State->inputBinding_ || !State->queue_.empty(); });
                    if (State->stop_.stop_requested()) break;
                    Resize = std::exchange(State->size_, {}); Focus = std::exchange(State->focus_, false);
                    InputBinding = std::exchange(State->inputBinding_, {});
                    if (!State->queue_.empty()) { Command = State->queue_.front(); State->queue_.pop_front(); }
                }
                if (Command && Command->command_ == 8) {
                    Result<QByteArray> Checked = ExternalBaseline;
                    std::optional<Error> Error;
                    if (Host) {
                        auto Closed = Host->ViewerCall(WireKind::ViewerClose, {}, Calls);
                        // Reap the process tree before inspecting the last saved bytes.
                        Host->Close(); Host.reset();
                        if (!Closed.HasValue()) Error = Closed.Failure();
                    }
                    if (!Error) { Checked = DraftHash(ExternalPath); if (!Checked.HasValue()) Error = Checked.Failure(); }
                    bool Done = false;
                    if (!Error && (Checked.Value() == ExternalBaseline || Command->parameter_ == 2)) Done = true;
                    else if (!Error && Command->parameter_ == 1) {
                        auto Saved = ExternalWriteback(ExternalPath.toStdString(), Calls);
                        if (Saved.HasValue()) Done = true; else Error = Saved.Failure();
                    }
                    if (Done) ExternalDraft->setAutoRemove(true);
                    std::lock_guard Lock(State->mutex_);
                    State->details_.externalArchiveClosed_ = true; State->details_.externalArchiveDone_ = Done;
                    State->details_.commandSequence_ = Command->sequence_; State->details_.commandError_ = Error;
                    continue;
                }
                if (!Host) continue;
                if (Resize)
                {
                    auto Result = Host->ViewerCall(WireKind::ViewerResize, {{"width", static_cast<int>(Resize->first)}, {"height", static_cast<int>(Resize->second)}}, Calls);
                    if (!Result.HasValue()) return Result.Failure();
                }
                if (InputBinding) { auto Result = Host->ViewerCall(WireKind::ViewerFocus, *InputBinding, Calls); if (!Result.HasValue()) return Result.Failure(); }
                if (Focus) { auto Result = Host->ViewerCall(WireKind::ViewerFocus, {}, Calls); if (!Result.HasValue()) return Result.Failure(); }
                if (Command && Document && (Command->command_ == 6 || Command->command_ == 7)) {
                    Result<QJsonObject> Result = QJsonObject{};
                    if (Command->command_ == 6) Result = Document->Execute(Command->document_, Calls);
                    else {
                        QJsonArray Inputs;
                        for (const auto& Input : Command->inputs_) {
                            PreparationStage("materialize");
                            const bool DirectBook=bool(ArchiveBroker) && IsDirectBook(Input.source_->Describe());
                            const bool DirectImage=bool(ArchiveBroker) && IsDirectNative(Input.source_->Describe());
                            if(!DirectBook && !DirectImage && !Materializer){auto Created=Factory();if(!Created.HasValue()){Result=Created.Failure();break;}Materializer=std::move(Created.Value());}
                            hc::document::ImageTraceSpan PrepareTrace("viewer.prepare-input",QString::fromUtf8(Input.source_->Describe().locator_.root_));
                            auto Materialized = DirectBook?BorrowBook(Input.source_,Calls):DirectImage?BorrowNative(Input.source_,Calls):Materializer->Materialize({Input.source_, {}, {}}, PreparationContext(Calls, Input.source_->Describe())).get();
                            ClearPreparation();
                            if (!Materialized.HasValue()) { Result = Materialized.Failure(); break; }
                            const auto Entry = Input.source_->Describe();
                            if(ArchiveBroker){ArchiveBroker->Allow(QString::fromStdString(Materialized.Value()->LocalPath()));const auto Origin=ReadingSource(Entry);if(!Origin.isEmpty())ArchiveBroker->Allow(Origin["path"].toString());}
                            auto Writer = DirectBook?std::shared_ptr<IWritableStorage>{}:Input.writer_;
                            if (!DirectBook && !Writer && (Entry.locator_.kind_ == LocatorKind::Local || Entry.locator_.kind_ == LocatorKind::Smb) && !(Entry.attributes_ & 1u))
                                Writer = MakeNativeWritableStorage(Entry.locator_.kind_, MakeNativeReadProvider(Entry.locator_.kind_));
                            const auto ArchiveSource = std::dynamic_pointer_cast<ProviderContentSource>(Input.source_);
                            const auto Writeback = ArchiveSource ? ArchiveSource->GetArchiveWriteback() : ProviderContentSource::ArchiveWriteback{};
                            const auto Token = Document->Bind(Entry, Writer, Materialized.Value(), {}, Writeback);
                            if (Token.isEmpty()) { Result = Invalid("document-source-binding"); break; }
                            Inputs.append(QJsonObject{{"path", QString::fromStdString(Materialized.Value()->LocalPath())},
                                {"displayName", QString::fromUtf8(Entry.name_)}, {"readPrefix", ReadPrefix(Input.source_)}, {"readingSource",ReadingSource(Entry)}, {"sourceToken", Token}, {"broker", bool(Writer) || bool(Writeback)}});
                        }
                        if (Result.HasValue() && Command->parameter_ == 1) {
                            const auto Current = Document->Call("DocumentState", {}, {}, {}, Calls);
                            if (!Current.HasValue()) Result = Current.Failure();
                            else for (const auto& Item : Current.Value()["documents"].toArray()) {
                                auto Closed = Document->Execute({{"action", "close"}, {"documentId", Item.toObject()["documentId"]}}, Calls);
                                if (!Closed.HasValue()) { Result = Closed.Failure(); break; }
                            }
                        }
                        PreparationStage("content");
                        if (Result.HasValue()) Result = Document->Call("OpenDocuments", {{"documents", Inputs}}, {}, {}, Calls);
                        if (Result.HasValue() && Command->parameter_ == 1) {
                            std::lock_guard Lock(State->mutex_);
                            State->details_.viewer_.source_ = Command->inputs_.front().source_->Describe();
                            State->details_.viewer_.bytesRead_ = State->details_.viewer_.source_.size_;
                        }
                    }
                    std::lock_guard Lock(State->mutex_); State->details_.commandSequence_ = Command->sequence_;
                    // Publish no old bootstrap state with the new completion
                    // sequence; the following poll supplies the new documents.
                    if (Command->command_ == 7 && Command->parameter_ == 1) State->details_.documentState_ = {};
                    State->details_.commandError_ = Result.HasValue() ? std::optional<Error>{} : Result.Failure();
                    State->details_.documentReply_ = Result.HasValue() ? Result.Value() : QJsonObject{};
                }
                else if (Command)
                {
                    QJsonObject Request{{"command", Command->command_}, {"parameter", Command->parameter_}};
                    if (Command->command_ == 5) Request.insert("text", Command->text_);
                    auto Result = Host->ViewerCall(WireKind::ViewerCommand, Request, Calls);
                    std::lock_guard Lock(State->mutex_); State->details_.commandSequence_ = Command->sequence_;
                    State->details_.commandError_ = Result.HasValue() ? std::optional<Error>{} : Result.Failure();
                    if (Result.HasValue() && Result.Value()["result"].toInt(-1) != 0) State->details_.commandError_ = Error{ErrorCategory::Unsupported, "host-viewer-command-rejected", {"wlx"}};
                }
                if (Document) {
                    auto Current = Document->Poll(Calls); if (!Current.HasValue()) return Current.Failure();
                    const bool Empty = Current.Value()["documents"].toArray().isEmpty();
                    if (Empty && !ArchiveIdle && ArchiveBroker) ArchiveBroker->Clear();
                    ArchiveIdle = Empty;
                    std::lock_guard Lock(State->mutex_); State->details_.documentState_ = Current.Value();
                }
                HostState = Host->Snapshot();
                { std::lock_guard Lock(State->mutex_); State->details_.host_ = HostState; }
                if (!HostState.running_) return HostState.failure_.value_or(Error{ErrorCategory::Unavailable, "host-exited", {"wlx"}});
            }
            return Unit{};
        };
        std::optional<Error> Failure;
        try { auto Result = Work(); if (!Result.HasValue()) Failure = Result.Failure(); }
        catch (const std::exception&) { Failure = Error{ErrorCategory::Internal, "viewer-worker-exception", {"wlx"}}; }
        catch (...) { Failure = Error{ErrorCategory::Internal, "viewer-worker-exception", {"wlx"}}; }
        // Never release a materialized file while a plugin can still read it.
        if (Host)
        {
            Host->Close(); auto FinalHost = Host->Snapshot();
              { std::lock_guard Lock(State->mutex_); State->details_.host_ = FinalHost; State->observation_ = {}; }
            Host.reset();
        }
        ArchiveBroker.reset(); Document.reset(); Lease.reset(); Source.reset();
        if (Materializer)
        {
            try
            {
                auto Cleanup = Materializer->DrainCleanup();
                if (!Failure && !Cleanup.errors_.empty()) Failure = Cleanup.errors_.front();
            }
            catch (...) { if (!Failure) Failure = Error{ErrorCategory::Internal, "viewer-cleanup-exception", {"wlx"}}; }
            Materializer.reset();
        }
        {
            std::lock_guard Lock(State->mutex_); auto& Snapshot = State->details_.viewer_;
            Snapshot.error_ = Failure; State->commands_ = false; State->queue_.clear();
            State->details_.noMatch_ = NoMatch && !Failure && !State->closed_ && !State->cancelled_;
            Snapshot.status_ = State->closed_ ? ViewerStatus::Closed : State->cancelled_ ? ViewerStatus::Cancelled : Failure ? ViewerStatus::Failed : ViewerStatus::Closed;
        }
    }
}
