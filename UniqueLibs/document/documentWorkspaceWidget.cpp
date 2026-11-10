#include "stdafx.h"
#include "documentWorkspaceWidget.hpp"
#include "hwpStartupTrace.hpp"
#include "documentWindowSize.hpp"
#include "UniqueLibs/errorPresentation.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "imageAsync.hpp"
#include "documentFocusRestore.hpp"
#include "documentProgressOverlay.hpp"
#include "recentDocuments.hpp"
#include "recentDocumentsDialog.hpp"
#include "documentStartPage.hpp"
#include "documentRecentMenu.hpp"
#include "newDocumentChooser.hpp"
#include "newEpubDialog.hpp"
#include "epubRestoreConflictDialog.hpp"
#include "commonLibs/document/epub/epubDraftStore.hpp"
#include "commonLibs/document/epub/epubTemplate.hpp"
#include <QMimeData>
#include <QCursor>
#include <QElapsedTimer>
#include <QLocale>
#include <QPainter>
#include <QSet>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QResizeEvent>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <atomic>
#include "documentViewPane.hpp"
#include "search/documentSearchPanel.hpp"
#include <QMainWindow>
#include <QDockWidget>
#include "imageFullscreen.hpp"
#include "imageReadingView.hpp"
#include "documentThemeAdapter.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "documentSettingsDialog.hpp"
#include <fmwidgets/WindowChrome.h>
#include "documentShortcutRouter.hpp"
#include "epubProcessView.hpp"
#include "epubSourceEditor.hpp"
#include "markdownDocumentView.hpp"
#include <QLineEdit>
#include <QScopedValueRollback>
#include "text/ScintillaQtDirectBackend.h"
#include <Scintilla.h>
#include "UniqueLibs/settings/appSettings.hpp"
#include "text/QTextView.h"
#include <QFileSystemWatcher>
#include <QLockFile>
#include <QInputMethod>
#include <QProgressDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDynamicPropertyChangeEvent>
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <mutex>
#include <utility>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QToolBar>
#include <QLabel>
#include <QFileDialog>
#include <QScreen>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QUuid>
#include <QTimer>
#include <QShortcut>
#include <QKeyEvent>
#include <QMenu>
#include <QStackedWidget>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QApplication>
#include <QIcon>
#include <QRegularExpression>
#include <QBuffer>
#include <QProcess>
#include <QWidgetAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QToolButton>
#include <QImageReader>
#include <QActionGroup>
#include <QContextMenuEvent>
#include "capture/CaptureOverlay.h"
#include "core/PrintManager.h"
#include "core/AppSettings.h"
#include "core/ToolbarIcons.h"
#include "core/ThemeManager.h"
#include <fmstyle/ThemeManager.h>
#include "core/ExternalChangeWatcher.h"
void InitializePortedViewResources();

namespace hc::document
{
    namespace
    {
        QJsonObject Error(const QString& Code) { return {{"error", Code}}; }
        QString Message(const QString& Code) { return hc::ui::DescribeDocumentError(Code).Summary(); }
        // A documentViews value from the applied settings (no disk read), else the product default.
        QVariant PreferenceValue(const QJsonObject& Applied, const QString& Key)
        {
            const auto Value = hc::settings::UnifiedConfig::Value(Applied["documentViews"].toObject(), Key.split('/'));
            return Value.isUndefined() ? hc::defaults::edit::ViewValue(Key) : hc::settings::UnifiedConfig::Decode(Value);
        }
        QString Life(DocumentLifecycle Value)
        {
            switch (Value) { case DocumentLifecycle::Opening: return "opening"; case DocumentLifecycle::Ready: return "ready";
            case DocumentLifecycle::Failed: return "failed"; case DocumentLifecycle::Closed: return "closed"; }
            return {};
        }
        QString SaveState(DocumentSaveState Value)
        {
            switch (Value) { case DocumentSaveState::Idle: return "idle"; case DocumentSaveState::Saving: return "saving";
            case DocumentSaveState::Conflict: return "conflict"; case DocumentSaveState::Failed: return "failed"; case DocumentSaveState::Exporting: return "exporting"; }
            return {};
        }
        void OpenFileDialog(QFileDialog* Dialog)
        {
            Dialog->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(Dialog,&QDialog::finished,Dialog,[Dialog] {
                // The host uses a manual event pump. Retire this dialog after
                // its completion callbacks, without flushing unrelated objects.
                QTimer::singleShot(0,Dialog,[Dialog] { delete Dialog; });
            });
            // A blocking dialog would stop the viewer host's pipe pump and
            // let the parent reap a healthy viewer while the user browses.
            Dialog->open();
        }
        void ChooseSavePath(QWidget* Parent,const QString& Current,const QString& ViewId,std::function<void(QString)> Completion)
        {
            // A text document whose start is .txt or .md offers that type first; other text keeps "all files".
            const auto TextSuffix=QFileInfo(Current).suffix().toLower();
            const auto TextFilters=TextSuffix=="txt"?QObject::tr("텍스트 문서 (*.txt);;Markdown (*.md *.markdown);;모든 파일 (*.*)")
                :TextSuffix=="md"||TextSuffix=="markdown"?QObject::tr("Markdown (*.md *.markdown);;텍스트 문서 (*.txt);;모든 파일 (*.*)"):QStringLiteral("Text files (*.*)");
            const auto Filters=ViewId=="image"?QStringLiteral("PNG (*.png);;JPEG (*.jpg *.jpeg);;BMP (*.bmp);;TIFF (*.tif *.tiff);;WebP (*.webp)")
                :ViewId=="pdf"?QStringLiteral("PDF (*.pdf)"):ViewId=="epub"?QStringLiteral("EPUB (*.epub)"):TextFilters;
            const auto Initial=ViewId=="epub"?QFileInfo(Current).dir().filePath(QFileInfo(Current).completeBaseName()+"-edited.epub"):Current;
            auto* Dialog=new QFileDialog(Parent,QObject::tr("다른 이름 저장"),Initial,Filters);Dialog->setAcceptMode(QFileDialog::AcceptSave);
            Dialog->setDefaultSuffix(ViewId=="image"?"png":ViewId=="pdf"?"pdf":QFileInfo(Current).suffix());
            QObject::connect(Dialog,&QFileDialog::filterSelected,Dialog,[Dialog](const QString& Filter){
                const auto Match=QRegularExpression("\\*\\.([a-z0-9]+)").match(Filter);if(Match.hasMatch())Dialog->setDefaultSuffix(Match.captured(1));
            });
            QObject::connect(Dialog,&QDialog::finished,Parent,[Dialog,Completion=std::move(Completion)](int Result){
                const auto Path=Result==QDialog::Accepted?Dialog->selectedFiles().value(0):QString{};
                // Re-enable save only after the old modal window is retired.
                // Otherwise a second shortcut can be swallowed by its teardown.
                QObject::connect(Dialog,&QObject::destroyed,Dialog->parent(),[Parent=Dialog->parent(),Completion,Path]{
                    QTimer::singleShot(0,Parent,[Completion,Path]{Completion(Path);});
                });
            });
            OpenFileDialog(Dialog);
        }
        bool StoreCopy(const QString& Path, const QByteArray& Bytes)
        {
            QSaveFile File(Path); File.setDirectWriteFallback(false);
            return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
        }
        void RefreshWorkspaceIcons(QWidget& Workspace)
        {
            ThemeManager::instance().setTheme(fm::style::isDarkVariant(fm::style::ThemeManager::instance().effectiveVariant())
                ? ThemeManager::Dark : ThemeManager::Light);
            static const QMap<QString, QString> Icons{
                {"newDocument", "new-file"}, {"openDocument", "open"}, {"saveDocument", "save"},
                {"saveDocumentAs", "save-as"}, {"reloadDocument", "refresh"},
                {"resolveDocumentConflict", "replace"}, {"closeDocument", "stop"}, {"cancelDocument", "stop"},
                {"printDocument", "print"}, {"previewDocument", "print-preview"}, {"captureScreen", "screen-capture"},
                {"editHwpDocument", "annotation-freetext"},
                {"undoDocument", "undo"}, {"redoDocument", "redo"}, {"documentPreferences", "toc-toggle"},
                {"workspaceDisplayOptions", "view-preview"},
                {"documentRecovery", "refresh"},
                {"tab.next", "chevron-right"}, {"tab.previous", "chevron-left"}, {"view.fullscreen", "fullscreen"},
                {"edit.cut", "cut"}, {"edit.copy", "copy"}, {"edit.paste", "paste"}, {"edit.selectAll", "select-all"},
                {"text.find", "search-next"}, {"text.findNext", "search-next"}, {"text.findPrev", "search-prev"},
                {"text.replace", "replace"}, {"text.goToLine", "toc-toggle"}, {"text.wordWrap", "word-wrap"},
                {"text.foldAll", "code-folding"}, {"text.unfoldAll", "code-folding"}};
            for (auto* Action : Workspace.findChildren<QAction*>()) {
                const auto NamedIcon = Action->property("documentIconName").toString();
                const auto Icon = NamedIcon.isEmpty() ? Icons.value(Action->objectName()) : NamedIcon;
                if (Icon.isEmpty()) continue;
                Action->setIcon(ToolbarIcons::iconByName(Icon));
                Action->setIconVisibleInMenu(true);
            }
        }
    }
    struct DocumentWorkspaceWidget::LocalSave
    {
        QString destination_;
        std::string content_;
        std::uint64_t revision_ = 0;
        DocumentEncoder encoder_;
        bool original_ = false, overwrite_ = false;
    };
    struct DocumentWorkspaceWidget::Tab
    {
        QString id_ = QUuid::createUuid().toString(QUuid::Id128), path_, error_;
        QString sourceToken_, pendingToken_, recoveredName_, temporarySource_, displayName_;
        // "새 문서 N" / "새 이미지 N" while the document has no file of its own; 0 otherwise.
        int untitledNumber_ = 0;
        bool untitledImage_ = false;
        // Where Save As of a recovered copy starts (the original it came from), when known.
        QString suggestedSavePath_;
        // HotExit PDF restore: the original's SHA-256 at close, and the view position to reapply once loaded.
        QString recoveryBaseline_;
        QJsonObject restorePosition_;
        QString readingPath_,readingEntry_, recoveryNotice_;
        QString navigationAfterSave_;
        QByteArray pendingHash_;
        std::shared_ptr<LocalSave> activeSave_, queuedSave_;
        bool composingSave_ = false;
        std::uint64_t checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
        std::shared_ptr<DocumentSession> session_ = std::make_shared<DocumentSession>(id_.toStdString());
        // Encoding verdict of the bytes the session holds. Updated only together with
        // the session text, never by a serialization that was rejected.
        std::optional<storage::Error> serializationError_;
        bool rejectingEdit_ = false;
        std::optional<SaveTicket> pending_;
        TextInput input_; QJsonObject readPrefix_;
        // "다른 인코딩으로 다시 읽기": later reloads of this tab decode with it too.
        QString encodingOverride_;
        // The running read or write shown by the progress overlay; kind_ None when idle.
        DocumentProgress progress_;
        bool everReady_ = false;
        // State recorded with a recent item, applied once when the tab is first ready.
        std::optional<RecentDocument> restore_;
        QPointer<DocumentViewPane> pane_;
        QPointer<QMessageBox> externalPrompt_;

