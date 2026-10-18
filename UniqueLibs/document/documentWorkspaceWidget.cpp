#include "documentWorkspaceWidget.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include "documentViewPane.hpp"
#include "documentThemeAdapter.hpp"
#include "documentSettingsDialog.hpp"
#include "documentShortcutRouter.hpp"
#include "text/ScintillaQtDirectBackend.h"
#include <Scintilla.h>
#include "UniqueLibs/settings/appSettings.hpp"
#include "text/QTextView.h"
#include <QFileSystemWatcher>
#include <QDynamicPropertyChangeEvent>
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QtConcurrent>
#include <QFutureWatcher>
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
#include <QRegularExpression>
#include <QBuffer>
#include "capture/CaptureOverlay.h"
#include "core/PrintManager.h"
#include "core/AppSettings.h"
#include "core/ExternalChangeWatcher.h"
#include "core/WatermarkDialog.h"

namespace hc::document
{
    namespace
    {
        // toPlainText substitutes NBSP and Unicode line separators. Preserve
        // scalar text while mapping Qt document block separators to LF.
        QString EditorText(const QPlainTextEdit* Editor)
        { return Editor->property("scalarText").isValid() ? Editor->property("scalarText").toString() : Editor->document()->toRawText().replace(QChar(0x2029), QChar('\n')); }
        QJsonObject Error(const QString& Code) { return {{"error", Code}}; }
        QString Message(const QString& Code)
        {
            if (Code.contains("external") || Code.contains("stale") || Code.contains("changed"))
                return QObject::tr("원본이 변경되었습니다. 다시 읽거나 다른 이름으로 저장하십시오.");
            if (Code.contains("cancel")) return QObject::tr("작업을 취소했습니다.");
            if (Code.contains("missing") || Code.contains("not-found")) return QObject::tr("파일을 찾을 수 없습니다.");
            if (Code.contains("denied") || Code.contains("access")) return QObject::tr("파일에 접근할 권한이 없습니다.");
            if (Code.contains("budget") || Code.contains("byte-limit")) return QObject::tr("텍스트는 16 MiB, 문서 입력은 64 MiB 이하를 지원합니다.");
            if (Code.contains("utf8") || Code.contains("eol")) return QObject::tr("문자의 인코딩과 파일 형식을 확인하십시오.");
            if (Code.contains("recovery")) return QObject::tr("복구 사본 작업에 실패했습니다. 기존 사본을 유지합니다.");
            if (Code.contains("original-save")) return QObject::tr("원본 저장 연결이 없습니다. 다른 이름으로 저장하십시오.");
            if (Code.contains("cleanup")) return QObject::tr("저장은 완료했으며 정리 작업을 다시 확인해야 합니다.");
            return QObject::tr("작업을 완료하지 못했습니다. 다시 시도하거나 다른 이름으로 저장하십시오.");
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
        QString ChooseSavePath(QWidget* Parent,const QString& Current,const QString& ViewId)
        {
            const auto Filters=ViewId=="image"?QStringLiteral("PNG (*.png);;JPEG (*.jpg *.jpeg);;BMP (*.bmp);;TIFF (*.tif *.tiff);;WebP (*.webp)")
                :ViewId=="pdf"?QStringLiteral("PDF (*.pdf)"):QStringLiteral("Text files (*.*)");
            QFileDialog Dialog(Parent,QObject::tr("다른 이름 저장"),Current,Filters);Dialog.setAcceptMode(QFileDialog::AcceptSave);
            Dialog.setDefaultSuffix(ViewId=="image"?"png":ViewId=="pdf"?"pdf":QFileInfo(Current).suffix());
            QObject::connect(&Dialog,&QFileDialog::filterSelected,&Dialog,[&Dialog](const QString& Filter){
                const auto Match=QRegularExpression("\\*\\.([a-z0-9]+)").match(Filter);if(Match.hasMatch())Dialog.setDefaultSuffix(Match.captured(1));
            });
            return Dialog.exec()==QDialog::Accepted?Dialog.selectedFiles().value(0):QString{};
        }
        bool StoreCopy(const QString& Path, const QByteArray& Bytes)
        {
            QSaveFile File(Path); File.setDirectWriteFallback(false);
            return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
        }
    }
    struct DocumentWorkspaceWidget::Tab
    {
        QString id_ = QUuid::createUuid().toString(QUuid::Id128), path_, error_;
        QString sourceToken_, pendingToken_, recoveredName_, temporarySource_;
        QByteArray pendingHash_;
        std::uint64_t checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
        std::shared_ptr<DocumentSession> session_ = std::make_shared<DocumentSession>(id_.toStdString());
        std::optional<SaveTicket> pending_;
        TextInput input_;
        QPointer<DocumentViewPane> pane_;
        QPointer<QMessageBox> externalPrompt_;
        QPointer<QPlainTextEdit> editor_;
        std::shared_ptr<std::stop_source> cancellation_ = std::make_shared<std::stop_source>();
        bool loading_ = true, saving_ = false, hostSlot_ = false, broker_ = false;
        bool originalAuthority_ = false, originalSave_ = true, closed_ = false, checkpointing_ = false, closeAfterSave_ = false, recovered_ = false;
        QByteArray pendingBytes_;
        bool Dirty() const { return session_->Snapshot().dirty_; }
        std::uint64_t Revision() const { return session_->Snapshot().revision_; }
    };
    DocumentWorkspaceWidget::DocumentWorkspaceWidget(QString RecoveryRoot, bool Hosted, QWidget* Parent)
        : QWidget(Parent), recoveryRoot_(std::move(RecoveryRoot)),
          workspaceId_(QUuid::createUuid().toString(QUuid::Id128)), hosted_(Hosted)
    {
        DocumentThemeAdapter::Install(*qApp);
        recovery_ = std::make_shared<DocumentRecoveryStore>(recoveryRoot_);
        setObjectName("documentWorkspace"); setWindowTitle(Hosted ? "HyperViewer" : "HyperEditor"); resize(1000, 700);
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(0);
        menu_ = new QMenuBar(this); toolbar_ = new QToolBar(tr("공통 도구모음"), this);
        menu_->setObjectName("mainMenu"); toolbar_->setObjectName("mainToolbar");
        tabs_ = new QTabWidget(this); tabs_->setObjectName("documentTabs"); tabs_->setTabsClosable(true);
        singlePane_ = new QStackedWidget(this); singlePane_->setObjectName("singleDocument"); singlePane_->hide();
        status_ = new QLabel(tr("PDF·텍스트·이미지를 열어 편집할 수 있습니다."), this); status_->setContentsMargins(8, 4, 8, 4);
        Layout->addWidget(menu_); Layout->addWidget(toolbar_); Layout->addWidget(tabs_, 1); Layout->addWidget(status_);
        chrome_ = new WorkspaceChromeController(menu_, toolbar_, this);
        auto* FileMenu = menu_->addMenu(tr("파일(&F)"));
        auto Add = [&](const QString& Label, const QKeySequence& Key, const char* Id, auto Callback) {
            auto* Action = FileMenu->addAction(Label); Action->setShortcut(Key); Action->setObjectName(Id); toolbar_->addAction(Action); addAction(Action);
            connect(Action, &QAction::triggered, this, Callback); return Action;
        };
        Add(tr("새 문서(&N)"), QKeySequence("Ctrl+N"), "newDocument", [this] { newDocument(); });
        Add(tr("열기(&O)"), QKeySequence::Open, "openDocument", [this] {
            QStringList Rejected;
            for (const auto& Path : QFileDialog::getOpenFileNames(this, tr("문서 열기")))
                if (open(Path, false, {}, false).contains("error")) Rejected.append(Path);
            ShowShellError(Rejected.isEmpty() ? QString{} : tr("문서 한도 때문에 열지 못한 파일: %1").arg(Rejected.join(", ")));
        });
        Add(tr("저장(&S)"), QKeySequence::Save, "saveDocument", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->broker_) { auto Result = prepareSave(Tab); if (Result.contains("error")) Tab->error_ = Result["error"].toString(); update(); }
            else if (Tab->originalSave_) save(Tab, Tab->path_);
            else {
                const auto Path = ChooseSavePath(this,Tab->path_,Tab->pane_->ViewId());
                if (!Path.isEmpty()) save(Tab, Path, true); else { Tab->closeAfterSave_ = false; closingWorkspace_ = false; }
            }
        });
        Add(tr("다른 이름 저장"), QKeySequence::SaveAs, "saveDocumentAs", [this] {
            auto Tab = active(); if (!Tab) return;
            const auto Path = ChooseSavePath(this,Tab->path_,Tab->pane_->ViewId());
            if (!Path.isEmpty()) save(Tab, Path, true);
        });
        Add(tr("다시 읽기"), QKeySequence("Ctrl+R"), "reloadDocument", [this] {
            auto Tab = active(); if (!Tab) return;
            if (Tab->Dirty() && QMessageBox::question(this, tr("다시 읽기"), tr("편집 내용을 폐기하고 원본을 다시 읽으시겠습니까?"),
                QMessageBox::Discard | QMessageBox::Cancel) != QMessageBox::Discard) return;
            load(Tab, true);
        });
        Add(tr("저장 충돌 처리"), {}, "resolveDocumentConflict", [this] {
            auto Tab = active(); if (!Tab) return;
            QMessageBox Dialog(QMessageBox::Warning, tr("원본 변경"), tr("원본이 변경되었습니다. 처리 방법을 선택하십시오."), QMessageBox::Cancel, this);
            auto* Overwrite = Dialog.addButton(tr("현재 내용으로 덮어쓰기"), QMessageBox::AcceptRole);
            auto* Reload = Dialog.addButton(tr("편집 폐기 후 다시 읽기"), QMessageBox::DestructiveRole);
            auto* SaveAs = Dialog.addButton(tr("다른 이름 저장"), QMessageBox::ActionRole);
            Dialog.exec();
            if (Dialog.clickedButton() == Overwrite) save(Tab, Tab->path_, true);
            else if (Dialog.clickedButton() == Reload) load(Tab, true);
            else if (Dialog.clickedButton() == SaveAs) findChild<QAction*>("saveDocumentAs")->trigger();
        });
        Add(tr("닫기(&W)"), QKeySequence::Close, "closeDocument", [this] {
            auto Tab = active(); if (Tab) closeTab(int(std::distance(documents_.begin(), std::find(documents_.begin(), documents_.end(), Tab))));
        });
        Add(tr("작업 취소"), QKeySequence("Ctrl+."), "cancelDocument", [this] {
            if (auto Tab = active()) { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->pane_->Cancel(); }
        });
        Add(tr("인쇄"),QKeySequence::Print,"printDocument",[this]{if(auto Tab=active())Tab->pane_->Execute({{"action","print"}});});
        Add(tr("인쇄 미리보기"),{},"previewDocument",[this]{if(auto Tab=active())Tab->pane_->Execute({{"action","printPreview"}});});
        Add(tr("화면 캡처"),QKeySequence("Alt+Shift+S"),"captureScreen",[this]{
            if(clipboardBroker_)return;const auto Captured=Capture::CaptureOverlay::run(this);if(!Captured.accepted||Captured.image.isNull())return;
            QDir().mkpath(recoveryRoot_);const auto Path=recoveryRoot_+"/capture-"+QUuid::createUuid().toString(QUuid::Id128)+".png";
            QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
            if(!Captured.image.save(&Buffer,"PNG")||!StoreCopy(Path,Bytes)){ShowShellError(tr("캡처 사본을 만들지 못했습니다."));return;}
            auto Reply=open(Path,false,{},false);if(auto Tab=find(Reply["documentId"].toString())){Tab->originalAuthority_=false;Tab->temporarySource_=Path;}
        });
        auto* EditMenu = menu_->addMenu(tr("편집(&E)"));
        for (const auto& Item : {std::pair{tr("실행 취소"), QKeySequence::Undo}, std::pair{tr("다시 실행"), QKeySequence::Redo}}) {
            auto* Action = EditMenu->addAction(Item.first); Action->setShortcut(Item.second); toolbar_->addAction(Action); addAction(Action);
            Action->setObjectName(Item.second == QKeySequence::Undo ? "undoDocument" : "redoDocument");
            connect(Action, &QAction::triggered, this, [this, Key = Item.second] {
                if (auto Tab = active()) { if (Key == QKeySequence::Undo) Tab->pane_->Undo(); else Tab->pane_->Redo(); }
            });
        }
        auto* ViewMenu = menu_->addMenu(tr("보기(&V)"));
        auto* Preferences = ViewMenu->addAction(tr("설정(&P)…"));
        Preferences->setObjectName("documentPreferences"); Preferences->setShortcut(QKeySequence("Ctrl+,")); addAction(Preferences);
        connect(Preferences, &QAction::triggered, this, &DocumentWorkspaceWidget::showPreferences);
        auto* Options = new QAction(tr("표시 옵션"), this); Options->setObjectName("workspaceDisplayOptions");
        Options->setShortcut(QKeySequence("Ctrl+Shift+M")); addAction(Options);
        connect(Options, &QAction::triggered, this, [this, ViewMenu] { ViewMenu->exec(mapToGlobal(QPoint(8, 8))); });
        setContextMenuPolicy(Qt::ActionsContextMenu); addAction(Options);
        for (const bool Menu : {true, false}) {
            auto* Modes = ViewMenu->addMenu(Menu ? tr("주 메뉴 표시") : tr("공통 도구모음 표시"));
            for (const auto& Mode : {QString("recommended"), QString("show"), QString("hide")}) {
                auto* Action = Modes->addAction(Mode == "recommended" ? tr("뷰 권장값") : Mode == "show" ? tr("표시") : tr("숨기기"));
                connect(Action, &QAction::triggered, this, [this, Menu, Mode] {
                    chrome_->SetGlobal(Menu, Mode); emit ChromeSettingsChanged(chrome_->Settings());
                });
            }
            auto* Overrides = Modes->addMenu(tr("현재 뷰 설정"));
            for (const auto& Mode : {QString("inherit"), QString("show"), QString("hide")}) {
                auto* Action = Overrides->addAction(Mode == "inherit" ? tr("전역 설정 사용") : Mode == "show" ? tr("표시") : tr("숨기기"));
                connect(Action, &QAction::triggered, this, [this, Menu, Mode] {
                    const auto Tab = active(); if (!Tab) return;
                    chrome_->SetViewOverride(Tab->pane_->Contribution().viewId_, Menu, Mode); emit ChromeSettingsChanged(chrome_->Settings());
                });
            }
        }
        auto* Watermark=ViewMenu->addAction(tr("워터마크"));connect(Watermark,&QAction::triggered,this,[this]{if(auto Tab=active()){if(!Tab->pane_->View())return;WatermarkDialog Dialog(Tab->pane_->View()->watermark(),this);if(Dialog.exec()==QDialog::Accepted)Tab->pane_->View()->setWatermark(Dialog.result());}});
        auto* Protection=ViewMenu->addAction(tr("화면 캡처 보호"));Protection->setCheckable(true);connect(Protection,&QAction::toggled,this,[this](bool Enabled){if(auto Tab=active()){auto Reply=Tab->pane_->Execute({{"action","captureProtection"},{"enabled",Enabled}});if(Reply.contains("error"))ShowShellError(tr("이 호스트에서는 화면 캡처 보호를 적용할 수 없습니다."));}});
        auto* Themes = ViewMenu->addMenu(tr("테마"));
        for (const auto& Mode : {QString("system"), QString("light"), QString("dark")}) {
            auto* Action = Themes->addAction(Mode == "system" ? tr("시스템") : Mode == "light" ? tr("라이트") : tr("다크"));
            connect(Action, &QAction::triggered, this, [this, Mode] {
                DocumentThemeAdapter::SetMode(Mode); auto Settings = chrome_->Settings(); Settings["themeMode"] = Mode;
                chrome_->ApplySettings(Settings); emit ChromeSettingsChanged(Settings);
            });
        }
        auto* Restore = FileMenu->addAction(tr("복구 목록"));
        connect(Restore, &QAction::triggered, this, [this] {
            auto* Watcher = new QFutureWatcher<storage::Result<std::vector<RecoverySnapshot>>>(this); ++recoveryWorkers_;
            connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher] {
                auto Result = Watcher->result(); Watcher->deleteLater(); --recoveryWorkers_;
                if (closingWorkspace_) { tryFinishClose(); return; }
                if (!Result.HasValue()) { ShowShellError(Message(QString::fromStdString(Result.Failure().code_))); return; }
                QStringList Labels, Paths;
                for (const auto& Record : Result.Value()) {
                    Labels.append(QString::fromStdString(Record.document_.source_.localPath_) + " · " + QString::number(Record.document_.revision_) + " · " + QString::fromStdString(Record.document_.id_).left(8));
                    Paths.append(QString::fromStdString(Record.snapshotPath_));
                }
                if (Paths.isEmpty()) { QMessageBox::information(this, tr("복구 목록"), tr("복구할 문서가 없습니다.")); return; }
                bool Accepted = false; const auto Item = QInputDialog::getItem(this, tr("복구 목록"), tr("복구할 문서"), Labels, 0, false, &Accepted);
                if (Accepted) Handle({{"action", "recover"}, {"path", Paths.value(Labels.indexOf(Item))}});
            });
            auto Recovery = recovery_; Watcher->setFuture(QtConcurrent::run([Recovery] { return Recovery->List(); }));
        });
        connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int Index) {
            auto* Pane = tabs_->widget(Index); for (std::size_t At = 0; At < documents_.size(); ++At) if (documents_[At]->pane_ == Pane) { closeTab(int(At)); break; }
        });
        connect(tabs_, &QTabWidget::currentChanged, this, [this] {
            if(auto Tab=active())setFocusProxy(Tab->pane_->InputWidget());
            update();
        });
        for (const auto& Item : {std::pair{QString("tab.next"), 1}, std::pair{QString("tab.previous"), -1}}) {
            auto* Action = ViewMenu->addAction(Item.second == 1 ? tr("다음 탭") : tr("이전 탭")); Action->setObjectName(Item.first); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Step = Item.second] {
                if (!tabbed_) return; const auto Count = tabs_->count();
                if (Count) tabs_->setCurrentIndex((tabs_->currentIndex() + Step + Count) % Count);
                if (auto Tab = active()) Tab->pane_->Focus();
            });
        }
        for (const auto& Command : DocumentShortcutRegistry::Commands()) {
            if (!Command.id_.startsWith("text.") && !QStringList{"edit.cut", "edit.copy", "edit.paste", "edit.selectAll"}.contains(Command.id_)) continue;
            auto* Action = EditMenu->addAction(Command.label_); Action->setObjectName(Command.id_); addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Id = Command.id_] {
                auto Tab = active(); if (!Tab || Tab->loading_) return;
                auto* Text = Tab->pane_->findChild<QTextView*>(); if (!Text) return;
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
        auto* Recovery = new QTimer(this); Recovery->setInterval(500);
        connect(Recovery, &QTimer::timeout, this, &DocumentWorkspaceWidget::Checkpoint); Recovery->start();
        const QMap<QString, QString> Commands{{"newDocument", "file.new"}, {"undoDocument", "edit.undo"}, {"redoDocument", "edit.redo"}, {"openDocument", "file.openWorkspace"}, {"saveDocument", "file.save"},
            {"saveDocumentAs", "file.saveAs"}, {"closeDocument", "tab.close"}, {"reloadDocument", "file.reload"},
            {"printDocument", "file.print"}, {"previewDocument", "file.printPreview"}, {"cancelDocument", "file.cancel"},
            {"captureScreen", "capture.screen"}, {"workspaceDisplayOptions", "view.displayOptions"}, {"documentPreferences", "app.settings"}};
        qApp->installEventFilter(this);
        if (hosted_) {
            viewerEscape_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
            viewerEscape_->setAutoRepeat(false);
            connect(viewerEscape_, &QShortcut::activated, this, &DocumentWorkspaceWidget::escapeViewer);
        }
        shortcutRouter_ = new DocumentShortcutRouter(this);
        for (const auto& Command : DocumentShortcutRegistry::Commands()) {
            const auto Name = Commands.key(Command.id_, Command.id_);
            if (auto* Action = findChild<QAction*>(Name)) {
                Action->setProperty("documentCommandId", Command.id_); Action->setProperty("documentDefaultShortcut", Command.key_);
                shortcutRouter_->Register(Command.id_, Action);
            }
        }
        shortcutRouter_->SetEligibility([this](const QString& Id, QWidget* Focus) {
            auto Tab = active();
            if (Id.startsWith("text.")) return Tab && !Tab->loading_ && Tab->pane_->ViewId() == "text";
            if (Id.startsWith("edit.")) {
                auto* Input = Tab ? Tab->pane_->InputWidget() : nullptr;
                return Input && !Tab->loading_ && (Focus == Input || Input->isAncestorOf(Focus));
            }
            return true;
        });
        const auto Override = qApp->property("documentViewsConfigDir");
        settingsPath_ = hc::settings::UnifiedConfig::FileName(Override.isValid() ? Override.toString() : hc::settings::DocumentPaths().directory_);
        settingsWatcher_ = new QFileSystemWatcher(this);
        auto* Reload = new QTimer(this); Reload->setSingleShot(true); Reload->setInterval(80);
        connect(settingsWatcher_, &QFileSystemWatcher::directoryChanged, Reload, qOverload<>(&QTimer::start));
        connect(Reload, &QTimer::timeout, this, &DocumentWorkspaceWidget::refreshPreferences);
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive) refreshPreferences(); });
        externalWatcher_ = new mrst::ExternalChangeWatcher(this);
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileChanged, this, [this](const QString& Path) { externalChange(Path, false); });
        connect(externalWatcher_, &mrst::ExternalChangeWatcher::sigFileVanished, this, [this](const QString& Path) { externalChange(Path, true); });
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) { if (State == Qt::ApplicationActive) externalWatcher_->recheckAll(); });
        refreshPreferences();
    }
    DocumentWorkspaceWidget::~DocumentWorkspaceWidget()
    {
        delete preferences_.data();
        // Children can emit modified/history signals while their PDF or image
        // resources are closed. Disconnect before members start destruction.
        for(const auto& Tab:documents_){Tab->closed_=true;Tab->cancellation_->request_stop();}
        for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);
    }
    void DocumentWorkspaceWidget::showPreferences()
    {
        if (preferences_) { preferences_->showNormal(); preferences_->raise(); preferences_->activateWindow(); return; }
        // A hosted workspace has a foreign-process native parent. A modal Qt
        // dialog owned by that embedded child can be hidden behind the host
        // while its nested event loop blocks input. Use an independent modeless
        // window in F3, with explicit lifetime management and no host disabling.
        auto* Dialog = new DocumentSettingsDialog(settingsPath_, findChildren<QAction*>(), hosted_ ? nullptr : this);
        preferences_ = Dialog; Dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(Dialog, &DocumentSettingsDialog::PreferencesSaved, this, &DocumentWorkspaceWidget::refreshPreferences);
        if (!hosted_) Dialog->setWindowModality(Qt::WindowModal);
        const auto Available = screen()->availableGeometry();
        const auto Center = mapToGlobal(rect().center());
        Dialog->move(qBound(Available.left(), Center.x() - Dialog->width() / 2, qMax(Available.left(), Available.right() - Dialog->width())),
            qBound(Available.top(), Center.y() - Dialog->height() / 2, qMax(Available.top(), Available.bottom() - Dialog->height())));
        Dialog->show();
        // The hosted process starts with STARTF_USESHOWWINDOW/SW_HIDE. Its
        // first top-level ShowWindow can consume that startup state and stay
        // hidden; this user-invoked dialog needs an explicit second show.
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
            auto* Dialog = new QMessageBox(QMessageBox::Warning, tr("원본 파일 변경"),
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
    void DocumentWorkspaceWidget::refreshPreferences()
    {
        auto Directory = QFileInfo(settingsPath_).absolutePath();
        while (!QDir(Directory).exists()) { const auto Parent = QFileInfo(Directory).absolutePath(); if (Parent == Directory) break; Directory = Parent; }
        if (QDir(Directory).exists() && !settingsWatcher_->directories().contains(Directory)) settingsWatcher_->addPath(Directory);
        QJsonObject Root; QString Error;
        if (!hc::settings::UnifiedConfig::Load(settingsPath_, Root, Error)) { ShowShellError(tr("설정을 읽지 못했습니다: %1\n%2").arg(settingsPath_, Error)); return; }
        closeViewerOnEscape_ = Root["documentViews"].toObject()["common"].toObject()["closeViewerOnEscape"].toBool(true);
        if (viewerEscape_) viewerEscape_->setEnabled(closeViewerOnEscape_);
        const auto Chrome = Root["appearance"].toObject()["documentWorkspace"].toObject();
        const QJsonObject Relevant{{"documentViews", Root.value("documentViews")}, {"chrome", Chrome}};
        if (Relevant == appliedPreferences_) return;
        appliedPreferences_ = Relevant;
        if (externalWatcher_) externalWatcher_->reloadSettings();
        if (!Chrome.isEmpty() && !ApplyChromeSettings(Chrome)) ShowShellError(tr("문서 표시 설정이 올바르지 않습니다."));
        for (auto* View : findChildren<QTextView*>()) View->ReloadPreferences();
        const auto Bindings = Root["documentViews"].toObject()["shortcuts"].toObject()["bindings"].toObject();
        const auto ShortcutError = shortcutRouter_->Apply(Bindings);
        if (!ShortcutError.isEmpty()) ShowShellError(tr("단축키 설정 오류 · 기본값을 사용합니다: %1").arg(ShortcutError));
        else if (const auto Unknown = DocumentShortcutRegistry::Unknown(Bindings); !Unknown.isEmpty())
            ShowShellError(tr("지원하지 않는 단축키 항목을 보존했습니다: %1").arg(Unknown.join(", ")));
    }
    QJsonObject DocumentWorkspaceWidget::newDocument()
    {
        if (documents_.size() >= 16 || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        const auto Path = recoveryRoot_ + "/untitled-" + QUuid::createUuid().toString(QUuid::Id128) + ".txt";
        if (!QDir().mkpath(recoveryRoot_) || !StoreCopy(Path, {})) return Error("document-new-file-failed");
        const auto Reply = open(Path, false, {}, false);
        if (auto Tab = find(Reply["documentId"].toString())) {
            Tab->originalAuthority_ = false; Tab->originalSave_ = false;
            Tab->temporarySource_ = Path; Tab->recoveredName_ = tr("새 문서");
        } else QFile::remove(Path);
        return Reply;
    }
    QWidget* DocumentWorkspaceWidget::currentPane() const { return tabbed_ ? tabs_->currentWidget() : singlePane_->currentWidget(); }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::active() const
    { for (const auto& Tab : documents_) if (Tab->pane_ == currentPane()) return Tab; return {}; }
    std::shared_ptr<DocumentWorkspaceWidget::Tab> DocumentWorkspaceWidget::find(const QString& Id) const
    { for (const auto& Tab : documents_) if (Tab->id_ == Id && !Tab->closed_) return Tab; return {}; }
    void DocumentWorkspaceWidget::activate(const std::shared_ptr<Tab>& Tab)
    {
        if (tabbed_) tabs_->setCurrentWidget(Tab->pane_); else singlePane_->setCurrentWidget(Tab->pane_);
        workspace_.Activate(Tab->id_.toStdString()); update();
    }
    bool DocumentWorkspaceWidget::SetTabbed(bool Enabled)
    {
        if (!documents_.empty()) return false;
        if (Enabled == tabbed_) return true;
        auto* Layout = qobject_cast<QVBoxLayout*>(layout()); Layout->removeWidget(tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_);
        tabs_->hide(); singlePane_->hide(); tabbed_ = Enabled;
        Layout->insertWidget(2, tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_, 1);
        (tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_)->show(); return true;
    }
    bool DocumentWorkspaceWidget::ApplyChromeSettings(const QJsonObject& Settings)
    {
        if (Settings.contains("themeMode") && !QStringList{"system", "light", "dark"}.contains(Settings["themeMode"].toString())) return false;
        if (!chrome_->ApplySettings(Settings)) return false;
        if (Settings.contains("themeMode")) DocumentThemeAdapter::SetMode(Settings["themeMode"].toString()); return true;
    }
    QJsonObject DocumentWorkspaceWidget::ChromeSettings() const { return chrome_->Settings(); }
    void DocumentWorkspaceWidget::ShowShellError(const QString& Message) { shellError_ = Message; update(); }
    bool DocumentWorkspaceWidget::IsIdle() const
    {
        return !recoveryWorkers_ && std::none_of(documents_.begin(), documents_.end(), [](const auto& Tab) {
            return Tab->loading_ || Tab->saving_ || Tab->checkpointing_ || !Tab->pendingToken_.isEmpty() || Tab->pane_->IsBusy();
        });
    }
    QJsonObject DocumentWorkspaceWidget::open(const QString& Path, bool HostSlot, const QString& SourceToken, bool Broker, const QString& RecoveryPath)
    {
        if (!QFileInfo(Path).isAbsolute() || Path.contains(QChar::Null)) return Error("document-open-path");
        for (const auto& Tab : documents_) if (Tab->hostSlot_ && HostSlot) {
            if (Tab->loading_ || Tab->saving_ || !Tab->pendingToken_.isEmpty() || Tab->pane_->IsBusy()) {
                pendingHostOpen_ = {{"path", Path}, {"sourceToken", SourceToken}, {"broker", Broker}};
                return {{"replacing", true}, {"documentId", Tab->id_}};
            }
            if (Tab->Dirty()) {
                if (Tab->session_->Text().size() > DocumentByteLimit) return Error("p0-text-byte-limit");
                // SDK owns one slot; a newer pending Load supersedes its predecessor.
                pendingHostOpen_ = {{"path", Path}, {"sourceToken", SourceToken}, {"broker", Broker}};
                Tab->editor_->setReadOnly(true); Checkpoint(); return {{"replacing", true}, {"documentId", Tab->id_}};
            }
            retire(Tab, true); break;
        }
        for (const auto& Tab : documents_) if (QFileInfo(Tab->path_).canonicalFilePath() == QFileInfo(Path).canonicalFilePath()
            && !HostSlot && !QFileInfo(Path).canonicalFilePath().isEmpty() && Tab->broker_ == Broker && Tab->sourceToken_ == SourceToken) {
            activate(Tab); return {{"documentId", Tab->id_}, {"duplicate", true}};
        }
        if (documents_.size() >= 16 || (!tabbed_ && !documents_.empty())) return Error("document-workspace-limit");
        auto Tab = std::make_shared<DocumentWorkspaceWidget::Tab>(); Tab->path_ = Path;
        Tab->hostSlot_ = HostSlot; Tab->sourceToken_ = SourceToken; Tab->broker_ = Broker; Tab->recovered_ = !RecoveryPath.isEmpty();
        // Authority follows the source: SDK slots have no original-provider link,
        // while explicitly opened local files are writable in either product.
        Tab->originalAuthority_ = !HostSlot && !Broker && SourceToken.isEmpty() && !Tab->recovered_; Tab->originalSave_ = Tab->originalAuthority_;
        auto* Pane = new DocumentViewPane(tabbed_ ? static_cast<QWidget*>(tabs_) : singlePane_);
        auto* Editor = Pane->Editor(); Tab->pane_ = Pane; Tab->editor_ = Editor;
        connect(Pane, &DocumentViewPane::ClipboardRequested, this, [this, Tab](const QString& Mode) { clipboard(Tab, Mode); });
        Editor->installEventFilter(this); Editor->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(Editor, &QWidget::customContextMenuRequested, this, [this, Tab, Editor](const QPoint& Position) {
            if (!clipboardBroker_) { auto* Menu = Editor->createStandardContextMenu(); Menu->exec(Editor->mapToGlobal(Position)); delete Menu; return; }
            QMenu Menu;
            for (const auto& Mode : {QString("copy"), QString("cut"), QString("paste")}) {
                auto* Action = Menu.addAction(Mode == "copy" ? tr("복사") : Mode == "cut" ? tr("잘라내기") : tr("붙여넣기"));
                connect(Action, &QAction::triggered, this, [this, Tab, Mode] { clipboard(Tab, Mode); });
            }
            Menu.exec(Editor->mapToGlobal(Position));
        });
        connect(Editor, &QPlainTextEdit::textChanged, this, [this, Tab] {
            if (Tab->loading_ || Tab->closed_) return;
            const auto Text = EditorText(Tab->editor_);
            auto Result = !Text.isValidUtf16() || EncodeText(Text, Tab->input_.bom_, Tab->input_.eol_).size() > TextByteLimit
                ? storage::Result<storage::Unit>{storage::Error{storage::ErrorCategory::BudgetExceeded, "p0-text-byte-limit"}}
                : Tab->session_->Execute("document.setText", Tab->pane_->Content().toStdString());
            if (!Result.HasValue()) {
                QSignalBlocker Block(Tab->editor_);
                if (Tab->editor_->document()->isUndoAvailable()) Tab->editor_->undo();
                if (EditorText(Tab->editor_).toUtf8().toStdString() != Tab->session_->Text())
                    Tab->editor_->setPlainText(QString::fromUtf8(Tab->session_->Text()));
                Tab->error_ = QString::fromStdString(Result.Failure().code_);
            }
            else Tab->error_.clear();
            Tab->session_->SetHistoryState(Tab->pane_->CanUndo(),Tab->pane_->CanRedo()); update();
        });
        connect(Editor, &QPlainTextEdit::undoAvailable, this, [this, Tab](bool) {
            Tab->session_->SetHistoryState(Tab->editor_->document()->isUndoAvailable(), Tab->editor_->document()->isRedoAvailable()); update();
        });
        connect(Editor, &QPlainTextEdit::redoAvailable, this, [this, Tab](bool) {
            Tab->session_->SetHistoryState(Tab->editor_->document()->isUndoAvailable(), Tab->editor_->document()->isRedoAvailable()); update();
        });
        connect(Pane,&DocumentViewPane::ContentChanged,this,[this,Tab] {
            if(Tab->loading_||Tab->closed_)return;
            auto Bytes=Tab->pane_->SessionContent();
            if(Tab->pane_->ViewId()!="text"&&Bytes.isEmpty()){Tab->error_="document-serialization-failed";update();return;}
            auto Result=Tab->session_->Execute("document.setText",Bytes.toStdString());
            if(!Result.HasValue())Tab->error_=QString::fromStdString(Result.Failure().code_);else Tab->error_.clear();
            Tab->session_->SetHistoryState(Tab->pane_->CanUndo(),Tab->pane_->CanRedo());update();
        });
        connect(Pane,&DocumentViewPane::ViewStateChanged,this,[this,Tab]{if(!Tab->closed_&&!Tab->loading_){update();tryFinishClose();}});
        connect(Pane,&DocumentViewPane::OpenRequested,this,[this,Tab](const QString& Path) {if(!Tab->closed_)open(Path,false,{},false);});
        connect(Pane,&DocumentViewPane::SaveRequested,this,[this,Tab] {activate(Tab);findChild<QAction*>("saveDocumentAs")->trigger();});
        connect(Pane,&DocumentViewPane::EncodingRequested,this,[this,Tab](const QString&) {Tab->error_="document-encoding-unrepresentable";update();});
        documents_.push_back(Tab); workspace_.Add(Tab->session_);
        if (tabbed_) tabs_->addTab(Pane, tr("여는 중…")); else singlePane_->addWidget(Pane);
        activate(Tab); load(Tab, false, RecoveryPath); return {{"documentId", Tab->id_}, {"loading", true}};
    }
    void DocumentWorkspaceWidget::load(const std::shared_ptr<Tab>& Tab, bool Discard, const QString& RecoveryPath)
    {
        if (Tab->saving_ || !Tab->pendingToken_.isEmpty()) return;
        auto Started = Tab->session_->BeginOpen(Discard); if (!Started.HasValue()) { Tab->error_ = QString::fromStdString(Started.Failure().code_); update(); return; }
        Tab->cancellation_->request_stop(); Tab->cancellation_ = std::make_shared<std::stop_source>();
        Tab->loading_ = true; Tab->pane_->SetReadOnly(true); Tab->error_.clear();
        const auto Generation = Started.Value(); auto Cancellation = Tab->cancellation_; const auto Path = Tab->path_;
        auto* Watcher = new QFutureWatcher<storage::Result<TextInput>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Generation] {
            auto Result = Watcher->result(); Watcher->deleteLater();
            if (Tab->closed_ || Generation != Tab->session_->Snapshot().generation_) return;
            Tab->loading_ = false;
            if (!Result.HasValue()) {
                Tab->session_->CompleteOpen(Generation, Result.Failure()); Tab->error_ = QString::fromStdString(Result.Failure().code_);
            }
            else {
                Tab->input_ = std::move(Result.Value()); Tab->path_ = Tab->input_.path_;
                if (!Tab->temporarySource_.isEmpty() && !Tab->originalAuthority_ && Tab->input_.viewId_ == "text") {
                    AppSettings Settings;
                    const auto Encoding = Settings.value("textView/saveEncoding", "UTF-8").toString();
                    Tab->input_.encoding_ = QStringList{"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}.contains(Encoding) ? Encoding : "UTF-8";
                    const int Bom = Settings.value("textView/saveBomMode", 1).toInt();
                    Tab->input_.bom_ = Tab->input_.encoding_ != "CP949" && (Bom == 1 || (Bom == 0 && Tab->input_.encoding_.startsWith("UTF-16")));
                }
                if (Tab->input_.recoveryOrigin_) Tab->recoveredName_ = tr("복구 · ") + QFileInfo(QString::fromStdString(Tab->input_.recoveryOrigin_->localPath_)).fileName();
                Tab->originalSave_ = Tab->originalAuthority_ && !Tab->recovered_ && !(Tab->input_.source_.attributes_ & 1u);
                SourceIdentity Source{Tab->input_.source_, {}, Tab->path_.toUtf8().toStdString(), Tab->sourceToken_.toStdString(),
                    (Tab->originalSave_ || Tab->broker_) && !Tab->recovered_};
                Tab->loading_=true;
                const bool Loaded=Tab->pane_->Load(Tab->input_,Tab->originalAuthority_);
                Tab->loading_=false;
                if(!Loaded){Tab->session_->CompleteOpen(Generation,storage::Error{storage::ErrorCategory::InvalidRequest,"document-view-load"});Tab->error_="document-view-load";}
                else {
                    Tab->session_->SetBinaryContent(true);
                    Tab->session_->CompleteOpen(Generation,OpenedDocument{std::move(Source),Tab->pane_->SessionContent().toStdString()});
                    Tab->pane_->SetClipboardBroker(clipboardBroker_);
                    if(auto* Input=Tab->pane_->InputWidget()){if(hosted_&&Tab->pane_->ViewId()=="text")Input->setAttribute(Qt::WA_NativeWindow);Input->installEventFilter(this);for(auto* Child:Input->findChildren<QWidget*>())Child->installEventFilter(this);}
                }
            }
            if(active()==Tab&&Tab->session_->Snapshot().lifecycle_==DocumentLifecycle::Ready){
                setFocusProxy(Tab->pane_->InputWidget());
                if (!hosted_ || IsWindowVisible(reinterpret_cast<HWND>(winId()))) Tab->pane_->Focus();
            }
            update(); tryFinishClose();
        });
        auto Recovery = recovery_;
        Watcher->setFuture(QtConcurrent::run([Path, Cancellation, RecoveryPath, Recovery] {
            storage::OperationContext Context; Context.stopToken_ = Cancellation->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            if (!RecoveryPath.isEmpty()) {
                auto Record = Recovery->Read(RecoveryPath.toStdString()); if (!Record.HasValue()) return storage::Result<TextInput>{Record.Failure()};
                if (auto Stop = Context.Check({"document"})) return storage::Result<TextInput>{*Stop};
                if (!StoreCopy(Path, QByteArray::fromStdString(Record.Value().bytes_))) return storage::Result<TextInput>{storage::Error{storage::ErrorCategory::Unavailable, "document-recovery-copy"}};
                auto Input = ReadDocumentFile(Path, Context); if (Input.HasValue()) Input.Value().recoveryOrigin_ = Record.Value().document_.source_;
                return Input;
            }
            return ReadDocumentFile(Path, Context);
        })); update();
    }
    QJsonObject DocumentWorkspaceWidget::prepareSave(const std::shared_ptr<Tab>& Tab)
    {
        if (!Tab || Tab->loading_ || Tab->saving_ || !Tab->pendingToken_.isEmpty()) return Error("document-save-busy-or-unavailable");
        if (!Tab->broker_ || Tab->sourceToken_.isEmpty()) return Error("document-source-token-required");
        auto Started = Tab->session_->Save({SaveDisposition::Original, Tab->path_.toUtf8().toStdString()});
        if (!Started.HasValue()) return Error(QString::fromStdString(Started.Failure().code_));
        Tab->pending_ = Started.Value(); const auto Exported = Tab->pane_->Export(Tab->path_);
        Tab->pendingBytes_ = Exported.HasValue() ? Exported.Value() : QByteArray{};
        if (!Exported.HasValue() || Tab->pendingBytes_.size() > DocumentByteLimit) {
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
    void DocumentWorkspaceWidget::save(const std::shared_ptr<Tab>& Tab, const QString& Destination, bool Overwrite)
    {
        if (!Tab || Tab->loading_ || Tab->saving_ || !Tab->pendingToken_.isEmpty()) return;
        const auto TargetCanonical = QFileInfo(Destination).canonicalFilePath();
        const bool Original = QFileInfo(Destination).absoluteFilePath() == QFileInfo(Tab->path_).absoluteFilePath()
            || (!TargetCanonical.isEmpty() && TargetCanonical == QFileInfo(Tab->path_).canonicalFilePath());
        if (Original && (!Tab->originalSave_ || Tab->broker_ || Tab->recovered_)) { Tab->error_ = "document-original-save-unavailable"; update(); return; }
        auto Started = Tab->session_->Save({Original ? SaveDisposition::Original : SaveDisposition::SaveAs, Destination.toUtf8().toStdString()});
        if (!Started.HasValue()) { Tab->error_ = QString::fromStdString(Started.Failure().code_); update(); return; }
        const auto Ticket = Started.Value();const auto Encoded=Tab->pane_->Export(Destination);
        if(!Encoded.HasValue()){Tab->session_->CompleteSave(Ticket,{false,{},Encoded.Failure()});Tab->error_=QString::fromStdString(Encoded.Failure().code_);update();return;}
        const auto Bytes=Encoded.Value();
        const auto Expected = Original ? storage::DestinationExpectation{true, Tab->input_.source_.id_.revision_} : storage::DestinationExpectation{};
        const auto Hash = Original ? Tab->input_.hash_ : QByteArray{};
        externalWatcher_->beginSelfWrite(Destination);
        Tab->saving_ = true; Tab->error_.clear(); Tab->cancellation_ = std::make_shared<std::stop_source>();
        auto* Watcher = new QFutureWatcher<storage::Result<LocalSaveResult>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Destination, Ticket, Bytes] {
            auto Result = Watcher->result(); Watcher->deleteLater(); Tab->saving_ = false;
            externalWatcher_->endSelfWrite(Destination, Result.HasValue() && Result.Value().receipt_.destinationCommitted_);
            if (Tab->closed_) return;
            DocumentSaveResult Completion;
            if (!Result.HasValue()) Completion.error_ = Result.Failure();
            else {
                Completion.committed_ = Result.Value().receipt_.destinationCommitted_; Completion.cleanupErrors_ = Result.Value().receipt_.cleanupErrors_;
                Completion.source_ = SourceIdentity{Result.Value().source_, {}, Destination.toUtf8().toStdString(), {}, Result.Value().source_.id_.revision_.has_value() && !(Result.Value().source_.attributes_ & 1u)};
            }
            if (!Tab->session_->CompleteSave(Ticket, std::move(Completion))) return;
            if (!Result.HasValue()) { Tab->error_ = QString::fromStdString(Result.Failure().code_); Tab->closeAfterSave_ = false; closingWorkspace_ = false; }
            else {
                if(!Tab->Dirty())Tab->pane_->Commit(Destination, Bytes);
                Tab->path_ = Destination; Tab->input_.source_ = Result.Value().source_; Tab->input_.hash_ = Result.Value().hash_;
                Tab->originalAuthority_ = true;
                Tab->originalSave_ = Result.Value().source_.id_.revision_.has_value() && !(Result.Value().source_.attributes_ & 1u); Tab->recovered_ = false; Tab->recoveredName_.clear();
                Tab->checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
                Tab->broker_ = false; Tab->sourceToken_.clear();
                if (!Result.Value().receipt_.cleanupErrors_.empty()) Tab->error_ = "document-saved-cleanup-warning";
                cleanupRecovery(Tab, Ticket.revision_);
                if (Tab->closeAfterSave_ && !Tab->Dirty()) retire(Tab, true);
                else Tab->closeAfterSave_ = false;
            }
            update(); tryFinishClose();
        });
        auto Cancellation = Tab->cancellation_;
        const bool Image = Tab->pane_->ViewId() == "image";
        Watcher->setFuture(QtConcurrent::run([Destination, Bytes, Expected, Hash, Cancellation, Original, Overwrite, Image] {
            storage::OperationContext Context; Context.stopToken_ = Cancellation->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            auto Expectation = Expected; auto ContentHash = Hash;
            if (Overwrite && QFileInfo::exists(Destination)) {
                auto Existing = ReadDocumentFile(Destination, Context); if (!Existing.HasValue()) return storage::Result<LocalSaveResult>{Existing.Failure()};
                Expectation = {true, Existing.Value().source_.id_.revision_}; ContentHash = Existing.Value().hash_;
            }
            return SaveTextFile(Destination, Bytes, Expectation, ContentHash, Context, Image);
        })); update();
    }
    void DocumentWorkspaceWidget::cleanupRecovery(const std::shared_ptr<Tab>& Tab, std::uint64_t ThroughRevision)
    {
        if (Tab->checkpointing_) return; // Completion of that checkpoint performs the cleanup.
        auto Recovery = recovery_; auto* Watcher = new QFutureWatcher<storage::Result<storage::Unit>>(this); ++recoveryWorkers_;
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Tab] {
            if (!Watcher->result().HasValue()) Tab->error_ = "document-recovery-cleanup";
            Watcher->deleteLater(); --recoveryWorkers_; update(); tryFinishClose();
        });
        const auto Id = Tab->id_.toStdString(); Watcher->setFuture(QtConcurrent::run([Recovery, Id, ThroughRevision] { return Recovery->Remove(Id, ThroughRevision); }));
    }
    void DocumentWorkspaceWidget::Checkpoint()
    {
        if (!pendingHostOpen_.isEmpty()) for (const auto& Tab : documents_) if (Tab->hostSlot_) {
            if (Tab->loading_ || Tab->saving_ || !Tab->pendingToken_.isEmpty() || Tab->checkpointing_ || Tab->pane_->IsBusy()) break;
            if (!Tab->Dirty() || Tab->checkpointedRevision_ == Tab->Revision()) {
                const auto Next = std::exchange(pendingHostOpen_, {});
                retire(Tab, true, true); open(Next["path"].toString(), true, Next["sourceToken"].toString(), Next["broker"].toBool()); return;
            }
        }
        // A host-slot replacement must preserve dirty content even when periodic recovery is disabled.
        if (pendingHostOpen_.isEmpty() && !appliedPreferences_["documentViews"].toObject()["textView"].toObject()["recoveryEnabled"].toBool(true)) return;
        for (const auto& Tab : documents_) {
            if (Tab->loading_ || Tab->closed_ || Tab->checkpointing_ || !Tab->Dirty() || Tab->checkpointedRevision_ == Tab->Revision()) continue;
            const auto Bytes = Tab->pane_->ViewId() == "image" ? Tab->pane_->Content() : QByteArray::fromStdString(Tab->session_->Text());
            if (Bytes.size() > DocumentByteLimit) { Tab->error_ = "document-recovery-budget"; continue; }
            RecoverySnapshot Record; Record.document_ = Tab->session_->Snapshot(); Record.bytes_ = Bytes.toStdString();Record.viewId_=Tab->pane_->ViewId().toStdString();
            const auto Revision = Tab->Revision(); Tab->checkpointing_ = true;
            auto* Watcher = new QFutureWatcher<storage::Result<RecoverySnapshot>>(this);
            connect(Watcher, &QFutureWatcherBase::finished, this, [this, Tab, Watcher, Revision, SavedRevision = Record.document_.savedRevision_, SourcePath = Record.document_.source_.localPath_] {
                const bool Stored = Watcher->result().HasValue(); Watcher->deleteLater(); Tab->checkpointing_ = false;
                if (!Stored) Tab->error_ = "document-recovery-write-failed";
                else Tab->checkpointedRevision_ = SavedRevision == Tab->session_->Snapshot().savedRevision_ && SourcePath == Tab->session_->Snapshot().source_.localPath_
                    ? Revision : std::numeric_limits<std::uint64_t>::max();
                if (Tab->hostSlot_ && !pendingHostOpen_.isEmpty()) {
                    if (Stored && !Tab->closed_) Checkpoint();
                    else { pendingHostOpen_ = {}; if (Tab->editor_) Tab->editor_->setReadOnly(false); }
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
            const auto State = Tab->session_->Snapshot(); const auto Capability = Tab->session_->Capabilities();
            const auto Code = !Tab->error_.isEmpty() ? Tab->error_ : State.error_ ? QString::fromStdString(State.error_->code_) : !State.cleanupErrors_.empty() ? QString("document-saved-cleanup-warning") : QString{};
            QJsonArray Cleanup; for (const auto& Issue : State.cleanupErrors_) Cleanup.append(QString::fromStdString(Issue.code_));
            Tabs.append(QJsonObject{{"documentId", Tab->id_}, {"path", Tab->path_}, {"dirty", State.dirty_}, {"loading", Tab->loading_},
                {"saving", Tab->saving_ || !Tab->pendingToken_.isEmpty()}, {"revision", QString::number(State.revision_)},
                {"savedRevision", QString::number(State.savedRevision_)}, {"generation", QString::number(State.generation_)},
                {"lifecycle", Life(State.lifecycle_)}, {"saveState", SaveState(State.saveState_)}, {"error", Code},
                {"errorCategory", State.error_ ? int(State.error_->category_) : -1}, {"cleanupWarnings", Cleanup},
                {"sourceToken", Tab->sourceToken_}, {"canSave", Capability.save_}, {"canSaveAs", Capability.saveAs_}, {"hostSlot", Tab->hostSlot_}, {"view",Tab->pane_->ViewState()}});
            if (!Tab->pendingToken_.isEmpty()) Pending.append(QJsonObject{{"documentId", Tab->id_}, {"sourceToken", Tab->sourceToken_},
                {"saveToken", Tab->pendingToken_}, {"generation", QString::number(Tab->pending_->generation_)},
                {"revision", QString::number(Tab->pending_->revision_)}, {"byteLength", QString::number(Tab->pendingBytes_.size())},
                {"sha256", QString::fromLatin1(Tab->pendingHash_)}});
        }
        return {{"protocolVersion", int(HostVersion)}, {"workspaceId", workspaceId_}, {"documents", Tabs}, {"pendingSaves", Pending},
            {"clipboard", clipboardRequest_}, {"recoveryList", recoveryList_}, {"activeDocumentId", active() ? active()->id_ : QString{}}, {"idle", IsIdle()},
            {"memoryPrivateBytes", QString::number(Memory.PrivateUsage)}, {"memoryPeakWorkingSetBytes", QString::number(Memory.PeakWorkingSetSize)}, {"memoryPeakPrivateBytes", QString::number(Memory.PeakPagefileUsage)},
            {"recoveryRoot", recoveryRoot_}, {"dpr", devicePixelRatioF()}, {"tabbed", tabbed_}, {"chrome", chrome_->Settings()},
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
            negotiated_ = true; return Reply({{"capabilities", QJsonArray{"mixedTabs", "saveStream", "clipboardBroker", "recovery"}},
                {"maxDocumentBytes", int(DocumentByteLimit)}, {"maxChunkBytes", int(HostChunkLimit)}, {"maxTabs", 16}}, "Ready");
        }
        if (Type == "Heartbeat") return Reply({{"alive", true}, {"idle", IsIdle()}});
        if (Type == "OpenDocuments") {
            const auto Inputs = Payload["documents"].toArray();
            if (Inputs.isEmpty() || Inputs.size() > 16) return Reply(Error("document-open-count"));
            // Commander recycles a closed viewer as its next warm viewer.
            viewerCloseRequested_ = false;
            QJsonArray Results; for (const auto& Input : Inputs) {
                const auto Item = Input.toObject();
                bool Bound = false;
                if(Item["bindExistingHost"].toBool() && Item["hostSlot"].toBool() && Item["sourceToken"].toString().size()==32) {
                    for(const auto& Tab:documents_) if(Tab->hostSlot_ && Tab->sourceToken_.isEmpty() && !Tab->loading_ && !Tab->saving_
                        && !Tab->Dirty() && !Tab->pane_->IsBusy() && Tab->session_->Snapshot().lifecycle_==DocumentLifecycle::Ready
                        && QFileInfo(Tab->path_).absoluteFilePath().compare(QFileInfo(Item["path"].toString()).absoluteFilePath(),Qt::CaseInsensitive)==0
                        && QString::fromLatin1(Tab->input_.hash_.toHex())==Item["sha256"].toString()) {
                        // The SDK already decoded this exact snapshot. Transfer its
                        // original-provider binding without decoding/creating a second view.
                        const auto Begun=Tab->session_->BeginOpen(true);
                        if(!Begun.HasValue())break;
                        Tab->sourceToken_=Item["sourceToken"].toString();Tab->broker_=Item["broker"].toBool();
                        SourceIdentity Source{Tab->input_.source_,{},Tab->path_.toUtf8().toStdString(),Tab->sourceToken_.toStdString(),Tab->broker_};
                        Tab->session_->CompleteOpen(Begun.Value(),OpenedDocument{std::move(Source),Tab->pane_->SessionContent().toStdString()});
                        Results.append(QJsonObject{{"documentId",Tab->id_},{"boundExistingHost",true}});
                        update();Bound=true;break;
                    }
                }
                if(!Bound)Results.append(open(Item["path"].toString(),Item["hostSlot"].toBool(),Item["sourceToken"].toString(),Item["broker"].toBool()));
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
        if (Action == "state") return Snapshot();
        if (Action == "escape") { escapeViewer(); return {{"accepted", hosted_ && closeViewerOnEscape_}}; }
        if (Action == "preferences") { findChild<QAction*>("documentPreferences")->trigger(); return {{"accepted", true}}; }
        if (Action == "new") return newDocument();
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
        if (Action == "open" || Action == "openHost") return open(Request["path"].toString(), Action == "openHost", Request["sourceToken"].toString(), Request["broker"].toBool());
        if (Action == "chrome") { if (!ApplyChromeSettings(Request["settings"].toObject())) return Error("document-chrome-settings"); emit ChromeSettingsChanged(chrome_->Settings()); return {{"accepted", true}}; }
        if (Action == "cancelHost") { for (const auto& Tab : documents_) if (Tab->hostSlot_) { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->pane_->Cancel(); } return {{"accepted", true}}; }
        if (Action == "checkpoint") { Checkpoint(); return {{"accepted", true}}; }
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
        if (Action == "cancel") { Tab->session_->Cancel(); Tab->cancellation_->request_stop();Tab->pane_->Cancel(); return {{"accepted", true}}; }
        if (Action == "reload") { if (Tab->Dirty() && !Request["discard"].toBool()) return Error("document-reload-dirty"); load(Tab, Request["discard"].toBool()); return {{"accepted", true}}; }
        if (Action == "close") {
            if (Tab->saving_ || !Tab->pendingToken_.isEmpty() || Tab->checkpointing_) return Error("document-close-busy");
            if (Tab->Dirty() && !Request["discard"].toBool()) return Error("document-close-dirty");
            retire(Tab, Request["discard"].toBool()); update(); return {{"accepted", true}};
        }
        if (Tab->loading_ || !Tab->editor_) return Error("document-not-ready");
        if (Action == "activate") { activate(Tab); Tab->pane_->Focus(); }
        else if (Action == "replaceText") {
            if (!Request["text"].toString().isValidUtf16()) return Error("p0-utf8-text-required");
            if(Tab->pane_->ViewId()!="text")return Error("document-text-command-on-binary");
            if (!Request["text"].isString() || EncodeText(Request["text"].toString(), Tab->input_.bom_, Tab->input_.eol_).size() > TextByteLimit || Request["text"].toString().contains(QChar::Null)) return Error("p0-text-byte-limit");
            if (Tab->editor_->isReadOnly()) return Error("document-edit-unavailable");
            auto Result=Tab->pane_->Execute(Request);if(Result.contains("error"))return Result;
        }
        else if (Action == "clipboard") return clipboard(Tab, Request["mode"].toString());
        else if (Action == "selectAll") Tab->pane_->Execute(Request);
        else if (Action == "undo") Tab->pane_->Undo();
        else if (Action == "redo") Tab->pane_->Redo();
        else if (Action == "save") { if (Tab->broker_) return prepareSave(Tab); save(Tab, Tab->path_, Request["overwrite"].toBool()); }
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
                Tab->checkpointedRevision_ = std::numeric_limits<std::uint64_t>::max();
                cleanupRecovery(Tab, Revision);
                if (Tab->closeAfterSave_ && !Tab->Dirty()) retire(Tab, true); else Tab->closeAfterSave_ = false;
            }
            else { Tab->closeAfterSave_ = false; closingWorkspace_ = false; }
        }
        else {auto Result=Tab->pane_->Execute(Request);if(Result.contains("error"))return Result;}
        update(); tryFinishClose(); return {{"accepted", true}};
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
            tabs_->setTabText(Index, (Tab->recoveredName_.isEmpty() ? QFileInfo(Tab->path_).fileName() : Tab->recoveredName_) + (Tab->Dirty() ? " *" : "") + (Tab->loading_ ? tr(" · 여는 중") : "")
                + (Tab->session_->Snapshot().error_ || !Tab->error_.isEmpty() ? tr(" · 오류") : ""));
        }
        auto Tab = active();
        if (Tab) {
            workspace_.Activate(Tab->id_.toStdString()); chrome_->SetActive(Tab->pane_->Contribution());
            const auto State = Tab->session_->Snapshot();
            const auto Code = !Tab->error_.isEmpty() ? Tab->error_ : State.error_ ? QString::fromStdString(State.error_->code_) : QString{};
            status_->setToolTip(Code); status_->setText(!Code.isEmpty() ? Message(Code) : Tab->loading_ ? tr("여는 중…") : Tab->saving_ || !Tab->pendingToken_.isEmpty() ? tr("저장 중…") :
                Tab->pane_->IsBusy()?tr("표시 또는 편집 작업 처리 중…") : Tab->pane_->ViewId().toUpper()+(Tab->broker_?tr(" · 원본 공급자에 저장"):Tab->originalSave_?tr(" · 로컬 파일"):tr(" · 사본 · 다른 이름 저장")));
        }
        else { chrome_->SetActive({}); status_->setText(shellError_.isEmpty() ? tr("PDF·텍스트·이미지를 열어 편집할 수 있습니다.") : shellError_); }
        const auto Capability = Tab ? Tab->session_->Capabilities() : DocumentCapabilities{};
        for (const auto& Item : {std::pair{"saveDocument", Capability.save_ || Capability.saveAs_},
            std::pair{"saveDocumentAs", Capability.saveAs_}, std::pair{"undoDocument", Capability.undo_}, std::pair{"redoDocument", Capability.redo_},
            std::pair{"closeDocument", Capability.close_ && Tab && !Tab->checkpointing_},
            std::pair{"resolveDocumentConflict", Tab && Capability.view_ && Tab->session_->Snapshot().saveState_ == DocumentSaveState::Conflict}, std::pair{"cancelDocument", Capability.cancel_ || (Tab && Tab->pane_->IsBusy())},
            std::pair{"reloadDocument", Tab && !Tab->saving_ && Tab->pendingToken_.isEmpty()}}) {
            if (auto* Action = findChild<QAction*>(Item.first)) Action->setEnabled(Item.second);
        }
        for (auto* Action : findChildren<QAction*>()) {
            const auto Id = Action->property("documentCommandId").toString();
            if (Id.startsWith("text.") || QStringList{"edit.cut", "edit.copy", "edit.paste", "edit.selectAll"}.contains(Id))
                Action->setEnabled(Tab && !Tab->loading_ && Tab->pane_->ViewId() == "text"
                    && ((Id != "edit.cut" && Id != "edit.paste" && Id != "text.replace") || !Tab->editor_->isReadOnly()));
            if (Id == "file.new") Action->setEnabled(documents_.size() < 16 && (tabbed_ || documents_.empty()));
            if (Id == "tab.next" || Id == "tab.previous") Action->setEnabled(tabbed_ && tabs_->count() > 1);
        }
        if (Tab && Tab->pane_->ViewId() == "text" && Tab->input_.bytes_.size() >=
            qBound(1, appliedPreferences_["documentViews"].toObject()["textView"].toObject()["largeFileMB"].toInt(1), 16) * 1024 * 1024)
            status_->setText(tr("대용량 문서 · 편집·검색에 시간이 걸릴 수 있습니다. ") + status_->text());
        if (Tab && !shellError_.isEmpty()) status_->setText(shellError_ + " · " + status_->text());
        emit StateChanged();
    }
    void DocumentWorkspaceWidget::retire(std::shared_ptr<Tab> Tab, bool Discard, bool PreserveRecovery)
    {
        if (!workspace_.Remove(Tab->id_.toStdString(), Discard ? CloseDisposition::Discard : CloseDisposition::KeepChanges).HasValue()) return;
        Tab->cancellation_->request_stop(); Tab->closed_ = true;
        if (tabbed_) tabs_->removeTab(tabs_->indexOf(Tab->pane_)); else singlePane_->removeWidget(Tab->pane_);
        Tab->pane_->deleteLater(); std::erase(documents_, Tab);
        if(!Tab->temporarySource_.isEmpty())QFile::remove(Tab->temporarySource_);
        if (!PreserveRecovery) cleanupRecovery(Tab, Tab->Revision());
    }
    void DocumentWorkspaceWidget::closeTab(int Index)
    {
        if (Index < 0 || Index >= int(documents_.size())) return;
        auto Target = documents_[Index];
        if (Target->saving_ || !Target->pendingToken_.isEmpty() || Target->checkpointing_ || Target->pane_->IsBusy()) return;
        if (Target->Dirty()) {
            const int Choice = closePrompt_ ? closePrompt_(Target->id_) : int(QMessageBox::question(this, tr("변경 내용 저장"),
                tr("변경 내용을 저장하시겠습니까?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel));
            if (Choice != QMessageBox::Save && Choice != QMessageBox::Discard) { closingWorkspace_ = false; return; }
            if (Choice == QMessageBox::Save) {
                activate(Target); Target->closeAfterSave_ = true; findChild<QAction*>("saveDocument")->trigger(); return;
            }
        }
        retire(Target, true); update();
    }
    QJsonObject DocumentWorkspaceWidget::clipboard(const std::shared_ptr<Tab>& Tab, const QString& Mode)
    {
        if (!clipboardBroker_ || !clipboardRequest_.isEmpty() || !Tab || (Mode != "copy" && Tab->editor_->isReadOnly())
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
        if (hosted_ && closeViewerOnEscape_ && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            auto* Widget = qobject_cast<QWidget*>(Watched);
            if (Key->key() == Qt::Key_Escape && Key->modifiers() == Qt::NoModifier && !Key->isAutoRepeat()
                && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()
                && !(preferences_ && preferences_->isVisible())
                && Widget && (Widget == this || isAncestorOf(Widget))) {
                escapeViewer(); return true;
            }
        }
        if (Watched == qApp && Event->type() == QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent*>(Event)->propertyName() == "documentSettingsError")
            ShowShellError(qApp->property("documentSettingsError").toString());
        if (clipboardBroker_ && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event); QString Mode;
            if (Key->matches(QKeySequence::Copy)) Mode = "copy"; if (Key->matches(QKeySequence::Cut)) Mode = "cut"; if (Key->matches(QKeySequence::Paste)) Mode = "paste";
            if (!Mode.isEmpty()) { for (const auto& Tab : documents_) if (Tab->editor_ == Watched || Tab->pane_->InputWidget()==Watched || (qobject_cast<QWidget*>(Watched)&&Tab->pane_->InputWidget()&&Tab->pane_->InputWidget()->isAncestorOf(qobject_cast<QWidget*>(Watched)))) {clipboard(Tab,Mode);return true;} }
        }
        return QWidget::eventFilter(Watched, Event);
    }
    bool DocumentWorkspaceWidget::RequestClose()
    {
        closingWorkspace_ = true;
        if (hosted_) for (const auto& Tab : documents_) Tab->pane_->Cancel();
        if (!IsIdle()) return false;
        while (!documents_.empty()) {
            const auto Count = documents_.size(); closeTab(int(Count - 1));
            if (documents_.size() == Count) return false;
        }
        return IsIdle();
    }
    void DocumentWorkspaceWidget::tryFinishClose()
    {
        if (closingWorkspace_) QTimer::singleShot(0, this, [this] { if (closingWorkspace_ && RequestClose()) finishWindowClose(); });
    }
    void DocumentWorkspaceWidget::escapeViewer()
    {
        if (!hosted_ || QApplication::activeModalWidget() || QApplication::activePopupWidget()
            || (preferences_ && preferences_->isVisible())) return;
        refreshPreferences();
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
