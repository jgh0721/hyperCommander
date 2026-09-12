#include "qtViewerSurface.hpp"
#include <QAction>
#include "qtViewerProvider.hpp"
#include <QBuffer>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

using namespace hc::viewer;
namespace
{
    class MarkdownDocument final : public QTextDocument
    {
    public:
        using QTextDocument::QTextDocument;
    protected:
        // An invalid QVariant asks Qt to try its fallback resource loaders.
        // A valid empty payload completes the request without filesystem I/O.
        QVariant loadResource(int, const QUrl&) override { return QByteArray{}; }
    };
    class MarkdownBrowser final : public QTextBrowser
    {
    public:
        explicit MarkdownBrowser(QWidget* Parent) : QTextBrowser(Parent) { setDocument(new MarkdownDocument(this)); }
    protected:
        // Markdown/HTML image URLs must never trigger filesystem or network I/O.
        QVariant loadResource(int, const QUrl&) override { return QByteArray{}; }
    };
    class ImageCanvas final : public QWidget
    {
    public:
        using QWidget::QWidget;
        void SetDocument(std::shared_ptr<const ViewerDocument> Document)
        {
            image_ = {}; document_ = std::move(Document);
            if (document_) image_ = QImage(reinterpret_cast<const uchar*>(document_->pixels_.data()),
                document_->width_, document_->height_, document_->width_ * 4, QImage::Format_RGBA8888);
            update();
        }
    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter Painter(this); Painter.fillRect(rect(), palette().base());
            Painter.setRenderHint(QPainter::SmoothPixmapTransform);
            if (!image_.isNull()) Painter.drawImage(rect(), image_);
        }
    private:
        std::shared_ptr<const ViewerDocument> document_;
        QImage image_;
    };
}
struct QtViewerSurface::State
{
    ViewerService& service_;
    ViewerRequest request_;
    std::optional<ViewerOpenHandle> handle_;
    std::shared_ptr<const ViewerDocument> document_;
    QTimer* timer_ = nullptr;
    QLabel* status_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    MarkdownBrowser* markdown_ = nullptr;
    QScrollArea* imageScroll_ = nullptr;
    ImageCanvas* image_ = nullptr;
    QLineEdit* search_ = nullptr;
    QWidget* searchBar_ = nullptr;
    QAction* wrap_ = nullptr;
    QAction* source_ = nullptr;
    QAction* stop_ = nullptr;
    QAction* fit_ = nullptr;
    bool ready_ = false, inserting_ = false, fitImage_ = true;
    std::size_t inserted_ = 0;
    QTextCursor insertion_;
    double imageScale_ = 1;
    QtWlxSurface* wlx_ = nullptr;
    QMenu* wlxMenu_ = nullptr;
    WlxChoices wlxChoices_;
    WlxSelection wlxSelection_;
    std::vector<WlxChoice> automaticChoices_;
    bool automaticWlx_ = false;
    std::optional<WlxChoice> selectedWlx_;
    std::vector<QAction*> builtinActions_;
    explicit State(ViewerService& Service) : service_(Service) {}
};
QtViewerSurface::QtViewerSurface(ViewerService& Service, QWidget* Parent)
    : QWidget(Parent), state_(std::make_unique<State>(Service))
{
    auto& S = *state_; setObjectName("hc.viewer.surface"); setMinimumSize(240, 180);
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(4, 4, 4, 4); Layout->setSpacing(4);
    auto* Toolbar = new QToolBar(this); Toolbar->setObjectName("hc.viewer.toolbar"); Toolbar->setIconSize(QSize(20, 20)); Layout->addWidget(Toolbar);
    const auto Action = [&](const QString& Text, const QString& Command, const QKeySequence& Shortcut = {}) {
        auto* Value = Toolbar->addAction(Text); Value->setObjectName("hc.viewer." + Command);
        Value->setShortcut(Shortcut); Value->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(Value);
        connect(Value, &QAction::triggered, this, [this, Command] { Execute(Command); }); return Value;
    };
    Action(tr("찾기"), "find", QKeySequence::Find);
    Action(tr("−"), "zoom-out", QKeySequence::ZoomOut)->setToolTip(tr("축소"));
    Action(tr("+"), "zoom-in", QKeySequence::ZoomIn)->setToolTip(tr("확대"));
    S.fit_ = Action(tr("맞춤"), "fit");
    S.stop_ = Action(tr("중지"), "cancel");
    auto* Options = Toolbar->addAction(tr("보기")); Options->setObjectName("hc.viewer.options");
    auto* Menu = new QMenu(Toolbar); Options->setMenu(Menu);
    if (auto* Button = qobject_cast<QToolButton*>(Toolbar->widgetForAction(Options))) Button->setPopupMode(QToolButton::InstantPopup);
    S.wrap_ = Menu->addAction(tr("자동 줄 바꿈")); S.wrap_->setCheckable(true); S.wrap_->setObjectName("hc.viewer.wrap");
    S.source_ = Menu->addAction(tr("Markdown 원문")); S.source_->setCheckable(true); S.source_->setObjectName("hc.viewer.source");
    auto* Original = Menu->addAction(tr("이미지 원본 크기")); connect(Original, &QAction::triggered, this, [this] { Execute("original-size"); });
    auto* Fallback = Menu->addAction(tr("텍스트로 다시 열기")); Fallback->setObjectName("hc.viewer.fallback");
    connect(Fallback, &QAction::triggered, this, [this] { state_->request_.provider_ = "builtin.text"; reopen(); });
    auto* Encoding = Menu->addMenu(tr("텍스트 인코딩"));
    for (const auto& Name : {"auto", "utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "cp949", "system"})
    {
        auto* Value = Encoding->addAction(QString::fromLatin1(Name)); Value->setObjectName("hc.viewer.encoding." + QString::fromLatin1(Name));
        connect(Value, &QAction::triggered, this, [this, Name] { state_->request_.encoding_ = Name; state_->request_.provider_ = "builtin.text"; reopen(); });
    }
    S.searchBar_ = new QWidget(this); auto* SearchLayout = new QHBoxLayout(S.searchBar_); SearchLayout->setContentsMargins(0, 0, 0, 0);
    S.search_ = new QLineEdit(S.searchBar_); S.search_->setObjectName("hc.viewer.search"); S.search_->setPlaceholderText(tr("찾을 내용")); S.search_->setAccessibleName(tr("찾을 내용"));
    SearchLayout->addWidget(S.search_);
    for (const bool Backward : {true, false})
    {
        auto* Button = new QToolButton(S.searchBar_); Button->setText(Backward ? tr("이전") : tr("다음"));
        SearchLayout->addWidget(Button); connect(Button, &QToolButton::clicked, this, [this, Backward] { find(Backward); });
    }
    S.search_->installEventFilter(this); connect(S.search_, &QLineEdit::returnPressed, this, [this] { find(false); });
    Layout->addWidget(S.searchBar_); S.searchBar_->hide();
    S.status_ = new QLabel(tr("파일을 선택하십시오."), this); S.status_->setObjectName("hc.viewer.status"); S.status_->setTextFormat(Qt::PlainText); S.status_->setWordWrap(true); Layout->addWidget(S.status_);
    S.pages_ = new QStackedWidget(this); Layout->addWidget(S.pages_, 1);
    S.text_ = new QPlainTextEdit(S.pages_); S.text_->setObjectName("hc.viewer.text"); S.text_->setReadOnly(true); S.text_->setUndoRedoEnabled(false); S.text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto Font = QFontDatabase::systemFont(QFontDatabase::FixedFont); Font.setPointSizeF(std::max(12.0, font().pointSizeF())); S.text_->setFont(Font);
    S.markdown_ = new MarkdownBrowser(S.pages_); S.markdown_->setObjectName("hc.viewer.markdown"); S.markdown_->setOpenLinks(false); S.markdown_->setOpenExternalLinks(false);
    auto ProseFont = font(); ProseFont.setPointSizeF(std::max(12.0, ProseFont.pointSizeF())); S.markdown_->setFont(ProseFont);
    connect(S.markdown_, &QTextBrowser::anchorClicked, this, [this](const QUrl& Url) {
        if (Url.isRelative() && Url.path().isEmpty() && !Url.fragment().isEmpty()) state_->markdown_->scrollToAnchor(Url.fragment());
        else if (Url.isValid() && (Url.scheme() == "https" || Url.scheme() == "http") && !Url.host().isEmpty()) emit LinkRequested(Url);
        else state_->status_->setText(tr("이 링크 형식은 열 수 없습니다."));
    });
    S.imageScroll_ = new QScrollArea(S.pages_); S.imageScroll_->setObjectName("hc.viewer.image"); S.imageScroll_->setAlignment(Qt::AlignCenter);
    S.image_ = new ImageCanvas; S.imageScroll_->setWidget(S.image_); S.imageScroll_->viewport()->installEventFilter(this);
    S.pages_->addWidget(S.text_); S.pages_->addWidget(S.markdown_); S.pages_->addWidget(S.imageScroll_);
    connect(S.wrap_, &QAction::toggled, this, [this](bool On) { state_->text_->setLineWrapMode(On ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap); });
    connect(S.source_, &QAction::toggled, this, [this] { if (state_->document_) present(); });
    S.timer_ = new QTimer(this); S.timer_->setInterval(16); connect(S.timer_, &QTimer::timeout, this, &QtViewerSurface::poll);
    S.builtinActions_ = {findChild<QAction*>("hc.viewer.find"), findChild<QAction*>("hc.viewer.zoom-out"),
        findChild<QAction*>("hc.viewer.zoom-in"), S.wrap_, S.source_, Original};
    S.wlxMenu_ = Menu->addMenu(tr("플러그인으로 열기")); S.wlxMenu_->setObjectName("hc.viewer.plugins");
    S.wlxMenu_->menuAction()->setVisible(false);
    connect(S.wlxMenu_, &QMenu::aboutToShow, this, &QtViewerSurface::refreshWlxMenu);
    setFocusProxy(S.text_); S.fit_->setEnabled(false); S.stop_->setEnabled(false);
}
QtViewerSurface::~QtViewerSurface() { Close(); }
void QtViewerSurface::ConfigureWlx(QtWlxController* Controller, WlxChoices Choices, WlxSelection Selection)
{
    auto& S = *state_; if (S.wlx_) return;
    S.wlxChoices_ = std::move(Choices); S.wlx_ = new QtWlxSurface(Controller, S.pages_); S.pages_->addWidget(S.wlx_);
    S.wlxSelection_ = std::move(Selection);
    S.wlxMenu_->menuAction()->setVisible(true);
    connect(S.wlx_, &QtWlxSurface::Ready, this, [this](const QString& Id) {
        auto& S = *state_; if ((!S.selectedWlx_ && !S.automaticWlx_) || QString::fromStdString(S.wlx_->Snapshot().id_) != Id) return;
        if (S.automaticWlx_)
        {
            const auto Selected = S.wlx_->Details().selectedCandidate_;
            if (!Selected || *Selected >= S.automaticChoices_.size()) { fail("viewer-selection-invalid"); return; }
            S.selectedWlx_ = S.automaticChoices_[*Selected];
        }
        S.ready_ = true; S.stop_->setEnabled(false);
        if (auto* Find = findChild<QAction*>("hc.viewer.find")) Find->setEnabled(S.wlx_->CanSearch());
        S.status_->setText(tr("%1 · %2").arg(QString::fromStdString(S.wlx_->Snapshot().source_.name_), S.selectedWlx_->name_));
        emit Ready(Id);
    });
    connect(S.wlx_, &QtWlxSurface::Failed, this, [this](const QString& Code) { if (state_->selectedWlx_ || state_->automaticWlx_) fail(Code); });
    connect(S.wlx_, &QtWlxSurface::NoMatch, this, [this] {
        if (!state_->automaticWlx_) return;
        state_->request_.provider_.clear(); reopen();
    });
    connect(S.wlx_, &QtWlxSurface::CloseRequested, this, &QtViewerSurface::CloseRequested);
}
void QtViewerSurface::refreshWlxMenu()
{
    auto& S = *state_; S.wlxMenu_->clear();
    for (const bool Force : {false, true})
    {
        auto* Action = S.wlxMenu_->addAction(Force ? tr("플러그인 우선으로 자동 선택") : tr("자동 선택"));
        Action->setObjectName(Force ? "hc.viewer.plugin.force" : "hc.viewer.plugin.automatic");
        Action->setEnabled(static_cast<bool>(S.request_.source_));
        connect(Action, &QAction::triggered, this, [this, Force] {
            state_->request_.provider_ = Force ? "wlx-force" : "wlx-auto"; reopen();
        });
    }
    S.wlxMenu_->addSeparator();
    const auto Choices = S.wlxChoices_ ? S.wlxChoices_() : std::vector<WlxChoice>{};
    for (const auto& Choice : Choices)
    {
        auto* Action = S.wlxMenu_->addAction(Choice.name_); Action->setObjectName("hc.viewer.plugin." + Choice.id_);
        Action->setEnabled(static_cast<bool>(S.request_.source_));
        connect(Action, &QAction::triggered, this, [this, Id = Choice.id_] {
            // Re-read approval when executing: the menu may predate revocation.
            const auto Current = state_->wlxChoices_();
            const auto It = std::find_if(Current.begin(), Current.end(), [&](const auto& Item) { return Item.id_ == Id; });
            if (It == Current.end())
            {
                auto Request = state_->request_; Close(); state_->request_ = std::move(Request);
                fail("viewer-plugin-unavailable"); return;
            }
            state_->selectedWlx_ = *It; state_->request_.provider_ = "wlx"; reopen();
        });
    }
    if (Choices.empty()) S.wlxMenu_->addAction(tr("호스트 사용으로 설정된 플러그인이 없습니다."))->setEnabled(false);
}
void QtViewerSurface::Open(std::shared_ptr<hc::storage::IContentSource> Source, ViewerPurpose Purpose)
{
    state_->request_ = ViewerRequest{}; state_->request_.source_ = std::move(Source); state_->request_.purpose_ = Purpose;
    const auto Mode = state_->wlxSelection_ ? state_->wlxSelection_() : WlxSelectionMode::Manual;
    if (state_->wlx_ && Mode != WlxSelectionMode::Manual)
        state_->request_.provider_ = Mode == WlxSelectionMode::Force ? "wlx-force" : "wlx-auto";
    reopen();
}
void QtViewerSurface::reopen()
{
    auto Request = state_->request_; auto Wlx = state_->selectedWlx_; Close(); state_->request_ = std::move(Request); auto& S = *state_;
    if (S.wlx_ && (S.request_.provider_ == "wlx-auto" || S.request_.provider_ == "wlx-force"))
    {
        S.automaticWlx_ = true;
        // Capture names and options together; later settings edits must not
        // change the identity associated with the selected candidate index.
        S.automaticChoices_ = S.wlxChoices_ ? S.wlxChoices_() : std::vector<WlxChoice>{};
        std::vector<hc::plugins::WlxViewerOptions> Candidates;
        for (const auto& Choice : S.automaticChoices_) Candidates.push_back(Choice.options_);
        hc::plugins::WlxAutomaticPolicy Policy; Policy.force_ = S.request_.provider_ == "wlx-force";
        Policy.builtinProbe_ = CanViewBuiltinMultimedia;
        S.pages_->setCurrentWidget(S.wlx_); setFocusProxy(S.wlx_); S.searchBar_->hide();
        for (auto* Action : S.builtinActions_) Action->setEnabled(false);
        S.status_->setText(tr("뷰어를 자동 선택하는 중…")); S.stop_->setEnabled(true);
        S.wlx_->OpenAutomatic(std::move(Candidates), S.request_.source_, std::move(Policy)); return;
    }
    if (S.request_.provider_ == "wlx" && Wlx && S.wlx_)
    {
        S.selectedWlx_ = std::move(Wlx); S.pages_->setCurrentWidget(S.wlx_); setFocusProxy(S.wlx_);
        S.searchBar_->hide(); for (auto* Action : S.builtinActions_) Action->setEnabled(false);
        S.status_->setText(tr("플러그인으로 여는 중…")); S.stop_->setEnabled(true);
        S.wlx_->Open(S.selectedWlx_->options_, S.request_.source_); return;
    }
    for (auto* Action : S.builtinActions_) Action->setEnabled(true);
    S.pages_->setCurrentWidget(S.text_); setFocusProxy(S.text_);
    S.status_->setText(tr("파일을 읽는 중…"));
    auto Opened = S.service_.Open(S.request_);
    if (!Opened.HasValue()) { fail(QString::fromStdString(Opened.Failure().code_)); return; }
    S.handle_ = std::move(Opened.Value()); S.stop_->setEnabled(true); S.timer_->start();
}
void QtViewerSurface::Close()
{
    auto& S = *state_; S.timer_->stop(); S.ready_ = false;
    if (S.wlx_) S.wlx_->Close(); S.selectedWlx_.reset();
    S.automaticWlx_ = false; S.automaticChoices_.clear();
    S.text_->document()->setLayoutEnabled(false);
    if (S.inserting_) S.insertion_.endEditBlock();
    S.inserting_ = false; S.insertion_ = QTextCursor{};
    if (S.handle_) S.handle_->session_->Close(); S.handle_.reset(); S.document_.reset(); S.request_.source_.reset();
    S.text_->document()->setLayoutEnabled(false); S.text_->clear(); S.text_->document()->setLayoutEnabled(true);
    S.markdown_->clear(); S.image_->SetDocument({}); S.stop_->setEnabled(false); S.fit_->setEnabled(false);
    S.status_->setText(tr("뷰어를 닫았습니다."));
}
bool QtViewerSurface::IsReady() const { return state_->ready_; }
ViewerSnapshot QtViewerSurface::Snapshot() const
{
    if ((state_->selectedWlx_ || state_->automaticWlx_) && state_->wlx_) return state_->wlx_->Snapshot();
    if (state_->handle_) return state_->handle_->session_->Snapshot();
    ViewerSnapshot Closed; Closed.status_ = ViewerStatus::Closed; return Closed;
}
void QtViewerSurface::poll()
{
    auto& S = *state_; S.service_.Collect();
    if (S.inserting_)
    {
        QElapsedTimer Slice; Slice.start();
        while (S.inserted_ < S.document_->text_.size() && Slice.elapsed() < 8)
        {
            auto Count = std::min<std::size_t>(8192, S.document_->text_.size() - S.inserted_);
            const auto Last = S.document_->text_[S.inserted_ + Count - 1];
            if (Last >= 0xd800 && Last <= 0xdbff && S.inserted_ + Count < S.document_->text_.size()) --Count;
            S.insertion_.insertText(QString::fromUtf16(S.document_->text_.data() + S.inserted_, Count)); S.inserted_ += Count;
        }
        if (S.inserted_ == S.document_->text_.size())
        {
            S.inserting_ = false; S.insertion_.endEditBlock(); S.insertion_ = QTextCursor{};
            S.text_->document()->setLayoutEnabled(true); S.text_->moveCursor(QTextCursor::Start); finishPresentation();
        }
        return;
    }
    if (!S.handle_ || !S.handle_->opened_.valid()) { S.timer_->stop(); return; }
    if (S.handle_->opened_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    { const auto Snapshot = S.handle_->session_->Snapshot(); S.status_->setText(tr("읽는 중: %1 / %2 byte").arg(Snapshot.bytesRead_).arg(Snapshot.source_.size_)); return; }
    auto Opened = S.handle_->opened_.get();
    if (!Opened.HasValue()) { fail(QString::fromStdString(Opened.Failure().code_)); return; }
    S.document_ = S.handle_->session_->Document(); present();
}
void QtViewerSurface::present()
{
    auto& S = *state_; if (!S.document_) return;
    const auto Id = QString::fromStdString(S.handle_->session_->Snapshot().id_);
    S.ready_ = false; QPointer<QtViewerSurface> Guard(this); emit Presenting(Id);
    // Observers may replace or destroy a surface while reacting to its phase.
    if (!Guard || !state_->handle_ || QString::fromStdString(state_->handle_->session_->Snapshot().id_) != Id) return;
    S.ready_ = false;
    if (S.inserting_) S.insertion_.endEditBlock();
    S.inserting_ = false; S.insertion_ = QTextCursor{};
    S.text_->clear(); S.markdown_->clear(); S.image_->SetDocument({});
    S.source_->setEnabled(S.document_->kind_ == ViewerKind::Markdown); S.fit_->setEnabled(S.document_->kind_ == ViewerKind::Image);
    if (S.document_->kind_ == ViewerKind::Image)
    { S.pages_->setCurrentWidget(S.imageScroll_); S.image_->SetDocument(S.document_); S.fitImage_ = true; updateImage(); finishPresentation(); }
    else if (S.document_->kind_ == ViewerKind::Markdown && !S.source_->isChecked())
    { S.pages_->setCurrentWidget(S.markdown_); S.markdown_->setMarkdown(QString::fromStdU16String(S.document_->text_)); finishPresentation(); }
    else
    {
        S.pages_->setCurrentWidget(S.text_); S.inserted_ = 0; S.inserting_ = true;
        // Reflowing a multi-megabyte paragraph after every insertion is
        // quadratic. Populate incrementally, then lay out the document once.
        S.text_->document()->setLayoutEnabled(false);
        S.insertion_ = QTextCursor(S.text_->document()); S.insertion_.beginEditBlock();
        S.status_->setText(tr("본문을 표시하는 중…")); S.timer_->start();
    }
}
void QtViewerSurface::finishPresentation()
{
    auto& S = *state_; S.ready_ = true; S.timer_->stop(); S.stop_->setEnabled(false);
    const auto Info = S.handle_->session_->Snapshot();
    S.status_->setText(tr("%1 · 읽기 전용 · %2").arg(QString::fromStdString(Info.source_.name_),
        S.document_->kind_ == ViewerKind::Image ? tr("%1 × %2").arg(S.document_->width_).arg(S.document_->height_) : QString::fromStdString(S.document_->encoding_)));
    setFocusProxy(S.pages_->currentWidget()); emit Ready(QString::fromStdString(Info.id_));
}
void QtViewerSurface::fail(const QString& Code)
{
    auto& S = *state_; S.timer_->stop(); S.ready_ = S.inserting_ = false; S.stop_->setEnabled(false);
    S.status_->setText(tr("파일을 표시할 수 없습니다. 보기 메뉴에서 텍스트 또는 인코딩을 선택할 수 있습니다.\n%1").arg(Code)); emit Failed(Code);
}
void QtViewerSurface::updateImage()
{
    auto& S = *state_; if (!S.document_ || S.document_->kind_ != ViewerKind::Image) return;
    if (S.fitImage_) S.imageScale_ = std::min(static_cast<double>(S.imageScroll_->viewport()->width()) / S.document_->width_, static_cast<double>(S.imageScroll_->viewport()->height()) / S.document_->height_);
    const auto LongSide = std::max(S.document_->width_, S.document_->height_);
    const auto Minimum = S.fitImage_ ? 1.0 / LongSide : 0.01;
    S.imageScale_ = std::clamp(S.imageScale_, Minimum, std::min(16.0, static_cast<double>(QWIDGETSIZE_MAX) / LongSide));
    S.image_->resize(std::max(1, static_cast<int>(S.document_->width_ * S.imageScale_)), std::max(1, static_cast<int>(S.document_->height_ * S.imageScale_)));
}
void QtViewerSurface::find(bool Backward)
{
    if (state_->ready_ && state_->selectedWlx_ && state_->wlx_) { state_->wlx_->Search(state_->search_->text(), Backward); return; }
    auto& S = *state_; if (!S.ready_ || !S.document_ || S.search_->text().isEmpty() || S.document_->kind_ == ViewerKind::Image) return;
    const auto Flags = Backward ? QTextDocument::FindBackward : QTextDocument::FindFlags{};
    const auto Find = [&] { return S.pages_->currentWidget() == S.text_ ? S.text_->find(S.search_->text(), Flags) : S.markdown_->find(S.search_->text(), Flags); };
    if (Find()) return;
    if (S.pages_->currentWidget() == S.text_) S.text_->moveCursor(Backward ? QTextCursor::End : QTextCursor::Start);
    else S.markdown_->moveCursor(Backward ? QTextCursor::End : QTextCursor::Start);
    if (!Find()) S.status_->setText(tr("일치하는 내용이 없습니다."));
}
void QtViewerSurface::Execute(const QString& Command)
{
    auto& S = *state_;
    if (Command == "cancel") { Close(); return; }
    if (Command == "find") { S.searchBar_->show(); S.search_->setFocus(); S.search_->selectAll(); return; }
    if (S.selectedWlx_ && S.wlx_) { S.wlx_->Execute(Command); return; }
    if (!S.document_) return;
    if (Command == "source") { S.source_->toggle(); return; }
    if (Command == "wrap") { S.wrap_->toggle(); return; }
    if (S.document_->kind_ == ViewerKind::Image)
    {
        if (Command == "fit") S.fitImage_ = true;
        else if (Command == "original-size") { S.fitImage_ = false; S.imageScale_ = 1; }
        else if (Command == "zoom-in" || Command == "zoom-out") { S.fitImage_ = false; S.imageScale_ *= Command == "zoom-in" ? 1.25 : 0.8; }
        updateImage(); return;
    }
    const int Step = Command == "zoom-in" ? 1 : Command == "zoom-out" ? -1 : 0;
    if (Step)
    {
        if (S.text_->font().pointSizeF() + Step >= 8 && S.text_->font().pointSizeF() + Step <= 48) S.text_->zoomIn(Step);
        if (S.markdown_->font().pointSizeF() + Step >= 8 && S.markdown_->font().pointSizeF() + Step <= 48) S.markdown_->zoomIn(Step);
    }
    else if (Command == "copy") { if (S.pages_->currentWidget() == S.text_) S.text_->copy(); else S.markdown_->copy(); }
}
bool QtViewerSurface::eventFilter(QObject* Object, QEvent* Event)
{
    if (Object == state_->imageScroll_->viewport() && Event->type() == QEvent::Resize) updateImage();
    if (Object == state_->search_ && Event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape)
    { state_->searchBar_->hide(); state_->pages_->currentWidget()->setFocus(); return true; }
    return QWidget::eventFilter(Object, Event);
}
