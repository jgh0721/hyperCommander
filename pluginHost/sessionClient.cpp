#include "sessionClient.hpp"
#include "archiveCodec.hpp"
#include <QJsonDocument>

namespace hc::plugins
{
    using namespace storage;
    Result<WireFrame> CallSession(HANDLE Pipe, HANDLE Process, const QString& Epoch, std::uint64_t Sequence,
        WireKind Kind, QJsonObject Request, QByteArray Binary, OperationContext Context,
        std::uint32_t ResponseMs, std::uint32_t NoProgressMs, std::uint32_t CancelMs, std::function<void(bool)> Waiting)
    {
        const auto Io = [&] { OperationContext Value; Value.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ResponseMs); return Value; };
        const auto Send = [&](WireKind Type, const QByteArray& Data) { return WriteFrame(Pipe, Process, Type, Sequence, Data, Io(), {2, 0}); };
        if (const auto Stop = Context.Check({"plugin-host"})) return *Stop;
        const bool CanPause = Kind == WireKind::ArchiveExtract && Request.value("indices").isArray() && Request.value("output").isString() && bool(Context.pauseRequested_);
        bool Paused = false;
        try { Paused = CanPause && Context.pauseRequested_(); } catch (...) { return HostError("session-pause-query"); }
        if (Paused) Request["initialPause"] = true;
        const auto Payload = EncodeSessionPayload({{"session", Epoch}, {"kind", int(Kind)}, {"request", Request}, {"context", EncodeContextV2(Context)}}, Binary);
        auto Sent = Send(WireKind::SessionInvoke, Payload); if (!Sent.HasValue()) return Sent.Failure();
        auto LastProgress = std::chrono::steady_clock::now(); auto CancelAt = LastProgress;
        std::uint64_t Completed = 0;
        std::optional<Error> Cancel;
        std::optional<Error> PromptFailure;
        std::optional<Task<ProviderInteractionReply>> Pending;
        QString PromptId;
        for (;;)
        {
            const auto Now = std::chrono::steady_clock::now();
            if (!Cancel)
            {
                Cancel = Context.Check({"plugin-host"});
                bool RequestedPause = false;
                try { RequestedPause = CanPause && Context.pauseRequested_(); } catch (...) { return HostError("session-pause-query"); }
                if (!Cancel && RequestedPause != Paused) {
                    Sent = Send(WireKind::SessionPause, QByteArray(1, RequestedPause ? '\1' : '\0')); if (!Sent.HasValue()) return Sent.Failure();
                    Paused = RequestedPause; LastProgress = Now;
                }
                // Explicit deadlines and host liveness remain bounded during a
                // user pause; only the work-progress watchdog is suspended.
                if (Paused) LastProgress = Now;
                if (!Cancel && !Pending && NoProgressMs && Now - LastProgress > std::chrono::milliseconds(NoProgressMs))
                    Cancel = HostError("session-no-progress");
                if (Cancel) { CancelAt = Now; Sent = Send(WireKind::SessionCancel, {}); if (!Sent.HasValue()) return Sent.Failure(); }
            }
            if (Cancel && Now - CancelAt > std::chrono::milliseconds(CancelMs)) return *Cancel;
            if (Pending && !Cancel && Pending->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                Result<ProviderInteractionReply> Answer = HostError("interaction-callback-failed");
                try { Answer = Pending->get(); } catch (...) {}
                Pending.reset(); LastProgress = Now; Waiting(false);
                if (!Answer.HasValue()) PromptFailure = Answer.Failure();
                const bool Accepted = Answer.HasValue() && Answer.Value().accepted_ && !Cancel;
                auto Secret = Accepted ? QString::fromStdString(Answer.Value().value_.Value()) : QString{};
                if (Secret.size() > 4096 || Secret.contains(QChar::Null)) { Secret.fill(QChar::Null); return HostError("interaction-value-size"); }
                auto Bytes = QJsonDocument(QJsonObject{{"id", PromptId}, {"accepted", Accepted}, {"value", Secret}}).toJson(QJsonDocument::Compact); Secret.fill(QChar::Null);
                Sent = Send(WireKind::InteractionReply, Bytes); Bytes.fill(0); if (!Sent.HasValue()) return Sent.Failure(); PromptId.clear();
            }
            auto Frame = ReadFrame(Pipe, Process, Io()); if (!Frame.HasValue()) return Frame.Failure();
            if (Frame.Value().header_.version_ != ProtocolVersion{2, 0} || Frame.Value().header_.sequence_ != Sequence) return HostError("session-response-envelope");
            const auto Type = Frame.Value().header_.kind_;
            if (Type == WireKind::Response || Type == WireKind::Failure || Type == WireKind::StreamData)
            {
                // A completed mutation wins a late cancel: never claim that an
                // acknowledged write did not happen. Graceful cancellation is
                // reported by the handler, not fabricated from the stop token.
                if (Type == WireKind::Failure && PromptFailure) {
                    const auto& Failure = *PromptFailure;
                    Frame.Value().payload_ = QJsonDocument(QJsonObject{{"code", QString::fromStdString(Failure.code_)}, {"category", int(Failure.category_)},
                        {"provider", QString::fromStdString(Failure.provider_.value_)}, {"retryable", Failure.retryable_}, {"userAction", QString::fromStdString(Failure.userAction_)}}).toJson(QJsonDocument::Compact);
                }
                return std::move(Frame.Value());
            }
            QJsonParseError Parse; const auto Doc = QJsonDocument::fromJson(Frame.Value().payload_, &Parse);
            if (Parse.error != QJsonParseError::NoError || !Doc.isObject()) return HostError("session-event-json");
            const auto Object = Doc.object();
            if (Type == WireKind::SessionProgress)
            {
                auto Value = WireUnsigned(Object["completed"]);
                if (Object.size() != 2 || !Value.HasValue() || !Object["waiting"].isBool() || Value.Value() < Completed) return HostError("session-progress-envelope");
                if (Value.Value() > Completed) {
                    Completed = Value.Value(); LastProgress = Now;
                    if (Context.progress_) { try { Context.progress_(Completed); } catch (...) { return HostError("session-progress-callback"); } }
                }
                if (Pending) LastProgress = Now;
                continue;
            }
            if (Type != WireKind::InteractionRequest || Pending || !PromptId.isEmpty() || Object.size() != 4
                || !Object["id"].isString() || Object["id"].toString().isEmpty() || Object["id"].toString().size() > 32
                || !Object["subject"].isString() || Object["subject"].toString().size() > 32768
                || Object["kind"].toInt(-1) < 0 || Object["kind"].toInt(-1) > int(ProviderInteractionKind::Conflict)
                || Object["attempt"].toInt() < 1 || Object["attempt"].toInt() > 16) return HostError("session-interaction-envelope");
            PromptId = Object["id"].toString();
            Waiting(true);
            ProviderInteractionRequest Query{ProviderInteractionKind(Object["kind"].toInt()), PromptId.toStdString(), Object["subject"].toString().toStdString(), unsigned(Object["attempt"].toInt())};
            try
            {
                if (Context.interactionStart_) Pending.emplace(Context.interactionStart_(Query, Context));
                else if (Context.interactionPrompt_) Pending.emplace(Ready<ProviderInteractionReply>(Context.interactionPrompt_(Query, Context)));
                else if (Query.kind_ == ProviderInteractionKind::Password && Context.passwordPrompt_)
                {
                    auto Answer = Context.passwordPrompt_(Query.subject_, Query.attempt_, Context);
                    Pending.emplace(Answer.HasValue() ? Ready<ProviderInteractionReply>(ProviderInteractionReply{true, std::move(Answer.Value())}) : Ready<ProviderInteractionReply>(Answer.Failure()));
                }
                else Pending.emplace(Ready<ProviderInteractionReply>(Error{ErrorCategory::AccessDenied, Query.kind_ == ProviderInteractionKind::Password ? "archive-password-required" : "interaction-required"}));
            }
            catch (...) { Pending.emplace(Ready<ProviderInteractionReply>(HostError("interaction-callback-failed"))); }
            if (!Pending->valid()) return HostError("interaction-future-invalid");
        }
    }
}
