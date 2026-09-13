#include "qtViewerHost.hpp"
#include "qtViewerProvider.hpp"
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QMessageBox>
#include <QTimer>
#include <QVBoxLayout>

QtViewerHost::QtViewerHost(QWidget* Main, hc::plugins::PluginViewerService::MaterializerFactory Factory,
    QtViewerSurface::ViewerChoices Choices, QtViewerSurface::ViewerSelection Selection) : QObject(Main), main_(Main),
    service_(std::make_unique<hc::viewer::ViewerService>(hc::viewer::MakeBuiltinViewerRegistry()))
{
    if (Factory) wlx_ = std::make_unique<QtPluginViewerController>(Main, std::move(Factory));
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
    if (wlx_) Surface->ConfigurePlugins(wlx_.get(), choices_, selection_);
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
void QtViewerHost::CloseAll()
{
    for (auto& Surface : surfaces_) if (Surface) Surface->Close();
    service_->CancelAll();
    if (wlx_) wlx_->CloseAll();
}
bool QtViewerHost::IsIdle() const { return service_->IsIdle() && (!wlx_ || wlx_->IsIdle()); }
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
