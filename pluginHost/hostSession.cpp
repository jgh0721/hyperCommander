#include "hostSession.hpp"
#include "archiveCodec.hpp"
#include <QJsonDocument>
#include <QUuid>
#include <atomic>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        OperationContext IoContext()
        { OperationContext Value; Value.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3); return Value; }
        QByteArray Json(const QJsonObject& Value) { return QJsonDocument(Value).toJson(QJsonDocument::Compact); }
        QByteArray FailureBytes(const Error& Failure)
        { return Json({{"code", QString::fromStdString(Failure.code_)}, {"category", int(Failure.category_)}, {"provider", QString::fromStdString(Failure.provider_.value_)}, {"retryable", Failure.retryable_}, {"userAction", QString::fromStdString(Failure.userAction_)}}); }
    }
    QByteArray EncodeSessionPayload(const QJsonObject& Envelope, const QByteArray& Binary)
    {
        const auto Header = Json(Envelope); QByteArray Output(4, '\0');
        for (unsigned Index = 0; Index < 4; ++Index) Output[Index] = char((std::uint32_t(Header.size()) >> (Index * 8)) & 255);
        Output += Header; Output += Binary; return Output;
    }
    Result<std::pair<QJsonObject, QByteArray>> DecodeSessionPayload(const QByteArray& Payload)
    {
        if (Payload.size() < 4 || Payload.size() > WirePayloadLimit) return HostError("session-envelope-size");
        std::uint32_t Length = 0;
        for (unsigned Index = 0; Index < 4; ++Index) Length |= std::uint32_t(static_cast<unsigned char>(Payload[Index])) << (Index * 8);
        if (Length > std::uint32_t(Payload.size() - 4)) return HostError("session-envelope-size");
        QJsonParseError Parse; const auto Doc = QJsonDocument::fromJson(Payload.mid(4, Length), &Parse);
        if (Parse.error != QJsonParseError::NoError || !Doc.isObject()) return HostError("session-envelope-json");
        return std::pair{Doc.object(), Payload.mid(4 + Length)};
    }
    struct HostSession::Work
    {
        WireKind kind_;
        QJsonObject request_;
        QByteArray binary_;
        OperationContext context_;
        std::stop_source stop_;
        std::atomic<bool> paused_ = false;
        std::mutex mutex_;
        std::condition_variable changed_;
        std::optional<Result<QByteArray>> result_;
        std::optional<ProviderInteractionRequest> prompt_;
        std::optional<Result<ProviderInteractionReply>> answer_;
        std::uint64_t progress_ = 0, promptSerial_ = 0;
        std::function<Result<QByteArray>()> invoke_;
    };
    HostSession::HostSession(Handler Dispatch) : epoch_(QUuid::createUuid().toString(QUuid::Id128)), dispatch_(std::move(Dispatch)), worker_([this](std::stop_token Stop) { work(Stop); }) {}
    HostSession::~HostSession()
    {
        worker_.request_stop();
        { std::lock_guard Lock(mutex_); if (pending_) { pending_->stop_.request_stop(); pending_->changed_.notify_all(); } }
        changed_.notify_all(); worker_.join();
    }
    Result<QByteArray> HostSession::Invoke(std::function<Result<QByteArray>()> Call)
    {
        auto Job = std::make_shared<Work>(); Job->invoke_ = std::move(Call);
        { std::lock_guard Lock(mutex_); if (pending_) return HostError("session-busy"); pending_ = Job; }
        changed_.notify_all();
        std::unique_lock Lock(Job->mutex_); Job->changed_.wait(Lock, [&] { return Job->result_.has_value(); });
        return std::move(*Job->result_);
    }
    void HostSession::work(std::stop_token Stop)
    {
        while (!Stop.stop_requested())
        {
            std::shared_ptr<Work> Job;
            { std::unique_lock Lock(mutex_); changed_.wait(Lock, Stop, [&] { return bool(pending_); }); if (Stop.stop_requested()) break; Job = pending_; }
            auto Context = Job->context_; Context.stopToken_ = Job->stop_.get_token();
            Context.pauseRequested_ = [Job] { return Job->paused_.load(); };
            Context.progress_ = [Job](std::uint64_t Value) { std::lock_guard Lock(Job->mutex_); Job->progress_ = std::max(Job->progress_, Value); };
            Context.interactionPrompt_ = [Job](const ProviderInteractionRequest& Request, const OperationContext& Operation) -> Result<ProviderInteractionReply> {
                std::unique_lock Lock(Job->mutex_);
                if (Job->prompt_) return HostError("session-interaction-busy");
                Job->prompt_ = Request; Job->prompt_->id_ = std::to_string(++Job->promptSerial_);
                while (!Job->answer_)
                {
                    if (const auto Cancel = Operation.Check({"plugin-host"})) { Job->prompt_.reset(); return *Cancel; }
                    Job->changed_.wait_for(Lock, std::chrono::milliseconds(20));
                }
                auto Answer = std::move(*Job->answer_); Job->answer_.reset(); Job->prompt_.reset(); return Answer;
            };
            Context.passwordPrompt_ = [Prompt = Context.interactionPrompt_](const std::string& Subject, unsigned Attempt, const OperationContext& Operation) -> Result<Password> {
                auto Reply = Prompt({ProviderInteractionKind::Password, {}, Subject, Attempt}, Operation);
                if (!Reply.HasValue()) return Reply.Failure();
                if (!Reply.Value().accepted_) return Error{ErrorCategory::Cancelled, "interaction-cancelled"};
                return std::move(Reply.Value().value_);
            };
            Result<QByteArray> Reply = HostError("session-handler-exception");
            try { Reply = Job->invoke_ ? Job->invoke_() : dispatch_(Job->kind_, Job->request_, Job->binary_, Context); } catch (...) {}
            { std::lock_guard Lock(mutex_); pending_.reset(); }
            { std::lock_guard Lock(Job->mutex_); Job->result_.emplace(std::move(Reply)); }
            Job->changed_.notify_all();
        }
    }
    Result<Unit> HostSession::Run(const WireFrame& Frame, HANDLE Pipe, HANDLE Parent)
    {
        const auto Send = [&](WireKind Kind, const QByteArray& Bytes) { return WriteFrame(Pipe, Parent, Kind, Frame.header_.sequence_, Bytes, IoContext(), {2, 0}); };
        const auto Fail = [&](Error Failure) { return Send(WireKind::Failure, FailureBytes(Failure)); };
        auto Reservation = quota_.Acquire(ResourceKind::Buffer, Frame.payload_.size()); if (!Reservation.HasValue()) return Fail(Reservation.Failure());
        auto Decoded = DecodeSessionPayload(Frame.payload_); if (!Decoded.HasValue()) return Fail(Decoded.Failure());
        const auto& Envelope = Decoded.Value().first;
        if (Envelope.size() != 4 || Envelope["session"].toString() != epoch_ || !Envelope["request"].isObject() || !Envelope["context"].isObject())
            return Fail({ErrorCategory::StaleRevision, "session-epoch-mismatch"});
        const auto Number = Envelope["kind"].toInt(-1); const auto Kind = static_cast<WireKind>(Number);
        if (Number < 0 || Number > UINT16_MAX || (!IsArchiveCommand(Kind) && !IsDataCommand(Kind)
            && Kind != WireKind::StreamOpen && Kind != WireKind::StreamRead && Kind != WireKind::StreamSeek && Kind != WireKind::StreamClose && Kind != WireKind::ArchiveUpdateBinary))
            return Fail({ErrorCategory::Unsupported, "session-command"});
        auto Context = DecodeContextV2(Envelope["context"].toObject()); if (!Context.HasValue()) return Fail(Context.Failure());
        auto Job = std::make_shared<Work>(); Job->kind_ = Kind; Job->request_ = Envelope["request"].toObject();
        if (Job->request_.contains("initialPause")) {
            if (Kind != WireKind::ArchiveExtract || !Job->request_["indices"].isArray() || !Job->request_["output"].isString()
                || Job->request_["initialPause"] != QJsonValue(true)) return Fail(HostError("session-pause-request"));
            Job->paused_ = true; Job->request_.remove("initialPause");
        }
        Job->binary_ = std::move(Decoded.Value().second); Job->context_ = std::move(Context.Value());
        { std::lock_guard Lock(mutex_); if (pending_) return Fail(HostError("session-busy")); pending_ = Job; }
        changed_.notify_all();
        QString SentPrompt;
        auto LastProgress = std::chrono::steady_clock::time_point::min();
        for (;;)
        {
            std::optional<ProviderInteractionRequest> Prompt;
            std::uint64_t Progress = 0;
            { std::lock_guard Lock(Job->mutex_);
                if (Job->result_) {
                    if (!Job->result_->HasValue()) return Fail(Job->result_->Failure());
                    return Send(Kind == WireKind::StreamRead ? WireKind::StreamData : WireKind::Response, Job->result_->Value());
                }
                Prompt = Job->prompt_; Progress = Job->progress_;
            }
            if (Prompt && QString::fromStdString(Prompt->id_) != SentPrompt)
            {
                SentPrompt = QString::fromStdString(Prompt->id_);
                auto Sent = Send(WireKind::InteractionRequest, Json({{"id", SentPrompt}, {"kind", int(Prompt->kind_)}, {"subject", QString::fromStdString(Prompt->subject_)}, {"attempt", int(Prompt->attempt_)}}));
                if (!Sent.HasValue()) { Job->stop_.request_stop(); return Sent; }
            }
            const auto Now = std::chrono::steady_clock::now();
            if (LastProgress == std::chrono::steady_clock::time_point::min() || Now - LastProgress >= std::chrono::milliseconds(50))
            {
                auto Sent = Send(WireKind::SessionProgress, Json({{"completed", QString::number(Progress)}, {"waiting", bool(Prompt)}}));
                if (!Sent.HasValue()) { Job->stop_.request_stop(); return Sent; } LastProgress = Now;
            }
            DWORD Available = 0;
            if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &Available, nullptr)) { Job->stop_.request_stop(); return HostError("session-pipe-closed"); }
            if (Available)
            {
                auto Input = ReadFrame(Pipe, Parent, IoContext());
                if (!Input.HasValue()) { Job->stop_.request_stop(); return Input.Failure(); }
                if (Input.Value().header_.version_ != ProtocolVersion{2, 0} || Input.Value().header_.sequence_ != Frame.header_.sequence_)
                { Job->stop_.request_stop(); return HostError("session-control-envelope"); }
                if (Input.Value().header_.kind_ == WireKind::SessionCancel) { Job->stop_.request_stop(); Job->changed_.notify_all(); }
                else if (Input.Value().header_.kind_ == WireKind::SessionPause && Input.Value().payload_.size() == 1
                    && (Input.Value().payload_[0] == '\0' || Input.Value().payload_[0] == '\1')) {
                    Job->paused_ = Input.Value().payload_[0] == '\1'; Job->changed_.notify_all();
                }
                else if (Input.Value().header_.kind_ == WireKind::InteractionReply)
                {
                    auto Doc = QJsonDocument::fromJson(Input.Value().payload_); Input.Value().payload_.fill(0); auto Object = Doc.object(); Doc = {};
                    std::lock_guard Lock(Job->mutex_);
                    if (!Job->prompt_ || Job->answer_ || Object.size() != 3 || Object["id"].toString() != QString::fromStdString(Job->prompt_->id_)
                        || !Object["accepted"].isBool() || !Object["value"].isString() || Object["value"].toString().size() > 4096 || Object["value"].toString().contains(QChar::Null))
                    { Job->stop_.request_stop(); return HostError("session-interaction-envelope"); }
                    auto Value = Object["value"].toString(); const bool Accepted = Object["accepted"].toBool(); Object = {};
                    Job->answer_.emplace(ProviderInteractionReply{Accepted, Password(Value.toStdString())}); Value.fill(QChar::Null); Job->changed_.notify_all();
                }
                else { Job->stop_.request_stop(); return HostError("session-control-kind"); }
            }
            if (WaitForSingleObject(Parent, 0) == WAIT_OBJECT_0) { Job->stop_.request_stop(); return HostError("session-parent-exited"); }
            std::unique_lock Lock(Job->mutex_);
            Job->changed_.wait_for(Lock, std::chrono::milliseconds(10), [&] {
                return Job->result_.has_value() || (Job->prompt_ && QString::fromStdString(Job->prompt_->id_) != SentPrompt);
            });
        }
    }
}
