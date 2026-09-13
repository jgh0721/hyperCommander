#include "hostedWlxSession.hpp"
#include <QFileInfo>
#include <QStringDecoder>
#include "wlxDetection.hpp"
#include <qt_windows.h>
#include "externalLibs/TotalCommander/tc_listplug.h"
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <thread>

namespace hc::plugins
{
    using namespace storage;
    using namespace viewer;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
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
        Result<QByteArray> ReadDetectionPrefix(IContentSource& Source, const EntrySnapshot& Entry, OperationContext Context)
        {
            const auto Required = static_cast<std::size_t>(std::min<std::uint64_t>(Entry.size_, 8192));
            if (Required > Context.maxReadBytes_) return Error{ErrorCategory::BudgetExceeded, "wlx-detect-read-budget", Entry.id_.providerId_};
            QByteArray Bytes;
            if (Required)
            {
                auto Opened = Source.OpenRead(Context).get(); if (!Opened.HasValue()) return Opened.Failure();
                if (!Opened.Value()) return Invalid("wlx-detect-stream-required");
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
            return Bytes;
        }
    }
    struct HostedWlxSession::State
    {
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::stop_source stop_;
        WlxSessionDetails details_;
        bool closed_ = false, cancelled_ = false, commands_ = false, search_ = false, focus_ = false;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> size_;
        struct Command { std::uint64_t sequence_; int command_, parameter_; QString text_; };
        std::deque<Command> queue_;
        std::uint64_t nextCommand_ = 1;
        void stop(bool Close)
        {
            { std::lock_guard Lock(mutex_); closed_ |= Close; cancelled_ |= !Close; }
            stop_.request_stop(); changed_.notify_all();
        }
    };
    struct WlxViewerService::Task
    {
        std::shared_ptr<HostedWlxSession::State> state_;
        std::jthread worker_;
    };
    HostedWlxSession::HostedWlxSession(std::shared_ptr<State> State) : state_(std::move(State)) {}
    HostedWlxSession::~HostedWlxSession() { Close(); }
    ViewerSnapshot HostedWlxSession::Snapshot() const { std::lock_guard Lock(state_->mutex_); return state_->details_.viewer_; }
    WlxSessionDetails HostedWlxSession::Details() const { std::lock_guard Lock(state_->mutex_); return state_->details_; }
    std::vector<std::string> HostedWlxSession::Commands() const
    {
        std::lock_guard Lock(state_->mutex_); auto Commands = state_->commands_ ? std::vector<std::string>{"copy", "wrap", "select-all", "set-percent"} : std::vector<std::string>{};
        if (state_->search_) Commands.push_back("find"); return Commands;
    }
    void HostedWlxSession::Cancel() { state_->stop(false); }
    void HostedWlxSession::Close() { state_->stop(true); }
    Result<Unit> HostedWlxSession::Resize(std::uint32_t Width, std::uint32_t Height)
    {
        if (Width > 32768 || Height > 32768) return Invalid("host-viewer-size");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->size_ = {Width, Height}; state_->changed_.notify_one(); return Unit{};
    }
    Result<Unit> HostedWlxSession::Focus()
    {
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->focus_ = true; state_->changed_.notify_one(); return Unit{};
    }
    Result<std::uint64_t> HostedWlxSession::SendCommand(int Command, int Parameter)
    {
        if (Command < 1 || Command > 4 || Parameter < 0 || Parameter > (Command == 4 ? 100 : 511)) return Invalid("host-viewer-command-fields");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready) return Error{ErrorCategory::Unavailable, "viewer-not-ready", {"wlx"}};
        if (!state_->commands_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"wlx"}};
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "viewer-command-budget", {"wlx"}};
        const auto Sequence = state_->nextCommand_++; state_->queue_.push_back({Sequence, Command, Parameter});
        state_->changed_.notify_one(); return Sequence;
    }
    Result<std::uint64_t> HostedWlxSession::Search(QString Text, int Flags)
    {
        if (Text.isEmpty() || Text.size() > 4096 || Text.contains(QChar::Null) || Flags < 0 || Flags > 15) return Invalid("host-viewer-search-fields");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready) return Error{ErrorCategory::Unavailable, "viewer-not-ready", {"wlx"}};
        if (!state_->search_) return Error{ErrorCategory::Unsupported, "host-viewer-search-unavailable", {"wlx"}};
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "viewer-command-budget", {"wlx"}};
        const auto Sequence = state_->nextCommand_++; state_->queue_.push_back({Sequence, 5, Flags, std::move(Text)}); state_->changed_.notify_one(); return Sequence;
    }
    WlxViewerService::WlxViewerService(MaterializerFactory Factory, std::size_t MaxSessions)
        : factory_(std::move(Factory)), maxSessions_(MaxSessions) {}
    WlxViewerService::~WlxViewerService() { CancelAll(); tasks_.clear(); }
    Result<std::shared_ptr<HostedWlxSession>> WlxViewerService::Open(WlxViewerOptions Options,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, std::uint32_t Width, std::uint32_t Height, OperationContext Context)
    { return open({std::move(Options)}, {}, std::move(Source), std::move(ParentWindow), Width, Height, Context); }
    Result<std::shared_ptr<HostedWlxSession>> WlxViewerService::OpenAutomatic(std::vector<WlxViewerOptions> Candidates,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, std::uint32_t Width, std::uint32_t Height,
        WlxAutomaticPolicy Policy, OperationContext Context)
    { return open(std::move(Candidates), Policy, std::move(Source), std::move(ParentWindow), Width, Height, Context); }
    Result<std::shared_ptr<HostedWlxSession>> WlxViewerService::open(std::vector<WlxViewerOptions> Candidates,
        std::optional<WlxAutomaticPolicy> Policy, std::shared_ptr<IContentSource> Source, QString ParentWindow,
        std::uint32_t Width, std::uint32_t Height, OperationContext Context)
    {
        if (!factory_ || !Source || ParentWindow.isEmpty() || Width > 32768 || Height > 32768 || Candidates.size() > 1000
            || std::any_of(Candidates.begin(), Candidates.end(), [](const auto& Item) { return Item.flags_ < 0 || Item.flags_ > 511; }))
            return Invalid("invalid-wlx-session-request");
        if (!maxSessions_ || maxSessions_ > 16) return Invalid("invalid-wlx-session-budget");
        Collect(); if (tasks_.size() >= maxSessions_) return Error{ErrorCategory::BudgetExceeded, "viewer-session-budget", {"wlx"}};
        auto State = std::make_shared<HostedWlxSession::State>(); State->size_ = {Width, Height};
        State->details_.viewer_.id_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        State->details_.viewer_.provider_ = Policy ? "wlx.automatic" : "wlx." + Candidates.front().approval_.reviewed_.id_;
        auto Session = std::shared_ptr<HostedWlxSession>(new HostedWlxSession(State));
        auto Task = std::make_unique<WlxViewerService::Task>(); Task->state_ = State;
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
    void WlxViewerService::CancelAll() { for (auto& Task : tasks_) Task->state_->stop(false); }
    bool WlxViewerService::IsIdle() const
    {
        for (const auto& Task : tasks_) { std::lock_guard Lock(Task->state_->mutex_); if (!Task->state_->details_.idle_) return false; }
        return true;
    }
    void WlxViewerService::Collect()
    {
        std::erase_if(tasks_, [](const auto& Task) { std::lock_guard Lock(Task->state_->mutex_); return Task->state_->details_.idle_; });
    }
    void WlxViewerService::run(const std::shared_ptr<HostedWlxSession::State>& State, std::vector<WlxViewerOptions> Candidates,
        std::optional<WlxAutomaticPolicy> Policy,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, MaterializerFactory Factory, OperationContext Context)
    {
        std::stop_callback External(Context.stopToken_, [&] { State->stop(false); });
        Context.stopToken_ = State->stop_.get_token();
        std::shared_ptr<IMaterializer> Materializer;
        std::shared_ptr<IMaterializedFileLease> Lease;
        std::unique_ptr<PluginProcess> Host;
        bool NoMatch = false;
        const auto CloseHost = [&] {
            if (!Host) return;
            Host->Close(); const auto Final = Host->Snapshot();
            { std::lock_guard Lock(State->mutex_); State->details_.host_ = Final; }
            Host.reset();
        };
        const auto Work = [&]() -> Result<Unit> {
            if (const auto Stop = Context.Check({"wlx"})) return *Stop;
            if (Policy)
            {
                Context.deadline_ = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(30));
                Context.denyConcurrentMutation_ = true;
            }
            std::optional<EntrySnapshot> Entry;
            WlxDetectionInput Detection; bool PrefixReady = false, ExtensionReady = false, FatalContent = false;
            const auto Materialize = [&]() -> Result<Unit> {
                if (Lease) return Unit{};
                auto Created = Factory(); if (!Created.HasValue()) return Created.Failure(); Materializer = std::move(Created.Value());
                if (!Materializer) return Invalid("viewer-materializer-required");
                auto Materialized = Materializer->Materialize({Source, {}, {}}, Context).get();
                if (!Materialized.HasValue()) return Materialized.Failure(); Lease = std::move(Materialized.Value());
                if (!Lease) return Invalid("viewer-content-lease-required");
                return Unit{};
            };
            const auto Attempt = [&](WlxViewerOptions& Options, std::size_t Index) -> Result<bool> {
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
                if (!Policy)
                {
                    auto Prepared = Materialize(); if (!Prepared.HasValue()) return Prepared.Failure();
                }
                auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_, Context, Options.defaultIniPath_);
                if (!Started.HasValue()) return Started.Failure(); Host = std::move(Started.Value());
                const auto StartedState = Host->Snapshot();
                { std::lock_guard Lock(State->mutex_); State->details_.host_ = StartedState; }
                auto Probe = Host->Probe(Context); if (!Probe.HasValue()) return Probe.Failure();
                if (Policy && !Dopus)
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
                    if (Probe.Value()["detectAvailable"].toBool() && !Decoded.decoded.trimmed().isEmpty())
                    {
                        auto Expression = WlxDetectionExpression::Parse(Decoded.decoded); if (!Expression.HasValue()) return Expression.Failure();
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
                    if (Match.matches_ != true || (Policy->builtinMultimedia_ && !Policy->force_ && !Match.multimedia_)) return false;
                    auto Prepared = Materialize();
                    if (!Prepared.HasValue()) { FatalContent = true; return Prepared.Failure(); }
                    Options.flags_ = (Options.flags_ & ~lcp_forceshow) | (Policy->force_ ? lcp_forceshow : 0);
                }
                if (Policy && Dopus)
                {
                    if (Policy->builtinMultimedia_ && !Policy->force_ && !(Probe.Value()["flags"].toInteger() & (1u << 15))) return false;
                    auto Prepared = Materialize(); if (!Prepared.HasValue()) { FatalContent = true; return Prepared.Failure(); }
                }
                std::pair<std::uint32_t, std::uint32_t> Size;
                { std::lock_guard Lock(State->mutex_); Size = *State->size_; }
                auto Opened = Host->ViewerCall(WireKind::ViewerOpen, {{"parent", ParentWindow}, {"file", QString::fromStdString(Lease->LocalPath())},
                    {"width", static_cast<int>(Size.first)}, {"height", static_cast<int>(Size.second)}, {"flags", Options.flags_}}, Context);
                if (!Opened.HasValue())
                {
                    if (Policy && Opened.Failure().code_ == "host-viewer-load-rejected") return false;
                    return Opened.Failure();
                }
                const auto HostState = Host->Snapshot();
                {
                    std::lock_guard Lock(State->mutex_); State->details_.host_ = HostState;
                    if (State->size_ && *State->size_ == Size) State->size_.reset();
                    State->details_.window_ = Opened.Value()["window"].toString(); State->details_.child_ = Opened.Value()["viewer"].toString();
                    State->details_.viewer_.bytesRead_ = Lease->Describe().size_; State->details_.viewer_.status_ = ViewerStatus::Ready;
                    State->details_.viewer_.provider_ = std::string(Dopus ? "dopus-viewer." : "wlx.") + Options.approval_.reviewed_.id_;
                    State->details_.selectedCandidate_ = Index; State->commands_ = Probe.Value()["commands"].toBool();
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
            auto HostState = Host->Snapshot();
            OperationContext Calls; Calls.stopToken_ = Context.stopToken_;
            while (!State->stop_.stop_requested())
            {
                std::optional<std::pair<std::uint32_t, std::uint32_t>> Resize;
                std::optional<HostedWlxSession::State::Command> Command; bool Focus = false;
                {
                    std::unique_lock Lock(State->mutex_);
                    State->changed_.wait_for(Lock, std::chrono::milliseconds(50), [&] { return State->stop_.stop_requested() || State->size_ || State->focus_ || !State->queue_.empty(); });
                    if (State->stop_.stop_requested()) break;
                    Resize = std::exchange(State->size_, {}); Focus = std::exchange(State->focus_, false);
                    if (!State->queue_.empty()) { Command = State->queue_.front(); State->queue_.pop_front(); }
                }
                if (Resize)
                {
                    auto Result = Host->ViewerCall(WireKind::ViewerResize, {{"width", static_cast<int>(Resize->first)}, {"height", static_cast<int>(Resize->second)}}, Calls);
                    if (!Result.HasValue()) return Result.Failure();
                }
                if (Focus) { auto Result = Host->ViewerCall(WireKind::ViewerFocus, {}, Calls); if (!Result.HasValue()) return Result.Failure(); }
                if (Command)
                {
                    QJsonObject Request{{"command", Command->command_}, {"parameter", Command->parameter_}};
                    if (Command->command_ == 5) Request.insert("text", Command->text_);
                    auto Result = Host->ViewerCall(WireKind::ViewerCommand, Request, Calls);
                    std::lock_guard Lock(State->mutex_); State->details_.commandSequence_ = Command->sequence_;
                    State->details_.commandError_ = Result.HasValue() ? std::optional<Error>{} : Result.Failure();
                    if (Result.HasValue() && Result.Value()["result"].toInt(-1) != 0) State->details_.commandError_ = Error{ErrorCategory::Unsupported, "host-viewer-command-rejected", {"wlx"}};
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
            { std::lock_guard Lock(State->mutex_); State->details_.host_ = FinalHost; }
            Host.reset();
        }
        Lease.reset(); Source.reset();
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
