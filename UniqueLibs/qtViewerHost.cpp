#include "qtViewerHost.hpp"
#include "qtViewerProvider.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QCoreApplication>
#include "storageProviders.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QPushButton>
#include <QApplication>
#include <QKeyEvent>
#include <qt_windows.h>
#include "qtPluginReview.hpp"

namespace {
class ViewerDialog final : public QDialog {
public:
    using QDialog::QDialog;
    std::function<void()> escape_;
    void RememberPreviousWindow() {
        previousWindow_ = GetForegroundWindow();
        previousFocus_ = QApplication::focusWidget();
        if (previousFocus_ && reinterpret_cast<HWND>(previousFocus_->window()->winId()) != previousWindow_) previousFocus_ = nullptr;
    }
    void done(int Result) override { returnFocus([&] { QDialog::done(Result); }); }
    // Hide without QDialog::done(): since Qt 6.3 it closes the window, which destroys
    // the HWND together with the helper's foreign child windows a warm viewer keeps.
    void Dismiss() { returnFocus([&] { hide(); }); }
    // Connections of one use (warm or document). Replacing it disconnects them.
    QObject* NewPhase() { delete phase_.data(); phase_ = new QObject(this); return phase_; }
protected:
    void keyPressEvent(QKeyEvent* Event) override {
        if (Event->key() == Qt::Key_Escape && Event->modifiers() == Qt::NoModifier && escape_) {
            if (!Event->isAutoRepeat()) escape_();
            Event->accept(); return;
        }
        QDialog::keyPressEvent(Event);
    }
private:
    void returnFocus(const std::function<void()>& Hide) {
        const auto Root = reinterpret_cast<HWND>(winId());
        const auto Foreground = GetForegroundWindow();
        // A separate viewer may already have returned activation to its owner
        // before its close notification arrives. Restore native input focus too.
        const bool Restore = Foreground == Root || Foreground == previousWindow_ || (Foreground && IsChild(Root, Foreground));
        const auto Previous = previousWindow_;
        const auto Focus = previousFocus_;
        auto* Owner = parentWidget();
        Hide();
        if (!Restore || !Owner || !Previous || Previous == Root || !IsWindow(Previous)) return;
        // The foreign viewer input queue may detach after the dialog hides.
        // Retry briefly, but stop immediately if the user activates another app.
        auto* Timer = new QTimer(Owner); Timer->setInterval(25);
        QObject::connect(Timer, &QTimer::timeout, Timer, [Timer, Previous, Focus, Root, Attempts = 0]() mutable {
            const auto Current = GetForegroundWindow();
            if (!IsWindow(Previous) || ++Attempts > 40 || (Attempts > 1 && Current != Previous && Current != Root)) {
                Timer->stop(); Timer->deleteLater(); return;
            }
            SetForegroundWindow(Previous);
            if (GetForegroundWindow() != Previous) return;
            if (Focus && Focus->isVisible() && Focus->isEnabled()) {
                QApplication::setActiveWindow(Focus->window());
                Focus->clearFocus(); Focus->setFocus(Qt::OtherFocusReason);
                SetFocus(reinterpret_cast<HWND>(Focus->winId()));
            }
            Timer->stop(); Timer->deleteLater();
        });
        Timer->start();
    }
    HWND previousWindow_ = nullptr;
    QPointer<QWidget> previousFocus_;
    QPointer<QObject> phase_;
};
}


