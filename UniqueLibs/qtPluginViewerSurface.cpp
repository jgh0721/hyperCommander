#include "qtPluginViewerSurface.hpp"
#include <QEvent>
#include <qt_windows.h>
#include "pluginHost/wlxWindowMessage.hpp"
#include <QFocusEvent>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QImage>
#include <QJsonArray>
#include "commonLibs/document/documentHostProtocol.hpp"

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

QtPluginViewerController::QtPluginViewerController(QWidget* Main, PluginViewerService::MaterializerFactory Factory)
    : QObject(Main), service_(std::make_unique<PluginViewerService>(std::move(Factory)))
{
    parking_ = new QWidget(Main); parking_->setObjectName("hc.wlx.retiring");
    parking_->setAttribute(Qt::WA_NativeWindow); parking_->resize(1, 1); parking_->hide(); parking_->winId();
    timer_ = new QTimer(this); timer_->setInterval(16); connect(timer_, &QTimer::timeout, this, &QtPluginViewerController::collect); timer_->start();
}
QtPluginViewerController::~QtPluginViewerController()
{
    CloseAll();
    // Normal host close drains IsIdle first. Direct owner destruction still
    // joins workers before destroying retained native parents and content.
    service_.reset();
    for (auto& Record : records_) delete Record.anchor_.data();
    records_.clear(); delete parking_.data();
}
Result<std::shared_ptr<HostedViewerSession>> QtPluginViewerController::Open(QWidget* Anchor, PluginViewerOptions Options, std::shared_ptr<IContentSource> Source)
{
    if (!Anchor || !service_) return Error{ErrorCategory::InvalidRequest, "viewer-parent-required", {"wlx"}};
    auto Opened = service_->Open(std::move(Options), std::move(Source), QString::number(Anchor->winId(), 16),
        qRound(Anchor->width() * Anchor->devicePixelRatioF()), qRound(Anchor->height() * Anchor->devicePixelRatioF()));
    if (Opened.HasValue()) records_.push_back({Anchor, Opened.Value()});
    return Opened;
}
Result<std::shared_ptr<HostedViewerSession>> QtPluginViewerController::OpenAutomatic(QWidget* Anchor,
    std::vector<PluginViewerOptions> Candidates, std::shared_ptr<IContentSource> Source, PluginViewerSelectionPolicy Policy)
{
    if (!Anchor || !service_) return Error{ErrorCategory::InvalidRequest, "viewer-parent-required", {"wlx"}};
    auto Opened = service_->OpenAutomatic(std::move(Candidates), std::move(Source), QString::number(Anchor->winId(), 16),
        qRound(Anchor->width() * Anchor->devicePixelRatioF()), qRound(Anchor->height() * Anchor->devicePixelRatioF()), std::move(Policy));
    if (Opened.HasValue()) records_.push_back({Anchor, Opened.Value()});
    return Opened;
}
void QtPluginViewerController::Retire(QWidget* Anchor, const std::shared_ptr<HostedViewerSession>& Session)
{
    if (Session) Session->Close();
    if (Anchor && parking_)
    {
        // Same Qt::Widget flags retain the HWND on Windows. The hidden native
        // parent outlives tab/surface deletion until SDK close and process reap.
        Anchor->hide(); Anchor->setParent(parking_);
    }
}
void QtPluginViewerController::CloseAll()
{
    for (auto& Record : records_) Retire(Record.anchor_, Record.session_);
    if (service_) service_->CancelAll();
}
bool QtPluginViewerController::IsIdle() const { return !service_ || service_->IsIdle(); }
void QtPluginViewerController::collect()
{
    for (auto It = records_.begin(); It != records_.end();)
    {
        if (!It->session_->Details().idle_) { ++It; continue; }
        delete It->anchor_.data(); It = records_.erase(It);
    }
    service_->Collect();
}
QtPluginViewerSurface::QtPluginViewerSurface(QtPluginViewerController* Controller, QWidget* Parent) : QWidget(Parent), controller_(Controller)
{
    setObjectName("hc.viewer.wlx"); setFocusPolicy(Qt::StrongFocus);
    auto* Layout = new QStackedLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    status_ = new QLabel(tr("뷰어를 닫았습니다."), this); status_->setWordWrap(true);status_->setAlignment(Qt::AlignCenter);status_->setAutoFillBackground(true);Layout->addWidget(status_);
    timer_ = new QTimer(this); timer_->setInterval(16); connect(timer_, &QTimer::timeout, this, &QtPluginViewerSurface::poll);
}
QtPluginViewerSurface::~QtPluginViewerSurface() { Close(); }
void QtPluginViewerSurface::Open(PluginViewerOptions Options, std::shared_ptr<IContentSource> Source)
{ open({std::move(Options)}, std::move(Source), {}); }
void QtPluginViewerSurface::OpenAutomatic(std::vector<PluginViewerOptions> Candidates, std::shared_ptr<IContentSource> Source, PluginViewerSelectionPolicy Policy)
{ open(std::move(Candidates), std::move(Source), std::move(Policy)); }
PluginViewerDetails QtPluginViewerSurface::Details() const
{ return session_ ? session_->Details() : PluginViewerDetails{}; }
void QtPluginViewerSurface::open(std::vector<PluginViewerOptions> Candidates, std::shared_ptr<IContentSource> Source, std::optional<PluginViewerSelectionPolicy> Policy)
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
    // Fix the native root before the isolated worker grants HWND access.
    window()->winId();
    anchor_->setAttribute(Qt::WA_DontCreateNativeAncestors); anchor_->setAttribute(Qt::WA_NativeWindow);
    anchor_->setFocusPolicy(Qt::StrongFocus); anchor_->installEventFilter(this);
    layout()->addWidget(anchor_);anchor_->setGeometry(rect());anchor_->hide();
    static_cast<QStackedLayout*>(layout())->setCurrentWidget(status_);layout()->activate();
    auto Opened = Policy ? controller_->OpenAutomatic(anchor_, std::move(Candidates), std::move(Source), std::move(*Policy))
        : controller_->Open(anchor_, std::move(Candidates.front()), std::move(Source));
    if (!Opened.HasValue())
    {
        delete anchor_.data(); status_->setText(tr("뷰어를 열 수 없습니다."));
        emit Failed(QString::fromStdString(Opened.Failure().code_)); return;
    }
    session_ = std::move(Opened.Value()); timer_->start();
}
void QtPluginViewerSurface::Close()
{
    lastSearch_.clear(); clipboardToken_.clear();
    timer_->stop(); ready_ = false; preparedSequence_ = 0;
    if (anchor_)
    {
        static_cast<WlxAnchor*>(anchor_.data())->closeRequested_ = {};
        anchor_->removeEventFilter(this); layout()->removeWidget(anchor_);
    }
    if (controller_) controller_->Retire(anchor_, session_);
    else if (session_) session_->Close();
    anchor_ = nullptr; session_.reset(); status_->setText(tr("뷰어를 닫았습니다.")); static_cast<QStackedLayout*>(layout())->setCurrentWidget(status_);
}
bool QtPluginViewerSurface::OpenPrepared(std::vector<HostedViewerSession::DocumentInput> Inputs)
{
    if (!session_ || !ready_) return false;
    auto Queued = session_->OpenDocuments(std::move(Inputs), true);
    if (!Queued.HasValue()) return false;
    preparedSequence_ = Queued.Value(); ready_ = false; timer_->start();
    status_->setText(tr("파일을 준비하는 중…"));
    static_cast<QStackedLayout*>(layout())->setCurrentWidget(status_);
    return true;
}
bool QtPluginViewerSurface::IsReady() const { return ready_; }
ViewerSnapshot QtPluginViewerSurface::Snapshot() const
{
    if (session_) return session_->Snapshot();
    ViewerSnapshot Result; Result.status_ = ViewerStatus::Closed; return Result;
}
void QtPluginViewerSurface::poll()
{
    if (!session_) return;
    const auto Details = session_->Details();
    if (Details.documentChannel_) {
        // A recycled viewer still reports the old close until the replacing open completes.
        if (Details.commandSequence_ >= preparedSequence_ && Details.documentState_["closeRequested"].toBool()) {
            timer_->stop();
            emit CloseRequested();
            return;
        }
        const auto Request = Details.documentState_["clipboard"].toObject();
        const auto Token = Request["token"].toString();
        if (!Token.isEmpty() && Token != clipboardToken_) {
            clipboardToken_ = Token; const auto Mode = Request["mode"].toString();
            QJsonObject Reply{{"action", "clipboardResult"}, {"token", Token}, {"accepted", false}};
            auto* Clipboard = QApplication::clipboard();
            if (Mode == "paste" && Clipboard) {
                const auto Text = Clipboard->text();
                if (Text.toUtf8().size() <= 64 * 1024 && !Text.contains(QChar::Null) && Text.isValidUtf16())
                    { Reply["text"] = Text; Reply["accepted"] = true; }
            } else if ((Mode == "copy" || Mode == "cut") && Clipboard) {
                if (Request["mime"].toString() == "text/plain" && Request["text"].toString().toUtf8().size() <= 64 * 1024) {
                    Clipboard->setText(Request["text"].toString()); Reply["accepted"] = true;
                } else if (Request["mime"].toString() == "image/png") {
                    const auto Encoded = Request["bytes"].toString().toLatin1();
                    if (Encoded.size() <= ((hc::document::HostChunkLimit + 2) / 3) * 4) {
                        const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
                        const auto Image = QImage::fromData(Bytes, "PNG");
                        if (!Image.isNull() && quint64(Image.width()) * Image.height() <= 8 * 1024 * 1024) {
                            Clipboard->setImage(Image); Reply["accepted"] = true;
                        }
                    }
                }
            }
            auto Sent = session_->SendDocument(Reply);
            if (!Sent.HasValue()) clipboardToken_.clear();
        }
    }
    bool ContentReady=Details.viewer_.status_==ViewerStatus::Ready && Details.commandSequence_ >= preparedSequence_;
    if (preparedSequence_ && Details.commandSequence_ >= preparedSequence_ && Details.commandError_) {
        preparedSequence_ = 0; timer_->stop(); status_->setText(tr("문서를 열 수 없습니다."));
        emit Failed(QString::fromStdString(Details.commandError_->code_)); return;
    }
    if (Details.documentChannel_ && Details.commandSequence_ >= preparedSequence_) {
        for (const auto& Item : Details.documentState_["documents"].toArray()) {
            const auto Record = Item.toObject();
            const auto Failure = Record["error"].toString();
            if (Failure.isEmpty() && Record["lifecycle"].toString() != "failed") continue;
            preparedSequence_ = 0; timer_->stop();
            const auto Reason = Failure.isEmpty() ? tr("문서 읽기 실패") : Failure;
            status_->setText(tr("문서를 열 수 없습니다: %1").arg(Reason));
            emit Failed(Reason); return;
        }
    }
    if(ContentReady&&Details.documentChannel_){
        const auto Documents=Details.documentState_["documents"].toArray();ContentReady=!Documents.isEmpty();
        for(const auto& Item:Documents){
            const auto Record=Item.toObject();
            const bool EpubSource=Record["view"].toObject()["viewId"]=="epub" && Record["sourceReady"].toBool()
                && !Record["canSave"].toBool() && !Record["canSaveAs"].toBool();
            ContentReady &= !Record["sourceToken"].toString().isEmpty() && (EpubSource ||
                (Details.documentState_["idle"].toBool() && !Record["loading"].toBool() && Record["lifecycle"].toString()=="ready"));
        }
    }
    if (ContentReady && !ready_)
    {
        ready_ = true;static_cast<QStackedLayout*>(layout())->setCurrentWidget(anchor_);layout()->activate(); resizeNative();
        // Focus may arrive while content is still being materialized; replay
        // the current Qt focus after the worker publishes the native window.
        if (isVisible() && (hasFocus() || (anchor_ && anchor_->hasFocus()))) session_->Focus();
        emit Ready(QString::fromStdString(Details.viewer_.id_)); return;
    }
    if (!Details.idle_) return;
    timer_->stop(); ready_ = false;static_cast<QStackedLayout*>(layout())->setCurrentWidget(status_);
    // NoMatch is published after host/lease cleanup. Failure and cancellation
    // must never initiate another read through the builtin fallback.
    if (Details.noMatch_) { emit NoMatch(); return; }
    if (Details.viewer_.status_ == ViewerStatus::Failed)
    {
        status_->setText(tr("뷰어를 열 수 없습니다. 플러그인 설정과 파일을 확인하세요."));
        emit Failed(QString::fromStdString(Details.viewer_.error_ ? Details.viewer_.error_->code_ : "viewer-failed"));
    }
    else status_->setText(tr("뷰어를 닫았습니다."));
}
void QtPluginViewerSurface::resizeNative()
{
    if (session_ && anchor_) session_->Resize(qRound(anchor_->width() * anchor_->devicePixelRatioF()), qRound(anchor_->height() * anchor_->devicePixelRatioF()));
}
bool QtPluginViewerSurface::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == anchor_)
    {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::DevicePixelRatioChange) resizeNative();
        if (Event->type() == QEvent::FocusIn && session_ && ready_ && isVisible()) session_->Focus();
    }
    return QWidget::eventFilter(Object, Event);
}
void QtPluginViewerSurface::focusInEvent(QFocusEvent* Event)
{ if (session_ && ready_ && isVisible()) session_->Focus(); QWidget::focusInEvent(Event); }
void QtPluginViewerSurface::Execute(const QString& Command)
{
    if (!session_) return;
    if (Command == "cancel") { Close(); return; }
    if (session_->Details().documentChannel_) {
        auto State = session_->Details().documentState_;
        const auto Id = State["activeDocumentId"].toString();
        if (Command == "copy") session_->SendDocument({{"action", "clipboard"}, {"documentId", Id}, {"mode", "copy"}});
        else if (Command == "select-all") session_->SendDocument({{"action", "selectAll"}, {"documentId", Id}});
        return;
    }
    if (Command == "copy") session_->SendCommand(1, 0);
    else if (Command == "select-all") session_->SendCommand(3, 0);
}
bool QtPluginViewerSurface::CanSearch() const
{
    if (!session_) return false; if (session_->Details().documentChannel_) return true; const auto Commands = session_->Commands(); return std::ranges::find(Commands, "find") != Commands.end();
}
void QtPluginViewerSurface::Search(const QString& Text, bool Backward)
{
    if (!session_) return;
    if (session_->Details().documentChannel_) {
        const auto State = session_->Details().documentState_; const auto Id = State["activeDocumentId"].toString();
        QString View; for (const auto& Item : State["documents"].toArray()) if (Item.toObject()["documentId"].toString() == Id) View = Item.toObject()["view"].toObject()["viewId"].toString();
        session_->SendDocument({{"action", View == "pdf" ? "pdf.search" : "text.find"}, {"documentId", Id}, {"text", Text}, {"backward", Backward}}); return;
    }
    const auto Result = session_->Search(Text, (Backward ? 8 : 0) | (Text != lastSearch_ ? 1 : 0));
    if (Result.HasValue()) lastSearch_ = Text;
    if (!Result.HasValue()) status_->setText(QString::fromStdString(Result.Failure().code_));
}

Result<std::uint64_t> QtPluginViewerSurface::SendDocument(QJsonObject Request)
{ if (!session_) return Error{ErrorCategory::Unavailable,"document-channel-not-ready"}; return session_->SendDocument(std::move(Request)); }
Result<std::uint64_t> QtPluginViewerSurface::OpenDocuments(std::vector<HostedViewerSession::DocumentInput> Inputs)
{ if (!session_) return Error{ErrorCategory::Unavailable,"document-channel-not-ready"}; return session_->OpenDocuments(std::move(Inputs)); }
