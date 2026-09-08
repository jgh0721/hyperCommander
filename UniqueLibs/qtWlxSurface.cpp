#include "qtWlxSurface.hpp"
#include <QEvent>
#include <qt_windows.h>
#include "pluginHost/wlxWindowMessage.hpp"
#include <QFocusEvent>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

using namespace hc::plugins;
using namespace hc::storage;
using namespace hc::viewer;

namespace
{
    class WlxAnchor final : public QWidget
    {
    public:
        using QWidget::QWidget;
        std::function<void(quintptr)> closeRequested_;
    protected:
        bool nativeEvent(const QByteArray& Type, void* Event, qintptr* Result) override
        {
            const auto* Message = static_cast<MSG*>(Event);
            static const auto CloseMessage = RegisterWindowMessageW(WlxCloseMessage);
            if (Type == "windows_generic_MSG" && CloseMessage && Message->message == CloseMessage)
            {
                if (closeRequested_) closeRequested_(Message->wParam);
                *Result = 0; return true;
            }
            return QWidget::nativeEvent(Type, Event, Result);
        }
    };
}

QtWlxController::QtWlxController(QWidget* Main, WlxViewerService::MaterializerFactory Factory)
    : QObject(Main), service_(std::make_unique<WlxViewerService>(std::move(Factory)))
{
    parking_ = new QWidget(Main); parking_->setObjectName("hc.wlx.retiring");
    parking_->setAttribute(Qt::WA_NativeWindow); parking_->resize(1, 1); parking_->hide(); parking_->winId();
    timer_ = new QTimer(this); timer_->setInterval(16); connect(timer_, &QTimer::timeout, this, &QtWlxController::collect); timer_->start();
}
QtWlxController::~QtWlxController()
{
    CloseAll();
    // Normal host close drains IsIdle first. Direct owner destruction still
    // joins workers before destroying retained native parents and content.
    service_.reset();
    for (auto& Record : records_) delete Record.anchor_.data();
    records_.clear(); delete parking_.data();
}
Result<std::shared_ptr<HostedWlxSession>> QtWlxController::Open(QWidget* Anchor, WlxViewerOptions Options, std::shared_ptr<IContentSource> Source)
{
    if (!Anchor || !service_) return Error{ErrorCategory::InvalidRequest, "viewer-parent-required", {"wlx"}};
    auto Opened = service_->Open(std::move(Options), std::move(Source), QString::number(Anchor->winId(), 16),
        qRound(Anchor->width() * Anchor->devicePixelRatioF()), qRound(Anchor->height() * Anchor->devicePixelRatioF()));
    if (Opened.HasValue()) records_.push_back({Anchor, Opened.Value()});
    return Opened;
}
void QtWlxController::Retire(QWidget* Anchor, const std::shared_ptr<HostedWlxSession>& Session)
{
    if (Session) Session->Close();
    if (Anchor && parking_)
    {
        // Same Qt::Widget flags retain the HWND on Windows. The hidden native
        // parent outlives tab/surface deletion until SDK close and process reap.
        Anchor->hide(); Anchor->setParent(parking_);
    }
}
void QtWlxController::CloseAll()
{
    for (auto& Record : records_) Retire(Record.anchor_, Record.session_);
    if (service_) service_->CancelAll();
}
bool QtWlxController::IsIdle() const { return !service_ || service_->IsIdle(); }
void QtWlxController::collect()
{
    for (auto It = records_.begin(); It != records_.end();)
    {
        if (!It->session_->Details().idle_) { ++It; continue; }
        delete It->anchor_.data(); It = records_.erase(It);
    }
    service_->Collect();
}
QtWlxSurface::QtWlxSurface(QtWlxController* Controller, QWidget* Parent) : QWidget(Parent), controller_(Controller)
{
    setObjectName("hc.viewer.wlx"); setFocusPolicy(Qt::StrongFocus);
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    status_ = new QLabel(tr("뷰어를 닫았습니다."), this); status_->setWordWrap(true); Layout->addWidget(status_);
    timer_ = new QTimer(this); timer_->setInterval(16); connect(timer_, &QTimer::timeout, this, &QtWlxSurface::poll);
}
QtWlxSurface::~QtWlxSurface() { Close(); }
void QtWlxSurface::Open(WlxViewerOptions Options, std::shared_ptr<IContentSource> Source)
{
    Close();
    if (!controller_) { status_->setText(tr("뷰어를 사용할 수 없습니다.")); emit Failed("viewer-host-unavailable"); return; }
    status_->setText(tr("파일을 준비하는 중…")); status_->show();
    auto* Anchor = new WlxAnchor(this); anchor_ = Anchor; anchor_->setObjectName("hc.wlx.native-parent");
    Anchor->closeRequested_ = [this](quintptr Child) {
        if (!session_ || !ready_ || session_->Details().child_ != QString::number(Child, 16)) return;
        const auto Id = session_->Snapshot().id_;
        // The receiver may delete this surface. Leave the native message stack
        // first, and discard gestures from a replaced or already closed session.
        QMetaObject::invokeMethod(this, [this, Id] {
            if (session_ && ready_ && session_->Snapshot().id_ == Id) emit CloseRequested();
        }, Qt::QueuedConnection);
    };
    anchor_->setAttribute(Qt::WA_DontCreateNativeAncestors); anchor_->setAttribute(Qt::WA_NativeWindow);
    anchor_->setFocusPolicy(Qt::StrongFocus); anchor_->installEventFilter(this);
    layout()->addWidget(anchor_); anchor_->show(); layout()->activate();
    auto Opened = controller_->Open(anchor_, std::move(Options), std::move(Source));
    if (!Opened.HasValue())
    {
        delete anchor_.data(); status_->setText(tr("뷰어를 열 수 없습니다."));
        emit Failed(QString::fromStdString(Opened.Failure().code_)); return;
    }
    session_ = std::move(Opened.Value()); timer_->start();
}
void QtWlxSurface::Close()
{
    timer_->stop(); ready_ = false;
    if (anchor_)
    {
        static_cast<WlxAnchor*>(anchor_.data())->closeRequested_ = {};
        anchor_->removeEventFilter(this); layout()->removeWidget(anchor_);
    }
    if (controller_) controller_->Retire(anchor_, session_);
    else if (session_) session_->Close();
    anchor_ = nullptr; session_.reset(); status_->setText(tr("뷰어를 닫았습니다.")); status_->show();
}
bool QtWlxSurface::IsReady() const { return ready_; }
ViewerSnapshot QtWlxSurface::Snapshot() const
{
    if (session_) return session_->Snapshot();
    ViewerSnapshot Result; Result.status_ = ViewerStatus::Closed; return Result;
}
void QtWlxSurface::poll()
{
    if (!session_) return;
    const auto Details = session_->Details();
    if (Details.viewer_.status_ == ViewerStatus::Ready && !ready_)
    {
        ready_ = true; status_->hide(); layout()->activate(); resizeNative();
        // Focus may arrive while content is still being materialized; replay
        // the current Qt focus after the worker publishes the native window.
        if (hasFocus() || (anchor_ && anchor_->hasFocus())) session_->Focus();
        emit Ready(QString::fromStdString(Details.viewer_.id_)); return;
    }
    if (!Details.idle_) return;
    timer_->stop(); ready_ = false; status_->show();
    if (Details.viewer_.status_ == ViewerStatus::Failed)
    {
        status_->setText(tr("뷰어를 열 수 없습니다. 플러그인 설정과 파일을 확인하세요."));
        emit Failed(QString::fromStdString(Details.viewer_.error_ ? Details.viewer_.error_->code_ : "viewer-failed"));
    }
    else status_->setText(tr("뷰어를 닫았습니다."));
}
void QtWlxSurface::resizeNative()
{
    if (session_ && anchor_) session_->Resize(qRound(anchor_->width() * anchor_->devicePixelRatioF()), qRound(anchor_->height() * anchor_->devicePixelRatioF()));
}
bool QtWlxSurface::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == anchor_)
    {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::DevicePixelRatioChange) resizeNative();
        if (Event->type() == QEvent::FocusIn && session_) session_->Focus();
    }
    return QWidget::eventFilter(Object, Event);
}
void QtWlxSurface::focusInEvent(QFocusEvent* Event)
{ if (session_) session_->Focus(); QWidget::focusInEvent(Event); }
void QtWlxSurface::Execute(const QString& Command)
{
    if (!session_) return;
    if (Command == "cancel") { Close(); return; }
    if (Command == "copy") session_->SendCommand(1, 0);
    else if (Command == "select-all") session_->SendCommand(3, 0);
}