QtViewerHost::QtViewerHost(QWidget* Main, hc::plugins::PluginViewerService::MaterializerFactory Factory,
    QtViewerSurface::ViewerChoices Choices, QtViewerSurface::ViewerSelection Selection) : QObject(Main), main_(Main),
    service_(std::make_unique<hc::viewer::ViewerService>(hc::viewer::MakeBuiltinViewerRegistry()))
{
    if (Factory) pluginViewer_ = std::make_unique<QtPluginViewerController>(Main, std::move(Factory));
    choices_ = std::move(Choices);
    selection_ = std::move(Selection);
    setObjectName("hc.viewer.host"); Main->installEventFilter(this);
    closeTimer_ = new QTimer(this); closeTimer_->setInterval(16);
    connect(closeTimer_, &QTimer::timeout, this, [this] {
        service_->Collect();
        if (!IsIdle()) return;
        closeTimer_->stop(); if (main_) main_->close();
    });
}
QtViewerHost::~QtViewerHost()
{
    // Normal window closure reaches here after the close-event drain. Parent
    // destruction without a close event still cancels and joins owned work.
    CloseAll();
}
QtViewerSurface* QtViewerHost::CreateSurface(QWidget* Parent)
{
    std::erase_if(surfaces_, [](const auto& Surface) { return Surface.isNull(); });
    auto* Surface = new QtViewerSurface(*service_, Parent); surfaces_.push_back(Surface);
    if (pluginViewer_) Surface->ConfigurePlugins(pluginViewer_.get(), choices_, selection_);
    connect(Surface, &QtViewerSurface::LinkRequested, Surface, [Surface](const QUrl& Url) {
        if (!Url.isValid() || (Url.scheme() != "https" && Url.scheme() != "http") || Url.host().isEmpty()) return;
        auto* Prompt = new QMessageBox(QMessageBox::Question, tr("외부 링크 열기"), tr("브라우저에서 다음 주소를 여시겠습니까?"),
            QMessageBox::Yes | QMessageBox::No, Surface);
        Prompt->setAttribute(Qt::WA_DeleteOnClose); Prompt->setTextFormat(Qt::PlainText);
        Prompt->setInformativeText(Url.toDisplayString()); Prompt->setDefaultButton(QMessageBox::No);
        connect(Prompt, &QDialog::finished, Surface, [Guard = QPointer<QtViewerSurface>(Surface), Url](int Answer) {
            if (Guard && Guard->IsReady() && Answer == QMessageBox::Yes) QDesktopServices::openUrl(Url);
        });
        Prompt->open();
    });
    return Surface;
}
QDialog* QtViewerHost::OpenFull(std::shared_ptr<hc::storage::IContentSource> Source)
{
    auto* Window = new QDialog(main_); Window->setObjectName("hc.viewer.full"); Window->setAttribute(Qt::WA_DeleteOnClose);
    Window->setWindowTitle(tr("내장 뷰어")); Window->resize(960, 680);
    auto* Layout = new QVBoxLayout(Window); Layout->setContentsMargins(0, 0, 0, 0);
    auto* Surface = CreateSurface(Window); Layout->addWidget(Surface);
    connect(Surface, &QtViewerSurface::CloseRequested, Window, &QDialog::close);
    connect(Window, &QDialog::finished, Surface, [Surface] { Surface->Close(); });
    connect(Surface, &QtViewerSurface::Ready, Window, [Window, Surface] { Window->setWindowTitle(QString::fromStdString(Surface->Snapshot().source_.name_) + tr(" — 읽기 전용")); });
    Surface->Open(std::move(Source)); Window->show(); return Window;
}
void QtViewerHost::PreparePlugin(hc::plugins::PluginViewerOptions Options)
{
    if (closing_ || !pluginViewer_ || preparedWindow_ || !Options.approval_.approved_
        || Options.approval_.reviewed_.id_ != "builtin.hyperviewer"
        || Options.approval_.reviewed_.compatibility_ != "hyperviewer-v1"
        || QFileInfo(Options.binaryPath_).canonicalFilePath() != QFileInfo(QCoreApplication::applicationDirPath() + '/'
            + hc::layout::HyperViewerPackage + "/HyperViewer.dll").canonicalFilePath()) return;
    // Warm exactly one isolated, pinned package. It has no user document and
    // keeps its native parent for adoption, so no reparenting or new HWND grant
    // is needed on F3. CloseAll cancels it like every other viewer session.
    preparedOptions_ = Options;
    auto Source = preparedSource(); if (!Source) return;
    auto* Window = new ViewerDialog(main_); Window->resize(1100,760);
    Window->winId(); // Pin the hidden dialog root before granting native access.
    auto* Layout = new QVBoxLayout(Window); Layout->setContentsMargins(0,0,0,0);
    auto* Surface = new QtPluginViewerSurface(pluginViewer_.get(),Window); Layout->addWidget(Surface);
    parkPrepared(Window, Surface);
    Surface->Open(std::move(Options), std::move(Source));
}
std::shared_ptr<hc::storage::IContentSource> QtViewerHost::preparedSource()
{
    if (!preparedDirectory_) preparedDirectory_ = std::make_unique<QTemporaryDir>();
    if (!preparedDirectory_->isValid()) return {};
    const auto Path = preparedDirectory_->filePath("prepare.txt");
    QFile Empty(Path); if (!Empty.open(QIODevice::WriteOnly)) return {}; Empty.close();
    auto Provider = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
    auto Entry = Provider->Resolve({hc::storage::LocatorKind::Local, Path.toStdString()}, {}).get();
    if (!Entry.HasValue()) return {};
    return std::make_shared<hc::storage::ProviderContentSource>(Provider, Entry.Value());
}
bool QtViewerHost::matchesPrepared(const hc::plugins::PluginViewerOptions& Options) const
{
    return preparedOptions_ && Options.approval_.approved_ && preparedOptions_->approval_.reviewed_ == Options.approval_.reviewed_
        && preparedOptions_->approval_.granted_ == Options.approval_.granted_
        && preparedOptions_->approval_.allowInProcess_ == Options.approval_.allowInProcess_
        && preparedOptions_->limits_ == Options.limits_ && preparedOptions_->purpose_ == Options.purpose_
        && preparedOptions_->defaultIniPath_ == Options.defaultIniPath_
        && preparedOptions_->binaryPath_ == Options.binaryPath_ && preparedOptions_->manifestPath_ == Options.manifestPath_
        && preparedOptions_->executable_ == Options.executable_ && preparedOptions_->flags_ == Options.flags_;
}
void QtViewerHost::parkPrepared(QDialog* Dialog, QtPluginViewerSurface* Surface)
{
    auto* Window = static_cast<ViewerDialog*>(Dialog); auto* Phase = Window->NewPhase();
    Window->escape_ = {}; Window->setObjectName("hc.document.prepared");
    // Warm windows stay hidden and are deleted explicitly; the surface destructor closes the session.
    Window->setAttribute(Qt::WA_DeleteOnClose, false); Window->setAttribute(Qt::WA_ShowWithoutActivating);
    preparedWindow_ = Window; preparedSurface_ = Surface;
    connect(Surface, &QtPluginViewerSurface::Failed, Phase, [this, Window] {
        if (preparedWindow_ != Window) return;
        preparedWindow_ = nullptr; preparedSurface_ = nullptr; Window->deleteLater();
    });
}
void QtViewerHost::retirePlugin(QDialog* Window, QtPluginViewerSurface* Surface, bool Recyclable)
{
    // ESC closed every document but left the helper running. Keep it as the
    // next warm viewer instead of starting another helper for the next F3.
    const auto Details = Surface->Details();
    if (Recyclable && !closing_ && Surface->IsReady() && Details.documentChannel_
        && Details.documentState_["closeRequested"].toBool() && (!preparedSurface_ || !preparedSurface_->IsReady()))
        if (auto Source = preparedSource()) {
            if (preparedWindow_) preparedWindow_->deleteLater(); // A slower, still starting warm viewer.
            parkPrepared(Window, Surface);
            if (Surface->OpenPrepared({{std::move(Source), {}}})) return;
            preparedWindow_ = nullptr; preparedSurface_ = nullptr;
        }
    Surface->Close(); Window->deleteLater();
}
QDialog* QtViewerHost::OpenPlugin(hc::plugins::PluginViewerOptions Options,
    std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs, const QString& Title,
    std::function<void(bool, const QString&)> Completion)
{
    if (!pluginViewer_ || Inputs.empty()) return nullptr;
    const bool Recyclable = matchesPrepared(Options);
    const bool Standalone = Options.approval_.reviewed_.compatibility_ == "hyperviewer-v1";
    const auto Warm = Recyclable && preparedWindow_ && preparedSurface_ ? preparedSurface_->Snapshot().status_ : hc::viewer::ViewerStatus::Closed;
    const bool Prepared = Warm == hc::viewer::ViewerStatus::Ready && preparedSurface_->IsReady();
    // A warm viewer that is still starting is ready sooner than a cold start:
    // adopt it now and open the documents on its first Ready.
    const bool Pending = !Prepared && (Warm == hc::viewer::ViewerStatus::Loading || Warm == hc::viewer::ViewerStatus::Ready);
    auto* Window = Prepared || Pending ? static_cast<ViewerDialog*>(preparedWindow_.data()) : new ViewerDialog(main_);
    auto* Surface = Prepared || Pending ? preparedSurface_.data() : nullptr;
    if (Prepared || Pending) { preparedWindow_ = nullptr; preparedSurface_ = nullptr; Window->setAttribute(Qt::WA_ShowWithoutActivating, false); }
    // Closing goes through retirePlugin, which may keep the window as the next warm viewer.
    auto* Phase = Window->NewPhase();
    Window->setObjectName("hc.document.workspace"); Window->setAttribute(Qt::WA_DeleteOnClose, false);
    Window->setWindowTitle(Title); Window->resize(1100, 760);
    if (!Surface) {
        auto* Layout = new QVBoxLayout(Window); Layout->setContentsMargins(0, 0, 0, 0);
        Surface = new QtPluginViewerSurface(pluginViewer_.get(), Window); Layout->addWidget(Surface);
    }
    if (Options.approval_.reviewed_.compatibility_ == "hyperviewer-v1")
        Window->escape_ = [Surface] { if (Surface->IsReady()) Surface->SendDocument({{"action", "escape"}}); };
    const auto Expected = Inputs.size();
    auto PreparedInputs = Prepared || Pending ? Inputs : std::vector<hc::plugins::HostedViewerSession::DocumentInput>{};
    Options.writer_ = Inputs.front().writer_; auto First = Inputs.front().source_; Inputs.erase(Inputs.begin());
    if (Prepared || Pending) Inputs.clear();
    // Any other close destroyed the HWND, so only the helper's accepted close can recycle.
    connect(Window, &QDialog::finished, Phase, [this, Window, Surface] { retirePlugin(Window, Surface, false); });
    connect(Surface, &QtPluginViewerSurface::CloseRequested, Phase, [this, Window, Surface, Recyclable] {
        Window->Dismiss(); retirePlugin(Window, Surface, Recyclable);
    });
    connect(Surface, &QtPluginViewerSurface::Failed, Phase, [this, Window, Completion, Binary = Options.binaryPath_](const QString& Code) {
        Window->setWindowTitle(QObject::tr("문서를 열 수 없습니다: %1").arg(Code)); if (Completion) Completion(false, Code);
        if (reviewPlugin_) hc::plugins::OfferPluginReview(Window, Code, Binary, [Window, Review = reviewPlugin_](const QString& Path) { Window->close(); Review(Path); });
    });
    connect(Surface, &QtPluginViewerSurface::Ready, Phase, [Surface, Window, Phase, Expected, Completion, Standalone, Waiting = Pending,
        PreparedInputs, Inputs = std::move(Inputs)](const QString&) mutable {
        if (std::exchange(Waiting, false)) {
            // The adopted warm viewer finished starting on its placeholder.
            if (Surface->OpenPrepared(std::exchange(PreparedInputs, {}))) return;
            Window->setWindowTitle(QObject::tr("문서를 열 수 없습니다: %1").arg("document-channel-not-ready"));
            if (Completion) Completion(false, "document-channel-not-ready");
            return;
        }
        if (!Inputs.empty()) { const auto Queued = Surface->OpenDocuments(std::exchange(Inputs, {})); if (!Queued.HasValue()) { if (Completion) Completion(false, QString::fromStdString(Queued.Failure().code_)); return; } }
        if (!Completion) return;
        if (!Surface->Details().documentChannel_) { Completion(true, {}); return; }
        auto* Timer = new QTimer(Phase); auto Elapsed = std::make_shared<QElapsedTimer>(); Elapsed->start();
        connect(Timer, &QTimer::timeout, Phase, [Timer, Elapsed, Surface, Window, Phase, Expected, Completion, Standalone, EpubPresented=false]() mutable {
            const auto Details = Surface->Details(); const auto Documents = Details.documentState_["documents"].toArray();
            QString Error; bool Loading = Documents.size() != Expected;
            if (Details.commandError_) Error = QString::fromStdString(Details.commandError_->code_);
            for (const auto& Item : Documents) { const auto Record = Item.toObject(); Loading |= Record["loading"].toBool(); if (!Record["error"].toString().isEmpty()) Error = Record["error"].toString(); }
            if(Standalone && !EpubPresented && Error.isEmpty() && Documents.size()==Expected) {
                bool EpubSource=false, Bound=true;
                for(const auto& Item:Documents) {
                    const auto Record=Item.toObject(); Bound &= !Record["sourceToken"].toString().isEmpty();
                    EpubSource |= Record["view"].toObject()["viewId"]=="epub" && Record["sourceReady"].toBool();
                }
                if(EpubSource && Bound) {
                    // The EPUB first frame requires a visible surface. Original-provider
                    // bindings are complete; completion still waits for actual rendering.
                    const auto Presented=Surface->SendDocument({{"action","present"}});
                    if(Presented.HasValue()) { EpubPresented=true; Window->hide(); }
                    else Error=QString::fromStdString(Presented.Failure().code_);
                }
            }
            if (Error.isEmpty() && Loading && Elapsed->elapsed() < 30000) return;
            if (Error.isEmpty() && Loading) Error = "document-open-timeout";
            Timer->stop(); Timer->deleteLater();
            if (Error.isEmpty() && Standalone) {
                // Hide the loading dialog before the separate top-level viewer
                // is presented. Hiding an active QDialog afterwards can return
                // the foreground to HyperCommander over the viewer.
                Window->hide();
                const auto Presented = Surface->SendDocument({{"action", "present"}});
                if (!Presented.HasValue()) {
                    Error = QString::fromStdString(Presented.Failure().code_);
                    Window->show();
                }
                else {
                    auto* PresentTimer = new QTimer(Phase);
                    auto PresentElapsed = std::make_shared<QElapsedTimer>(); PresentElapsed->start();
                    connect(PresentTimer, &QTimer::timeout, Phase,
                        [PresentTimer, PresentElapsed, Surface, Window, Completion, Sequence = Presented.Value()] {
                            const auto Details = Surface->Details();
                            if (Details.commandSequence_ < Sequence && PresentElapsed->elapsed() < 10000) return;
                            PresentTimer->stop(); PresentTimer->deleteLater();
                            const auto Failure = Details.commandError_ ? QString::fromStdString(Details.commandError_->code_)
                                : Details.commandSequence_ < Sequence ? QString("viewer-present-timeout") : QString{};
                            if (Failure.isEmpty()) {
                                // The viewer belongs to the isolated host. Its own
                                // activation request can be denied while HC is in
                                // front, so the foreground owner hands it focus.
                                bool Parsed = false;
                                const auto Value = Details.documentReply_["nativeWindow"].toString().toULongLong(&Parsed, 16);
                                const auto Viewer = reinterpret_cast<HWND>(quintptr(Value));
                                DWORD Process = 0;
                                const auto Main = Window->parentWidget()
                                    ? reinterpret_cast<HWND>(Window->parentWidget()->winId()) : nullptr;
                                if (Parsed && Viewer && Details.host_.processId_
                                    && GetWindowThreadProcessId(Viewer, &Process) && Process == Details.host_.processId_
                                    && GetAncestor(Viewer, GA_ROOT) == Viewer && IsWindowVisible(Viewer)) {
                                    if (GetForegroundWindow() == Main) {
                                        if (IsIconic(Viewer)) ShowWindow(Viewer, SW_RESTORE);
                                        SetForegroundWindow(Viewer);
                                    }
                                    // A warm helper can reject its initial focus
                                    // request before HC grants the foreground.
                                    // Replay it on the helper's GUI thread only
                                    // after its own top-level window is in front.
                                    if (GetForegroundWindow() == Viewer)
                                        Surface->SendDocument({{"action", "focus"}});
                                }
                            } else {
                                Window->setWindowTitle(QObject::tr("문서를 열 수 없습니다: %1").arg(Failure));
                                Window->show();
                            }
                            Completion(Failure.isEmpty(), Failure);
                        });
                    PresentTimer->start(25); return;
                }
            }
            if (!Error.isEmpty()) Window->setWindowTitle(QObject::tr("문서를 열 수 없습니다: %1").arg(Error));
            Completion(Error.isEmpty(), Error);
        }); Timer->start(50);
    });
    if (Prepared) {
        if (!Surface->OpenPrepared(std::move(PreparedInputs))) { Window->deleteLater(); return nullptr; }
    } else if (!Pending) Surface->Open(std::move(Options), std::move(First));
    if (preparedOptions_ && !preparedWindow_) QTimer::singleShot(500, this, [this] { if (preparedOptions_ && !closing_) PreparePlugin(*preparedOptions_); });
    Window->RememberPreviousWindow();
    Window->show(); Surface->setFocus(); return Window;
}
void QtViewerHost::CloseAll()
{
    closing_ = true;
    if (preparedWindow_) { delete preparedWindow_.data(); preparedWindow_ = nullptr; preparedSurface_ = nullptr; }
    for (auto& Surface : surfaces_) if (Surface) Surface->Close();
    service_->CancelAll();
    if (pluginViewer_) pluginViewer_->CloseAll();
}
bool QtViewerHost::IsIdle() const { return service_->IsIdle() && (!pluginViewer_ || pluginViewer_->IsIdle()); }
bool QtViewerHost::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == main_ && Event->type() == QEvent::Close)
    {
        CloseAll();
        if (!IsIdle())
        { static_cast<QCloseEvent*>(Event)->ignore(); closeTimer_->start(); return true; }
    }
    return QObject::eventFilter(Object, Event);
}
