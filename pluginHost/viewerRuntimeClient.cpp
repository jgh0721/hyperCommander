#include "stdafx.h"
#include "viewerRuntimeClient.hpp"

/// 모듈
#include "viewerHostRuntime.hpp"
#include "archiveCodec.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/storage/resourceQuota.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <condition_variable>
#include <deque>
#include <thread>
#include <system_error>
#include <QLoggingCategory>

namespace hc::plugins
{
    Q_LOGGING_CATEGORY(lcViewerRuntimeClient, "hc.plugins.viewer.transport", QtWarningMsg)
    using namespace storage;
    namespace
    {
        using Clock = std::chrono::steady_clock;
        unsigned ProbePeriod(const HostLimits& Limits) { return std::min(100u, Limits.heartbeatMs_ / 2); }
        OperationContext Budget(unsigned Ms, std::stop_token Stop)
        { OperationContext Value; Value.stopToken_ = Stop; Value.deadline_ = Clock::now() + std::chrono::milliseconds(Ms); return Value; }
    }
    struct ViewerRuntimeClient::State
    {
        struct Command
        {
            WireKind kind_;
            std::uint64_t sequence_, generation_ = 0;
            QJsonObject request_;
            bool sent_ = false, done_ = false, detached_ = false, cancel_ = false;
            Clock::time_point sentAt_{}, terminalAt_{};
            Result<QJsonObject> result_ = HostError("host-no-response");
            ResourceQuota::Lease input_, output_;
        };
        HANDLE pipe_, control_, peer_, job_;
        WinHandle ownedPipe_, ownedControl_, ownedPeer_, ownedJob_;
        QString epoch_;
        HostLimits limits_;
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::deque<std::shared_ptr<Command>> queue_;
        std::shared_ptr<Command> current_;
        std::shared_ptr<Command> detached_;
        std::uint64_t generation_ = 0, sequence_ = 1;
        bool closing_ = false, closed_ = false, forced_ = false;
        std::optional<Error> failure_;
        QJsonObject observation_;
        QString cancelState_;
        HostSnapshot cached_;
        Clock::time_point faultAt_{}, closeAt_{};
        QJsonObject timings_;
        std::stop_source stop_;
        ResourceQuota quota_{{16 * WirePayloadLimit, 2 * WirePayloadLimit, 1, 0, 0}};
        bool stopping() { std::lock_guard Lock(mutex_); return closing_ || stop_.stop_requested(); }
        void refresh()
        {
            HostSnapshot Value; Value.processId_ = GetProcessId(peer_);
            Value.running_ = WaitForSingleObject(peer_, 0) == WAIT_TIMEOUT;
            if (!Value.running_) { DWORD Exit = 0; if (GetExitCodeProcess(peer_, &Exit)) Value.exitCode_ = Exit; }
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
            if (QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr)) {
                Value.activeProcesses_ = Accounting.ActiveProcesses; Value.totalProcesses_ = Accounting.TotalProcesses;
            }
            std::lock_guard Lock(mutex_); cached_ = Value;
            if (!Value.running_) {
                const auto Now = Clock::now();
                if (faultAt_ != Clock::time_point{} && !timings_.contains("faultReapMs")) timings_["faultReapMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(Now - faultAt_).count();
                if (closeAt_ != Clock::time_point{} && !timings_.contains("closeExitMs")) timings_["closeExitMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(Now - closeAt_).count();
            }
        }
        HostSnapshot cached() const
        {
            std::lock_guard Lock(mutex_); auto Value = cached_;
            Value.failure_ = failure_; Value.forcedTermination_ = forced_; Value.busy_ = bool(current_);
            // The process handle can become signalled before either I/O owner
            // classifies EOF. A value-only observer must already report loss.
            if (!Value.running_ && Value.processId_ && !closing_ && !Value.failure_) Value.failure_ = HostError("host-exited");
            Value.runtime_ = observation_; Value.callerDetached_ = current_ && current_->detached_;
            Value.runtime_["clientQueued"] = qint64(queue_.size());
            if (!cancelState_.isEmpty()) Value.runtime_["cancelState"] = cancelState_;
            Value.runtime_["timings"] = timings_;
            if (current_ && current_->done_ && !current_->result_.HasValue() && current_->result_.Failure().userAction_ == "result-unknown") Value.runtime_["workState"] = "Lost";
            return Value;
        }
        void fault(Error Error)
        {
            const auto Detected = Clock::now();
            const auto Exited = WaitForSingleObject(peer_, Error.code_ == "host-ui-unresponsive" || Error.code_ == "host-user-force-stop" ? 0 : 50) == WAIT_OBJECT_0;
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
            const bool Helpers = QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr) && Accounting.ActiveProcesses > (Exited ? 0u : 1u);
            { std::lock_guard Lock(mutex_);
                if (closed_ || failure_) return;
                failure_ = std::move(Error);
                if (faultAt_ == Clock::time_point{}) faultAt_ = Detected;
                if (!closing_ && (!Exited || Helpers)) { forced_ = true; TerminateJobObject(job_, 0xe0000001); }
                if (current_ && !current_->done_) {
                    current_->result_ = storage::Error{ErrorCategory::Unavailable, "host-command-lost", {"plugin-host"}, {}, false, "result-unknown"};
                    current_->done_ = true;
                }
                for (auto& Item : queue_) { Item->result_ = *failure_; Item->done_ = true; }
                queue_.clear();
            }
            refresh(); changed_.notify_all(); stop_.request_stop();
            const auto Value = cached();
            qCWarning(lcViewerRuntimeClient) << "Viewer runtime fault" << epoch_ << Value.runtime_["generation"] << Value.runtime_["commandSequence"]
                << Value.runtime_["kind"] << QString::fromStdString(Value.failure_ ? Value.failure_->code_ : "host-closed") << Value.runtime_;
        }
        void commands()
        {
            for (;;) {
                std::shared_ptr<Command> Item;
                { std::unique_lock Lock(mutex_); changed_.wait(Lock, [&] { return closing_ || failure_ || !queue_.empty() || stop_.stop_requested(); });
                    if (closing_ || failure_ || stop_.stop_requested()) return;
                    if (detached_ && detached_->done_) detached_.reset();
                    Item = queue_.front(); queue_.pop_front(); current_ = Item;
                    Item->sequence_ = ++sequence_;
                    if (Item->kind_ == WireKind::ViewerOpen) ++generation_;
                    Item->generation_ = generation_; Item->sent_ = true;
                }
                const auto Envelope = RuntimeJson({{"epoch", epoch_}, {"generation", QString::number(Item->generation_)}, {"request", Item->request_}});
                auto Sent = WriteFrame(pipe_, peer_, Item->kind_, Item->sequence_, Envelope, Budget(limits_.requestMs_, stop_.get_token()));
                if (!Sent.HasValue()) { fault(Sent.Failure()); return; }
                { std::lock_guard Lock(mutex_); Item->sentAt_ = Clock::now(); }
                OperationContext Receive; Receive.stopToken_ = stop_.get_token();
                auto Output = quota_.Acquire(ResourceKind::Buffer, WirePayloadLimit, Receive);
                if (!Output.HasValue()) { fault(Output.Failure()); return; }
                Item->output_ = std::move(Output.Value());
                // The detached caller cannot cancel this read. It drains even
                // a 768 KiB late document response before sending another work.
                auto Frame = ReadFrame(pipe_, peer_, Receive, false, WirePayloadLimit, 3000);
                if (!Frame.HasValue()) { fault(Frame.Failure()); return; }
                const auto& Header = Frame.Value().header_;
                if (Header.sequence_ != Item->sequence_ || Header.version_ != ProtocolVersion{1, 0}
                    || (Header.kind_ != WireKind::Response && Header.kind_ != WireKind::Failure)) { fault(HostError("host-response-mismatch")); return; }
                auto Object = RuntimeObject(Frame.Value().payload_);
                if (!Object.HasValue()) { fault(Object.Failure()); return; }
                Result<QJsonObject> Value = Object.Value();
                if (Header.kind_ == WireKind::Response && Item->kind_ == WireKind::Hello
                    && (!Object.Value()["inJob"].toBool() || !Object.Value()["adminDenied"].toBool() || !Object.Value()["privilegesRemoved"].toBool())) {
                    fault(HostError("host-restriction-verification")); return;
                }
                if (Header.kind_ == WireKind::Failure) {
                    const auto Code = Object.Value()["code"].toString(); const auto Category = Object.Value()["category"].toInt(int(ErrorCategory::Unavailable));
                    if (Code.isEmpty() || Code.size() > 1024 || Category < 0 || Category > int(ErrorCategory::Internal)) { fault(HostError("host-failure-payload")); return; }
                    Value = Error{ErrorCategory(Category), Code.toStdString(), {Object.Value()["provider"].toString("plugin-host").toStdString()}, {},
                        Object.Value()["retryable"].toBool(), Object.Value()["userAction"].toString().toStdString()};
                }
                { std::lock_guard Lock(mutex_);
                    // A control fault can win after ReadFrame completed but
                    // before JSON decoding did. Never replace its Lost terminal
                    // result after a subscriber has already observed it.
                    if (!Item->done_) { Item->result_ = std::move(Value); Item->done_ = true; }
                    Item->input_.Reset(); Item->request_ = {}; current_.reset(); }
                changed_.notify_all();
            }
        }
        void controls()
        {
            std::uint64_t Sequence = 1; bool StopSent = false;
            auto LastControl = Clock::now();
            while (!stop_.stop_requested()) {
                WireKind Kind = WireKind::ControlPing; QJsonObject Request{{"epoch", epoch_}};
                { std::lock_guard Lock(mutex_);
                    if (closing_ && !StopSent) { Kind = WireKind::ControlStop; StopSent = true; }
                    else if (current_ && current_->cancel_) {
                        Kind = WireKind::ControlCancel; Request["generation"] = QString::number(current_->generation_);
                        Request["commandSequence"] = QString::number(current_->sequence_); current_->cancel_ = false;
                    }
                }
                // Keep one response outstanding. A transient delay never starts
                // another reader or leaves a partial frame for the next ping.
                const auto Silence = limits_.heartbeatMs_ + 500;
                auto Sent = WriteFrame(control_, peer_, Kind, ++Sequence, RuntimeJson(Request), Budget(std::min(500u, Silence), stop_.get_token()));
                if (!Sent.HasValue()) { if (!stopping()) fault(Sent.Failure()); return; }
                const auto Missing = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - LastControl).count();
                const auto Remaining = unsigned(std::max<qint64>(100, Silence - Missing));
                auto Frame = ReadFrame(control_, peer_, Budget(Remaining, stop_.get_token()), false, ViewerControlLimit, 500);
                if (!Frame.HasValue()) { if (!stopping()) fault(Frame.Failure()); return; }
                auto Reply = RuntimeObject(Frame.Value().payload_);
                if (!Reply.HasValue() || Frame.Value().header_.kind_ != WireKind::Response || Frame.Value().header_.sequence_ != Sequence
                    || Frame.Value().header_.version_ != ProtocolVersion{1, 0} || Reply.Value()["epoch"] != epoch_) { fault(HostError("host-control-envelope")); return; }
                LastControl = Clock::now();
                refresh();
                if (Kind == WireKind::ControlCancel) {
                    const auto Status = Reply.Value()["cancelState"].toString();
                    if (Status.isEmpty() || Status.size() > 64) { fault(HostError("host-control-cancel")); return; }
                    std::lock_guard Lock(mutex_); cancelState_ = Status;
                } else {
                    const auto Value = Reply.Value();
                    const auto Age = Value["uiAgeMs"].toInteger(-1);
                    const auto Failures = Value["probeFailures"].toInt(-1);
                    const auto WorkAge = Value["workAgeMs"].toInteger(-1); const auto Running = Value["workState"] == "Running";
                    const auto Phase = Value["phase"].toString();
                    const auto Generation = WireUnsigned(Value["generation"]);
                    const auto WorkState = Value["workState"].toString();
                    const auto CommandSequence = WireUnsigned(Value["commandSequence"]);
                    if (Age < 0 || Failures < 0 || !Generation.HasValue() || !WireUnsigned(Value["probeSerial"]).HasValue()
                        || !WireUnsigned(Value["boundarySerial"]).HasValue() || (Running && WorkAge < 0)
                        || (!WorkState.isEmpty() && (!CommandSequence.HasValue() || !QStringList{"Queued", "Cancelling", "Running", "Completed", "Cancelled"}.contains(WorkState)))
                        || !QStringList{"Negotiating", "Initializing", "Ready", "Closing", "Stopped", "Faulted"}.contains(Phase)) {
                        fault(HostError("host-control-snapshot")); return;
                    }
                    if (Phase == "Faulted") { fault(HostError("host-runtime-fault")); return; }
                    // Native/Qt callbacks can keep UI main busy after the SDK
                    // returned (for example initial text layout). The old idle
                    // heartbeat also waited under requestMs, so apply that same
                    // configured UI budget rather than the control-silence budget.
                    // Communication receipt/delivery still has separate bounds.
                    const auto Initializing = Phase == "Negotiating" || Phase == "Initializing" || (Running && Value["kind"].toInt() == int(WireKind::Hello));
                    const auto Base = Initializing ? limits_.startupMs_ : limits_.requestMs_;
                    // startupMs is the DLL initialization allowance, not a
                    // total including a probe/reap reserve. In particular a
                    // valid 750 ms DLL attach must fit a configured 1000 ms.
                    const auto Threshold = Initializing ? Base
                        : std::max(100u, Base > 2 * ProbePeriod(limits_) + 100 ? Base - 2 * ProbePeriod(limits_) - 100 : 100);
                    bool Closing = false;
                    const char* TransportFailure = nullptr;
                    { std::lock_guard Lock(mutex_); observation_ = Value; Closing = closing_;
                        const auto Now = Clock::now();
                        if (Generation.Value() > generation_ || (CommandSequence.HasValue() && CommandSequence.Value() > sequence_))
                            TransportFailure = "host-control-work-identity";
                        if (!Closing && current_ && !current_->done_ && current_->sentAt_ != Clock::time_point{}) {
                            const auto Accepted = CommandSequence.HasValue() && CommandSequence.Value() == current_->sequence_;
                            if (Accepted && Generation.Value() != current_->generation_) TransportFailure = "host-control-work-identity";
                            const auto Wait = std::chrono::duration_cast<std::chrono::milliseconds>(Now - current_->sentAt_).count();
                            // Receipt is performed by Command I/O, including while
                            // UI main is in a native menu. It has a transport budget,
                            // independent of an arbitrarily long responsive SDK call.
                            if (!Accepted && Wait >= std::clamp(limits_.requestMs_, 500u, 3000u)) {
                                TransportFailure = "host-command-acceptance-stalled"; timings_["commandAcceptWaitMs"] = Wait;
                            }
                            if (Accepted && (WorkState == "Completed" || WorkState == "Cancelled")) {
                                if (current_->terminalAt_ == Clock::time_point{}) current_->terminalAt_ = Now;
                                const auto Delivery = std::chrono::duration_cast<std::chrono::milliseconds>(Now - current_->terminalAt_).count();
                                // Match the frame-completion bound. Terminal bytes
                                // have already been serialized by the host here.
                                if (Delivery >= 3000) { TransportFailure = "host-command-delivery-stalled"; timings_["terminalDeliveryWaitMs"] = Delivery; }
                            }
                        }
                    }
                    if (TransportFailure) { fault(HostError(TransportFailure)); return; }
                    if (!Closing && Failures >= 2 && (Running ? std::min(Age, WorkAge) : Age) >= Threshold) {
                        { std::lock_guard Lock(mutex_); timings_["detectUiAgeMs"] = Age; timings_["detectWorkAgeMs"] = WorkAge;
                            timings_["thresholdMs"] = int(Threshold); timings_["baseBudgetMs"] = int(Base); }
                        fault(HostError("host-ui-unresponsive")); return;
                    }
                }
                std::unique_lock Lock(mutex_);
                changed_.wait_for(Lock, std::chrono::milliseconds(ProbePeriod(limits_)), [&] {
                    return stop_.stop_requested() || (closing_ && !StopSent) || (current_ && current_->cancel_);
                });
            }
        }
    };
    ViewerRuntimeClient::ViewerRuntimeClient(HANDLE Pipe, HANDLE Control, HANDLE Peer, HANDLE Job, QString Epoch, HostLimits Limits)
        : state_(std::make_shared<State>())
    {
        auto& State = *state_;
        const auto Duplicate = [](HANDLE Original, WinHandle& Owner) {
            HANDLE Value = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), Original, GetCurrentProcess(), &Value, 0, FALSE, DUPLICATE_SAME_ACCESS))
                throw std::system_error(int(GetLastError()), std::system_category(), "host-runtime-handle");
            Owner.Reset(Value); return Value;
        };
        State.pipe_ = Duplicate(Pipe, State.ownedPipe_); State.control_ = Duplicate(Control, State.ownedControl_);
        State.peer_ = Duplicate(Peer, State.ownedPeer_); State.job_ = Duplicate(Job, State.ownedJob_);
        State.epoch_ = std::move(Epoch); State.limits_ = Limits;
        State.refresh();
        try {
            command_ = std::jthread([State = state_] { State->commands(); });
            control_ = std::jthread([State = state_] { State->controls(); });
        } catch (...) {
            State.stop_.request_stop(); State.changed_.notify_all();
            if (command_.joinable()) command_.join();
            throw;
        }
    }
    ViewerRuntimeClient::~ViewerRuntimeClient() { Close(); }
    Result<QJsonObject> ViewerRuntimeClient::Call(WireKind Kind, std::uint64_t Sequence, const QByteArray& Payload, OperationContext Context)
    {
        if (!IsRuntimeCommand(Kind)) return Error{ErrorCategory::InvalidRequest, "host-runtime-command"};
        auto Item = std::make_shared<State::Command>(); Item->kind_ = Kind; Item->sequence_ = Sequence;
        if (!Payload.isEmpty()) { auto Value = RuntimeObject(Payload); if (!Value.HasValue()) return Value.Failure(); Item->request_ = Value.Value(); }
        if (RuntimeJson({{"epoch", state_->epoch_}, {"generation", QString::number(UINT64_MAX)}, {"request", Item->request_}}).size() > WirePayloadLimit)
            return Error{ErrorCategory::BudgetExceeded, "host-wire-budget"};
        auto Reservation = state_->quota_.Acquire(ResourceKind::Memory, Payload.size(), Context);
        if (!Reservation.HasValue()) return Reservation.Failure(); Item->input_ = std::move(Reservation.Value());
        std::unique_lock Lock(state_->mutex_);
        if (state_->failure_) return *state_->failure_;
        if (state_->closing_) return HostError("host-closed");
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "host-command-queue-full"};
        state_->queue_.push_back(Item); state_->changed_.notify_all();
        for (;;) {
            if (Item->done_) return Item->result_;
            if (const auto Stop = Context.Check({"plugin-host"})) {
                if (!Item->sent_) {
                    std::erase(state_->queue_, Item); Item->input_.Reset();
                    return Error{Stop->category_, "host-cancelled-before-start", {"plugin-host"}};
                }
                Item->detached_ = true; Item->cancel_ = true; state_->detached_ = Item; state_->changed_.notify_all();
                return Error{Stop->category_, "host-caller-detached", {"plugin-host"}, {}, false, "request-still-running"};
            }
            if (state_->failure_) return *state_->failure_;
            state_->changed_.wait_for(Lock, std::chrono::milliseconds(10));
        }
    }
    Result<QJsonObject> ViewerRuntimeClient::AwaitPending(OperationContext Context, WireKind* Kind)
    {
        std::unique_lock Lock(state_->mutex_); auto Item = state_->detached_;
        if (!Item) return Error{ErrorCategory::InvalidRequest, "host-no-detached-command"};
        if (Kind) *Kind = Item->kind_;
        while (!Item->done_) {
            if (const auto Stop = Context.Check({"plugin-host"})) return Error{Stop->category_, "host-caller-detached", {"plugin-host"}, {}, false, "request-still-running"};
            state_->changed_.wait_for(Lock, std::chrono::milliseconds(10));
        }
        if (state_->detached_ == Item) state_->detached_.reset();
        return Item->result_;
    }
    HostSnapshot ViewerRuntimeClient::Snapshot() const
    {
        state_->refresh(); return state_->cached();
    }
    std::function<HostSnapshot()> ViewerRuntimeClient::Observe() const
    {
        return [Weak = std::weak_ptr<State>(state_)] {
            if (const auto State = Weak.lock()) return State->cached();
            return HostSnapshot{};
        };
    }
    std::function<void()> ViewerRuntimeClient::ForceStopSource() const
    {
        return [Weak = std::weak_ptr<State>(state_)] {
            if (const auto State = Weak.lock()) State->fault(Error{ErrorCategory::Cancelled, "host-user-force-stop", {"plugin-host"}, {}, false, "result-unknown"});
        };
    }
    void ViewerRuntimeClient::Close()
    {
        std::lock_guard CloseLock(closeMutex_);
        const auto Started = Clock::now();
        { std::lock_guard Lock(state_->mutex_); if (state_->closed_) return;
            state_->closeAt_ = Started;
            state_->closing_ = true;
            for (auto& Item : state_->queue_) { Item->done_ = true; Item->result_ = Error{ErrorCategory::Cancelled, "host-runtime-closing"}; }
            state_->queue_.clear(); }
        state_->changed_.notify_all();
        if (WaitForSingleObject(state_->peer_, 500) == WAIT_TIMEOUT) {
            { std::lock_guard Lock(state_->mutex_); state_->forced_ = true; }
            TerminateJobObject(state_->job_, 0);
        }
        const auto Remaining = [&] { return DWORD(std::max<qint64>(0, 2700 - std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Started).count())); };
        WaitForSingleObject(state_->peer_, std::min(DWORD(2000), Remaining()));
        // Reap helpers as part of the same budget; no independent extra grace.
        TerminateJobObject(state_->job_, 0);
        while (Remaining()) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
            if (!QueryInformationJobObject(state_->job_, JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr) || !Accounting.ActiveProcesses) break;
            Sleep(5);
        }
        state_->stop_.request_stop(); state_->changed_.notify_all();
        if (command_.joinable()) command_.join(); if (control_.joinable()) control_.join();
        state_->refresh();
        { std::lock_guard Lock(state_->mutex_); state_->closed_ = true; }
    }
}
