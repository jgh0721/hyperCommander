#include "hwpStartupTrace.hpp"
#include "hwpLoadBridge.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <cmath>

namespace hc::document
{
    HwpLoadEndpoint::HwpLoadEndpoint(HwpLoadBridge* Bridge, QObject* Parent)
        : QObject(Parent), bridge_(Bridge)
    {
        connect(Bridge, &HwpLoadBridge::CancelRequested, this, &HwpLoadEndpoint::CancelRequested);
        // QObject's inherited deleteLater can still be invoked remotely. Deleting
        // the facade is a load failure, never native retirement or successful load.
        connect(this, &QObject::destroyed, Bridge, &HwpLoadBridge::FailLoad);
    }
    QVariantMap HwpLoadEndpoint::PendingOpen() const
    { return bridge_ ? bridge_->PendingOpen() : QVariantMap{}; }
    QString HwpLoadEndpoint::ReadChunk(const QString& Operation, qint64 Offset) const
    { return bridge_ ? bridge_->ReadChunk(Operation, Offset) : QString{}; }
    bool HwpLoadEndpoint::Report(const QString& Json)
    { return bridge_ && bridge_->Report(Json); }

    HwpLoadBridge::HwpLoadBridge(QObject* Parent) : QObject(Parent)
    {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, &HwpLoadBridge::expire);
    }
    bool HwpLoadBridge::expired() const
    {
        return (state_ == State::Loading || state_ == State::Parsed) && deadline_.hasExpired();
    }
    void HwpLoadBridge::failView(const QString& Code)
    {
        if(!deferredPages_ || state_ != State::Ready)return;
        timer_.stop();rendering_=false;state_=State::Failed;
        QPointer<HwpLoadBridge> Guard(this);emit ViewFailed(Code);
        if(Guard)requestRetirement();
    }
    void HwpLoadBridge::expire()
    {
        if(state_==State::Ready && rendering_) {
            if(renderDeadline_.hasExpired())failView("rhwp-view-render-timeout");
            else timer_.start(int(renderDeadline_.remainingTime()));
            return;
        }
        if (expired()) fail("rhwp-load-timeout");
        else if (state_ == State::Loading || state_ == State::Parsed)
            timer_.start(static_cast<int>(deadline_.remainingTime()));
    }
    void HwpLoadBridge::requestRetirement()
    {
        if (retirementRequested_ || state_ == State::Retired) return;
        retirementRequested_ = true; emit RetirementRequested();
    }
    void HwpLoadBridge::fail(const QString& Code)
    {
        if (state_ != State::Loading && state_ != State::Parsed) return;
        timer_.stop(); state_ = State::Failed; bytes_.clear();
        QPointer<HwpLoadBridge> Guard(this);
        emit Failed(Code);
        if (Guard) requestRetirement();
    }
    bool HwpLoadBridge::Begin(const HwpNativeLoad& Request, QByteArray Bytes, QString& Error, int TimeoutMs, bool DeferredPages)
    {
        startup::Scope Validation("helper.loadInputHash");
        Error.clear();
        if (TimeoutMs < 1 || TimeoutMs > 120000 || state_ != State::Idle || Request.documentId_.isEmpty() || Request.documentId_.size() > 128 || !Request.documentGeneration_
            || !Request.bindingOwner_ || !Request.bindingGeneration_ || !Request.epoch_
            || (Request.format_ != "hwp" && Request.format_ != "hwpx")
            || Bytes.isEmpty()
            || Request.sha256_.size() != 64
            || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Request.sha256_) {
            Error = "rhwp-load-admission"; return false;
        }
        // Decimal strings retain all 64 identity bits across JavaScript.
        identity_ = {{"version", 1}, {"format", Request.format_}, {"documentId", Request.documentId_},
            {"documentGeneration", QString::number(Request.documentGeneration_)},
            {"bindingOwner", QString::number(Request.bindingOwner_)},
            {"bindingGeneration", QString::number(Request.bindingGeneration_)},
            {"epoch", QString::number(Request.epoch_)},
            {"operation", QUuid::createUuid().toString(QUuid::Id128)}};
        deferredPages_=DeferredPages;renderTimeoutMs_=TimeoutMs;
        sha256_ = Request.sha256_; bytes_ = std::move(Bytes); state_ = State::Loading;
        deadline_ = QDeadlineTimer(TimeoutMs, Qt::PreciseTimer);
        timer_.start(TimeoutMs); return true;
    }
    QVariantMap HwpLoadBridge::PendingOpen() const
    {
        if (expired() || (state_ != State::Loading && state_ != State::Parsed)) return {};
        startup::Data().EndNamed("helper.navigationToBridge");
        // Begin only once although PendingOpen may be called again.
        if(startup::Enabled() && !property("hcStartupBridgeSeen").toBool()) {
            const_cast<HwpLoadBridge*>(this)->setProperty("hcStartupBridgeSeen",true);
            startup::Data().Begin("helper.bridgeToFirstFrame");
        }
        auto Result = identity_;
        if(deferredPages_)Result["renderMode"]="viewport-v1";
        Result["readOnly"] = true;
        Result["byteLength"] = qint64(bytes_.size());
        Result["chunkSize"] = 192 * 1024;
        Result["sha256"] = QString::fromLatin1(sha256_);
        return Result.toVariantMap();
    }
    QString HwpLoadBridge::ReadChunk(const QString& Operation, qint64 Offset) const
    {
        if (expired() || (state_ != State::Loading && state_ != State::Parsed)
            || Operation != identity_["operation"].toString() || Offset < 0
            || Offset >= bytes_.size() || Offset % (192 * 1024) != 0) return {};
        return QString::fromLatin1(bytes_.mid(Offset, 192 * 1024).toBase64());
    }
    bool HwpLoadBridge::Report(const QString& Json)
    {
        // Check the monotonic deadline even when the timer event is queued behind a reply.
        if (expired() || (rendering_ && renderDeadline_.hasExpired())) { expire(); return false; }
        if (Json.size() > 4096) return false;
        QJsonParseError Error;
        const auto Document = QJsonDocument::fromJson(Json.toUtf8(), &Error);
        if (Error.error != QJsonParseError::NoError || !Document.isObject()) return false;
        auto Report = Document.object();
        for (auto At = identity_.begin(); At != identity_.end(); ++At) {
            if (!Report.contains(At.key()) || Report.take(At.key()) != At.value()) return false;
        }
        const auto TypeValue = Report.take("type");
        if (!TypeValue.isString()) return false;
        const auto Type = TypeValue.toString();
        auto Positive = [&](const char* Key, int Maximum) {
            const auto Value = Report.take(Key);
            const auto Number = Value.toDouble(-1);
            return Value.isDouble() && Number >= 1 && Number <= Maximum && std::floor(Number) == Number;
        };
        if (Type == "Parsed" && state_ == State::Loading) {
            pageCount_=Report.value("pageCount").toInt(-1);
            if (!Positive("pageCount", std::numeric_limits<int>::max()) || !Report.isEmpty()) return false;
            state_ = State::Parsed; return true;
        }
        if (Type == "FirstFrame" && state_ == State::Parsed) {
            const auto Page = Report.take("page");
            if (!Page.isDouble() || Page.toDouble(-1) != 0 || !Positive("width", std::numeric_limits<int>::max())
                || !Positive("height", std::numeric_limits<int>::max()) || !Report.isEmpty()) return false;
            timer_.stop(); rendered_.insert(0); state_ = State::Ready; bytes_.clear();
            startup::Data().EndNamed("helper.bridgeToFirstFrame");
            emit Ready(); return true;
        }
        if (Type == "Failed" && (state_ == State::Loading || state_ == State::Parsed)) {
            if (!Report.isEmpty()) return false;
            fail("rhwp-renderer-load"); return true;
        }
        if(Type=="StartupTrace" && startup::Enabled() && state_==State::Ready) {
            const auto Spans=Report.take("spans");
            return Report.isEmpty() && Spans.isArray() && startup::Data().AcceptRenderer(Spans.toArray());
        }
        if(deferredPages_ && state_==State::Ready) {
            if(Type=="ViewFailed" && Report.isEmpty()){failView("rhwp-view-renderer");return true;}
            const auto PageValue=Report.take("page");const auto Page=PageValue.toInt(-1);
            const auto SequenceText=Report.take("sequence").toString();bool Ok=false;const auto Sequence=SequenceText.toULongLong(&Ok);
            if(!Ok||!Sequence||QString::number(Sequence)!=SequenceText||PageValue!=QJsonValue(Page)||Page<1||Page>=pageCount_)return false;
            if(Type=="ViewRenderBegin"&&!rendering_&&Sequence==renderSequence_+1&&!rendered_.contains(Page)&&Report.isEmpty()) {
                renderSequence_=Sequence;activePage_=Page;rendering_=true;renderDeadline_=QDeadlineTimer(renderTimeoutMs_,Qt::PreciseTimer);timer_.start(renderTimeoutMs_);return true;
            }
            if(Type=="ViewRenderEnd"&&rendering_&&Sequence==renderSequence_&&Page==activePage_
                &&Positive("width",std::numeric_limits<int>::max())&&Positive("height",std::numeric_limits<int>::max())&&Report.isEmpty()) {
                timer_.stop();rendering_=false;rendered_.insert(Page);activePage_=-1;return true;
            }
            return false;
        }
        // A renderer ACK is observational only. Native teardown owns cancellation completion.
        return Type == "CancelObserved" && state_ == State::CancelPending && Report.isEmpty();
    }
    void HwpLoadBridge::FailLoad()
    {
        if(deferredPages_&&state_==State::Ready)failView("rhwp-native-view");
        else fail("rhwp-native-load");
    }
    void HwpLoadBridge::RequestCancel()
    {
        if (state_ != State::Loading && state_ != State::Parsed && !(deferredPages_&&state_==State::Ready)) return;
        rendering_=false;timer_.stop(); state_ = State::CancelPending; bytes_.clear();
        QPointer<HwpLoadBridge> Guard(this);
        emit CancelRequested();
        if (Guard) requestRetirement();
    }
    void HwpLoadBridge::Retire()
    {
        const auto Previous = state_;
        if (Previous == State::Retired) return;
        rendering_=false;rendered_.clear();timer_.stop(); state_ = State::Retired; bytes_.clear();
        if (Previous == State::CancelPending) emit Cancelled();
        else if (Previous == State::Loading || Previous == State::Parsed) emit Failed("rhwp-native-retired");
    }
}
