#include "stdafx.h"
#include "viewerHostRuntime.hpp"

/// 모듈
#include "viewerUiDispatcher.hpp"
#include "archiveCodec.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/storage/resourceQuota.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonDocument>
#include <QThread>
#include <QLoggingCategory>
#include <condition_variable>
#include <thread>

namespace hc::plugins
{
    Q_LOGGING_CATEGORY(lcViewerHostRuntime, "hc.plugins.viewer.runtime", QtWarningMsg)
    using namespace storage;
    namespace
    {
        using Clock = std::chrono::steady_clock;
        OperationContext Budget(unsigned Ms, std::stop_token Stop = {})
        { OperationContext Context; Context.stopToken_ = Stop; Context.deadline_ = Clock::now() + std::chrono::milliseconds(Ms); return Context; }
        qint64 Elapsed(Clock::time_point Start) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Start).count(); }
        struct Work
        {
            WireKind kind_;
            std::uint64_t sequence_, generation_;
            QJsonObject request_;
            QString state_ = "Queued";
            bool cancel_ = false, effects_ = false, terminal_ = false, cooperative_ = false, delivered_ = false;
            ResourceQuota::Lease input_, output_;
            QByteArray result_;
            WireKind resultKind_ = WireKind::Failure;
            Clock::time_point started_ = Clock::now();
        };
        struct Runtime
        {
            ViewerUiDispatcher ui_;
            ViewerRuntimeCallbacks callbacks_;
            HANDLE pipe_, parent_;
            DWORD parentId_;
            QString epoch_, nonce_, controlName_, phase_ = "Negotiating";
            unsigned probeMs_ = 200;
            std::mutex mutex_;
            std::condition_variable changed_;
            std::shared_ptr<Work> work_;
            std::uint64_t generation_ = 0;
            std::atomic<std::uint64_t> probeSerial_{0};
            unsigned probeFailures_ = 0;
            DWORD probeError_ = 0;
            Clock::time_point lastUi_ = Clock::now();
            bool negotiated_ = false, closing_ = false, stopAcknowledged_ = false, cleaned_ = false, fault_ = false, initialized_ = false, cooperativeViewer_ = false;
            HANDLE armedEvent_ = nullptr;
            std::uint64_t armedSequence_ = 0;
            std::stop_source stop_;
            ResourceQuota quota_{{8 * WirePayloadLimit, 2 * WirePayloadLimit, 1, 0, 0}};
            void fault()
            { { std::lock_guard Lock(mutex_); fault_ = true; closing_ = true; phase_ = "Faulted"; } changed_.notify_all(); ui_.Wake(); }
            QJsonObject snapshot()
            {
                std::lock_guard Lock(mutex_);
                QJsonObject Reply{{"epoch", epoch_}, {"generation", QString::number(generation_)}, {"phase", phase_},
                    {"probeSerial", QString::number(probeSerial_.load())}, {"uiAgeMs", Elapsed(lastUi_)}, {"probeFailures", int(probeFailures_)},
                    {"probeError", qint64(probeError_)}, {"boundarySerial", QString::number(ui_.BoundarySerial())}};
                Reply["inputAttached"] = ui_.InputAttached();
                if (work_) {
                    Reply["commandSequence"] = QString::number(work_->sequence_); Reply["kind"] = int(work_->kind_);
                    Reply["workState"] = work_->state_; Reply["workAgeMs"] = Elapsed(work_->started_);
                    Reply["cancelRequested"] = work_->cancel_; Reply["sideEffects"] = work_->effects_;
                }
                return Reply;
            }
            void complete(const std::shared_ptr<Work>& Item, Result<QJsonObject> Value, const QString& State = "Completed")
            {
                const auto Object = Value.HasValue() ? Value.Value() : QJsonObject{{"code", QString::fromStdString(Value.Failure().code_)},
                    {"category", int(Value.Failure().category_)}, {"provider", QString::fromStdString(Value.Failure().provider_.value_)},
                    {"retryable", Value.Failure().retryable_}, {"userAction", QString::fromStdString(Value.Failure().userAction_)}};
                auto Bytes = RuntimeJson(Object);
                if (Bytes.size() > WirePayloadLimit) { Value = HostError("host-wire-budget"); Bytes = RuntimeJson({{"code", "host-wire-budget"}}); }
                auto Reservation = quota_.Acquire(ResourceKind::Buffer, Bytes.size());
                std::lock_guard Lock(mutex_);
                if (Item->terminal_) return;
                if (!Reservation.HasValue()) { fault_ = true; closing_ = true; }
                else Item->output_ = std::move(Reservation.Value());
                Item->result_ = std::move(Bytes); Item->resultKind_ = Value.HasValue() ? WireKind::Response : WireKind::Failure;
                Item->state_ = State; Item->terminal_ = true;
                changed_.notify_all(); ui_.Wake();
            }
            void command(const WireFrame& Hello)
            {
                SetThreadDescription(GetCurrentThread(), L"HC viewer command I/O");
                {
                    std::unique_lock Lock(mutex_);
                    changed_.wait_for(Lock, std::chrono::seconds(5), [&] { return negotiated_ || fault_; });
                    if (!negotiated_ || fault_) { Lock.unlock(); fault(); return; }
                }
                if (!WriteFrame(pipe_, parent_, WireKind::Response, 1, RuntimeJson({{"profile", "viewer-ui/1"}, {"epoch", epoch_}}), Budget(3000)).HasValue()) { fault(); return; }
                std::uint64_t LastSequence = Hello.header_.sequence_;
                for (;;) {
                    OperationContext Context; Context.stopToken_ = stop_.get_token();
                    auto Frame = ReadFrame(pipe_, parent_, Context, false, WirePayloadLimit, 3000);
                    if (!Frame.HasValue()) { if (!stop_.stop_requested()) fault(); break; }
                    const auto& Header = Frame.Value().header_;
                    if (Header.version_ != ProtocolVersion{1, 0} || Header.sequence_ != ++LastSequence
                        || !IsRuntimeCommand(Header.kind_)) { fault(); break; }
                    auto Object = RuntimeObject(Frame.Value().payload_);
                    if (!Object.HasValue()) { fault(); break; }
                    auto Generation = WireUnsigned(Object.Value()["generation"]);
                    if (Object.Value().size() != 3 || Object.Value()["epoch"] != epoch_ || !Generation.HasValue()
                        || !Object.Value()["request"].isObject()) { fault(); break; }
                    auto Item = std::make_shared<Work>(); Item->kind_ = Header.kind_; Item->sequence_ = Header.sequence_;
                    Item->generation_ = Generation.Value(); Item->request_ = Object.Value()["request"].toObject();
                    auto Reservation = quota_.Acquire(ResourceKind::Memory, Frame.Value().payload_.size());
                    if (!Reservation.HasValue()) { fault(); break; }
                    Item->input_ = std::move(Reservation.Value());
                    {
                        std::lock_guard Lock(mutex_);
                        const auto Expected = generation_ + (Header.kind_ == WireKind::ViewerOpen ? 1 : 0);
                        if (closing_ || Item->generation_ != Expected || (work_ && !work_->terminal_)
                            || (!initialized_ && Header.kind_ != WireKind::Hello) || (initialized_ && Header.kind_ == WireKind::Hello)) {
                            fault_ = true; closing_ = true; changed_.notify_all(); ui_.Wake(); break;
                        }
                        generation_ = Expected; work_ = Item;
                        Item->cooperative_ = cooperativeViewer_ && (Header.kind_ == WireKind::ViewerOpen || Header.kind_ == WireKind::MetadataQuery || Header.kind_ == WireKind::ContentConvert);
                    }
                    ui_.Wake();
                    {
                        std::unique_lock Lock(mutex_);
                        changed_.wait(Lock, [&] { return Item->terminal_ || fault_ || stop_.stop_requested(); });
                        if (!Item->terminal_) break;
                    }
                    if (!WriteFrame(pipe_, parent_, Item->resultKind_, Header.sequence_, Item->result_, Budget(3000, stop_.get_token())).HasValue()) {
                        if (!stop_.stop_requested()) fault(); break;
                    }
                    { std::lock_guard Lock(mutex_); Item->delivered_ = true; Item->input_.Reset(); Item->output_.Reset(); Item->request_ = {}; Item->result_.clear(); }
                    changed_.notify_all(); ui_.Wake();
                }
            }
            void control()
            {
                SetThreadDescription(GetCurrentThread(), L"HC viewer control I/O");
                WinHandle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(controlName_.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
                ULONG Server = 0;
                if (!Pipe.Valid() || !GetNamedPipeServerProcessId(Pipe.Get(), &Server) || Server != parentId_) { fault(); return; }
                const QJsonObject Hello{{"profile", "viewer-ui/1"}, {"nonce", nonce_}, {"epoch", epoch_}, {"processId", qint64(GetCurrentProcessId())}};
                if (!WriteFrame(Pipe.Get(), parent_, WireKind::ControlHello, 1, RuntimeJson(Hello), Budget(5000)).HasValue()) { fault(); return; }
                auto Reply = ReadFrame(Pipe.Get(), parent_, Budget(5000), false, ViewerControlLimit, 1000);
                if (!Reply.HasValue() || Reply.Value().header_.kind_ != WireKind::Response || Reply.Value().header_.sequence_ != 1
                    || Reply.Value().header_.version_ != ProtocolVersion{1, 0} || Reply.Value().payload_ != RuntimeJson({{"epoch", epoch_}})) { fault(); return; }
                { std::lock_guard Lock(mutex_); negotiated_ = true; } changed_.notify_all();
                std::uint64_t Sequence = 1; auto NextProbe = Clock::now();
                while (!stop_.stop_requested()) {
                    if (ui_.QuitRequested()) { fault(); break; }
                    if (WaitForSingleObject(parent_, 0) == WAIT_OBJECT_0) { fault(); break; }
                    if (Clock::now() >= NextProbe) {
                        const auto Probe = ui_.Probe(++probeSerial_);
                        { std::lock_guard Lock(mutex_); probeError_ = Probe.error_;
                            if (Probe.acknowledged_) { lastUi_ = Clock::now(); probeFailures_ = 0; } else ++probeFailures_; }
                        NextProbe = Clock::now() + std::chrono::milliseconds(probeMs_);
                    }
                    DWORD Available = 0;
                    if (!PeekNamedPipe(Pipe.Get(), nullptr, 0, nullptr, &Available, nullptr)) { fault(); break; }
                    if (!Available) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                    auto Request = ReadFrame(Pipe.Get(), parent_, Budget(500, stop_.get_token()), false, ViewerControlLimit, 500);
                    if (!Request.HasValue()) { if (!stop_.stop_requested()) fault(); break; }
                    const auto& Header = Request.Value().header_;
                    auto Value = RuntimeObject(Request.Value().payload_);
                    if (Header.version_ != ProtocolVersion{1, 0} || Header.sequence_ != ++Sequence || !Value.HasValue()
                        || Value.Value()["epoch"] != epoch_ || !IsRuntimeControl(Header.kind_) || Header.kind_ == WireKind::ControlHello) { fault(); break; }
                    QJsonObject Response;
                    if (Header.kind_ == WireKind::ControlCancel) {
                        const auto Target = WireUnsigned(Value.Value()["commandSequence"]), Generation = WireUnsigned(Value.Value()["generation"]);
                        if (Value.Value().size() != 3 || !Target.HasValue() || !Generation.HasValue()) { fault(); break; }
                        std::shared_ptr<Work> Cancelled;
                        { std::lock_guard Lock(mutex_); QString Status = "Unknown";
                            if (work_ && work_->sequence_ == Target.Value() && work_->generation_ == Generation.Value()) {
                                if (work_->terminal_) Status = work_->state_;
                                else {
                                    work_->cancel_ = true;
                                    if (work_->state_ == "Queued") { work_->state_ = "Cancelling"; Cancelled = work_; Status = "CancelledBeforeStart"; }
                                    else if (work_->cooperative_) {
                                        if (armedSequence_ == work_->sequence_ && armedEvent_) SetEvent(armedEvent_);
                                        Status = "CancellationRequested";
                                    }
                                    else Status = "CancellationNotSupported";
                                }
                            }
                            Response = {{"epoch", epoch_}, {"cancelState", Status}};
                        }
                        if (Cancelled) complete(Cancelled, Error{ErrorCategory::Cancelled, "host-cancelled-before-start"}, "Cancelled");
                    } else {
                        if (Value.Value().size() != 1) { fault(); break; }
                        if (Header.kind_ == WireKind::ControlStop) {
                            { std::lock_guard Lock(mutex_); closing_ = true; phase_ = "Closing"; } ui_.Wake();
                        }
                        Response = snapshot();
                    }
                    if (!WriteFrame(Pipe.Get(), parent_, WireKind::Response, Header.sequence_, RuntimeJson(Response), Budget(500, stop_.get_token())).HasValue()) {
                        if (!stop_.stop_requested()) fault(); break;
                    }
                    if (Header.kind_ == WireKind::ControlStop) {
                        { std::lock_guard Lock(mutex_); stopAcknowledged_ = true; } ui_.Wake();
                    }
                }
            }
        };
    }
    int RunViewerHostRuntime(HANDLE Pipe, HANDLE Parent, DWORD ParentId, const WireFrame& Hello, ViewerRuntimeCallbacks Callbacks)
    {
        auto Declaration = RuntimeObject(Hello.payload_);
        if (!Declaration.HasValue() || Hello.header_.sequence_ != 1 || Hello.header_.version_ != ProtocolVersion{1, 0}) return 7;
        const auto Object = Declaration.Value();
        if (Object.size() != 5 || Object["profile"] != "viewer-ui/1" || !RuntimeIdentity(Object["nonce"].toString())
            || !RuntimeIdentity(Object["epoch"].toString()) || !Object["control"].toString().startsWith("\\\\.\\pipe\\hyperCommander-")
            || Object["control"].toString().size() > 200 || Object["heartbeatMs"].toInt() < 100 || Object["heartbeatMs"].toInt() > 60000) return 7;
        Runtime State; State.pipe_ = Pipe; State.parent_ = Parent; State.parentId_ = ParentId;
        State.callbacks_ = std::move(Callbacks); State.epoch_ = Object["epoch"].toString(); State.nonce_ = Object["nonce"].toString();
        State.controlName_ = Object["control"].toString(); State.probeMs_ = std::min(100, Object["heartbeatMs"].toInt() / 2);
        if (!State.ui_.Initialize()) return 12;
        // All threads exist before the first DLL is loaded. Their exits happen
        // after UI cleanup, so ordinary DLL_THREAD_DETACH cannot precede Uninit.
        std::thread Command, Control;
        try { Command = std::thread([&] { State.command(Hello); }); Control = std::thread([&] { State.control(); }); }
        catch (const std::system_error&) {
            State.fault(); State.stop_.request_stop(); State.changed_.notify_all();
            if (Command.joinable()) Command.join(); if (Control.joinable()) Control.join(); return 12;
        }
        for (;;) {
            std::shared_ptr<Work> Item; bool Closing = false, Fault = false, StopAcknowledged = false;
            { std::lock_guard Lock(State.mutex_); Fault = State.fault_; Closing = State.closing_;
                StopAcknowledged = State.stopAcknowledged_;
                if (State.work_ && !State.work_->terminal_ && State.work_->state_ == "Queued") {
                    Item = State.work_; Item->state_ = "Running"; Item->started_ = Clock::now();
                    if (Item->kind_ == WireKind::Hello) State.phase_ = "Initializing";
                }
            }
            if (State.ui_.QuitRequested()) { if (!Fault) State.fault(); Fault = true; }
            if (Fault) {
                // The parent owns fault Job reclamation. Avoid SDK Close or
                // FreeLibrary on this path; ExitProcess can itself hang in a
                // detach routine, still bounded by the parent's Job watchdog.
                ExitProcess(6);
            }
            if (Closing) {
                if (Item) State.complete(Item, Error{ErrorCategory::Cancelled, "host-runtime-closing"}, "Cancelled");
                if (!StopAcknowledged) { State.ui_.Cycle(true); continue; }
                { ViewerUiDispatcher::Request Request(State.ui_); State.callbacks_.cleanup_(); }
                // Widget deletion queued by Uninit must run while its DLL is
                // still loaded, at a genuine boundary with requestDepth == 0.
                State.ui_.Cycle(false);
                { ViewerUiDispatcher::Request Request(State.ui_); State.callbacks_.unload_(); }
                { std::lock_guard Lock(State.mutex_); State.cleaned_ = true; State.phase_ = "Stopped"; State.armedEvent_ = nullptr; State.armedSequence_ = 0; }
                break;
            }
            if (Item) {
                ViewerUiDispatcher::Request Request(State.ui_);
                State.callbacks_.prepare_(Item->kind_);
                bool Cancel = false; std::optional<Error> ArmFailure;
                { std::lock_guard Lock(State.mutex_); Item->effects_ = Item->kind_ == WireKind::ViewerOpen;
                    Cancel = Item->cancel_;
                    if (!Cancel) {
                        auto Armed = State.callbacks_.arm_(Item->kind_);
                        if (!Armed.HasValue()) ArmFailure = Armed.Failure();
                        else { State.armedEvent_ = Armed.Value(); State.armedSequence_ = Item->sequence_; }
                    }
                }
                const bool CancelledBeforeSdk = Cancel;
                auto Value = Cancel ? storage::Result<QJsonObject>(Error{ErrorCategory::Cancelled, "host-cancelled", {}, {}, false, "previous-viewer-retired"})
                    : ArmFailure ? storage::Result<QJsonObject>(*ArmFailure) : State.callbacks_.execute_(Item->kind_, Item->request_);
                { std::lock_guard Lock(State.mutex_); State.armedEvent_ = nullptr; State.armedSequence_ = 0; Cancel = Item->cancel_;
                    if (Item->kind_ == WireKind::Hello && Value.HasValue()) {
                        State.initialized_ = true; State.phase_ = "Ready"; State.cooperativeViewer_ = Value.Value()["viewerCancellation"].toBool();
                    }
                }
                State.callbacks_.finish_(Item->kind_, Cancel);
                if (Cancel && Item->kind_ == WireKind::ViewerOpen && Value.HasValue()) {
                    auto Reply = Value.Value(); Reply["cancelRequested"] = true; Reply["viewerReusable"] = false; Value = Reply;
                }
                State.complete(Item, std::move(Value), CancelledBeforeSdk ? "Cancelled" : "Completed");
            }
            State.ui_.Cycle(!Item);
        }
        const auto DeliveryDeadline = Clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            bool Pending = false;
            { std::lock_guard Lock(State.mutex_); Pending = !State.fault_ && State.work_ && State.work_->terminal_ && !State.work_->delivered_; }
            if (!Pending || Clock::now() >= DeliveryDeadline) break;
            State.ui_.Cycle(false); Sleep(1);
        }
        State.stop_.request_stop(); State.changed_.notify_all(); State.ui_.Wake();
        // PINned DLLs may still receive thread detach. Never wait indefinitely
        // without pumping the UI probe. The parent's close budget remains final.
        const auto Until = Clock::now() + std::chrono::milliseconds(1000);
        while ((WaitForSingleObject(Command.native_handle(), 0) == WAIT_TIMEOUT || WaitForSingleObject(Control.native_handle(), 0) == WAIT_TIMEOUT)
            && Clock::now() < Until) { State.ui_.Cycle(false); Sleep(1); }
        if (WaitForSingleObject(Command.native_handle(), 0) != WAIT_OBJECT_0 || WaitForSingleObject(Control.native_handle(), 0) != WAIT_OBJECT_0) ExitProcess(12);
        Command.join(); Control.join();
        return 0; // Probe HWND is destroyed here, after both I/O threads.
    }
}