        std::shared_ptr<std::stop_source> cancellation_ = std::make_shared<std::stop_source>();
        bool loading_ = true, saving_ = false, hostSlot_ = false, broker_ = false;
        bool navigating_ = false, closeRequested_ = false, choosingSavePath_ = false;
        bool archivePending_ = false, archiveUpdating_ = false, archiveDiscard_ = false;
        QString archiveDraft_;
        bool reloadAfterClose_ = false;
        bool originalAuthority_ = false, originalSave_ = true, closed_ = false, checkpointing_ = false, closeAfterSave_ = false, recovered_ = false;
        std::optional<RecoverySnapshot> checkpointReceipt_;
        std::optional<TextRecoveryState> cleanRestoreState_;
        bool hotExitRestore_ = false;
        QJsonObject preparation_;
        QByteArray pendingBytes_;
        bool Dirty() const { return session_->Snapshot().dirty_; }
        std::uint64_t Revision() const { return session_->Snapshot().revision_; }
    };
    DocumentWorkspaceWidget::DocumentWorkspaceWidget(QString RecoveryRoot, bool Hosted, QWidget* Parent)
        : QWidget(Parent), recoveryRoot_(std::move(RecoveryRoot)),
          workspaceId_(QUuid::createUuid().toString(QUuid::Id128)), hosted_(Hosted), workspace_(Hosted?0:16)
    {
        hc::document::startup::Scope BuildTrace("editor.shellControls");
        if (Hosted) { DocumentThemeAdapter::InstallLocal(*this); QImageReader::setAllocationLimit(0); }
        else DocumentThemeAdapter::Install(*qApp);
        InitializePortedViewResources();
        setWindowIcon(QIcon(Hosted ? ":/icons/app/hyperviewer.svg" : ":/icons/app/hypereditor.svg"));
        recovery_ = std::make_shared<DocumentRecoveryStore>(recoveryRoot_);
        if (!Hosted && QDir().mkpath(recoveryRoot_ + "/sessions")) {
            hotExitLock_ = std::make_shared<QLockFile>(recovery_->SessionLockPath(workspaceId_));
            hotExitLock_->setStaleLockTime(0);
            if (!hotExitLock_->tryLock()) hotExitLock_.reset();
        }
        setObjectName("documentWorkspace"); setWindowTitle(Hosted ? "HyperViewer" : "HyperEditor"); resize(hc::defaults::edit::WindowSize);
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(0);
        hc::document::startup::Scope BasicTrace("editor.basicWidgets");
        menu_ = new QMenuBar(this); toolbar_ = new QToolBar(tr("공통 도구모음"), this);
        menu_->setObjectName("mainMenu"); toolbar_->setObjectName("mainToolbar");
        toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly); toolbar_->setIconSize(QSize(24, 24));
        tabs_ = new QTabWidget(this); tabs_->setObjectName("documentTabs"); tabs_->setTabsClosable(true);
        singlePane_ = new QStackedWidget(this); singlePane_->setObjectName("singleDocument"); singlePane_->hide();
        BasicTrace.End();
        // The standalone editor uses the product title bar; a hosted viewer keeps its host's frame.
        if (!Hosted && !Parent) {
            setWindowFlag(Qt::FramelessWindowHint);
            // Native editor/foreign preview HWNDs consume edge hit tests before
            // WindowChrome sees them. Keep its six-DIP resize border host-owned.
            Layout->setContentsMargins(6, 6, 6, 6);
            hc::document::startup::Scope ChromeTrace("editor.titleChrome");
            auto* Chrome = new fm::ui::WindowChrome(this, this); Chrome->setObjectName("documentWindowChrome"); Layout->addWidget(Chrome);
        }
        // One measured status row owns the active view fields and session state.
        auto* StatusRow = new QWidget(this); StatusRow->setObjectName("documentStatusRow"); auto* StatusLayout = new QHBoxLayout(StatusRow); StatusLayout->setContentsMargins(0, 0, 8, 0); StatusLayout->setSpacing(0);
        statusSlot_ = new QWidget(StatusRow); statusSlot_->setObjectName("documentStatusSlot"); auto* SlotLayout = new QHBoxLayout(statusSlot_); SlotLayout->setContentsMargins(8, 2, 0, 2);
        StatusLayout->addWidget(statusSlot_, 1);
        statusBar_ = new DocumentStatusBar(statusSlot_); SlotLayout->addWidget(statusBar_);
        statusBar_->activated_ = [this](const QString& Id) { if(auto Tab=active())Tab->pane_->RunStatusCommand(Id); };
        statusBar_->pickerRequested_ = [this](const QString& Id, QWidget* Anchor) { if(auto Tab=active();Tab&&!Tab->loading_)Tab->pane_->OpenStatusPicker(Id,Anchor); };
        statusBar_->returnFocus_ = [this] { if(auto Tab=active())Tab->pane_->Focus(); };
        progressOverlay_ = new DocumentProgressOverlay(statusBar_); progressOverlay_->setObjectName("documentProgress");
        connect(progressOverlay_, &DocumentProgressOverlay::CancelRequested, this, [this](const QString& Token) {
            auto Tab = active(); if (!Tab || Tab->progress_.token_ != Token) return;
            progressOverlay_->SetCancelling(Token);
            if (auto* Cancel = findChild<QAction*>("cancelDocument")) Cancel->trigger();
        });
        connect(progressOverlay_, &DocumentProgressOverlay::DetailRequested, this, [this](const QString& Detail) {
            auto* Box = new hc::ui::MessageDialog(QMessageBox::Warning, tr("작업 실패"), Detail, QMessageBox::Close, this);
            Box->setAttribute(Qt::WA_DeleteOnClose); Box->setModal(false); Box->show();
        });
        auto* StatusRefresh = new QTimer(this); StatusRefresh->setObjectName("documentStatusRefresh");StatusRefresh->setSingleShot(true);StatusRefresh->setInterval(50);
        connect(StatusRefresh,&QTimer::timeout,this,[this]{update();tryFinishClose();});
        Layout->addWidget(menu_); Layout->addWidget(toolbar_); Layout->addWidget(tabs_, 1); Layout->addWidget(StatusRow);
        chrome_ = new WorkspaceChromeController(menu_, toolbar_, this);
        chrome_->SetStatusWidget(StatusRow);
        chrome_->SetToolbarRowProvider([this](bool Visible) { toolbarVisible_ = Visible; syncToolbarRow(); });
        BuildTrace.End(); hc::document::startup::Scope ActionsTrace("editor.actions");
        // Actions are workspace children; buildMenus() arranges them per the chrome design (prototype spec.js).
        auto Add = [&](const QString& Label, const char* Id, auto Callback) {
            auto* Action = new QAction(Label, this); Action->setObjectName(Id); addAction(Action);
            connect(Action, &QAction::triggered, this, Callback); return Action;
        };
        Add(tr("새 문서…"), "newDocument", [this] { requestNewDocument(nullptr); });
        Add(tr("새 텍스트 문서"), "file.newText", [this] { newDocument(NewDocumentKind::Text); });
        Add(tr("새 Markdown 문서"), "file.newMarkdown", [this] { newDocument(NewDocumentKind::Markdown); });
        Add(tr("새 EPUB 책…"), "file.newEpub", [this] { newDocument(NewDocumentKind::Epub); });
        Add(tr("새 이미지…"), "file.newImage", [this] { requestNewDocument(nullptr, true); });
        Add(tr("열기…"), "openDocument", [this] {
            auto* Dialog = new QFileDialog(this, tr("문서 열기"));
            Dialog->setFileMode(QFileDialog::ExistingFiles);
            connect(Dialog, &QFileDialog::filesSelected, this, [this](const QStringList& Paths) {
                QStringList Rejected;
                for (const auto& Path : Paths)
                    if (open(Path, false, {}, false).contains("error")) Rejected.append(Path);
                ShowShellError(Rejected.isEmpty() ? QString{} : tr("문서 한도 때문에 열지 못한 파일: %1").arg(Rejected.join(", ")));
            });
            OpenFileDialog(Dialog);
        });
        Add(tr("저장"), "saveDocument", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->choosingSavePath_) return;
            if(Tab->activeSave_){save(Tab,Tab->activeSave_->destination_,Tab->activeSave_->overwrite_,Tab->activeSave_->original_ && Tab->pane_->CanEditEpub());return;}
            if (Tab->broker_) { auto Result = prepareSave(Tab); if (Result.contains("error")) Tab->error_ = Result["error"].toString(); update(); }
            else if (Tab->originalSave_) save(Tab, Tab->path_, false, Tab->pane_->CanEditEpub());
            else {
                Tab->choosingSavePath_ = true;
                ChooseSavePath(this,saveAsStart(*Tab),Tab->pane_->ViewId(),[this,Tab](const QString& Path) {
                    Tab->choosingSavePath_ = false;
                    if (Tab->closed_) return;
                    if (!Path.isEmpty()) save(Tab, Path, true);
                    else { Tab->closeRequested_ = Tab->closeAfterSave_ = false; Tab->navigationAfterSave_.clear(); closingWorkspace_ = false; }
                });
            }
        });
        Add(tr("다른 이름으로 저장…"), "saveDocumentAs", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->choosingSavePath_ || Tab->saving_) return;
            Tab->choosingSavePath_ = true;
            ChooseSavePath(this,saveAsStart(*Tab),Tab->pane_->ViewId(),[this,Tab](const QString& Path) {
                Tab->choosingSavePath_ = false;
                // Lossy formats ask for the quality after the path (plan 8.4, D7); cancel keeps the document as is.
                const auto Suffix = QFileInfo(Path).suffix().toLower();
                if (!Tab->closed_ && !Path.isEmpty() && Tab->pane_->ViewId() == "image" && QStringList{"jpg", "jpeg", "webp"}.contains(Suffix)) {
                    bool Accepted = false;
                    const int Quality = hc::ui::InputDialog::getInt(this, tr("이미지 품질"), tr("품질 (1–100)"), 95, 1, 100, 1, &Accepted);
                    if (!Accepted) { Tab->closeRequested_ = Tab->closeAfterSave_ = false; closingWorkspace_ = false; return; }
                    Tab->pane_->Execute({{"action", "image.exportQuality"}, {"quality", Quality}});
                }
                if (!Tab->closed_ && !Path.isEmpty()) save(Tab, Path, true);
                else if (!Tab->closed_) { Tab->closeRequested_ = Tab->closeAfterSave_ = false; closingWorkspace_ = false; }
            });
        });
        Add(tr("다시 읽기"), "reloadDocument", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->Dirty() && hc::ui::MessageDialog::question(this, tr("다시 읽기"), tr("편집 내용을 폐기하고 원본을 다시 읽으시겠습니까?"),
                QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard) return;
            load(Tab, true);
        });
        Add(tr("저장 충돌 처리…"), "resolveDocumentConflict", [this] {
            auto Tab = active(); if (!Tab) return;
            hc::ui::MessageDialog Dialog(QMessageBox::Warning, tr("원본 변경"), tr("원본이 변경되었습니다. 처리 방법을 선택하십시오."), QMessageBox::Cancel, this);
            auto* Overwrite = Dialog.addButton(tr("현재 내용으로 덮어쓰기"), QMessageBox::AcceptRole);
            auto* Reload = Dialog.addButton(tr("편집 폐기 후 다시 읽기"), QMessageBox::DestructiveRole);
            auto* SaveAs = Dialog.addButton(tr("다른 이름 저장"), QMessageBox::ActionRole);
            Dialog.exec();
            if (Dialog.clickedButton() == Overwrite) save(Tab, Tab->path_, true, Tab->pane_->CanEditEpub());
            else if (Dialog.clickedButton() == Reload) load(Tab, true);
            else if (Dialog.clickedButton() == SaveAs) findChild<QAction*>("saveDocumentAs")->trigger();
        });
        Add(tr("닫기"), "closeDocument", [this] {
            auto Tab = active(); if (Tab) closeTab(int(std::distance(documents_.begin(), std::find(documents_.begin(), documents_.end(), Tab))));
        });
        Add(tr("작업 취소"), "cancelDocument", [this] {
            if (hotExitClosing_) { cancelHotExitClose(); return; }
            if (auto Tab = active()) { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->queuedSave_.reset();Tab->composingSave_=false;Tab->pane_->Cancel(); }
        });
        Add(tr("인쇄…"), "printDocument",[this]{if(auto Tab=active())Tab->pane_->Execute({{"action","print"}});});
        auto* EditHwp=Add(tr("편집"), "editHwpDocument",[this]{editHwp(active());});
        EditHwp->setVisible(false);EditHwp->setToolTip(tr("HyperEditor에서 한글 문서 편집"));
        Add(tr("인쇄 미리보기"), "previewDocument",[this]{if(auto Tab=active())Tab->pane_->Execute({{"action","printPreview"}});});
        Add(tr("화면 캡처"), "captureScreen",[this]{
            if(clipboardBroker_)return;const auto Captured=Capture::CaptureOverlay::run(this);if(!Captured.accepted||Captured.image.isNull())return;
            QDir().mkpath(recoveryRoot_);const auto Path=recoveryRoot_+"/capture-"+QUuid::createUuid().toString(QUuid::Id128)+".png";
            QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
            if(!Captured.image.save(&Buffer,"PNG")||!StoreCopy(Path,Bytes)){ShowShellError(tr("캡처 사본을 만들지 못했습니다."));return;}
            auto Reply=open(Path,false,{},false);if(auto Tab=find(Reply["documentId"].toString())){Tab->originalAuthority_=false;Tab->temporarySource_=Path;nameUntitled(Tab,true);update();}
        });
        for (const auto& Command : DocumentShortcutRegistry::Commands()) if (Command.id_ == "document.findInFiles" || Command.id_ == "document.replaceInFiles") {
            auto* Action = new QAction(Command.label_ + QStringLiteral("…"), this); Action->setObjectName(Command.id_); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Replace = Command.id_ == "document.replaceInFiles"] { openSearch(Replace); });
        }
        for (const auto& Item : {std::pair{tr("실행 취소"), QKeySequence::Undo}, std::pair{tr("다시 실행"), QKeySequence::Redo}}) {
            auto* Action = new QAction(Item.first, this); addAction(Action);
            Action->setObjectName(Item.second == QKeySequence::Undo ? "undoDocument" : "redoDocument");
            connect(Action, &QAction::triggered, this, [this, Key = Item.second] {
                if (auto Tab = active()) { if (Key == QKeySequence::Undo) Tab->pane_->Undo(); else Tab->pane_->Redo(); }
            });
        }
        auto* Fullscreen=new ImageFullscreen(this,[this]()->ImageReadingView*{const auto Tab=active();return Tab?Tab->pane_->findChild<ImageReadingView*>():nullptr;},[this](int Direction){if(auto Tab=active())if(auto* Action=Tab->pane_->findChild<QAction*>(Direction>0?"image.next":"image.previous"))Action->trigger();});Fullscreen->setObjectName("imageFullscreen");
        connect(Fullscreen, &ImageFullscreen::ActiveChanged, chrome_, &WorkspaceChromeController::SetSuppressed);
        // F11 belongs to the active view: EPUB and image readers own their fullscreen, other views fill the screen with the window.
        auto* FullscreenAction=new QAction(tr("전체 화면"),this);FullscreenAction->setObjectName("view.fullscreen");addAction(FullscreenAction);
        FullscreenAction->setProperty("documentIconName","fullscreen");
        connect(FullscreenAction,&QAction::triggered,this,[this,Fullscreen]{
            const auto Tab=active();
            if(epubFullscreen_ || (Tab && Tab->pane_->IsEpub()))toggleEpubFullscreen();
            else if(Fullscreen->Active() || (Tab && Tab->pane_->ViewId()=="image"))Fullscreen->Toggle();
            else if(auto* Window=window())Window->isFullScreen()?Window->showNormal():Window->showFullScreen();
        });
        auto* Preferences = new QAction(tr("설정…"), this);
        Preferences->setObjectName("documentPreferences"); addAction(Preferences);
        connect(Preferences, &QAction::triggered, this, &DocumentWorkspaceWidget::showPreferences);
        // Ctrl+Shift+M is the way back when the main menu is hidden.
        auto* Options = new QAction(tr("주 메뉴"), this); Options->setObjectName("workspaceDisplayOptions"); Options->setCheckable(true);
        addAction(Options);
        connect(Options, &QAction::triggered, this, [this] { setChromePart(WorkspaceChromeController::Part::Menu, !chrome_->MenuVisible()); });
        // The workspace itself has no context menu; surfaces install their own (no fall-through to every action).
        setContextMenuPolicy(Qt::DefaultContextMenu);
        auto* Restore = new QAction(tr("복구 목록…"), this); addAction(Restore);
        auto* EpubRestore=new QAction(tr("EPUB 편집 초안 복구…"),this);EpubRestore->setObjectName("epubDraftRecovery");EpubRestore->setProperty("documentIconName","refresh");addAction(EpubRestore);
        connect(EpubRestore,&QAction::triggered,this,&DocumentWorkspaceWidget::showEpubRecovery);
        auto* EpubReopen=new QAction(tr("저장본 다시 열기"),this);EpubReopen->setObjectName("epubReopenDocument");EpubReopen->setProperty("documentIconName","open");addAction(EpubReopen);
        connect(EpubReopen,&QAction::triggered,this,[this]{if(auto Tab=active())if(auto* Reopen=Tab->pane_->findChild<QAction*>("epubReopen"))Reopen->trigger();});
        Restore->setObjectName("documentRecovery");
        connect(Restore, &QAction::triggered, this, &DocumentWorkspaceWidget::ShowRecoveryManager);
        connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int Index) {
            if (searchPanel_ && tabs_->widget(Index) == searchPanel_) { tabs_->removeTab(Index); searchPanel_->hide(); return; }
            auto* Pane = tabs_->widget(Index); for (std::size_t At = 0; At < documents_.size(); ++At) if (documents_[At]->pane_ == Pane) { closeTab(int(At)); break; }
        });
        connect(tabs_, &QTabWidget::currentChanged, this, [this] {
            if(auto Tab=active()){
                setFocusProxy(Tab->pane_->InputWidget());
                if(Tab->pane_->ViewId()=="pdf" || Tab->pane_->IsProcessDocument())Tab->pane_->Focus();
            }
            update();
        });
        for (const auto& Item : {std::pair{QString("tab.next"), 1}, std::pair{QString("tab.previous"), -1}}) {
            auto* Action = new QAction(Item.second == 1 ? tr("다음 탭") : tr("이전 탭"), this); Action->setObjectName(Item.first); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Step = Item.second] {
                if (!tabbed_) return; const auto Count = tabs_->count();
                if (Count) tabs_->setCurrentIndex((tabs_->currentIndex() + Step + Count) % Count);
                if (auto Tab = active()) Tab->pane_->Focus();
            });
        }
        auto* EpubMenu=new QMenu(tr("EPUB"),this);EpubMenu->setObjectName("epubCommands");
        EpubMenu->menuAction()->setProperty("documentIconName","toc-toggle");
        for(const auto& Command:DocumentShortcutRegistry::Commands())if(Command.scope_=="epub") {
            auto* Action=EpubMenu->addAction(Command.label_);Action->setObjectName(Command.id_);addAction(Action);
            Action->setProperty("documentIconName",Command.id_.endsWith("next")?"chevron-right":Command.id_.endsWith("previous")?"chevron-left":"toc-toggle");
            connect(Action,&QAction::triggered,this,[this,Id=Command.id_]{if(auto Tab=active();Tab && Tab->pane_->IsEpub() && !Tab->pane_->IsEpubEditing())Tab->pane_->Execute({{"action","epub.reader."+Id.mid(5)}});});
        }
        for (const auto& Command : DocumentShortcutRegistry::Commands()) {
            if (!Command.id_.startsWith("text.") && !QStringList{"edit.cut", "edit.copy", "edit.paste", "edit.selectAll"}.contains(Command.id_)) continue;
            // Menu labels follow the chrome design; dialogs get an ellipsis.
            static const QMap<QString, QString> MenuLabels{{"text.find", tr("찾기…")}, {"text.replace", tr("바꾸기…")}, {"text.goToLine", tr("줄로 이동…")}};
            auto* Action = new QAction(MenuLabels.value(Command.id_, Command.label_), this); Action->setObjectName(Command.id_); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Id = Command.id_] {
                auto Tab = active(); if (!Tab || Tab->loading_) return;
                if(auto* Markdown=Tab->pane_->findChild<MarkdownDocumentView*>();Markdown&&Markdown->RunPreviewCommand(Id))return;
                auto* Text = Tab->pane_->findChild<QTextView*>();
                // Copy and select-all also apply to PDF selections, images and process views.
                if (!Text || (Tab->pane_->ViewId() != "text" && !Tab->pane_->IsEpubEditing())) { Tab->pane_->RunViewCommand(Id); return; }
                if (Id == "text.wordWrap") { Text->setWordWrap(!Text->isWordWrapEnabled()); return; }
                if (Id == "text.foldAll" || Id == "text.unfoldAll") { Text->FoldAll(Id == "text.foldAll"); return; }
                if (Id.startsWith("text.")) { if (auto* Local = Text->findChild<QAction*>(Id)) Local->trigger(); return; }
                if (Id == "edit.selectAll") { Handle({{"action", "selectAll"}, {"documentId", Tab->id_}}); return; }
                const auto Mode = Id.mid(5);
                if (Mode != "copy" && Text->isReadOnly()) return;
                if (clipboardBroker_) { clipboard(Tab, Mode); return; }
                auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
                Backend->Send(Mode == "copy" ? SCI_COPY : Mode == "cut" ? SCI_CUT : SCI_PASTE);
            });
        }
        // Del is native in the editor; the menu item only mirrors it (no window shortcut that would steal Del from trees).
        auto* Delete = new QAction(tr("삭제") + QStringLiteral("\tDel"), this); Delete->setObjectName("edit.delete"); addAction(Delete);
        connect(Delete, &QAction::triggered, this, [this] {
            auto Tab = active(); if (!Tab || Tab->loading_) return;
            auto* Text = Tab->pane_->findChild<QTextView*>(); if (!Text || Text->isReadOnly()) return;
            static_cast<ScintillaQtDirectBackend*>(Text->Backend())->Send(SCI_CLEAR);
        });
        for (const auto& Item : {std::pair{QString("epub.switchEdit"), QString("epubEdit")}, std::pair{QString("epub.switchRead"), QString("epubRead")}}) {
            auto* Action = new QAction(Item.second == "epubEdit" ? tr("편집 모드로 전환") : tr("읽기 모드로 전환"), this);
            Action->setObjectName(Item.first); Action->setProperty("documentIconName", Item.second == "epubEdit" ? "annotation-freetext" : "toc-toggle"); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Target = Item.second] {
                if (auto Tab = active()) if (auto* Mode = Tab->pane_->findChild<QAction*>(Target)) if (Mode->isEnabled()) Mode->trigger();
            });
        }
        auto* Recovery = new QTimer(this); Recovery->setInterval(500);
        connect(Recovery, &QTimer::timeout, this, &DocumentWorkspaceWidget::Checkpoint); Recovery->start();
        const QMap<QString, QString> Commands{{"newDocument", "file.new"}, {"undoDocument", "edit.undo"}, {"redoDocument", "edit.redo"}, {"openDocument", "file.openWorkspace"}, {"saveDocument", "file.save"},
            {"saveDocumentAs", "file.saveAs"}, {"closeDocument", "tab.close"}, {"reloadDocument", "file.reload"},
            {"printDocument", "file.print"}, {"previewDocument", "file.printPreview"}, {"cancelDocument", "file.cancel"},
            {"captureScreen", "capture.screen"}, {"workspaceDisplayOptions", "view.displayOptions"}, {"documentPreferences", "app.settings"}};
        qApp->installEventFilter(this);
        new DocumentFocusRestore(this,[this]()->QWidget*{auto* Focus=QApplication::focusWidget();if(searchPanel_&&Focus&&(Focus==searchPanel_||searchPanel_->isAncestorOf(Focus)))return Focus;auto Tab=active();return Tab&&!Tab->loading_?Tab->pane_->InputWidget():nullptr;},[this]{auto* Focus=QApplication::focusWidget();if(searchPanel_&&Focus&&(Focus==searchPanel_||searchPanel_->isAncestorOf(Focus)))return;auto Tab=active();if(Tab&&!Tab->loading_&&Tab->pane_->IsMarkdown())Tab->pane_->Focus();});
        if (hosted_) {
            viewerEscape_ = new QShortcut(QKeySequence(hc::defaults::edit::AuxiliaryShortcut("viewer.close")), this);
            viewerEscape_->setAutoRepeat(false);
            connect(viewerEscape_, &QShortcut::activated, this, &DocumentWorkspaceWidget::escapeViewer);
        }
        shortcutRouter_ = new DocumentShortcutRouter(this);
        // EPUB editing, image and PDF commands reach the active pane through window-level actions.
        for (const auto& Command : DocumentShortcutRegistry::Commands()) {
            if (!QStringList{"epub-edit", "epub-edit.tree", "epub-edit.source", "image", "image.comic", "pdf", "markdown"}.contains(Command.scope_) && !Command.id_.startsWith("view.zoom")) continue;
            auto* Action = new QAction(Command.label_, this); Action->setObjectName(Command.id_); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Id = Command.id_] { if (auto Tab = active(); Tab && !Tab->loading_) {if(Id=="image.fullscreen")findChild<ImageFullscreen*>("imageFullscreen")->Toggle();else Tab->pane_->RunViewCommand(Id);} });
        }
        for (const auto& Command : DocumentShortcutRegistry::Commands()) {
            const auto Name = Commands.key(Command.id_, Command.id_);
            if (auto* Action = findChild<QAction*>(Name)) {
                Action->setProperty("documentCommandId", Command.id_); Action->setProperty("documentDefaultShortcut", Command.key_);
                shortcutRouter_->Register(Command.id_, Action);
            }
        }
        buildMenus();
        if(auto* Fallback=findChild<QShortcut*>("imageFullscreenFallback"))Fallback->setEnabled(false);
        // Scopes follow the focused widget; DocumentShortcutRegistry::FocusContexts names the combinations.
        shortcutRouter_->SetEligibility([this](const QString& Id, QWidget* Focus) {
            auto Tab = active(); const auto Scope = DocumentShortcutRegistry::ScopeOf(Id);
            const auto Within = [Focus](const auto& Test) { for (auto* Widget = Focus; Widget; Widget = Widget->parentWidget()) if (Test(Widget)) return true; return false; };
            const bool InText = Within([](QWidget* Widget) { return qobject_cast<QTextView*>(Widget) != nullptr; });
            const bool EpubEditing = Tab && !Tab->loading_ && Tab->pane_->IsEpubEditing() && Tab->pane_->isAncestorOf(Focus);
            if (Scope == "epub") return Tab && !Tab->loading_ && Tab->pane_->IsEpub() && !Tab->pane_->IsEpubEditing() && !qobject_cast<QLineEdit*>(Focus) && !qobject_cast<QPlainTextEdit*>(Focus);
            if(Scope=="markdown")return Tab && !Tab->loading_ && Tab->pane_->IsMarkdown() && Tab->pane_->CanRunViewCommand(Id);
            if (Scope == "text" && Tab && Tab->pane_->IsMarkdown())return !Tab->loading_ && Tab->pane_->MarkdownSourceVisible() && InText;
            if (Scope == "text") return Tab && !Tab->loading_ && (Tab->pane_->ViewId() == "text" || (EpubEditing && InText));
            if (Scope == "edit") {
                auto* Input = Tab ? Tab->pane_->InputWidget() : nullptr;
                return Input && !Tab->loading_ && (Focus == Input || Input->isAncestorOf(Focus));
            }
            if (Scope == "epub-edit") return EpubEditing;
            if (Scope == "epub-edit.tree") return EpubEditing && Within([](QWidget* Widget) { return Widget->objectName() == "epubResourceTree"; });
            if (Scope == "epub-edit.source") return EpubEditing && InText;
            if (Id == "image.fullscreen") return Tab && !Tab->loading_ && Tab->pane_->ViewId() == "image";
            if (Scope == "image" || Scope == "image.comic" || Scope == "pdf") return Tab && !Tab->loading_ && Tab->pane_->isAncestorOf(Focus) && !qobject_cast<QLineEdit*>(Focus)
                && !qobject_cast<QAbstractSpinBox*>(Focus) && !qobject_cast<QComboBox*>(Focus) && Tab->pane_->CanRunViewCommand(Id);
            return true;
        });
        ActionsTrace.End(); hc::document::startup::Scope WatchersTrace("editor.watchers");
        const auto Override = qApp->property("documentViewsConfigDir");
        settingsPath_ = hc::settings::UnifiedConfig::FileName(Override.isValid() ? Override.toString() : hc::settings::DocumentPaths().directory_);
        settingsWatcher_ = new QFileSystemWatcher(this);
        auto* Reload = new QTimer(this); Reload->setSingleShot(true); Reload->setInterval(80);
        connect(settingsWatcher_, &QFileSystemWatcher::directoryChanged, Reload, qOverload<>(&QTimer::start));
        connect(Reload, &QTimer::timeout, this, [this] { if (settingsChanged()) refreshPreferencesAsync(); });
        recent_ = new RecentDocumentStore(Override.isValid() ? Override.toString() : hc::settings::DocumentPaths().directory_, this);
        recent_->Load();
        // Another process (HyperViewer or a second HyperEditor) changes the file in the same folder.
        connect(Reload, &QTimer::timeout, recent_, &RecentDocumentStore::ReloadIfChanged);
        if (!hosted_) {
            startPage_ = new DocumentStartPage(recent_, this); startPage_->setObjectName("documentStartPage"); startPage_->hide();
            connect(startPage_, &DocumentStartPage::OpenRequested, this, [this](const RecentDocument& Document) { openRecent(Document); });
            connect(startPage_, &DocumentStartPage::OpenFileRequested, this, [this] { if (auto* Open = findChild<QAction*>("openDocument")) Open->trigger(); });
            connect(startPage_, &DocumentStartPage::NewDocumentRequested, this, [this] { requestNewDocument(startPage_ ? startPage_->findChild<QWidget*>("startNew") : nullptr); });
            connect(startPage_, &DocumentStartPage::ShowAllRequested, this, &DocumentWorkspaceWidget::showRecentDialog);
            connect(startPage_, &DocumentStartPage::FilesDropped, this, [this](const QStringList& Paths) { for (const auto& Path : Paths) open(Path, false, {}, false); });
            setAcceptDrops(true);
        }
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive && settingsChanged()) refreshPreferencesAsync(); });
        externalWatcher_ = new mrst::ExternalChangeWatcher(this);
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileChanged, this, [this](const QString& Path) { externalChange(Path, false); });
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileVanished, this, [this](const QString& Path) { externalChange(Path, true); });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive) externalWatcher_->recheckAll(); });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed,
            this, [this] { RefreshWorkspaceIcons(*this); });
        WatchersTrace.End();
        { hc::document::startup::Scope Trace("editor.preferences"); refreshPreferences(); }
        { hc::document::startup::Scope Trace("editor.icons"); RefreshWorkspaceIcons(*this); }
        { hc::document::startup::Scope Trace("editor.windowSize"); windowSize_ = std::make_unique<DocumentWindowSize>(this, hosted_, [this](const QString& Error) { ShowShellError(Error); }); }
    }
    namespace
    {
        // A menu row that needs its own label or visibility forwards to the source command.
        // The key is shown as text so the binding is not registered twice.
        QAction* MakeProxy(QAction* Source, const QString& Label, QObject* Parent, const QString& Name = {}, bool ShowKey = true)
        {
            auto* Proxy = new QAction(Parent); if (!Name.isEmpty()) Proxy->setObjectName(Name);
            const auto Sync = [Proxy, Source, Label, ShowKey] {
                const auto Key = Source->shortcut().toString(QKeySequence::NativeText);
                Proxy->setText(ShowKey && !Key.isEmpty() ? Label + QLatin1Char('\t') + Key : Label);
                Proxy->setToolTip(Key.isEmpty() ? Label : QStringLiteral("%1 (%2)").arg(Label, Key));
                Proxy->setEnabled(Source->isEnabled()); Proxy->setIcon(Source->icon()); Proxy->setIconVisibleInMenu(true);
                Proxy->setCheckable(Source->isCheckable()); Proxy->setChecked(Source->isChecked());
            };
            Sync(); QObject::connect(Source, &QAction::changed, Proxy, Sync);
            QObject::connect(Proxy, &QAction::triggered, Source, [Source] { Source->trigger(); });
            Proxy->setProperty("documentProxyOf", Source->objectName());
            return Proxy;
        }
        QAction* Separator(QObject* Parent) { auto* Action = new QAction(Parent); Action->setSeparator(true); return Action; }
    }
    QAction* DocumentWorkspaceWidget::scoped(QAction* Action, quint32 Kinds)
    {
        if (!Action) return Action;
        viewKinds::Set(Action, Kinds); kindActions_.append(Action); return Action;
    }
    void DocumentWorkspaceWidget::buildMenus()
    {
        using K = DocumentViewKind; using namespace viewKinds;
        const auto A = [this](const char* Name) { return findChild<QAction*>(QString::fromLatin1(Name), Qt::FindDirectChildrenOnly); };
        const quint32 Editing = TextEditing;
        const quint32 Saving = All & ~Reading;
        // 파일
        fileMenu_ = menu_->addMenu(tr("파일(&F)")); fileMenu_->setObjectName("documentFileMenu");
        if (!hosted_) fileMenu_->addAction(scoped(A("newDocument"), Of({K::None, K::Text, K::MarkdownPreview, K::MarkdownEdit, K::EpubRead, K::EpubEdit})));
        fileMenu_->addAction(A("openDocument"));
        fileMenu_->addAction(scoped(A("reloadDocument"), Documents));
        fileMenu_->addAction(A("editHwpDocument"));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("saveDocument"), Saving));
        fileMenu_->addAction(scoped(A("saveDocumentAs"), Saving));
        fileMenu_->addAction(scoped(MakeProxy(A("saveDocumentAs"), tr("사본 저장…"), this, "saveDocumentCopy"), Reading));
        fileMenu_->addAction(A("epubReopenDocument"));
        fileMenu_->addAction(A("resolveDocumentConflict"));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("printDocument"), All & ~Epub));
        fileMenu_->addAction(scoped(A("previewDocument"), All & ~Epub));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("documentRecovery"), All & ~Epub));
        auto* Recover = new QMenu(tr("복구"), fileMenu_); Recover->setObjectName("documentRecoveryMenu");
        Recover->menuAction()->setProperty("documentIconName", "refresh");
        Recover->addAction(MakeProxy(A("documentRecovery"), tr("복구 목록…"), Recover, {}, false));
        Recover->addAction(A("epubDraftRecovery"));
        fileMenu_->addAction(scoped(Recover->menuAction(), Epub));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("closeDocument"), Documents));
        recentAnchor_ = fileMenu_->addSeparator();
        // The recent section is rebuilt from the in-memory list each time the menu opens (no disk access).
        connect(fileMenu_, &QMenu::aboutToShow, this, &DocumentWorkspaceWidget::refreshRecentMenu);
        // 편집
        editMenu_ = menu_->addMenu(tr("편집(&E)")); editMenu_->setObjectName("documentEditMenu");
        const quint32 Undo = Editing | Of({K::None, K::Pdf, K::Image});
        editMenu_->addAction(scoped(A("undoDocument"), Undo));
        editMenu_->addAction(scoped(A("redoDocument"), Undo));
        editMenu_->addSeparator();
        editMenu_->addAction(scoped(A("edit.cut"), Editing));
        editMenu_->addAction(scoped(A("edit.copy"), Editing | Of({K::Pdf, K::EpubRead, K::MarkdownPreview})));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("이미지 복사"), this, "edit.copyImage"), Of(K::Image)));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("현재 쪽 이미지 복사"), this, "edit.copyPage"), Of(K::Comic)));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("현재 이미지 복사"), this, "edit.copyStrip"), Of(K::Webtoon)));
        editMenu_->addAction(scoped(A("edit.paste"), Editing));
        editMenu_->addAction(scoped(A("edit.delete"), Editing));
        editMenu_->addAction(scoped(A("edit.selectAll"), Editing | Of({K::Pdf, K::MarkdownPreview})));
        editMenu_->addSeparator();
        editMenu_->addAction(scoped(A("text.find"), Editing | Of(K::MarkdownPreview)));
        if (auto* Book = A("epub.focus-search")) editMenu_->addAction(scoped(MakeProxy(Book, tr("책에서 찾기…"), this, "epub.findInBook"), Of(K::EpubRead)));
        editMenu_->addAction(scoped(A("text.replace"), Editing));
        editMenu_->addAction(scoped(A("text.findNext"), Editing));
        editMenu_->addAction(scoped(A("text.findPrev"), Editing));
        editMenu_->addAction(scoped(A("text.goToLine"), Editing));
        auto* EditAnchor = scoped(Separator(editMenu_), All); editMenu_->addAction(EditAnchor);
        editMenu_->addAction(scoped(A("document.findInFiles"), Of(K::None) | Editing | Of({K::EpubRead, K::MarkdownPreview})));
        editMenu_->addAction(scoped(A("document.replaceInFiles"), Of(K::None) | Editing | Of({K::EpubRead, K::MarkdownPreview})));
        // 보기: the view inserts its items before the common tail.
        viewMenu_ = menu_->addMenu(tr("보기(&V)")); viewMenu_->setObjectName("documentViewMenu");
        auto* ViewAnchor = viewMenu_->addSeparator();
        auto* Fullscreen = A("view.fullscreen"); Fullscreen->setText(tr("전체 화면") + QStringLiteral("\tF11"));
        viewMenu_->addAction(Fullscreen);
        chromeMenu_ = viewMenu_->addMenu(tr("화면 구성")); chromeMenu_->setObjectName("documentChromeMenu");
        chromeMenu_->menuAction()->setProperty("documentIconName", "view-preview");
        chromeMenu_->addAction(A("workspaceDisplayOptions"));
        for (const auto& Item : {std::pair{QString("view.toolbar"), tr("도구모음")}, std::pair{QString("view.statusBar"), tr("상태표시줄")}}) {
            auto* Action = new QAction(Item.second, this); Action->setObjectName(Item.first); Action->setCheckable(true); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Which = Item.first == "view.toolbar" ? WorkspaceChromeController::Part::Toolbar : WorkspaceChromeController::Part::Status, Action] {
                setChromePart(Which, Action->isChecked());
            });
            chromeMenu_->addAction(Action);
        }
        chromeMenu_->addSeparator();
        auto* ViewOnly = new QAction(tr("이 보기에만 적용"), this); ViewOnly->setObjectName("view.chromeViewOnly"); ViewOnly->setCheckable(true); addAction(ViewOnly);
        connect(ViewOnly, &QAction::toggled, this, [this](bool Enabled) { chrome_->SetActiveViewOnly(Enabled); emit ChromeSettingsChanged(chrome_->Settings()); });
        chromeMenu_->addAction(ViewOnly);
        auto* Reset = new QAction(tr("모든 보기 권장값으로"), this); Reset->setObjectName("view.chromeReset"); addAction(Reset);
        connect(Reset, &QAction::triggered, this, [this] { chrome_->ResetParts(); emit ChromeSettingsChanged(chrome_->Settings()); refreshChromeChecks(); });
        chromeMenu_->addAction(Reset);
        connect(chromeMenu_, &QMenu::aboutToShow, this, &DocumentWorkspaceWidget::refreshChromeChecks);
        themeMenu_ = viewMenu_->addMenu(tr("테마")); themeMenu_->setObjectName("documentThemeMenu");
        themeMenu_->menuAction()->setProperty("documentIconName", "theme-toggle");
        auto* Themes = new QActionGroup(themeMenu_); Themes->setExclusive(true);
        for (const auto& Mode : {QString("product"), QString("system"), QString("light"), QString("dark")}) {
            auto* Action = themeMenu_->addAction(Mode == "product" ? tr("본체 따르기") : Mode == "system" ? tr("시스템") : Mode == "light" ? tr("라이트") : tr("다크"));
            Action->setCheckable(true); Action->setData(Mode); Themes->addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Mode] {
                DocumentThemeAdapter::SetMode(Mode); auto Settings = chrome_->Settings(); Settings["themeMode"] = Mode;
                chrome_->ApplySettings(Settings); emit ChromeSettingsChanged(Settings);
            });
        }
        connect(themeMenu_, &QMenu::aboutToShow, this, [this] {
            const auto Mode = chrome_->Settings()["themeMode"].toString("product");
            for (auto* Action : themeMenu_->actions()) Action->setChecked(Action->data().toString() == Mode);
        });
        viewMenu_->addSeparator();
        viewMenu_->addAction(scoped(A("tab.next"), Documents));
        viewMenu_->addAction(scoped(A("tab.previous"), Documents));
        // 도구
        toolsMenu_ = menu_->addMenu(tr("도구(&T)")); toolsMenu_->setObjectName("documentToolsMenu");
        toolsMenu_->addAction(A("captureScreen"));
        toolsMenu_->addSeparator();
        auto* Keys = new QAction(tr("단축키…"), this); Keys->setObjectName("app.shortcuts"); addAction(Keys);
        connect(Keys, &QAction::triggered, this, [this] { showPreferences(); if (preferences_) preferences_->ShowPage(QStringLiteral("shortcuts")); });
        toolsMenu_->addAction(Keys);
        toolsMenu_->addAction(A("documentPreferences"));
        chrome_->SetModeMenuBefore(toolsMenu_->menuAction());
        chrome_->SetEditMenu(editMenu_, EditAnchor);
        chrome_->SetViewMenu(viewMenu_, ViewAnchor);
        // The common toolbar holds only the document group (prototype toolbar items before ‖).
        const auto Tool = [this](QAction* Source, const QString& Label, quint32 Kinds) {
            auto* Proxy = MakeProxy(Source, Label, this, QStringLiteral("toolbar.") + Source->objectName(), false);
            toolbar_->addAction(scoped(Proxy, Kinds)); return Proxy;
        };
        Tool(A("newDocument"), tr("새 문서"), Of({K::None, K::Text, K::MarkdownPreview, K::MarkdownEdit}))->setVisible(!hosted_);
        Tool(A("openDocument"), tr("열기"), All);
        Tool(A("saveDocument"), tr("저장"), Of({K::None, K::Text, K::MarkdownPreview, K::MarkdownEdit, K::Pdf, K::Image, K::EpubEdit}));
        Tool(A("printDocument"), tr("인쇄"), Of(K::Pdf));
        const quint32 History = Of({K::None, K::Text, K::MarkdownEdit, K::Image, K::EpubEdit});
        toolbar_->addAction(scoped(Separator(this), History));
        Tool(A("undoDocument"), tr("실행 취소"), History);
        Tool(A("redoDocument"), tr("다시 실행"), History);
        auto* Hwp = A("editHwpDocument"); toolbar_->addAction(Hwp);
        if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(Hwp))) Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        applyKind(DocumentViewKind::None);
    }
    DocumentViewKind DocumentWorkspaceWidget::CurrentKind() const
    {
        const auto Tab = active();
        return Tab && Tab->pane_ ? Tab->pane_->Kind() : DocumentViewKind::None;
    }
    void DocumentWorkspaceWidget::applyKind(DocumentViewKind Kind)
    {
        if (kindApplied_ && Kind == appliedKind_) return;
        kindApplied_ = true; appliedKind_ = Kind;
        for (const auto& Action : kindActions_) if (Action) Action->setVisible(viewKinds::Applies(Action, Kind));
        if (auto* Draft = findChild<QAction*>("epubDraftRecovery", Qt::FindDirectChildrenOnly)) Draft->setVisible(!hosted_);
        if (auto* New = findChild<QAction*>("toolbar.newDocument", Qt::FindDirectChildrenOnly); New && hosted_) New->setVisible(false);
    }
    void DocumentWorkspaceWidget::setChromePart(WorkspaceChromeController::Part Which, bool Visible)
    {
        chrome_->SetPart(Which, Visible, chrome_->ActiveViewOnly());
        emit ChromeSettingsChanged(chrome_->Settings());
        refreshChromeChecks();
    }
    void DocumentWorkspaceWidget::refreshChromeChecks()
    {
        const QSignalBlocker Block(this);
        if (auto* Menu = findChild<QAction*>("workspaceDisplayOptions", Qt::FindDirectChildrenOnly)) Menu->setChecked(chrome_->MenuVisible());
        if (auto* Toolbar = findChild<QAction*>("view.toolbar", Qt::FindDirectChildrenOnly)) Toolbar->setChecked(chrome_->ToolbarVisible());
        if (auto* Status = findChild<QAction*>("view.statusBar", Qt::FindDirectChildrenOnly)) Status->setChecked(chrome_->StatusVisible());
        if (auto* ViewOnly = findChild<QAction*>("view.chromeViewOnly", Qt::FindDirectChildrenOnly)) { const QSignalBlocker Quiet(ViewOnly); ViewOnly->setChecked(chrome_->ActiveViewOnly()); }
    }
    void DocumentWorkspaceWidget::updateTitle()
    {
        // The standalone editor names the document; a hosted viewer keeps its host's frame title.
        if (hosted_ || parentWidget()) return;
        const auto Tab = active();
        QString Title = QStringLiteral("HyperEditor");
        if (Tab) {
            const auto Name = documentName(*Tab);
            const auto Kind = Tab->pane_->Kind();
            const auto Mode = Kind == DocumentViewKind::EpubEdit ? tr("EPUB 편집") : Kind == DocumentViewKind::EpubRead ? tr("읽기") : QString();
            if (!Name.isEmpty()) Title = Name + (Mode.isEmpty() ? QString() : QStringLiteral(" — ") + Mode) + QStringLiteral(" — HyperEditor");
        }
        if (windowTitle() != Title) setWindowTitle(Title);
    }
    void DocumentWorkspaceWidget::contextMenuEvent(QContextMenuEvent* Event)
    {
        Event->accept();
        // Chrome surfaces do not handle the event themselves; it arrives here with the hit position.
        auto* Hit = childAt(Event->pos());
        const auto Within = [Hit](const QWidget* Widget) { return Widget && Hit && (Hit == Widget || Widget->isAncestorOf(Hit)); };
        if (Within(statusSlot_)) { showStatusMenu(Event->globalPos()); return; }
        if (Within(menu_)) { showChromeMenu(Event->globalPos(), false); return; }
        if (Within(toolbar_)) { showChromeMenu(Event->globalPos(), true); return; }
        if (tabbed_ && Within(tabs_->tabBar())) {
            const int Index = tabs_->tabBar()->tabAt(tabs_->tabBar()->mapFrom(this, Event->pos()));
            if (Index >= 0) showTabMenu(Event->globalPos(), Index);
            return;
        }
        for (const auto& Each : documents_)
            if (Each->pane_ && Within(Each->pane_->PrimaryToolbar())) { showChromeMenu(Event->globalPos(), true); return; }
    }
    DocumentWorkspaceWidget::~DocumentWorkspaceWidget()
    {
        windowSize_->Save();
        delete preferences_.data();
        // Children can emit modified/history signals while their PDF or image
        // resources are closed. Disconnect before members start destruction.
        for(const auto& Tab:documents_){Tab->closed_=true;Tab->cancellation_->request_stop();}
        for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);
        DocumentThemeAdapter::ReleaseLocal(*this);
    }
    void DocumentWorkspaceWidget::RestoreWindowSize()
    {
        windowSize_->Restore();
    }
    void DocumentWorkspaceWidget::showPreferences()
    {
        if (preferences_) { preferences_->showNormal(); preferences_->raise(); preferences_->activateWindow(); return; }
        // The DLL viewer owns its top-level window in the isolated host.
        // Preferences remain modeless so each viewer window accepts input independently.
        auto* Dialog = new DocumentSettingsDialog(settingsPath_, findChildren<QAction*>(), hosted_ ? nullptr : this);
        preferences_ = Dialog; Dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(Dialog, &DocumentSettingsDialog::PreferencesSaved, this, &DocumentWorkspaceWidget::refreshPreferences);
        connect(Dialog, &DocumentSettingsDialog::ClearRecentRequested, this, &DocumentWorkspaceWidget::clearRecent);
        connect(Dialog, &DocumentSettingsDialog::RecoveryManagerRequested, this, &DocumentWorkspaceWidget::ShowRecoveryManager);
        if (!hosted_) Dialog->setWindowModality(Qt::WindowModal);
        const auto Available = screen()->availableGeometry();
        const auto Center = mapToGlobal(rect().center());
        Dialog->move(qBound(Available.left(), Center.x() - Dialog->width() / 2, qMax(Available.left(), Available.right() - Dialog->width())),
            qBound(Available.top(), Center.y() - Dialog->height() / 2, qMax(Available.top(), Available.bottom() - Dialog->height())));
        Dialog->show();
        // Keep the preferences window visible when the host starts hidden.
        if (hosted_) ShowWindow(reinterpret_cast<HWND>(Dialog->winId()), SW_SHOWNORMAL);
        Dialog->raise(); Dialog->activateWindow();
    }
    void DocumentWorkspaceWidget::externalChange(const QString& Path, bool Missing)
    {
        for (const auto& Tab : documents_) {
            if (Tab->closed_ || Tab->loading_ || Tab->saving_ || !Tab->originalAuthority_ ||
                QString::compare(QFileInfo(Tab->path_).absoluteFilePath(), Path, Qt::CaseInsensitive) != 0) continue;
            Tab->error_ = Missing ? "document-external-missing" : "document-external-changed";
            update();
            if (Missing || Tab->externalPrompt_) return;
            if (!Tab->Dirty() && externalWatcher_->action() == mrst::ExternalChangeWatcher::Action::Reload) { load(Tab); return; }
            // Nonblocking: an IPC request must not wait on a nested dialog loop.
            auto* Dialog = new hc::ui::MessageDialog(QMessageBox::Warning, tr("원본 파일 변경"),
                tr("%1이 다른 프로그램에서 변경되었습니다. 다시 읽으면 현재 편집 내용이 대체됩니다.").arg(QFileInfo(Path).fileName()),
                QMessageBox::Yes | QMessageBox::No, hosted_ ? nullptr : this);
            Tab->externalPrompt_ = Dialog;
            Dialog->setObjectName("externalChangePrompt"); Dialog->setDefaultButton(QMessageBox::No);
            Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setWindowModality(Qt::NonModal);
            const auto Revision = Tab->Revision(); const auto Generation = Tab->session_->Snapshot().generation_;
            connect(this, &QObject::destroyed, Dialog, &QObject::deleteLater);
            connect(Dialog, &QDialog::finished, this, [this, Tab, Revision, Generation](int Result) {
                if (Result == QMessageBox::Yes && !Tab->closed_ && !Tab->loading_ && !Tab->saving_ &&
                    Revision == Tab->Revision() && Generation == Tab->session_->Snapshot().generation_) load(Tab, true);
            });
            Dialog->show(); if (hosted_) ShowWindow(reinterpret_cast<HWND>(Dialog->winId()), SW_SHOWNORMAL);
            Dialog->raise(); Dialog->activateWindow(); return;
        }
    }
    QString DocumentWorkspaceWidget::settingsSignature() const
    {
        auto Files = DocumentThemeAdapter::ProductThemeFiles(QFileInfo(settingsPath_).absolutePath());
        Files.prepend(settingsPath_);
        QStringList Parts;
        for (const auto& File : Files) {
            const auto Print = mrst::ExternalChangeWatcher::fingerprintOf(File);
            Parts.append(QStringLiteral("%1:%2:%3").arg(Print.exists).arg(Print.size).arg(Print.lastModifiedUtcMs));
        }
        return Parts.join('|');
    }
    bool DocumentWorkspaceWidget::settingsChanged() const
    {
        // A settings folder created after start is watched only once refreshPreferences() runs.
        const auto Directory = QFileInfo(settingsPath_).absolutePath();
        return settingsSignature() != settingsSignature_ || (QDir(Directory).exists() && !settingsWatcher_->directories().contains(Directory));
    }
    namespace
    {
        // Everything refreshPreferences() reads from disk; parsed off the GUI thread when it can be.
        struct PreferencesInput
        {
            QJsonObject product_, root_;
            bool loaded_ = false;
            QString error_;
        };
        PreferencesInput ReadPreferences(const QString& SettingsPath)
        {
            PreferencesInput Input;
            // The watcher sees HyperCommander.json in the same folder, so product theme edits reach open windows.
            Input.product_ = DocumentThemeAdapter::ReadProductTheme(QFileInfo(SettingsPath).absolutePath());
            Input.loaded_ = hc::settings::UnifiedConfig::Load(SettingsPath, Input.root_, Input.error_);
            return Input;
        }
    }
    void DocumentWorkspaceWidget::watchSettingsFolder()
    {
        auto Directory = QFileInfo(settingsPath_).absolutePath();
        while (!QDir(Directory).exists()) { const auto Parent = QFileInfo(Directory).absolutePath(); if (Parent == Directory) break; Directory = Parent; }
        if (QDir(Directory).exists() && !settingsWatcher_->directories().contains(Directory)) settingsWatcher_->addPath(Directory);
    }
    void DocumentWorkspaceWidget::refreshPreferences()
    {
        // Taken before reading: a write during the read is seen by the next check.
        settingsSignature_ = settingsSignature();
        // A read still in flight is older than this one and is dropped.
        ++preferencesGeneration_;
        watchSettingsFolder();
        const auto Input = ReadPreferences(settingsPath_);
        applyPreferences(Input.product_, Input.loaded_, Input.root_, Input.error_);
    }
    void DocumentWorkspaceWidget::refreshPreferencesAsync()
    {
        // The settings watcher and window activation: the product file is hundreds of KiB, and
        // reading and parsing it here stalled typing. A change during a read runs one more read.
        if (preferencesReading_) { preferencesAgain_ = true; return; }
        settingsSignature_ = settingsSignature();
        watchSettingsFolder();
        preferencesReading_ = true;
        const auto Generation = ++preferencesGeneration_;
        auto* Watcher = new QFutureWatcher<PreferencesInput>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
            const auto Input = Watcher->result(); Watcher->deleteLater(); preferencesReading_ = false;
            if (Generation == preferencesGeneration_) applyPreferences(Input.product_, Input.loaded_, Input.root_, Input.error_);
            if (std::exchange(preferencesAgain_, false) && settingsChanged()) refreshPreferencesAsync();
        });
        Watcher->setFuture(QtConcurrent::run([Path = settingsPath_] { return ReadPreferences(Path); }));
    }
    void DocumentWorkspaceWidget::applyPreferences(const QJsonObject& Product, bool Loaded, const QJsonObject& Root, const QString& Error)
    {
        DocumentThemeAdapter::ApplyProductTheme(Product);
        if (!Loaded) { ShowShellError(tr("설정을 읽지 못했습니다: %1\n%2").arg(QDir::toNativeSeparators(settingsPath_), Error)); return; }
        closeViewerOnEscape_ = Root["documentViews"].toObject()["common"].toObject()["closeViewerOnEscape"].toBool(hc::defaults::edit::ViewValue("common/closeViewerOnEscape").toBool());
        if (viewerEscape_) viewerEscape_->setEnabled(closeViewerOnEscape_);
        const auto Chrome = Root["appearance"].toObject()["documentWorkspace"].toObject();
        // The EPUB editor's saved panel layout changes while dragging and never needs a reload.
        auto Views = Root.value("documentViews").toObject(); auto EpubEditor = Views["epubEditor"].toObject(); EpubEditor.remove("layout"); Views["epubEditor"] = EpubEditor;
        const QJsonObject Relevant{{"documentViews", Views}, {"chrome", Chrome}};
        if (Relevant == appliedPreferences_) return;
        appliedPreferences_ = Relevant;
        if (searchPanel_) {
            const auto Location = Views["textView"].toObject()["findInFiles"].toObject()["openLocation"].toString("bottom");
            if (searchPanel_->property("configuredLocation").toString() != Location) { searchPanel_->setProperty("configuredLocation", Location); placeSearch(Location); }
        }
        if (externalWatcher_) externalWatcher_->reloadSettings();
        if (!Chrome.isEmpty() && !ApplyChromeSettings(Chrome)) ShowShellError(tr("문서 표시 설정이 올바르지 않습니다."));
        const bool Cover = Views["imageView"].toObject()["comic"].toObject()["coverAlone"].toBool(hc::defaults::edit::ViewValue("imageView/comic/coverAlone").toBool());
        for(auto* Reader:findChildren<ImageReadingView*>())if(Reader->State()["cover"].toBool()!=Cover)Reader->SetCover(Cover);
        for (auto* View : findChildren<QTextView*>()) View->ReloadPreferences();
        for(auto* Markdown:findChildren<MarkdownDocumentView*>())Markdown->ReloadPreferences();
        // Root is already loaded; editors apply it without reading the file again.
        for(auto* Source:findChildren<EpubSourceEditor*>())Source->ApplyValidationSettings(Root["documentViews"].toObject()["epubEditor"].toObject());
        for(const auto& Tab:documents_)Tab->pane_->SetEpubRecoveryEnabled(Root["documentViews"].toObject()["textView"].toObject()["recoveryEnabled"].toBool(hc::defaults::edit::ViewValue("textView/recoveryEnabled").toBool()));
        const auto Bindings = Root["documentViews"].toObject()["shortcuts"].toObject()["bindings"].toObject();
        const auto ShortcutError = shortcutRouter_->Apply(Bindings);
        EpubProcessView::SetDefaultBindings(settingsPath_, Bindings);
        for(auto* Epub:findChildren<EpubProcessView*>())Epub->ReaderAction("bindings",Bindings);
        for(const auto& Tab:documents_)Tab->pane_->SetShortcutBindings(shortcutRouter_->Bindings());
        if (!ShortcutError.isEmpty()) ShowShellError(tr("단축키 설정 오류 · 기본값을 사용합니다: %1").arg(ShortcutError));
        else if (const auto Unknown = DocumentShortcutRegistry::Unknown(Bindings); !Unknown.isEmpty())
            ShowShellError(tr("지원하지 않는 단축키 항목을 보존했습니다: %1").arg(Unknown.join(", ")));
    }
    void DocumentWorkspaceWidget::showEpubRecovery()
    {
        auto* Watcher=new QFutureWatcher<QList<epub::DraftInfo>>(this);++recoveryWorkers_;
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher]{
            const auto Entries=Watcher->result();Watcher->deleteLater();--recoveryWorkers_;
            if(closingWorkspace_){tryFinishClose();return;}
            if(Entries.isEmpty()){ShowShellError(tr("복구할 EPUB 편집 초안이 없습니다."));return;}
            QStringList Labels,Paths;for(const auto& Entry:Entries){Labels.append(QFileInfo(Entry.sourcePath_).fileName()+" · "+QFileInfo(Entry.path_).baseName().left(8)+(Entry.error_.isEmpty()?QString{}:tr(" · 손상 여부 확인 필요")));Paths.append(Entry.path_);}
            auto* Dialog=new QInputDialog(this);Dialog->setAttribute(Qt::WA_DeleteOnClose);Dialog->setWindowTitle(tr("EPUB 편집 초안 복구"));Dialog->setLabelText(tr("원본이 이동·삭제되어도 보관된 판본으로 복구합니다."));Dialog->setComboBoxItems(Labels);Dialog->setComboBoxEditable(false);
            connect(Dialog,&QDialog::accepted,this,[this,Dialog,Labels,Paths]{const auto Reply=openEpubRecovery(Paths.value(Labels.indexOf(Dialog->textValue())));if(Reply.contains("error"))ShowShellError(Reply["error"].toString());});Dialog->open();
        });
        Watcher->setFuture(QtConcurrent::run([Root=EpubSourceEditor::RecoveryDirectory()]{return epub::DraftStore::List(Root);}));
    }
    QJsonObject DocumentWorkspaceWidget::openEpubRecovery(const QString& Path)
    {
        if(hosted_ || closingWorkspace_)return Error("epub-draft-editor-required");
        const auto Root=EpubSourceEditor::RecoveryDirectory(),Folder=QDir(Root).filePath("sources");
        const auto Copy=QDir(Folder).filePath("restored-"+QUuid::createUuid().toString(QUuid::Id128)+".epub");
        auto* Watcher=new QFutureWatcher<epub::DraftRead>(this);++recoveryWorkers_;
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Path,Copy]{
            const auto Result=Watcher->result();Watcher->deleteLater();--recoveryWorkers_;
            if(closingWorkspace_){QFile::remove(Copy);tryFinishClose();return;}
            if(!Result){ShowShellError(tr("초안을 복원하지 못했습니다: %1 · 초안 파일은 유지됩니다.").arg(Result.error_));return;}
            const auto Reply=open(Copy,false,{},false,{},tr("복구 초안 · %1").arg(QFileInfo(Result.snapshot_.sourcePath_).fileName()));
            auto Tab=find(Reply["documentId"].toString());if(!Tab){QFile::remove(Copy);ShowShellError(Reply["error"].toString());return;}
            Tab->temporarySource_=Copy;Tab->originalAuthority_=false;Tab->originalSave_=false;Tab->recovered_=true;
            Tab->suggestedSavePath_=Result.snapshot_.sourcePath_;
            auto* Timer=new QTimer(this);Timer->setInterval(100);auto Elapsed=std::make_shared<QElapsedTimer>();
            connect(Timer,&QTimer::timeout,this,[this,Timer,Elapsed,Id=Tab->id_,Path]{
                auto Current=find(Id);if(!Current || closingWorkspace_ || (Elapsed->isValid() && Elapsed->elapsed()>60000)){Timer->stop();Timer->deleteLater();return;}
                if(Current->loading_)return;
                if(!Current->pane_->IsEpub()){Timer->stop();Timer->deleteLater();ShowShellError(tr("보관된 EPUB을 열지 못했습니다. 초안 파일은 유지됩니다."));return;}
                // Opening a recovery entry also starts in reader mode. Only the
                // EPUB Edit action authorizes entering the source editor.
                if(!Current->pane_->IsEpubEditing())return;
                if(!Elapsed->isValid())Elapsed->start();
                if(!Current->pane_->CanEditEpub())return;
                Current->pane_->Execute({{"action","epub.edit.recover"},{"path",Path}});Timer->stop();Timer->deleteLater();
            });Timer->start();update();
        });
        Watcher->setFuture(QtConcurrent::run([Root,Path,Copy,Folder]{
            auto Result=epub::DraftStore::Read(Root,Path);if(Result){epub::EditModel Model;if(!Model.Load(Result.snapshot_.original_))Result.error_=Model.Error();
                else if(QFileInfo(Folder).isSymLink() || !QDir().mkpath(Folder) || !StoreCopy(Copy,Result.snapshot_.original_))Result.error_="epub-draft-copy-write";}
            return Result;
        }));
        return {{"accepted",true}};
    }
    QJsonObject DocumentWorkspaceWidget::newDocument(NewDocumentKind Kind)
    {
        if (hotExitClosing_) return Error("document-hotexit-closing");
        if (Kind == NewDocumentKind::Image) {
            AppSettings Settings; NewImageSpec Spec;
            Spec.width_ = Settings.value("common/newImageWidth").toInt(); Spec.height_ = Settings.value("common/newImageHeight").toInt();
            Spec.background_ = Settings.value("common/newImageBackground").toString();
            return newImage(Spec);
        }
        if (Kind == NewDocumentKind::Hwpx) {
            if (hosted_) {
                if (!editRequest_.isEmpty()) return Error("document-edit-busy");
                editRequest_ = {{"token", QUuid::createUuid().toString(QUuid::Id128)}, {"format", "hwpx-new"}};
                update();
            } else if (!QProcess::startDetached(QCoreApplication::applicationDirPath() + "/HyperEditor.exe",
                {"--new-hwpx", "--config-dir", QFileInfo(settingsPath_).absolutePath()})) return Error("document-editor-start-failed");
            return {{"accepted", true}};
        }
        if (Kind == NewDocumentKind::Epub) {
            // HyperViewer never edits an EPUB: like "편집", the host starts HyperEditor (--new-epub).
            if (hosted_) {
                if (!editRequest_.isEmpty()) return Error("document-edit-busy");
                editRequest_ = {{"token", QUuid::createUuid().toString(QUuid::Id128)}, {"format", "epub-new"}, {"appearance", epubui::Appearance()}};
                update(); return {{"accepted", true}};
            }
            ShowNewEpubDialog(); return {{"accepted", true}};
        }
        if ((!hosted_ && documents_.size() >= 16) || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        const bool Markdown = Kind == NewDocumentKind::Markdown;
        // The GUID only keeps recovery files apart; the tab shows "새 문서 N" (documentName()).
        const auto Path = recoveryRoot_ + "/untitled-" + QUuid::createUuid().toString(QUuid::Id128) + (Markdown ? ".md" : ".txt");
        if (!QDir().mkpath(recoveryRoot_) || !StoreCopy(Path, {})) return Error("document-new-file-failed");
        const auto Reply = open(Path, false, {}, false);
        if (auto Tab = find(Reply["documentId"].toString())) {
            Tab->originalAuthority_ = false; Tab->originalSave_ = false; Tab->temporarySource_ = Path;
            nameUntitled(Tab, false);
            // HyperViewer's default Markdown view (preview) would show an empty page: a new one starts in source.
            if (Markdown && hosted_) { RecentDocument Start; Start.markdownView_ = QStringLiteral("source"); Tab->restore_ = Start; }
            update();
        } else QFile::remove(Path);
        return Reply;
    }
    void DocumentWorkspaceWidget::requestNewDocument(QWidget* Anchor, bool ImagePage)
    {
        if (hotExitClosing_) return;
        Q_UNUSED(Anchor);
        // file.new again while a dialog is open (normally the dialog sees the key first): make the selected kind.
        // A dialog that already chose is only waiting for deleteLater(): open a new one.
        if (chooser_ && chooser_->isVisible()) { chooser_->ActivateCurrent(); return; }
        if (imageDialog_ && imageDialog_->isVisible()) { imageDialog_->ActivateCurrent(); return; }
        AppSettings Settings;
        QKeySequence Repeat;
        if (auto* Command = findChild<QAction*>("newDocument", Qt::FindDirectChildrenOnly)) Repeat = Command->shortcut();
        if (ImagePage) { showNewImageDialog(); return; }
        const auto Mode = Settings.value("common/newDocumentAction").toString();
        if (Mode == "text" || Mode == "markdown") {
            const auto Result = newDocument(Mode == "markdown" ? NewDocumentKind::Markdown : NewDocumentKind::Text);
            if (Result.contains("error")) ShowShellError(Message(Result["error"].toString()));
            return;
        }
        // The product dialog frame, centred on this window like the other editor dialogs. open(): never block the host pump.
        NewDocumentChooser::Options Options;
        Options.initial_ = NewDocumentKindFrom(Settings.value("common/newDocumentLastKind").toString());
        Options.viewer_ = hosted_; Options.repeatKey_ = Repeat;
        Options.nextDocument_ = nextUntitledNumber(false); Options.nextImage_ = nextUntitledNumber(true);
        chooser_ = new NewDocumentChooser(this, Options);
        connect(chooser_, &NewDocumentChooser::Chosen, this, [this](NewDocumentKind Kind) {
            AppSettings Remember; Remember.setValue("common/newDocumentLastKind", NewDocumentKindId(Kind)); Remember.sync();
            // An image continues in its own dialog, after this one has gone.
            if (Kind == NewDocumentKind::Image) { QTimer::singleShot(0, this, [this] { showNewImageDialog(); }); return; }
            const auto Result = newDocument(Kind);
            if (Result.contains("error")) ShowShellError(Message(Result["error"].toString()));
        });
        chooser_->open();
    }
    void DocumentWorkspaceWidget::showNewImageDialog()
    {
        if (hotExitClosing_ || (imageDialog_ && imageDialog_->isVisible())) return;
        AppSettings Settings;
        NewImageDialog::Options Options;
        Options.nextImage_ = nextUntitledNumber(true);
        Options.lastImage_.preset_ = Settings.value("common/newImagePreset").toString();
        Options.lastImage_.width_ = Settings.value("common/newImageWidth").toInt();
        Options.lastImage_.height_ = Settings.value("common/newImageHeight").toInt();
        Options.lastImage_.background_ = Settings.value("common/newImageBackground").toString();
        if (auto* Command = findChild<QAction*>("newDocument", Qt::FindDirectChildrenOnly)) Options.repeatKey_ = Command->shortcut();
        // A restricted HyperViewer reads the clipboard through the host broker: no clipboard item there.
        if (!clipboardBroker_) if (const auto* Mime = QGuiApplication::clipboard()->mimeData(); Mime && Mime->hasImage())
            if (const auto Image = qvariant_cast<QImage>(Mime->imageData()); !Image.isNull()) Options.clipboardSize_ = Image.size();
        imageDialog_ = new NewImageDialog(this, Options);
        connect(imageDialog_, &NewImageDialog::ImageChosen, this, [this](const NewImageSpec& Spec) {
            AppSettings Remember; Remember.setValue("common/newDocumentLastKind", "image");
            Remember.setValue("common/newImageBackground", Spec.background_);
            if (!Spec.clipboard_) {
                Remember.setValue("common/newImagePreset", Spec.preset_);
                if (Spec.preset_ == "custom") { Remember.setValue("common/newImageWidth", Spec.width_); Remember.setValue("common/newImageHeight", Spec.height_); }
            }
            Remember.sync();
            const auto Result = newImage(Spec);
            if (Result.contains("error") && shellError_.isEmpty()) ShowShellError(Message(Result["error"].toString()));
        });
        imageDialog_->open();
    }
    QJsonObject DocumentWorkspaceWidget::newImage(const NewImageSpec& Spec)
    {
        if (hotExitClosing_) return Error("document-hotexit-closing");
        if ((!hosted_ && documents_.size() >= 16) || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        QImage Clip;
        if (Spec.clipboard_ && !clipboardBroker_) if (const auto* Mime = QGuiApplication::clipboard()->mimeData(); Mime && Mime->hasImage())
            Clip = qvariant_cast<QImage>(Mime->imageData());
        if (Spec.clipboard_ && Clip.isNull()) { ShowShellError(tr("클립보드에 이미지가 없습니다.")); return Error("document-new-image-clipboard"); }
        const QSize Size = Clip.isNull() ? QSize(Spec.width_, Spec.height_) : Clip.size();
        if (const auto Problem = NewImageError(Size.width(), Size.height()); !Problem.isEmpty()) { ShowShellError(Problem); return Error("document-new-image-size"); }
        QImage Image(Size, QImage::Format_ARGB32_Premultiplied);
        if (Image.isNull()) { ShowShellError(tr("새 이미지를 만들 메모리가 부족합니다.")); return Error("document-new-image-memory"); }
        Image.fill(Spec.background_ == "clear" ? QColor(Qt::transparent) : Spec.background_ == "black" ? QColor(Qt::black) : QColor(Qt::white));
        if (!Clip.isNull()) { QPainter Painter(&Image); Painter.drawImage(0, 0, Clip); }
        else if (Spec.background_ != "clear") Image = Image.convertToFormat(QImage::Format_RGB32);
        const int DotsPerMeter = qRound(qMax(1, Spec.dotsPerInch_) / 0.0254);
        Image.setDotsPerMeterX(DotsPerMeter); Image.setDotsPerMeterY(DotsPerMeter);
        QByteArray Bytes; QBuffer Buffer(&Bytes); Buffer.open(QIODevice::WriteOnly);
        const auto Path = recoveryRoot_ + "/untitled-" + QUuid::createUuid().toString(QUuid::Id128) + ".png";
        if (!Image.save(&Buffer, "PNG") || !QDir().mkpath(recoveryRoot_) || !StoreCopy(Path, Bytes)) { ShowShellError(tr("새 이미지를 만들지 못했습니다.")); return Error("document-new-file-failed"); }
        const auto Reply = open(Path, false, {}, false);
        if (auto Tab = find(Reply["documentId"].toString())) {
            // A temporary source opens in edit mode and without folder navigation (load()).
            Tab->originalAuthority_ = false; Tab->originalSave_ = false; Tab->temporarySource_ = Path;
            nameUntitled(Tab, true); update();
        } else QFile::remove(Path);
        return Reply;
    }
    void DocumentWorkspaceWidget::ShowNewEpubDialog(bool CloseWhenCancelled)
    {
        if (hosted_ || hotExitClosing_) return;
        if (epubCreating_) { ShowShellError(tr("새 EPUB을 만드는 중입니다.")); return; }
        if (documents_.size() >= 16 || (!tabbed_ && !documents_.empty())) { ShowShellError(Message("document-workspace-limit")); return; }
        if (auto* Open = findChild<NewEpubDialog*>(QString(), Qt::FindDirectChildrenOnly)) { Open->raise(); Open->activateWindow(); return; }
        AppSettings Settings;
        QString Folder;
        if (auto Tab = active(); Tab && Tab->originalAuthority_ && Tab->temporarySource_.isEmpty() && !Tab->broker_ && QFileInfo(Tab->path_).isAbsolute())
            Folder = QFileInfo(Tab->path_).absolutePath();
        if (Folder.isEmpty() || !QFileInfo(Folder).isDir()) Folder = Settings.value("common/newEpubFolder").toString();
        if (Folder.isEmpty() || !QFileInfo(Folder).isDir()) Folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const auto Language = QLocale().name().section('_', 0, 0);
        auto* Dialog = new NewEpubDialog(this, Folder, Settings.value("common/newEpubCreator").toString(), Language.isEmpty() ? QStringLiteral("ko") : Language);
        connect(Dialog, &NewEpubDialog::Requested, this, [this, CloseWhenCancelled](const NewEpubRequest& Request) { createEpub(Request, CloseWhenCancelled); });
        if (CloseWhenCancelled) connect(Dialog, &QDialog::rejected, this, [this] { if (documents_.empty() && !epubCreating_) QTimer::singleShot(0, this, [this] { close(); }); });
        Dialog->open();
    }
    void DocumentWorkspaceWidget::createEpub(const NewEpubRequest& Request, bool CloseWhenCancelled)
    {
        epubCreating_ = true; update();
        using Created = std::pair<epub::EditOutput, QString>;
        auto* Watcher = new QFutureWatcher<Created>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Request, CloseWhenCancelled] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); epubCreating_ = false;
            if (Result.second.isEmpty()) {
                AppSettings Settings;
                Settings.setValue("common/newEpubFolder", QFileInfo(Request.path_).absolutePath());
                Settings.setValue("common/newEpubCreator", Request.book_.creator_); Settings.sync();
                // The caret goes into the first chapter's empty paragraph.
                const auto Chapter = Result.first.prepared_ ? QString::fromUtf8(Result.first.prepared_->Read(QString::fromLatin1(epub::NewBookFirstChapter))) : QString{};
                const auto At = Chapter.indexOf(QStringLiteral("<p></p>"));
                const auto Reply = OpenForEpubEdit(Request.path_, {}, -1, QString::fromLatin1(epub::NewBookFirstChapter), At < 0 ? -1 : int(At) + 3);
                if (Reply.contains("error")) ShowShellError(Message(Reply["error"].toString()));
                update(); return;
            }
            // D5: the dialog closed before writing; a failure brings it back with the same values.
            const bool Exists = Result.second == "exists";
            auto* Dialog = new NewEpubDialog(this, QFileInfo(Request.path_).absolutePath(), Request.book_.creator_, Request.book_.language_);
            connect(Dialog, &NewEpubDialog::Requested, this, [this, CloseWhenCancelled](const NewEpubRequest& Next) { createEpub(Next, CloseWhenCancelled); });
            if (CloseWhenCancelled) connect(Dialog, &QDialog::rejected, this, [this] { if (documents_.empty()) QTimer::singleShot(0, this, [this] { close(); }); });
            Dialog->Restore(Request, Exists ? QString{} : tr("EPUB을 만들지 못했습니다. (%1)").arg(Result.second), Exists);
            Dialog->open(); update();
        });
        Watcher->setFuture(QtConcurrent::run([Request]() -> Created {
            auto Output = epub::CreateBook(Request.book_);
            if (!Output) return {Output, Output.error_ + (Output.location_.isEmpty() ? QString{} : ": " + Output.location_)};
            if (QFileInfo::exists(Request.path_)) return {Output, QStringLiteral("exists")};
            // The product's save path with a "must not exist" expectation: never replaces a file that appeared meanwhile.
            auto Saved = SaveTextFile(Request.path_, Output.bytes_, storage::DestinationExpectation{}, {}, {}, false, true);
            if (!Saved.HasValue()) return {Output, QFileInfo::exists(Request.path_) ? QStringLiteral("exists") : QString::fromStdString(Saved.Failure().code_)};
            return {Output, QString{}};
        }));
    }
    QJsonObject DocumentWorkspaceWidget::OpenForEpubEdit(const QString& Path, const QString& Locator, int Chapter, const QString& Resource, int Position, const QString& DraftPath)
    {
        if (hosted_) return Error("document-edit-unavailable");
        const auto Reply = open(Path, false, {}, false);
        const auto Id = Reply["documentId"].toString();
        if (Id.isEmpty() || Reply.contains("error")) return Reply;
        auto* Timer = new QTimer(this); Timer->setInterval(50);
        auto Elapsed = std::make_shared<QElapsedTimer>(); Elapsed->start();
        auto Begun = std::make_shared<bool>(false);
        connect(Timer, &QTimer::timeout, this, [this, Timer, Elapsed, Begun, Id, Locator, Chapter, Resource, Position, DraftPath] {
            const auto Stop = [Timer] { Timer->stop(); Timer->deleteLater(); };
            auto Tab = find(Id);
            if (!Tab || closingWorkspace_ || Elapsed->elapsed() > 60000) { Stop(); return; }
            if (Tab->loading_) return;
            if (!Tab->error_.isEmpty() || !Tab->pane_->IsEpub()) { Stop(); return; }
            if (!*Begun) {
                *Begun = true;
                const auto Result = Handle({{"action", "epub.edit.begin"}, {"documentId", Id}, {"locator", Locator}, {"chapter", Chapter}});
                if (Result.contains("error")) { ShowShellError(tr("EPUB 편집 모드를 열지 못했습니다: %1").arg(Result["error"].toString())); Stop(); }
                return;
            }
            if (!Tab->pane_->IsEpubEditing() || !Tab->pane_->CanEditEpub() || Tab->pane_->IsBusy()) return;
            if (!DraftPath.isEmpty()) Tab->pane_->Execute({{"action", "epub.edit.recover"}, {"path", DraftPath}});
            else if (!Resource.isEmpty()) {
                Tab->pane_->Execute({{"action", "epub.edit.select"}, {"resource", Resource}});
                if (Position >= 0) Tab->pane_->Execute({{"action", "epub.edit.cursor"}, {"position", Position}});
                if (active() == Tab) Tab->pane_->Focus();
            }
            Stop(); update();
        });
        Timer->start();
        return Reply;
    }
    QString DocumentWorkspaceWidget::documentName(const Tab& Value) const
    {
        if (!Value.recoveredName_.isEmpty()) return Value.recoveredName_;
        if (Value.untitledNumber_ > 0) {
            if (Value.untitledImage_) return tr("새 이미지 %1").arg(Value.untitledNumber_);
            const auto Suffix = QFileInfo(Value.path_).suffix().toLower();
            return Suffix == "md" || Suffix == "markdown" ? tr("새 문서 %1.md").arg(Value.untitledNumber_) : tr("새 문서 %1").arg(Value.untitledNumber_);
        }
        if (!Value.displayName_.isEmpty()) return Value.displayName_;
        return QFileInfo(Value.path_).fileName();
    }
    QString DocumentWorkspaceWidget::untitledFileName(const Tab& Value) const
    {
        if (Value.untitledImage_) return tr("새 이미지 %1.png").arg(Value.untitledNumber_);
        const auto Suffix = QFileInfo(Value.path_).suffix().toLower();
        return Suffix == "md" || Suffix == "markdown" ? tr("새 문서 %1.md").arg(Value.untitledNumber_) : tr("새 문서 %1.txt").arg(Value.untitledNumber_);
    }
    int DocumentWorkspaceWidget::nextUntitledNumber(bool Image, int Preferred) const
    {
        QSet<int> Used;
        for (const auto& Tab : documents_) if (!Tab->closed_ && Tab->untitledNumber_ > 0 && Tab->untitledImage_ == Image) Used.insert(Tab->untitledNumber_);
        if (Preferred > 0 && !Used.contains(Preferred)) return Preferred;
        int Number = 1; while (Used.contains(Number)) ++Number;
        return Number;
    }
    void DocumentWorkspaceWidget::nameUntitled(const std::shared_ptr<Tab>& Value, bool Image, int Preferred)
    {
        if (!Value) return;
        Value->untitledNumber_ = 0; Value->untitledImage_ = Image; Value->recoveredName_.clear();
        Value->untitledNumber_ = nextUntitledNumber(Image, Preferred);
    }
    QString DocumentWorkspaceWidget::saveAsStart(const Tab& Value) const
    {
        if (!Value.suggestedSavePath_.isEmpty()) return Value.suggestedSavePath_;
        if (Value.temporarySource_.isEmpty()) return Value.path_;
        QString Folder, Name = Value.untitledNumber_ > 0 ? untitledFileName(Value) : QString{};
        if (Value.input_.recoveryOrigin_) {
            const auto Origin = QString::fromStdString(Value.input_.recoveryOrigin_->localPath_);
            if (!Origin.isEmpty() && QFileInfo(Origin).isAbsolute()) {
                if (QFileInfo(QFileInfo(Origin).absolutePath()).isDir()) Folder = QFileInfo(Origin).absolutePath();
                if (Name.isEmpty()) Name = QFileInfo(Origin).fileName();
            }
        }
        if (Folder.isEmpty()) Folder = lastSaveFolder();
        if (Name.isEmpty()) Name = documentName(Value);
        if (QFileInfo(Name).suffix().isEmpty()) Name += "." + QFileInfo(Value.path_).suffix();
        return QDir(Folder).filePath(Name);
    }
    QString DocumentWorkspaceWidget::lastSaveFolder() const
    {
        const auto Folder = AppSettings().value("common/lastSaveFolder").toString();
        if (!Folder.isEmpty() && QFileInfo(Folder).isAbsolute() && QFileInfo(Folder).isDir()) return Folder;
        return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    QWidget* DocumentWorkspaceWidget::currentPane() const { if (searchPanel_ && tabs_->isVisible() && tabs_->currentWidget() == searchPanel_) return nullptr; return tabbed_ ? tabs_->currentWidget() : singlePane_->currentWidget(); }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::active() const
    { for (const auto& Tab : documents_) if (Tab->pane_ == currentPane()) return Tab; return {}; }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::find(const QString& Id) const
    { for (const auto& Tab : documents_) if (Tab->id_ == Id && !Tab->closed_) return Tab; return {}; }
    void DocumentWorkspaceWidget::activate(const std::shared_ptr<Tab>& Tab)
    {
        if (!tabbed_ && tabs_->indexOf(singlePane_) >= 0) tabs_->setCurrentWidget(singlePane_);
        if (tabbed_) tabs_->setCurrentWidget(Tab->pane_); else singlePane_->setCurrentWidget(Tab->pane_);
        workspace_.Activate(Tab->id_.toStdString()); update();
    }
    bool DocumentWorkspaceWidget::SetTabbed(bool Enabled)
    {
        if (!documents_.empty()) return false;
        if (Enabled == tabbed_) return true;
        auto* Layout = qobject_cast<QVBoxLayout*>(layout()); Layout->removeWidget(tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_);
        tabs_->hide(); singlePane_->hide(); tabbed_ = Enabled;
        Layout->insertWidget(Layout->indexOf(toolbar_) + 1, tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_, 1);
        (tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_)->show(); return true;
    }
    bool DocumentWorkspaceWidget::ApplyChromeSettings(const QJsonObject& Settings)
    {
        if (Settings.contains("themeMode") && !QStringList{"product", "system", "light", "dark"}.contains(Settings["themeMode"].toString())) return false;
        if (!chrome_->ApplySettings(Settings)) return false;
        if (Settings.contains("themeMode")) DocumentThemeAdapter::SetMode(Settings["themeMode"].toString()); return true;
    }
    QJsonObject DocumentWorkspaceWidget::ChromeSettings() const { return chrome_->Settings(); }
    void DocumentWorkspaceWidget::SetClipboardBroker(bool Enabled)
    {
        clipboardBroker_=Enabled;
        for(const auto& Tab:documents_)Tab->pane_->SetClipboardBroker(Enabled);
    }
    void DocumentWorkspaceWidget::ShowShellError(const QString& Message) { shellError_ = Message; update(); }
    bool DocumentWorkspaceWidget::IsIdle() const
    {
        return (!searchPanel_ || !searchPanel_->Busy()) && !recoveryWorkers_ && std::none_of(documents_.begin(), documents_.end(), [](const auto& Tab) {
            return Tab->archiveUpdating_ || Tab->loading_ || Tab->navigating_ || Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || Tab->checkpointing_ || !Tab->pendingToken_.isEmpty() || Tab->pane_->IsBusy();
        });
    }
    QJsonObject DocumentWorkspaceWidget::open(const QString& Path, bool HostSlot, const QString& SourceToken, bool Broker,
        const QString& RecoveryPath, const QString& DisplayName, QJsonObject ReadPrefix)
    {
        if (hotExitClosing_) return Error("document-hotexit-closing");
        if (!QFileInfo(Path).isAbsolute() || Path.contains(QChar::Null)) return Error("document-open-path");
        for (const auto& Tab : documents_) if (Tab->hostSlot_ && HostSlot) {
            if(Tab->pane_->IsProcessDocument()&&!Tab->pane_->RequestClose()) {
                pendingHostOpen_={{"path",Path},{"sourceToken",SourceToken},{"broker",Broker},{"displayName",DisplayName}};
                return {{"replacing",true},{"documentId",Tab->id_}};
            }
            if (Tab->loading_ || Tab->navigating_ || Tab->saving_ || !Tab->pendingToken_.isEmpty() || Tab->pane_->IsBusy()) {
                pendingHostOpen_ = {{"path", Path}, {"sourceToken", SourceToken}, {"broker", Broker}, {"displayName", DisplayName}};
                return {{"replacing", true}, {"documentId", Tab->id_}};
            }
            if (Tab->Dirty()) {
                if (!hosted_ && Tab->session_->Text().size() > DocumentByteLimit) return Error("p0-text-byte-limit");
                // SDK owns one slot; a newer pending Load supersedes its predecessor.
                pendingHostOpen_ = {{"path", Path}, {"sourceToken", SourceToken}, {"broker", Broker}, {"displayName", DisplayName}};
                Tab->pane_->SetReadOnly(true); Checkpoint(); return {{"replacing", true}, {"documentId", Tab->id_}};
            }
            retire(Tab, true); break;
        }
        for (const auto& Tab : documents_) if (QFileInfo(Tab->path_).canonicalFilePath() == QFileInfo(Path).canonicalFilePath()
            && !HostSlot && !QFileInfo(Path).canonicalFilePath().isEmpty() && Tab->broker_ == Broker && Tab->sourceToken_ == SourceToken) {
            activate(Tab); return {{"documentId", Tab->id_}, {"duplicate", true}};
        }
        if ((!hosted_ && documents_.size() >= 16) || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        auto Tab = std::make_shared<DocumentWorkspaceWidget::Tab>(); Tab->path_ = Path; Tab->readPrefix_ = std::move(ReadPrefix);
        Tab->session_->SetUnlimitedBytes(hosted_);
        Tab->hotExitRestore_ = pendingHotExitRestore_;
        Tab->recoveryBaseline_ = pendingRecoveryBaseline_;
        Tab->displayName_ = DisplayName;
        Tab->encodingOverride_ = std::exchange(pendingOpenEncoding_, {});
        Tab->hostSlot_ = HostSlot; Tab->sourceToken_ = SourceToken; Tab->broker_ = Broker; Tab->recovered_ = !RecoveryPath.isEmpty();
        // Authority follows the source: SDK slots have no original-provider link,
        // while explicitly opened local files are writable in either product.
        Tab->originalAuthority_ = !HostSlot && !Broker && SourceToken.isEmpty() && !Tab->recovered_; Tab->originalSave_ = Tab->originalAuthority_;
        auto* Pane = new DocumentViewPane(tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_);
        Pane->setProperty("hc.previewPath", Path);
        Pane->SetUnlimitedBytes(hosted_);
        Pane->SetShortcutBindings(shortcutRouter_->Bindings());
        Tab->pane_ = Pane;
        connect(Pane,&DocumentViewPane::SearchRequested,this,[this,Tab](const QString& Query){if(Tab->closed_)return;activate(Tab);openSearch(false);if(searchPanel_)searchPanel_->SetQuery(Query);});
        connect(Pane, &DocumentViewPane::ClipboardRequested, this, [this, Tab](const QString& Mode) { clipboard(Tab, Mode); });
        connect(Pane,&DocumentViewPane::ContentChanged,this,[this,Tab] {
            if(Tab->loading_||Tab->closed_)return;
            auto Serialized=Tab->pane_->SerializeSession();const auto& Bytes=Serialized.bytes_;
            if(Tab->pane_->ViewId()=="text" && !hosted_ && Bytes.size()>TextByteLimit) {
                Tab->error_="p0-text-byte-limit";update();
                // This runs inside the editor's change notification, where Scintilla ignores
                // undo; revert once the edit has returned. One revert per rejected edit.
                if(std::exchange(Tab->rejectingEdit_,true))return;
                QTimer::singleShot(0,this,[this,Tab]{
                    Tab->rejectingEdit_=false;if(Tab->closed_||!Tab->pane_)return;
                    {const QSignalBlocker Block(Tab->pane_);Tab->pane_->Undo();}
                    // A replacement arrives as a deletion and then an insertion, and the
                    // deletion has already reached the session. Resynchronise with the
                    // restored text so a save never writes that intermediate state, and
                    // keep the verdict of those bytes rather than the rejected ones.
                    auto Restored=Tab->pane_->SerializeSession();
                    if(Restored.bytes_.size()<=TextByteLimit && Tab->session_->Execute("document.setText",Restored.bytes_.toStdString()).HasValue())
                        Tab->serializationError_=std::move(Restored.error_);
                    Tab->error_="p0-text-byte-limit";
                    Tab->session_->SetHistoryState(Tab->pane_->CanUndo(),Tab->pane_->CanRedo());update();
                });
                return;
            }
            if(Tab->pane_->ViewId()!="text"&&Bytes.isEmpty()){Tab->error_="document-serialization-failed";update();return;}
            auto Result=Tab->session_->Execute("document.setText",Bytes.toStdString());
            if(!Result.HasValue())Tab->error_=QString::fromStdString(Result.Failure().code_);
            else {Tab->error_.clear();Tab->serializationError_=std::move(Serialized.error_);}
            Tab->session_->SetHistoryState(Tab->pane_->CanUndo(),Tab->pane_->CanRedo());update();
            if(searchPanel_)searchPanel_->SourcesChanged();
        });
        connect(Pane,&DocumentViewPane::ViewStateChanged,this,[this,Tab]{if(!Tab->closed_&&!Tab->loading_){auto* Timer=findChild<QTimer*>("documentStatusRefresh");if(!Timer->isActive())Timer->start();}});
        connect(Pane,&DocumentViewPane::EscapeRequested,this,[this,Tab]{if(active()==Tab && !Tab->closed_){if(epubFullscreen_)toggleEpubFullscreen();else escapeViewer();}});
        // Images (and comic/webtoon books) use the image full screen overlay; EPUB has its own.
        connect(Pane,&DocumentViewPane::FullscreenRequested,this,[this,Tab]{if(active()!=Tab || Tab->closed_)return;if(Tab->pane_->ViewId()=="image")findChild<ImageFullscreen*>("imageFullscreen")->Toggle();else toggleEpubFullscreen();});
        connect(Pane,&DocumentViewPane::RetryRequested,this,[this,Tab]{if(!Tab->closed_){activate(Tab);findChild<QAction*>("reloadDocument")->trigger();}});
        connect(Pane,&DocumentViewPane::ReadingVolumeChanged,this,[this,Tab](const QString& Path){
            if(Tab->closed_ || Tab->loading_)return;
            Tab->path_=Path;Tab->input_.path_=Path;Tab->readingPath_=Path;Tab->readingEntry_.clear();
            Tab->displayName_.clear();Tab->recoveredName_.clear();Tab->sourceToken_.clear();
            Tab->originalAuthority_=false;Tab->originalSave_=false;Tab->broker_=false;Tab->hostSlot_=false;
            const auto Started=Tab->session_->BeginOpen(true);
            if(Started.HasValue()){
                SourceIdentity Source{{},{},Path.toUtf8().toStdString(),{},false};
                Tab->session_->CompleteOpen(Started.Value(),OpenedDocument{std::move(Source),{}});
            }
            update();
        });
        connect(Pane,&DocumentViewPane::ReadingSelected,this,[this,Tab]{Tab->originalSave_=false;Tab->broker_=false;});
        connect(Pane,&DocumentViewPane::OpenRequested,this,[this,Tab](const QString& Path) {
            if(Tab->closed_)return;
            if(Tab->pane_->CanEditEpub() && QFileInfo(Path).canonicalFilePath()==QFileInfo(Tab->path_).canonicalFilePath()) {
                activate(Tab);findChild<QAction*>("reloadDocument")->trigger();return;
            }
            if(Tab->pane_->ViewId()=="image")navigateImage(Tab,Path);
            else open(Path,false,{},false);
        });
        connect(Pane,&DocumentViewPane::SaveRequested,this,[this,Tab] {activate(Tab);findChild<QAction*>("saveDocumentAs")->trigger();});
        connect(Pane,&DocumentViewPane::EpubSaveRequested,this,[this,Tab](bool SaveAs) {if(Tab->closed_)return;activate(Tab);findChild<QAction*>(SaveAs?"saveDocumentAs":"saveDocument")->trigger();});
        connect(Pane,&DocumentViewPane::EditEpubRequested,this,[this,Tab]{const auto Result=editEpub(Tab);if(Result.contains("error")){Tab->error_=Result["error"].toString();update();}});
        connect(Pane,&DocumentViewPane::EditImageRequested,this,[this](const QImage& Image){
            const auto Path=recoveryRoot_+"/image-copy-"+QUuid::createUuid().toString(QUuid::Id128)+".png";
            QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);QDir().mkpath(recoveryRoot_);
            if(!Image.save(&Buffer,"PNG")||!StoreCopy(Path,Bytes)){ShowShellError(tr("이미지 사본을 만들지 못했습니다."));return;}
            auto Reply=open(Path,false,{},false);if(auto Copy=find(Reply["documentId"].toString())){Copy->originalAuthority_=false;Copy->originalSave_=false;Copy->temporarySource_=Path;nameUntitled(Copy,true);update();}
        });
        connect(Pane,&DocumentViewPane::EncodingRequested,this,[this,Tab](const QString&) {Tab->error_="document-encoding-unrepresentable";update();});
        connect(Pane,&DocumentViewPane::ReinterpretRequested,this,[this,Tab](const QString& Encoding) {
            // Re-decodes the file on disk; edits are not carried over (the menu heading says so).
            if(Tab->closed_ || Tab->loading_ || Tab->temporarySource_.size())return;
            if(Tab->Dirty() && hc::ui::MessageDialog::question(this,tr("다른 인코딩으로 다시 읽기"),
                tr("편집 내용을 폐기하고 %1(으)로 다시 읽으시겠습니까?").arg(EncodingInfo(Encoding).label_),QMessageBox::Discard|QMessageBox::Cancel)!=QMessageBox::Discard)return;
            Tab->encodingOverride_=CanonicalEncodingName(Encoding);load(Tab,true);
        });
        connect(Pane,&DocumentViewPane::StatusPickerRequested,this,[this,Tab](const QString& Id){if(active()==Tab)openStatusPicker(Id);});
        documents_.push_back(Tab); workspace_.Add(Tab->session_);
        if (tabbed_) tabs_->addTab(Pane, tr("여는 중…")); else singlePane_->addWidget(Pane);
        activate(Tab); load(Tab, false, RecoveryPath); return {{"documentId", Tab->id_}, {"loading", true}};
    }
    void DocumentWorkspaceWidget::navigateImage(const std::shared_ptr<Tab>& Tab, const QString& Path)
    {
        if(Tab->closed_||Tab->loading_||Tab->navigating_||Tab->choosingSavePath_||Tab->saving_||Tab->checkpointing_||!Tab->pendingToken_.isEmpty()
            ||Tab->pane_->IsEditing()||Tab->pane_->ViewId()!="image"||!QFileInfo(Path).isAbsolute())return;
        if(Tab->Dirty()){
            const auto Choice=closePrompt_?closePrompt_(Tab->id_):int(hc::ui::MessageDialog::question(this,tr("변경 내용 저장"),
                tr("다음 이미지를 열기 전에 변경 내용을 저장하시겠습니까?"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel));
            if(Choice==QMessageBox::Save){
                Tab->navigationAfterSave_=Path;activate(Tab);findChild<QAction*>("saveDocument")->trigger();
                if(!Tab->choosingSavePath_&&!Tab->saving_&&Tab->pendingToken_.isEmpty())Tab->navigationAfterSave_.clear();
                return;
            }
            if(Choice!=QMessageBox::Discard)return;
        }
        // Decode before replacing source identity, so a failed transition keeps the old image and edits.
        const auto Revision=Tab->Revision();
        Tab->navigating_=true;Tab->pane_->setEnabled(false);Tab->error_.clear();
        Tab->cancellation_=std::make_shared<std::stop_source>();const auto Cancellation=Tab->cancellation_;
        auto* Watcher=new QFutureWatcher<storage::Result<TextInput>>(this);
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Tab,Watcher,Revision]{
            auto Result=Watcher->result();DeleteCompletedImageWork(Watcher);
            if(Tab->closed_)return;
            Tab->navigating_=false;Tab->pane_->setEnabled(true);
            if(!Result.HasValue()||Result.Value().viewId_!="image"||Result.Value().image_.isNull()||Result.Value().book_){
                Tab->error_=Result.HasValue()?"document-image-navigation-format":QString::fromStdString(Result.Failure().code_);
            }else if(Tab->Revision()!=Revision||Tab->saving_||!Tab->pendingToken_.isEmpty()){
                Tab->error_="document-image-navigation-changed";
            }else{
                auto Started=Tab->session_->BeginOpen(true);
                if(!Started.HasValue())Tab->error_=QString::fromStdString(Started.Failure().code_);
                else{
                    const auto Temporary=Tab->temporarySource_;
                    Tab->pane_->Cancel();
                    Tab->input_=std::move(Result.Value());Tab->path_=Tab->input_.path_;
                    Tab->sourceToken_.clear();Tab->broker_=false;Tab->hostSlot_=false;
                    Tab->originalAuthority_=true;Tab->originalSave_=!(Tab->input_.source_.attributes_&1u);
                    Tab->readingPath_.clear();Tab->readingEntry_.clear();Tab->displayName_.clear();Tab->recoveredName_.clear();
                    Tab->temporarySource_.clear();Tab->recovered_=false;Tab->closeAfterSave_=false;
                    Tab->checkpointedRevision_=(std::numeric_limits<std::uint64_t>::max)();
                    Tab->loading_=true;Tab->pane_->SetReadingSource({},{});
                    Tab->pane_->Load(Tab->input_,true,!hosted_);Tab->loading_=false;
                    SourceIdentity Source{Tab->input_.source_,{},Tab->path_.toUtf8().toStdString(),{},Tab->originalSave_};
                    Tab->session_->SetBinaryContent(true);
                    {auto Serialized=Tab->pane_->SerializeSession();Tab->serializationError_=Serialized.error_;Tab->session_->CompleteOpen(Started.Value(),OpenedDocument{std::move(Source),Serialized.bytes_.toStdString()});}
                    Tab->pane_->SetClipboardBroker(clipboardBroker_);
                    if(auto* Input=Tab->pane_->InputWidget()){Input->installEventFilter(this);for(auto* Child:Input->findChildren<QWidget*>())Child->installEventFilter(this);}
                    cleanupRecovery(Tab,Revision);
                    if(!Temporary.isEmpty()&&Temporary!=Tab->path_)QFile::remove(Temporary);
                }
            }
            if(active()==Tab){setFocusProxy(Tab->pane_->InputWidget());Tab->pane_->Focus();}
            update();tryFinishClose();
        });
        Watcher->setFuture(QtConcurrent::run([Path,Cancellation]{
            storage::OperationContext Context;Context.stopToken_=Cancellation->get_token();
            Context.deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            return ReadDocumentFile(Path,Context);
        }));
        update();
    }
    void DocumentWorkspaceWidget::load(const std::shared_ptr<Tab>& Tab, bool Discard, const QString& RecoveryPath)
    {
        if (Tab->choosingSavePath_ || Tab->saving_ || Tab->navigating_ || !Tab->pendingToken_.isEmpty()) return;
        if(Discard)Tab->pane_->DiscardEpubRecovery();
        if(Tab->pane_->IsProcessDocument()&&!Tab->pane_->RequestClose()) {Tab->reloadAfterClose_=true;return;}
        Tab->reloadAfterClose_=false;
        auto Started = Tab->session_->BeginOpen(Discard); if (!Started.HasValue()) { Tab->error_ = QString::fromStdString(Started.Failure().code_); update(); return; }
        Tab->cancellation_->request_stop(); Tab->cancellation_ = std::make_shared<std::stop_source>();
        Tab->loading_ = true; Tab->pane_->SetReadOnly(true);Tab->pane_->ShowLoadingPreview({}); Tab->error_.clear();
        const auto Generation = Started.Value(); auto Cancellation = Tab->cancellation_; const auto Path = Tab->path_;
        // A hosted viewer's first open is shown by the HyperCommander surface; only later reads use the overlay.
        if (!hosted_ || Tab->everReady_) {
            const auto Name = QFileInfo(Path).fileName();
            const bool Reinterpret = !Tab->encodingOverride_.isEmpty() && Tab->input_.viewId_ == "text" && Tab->everReady_;
            Tab->progress_ = {Reinterpret ? DocumentProgress::Kind::Reinterpret : DocumentProgress::Kind::Read, Tab->id_ + ":read:" + QString::number(Generation),
                Reinterpret ? tr("다시 읽는 중 · %1 (%2)").arg(Name, EncodingInfo(Tab->encodingOverride_).label_) : tr("여는 중 · %1").arg(Name), std::nullopt, std::nullopt, true};
        } else Tab->progress_ = {};
        refreshProgress(Tab);
        auto* Watcher = new QFutureWatcher<storage::Result<TextInput>>(this);
        struct PreviewState {std::mutex mutex_;QImage image_;QString text_;QJsonObject preparation_;};
        Tab->preparation_={};
        const auto Progress=std::make_shared<PreviewState>();
        auto* PreviewTimer=new QTimer(Watcher);PreviewTimer->setInterval(50);
        connect(PreviewTimer,&QTimer::timeout,this,[this,Tab,Generation,Cancellation,Progress]{
            if(Tab->closed_ || !Tab->loading_ || Cancellation->stop_requested() || Generation!=Tab->session_->Snapshot().generation_)return;
            QImage Image;QString Text;{std::lock_guard Lock(Progress->mutex_);Image=Progress->image_;Text=Progress->text_;Tab->preparation_=Progress->preparation_;}
            if(!Image.isNull())Tab->pane_->ShowLoadingPreview(Image);
            else if(!Text.isEmpty())Tab->pane_->ShowTextLoadingPreview(Text);
            if(Tab->progress_.kind_==DocumentProgress::Kind::Read) {
                const auto Name=documentName(*Tab);const auto& State=Tab->preparation_;
                const auto Done=State["bytesDone"].toInteger(),Total=State["bytesTotal"].toInteger();
                const auto Items=State["itemsDone"].toInteger(),ItemsTotal=State["itemsTotal"].toInteger();
                if(ItemsTotal>0){Tab->progress_.label_=tr("이미지 분석 중 · %1").arg(Name);Tab->progress_.fraction_=double(qMin(Items,ItemsTotal))/double(ItemsTotal);Tab->progress_.count_.reset();}
                else if(Items>0){Tab->progress_.label_=tr("목록 확인 중 · %1 · %2개").arg(Name).arg(Items);Tab->progress_.fraction_.reset();Tab->progress_.count_=Items;}
                else if(Total>0 && Done<Total){Tab->progress_.label_=tr("읽는 중 · %1").arg(Name);Tab->progress_.fraction_=double(Done)/double(Total);}
                else {Tab->progress_.label_=tr("여는 중 · %1").arg(Name);Tab->progress_.fraction_.reset();}
                refreshProgress(Tab);
            }
        });PreviewTimer->start();
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, PreviewTimer, Progress, Generation] {
            PreviewTimer->stop(); Tab->preparation_={};
            auto Result = Watcher->result();
            if(Result.HasValue()&&Result.Value().viewId_=="image")DeleteCompletedImageWork(Watcher);else Watcher->deleteLater();
            if (Tab->closed_ || Generation != Tab->session_->Snapshot().generation_) return;
            Tab->loading_ = false;Tab->pane_->ShowLoadingPreview({});
            if(!Result.HasValue())finishProgress(Tab,false,Result.Failure().category_==storage::ErrorCategory::Cancelled?QString():QString::fromStdString(Result.Failure().code_));
            if (!Result.HasValue()) {
                if(Result.Failure().category_!=storage::ErrorCategory::Cancelled){
                    QImage Image;QString Text;{std::lock_guard Lock(Progress->mutex_);Image=Progress->image_;Text=Progress->text_;Tab->preparation_=Progress->preparation_;}
                    Tab->pane_->ShowLoadingPreview(Image,true);
                }
                if (Tab->hotExitRestore_) restorationIncomplete_ = true;
                Tab->session_->CompleteOpen(Generation, Result.Failure()); Tab->error_ = QString::fromStdString(Result.Failure().code_);
            }
            else {
                Tab->input_ = std::move(Result.Value()); Tab->path_ = Tab->input_.path_;
                if (!Tab->temporarySource_.isEmpty() && !Tab->originalAuthority_ && Tab->input_.viewId_ == "text") {
                    AppSettings Settings;
                    const auto Encoding = Settings.value("textView/saveEncoding").toString();
                    Tab->input_.encoding_ = CanonicalEncodingName(Encoding).isEmpty() ? QStringLiteral("UTF-8") : CanonicalEncodingName(Encoding);
                    const int Bom = Settings.value("textView/saveBomMode").toInt();
                    Tab->input_.bom_ = EncodingInfo(Tab->input_.encoding_).bomCapable_ && (Bom == 1 || (Bom == 0 && Tab->input_.encoding_.startsWith("UTF-16")));
                }
                if (Tab->input_.recoveryRecord_) {
                    Tab->originalAuthority_ = Tab->input_.recoveryOriginalMatched_;
                    Tab->recovered_ = !Tab->originalAuthority_;
                    if (!Tab->originalAuthority_) Tab->temporarySource_ = Tab->path_;
                }
                if (Tab->input_.recoveryOrigin_) Tab->recoveredName_ = tr("복구 · ") + QFileInfo(QString::fromStdString(Tab->input_.recoveryOrigin_->localPath_)).fileName();
                const bool Hwp=Tab->input_.viewId_=="hwp"||Tab->input_.viewId_=="hwpx";
                Tab->originalSave_ = !Hwp && Tab->originalAuthority_ && !Tab->input_.book_ && !Tab->recovered_ && !(Tab->input_.source_.attributes_ & 1u);
                if(Hwp || Tab->input_.viewId_=="epub")Tab->broker_=false;
                if (Tab->input_.book_) Tab->broker_ = false;
                SourceIdentity Source{Tab->input_.source_, {}, Tab->path_.toUtf8().toStdString(), Tab->sourceToken_.toStdString(),
                    (Tab->originalSave_ || Tab->broker_) && !Tab->recovered_};
                Tab->loading_=true;
                Tab->pane_->SetReadingSource(Tab->readingPath_,Tab->readingEntry_);
                Tab->pane_->ConfigureHwp(hwpRuntime_);
                if (Tab->input_.viewId_ == "epub") Tab->pane_->ConfigureEpub(epubRuntime_ ? *epubRuntime_ : MakeProductEpubRuntime(QCoreApplication::applicationDirPath()),!hosted_);
                const bool Loaded=Tab->pane_->Load(Tab->input_,Tab->originalAuthority_,!hosted_||!Tab->temporarySource_.isEmpty());
                Tab->loading_=false;
                if(!Loaded){Tab->session_->CompleteOpen(Generation,storage::Error{storage::ErrorCategory::InvalidRequest,"document-view-load"});Tab->error_="document-view-load";finishProgress(Tab,false,Tab->error_);}
                else {
                    Tab->everReady_=true;finishProgress(Tab,true);
                    Tab->session_->SetBinaryContent(true);
                    // D10: a preserved PDF opens dirty, against its original's bytes (reattached) or, as a copy, against nothing.
                    const bool PdfRecovery = Tab->input_.recoveryRecord_ && !Tab->input_.recoveryRecord_->text_;
                    {
                        auto Serialized=Tab->pane_->SerializeSession();Tab->serializationError_=Serialized.error_;
                        const auto Baseline = !PdfRecovery ? Serialized.bytes_.toStdString()
                            : Tab->input_.recoveryOriginalMatched_ ? Tab->input_.recoveryRecord_->bytes_ : std::string{};
                        Tab->session_->CompleteOpen(Generation,OpenedDocument{std::move(Source),Baseline});
                        if (PdfRecovery) {
                            Tab->session_->Execute("document.setText", Serialized.bytes_.toStdString()); Tab->session_->MarkRecovered();
                            if (Tab->hotExitRestore_) { auto Origin = *Tab->input_.recoveryRecord_; Origin.bytes_.clear(); restoredOriginals_.push_back(std::move(Origin)); }
                            Tab->input_.recoveryRecord_->bytes_.clear();
                            Tab->recoveryNotice_ = Tab->originalAuthority_ ? tr("저장하지 않은 주석을 복구했습니다. 원본 파일은 변경하지 않았습니다.")
                                : tr("복구 내용을 별도 문서로 열었습니다. 원본 파일은 변경하지 않았습니다.");
                        }
                    }
                    if (Tab->input_.recoveryRecord_ && !PdfRecovery) {
                        const auto Record = *Tab->input_.recoveryRecord_;
                        Tab->loading_ = true;
                        const bool Restored = Tab->pane_->RestoreTextRecovery(Record);
                        Tab->loading_ = false;
                        if (Restored) {
                            auto Serialized = Tab->pane_->SerializeSession(); Tab->serializationError_ = Serialized.error_;
                            Tab->session_->Execute("document.setText", Serialized.bytes_.toStdString()); Tab->session_->MarkRecovered();
                            if (Tab->hotExitRestore_) { auto Origin = Record; Origin.bytes_.clear(); restoredOriginals_.push_back(std::move(Origin)); }
                            Tab->input_.recoveryRecord_->bytes_.clear();
                            // D8: an untitled document keeps its number (and .md, from the restored copy's extension).
                            if (Record.text_->untitled_) nameUntitled(Tab, false, int(Record.text_->untitledNumber_));
                            else if (Tab->originalAuthority_) Tab->recoveredName_.clear();
                            Tab->recoveryNotice_ = Tab->originalAuthority_ ? tr("저장하지 않은 변경을 복구했습니다. 원본 파일은 변경하지 않았습니다.")
                                : tr("복구 내용을 별도 문서로 열었습니다. 원본 파일은 변경하지 않았습니다.");
                        } else { Tab->error_ = "document-recovery-text-restore"; if (Tab->hotExitRestore_) restorationIncomplete_ = true; }
                    }
                    Tab->pane_->SetClipboardBroker(clipboardBroker_);
                    if (auto Position = std::exchange(Tab->cleanRestoreState_, std::nullopt)) Tab->pane_->RestoreTextPosition(*Position);
                    // D11: a PDF's page, zoom and in-page scroll; an EPUB reader's recorded locator.
                    if (const auto Position = std::exchange(Tab->restorePosition_, QJsonObject{}); !Position.isEmpty()) {
                        if (Tab->pane_->ViewId() == "pdf") { auto Request = Position; Request["action"] = "pdf.position"; Tab->pane_->Execute(Request); }
                        else if (Tab->pane_->IsEpub()) Tab->pane_->LocateEpub(Position["locator"].toString());
                    }
                    if(auto Restore=std::exchange(Tab->restore_,std::nullopt)) {
                        if(Restore->pdfPage_>0)Tab->pane_->Execute({{"action","pdf.page"},{"page",Restore->pdfPage_-1}});
                        if(!Restore->markdownView_.isEmpty()&&Tab->pane_->IsMarkdown())Tab->pane_->RunViewCommand("markdown."+Restore->markdownView_);
                        if(Restore->imageMode_=="webtoon"||Restore->imageMode_=="comic")Tab->pane_->Execute({{"action","reading.mode"},{"mode",Restore->imageMode_=="webtoon"?2:1}});
                        if(Restore->epubMode_=="edit"&&!hosted_)Tab->pane_->Execute({{"action","epub.edit.begin"}});
                    }
                    recordRecent(Tab);
                    if(auto* Input=Tab->pane_->InputWidget()){if(hosted_&&Tab->pane_->ViewId()=="text")Input->setAttribute(Qt::WA_NativeWindow);Input->installEventFilter(this);for(auto* Child:Input->findChildren<QWidget*>())Child->installEventFilter(this);}
                }
            }
            if(active()==Tab&&Tab->session_->Snapshot().lifecycle_==DocumentLifecycle::Ready){
                if(Tab->input_.viewId_=="image")findChild<ImageFullscreen*>("imageFullscreen")->Restore();
                setFocusProxy(Tab->pane_->InputWidget());
                const auto Native = reinterpret_cast<HWND>(winId());
                const auto Focus = GetFocus();
                // A visible embedded preview loads while the file panel keeps
                // keyboard ownership. Only refocus after the user entered it.
                const bool MayFocus = !property("hc.quickPreview").toBool()
                    || (Focus && (Focus == Native || IsChild(Native, Focus)));
                if (MayFocus && (!hosted_ || IsWindowVisible(Native))) Tab->pane_->Focus();
            }
            if (!restoredActive_.isEmpty() && IsIdle()) {
                if (auto Active = find(restoredIds_.value(restoredActive_))) activate(Active);
                restoredActive_.clear();
            }
            update(); tryFinishClose();
        });
        auto Recovery = recovery_;
        const QSize PreviewBounds=(Tab->pane_->size()*Tab->pane_->devicePixelRatioF()).expandedTo(QSize(256,256));
        Watcher->setFuture(QtConcurrent::run([Path, Cancellation, RecoveryPath, Recovery, Progress, PreviewBounds, AutoRestore=Tab->hotExitRestore_, Baseline=Tab->recoveryBaseline_, Unlimited=hosted_, Override=Tab->encodingOverride_, ReadPrefix=std::exchange(Tab->readPrefix_, {})]()->storage::Result<TextInput> {
            const auto Read=[&]()->storage::Result<TextInput> {
                storage::OperationContext Context; Context.stopToken_ = Cancellation->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                Context.reportProgress_=[Progress](const storage::OperationContext::Progress& P){
                    QJsonObject State{{"bytesDone",qint64(P.bytesDone_)},{"itemsDone",qint64(P.itemsDone_)},{"itemsTotal",qint64(P.itemsTotal_)}};
                    if(P.bytesTotal_)State["bytesTotal"]=qint64(*P.bytesTotal_);
                    std::lock_guard Lock(Progress->mutex_);Progress->preparation_=State;
                };
                if (!RecoveryPath.isEmpty()) {
                    auto Record = Recovery->Read(RecoveryPath.toStdString()); if (!Record.HasValue()) return storage::Result<TextInput>{Record.Failure()};
                    if (auto Stop = Context.Check({"document"})) return storage::Result<TextInput>{*Stop};
                    if (Record.Value().text_ && AutoRestore && !Record.Value().text_->untitled_
                        && !Record.Value().text_->baselineSha256_.empty() && Record.Value().document_.source_.sourceToken_.empty()) {
                        auto Original = ReadTextFile(QString::fromStdString(Record.Value().document_.source_.localPath_), Context);
                        if (Original.HasValue() && Original.Value().hash_.toHex().toStdString() == Record.Value().text_->baselineSha256_) {
                            Original.Value().recoveryRecord_ = Record.Value(); Original.Value().recoveryOriginalMatched_ = true; return Original;
                        }
                    }
                    // D10: a HotExit PDF snapshot reattaches to an unchanged original. The view shows the preserved
                    // annotations; the record keeps the original bytes as the clean baseline (load() marks it dirty).
                    if (!Record.Value().text_ && Record.Value().viewId_ == "pdf" && AutoRestore && !Baseline.isEmpty()
                        && Record.Value().document_.source_.sourceToken_.empty() && !Record.Value().document_.source_.localPath_.empty()) {
                        auto Original = ReadDocumentFile(QString::fromStdString(Record.Value().document_.source_.localPath_), Context, Unlimited);
                        if (Original.HasValue() && Original.Value().viewId_ == "pdf" && QString::fromLatin1(Original.Value().hash_.toHex()) == Baseline) {
                            auto Preserved = Record.Value(); const auto Annotated = QByteArray::fromStdString(Preserved.bytes_);
                            Preserved.bytes_ = Original.Value().bytes_.toStdString();
                            Original.Value().bytes_ = Annotated;
                            Original.Value().recoveryRecord_ = std::move(Preserved); Original.Value().recoveryOriginalMatched_ = true; return Original;
                        }
                    }
                    // The copy's extension selects the view: a Markdown record comes back as Markdown (txt · md · markdown).
                    auto Copy = Path;
                    if (Record.Value().text_ && QFileInfo(Path).fileName().startsWith("restored-")) {
                        const auto Suffix = QString::fromStdString(Record.Value().text_->suffix_);
                        if (QStringList{"txt", "md", "markdown"}.contains(Suffix)) Copy = QFileInfo(Path).dir().filePath(QFileInfo(Path).completeBaseName() + "." + Suffix);
                    }
                    if (!StoreCopy(Copy, QByteArray::fromStdString(Record.Value().bytes_))) return storage::Result<TextInput>{storage::Error{storage::ErrorCategory::Unavailable, "document-recovery-copy"}};
                    auto Input = ReadDocumentFile(Copy, Context, Unlimited);
                    if (Input.HasValue()) {
                        Input.Value().recoveryOrigin_ = Record.Value().document_.source_;
                        if (Record.Value().text_ || (AutoRestore && Record.Value().viewId_ == "pdf")) Input.Value().recoveryRecord_ = Record.Value();
                    }
                    return Input;
                }
                return ReadDocumentFile(Path, Context, Unlimited,[Progress](QImage Image){
                    std::lock_guard Lock(Progress->mutex_);Progress->image_=std::move(Image);
                },PreviewBounds,[Progress](QString Text){std::lock_guard Lock(Progress->mutex_);Progress->text_=std::move(Text);},ReadPrefix);
            };
            auto Input=Read();
            if(Override.isEmpty() || !Input.HasValue() || Input.Value().viewId_!="text")return Input;
            return DecodeTextInput(std::move(Input.Value()),Override);
        })); update();
    }
    QJsonObject DocumentWorkspaceWidget::prepareSave(const std::shared_ptr<Tab>& Tab)
    {
        if(Tab&&Tab->pane_->IsProcessDocument())return Error(Tab->pane_->IsEpub()?"epub-read-only":"document-edit-in-hypereditor");
        if (!Tab || Tab->loading_ || Tab->navigating_ || Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || !Tab->pendingToken_.isEmpty()) return Error("document-save-busy-or-unavailable");
        if (!Tab->broker_ || Tab->sourceToken_.isEmpty()) return Error("document-source-token-required");
        auto Started = Tab->session_->Save({SaveDisposition::Original, Tab->path_.toUtf8().toStdString()});
        if (!Started.HasValue()) return Error(QString::fromStdString(Started.Failure().code_));
        Tab->pending_ = Started.Value(); const auto Exported = Tab->pane_->Export(Tab->path_);
        Tab->pendingBytes_ = Exported.HasValue() ? Exported.Value() : QByteArray{};
        if (!Exported.HasValue() || (!hosted_ && Tab->pendingBytes_.size() > DocumentByteLimit)) {
            const auto Failure = Exported.HasValue() ? storage::Error{storage::ErrorCategory::BudgetExceeded, "document-save-byte-budget"} : Exported.Failure();
            Tab->session_->CompleteSave(*Tab->pending_, {false, {}, Failure});
            Tab->pending_.reset(); Tab->pendingBytes_.clear(); return Error(QString::fromStdString(Failure.code_));
        }
        Tab->pendingToken_ = QUuid::createUuid().toString(QUuid::Id128);
        Tab->pendingHash_ = QCryptographicHash::hash(Tab->pendingBytes_, QCryptographicHash::Sha256).toHex();
        auto Result = QJsonObject{{"saveToken", Tab->pendingToken_}, {"sourceToken", Tab->sourceToken_},
            {"documentId", Tab->id_}, {"generation", QString::number(Tab->pending_->generation_)},
            {"revision", QString::number(Tab->pending_->revision_)}, {"byteLength", QString::number(Tab->pendingBytes_.size())},
            {"sha256", QString::fromLatin1(Tab->pendingHash_)}};
        if (Tab->pendingBytes_.size() <= HostChunkLimit) Result["bytes"] = QString::fromLatin1(Tab->pendingBytes_.toBase64());
        return Result;
    }
    void DocumentWorkspaceWidget::save(const std::shared_ptr<Tab>& Tab, const QString& Destination, bool Overwrite, bool SaveEpubOriginal)
    {
        if(Tab&&Tab->pane_->IsProcessDocument()&&!Tab->pane_->CanEditEpub()){Tab->error_=Tab->pane_->IsEpub()?"epub-read-only":"document-edit-in-hypereditor";update();return;}
        if (!Tab || Tab->loading_ || Tab->navigating_ || Tab->choosingSavePath_ || !Tab->pendingToken_.isEmpty() || Destination.isEmpty()) return;
        if(Tab->pane_->InputWidget() && Tab->pane_->InputWidget()->property("epubSourceComposing").toBool()) {
            if(!Tab->composingSave_) {
                Tab->composingSave_=true;const auto Generation=Tab->session_->Snapshot().generation_;
                QTimer::singleShot(30,this,[this,Tab,Destination,Overwrite,SaveEpubOriginal,Generation]{
                    if(!Tab->composingSave_ || Tab->closed_ || Tab->session_->Snapshot().generation_!=Generation)return;
                    Tab->composingSave_=false;save(Tab,Destination,Overwrite,SaveEpubOriginal);
                });
            }
            return;
        }
        // absoluteFilePath is lexical. Canonical/identity probes may block on SMB
        // or redirected drives and are performed by the captured encoder/writer.
        const bool Original=QFileInfo(Destination).absoluteFilePath().compare(QFileInfo(Tab->path_).absoluteFilePath(),Qt::CaseInsensitive)==0;
        if (Original && ((Tab->pane_->IsEpub() && (!SaveEpubOriginal || hosted_)) || !Tab->originalSave_ || Tab->broker_ || Tab->recovered_)) { Tab->error_ = "document-original-save-unavailable"; update(); return; }
        const auto Target=QFileInfo(Destination).absoluteFilePath().toCaseFolded();
        for(const auto& Other:documents_)if(Other!=Tab && Other->activeSave_ && QFileInfo(Other->activeSave_->destination_).absoluteFilePath().toCaseFolded()==Target){
            Tab->error_="document-save-target-busy";update();return;
        }
        auto Request=std::make_shared<LocalSave>();Request->destination_=Destination;Request->original_=Original;Request->overwrite_=Overwrite;
        Request->content_=Tab->session_->Text();Request->revision_=Tab->Revision();
        const auto Capture=[&]{
            std::optional<QByteArray> Content;std::optional<storage::Error> ContentError;
            if(Tab->pane_->ViewId()=="text"){Content=QByteArray::fromStdString(Request->content_);ContentError=Tab->serializationError_;}
            return Tab->pane_->CaptureSave(Destination,Original && SaveEpubOriginal,std::move(Content),std::move(ContentError));
        };
        if(Tab->saving_) {
            if(!Tab->activeSave_ || Tab->activeSave_->destination_!=Destination || Tab->activeSave_->overwrite_!=Overwrite){Tab->error_="document-save-target-busy";update();return;}
            // A repeated request supersedes the single queued snapshot, including
            // an undo back to the in-flight content. No unrequested typing is saved.
            if(Request->content_==Tab->activeSave_->content_)Tab->queuedSave_.reset();
            else {Request->encoder_=Capture();Tab->queuedSave_=std::move(Request);}
            update();return;
        }
        Request->encoder_=Capture();
        startSave(Tab,std::move(Request));
    }
    void DocumentWorkspaceWidget::startSave(const std::shared_ptr<Tab>& Tab,std::shared_ptr<LocalSave> Request)
    {
        const auto Destination=Request->destination_;
        const bool ExportOnly=Tab->pane_->ViewState().contains("reading");
        std::optional<SaveTicket> Ticket;
        if(!ExportOnly) {
            auto Started=Tab->session_->SaveSnapshot({Request->original_?SaveDisposition::Original:SaveDisposition::SaveAs,Destination.toUtf8().toStdString()},Request->revision_,Request->content_);
            if(!Started.HasValue()){Tab->error_=QString::fromStdString(Started.Failure().code_);update();return;}
            Ticket=Started.Value();
        }
        const auto Expected=Request->original_?storage::DestinationExpectation{true,Tab->input_.source_.id_.revision_}:storage::DestinationExpectation{};
        const auto Hash=Request->original_?Tab->input_.hash_:QByteArray{};
        externalWatcher_->beginSelfWrite(Destination);
        Tab->activeSave_=Request;Tab->saving_=true;Tab->error_.clear();Tab->cancellation_=std::make_shared<std::stop_source>();
        {
            const auto Name=QFileInfo(Destination).fileName();
            const bool Converted=Tab->pane_->ViewId()=="text" && CanonicalEncodingName(Tab->pane_->SaveEncoding())!=CanonicalEncodingName(Tab->input_.encoding_);
            Tab->progress_={Tab->pane_->IsEpub()?DocumentProgress::Kind::EpubSave:DocumentProgress::Kind::Save,Tab->id_+":save:"+QUuid::createUuid().toString(QUuid::Id128),
                Converted?tr("저장 중 · %1 (%2로 변환)").arg(Name,EncodingInfo(Tab->pane_->SaveEncoding()).label_):tr("저장 중 · %1").arg(Name),std::nullopt,std::nullopt,true};
            refreshProgress(Tab);
        }
        struct SaveProgress {std::atomic<qint64> done_{0},total_{0};};
        const auto Written=std::make_shared<SaveProgress>();
        struct Output {storage::Result<LocalSaveResult> result_;QByteArray bytes_;mrst::ExternalChangeWatcher::Fingerprint fingerprint_;};
        auto* Watcher=new QFutureWatcher<Output>(this);
        auto* SaveTimer=new QTimer(Watcher);SaveTimer->setInterval(50);
        connect(SaveTimer,&QTimer::timeout,this,[this,Tab,Written]{
            const auto Total=Written->total_.load();if(Total<=0 || Tab->progress_.kind_!=DocumentProgress::Kind::Save)return;
            Tab->progress_.fraction_=double(qMin(Written->done_.load(),Total))/double(Total);refreshProgress(Tab);
        });SaveTimer->start();
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Tab,Watcher,Request,Ticket,Destination,ExportOnly,SaveTimer]{
            SaveTimer->stop();
            auto Output=Watcher->result();auto& Result=Output.result_;Watcher->deleteLater();Tab->saving_=false;Tab->activeSave_.reset();
            {
                const bool Saved=Result.HasValue() && Result.Value().receipt_.destinationCommitted_;
                const bool Cancelled=!Result.HasValue() && Result.Failure().category_==storage::ErrorCategory::Cancelled;
                finishProgress(Tab,Saved,Saved||Cancelled?QString():Result.HasValue()?QStringLiteral("document-not-committed"):QString::fromStdString(Result.Failure().code_));
            }
            const bool Committed=Result.HasValue() && Result.Value().receipt_.destinationCommitted_;
            if(Committed && !ExportOnly)externalWatcher_->endSelfWrite(Destination,Output.fingerprint_);
            else externalWatcher_->endSelfWrite(Destination,false);
            if(Tab->closed_)return;
            if(Ticket) {
                DocumentSaveResult Completion;
                if(!Result.HasValue())Completion.error_=Result.Failure();
                else {
                    Completion.committed_=Committed;Completion.cleanupErrors_=Result.Value().receipt_.cleanupErrors_;
                    Completion.source_=SourceIdentity{Result.Value().source_,{},Destination.toUtf8().toStdString(),{},Result.Value().source_.id_.revision_.has_value() && !(Result.Value().source_.attributes_ & 1u)};
                }
                if(!Tab->session_->CompleteSave(*Ticket,std::move(Completion)))return;
            }
            if(!Committed) {
                Tab->error_=Result.HasValue()?"document-not-committed":QString::fromStdString(Result.Failure().code_);
                Tab->pane_->ReportSaveFailure(QByteArray::fromStdString(Request->content_),Tab->error_);
                // A failed/conflicting write never starts a queued write blindly.
                Tab->queuedSave_.reset();Tab->closeAfterSave_=false;closingWorkspace_=false;
                if (hotExitClosing_) cancelHotExitClose(Tab->error_);
            } else if(!ExportOnly) {
                Tab->error_.clear();
                Tab->pane_->CommitSaved(Destination,Output.bytes_,QByteArray::fromStdString(Request->content_),Result.Value().hash_,!Tab->Dirty());
                // Save As into another folder: untitled documents start there next time.
                if(!Tab->temporarySource_.isEmpty() || QFileInfo(Tab->path_).absolutePath()!=QFileInfo(Destination).absolutePath()){
                    AppSettings Settings;Settings.setValue("common/lastSaveFolder",QFileInfo(Destination).absolutePath());Settings.sync();
                }
                // The document now has a file of its own: drop the untitled name and the temporary source.
                if(!Tab->temporarySource_.isEmpty()){
                    if(QFileInfo(Tab->temporarySource_).absoluteFilePath().compare(QFileInfo(Destination).absoluteFilePath(),Qt::CaseInsensitive)!=0)QFile::remove(Tab->temporarySource_);
                    Tab->temporarySource_.clear();
                }
                Tab->untitledNumber_=0;Tab->suggestedSavePath_.clear();
                Tab->path_=Destination;Tab->displayName_.clear();Tab->input_.source_=Result.Value().source_;Tab->input_.hash_=Result.Value().hash_;
                Tab->originalAuthority_=true;Tab->originalSave_=Result.Value().source_.id_.revision_.has_value() && !(Result.Value().source_.attributes_ & 1u);
                Tab->recoveryNotice_.clear();
                Tab->recovered_=false;Tab->recoveredName_.clear();Tab->broker_=false;Tab->sourceToken_.clear();
                Tab->checkpointedRevision_=std::numeric_limits<std::uint64_t>::max();
                cleanupRecovery(Tab,Ticket->revision_);
                recordRecent(Tab);
            }
            if(Committed && !Result.Value().receipt_.cleanupErrors_.empty())Tab->error_="document-saved-cleanup-warning";
            if(Committed && Tab->queuedSave_) {
                auto Next=std::exchange(Tab->queuedSave_,{});
                // Save As has adopted its destination; the next write must use the
                // just-committed identity/hash, never overwrite a newly changed file.
                Next->original_=true;Next->overwrite_=false;
                if(Tab->originalSave_ && Tab->error_.isEmpty()){startSave(Tab,std::move(Next));return;}
                Tab->closeAfterSave_=false;closingWorkspace_=false;
            }
            if(Committed && Tab->closeAfterSave_ && !Tab->Dirty())retire(Tab,true);
            else Tab->closeAfterSave_=false;
            update();tryFinishClose();
        });
        const bool Image=Tab->pane_->ViewId()=="image";
        const auto Cancellation=Tab->cancellation_;
        Watcher->setFuture(QtConcurrent::run([Request,Expected,Hash,Cancellation,Image,Unlimited=hosted_,Written]() mutable ->Output {
            try {
                storage::OperationContext Context;Context.stopToken_=Cancellation->get_token();Context.deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(150);
                Context.reportProgress_=[Written](const storage::OperationContext::Progress& P){if(P.bytesTotal_){Written->total_=qint64(*P.bytesTotal_);Written->done_=qint64(P.bytesDone_);}};
                auto Encoded=Request->encoder_(Context);if(!Encoded.HasValue())return {Encoded.Failure(),{}};
                auto Bytes=std::move(Encoded.Value());
                if(auto Stop=Context.Check({"document"}))return {*Stop,{}};
                auto Expectation=Expected;auto ContentHash=Hash;
                if(Request->overwrite_ && QFileInfo::exists(Request->destination_)) {
                    auto Existing=ReadDocumentFile(Request->destination_,Context,Unlimited);if(!Existing.HasValue())return {Existing.Failure(),{}};
                    Expectation={true,Existing.Value().source_.id_.revision_};ContentHash=Existing.Value().hash_;
                }
                auto Result=SaveTextFile(Request->destination_,Bytes,Expectation,ContentHash,Context,Image,Unlimited);
                const auto Fingerprint=Result.HasValue()?mrst::ExternalChangeWatcher::fingerprintOf(Request->destination_):mrst::ExternalChangeWatcher::Fingerprint{};
                return {std::move(Result),std::move(Bytes),Fingerprint};
            } catch(const std::exception&) {return {storage::Error{storage::ErrorCategory::Unavailable,"document-save-worker-failed"},{}};}
        }));update();
    }
    void DocumentWorkspaceWidget::cleanupRecovery(const std::shared_ptr<Tab>& Tab, std::uint64_t ThroughRevision)
    {
        if (Tab->checkpointing_) return; // Completion of that checkpoint performs the cleanup.
        auto Recovery = recovery_; auto* Watcher = new QFutureWatcher<storage::Result<storage::Unit>>(this); ++recoveryWorkers_;
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Tab] {
            if (!Watcher->result().HasValue()) Tab->error_ = "document-recovery-cleanup";
            Watcher->deleteLater(); --recoveryWorkers_; update(); tryFinishClose();
        });
        const auto Id = Tab->id_.toStdString(); const auto Generation = Tab->session_->Snapshot().generation_;
        const auto Original = Tab->hotExitRestore_ ? std::optional<RecoverySnapshot>{} : Tab->input_.recoveryRecord_;
        Watcher->setFuture(QtConcurrent::run([Recovery, Id, Generation, ThroughRevision, Original]()->storage::Result<storage::Unit> {
            auto Removed = Recovery->RemoveThrough(Id, Generation, ThroughRevision); if (!Removed.HasValue()) return Removed.Failure();
            if (Original) return Recovery->RemoveThrough(Original->document_.id_, Original->document_.generation_, Original->document_.revision_);
            return storage::Unit{};
        }));
    }
    void DocumentWorkspaceWidget::Checkpoint()
    {
        if (hotExitClosing_ || hotExitPublishing_) return;
        if (hotExitEnabled() && IsIdle()) publishHotExitSession(false);
        if (!pendingHostOpen_.isEmpty()) for (const auto& Tab : documents_) if (Tab->hostSlot_) {
            if (Tab->loading_ || Tab->navigating_ || Tab->saving_ || !Tab->pendingToken_.isEmpty() || Tab->checkpointing_ || Tab->pane_->IsBusy()) break;
            if (!Tab->Dirty() || Tab->checkpointedRevision_ == Tab->Revision()) {
                const auto Next = std::exchange(pendingHostOpen_, {});
                retire(Tab, true, true); open(Next["path"].toString(), true, Next["sourceToken"].toString(), Next["broker"].toBool(), {}, Next["displayName"].toString()); return;
            }
        }
        // A host-slot replacement must preserve dirty content even when periodic recovery is disabled.
        if (pendingHostOpen_.isEmpty() && !appliedPreferences_["documentViews"].toObject()["textView"].toObject()["recoveryEnabled"].toBool(hc::defaults::edit::ViewValue("textView/recoveryEnabled").toBool())) return;
        for (const auto& Tab : documents_) {
            // EPUB source drafts are tokens, not a recovery archive. Never persist them as EPUB bytes.
            if(Tab->pane_->IsEpub()){Tab->pane_->CheckpointEpub();continue;}
            if (Tab->loading_ || Tab->navigating_ || Tab->closed_ || Tab->checkpointing_ || !Tab->Dirty() || Tab->checkpointedRevision_ == Tab->Revision()) continue;
            auto Bytes = Tab->pane_->ViewId() == "image" ? Tab->pane_->Content().toStdString() : Tab->session_->Text();
            if (!hosted_ && Bytes.size() > DocumentByteLimit) { Tab->error_ = "document-recovery-budget"; continue; }
            RecoverySnapshot Record; Record.document_ = Tab->session_->Snapshot(); Record.bytes_ = std::move(Bytes);Record.viewId_=Tab->pane_->ViewId().toStdString();
            if (hotExitEligible(Tab)) {
                QByteArray Utf8; Record.text_ = Tab->pane_->CaptureTextRecovery(Utf8);
                if (Record.text_) {
                    Record.viewId_ = "text"; Record.bytes_ = Utf8.toStdString(); Record.ownerWindow_ = workspaceId_.toStdString();
                    Record.text_->untitled_ = !Tab->temporarySource_.isEmpty(); Record.text_->untitledNumber_ = Tab->untitledNumber_;
                    Record.text_->baselineSha256_ = Record.text_->untitled_ ? std::string{} : Tab->input_.hash_.toHex().toStdString();
                    if (Record.text_->untitled_) Record.document_.source_.localPath_.clear();
                }
            }
            const auto Revision = Tab->Revision(); Tab->checkpointing_ = true;
            auto* Watcher = new QFutureWatcher<storage::Result<RecoverySnapshot>>(this);
            connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Revision, SavedRevision = Record.document_.savedRevision_, SourcePath = Tab->session_->Snapshot().source_.localPath_] {
                const bool Stored = Watcher->result().HasValue();
                if (Stored) Tab->checkpointReceipt_ = Watcher->result().Value();
                Watcher->deleteLater(); Tab->checkpointing_ = false;
                const bool CurrentBaseline=SavedRevision==Tab->session_->Snapshot().savedRevision_ && SourcePath==Tab->session_->Snapshot().source_.localPath_;
                // A completed save retires older checkpoints. A store rejected by
                // that retirement is not a failure of the current dirty revision.
                if (!Stored && CurrentBaseline) Tab->error_ = "document-recovery-write-failed";
                else Tab->checkpointedRevision_ = Stored && CurrentBaseline
                    ? Revision : std::numeric_limits<std::uint64_t>::max();
                if (Tab->hostSlot_ && !pendingHostOpen_.isEmpty()) {
                    if (Stored && !Tab->closed_) Checkpoint();
                    else { pendingHostOpen_ = {}; if (Tab->pane_) Tab->pane_->SetReadOnly(false); }
                }
                else if (!Tab->Dirty()) cleanupRecovery(Tab, Tab->session_->Snapshot().savedRevision_);
                update(); tryFinishClose();
            });
            auto Recovery = recovery_; Watcher->setFuture(QtConcurrent::run([Recovery, Record = std::move(Record)]() mutable { return Recovery->Store(std::move(Record)); }));
        }
    }
    QJsonObject DocumentWorkspaceWidget::Snapshot() const
    {
        PROCESS_MEMORY_COUNTERS_EX Memory{}; Memory.cb = sizeof(Memory);
        K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Memory), sizeof(Memory));
        QJsonArray Tabs, Pending;
        for (const auto& Tab : documents_) {
            auto State = Tab->session_->Snapshot(); auto Capability = Tab->session_->Capabilities();
            if(Tab->pane_->IsProcessDocument()){
                const auto View=Tab->pane_->ViewState();Capability.save_=Tab->originalSave_ && Tab->pane_->CanEditEpub();Capability.saveAs_=Tab->pane_->CanEditEpub();
                if(!View["error"].toString().isEmpty())State.error_=storage::Error{storage::ErrorCategory::Unavailable,View["error"].toString().toStdString()};
                if(State.error_)State.lifecycle_=DocumentLifecycle::Failed;
                else if(!View["ready"].toBool())State.lifecycle_=DocumentLifecycle::Opening;
            }
            const auto Code = !Tab->error_.isEmpty() ? Tab->error_ : State.error_ ? QString::fromStdString(State.error_->code_) : !State.cleanupErrors_.empty() ? QString("document-saved-cleanup-warning") : QString{};
            QJsonArray Cleanup; for (const auto& Issue : State.cleanupErrors_) Cleanup.append(QString::fromStdString(Issue.code_));
            Tabs.append(QJsonObject{{"documentId", Tab->id_}, {"path", Tab->path_},
                {"displayName", documentName(*Tab)}, {"inputSha256", QString::fromLatin1(Tab->input_.hash_.toHex())},
                {"sourceReady", Tab->pane_->IsEpub() && !Tab->loading_ && !Tab->closeRequested_ && !closingWorkspace_ && !Tab->input_.hash_.isEmpty() && Tab->error_.isEmpty()},
                {"textPresentationReady", Tab->loading_ && !Tab->closeRequested_ && Tab->pane_->HasTextLoadingPreview()},
                {"imagePresentationReady", !Tab->closeRequested_ && !closingWorkspace_ && Code.isEmpty()
                    && (Tab->loading_ || Tab->navigating_ ? Tab->pane_->HasLoadingPreview() : Tab->pane_->ImagePresentationReady())},
                {"archivePending", Tab->archivePending_}, {"archiveUpdating", Tab->archiveUpdating_}, {"archiveDiscard", Tab->archiveDiscard_}, {"dirty", State.dirty_}, {"loading", Tab->loading_ || Tab->navigating_ || (Tab->pane_->IsProcessDocument()&&State.lifecycle_==DocumentLifecycle::Opening)},
                {"saving", Tab->saving_ || !Tab->pendingToken_.isEmpty()}, {"saveQueued", bool(Tab->queuedSave_)}, {"revision", QString::number(State.revision_)},
                {"savedRevision", QString::number(State.savedRevision_)}, {"generation", QString::number(State.generation_)},
                {"lifecycle", Life(State.lifecycle_)}, {"saveState", SaveState(State.saveState_)}, {"error", Code},
                {"errorCategory", State.error_ ? int(State.error_->category_) : -1}, {"cleanupWarnings", Cleanup},
                {"preparation",Tab->preparation_}, {"sourceToken", Tab->sourceToken_}, {"canSave", Capability.save_}, {"canSaveAs", Capability.saveAs_}, {"hostSlot", Tab->hostSlot_}, {"view",Tab->pane_->ViewState()}});
            if (!Tab->pendingToken_.isEmpty()) Pending.append(QJsonObject{{"documentId", Tab->id_}, {"sourceToken", Tab->sourceToken_},
                {"saveToken", Tab->pendingToken_}, {"generation", QString::number(Tab->pending_->generation_)},
                {"revision", QString::number(Tab->pending_->revision_)}, {"byteLength", QString::number(Tab->pendingBytes_.size())},
                {"sha256", QString::fromLatin1(Tab->pendingHash_)}});
        }
        return {{"protocolVersion", int(HostVersion)}, {"workspaceId", workspaceId_}, {"documents", Tabs}, {"pendingSaves", Pending},
            {"clipboard", clipboardRequest_}, {"editRequest",editRequest_}, {"recoveryList", recoveryList_}, {"activeDocumentId", active() ? active()->id_ : QString{}}, {"idle", IsIdle()},
            {"memoryPrivateBytes", QString::number(Memory.PrivateUsage)}, {"memoryPeakWorkingSetBytes", QString::number(Memory.PeakWorkingSetSize)}, {"memoryPeakPrivateBytes", QString::number(Memory.PeakPagefileUsage)},
            {"hotExitClosing", hotExitClosing_}, {"hotExitEnabled", hotExitEnabled()}, {"shellError", shellError_}, {"recoveryRoot", recoveryRoot_}, {"dpr", devicePixelRatioF()}, {"tabbed", tabbed_}, {"chrome", chrome_->Settings()},
            {"closeViewerOnEscape", closeViewerOnEscape_}, {"closeRequested", viewerCloseRequested_}};
    }
    QJsonObject DocumentWorkspaceWidget::HandleMessage(const QJsonObject& Envelope)
    {
        const auto Type = Envelope["messageType"].toString();
        const auto Payload = Envelope["payload"].toObject();
        bool Valid = false; const auto Id = Envelope["requestId"].toString().toULongLong(&Valid);
        auto Reply = [&](const QJsonObject& Result, const QString& ReplyType = {}) {
            const auto Body = Result.contains("error") ? QJsonObject{{"code", Result["error"]}, {"category", Result["errorCategory"]}} : Result;
            return QJsonObject{{"protocolVersion", int(HostVersion)}, {"requestId", Envelope["requestId"]}, {"workspaceId", workspaceId_},
                {"documentId", Envelope["documentId"]}, {"generation", Envelope["generation"]},
                {"messageType", Result.contains("error") ? QString("Error") : ReplyType.isEmpty() ? Type + "Result" : ReplyType},
                {"payloadLength", QJsonDocument(Body).toJson(QJsonDocument::Compact).size()}, {"payload", Body}};
        };
        if (Envelope["protocolVersion"].toInt(-1) != HostVersion) return Reply(Error("document-protocol-version"));
        if (!Valid || Id <= lastMessage_ || !Envelope["payload"].isObject()
            || Envelope["payloadLength"].toInteger(-1) != QJsonDocument(Payload).toJson(QJsonDocument::Compact).size())
            return Reply(Error("document-envelope-fields"));
        if (Type != "Hello" && (!negotiated_ || Envelope["workspaceId"].toString() != workspaceId_)) return Reply(Error("document-workspace-token"));
        lastMessage_ = Id;
        if (Type == "Hello") {
            if (negotiated_) return Reply(Error("document-hello-repeated"));
            // Negotiating the existing broker channel keeps clipboard commands
            // on the parent side even when RHWP needs an unrestricted native UI.
            SetClipboardBroker(true);
            negotiated_ = true; return Reply({{"capabilities", QJsonArray{"mixedTabs", "saveStream", "clipboardBroker", "recovery"}},
                {"maxDocumentBytes", hosted_?0:int(DocumentByteLimit)}, {"maxChunkBytes", int(HostChunkLimit)}, {"maxTabs", hosted_?0:16}}, "Ready");
        }
        if (Type == "Heartbeat") return Reply({{"alive", true}, {"idle", IsIdle()}});
        if (Type == "OpenDocuments") {
            const auto Inputs = Payload["documents"].toArray();
            if (Inputs.isEmpty() || (!hosted_ && Inputs.size() > 16)) return Reply(Error("document-open-count"));
            for (const auto& Input : Inputs) {
                const auto Name = Input.toObject()["displayName"];
                if (!Name.isUndefined() && (!Name.isString() || Name.toString().size() > 1024
                    || Name.toString().contains(QChar::Null) || Name.toString().contains('/')
                    || Name.toString().contains('\\') || Name.toString().contains('\r') || Name.toString().contains('\n')))
                    return Reply(Error("document-display-name"));
            }
            // Commander recycles a closed viewer as its next warm viewer.
            viewerCloseRequested_ = false;
            QJsonArray Results; for (const auto& Input : Inputs) {
                const auto Item = Input.toObject();
                bool Bound = false;
                if(Item["bindExistingHost"].toBool() && Item["hostSlot"].toBool() && Item["sourceToken"].toString().size()==32) {
                    for(const auto& Tab:documents_) if(Tab->hostSlot_ && Tab->sourceToken_.isEmpty() && !Tab->loading_ && !Tab->saving_
                        && !Tab->closeRequested_ && !closingWorkspace_ && !Tab->Dirty()
                        && (!Tab->pane_->IsBusy() || Tab->pane_->IsEpub() || (Tab->pane_->ImagePresentationReady() && !Tab->pane_->IsEditing()))
                        && Tab->session_->Snapshot().lifecycle_==DocumentLifecycle::Ready
                        && QFileInfo(Tab->path_).absoluteFilePath().compare(QFileInfo(Item["path"].toString()).absoluteFilePath(),Qt::CaseInsensitive)==0
                        && ((Item["readOnlyBook"].toBool() && Tab->input_.book_ && !Item["broker"].toBool())
                            || QString::fromLatin1(Tab->input_.hash_.toHex())==Item["sha256"].toString())) {
                        // The SDK already decoded this exact snapshot. Transfer its
                        // original-provider binding without decoding/creating a second view.
                        const auto Begun=Tab->session_->BeginOpen(true);
                        if(!Begun.HasValue())break;
                        Tab->sourceToken_=Item["sourceToken"].toString();Tab->broker_=Item["broker"].toBool()&&!Tab->input_.book_&&!Tab->pane_->IsProcessDocument();
                        Tab->displayName_=Item["displayName"].toString();
                        SourceIdentity Source{Tab->input_.source_,{},Tab->path_.toUtf8().toStdString(),Tab->sourceToken_.toStdString(),Tab->broker_};
                        {auto Serialized=Tab->pane_->SerializeSession();Tab->serializationError_=Serialized.error_;Tab->session_->CompleteOpen(Begun.Value(),OpenedDocument{std::move(Source),Serialized.bytes_.toStdString()});}
                        Results.append(QJsonObject{{"documentId",Tab->id_},{"boundExistingHost",true}});
                        update();Bound=true;break;
                    }
                }
                if(!Bound)Results.append(open(Item["path"].toString(),Item["hostSlot"].toBool(),Item["sourceToken"].toString(),Item["broker"].toBool(),{},Item["displayName"].toString(),Item["readPrefix"].toObject()));
                const auto Origin=Item["readingSource"].toObject();
                if(!Origin.isEmpty()&&QFileInfo(Origin["path"].toString()).isAbsolute())for(const auto& Tab:documents_)if(Tab->sourceToken_==Item["sourceToken"].toString()&&!Tab->sourceToken_.isEmpty()){
                    Tab->readingPath_=Origin["path"].toString();Tab->readingEntry_=Origin["entry"].toString().replace('\\','/');Tab->pane_->SetReadingSource(Tab->readingPath_,Tab->readingEntry_);
                }
            }
            return Reply({{"documents", Results}});
        }
        static const QMap<QString, QString> Actions{{"DocumentState", "state"}, {"ActivateDocument", "activate"}, {"SaveRequest", "prepareSave"},
            {"SaveRead", "saveRead"}, {"SaveResult", "commitSave"}, {"ClipboardResult", "clipboardResult"},
            {"PrepareClose", "prepareClose"}, {"CloseDecision", "close"}, {"Cancel", "cancel"}, {"Execute", ""}};
        if (!Actions.contains(Type)) return Reply(Error("document-message-type"));
        auto Request = Payload;
        if (Type != "Execute") Request["action"] = Actions[Type];
        if (!Envelope["documentId"].toString().isEmpty()) {
            Request["documentId"] = Envelope["documentId"]; Request["generation"] = Envelope["generation"];
        }
        return Reply(Handle(Request), Type == "DocumentState" ? "DocumentStateChanged" : QString{});
    }
    QJsonObject DocumentWorkspaceWidget::Handle(const QJsonObject& Request)
    {
        const auto Action = Request["action"].toString();
        if (Request.contains("protocolVersion") && Request["protocolVersion"].toInt() != HostVersion) return Error("document-protocol-version");
        if (Action == "setDocumentAppearance") return epubui::ApplyAppearance(Request["appearance"].toObject())
            ? QJsonObject{{"accepted", true}} : Error("document-appearance-invalid");
        if (Action == "state") return Snapshot();
        if (Action == "clearPreview") {
            if (!hosted_ || !property("hc.quickPreview").toBool() || !IsIdle()) return Error("preview-cache-busy");
            for (const auto& Tab : documents_) if (Tab->Dirty() || Tab->archivePending_ || Tab->archiveUpdating_) return Error("preview-cache-dirty");
            const auto Tabs = documents_;
            for (const auto& Tab : Tabs) retire(Tab, false);
            update();
            return documents_.empty() ? QJsonObject{{"accepted", true}} : Error("preview-cache-busy");
        }
        if (Action == "requestViewerClose") {
            if (!hosted_) return Error("document-not-hosted");
            QTimer::singleShot(0, this, [this] { RequestViewerClose(); });
            return {{"accepted", true}};
        }
        if (Action == "present" || Action == "conceal") return {{"accepted", hosted_}};
        if (Action == "escape") { escapeViewer(); return {{"accepted", hosted_ && closeViewerOnEscape_}}; }
        if (Action == "preferences") { findChild<QAction*>("documentPreferences")->trigger(); return {{"accepted", true}}; }
        // Automation keeps "new" = a text document; "kind" picks markdown · image · epub without the popup.
        if (Action == "new") return newDocument(NewDocumentKindFrom(Request["kind"].toString()));
        if (Action == "epub.recovery.open") return openEpubRecovery(Request["path"].toString());
        if (Action == "focus") {
            if (hosted_ && !IsWindowVisible(reinterpret_cast<HWND>(winId()))) return {{"accepted", false}};
            // A background/prepared helper must not activate either its Qt
            // shell or HC over another foreground application or command window.
            const auto NativeWindow = reinterpret_cast<HWND>(winId());
            const auto Foreground = GetForegroundWindow();
            // Showing a reparented Qt shell can briefly make that child the
            // foreground window. Permit repairing our own shell's activation,
            // but never steal focus from another application or a settings dialog.
            if (hosted_ && Foreground != GetAncestor(NativeWindow, GA_ROOT) && Foreground != NativeWindow) return {{"accepted", false}};
            if(auto Tab=active();Tab&&!Tab->loading_){
                if(!QApplication::activePopupWidget()&&!QApplication::activeModalWidget()){
                    QApplication::setActiveWindow(this);setFocusProxy(Tab->pane_->InputWidget());
                    // Set the native focus on the helper GUI owner as well. The
                    // SDK container runs on a different thread/process.
                    const auto Native=reinterpret_cast<HWND>(winId());
                    const auto Target=Tab->pane_->InputWidget();
                    // Qt tracks this workspace as its active shell, while Win32
                    // must keep the containing HC window active for keyboard input.
                    if(hosted_)SetActiveWindow(GetAncestor(Native,GA_ROOT));
                    SetFocus(Target?reinterpret_cast<HWND>(Target->winId()):Native);
                    Tab->pane_->Focus();
                }
            }
            return {{"accepted",true}};
        }
        if (Action == "open" || Action == "openHost") {
            const auto Reply=open(Request["path"].toString(), Action == "openHost", Request["sourceToken"].toString(), Request["broker"].toBool(), {}, {}, Request["readPrefix"].toObject());
            if(!hosted_ && Request["copyOnly"].toBool())if(auto Tab=find(Reply["documentId"].toString())){Tab->originalAuthority_=false;Tab->originalSave_=false;}
            return Reply;
        }
        if (Action == "chrome") { if (!ApplyChromeSettings(Request["settings"].toObject())) return Error("document-chrome-settings"); emit ChromeSettingsChanged(chrome_->Settings()); return {{"accepted", true}}; }
        if (Action == "cancelHost") { for (const auto& Tab : documents_) if (Tab->hostSlot_) { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->queuedSave_.reset();Tab->composingSave_=false;Tab->pane_->Cancel(); } return {{"accepted", true}}; }
        if (hotExitClosing_ && Action == "cancel") { cancelHotExitClose(); return {{"accepted", true}}; }
        if (hotExitClosing_ && Action != "snapshot" && Action != "prepareClose") return Error("document-hotexit-closing");
        if (Action == "checkpoint") { Checkpoint(); return {{"accepted", true}}; }
        if(Action=="editResult"){
            if(editRequest_.isEmpty()||Request["token"]!=editRequest_["token"])return Error("document-edit-token");
            if(auto Target=find(editRequest_["documentId"].toString());Target&&!Request["accepted"].toBool())Target->error_="document-editor-start-failed";
            else if((editRequest_["format"]=="epub-new" || editRequest_["format"]=="hwpx-new")&&!Request["accepted"].toBool())ShowShellError(tr("HyperEditor를 시작하지 못했습니다."));
            editRequest_={};update();return {{"accepted",true}};
        }
        if (Action == "prepareClose") { for (const auto& Tab : documents_) if (Tab->Dirty()) return {{"canClose", false}}; return {{"canClose", IsIdle()}}; }
        if (Action == "recoveryList") {
            if (recoveryList_["loading"].toBool()) return recoveryList_;
            recoveryList_ = {{"loading", true}};
            auto* Watcher = new QFutureWatcher<storage::Result<std::vector<RecoverySnapshot>>>(this); ++recoveryWorkers_;
            connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher] {
                auto Result = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_;
                recoveryList_ = {{"loading", false}}; QJsonArray Entries;
                if (!Result.HasValue()) recoveryList_["error"] = QString::fromStdString(Result.Failure().code_);
                else for (const auto& Record : Result.Value()) Entries.append(QJsonObject{{"documentId", QString::fromStdString(Record.document_.id_)},
                    {"sourcePath", QString::fromStdString(Record.document_.source_.localPath_)}, {"revision", QString::number(Record.document_.revision_)},
                    {"path", QString::fromStdString(Record.snapshotPath_)}, {"utcMs", QString::number(Record.utcMs_)}});
                recoveryList_["entries"] = Entries; update(); tryFinishClose();
            });
            auto Recovery = recovery_; Watcher->setFuture(QtConcurrent::run([Recovery] { return Recovery->List(); })); return recoveryList_;
        }
        if (Action == "recover") {
            const auto RecoveryPath = QFileInfo(Request["path"].toString()).canonicalFilePath();
            const auto Root = QFileInfo(recoveryRoot_).canonicalFilePath();
            if (Root.isEmpty() || QFileInfo(RecoveryPath).absolutePath() != Root || !RecoveryPath.endsWith(".json")) return Error("document-recovery-path");
            const auto Restored = recoveryRoot_ + "/restored-" + QUuid::createUuid().toString(QUuid::Id128) + ".txt";
            return open(Restored, false, {}, false, RecoveryPath);
        }
        if (Action == "clipboardResult") {
            if (clipboardRequest_.isEmpty() || Request["token"] != clipboardRequest_["token"] || !Request["accepted"].isBool()) return Error("document-clipboard-token");
            auto Target = find(clipboardRequest_["documentId"].toString()); const auto Mode = clipboardRequest_["mode"].toString();
            const bool Current = Target && QString::number(Target->Revision()) == clipboardRequest_["generation"].toString();
            if (Request["accepted"].toBool() && Current) {
                if (Mode == "cut")Target->pane_->ApplyClipboard(Mode,{},clipboardSelection_);
                if (Mode == "paste") { const auto Text = Request["text"].toString(); if (Text.toUtf8().size() > 64 * 1024 || clipboardSelection_["bytes"].toString().size() > int(((HostChunkLimit + 2) / 3) * 4)) return Error("document-clipboard-budget"); Target->pane_->ApplyClipboard(Mode,Text,clipboardSelection_); }
            }
            clipboardRequest_ = {}; update(); return {{"accepted", true}};
        }
        auto Tab = find(Request["documentId"].toString()); if (!Tab) return Error("document-stale-id");
        if (Request.contains("generation") && Request["generation"].toString() != QString::number(Tab->session_->Snapshot().generation_)) return Error("document-stale-generation");
        if (Request.contains("revision") && Action != "commitSave" && Action != "saveRead" && Request["revision"].toString() != QString::number(Tab->Revision())) return Error("document-stale-revision");
        if (Action == "cancel") { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->queuedSave_.reset();Tab->composingSave_=false;Tab->pane_->Cancel(); return {{"accepted", true}}; }
        if (Action == "reload") { if (Tab->Dirty() && !Request["discard"].toBool()) return Error("document-reload-dirty"); load(Tab, Request["discard"].toBool()); return {{"accepted", true}}; }
        if(Tab->pane_->IsProcessDocument()&& !(Tab->pane_->CanEditEpub() && (Action=="saveAs" || Action=="save")) && (Action=="save"||Action=="saveAs"||Action=="prepareSave"))return Error(Tab->pane_->IsEpub()?"epub-read-only":"document-edit-in-hypereditor");
        if (Action == "close") {
            if (Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || !Tab->pendingToken_.isEmpty() || Tab->checkpointing_) return Error("document-close-busy");
            if ((Tab->Dirty() || Tab->archivePending_) && !Request["discard"].toBool()) return Error("document-close-dirty");
            retire(Tab, Request["discard"].toBool()); update(); return {{"accepted", true}};
        }
        if (Tab->loading_ || Tab->navigating_ || !Tab->pane_) return Error("document-not-ready");
        if (Action == "activate") { activate(Tab); Tab->pane_->Focus(); }
        else if(Action=="editHwp")return editHwp(Tab);
        else if(Action=="editEpub")return editEpub(Tab);
        else if (Action == "replaceText") {
            if (!Request["text"].toString().isValidUtf16()) return Error("p0-utf8-text-required");
            if(Tab->pane_->ViewId()!="text")return Error("document-text-command-on-binary");
            if (!Request["text"].isString() || (!hosted_ && EncodeText(Request["text"].toString(), Tab->input_.bom_, Tab->input_.eol_).size() > TextByteLimit) || Request["text"].toString().contains(QChar::Null)) return Error("p0-text-byte-limit");
            if (Tab->pane_->IsReadOnly()) return Error("document-edit-unavailable");
            auto Result=Tab->pane_->Execute(Request);if(Result.contains("error"))return Result;
        }
        else if (Action == "clipboard") return clipboard(Tab, Request["mode"].toString());
        else if (Action == "selectAll") Tab->pane_->Execute(Request);
        else if (Action == "undo") Tab->pane_->Undo();
        else if (Action == "redo") Tab->pane_->Redo();
        else if (Action == "save") {
            if (Tab->broker_) return prepareSave(Tab);
            if(Tab->activeSave_)save(Tab,Tab->activeSave_->destination_,Tab->activeSave_->overwrite_,Tab->activeSave_->original_ && Tab->pane_->CanEditEpub());
            else save(Tab,Tab->path_,Tab->pane_->IsEpub()?false:Request["overwrite"].toBool(),Tab->pane_->CanEditEpub());
        }
        else if (Action == "saveAs") save(Tab, Request["path"].toString(), Request["overwrite"].toBool());
        else if (Action == "prepareSave") return prepareSave(Tab);
        else if (Action == "saveRead") {
            bool Valid = false; const auto Offset = Request["offset"].toString().toULongLong(&Valid);
            const auto Count = Request["count"].toInt();
            if (!Tab->pending_ || Request["saveToken"].toString() != Tab->pendingToken_ || !Valid
                || Offset > quint64(Tab->pendingBytes_.size()) || Count <= 0 || Count > int(HostChunkLimit)) return Error("document-save-stream-fields");
            const auto Chunk = Tab->pendingBytes_.mid(qsizetype(Offset), Count);
            return {{"bytes", QString::fromLatin1(Chunk.toBase64())}, {"offset", QString::number(Offset)},
                {"eof", Offset + quint64(Chunk.size()) == quint64(Tab->pendingBytes_.size())}};
        }
        else if (Action == "archiveResult") {
            if (!Tab->archiveUpdating_) return Error("archive-update-not-pending");
            Tab->archiveUpdating_ = false;
            if (Request["committed"].toBool()) {
                Tab->archivePending_ = false; retire(Tab, true);
            } else {
                closingWorkspace_ = false; Tab->closeRequested_ = false; Tab->pane_->SetReadOnly(false);
                Tab->error_ = Request["error"].toString("archive-update-failed");
                ShowShellError(tr("압축 파일에 반영하지 못했습니다. 다시 닫아 재시도하거나 다른 위치에 저장하십시오.\n편집본: %1\n%2")
                    .arg(Tab->archiveDraft_, Tab->error_));
            }
        }
        else if (Action == "commitSave") {
            if (!Tab->pending_ || Tab->pendingToken_.isEmpty() || Request["saveToken"].toString() != Tab->pendingToken_
                || Request["revision"].toString() != QString::number(Tab->pending_->revision_) || !Request["committed"].isBool()) return Error("document-stale-save-token");
            DocumentSaveResult Result; Result.committed_ = Request["committed"].toBool();
            if (!Result.committed_) {
                const auto Category = Request["errorCategory"].toInt(int(storage::ErrorCategory::Unavailable));
                Result.error_ = storage::Error{Category >= 0 && Category <= int(storage::ErrorCategory::Internal)
                    ? storage::ErrorCategory(Category) : storage::ErrorCategory::Unavailable, Request["error"].toString("document-source-save-failed").toStdString()};
            }
            if (Request["cleanupWarning"].toBool()) Result.cleanupErrors_.push_back({storage::ErrorCategory::Unavailable, "document-saved-cleanup-warning"});
            const auto Revision = Tab->pending_->revision_;
            if (!Tab->session_->CompleteSave(*Tab->pending_, std::move(Result))) return Error("document-stale-save-token");
            Tab->pendingToken_.clear(); Tab->pendingBytes_.clear(); Tab->pending_.reset();
            if (Request["committed"].toBool()) {
                Tab->archivePending_ = Request["archivePending"].toBool(); Tab->archiveDraft_ = Request["draftPath"].toString();
                Tab->checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
                cleanupRecovery(Tab, Revision);
                if (Tab->closeAfterSave_ && !Tab->Dirty()) { Tab->closeAfterSave_ = false; closeTab(int(std::distance(documents_.begin(), std::find(documents_.begin(), documents_.end(), Tab)))); } else Tab->closeAfterSave_ = false;
            }
            else { Tab->closeAfterSave_ = false; closingWorkspace_ = false; }
        }
        else {auto Result=Tab->pane_->Execute(Request);if(Result.contains("error"))return Result;}
        update(); tryFinishClose(); return {{"accepted", true}};
    }
    QJsonObject DocumentWorkspaceWidget::editHwp(const std::shared_ptr<Tab>& Tab)
    {
        if(!Tab||!Tab->pane_->IsHwp()||Tab->loading_||Tab->pane_->IsBusy()||!Tab->pane_->ViewState()["ready"].toBool())return Error("document-edit-unavailable");
        if(!editRequest_.isEmpty())return Error("document-edit-busy");
        if(hosted_){
            editRequest_={{"token",QUuid::createUuid().toString(QUuid::Id128)},{"documentId",Tab->id_},
                {"generation",QString::number(Tab->session_->Snapshot().generation_)}};
        }else{
            const auto Program=QCoreApplication::applicationDirPath()+"/HyperEditor.exe";
            if(!QProcess::startDetached(Program,{"--rhwp-edit","--config-dir",QFileInfo(settingsPath_).absolutePath(),"--",Tab->path_}))return Error("document-editor-start-failed");
        }
        update();return {{"accepted",true}};
    }
    QJsonObject DocumentWorkspaceWidget::editEpub(const std::shared_ptr<Tab>& Tab)
    {
        if(!Tab || !hosted_ || !Tab->pane_->IsEpub() || Tab->loading_ || Tab->pane_->IsBusy() || !Tab->pane_->ViewState()["ready"].toBool())return Error("document-edit-unavailable");
        if(!editRequest_.isEmpty())return Error("document-edit-busy");
        const auto State=Tab->pane_->ViewState()["epub"].toObject();
        editRequest_={{"token",QUuid::createUuid().toString(QUuid::Id128)},{"documentId",Tab->id_},
            {"generation",QString::number(Tab->session_->Snapshot().generation_)},{"format","epub"},
            {"locator",State["locator"]},{"chapter",State["chapterIndex"]},{"appearance",epubui::Appearance()}};
        update();return {{"accepted",true}};
    }
    void DocumentWorkspaceWidget::update()
    {
        if (externalWatcher_) {
            QStringList Paths;
            for (const auto& Tab : documents_) if (!Tab->closed_ && !Tab->loading_ && Tab->originalAuthority_ && !Tab->broker_ && Tab->sourceToken_.isEmpty()) Paths.append(Tab->path_);
            externalWatcher_->setWatchedFiles(Paths);
        }
        for (const auto& Tab : documents_) if (tabbed_) {
            const auto Index = tabs_->indexOf(Tab->pane_); if (Index < 0) continue;
            const auto Name = documentName(*Tab);
            tabs_->setTabText(Index, Name + (Tab->Dirty() ? " *" : "") + (Tab->loading_ ? tr(" · 여는 중") : "")
                + (Tab->session_->Snapshot().error_ || !Tab->error_.isEmpty() ? tr(" · 오류") : ""));
        }
        auto Tab = active();
        if(epubFullscreen_ && (!Tab || !Tab->pane_->IsEpub()))toggleEpubFullscreen();
        if (Tab) {
            workspace_.Activate(Tab->id_.toStdString());
            if(!findChild<ImageFullscreen*>("imageFullscreen")->Active()) {
                auto Contribution=Tab->pane_->Contribution();
                if(Tab->pane_->IsEpub())Contribution.menuActions_=Tab->pane_->IsEpubEditing() && Tab->pane_->EpubEditor()?Tab->pane_->EpubEditor()->MenuActions():findChild<QMenu*>("epubCommands")->actions();
                chrome_->SetActive(std::move(Contribution));
            }
        }
        // Search is an auxiliary tab: retain the document's chrome policy.
        else if (!searchPanel_ || tabs_->currentWidget() != searchPanel_) chrome_->SetActive({});
        if (Tab || !searchPanel_ || tabs_->currentWidget() != searchPanel_) applyKind(Tab ? Tab->pane_->Kind() : DocumentViewKind::None);
        updateTitle();
        if (auto* Conflict = findChild<QAction*>("resolveDocumentConflict", Qt::FindDirectChildrenOnly))
            Conflict->setVisible(Tab && Tab->session_->Snapshot().saveState_ == DocumentSaveState::Conflict);
        auto Capability = Tab ? Tab->session_->Capabilities() : DocumentCapabilities{};
        if(Tab && Tab->pane_->IsProcessDocument()){Capability.save_=Tab->originalSave_ && Tab->pane_->CanEditEpub();Capability.saveAs_=Tab->pane_->CanEditEpub();Capability.undo_=Tab->pane_->CanUndo();Capability.redo_=Tab->pane_->CanRedo();}
        for (const auto& Item : {std::pair{"saveDocument", Capability.save_ || Capability.saveAs_},
            std::pair{"saveDocumentAs", Capability.saveAs_}, std::pair{"undoDocument", Capability.undo_}, std::pair{"redoDocument", Capability.redo_},
            std::pair{"closeDocument", Capability.close_ && Tab && !Tab->checkpointing_},
            std::pair{"resolveDocumentConflict", Tab && Capability.view_ && Tab->session_->Snapshot().saveState_ == DocumentSaveState::Conflict}, std::pair{"cancelDocument", hotExitClosing_ || Capability.cancel_ || (Tab && (Tab->composingSave_ || Tab->pane_->IsBusy()))},
            std::pair{"reloadDocument", Tab && !Tab->saving_ && Tab->pendingToken_.isEmpty()}}) {
            if (auto* Action = findChild<QAction*>(Item.first)) Action->setEnabled(Item.second);
        }
        if(Tab && Tab->activeSave_)findChild<QAction*>("saveDocument")->setEnabled(true);
        const bool Hwp=Tab&&Tab->pane_->IsHwp();
        auto* EditHwp=findChild<QAction*>("editHwpDocument");EditHwp->setVisible(Hwp);
        EditHwp->setEnabled(Hwp&&!Tab->loading_&&!Tab->pane_->IsBusy()&&Tab->pane_->ViewState()["ready"].toBool()&&editRequest_.isEmpty());
        if(Hwp){
            findChild<QAction*>("saveDocument")->setEnabled(false);findChild<QAction*>("saveDocumentAs")->setEnabled(false);
        }
        if(Tab&&Tab->pane_->IsEpub()){
            // Both commands apply and validate; only an authoritative local source permits original Save.
            // Draft writes and discovery do not mutate the live source and may overlap a save.
            const bool CanSave = !hosted_ && !Tab->loading_ && !Tab->saving_ && !Tab->choosingSavePath_ && Tab->pendingToken_.isEmpty() && Tab->pane_->CanPrepareEpubSave();
            findChild<QAction*>("saveDocument")->setEnabled(CanSave && Tab->originalSave_ || bool(Tab->activeSave_));findChild<QAction*>("saveDocumentAs")->setEnabled(CanSave);
        }
        if(auto* Reopen=findChild<QAction*>("epubReopenDocument"))Reopen->setVisible(Tab && Tab->pane_->IsEpub() && Tab->pane_->findChild<QAction*>("epubReopen"));
        findChild<QAction*>("printDocument")->setEnabled(Tab&&(!Tab->pane_->IsMarkdown()||Tab->pane_->MarkdownSourceVisible())&&!Tab->pane_->IsEpub()&&!Tab->loading_&&!Tab->pane_->IsBusy()&&(!Hwp||Tab->pane_->ViewState()["ready"].toBool()));
        findChild<QAction*>("previewDocument")->setEnabled(Tab&&(!Tab->pane_->IsMarkdown()||Tab->pane_->MarkdownSourceVisible())&&!Tab->pane_->IsEpub()&&!Hwp&&!Tab->loading_&&!Tab->pane_->IsBusy());
        for (auto* Action : findChildren<QAction*>()) {
            const auto Id = Action->property("documentCommandId").toString();
            const auto Scope=DocumentShortcutRegistry::ScopeOf(Id);
            if(Scope=="markdown")Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->CanRunViewCommand(Id));
            if(Scope=="epub")Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->IsEpub() && !Tab->pane_->IsEpubEditing());
            // Editor-owned actions retain their resource/selection eligibility during status polling.
            if(Scope.startsWith("epub-edit") && Action->parent()==this)Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->IsEpubEditing());
            if(Scope=="image" || Scope=="image.comic" || Scope=="pdf")Action->setEnabled(Tab && !Tab->loading_);
            if (Id.startsWith("text.") || QStringList{"edit.cut", "edit.copy", "edit.paste", "edit.selectAll"}.contains(Id))
                Action->setEnabled(Tab && !Tab->loading_ && ((Tab->pane_->ViewId() == "text"
                    && ((Id != "edit.cut" && Id != "edit.paste" && Id != "text.replace") || !Tab->pane_->IsReadOnly())) || Tab->pane_->IsEpubEditing()));
            if (Id == "file.new" || Id == "file.newText" || Id == "file.newMarkdown" || Id == "file.newImage")
                Action->setEnabled((hosted_ || documents_.size() < 16) && (tabbed_ || documents_.empty()));
            // HyperViewer hands a new EPUB to HyperEditor: no tab of its own.
            if (Id == "file.newEpub") Action->setEnabled(!epubCreating_ && (hosted_ || ((documents_.size() < 16) && (tabbed_ || documents_.empty()))));
            if(Tab&&Tab->pane_->IsMarkdown()&&!Tab->pane_->MarkdownSourceVisible()&&(Id.startsWith("text.")||Id.startsWith("edit.")))Action->setEnabled(Id=="text.find"||Id=="edit.copy"||Id=="edit.selectAll");
            if (Id == "tab.next" || Id == "tab.previous") Action->setEnabled(tabbed_ && tabs_->count() > 1);
        }
        QList<DocumentStatusField> Fields;
        // The overlay follows the active tab: a switch shows that tab's operation at once, or clears.
        if(const auto Current=Tab?Tab->id_:QString();progressOverlay_&&Current!=progressTab_){progressTab_=Current;progressOverlay_->ShowCurrent(Tab?Tab->progress_:DocumentProgress{});}
        if(Tab) {
            if(!Tab->loading_)Fields=Tab->pane_->StatusFields();
            const auto State=Tab->session_->Snapshot();const auto View=Tab->pane_->ViewState();
            const bool Reading=View.contains("reading") || (Tab->pane_->IsProcessDocument()&&!Tab->pane_->IsEpubEditing());
            const bool ReadOnly=(Tab->pane_->IsMarkdown()&&!Tab->pane_->MarkdownSourceVisible()) || Reading || (Tab->pane_->ViewId()=="text"?View["readOnly"].toBool():!(Capability.save_||Capability.saveAs_));
            QString Mode=ReadOnly?tr("읽기 전용"):Tab->pane_->ViewId()=="pdf"?tr("주석 편집"):tr("편집");
            const auto Destination=Tab->pane_->IsEpub()?(Tab->originalSave_?tr("현재 EPUB에 저장"):tr("사본 · 다른 이름으로 저장")):Tab->broker_?tr("원본 공급자에 저장"):Tab->originalSave_?tr("로컬 파일"):tr("사본 · 다른 이름으로 저장");
            // Where a save goes only shows when it is not the file itself (prototype "편집 · 사본으로 저장").
            if(!ReadOnly && !Tab->originalSave_)Mode+=Tab->broker_?tr(" · 원본 공급자에 저장"):tr(" · 사본으로 저장");
            if(!Tab->loading_ && !Tab->input_.bytes_.isEmpty()) {
                const auto Size=QLocale().formattedDataSize(Tab->input_.bytes_.size());
                const auto Format=std::find_if(Fields.begin(),Fields.end(),[](const auto& Field){return Field.id_=="format";});
                if(Format!=Fields.end()){Format->text_+=" · "+Size;Format->detail_=tr("열 때의 파일 크기 %1").arg(Size);}
                else Fields.append(DocumentStatusField{"fileSize",Size,{},tr("열 때의 파일 크기"),{},false});
            }
            if(Tab->pane_->ViewId()=="text"&&Tab->input_.bytes_.size()>=qBound(1,appliedPreferences_["documentViews"].toObject()["textView"].toObject()["largeFileMB"].toInt(hc::defaults::edit::ViewValue("textView/largeFileMB").toInt()),16)*1024*1024)
                Fields.append(DocumentStatusField{"largeFile",tr("대용량 문서"),{},tr("편집·검색에 시간이 걸릴 수 있습니다."),{},false});
            Fields.append(DocumentStatusField{"mode",Mode,{},Destination,{},true,true});
            QString MessageText,Detail;
            using OverlayState=DocumentProgressOverlay::State;
            const auto Shown=progressOverlay_?progressOverlay_->CurrentState():OverlayState::Idle;
            // The overlay already says '여는 중' · '저장 중'; the right-hand item keeps only what outlives it.
            const bool Overlaid=progressOverlay_ && Tab->progress_.kind_!=DocumentProgress::Kind::None && progressOverlay_->CurrentToken()==Tab->progress_.token_
                && (Shown==OverlayState::Pending||Shown==OverlayState::Visible||Shown==OverlayState::Done||Shown==OverlayState::Cancelling);
            const auto Code=!Tab->error_.isEmpty()?Tab->error_:!View["error"].toString().isEmpty()?View["error"].toString():State.error_?QString::fromStdString(State.error_->code_):QString{};
            if(hotExitClosing_){MessageText=tr("종료 준비 · 복구 사본 보존 중…");Detail=tr("취소 명령으로 종료를 중단할 수 있습니다.");}
            else if(!shellError_.isEmpty()){MessageText=tr("⚠ 작업 오류");Detail=shellError_;}
            else if(State.saveState_==DocumentSaveState::Conflict){MessageText=tr("⚠ 원본 변경 충돌");Detail=Message("external-changed");}
            else if(!Code.isEmpty()){MessageText=State.saveState_==DocumentSaveState::Failed?tr("⚠ 저장 실패"):tr("⚠ 문서 오류");Detail=Message(Code)+"\n"+Code;}
            else if(!Tab->recoveryNotice_.isEmpty()){MessageText=tr("복구된 변경");Detail=Tab->recoveryNotice_;}
            else if((Tab->loading_||Tab->navigating_) && !Overlaid)MessageText=tr("여는 중…");
            else if(Tab->composingSave_)MessageText=tr("한글 조합 완료 후 저장…");
            else if(Tab->saving_||!Tab->pendingToken_.isEmpty())MessageText=Tab->queuedSave_?tr("저장 중 · 다음 저장 예약됨…"):Overlaid?QString():tr("저장 중…");
            else if(Tab->pane_->IsBusy())MessageText=tr("처리 중…");
            if(Tab->Dirty())MessageText+=MessageText.isEmpty()?tr("● 미저장"):tr(" · 미저장");
            if(!MessageText.isEmpty())Fields.append(DocumentStatusField{"state",MessageText,{},Detail,{},true,true});
        } else Fields.append(DocumentStatusField{"empty",shellError_.isEmpty()?tr("파일을 열거나 최근 항목을 선택하십시오."):shellError_});
        statusFieldNames_.clear();for(const auto& Field:Fields)statusFieldNames_.append({Field.id_,Field.text_});
        if(Tab){
            const auto Hidden=chrome_->Settings()["statusHidden"].toObject()[viewKinds::Name(CurrentKind())].toArray();
            if(!Hidden.isEmpty())Fields.erase(std::remove_if(Fields.begin(),Fields.end(),[&Hidden](const auto& Field){return Field.id_!="mode"&&Field.id_!="state"&&Hidden.contains(Field.id_);}),Fields.end());
        }
        statusBar_->SetFields(Fields);statusSlot_->show();
        placeStartPage(); syncToolbarRow(); syncSearchHost();
        findChild<ImageFullscreen*>("imageFullscreen")->UpdateStatus();
        emit StateChanged();
    }
    void DocumentWorkspaceWidget::refreshRecentMenu()
    {
        for (const auto& Action : std::exchange(recentActions_, {})) if (Action) { fileMenu_->removeAction(Action); Action->deleteLater(); }
        if (!recent_ || !fileMenu_) return;
        const int Count = qBound(0, PreferenceValue(appliedPreferences_,"recent/menuCount").toInt(), 15);
        if (recentAnchor_) recentAnchor_->setVisible(Count > 0);
        RecentMenuHandlers Handlers;
        Handlers.open_ = [this](const RecentDocument& Document) { openRecent(Document); };
        Handlers.showAll_ = [this] { showRecentDialog(); };
        Handlers.clear_ = [this] { clearRecent(); };
        for (auto* Action : AppendRecentSection(fileMenu_, recent_->Entries(), Count, Handlers)) recentActions_.append(Action);
        fileMenu_->setToolTipsVisible(true);
    }
    void DocumentWorkspaceWidget::recordRecent(const std::shared_ptr<Tab>& Tab)
    {
        if (!recent_ || !Tab || !Tab->pane_ || Tab->path_.isEmpty()) return;
        const auto View = Tab->pane_->ViewState();
        RecentRecordContext Context;
        Context.enabled_ = PreferenceValue(appliedPreferences_,"recent/enabled").toBool();
        Context.temporarySource_ = !Tab->temporarySource_.isEmpty();
        Context.brokerCopy_ = Tab->broker_ || !Tab->sourceToken_.isEmpty();
        // A hosted viewer opens local files through its slot: the path is still the user's file.
        Context.originalAuthority_ = Tab->originalAuthority_ || (Tab->hostSlot_ && !Context.brokerCopy_);
        Context.recovered_ = Tab->recovered_;
        Context.failed_ = Tab->session_->Snapshot().lifecycle_ != DocumentLifecycle::Ready;
        Context.privateReading_ = View["reading"].toObject()["private"].toBool() || qApp->property("privateReading").toBool()
            || View["epub"].toObject()["profile"].toObject()["privateReading"].toBool();
        if (!ShouldRecordRecent(Context)) return;
        RecentDocument Document;
        Document.path_ = Tab->path_; Document.viewId_ = Tab->pane_->ViewId(); Document.openedAt_ = QDateTime::currentDateTime();
        Document.encoding_ = Tab->encodingOverride_;
        switch (Tab->pane_->Kind()) {
            case DocumentViewKind::MarkdownPreview: case DocumentViewKind::MarkdownEdit:
                Document.kind_ = "markdown"; Document.markdownView_ = View["markdown"].toObject()["mode"].toString(); break;
            case DocumentViewKind::Pdf: Document.kind_ = "pdf"; Document.pdfPage_ = View["page"].toInt() + 1; break;
            case DocumentViewKind::Image: Document.kind_ = "image"; Document.imageMode_ = "single"; break;
            case DocumentViewKind::Comic: case DocumentViewKind::Webtoon: {
                const auto Reading = View["reading"].toObject();
                Document.kind_ = Tab->pane_->Kind() == DocumentViewKind::Webtoon ? "webtoon" : "comic"; Document.imageMode_ = Document.kind_;
                Document.readingPage_ = Reading["page"].toInt() + 1; Document.readingPageCount_ = Reading["pageCount"].toInt();
                break;
            }
            case DocumentViewKind::EpubRead: case DocumentViewKind::EpubEdit:
                Document.kind_ = "epub"; Document.epubMode_ = Tab->pane_->IsEpubEditing() ? "edit" : "read"; break;
            case DocumentViewKind::Hwp: Document.kind_ = "hwp"; break;
            default: Document.kind_ = "text"; break;
        }
        recent_->Record(std::move(Document));
    }
    void DocumentWorkspaceWidget::openRecent(const RecentDocument& Document)
    {
        if (!QFileInfo::exists(Document.path_)) {
            auto* Box = new hc::ui::MessageDialog(QMessageBox::Question, tr("최근 항목"),
                tr("%1을(를) 찾을 수 없습니다 — 목록에서 제거할까요?").arg(QDir::toNativeSeparators(Document.path_)), QMessageBox::Yes | QMessageBox::No, this);
            Box->setAttribute(Qt::WA_DeleteOnClose);
            connect(Box, &QMessageBox::finished, this, [this, Key = Document.Key()](int Result) { if (Result == QMessageBox::Yes && recent_) recent_->Remove(Key); });
            Box->open(); return;
        }
        pendingOpenEncoding_ = CanonicalEncodingName(Document.encoding_);
        // HyperViewer replaces its current document; HyperEditor opens a tab (or goes to the open one).
        const auto Reply = hosted_ ? open(Document.path_, true, {}, false) : open(Document.path_, false, {}, false);
        pendingOpenEncoding_.clear();
        if (Reply.contains("error")) { ShowShellError(Message(Reply["error"].toString())); return; }
        if (Reply["duplicate"].toBool()) return;
        if (auto Tab = find(Reply["documentId"].toString())) {
            auto Restore = Document; if (hosted_) Restore.epubMode_.clear();
            Tab->restore_ = Restore;
        }
    }
    void DocumentWorkspaceWidget::showRecentDialog()
    {
        if (!recent_) return;
        if (!recentDialog_) {
            recentDialog_ = new RecentDocumentsDialog(recent_, this); recentDialog_->setAttribute(Qt::WA_DeleteOnClose);
            connect(recentDialog_, &RecentDocumentsDialog::OpenRequested, this, [this](const RecentDocument& Document) {
                if (recentDialog_) recentDialog_->close();
                openRecent(Document);
            });
        }
        recentDialog_->show(); recentDialog_->raise(); recentDialog_->activateWindow();
    }
    void DocumentWorkspaceWidget::clearRecent()
    {
        if (!recent_) return;
        // From the settings dialog the question belongs to that dialog, which is modal over the workspace.
        QWidget* Owner = preferences_ && preferences_->isVisible() ? static_cast<QWidget*>(preferences_.data()) : this;
        auto* Box = new hc::ui::MessageDialog(QMessageBox::Question, tr("최근 항목 지우기"), tr("고정하지 않은 최근 항목을 모두 지우시겠습니까?"),
            QMessageBox::Yes | QMessageBox::Cancel, Owner);
        Box->setAttribute(Qt::WA_DeleteOnClose);
        connect(Box, &QMessageBox::finished, this, [this](int Result) { if (Result == QMessageBox::Yes && recent_) recent_->Clear(false); });
        Box->open();
    }
    void DocumentWorkspaceWidget::syncToolbarRow()
    {
        if (!chrome_ || !toolbar_) return;
        const bool Single = chrome_->Settings()["toolbarLayout"].toString(QStringLiteral("single")) != QLatin1String("split");
        const auto Layout = Single ? QStringLiteral("single") : QStringLiteral("split");
        const auto Tab = active();
        QToolBar* ActiveBar = Tab && Tab->pane_ && !Tab->loading_ ? Tab->pane_->PrimaryToolbar() : nullptr;
        toolbar_->setVisible(toolbarVisible_ && (!Single || !ActiveBar));
        for (const auto& Each : documents_) {
            auto* Bar = Each->pane_ ? Each->pane_->PrimaryToolbar() : nullptr; if (!Bar) continue;
            if (Bar->isHidden() == toolbarVisible_) Bar->setVisible(toolbarVisible_);
            if (Bar->property("documentCommonLayout").toString() == Layout) continue;
            Bar->setProperty("documentCommonLayout", Layout);
            for (auto* Action : Bar->actions()) {
                if (!Action->property("documentCommon").toBool()) continue;
                Bar->removeAction(Action);
                if (Action->objectName() == QLatin1String("documentCommonSeparator")) Action->deleteLater();
            }
            if (!Single) continue;
            // Shared QActions: kind visibility and enablement follow the common toolbar.
            auto* First = Bar->actions().value(0);
            for (auto* Action : toolbar_->actions()) {
                if (qobject_cast<QWidgetAction*>(Action)) continue;
                Action->setProperty("documentCommon", true); Bar->insertAction(First, Action);
            }
            auto* Split = new QAction(Bar); Split->setSeparator(true); Split->setObjectName("documentCommonSeparator"); Split->setProperty("documentCommon", true);
            Bar->insertAction(First, Split);
        }
    }
    void DocumentWorkspaceWidget::showChromeMenu(const QPoint& Global, bool Toolbar)
    {
        refreshChromeChecks();
        QMenu Menu(this);
        for (const auto* Name : {"workspaceDisplayOptions", "view.toolbar", "view.statusBar"})
            if (auto* Action = findChild<QAction*>(QString::fromLatin1(Name))) Menu.addAction(Action);
        if (Toolbar) {
            Menu.addSeparator();
            for (const auto* Name : {"view.chromeViewOnly", "view.chromeReset"})
                if (auto* Action = findChild<QAction*>(QString::fromLatin1(Name))) Menu.addAction(Action);
            Menu.addSeparator();
            auto* Split = Menu.addAction(tr("도구모음 두 줄로 표시"));
            Split->setCheckable(true); Split->setChecked(chrome_->Settings()["toolbarLayout"].toString() == QLatin1String("split"));
            connect(Split, &QAction::toggled, this, [this](bool On) {
                auto Settings = chrome_->Settings(); Settings["toolbarLayout"] = On ? QStringLiteral("split") : QStringLiteral("single");
                if (chrome_->ApplySettings(Settings)) { syncToolbarRow(); emit ChromeSettingsChanged(chrome_->Settings()); }
            });
        }
        Menu.exec(Global);
    }
    void DocumentWorkspaceWidget::showTabMenu(const QPoint& Global, int Index)
    {
        auto* Pane = tabs_->widget(Index);
        std::shared_ptr<Tab> Target; for (const auto& Each : documents_) if (Each->pane_ == Pane) Target = Each;
        if (!Target) return;
        activate(Target);
        QMenu Menu(this);
        if (auto* Close = findChild<QAction*>("closeDocument")) Menu.addAction(Close);
        auto* Others = Menu.addAction(tr("다른 탭 닫기"), this, [this, Target] {
            for (const auto& Each : std::vector<std::shared_ptr<Tab>>(documents_)) if (Each != Target) {
                const auto At = std::find(documents_.begin(), documents_.end(), Each); if (At != documents_.end()) closeTab(int(std::distance(documents_.begin(), At)));
            }
        });
        Others->setEnabled(documents_.size() > 1);
        const int Position = tabs_->indexOf(Target->pane_);
        auto* Right = Menu.addAction(tr("오른쪽 탭 닫기"), this, [this, Position] {
            std::vector<std::shared_ptr<Tab>> Targets;
            for (const auto& Each : documents_) if (tabs_->indexOf(Each->pane_) > Position) Targets.push_back(Each);
            for (const auto& Each : Targets) { const auto At = std::find(documents_.begin(), documents_.end(), Each); if (At != documents_.end()) closeTab(int(std::distance(documents_.begin(), At))); }
        });
        Right->setEnabled(Position >= 0 && Position < tabs_->count() - 1);
        Menu.addSeparator();
        const bool Local = Target->temporarySource_.isEmpty() && !Target->path_.isEmpty();
        const auto Native = QDir::toNativeSeparators(Target->path_);
        Menu.addAction(tr("경로 복사"), this, [Native] { QGuiApplication::clipboard()->setText(Native); })->setEnabled(Local);
        Menu.addAction(tr("파일 이름 복사"), this, [Native] { QGuiApplication::clipboard()->setText(QFileInfo(Native).fileName()); })->setEnabled(Local);
        Menu.addAction(tr("탐색기에서 위치 열기"), this, [Native] {
            // HyperCommander has no select-path command line contract yet; Explorer /select is the stand-in (plan 6.4).
            QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + Native});
        })->setEnabled(Local && QFileInfo::exists(Target->path_));
        Menu.addSeparator();
        if (auto* Reload = findChild<QAction*>("reloadDocument")) Menu.addAction(Reload);
        Menu.exec(Global);
    }
    void DocumentWorkspaceWidget::showStatusMenu(const QPoint& Global)
    {
        // Per view kind: the fields a user hid (chrome settings "statusHidden": {kind: [id…]}).
        const auto Kind = viewKinds::Name(CurrentKind());
        const auto Hidden = chrome_->Settings()["statusHidden"].toObject()[Kind].toArray();
        const auto Store = [this, Kind](const QJsonArray& List) {
            auto Settings = chrome_->Settings(); auto All = Settings["statusHidden"].toObject();
            if (List.isEmpty()) All.remove(Kind); else All[Kind] = List;
            Settings["statusHidden"] = All;
            if (chrome_->ApplySettings(Settings)) { emit ChromeSettingsChanged(chrome_->Settings()); update(); }
        };
        QMenu Menu(this);
        auto* Heading = Menu.addAction(tr("상태표시줄 항목")); Heading->setEnabled(false);
        for (const auto& Field : statusFieldNames_) {
            if (Field.first == QLatin1String("mode") || Field.first == QLatin1String("state") || Field.first == QLatin1String("empty")) continue;
            auto* Item = Menu.addAction(Field.second); Item->setCheckable(true); Item->setChecked(!Hidden.contains(Field.first));
            connect(Item, &QAction::toggled, this, [this, Kind, Store, Id = Field.first](bool On) {
                auto List = chrome_->Settings()["statusHidden"].toObject()[Kind].toArray();
                for (qsizetype At = List.size(); At-- > 0;) if (List[At].toString() == Id) List.removeAt(At);
                if (!On) List.append(Id);
                Store(List);
            });
        }
        Menu.addSeparator();
        Menu.addAction(tr("기본값으로"), this, [Store] { Store({}); })->setEnabled(!Hidden.isEmpty());
        Menu.exec(Global);
    }
    void DocumentWorkspaceWidget::placeStartPage()
    {
        if (!startPage_) return;
        const bool Show = documents_.empty();
        auto* Area = tabbed_ ? static_cast<QWidget*>(tabs_) : static_cast<QWidget*>(singlePane_);
        if (Show) { startPage_->setGeometry(Area->geometry()); startPage_->show(); startPage_->raise(); }
        else startPage_->hide();
    }
    void DocumentWorkspaceWidget::resizeEvent(QResizeEvent* Event)
    {
        QWidget::resizeEvent(Event);
        // The layout places the document area after this event; place the start page once it has.
        QMetaObject::invokeMethod(this, &DocumentWorkspaceWidget::placeStartPage, Qt::QueuedConnection);
    }
    void DocumentWorkspaceWidget::dragEnterEvent(QDragEnterEvent* Event)
    {
        if (!hosted_ && Event->mimeData()->hasUrls()) Event->acceptProposedAction();
    }
    void DocumentWorkspaceWidget::dropEvent(QDropEvent* Event)
    {
        if (hosted_ || !Event->mimeData()->hasUrls()) return;
        QStringList Rejected;
        for (const auto& Url : Event->mimeData()->urls())
            if (Url.isLocalFile() && open(Url.toLocalFile(), false, {}, false).contains("error")) Rejected.append(Url.toLocalFile());
        ShowShellError(Rejected.isEmpty() ? QString{} : tr("문서 한도 때문에 열지 못한 파일: %1").arg(Rejected.join(", ")));
        Event->acceptProposedAction();
    }
    void DocumentWorkspaceWidget::refreshProgress(const std::shared_ptr<Tab>& Tab)
    {
        if (!progressOverlay_ || !Tab || Tab != active()) return;
        if (Tab->progress_.kind_ != DocumentProgress::Kind::None) progressOverlay_->Update(Tab->progress_);
    }
    void DocumentWorkspaceWidget::finishProgress(const std::shared_ptr<Tab>& Tab, bool Success, const QString& Code)
    {
        if (Tab->progress_.kind_ == DocumentProgress::Kind::None) return;
        const auto Progress = std::exchange(Tab->progress_, {});
        if (!progressOverlay_ || Tab != active()) return;
        const bool Saving = Progress.kind_ == DocumentProgress::Kind::Save || Progress.kind_ == DocumentProgress::Kind::EpubSave;
        DocumentProgressResult Result; Result.success_ = Success;
        if (Success) Result.doneLabel_ = Saving ? tr("저장 완료") : tr("열기 완료");
        else if (Code.isEmpty()) { if (progressOverlay_->CurrentToken() == Progress.token_) progressOverlay_->Clear(); return; }
        else {
            Result.failureSummary_ = (Saving ? tr("저장 실패 · %1") : tr("열기 실패 · %1")).arg(Message(Code).section(QRegularExpression(QStringLiteral("[.。]\\s")), 0, 0));
            Result.failureDetail_ = Message(Code) + "\n" + Code;
        }
        progressOverlay_->Finish(Progress.token_, Result);
    }
    void DocumentWorkspaceWidget::openStatusPicker(const QString& Id)
    {
        auto Tab=active();if(!Tab||Tab->loading_)return;
        auto* Field=statusBar_->findChild<QWidget*>("status."+Id);
        if(Field&&Field->isVisible())Tab->pane_->OpenStatusPicker(Id,Field);
        else Tab->pane_->RunStatusCommand(Id=="position"?QStringLiteral("text.goToLine"):Id=="page"?QStringLiteral("status.page"):Id=="zoom"?QStringLiteral("status.zoom"):Id);
    }
    void DocumentWorkspaceWidget::retire(std::shared_ptr<Tab> Tab, bool Discard, bool PreserveRecovery)
    {
        if(Discard && !PreserveRecovery)Tab->pane_->DiscardEpubRecovery();
        if(!Tab->pane_->RequestClose()){Tab->closeRequested_=true;return;}
        if(Tab->everReady_ && !Tab->loading_)recordRecent(Tab);
        if(editRequest_["documentId"]==Tab->id_)editRequest_={};
        // An EPUB keeps its edits in the EPUB draft (kept by PreserveRecovery); its session closes as discarded.
        const auto Disposition = PreserveRecovery && hotExitReady_ && Tab->Dirty() && !Tab->pane_->IsEpub() ? CloseDisposition::PreserveSnapshot
            : Discard ? CloseDisposition::Discard : CloseDisposition::KeepChanges;
        if (!workspace_.Remove(Tab->id_.toStdString(), Disposition).HasValue()) return;
        Tab->cancellation_->request_stop(); Tab->closed_ = true; Tab->pane_->Cancel();
        if (tabbed_) tabs_->removeTab(tabs_->indexOf(Tab->pane_)); else singlePane_->removeWidget(Tab->pane_);
        Tab->pane_->deleteLater(); std::erase(documents_, Tab);
        if(!Tab->temporarySource_.isEmpty())QFile::remove(Tab->temporarySource_);
        if (!PreserveRecovery) cleanupRecovery(Tab, Tab->Revision());
    }
    void DocumentWorkspaceWidget::closeTab(int Index)
    {
        if (hotExitClosing_ || closePromptActive_ || Index < 0 || Index >= int(documents_.size())) return;
        auto Target = documents_[Index];
        Target->closeRequested_ = true;
        if(Target->pane_->IsProcessDocument()&&!Target->pane_->CanEditEpub()&&!Target->pane_->RequestClose())return;
        if (Target->choosingSavePath_ || Target->saving_ || Target->composingSave_ || !Target->pendingToken_.isEmpty() || Target->checkpointing_ || Target->pane_->IsEditing()
            || (Target->pane_->ViewId() != "image" && Target->pane_->IsBusy())) return;
        Target->closeRequested_ = false;
        if (Target->Dirty()) {
            // The modal prompt pumps events; preview reports can request another close meanwhile.
            const QScopedValueRollback PromptGuard(closePromptActive_,true);
            const int Choice = closePrompt_ ? closePrompt_(Target->id_) : int(hc::ui::MessageDialog::question(this, tr("변경 내용 저장"),
                tr("변경 내용을 저장하시겠습니까?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel));
            if (Choice != QMessageBox::Save && Choice != QMessageBox::Discard) { closingWorkspace_ = false; return; }
            if (Choice == QMessageBox::Save) {
                activate(Target); Target->closeAfterSave_ = true; findChild<QAction*>(Target->pane_->CanEditEpub() && !Target->originalSave_?"saveDocumentAs":"saveDocument")->trigger(); return;
            }
        }
        if (Target->archiveUpdating_) return;
        if (Target->archivePending_) {
            const QScopedValueRollback PromptGuard(closePromptActive_, true);
            const auto Choice = hc::ui::MessageDialog::question(this, tr("압축 파일 갱신"),
                tr("임시 파일에 저장한 변경 내용을 압축 파일에 반영하시겠습니까?\n아니요를 선택하면 저장한 임시 편집본을 버립니다."),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
            if (Choice == QMessageBox::Cancel) { closingWorkspace_ = false; return; }
            Target->pane_->SetReadOnly(true); Target->archiveUpdating_ = true; Target->archiveDiscard_ = Choice == QMessageBox::No;
            update(); return;
        }
        retire(Target, true); update();
    }
    QJsonObject DocumentWorkspaceWidget::clipboard(const std::shared_ptr<Tab>& Tab, const QString& Mode)
    {
        if (!clipboardBroker_ || !clipboardRequest_.isEmpty() || !Tab || (Mode != "copy" && Tab->pane_->IsReadOnly())
            || (Mode != "copy" && Mode != "cut" && Mode != "paste")) return Error("document-clipboard-unavailable");
        if (Tab->pane_->ViewId() != "text" && Mode != "copy") return Error("document-clipboard-view-unavailable");
        clipboardSelection_=Tab->pane_->CaptureSelection();
        if(clipboardSelection_.isEmpty())return Error("document-clipboard-view-unavailable");
        const auto Text=clipboardSelection_["text"].toString();
        if (Text.toUtf8().size() > 64 * 1024) return Error("document-clipboard-budget");
        clipboardRequest_ = {{"token", QUuid::createUuid().toString(QUuid::Id128)}, {"mode", Mode},
            {"documentId", Tab->id_}, {"generation", QString::number(Tab->Revision())}, {"text", Text},
            {"mime", clipboardSelection_["mime"]}, {"bytes", clipboardSelection_["bytes"]}};
        return {{"accepted", true}};
    }
    bool DocumentWorkspaceWidget::eventFilter(QObject* Watched, QEvent* Event)
    {
        if (Watched == this && Event->type() == QEvent::WindowStateChange
            && !hosted_ && !parentWidget() && layout()) {
            const int Border = isMaximized() || isFullScreen() ? 0 : 6;
            layout()->setContentsMargins(Border, Border, Border, Border);
        }
        if (Event->type() == QEvent::ShortcutOverride || Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            auto* Widget = qobject_cast<QWidget*>(Watched);
            if (Key->key() == Qt::Key_Escape && Key->modifiers() == Qt::NoModifier && !Key->isAutoRepeat()
                && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()
                && !(preferences_ && preferences_->isVisible())
                && Widget && (Widget == this || isAncestorOf(Widget))) {
                const auto Tab = active();
                auto* Text = Tab && Tab->pane_->ViewId() == "text" ? Tab->pane_->findChild<QTextView*>() : nullptr;
                // Reserve ESC before Qt's window shortcut can close the viewer.
                // Search takes precedence even when focus has returned to the document.
                if (Text && Text->IsFindBarVisible()) {
                    if (Event->type() == QEvent::KeyPress) Text->hideFindBar();
                    Event->accept(); return true;
                }
                if (hosted_ && closeViewerOnEscape_ && Event->type() == QEvent::KeyPress) {
                    escapeViewer(); return true;
                }
            }
        }
        if (Watched == qApp && Event->type() == QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent*>(Event)->propertyName() == "documentSettingsError")
            ShowShellError(qApp->property("documentSettingsError").toString());
        if (clipboardBroker_ && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event); QString Mode;
            if (Key->matches(QKeySequence::Copy)) Mode = "copy"; if (Key->matches(QKeySequence::Cut)) Mode = "cut"; if (Key->matches(QKeySequence::Paste)) Mode = "paste";
            if (!Mode.isEmpty()) { for (const auto& Tab : documents_) if (Tab->pane_->InputWidget()==Watched || (qobject_cast<QWidget*>(Watched)&&Tab->pane_->InputWidget()&&Tab->pane_->InputWidget()->isAncestorOf(qobject_cast<QWidget*>(Watched)))) {clipboard(Tab,Mode);return true;} }
        }
        return QWidget::eventFilter(Watched, Event);
    }


    void DocumentWorkspaceWidget::ShowRecoveryManager()
    {
        if (hotExitClosing_) return;
        auto* Dialog = new QDialog(preferences_ ? static_cast<QWidget*>(preferences_.data()) : this);
        Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setWindowTitle(tr("복구 사본 관리")); Dialog->resize(760, 440);
        auto* Layout = new QVBoxLayout(Dialog); auto* Note = new QLabel(tr("복구 사본을 읽는 중입니다…"), Dialog); Note->setWordWrap(true); Layout->addWidget(Note);
        auto* Table = new QTableWidget(0, 4, Dialog); Table->setObjectName("recoveryCopies");
        Table->setHorizontalHeaderLabels({tr("문서"), tr("보존 시각"), tr("크기"), tr("상태")});
        Table->setSelectionBehavior(QAbstractItemView::SelectRows); Table->setSelectionMode(QAbstractItemView::SingleSelection);
        Table->setEditTriggers(QAbstractItemView::NoEditTriggers); Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); Layout->addWidget(Table);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, Dialog);
        auto* Restore = Buttons->addButton(tr("복구 사본으로 열기"), QDialogButtonBox::ActionRole);
        auto* Delete = Buttons->addButton(tr("사본 삭제…"), QDialogButtonBox::ActionRole);
        Restore->setEnabled(false); Delete->setEnabled(false); Layout->addWidget(Buttons);
        connect(Buttons, &QDialogButtonBox::rejected, Dialog, &QDialog::reject);
        auto Records = std::make_shared<std::vector<RecoverySnapshot>>();
        const auto Owned = [this](const RecoverySnapshot& Record) {
            if (find(QString::fromStdString(Record.document_.id_))) return true;
            if (Record.ownerWindow_.empty()) return false;
            QLockFile Lock(recovery_->SessionLockPath(QString::fromStdString(Record.ownerWindow_))); Lock.setStaleLockTime(0);
            return !Lock.tryLock();
        };
        connect(Table, &QTableWidget::itemSelectionChanged, Dialog, [Table, Restore, Delete, Records, Owned] {
            const int Row = Table->currentRow(); const bool Valid = Row >= 0 && Row < int(Records->size());
            const bool Busy = Valid && Owned((*Records)[Row]); Restore->setEnabled(Valid && !Busy); Delete->setEnabled(Valid && !Busy);
        });
        connect(Restore, &QPushButton::clicked, Dialog, [this, Table, Records, Owned, Dialog] {
            const int Row = Table->currentRow(); if (Row < 0 || Row >= int(Records->size()) || Owned((*Records)[Row])) return;
            std::shared_ptr<QLockFile> Lease;
            const auto Owner = QString::fromStdString((*Records)[Row].ownerWindow_);
            if (!Owner.isEmpty()) {
                Lease = std::make_shared<QLockFile>(recovery_->SessionLockPath(Owner)); Lease->setStaleLockTime(0);
                if (!Lease->tryLock()) return;
            }
            const auto Reply = Handle({{"action", "recover"}, {"path", QString::fromStdString((*Records)[Row].snapshotPath_)}});
            if (!Reply.contains("error") && Lease) manualRecoveryLocks_.push_back(std::move(Lease));
            if (Reply.contains("error")) ShowShellError(Reply["error"].toString()); else Dialog->accept();
        });
        connect(Delete, &QPushButton::clicked, Dialog, [this, Table, Records, Owned, Dialog, Note, Delete, Restore] {
            const int Row = Table->currentRow(); if (Row < 0 || Row >= int(Records->size()) || Owned((*Records)[Row])) return;
            const auto Record = (*Records)[Row];
            if (hc::ui::MessageDialog::question(Dialog, tr("복구 사본 삭제"), tr("이 사본을 삭제하면 저장하지 않은 내용을 되돌릴 수 없습니다. 설정 화면에서 취소해도 삭제는 되돌릴 수 없습니다. 삭제하시겠습니까?"), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) return;
            Delete->setEnabled(false); Restore->setEnabled(false); Table->setEnabled(false);
            auto Store = recovery_; auto* Worker = new QFutureWatcher<storage::Result<storage::Unit>>(Dialog);
            connect(Worker, &QFutureWatcherBase::finished, Dialog, [Worker, Table, Records, Row, Note] {
                const bool Success = Worker->result().HasValue(); Worker->deleteLater(); Table->setEnabled(true);
                if (Success) { Records->erase(Records->begin() + Row); Table->removeRow(Row); Note->setText(tr("선택한 복구 사본을 삭제했습니다.")); }
                else Note->setText(tr("복구 사본을 삭제하지 못했습니다. 다른 창에서 사용 중이거나 저장소를 변경할 수 없습니다."));
            });
            Worker->setFuture(QtConcurrent::run([Store, Record]()->storage::Result<storage::Unit> {
                std::unique_ptr<QLockFile> Lock;
                const auto Owner = QString::fromStdString(Record.ownerWindow_);
                if (!Owner.isEmpty()) { Lock = std::make_unique<QLockFile>(Store->SessionLockPath(Owner)); Lock->setStaleLockTime(0); if (!Lock->tryLock()) return storage::Error{storage::ErrorCategory::Unavailable, "document-recovery-owned"}; }
                auto Removed = Store->RemoveThrough(Record.document_.id_, Record.document_.generation_, Record.document_.revision_);
                if (!Removed.HasValue()) return Removed.Failure();
                if (!Owner.isEmpty()) {
                    auto Session = Store->ReadSession(Owner);
                    if (Session.HasValue()) {
                        auto Json = Session.Value(); QJsonArray Items;
                        for (const auto& Item : Json["documents"].toArray()) if (Item.toObject()["documentId"].toString().toStdString() != Record.document_.id_) Items.append(Item);
                        Json["documents"] = Items; return Store->StoreSession(Owner, Json);
                    }
                }
                return storage::Unit{};
            }));
        });
        auto* Watcher = new QFutureWatcher<storage::Result<std::vector<RecoverySnapshot>>>(Dialog);
        connect(Watcher, &QFutureWatcherBase::finished, Dialog, [Watcher, Records, Table, Note, Owned] {
            auto Result = Watcher->result(); Watcher->deleteLater();
            if (!Result.HasValue()) { Note->setText(tr("복구 사본을 읽지 못했습니다.")); return; }
            *Records = std::move(Result.Value()); Table->setRowCount(int(Records->size()));
            for (int Row = 0; Row < int(Records->size()); ++Row) {
                const auto& Record = (*Records)[Row]; const auto Path = QString::fromStdString(Record.document_.source_.localPath_);
                Table->setItem(Row, 0, new QTableWidgetItem(Path.isEmpty() ? tr("새 문서") : QDir::toNativeSeparators(Path)));
                Table->setItem(Row, 1, new QTableWidgetItem(QDateTime::fromMSecsSinceEpoch(Record.utcMs_).toString("yyyy-MM-dd HH:mm")));
                Table->setItem(Row, 2, new QTableWidgetItem(tr("%1 KiB").arg((Record.bytes_.size() + 1023) / 1024)));
                Table->setItem(Row, 3, new QTableWidgetItem(Owned(Record) ? tr("사용 중") : tr("복구 가능")));
            }
            Note->setText(tr("원본을 덮어쓰지 않고 별도 문서로 복구합니다. 사용 중인 사본은 삭제할 수 없습니다."));
        });
        auto Store = recovery_; Watcher->setFuture(QtConcurrent::run([Store] { return Store->List(); })); Dialog->open();
    }
    bool DocumentWorkspaceWidget::hotExitEnabled() const
    {
        const auto Text = appliedPreferences_["documentViews"].toObject()["textView"].toObject();
        return !hosted_ && Text["hotExitPolicyVersion"].toInt() == 1 && Text["hotExitEnabled"].toBool(true);
    }
    void DocumentWorkspaceWidget::nextEpubRestore()
    {
        if (epubRestoreBusy_ || epubRestoreQueue_.isEmpty() || closingWorkspace_ || hosted_) return;
        epubRestoreBusy_ = true;
        const auto Item = epubRestoreQueue_.takeFirst();
        const auto Root = EpubSourceEditor::RecoveryDirectory();
        // The session names a file inside the draft folder; nothing else is followed.
        const auto Draft = QDir(Root).filePath(QFileInfo(Item["draft"].toString()).fileName());
        const auto Path = Item["path"].toString();
        ++recoveryWorkers_;
        auto* Watcher = new QFutureWatcher<EpubRestoreFacts>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Item, Draft, Path] {
            const auto Facts = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_;
            const auto Next = [this] { epubRestoreBusy_ = false; QTimer::singleShot(0, this, [this] { nextEpubRestore(); }); update(); };
            const auto OpenCurrent = [this, Item, Path] {
                if (!QFileInfo::exists(Path)) return;
                if (Item["mode"].toString() == "edit") OpenForEpubEdit(Path, Item["locator"].toString(), Item["chapter"].toInt(-1));
                else open(Path, false, {}, false);
            };
            if (!Facts.error_.isEmpty()) {
                ShowShellError(tr("EPUB 편집 초안을 읽지 못해 파일만 엽니다: %1 · 초안 파일은 유지합니다.").arg(Facts.error_));
                OpenCurrent(); Next(); return;
            }
            // Unchanged on disk: open as if A were reopened, with the edits applied from the draft (A itself untouched).
            if (!Facts.sourceChanged_) {
                const auto Reply = OpenForEpubEdit(Path, Item["locator"].toString(), Item["chapter"].toInt(-1), {}, -1, Draft);
                if (Reply.contains("error")) ShowShellError(Message(Reply["error"].toString()));
                Next(); return;
            }
            auto* Dialog = new EpubRestoreConflictDialog(Facts, this);
            connect(Dialog, &EpubRestoreConflictDialog::Chosen, this, [this, Draft, OpenCurrent, Next](int Choice) {
                if (Choice == EpubRestoreConflictDialog::OpenEditsAsCopy) {
                    const auto Reply = openEpubRecovery(Draft);
                    if (Reply.contains("error")) ShowShellError(Message(Reply["error"].toString()));
                } else if (Choice == EpubRestoreConflictDialog::OpenCurrent) OpenCurrent();
                else if (Choice == EpubRestoreConflictDialog::DiscardDraft) {
                    (void)QtConcurrent::run([Root = EpubSourceEditor::RecoveryDirectory(), Draft] { epub::DraftStore::Remove(Root, Draft); });
                    OpenCurrent();
                }
                Next();
            });
            Dialog->open();
        });
        Watcher->setFuture(QtConcurrent::run([Root, Draft, Path] { return GatherEpubRestoreFacts(Root, Draft, Path); }));
    }
    bool DocumentWorkspaceWidget::hotExitEpub(const std::shared_ptr<Tab>& Tab) const
    {
        // A real local EPUB: edits live in the EPUB draft store (D9). Copies and provider items keep asking.
        return !hosted_ && Tab->pane_->IsEpub() && !Tab->loading_ && Tab->originalAuthority_ && Tab->temporarySource_.isEmpty()
            && !Tab->hostSlot_ && !Tab->broker_ && Tab->sourceToken_.isEmpty() && QFileInfo(Tab->path_).isAbsolute()
            && (!Tab->Dirty() || (Tab->pane_->IsEpubEditing() && Tab->pane_->EpubDraftEnabled()));
    }
    bool DocumentWorkspaceWidget::hotExitPdf(const std::shared_ptr<Tab>& Tab) const
    {
        // D10: the annotated PDF fits one recovery snapshot (64 MiB); larger ones keep asking.
        return !hosted_ && Tab->pane_->ViewId() == "pdf" && !Tab->loading_ && Tab->originalAuthority_ && Tab->temporarySource_.isEmpty()
            && !Tab->hostSlot_ && !Tab->broker_ && Tab->sourceToken_.isEmpty() && QFileInfo(Tab->path_).isAbsolute()
            && Tab->session_->Text().size() <= DocumentByteLimit && !Tab->pane_->IsBusy();
    }
    bool DocumentWorkspaceWidget::hotExitEligible(const std::shared_ptr<Tab>& Tab) const
    {
        return !hosted_ && !Tab->hostSlot_ && !Tab->broker_ && Tab->sourceToken_.isEmpty()
            && (Tab->originalAuthority_ || !Tab->temporarySource_.isEmpty())
            && Tab->pane_->ViewId() == "text" && !Tab->loading_ && !Tab->navigating_
            && !Tab->pane_->ViewState()["partial"].toBool();
    }
    void DocumentWorkspaceWidget::cancelHotExitClose(const QString& Error)
    {
        if (hotExitProgress_) { hotExitProgress_->reset(); hotExitProgress_->deleteLater(); hotExitProgress_.clear(); }
        ++hotExitCloseEpoch_; hotExitClosing_ = false; closingWorkspace_ = false; hotExitDiscard_.clear();
        menu_->setEnabled(true); toolbar_->setEnabled(true);
        for (const auto& Tab : documents_) Tab->pane_->setEnabled(true);
        if (!Error.isEmpty()) ShowShellError(tr("복구 사본을 보존하지 못했습니다. 창을 닫지 않았습니다. 다시 닫아 재시도하거나 문서를 저장하십시오.\n%1").arg(Error));
        update();
    }
    bool DocumentWorkspaceWidget::requestHotExitClose()
    {
        if (searchPanel_ && searchPanel_->Busy()) { ShowShellError(tr("파일 바꾸기가 진행 중입니다. 완료하거나 중지한 뒤 닫으십시오.")); return false; }
        if (!hotExitClosing_) { ++hotExitCloseEpoch_; QGuiApplication::inputMethod()->commit(); hotExitClosing_ = true; closingWorkspace_ = true; }
        // D9: an edited EPUB writes its final draft and editor workspace first; IsIdle() waits for that write.
        const bool FlushWorkspaces = std::exchange(epubFlushEpoch_, hotExitCloseEpoch_) != hotExitCloseEpoch_;
        for (const auto& Tab : documents_) if (hotExitEpub(Tab) && !hotExitDiscard_.contains(Tab->id_)) {
            if (Tab->Dirty()) Tab->pane_->CheckpointEpub();
            if (FlushWorkspaces) Tab->pane_->FlushEpubWorkspace();
        }
        if (!IsIdle() || hotExitPublishing_) return false;
        for (const auto& Tab : documents_) {
            if (!Tab->Dirty() || hotExitDiscard_.contains(Tab->id_)) continue;
            QByteArray Bytes;
            if (hotExitEligible(Tab) && Tab->pane_->CaptureTextRecovery(Bytes, false)) continue;
            if (hotExitEpub(Tab) && Tab->pane_->EpubDraftCurrent() && !Tab->pane_->EpubDraftPath().isEmpty()) continue;
            if (hotExitPdf(Tab)) continue;
            const QScopedValueRollback Guard(closePromptActive_, true);
            const int Choice = closePrompt_ ? closePrompt_(Tab->id_) : int(hc::ui::MessageDialog::question(this,
                tr("변경 내용 저장"), tr("이 문서는 HotExit로 보존할 수 없습니다. 변경 내용을 저장하시겠습니까?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel));
            if (Choice == QMessageBox::Discard) { hotExitDiscard_.append(Tab->id_); continue; }
            if (Choice == QMessageBox::Save) {
                activate(Tab); findChild<QAction*>(Tab->originalSave_ ? "saveDocument" : "saveDocumentAs")->trigger();
                if (!Tab->saving_ && !Tab->choosingSavePath_ && !Tab->composingSave_) cancelHotExitClose();
                return false;
            }
            cancelHotExitClose(); return false;
        }
        menu_->setEnabled(false); toolbar_->setEnabled(false);
        for (const auto& Tab : documents_) Tab->pane_->setEnabled(false);
        if (!hotExitProgress_) {
            hotExitProgress_ = new QProgressDialog(tr("변경 내용을 보존한 뒤 창을 닫습니다."), tr("종료 취소"), 0, 0, this);
            hotExitProgress_->setWindowTitle(tr("편집 내용 보존")); hotExitProgress_->setWindowModality(Qt::WindowModal);
            hotExitProgress_->setMinimumDuration(500); hotExitProgress_->setAutoClose(false); hotExitProgress_->setValue(0);
            connect(hotExitProgress_, &QProgressDialog::canceled, this, [this] { cancelHotExitClose(); });
        }
        publishHotExitSession(true); update(); return false;
    }
    void DocumentWorkspaceWidget::publishHotExitSession(bool Closing)
    {
        if (hotExitPublishing_ || !IsIdle() || (!Closing && !hotExitEnabled())) return;
        if (!hotExitLock_) { if (Closing) cancelHotExitClose(tr("복구 폴더 잠금을 얻지 못했습니다.")); return; }
        const bool Periodic = PreferenceValue(appliedPreferences_, "textView/recoveryEnabled").toBool();
        if (!Closing && !Periodic && restoredOriginals_.empty()) return;
        if (std::any_of(documents_.begin(), documents_.end(), [](const auto& Tab) { return Tab->hotExitRestore_ && (!Tab->everReady_ || !Tab->error_.isEmpty()); })) {
            if (Closing) cancelHotExitClose(tr("복원에 실패한 문서를 먼저 확인하십시오. 기존 사본은 유지합니다.")); return;
        }
        QJsonArray Items; std::vector<RecoverySnapshot> Captures;
        const bool RestoreClean = PreferenceValue(appliedPreferences_, "textView/restoreCleanDocuments").toBool();
        for (const auto& Tab : documents_) {
            if (hotExitDiscard_.contains(Tab->id_)) continue;
            // D9: the EPUB itself plus its draft; the draft is the preserved edit, the original stays untouched.
            if (hotExitEpub(Tab)) {
                if (!Tab->Dirty() && !RestoreClean) continue;
                if (Tab->Dirty() && (!Tab->pane_->EpubDraftCurrent() || Tab->pane_->EpubDraftPath().isEmpty())) {
                    if (Closing) { cancelHotExitClose(tr("EPUB 편집 초안을 아직 쓰지 못했습니다. 잠시 후 다시 닫으십시오.")); return; }
                    continue;
                }
                const auto Reader = Tab->pane_->ViewState()["epub"].toObject();
                QJsonObject Item{{"documentId", Tab->id_}, {"path", Tab->path_}, {"kind", "epub"}, {"mode", Tab->pane_->IsEpubEditing() ? "edit" : "read"},
                    {"locator", Reader["locator"].toString()}, {"chapter", Reader["chapterIndex"].toInt(-1)}};
                if (Tab->Dirty()) Item["draft"] = QFileInfo(Tab->pane_->EpubDraftPath()).fileName();
                Items.append(Item); continue;
            }
            // D10: the annotated PDF as one snapshot, reattached on restore when the original is unchanged.
            if (hotExitPdf(Tab)) {
                QJsonObject Item{{"documentId", Tab->id_}, {"path", Tab->path_}, {"kind", "pdf"}, {"position", Tab->pane_->PdfPosition()}};
                if (Tab->Dirty()) {
                    Item["baselineSha256"] = QString::fromLatin1(Tab->input_.hash_.toHex());
                    if (Closing) {
                        Tab->session_->AdvanceRecoveryRevision();
                        RecoverySnapshot Capture; Capture.document_ = Tab->session_->Snapshot(); Capture.viewId_ = "pdf";
                        Capture.bytes_ = Tab->session_->Text(); Capture.ownerWindow_ = workspaceId_.toStdString();
                        Captures.push_back(std::move(Capture));
                    } else {
                        const auto& Receipt = Tab->checkpointReceipt_;
                        if (!Receipt || Receipt->viewId_ != "pdf" || Receipt->document_.generation_ != Tab->session_->Snapshot().generation_
                            || Receipt->document_.revision_ != Tab->Revision()) return;
                        Item["snapshot"] = QFileInfo(QString::fromStdString(Receipt->snapshotPath_)).fileName();
                        Item["sha256"] = QString::fromStdString(Receipt->checksum_);
                        Item["generation"] = QString::number(Receipt->document_.generation_);
                        Item["revision"] = QString::number(Receipt->document_.revision_);
                    }
                } else if (!RestoreClean) continue;
                Items.append(Item); continue;
            }
            if (!hotExitEligible(Tab)) continue;
            QByteArray Bytes; auto Text = Tab->pane_->CaptureTextRecovery(Bytes, Closing && Tab->Dirty());
            if (!Text) { if (Closing && Tab->Dirty()) { cancelHotExitClose(tr("완전한 텍스트를 읽을 수 없습니다.")); return; } continue; }
            QJsonObject Item{{"documentId", Tab->id_}, {"path", Tab->temporarySource_.isEmpty() ? Tab->path_ : QString{}}};
            if (Tab->Dirty()) {
                if (Bytes.size() > DocumentByteLimit) { if (Closing) cancelHotExitClose(tr("복구 사본 크기 제한을 초과했습니다.")); return; }
                if (Closing) {
                    // Caret/encoding are part of the final capture even without another content edit.
                    Tab->session_->AdvanceRecoveryRevision();
                    RecoverySnapshot Capture; Capture.document_ = Tab->session_->Snapshot(); Capture.text_ = Text;
                    Capture.bytes_ = Bytes.toStdString(); Capture.ownerWindow_ = workspaceId_.toStdString();
                    Capture.text_->untitled_ = !Tab->temporarySource_.isEmpty(); Capture.text_->untitledNumber_ = Tab->untitledNumber_;
                    Capture.text_->baselineSha256_ = Capture.text_->untitled_ ? std::string{} : Tab->input_.hash_.toHex().toStdString();
                    if (Capture.text_->untitled_) Capture.document_.source_.localPath_.clear();
                    Captures.push_back(std::move(Capture));
                } else {
                    const auto& Receipt = Tab->checkpointReceipt_;
                    if (!Receipt || !Receipt->text_ || Receipt->document_.generation_ != Tab->session_->Snapshot().generation_
                        || Receipt->document_.revision_ != Tab->Revision()) return;
                    Item["snapshot"] = QFileInfo(QString::fromStdString(Receipt->snapshotPath_)).fileName();
                    Item["sha256"] = QString::fromStdString(Receipt->checksum_);
                    Item["generation"] = QString::number(Receipt->document_.generation_);
                    Item["revision"] = QString::number(Receipt->document_.revision_);
                }
            } else if (!PreferenceValue(appliedPreferences_, "textView/restoreCleanDocuments").toBool() || !Tab->temporarySource_.isEmpty()) continue;
            Item["position"] = QJsonObject{{"anchor", double(Text->anchor_)}, {"caret", double(Text->caret_)}, {"firstLine", double(Text->firstLine_)},
                {"firstSubLine", double(Text->firstSubLine_)}, {"xOffset", double(Text->xOffset_)}};
            Items.append(Item);
        }
        QJsonObject Manifest{{"profileId", qApp->property("documentRecoveryProfile").toString()}, {"documents", Items}, {"activeDocumentId", active() ? active()->id_ : QString{}},
            {"geometry", QString::fromLatin1(saveGeometry().toBase64())}, {"cleanShutdown", Closing}};
        if (!Closing && Manifest == publishedSession_) return;
        hotExitPublishing_ = true; ++recoveryWorkers_;
        using Published = std::pair<QJsonObject, std::vector<RecoverySnapshot>>;
        auto* Watcher = new QFutureWatcher<storage::Result<Published>>(this);
        const auto CloseEpoch = hotExitCloseEpoch_;
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Closing, CloseEpoch] {
            auto Result = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_; hotExitPublishing_ = false;
            if (Closing && CloseEpoch != hotExitCloseEpoch_) return;
            if (!Result.HasValue()) { if (Closing) cancelHotExitClose(QString::fromStdString(Result.Failure().code_)); else ShowShellError(tr("복구 세션 보존 실패: %1").arg(QString::fromStdString(Result.Failure().code_))); return; }
            publishedSession_ = Result.Value().first;
            for (const auto& Receipt : Result.Value().second) if (auto Tab = find(QString::fromStdString(Receipt.document_.id_))) Tab->checkpointReceipt_ = Receipt;
            // The replacement manifest is durable before retiring the imported session's snapshots.
            if (!restoredOriginals_.empty() && !restorationIncomplete_) {
                restoringSessionId_.clear();
                auto Originals = std::exchange(restoredOriginals_, {}); auto Store = recovery_;
                auto* Cleanup = new QFutureWatcher<storage::Result<storage::Unit>>(this); ++recoveryWorkers_;
                connect(Cleanup, &QFutureWatcherBase::finished, this, [this, Cleanup] {
                    if (!Cleanup->result().HasValue()) ShowShellError(tr("이전 복구 사본을 정리하지 못했습니다."));
                    Cleanup->deleteLater(); --recoveryWorkers_; tryFinishClose();
                });
                Cleanup->setFuture(QtConcurrent::run([Store, Originals, Lease = std::exchange(restoringLock_, {})]()->storage::Result<storage::Unit> {
                    for (const auto& Original : Originals) {
                        auto Removed = Store->RemoveThrough(Original.document_.id_, Original.document_.generation_, Original.document_.revision_);
                        if (!Removed.HasValue()) return Removed.Failure();
                    }
                    return storage::Unit{};
                }));
            }
            if (Closing) {
                // EPUB edits are preserved by their draft, not by a session snapshot.
                for (const auto& Tab : documents_) if (Tab->Dirty() && !hotExitDiscard_.contains(Tab->id_) && !Tab->pane_->IsEpub()) {
                    if (!Tab->checkpointReceipt_ || !Tab->session_->AuthorizeRecoveryClose(*Tab->checkpointReceipt_)) { cancelHotExitClose(tr("보존한 문서 버전이 현재 편집 내용과 다릅니다.")); return; }
                }
                if (hotExitProgress_) { hotExitProgress_->reset(); hotExitProgress_->deleteLater(); hotExitProgress_.clear(); }
                hotExitReady_ = true;
                const auto Tabs = documents_;
                for (const auto& Tab : Tabs) retire(Tab, true, !hotExitDiscard_.contains(Tab->id_));
                hotExitClosing_ = false;
                update(); tryFinishClose();
            }
        });
        auto Store = recovery_; const auto Id = workspaceId_;
        const auto RetireSession = restorationIncomplete_ ? QString{} : restoringSessionId_;
        Watcher->setFuture(QtConcurrent::run([Store, Id, RetireSession, Manifest, Captures = std::move(Captures)]() mutable ->storage::Result<Published> {
            std::vector<RecoverySnapshot> Receipts; auto Entries = Manifest["documents"].toArray();
            for (auto& Capture : Captures) {
                auto Result = Store->Store(std::move(Capture)); if (!Result.HasValue()) return Result.Failure();
                auto Receipt = std::move(Result.Value());
                for (qsizetype At = 0; At < Entries.size(); ++At) {
                    auto Item = Entries[At].toObject(); if (Item["documentId"].toString().toStdString() != Receipt.document_.id_) continue;
                    Item["snapshot"] = QFileInfo(QString::fromStdString(Receipt.snapshotPath_)).fileName(); Item["sha256"] = QString::fromStdString(Receipt.checksum_);
                    Item["generation"] = QString::number(Receipt.document_.generation_); Item["revision"] = QString::number(Receipt.document_.revision_); Entries[At] = Item;
                }
                Receipts.push_back(std::move(Receipt));
            }
            Manifest["documents"] = Entries; auto Saved = Store->StoreSession(Id, Manifest);
            if (!Saved.HasValue()) return Saved.Failure();
            if (!RetireSession.isEmpty() && !Store->RemoveSession(RetireSession)) return storage::Error{storage::ErrorCategory::Unavailable, "document-hotexit-session-retire"};
            return Published{Manifest, std::move(Receipts)};
        }));
    }
    bool DocumentWorkspaceWidget::RestoreHotExit(const QString& SessionId)
    {
        if (hosted_ || !documents_.empty() || !hotExitEnabled()) return false;
        auto Lock = std::make_shared<QLockFile>(recovery_->SessionLockPath(SessionId)); Lock->setStaleLockTime(0);
        if (recovery_->SessionLockPath(SessionId).isEmpty() || !Lock->tryLock()) return false;
        restoringLock_ = std::move(Lock); restoringSessionId_ = SessionId;
        auto Store = recovery_; ++recoveryWorkers_;
        auto* Watcher = new QFutureWatcher<storage::Result<QJsonObject>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher] {
            auto Result = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_;
            if (!Result.HasValue()) { restoringLock_.reset(); restoringSessionId_.clear(); ShowShellError(tr("복구 세션을 읽지 못했습니다: %1").arg(QString::fromStdString(Result.Failure().code_))); return; }
            const auto Manifest = Result.Value();
            if (!Manifest["cleanShutdown"].toBool() && hc::ui::MessageDialog::question(this, tr("편집 내용 복구"),
                tr("이전 편집이 정상적으로 종료되지 않았습니다. 보존된 내용을 복구하시겠습니까?"), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) { restoringLock_.reset(); restoringSessionId_.clear(); return; }
            restoredActive_ = Manifest["activeDocumentId"].toString(); pendingHotExitRestore_ = true;
            for (const auto& Value : Manifest["documents"].toArray()) {
                const auto Item = Value.toObject(); QJsonObject Reply; const auto Kind = Item["kind"].toString();
                if (Kind == "epub") {
                    // D9: an EPUB with a draft is compared with the file first (nextEpubRestore); others reopen in their mode.
                    if (!Item["draft"].toString().isEmpty()) { epubRestoreQueue_.append(Item); continue; }
                    if (Item["mode"].toString() == "edit") Reply = OpenForEpubEdit(Item["path"].toString(), Item["locator"].toString(), Item["chapter"].toInt(-1));
                    else {
                        Reply = open(Item["path"].toString(), false, {}, false);
                        if (auto Tab = find(Reply["documentId"].toString())) Tab->restorePosition_ = QJsonObject{{"locator", Item["locator"].toString()}};
                    }
                } else if (Kind == "pdf") {
                    // D10: the snapshot reattaches to the original only while the original keeps its SHA-256.
                    if (!Item["snapshot"].toString().isEmpty()) {
                        pendingRecoveryBaseline_ = Item["baselineSha256"].toString();
                        Reply = Handle({{"action", "recover"}, {"path", recoveryRoot_ + "/" + Item["snapshot"].toString()}});
                        pendingRecoveryBaseline_.clear();
                    } else if (!Item["path"].toString().isEmpty()) Reply = open(Item["path"].toString(), false, {}, false);
                    if (auto Tab = find(Reply["documentId"].toString())) Tab->restorePosition_ = Item["position"].toObject();
                } else {
                    if (!Item["snapshot"].toString().isEmpty()) Reply = Handle({{"action", "recover"}, {"path", recoveryRoot_ + "/" + Item["snapshot"].toString()}});
                    else if (!Item["path"].toString().isEmpty()) Reply = open(Item["path"].toString(), false, {}, false);
                    if (Item["snapshot"].toString().isEmpty()) if (auto Tab = find(Reply["documentId"].toString())) {
                        const auto Position = Item["position"].toObject(); TextRecoveryState State;
                        State.anchor_ = qMax(0, Position["anchor"].toInt()); State.caret_ = qMax(0, Position["caret"].toInt());
                        State.firstLine_ = qMax(0, Position["firstLine"].toInt()); State.xOffset_ = qMax(0, Position["xOffset"].toInt());
                        State.firstSubLine_ = qMax(0, Position["firstSubLine"].toInt());
                        Tab->cleanRestoreState_ = State;
                    }
                }
                restoredIds_[Item["documentId"].toString()] = Reply["documentId"].toString();
                if (Reply.contains("error")) { restorationIncomplete_ = true; ShowShellError(tr("일부 문서를 복원하지 못했습니다. 복구 사본은 유지합니다.")); }
            }
            pendingHotExitRestore_ = false;
            if (!epubRestoreQueue_.isEmpty()) QTimer::singleShot(0, this, [this] { nextEpubRestore(); });
            const auto Geometry = QByteArray::fromBase64(Manifest["geometry"].toString().toLatin1());
            if (!Geometry.isEmpty()) { restoreGeometry(Geometry); if (screen() && !screen()->availableGeometry().intersects(frameGeometry())) move(screen()->availableGeometry().topLeft()); }
        });
        Watcher->setFuture(QtConcurrent::run([Store, SessionId]()->storage::Result<QJsonObject> {
            auto Session = Store->ReadSession(SessionId); if (!Session.HasValue()) return Session.Failure();
            auto Latest = Store->List(); if (!Latest.HasValue()) return Latest.Failure();
            auto Json = Session.Value(); auto Items = Json["documents"].toArray();
            for (qsizetype At = 0; At < Items.size(); ++At) {
                auto Item = Items[At].toObject(); if (Item["snapshot"].toString().isEmpty()) continue;
                for (const auto& Record : Latest.Value()) if (Record.document_.id_ == Item["documentId"].toString().toStdString() && Record.ownerWindow_ == SessionId.toStdString()) {
                    Item["snapshot"] = QFileInfo(QString::fromStdString(Record.snapshotPath_)).fileName(); Item["sha256"] = QString::fromStdString(Record.checksum_);
                    Item["generation"] = QString::number(Record.document_.generation_); Item["revision"] = QString::number(Record.document_.revision_); break;
                }
                Items[At] = Item;
            }
            QJsonArray Remaining;
            for (const auto& Value : Items) {
                const auto Item = Value.toObject(); DocumentSnapshot Stamp; Stamp.id_ = Item["documentId"].toString().toStdString();
                Stamp.generation_ = Item["generation"].toString().toULongLong(); Stamp.revision_ = Item["revision"].toString().toULongLong();
                if (Item["snapshot"].toString().isEmpty() || !Store->IsRetired(Stamp)) Remaining.append(Item);
            }
            Json["documents"] = Remaining; return Json;
        }));
        return true;
    }
    bool DocumentWorkspaceWidget::RequestClose()
    {
        if (hotExitReady_) {
            const auto Tabs = documents_; for (const auto& Tab : Tabs) retire(Tab, true, !hotExitDiscard_.contains(Tab->id_));
            return documents_.empty() && IsIdle();
        }
        if (hotExitClosing_ || (!hosted_ && hotExitEnabled() && !documents_.empty())) return requestHotExitClose();
        if (searchPanel_ && searchPanel_->Busy()) { ShowShellError(tr("파일 바꾸기가 진행 중입니다. 완료하거나 중지한 뒤 닫으십시오.")); return false; }
        closingWorkspace_ = true;
        while (!documents_.empty()) {
            const auto Count = documents_.size(); closeTab(int(Count - 1));
            if (documents_.size() == Count) return false;
        }
        if (!IsIdle()) return false;
        if (!hosted_ && !recovery_->RemoveSession(workspaceId_)) { ShowShellError(tr("종료한 세션을 정리하지 못했습니다.")); return false; }
        return true;
    }
    bool DocumentWorkspaceWidget::RequestViewerClose()
    {
        if (!hosted_ || !RequestClose()) return false;
        finishWindowClose();
        return true;
    }
    void DocumentWorkspaceWidget::tryFinishClose()
    {
        if(pendingActionsScheduled_)return;
        if(!closingWorkspace_&&std::none_of(documents_.begin(),documents_.end(),[](const auto& Tab){return Tab->closeRequested_||Tab->reloadAfterClose_||!Tab->navigationAfterSave_.isEmpty();}))return;
        pendingActionsScheduled_=true;
        QTimer::singleShot(0,this,[this]{
            pendingActionsScheduled_=false;
            if(closingWorkspace_){if(RequestClose())finishWindowClose();return;}
            const auto Pending=documents_;
            for(const auto& Tab:Pending){
                if(Tab->closed_)continue;
                if(Tab->closeRequested_){const auto At=std::find(documents_.begin(),documents_.end(),Tab);if(At!=documents_.end())closeTab(int(std::distance(documents_.begin(),At)));}
                else if(Tab->reloadAfterClose_&&Tab->pane_->ViewState()["closed"].toBool())load(Tab,true);
                else if(!Tab->navigationAfterSave_.isEmpty()&&!Tab->saving_&&Tab->pendingToken_.isEmpty()&&!Tab->checkpointing_){
                    const auto State=Tab->session_->Snapshot();
                    if(State.saveState_==DocumentSaveState::Failed||State.saveState_==DocumentSaveState::Conflict)Tab->navigationAfterSave_.clear();
                    else if(!Tab->Dirty())navigateImage(Tab,std::exchange(Tab->navigationAfterSave_,{}));
                }
            }
        });
    }
    void DocumentWorkspaceWidget::toggleEpubFullscreen() {
        auto* Root=window();
        if(epubFullscreen_){epubFullscreen_=false;Root->showNormal();if(epubWasMaximized_)Root->showMaximized();else Root->setGeometry(epubWindowGeometry_);}
        else {const auto Tab=active();if(!Tab || !Tab->pane_->IsEpub())return;epubWasMaximized_=Root->isMaximized();epubWindowGeometry_=Root->geometry();epubFullscreen_=true;Root->showFullScreen();}
        if(auto* Chrome=findChild<QWidget*>("documentWindowChrome",Qt::FindDirectChildrenOnly))Chrome->setVisible(!epubFullscreen_);
        if(auto Tab=active())Tab->pane_->Focus();
    }
    void DocumentWorkspaceWidget::escapeViewer()
    {
        if (!hosted_ || QApplication::activeModalWidget() || QApplication::activePopupWidget()
            || (preferences_ && preferences_->isVisible())) return;
        // The Escape setting must be current, but an unchanged settings file is not read again.
        if (settingsChanged()) { hc::document::startup::Scope Trace("editor.preferences"); refreshPreferences(); }
        if (closeViewerOnEscape_ && RequestClose()) finishWindowClose();
    }
    void DocumentWorkspaceWidget::finishWindowClose()
    {
        if (!hosted_) { close(); return; }
        // Keep the IPC endpoint alive until Commander observes the accepted
        // close. It owns the outer window and must restore keyboard focus.
        viewerCloseRequested_ = true;
        closingWorkspace_ = false;
        emit StateChanged();
    }
}

