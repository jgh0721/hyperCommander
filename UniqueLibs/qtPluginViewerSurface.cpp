#include "qtPluginViewerSurface.hpp"
#include "UniqueLibs/errorPresentation.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <QMessageBox>
#include "commonLibs/document/epub/epubAppearance.hpp"
#include <QEvent>
#include <QFileInfo>
#include <QDateTime>
#include <qt_windows.h>
#include "pluginHost/wlxWindowMessage.hpp"
#include <QFocusEvent>
#include <QLabel>
#include <QProgressBar>
#include <fmwidgets/ProgressBar.h>
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
    QByteArray PreviewFiles(const PluginViewerOptions& Options)
    {
        QByteArray Key;
        for (const auto& Path : {Options.binaryPath_, Options.manifestPath_, Options.executable_, Options.defaultIniPath_}) {
            const QFileInfo File(Path);
            Key += Path.toUtf8() + '\0' + QByteArray::number(File.size()) + ':' + QByteArray::number(File.lastModified().toMSecsSinceEpoch()) + '\0';
        }
        return Key;
    }
    class WlxAnchor final : public QWidget
    {
    public:
        using QWidget::QWidget;
        std::function<void(quintptr)> closeRequested_;
        std::function<void(quintptr)> returnRequested_;
        std::function<void(quintptr)> interactionRequested_;
    protected:
        bool nativeEvent(const QByteArray& Type, void* Event, qintptr* Result) override
        {
            const auto* Message = static_cast<MSG*>(Event);
            static const auto CloseMessage = RegisterWindowMessageW(WlxCloseMessage);
            static const auto ReturnMessage = RegisterWindowMessageW(WlxReturnMessage);
            static const auto InteractionMessage = RegisterWindowMessageW(WlxInteractionMessage);
            if (Type == "windows_generic_MSG" && InteractionMessage && Message->message == InteractionMessage) {
                if (interactionRequested_) interactionRequested_(Message->wParam);
                *Result = 0; return true;
            }
            if (Type == "windows_generic_MSG" && ReturnMessage && Message->message == ReturnMessage) {
                if (returnRequested_) returnRequested_(Message->wParam);
                *Result = 0; return true;
            }
            if (Type == "windows_generic_MSG" && CloseMessage && Message->message == CloseMessage)
            {
                if (closeRequested_) closeRequested_(Message->wParam);
                *Result = 0; return true;
            }
            return QWidget::nativeEvent(Type, Event, Result);
        }
    };
}

