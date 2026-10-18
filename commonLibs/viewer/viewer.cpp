#include "viewer.hpp"
#include <atomic>

namespace hc::viewer
{
    using namespace storage;
    using namespace operations;
    Result<Unit> ViewerRegistry::Register(std::shared_ptr<IViewerProvider> Provider)
    {
        if (!Provider) return Error{ErrorCategory::InvalidRequest, "invalid-viewer-provider"};
        const auto Id = Provider->Id();
        if (Id.empty() || Id.size() > 128) return Error{ErrorCategory::InvalidRequest, "invalid-viewer-provider"};
        std::lock_guard Lock(mutex_);
        if (!providers_.emplace(Id, std::move(Provider)).second) return Error{ErrorCategory::InvalidRequest, "duplicate-viewer-provider"};
        return Unit{};
    }
    std::vector<std::shared_ptr<IViewerProvider>> ViewerRegistry::Providers() const
    {
        std::lock_guard Lock(mutex_); std::vector<std::shared_ptr<IViewerProvider>> Values;
        for (const auto& [Id, Provider] : providers_) Values.push_back(Provider);
        return Values;
    }
    namespace
    {
        struct SessionState
        {
            mutable std::mutex mutex_;
            ViewerSnapshot snapshot_;
            std::shared_ptr<const ViewerDocument> document_;
            std::vector<std::string> commands_;
            std::promise<Result<Unit>> opened_;
            std::string jobId_;
            bool cancelRequested_ = false, closed_ = false;
        };
        struct MemoryBudget
        {
            std::atomic<std::uint64_t> used_ = 0;
            std::uint64_t limit_;
            explicit MemoryBudget(std::uint64_t Limit) : limit_(Limit) {}
        };
        class LoadTask final : public IJobTask
        {
        public:
            LoadTask(ViewerRequest Request, std::vector<std::shared_ptr<IViewerProvider>> Providers,
                std::shared_ptr<SessionState> State, std::shared_ptr<MemoryBudget> Budget)
                : providers_(std::move(Providers)), state_(std::move(State)), budget_(std::move(Budget))
            { input_.request_ = std::move(Request); input_.source_ = state_->snapshot_.source_; }
            JobStep Advance(JobStepContext Context) override
            {
                try
                {
                    if (const auto Failure = Context.operation_.Check({})) return failed(*Failure);
                    { std::lock_guard Lock(state_->mutex_); if (state_->cancelRequested_ || state_->closed_) return failed({ErrorCategory::Cancelled, "viewer-cancelled"}); }
                    if (!input_.stream_)
                    {
                        auto Verified = input_.request_.source_->Verify(Context.operation_).get(); if (!Verified.HasValue()) return failed(Verified.Failure());
                        auto ReadContext = Context.operation_; ReadContext.denyConcurrentMutation_ = true;
                        auto Stream = input_.request_.source_->OpenRead(ReadContext).get(); if (!Stream.HasValue()) return failed(Stream.Failure());
                        input_.stream_ = std::move(Stream.Value()); if (!input_.stream_) return failed({ErrorCategory::Internal, "viewer-stream-required"});
                        return JobStep{JobState::Running};
                    }
                    if (!provider_)
                    {
                        const auto PrefixLimit = std::min<std::size_t>(4096 - input_.prefix_.size(), Context.operation_.maxReadBytes_);
                        auto Prefix = input_.stream_->Read(PrefixLimit, Context.operation_).get();
                        if (!Prefix.HasValue()) return failed(Prefix.Failure());
                        if (Prefix.Value().bytes_.size() > PrefixLimit || Prefix.Value().bytes_.size() > input_.source_.size_ - input_.prefix_.size()
                            || (Prefix.Value().bytes_.empty() && !Prefix.Value().eof_)) return failed({ErrorCategory::Internal, "invalid-viewer-prefix"});
                        input_.prefix_.insert(input_.prefix_.end(), Prefix.Value().bytes_.begin(), Prefix.Value().bytes_.end());
                        input_.eof_ = Prefix.Value().eof_;
                        // A caller's read budget can be smaller than a BOM or
                        // image signature. Gather the prefix across safe points.
                        if (!input_.eof_ && input_.prefix_.size() < std::min<std::uint64_t>(4096, input_.source_.size_))
                            return JobStep{JobState::Running};
                        int Score = 0;
                        for (const auto& Provider : providers_)
                        {
                            if (!input_.request_.provider_.empty())
                            { if (Provider->Id() == input_.request_.provider_) { provider_ = Provider; break; } continue; }
                            auto Probed = Provider->Probe(input_.source_, input_.prefix_); if (!Probed.HasValue()) return failed(Probed.Failure());
                            if (Probed.Value() < 0 || Probed.Value() > 100) return failed({ErrorCategory::Internal, "invalid-viewer-probe-score"});
                            if (Probed.Value() > Score) { Score = Probed.Value(); provider_ = Provider; }
                        }
                        if (!provider_) return failed({ErrorCategory::Unsupported, "viewer-no-provider"});
                        { std::lock_guard Lock(state_->mutex_); state_->snapshot_.provider_ = provider_->Id(); state_->snapshot_.bytesRead_ = input_.prefix_.size(); }
                        return JobStep{JobState::Running};
                    }
                    const auto Source = input_.request_.source_;
                    const auto Limits = input_.request_.limits_;
                    auto Opened = provider_->Open(std::move(input_), Context.operation_, [State = state_](std::uint64_t Bytes) {
                        std::lock_guard Lock(State->mutex_); State->snapshot_.bytesRead_ = Bytes;
                    });
                    if (!Opened.HasValue()) return failed(Opened.Failure());
                    if (const auto Failure = Context.operation_.Check({})) return failed(*Failure);
                    const auto Verified = Source->Verify(Context.operation_).get(); if (!Verified.HasValue()) return failed(Verified.Failure());
                    const auto& Data = Opened.Value();
                    const auto Size = Data.Bytes(); if (Size > Limits.maxDecodedBytes_) return failed({ErrorCategory::BudgetExceeded, "viewer-decoded-budget"});
                    if (Data.kind_ == ViewerKind::Image)
                    {
                        const auto Pixels = static_cast<std::uint64_t>(Data.width_) * Data.height_;
                        if (!Pixels || Pixels > Limits.maxImagePixels_ || Pixels > Limits.maxDecodedBytes_ / 4
                            || Data.pixels_.size() != Pixels * 4 || !Data.text_.empty())
                            return failed({ErrorCategory::Internal, "invalid-viewer-image"});
                    }
                    else if ((Data.kind_ != ViewerKind::Text && Data.kind_ != ViewerKind::Markdown) || !Data.pixels_.empty() || Data.width_ || Data.height_
                        || Data.text_.size() > (Data.kind_ == ViewerKind::Markdown ? Limits.maxMarkdownCharacters_ : Limits.maxTextCharacters_))
                        return failed({ErrorCategory::Internal, "invalid-viewer-text"});
                    commands_ = provider_->Commands();
                    auto Value = std::make_unique<ViewerDocument>(std::move(Opened.Value()));
                    auto Used = budget_->used_.load();
                    do { if (Size > budget_->limit_ - Used) return failed({ErrorCategory::BudgetExceeded, "viewer-total-memory-budget"}); }
                    while (!budget_->used_.compare_exchange_weak(Used, Used + Size));
                    document_ = std::shared_ptr<const ViewerDocument>(Value.release(), [Budget = budget_, Size](const ViewerDocument* Value) {
                        delete Value; Budget->used_ -= Size;
                    });
                    return JobStep{JobState::Completed};
                }
                catch (...) { return failed({ErrorCategory::Internal, "viewer-open-exception"}); }
            }
            std::vector<Error> Finalize(JobState State) override
            {
                // Archive read streams own materialized leases. Release them on
                // this worker before publishing completion to a GUI consumer.
                input_.stream_.reset(); input_.request_.source_.reset();
                std::lock_guard Lock(state_->mutex_);
                if (state_->closed_ || state_->cancelRequested_ || State == JobState::Cancelled)
                {
                    document_.reset();
                    state_->snapshot_.status_ = state_->closed_ ? ViewerStatus::Closed : ViewerStatus::Cancelled;
                    state_->snapshot_.error_ = Error{ErrorCategory::Cancelled, "viewer-cancelled"};
                    state_->opened_.set_value(*state_->snapshot_.error_);
                }
                else if (State == JobState::Completed && document_)
                {
                    state_->document_ = std::move(document_); state_->commands_ = std::move(commands_);
                    state_->snapshot_.status_ = ViewerStatus::Ready; state_->opened_.set_value(Unit{});
                }
                else
                {
                    state_->snapshot_.status_ = ViewerStatus::Failed;
                    state_->snapshot_.error_ = failure_.value_or(Error{ErrorCategory::Unavailable, "viewer-open-failed"});
                    state_->opened_.set_value(*state_->snapshot_.error_);
                }
                return {};
            }
        private:
            JobStep failed(Error Failure) { failure_ = std::move(Failure); JobStep Step{failure_->category_ == ErrorCategory::Cancelled ? JobState::Cancelled : JobState::Failed}; Step.error_ = failure_; return Step; }
            ViewerInput input_;
            std::vector<std::shared_ptr<IViewerProvider>> providers_;
            std::shared_ptr<IViewerProvider> provider_;
            std::shared_ptr<SessionState> state_;
            std::shared_ptr<MemoryBudget> budget_;
            std::shared_ptr<const ViewerDocument> document_;
            std::vector<std::string> commands_;
            std::optional<Error> failure_;
        };
        class Session final : public IViewerSession
        {
        public:
            Session(std::shared_ptr<SessionState> State, std::function<void(const std::string&)> Cancel) : state_(std::move(State)), cancel_(std::move(Cancel)) {}
            ~Session() override { Close(); }
            ViewerSnapshot Snapshot() const override { std::lock_guard Lock(state_->mutex_); return state_->snapshot_; }
            std::shared_ptr<const ViewerDocument> Document() const override { std::lock_guard Lock(state_->mutex_); return state_->document_; }
            std::vector<std::string> Commands() const override { std::lock_guard Lock(state_->mutex_); return state_->commands_; }
            void Cancel() override
            {
                std::string Id;
                { std::lock_guard Lock(state_->mutex_); if (state_->snapshot_.status_ != ViewerStatus::Loading) return; state_->cancelRequested_ = true; Id = state_->jobId_; }
                cancel_(Id);
            }
            void Close() override
            {
                std::string Id;
                { std::lock_guard Lock(state_->mutex_); if (state_->closed_) return; state_->closed_ = true; state_->document_.reset(); state_->commands_.clear(); state_->snapshot_.status_ = ViewerStatus::Closed; Id = state_->jobId_; }
                cancel_(Id);
            }
        private:
            std::shared_ptr<SessionState> state_;
            std::function<void(const std::string&)> cancel_;
        };
    }
    struct ViewerService::Backend
    {
        std::shared_ptr<ViewerRegistry> registry_;
        ViewerServiceLimits limits_;
        JobScheduler scheduler_;
        std::shared_ptr<MemoryBudget> budget_;
        std::mutex mutex_;
        std::vector<std::weak_ptr<SessionState>> sessions_;
        std::uint64_t sequence_ = 0;
        Backend(std::shared_ptr<ViewerRegistry> Registry, ViewerServiceLimits Limits) : registry_(std::move(Registry)), limits_(Limits),
            scheduler_(std::make_shared<commands::EventHub>(), Limits.workers_, 256), budget_(std::make_shared<MemoryBudget>(Limits.maxRetainedBytes_)) {}
    };
    ViewerService::ViewerService(std::shared_ptr<ViewerRegistry> Registry, ViewerServiceLimits Limits)
    {
        if (!Registry || !Limits.workers_ || Limits.workers_ > 64 || !Limits.maxSessions_
            || Limits.maxSessions_ > 256 || !Limits.maxRetainedBytes_)
            throw std::invalid_argument("invalid-viewer-service-limits");
        backend_ = std::make_shared<Backend>(std::move(Registry), Limits);
    }
    ViewerService::~ViewerService() { CancelAll(); backend_->scheduler_.Shutdown(); }
    Result<ViewerOpenHandle> ViewerService::Open(ViewerRequest Request, OperationContext Context)
    {
        if (!Request.source_ || !backend_->registry_ || !Request.limits_.maxInputBytes_ || Request.limits_.maxInputBytes_ > 128ULL * 1024 * 1024
            || !Request.limits_.maxDecodedBytes_ || Request.limits_.maxDecodedBytes_ > 128ULL * 1024 * 1024 || !Context.maxReadBytes_
            || !Request.limits_.maxTextCharacters_ || !Request.limits_.maxMarkdownCharacters_ || !Request.limits_.maxImagePixels_)
            return Error{ErrorCategory::InvalidRequest, "invalid-viewer-request"};
        EntrySnapshot Source;
        try { Source = Request.source_->Describe(); }
        catch (...) { return Error{ErrorCategory::Internal, "viewer-describe-exception"}; }
        if (Source.directory_) return Error{ErrorCategory::Unsupported, "viewer-directory"};
        if (Source.size_ > Request.limits_.maxInputBytes_) return Error{ErrorCategory::BudgetExceeded, "viewer-input-budget"};
        Collect();
        auto State = std::make_shared<SessionState>();
        {
            std::lock_guard Lock(backend_->mutex_); std::size_t Active = 0;
            std::erase_if(backend_->sessions_, [](const auto& Weak) { return Weak.expired(); });
            for (const auto& Weak : backend_->sessions_) if (const auto Value = Weak.lock()) { std::lock_guard StateLock(Value->mutex_); if (!Value->closed_) ++Active; }
            if (Active >= backend_->limits_.maxSessions_) return Error{ErrorCategory::BudgetExceeded, "viewer-session-budget"};
            State->snapshot_.id_ = "viewer-" + std::to_string(++backend_->sequence_); State->snapshot_.source_ = std::move(Source);
            backend_->sessions_.push_back(State);
        }
        auto Completion = State->opened_.get_future();
        JobRequest Job; Job.operation_ = Context;
        auto Submitted = backend_->scheduler_.Submit(Job, std::make_unique<LoadTask>(std::move(Request), backend_->registry_->Providers(), State, backend_->budget_));
        if (!Submitted.HasValue()) return Submitted.Failure();
        { std::lock_guard Lock(State->mutex_); State->jobId_ = Submitted.Value().id_; }
        std::weak_ptr<Backend> Weak = backend_;
        auto Value = std::make_shared<Session>(State, [Weak](const std::string& Id) { if (auto Backend = Weak.lock(); Backend && !Id.empty()) Backend->scheduler_.Cancel(Id); });
        return ViewerOpenHandle{std::move(Value), std::move(Completion)};
    }
    void ViewerService::CancelAll() { for (const auto& Snapshot : backend_->scheduler_.Snapshots()) if (!IsTerminal(Snapshot.state_)) backend_->scheduler_.Cancel(Snapshot.id_); }
    bool ViewerService::IsIdle() const { for (const auto& Snapshot : backend_->scheduler_.Snapshots()) if (!IsTerminal(Snapshot.state_)) return false; return true; }
    void ViewerService::Collect() { for (const auto& Snapshot : backend_->scheduler_.Snapshots()) if (IsTerminal(Snapshot.state_)) backend_->scheduler_.Forget(Snapshot.id_); }
    std::uint64_t ViewerService::RetainedBytes() const { return backend_->budget_->used_; }
}