namespace hc::document {
QJsonArray DocumentWorkspaceWidget::searchSources() const {
    QJsonArray Sources;
    for (const auto& Tab : documents_) {
        if (Tab->loading_ || Tab->closed_ || (Tab->pane_->ViewId() != "text" && !Tab->pane_->IsMarkdown())) continue;
        Sources.append(QJsonObject{{"path", Tab->path_}, {"document", Tab->id_}, {"text", Tab->pane_->SearchText()},
            {"revision", QString::number(Tab->Revision())}, {"encoding", Tab->input_.encoding_}, {"bom", Tab->input_.bom_}, {"readOnly", Tab->pane_->IsReadOnly()},
            // Untitled and recovered documents live in the recovery folder: results show their name, not that path.
            {"displayName", Tab->temporarySource_.isEmpty() ? QString{} : documentName(*Tab)}});
    }
    return Sources;
}
void DocumentWorkspaceWidget::openSearch(bool Replace) {
    auto Tab = active(); if (!Tab) Tab = find(searchDocument_);
    if (!Tab || Tab->loading_) return;
    if (Tab->pane_->ViewId() != "text" && !Tab->pane_->IsMarkdown() && !Tab->pane_->IsEpub()) return;
    if(Replace && !hosted_ && Tab->pane_->IsEpub() && !Tab->pane_->EpubEditor())Tab->pane_->Execute({{"action","epub.edit.begin"}});
    const bool ChangedBook=Tab->pane_->IsEpub() && searchDocument_!=Tab->id_;searchDocument_ = Tab->id_;
    if (!searchPanel_) {
        auto* Layout = qobject_cast<QVBoxLayout*>(layout()); auto* Content = tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_;
        const auto Position = Layout->indexOf(Content); Layout->removeWidget(Content);
        searchHost_ = new QMainWindow(this,Qt::Widget); searchHost_->setWindowFlags(Qt::Widget); searchHost_->setObjectName("documentSearchHost"); searchHost_->setDockOptions({}); searchHost_->setCentralWidget(Content); Layout->insertWidget(Position, searchHost_, 1);searchHost_->show();Content->show();
        searchDock_ = new QDockWidget(tr("파일에서 찾기 / 바꾸기"), searchHost_); searchDock_->setObjectName("documentSearchDock"); searchDock_->setAllowedAreas(Qt::BottomDockWidgetArea); searchHost_->addDockWidget(Qt::BottomDockWidgetArea, searchDock_);
        searchPanel_ = new search::Panel(searchDock_); searchDock_->setWidget(searchPanel_); shortcutRouter_->AddScope(searchDock_);
        searchPanel_->SetSources([this] { return searchSources(); });
        searchPanel_->SetBookSources([this] { const auto Target = find(searchDocument_); return Target ? Target->pane_->CaptureBookSearchSources(Target->id_) : std::function<QJsonArray()>{}; });
        connect(searchPanel_, &search::Panel::Navigate, this, &DocumentWorkspaceWidget::navigateSearch);
        connect(searchPanel_, &search::Panel::EditorRequested, this, [this](const QJsonObject& Context) {
            const auto Target=find(searchDocument_); if(!hosted_||!Target||Target->loading_||!editRequest_.isEmpty())return;
            editRequest_={{"token",QUuid::createUuid().toString(QUuid::Id128)},{"documentId",Target->id_},
                {"generation",QString::number(Target->session_->Snapshot().generation_)},{"format","search"},{"search",Context}};update();
        });
        connect(searchPanel_, &search::Panel::ApplyBuffer, this, [this](quint64 Ticket, const QJsonObject& Source, const QString& Expected, const QString& Replacement) {
            auto Target = find(Source["document"].toString()); QString Error;
            if (!Target || Target->loading_ || Target->saving_ || Target->pane_->IsReadOnly()) Error = tr("문서가 닫혔거나 지금 편집할 수 없습니다.");
            else if (Source.contains("bookBatch")) {
                auto* Editor = Target->pane_->EpubEditor();
                if (!Editor || !Editor->IsReady()) Error = tr("책의 편집 모드를 연 뒤 다시 검색하여 바꾸십시오.");
                else Error = Editor->ApplySearchBatch(Source["bookBatch"].toArray());
            }
            else if (Source["undo"].toBool() && Target->pane_->SearchText() == Replacement) { /* already undone */ }
            else if (!Target->pane_->ApplySearchText(Expected, Replacement)) Error = tr("문서가 검색/치환 후 변경되었습니다.");
            searchPanel_->FinishBuffer(Ticket, Error);
        });
        connect(searchPanel_, &search::Panel::PlacementRequested, this, &DocumentWorkspaceWidget::placeSearch);
        connect(searchPanel_, &search::Panel::SummaryChanged, this, [this](const QString& Summary) {
            if (auto* Editor = qobject_cast<EpubSourceEditor*>(searchEpubHost_.data())) Editor->SetSearchPanelTitle(Summary);
        });
        connect(searchPanel_, &search::Panel::CloseRequested, this, [this] {
            const auto Index = tabs_->indexOf(searchPanel_); if (Index >= 0) tabs_->removeTab(Index);
            searchDock_->hide(); searchPanel_->hide();
        });
    }
    const auto Location = appliedPreferences_["documentViews"].toObject()["textView"].toObject()["findInFiles"].toObject()["openLocation"].toString("bottom");
    searchPanel_->setProperty("configuredLocation", Location);
    // While a book is edited the results dock into its problems panel (plan 8.7).
    if (Tab->pane_->IsEpubEditing() && Tab->pane_->EpubEditor()) placeSearch(QStringLiteral("epubProblems"));
    else if (searchPlacement_.isEmpty() || searchPlacement_ == QLatin1String("epubProblems") || !searchPanel_->isVisible()) placeSearch(Location);
    searchPanel_->SetReadOnly(hosted_);
    searchPanel_->Open(Replace, Tab->pane_->IsEpub() ? Tab->path_ : QFileInfo(Tab->path_).absolutePath(), Tab->pane_->IsEpub(), searchSources());
    if(ChangedBook)searchPanel_->SourcesChanged();
}
void DocumentWorkspaceWidget::placeSearch(const QString& Requested) {
    if (!searchPanel_) return;
    auto Location = Requested;
    const auto Configured = searchPanel_->property("configuredLocation").toString().isEmpty() ? QStringLiteral("bottom") : searchPanel_->property("configuredLocation").toString();
    if (auto* Host = qobject_cast<EpubSourceEditor*>(searchEpubHost_.data())) Host->DetachSearchPanel();
    searchEpubHost_.clear(); searchPanel_->SetEmbedded(false);
    if (Location == QLatin1String("epubProblems")) {
        auto Tab = active(); auto* Editor = Tab && Tab->pane_->IsEpubEditing() ? Tab->pane_->EpubEditor() : nullptr;
        if (Editor) {
            searchPlacement_ = Location;
            const auto Index = tabs_->indexOf(searchPanel_); if (Index >= 0) tabs_->removeTab(Index);
            if (searchDock_->widget() == searchPanel_) searchDock_->setWidget(nullptr);
            searchDock_->hide();
            Editor->AttachSearchPanel(searchPanel_); searchPanel_->SetEmbedded(true); Editor->SetSearchPanelTitle(searchPanel_->Summary());
            searchEpubHost_ = Editor;
            if (!Editor->property("searchReleaseConnected").toBool()) {
                Editor->setProperty("searchReleaseConnected", true);
                // The editor hands the panel back before it is destroyed; the panel returns to its configured place.
                connect(Editor, &EpubSourceEditor::SearchPanelReleased, this, [this](QWidget*) {
                    searchEpubHost_.clear(); searchPanel_->SetEmbedded(false);
                    placeSearch(searchPanel_->property("configuredLocation").toString().isEmpty() ? QStringLiteral("bottom") : searchPanel_->property("configuredLocation").toString());
                });
            }
            searchPanel_->show(); return;
        }
        Location = Configured;
    }
    searchPlacement_ = Location;
    const auto Index = tabs_->indexOf(searchPanel_); if (Index >= 0) tabs_->removeTab(Index);
    if(searchDock_->widget()==searchPanel_)searchDock_->setWidget(nullptr);
    searchPanel_->setParent(this);
    if (Location == "tab") {
        searchDock_->hide();
        if (!tabbed_ && tabs_->indexOf(singlePane_) < 0) {
            searchHost_->takeCentralWidget(); tabs_->addTab(singlePane_, tr("문서")); searchHost_->setCentralWidget(tabs_); tabs_->show(); singlePane_->show();
        }
        tabs_->addTab(searchPanel_, tr("파일에서 찾기 / 바꾸기")); tabs_->setCurrentWidget(searchPanel_);
    } else {
        if (!tabbed_ && tabs_->indexOf(singlePane_) >= 0) { tabs_->removeTab(tabs_->indexOf(singlePane_)); searchHost_->takeCentralWidget(); tabs_->setParent(this); tabs_->hide(); searchHost_->setCentralWidget(singlePane_); singlePane_->show(); }
        searchDock_->setWidget(searchPanel_); searchDock_->setFloating(false);searchHost_->addDockWidget(Qt::BottomDockWidgetArea,searchDock_);searchDock_->setFloating(Location == "floating"); searchDock_->show();
        if (Location == "floating") searchDock_->resize(1000, 600);
        else searchHost_->resizeDocks({searchDock_}, {qBound(360,height()*55/100,560)}, Qt::Vertical);
    }
    searchPanel_->show();
}
void DocumentWorkspaceWidget::OpenSearchContext(const QJsonObject& Context) {
    openSearch(true); if(searchPanel_)searchPanel_->RestoreContext(Context);
}
void DocumentWorkspaceWidget::syncSearchHost() {
    // The docked panel follows its book: leaving edit mode returns it to the configured place.
    if (!searchPanel_ || searchPlacement_ != QLatin1String("epubProblems")) return;
    bool Live = false;
    for (const auto& Each : documents_) if (Each->pane_ && Each->pane_->EpubEditor() == searchEpubHost_.data() && Each->pane_->IsEpubEditing()) Live = true;
    if (!Live) placeSearch(searchPanel_->property("configuredLocation").toString().isEmpty() ? QStringLiteral("bottom") : searchPanel_->property("configuredLocation").toString());
}
void DocumentWorkspaceWidget::navigateSearch(const QJsonObject& Row) {
    auto Tab = find(Row["document"].toString());
    if (!Tab) { const auto Reply = open(Row["path"].toString(), false, {}, false); Tab = find(Reply["documentId"].toString()); }
    if (!Tab) return; activate(Tab);
    if(!hosted_ && !Row["resource"].toString().isEmpty() && !Tab->pane_->EpubEditor())Tab->pane_->Execute({{"action","epub.edit.begin"}});
    auto* Wait = new QTimer(this); Wait->setInterval(30); auto Attempts = std::make_shared<int>(0);
    connect(Wait, &QTimer::timeout, this, [this, Wait, Id = Tab->id_, Row, Attempts] {
        auto Current = find(Id); if (!Current || ++*Attempts > 1000) { Wait->deleteLater(); return; }
        if (Current->loading_) return;
        if (!Row["resource"].toString().isEmpty()) {
            auto* Editor = Current->pane_->EpubEditor();
            if(Editor && !Editor->IsReady())return;
            if (Editor && Editor->IsReady()) {
                Editor->SelectSearchRange(Row["resource"].toString(), Row["start"].toInteger(), Row["length"].toInteger());
                // 미리보기에서 보기 (result context menu): the same command as the source context menu.
                if (Row["preview"].toBool()) if (auto* Locate = Editor->findChild<QAction*>("epubLocateSource")) Locate->trigger();
            }
            else if(Row["chapter"].toInt(-1)>=0)Current->pane_->Execute({{"action","epub.reader.chapter"},{"value",Row["chapter"]}});
        } else Current->pane_->SelectSearchRange(Row["start"].toInteger(), Row["length"].toInteger()); Wait->deleteLater();
    }); Wait->start();
}
}