QtPluginViewerController::QtPluginViewerController(QWidget* Main, PluginViewerService::MaterializerFactory Factory, int PreviewIdleMs)
    : QObject(Main), service_(std::make_unique<PluginViewerService>(std::move(Factory)))
{
    previewIdleMs_ = std::max(1, PreviewIdleMs);
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
bool QtPluginViewerController::CachePreview(QWidget* Anchor, const std::shared_ptr<HostedViewerSession>& Session,
    const PluginViewerOptions& Options)
{
    if (!Anchor || !Session || Options.purpose_ != ViewerPurpose::QuickPreview
        || Options.approval_.reviewed_.id_ != "builtin.hyperviewer") return false;
    const auto Details = Session->Details();
    if (!Details.documentChannel_ || Details.viewer_.status_ != ViewerStatus::Ready) return false;
    for (const auto& Item : Details.documentState_["documents"].toArray()) {
        const auto Tab = Item.toObject();
        if (Tab["dirty"].toBool() || Tab["loading"].toBool() || Tab["saving"].toBool() || Tab["archivePending"].toBool() || Tab["archiveUpdating"].toBool()) return false;
    }
    const auto Clear = Session->SendDocument({{"action", "clearPreview"}});
    if (!Clear.HasValue()) return false;
    if (previewCache_.session_) Retire(previewCache_.anchor_, previewCache_.session_);
    previewCache_ = {Anchor, Session}; previewOptions_ = Options; previewFiles_ = PreviewFiles(Options);
    // Writer authority belongs to a document, never to the cached process.
    previewOptions_->writer_.reset(); previewClearSequence_ = Clear.Value(); previewIdle_.invalidate();
    Anchor->hide(); Anchor->setParent(parking_);
    return true;
}
QtPluginViewerController::CachedPreview QtPluginViewerController::TakePreview(const PluginViewerOptions& Options)
{
    if (!previewCache_.session_ || !previewOptions_) return {};
    const auto& Cached = *previewOptions_;
    const auto Details = previewCache_.session_->Details();
    const bool Compatible = previewFiles_ == PreviewFiles(Options) && Options.purpose_ == ViewerPurpose::QuickPreview && Options.approval_.approved_
        && Cached.approval_.reviewed_ == Options.approval_.reviewed_
        && Cached.approval_.granted_ == Options.approval_.granted_
        && Cached.binaryPath_ == Options.binaryPath_ && Cached.manifestPath_ == Options.manifestPath_
        && Cached.executable_ == Options.executable_ && Cached.applicationRule_ == Options.applicationRule_
        && Cached.detectionOverride_ == Options.detectionOverride_ && Cached.flags_ == Options.flags_;
    if (!Compatible || Details.idle_ || Details.commandError_) {
        Retire(previewCache_.anchor_, previewCache_.session_); previewCache_ = {}; previewOptions_.reset(); return {};
    }
    // Clear and replacement are serialized on the same worker. Waiting for an empty
    // snapshot prevents a failed close from becoming an implicit discard.
    if (Details.commandSequence_ < previewClearSequence_ || !Details.documentState_["documents"].toArray().isEmpty()) return {};
    previewOptions_.reset(); previewIdle_.invalidate();
    return std::exchange(previewCache_, {});
}
void QtPluginViewerController::CloseAll()
{
    previewCache_ = {}; previewOptions_.reset(); previewIdle_.invalidate();
    for (auto& Record : records_) Retire(Record.anchor_, Record.session_);
    if (service_) service_->CancelAll();
}
bool QtPluginViewerController::IsIdle() const { return !service_ || service_->IsIdle(); }
void QtPluginViewerController::collect()
{
    if (previewCache_.session_) {
        const auto Details = previewCache_.session_->Details();
        const bool Cleared = Details.commandSequence_ >= previewClearSequence_;
        if (Cleared && !Details.commandError_ && Details.documentState_["documents"].toArray().isEmpty() && !previewIdle_.isValid()) previewIdle_.start();
        if (Details.idle_ || (Cleared && Details.commandError_) || (previewIdle_.isValid() && previewIdle_.elapsed() >= previewIdleMs_)) {
            Retire(previewCache_.anchor_, previewCache_.session_); previewCache_ = {}; previewOptions_.reset(); previewIdle_.invalidate();
        }
    }
    for (auto It = records_.begin(); It != records_.end();)
    {
        if (!It->session_->Details().idle_) { ++It; continue; }
        delete It->anchor_.data(); It = records_.erase(It);
    }
    service_->Collect();
}
QtPluginViewerSurface::QtPluginViewerSurface(QtPluginViewerController* Controller, QWidget* Parent) : QWidget(Parent), controller_(Controller)
{
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { appearancePending_ = true; });
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::fontsChanged, this, [this] { appearancePending_ = true; });
    setObjectName("hc.viewer.wlx"); setFocusPolicy(Qt::StrongFocus);
    auto* Layout = new QStackedLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    preparation_ = new QWidget(this);
    preparation_->setAutoFillBackground(true);
    auto* PreparationLayout = new QVBoxLayout(preparation_);
    PreparationLayout->addStretch();
    status_ = new QLabel(preparation_);
    status_->setObjectName("viewerPreparationStatus");
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    PreparationLayout->addWidget(status_);
    preparationDetail_ = new QLabel(preparation_);
    preparationDetail_->setObjectName("viewerPreparationDetail");
    preparationDetail_->setTextFormat(Qt::PlainText);
    preparationDetail_->setAlignment(Qt::AlignCenter);
    preparationDetail_->setWordWrap(true);
    PreparationLayout->addWidget(preparationDetail_);
    progress_ = new fm::ui::ProgressBar(preparation_);
    progress_->setMinimumWidth(260);
    progress_->setObjectName("viewerPreparationProgress");
    progress_->setMaximumWidth(420);
    auto ProgressPolicy = progress_->sizePolicy();
    ProgressPolicy.setRetainSizeWhenHidden(true);
    progress_->setSizePolicy(ProgressPolicy);
    PreparationLayout->addWidget(progress_, 0, Qt::AlignHCenter);
    progress_->hide();
    PreparationLayout->addStretch();
    Layout->addWidget(preparation_);
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
    preview_ = !Candidates.empty() && Candidates.front().purpose_ == ViewerPurpose::QuickPreview;
    if (!controller_) { status_->setText(tr("뷰어를 사용할 수 없습니다.")); emit Failed("viewer-host-unavailable"); return; }
    presentationTrace_=std::make_unique<hc::document::ImageTraceSpan>("viewer.prepare-to-present");
    beginPreparation(Source ? QString::fromStdString(Source->Describe().name_) : QString{});
    previewOptions_.reset();
    if (preview_ && !Candidates.empty() && Candidates.front().approval_.reviewed_.id_ == "builtin.hyperviewer") previewOptions_ = Candidates.front();
    if (previewOptions_) previewFiles_ = PreviewFiles(*previewOptions_);
    auto Cached = previewOptions_ ? controller_->TakePreview(*previewOptions_) : QtPluginViewerController::CachedPreview{};
    auto* Anchor = Cached.anchor_ ? static_cast<WlxAnchor*>(Cached.anchor_.data()) : new WlxAnchor(this);
    if (Cached.anchor_) Anchor->setParent(this);
    anchor_ = Anchor; anchor_->setObjectName("hc.wlx.native-parent");
    Anchor->interactionRequested_ = [this](quintptr Child) {
        if (session_ && session_->Details().child_ == QString::number(Child, 16)) interacted_ = true;
    };
    Anchor->returnRequested_ = [this](quintptr Child) {
        if (!session_ || session_->Details().child_ != QString::number(Child, 16)) return;
        const auto Id = session_->Snapshot().id_;
        QMetaObject::invokeMethod(this, [this, Id] {
            if (session_ && session_->Snapshot().id_ == Id) emit ReturnFocusRequested();
        }, Qt::QueuedConnection);
    };
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
    static_cast<QStackedLayout*>(layout())->setCurrentWidget(preparation_);layout()->activate();
    if (Cached.session_) {
        session_ = std::move(Cached.session_); ready_ = true;
        if (OpenPrepared({{Source, previewOptions_->writer_}})) { SetReturnKey(returnKey_); return; }
        Close(); open(std::move(Candidates), std::move(Source), std::move(Policy)); return;
    }
    auto Opened = Policy ? controller_->OpenAutomatic(anchor_, std::move(Candidates), std::move(Source), std::move(*Policy))
        : controller_->Open(anchor_, std::move(Candidates.front()), std::move(Source));
    if (!Opened.HasValue())
    {
        delete anchor_.data(); status_->setText(tr("뷰어를 열 수 없습니다."));
        emit Failed(QString::fromStdString(Opened.Failure().code_)); return;
    }
    session_ = std::move(Opened.Value()); SetReturnKey(returnKey_); timer_->start();
}
void QtPluginViewerSurface::Close()
{
    const bool Cache = preview_ && previewOptions_ && ready_ && previewFiles_ == PreviewFiles(*previewOptions_);
    preview_ = interacted_ = false;
    presentationTrace_.reset();
    lastSearch_.clear(); clipboardToken_.clear();
    timer_->stop(); ready_ = false; preparedSequence_ = 0;
    preparationElapsed_.invalidate(); progress_->hide(); preparationDetail_->clear();
    if (anchor_)
    {
        static_cast<WlxAnchor*>(anchor_.data())->closeRequested_ = {};
        static_cast<WlxAnchor*>(anchor_.data())->returnRequested_ = {};
        static_cast<WlxAnchor*>(anchor_.data())->interactionRequested_ = {};
        anchor_->removeEventFilter(this); layout()->removeWidget(anchor_);
    }
    if (controller_) {
        if (!Cache || !controller_->CachePreview(anchor_, session_, *previewOptions_)) controller_->Retire(anchor_, session_);
    }
    else if (session_) session_->Close();
    previewOptions_.reset(); anchor_ = nullptr; session_.reset(); status_->setText(tr("뷰어를 닫았습니다.")); static_cast<QStackedLayout*>(layout())->setCurrentWidget(preparation_);
    archiveCloseSequence_ = 0;
}
void QtPluginViewerSurface::RequestArchiveClose()
{
    if (!session_ || archiveCloseSequence_) return;
    const auto Sent = session_->CloseArchive();
    if (Sent.HasValue()) { archiveCloseSequence_ = Sent.Value(); timer_->start(); }
}
bool QtPluginViewerSurface::ReusePreview(const PluginViewerOptions& Options, std::shared_ptr<IContentSource> Source)
{
    if (!previewOptions_ || !preview_ || !ready_ || interacted_ || previewFiles_ != PreviewFiles(Options)) return false;
    const auto& Previous = *previewOptions_;
    if (!Options.approval_.approved_ || Previous.approval_.reviewed_ != Options.approval_.reviewed_
        || Previous.approval_.granted_ != Options.approval_.granted_ || Previous.applicationRule_ != Options.applicationRule_
        || Previous.detectionOverride_ != Options.detectionOverride_ || Previous.flags_ != Options.flags_) return false;
    if (!OpenPrepared({{std::move(Source), Options.writer_}})) return false;
    previewOptions_ = Options; previewOptions_->purpose_ = ViewerPurpose::QuickPreview; return true;
}
bool QtPluginViewerSurface::OpenPrepared(std::vector<HostedViewerSession::DocumentInput> Inputs)
{
    if (!session_ || !ready_) return false;
    presentationTrace_=std::make_unique<hc::document::ImageTraceSpan>("viewer.prepare-to-present");
    const auto Name = Inputs.empty() || !Inputs.front().source_ ? QString{} : QString::fromStdString(Inputs.front().source_->Describe().name_);
    auto Queued = session_->OpenDocuments(std::move(Inputs), true);
    if (!Queued.HasValue()) return false;
    preparedSequence_ = Queued.Value(); ready_ = false; timer_->start();
    beginPreparation(Name);
    static_cast<QStackedLayout*>(layout())->setCurrentWidget(preparation_);
    return true;
}
bool QtPluginViewerSurface::IsReady() const { return ready_; }
ViewerSnapshot QtPluginViewerSurface::Snapshot() const
{
    if (session_) return session_->Snapshot();
    ViewerSnapshot Result; Result.status_ = ViewerStatus::Closed; return Result;
}
void QtPluginViewerSurface::beginPreparation(const QString& Name)
{
    status_->setText(tr("파일을 준비하는 중…\n%1").arg(Name));
    progress_->hide();
    progress_->setRange(0, 0);
    preparationDetail_->setText(tr("파일 확인 중"));
    preparationElapsed_.start();
}
void QtPluginViewerSurface::poll()
{
    if (!session_) return;
    const auto Details = session_->Details();
    if (archiveCloseSequence_) {
        if (Details.commandSequence_ < archiveCloseSequence_) return;
        archiveCloseSequence_ = 0;
        if (Details.externalArchiveClosed_) {
            static_cast<QStackedLayout*>(layout())->setCurrentWidget(preparation_);
            status_->setText(tr("플러그인이 종료되었습니다. 편집본: %1").arg(Details.externalArchiveDraft_));
        }
        if (Details.commandError_) {
            hc::ui::MessageDialog::warning(this, tr("압축 갱신 실패"),
                tr("편집본을 보존했습니다. 다시 닫기를 실행하면 재시도할 수 있습니다.\n%1\n%2")
                    .arg(Details.externalArchiveDraft_, QString::fromStdString(Details.commandError_->code_)));
            return;
        }
        if (Details.externalArchiveDone_) { timer_->stop(); emit CloseRequested(); return; }
        const auto Decision = hc::ui::MessageDialog::question(this, tr("압축 파일 갱신"),
            tr("플러그인에서 저장한 변경 내용을 압축 파일에 반영하시겠습니까?\n아니요: 저장한 편집본 폐기\n취소: 편집본 보존\n%1").arg(Details.externalArchiveDraft_),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel);
        if (Decision == QMessageBox::Cancel) return;
        const auto Sent = session_->CloseArchive(Decision == QMessageBox::Yes ? 1 : 2);
        if (Sent.HasValue()) archiveCloseSequence_ = Sent.Value();
        return;
    }
    if (Details.externalArchiveClosed_) return;
    if (!ready_ && preparationElapsed_.isValid()) {
        QString Stage;
        const auto Phase=Details.preparationStage_;
        if(Phase=="materialize")Stage=tr("파일 복사·압축 해제 중");
        else if(Phase=="host")Stage=tr("뷰어 시작 중");
        else if(Phase=="content")Stage=tr("문서 내용 읽는 중");
        else if(Phase=="verify")Stage=tr("파일 검증 중");
        else Stage=tr("파일 확인 중");
        auto Progress=Details.preparationProgress_;
        if(Phase!="verify")for(const auto& Value:Details.documentState_["documents"].toArray()) {
            const auto Document=Value.toObject(), Preparation=Document["preparation"].toObject();
            if(Document["loading"].toBool()) {
                Stage=Document["path"].toString().endsWith(".zip",Qt::CaseInsensitive) || Document["path"].toString().endsWith(".cbz",Qt::CaseInsensitive)
                    ? tr("압축파일 목록·이미지 확인 중") : tr("문서 내용 읽는 중");
                if(!Preparation.isEmpty()) {
                    OperationContext::Progress P;P.bytesDone_=Preparation["bytesDone"].toInteger();
                    if(Preparation.contains("bytesTotal"))P.bytesTotal_=Preparation["bytesTotal"].toInteger();
                    P.itemsDone_=Preparation["itemsDone"].toInteger();P.itemsTotal_=Preparation["itemsTotal"].toInteger();Progress=P;
                    if(P.itemsTotal_)Stage=tr("이미지 구성 분석 중");
                }
            } else Stage=tr("첫 화면 그리는 중");
            break;
        }
        QString Amount=tr("진행률 계산 중");
        const auto Total=Progress ? (Progress->itemsTotal_ ? Progress->itemsTotal_ : Progress->bytesTotal_.value_or(0)) : 0;
        const auto Done=Progress ? (Progress->itemsTotal_ ? Progress->itemsDone_ : Progress->bytesDone_) : 0;
        if(Total) {
            const auto Percent=int(100.0L * std::min(Done,Total) / Total);
            progress_->setRange(0,100);progress_->setValue(Percent);
            Amount=tr("%1%").arg(Percent);
            if(Progress->itemsTotal_)Amount+=tr(" (%1/%2)").arg(Done).arg(Total);
        } else {
            progress_->setRange(0,0);
            if(Progress && Progress->itemsDone_)Amount=tr("%1개 확인 · 전체 개수 확인 중").arg(Progress->itemsDone_);
        }
        const bool Show=preparationElapsed_.elapsed()>=500;
        preparationDetail_->setText(Show ? tr("%1 · %2 · %3초").arg(Stage,Amount).arg(preparationElapsed_.elapsed()/1000) : Stage);
        progress_->setVisible(Show);
    }
    if(appearancePending_ && Details.documentChannel_ && Details.viewer_.status_ == ViewerStatus::Ready) {
        if(session_->SendDocument({{"action", "setDocumentAppearance"}, {"appearance", hc::document::epubui::Appearance()}}).HasValue()) appearancePending_ = false;
    }
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
    if (!ready_ && Details.documentChannel_ && !Details.child_.isEmpty()) {
        for (const auto& Item : Details.documentState_["documents"].toArray()) {
            if (!Item.toObject()["textPresentationReady"].toBool()) continue;
            static_cast<QStackedLayout*>(layout())->setCurrentWidget(anchor_); layout()->activate(); resizeNative();
            if (presentationTrace_) { hc::document::ImageTraceSpan FirstText("viewer.first-text-prefix"); presentationTrace_.reset(); }
            break;
        }
    }
    bool ContentReady=Details.viewer_.status_==ViewerStatus::Ready && Details.commandSequence_ >= preparedSequence_;
    if (preparedSequence_ && Details.commandSequence_ >= preparedSequence_ && Details.commandError_) {
        preparedSequence_ = 0; timer_->stop(); progress_->hide(); preparationDetail_->clear(); status_->setText(tr("문서를 열 수 없습니다."));
        emit Failed(QString::fromStdString(Details.commandError_->code_)); return;
    }
    if (Details.documentChannel_ && Details.commandSequence_ >= preparedSequence_) {
        for (const auto& Item : Details.documentState_["documents"].toArray()) {
            const auto Record = Item.toObject();
            const auto Failure = Record["error"].toString();
            // A save conflict is a live document state, not a failed viewer.
            // Keep polling so the user can resolve it and save again.
            if (ready_ && Record["lifecycle"].toString() != "failed") continue;
            if (Failure.isEmpty() && Record["lifecycle"].toString() != "failed") continue;
            preparedSequence_ = 0; timer_->stop(); progress_->hide(); preparationDetail_->clear();
            const auto Reason = Failure.isEmpty() ? tr("문서 읽기 실패") : Failure;
            status_->setText(tr("문서를 열 수 없습니다: %1").arg(hc::ui::ErrorText(Reason)));
            emit Failed(Reason); return;
        }
    }
    if(ContentReady&&Details.documentChannel_){
        const auto Documents=Details.documentState_["documents"].toArray();ContentReady=!Documents.isEmpty();
        for(const auto& Item:Documents){
            const auto Record=Item.toObject();
            const bool EpubSource=Record["view"].toObject()["viewId"]=="epub" && Record["sourceReady"].toBool()
                && !Record["canSave"].toBool() && !Record["canSaveAs"].toBool();
            // Retired-tab recovery cleanup must not hide an already attached text document.
            ContentReady &= !Record["sourceToken"].toString().isEmpty() && (EpubSource || Record["imagePresentationReady"].toBool() ||
                ((Record["view"].toObject()["viewId"] == "text" || Details.documentState_["idle"].toBool())
                    && !Record["loading"].toBool() && Record["lifecycle"].toString()=="ready"));
        }
    }
    if (ContentReady && !ready_)
    {
        presentationTrace_.reset();
        ready_ = true;static_cast<QStackedLayout*>(layout())->setCurrentWidget(anchor_);layout()->activate(); resizeNative();
        // Focus may arrive while content is still being materialized; replay
        // the current Qt focus after the worker publishes the native window.
        if (isVisible() && (hasFocus() || (anchor_ && anchor_->hasFocus()))) session_->Focus();
        emit Ready(QString::fromStdString(Details.viewer_.id_)); return;
    }
    if (!Details.idle_) return;
    timer_->stop(); progress_->hide(); preparationDetail_->clear(); ready_ = false;static_cast<QStackedLayout*>(layout())->setCurrentWidget(preparation_);
    // NoMatch is published after host/lease cleanup. Failure and cancellation
    // must never initiate another read through the builtin fallback.
    if (Details.noMatch_) { emit NoMatch(); return; }
    if (Details.viewer_.status_ == ViewerStatus::Failed)
    {
        status_->setText(tr("뷰어를 열 수 없습니다. 플러그인 설정과 파일을 확인하세요."));
        auto Error = QString::fromStdString(Details.viewer_.error_ ? Details.viewer_.error_->code_ : "viewer-failed");
        if (Details.externalArchive_) Error += tr("\n보존한 압축 편집본: %1").arg(Details.externalArchiveDraft_);
        emit Failed(Error);
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
void QtPluginViewerSurface::SetReturnKey(QKeyCombination Key)
{
    returnKey_ = Key;
    if (!session_ || !preview_) return;
    const auto Code = int(Key.key());
    quintptr Virtual = Code >= Qt::Key_F1 && Code <= Qt::Key_F24 ? VK_F1 + Code - Qt::Key_F1
        : Code >= Qt::Key_A && Code <= Qt::Key_Z ? Code : Code >= Qt::Key_0 && Code <= Qt::Key_9 ? Code : 0;
    if (!Virtual && Code >= 32 && Code < 127) {
        const auto Mapped = VkKeyScanW(wchar_t(Code));
        if (Mapped != -1) Virtual = LOBYTE(Mapped);
    }
    if (!Virtual) switch (Code) {
    case Qt::Key_Escape: Virtual = VK_ESCAPE; break;
    case Qt::Key_Tab: Virtual = VK_TAB; break;
    case Qt::Key_Return: case Qt::Key_Enter: Virtual = VK_RETURN; break;
    case Qt::Key_Backspace: Virtual = VK_BACK; break;
    case Qt::Key_Delete: Virtual = VK_DELETE; break;
    case Qt::Key_Insert: Virtual = VK_INSERT; break;
    case Qt::Key_Home: Virtual = VK_HOME; break;
    case Qt::Key_End: Virtual = VK_END; break;
    case Qt::Key_PageUp: Virtual = VK_PRIOR; break;
    case Qt::Key_PageDown: Virtual = VK_NEXT; break;
    case Qt::Key_Left: Virtual = VK_LEFT; break;
    case Qt::Key_Right: Virtual = VK_RIGHT; break;
    case Qt::Key_Up: Virtual = VK_UP; break;
    case Qt::Key_Down: Virtual = VK_DOWN; break;
    default: break;
    }
    const auto Mods = Key.keyboardModifiers();
    const quintptr Bits = (Mods.testFlag(Qt::ControlModifier) ? 1 : 0) | (Mods.testFlag(Qt::AltModifier) ? 2 : 0) | (Mods.testFlag(Qt::ShiftModifier) ? 4 : 0)
        | (Mods.testFlag(Qt::MetaModifier) ? 8 : 0);
    session_->SetReturnKey(unsigned(Virtual), unsigned(Bits), RegisterWindowMessageW(WlxReturnMessage),
        RegisterWindowMessageW(WlxInteractionMessage));
}
bool QtPluginViewerSurface::WasInteracted() const
{
    return interacted_;
}
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
    if (!Result.HasValue()) status_->setText(hc::ui::ErrorText(Result.Failure()));
}

Result<std::uint64_t> QtPluginViewerSurface::SendDocument(QJsonObject Request)
{ if (!session_) return Error{ErrorCategory::Unavailable,"document-channel-not-ready"}; return session_->SendDocument(std::move(Request)); }
Result<std::uint64_t> QtPluginViewerSurface::OpenDocuments(std::vector<HostedViewerSession::DocumentInput> Inputs)
{ if (!session_) return Error{ErrorCategory::Unavailable,"document-channel-not-ready"}; return session_->OpenDocuments(std::move(Inputs)); }
