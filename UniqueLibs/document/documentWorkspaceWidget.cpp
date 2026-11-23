#include "stdafx.h"
#include "documentWorkspaceWidget.hpp"
#include "startupTrace.hpp"
#include "hwpStartupTrace.hpp"
#include "documentWindowSize.hpp"
#include "UniqueLibs/errorPresentation.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "imageAsync.hpp"
#include "documentFocusRestore.hpp"
#include "UniqueLibs/document/sqlite/sqliteFormat.hpp"
#include "documentProgressOverlay.hpp"
#include "recentDocuments.hpp"
#include "recentDocumentsDialog.hpp"
#include "documentStartPage.hpp"
#include "documentRecentMenu.hpp"
#include "newDocumentChooser.hpp"
#include "newEpubDialog.hpp"
#include "newSqliteDialog.hpp"
#include "UniqueLibs/document/sqlite/sqliteKeyStore.hpp"
#include "epubRestoreConflictDialog.hpp"
#include "documentRecoveryMaintenance.hpp"
#include "recoveryManagerDialog.hpp"
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
#include "documentToolPanel.hpp"
#include "search/documentSearchPanel.hpp"
#include <fmdock/DockManager.h>
#include "imageFullscreen.hpp"
#include "imageReadingView.hpp"
#include "documentThemeAdapter.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "documentSettingsDialog.hpp"
#include <fmwidgets/Menu.h>
#include <fmwidgets/ToolTip.h>
#include <fmwidgets/WindowChrome.h>
#include "documentShortcutRouter.hpp"
#include "epubProcessView.hpp"
#include "epubSourceEditor.hpp"
#include "markdownDocumentView.hpp"
#include "rstDocumentView.hpp"
#include "rstProjectDialogs.hpp"
#include "explorerPanel.hpp"
#include "explorerFileOps.hpp"
#include "workspace/workspacePaths.hpp"
#include "rst/sphinxScanner.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include <fmwidgets/Banner.h>
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
#include <QTemporaryFile>
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
void PrepareDocumentTextRuntime(QWidget* Parent);

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
            const auto TextFilters=TextSuffix=="txt"?QObject::tr("텍스트 문서 (*.txt);;Markdown (*.md *.markdown);;reStructuredText (*.rst *.rest);;모든 파일 (*.*)")
                :TextSuffix=="html"||TextSuffix=="htm"?QObject::tr("HTML (*.html *.htm);;텍스트 문서 (*.txt);;모든 파일 (*.*)")
                :TextSuffix=="md"||TextSuffix=="markdown"?QObject::tr("Markdown (*.md *.markdown);;reStructuredText (*.rst *.rest);;텍스트 문서 (*.txt);;모든 파일 (*.*)")
                :TextSuffix=="rst"||TextSuffix=="rest"?QObject::tr("reStructuredText (*.rst *.rest);;Markdown (*.md *.markdown);;텍스트 문서 (*.txt);;모든 파일 (*.*)"):QStringLiteral("Text files (*.*)");
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

        QString CreateSqliteFile(const NewSqliteRequest& Request)
        {
            using namespace hc::sqlite;
            const auto& Path = Request.path_;
            if (!QFileInfo(Path).dir().exists()) return QObject::tr("저장 위치 폴더를 찾을 수 없습니다.");
            const auto Occupied = [&] {
                if (QFileInfo::exists(Path)) return QObject::tr("같은 이름의 파일이 이미 있습니다.");
                for (const auto Suffix : {"-journal", "-wal", "-shm"})
                    if (QFileInfo::exists(Path + QLatin1String(Suffix)))
                        return QObject::tr("같은 이름의 SQLite 보조 파일이 있습니다. 다른 이름을 지정하세요.");
                return QString{};
            };
            if (const auto Error = Occupied(); !Error.isEmpty()) return Error;

            QTemporaryFile Scratch(QDir::temp().filePath(QStringLiteral("hyper-sqlite-XXXXXX")));
            if (!Scratch.open()) return Scratch.errorString();
            Scratch.close();
            QString Error;
            auto Db = Connection::Open(Scratch.fileName(), {}, &Error);
            if (!Db) return Error;
            const bool Encrypted = !Request.key_.bytes_.isEmpty();
            if (Encrypted && !ApplyProtectedKey(*Db, Request.key_.bytes_, Request.key_.format_, &Error))
                return Error;
            // SQLite는 파일을 열기만 하면 0바이트로 둔다. VACUUM으로 빈 스키마의 첫 페이지를 쓴다.
            if (!Db->Exec(QStringLiteral("VACUUM"), &Error)) return Error;
            Db.reset();

            QFile File(Scratch.fileName());
            if (!File.open(QIODevice::ReadOnly)) return File.errorString();
            const auto Bytes = File.readAll();
            if (File.error() != QFileDevice::NoError) return File.errorString();
            if (InspectHeader(Bytes, Bytes.size()).kind_ != (Encrypted ? FileKind::SeeEncrypted : FileKind::Plain))
                return QObject::tr("생성한 데이터베이스의 헤더 또는 암호화 상태가 올바르지 않습니다.");
            if (const auto Problem = Occupied(); !Problem.isEmpty()) return Problem;
            auto Saved = SaveTextFile(Path, Bytes, storage::DestinationExpectation{}, {}, {}, false, true);
            if (!Saved.HasValue()) {
                if (const auto Problem = Occupied(); !Problem.isEmpty()) return Problem;
                return Message(QString::fromStdString(Saved.Failure().code_));
            }
            return {};
        }

        struct NewSqliteResult
        {
            QString error_;
            bool keySaved_ = true;
        };
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
                {"text.replace", "replace"}, {"text.goToLine", "toc-toggle"}, {"text.wordWrap", "word-wrap"}, {"text.minimap", "minimap"},
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
        std::optional<TextSnapshot> textSnapshot_;
        std::uint64_t revision_ = 0;
        DocumentEncoder encoder_;
        bool original_ = false, overwrite_ = false;
    };
    struct DocumentWorkspaceWidget::Tab
    {
        // 중앙 영역의 문서 도크(내용 = pane_)와 ‘옆에 미리보기 열기’로 연 미리보기 도크.
        fm::dock::DockWidget* dock_ = nullptr;
        QPointer<fm::dock::DockWidget> previewDock_;
        // 도구 창에 넘긴 이 문서의 칸.
        QList<DocumentToolPanelItem> toolPanels_;
        QString id_ = QUuid::createUuid().toString(QUuid::Id128), path_, error_;
        QString sourceToken_, pendingToken_, recoveredName_, temporarySource_, displayName_;
        // "새 문서 N" / "새 이미지 N" while the document has no file of its own; 0 otherwise.
        int untitledNumber_ = 0;
        bool untitledImage_ = false;
        // Where Save As of a recovered copy starts (the original it came from), when known.
        QString suggestedSavePath_;
        // A line to go to once the document has loaded (opened from another document's outline, diagnostics or preview).
        int openLine_ = 0;
        // HotExit PDF restore: the original's SHA-256 at close, and the view position to reapply once loaded.
        QString recoveryBaseline_;
        QJsonObject restorePosition_;
        QString readingPath_,readingEntry_, recoveryNotice_;
        QString navigationAfterSave_;
        QByteArray pendingHash_;
        sqlite::ProtectedKey newSqliteKey_;
        std::shared_ptr<LocalSave> activeSave_, queuedSave_;
        bool composingSave_ = false;
        std::uint64_t checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
        std::shared_ptr<DocumentSession> session_ = std::make_shared<DocumentSession>(id_.toStdString());
        // Encoding verdict of the bytes the session holds. Updated only together with
        // the session text, never by a serialization that was rejected.
        std::optional<storage::Error> serializationError_;
        bool rejectingEdit_ = false;
        bool comparingText_ = false;
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
        // A hosted viewer publishes no session, but its lock still marks the snapshots it writes as in use
        // for the recovery list and the retention clean-up of other processes sharing the folder.
        if (QDir().mkpath(recoveryRoot_ + "/sessions")) {
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
        buildDocks();
        BasicTrace.End();
        // The standalone editor uses the product title bar; a hosted viewer keeps its host's frame.
        if (!Hosted && !Parent) {
            // WindowChrome keeps the left, right and bottom resize borders outside the client area, where
            // native editor and foreign preview HWNDs cannot take the hit test, so the content reaches the edges.
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
        Layout->addWidget(menu_); Layout->addWidget(toolbar_); Layout->addWidget(docks_, 1); Layout->addWidget(StatusRow);
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
        Add(tr("새 reStructuredText 문서"), "file.newRst", [this] { newDocument(NewDocumentKind::Rst); });
        Add(tr("새 Sphinx 프로젝트…"), "file.newSphinxProject", [this] { newDocument(NewDocumentKind::SphinxProject); });
        Add(tr("새 EPUB 책…"), "file.newEpub", [this] { newDocument(NewDocumentKind::Epub); });
        Add(tr("새 이미지…"), "file.newImage", [this] { requestNewDocument(nullptr, true); });
        Add(tr("열기…"), "openDocument", [this] {
            auto* Dialog = new QFileDialog(this, tr("문서 열기"));
            Dialog->setFileMode(QFileDialog::ExistingFiles);
            connect(Dialog, &QFileDialog::filesSelected, this, [this](const QStringList& Paths) {
                QStringList Rejected;
                for (const auto& Path : Paths)
                    if (openByUser(Path).contains("error")) Rejected.append(Path);
                ShowShellError(Rejected.isEmpty() ? QString{} : tr("문서 한도 때문에 열지 못한 파일: %1").arg(Rejected.join(", ")));
            });
            OpenFileDialog(Dialog);
        });
        Add(tr("저장"), "saveDocument", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->choosingSavePath_) return;
            // SQLite: 저장은 커밋(설계 4.5 · 6.2). 문서 바이트를 쓰지 않는다.
            if (Tab->pane_->IsSqlite()) { if (!Tab->loading_) Tab->pane_->CommitSqlite(); update(); return; }
            if(Tab->activeSave_){save(Tab,Tab->activeSave_->destination_,Tab->activeSave_->overwrite_,Tab->activeSave_->original_ && Tab->pane_->CanEditEpub());return;}
            if (Tab->broker_) prepareSaveAsync(Tab);
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
            // SQLite: 연 데이터베이스는 스키마 · 열린 표 · 행 수만 다시 읽는다(쿼리 탭은 둔다). 열지 못한 문서는 다시 연다.
            if (Tab->pane_->IsSqlite() && !Tab->loading_ && Tab->pane_->RefreshSqlite()) return;
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
            else if(previewFullscreen_ || (Tab && Tab->pane_->HasPreview() && !Fullscreen->Active()))togglePreviewFullscreen();
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
        connect(EpubRestore,&QAction::triggered,this,[this]{ShowRecoveryManager("epub");});
        auto* EpubReopen=new QAction(tr("저장본 다시 열기"),this);EpubReopen->setObjectName("epubReopenDocument");EpubReopen->setProperty("documentIconName","open");addAction(EpubReopen);
        connect(EpubReopen,&QAction::triggered,this,[this]{if(auto Tab=active())if(auto* Reopen=Tab->pane_->findChild<QAction*>("epubReopen"))Reopen->trigger();});
        Restore->setObjectName("documentRecovery");
        connect(Restore, &QAction::triggered, this, [this] { ShowRecoveryManager(); });
        for (const auto& Item : {std::pair{QString("tab.next"), 1}, std::pair{QString("tab.previous"), -1}}) {
            auto* Action = new QAction(Item.second == 1 ? tr("다음 탭") : tr("이전 탭"), this); Action->setObjectName(Item.first); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Step = Item.second] {
                // 포커스 문서가 든 묶음 안에서 돈다.
                auto Tab = active(); auto* Area = Tab ? Tab->dock_->dockArea() : nullptr;
                if (!tabbed_ || !Area) return;
                QList<fm::dock::DockWidget*> Open; for (auto* Dock : Area->openDockWidgets()) if (tabOf(Dock) && Dock == tabOf(Dock)->dock_) Open.append(Dock);
                const auto Count = Open.size(); if (Count < 2) return;
                releaseFocusFrom(Tab);
                const auto Next = tabOf(Open[(Open.indexOf(Tab->dock_) + Step + Count) % Count]);
                activate(Next); Next->pane_->Focus();
                focusReleased_ = false;
            });
        }
        // 옆에 미리보기 열기(Markdown · reST): 미리보기만 따로 문서 탭으로 열어 옆 묶음에 둔다.
        auto* Beside = new QAction(tr("옆에 미리보기 열기"), this); Beside->setObjectName("view.previewBeside"); addAction(Beside);
        Beside->setProperty("documentIconName", "view-split");
        connect(Beside, &QAction::triggered, this, [this] { if (auto Tab = active()) openPreviewBeside(Tab->id_); });
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
                if(auto* Rst=Tab->pane_->findChild<RstDocumentView*>();Rst&&Rst->RunPreviewCommand(Id))return;
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
        // 탐색기 · 워크스페이스 명령(폴더 열기 · 워크스페이스 닫기 · 탐색기로 이동 · explorer.*).
        connectExplorer();
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
            if (!QStringList{"epub-edit", "epub-edit.tree", "epub-edit.source", "image", "image.comic", "pdf", "markdown", "rst", "sqlite", "sqlite.query", "sqlite.grid", "sqlite.schema"}.contains(Command.scope_)
                && !Command.id_.startsWith("view.zoom")) continue;
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
            if(Scope=="rst")return Tab && !Tab->loading_ && Tab->pane_->IsRst() && Tab->pane_->CanRunViewCommand(Id);
            // The minimap toggle is a view setting: it works while the source is shown, whichever pane has focus.
            const bool Minimap = Id == QLatin1String("text.minimap");
            if (Scope == "text" && Tab && Tab->pane_->HasPreview())return !Tab->loading_ && Tab->pane_->PreviewSourceVisible() && (InText || Minimap);
            if (Scope == "text") return Tab && !Tab->loading_ && (Tab->pane_->ViewId() == "text" || (EpubEditing && (InText || Minimap)));
            if (Scope == "edit") {
                auto* Input = Tab ? Tab->pane_->InputWidget() : nullptr;
                return Input && !Tab->loading_ && (Focus == Input || Input->isAncestorOf(Focus));
            }
            if (Scope == "epub-edit") return EpubEditing;
            if (Scope == "epub-edit.tree") return EpubEditing && Within([](QWidget* Widget) { return Widget->objectName() == "epubResourceTree"; });
            if (Scope == "epub-edit.source") return EpubEditing && InText;
            if (Id == "image.fullscreen") return Tab && !Tab->loading_ && Tab->pane_->ViewId() == "image";
            // 탐색기: 트리에 포커스가 있을 때만(필터 칸 · 이름 입력 칸은 뺀다). 워크스페이스 명령은 HyperEditor 만.
            if (Scope == "explorer") return explorer_ && explorer_->OwnsShortcutFocus(Focus);
            if (Id.startsWith(QLatin1String("workspace."))) return !hosted_;
            if (Id == QLatin1String("view.explorer")) return explorer_ != nullptr;
            if (Scope == "image" || Scope == "image.comic" || Scope == "pdf") return Tab && !Tab->loading_ && Tab->pane_->isAncestorOf(Focus) && !qobject_cast<QLineEdit*>(Focus)
                && !qobject_cast<QAbstractSpinBox*>(Focus) && !qobject_cast<QComboBox*>(Focus) && Tab->pane_->CanRunViewCommand(Id);
            // SQLite: 작업 창이 포커스 자리(쿼리 편집기 · 그리드 · 스키마 트리)로 범위를 판정한다.
            if (Scope == "sqlite" || Scope.startsWith(QLatin1String("sqlite."))) return Tab && !Tab->loading_ && Tab->pane_->OwnsSqliteScope(Scope, Focus) && Tab->pane_->CanRunViewCommand(Id);
            return true;
        });
        ActionsTrace.End(); hc::document::startup::Scope WatchersTrace("editor.watchers");
        const auto Override = qApp->property("documentViewsConfigDir");
        settingsPath_ = hc::settings::UnifiedConfig::FileName(Override.isValid() ? Override.toString() : hc::settings::DocumentPaths().directory_);
        settingsWatcher_ = new QFileSystemWatcher(this);
        auto* Reload = new QTimer(this); Reload->setSingleShot(true); Reload->setInterval(80);
        connect(settingsWatcher_, &QFileSystemWatcher::directoryChanged, Reload, qOverload<>(&QTimer::start));
        connect(Reload, &QTimer::timeout, this, [this] { refreshPreferencesAsync(); });
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
            connect(startPage_, &DocumentStartPage::FilesDropped, this, [this](const QStringList& Paths) {
                for (const auto& Path : Paths) { if (QFileInfo(Path).isDir()) OpenWorkspace(Path); else openByUser(Path); }
            });
            connect(startPage_, &DocumentStartPage::OpenFolderRequested, this, &DocumentWorkspaceWidget::showOpenFolderDialog);
            setAcceptDrops(true);
            // 문서가 하나도 없는 중앙 영역에 보인다(도구 창은 그대로).
            docks_->setCentralPlaceholderWidget(startPage_);
        }
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive) refreshPreferencesAsync(); });
        externalWatcher_ = new mrst::ExternalChangeWatcher(this);
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileChanged, this, [this](const QString& Path) { externalChange(Path, false); });
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileVanished, this, [this](const QString& Path) { externalChange(Path, true); });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive) externalWatcher_->recheckAll(); });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed,
            this, [this] { RefreshWorkspaceIcons(*this); });
        WatchersTrace.End();
        if (explorerTool_) shortcutRouter_->AddScope(explorerTool_);
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
        if (!hosted_) fileMenu_->addAction(scoped(A("newDocument"), Of({K::None, K::Text, K::MarkdownPreview, K::RstPreview, K::MarkdownEdit, K::RstEdit, K::EpubRead, K::EpubEdit, K::Sqlite})));
        fileMenu_->addAction(A("openDocument"));
        if (!hosted_) fileMenu_->addAction(A("workspace.openFolder"));
        fileMenu_->addAction(scoped(A("reloadDocument"), Documents));
        fileMenu_->addAction(A("editHwpDocument"));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("saveDocument"), Saving));
        fileMenu_->addAction(scoped(A("saveDocumentAs"), Saving));
        fileMenu_->addAction(scoped(MakeProxy(A("saveDocumentAs"), tr("사본 저장…"), this, "saveDocumentCopy"), Reading));
        fileMenu_->addAction(A("epubReopenDocument"));
        fileMenu_->addAction(A("resolveDocumentConflict"));
        fileMenu_->addSeparator();
        // SQLite 작업 창은 인쇄 · 복구 대상이 아니다(내보내기 · 커밋을 쓴다).
        fileMenu_->addAction(scoped(A("printDocument"), All & ~Epub & ~Of(K::Sqlite)));
        fileMenu_->addAction(scoped(A("previewDocument"), All & ~Epub & ~Of(K::Sqlite)));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("documentRecovery"), All & ~Epub & ~Of(K::Sqlite)));
        auto* Recover = new QMenu(tr("복구"), fileMenu_); Recover->setObjectName("documentRecoveryMenu");
        Recover->menuAction()->setProperty("documentIconName", "refresh");
        Recover->addAction(MakeProxy(A("documentRecovery"), tr("복구 목록…"), Recover, {}, false));
        Recover->addAction(A("epubDraftRecovery"));
        fileMenu_->addAction(scoped(Recover->menuAction(), Epub));
        fileMenu_->addSeparator();
        fileMenu_->addAction(scoped(A("closeDocument"), Documents));
        if (!hosted_) fileMenu_->addAction(A("workspace.close"));
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
        editMenu_->addAction(scoped(A("edit.copy"), Editing | Of({K::Pdf, K::EpubRead, K::MarkdownPreview, K::RstPreview})));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("이미지 복사"), this, "edit.copyImage"), Of(K::Image)));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("현재 쪽 이미지 복사"), this, "edit.copyPage"), Of(K::Comic)));
        editMenu_->addAction(scoped(MakeProxy(A("edit.copy"), tr("현재 이미지 복사"), this, "edit.copyStrip"), Of(K::Webtoon)));
        editMenu_->addAction(scoped(A("edit.paste"), Editing));
        editMenu_->addAction(scoped(A("edit.delete"), Editing));
        editMenu_->addAction(scoped(A("edit.selectAll"), Editing | Of({K::Pdf, K::MarkdownPreview, K::RstPreview})));
        editMenu_->addSeparator();
        editMenu_->addAction(scoped(A("text.find"), Editing | Of({K::MarkdownPreview, K::RstPreview})));
        if (auto* Book = A("epub.focus-search")) editMenu_->addAction(scoped(MakeProxy(Book, tr("책에서 찾기…"), this, "epub.findInBook"), Of(K::EpubRead)));
        editMenu_->addAction(scoped(A("text.replace"), Editing));
        editMenu_->addAction(scoped(A("text.findNext"), Editing));
        editMenu_->addAction(scoped(A("text.findPrev"), Editing));
        editMenu_->addAction(scoped(A("text.goToLine"), Editing));
        auto* EditAnchor = scoped(Separator(editMenu_), All); editMenu_->addAction(EditAnchor);
        editMenu_->addAction(scoped(A("document.findInFiles"), Of(K::None) | Editing | Of({K::EpubRead, K::MarkdownPreview, K::RstPreview})));
        editMenu_->addAction(scoped(A("document.replaceInFiles"), Of(K::None) | Editing | Of({K::EpubRead, K::MarkdownPreview, K::RstPreview})));
        // 보기: the view inserts its items before the common tail.
        viewMenu_ = menu_->addMenu(tr("보기(&V)")); viewMenu_->setObjectName("documentViewMenu");
        auto* ViewAnchor = viewMenu_->addSeparator();
        viewMenu_->addAction(scoped(A("view.previewBeside"), viewKinds::Markup));
        auto* Fullscreen = A("view.fullscreen"); Fullscreen->setText(tr("전체 화면") + QStringLiteral("\tF11"));
        viewMenu_->addAction(scoped(Fullscreen, All & ~viewKinds::Markup));
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
        // 도구 창 켜기 · 끄기와 창 배치(이름 붙인 배치 · 잠금). EPUB 보기의 ‘배치’와 겹치지 않게 ‘창 배치’.
        viewMenu_->addSeparator();
        if (auto* Explorer = A("view.explorer")) viewMenu_->addAction(Explorer);
        toolMenu_ = viewMenu_->addMenu(tr("도구 창")); toolMenu_->setObjectName("documentToolMenu");
        toolMenu_->menuAction()->setProperty("documentIconName", "toc-toggle");
        connect(toolMenu_, &QMenu::aboutToShow, this, [this] { fillToolMenu(toolMenu_); });
        layoutMenu_ = viewMenu_->addMenu(tr("창 배치")); layoutMenu_->setObjectName("documentLayoutMenu");
        layoutMenu_->menuAction()->setProperty("documentIconName", "view-split");
        connect(layoutMenu_, &QMenu::aboutToShow, this, [this] { fillLayoutMenu(layoutMenu_); });
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
        Tool(A("newDocument"), tr("새 문서"), Of({K::None, K::Text, K::MarkdownPreview, K::RstPreview, K::MarkdownEdit, K::RstEdit}))->setVisible(!hosted_);
        Tool(A("openDocument"), tr("열기"), All);
        Tool(A("saveDocument"), tr("저장"), Of({K::None, K::Text, K::MarkdownPreview, K::RstPreview, K::MarkdownEdit, K::RstEdit, K::Pdf, K::Image, K::EpubEdit}));
        Tool(A("printDocument"), tr("인쇄"), Of(K::Pdf));
        const quint32 History = Of({K::None, K::Text, K::MarkdownEdit, K::RstEdit, K::Image, K::EpubEdit});
        toolbar_->addAction(scoped(Separator(this), History));
        Tool(A("undoDocument"), tr("실행 취소"), History);
        Tool(A("redoDocument"), tr("다시 실행"), History);
        auto* Hwp = A("editHwpDocument"); toolbar_->addAction(Hwp);
        if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(Hwp))) Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        // 오른쪽 끝: 창 배치 고르개(보기 › 창 배치와 같은 메뉴). 임베드에는 두지 않는다.
        if (!hosted_) {
            auto* Spacer = new QWidget(toolbar_); Spacer->setObjectName("documentToolbarSpacer"); Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            toolbar_->addWidget(Spacer);
            auto* LayoutButton = new QToolButton(toolbar_); LayoutButton->setObjectName("documentLayoutButton");
            LayoutButton->setToolButtonStyle(Qt::ToolButtonTextOnly); LayoutButton->setPopupMode(QToolButton::InstantPopup);
            LayoutButton->setMenu(layoutMenu_); LayoutButton->setText(tr("창 배치: %1").arg(tr("기본")));
            LayoutButton->setToolTip(tr("창 배치 — 도구 창의 자리를 이름 붙여 저장하고 바꿉니다."));
            toolbar_->addWidget(LayoutButton);
        }
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
        // 워크스페이스 창: ‘문서 — 워크스페이스 — HyperEditor’.
        if (!workspaceRoot_.isEmpty()) {
            const auto Name = QFileInfo(workspaceRoot_).fileName().isEmpty() ? QDir::toNativeSeparators(workspaceRoot_) : QFileInfo(workspaceRoot_).fileName();
            Title = Title == QLatin1String("HyperEditor") ? Name + QStringLiteral(" — HyperEditor") : Title.chopped(int(QStringLiteral(" — HyperEditor").size())) + QStringLiteral(" — ") + Name + QStringLiteral(" — HyperEditor");
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
        // 문서 탭의 메뉴는 도킹 관리자가 연결점(showDocumentMenu)으로 부른다.
        for (const auto& Each : documents_)
            if (Each->pane_ && Within(Each->pane_->PrimaryToolbar())) { showChromeMenu(Event->globalPos(), true); return; }
    }
    DocumentWorkspaceWidget::~DocumentWorkspaceWidget()
    {
        // 사라지는 창이 다른 창의 라우팅(열린 워크스페이스 고르기)에 잡히지 않게 먼저 푼다.
        workspace::Registry::Instance().Unbind(this);
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
        connect(Dialog, &DocumentSettingsDialog::RecoveryManagerRequested, this, [this] { ShowRecoveryManager(); });
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
    namespace
    {
        QString SettingsSignature(const QString& Path)
        {
            hc::startup::Span InputTrace("input.settings.probe");
            auto Files = DocumentThemeAdapter::ProductThemeFiles(QFileInfo(Path).absolutePath());
            Files.prepend(Path);
            QStringList Parts;
            for (const auto& File : Files) {
                const auto Print = mrst::ExternalChangeWatcher::fingerprintOf(File);
                Parts.append(QStringLiteral("%1:%2:%3").arg(Print.exists).arg(Print.size).arg(Print.lastModifiedUtcMs));
            }
            return Parts.join('|');
        }
        QString SettingsWatchDirectory(const QString& Path)
        {
            auto Directory = QFileInfo(Path).absolutePath();
            while (!QDir(Directory).exists()) {
                const auto Parent = QFileInfo(Directory).absolutePath();
                if (Parent == Directory) return {};
                Directory = Parent;
            }
            return Directory;
        }
    }
    QString DocumentWorkspaceWidget::settingsSignature() const
    {
        return SettingsSignature(settingsPath_);
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
            QString error_, signature_, directory_;
            bool changed_ = true;
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
        const auto Directory = SettingsWatchDirectory(settingsPath_);
        if (!Directory.isEmpty() && !settingsWatcher_->directories().contains(Directory)) settingsWatcher_->addPath(Directory);
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
        // Activation never stats a settings path on the input thread. Coalesce both
        // probing and parsing: one immutable request in flight, at most one follow-up.
        if (preferencesReading_) { preferencesAgain_ = true; return; }
        preferencesReading_ = true;
        const auto Generation = ++preferencesGeneration_;
        const auto Path = settingsPath_;
        auto* Watcher = new QFutureWatcher<PreferencesInput>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation, Path] {
            const auto Input = Watcher->result(); Watcher->deleteLater(); preferencesReading_ = false;
            if (Generation == preferencesGeneration_ && Path == settingsPath_) {
                settingsSignature_ = Input.signature_;
                if (!Input.directory_.isEmpty() && !settingsWatcher_->directories().contains(Input.directory_))
                    settingsWatcher_->addPath(Input.directory_);
                if (Input.changed_) applyPreferences(Input.product_, Input.loaded_, Input.root_, Input.error_);
            }
            if (std::exchange(preferencesAgain_, false)) refreshPreferencesAsync();
        });
        Watcher->setFuture(QtConcurrent::run([Path, Previous = settingsSignature_] {
            const auto Signature = SettingsSignature(Path); // before reading: a concurrent write is detected next time
            auto Input = Signature == Previous ? PreferencesInput{} : ReadPreferences(Path);
            Input.changed_ = Signature != Previous;
            Input.signature_ = Signature;
            Input.directory_ = SettingsWatchDirectory(Path);
            return Input;
        }));
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
            const auto Location = searchLocation();
            // A closed panel stays closed and opens at the new place next time.
            if (searchPanel_->property("configuredLocation").toString() != Location) {
                searchPanel_->setProperty("configuredLocation", Location);
                if (searchPanel_->isVisible()) placeSearch(Location);
            }
        }
        if (externalWatcher_) externalWatcher_->reloadSettings();
        if (!Chrome.isEmpty() && !ApplyChromeSettings(Chrome)) ShowShellError(tr("문서 표시 설정이 올바르지 않습니다."));
        const bool Cover = Views["imageView"].toObject()["comic"].toObject()["coverAlone"].toBool(hc::defaults::edit::ViewValue("imageView/comic/coverAlone").toBool());
        for(auto* Reader:findChildren<ImageReadingView*>())if(Reader->State()["cover"].toBool()!=Cover)Reader->SetCover(Cover);
        for (auto* View : findChildren<QTextView*>()) View->ReloadPreferences();
        for(auto* Markdown:findChildren<MarkdownDocumentView*>())Markdown->ReloadPreferences();
        for(auto* Rst:findChildren<RstDocumentView*>())Rst->ReloadPreferences();
        // 설정 › SQLite › 열기: 확장자가 달라도 머리가 SQLite 면 SQLite 로(다음에 여는 파일부터).
        hc::sqlite::SetHeaderSniffing(Views["sqlite"].toObject()["open"].toObject()["sniffHeader"].toBool(hc::defaults::edit::ViewValue("sqlite/open/sniffHeader").toBool()));
        for(auto* Pane:findChildren<DocumentViewPane*>())Pane->ReloadSqlitePreferences();
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
        applyExplorerPreferences();
    }
    QJsonObject DocumentWorkspaceWidget::OpenArchiveDraftCopy(const QString& Directory)
    {
        if (closingWorkspace_) return Error("document-workspace-closing");
        // The copy opens like a recovered document: never the archive itself, and Save As starts
        // from the file's own name.
        const auto Content = recovery::ArchiveDraftContent(Directory);
        if (Content.isEmpty() || !QFileInfo(Content).isFile()) return Error("archive-draft-read");
        const auto Suffix = QFileInfo(Content).suffix();
        const auto Copy = recoveryRoot_ + "/restored-" + QUuid::createUuid().toString(QUuid::Id128) + (Suffix.isEmpty() ? QString{} : "." + Suffix);
        if (!QDir().mkpath(recoveryRoot_) || !QFile::copy(Content, Copy)) return Error("archive-draft-read");
        const auto Reply = open(Copy, false, {}, false, {}, tr("복구 · %1").arg(QFileInfo(Content).fileName()));
        auto Tab = find(Reply["documentId"].toString());
        if (!Tab) { QFile::remove(Copy); return Reply.contains("error") ? Reply : Error("archive-draft-open"); }
        Tab->temporarySource_ = Copy; Tab->originalAuthority_ = false; Tab->originalSave_ = false; Tab->recovered_ = true;
        Tab->suggestedSavePath_ = QFileInfo(Content).fileName();
        return Reply;
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
        if (Kind == NewDocumentKind::SphinxProject) {
            // conf.py 와 index.rst 는 제품이 직접 쓴다. HyperViewer 에서도 만들고 index.rst 를 그 창에서 연다.
            if ((!hosted_ && documents_.size() >= 16) || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
            showNewSphinxProjectDialog(); return {{"accepted", true}};
        }
        if ((!hosted_ && documents_.size() >= 16) || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        if (Kind == NewDocumentKind::Sqlite) {
            if (property("hc.quickPreview").toBool() || closingWorkspace_) return Error("document-edit-unavailable");
            showNewSqliteDialog();
            return {{"accepted", true}};
        }
        const bool Markdown = Kind == NewDocumentKind::Markdown, Rst = Kind == NewDocumentKind::Rst;
        // The GUID only keeps recovery files apart; the tab shows "새 문서 N" (documentName()).
        const auto Path = recoveryRoot_ + "/untitled-" + QUuid::createUuid().toString(QUuid::Id128) + (Markdown ? ".md" : Rst ? ".rst" : ".txt");
        if (!QDir().mkpath(recoveryRoot_) || !StoreCopy(Path, {})) return Error("document-new-file-failed");
        const auto Reply = open(Path, false, {}, false);
        if (auto Tab = find(Reply["documentId"].toString())) {
            Tab->originalAuthority_ = false; Tab->originalSave_ = false; Tab->temporarySource_ = Path;
            nameUntitled(Tab, false);
            // HyperViewer's default Markdown view (preview) would show an empty page: a new one starts in source.
            if ((Markdown || Rst) && hosted_) { RecentDocument Start; Start.markdownView_ = QStringLiteral("source"); Tab->restore_ = Start; }
            update();
        } else QFile::remove(Path);
        return Reply;
    }
    void DocumentWorkspaceWidget::showNewSphinxProjectDialog()
    {
        if (hotExitClosing_) return;
        if (auto* Open = findChild<NewSphinxProjectDialog*>(QString(), Qt::FindDirectChildrenOnly)) { Open->raise(); Open->activateWindow(); return; }
        AppSettings Settings;
        QString Folder;
        if (auto Tab = active(); Tab && Tab->originalAuthority_ && Tab->temporarySource_.isEmpty() && !Tab->broker_ && QFileInfo(Tab->path_).isAbsolute())
            Folder = QFileInfo(Tab->path_).absolutePath();
        if (Folder.isEmpty() || !QFileInfo(Folder).isDir()) Folder = Settings.value("common/newSphinxFolder").toString();
        if (Folder.isEmpty() || !QFileInfo(Folder).isDir()) Folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        NewSphinxProjectDialog::Options Options;
        Options.directory_ = Folder; Options.author_ = Settings.value("common/newSphinxAuthor").toString();
        if (auto* Command = findChild<QAction*>("newDocument", Qt::FindDirectChildrenOnly)) Options.repeatKey_ = Command->shortcut();
        auto* Dialog = new NewSphinxProjectDialog(this, Options);
        Dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(Dialog, &NewSphinxProjectDialog::Created, this, [this](const QString& Index, const SphinxProjectSpec& Spec) {
            AppSettings Remember;
            Remember.setValue("common/newSphinxFolder", Spec.directory_); Remember.setValue("common/newSphinxAuthor", Spec.author_); Remember.sync();
            const auto Result = open(Index, false, {}, false);
            if (Result.contains("error")) ShowShellError(Message(Result["error"].toString()));
        });
        Dialog->open();
    }
    void DocumentWorkspaceWidget::togglePreviewFullscreen()
    {
        auto* Root = window(); const auto Tab = active();
        if (previewFullscreen_) {
            previewFullscreen_ = false;
            for (const auto& Item : documents_) if (Item->pane_->IsPreviewOnly()) Item->pane_->SetPreviewOnly(false);
            for (const auto& Widget : std::exchange(previewHidden_, {})) if (Widget) Widget->show();
            chrome_->SetSuppressed(false);
            // 전체 화면인 채 닫아도 다음 실행은 보통 창이다: 들어가기 직전의 자리로 돌아간다.
            Root->showNormal(); if (epubWasMaximized_) Root->showMaximized(); else Root->setGeometry(epubWindowGeometry_);
            fm::ui::ToolTip::hideText();
        } else {
            if (!Tab || Tab->loading_ || !Tab->pane_->HasPreview() || epubFullscreen_ || findChild<ImageFullscreen*>("imageFullscreen")->Active()) return;
            epubWasMaximized_ = Root->isMaximized(); epubWindowGeometry_ = Root->isMaximized() ? Root->normalGeometry() : Root->geometry();
            previewFullscreen_ = true;
            // 제목 표시줄 · 메뉴 · 도구모음 · 탭 · 상태 표시줄을 감춘다. 개요 · 소스 · 진단 패널은 보기가 감춘다.
            chrome_->SetSuppressed(true);
            // 도구 창 · 다른 묶음은 문서 도크만 남기고 감춘다(보이는 것만, 나올 때 되돌림).
            QList<QWidget*> Chrome{findChild<QWidget*>("documentWindowChrome", Qt::FindDirectChildrenOnly), findChild<QWidget*>("documentStatusRow", Qt::FindDirectChildrenOnly)};
            if (auto* Area = Tab->dock_->dockArea()) Chrome.append(Area->findChild<QWidget*>(QStringLiteral("fmDockAreaTitleBar"), Qt::FindDirectChildrenOnly));
            for (auto* Dock : docks_->dockWidgets()) if (Dock != Tab->dock_ && Dock->dockArea() && Dock->dockArea() != Tab->dock_->dockArea() && !Dock->isFloating()) Chrome.append(Dock->dockArea());
            for (auto* Bar : docks_->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^fmDockSideBar")), Qt::FindDirectChildrenOnly)) Chrome.append(Bar);
            for (auto* Widget : Chrome) if (Widget && !Widget->isHidden()) { previewHidden_.append(Widget); Widget->hide(); }
            Tab->pane_->SetPreviewOnly(true);
            Root->showFullScreen();
            // 들어갈 때 잠깐 뜨는 안내: 2초 뒤 사라진다.
            QTimer::singleShot(150, this, [this] {
                if (!previewFullscreen_) return;
                const auto Area = window()->geometry();
                fm::ui::ToolTip::showText(QPoint(Area.center().x() - 120, Area.top() + 28), tr("미리보기 전체 화면 · 나가기 Esc 또는 F11"), this);
                QTimer::singleShot(2000, this, [] { fm::ui::ToolTip::hideText(); });
            });
        }
        if (auto Current = active()) Current->pane_->Focus();
        update();
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
        if (Mode == "text" || Mode == "markdown" || Mode == "rst") {
            const auto Result = newDocument(Mode == "markdown" ? NewDocumentKind::Markdown : Mode == "rst" ? NewDocumentKind::Rst : NewDocumentKind::Text);
            if (Result.contains("error")) ShowShellError(Message(Result["error"].toString()));
            return;
        }
        // The product dialog frame, centred on this window like the other editor dialogs. open(): never block the host pump.
        NewDocumentChooser::Options Options;
        Options.initial_ = NewDocumentKindFrom(Settings.value("common/newDocumentLastKind").toString());
        Options.viewer_ = hosted_; Options.repeatKey_ = Repeat;
        Options.offerSqlite_ = !property("hc.quickPreview").toBool();
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
    void DocumentWorkspaceWidget::showNewSqliteDialog()
    {
        if (auto* Open = findChild<NewSqliteDialog*>(QString(), Qt::FindDirectChildrenOnly); Open && Open->isVisible()) {
            Open->raise(); Open->activateWindow(); return;
        }
        AppSettings Settings;
        QString Folder;
        if (auto Tab = active(); Tab && Tab->originalAuthority_ && Tab->temporarySource_.isEmpty() && !Tab->broker_ && QFileInfo(Tab->path_).isAbsolute())
            Folder = QFileInfo(Tab->path_).absolutePath();
        if (!QFileInfo(Folder).isDir()) Folder = workspaceRoot_;
        if (!QFileInfo(Folder).isDir()) Folder = Settings.value("common/lastSaveFolder").toString();
        if (!QFileInfo(Folder).isDir()) Folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const auto Format = Settings.value("sqlite/see/keyFormat").toString();
        const auto Profile = QFileInfo(Settings.fileName()).absolutePath();
        auto* Dialog = new NewSqliteDialog(this, Folder,
            Format == "hexkey" ? sqlite::KeyFormat::Hex : Format == "key" ? sqlite::KeyFormat::Raw : sqlite::KeyFormat::Text,
            Settings.value("sqlite/see/saveByDefault").toBool());
        connect(Dialog, &NewSqliteDialog::Requested, this, [this, Dialog, Profile](const NewSqliteRequest& Request) {
            sqliteCreating_ = true;
            auto* Watcher = new QFutureWatcher<NewSqliteResult>(this);
            connect(Watcher, &QFutureWatcherBase::finished, this, [this, Dialog, Watcher, Request] {
                const auto Result = Watcher->result();
                Watcher->deleteLater();
                sqliteCreating_ = false;
                if (closingWorkspace_ || hotExitClosing_) {
                    Dialog->done(QDialog::Rejected);
                    tryFinishClose();
                    return;
                }
                if (!Result.error_.isEmpty()) {
                    Dialog->ShowError(tr("데이터베이스를 만들지 못했습니다. %1").arg(Result.error_));
                    return;
                }
                Dialog->done(QDialog::Accepted);
                const auto Reply = open(Request.path_, false, {}, false);
                if (auto Tab = find(Reply["documentId"].toString())) Tab->newSqliteKey_ = Request.key_;
                else ShowShellError(tr("데이터베이스를 만들었습니다. %1").arg(Message(Reply["error"].toString())));
                if (!Result.keySaved_) ShowShellError(tr("데이터베이스를 만들었지만 이 PC에 키를 저장하지 못했습니다."));
            });
            Watcher->setFuture(QtConcurrent::run([Request, Profile] {
                NewSqliteResult Result{CreateSqliteFile(Request)};
                if (Result.error_.isEmpty() && Request.saveKey_)
                    Result.keySaved_ = sqlite::KeyStore(Profile).SaveProtected(Request.path_, Request.key_);
                return Result;
            }));
        });
        Dialog->open();
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
            if (Suffix == "rst" || Suffix == "rest") return tr("새 문서 %1.rst").arg(Value.untitledNumber_);
            return Suffix == "md" || Suffix == "markdown" ? tr("새 문서 %1.md").arg(Value.untitledNumber_) : tr("새 문서 %1").arg(Value.untitledNumber_);
        }
        if (!Value.displayName_.isEmpty()) return Value.displayName_;
        return QFileInfo(Value.path_).fileName();
    }
    QString DocumentWorkspaceWidget::untitledFileName(const Tab& Value) const
    {
        if (Value.untitledImage_) return tr("새 이미지 %1.png").arg(Value.untitledNumber_);
        const auto Suffix = QFileInfo(Value.path_).suffix().toLower();
        if (Suffix == "rst" || Suffix == "rest") return tr("새 문서 %1.rst").arg(Value.untitledNumber_);
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
    QWidget* DocumentWorkspaceWidget::currentPane() const { const auto Tab = active(); return Tab ? Tab->pane_ : nullptr; }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::active() const
    { if (!currentDock_) return {}; for (const auto& Tab : documents_) if (Tab->dock_ == currentDock_ && !Tab->closed_) return Tab; return {}; }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::tabOf(const fm::dock::DockWidget* Dock) const
    { if (!Dock) return {}; for (const auto& Tab : documents_) if (!Tab->closed_ && (Tab->dock_ == Dock || Tab->previewDock_ == Dock)) return Tab; return {}; }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::find(const QString& Id) const
    { for (const auto& Tab : documents_) if (Tab->id_ == Id && !Tab->closed_) return Tab; return {}; }
    void DocumentWorkspaceWidget::activate(const std::shared_ptr<Tab>& Tab)
    {
        if (const auto Previous = active(); Previous && Previous != Tab) releaseFocusFrom(Previous);
        // 묶음에서 앞으로 꺼내고(떠 있는 창이면 그 창을 올린다) 포커스 문서로 삼는다.
        Tab->dock_->setAsCurrentTab();
        if (auto* Floating = Tab->dock_->floatingWindow()) Floating->raise();
        setCurrentDocument(Tab->dock_);
        if (std::exchange(focusReleased_, false) && active() == Tab) Tab->pane_->Focus();
        workspace_.Activate(Tab->id_.toStdString()); update();
    }
    void DocumentWorkspaceWidget::releaseFocusFrom(const std::shared_ptr<Tab>& Tab)
    {
        if (Tab && !Tab->closed_ && Tab->pane_ && Tab->pane_->ReleaseNativeFocus()) focusReleased_ = true;
    }
    bool DocumentWorkspaceWidget::triggerCommand(const QString& Id)
    {
        for (auto* Action : findChildren<QAction*>(Qt::FindDirectChildrenOnly))
            if (Action->property("documentCommandId").toString() == Id || Action->objectName() == Id) {
                if (!Action->isEnabled()) return false;
                Action->trigger(); return true;
            }
        return false;
    }
    void DocumentWorkspaceWidget::runHostCommand(const std::shared_ptr<Tab>& Tab, const QString& Id)
    {
        // The reader process reports only window shortcuts; anything else from it is ignored.
        if (active() != Tab || Tab->closed_ || DocumentShortcutRegistry::ScopeOf(Id) != QLatin1String("common")
            || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
        // The keyboard is still in the reader: take it back so the search field, a dialog or the next tab gets it.
        const bool Released = Tab->pane_->ReleaseNativeFocus();
        const QPointer<DocumentViewPane> Pane = Tab->pane_;
        // A tab the command shows (next tab, closing this one) takes the keyboard.
        focusReleased_ = focusReleased_ || Released;
        triggerCommand(Id);
        focusReleased_ = false;
        // A command that left this book shown and gave the keyboard to nothing else (save, reload) returns it.
        if (!Released || !Pane || active() != Tab || Tab->closed_ || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
        auto* Focus = QApplication::focusWidget();
        if (!Focus || Focus == Pane || Pane->isAncestorOf(Focus)) Pane->Focus();
    }
    bool DocumentWorkspaceWidget::SetTabbed(bool Enabled)
    {
        if (!documents_.empty()) return false;
        // Single-document mode: one document, no tab strip, no splitting or floating (open() sets the features).
        tabbed_ = Enabled; return true;
    }
    bool DocumentWorkspaceWidget::ApplyChromeSettings(const QJsonObject& Settings)
    {
        if (Settings.contains("themeMode") && !QStringList{"product", "system", "light", "dark"}.contains(Settings["themeMode"].toString())) return false;
        if (!chrome_->ApplySettings(Settings)) return false;
        // 도킹 배치(도구 창 자리 · 이름 붙인 배치 · 잠금)는 같은 설정에 함께 있다.
        if (Settings.contains("dockLayout") || Settings.contains("dockPerspectives")) restoreDockSettings(Settings);
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
        return !sqliteCreating_ && (!searchPanel_ || !searchPanel_->Busy()) && !recoveryWorkers_ && std::none_of(documents_.begin(), documents_.end(), [](const auto& Tab) {
            return Tab->archiveUpdating_ || Tab->loading_ || Tab->navigating_ || Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || Tab->comparingText_ || Tab->checkpointing_ || !Tab->pendingToken_.isEmpty() || Tab->pane_->IsBusy();
        });
    }
    void DocumentWorkspaceWidget::verifyTextEquality(const std::shared_ptr<Tab>& Tab)
    {
        if (Tab->comparingText_ || Tab->closed_ || !Tab->Dirty() || !Tab->session_->NativeText() || !Tab->session_->SavedNativeText()) return;
        const auto Current = *Tab->session_->NativeText();
        const auto Baseline = *Tab->session_->SavedNativeText();
        if (Current.SameSerializedRoot(Baseline) || !Current.MayEqualSerialized(Baseline)) return;
        const auto State = Tab->session_->Snapshot();
        Tab->comparingText_ = true;
        auto* Watcher = new QFutureWatcher<bool>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Baseline, State] {
            const bool Equal = Watcher->result(); Watcher->deleteLater(); Tab->comparingText_ = false;
            if (Tab->closed_) return;
            if (!Tab->session_->ResolveTextEquality(State.generation_, State.revision_, Baseline, Equal)) verifyTextEquality(Tab);
            update(); tryFinishClose();
        });
        Watcher->setFuture(QtConcurrent::run([Current, Baseline] { return Current.SerializedBytes() == Baseline.SerializedBytes(); }));
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
                if (!hosted_ && Tab->session_->ByteSize() > DocumentByteLimit) return Error("p0-text-byte-limit");
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
        // 문서 도크: 내용은 문서 칸이다. 닫기 단추는 저장 확인을 거치도록 closeRequested만 보낸다.
        using fm::dock::DockWidget;
        auto* Dock = new DockWidget(QStringLiteral("doc:") + Tab->id_, tr("여는 중…"), this);
        Dock->setDocument(true);
        DockWidget::Features Features = DockWidget::Closable | DockWidget::Focusable | DockWidget::CustomCloseHandling;
        if (tabbed_) Features |= DockWidget::Movable | DockWidget::Floatable; else Features |= DockWidget::NoTab;
        Dock->setFeatures(Features);
        Tab->dock_ = Dock;
        connect(Dock, &DockWidget::closeRequested, this, [this, Tab] {
            const auto At = std::find(documents_.begin(), documents_.end(), Tab);
            if (At != documents_.end()) { activate(Tab); closeTab(int(std::distance(documents_.begin(), At))); }
        });
        auto* Pane = new DocumentViewPane(Dock);
        Pane->setProperty("hc.previewPath", Path);
        connect(Pane, &DocumentViewPane::ToolPanelRequested, this, [this, Tab](const QString& Kind) {
            if (Tab->closed_ || active() != Tab) return;
            for (const auto& Item : Tab->toolPanels_) if (Item.kind_ == Kind && Item.widget_) toolHosts_.value(Kind)->ShowPanel(Item.widget_);
            showTool(Kind, true, true);
        });
        Pane->SetUnlimitedBytes(hosted_);
        Pane->SetShortcutBindings(shortcutRouter_->Bindings());
        Tab->pane_ = Pane;
        connect(Pane,&DocumentViewPane::SearchRequested,this,[this,Tab](const QString& Query){if(Tab->closed_)return;activate(Tab);openSearch(false);if(searchPanel_)searchPanel_->SetQuery(Query);});
        connect(Pane, &DocumentViewPane::ClipboardRequested, this, [this, Tab](const QString& Mode) { clipboard(Tab, Mode); });
        connect(Pane,&DocumentViewPane::ContentChanged,this,[this,Tab] {
            if(Tab->loading_||Tab->closed_)return;
            if (Tab->pane_->ViewId() == "text" && Tab->session_->NativeText()) {
                const auto Capture = Tab->pane_->CaptureTextSnapshot();
                if (!hosted_ && Capture.SerializedSize() > TextByteLimit) {
                    Tab->error_ = "p0-text-byte-limit"; update();
                    if (std::exchange(Tab->rejectingEdit_, true)) return;
                    QTimer::singleShot(0, this, [this, Tab] {
                        Tab->rejectingEdit_ = false; if (Tab->closed_ || !Tab->pane_) return;
                        { const QSignalBlocker Block(Tab->pane_); Tab->pane_->Undo(); }
                        Tab->session_->ApplyTextSnapshot(Tab->pane_->CaptureTextSnapshot());
                        Tab->session_->SetHistoryState(Tab->pane_->CanUndo(), Tab->pane_->CanRedo());
                        verifyTextEquality(Tab); Tab->error_ = "p0-text-byte-limit"; update();
                    });
                    return;
                }
                const auto Result = Tab->session_->ApplyTextSnapshot(Capture);
                Tab->error_ = Result.HasValue() ? QString{} : QString::fromStdString(Result.Failure().code_);
                Tab->session_->SetHistoryState(Tab->pane_->CanUndo(), Tab->pane_->CanRedo());
                verifyTextEquality(Tab); update();
                if (searchPanel_) searchPanel_->SourcesChanged();
                return;
            }
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
        connect(Pane,&DocumentViewPane::EscapeRequested,this,[this,Tab]{if(active()==Tab && !Tab->closed_){if(epubFullscreen_)toggleEpubFullscreen();else if(previewFullscreen_)togglePreviewFullscreen();else escapeViewer();}});
        connect(Pane,&DocumentViewPane::PreviewFullScreenRequested,this,[this,Tab]{if(active()==Tab && !Tab->closed_)togglePreviewFullscreen();});
        connect(Pane,&DocumentViewPane::WorkspaceCommandRequested,this,[this,Tab](const QString& Id){
            if(active()!=Tab || Tab->closed_)return;
            if(Id=="app.settings.rst"){showPreferences();if(preferences_)preferences_->ShowPage(QStringLiteral("rst"));return;}
            if(Id=="app.settings.sqlite"||Id=="app.settings.sqliteKeys"){showPreferences();if(preferences_)preferences_->ShowPage(Id=="app.settings.sqlite"?QStringLiteral("sqlite"):QStringLiteral("sqlite-keys"));return;}
            // 미리보기에 포커스가 있을 때 눌린 창 단축키( 저장 · 새 문서 · 탭 이동 등 ).
            triggerCommand(Id);
        });
        connect(Pane,&DocumentViewPane::HostCommandRequested,this,[this,Tab](const QString& Id){runHostCommand(Tab,Id);});
        // 다른 파일을 열고 그 줄로 간다: 이미 열려 있으면 바로, 여는 중이면 다 읽은 뒤에.
        connect(Pane,&DocumentViewPane::OpenAtRequested,this,[this,Tab](const QString& Path,int Line){
            if(Tab->closed_)return;
            const auto Reply=open(Path,false,{},false);
            if(Reply.contains("error")){ShowShellError(Message(Reply["error"].toString()));return;}
            if(auto Target=find(Reply["documentId"].toString())){if(Target->loading_)Target->openLine_=Line;else Target->pane_->GoToLine(Line);}
        });
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
        Dock->setWidget(Pane, DockWidget::InsertMode::ForceNoScrollArea);
        docks_->addDockWidgetToCentral(Dock);
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
                    if (Tab->pane_->ViewId()=="text") Tab->session_->AdoptTextSnapshot(Tab->pane_->CaptureTextSnapshot());
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
                // SQLite 는 sqlite3 가 파일을 직접 쓴다(커밋). 문서 바이트 저장 · 중개 저장을 쓰지 않는다.
                const bool Sqlite=Tab->input_.viewId_=="sqlite";
                Tab->originalSave_ = !Hwp && !Sqlite && Tab->originalAuthority_ && !Tab->input_.book_ && !Tab->recovered_ && !(Tab->input_.source_.attributes_ & 1u);
                if(Hwp || Sqlite || Tab->input_.viewId_=="epub")Tab->broker_=false;
                if (Tab->input_.book_) Tab->broker_ = false;
                SourceIdentity Source{Tab->input_.source_, {}, Tab->path_.toUtf8().toStdString(), Tab->sourceToken_.toStdString(),
                    (Tab->originalSave_ || Tab->broker_) && !Tab->recovered_};
                Tab->loading_=true;
                Tab->pane_->SetReadingSource(Tab->readingPath_,Tab->readingEntry_);
                Tab->pane_->ConfigureHwp(hwpRuntime_);
                if (Tab->input_.viewId_ == "epub") Tab->pane_->ConfigureEpub(epubRuntime_ ? *epubRuntime_ : MakeProductEpubRuntime(QCoreApplication::applicationDirPath()),!hosted_);
                const auto SqliteKey = std::exchange(Tab->newSqliteKey_, {});
                const bool Loaded=Tab->pane_->Load(Tab->input_,Tab->originalAuthority_,!hosted_||(!Tab->temporarySource_.isEmpty() && !MarkdownDocumentView::IsHtmlPath(Tab->input_.path_)),&SqliteKey);
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
                        if (Tab->pane_->ViewId() == "text") Tab->session_->AdoptTextSnapshot(Tab->pane_->CaptureTextSnapshot());
                        if (PdfRecovery) {
                            if (Tab->session_->NativeText()) Tab->session_->ApplyTextSnapshot(Tab->pane_->CaptureTextSnapshot());
                            else Tab->session_->Execute("document.setText", Serialized.bytes_.toStdString());
                            Tab->session_->MarkRecovered();
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
                            if (Tab->session_->NativeText()) Tab->session_->ApplyTextSnapshot(Tab->pane_->CaptureTextSnapshot());
                            else Tab->session_->Execute("document.setText", Serialized.bytes_.toStdString());
                            Tab->session_->MarkRecovered();
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
                        if(!Restore->markdownView_.isEmpty()&&Tab->pane_->IsRst())Tab->pane_->RunViewCommand("rst."+Restore->markdownView_);
                        if(Restore->imageMode_=="webtoon"||Restore->imageMode_=="comic")Tab->pane_->Execute({{"action","reading.mode"},{"mode",Restore->imageMode_=="webtoon"?2:1}});
                        if(Restore->epubMode_=="edit"&&!hosted_)Tab->pane_->Execute({{"action","epub.edit.begin"}});
                    }
                    if(const int Line=std::exchange(Tab->openLine_,0);Line>0)Tab->pane_->GoToLine(Line);
                    recordRecent(Tab);
                    if(auto* Input=Tab->pane_->InputWidget()){if(hosted_&&Tab->pane_->ViewId()=="text")Input->setAttribute(Qt::WA_NativeWindow);Input->installEventFilter(this);for(auto* Child:Input->findChildren<QWidget*>())Child->installEventFilter(this);}
                }
            }
            if(active()==Tab&&Tab->session_->Snapshot().lifecycle_==DocumentLifecycle::Ready){
                if(Tab->input_.viewId_=="image")findChild<ImageFullscreen*>("imageFullscreen")->Restore();
                setFocusProxy(Tab->pane_->InputWidget());
                const auto Native = reinterpret_cast<HWND>(winId());
                // A visible embedded preview loads while the file panel keeps
                // keyboard ownership. Only refocus after the user entered it.
                const bool MayFocus = !PreviewKeepsPanelKeyboard(this);
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
                        if (QStringList{"txt", "md", "markdown", "htm", "html", "rst"}.contains(Suffix)) Copy = QFileInfo(Path).dir().filePath(QFileInfo(Path).completeBaseName() + "." + Suffix);
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
        const auto Native = Tab->session_->NativeText();
        auto Started = Native ? Tab->session_->SaveTextSnapshot({SaveDisposition::Original, Tab->path_.toUtf8().toStdString()}, Tab->Revision(), *Native)
            : Tab->session_->Save({SaveDisposition::Original, Tab->path_.toUtf8().toStdString()});
        if (!Started.HasValue()) return Error(QString::fromStdString(Started.Failure().code_));
        Tab->pending_ = Started.Value();
        // Compatibility command replies are synchronous, but native text already owns exact encoded chunks.
        const auto Exported = [&]() -> storage::Result<QByteArray> {
            if (!Native) return Tab->pane_->Export(Tab->path_);
            if (const auto Failure = Native->SerializationError(); !Failure.empty())
                return storage::Error{storage::ErrorCategory::InvalidRequest, Failure};
            return QByteArray::fromStdString(Native->SerializedBytes());
        }();
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
    void DocumentWorkspaceWidget::prepareSaveAsync(const std::shared_ptr<Tab>& Tab)
    {
        if (!Tab || !Tab->session_->NativeText()) {
            const auto Result = prepareSave(Tab);
            if (Tab && Result.contains("error")) Tab->error_ = Result["error"].toString();
            update(); return;
        }
        if (Tab->loading_ || Tab->navigating_ || Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || !Tab->pendingToken_.isEmpty()) return;
        if (!Tab->broker_ || Tab->sourceToken_.isEmpty()) { Tab->error_ = "document-source-token-required"; update(); return; }
        const auto Snapshot = *Tab->session_->NativeText();
        auto Started = Tab->session_->SaveTextSnapshot({SaveDisposition::Original, Tab->path_.toUtf8().toStdString()}, Tab->Revision(), Snapshot);
        if (!Started.HasValue()) { Tab->error_ = QString::fromStdString(Started.Failure().code_); update(); return; }
        const auto Ticket = Started.Value();
        Tab->pending_ = Ticket; Tab->saving_ = true; Tab->error_.clear();
        Tab->cancellation_ = std::make_shared<std::stop_source>();
        const auto Cancellation = Tab->cancellation_;
        struct Prepared { QByteArray bytes_, hash_; std::optional<storage::Error> error_; };
        auto* Watcher = new QFutureWatcher<Prepared>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Ticket, Cancellation] {
            auto Result = Watcher->result(); Watcher->deleteLater(); Tab->saving_ = false;
            if (Tab->closed_ || Tab->session_->Snapshot().generation_ != Ticket.generation_) return;
            if (Cancellation->stop_requested()) Result.error_ = storage::Error{storage::ErrorCategory::Cancelled, "cancelled"};
            if (Result.error_) {
                Tab->session_->CompleteSave(Ticket, {false, {}, Result.error_});
                Tab->pending_.reset(); Tab->error_ = QString::fromStdString(Result.error_->code_);
            } else {
                Tab->pendingBytes_ = std::move(Result.bytes_); Tab->pendingHash_ = std::move(Result.hash_);
                Tab->pendingToken_ = QUuid::createUuid().toString(QUuid::Id128);
            }
            update(); tryFinishClose();
        });
        Watcher->setFuture(QtConcurrent::run([Snapshot, Cancellation, Unlimited = hosted_] {
            Prepared Result;
            if (const auto Error = Snapshot.SerializationError(); !Error.empty())
                Result.error_ = storage::Error{storage::ErrorCategory::InvalidRequest, Error};
            else if (!Unlimited && Snapshot.SerializedSize() > DocumentByteLimit)
                Result.error_ = storage::Error{storage::ErrorCategory::BudgetExceeded, "document-save-byte-budget"};
            else if (!Cancellation->stop_requested()) {
                Result.bytes_ = QByteArray::fromStdString(Snapshot.SerializedBytes());
                Result.hash_ = QCryptographicHash::hash(Result.bytes_, QCryptographicHash::Sha256).toHex();
            }
            return Result;
        }));
        update();
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
        Request->textSnapshot_ = Tab->session_->NativeText();
        if (!Request->textSnapshot_) Request->content_=Tab->session_->Text();
        Request->revision_=Tab->Revision();
        const auto Capture=[&]{
            std::optional<QByteArray> Content;std::optional<storage::Error> ContentError;
            if(Tab->pane_->ViewId()=="text" && !Request->textSnapshot_){Content=QByteArray::fromStdString(Request->content_);ContentError=Tab->serializationError_;}
            return Tab->pane_->CaptureSave(Destination,Original && SaveEpubOriginal,std::move(Content),std::move(ContentError));
        };
        if(Tab->saving_) {
            if(!Tab->activeSave_ || Tab->activeSave_->destination_!=Destination || Tab->activeSave_->overwrite_!=Overwrite){Tab->error_="document-save-target-busy";update();return;}
            // A repeated request supersedes the single queued snapshot, including
            // an undo back to the in-flight content. No unrequested typing is saved.
            if(Request->textSnapshot_ && Tab->activeSave_->textSnapshot_
                ? Request->textSnapshot_->SameSerializedRoot(*Tab->activeSave_->textSnapshot_)
                : !Request->textSnapshot_ && !Tab->activeSave_->textSnapshot_ && Request->content_==Tab->activeSave_->content_)Tab->queuedSave_.reset();
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
            SaveRequest Save{Request->original_?SaveDisposition::Original:SaveDisposition::SaveAs,Destination.toUtf8().toStdString()};
            auto Started = Request->textSnapshot_ ? Tab->session_->SaveTextSnapshot(std::move(Save), Request->revision_, *Request->textSnapshot_)
                : Tab->session_->SaveSnapshot(std::move(Save),Request->revision_,Request->content_);
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
                verifyTextEquality(Tab);
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
        hc::startup::Span InputTrace("input.recovery.checkpoint");
        if (hotExitClosing_ || hotExitPublishing_) return;
        if (sessionPublishing() && IsIdle()) publishHotExitSession(false);
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
            const auto Native = Tab->session_->NativeText();
            auto Bytes = Native ? std::string{} : Tab->pane_->ViewId() == "image" ? Tab->pane_->Content().toStdString() : Tab->session_->Text();
            if (!hosted_ && (Native ? qMax(Native->Size(), Native->SerializedSize()) : Bytes.size()) > DocumentByteLimit) { Tab->error_ = "document-recovery-budget"; continue; }
            RecoverySnapshot Record; Record.document_ = Tab->session_->Snapshot(); Record.bytes_ = std::move(Bytes);Record.viewId_=Tab->pane_->ViewId().toStdString();
            if (hotExitEligible(Tab)) {
                QByteArray Utf8; Record.text_ = Tab->pane_->CaptureTextRecovery(Utf8, !Native);
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
            auto Recovery = recovery_; Watcher->setFuture(QtConcurrent::run([Recovery, Native, Record = std::move(Record)]() mutable {
                if (Native) Record.bytes_ = Record.text_ ? Native->Bytes() : Native->SerializedBytes();
                return Recovery->Store(std::move(Record));
            }));
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
            {"closeViewerOnEscape", closeViewerOnEscape_}, {"closeRequested", viewerCloseRequested_}, {"workspaceRoot", workspaceRoot_}};
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
                        if (Tab->pane_->ViewId() == "text") Tab->session_->AdoptTextSnapshot(Tab->pane_->CaptureTextSnapshot());
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
            // The host's own foreground can also be one of its child windows (HyperCommander's file list).
            if (hosted_ && GetAncestor(Foreground, GA_ROOT) != GetAncestor(NativeWindow, GA_ROOT)) return {{"accepted", false}};
            if(auto Tab=active();Tab&&!Tab->loading_){
                if(!QApplication::activePopupWidget()&&!QApplication::activeModalWidget()){
                    QApplication::setActiveWindow(this);setFocusProxy(Tab->pane_->InputWidget());
                    const auto Native=reinterpret_cast<HWND>(winId());
                    // Qt tracks this workspace as its active shell, while Win32
                    // must keep the containing HC window active for keyboard input.
                    if(hosted_)SetActiveWindow(GetAncestor(Native,GA_ROOT));
                    // The native focus stays on this window and Qt hands the keys to the view's input. Never on the
                    // input's own HWND: a Markdown/reST preview or EPUB reader of another process then takes the
                    // keyboard from that container, and Qt counts it as gone from the application (no active
                    // window). A later switch to the source then has no focus to move, and keys stay in the
                    // hidden preview.
                    SetFocus(Native);
                    Tab->pane_->Focus();
                }
            }
            return {{"accepted",true}};
        }
        if (Action == "prepareViewer") {
            if (!hosted_ || !documents_.empty()) return Error("viewer-prepare-not-empty");
            PrepareDocumentTextRuntime(this);
            viewerCloseRequested_ = false;
            return {{"accepted", true}};
        }
        if (Action == "open" || Action == "openHost") {
            const auto Reply=open(Request["path"].toString(), Action == "openHost", Request["sourceToken"].toString(), Request["broker"].toBool(), {}, {}, Request["readPrefix"].toObject());
            if(!hosted_ && Request["copyOnly"].toBool())if(auto Tab=find(Reply["documentId"].toString())){Tab->originalAuthority_=false;Tab->originalSave_=false;}
            // 사용자가 연 파일(명령줄 · 두 번째 프로세스): 위 폴더의 워크스페이스를 찾는다(설계 6.3).
            if(!hosted_ && Action=="open" && !Reply.contains("error") && !Reply["duplicate"].toBool() && Request.contains("ancestors"))
                considerAncestors(Request["path"].toString(), Request["ancestors"].toString());
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
            else if((editRequest_["format"]=="epub-new" || editRequest_["format"]=="hwpx-new" || editRequest_["format"]=="explorer")&&!Request["accepted"].toBool())ShowShellError(tr("HyperEditor를 시작하지 못했습니다."));
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
            if (Tab->choosingSavePath_ || Tab->saving_ || Tab->composingSave_ || Tab->comparingText_ || !Tab->pendingToken_.isEmpty() || Tab->checkpointing_) return Error("document-close-busy");
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
            verifyTextEquality(Tab);
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
        for (const auto& Tab : documents_) {
            if (Tab->closed_ || !Tab->dock_) continue;
            const auto Name = documentName(*Tab);
            // The tab has its own tooltip. A tooltip on the dock would also appear over its contents.
            Tab->dock_->setTitle(Name + (Tab->Dirty() || (!Tab->loading_ && Tab->pane_->SqliteDirty()) ? " *" : "") + (Tab->loading_ ? tr(" · 여는 중") : "")
                + (Tab->session_->Snapshot().error_ || !Tab->error_.isEmpty() ? tr(" · 오류") : ""));
            // 다른 프로세스 창(미리보기 · EPUB · 한글)은 끄는 동안 grab()으로 그릴 수 없다: 끌기 미리보기는 틀만.
            Tab->dock_->setNativeContent(Tab->pane_->IsProcessDocument() || Tab->pane_->HasPreview());
            if (Tab->previewDock_) Tab->previewDock_->setTitle(tr("%1 미리보기").arg(Name));
            for (auto* Host : std::as_const(toolHosts_)) Host->RenameOwner(Tab->id_, Name);
        }
        collectToolPanels();
        auto Tab = active();
        if(epubFullscreen_ && (!Tab || !Tab->pane_->IsEpub()))toggleEpubFullscreen();
        // 미리보기 전체 화면: 옮겨 간 탭이 Markdown 이나 reST 면 그 미리보기를 이어서 보여 주고, 아니면 나온다.
        if(previewFullscreen_) {
            if(!Tab || Tab->loading_ || !Tab->pane_->HasPreview())togglePreviewFullscreen();
            else {
                for(const auto& Item:documents_)if(Item!=Tab && Item->pane_->IsPreviewOnly())Item->pane_->SetPreviewOnly(false);
                if(!Tab->pane_->IsPreviewOnly())Tab->pane_->SetPreviewOnly(true);
            }
        }
        if (Tab) {
            workspace_.Activate(Tab->id_.toStdString());
            if(!findChild<ImageFullscreen*>("imageFullscreen")->Active()) {
                auto Contribution=Tab->pane_->Contribution();
                if(Tab->pane_->IsEpub())Contribution.menuActions_=Tab->pane_->IsEpubEditing() && Tab->pane_->EpubEditor()?Tab->pane_->EpubEditor()->MenuActions():findChild<QMenu*>("epubCommands")->actions();
                chrome_->SetActive(std::move(Contribution));
            }
        }
        else chrome_->SetActive({});
        applyKind(Tab ? Tab->pane_->Kind() : DocumentViewKind::None);
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
        if(Hwp || (Tab && Tab->pane_->IsSqlite())){
            findChild<QAction*>("saveDocument")->setEnabled(!Hwp && !Tab->loading_ && Tab->pane_->SqliteDirty());findChild<QAction*>("saveDocumentAs")->setEnabled(false);
        }
        if(Tab&&Tab->pane_->IsEpub()){
            // Both commands apply and validate; only an authoritative local source permits original Save.
            // Draft writes and discovery do not mutate the live source and may overlap a save.
            const bool CanSave = !hosted_ && !Tab->loading_ && !Tab->saving_ && !Tab->choosingSavePath_ && Tab->pendingToken_.isEmpty() && Tab->pane_->CanPrepareEpubSave();
            findChild<QAction*>("saveDocument")->setEnabled(CanSave && Tab->originalSave_ || bool(Tab->activeSave_));findChild<QAction*>("saveDocumentAs")->setEnabled(CanSave);
        }
        if(auto* Reopen=findChild<QAction*>("epubReopenDocument"))Reopen->setVisible(Tab && Tab->pane_->IsEpub() && Tab->pane_->findChild<QAction*>("epubReopen"));
        findChild<QAction*>("printDocument")->setEnabled(Tab&&(!Tab->pane_->HasPreview()||Tab->pane_->PreviewSourceVisible())&&!Tab->pane_->IsEpub()&&!Tab->loading_&&!Tab->pane_->IsBusy()&&(!Hwp||Tab->pane_->ViewState()["ready"].toBool()));
        findChild<QAction*>("previewDocument")->setEnabled(Tab&&(!Tab->pane_->HasPreview()||Tab->pane_->PreviewSourceVisible())&&!Tab->pane_->IsEpub()&&!Hwp&&!Tab->loading_&&!Tab->pane_->IsBusy());
        for (auto* Action : findChildren<QAction*>()) {
            const auto Id = Action->property("documentCommandId").toString();
            const auto Scope=DocumentShortcutRegistry::ScopeOf(Id);
            if(Scope=="markdown" || Scope=="rst")Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->CanRunViewCommand(Id));
            if(Scope=="epub")Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->IsEpub() && !Tab->pane_->IsEpubEditing());
            // Editor-owned actions retain their resource/selection eligibility during status polling.
            if(Scope.startsWith("epub-edit") && Action->parent()==this)Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->IsEpubEditing());
            if(Scope=="image" || Scope=="image.comic" || Scope=="pdf")Action->setEnabled(Tab && !Tab->loading_);
            // 쿼리 탭 · 그리드 상태는 키를 누를 때 판정한다(라우터 조건 · RunViewCommand). 상태 갱신 타이머를 기다리지 않게 켜 둔다.
            if(Scope=="sqlite" || Scope.startsWith(QLatin1String("sqlite.")))Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->IsSqlite());
            if (Id.startsWith("text.") || QStringList{"edit.cut", "edit.copy", "edit.paste", "edit.selectAll"}.contains(Id))
                Action->setEnabled(Tab && !Tab->loading_ && ((Tab->pane_->ViewId() == "text"
                    && ((Id != "edit.cut" && Id != "edit.paste" && Id != "text.replace") || !Tab->pane_->IsReadOnly())) || Tab->pane_->IsEpubEditing()));
            if (Id == "file.new" || Id == "file.newText" || Id == "file.newMarkdown" || Id == "file.newRst" || Id == "file.newSphinxProject" || Id == "file.newImage")
                Action->setEnabled((hosted_ || documents_.size() < 16) && (tabbed_ || documents_.empty()));
            // HyperViewer hands a new EPUB to HyperEditor: no tab of its own.
            if (Id == "file.newEpub") Action->setEnabled(!epubCreating_ && (hosted_ || ((documents_.size() < 16) && (tabbed_ || documents_.empty()))));
            if(Tab&&Tab->pane_->HasPreview()&&!Tab->pane_->PreviewSourceVisible()&&(Id.startsWith("text.")||Id.startsWith("edit.")))Action->setEnabled(Id=="text.find"||Id=="edit.copy"||Id=="edit.selectAll");
            if (Id == "tab.next" || Id == "tab.previous") {
                int Open = 0; if (auto* Area = Tab ? Tab->dock_->dockArea() : nullptr) for (auto* Dock : Area->openDockWidgets()) Open += tabOf(Dock) && tabOf(Dock)->dock_ == Dock;
                Action->setEnabled(tabbed_ && Open > 1);
            }
        }
        QList<DocumentStatusField> Fields;
        // The overlay follows the active tab: a switch shows that tab's operation at once, or clears.
        if(const auto Current=Tab?Tab->id_:QString();progressOverlay_&&Current!=progressTab_){progressTab_=Current;progressOverlay_->ShowCurrent(Tab?Tab->progress_:DocumentProgress{});}
        if(Tab) {
            if(!Tab->loading_)Fields=Tab->pane_->StatusFields();
            const auto State=Tab->session_->Snapshot();const auto View=Tab->pane_->ViewState();
            const bool Reading=View.contains("reading") || (Tab->pane_->IsProcessDocument()&&!Tab->pane_->IsEpubEditing());
            const bool ReadOnly=(Tab->pane_->HasPreview()&&!Tab->pane_->PreviewSourceVisible()) || Reading || (Tab->pane_->ViewId()=="text"?View["readOnly"].toBool():!(Capability.save_||Capability.saveAs_));
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
            // SQLite 작업 창은 저장 대상이 없다(sqlite3 가 커밋으로 쓴다). 읽기 전용 표시는 작업 창이 낸다.
            if(!Tab->pane_->IsSqlite())Fields.append(DocumentStatusField{"mode",Mode,{},Destination,{},true,true});
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
            const auto Unsaved = tr("● 미저장");
            const bool EpubEditing = Tab->pane_->IsEpubEditing();
            if (Tab->Dirty()) MessageText += MessageText.isEmpty() ? Unsaved : tr(" · 미저장");
            if (EpubEditing || !MessageText.isEmpty()) {
                DocumentStatusField Status{"state", MessageText, {}, Detail, {}, true, true};
                // Keep the right-hand fields in place on the first edit and when undo makes the book clean again.
                if (EpubEditing) Status.widthSamples_.append(Unsaved);
                Fields.append(std::move(Status));
            }
        } else Fields.append(DocumentStatusField{"empty",shellError_.isEmpty()?tr("파일을 열거나 최근 항목을 선택하십시오."):shellError_});
        statusFieldNames_.clear();for(const auto& Field:Fields)statusFieldNames_.append({Field.id_,Field.text_});
        if(Tab){
            const auto Hidden=chrome_->Settings()["statusHidden"].toObject()[viewKinds::Name(CurrentKind())].toArray();
            if(!Hidden.isEmpty())Fields.erase(std::remove_if(Fields.begin(),Fields.end(),[&Hidden](const auto& Field){return Field.id_!="mode"&&Field.id_!="state"&&Hidden.contains(Field.id_);}),Fields.end());
        }
        statusBar_->SetFields(Fields);statusSlot_->show();
        placeStartPage(); syncToolbarRow();
        if (auto* Beside = findChild<QAction*>("view.previewBeside", Qt::FindDirectChildrenOnly))
            Beside->setEnabled(Tab && !Tab->loading_ && tabbed_ && (Tab->pane_->CanTakePreview() || Tab->previewDock_));
        findChild<ImageFullscreen*>("imageFullscreen")->UpdateStatus();
        refreshExplorer();
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
                Document.kind_ = MarkdownDocumentView::IsHtmlPath(Tab->path_) ? "html" : "markdown"; Document.markdownView_ = View["markdown"].toObject()["mode"].toString(); break;
            case DocumentViewKind::RstPreview: case DocumentViewKind::RstEdit:
                // 보기 방식은 Markdown 과 같은 칸( markdownView_ )에 적는다: preview · source · split.
                Document.kind_ = "rst"; Document.markdownView_ = View["rst"].toObject()["mode"].toString(); break;
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
            case DocumentViewKind::Sqlite: Document.kind_ = "sqlite"; break;
            default: Document.kind_ = "text"; break;
        }
        recent_->Record(std::move(Document));
    }
    void DocumentWorkspaceWidget::openRecent(const RecentDocument& Document)
    {
        // 최근 워크스페이스(폴더): HyperEditor 는 워크스페이스로 열고, HyperViewer 는 HyperEditor 에 넘긴다.
        if (Document.kind_ == QLatin1String("folder") && QFileInfo(Document.path_).isDir()) {
            if (!hosted_) OpenWorkspace(Document.path_);
            else if (editRequest_.isEmpty()) { editRequest_ = {{"token", QUuid::createUuid().toString(QUuid::Id128)}, {"format", "explorer"}, {"path", Document.path_}, {"folder", true}}; update(); }
            return;
        }
        if (!QFileInfo::exists(Document.path_)) {
            auto* Box = new hc::ui::MessageDialog(QMessageBox::Question, tr("최근 항목"),
                tr("%1을(를) 찾을 수 없습니다 — 목록에서 제거할까요?").arg(QDir::toNativeSeparators(Document.path_)), QMessageBox::Yes | QMessageBox::No, this);
            Box->setAttribute(Qt::WA_DeleteOnClose);
            connect(Box, &QMessageBox::finished, this, [this, Key = Document.Key()](int Result) { if (Result == QMessageBox::Yes && recent_) recent_->Remove(Key); });
            Box->open(); return;
        }
        pendingOpenEncoding_ = CanonicalEncodingName(Document.encoding_);
        // HyperViewer replaces its current document; HyperEditor opens a tab (or goes to the open one).
        const auto Reply = hosted_ ? open(Document.path_, true, {}, false) : openByUser(Document.path_);
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
        // 문서 묶음이 여럿이면 공통 동작을 합칠 문서 도구모음을 하나로 정할 수 없다: 공통 도구모음은 창 위 한 줄로 둔다.
        const bool Single = false;
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
        }
        Menu.exec(Global);
    }
    bool DocumentWorkspaceWidget::showDocumentMenu(fm::dock::DockWidget* Dock, const QPoint& Global)
    {
        // 도구 창과 ‘옆에 미리보기’ 탭은 도킹 관리자의 기본 메뉴(떼어 내기 · 자동 숨김 · 닫기)를 쓴다.
        using fm::dock::DropArea;
        const auto Target = tabOf(Dock);
        if (!Target || Target->dock_ != Dock) return false;
        activate(Target);
        auto* Area = Dock->dockArea();
        QList<fm::dock::DockWidget*> Group; if (Area) for (auto* Each : Area->openDockWidgets()) if (tabOf(Each)) Group.append(Each);
        const auto CloseDocks = [this](const QList<fm::dock::DockWidget*>& Docks) {
            for (auto* Each : Docks) if (auto Tab = tabOf(Each)) {
                if (Tab->dock_ != Each) { closePreviewBeside(Tab->id_); continue; }
                const auto At = std::find(documents_.begin(), documents_.end(), Tab); if (At != documents_.end()) closeTab(int(std::distance(documents_.begin(), At)));
            }
        };
        fm::ui::Menu Menu(this); Menu.setObjectName("documentTabMenu");
        if (auto* Close = findChild<QAction*>("closeDocument")) Menu.addAction(Close);
        const int Position = int(Group.indexOf(Dock));
        auto* Others = Menu.addAction(tr("다른 탭 닫기"), this, [CloseDocks, Group, Dock] { auto Rest = Group; Rest.removeAll(Dock); CloseDocks(Rest); });
        Others->setEnabled(Group.size() > 1);
        auto* Right = Menu.addAction(tr("오른쪽 탭 닫기"), this, [CloseDocks, Group, Position] { CloseDocks(Group.mid(Position + 1)); });
        Right->setEnabled(Position >= 0 && Position < Group.size() - 1);
        // 묶음 나누기 · 옮기기 · 떼어 내기: 끌어 놓기와 같은 결과를 메뉴로.
        Menu.addSeparator();
        const bool Movable = tabbed_ && Dock->hasFeature(fm::dock::DockWidget::Movable);
        const bool Splittable = Movable && Area && Area->openDockWidgetsCount() > 1;
        Menu.addAction(tr("오른쪽으로 분할"), this, [this, Dock] { if (auto* From = Dock->dockArea()) docks_->moveDockWidget(Dock, DropArea::Right, From); })->setEnabled(Splittable);
        Menu.addAction(tr("아래로 분할"), this, [this, Dock] { if (auto* From = Dock->dockArea()) docks_->moveDockWidget(Dock, DropArea::Bottom, From); })->setEnabled(Splittable);
        auto* Move = Menu.addMenu(tr("다른 묶음으로 옮기기"));
        QList<fm::dock::DockArea*> Targets;
        for (auto* Container : docks_->dockContainers()) for (auto* Each : Container->openedDockAreas()) {
            // openDockWidgets() 는 부를 때마다 새 목록이다: 한 목록의 처음과 끝을 써야 한다.
            const auto Open = Each->openDockWidgets();
            if (Each != Area && std::any_of(Open.cbegin(), Open.cend(), [this](auto* D) { return tabOf(D) != nullptr; })) Targets.append(Each);
        }
        for (auto* Each : Targets) {
            auto* Current = Each->currentDockWidget();
            const auto Label = Current ? Current->title() : tr("묶음");
            Move->addAction(Each->dockContainer() && Each->dockContainer()->isFloating() ? tr("%1 (떠 있는 창)").arg(Label) : Label, this,
                [this, Dock, Guard = QPointer<fm::dock::DockArea>(Each)] { if (Guard) docks_->moveDockWidget(Dock, DropArea::Center, Guard); });
        }
        Move->setEnabled(Movable && !Targets.isEmpty());
        Menu.addAction(tr("새 창으로 떼어 내기"), this, [Dock] { Dock->setFloating(); })
            ->setEnabled(Movable && Dock->hasFeature(fm::dock::DockWidget::Floatable) && !docks_->testConfigFlag(fm::dock::DockManager::NoTopLevelWindows)
                && !(Dock->isFloating() && Dock->dockContainer() && Dock->dockContainer()->openedDockWidgets().size() == 1));
        if (Target->pane_->HasPreview()) {
            Menu.addSeparator();
            if (auto* Beside = findChild<QAction*>("view.previewBeside", Qt::FindDirectChildrenOnly)) Menu.addAction(Beside);
        }
        Menu.addSeparator();
        const bool Local = Target->temporarySource_.isEmpty() && !Target->path_.isEmpty();
        const auto Native = QDir::toNativeSeparators(Target->path_);
        Menu.addAction(tr("경로 복사"), this, [Native] { QGuiApplication::clipboard()->setText(Native); })->setEnabled(Local);
        Menu.addAction(tr("파일 이름 복사"), this, [Native] { QGuiApplication::clipboard()->setText(QFileInfo(Native).fileName()); })->setEnabled(Local);
        if (!workspaceRoot_.isEmpty()) {
            const auto Relative = workspace::RelativeTo(workspaceRoot_, Target->path_);
            Menu.addAction(tr("상대 경로 복사"), this, [Relative] { QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(Relative)); })->setEnabled(Local && !Relative.isEmpty());
        }
        Menu.addAction(tr("탐색기 패널에서 찾기"), this, [this, Path = Target->path_] {
            if (!explorerTool_ || !explorer_) return;
            explorerTool_->toggleView(true); explorerTool_->bringToFront();
            explorer_->SetActiveDocument(Path); explorer_->RevealActive(true);
        })->setEnabled(Local && explorer_ != nullptr);
        Menu.addAction(tr("Windows 탐색기에서 위치 열기"), this, [Native] {
            // HyperCommander has no select-path command line contract yet; Explorer /select is the stand-in (plan 6.4).
            QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + Native});
        })->setEnabled(Local && QFileInfo::exists(Target->path_));
        Menu.addSeparator();
        if (auto* Reload = findChild<QAction*>("reloadDocument")) Menu.addAction(Reload);
        Menu.exec(Global);
        return true;
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
        // 시작 페이지는 중앙 영역의 빈 자리 위젯이다(DockManager::setCentralPlaceholderWidget): 중앙 영역이
        // 비면 저절로 보인다. 여기서는 최근 항목만 새로 읽힌 것으로 맞춘다.
        if (!startPage_) return;
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
        for (const auto& Url : Event->mimeData()->urls()) {
            if (!Url.isLocalFile()) continue;
            // 폴더는 워크스페이스로 연다(MRE 와 다름: 바로 아래 파일을 열지 않는다).
            if (QFileInfo(Url.toLocalFile()).isDir()) { OpenWorkspace(Url.toLocalFile()); continue; }
            if (openByUser(Url.toLocalFile()).contains("error")) Rejected.append(Url.toLocalFile());
        }
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
        // Closing parks a reader's window with the keyboard still in it.
        if(active()==Tab)releaseFocusFrom(Tab);
        if(!Tab->pane_->RequestClose()){Tab->closeRequested_=true;return;}
        if(Tab->everReady_ && !Tab->loading_)recordRecent(Tab);
        if(editRequest_["documentId"]==Tab->id_)editRequest_={};
        // An EPUB keeps its edits in the EPUB draft (kept by PreserveRecovery); its session closes as discarded.
        const auto Disposition = PreserveRecovery && hotExitReady_ && Tab->Dirty() && !Tab->pane_->IsEpub() ? CloseDisposition::PreserveSnapshot
            : Discard ? CloseDisposition::Discard : CloseDisposition::KeepChanges;
        if (!workspace_.Remove(Tab->id_.toStdString(), Disposition).HasValue()) return;
        Tab->cancellation_->request_stop(); Tab->closed_ = true; Tab->pane_->Cancel();
        // 옆 미리보기 · 문서 도크를 닫아 탭을 바로 없앤다. 지우기는 나중에(칸과 함께).
        if (auto* Preview = Tab->previewDock_.data()) { Tab->previewDock_.clear(); Preview->toggleView(false); Preview->deleteLater(); }
        const bool WasCurrent = currentDock_ == Tab->dock_;
        Tab->dock_->toggleView(false); Tab->dock_->deleteLater();
        Tab->pane_->deleteLater(); std::erase(documents_, Tab);
        if (WasCurrent) { currentDock_.clear(); pickCurrentDocument(); }
        if(!Tab->temporarySource_.isEmpty())QFile::remove(Tab->temporarySource_);
        if (!PreserveRecovery) cleanupRecovery(Tab, Tab->Revision());
    }
    void DocumentWorkspaceWidget::closeTab(int Index)
    {
        if (hotExitClosing_ || closePromptActive_ || Index < 0 || Index >= int(documents_.size())) return;
        auto Target = documents_[Index];
        Target->closeRequested_ = true;
        if(Target->pane_->IsProcessDocument()&&!Target->pane_->CanEditEpub()){
            if(active()==Target)releaseFocusFrom(Target);
            if(!Target->pane_->RequestClose())return;
        }
        if (Target->choosingSavePath_ || Target->saving_ || Target->composingSave_ || Target->comparingText_ || !Target->pendingToken_.isEmpty() || Target->checkpointing_ || Target->pane_->IsEditing()
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


    void DocumentWorkspaceWidget::ShowRecoveryManager(const QString& Filter)
    {
        if (hotExitClosing_) return;
        if (auto* Open = qobject_cast<RecoveryManagerDialog*>(recoveryManager_.data())) {
            if (!Filter.isEmpty()) Open->SetFilter(Filter);
            Open->Reload(); Open->raise(); Open->activateWindow(); return;
        }
        RecoveryManagerDialog::Hooks Hooks;
        // This window's own documents are in use even before the next session publication says so.
        Hooks.inUse_ = [this](const recovery::Item& Entry) {
            if (Entry.kind_ == recovery::Kind::Epub) {
                for (const auto& Tab : documents_)
                    if (Tab->pane_->IsEpub() && QFileInfo(Tab->pane_->EpubDraftPath()).fileName() == QFileInfo(Entry.path_).fileName()) return true;
                return false;
            }
            return Entry.kind_ != recovery::Kind::Archive && find(Entry.key_) != nullptr;
        };
        Hooks.open_ = [this](const recovery::Item& Entry) -> QString {
            QJsonObject Reply;
            if (Entry.kind_ == recovery::Kind::Epub) Reply = openEpubRecovery(Entry.path_);
            else if (Entry.kind_ == recovery::Kind::Archive) Reply = OpenArchiveDraftCopy(Entry.key_);
            else {
                // Hold the writing window's session while the copy is open here, so HotExit cannot reopen it twice.
                std::shared_ptr<QLockFile> Lease;
                if (!Entry.owner_.isEmpty()) {
                    Lease = std::make_shared<QLockFile>(recovery_->SessionLockPath(Entry.owner_)); Lease->setStaleLockTime(0);
                    if (!Lease->tryLock()) return tr("다른 창에서 사용 중입니다.");
                }
                Reply = Handle({{"action", "recover"}, {"path", Entry.path_}});
                if (!Reply.contains("error") && Lease) manualRecoveryLocks_.push_back(std::move(Lease));
            }
            return Reply.contains("error") ? Message(Reply["error"].toString()) : QString{};
        };
        auto* Dialog = new RecoveryManagerDialog(recoveryRoot_, recovery::PolicyFrom(appliedPreferences_), std::move(Hooks),
            preferences_ ? static_cast<QWidget*>(preferences_.data()) : this);
        Dialog->setAttribute(Qt::WA_DeleteOnClose); recoveryManager_ = Dialog;
        if (!Filter.isEmpty()) Dialog->SetFilter(Filter);
        connect(Dialog, &RecoveryManagerDialog::SettingsRequested, this, [this] {
            showPreferences(); if (preferences_) preferences_->ShowPage("recovery");
        });
        Dialog->open();
    }
    bool DocumentWorkspaceWidget::hotExitEnabled() const
    {
        const auto Text = appliedPreferences_["documentViews"].toObject()["textView"].toObject();
        return !hosted_ && Text["hotExitPolicyVersion"].toInt() == 1 && Text["hotExitEnabled"].toBool(true);
    }
    bool DocumentWorkspaceWidget::sessionPublishing() const
    {
        return !hosted_ && (hotExitEnabled() || !restoredOriginals_.empty() || PreferenceValue(appliedPreferences_, "textView/recoveryEnabled").toBool());
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
            && Tab->session_->ByteSize() <= DocumentByteLimit && !Tab->pane_->IsBusy();
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
        if (hotExitPublishing_ || !IsIdle() || (!Closing && !sessionPublishing())) return;
        if (!hotExitLock_) { if (Closing) cancelHotExitClose(tr("복구 폴더 잠금을 얻지 못했습니다.")); return; }
        const bool Periodic = PreferenceValue(appliedPreferences_, "textView/recoveryEnabled").toBool();
        if (!Closing && !Periodic && restoredOriginals_.empty()) return;
        if (std::any_of(documents_.begin(), documents_.end(), [](const auto& Tab) { return Tab->hotExitRestore_ && (!Tab->everReady_ || !Tab->error_.isEmpty()); })) {
            if (Closing) cancelHotExitClose(tr("복원에 실패한 문서를 먼저 확인하십시오. 기존 사본은 유지합니다.")); return;
        }
        QJsonArray Items; std::vector<RecoverySnapshot> Captures;
        QHash<QString, TextSnapshot> TextCaptures;
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
            const auto Native = Tab->session_->NativeText();
            QByteArray Bytes; auto Text = Tab->pane_->CaptureTextRecovery(Bytes, Closing && Tab->Dirty() && !Native);
            if (!Text) { if (Closing && Tab->Dirty()) { cancelHotExitClose(tr("완전한 텍스트를 읽을 수 없습니다.")); return; } continue; }
            QJsonObject Item{{"documentId", Tab->id_}, {"path", Tab->temporarySource_.isEmpty() ? Tab->path_ : QString{}}};
            if (Tab->Dirty()) {
                if ((Native ? Native->Size() : Bytes.size()) > DocumentByteLimit) { if (Closing) cancelHotExitClose(tr("복구 사본 크기 제한을 초과했습니다.")); return; }
                if (Closing) {
                    // Caret/encoding are part of the final capture even without another content edit.
                    Tab->session_->AdvanceRecoveryRevision();
                    RecoverySnapshot Capture; Capture.document_ = Tab->session_->Snapshot(); Capture.text_ = Text;
                    Capture.bytes_ = Bytes.toStdString(); Capture.ownerWindow_ = workspaceId_.toStdString();
                    if (Native) TextCaptures.insert(Tab->id_, *Native);
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
        // 워크스페이스 창은 그 루트에 다시 묶인다(문서는 HotExit 가 되살리고, 워크스페이스 상태는 탐색기 · 배치만).
        if (!workspaceRoot_.isEmpty()) Manifest["workspaceRoot"] = workspaceRoot_;
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
            // The replacement manifest is durable before retiring the imported session's snapshots. Only the
            // restored documents' copies go; a document that did not restore keeps its copy in the recovery list.
            if (!restoredOriginals_.empty()) {
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
        // The previous session ends once this window's manifest is durable, also after a partial restore:
        // keeping it would reopen the restored documents a second time at the next start.
        const auto RetireSession = restoringSessionId_;
        Watcher->setFuture(QtConcurrent::run([Store, Id, RetireSession, Manifest, Captures = std::move(Captures), TextCaptures = std::move(TextCaptures)]() mutable ->storage::Result<Published> {
            std::vector<RecoverySnapshot> Receipts; auto Entries = Manifest["documents"].toArray();
            for (auto& Capture : Captures) {
                if (const auto Found = TextCaptures.constFind(QString::fromStdString(Capture.document_.id_)); Found != TextCaptures.cend())
                    Capture.bytes_ = Found->Bytes();
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
    bool DocumentWorkspaceWidget::RestoreHotExit(const QString& SessionId, const std::optional<QStringList>& Documents)
    {
        // The caller decides: HotExit reopens a cleanly closed window, the restart prompt an abnormally ended one.
        if (hosted_ || !documents_.empty()) return false;
        auto Lock = std::make_shared<QLockFile>(recovery_->SessionLockPath(SessionId)); Lock->setStaleLockTime(0);
        if (recovery_->SessionLockPath(SessionId).isEmpty() || !Lock->tryLock()) return false;
        restoringLock_ = std::move(Lock); restoringSessionId_ = SessionId;
        auto Store = recovery_; ++recoveryWorkers_;
        auto* Watcher = new QFutureWatcher<storage::Result<QJsonObject>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Documents] {
            auto Result = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_;
            if (!Result.HasValue()) { restoringLock_.reset(); restoringSessionId_.clear(); ShowShellError(tr("복구 세션을 읽지 못했습니다: %1").arg(QString::fromStdString(Result.Failure().code_))); return; }
            const auto Manifest = Result.Value();
            if (const auto Root = Manifest["workspaceRoot"].toString(); !Root.isEmpty() && QFileInfo(Root).isDir()) OpenWorkspace(Root, WorkspaceOrigin::HotExit);
            restoredActive_ = Manifest["activeDocumentId"].toString(); pendingHotExitRestore_ = true;
            for (const auto& Value : Manifest["documents"].toArray()) {
                const auto Item = Value.toObject(); QJsonObject Reply; const auto Kind = Item["kind"].toString();
                // Unchosen documents with preserved edits stay in the recovery list; unchanged ones reopen.
                const bool Preserved = !Item["snapshot"].toString().isEmpty() || !Item["draft"].toString().isEmpty();
                if (Documents && Preserved && !Documents->contains(Item["documentId"].toString())) continue;
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
        // 창을 닫는 모든 길(X · HotExit · 빠른 보기 닫기)이 여기를 지난다: 다음에 열 때 도구 창 자리를 그대로.
        storeDockSettings();
        // 워크스페이스 상태는 문서를 닫기 전에 남긴다(닫기를 취소해도 해가 없다).
        if (!workspaceRoot_.isEmpty() && !hotExitReady_) saveWorkspace();
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
void DocumentWorkspaceWidget::openSearch(bool Replace, const QString& Root) {
    auto Tab = active(); if (!Tab) Tab = find(searchDocument_);
    const bool Usable = Tab && !Tab->loading_ && (Tab->pane_->ViewId() == "text" || Tab->pane_->IsMarkdown() || Tab->pane_->IsEpub());
    // EPUB 은 책 안에서 찾는다. 그 밖에는 고른 폴더 · 워크스페이스 루트 · 문서 폴더 순(워크스페이스는 문서 없이도 찾는다).
    const bool Book = Usable && Root.isEmpty() && Tab->pane_->IsEpub();
    const auto Folder = !Root.isEmpty() ? Root : Book ? QString() : !workspaceRoot_.isEmpty() ? workspaceRoot_ : Usable ? QFileInfo(Tab->path_).absolutePath() : QString();
    if (!Book && Folder.isEmpty()) return;
    if(Book && Replace && !hosted_ && !Tab->pane_->EpubEditor())Tab->pane_->Execute({{"action","epub.edit.begin"}});
    const bool ChangedBook=Book && searchDocument_!=Tab->id_; if (Usable) searchDocument_ = Tab->id_;
    if (!searchPanel_) {
        // The ‘파일에서 찾기’ tool window exists from the start (its place is part of the saved layout); the panel joins it now.
        auto* Host = searchTool_->widget();
        searchPanel_ = new search::Panel(Host); Host->layout()->addWidget(searchPanel_); shortcutRouter_->AddScope(searchTool_);
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
        connect(searchPanel_, &search::Panel::SummaryChanged, this, [this](const QString& Summary) { searchTool_->setToolTip(Summary); });
        connect(searchPanel_, &search::Panel::CloseRequested, this, [this] { searchTool_->toggleView(false); });
    }
    const auto Location = searchLocation();
    searchPanel_->setProperty("configuredLocation", Location);
    // The configured place applies the first time; afterwards the tool window reopens where the user left it.
    if (searchPlacement_.isEmpty()) placeSearch(Location);
    else { searchTool_->toggleView(true); searchTool_->bringToFront(); }
    searchPanel_->SetReadOnly(hosted_);
    searchPanel_->Open(Replace, Book ? Tab->path_ : Folder, Book, searchSources());
    if (!Root.isEmpty()) searchPanel_->SetRoot(Root);
    if(ChangedBook)searchPanel_->SourcesChanged();
}
void DocumentWorkspaceWidget::placeSearch(const QString& Requested) {
    if (!searchPanel_ || !searchTool_) return;
    using fm::dock::DropArea;
    // ‘위치’: 하단 패널 = 아래 도구 창(문제 · 출력과 같은 묶음), 콘텐츠 탭 = 중앙 문서 탭, 분리 창 = 떠 있는 창.
    // 임베드(최상위 창 없음)에서는 분리 창 대신 하단 패널이다.
    auto Location = Requested == QLatin1String("epubProblems") ? QStringLiteral("bottom") : Requested;
    if (Location == QLatin1String("floating") && docks_->testConfigFlag(fm::dock::DockManager::NoTopLevelWindows)) Location = QStringLiteral("bottom");
    searchPlacement_ = Location;
    if (searchTool_->isAutoHidden()) docks_->setAutoHidden(searchTool_, false);
    if (Location == QLatin1String("tab")) {
        // The panel's tab hides the document; a reader there gives up the keyboard first.
        releaseFocusFrom(active());
        auto Tab = active(); auto* Area = Tab ? Tab->dock_->dockArea() : nullptr;
        if (Area && Area->dockContainer() == docks_->dockContainer()) docks_->moveDockWidget(searchTool_, DropArea::Center, Area);
        else docks_->moveDockWidget(searchTool_, DropArea::Center);
    } else if (Location == QLatin1String("floating")) {
        const auto Size = QSize(1000, 600);
        const auto Center = mapToGlobal(rect().center());
        if (!searchTool_->isFloating()) docks_->addFloatingDockWidget(searchTool_, QRect(Center - QPoint(Size.width() / 2, Size.height() / 2), Size));
    } else {
        fm::dock::DockArea* Bottom = nullptr;
        for (const auto& Kind : {QStringLiteral("problems"), QStringLiteral("output")})
            if (auto* Tool = tools_.value(Kind); Tool && !Tool->isClosed() && !Tool->isAutoHidden() && !Tool->isFloating() && Tool->dockArea() && !Tool->dockArea()->isCentral()) { Bottom = Tool->dockArea(); break; }
        if (Bottom) docks_->moveDockWidget(searchTool_, DropArea::Center, Bottom, 0);
        else {
            docks_->moveDockWidget(searchTool_, DropArea::Bottom);
            if (searchTool_->dockArea()) docks_->setAreaSize(searchTool_->dockArea(), Qt::Vertical, qBound(220, height() * 30 / 100, 420));
        }
    }
    searchTool_->toggleView(true); searchTool_->bringToFront(); searchPanel_->show();
}
void DocumentWorkspaceWidget::OpenSearchContext(const QJsonObject& Context) {
    openSearch(true); if(searchPanel_)searchPanel_->RestoreContext(Context);
}
QString DocumentWorkspaceWidget::searchLocation() const {
    const auto Value = appliedPreferences_["documentViews"].toObject()["textView"].toObject()["findInFiles"].toObject()["openLocation"].toString();
    // Same default as the settings dialog shows: a file written before this key existed opens a content tab.
    return Value.isEmpty() ? hc::defaults::edit::ViewValue("textView/findInFiles/openLocation").toString() : Value;
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

// ============================================================================================ 도킹
namespace hc::document {
namespace {
// 배치 상태의 판(fmdock saveState userVersion). 도구 창 구성이 바뀌면 올린다 — 옛 상태는 기본 배치가 된다.
constexpr int DockLayoutVersion = 1;
constexpr fm::dock::DockWidget::Features LayoutLock = fm::dock::DockWidget::Movable | fm::dock::DockWidget::Floatable | fm::dock::DockWidget::Pinnable;
}
void DocumentWorkspaceWidget::buildDocks()
{
    using namespace fm::dock;
    docks_ = new DockManager(this); docks_->setObjectName("documentDocks");
    // 임베드(HyperViewer 빠른 보기)는 다른 창 안에 있다: 떠 있는 창을 만들지 않는다(07 §7.2).
    if (hosted_) docks_->setConfigFlag(DockManager::NoTopLevelWindows);
    docks_->setShortcutSource(this);
    docks_->setDockMenuHook([this](DockWidget* Dock, const QPoint& Global) { return showDocumentMenu(Dock, Global); });
    connect(docks_, &DockManager::focusedDockWidgetChanged, this, [this](DockWidget*, DockWidget* Now) { if (auto Tab = tabOf(Now)) setCurrentDocument(Tab->dock_); });
    connect(docks_, &DockManager::dockAreaCreated, this, &DocumentWorkspaceWidget::watchArea);
    docks_->ensureCentralArea();
    toolHosts_.insert(QStringLiteral("outline"), new DocumentToolHost(tr("이 문서에는 개요가 없습니다."), false));
    toolHosts_.insert(QStringLiteral("bookTree"), new DocumentToolHost(tr("EPUB을 편집하면 책 구조가 여기에 나옵니다."), false));
    toolHosts_.insert(QStringLiteral("problems"), new DocumentToolHost(tr("이 문서에는 문제 목록이 없습니다."), false));
    toolHosts_.insert(QStringLiteral("output"), new DocumentToolHost(tr("빌드 로그 · EPUBCheck 출력이 여기에 나옵니다."), true));
    for (auto At = toolHosts_.cbegin(); At != toolHosts_.cend(); ++At) At.value()->setObjectName(QStringLiteral("documentTool.") + At.key());
    tools_.insert(QStringLiteral("outline"), makeTool(QStringLiteral("outline"), tr("개요"), toolHosts_["outline"]));
    tools_.insert(QStringLiteral("bookTree"), makeTool(QStringLiteral("bookTree"), tr("책 구조"), toolHosts_["bookTree"]));
    tools_.insert(QStringLiteral("problems"), makeTool(QStringLiteral("problems"), tr("문제"), toolHosts_["problems"]));
    tools_.insert(QStringLiteral("output"), makeTool(QStringLiteral("output"), tr("출력"), toolHosts_["output"]));
    auto* SearchHost = new QWidget; SearchHost->setObjectName("documentSearchHost");
    auto* SearchLayout = new QVBoxLayout(SearchHost); SearchLayout->setContentsMargins(0, 0, 0, 0); SearchLayout->setSpacing(0);
    searchTool_ = makeTool(QStringLiteral("search"), tr("파일에서 찾기"), SearchHost);
    buildExplorer();
    if (hosted_) {
        // 좁은 패널: 도구 창은 사이드바(자동 숨김)에서 시작한다. 탐색기 · 개요 · 파일에서 찾기만 처음부터 탭이 보인다.
        docks_->addAutoHideDockWidget(SideBarLocation::Left, explorerTool_);
        docks_->addAutoHideDockWidget(SideBarLocation::Left, tools_["outline"]);
        docks_->addAutoHideDockWidget(SideBarLocation::Left, tools_["bookTree"]);
        docks_->addAutoHideDockWidget(SideBarLocation::Bottom, searchTool_);
        docks_->addAutoHideDockWidget(SideBarLocation::Bottom, tools_["problems"]);
        docks_->addAutoHideDockWidget(SideBarLocation::Bottom, tools_["output"]);
        for (auto* Dock : {tools_["bookTree"], tools_["problems"], tools_["output"]}) Dock->toggleView(false);
    } else {
        // 시안: 아래 = 파일에서 찾기 | 문제(문서 열 아래), 왼쪽 위 = 탐색기, 왼쪽 아래 = 개요 | 책 구조, 출력 = 아래 자동 숨김.
        // 탐색기는 기본 기능이라 처음부터 열려 있다. 나머지는 출력 말고 닫혀 있다가 문서가 원할 때(개요 켜짐 · EPUB 편집 · 찾기) 열린다.
        auto* Bottom = docks_->addDockWidget(DropArea::Bottom, searchTool_);
        docks_->addDockWidgetTab(tools_["problems"], Bottom);
        auto* Left = docks_->addDockWidget(DropArea::Left, explorerTool_);
        auto* Below = docks_->addDockWidget(DropArea::Bottom, tools_["outline"], Left);
        docks_->addDockWidgetTab(tools_["bookTree"], Below);
        docks_->setAreaSize(Bottom, Qt::Vertical, 200);
        docks_->setAreaSize(Left, Qt::Horizontal, 260);
        docks_->addAutoHideDockWidget(SideBarLocation::Bottom, tools_["output"]);
        for (auto* Dock : {tools_["outline"], tools_["bookTree"], tools_["problems"], searchTool_}) Dock->toggleView(false);
    }
    dockDefaultLayout_ = docks_->saveState(DockLayoutVersion);
    docks_->setDefaultLayout(dockDefaultLayout_);
    // 다른 프로세스 창(미리보기 · EPUB · 한글)에 간 포커스는 Qt가 모른다: 그 창의 문서를 포커스 문서로 삼는다.
    nativeFocus_ = new QTimer(this); nativeFocus_->setObjectName("documentNativeFocus"); nativeFocus_->setInterval(250);
    connect(nativeFocus_, &QTimer::timeout, this, &DocumentWorkspaceWidget::pollNativeFocus);
    nativeFocus_->start();
}
fm::dock::DockWidget* DocumentWorkspaceWidget::makeTool(const QString& Kind, const QString& Title, QWidget* Content)
{
    using fm::dock::DockWidget;
    auto* Dock = new DockWidget(QStringLiteral("tool:") + Kind, Title, this);
    Dock->setWidget(Content, DockWidget::InsertMode::ForceNoScrollArea);
    Dock->setProperty("hc.toolKind", Kind);
    connect(Dock, &DockWidget::viewToggled, this, [this, Kind](bool Open) {
        if (toolSyncing_ || !docks_ || docks_->isRestoringState()) return;
        // 사용자가 도구 창을 열거나 닫았다: 포커스 문서 화면의 켜기 · 끄기(개요 단추 등)를 맞춘다. 포커스 문서는 동작을
        // 실행해 설정까지 남기고, 나머지 문서는 표시만 끈다(다른 문서로 옮겨도 다시 열리지 않게).
        const QScopedValueRollback Guard(toolSyncing_, true);
        const auto Current = active();
        for (const auto& Tab : documents_) for (const auto& Item : Tab->toolPanels_) {
            if (Item.kind_ != Kind || !Item.toggle_ || Item.toggle_->isChecked() == Open) continue;
            if (Tab == Current) Item.toggle_->trigger();
            else if (!Open) { const QSignalBlocker Block(Item.toggle_); Item.toggle_->setChecked(false); }
        }
    });
    if (Kind == QLatin1String("search"))
        connect(Dock, &DockWidget::visibilityChanged, this, [this](bool Visible) {
            if (Visible && !searchPanel_) QTimer::singleShot(0, this, [this] { if (!searchPanel_) openSearch(false); });
        });
    return Dock;
}
void DocumentWorkspaceWidget::watchArea(fm::dock::DockArea* Area)
{
    if (!Area || Area->property("hc.watched").toBool()) return;
    Area->setProperty("hc.watched", true);
    connect(Area, &fm::dock::DockArea::currentChanged, this, [this, Guard = QPointer<fm::dock::DockArea>(Area)] {
        if (!Guard || docks_->isRestoringState()) return;
        // 포커스 문서가 든 묶음에서 탭을 바꿨거나(그 문서가 가려짐) 마지막으로 쓴 문서 묶음이면 그 앞 탭을 따른다.
        const auto Tab = tabOf(Guard->currentDockWidget());
        const bool Holds = currentDock_ && currentDock_->dockArea() == Guard;
        if (Tab && (Holds || Guard == docks_->lastActiveCentralArea() || !currentDock_)) setCurrentDocument(Tab->dock_);
        else if (Holds && !currentDock_->isCurrentTab()) pickCurrentDocument();
    });
}
void DocumentWorkspaceWidget::setCurrentDocument(fm::dock::DockWidget* Dock)
{
    const auto Tab = tabOf(Dock);
    if (!Tab || Tab->dock_ == currentDock_) return;
    currentDock_ = Tab->dock_;
    bool Refocus = std::exchange(focusReleased_, false);
    // Fallback for switches that did not release first: a hidden foreign view that kept the Win32
    // focus still receives every key, including the ones that would switch back.
    const auto Native = reinterpret_cast<HWND>(window()->winId()); const auto Root = GetAncestor(Native, GA_ROOT);
    if (const auto Focus = GetFocus(); Root && Focus && !IsWindowVisible(Focus) && GetWindowThreadProcessId(Focus, nullptr) != GetCurrentThreadId()
        && GetAncestor(GetForegroundWindow(), GA_ROOT) == Root && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
        QApplication::setActiveWindow(window());
        if (GetWindowThreadProcessId(Root, nullptr) == GetCurrentThreadId()) SetActiveWindow(Root);
        SetFocus(Native); Refocus = true;
    }
    setFocusProxy(Tab->pane_->InputWidget());
    if (Refocus || (!Tab->loading_ && (Tab->pane_->ViewId() == "pdf" || Tab->pane_->IsProcessDocument()) && !Tab->pane_->isAncestorOf(QApplication::focusWidget())))
        Tab->pane_->Focus();
    syncToolHosts();
    update();
}
void DocumentWorkspaceWidget::pickCurrentDocument()
{
    // 포커스 문서가 닫혔거나 가려졌다: 마지막 문서 묶음의 앞 탭, 없으면 보이는 아무 문서.
    std::shared_ptr<Tab> Next;
    if (auto* Area = docks_->lastActiveCentralArea()) Next = tabOf(Area->currentDockWidget());
    if (!Next) for (const auto& Each : documents_) if (!Each->closed_ && !Each->dock_->isClosed() && Each->dock_->isCurrentTab()) { Next = Each; break; }
    if (!Next) for (const auto& Each : documents_) if (!Each->closed_) { Next = Each; break; }
    if (Next) { setCurrentDocument(Next->dock_); return; }
    currentDock_.clear(); setFocusProxy(nullptr); syncToolHosts(); update();
}
void DocumentWorkspaceWidget::collectToolPanels()
{
    for (const auto& Tab : documents_) {
        if (Tab->closed_ || Tab->loading_ || !Tab->pane_) continue;
        const auto Items = Tab->pane_->TakeToolPanels();
        if (Items.isEmpty()) continue;
        const auto Name = documentName(*Tab);
        for (const auto& Item : Items) {
            auto* Host = toolHosts_.value(Item.kind_); if (!Host || !Item.widget_) continue;
            Host->Add(Tab->id_, Name, Item.widget_, Item.label_);
            // 화면의 켜기 · 끄기(개요 단추 · 진단 패널 · 책 구조 패널)는 이 문서가 포커스일 때 도구 창을 연다 · 닫는다.
            if (Item.toggle_) connect(Item.toggle_.data(), &QAction::toggled, this, [this, Kind = Item.kind_, Id = Tab->id_](bool On) {
                if (toolSyncing_) return;
                const auto Owner = find(Id); if (!Owner || active() != Owner) return;
                showTool(Kind, On, On);
            });
            Tab->toolPanels_.append(Item);
        }
        if (Tab == active()) syncToolHosts();
    }
}
void DocumentWorkspaceWidget::syncToolHosts()
{
    const auto Tab = active();
    for (auto* Host : std::as_const(toolHosts_)) Host->SetActiveOwner(Tab ? Tab->id_ : QString());
    if (!Tab) return;
    // 포커스 문서가 켜 둔 칸은 도구 창을 연다. 닫지는 않는다 — 다른 문서로 옮겨도 배치가 흔들리지 않게.
    for (const auto& Item : Tab->toolPanels_) if (Item.toggle_ && Item.toggle_->isChecked()) showTool(Item.kind_, true);
}
void DocumentWorkspaceWidget::showTool(const QString& Kind, bool Show, bool Front)
{
    auto* Dock = Kind == QLatin1String("search") ? searchTool_ : tools_.value(Kind);
    if (!Dock) return;
    const QScopedValueRollback Guard(toolSyncing_, true);
    if (!Show) { if (!Dock->isClosed()) Dock->toggleView(false); return; }
    if (Dock->isClosed()) Dock->toggleView(true);
    // 앞으로: 탭이면 그 탭을, 자동 숨김이면 펼친다.
    if (Front) Dock->bringToFront();
}
void DocumentWorkspaceWidget::fillToolMenu(QMenu* Menu)
{
    Menu->clear();
    const auto Tab = active();
    const auto KeyOf = [this, Tab](const QString& Kind) -> QString {
        if (Kind == QLatin1String("search")) { auto* Find = findChild<QAction*>("document.findInFiles", Qt::FindDirectChildrenOnly); return Find ? Find->shortcut().toString(QKeySequence::NativeText) : QString(); }
        if (Tab) for (const auto& Item : Tab->toolPanels_) if (Item.kind_ == Kind && Item.toggle_) return Item.toggle_->shortcut().toString(QKeySequence::NativeText);
        return {};
    };
    const std::array<std::pair<QString, QString>, 6> Items{{{QStringLiteral("explorer"), tr("탐색기")}, {QStringLiteral("outline"), tr("개요")}, {QStringLiteral("bookTree"), tr("책 구조")},
        {QStringLiteral("problems"), tr("문제")}, {QStringLiteral("output"), tr("출력")}, {QStringLiteral("search"), tr("파일에서 찾기")}}};
    for (const auto& [Kind, Label] : Items) {
        auto* Dock = Kind == QLatin1String("search") ? searchTool_ : tools_.value(Kind);
        // 빠른 보기에는 탐색기가 없다.
        if (!Dock) continue;
        auto Key = KeyOf(Kind);
        if (Kind == QLatin1String("explorer")) if (auto* Go = findChild<QAction*>("view.explorer", Qt::FindDirectChildrenOnly)) Key = Go->shortcut().toString(QKeySequence::NativeText);
        auto* Item = Menu->addAction(Key.isEmpty() ? Label : Label + QLatin1Char('\t') + Key);
        Item->setObjectName(QStringLiteral("view.tool.") + Kind);
        Item->setCheckable(true); Item->setChecked(Dock && !Dock->isClosed());
        connect(Item, &QAction::triggered, this, [this, Kind](bool On) {
            if (Kind == QLatin1String("search") && On) { openSearch(false); if (!searchPanel_) { searchTool_->toggleView(true); searchTool_->bringToFront(); } return; }
            auto* Dock = Kind == QLatin1String("search") ? searchTool_ : tools_.value(Kind);
            if (!Dock) return;
            if (On) { Dock->toggleView(true); Dock->bringToFront(); } else Dock->toggleView(false);
        });
    }
}
void DocumentWorkspaceWidget::fillLayoutMenu(QMenu* Menu)
{
    Menu->clear();
    auto* Group = new QActionGroup(Menu); Group->setExclusive(true);
    const auto Add = [this, Menu, Group](const QString& Name, const QString& Label) {
        auto* Item = Menu->addAction(Label); Item->setCheckable(true); Item->setChecked(layoutName_ == Name); Group->addAction(Item);
        connect(Item, &QAction::triggered, this, [this, Name] { applyLayout(Name); });
    };
    Add(QString(), tr("기본"));
    Add(QStringLiteral(":focus"), tr("집필"));
    const auto Names = docks_->perspectiveNames();
    if (!Names.isEmpty()) { Menu->addSeparator(); for (const auto& Name : Names) Add(Name, Name); }
    Menu->addSeparator();
    Menu->addAction(tr("현재 배치 저장…"), this, [this] {
        bool Accepted = false;
        const auto Name = hc::ui::InputDialog::getText(this, tr("배치 저장"), tr("배치 이름"), QLineEdit::Normal,
            layoutName_.startsWith(QLatin1Char(':')) ? QString() : layoutName_, &Accepted).trimmed();
        if (!Accepted || Name.isEmpty() || Name.startsWith(QLatin1Char(':'))) return;
        docks_->addPerspective(Name); layoutName_ = Name; storeDockSettings();
    });
    auto* Remove = Menu->addMenu(tr("저장한 배치 삭제"));
    for (const auto& Name : Names) Remove->addAction(Name, this, [this, Name] {
        docks_->removePerspective(Name); if (layoutName_ == Name) layoutName_.clear(); storeDockSettings();
    });
    Remove->setEnabled(!Names.isEmpty());
    Menu->addSeparator();
    Menu->addAction(tr("기본 배치로 되돌리기"), this, [this] { applyLayout({}); });
    auto* Lock = Menu->addAction(tr("배치 잠금")); Lock->setObjectName("view.layoutLock");
    Lock->setCheckable(true); Lock->setChecked(docks_->lockedFeatures() != fm::dock::DockWidget::NoFeatures);
    Lock->setToolTip(tr("끌어 놓기 · 떼어 내기 · 자동 숨김을 막습니다. 닫기와 탭 바꾸기는 그대로입니다."));
    connect(Lock, &QAction::toggled, this, [this](bool On) { docks_->lockFeatures(On ? LayoutLock : fm::dock::DockWidget::NoFeatures); storeDockSettings(); });
}
void DocumentWorkspaceWidget::applyLayout(const QString& Name)
{
    if (Name.isEmpty()) docks_->resetToDefaultLayout();
    else if (Name == QLatin1String(":focus")) {
        // 집필: 열린 도구 창을 모두 가장자리 자동 숨김으로 접어 문서를 넓게 쓴다. 다시 기본을 고르면 돌아온다.
        for (auto* Dock : docks_->dockWidgets())
            if (Dock->property("hc.toolKind").isValid() && !Dock->isClosed() && !Dock->isAutoHidden() && !Dock->isFloating()) docks_->setAutoHidden(Dock, true);
    } else if (!docks_->openPerspective(Name)) return;
    layoutName_ = Name;
    // 다시 짠 배치에서 포커스 문서의 도구 창 내용을 맞춘다.
    pickCurrentDocument(); syncToolHosts();
    storeDockSettings();
}
void DocumentWorkspaceWidget::storeDockSettings()
{
    if (!docks_ || !chrome_) return;
    auto Settings = chrome_->Settings();
    Settings["dockLayout"] = QString::fromLatin1(docks_->saveState(DockLayoutVersion).toBase64());
    QJsonObject Perspectives;
    const auto Saved = docks_->perspectives();
    for (auto At = Saved.cbegin(); At != Saved.cend(); ++At) Perspectives[At.key()] = QString::fromLatin1(At.value().toBase64());
    Settings["dockPerspectives"] = Perspectives;
    Settings["dockLayoutName"] = layoutName_;
    Settings["dockLocked"] = docks_->lockedFeatures() != fm::dock::DockWidget::NoFeatures;
    Settings["dockSearchPlaced"] = !searchPlacement_.isEmpty();
    if (auto* Button = findChild<QToolButton*>("documentLayoutButton"))
        Button->setText(tr("창 배치: %1").arg(layoutName_.isEmpty() ? tr("기본") : layoutName_ == QLatin1String(":focus") ? tr("집필") : layoutName_));
    // 바뀐 것이 없으면 설정 파일을 다시 쓰지 않는다(닫는 동안 여러 번 불린다).
    if (Settings == chrome_->Settings()) return;
    if (chrome_->ApplySettings(Settings)) emit ChromeSettingsChanged(chrome_->Settings());
}
void DocumentWorkspaceWidget::restoreDockSettings(const QJsonObject& Settings)
{
    // 시작할 때 한 번만(문서를 열기 전에). 설정 파일이 바뀌어 다시 읽혀도 쓰는 중인 배치는 그대로 둔다.
    if (dockRestored_ || !docks_) return;
    dockRestored_ = true;
    QMap<QString, QByteArray> Perspectives;
    const auto Saved = Settings["dockPerspectives"].toObject();
    for (auto At = Saved.begin(); At != Saved.end(); ++At) if (!At.key().startsWith(QLatin1Char(':'))) Perspectives[At.key()] = QByteArray::fromBase64(At.value().toString().toLatin1());
    docks_->setPerspectives(Perspectives);
    const auto State = QByteArray::fromBase64(Settings["dockLayout"].toString().toLatin1());
    if (!State.isEmpty() && documents_.empty() && docks_->restoreState(State, DockLayoutVersion)) {
        layoutName_ = Settings["dockLayoutName"].toString();
        if (Settings["dockSearchPlaced"].toBool()) searchPlacement_ = QStringLiteral("saved");
    }
    // 잠금은 상태 복원 뒤에(복원이 상태에 든 잠금으로 되돌린다).
    docks_->lockFeatures(Settings["dockLocked"].toBool() ? LayoutLock : fm::dock::DockWidget::NoFeatures);
    if (auto* Button = findChild<QToolButton*>("documentLayoutButton"))
        Button->setText(tr("창 배치: %1").arg(layoutName_.isEmpty() ? tr("기본") : layoutName_ == QLatin1String(":focus") ? tr("집필") : layoutName_));
}
void DocumentWorkspaceWidget::openPreviewBeside(const QString& Id)
{
    using namespace fm::dock;
    const auto Tab = find(Id);
    if (!Tab || Tab->loading_) return;
    if (Tab->previewDock_) { Tab->previewDock_->bringToFront(); return; }
    if (!tabbed_ || !Tab->pane_->CanTakePreview()) return;
    auto* Dock = new DockWidget(QStringLiteral("preview:") + Tab->id_, tr("%1 미리보기").arg(documentName(*Tab)), this);
    Dock->setDocument(true); Dock->setNativeContent(true);
    Dock->setFeatures(DockWidget::Closable | DockWidget::Movable | DockWidget::Floatable | DockWidget::Focusable | DockWidget::CustomCloseHandling);
    Dock->setProperty("hc.previewOf", Tab->id_);
    // 미리보기 칸은 다른 프로세스 창을 품는다: 문서 화면의 분할에서 이 도크로 바로 옮긴다(최상위 창을 거치지 않음).
    auto* Panel = Tab->pane_->TakePreview();
    if (!Panel) { delete Dock; return; }
    Dock->setWidget(Panel, DockWidget::InsertMode::ForceNoScrollArea); Panel->show();
    Tab->previewDock_ = Dock;
    connect(Dock, &DockWidget::closeRequested, this, [this, Id] { closePreviewBeside(Id); });
    if (auto* Area = Tab->dock_->dockArea(); Area && !Tab->dock_->isAutoHidden()) docks_->addDockWidget(DropArea::Right, Dock, Area);
    else docks_->addDockWidgetToCentral(Dock);
    update();
}
void DocumentWorkspaceWidget::closePreviewBeside(const QString& Id)
{
    const auto Tab = find(Id);
    if (!Tab || !Tab->previewDock_) return;
    auto* Dock = Tab->previewDock_.data(); Tab->previewDock_.clear();
    // 미리보기 칸을 문서 화면의 분할로 돌려준 뒤(부모 → 부모) 빈 도크를 지운다.
    Tab->pane_->ReturnPreview();
    Dock->toggleView(false); Dock->deleteLater();
    update();
}
void DocumentWorkspaceWidget::pollNativeFocus()
{
    if (!docks_ || documents_.empty() || !QApplication::activeWindow() || QApplication::activePopupWidget()) return;
    const HWND Focus = GetFocus();
    if (!Focus || QWidget::find(reinterpret_cast<WId>(Focus))) return;
    // 다른 프로세스 창 → 그 창을 품은 우리 위젯 → 그 위젯이 든 문서 도크.
    for (HWND Window = GetParent(Focus); Window; Window = GetParent(Window)) {
        auto* Widget = QWidget::find(reinterpret_cast<WId>(Window));
        if (!Widget) continue;
        for (auto* At = Widget; At; At = At->parentWidget())
            if (auto* Dock = qobject_cast<fm::dock::DockWidget*>(At)) {
                if (const auto Tab = tabOf(Dock); Tab && Tab->dock_ != currentDock_) docks_->setFocusedDockWidget(Dock);
                return;
            }
        return;
    }
}
}
// ============================================================================================ 탐색기 · 워크스페이스
// 설계: documentation/source/261113_hypereditor_explorer_workspace_design.rst 5–6장.
namespace hc::document {
namespace {
std::function<bool(const QString&, const QStringList&)>& WindowRequestHandler()
{
    static std::function<bool(const QString&, const QStringList&)> Handler;
    return Handler;
}
QString FolderName(const QString& Root)
{
    const auto Name = QFileInfo(Root).fileName();
    return Name.isEmpty() ? QDir::toNativeSeparators(Root) : Name;
}
void RememberSetting(const QString& Key, const QVariant& Value)
{
    AppSettings Settings; Settings.setValue(Key, Value); Settings.sync();
}
// 기억한 상위 찾기 선택(workspace/ancestorChoices): "기준 루트|고른 루트" 또는 "기준 루트|none" 줄의 목록.
// '|' 는 Windows 경로에 올 수 없다. 설정 파일에서 사람이 읽고 지울 수 있게 문자열 배열로 둔다.
QMap<QString, QString> AncestorChoices(const QJsonObject& Applied)
{
    QMap<QString, QString> Result;
    for (const auto& Line : PreferenceValue(Applied, QStringLiteral("workspace/ancestorChoices")).toStringList())
        if (const auto At = Line.indexOf(QLatin1Char('|')); At > 0) Result.insert(Line.left(At), Line.mid(At + 1));
    return Result;
}
void StoreAncestorChoices(const QMap<QString, QString>& Choices)
{
    QStringList Lines;
    for (auto At = Choices.cbegin(); At != Choices.cend(); ++At) Lines.append(At.key() + QLatin1Char('|') + At.value());
    RememberSetting(QStringLiteral("workspace/ancestorChoices"), Lines);
}
}
void DocumentWorkspaceWidget::SetWindowRequestHandler(std::function<bool(const QString& Root, const QStringList& Files)> Handler)
{
    WindowRequestHandler() = std::move(Handler);
}
bool DocumentWorkspaceWidget::event(QEvent* Event)
{
    // Ctrl+Q 빠른 보기에는 탐색기가 없다(설계 5.1): HyperViewer 가 창을 만든 뒤 hc.quickPreview 를 붙인다.
    if (Event->type() == QEvent::DynamicPropertyChange && explorerTool_
        && static_cast<QDynamicPropertyChangeEvent*>(Event)->propertyName() == "hc.quickPreview" && property("hc.quickPreview").toBool()) {
        tools_.remove(QStringLiteral("explorer"));
        auto* Tool = std::exchange(explorerTool_, nullptr); explorer_ = nullptr; explorerSignature_.clear();
        docks_->removeDockWidget(Tool); Tool->deleteLater();
        dockDefaultLayout_ = docks_->saveState(DockLayoutVersion); docks_->setDefaultLayout(dockDefaultLayout_);
    }
    return QWidget::event(Event);
}
void DocumentWorkspaceWidget::buildExplorer()
{
    explorer_ = new ExplorerPanel(hosted_);
    explorer_->setObjectName("documentExplorer");
    explorerTool_ = makeTool(QStringLiteral("explorer"), tr("탐색기"), explorer_);
    tools_.insert(QStringLiteral("explorer"), explorerTool_);
    workspaceSave_ = new QTimer(this); workspaceSave_->setObjectName("documentWorkspaceSave"); workspaceSave_->setSingleShot(true); workspaceSave_->setInterval(1500);
    connect(workspaceSave_, &QTimer::timeout, this, &DocumentWorkspaceWidget::saveWorkspace);
}
void DocumentWorkspaceWidget::connectExplorer()
{
    const auto Add = [this](const QString& Label, const QString& Id, auto Callback) {
        auto* Action = new QAction(Label, this); Action->setObjectName(Id); addAction(Action);
        connect(Action, &QAction::triggered, this, Callback); return Action;
    };
    Add(tr("폴더 열기…"), QStringLiteral("workspace.openFolder"), [this] { showOpenFolderDialog(); })->setProperty("documentIconName", "open");
    Add(tr("워크스페이스 닫기"), QStringLiteral("workspace.close"), [this] { CloseWorkspace(); })->setEnabled(false);
    Add(tr("탐색기로 이동"), QStringLiteral("view.explorer"), [this] {
        if (!explorerTool_ || !explorer_) return;
        explorerTool_->toggleView(true); explorerTool_->bringToFront();
        explorer_->RevealActive(true);
    })->setProperty("documentIconName", "toc-toggle");
    for (const auto& Command : DocumentShortcutRegistry::Commands()) {
        if (Command.scope_ != QLatin1String("explorer")) continue;
        Add(Command.label_, Command.id_, [this, Id = Command.id_] {
            if (!explorer_) return;
            if (Id == QLatin1String("explorer.open")) explorer_->OpenCurrent(false);
            else if (Id == QLatin1String("explorer.openBeside")) explorer_->OpenCurrent(true);
            else if (Id == QLatin1String("explorer.rename")) explorer_->RenameCurrent();
            else if (Id == QLatin1String("explorer.recycle")) explorer_->DeleteCurrent(false);
            else if (Id == QLatin1String("explorer.delete")) explorer_->DeleteCurrent(true);
            else if (Id == QLatin1String("explorer.copyPath")) explorer_->CopyCurrentPath(false);
            else if (Id == QLatin1String("explorer.copyRelativePath")) explorer_->CopyCurrentPath(true);
            else if (Id == QLatin1String("explorer.newFile")) explorer_->NewFile();
            else if (Id == QLatin1String("explorer.newFolder")) explorer_->NewFolder();
            else if (Id == QLatin1String("explorer.collapseAll")) explorer_->CollapseAll();
            else if (Id == QLatin1String("explorer.refresh")) explorer_->Refresh();
        });
    }
    if (recent_) connect(recent_, &RecentDocumentStore::Changed, this, &DocumentWorkspaceWidget::refreshExplorerRecent);
    if (!explorer_) return;
    explorer_->SetShortcutText([this](const QString& Id) {
        auto* Action = findChild<QAction*>(Id, Qt::FindDirectChildrenOnly);
        return Action ? Action->shortcut().toString(QKeySequence::NativeText) : QString();
    });
    // HyperViewer 는 바꾸지 않는다: 편집 · 워크스페이스는 HyperEditor 에 넘긴다(editRequest format "explorer").
    const auto ToEditor = [this](const QString& Path, bool Folder) {
        if (!hosted_) { if (Folder) OpenWorkspace(Path); else openByUser(Path); return; }
        if (!editRequest_.isEmpty()) return;
        editRequest_ = {{"token", QUuid::createUuid().toString(QUuid::Id128)}, {"format", "explorer"}, {"path", QFileInfo(Path).absoluteFilePath()}, {"folder", Folder}};
        update();
    };
    connect(explorer_, &ExplorerPanel::OpenRequested, this, &DocumentWorkspaceWidget::openFromExplorer);
    connect(explorer_, &ExplorerPanel::OpenFolderRequested, this, &DocumentWorkspaceWidget::showOpenFolderDialog);
    connect(explorer_, &ExplorerPanel::OpenWorkspaceRequested, this, [ToEditor](const QString& Folder) { ToEditor(Folder, true); });
    connect(explorer_, &ExplorerPanel::EditInEditorRequested, this, [ToEditor](const QString& Path) { ToEditor(Path, false); });
    connect(explorer_, &ExplorerPanel::OpenFolderInEditorRequested, this, [ToEditor](const QString& Folder) { ToEditor(Folder, true); });
    connect(explorer_, &ExplorerPanel::FindInFolderRequested, this, [this](const QString& Folder) { openSearch(false, Folder); });
    connect(explorer_, &ExplorerPanel::BuildRequested, this, [this](const QString& Folder) {
        // 그 프로젝트의 reST 문서로 빌드한다: 열린 것이 있으면 그것, 없으면 루트 문서(index.rst)를 연다.
        std::shared_ptr<Tab> Target;
        for (const auto& Each : documents_) if (!Each->closed_ && Each->pane_ && Each->pane_->IsRst() && workspace::IsInside(Each->path_, Folder, false)) { Target = Each; break; }
        if (!Target) {
            const auto Index = QDir(Folder).filePath(QStringLiteral("index.rst"));
            if (!QFileInfo(Index).isFile()) { ShowShellError(tr("빌드할 문서를 찾지 못했습니다. 프로젝트의 reST 문서를 연 뒤 빌드하십시오.")); return; }
            Target = find(open(Index, false, {}, false)["documentId"].toString());
        }
        if (!Target) return;
        activate(Target);
        auto* Wait = new QTimer(this); Wait->setInterval(50); auto Attempts = std::make_shared<int>(0);
        connect(Wait, &QTimer::timeout, this, [this, Wait, Id = Target->id_, Attempts] {
            auto Current = find(Id); if (!Current || ++*Attempts > 600) { Wait->deleteLater(); return; }
            if (Current->loading_) return;
            Wait->deleteLater();
            if (!Current->pane_->RunViewCommand(QStringLiteral("rst.buildProject"))) ShowShellError(tr("이 문서에서는 프로젝트를 빌드할 수 없습니다."));
        });
        Wait->start();
    });
    connect(explorer_, &ExplorerPanel::RenameRequested, this, &DocumentWorkspaceWidget::explorerRename);
    connect(explorer_, &ExplorerPanel::Created, this, [this](const QString& Path, bool Folder) {
        if (Folder || hosted_) return;
        // 새 파일은 이름을 정하면 바로 연다.
        openFromExplorer(Path, false);
    });
    connect(explorer_, &ExplorerPanel::NoticeAction, this, [this](const QString& Id, int Index) {
        if (Id == QLatin1String("ancestorOpened")) { if (Index == 0) undoAncestorWorkspace(); else openRemainingSessionDocuments(); }
        else if (Id == QLatin1String("sessionPending") && Index == 0) openRemainingSessionDocuments();
        else if (Id == QLatin1String("alreadyOpen") || Id == QLatin1String("stateFallback") || Id == QLatin1String("mreImported")) explorer_->HideNotice(Id);
    });
    connect(explorer_, &ExplorerPanel::PickChosen, this, [this](int Index, bool Remember) {
        if (Index < 0 || Index >= ancestorPicks_.size()) return;
        const auto Pick = ancestorPicks_[Index]; ancestorPicks_.clear();
        if (Remember && !Pick.rememberKey_.isEmpty()) {
            auto Choices = AncestorChoices(appliedPreferences_);
            Choices.insert(Pick.rememberKey_, Pick.root_.isEmpty() ? QStringLiteral("none") : Pick.root_);
            StoreAncestorChoices(Choices); refreshPreferences();
        }
        if (!Pick.root_.isEmpty()) openAncestor(Pick.root_, Pick.file_, QStringLiteral("plain"));
    });
    connect(explorer_, &ExplorerPanel::StateChanged, this, &DocumentWorkspaceWidget::scheduleWorkspaceSave);
    connect(explorer_, &ExplorerPanel::SettingChanged, this, [this](const QString& Key, bool Value) {
        RememberSetting(QStringLiteral("explorer/") + Key, Value); refreshPreferences();
    });
    connect(explorer_, &ExplorerPanel::ShowSettingsRequested, this, [this] { showPreferences(); if (preferences_) preferences_->ShowPage(QStringLiteral("explorer")); });
}
void DocumentWorkspaceWidget::refreshExplorer()
{
    if (!explorer_) return;
    const auto Current = active();
    QList<ExplorerPanel::OpenDocument> Open; QStringList Parts;
    // 복구 사본 · 새 문서 · 제공자 사본은 디스크의 자리가 아니다.
    const auto Real = [](const std::shared_ptr<Tab>& Each) { return !Each->closed_ && !Each->path_.isEmpty() && Each->temporarySource_.isEmpty() && !Each->recovered_ && !Each->broker_ && Each->sourceToken_.isEmpty(); };
    for (const auto& Each : documents_) {
        if (!Real(Each)) continue;
        const bool Dirty = Each->Dirty();
        Open.append({Each->path_, Dirty}); Parts.append(Each->path_ + (Dirty ? QStringLiteral("*") : QString()));
    }
    const auto ActivePath = Current && Real(Current) ? Current->path_ : QString();
    const auto Signature = Parts.join(QLatin1Char('\n')) + QLatin1Char('\n') + ActivePath + QLatin1Char('\n') + workspaceRoot_;
    if (Signature == explorerSignature_) return;
    explorerSignature_ = Signature;
    explorer_->SetOpenDocuments(Open);
    if (!ActivePath.isEmpty()) explorer_->SetDocumentFolder(QFileInfo(ActivePath).absolutePath());
    else if (documents_.empty()) explorer_->SetDocumentFolder({});
    explorer_->SetActiveDocument(ActivePath);
    scheduleWorkspaceSave();
}
void DocumentWorkspaceWidget::refreshExplorerRecent()
{
    if (!explorer_ || !recent_) return;
    QList<ExplorerPanel::RecentWorkspace> Items;
    for (const auto& Entry : recent_->Entries()) {
        if (Entry.kind_ != QLatin1String("folder")) continue;
        Items.append({Entry.path_, FolderName(Entry.path_)});
        if (Items.size() == 5) break;
    }
    explorer_->SetRecentWorkspaces(Items);
}
void DocumentWorkspaceWidget::applyExplorerPreferences()
{
    if (!explorer_) return;
    ExplorerPanel::Settings Value;
    const auto Bool = [this](const char* Key) { return PreferenceValue(appliedPreferences_, QStringLiteral("explorer/") + QLatin1String(Key)).toBool(); };
    Value.autoReveal_ = Bool("autoReveal"); Value.showAllFiles_ = Bool("showAllFiles"); Value.showHidden_ = Bool("showHidden");
    Value.showAllDrives_ = Bool("showAllDrives"); Value.confirmRecycle_ = Bool("confirmRecycle");
    for (const auto& Name : PreferenceValue(appliedPreferences_, QStringLiteral("explorer/excludedFolders")).toString().split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (const auto Trimmed = Name.trimmed(); !Trimmed.isEmpty()) Value.excluded_.append(Trimmed);
    for (const auto Suffix : DocumentExtensions) Value.openableSuffixes_.append(QString::fromLatin1(Suffix.data(), qsizetype(Suffix.size())));
    if (const auto Font = PreferenceValue(appliedPreferences_, QStringLiteral("explorer/font")).toString(); !Font.isEmpty()) Value.font_.fromString(Font);
    else Value.font_ = QFont();
    explorer_->ApplySettings(Value);
    refreshExplorerRecent();
}
void DocumentWorkspaceWidget::showOpenFolderDialog()
{
    if (hosted_) return;
    const auto Start = !workspaceRoot_.isEmpty() ? QFileInfo(workspaceRoot_).absolutePath() : explorer_ ? explorer_->DocumentFolder() : QString();
    auto* Dialog = new QFileDialog(this, tr("폴더 열기"), Start);
    Dialog->setFileMode(QFileDialog::Directory); Dialog->setOption(QFileDialog::ShowDirsOnly, true);
    connect(Dialog, &QFileDialog::fileSelected, this, [this](const QString& Folder) { if (!Folder.isEmpty()) OpenWorkspace(Folder); });
    OpenFileDialog(Dialog);
}
bool DocumentWorkspaceWidget::OpenWorkspace(const QString& Path, WorkspaceOrigin Origin)
{
    if (hosted_ || Path.isEmpty()) return false;
    const auto Root = workspace::NormalizeRoot(QFileInfo(Path).absoluteFilePath());
    // Older profiles may have recorded the private recovery store as a workspace.
    // HotExit can still reopen its documents without binding the window to that folder.
    if (workspace::IsInside(Root, recoveryRoot_, false)) {
        if (Origin == WorkspaceOrigin::User) ShowShellError(tr("자동 복구 폴더는 작업공간으로 열 수 없습니다."));
        return false;
    }
    if (!QFileInfo(Root).isDir()) { ShowShellError(tr("폴더를 찾을 수 없습니다: %1").arg(QDir::toNativeSeparators(Root))); return false; }
    if (workspace::SamePath(Root, workspaceRoot_)) {
        if (explorerTool_) { explorerTool_->toggleView(true); explorerTool_->bringToFront(); }
        return true;
    }
    // 같은 폴더는 프로세스 안에서 한 창에만: 그 창을 앞으로 가져오고 알린다.
    if (auto* Owner = qobject_cast<DocumentWorkspaceWidget*>(workspace::Registry::Instance().WindowFor(Root)); Owner && Owner != this) {
        auto* Window = Owner->window(); if (Window->isMinimized()) Window->showNormal();
        Window->raise(); Window->activateWindow();
        if (Owner->explorer_) Owner->explorer_->ShowNotice(QStringLiteral("alreadyOpen"), fm::ui::Banner::Info, tr("이 창에 이미 열려 있는 워크스페이스입니다"), QDir::toNativeSeparators(Root));
        return true;
    }
    // 이 창에 다른 워크스페이스가 있으면 새 창에 연다(지금 창의 탭은 닫지 않는다).
    if (!workspaceRoot_.isEmpty()) {
        if (const auto& Handler = WindowRequestHandler()) return Handler(Root, {});
        if (!CloseWorkspace()) return false;
    }
    return bindWorkspace(Root, Origin);
}
bool DocumentWorkspaceWidget::CloseWorkspace()
{
    if (workspaceRoot_.isEmpty()) return true;
    saveWorkspace();
    // 지금의 닫기 확인을 거친다. 취소하거나 저장이 이어지면 워크스페이스는 그대로 둔다.
    while (!documents_.empty()) {
        const auto Count = documents_.size(); closeTab(int(Count - 1));
        if (documents_.size() == Count) return false;
    }
    RememberSetting(QStringLiteral("workspace/lastRoot"), QString());
    unbindWorkspace();
    return true;
}
bool DocumentWorkspaceWidget::bindWorkspace(const QString& Root, WorkspaceOrigin Origin)
{
    if (!workspace::Registry::Instance().Bind(Root, this)) return false;
    // 빈 창(상위 찾기는 방금 연 파일만 있는 창도)이면 지난 세션 문서까지 되살린다.
    const bool Empty = documents_.empty() || (Origin == WorkspaceOrigin::Ancestor && documents_.size() == 1);
    workspaceRoot_ = Root; workspaceFallbackNotified_ = false; pendingSession_.reset(); autoRestoredPaths_.clear();
    workspaceExtra_ = {}; workspaceFind_ = {}; explorerSignature_.clear();
    if (explorer_) { explorer_->HidePick(); explorer_->HideNotice(); explorer_->SetWorkspace(Root, FolderName(Root)); }
    ancestorPicks_.clear(); ++ancestorGeneration_;
    if (auto* Close = findChild<QAction*>("workspace.close", Qt::FindDirectChildrenOnly)) Close->setEnabled(true);
    // 상태: 폴더 · 프로필 가운데 새 쪽. 없으면 MRE 세션을 한 번 가져온다(MRE 파일은 그대로 둔다).
    const auto Loaded = workspaceStore().Load(Root);
    std::optional<workspace::State> State; bool Imported = false;
    if (Loaded.found_) State = Loaded.state_;
    else if (PreferenceValue(appliedPreferences_, QStringLiteral("workspace/importMultiRoot")).toBool())
        if (auto Session = workspace::ImportMultiRoot(Root)) { State = std::move(Session); Imported = true; }
    const bool Layout = PreferenceValue(appliedPreferences_, QStringLiteral("workspace/restoreLayout")).toBool();
    if (State) {
        workspaceExtra_ = State->extra_; workspaceFind_ = State->findInFiles_;
        if (Empty && !bindOnly_ && Origin != WorkspaceOrigin::HotExit) restoreWorkspaceDocuments(*State, Layout);
        else {
            // 묶기만: 탐색기 · (아직 문서가 없으면) 배치. 지난 세션 문서는 ‘모두 열기’ 로 넘긴다.
            const QScopedValueRollback Guard(workspaceRestoring_, true);
            if (Layout && documents_.empty() && !State->dockLayout_.isEmpty() && State->dockVersion_ == DockLayoutVersion) docks_->restoreState(State->dockLayout_, DockLayoutVersion);
            if (explorer_) explorer_->RestoreExpansion(State->explorer_.expanded_, State->explorer_.selected_, State->explorer_.topItem_);
            if (Origin != WorkspaceOrigin::HotExit && !State->documents_.isEmpty()) pendingSession_ = State;
        }
    }
    if (explorer_) {
        if (Loaded.found_ && Loaded.state_.newerSchema_)
            explorer_->ShowNotice(QStringLiteral("newerSchema"), fm::ui::Banner::Warn, tr("더 새 판이 저장한 워크스페이스 상태입니다"),
                tr("읽기만 하고 덮어쓰지 않습니다. 이 창의 상태는 저장되지 않습니다."));
        else if (Imported)
            explorer_->ShowNotice(QStringLiteral("mreImported"), fm::ui::Banner::Info, tr("MultiRootRestCPP 세션을 가져왔습니다"),
                tr("%1 은(는) 그대로 둡니다.").arg(QDir::toNativeSeparators(workspace::MultiRootSessionPath(Root))));
        else if (pendingSession_ && Origin != WorkspaceOrigin::Ancestor)
            explorer_->ShowNotice(QStringLiteral("sessionPending"), fm::ui::Banner::Info, tr("지난 세션 문서 %1개").arg(pendingSession_->documents_.size()),
                tr("이 창에 이미 문서가 있어 지난 세션 문서는 열지 않았습니다."), {tr("모두 열기")});
    }
    recordRecentWorkspace(Root);
    RememberSetting(QStringLiteral("workspace/lastRoot"), Root);
    scanWorkspaceProjects();
    updateTitle(); update();
    // 처음 연 폴더도 다음부터 워크스페이스로 보이게 상태 파일을 만든다(상태 위치 설정을 따름).
    if (Origin != WorkspaceOrigin::HotExit) saveWorkspace();
    return true;
}
void DocumentWorkspaceWidget::unbindWorkspace()
{
    workspace::Registry::Instance().Unbind(this);
    workspaceRoot_.clear(); pendingSession_.reset(); autoRestoredPaths_.clear(); workspaceExtra_ = {}; workspaceFind_ = {};
    ancestorPicks_.clear(); explorerSignature_.clear();
    if (workspaceSave_) workspaceSave_->stop();
    if (explorer_) { explorer_->HideNotice(); explorer_->HidePick(); explorer_->SetSphinxRoots({}); explorer_->SetWorkspace({}); }
    if (auto* Close = findChild<QAction*>("workspace.close", Qt::FindDirectChildrenOnly)) Close->setEnabled(false);
    updateTitle(); update();
}
void DocumentWorkspaceWidget::restoreWorkspaceDocuments(const workspace::State& State, bool Layout)
{
    const QScopedValueRollback Guard(workspaceRestoring_, true);
    // 배치와 창 위치는 문서가 없을 때만(문서 도크 ID 는 세션마다 다르다).
    if (Layout && documents_.empty()) {
        if (!State.dockLayout_.isEmpty() && State.dockVersion_ == DockLayoutVersion) docks_->restoreState(State.dockLayout_, DockLayoutVersion);
        if (!State.geometry_.isEmpty() && !parentWidget()) {
            restoreGeometry(State.geometry_);
            if (screen() && !screen()->availableGeometry().intersects(frameGeometry())) move(screen()->availableGeometry().topLeft());
        }
    }
    const auto Current = active();
    QStringList Opened;
    for (const auto& Entry : State.documents_) {
        if (!QFileInfo(Entry.path_).isFile()) continue;
        if (std::any_of(documents_.cbegin(), documents_.cend(), [&Entry](const auto& Each) { return workspace::SamePath(Each->path_, Entry.path_); })) continue;
        const auto Epub = Entry.view_[QStringLiteral("epub")].toObject();
        const bool EpubEdit = Entry.kind_ == QLatin1String("epub") && Epub[QStringLiteral("mode")].toString() == QLatin1String("edit");
        const auto Reply = EpubEdit ? OpenForEpubEdit(Entry.path_, Epub[QStringLiteral("locator")].toString()) : open(Entry.path_, false, {}, false);
        if (Reply.contains("error")) { if (Reply["error"].toString() == QLatin1String("document-workspace-limit")) break; continue; }
        auto Tab = find(Reply["documentId"].toString()); if (!Tab) continue;
        if (Entry.position_) {
            const auto& Position = *Entry.position_;
            if (Position.IsLineColumn()) Tab->openLine_ = Position.caretLine_;
            else if (Position.caret_ >= 0) {
                TextRecoveryState Text;
                Text.anchor_ = qMax<qint64>(0, Position.anchor_ < 0 ? Position.caret_ : Position.anchor_); Text.caret_ = Position.caret_;
                Text.firstLine_ = qMax(0, Position.firstLine_); Text.firstSubLine_ = qMax(0, Position.firstSubLine_); Text.xOffset_ = qMax(0, Position.xOffset_);
                Tab->cleanRestoreState_ = Text;
            }
        }
        if (Entry.kind_ == QLatin1String("pdf") && Entry.view_[QStringLiteral("pdf")].isObject()) Tab->restorePosition_ = Entry.view_[QStringLiteral("pdf")].toObject();
        else if (Entry.kind_ == QLatin1String("epub") && !EpubEdit && !Epub[QStringLiteral("locator")].toString().isEmpty())
            Tab->restorePosition_ = QJsonObject{{"locator", Epub[QStringLiteral("locator")].toString()}};
        if (const auto Mode = Entry.view_[QStringLiteral("mode")].toString(); !Mode.isEmpty()) {
            RecentDocument Restore; Restore.path_ = Entry.path_; Restore.markdownView_ = Mode; Tab->restore_ = Restore;
        }
        Opened.append(Entry.path_);
    }
    // 활성 문서: 빈 창이면 지난 세션의 것, 아니면(상위 찾기) 방금 연 문서를 둔다.
    if (Current) activate(Current);
    else for (const auto& Each : documents_) if (!State.active_.isEmpty() && workspace::SamePath(Each->path_, State.active_)) { activate(Each); break; }
    if (explorer_) explorer_->RestoreExpansion(State.explorer_.expanded_, State.explorer_.selected_, State.explorer_.topItem_);
    autoRestoredPaths_ = Opened;
}
void DocumentWorkspaceWidget::scheduleWorkspaceSave()
{
    if (workspaceRoot_.isEmpty() || workspaceRestoring_ || !workspaceSave_) return;
    workspaceSave_->start();
}
workspace::Store DocumentWorkspaceWidget::workspaceStore() const
{
    return workspace::Store(QFileInfo(settingsPath_).absolutePath());
}
workspace::State DocumentWorkspaceWidget::captureWorkspace() const
{
    workspace::State State;
    State.root_ = workspaceRoot_; State.savedAt_ = QDateTime::currentDateTimeUtc(); State.extra_ = workspaceExtra_;
    State.findInFiles_ = searchPanel_ ? searchPanel_->Context() : workspaceFind_;
    for (const auto& Each : documents_) {
        if (Each->closed_ || Each->path_.isEmpty() || !Each->temporarySource_.isEmpty() || Each->recovered_ || Each->broker_ || !Each->sourceToken_.isEmpty() || !Each->pane_) continue;
        workspace::DocumentEntry Entry; Entry.path_ = Each->path_; Entry.kind_ = Each->pane_->ViewId();
        Entry.outside_ = !workspace::IsInside(Each->path_, workspaceRoot_, false);
        if (!Each->loading_) {
            QByteArray Unused;
            if (const auto Text = Each->pane_->CaptureTextRecovery(Unused, false)) {
                workspace::Position Position;
                Position.anchor_ = Text->anchor_; Position.caret_ = Text->caret_; Position.firstLine_ = int(Text->firstLine_);
                Position.firstSubLine_ = int(Text->firstSubLine_); Position.xOffset_ = int(Text->xOffset_);
                Entry.position_ = Position;
            }
            const auto View = Each->pane_->ViewState();
            if (Each->pane_->ViewId() == QLatin1String("pdf")) Entry.view_[QStringLiteral("pdf")] = Each->pane_->PdfPosition();
            if (Each->pane_->IsMarkdown()) Entry.view_[QStringLiteral("mode")] = View["markdown"].toObject()["mode"].toString();
            if (Each->pane_->IsRst()) Entry.view_[QStringLiteral("mode")] = View["rst"].toObject()["mode"].toString();
            if (Each->pane_->IsEpub()) Entry.view_[QStringLiteral("epub")] = QJsonObject{{"mode", Each->pane_->IsEpubEditing() ? "edit" : "read"},
                {"locator", View["epub"].toObject()["locator"].toString()}};
        } else if (Each->cleanRestoreState_) {
            // 아직 읽는 중: 되살릴 위치를 그대로 남긴다.
            workspace::Position Position; const auto& Text = *Each->cleanRestoreState_;
            Position.anchor_ = Text.anchor_; Position.caret_ = Text.caret_; Position.firstLine_ = int(Text.firstLine_);
            Position.firstSubLine_ = int(Text.firstSubLine_); Position.xOffset_ = int(Text.xOffset_);
            Entry.position_ = Position;
        }
        State.documents_.append(Entry);
    }
    if (const auto Current = active(); Current && Current->temporarySource_.isEmpty()) State.active_ = Current->path_;
    if (explorer_) { State.explorer_.expanded_ = explorer_->ExpandedPaths(); State.explorer_.selected_ = explorer_->SelectedPath(); State.explorer_.topItem_ = explorer_->TopPath(); }
    if (docks_ && PreferenceValue(appliedPreferences_, QStringLiteral("workspace/restoreLayout")).toBool()) {
        State.dockLayout_ = docks_->saveState(DockLayoutVersion); State.dockVersion_ = DockLayoutVersion;
        if (!parentWidget()) State.geometry_ = saveGeometry();
    }
    return State;
}
void DocumentWorkspaceWidget::saveWorkspace()
{
    if (workspaceRoot_.isEmpty() || workspaceRestoring_ || hosted_) return;
    if (workspaceSave_) workspaceSave_->stop();
    const auto Preferred = PreferenceValue(appliedPreferences_, QStringLiteral("workspace/stateLocation")).toString() == QLatin1String("profile")
        ? workspace::Location::Profile : workspace::Location::Folder;
    const auto Result = workspaceStore().Save(workspaceRoot_, captureWorkspace(), Preferred);
    workspace::MarkerCache::Instance().Invalidate(workspaceRoot_);
    if (!explorer_) return;
    if (Result.fellBack_ && !std::exchange(workspaceFallbackNotified_, true))
        explorer_->ShowNotice(QStringLiteral("stateFallback"), fm::ui::Banner::Info, tr("워크스페이스 상태를 설정 폴더에 저장했습니다"),
            tr("%1 에 쓸 수 없어 설정 폴더의 workspaces 에 저장합니다.").arg(QDir::toNativeSeparators(workspace::Store::FolderStatePath(workspaceRoot_))));
    else if (!Result.ok_ && Result.error_ != QLatin1String("newer-schema") && !std::exchange(workspaceFallbackNotified_, true))
        explorer_->ShowNotice(QStringLiteral("stateFailed"), fm::ui::Banner::Warn, tr("워크스페이스 상태를 저장하지 못했습니다"), Result.error_);
}
void DocumentWorkspaceWidget::scanWorkspaceProjects()
{
    if (workspaceRoot_.isEmpty() || !explorer_) return;
    const auto Root = workspaceRoot_;
    // 캐시(projects.json, MRE 와 같은 형식)를 먼저 보이고, 스캔이 끝나면 바꾼다.
    QStringList Cached;
    for (const auto& Record : workspace::ReadProjectsCache(Root)) Cached.append(QFileInfo(Record.confPath_).absolutePath());
    explorer_->SetSphinxRoots(Cached);
    // 상태를 프로필에 두는 설정이면 폴더에 아무것도 만들지 않는다.
    const bool WriteCache = PreferenceValue(appliedPreferences_, QStringLiteral("workspace/stateLocation")).toString() != QLatin1String("profile");
    auto* Watcher = new QFutureWatcher<QList<workspace::ProjectRecord>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Root] {
        const auto Projects = Watcher->result(); Watcher->deleteLater();
        if (!explorer_ || !workspace::SamePath(Root, workspaceRoot_)) return;
        QStringList Roots; for (const auto& Each : Projects) Roots.append(QFileInfo(Each.confPath_).absolutePath());
        explorer_->SetSphinxRoots(Roots);
    });
    Watcher->setFuture(QtConcurrent::run([Root, WriteCache] {
        QList<workspace::ProjectRecord> Records;
        for (const auto& Project : hc::rst::ProjectScanner(hc::rst::toPath(Root)).scan()) {
            workspace::ProjectRecord Record;
            Record.projectId_ = QString::fromStdWString(Project.projectId); Record.rootPath_ = hc::rst::toQString(Project.rootPath);
            Record.confPath_ = hc::rst::toQString(Project.confPath); Record.sourcePath_ = hc::rst::toQString(Project.sourcePath);
            Record.rootDoc_ = QString::fromStdString(Project.rootDoc); Record.buildPath_ = hc::rst::toQString(Project.buildPath);
            Record.mystMarkdown_ = Project.mystMarkdown;
            Records.append(Record);
        }
        if (WriteCache && Records != workspace::ReadProjectsCache(Root)) workspace::WriteProjectsCache(Root, Records);
        return Records;
    }));
}
void DocumentWorkspaceWidget::recordRecentWorkspace(const QString& Root)
{
    if (!recent_ || !PreferenceValue(appliedPreferences_, QStringLiteral("recent/enabled")).toBool()) return;
    RecentDocument Document;
    Document.path_ = Root; Document.kind_ = QStringLiteral("folder"); Document.viewId_ = QStringLiteral("folder"); Document.openedAt_ = QDateTime::currentDateTime();
    recent_->Record(std::move(Document));
}
QJsonObject DocumentWorkspaceWidget::openByUser(const QString& Path, const QString& AncestorMode)
{
    const auto Reply = open(Path, false, {}, false);
    if (!hosted_ && !Reply.contains("error") && !Reply["duplicate"].toBool()) considerAncestors(Path, AncestorMode);
    return Reply;
}
void DocumentWorkspaceWidget::considerAncestors(const QString& Path, const QString& Mode)
{
    if (hosted_ || Mode == QLatin1String("off") || !workspaceRoot_.isEmpty() || Path.isEmpty() || !QFileInfo(Path).isAbsolute()) return;
    if (workspace::IsInside(Path, recoveryRoot_, false)) return;
    if (PreferenceValue(appliedPreferences_, QStringLiteral("workspace/ancestorDetection")).toString() == QLatin1String("off")) return;
    // 열린 워크스페이스 창 안의 파일이면 찾지 않는다(그 창이 더 깊은 후보보다 이긴다).
    if (workspace::Registry::Instance().WindowContaining(Path)) return;
    // 기억한 선택: 파일을 품은 가장 깊은 기준 루트.
    auto Choices = AncestorChoices(appliedPreferences_);
    QString Base;
    for (auto At = Choices.cbegin(); At != Choices.cend(); ++At)
        if (At.key().size() > Base.size() && workspace::IsInside(Path, At.key(), false)) Base = At.key();
    if (!Base.isEmpty()) {
        const auto Chosen = Choices.value(Base);
        if (Chosen == QLatin1String("none")) return;
        if (QFileInfo(Chosen).isDir()) { openAncestor(Chosen, Path, Mode); return; }
        // 기억한 루트가 사라졌다: 그 기억은 버리고 다시 찾는다.
        Choices.remove(Base); StoreAncestorChoices(Choices); refreshPreferences();
    }
    const auto Generation = ++ancestorGeneration_;
    auto Known = workspaceStore().ProfileRoots();
    if (recent_) for (const auto& Entry : recent_->Entries()) if (Entry.kind_ == QLatin1String("folder")) Known.append(Entry.path_);
    auto* Watcher = new QFutureWatcher<workspace::FindResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation, Path, Mode] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation == ancestorGeneration_) applyAncestorResult(Path, Mode, Result.candidates_);
    });
    Watcher->setFuture(QtConcurrent::run([Path, Known] { return workspace::FindAncestorWorkspaces(Path, Known); }));
}
void DocumentWorkspaceWidget::applyAncestorResult(const QString& Path, const QString& Mode, const QList<workspace::Candidate>& Candidates)
{
    if (!workspaceRoot_.isEmpty() || Candidates.isEmpty() || !explorer_) return;
    // 그동안 파일을 닫았으면 그만둔다.
    if (std::none_of(documents_.cbegin(), documents_.cend(), [&Path](const auto& Each) { return workspace::SamePath(Each->path_, Path); })) return;
    const bool Ask = PreferenceValue(appliedPreferences_, QStringLiteral("workspace/ancestorDetection")).toString() == QLatin1String("ask");
    if (Candidates.size() == 1 && !Ask) { openAncestor(Candidates.first().root_, Path, Mode); return; }
    // 둘 이상(또는 ‘묻기’): 탐색기 맨 위 선택 카드. 기본은 가장 최근에 저장한 후보, 없으면 가장 가까운 후보.
    ancestorPicks_.clear();
    QList<ExplorerPanel::PickChoice> Choices;
    const auto Remember = Candidates.first().root_;
    int Default = 0; QDateTime Newest;
    for (int At = 0; At < Candidates.size(); ++At) {
        const auto& Each = Candidates[At];
        if (Each.savedAt_.isValid() && (!Newest.isValid() || Each.savedAt_ > Newest)) { Newest = Each.savedAt_; Default = At; }
        ExplorerPanel::PickChoice Choice;
        Choice.root_ = Each.root_; Choice.name_ = FolderName(Each.root_);
        Choice.tag_ = Each.kind_ == workspace::Candidate::Kind::MultiRoot ? tr("MRE 세션") : tr("워크스페이스");
        QStringList Detail{QDir::toNativeSeparators(Each.root_)};
        if (Each.documentCount_ >= 0) Detail.append(tr("지난 세션 문서 %1개").arg(Each.documentCount_));
        if (Each.savedAt_.isValid()) Detail.append(QLocale().toString(Each.savedAt_.toLocalTime(), QLocale::ShortFormat));
        Choice.description_ = Detail.join(QStringLiteral(" · "));
        Choices.append(Choice); ancestorPicks_.append({Each.root_, Path, Remember, false});
    }
    // 바로 위 폴더를 새 워크스페이스로(MRE 의 동작). 이미 후보면 뺀다.
    const auto Parent = workspace::NormalizePath(QFileInfo(Path).absolutePath());
    if (std::none_of(Candidates.cbegin(), Candidates.cend(), [&Parent](const auto& Each) { return workspace::SamePath(Each.root_, Parent); })) {
        ExplorerPanel::PickChoice Choice; Choice.root_ = Parent; Choice.name_ = tr("바로 위 폴더를 새 워크스페이스로");
        Choice.description_ = QDir::toNativeSeparators(Parent); Choice.newWorkspace_ = true;
        Choices.append(Choice); ancestorPicks_.append({Parent, Path, Remember, true});
    }
    ExplorerPanel::PickChoice None; None.name_ = tr("워크스페이스 없이 열기"); None.description_ = tr("문서 모드로 둡니다.");
    Choices.append(None); ancestorPicks_.append({QString(), Path, Remember, false});
    Choices[Default].checked_ = true;
    explorer_->ShowPick(QFileInfo(Path).fileName(), Choices);
    // 탐색기가 닫혀 있거나 접혀 있으면 펼친다. 키보드 포커스는 옮기지 않는다.
    if (explorerTool_) { if (explorerTool_->isClosed()) explorerTool_->toggleView(true); if (explorerTool_->isAutoHidden()) explorerTool_->bringToFront(); }
}
void DocumentWorkspaceWidget::openAncestor(const QString& Root, const QString& Path, const QString& Mode)
{
    if (!workspaceRoot_.isEmpty() || workspace::IsInside(Root, recoveryRoot_, false)) return;
    {
        const QScopedValueRollback Guard(bindOnly_, Mode == QLatin1String("bind"));
        if (!bindWorkspace(workspace::NormalizeRoot(Root), WorkspaceOrigin::Ancestor)) return;
    }
    for (const auto& Each : documents_) if (workspace::SamePath(Each->path_, Path)) { activate(Each); break; }
    if (!explorer_) return;
    QStringList Actions{tr("되돌리기")};
    if (pendingSession_ && !pendingSession_->documents_.isEmpty()) Actions.append(tr("모두 열기"));
    explorer_->ShowNotice(QStringLiteral("ancestorOpened"), fm::ui::Banner::Info, tr("상위 폴더의 워크스페이스 %1을(를) 열었습니다").arg(FolderName(workspaceRoot_)),
        QDir::toNativeSeparators(workspaceRoot_), Actions);
}
void DocumentWorkspaceWidget::undoAncestorWorkspace()
{
    if (workspaceRoot_.isEmpty()) return;
    saveWorkspace();
    // 방금 연 파일과 원래 있던 문서는 남기고, 자동으로 되살린 문서 가운데 고치지 않은 것만 닫는다.
    for (const auto& Path : std::exchange(autoRestoredPaths_, {})) {
        const auto At = std::find_if(documents_.begin(), documents_.end(), [&Path](const auto& Each) { return workspace::SamePath(Each->path_, Path); });
        if (At != documents_.end() && !(*At)->Dirty()) closeTab(int(std::distance(documents_.begin(), At)));
    }
    RememberSetting(QStringLiteral("workspace/lastRoot"), QString());
    unbindWorkspace();
}
void DocumentWorkspaceWidget::openRemainingSessionDocuments()
{
    if (explorer_) explorer_->HideNotice();
    if (!pendingSession_) return;
    const auto State = *std::exchange(pendingSession_, std::nullopt);
    restoreWorkspaceDocuments(State, false);
}
void DocumentWorkspaceWidget::explorerRename(const QString& Path, const QString& NewName)
{
    if (!explorer_ || hosted_) return;
    // 열린 문서(그 폴더 아래 포함)는 닫았다가 새 이름으로 다시 연다. 저장하지 않은 변경이 있으면 바꾸지 않는다.
    QList<std::shared_ptr<Tab>> Affected;
    for (const auto& Each : documents_) if (!Each->closed_ && !Each->path_.isEmpty() && Each->temporarySource_.isEmpty() && workspace::IsInside(Each->path_, Path, false)) Affected.append(Each);
    for (const auto& Each : Affected) if (Each->Dirty() || Each->saving_ || Each->loading_ || Each->pane_->SqliteDirty()) {
        explorer_->FinishRename(Path, {}, tr("%1에 저장하지 않은 변경이 있습니다. 저장하거나 닫은 뒤 다시 바꾸십시오.").arg(documentName(*Each))); return;
    }
    const auto Current = active();
    struct Reopen { QString path_; bool active_ = false; };
    QList<Reopen> Again;
    for (const auto& Each : Affected) {
        Again.append({Each->path_, Each == Current});
        const auto At = std::find(documents_.begin(), documents_.end(), Each);
        if (At != documents_.end()) closeTab(int(std::distance(documents_.begin(), At)));
        if (std::find(documents_.begin(), documents_.end(), Each) != documents_.end()) {
            explorer_->FinishRename(Path, {}, tr("%1을(를) 닫지 못해 이름을 바꾸지 않았습니다.").arg(documentName(*Each))); return;
        }
    }
    QString Renamed;
    const auto Result = explorer::RenameEntry(Path, NewName, &Renamed);
    const auto From = workspace::NormalizePath(Path);
    std::shared_ptr<Tab> Focus;
    for (const auto& Item : Again) {
        const auto Target = Result.ok_ ? Renamed + workspace::NormalizePath(Item.path_).mid(From.size()) : Item.path_;
        auto Opened = find(open(Target, false, {}, false)["documentId"].toString());
        if (Item.active_) Focus = Opened;
    }
    if (Focus) activate(Focus);
    explorer_->FinishRename(Path, Result.ok_ ? Renamed : QString(), Result.ok_ ? QString() : Result.error_);
    update();
}
void DocumentWorkspaceWidget::openFromExplorer(const QString& Path, bool Beside)
{
    // HyperViewer 는 지금 문서를 바꾼다. HyperEditor 는 탭으로 연다(옆에 열기는 포커스 문서 묶음의 오른쪽).
    if (hosted_) {
        const auto Reply = open(Path, true, {}, false);
        if (Reply.contains("error")) ShowShellError(Message(Reply["error"].toString()));
        return;
    }
    const auto Before = active();
    const auto Reply = open(Path, false, {}, false);
    if (Reply.contains("error")) { ShowShellError(Message(Reply["error"].toString())); return; }
    if (!Beside || !tabbed_) return;
    const auto Opened = find(Reply["documentId"].toString());
    if (Opened && Before && Opened != Before && Before->dock_ && Before->dock_->dockArea() && !Before->dock_->isAutoHidden())
        docks_->moveDockWidget(Opened->dock_, fm::dock::DropArea::Right, Before->dock_->dockArea());
}
}
