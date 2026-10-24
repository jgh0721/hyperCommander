#include "stdafx.h"
#include "compareWindow.hpp"
#include "saveRecoveryDialog.hpp"
#include "runtime/saveRecovery.hpp"
#include "runtime/gitTool.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QPushButton>
#include "mergeController.hpp"
#include "chunkGutter.hpp"
#include "scrollSynchronizer.hpp"
#include "multiPaneAlignment.hpp"
#include "findBar.hpp"
#include "overviewBar.hpp"
#include "directoryView.hpp"
#include "directoryMergeDialog.hpp"
#include "semanticView.hpp"
#include "extensionView.hpp"
#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QLabel>
#include <QJsonDocument>
#include <QDateTime>
#include <QCryptographicHash>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QToolBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtConcurrentRun>
#include <fmstyle/ThemeManager.h>
#include <ScintillaEditBase.h>
#include <Scintilla.h>
#include <ILexer.h>
#include <Lexilla.h>

namespace hc::compare
{
    namespace
    {
        QByteArray contents(ScintillaEditBase* Editor)
        {
            const auto length = Editor->send(SCI_GETLENGTH);
            QByteArray result(qsizetype(length) + 1, '\0');
            Editor->send(SCI_GETTEXT, uptr_t(result.size()), reinterpret_cast<sptr_t>(result.data()));
            result.resize(qsizetype(length)); return result;
        }
        const char* lexer(const QString& Path)
        {
            const auto ext = QFileInfo(Path).suffix().toLower();
            if (QStringList{"c", "cpp", "cc", "cxx", "h", "hpp", "cs", "java", "js", "ts"}.contains(ext)) return "cpp";
            if (ext == "py") return "python";
            if (ext == "json") return "json";
            if (ext == "xml" || ext == "html") return "xml";
            return "null";
        }
        sptr_t color(const QColor& Color) { return Color.red() | (Color.green() << 8) | (Color.blue() << 16); }
    }
    CompareWindow::CompareWindow(Request Input) : request_(std::move(Input))
    {
        setWindowTitle("HyperCompare"); resize(1280, 800);
        QSettings settings; dark_ = request_.theme_.isEmpty() ? settings.value("theme/dark", false).toBool() : request_.theme_ == "dark";
        uiFont_ = qApp->font(); uiFont_.fromString(settings.value("fonts/ui", uiFont_.toString()).toString());
        listFont_ = uiFont_; listFont_.fromString(settings.value("fonts/list", listFont_.toString()).toString());
        textFont_ = QFont("Cascadia Mono", 11); textFont_.fromString(settings.value("fonts/text", textFont_.toString()).toString());
        auto* layout = new QSplitter(Qt::Vertical, this);
        auto* sources = new QWidget(layout); sources_ = sources; auto* row = new QHBoxLayout(sources); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
        panes_ = new QSplitter(Qt::Horizontal, sources); overview_ = new OverviewBar(sources);
        row->addWidget(panes_, 1); row->addWidget(overview_);
        details_ = new QTabWidget(layout); details_->setObjectName("comparisonDetails");
        changes_ = new QTreeWidget(details_); semantic_ = new SemanticView(details_);
        details_->addTab(changes_, tr("줄 차이")); details_->addTab(semantic_, tr("함수 변경")); details_->setTabVisible(1, false);
        layout->addWidget(sources); layout->addWidget(details_);
        connect(details_, &QTabWidget::currentChanged, this, [layout](int Index) { if (Index == 1) layout->setSizes({500, 240}); });
        semantic_->pairChanged_ = [this](int Index) { if (!busy() && result_.operation_ == "compare3") pairSelector_->setCurrentIndex(Index); };
        semantic_->reveal_ = [this](int A, const QJsonObject& First, int B, const QJsonObject& Second) {
            if (busy()) return;
            if (scroll_) scroll_->SetEnabled(false);
            if (multi_) multi_->SetEnabled(false);
            for (const auto& entry : {std::pair{A, First}, std::pair{B, Second}})
            {
                if (entry.second.isEmpty() || entry.first < 0 || entry.first >= result_.inputs_.size() || entry.first >= editors_.size()) continue;
                auto* editor = editors_[entry.first]; editor->send(SCI_SETSEL, uptr_t(entry.second["begin"].toInt()), entry.second["end"].toInt());
                editor->send(SCI_SCROLLRANGE, uptr_t(entry.second["begin"].toInt()), entry.second["end"].toInt());
            }
            if (scroll_) scroll_->SetEnabled(syncScroll_->isChecked() && !multi_);
            if (multi_) multi_->SetEnabled(syncScroll_->isChecked());
        };
        directory_ = new DirectoryView(layout); layout->addWidget(directory_); directory_->hide();
        directory_->activityChanged_ = [this] { updateMerge(); cancel_->setEnabled(busy()); if (closing_ && !busy()) close(); };
        directory_->open_ = [this](Request Input) {
            if (busy() && !directoryStreaming_) return; Input.theme_ = dark_ ? "dark" : "light";
            auto* window = new CompareWindow(std::move(Input)); window->setAttribute(Qt::WA_DeleteOnClose); window->show();
        };
        directory_->levelChanged_ = [this](const QString& Level) {
            if (busy()) return; request_.directory_.level_ = Level; QSettings{}.setValue("directory/level", Level); start();
        };
        panes_->setChildrenCollapsible(false); layout->setChildrenCollapsible(false);
        changes_->setHeaderLabels({tr("차이 / 경로"), tr("왼쪽"), tr("오른쪽")});
        changes_->setObjectName("compareChanges");
        changes_->setRootIsDecorated(false); changes_->setUniformRowHeights(true);
        layout->setStretchFactor(0, 4); layout->setStretchFactor(1, 1); layout->setSizes({560, 140}); setCentralWidget(layout);
        auto* tools = addToolBar(tr("비교"));
        openFiles_ = tools->addAction(tr("파일 열기"), this, &CompareWindow::openInputs);
        openFolders_ = tools->addAction(tr("디렉터리 열기"), this, &CompareWindow::openDirectories);
        tools->addAction(tr("다시 비교"), this, [this] { start(true); });
        cancel_ = tools->addAction(tr("취소"), this, [this] { CancelRemoteRequest(); state_->setText(tr("취소 요청 중")); });
        cancel_->setEnabled(false);
        save_ = tools->addAction(tr("결과 저장"), this, &CompareWindow::save); save_->setEnabled(false);
        pairSelector_ = new QComboBox(tools); pairSelector_->setObjectName("comparePairSelector");
        pairSelector_->setAccessibleName(tr("비교 쌍"));
        pairSelector_->addItems({tr("FILE 1 ↔ FILE 2"), tr("FILE 1 ↔ FILE 3"), tr("FILE 2 ↔ FILE 3")});
        pairSelector_->setToolTip(tr("선택한 두 원본에 차이 강조·정렬·개요·탐색을 적용합니다. 열린 입력 내용으로 전환하며 파일을 다시 읽으려면 ‘다시 비교’를 사용합니다."));
        pairSelectorAction_ = tools->addWidget(pairSelector_); pairSelectorAction_->setVisible(false);
        connect(pairSelector_, &QComboBox::currentIndexChanged, this, [this](int Index) {
            if (busy() || result_.operation_ != "compare3" || Index < 0 || Index > 2 || !result_.pairViews_[std::size_t(Index)]) return;
            pairIndex_ = Index; applyPairView(); rebuildGutters(); rebuildMultiAlignment(); applyAppearance(); updateMerge();
            state_->setText(tr("%1 — 차이 %2개 · 전체 3파일: %3").arg(pairSelector_->currentText()).arg(currentPair()->changes_.size()).arg(result_.outcome_));
        });
        wholeActions_.push_back(tools->addAction(tr("왼쪽 전체 선택"), this, [this] { chooseSide(0); }));
        wholeActions_.push_back(tools->addAction(tr("오른쪽 전체 선택"), this, [this] { chooseSide(1); }));
        wholeActions_.push_back(tools->addAction(tr("전체 수동 완료"), this, [this] {
            if (!merge_ || busy()) return;
            try { merge_->ChooseAll(Resolution::Manual); }
            catch (const Failure& error) { state_->setText(QString::fromStdString(error.code_)); }
        }));
        for (auto* action : wholeActions_) action->setEnabled(false);
        addToolBarBreak(); auto* mergeTools = addToolBar(tr("블록 병합"));
        auto* previous = mergeTools->addAction(tr("이전 차이"), this, [this] { navigateBlock(-1); });
        auto* next = mergeTools->addAction(tr("다음 차이"), this, [this] { navigateBlock(1); });
        previous->setShortcut(QKeySequence("Shift+F7")); next->setShortcut(QKeySequence("F7"));
        for (const auto& choice : {std::pair{tr("블록 LEFT"), Resolution::Left}, {tr("블록 RIGHT"), Resolution::Right},
            {tr("블록 BASE"), Resolution::Base}, {tr("LEFT → RIGHT"), Resolution::LeftThenRight},
            {tr("RIGHT → LEFT"), Resolution::RightThenLeft}, {tr("블록 제거"), Resolution::Omit}, {tr("블록 수동 완료"), Resolution::Manual}})
        {
            auto* action = mergeTools->addAction(choice.first, this, [this, value = choice.second] { chooseBlock(value); });
            blockActions_.push_back(action); action->setEnabled(false);
            if (choice.second == Resolution::Base) baseChoice_ = action;
        }
        auto* fileMenu = menuBar()->addMenu(tr("파일"));
        exportReport_ = fileMenu->addAction(tr("비교 보고서 내보내기…"), this, [this] {
            if (!exportReport_->isEnabled()) return;
            QFileDialog dialog(this, tr("비교 보고서 내보내기"), {}, tr("JSON 보고서 (*.json)"));
            dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("json");
            if (dialog.exec() == QDialog::Accepted) ExportReport(dialog.selectedFiles().front(), true);
        });
        exportReport_->setObjectName("exportComparisonReport"); exportReport_->setEnabled(false);
        recoveryAction_ = fileMenu->addAction(tr("저장 복구…"), this, [this] {
            if (busy()) return;
            if (recoveryDialog_) { recoveryDialog_->show(); recoveryDialog_->raise(); recoveryDialog_->activateWindow(); return; }
            auto* dialog = new SaveRecoveryDialog(request_, this); recoveryDialog_ = dialog; dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->activityChanged_ = [this] { QTimer::singleShot(0, this, [this] { updateMerge(); if (closing_ && !busy()) close(); }); };
            updateMerge(); dialog->show();
        });
        recoveryAction_->setObjectName("openSaveRecovery");
        auto* gitSetup = fileMenu->addAction(tr("Git 연동 설정…"), this, [this] {
            auto* dialog = new QDialog(this); dialog->setObjectName("gitSetupDialog"); dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(tr("Git 연동 설정")); dialog->resize(800, 480);
            auto* layout = new QVBoxLayout(dialog);
            auto* explanation = new QLabel(tr("git config --local --edit로 저장소 설정을 열어 아래 내용을 기존 항목과 병합하십시오.\n"
                "일반 파일 충돌과 add/add를 지원합니다. 삭제·복잡한 이름 변경·파일 모드 충돌은 지원하지 않습니다."), dialog);
            explanation->setWordWrap(true); layout->addWidget(explanation);
            auto* text = new QPlainTextEdit(dialog); text->setObjectName("gitSetupConfig"); text->setReadOnly(true); text->setFont(textFont_);
            text->setLineWrapMode(QPlainTextEdit::NoWrap); text->setAccessibleName(tr("Git 연동 설정 내용"));
            try { text->setPlainText(GitToolConfig(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe")); }
            catch (const std::exception& error) { text->setPlainText(QString::fromUtf8(error.what())); }
            layout->addWidget(text);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); auto* copy = buttons->addButton(tr("설정 복사"), QDialogButtonBox::ActionRole);
            buttons->button(QDialogButtonBox::Close)->setText(tr("닫기"));
            copy->setObjectName("copyGitSetup"); connect(copy, &QPushButton::clicked, dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close); layout->addWidget(buttons); dialog->show();
        });
        gitSetup->setObjectName("openGitSetup");
        saveWork_ = fileMenu->addAction(tr("작업 저장..."), this, [this] {
            if (!merge_ || busy()) return;
            QFileDialog dialog(this, tr("병합 작업 저장"), workPath_, tr("HyperCompare 작업 (*.hcmerge)"));
            dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("hcmerge");
            if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) SaveWork(dialog.selectedFiles().front(), true);
        });
        loadWork_ = fileMenu->addAction(tr("작업 불러오기..."), this, [this] {
            if (!merge_ || busy()) return;
            const auto path = QFileDialog::getOpenFileName(this, tr("병합 작업 불러오기"), workPath_, tr("HyperCompare 작업 (*.hcmerge)"));
            if (!path.isEmpty()) LoadWork(path);
        });
        importDecisions_ = fileMenu->addAction(tr("결정 계획 불러오기…"), this, [this] {
            if (!importDecisions_->isEnabled()) return;
            const auto path = QFileDialog::getOpenFileName(this, tr("결정 계획 불러오기"), {}, tr("JSON 결정 계획 (*.json)"));
            if (!path.isEmpty()) ImportDecisions(path);
        });
        exportDecisions_ = fileMenu->addAction(tr("결정 계획 내보내기…"), this, [this] {
            if (!exportDecisions_->isEnabled()) return;
            QFileDialog dialog(this, tr("결정 계획 내보내기"), {}, tr("JSON 결정 계획 (*.json)"));
            dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("json");
            if (dialog.exec() == QDialog::Accepted) ExportDecisions(dialog.selectedFiles().front(), true);
        });
        importDecisions_->setObjectName("importDecisionPlan"); exportDecisions_->setObjectName("exportDecisionPlan");
        importDecisions_->setEnabled(false); exportDecisions_->setEnabled(false);
        importDecisions_->setToolTip(tr("CLI 결정 계획을 RESULT에 적용합니다. 실행 취소할 수 있습니다."));
        exportDecisions_->setToolTip(tr("현재 블록 선택을 CLI -decisions용 JSON으로 저장합니다."));
        saveWork_->setShortcut(QKeySequence("Ctrl+Shift+S")); loadWork_->setShortcut(QKeySequence("Ctrl+Shift+O"));
        saveWork_->setEnabled(false); loadWork_->setEnabled(false);
        auto* editMenu = menuBar()->addMenu(tr("편집"));
        undo_ = editMenu->addAction(tr("실행 취소"), this, [this] { inputUndo(false); });
        redo_ = editMenu->addAction(tr("다시 실행"), this, [this] { inputUndo(true); });
        undo_->setShortcut(QKeySequence::Undo); redo_->setShortcut(QKeySequence::Redo); undo_->setEnabled(false); redo_->setEnabled(false);
        find_ = new FindBar(this); addToolBar(Qt::BottomToolBarArea, find_);
        editMenu->addSeparator();
        auto* find = editMenu->addAction(tr("찾기"), find_, &FindBar::Open); find->setShortcut(QKeySequence::Find);
        auto* findNext = editMenu->addAction(tr("다음 찾기"), this, [this] { find_->Find(false); }); findNext->setShortcut(QKeySequence("F3"));
        auto* findPrevious = editMenu->addAction(tr("이전 찾기"), this, [this] { find_->Find(true); }); findPrevious->setShortcut(QKeySequence("Shift+F3"));
        auto* findAll = editMenu->addAction(tr("모두 찾기"), find_, &FindBar::FindAll); findAll->setShortcut(QKeySequence("Ctrl+Shift+F"));
        findActions_ = {find, findNext, findPrevious, findAll}; for (auto* action : findActions_) action->setEnabled(false);
        auto* closeFind = new QAction(this); closeFind->setShortcut(QKeySequence(Qt::Key_Escape)); closeFind->setEnabled(false); addAction(closeFind);
        connect(closeFind, &QAction::triggered, find_, &FindBar::Close);
        connect(find_, &QToolBar::visibilityChanged, closeFind, &QAction::setEnabled);
        auto* viewMenu = menuBar()->addMenu(tr("보기"));
        alignAll_ = viewMenu->addAction(tr("전체 패널 표시 줄 정렬")); alignAll_->setCheckable(true);
        alignAll_->setChecked(settings.value("view/alignAllPanes", true).toBool()); alignAll_->setEnabled(false);
        alignAll_->setToolTip(tr("BASE·RESULT를 포함한 모든 패널을 맞춥니다. RESULT 편집 후 다시 계산하며 표시 여백은 복사·저장 내용에 포함되지 않습니다."));
        connect(alignAll_, &QAction::toggled, this, [this](bool Enabled) {
            QSettings{}.setValue("view/alignAllPanes", Enabled);
            if (!busy()) { rebuildMultiAlignment(); updateMerge(); }
        });
        auto* encodingMenu = menuBar()->addMenu(tr("인코딩"));
        for (int role = 0; role < 3; ++role)
        {
            auto* menu = encodingMenu->addMenu(role == 2 ? tr("FILE 3 / BASE") : tr("FILE %1").arg(role + 1));
            auto* group = new QActionGroup(menu);
            for (const auto& codec : {QString("auto"), QString("UTF-8"), QString("UTF-16LE"), QString("UTF-16BE"), QString("CP949")})
            {
                auto* action = menu->addAction(codec == "auto" ? tr("자동 (BOM / UTF-8)") : codec);
                action->setCheckable(true); group->addAction(action); action->setData(role);
                action->setObjectName(QString("encoding%1_%2").arg(role + 1).arg(codec));
                action->setProperty("codec", codec); encodingActions_.push_back(action);
                connect(action, &QAction::triggered, this, [this, role, codec] {
                    if (busy()) return;
                    request_.inputEncodings_[role] = codec; start(true);
                });
            }
        }
        alignRows_ = viewMenu->addAction(tr("왼쪽·오른쪽 표시 줄 정렬")); alignRows_->setCheckable(true);
        alignRows_->setChecked(settings.value("view/alignSourceRows", true).toBool()); alignRows_->setEnabled(false);
        alignRows_->setToolTip(tr("두 원본의 대응 줄을 빈 여백으로 맞춥니다. 여백은 복사하거나 저장하는 텍스트에 포함되지 않습니다."));
        connect(alignRows_, &QAction::toggled, this, [this](bool Enabled) {
            QSettings{}.setValue("view/alignSourceRows", Enabled);
            if (scroll_ && !busy()) { scroll_->SetAligned(Enabled); overview_->SetAligned(Enabled); }
        });
        syncScroll_ = viewMenu->addAction(tr("왼쪽·오른쪽 스크롤 동기화")); syncScroll_->setCheckable(true);
        syncScroll_->setChecked(settings.value("view/synchronizeSources", true).toBool()); syncScroll_->setEnabled(false);
        syncScroll_->setToolTip(tr("첫 번째와 두 번째 원본 패널의 대응 줄과 가로 위치를 함께 이동합니다."));
        connect(syncScroll_, &QAction::toggled, this, [this](bool Enabled) {
            QSettings{}.setValue("view/synchronizeSources", Enabled);
            if (scroll_)
            {
                scroll_->SetEnabled(Enabled && !busy() && !multi_);
                if (Enabled && !multi_) scroll_->Synchronize(editors_[currentPair()->inputs_[1]]->hasFocus() ? 1 : 0);
            }
            if (multi_) multi_->SetEnabled(Enabled && !busy());
        });
        inlineHighlights_ = viewMenu->addAction(tr("줄 내부 차이 강조")); inlineHighlights_->setCheckable(true);
        inlineHighlights_->setChecked(settings.value("view/inlineHighlights", true).toBool());
        inlineHighlights_->setToolTip(tr("선택한 두 원본의 변경 줄을 순서대로 짝지어 동일한 단어·식별자를 먼저 대응시키고, 나머지는 문자 묶음 단위로 차이를 표시합니다. 결합 문자와 이모지 내부는 나누지 않습니다. 긴 줄은 줄 단위 표시를 유지합니다."));
        connect(inlineHighlights_, &QAction::toggled, this, [this](bool Enabled) {
            QSettings{}.setValue("view/inlineHighlights", Enabled); applyInlineHighlights();
        });
        showOverview_ = viewMenu->addAction(tr("차이 개요 막대")); showOverview_->setCheckable(true);
        showOverview_->setChecked(settings.value("view/overview", true).toBool()); showOverview_->setEnabled(false);
        connect(showOverview_, &QAction::toggled, this, [this](bool Enabled) {
            // Hiding the focused bar must not focus the change list: its first
            // current-item event would navigate and replace source selections.
            if (!Enabled && overview_->hasFocus()) overview_->FocusSource();
            QSettings{}.setValue("view/overview", Enabled); overview_->setVisible(Enabled && scroll_);
        });
        auto* dark = viewMenu->addAction(tr("Dark")); dark->setCheckable(true); dark->setChecked(dark_);
        connect(dark, &QAction::toggled, this, [this](bool Value) { dark_ = Value; applyAppearance(); });
        for (const auto& role : {QString("ui"), QString("list"), QString("text")})
            viewMenu->addAction(role.toUpper() + tr(" 글꼴"), this, [this, role] { chooseFont(role); });
        state_ = new QLabel(this); statusBar()->addWidget(state_, 1);
        state_->setObjectName("compareStatus");
        inlineStatus_ = new QLabel(this); inlineStatus_->setObjectName("compareInlineStatus");
        inlineStatus_->setToolTip(tr("일부 변경 줄은 문자 강조 계산 한도를 넘었습니다. 해당 줄의 줄 단위 차이 표시와 비교·병합 결과는 유지됩니다."));
        statusBar()->addPermanentWidget(inlineStatus_); inlineStatus_->hide();
        alignmentStatus_ = new QLabel(this); alignmentStatus_->setObjectName("alignmentStatus");
        statusBar()->addPermanentWidget(alignmentStatus_); alignmentStatus_->hide();
        directoryTimer_.setInterval(30);
        connect(&directoryTimer_, &QTimer::timeout, this, [this] {
            if (!request_.directoryProgress_ || closing_) return;
            if (auto progress = request_.directoryProgress_->Take())
            {
                if (!directoryStreaming_)
                {
                    directory_->BeginComparison(request_); directoryStreaming_ = true;
                    sources_->hide(); details_->hide(); directory_->show(); updateMerge();
                }
                directory_->ApplyProgress(*progress);
                state_->setText(progress->phase_ == "scan" ? tr("폴더 열거 중 — 발견 %1개").arg(progress->processed_) :
                    progress->phase_ == "compare" ? tr("파일 내용 비교 중 — %1 / %2").arg(progress->processed_).arg(progress->total_) : tr("디렉터리 변경 여부 재확인 중…"));
            }
        });
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
            // The producer is finished and the channel holds at most four batches.
            // Keep their final statuses before reusing the populated tree.
            if (directoryStreaming_ && !closing_ && request_.directoryProgress_)
                while (auto progress = request_.directoryProgress_->Take()) directory_->ApplyProgress(*progress);
            directoryTimer_.stop(); directoryStreaming_ = false;
            cancel_->setEnabled(false);
            if (refreshing_)
            {
                refreshing_ = false; refresh(watcher_.result());
                if (closing_) close();
                return;
            }
            result_ = watcher_.result();
            if (saving_)
            {
                saving_ = false;
                if (output_) output_->send(SCI_SETREADONLY, 0);
                updateMerge();
                if (result_.exitCode_ == 0)
                {
                    if (merge_) merge_->MarkSaved();
                    dirty_ = false; exitCode_ = 0;
                    state_->setText(result_.warnings_.isEmpty() && result_.recovery_.isEmpty() ? tr("저장 완료") : tr("저장 완료 — 임시 파일 정리 확인 필요"));
                    state_->setToolTip(QString::fromUtf8(QJsonDocument(result_.Json()).toJson()));
                }
                else { exitCode_ = 2; state_->setText(tr("저장 실패: ") + result_.error_); }
                state_->setToolTip(QString::fromUtf8(QJsonDocument(result_.Json()).toJson()));
                if (closing_) close();
                return;
            }
            if (closing_) { close(); return; }
            display();
        });
        connect(&workWatcher_, &QFutureWatcher<WorkTransfer>::finished, this, [this] {
            cancel_->setEnabled(false); if (output_) output_->send(SCI_SETREADONLY, 0);
            auto transfer = workWatcher_.result();
            if (transfer.loading_ && transfer.error_.isEmpty() && canceled_->load()) transfer.error_ = "Canceled";
            if (transfer.error_.isEmpty() && !closing_)
            {
                try
                {
                    if (transfer.loading_)
                        merge_->RestoreWork(transfer.file_.work_.text_, transfer.file_.work_.state_, transfer.file_.work_.reviewRequired_);
                    if (!transfer.decisions_) { workPath_ = transfer.path_; workExpected_ = transfer.file_.expected_; }
                }
                catch (const std::exception& error) { transfer.error_ = QString::fromUtf8(error.what()); }
            }
            updateMerge();
            if (!transfer.error_.isEmpty()) state_->setText(tr("작업 파일 처리 중단: ") + transfer.error_);
            else if (transfer.loading_) state_->setText(transfer.file_.work_.reviewRequired_ ? tr("작업 복원 완료 — 전체 재검토 필요 · 실행 취소로 이전 RESULT 복원 가능") : tr("작업 복원 완료 — 실행 취소로 이전 RESULT 복원 가능"));
            else state_->setText(transfer.file_.warnings_.isEmpty() && transfer.file_.recovery_.isEmpty() ?
                tr("작업 파일 저장 완료 — RESULT 저장 상태 유지") : tr("작업 파일 저장 완료 — 정리·파일 변경 확인 필요"));
            if (transfer.decisions_)
            {
                if (transfer.error_ == "DecisionWorkNotRepresentable" || transfer.error_ == "DecisionStringLimit")
                    state_->setText(tr("결정 계획으로 보존할 수 없는 편집 내용이 있습니다. 작업 저장을 사용하십시오."));
                else if (transfer.error_ == "DecisionWorkReviewRequired") state_->setText(tr("전체 재검토를 완료한 뒤 결정 계획을 내보내십시오. 현재 내용은 작업 저장으로 보존할 수 있습니다."));
                else if (!transfer.error_.isEmpty()) state_->setText(tr("결정 계획 처리 중단: ") + transfer.error_);
                else if (transfer.loading_) state_->setText(tr("결정 계획 적용 완료 — 실행 취소 가능 · RESULT 저장 필요"));
                else state_->setText(transfer.file_.warnings_.isEmpty() && transfer.file_.recovery_.isEmpty() ?
                    tr("결정 계획 내보내기 완료 — RESULT 저장 상태 유지") : tr("결정 계획 저장 완료 — 정리·파일 변경 확인 필요"));
            }
            state_->setToolTip(transfer.path_ + '\n' + QString::fromUtf8(QJsonDocument(QJsonObject{
                {"warnings", transfer.file_.warnings_}, {"recovery", transfer.file_.recovery_}}).toJson()));
            if (closing_) close();
        });
        connect(&reportWatcher_, &QFutureWatcher<ReportTransfer>::finished, this, [this] {
            cancel_->setEnabled(false); const auto transfer = reportWatcher_.result(); updateMerge();
            if (!transfer.error_.isEmpty()) state_->setText(tr("보고서 저장 중단: ") + transfer.error_);
            else state_->setText(transfer.file_.warnings_.isEmpty() && transfer.file_.recovery_.isEmpty() ? tr("비교 보고서 저장 완료") : tr("보고서 저장 완료 — 정리 확인 필요"));
            state_->setToolTip(transfer.path_ + '\n' + QString::fromUtf8(QJsonDocument(QJsonObject{
                {"warnings", transfer.file_.warnings_}, {"recovery", transfer.file_.recovery_}}).toJson()));
            if (closing_) close();
        });
        connect(changes_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Item) {
            if (!Item || editors_.size() < 2 || result_.operation_.startsWith("directory")) return;
            selectBlock(); if (merge_) updateMerge();
        });
        applyAppearance();
        setupInputEditing();
        if (!request_.file1_.isEmpty()) QTimer::singleShot(0, this, [this] { if (!canceled_->load()) start(); });
        else state_->setText(tr("파일 열기 또는 -file1/-file2로 비교를 시작하십시오."));
    }
    CompareWindow::~CompareWindow() { sourceEditing_ = false; bufferTimer_.stop(); if (bufferCanceled_) bufferCanceled_->store(true); bufferWatcher_.waitForFinished(); inputSaveWatcher_.waitForFinished(); canceled_->store(true); watcher_.waitForFinished(); workWatcher_.waitForFinished(); reportWatcher_.waitForFinished(); delete multi_; multi_ = nullptr; }
    void CompareWindow::openDirectories()
    {
        if (busy()) return;
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 디렉터리를 여시겠습니까?")) != QMessageBox::Yes) return;
        const auto left = QFileDialog::getExistingDirectory(this, tr("왼쪽 디렉터리")); if (left.isEmpty()) return;
        const auto right = QFileDialog::getExistingDirectory(this, tr("오른쪽 디렉터리")); if (right.isEmpty()) return;
        Request request; request.mode_ = "directory"; request.timeoutMs_ = 0; request.directory_.level_ = QSettings{}.value("directory/level", "text").toString(); request.file1_ = left; request.file2_ = right; request_ = request; dirty_ = false; start();
    }
    void CompareWindow::openInputs()
    {
        if (busy()) return;
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 파일을 여시겠습니까?")) != QMessageBox::Yes) return;
        const auto left = QFileDialog::getOpenFileName(this, tr("왼쪽 파일")); if (left.isEmpty()) return;
        const auto right = QFileDialog::getOpenFileName(this, tr("오른쪽 파일")); if (right.isEmpty()) return;
        request_.file1_ = left; request_.file2_ = right; request_.base_.clear(); request_.file3_.clear();
        request_.output_.clear(); request_.timeoutMs_ = 60000; request_.allowMissingInputs_ = {}; request_.mode_ = "compare"; start();
    }
    void CompareWindow::start(bool Preserve)
    {
        if (busy() || request_.file1_.isEmpty()) return;
        if (Preserve && sourceEditing_ && dirty_) { invalidateInputComparison(); return; }
        sourceEditing_ = false; bufferTimer_.stop(); ++bufferGeneration_; if (bufferCanceled_) bufferCanceled_->store(true);
        canceled_ = std::make_shared<std::atomic_bool>(false);
        request_.directoryProgress_ = std::make_shared<DirectoryProgressChannel>(); directoryStreaming_ = false; directoryTimer_.start();
        refreshing_ = Preserve && merge_;
        if (!refreshing_) { dirty_ = false; exitCode_ = 3; }
        save_->setEnabled(false); state_->setText(tr("비교 중…"));
        cancel_->setEnabled(true);
        for (auto* editor : editors_) editor->send(SCI_SETREADONLY, 1);
        if (output_) output_->send(SCI_SETREADONLY, 1);
        if (refreshing_)
            watcher_.setFuture(QtConcurrent::run([input = request_, previous = result_, states = merge_->Model().State(),
                bytes = std::size_t(output_->send(SCI_GETLENGTH)), cancel = canceled_] { return Recompare(input, previous, states, bytes, cancel); }));
        else watcher_.setFuture(QtConcurrent::run([input = request_, cancel = canceled_] { return Execute(input, cancel, false); }));
        updateMerge();
    }
    void CompareWindow::refresh(Result Value)
    {
        output_->send(SCI_SETREADONLY, 0);
        if (Value.completion_ != "Complete")
        {
            updateMerge(); state_->setText(tr("재비교 중단 — RESULT 보존: ") + Value.error_); return;
        }
        const bool changed = Value.inputsChanged_; result_ = std::move(Value);
        updateFormats();
        semantic_->SetResult(result_.semantic_, !request_.base_.isEmpty());
        details_->setTabVisible(1, !result_.semantic_["pairs"].toArray().isEmpty());
        if (changed)
        {
            delete multi_; multi_ = nullptr;
            if (scroll_) { scroll_->SetEnabled(false); scroll_->ClearPadding(); }
            for (qsizetype i = 0; i < result_.inputs_.size(); ++i)
            {
                auto* editor = editors_[i]; editor->send(SCI_SETREADONLY, 0);
                editor->send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(result_.inputs_[i].utf8_.constData()));
                editor->send(SCI_EMPTYUNDOBUFFER); editor->send(SCI_SETSAVEPOINT); editor->send(SCI_SETREADONLY, 1);
                editor->send(SCI_MARKERDELETEALL, 8);
            }
            // Rebuild rows before the controller emits changed_ with the new plan.
            applyPairView();
            showBlocks(); merge_->Refresh(result_.plan_); rebuildGutters(); rebuildMultiAlignment(); applyAppearance();
        }
        updateMerge();
        if (!changed) state_->setText(tr("입력 내용 동일 — RESULT·Undo·해결 상태 보존"));
    }
    void CompareWindow::showBlocks()
    {
        const QSignalBlocker blocker(changes_); changes_->clear();
        for (const auto& block : result_.plan_->blocks_)
        {
            auto* item = new QTreeWidgetItem(changes_, {QString::number(block.id_), QString::number(block.leftLine_ + 1), QString::number(block.rightLine_ + 1)});
            item->setData(0, Qt::UserRole, qulonglong(block.id_));
            if (result_.plan_->hasBase_) item->setText(0, QString::number(block.id_) + (block.automatic_ ? tr(" · 자동 구간") : tr(" · 충돌 구간")));
        }
        if (changes_->topLevelItemCount()) changes_->setCurrentItem(changes_->topLevelItem(0));
    }
    ScintillaEditBase* CompareWindow::addPane(const QString& Title, const QString& Path, const QByteArray& Text, bool ReadOnly)
    {
        auto* pane = new QWidget(panes_); auto* box = new QVBoxLayout(pane); box->setContentsMargins(2, 2, 2, 2);
        auto* label = new QLabel(Title + " — " + QFileInfo(Path).fileName(), pane); label->setTextFormat(Qt::PlainText); label->setToolTip(Path);
        label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); box->addWidget(label);
        auto* format = new QLabel(pane); format->setObjectName("format_" + Title);
        format->setTextFormat(Qt::PlainText); box->addWidget(format); formatLabels_.push_back(format);
        auto* editor = new ScintillaEditBase(pane); editor->setObjectName(Title); box->addWidget(editor); editors_.push_back(editor);
        panes_->addWidget(pane); pane->setMinimumWidth(120);
        editor->send(SCI_SETCODEPAGE, SC_CP_UTF8); editor->send(SCI_SETWRAPMODE, SC_WRAP_NONE);
        editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER); editor->send(SCI_SETMARGINWIDTHN, 0, 56);
        editor->send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(Text.constData())); editor->send(SCI_EMPTYUNDOBUFFER);
        editor->send(SCI_SETSAVEPOINT); editor->send(SCI_SETREADONLY, ReadOnly);
        editor->send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer(lexer(Path))));
        static constexpr char keywords[] = "alignas auto bool break case catch char class const constexpr continue default delete do double else enum explicit extern false float for friend if inline int long namespace new noexcept nullptr operator private protected public return short signed sizeof static struct switch template this throw true try typedef typename union unsigned using virtual void volatile while";
        editor->send(SCI_SETKEYWORDS, 0, reinterpret_cast<sptr_t>(keywords));
        editor->send(SCI_MARKERDEFINE, 8, SC_MARK_BACKGROUND);
        return editor;
    }
    void CompareWindow::display()
    {
        sourceEditing_ = false; sourcePending_ = false; bufferTimer_.stop(); ++bufferGeneration_;
        if (bufferCanceled_) bufferCanceled_->store(true);
        save_->setText(tr("결과 저장"));
        delete directoryMerge_; directoryMerge_.clear();
        delete extensionView_; extensionView_ = nullptr;
        if (result_.operation_ != "compare3") pairIndex_ = 0;
        { const QSignalBlocker blocker(pairSelector_); pairSelector_->setCurrentIndex(pairIndex_); }
        const bool directoryResult = result_.operation_.startsWith("directory");
        if (!directoryResult) { directory_->hide(); sources_->show(); details_->show(); }
        semantic_->SetResult(result_.semantic_, !request_.base_.isEmpty());
        details_->setTabVisible(1, !result_.semantic_["pairs"].toArray().isEmpty());
        if (!directoryResult) directory_->SetComparison({}, {});
        workPath_.clear(); workExpected_.reset();
        find_->SetEditors({});
        overview_->SetSession(nullptr, nullptr, {}); overview_->hide();
        for (auto* gutter : gutters_) delete gutter; gutters_.clear();
        delete multi_; multi_ = nullptr; alignmentStatus_->hide();
        delete scroll_; scroll_ = nullptr;
        delete merge_; merge_ = nullptr; output_ = nullptr; editors_.clear(); formatLabels_.clear(); changes_->clear();
        inlineStatus_->hide(); inlineHighlights_->setEnabled(false);
        changes_->setHeaderLabels({tr("차이 / 경로"), tr("왼쪽"), tr("오른쪽"), tr("해결")});
        while (panes_->count()) delete panes_->widget(0);
        exitCode_ = result_.exitCode_; unresolved_ = !result_.merged_.conflicts_.empty() || result_.error_ == "DecisionsRequired";
        if (result_.completion_ != "Complete") { directory_->EndProgress(); updateMerge(); state_->setText(result_.error_); return; }
        if (!result_.extension_.isEmpty())
        {
            try
            {
                extensionView_ = CreateExtensionView(result_.extension_, {request_.file1_, request_.file2_}, centralWidget());
                auto* layout = qobject_cast<QSplitter*>(centralWidget()); layout->addWidget(extensionView_);
                sources_->hide(); details_->hide(); directory_->hide(); extensionView_->show();
                layout->setSizes({0, 0, 0, 700}); state_->setText(tr("비교 — %1").arg(result_.outcome_));
            }
            catch (const std::exception& error) { exitCode_ = 2; state_->setText(QString::fromUtf8(error.what())); }
            applyAppearance(); updateMerge(); return;
        }
        if (result_.operation_.startsWith("directory"))
        {
            directory_->SetComparison(request_, result_); sources_->hide(); details_->hide(); directory_->show();
            if (request_.mode_ == "directory-merge" && result_.directoryComplete_)
            {
                auto* dialog = new DirectoryMergeDialog(request_, result_, this); directoryMerge_ = dialog; exitCode_ = 3;
                dialog->statusChanged_ = [this](int Code) { exitCode_ = Code; if (closing_) QTimer::singleShot(0, this, [this] { close(); }); };
                dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
                dialog->Prepare(request_.output_, request_.mirror_, request_.overwrite_);
            }
        }
        else
        {
            for (int i = 0; i < result_.inputs_.size(); ++i)
            {
                const auto& input = result_.inputs_[i];
                const auto name = i == 2 && !request_.base_.isEmpty() ? "BASE" : request_.mode_ == "merge" ?
                    QString(i == 0 ? "LEFT" : "RIGHT") : QString("FILE %1").arg(i + 1);
                addPane(name, input.path_, input.utf8_, true);
            }
            applyPairView();
            if (request_.mode_ == "merge")
            {
                const auto initial = result_.error_ == "DecisionsRequired" ? result_.inputs_[0].utf8_ : result_.output_;
                output_ = addPane("RESULT", request_.output_, initial, false); save_->setEnabled(true);
                changes_->clear();
                merge_ = new MergeController(output_, result_.plan_, this);
                merge_->changed_ = [this] { updateMerge(); };
                showBlocks();
            }
        }
        state_->setText(QString("%1 — %2, conflicts: %3").arg(result_.operation_, result_.outcome_).arg(result_.merged_.conflicts_.size()));
        if (result_.textComparison_["encodingOnly"].toBool())
            state_->setText(request_.textEquality_ == "bytes" ? tr("내용 동일 · 인코딩/BOM 차이 (바이트 비교)") : tr("내용 동일 — 인코딩/BOM만 다름"));
        updateFormats();
        if (result_.operation_.startsWith("directory"))
        {
            qsizetype differences = 0, conflicts = 0;
            for (const auto& item : result_.entries_)
            { const auto status = item.Status(); differences += !DirectoryContentEqual(status); conflicts += status == "Conflict" || status == "TypeConflict"; }
            state_->setText((result_.directoryComplete_ ? tr("디렉터리 비교 — 차이 %1개 · 충돌 %2개") : tr("디렉터리 일부 비교 — 미확인 항목 포함 %1개 · 충돌 %2개")).arg(differences).arg(conflicts));
        }
        if (request_.mode_ == "merge") exitCode_ = 3;
        bindInputEditors();
        rebuildGutters();
        rebuildMultiAlignment();
        QList<int> widths; for (int i = 0; i < panes_->count(); ++i) widths.push_back(1);
        panes_->setSizes(widths);
        auto* layout = qobject_cast<QSplitter*>(centralWidget()); layout->setSizes({560, 140});
        find_->SetEditors(editors_);
        applyAppearance();
        updateMerge();
    }
    void CompareWindow::applyPairView()
    {
        delete multi_; multi_ = nullptr;
        semantic_->SetPair(pairIndex_);
        overview_->SetSession(nullptr, nullptr, {});
        if (scroll_) { scroll_->SetEnabled(false); scroll_->ClearPadding(); delete scroll_; scroll_ = nullptr; }
        for (auto* editor : editors_) editor->send(SCI_MARKERDELETEALL, 8);
        const auto* pair = currentPair(); if (!pair) return;
        auto* first = editors_[pair->inputs_[0]]; auto* second = editors_[pair->inputs_[1]];
        const QSignalBlocker blocker(changes_);
        if (!merge_)
        {
            changes_->clear();
            changes_->setHeaderLabels({tr("차이"), first->objectName(), second->objectName(), tr("해결")});
        }
        for (std::size_t i = 0; i < pair->changes_.size(); ++i)
        {
            const auto& edit = pair->changes_[i];
            if (!merge_)
            {
                auto* item = new QTreeWidgetItem(changes_, {QString::number(i + 1), QString::number(edit.begin_ + 1), QString::number(edit.otherBegin_ + 1)});
                item->setData(1, Qt::UserRole, qulonglong(edit.begin_)); item->setData(2, Qt::UserRole, qulonglong(edit.otherBegin_));
            }
            for (auto line = edit.begin_; line < edit.end_; ++line) first->send(SCI_MARKERADD, uptr_t(line), 8);
            for (auto line = edit.otherBegin_; line < edit.otherEnd_; ++line) second->send(SCI_MARKERADD, uptr_t(line), 8);
        }
        scroll_ = new ScrollSynchronizer(first, second, pair->alignment_, this);
        scroll_->SetAligned(alignRows_->isChecked()); overview_->SetAligned(alignRows_->isChecked());
        overview_->SetSourceNumbers(unsigned(pair->inputs_[0] + 1), unsigned(pair->inputs_[1] + 1));
        overview_->SetSession(first, second, pair->alignment_);
        // Include initial insertion gaps without changing caret/selection bytes.
        first->send(SCI_SETFIRSTVISIBLELINE, 0); second->send(SCI_SETFIRSTVISIBLELINE, 0);
        syncScroll_->setToolTip(tr("%1 / %2의 대응 줄과 가로 위치를 함께 이동합니다.").arg(first->objectName(), second->objectName()));
    }
    void CompareWindow::rebuildMultiAlignment()
    {
        delete multi_; multi_ = nullptr;
        if (!currentPair() || !scroll_) { updateAlignmentView(); return; }
        scroll_->SetEnabled(false); scroll_->ClearPadding();
        if (alignAll_->isChecked() && editors_.size() >= 3)
        {
            scroll_->SetAligned(false);
            auto ordered = editors_;
            if (!request_.base_.isEmpty()) ordered.swapItemsAt(0, 2);
            multi_ = new MultiPaneAlignment(ordered, this);
            multi_->SetEnabled(syncScroll_->isChecked() && !busy());
            multi_->changed_ = [this] { updateAlignmentView(); };
        }
        else
        {
            // ClearPadding does not change the synchronizer's mode bit.
            scroll_->SetAligned(false); scroll_->SetAligned(alignRows_->isChecked());
            scroll_->SetEnabled(syncScroll_->isChecked() && !busy());
        }
        updateAlignmentView();
    }
    void CompareWindow::updateAlignmentView()
    {
        const auto* pair = currentPair();
        if (!pair || editors_.size() < 2) { alignmentStatus_->hide(); return; }
        auto* left = editors_[pair->inputs_[0]]; auto* right = editors_[pair->inputs_[1]];
        const auto map = multi_ ? multi_->Pair(left, right) : pair->alignment_;
        overview_->SetSession(left, right, map ? map : pair->alignment_);
        overview_->SetAligned(multi_ ? multi_->Ready() : alignRows_->isChecked());
        alignmentStatus_->setVisible(multi_ && !multi_->Ready());
        if (multi_)
        {
            alignmentStatus_->setText(multi_->Error().isEmpty() ? tr("전체 정렬 계산 중") : tr("전체 정렬 보류"));
            alignmentStatus_->setToolTip(multi_->Error().isEmpty() ? tr("편집 내용과 Undo는 유지됩니다.") : tr("계산 한도로 개별 패널 표시를 유지합니다. 전체 패널 정렬을 껐다 켜서 다시 시도할 수 있습니다.\n") + multi_->Error());
        }
        applyInlineHighlights();
        for (auto* gutter : gutters_) gutter->Refresh();
    }
    void CompareWindow::rebuildGutters()
    {
        for (auto* gutter : gutters_) delete gutter; gutters_.clear();
        const auto* pair = currentPair(); if (!pair || editors_.size() < 2) return;
        const auto create = [this](ScintillaEditBase* Left, ScintillaEditBase* Right, int Side, int Position) {
            auto* gutter = new ChunkGutter(Left, Right, Side, panes_);
            panes_->insertWidget(Position, gutter); gutters_.push_back(gutter);
            gutter->choose_ = [this](auto Id, auto Revision, auto Choice) { chooseGutter(Id, Revision, Choice); };
        };
        if (merge_)
        {
            // Physical arrows point from each source toward the editable RESULT.
            panes_->insertWidget(1, output_->parentWidget());
            create(editors_[0], output_, 0, 1);
            create(output_, editors_[1], 1, 3);
        }
        else create(editors_[pair->inputs_[0]], editors_[pair->inputs_[1]], -1, panes_->indexOf(editors_[pair->inputs_[0]]->parentWidget()) + 1);
        updateGutters();
    }
    void CompareWindow::updateGutters()
    {
        const auto* pair = currentPair(); if (!pair || gutters_.isEmpty()) return;
        for (qsizetype side = 0; side < gutters_.size(); ++side)
        {
            auto* gutter = gutters_[side]; gutter->setEnabled(!busy()); std::vector<GutterChunk> chunks;
            if (merge_)
            {
                const auto& model = merge_->Model(); chunks.reserve(model.State().size());
                for (std::size_t i = 0; i < model.State().size(); ++i)
                {
                    const auto& state = model.State()[i]; const auto& block = model.Plan().blocks_[i];
                    // Unmapped edits cannot be drawn or applied at a trusted location.
                    if (!state.mapped_) continue;
                    const auto begin = side == 0 ? block.leftBegin_ : block.rightBegin_;
                    const auto end = begin + (side == 0 ? block.left_.size() : block.right_.size());
                    chunks.push_back({block.id_, side == 0 ? std::array{begin, state.begin_} : std::array{state.begin_, begin},
                        side == 0 ? std::array{end, state.end_} : std::array{state.end_, end}, true, state.resolution_ != Resolution::Unresolved});
                }
                gutter->SetChunks(std::move(chunks), model.Revision());
            }
            else
            {
                const auto position = [this, pair](int Side, std::size_t Line) {
                    auto* editor = editors_[pair->inputs_[Side]];
                    const auto at = editor->send(SCI_POSITIONFROMLINE, Line);
                    return std::size_t(at < 0 ? editor->send(SCI_GETLENGTH) : at);
                };
                chunks.reserve(pair->changes_.size());
                for (const auto& change : pair->changes_)
                    chunks.push_back({chunks.size() + 1, {position(0, change.begin_), position(1, change.otherBegin_)},
                        {position(0, change.end_), position(1, change.otherEnd_)}});
                gutter->SetChunks(std::move(chunks), bufferGeneration_);
            }
        }
    }
    void CompareWindow::chooseGutter(std::uint64_t Id, std::uint64_t Revision, Resolution Choice)
    {
        if (busy()) return;
        if (merge_)
        {
            try
            {
                if (Choice == Resolution::Left || Choice == Resolution::Right)
                {
                    const auto verified = merge_->Model().Prepare(Id, Revision, Choice);
                    const auto& block = merge_->Model().Plan().blocks_[Id - 1];
                    const int side = Choice == Resolution::Left ? 0 : 1;
                    const auto begin = side ? block.rightBegin_ : block.leftBegin_;
                    const auto length = side ? block.right_.size() : block.left_.size();
                    if (applySelectedChunk(editors_[side], output_, begin, begin + length, verified.begin_, verified.end_, Id, Revision)) return;
                }
                merge_->Choose(Id, Revision, Choice);
                if (Id && Id <= std::uint64_t(changes_->topLevelItemCount())) changes_->setCurrentItem(changes_->topLevelItem(int(Id - 1)));
                selectBlock();
            }
            catch (const Failure& error) { state_->setText(QString::fromStdString(error.code_)); }
            return;
        }
        applyInputChunk(Id, Revision, Choice);
    }
    void CompareWindow::chooseBlock(Resolution Choice)
    {
        if (!merge_ || busy() || !changes_->currentItem()) return;
        const auto* item = changes_->currentItem();
        try
        {
            merge_->Choose(item->data(0, Qt::UserRole).toULongLong(), item->data(3, Qt::UserRole).toULongLong(), Choice);
            selectBlock();
        }
        catch (const Failure& error) { state_->setText(QString::fromStdString(error.code_)); }
    }
    void CompareWindow::chooseSide(int Index)
    {
        if (!merge_ || busy() || result_.inputs_.size() <= Index) return;
        try { merge_->ChooseAll(Index == 0 ? Resolution::Left : Resolution::Right, result_.inputs_[Index].utf8_); }
        catch (const Failure& error) { state_->setText(QString::fromStdString(error.code_)); }
    }
    void CompareWindow::updateMerge()
    {
        recoveryAction_->setEnabled(!busy());
        exportReport_->setEnabled(!busy() && result_.completion_ == "Complete" && (!sourceEditing_ || (!sourcePending_ && sourceValid_)));
        for (auto* action : encodingActions_)
        {
            const int role = action->data().toInt();
            const auto path = role == 0 ? request_.file1_ : role == 1 ? request_.file2_ : request_.base_.isEmpty() ? request_.file3_ : request_.base_;
            action->setEnabled(!busy() && !path.isEmpty() && (!QFileInfo(path).isDir() || request_.directory_.level_ == "text") && result_.extension_.isEmpty());
            const auto codec = InputEncoding(request_, role);
            action->setChecked(action->property("codec").toString() == (codec.isEmpty() ? "auto" : codec));
        }
        directory_->setEnabled(!busy() || directoryStreaming_);
        const bool directoryMode = !directory_->isHidden() || extensionView_ != nullptr;
        for (auto* action : wholeActions_) action->setVisible(!directoryMode);
        for (auto* action : blockActions_) action->setVisible(!directoryMode);
        save_->setVisible(!directoryMode);
        const bool searchable = !editors_.isEmpty() && !busy();
        const bool pairSelection = result_.operation_ == "compare3" && result_.completion_ == "Complete";
        pairSelectorAction_->setVisible(pairSelection); pairSelector_->setEnabled(pairSelection && !busy());
        find_->SetBusy(busy()); for (auto* action : findActions_) action->setEnabled(searchable);
        syncScroll_->setEnabled(scroll_ && !busy());
        alignRows_->setEnabled(scroll_ && !busy() && !multi_);
        alignAll_->setEnabled(currentPair() && editors_.size() >= 3 && !busy());
        showOverview_->setEnabled(scroll_ != nullptr);
        overview_->setVisible(scroll_ && showOverview_->isChecked()); overview_->setEnabled(scroll_ && !busy());
        if (scroll_) scroll_->SetEnabled(syncScroll_->isChecked() && !busy() && !multi_ && currentPair());
        if (multi_) multi_->SetEnabled(syncScroll_->isChecked() && !busy());
        const bool ready = merge_ && !busy();
        saveWork_->setEnabled(ready); loadWork_->setEnabled(ready);
        importDecisions_->setEnabled(ready && request_.takeAll_.isEmpty()); exportDecisions_->setEnabled(ready && request_.takeAll_.isEmpty());
        for (auto* action : blockActions_) action->setEnabled(false);
        for (auto* action : wholeActions_) action->setEnabled(ready);
        undo_->setEnabled(ready && output_->send(SCI_CANUNDO)); redo_->setEnabled(ready && output_->send(SCI_CANREDO));
        updateGutters();
        for (int i = 0; i < inputSaveActions_.size(); ++i) inputSaveActions_[i]->setVisible(sourceEditing_ && i < result_.inputs_.size() && !(i == 2 && !request_.base_.isEmpty()));
        if (sourceEditing_) { updateInputState(); return; }
        if (!merge_) return;
        const auto& model = merge_->Model();
        for (std::size_t i = 0; i < model.State().size(); ++i)
        {
            auto* item = changes_->topLevelItem(int(i)); const auto& block = model.State()[i];
            const auto label = block.resolution_ == Resolution::Unresolved ? tr("미해결") : block.resolution_ == Resolution::Left ? "LEFT" :
                block.resolution_ == Resolution::Right ? "RIGHT" : block.resolution_ == Resolution::Base ? "BASE" :
                block.resolution_ == Resolution::LeftThenRight ? "LEFT → RIGHT" : block.resolution_ == Resolution::RightThenLeft ? "RIGHT → LEFT" : block.resolution_ == Resolution::Omit ? tr("제거") : tr("수동 완료");
            item->setText(3, label + (block.mapped_ ? QString{} : tr(" · 위치 재검토")));
            item->setToolTip(3, block.mapped_ ? QString{} : tr("RESULT의 블록 위치를 확정할 수 없습니다. 전체 결과 검토 후 ‘전체 수동 완료’를 선택하거나 전체 방향을 다시 선택하십시오."));
            item->setData(3, Qt::UserRole, qulonglong(model.Revision()));
        }
        if (ready && changes_->currentItem())
        {
            const auto index = changes_->indexOfTopLevelItem(changes_->currentItem());
            const bool mapped = index >= 0 && model.State()[std::size_t(index)].mapped_;
            for (auto* action : blockActions_) action->setEnabled(mapped);
            baseChoice_->setEnabled(mapped && model.Plan().hasBase_);
        }
        save_->setEnabled(ready && model.UnresolvedCount() == 0 && !model.ReviewRequired());
        if (ready)
        {
            unresolved_ = model.UnresolvedCount() != 0 || model.ReviewRequired(); dirty_ = merge_->Dirty();
            exitCode_ = merge_->Saved() ? 0 : 3;
            state_->setText(tr("미해결: %1 / %2 — %3").arg(model.UnresolvedCount()).arg(model.State().size()).arg(merge_->Saved() ? tr("저장 완료") : tr("결과 저장 필요")));
            if (model.ReviewRequired()) state_->setText(tr("RESULT 보존 · 전체 재검토 필요 — 새 입력 확인 후 ‘전체 수동 완료’ 또는 전체 방향 선택 (미해결 %1)").arg(model.UnresolvedCount()));
        }
    }
    void CompareWindow::updateFormats()
    {
        for (qsizetype i = 0; i < formatLabels_.size(); ++i)
        {
            const bool output = i >= result_.inputs_.size();
            const auto source = output ? (request_.takeAll_ == "right" ? 1 : 0) : i;
            if (source >= result_.inputs_.size()) continue;
            const auto& input = result_.inputs_[source];
            formatLabels_[i]->setText(QString("%1 · %2%3").arg(input.encoding_, input.bom_ ? tr("BOM 있음") : tr("BOM 없음"),
                output ? tr(" · 저장 형식 (%1)").arg(source == 0 ? "LEFT" : "RIGHT") : QString{}));
        }
    }
    void CompareWindow::selectBlock()
    {
        if (busy() || !changes_->currentItem() || !currentPair()) return;
        if (!merge_)
        {
            const auto* item = changes_->currentItem();
            if (multi_ && multi_->Ready()) { multi_->Reveal(editors_[currentPair()->inputs_[0]], item->data(1, Qt::UserRole).toULongLong()); return; }
            if (scroll_) scroll_->Reveal(item->data(1, Qt::UserRole).toULongLong(), item->data(2, Qt::UserRole).toULongLong());
            return;
        }
        const auto at = changes_->indexOfTopLevelItem(changes_->currentItem()); if (at < 0) return;
        const auto& block = merge_->Model().Plan().blocks_[std::size_t(at)];
        const auto& state = merge_->Model().State()[std::size_t(at)];
        if (scroll_) scroll_->Reveal(block.leftLine_, block.rightLine_);
        if (state.mapped_) { output_->send(SCI_SETSEL, state.begin_, sptr_t(state.end_)); output_->send(SCI_SCROLLCARET); }
        if (multi_ && multi_->Ready()) multi_->Reveal(state.mapped_ ? output_ : editors_[0],
            state.mapped_ ? std::size_t(output_->send(SCI_LINEFROMPOSITION, state.begin_)) : block.leftLine_);
    }
    void CompareWindow::navigateBlock(int Direction)
    {
        if (busy()) return;
        if (directory_->isVisible()) { directory_->NavigateDifference(Direction); return; }
        const auto count = changes_->topLevelItemCount(); if (!count) return;
        const auto at = changes_->indexOfTopLevelItem(changes_->currentItem());
        auto* target = changes_->topLevelItem(at < 0 ? (Direction > 0 ? 0 : count - 1) : (at + Direction + count) % count);
        if (target == changes_->currentItem()) selectBlock(); else changes_->setCurrentItem(target);
    }
    void CompareWindow::save()
    {
        if (sourceEditing_) { saveInput(activeInput_); return; }
        if (!output_ || busy()) return;
        if (unresolved_) { QMessageBox::information(this, tr("충돌 해결"), tr("충돌을 검토하고 ‘수동 해결 완료’를 선택하십시오.")); return; }
        const auto text = contents(output_);
        output_->send(SCI_SETREADONLY, 1); save_->setEnabled(false); saving_ = true; state_->setText(tr("저장 중…"));
        watcher_.setFuture(QtConcurrent::run([request = request_, result = result_, text]() mutable {
            try { SaveResult(request, result, text); }
            catch (const std::exception& error) { result.exitCode_ = 2; result.error_ = QString::fromUtf8(error.what()); }
            return result;
        }));
        updateMerge();
    }
    void CompareWindow::SaveWork(const QString& Path, bool ReplaceExisting) { transferWork(Path, false, ReplaceExisting); }
    void CompareWindow::LoadWork(const QString& Path) { transferWork(Path, true, false); }
    void CompareWindow::ImportDecisions(const QString& Path) { transferWork(Path, true, false, true); }
    void CompareWindow::ExportDecisions(const QString& Path, bool ReplaceExisting) { transferWork(Path, false, ReplaceExisting, true); }
    void CompareWindow::transferWork(const QString& Path, bool Loading, bool ReplaceExisting, bool Decisions)
    {
        if (!merge_ || busy() || Path.isEmpty()) return;
        const auto absolute = QFileInfo(Path).absoluteFilePath();
        const auto expected = absolute.compare(workPath_, Qt::CaseInsensitive) == 0 ? workExpected_ : std::nullopt;
        const MergeWork work = Loading ? MergeWork{} : MergeWork{contents(output_), merge_->Model().State(), merge_->Model().ReviewRequired()};
        canceled_ = std::make_shared<std::atomic_bool>(false);
        output_->send(SCI_SETREADONLY, 1); cancel_->setEnabled(true);
        auto input = request_; if (Decisions && !workPath_.isEmpty()) input.protectedInputs_.append(workPath_);
        workWatcher_.setFuture(QtConcurrent::run([path = absolute, input, session = result_, work, expected, Loading, ReplaceExisting, Decisions, cancel = canceled_] {
            WorkTransfer transfer; transfer.path_ = path; transfer.loading_ = Loading; transfer.decisions_ = Decisions;
            Budget limits; limits.canceled_ = cancel.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(input.timeoutMs_);
            try
            {
                if (Decisions) transfer.file_ = Loading ? LoadDecisionWorkFile(path, input, session, limits) : SaveDecisionWorkFile(path, input, session, work, ReplaceExisting, limits);
                else transfer.file_ = Loading ? LoadMergeWorkFile(path, input, session, limits) : SaveMergeWorkFile(path, input, session, work, expected, ReplaceExisting, limits);
            }
            catch (const SaveRecoveryFailure& error) { transfer.error_ = QString::fromUtf8(error.what()); transfer.file_.recovery_ = error.recovery_; }
            catch (const std::exception& error) { transfer.error_ = QString::fromUtf8(error.what()); }
            return transfer;
        }));
        updateMerge(); state_->setText(Decisions ? (Loading ? tr("결정 계획 불러오는 중…") : tr("결정 계획 내보내는 중…")) : (Loading ? tr("작업 불러오는 중…") : tr("작업 저장 중…")));
    }
    void CompareWindow::ExportReport(const QString& Path, bool ReplaceExisting)
    {
        if (Path.isEmpty() || !exportReport_->isEnabled() || busy() || (sourceEditing_ && (sourcePending_ || !sourceValid_))) return;
        auto session = result_; QJsonArray workingInputs;
        for (int i = 0; i < session.inputs_.size(); ++i)
        {
            const auto& text = session.inputs_[i].utf8_;
            workingInputs.append(QJsonObject{{"index", i + 1}, {"utf8Sha256", QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex())},
                {"modified", sourceEditing_ && i < editors_.size() && bool(editors_[i]->send(SCI_GETMODIFY))}});
        }
        QJsonObject view{{"capturedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"scope", sourceEditing_ ? "working-buffers" : "loaded-snapshots"}, {"workingInputs", workingInputs}, {"guiExitCode", exitCode_}};
        if (!session.extension_.isEmpty()) view.remove("workingInputs");
        if (merge_)
        {
            auto plan = std::make_shared<MergePlan>(*session.plan_); plan->initial_ = merge_->Model().State(); session.plan_ = plan;
            const auto text = contents(output_);
            view["workingResult"] = QJsonObject{{"utf8Sha256", QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex())},
                {"bytes", text.size()}, {"modified", merge_->Dirty()}, {"saved", merge_->Saved()}, {"reviewRequired", merge_->Model().ReviewRequired()}};
        }
        auto input = request_; if (!workPath_.isEmpty()) input.protectedInputs_.append(workPath_);
        canceled_ = std::make_shared<std::atomic_bool>(false);
        reportWatcher_.setFuture(QtConcurrent::run([path = QFileInfo(Path).absoluteFilePath(), input, session, view, ReplaceExisting, cancel = canceled_] {
            ReportTransfer transfer; transfer.path_ = path;
            Budget limits; limits.canceled_ = cancel.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(input.timeoutMs_);
            try { transfer.file_ = SaveComparisonReport(path, input, session, view, ReplaceExisting, limits); }
            catch (const SaveRecoveryFailure& error) { transfer.error_ = QString::fromUtf8(error.what()); transfer.file_.recovery_ = error.recovery_; }
            catch (const std::exception& error) { transfer.error_ = QString::fromUtf8(error.what()); }
            return transfer;
        }));
        updateMerge(); cancel_->setEnabled(true); state_->setText(tr("비교 보고서 저장 중…"));
    }
    void CompareWindow::applyInlineHighlights()
    {
        const auto* pair = currentPair();
        const bool enabled = inlineHighlights_->isChecked() && pair;
        inlineHighlights_->setEnabled(pair != nullptr);
        const auto* detail = pair ? &pair->inline_ : nullptr;
        if (multi_ && pair) detail = multi_->Inline(editors_[pair->inputs_[0]], editors_[pair->inputs_[1]]);
        const auto* resultDetail = multi_ && !request_.base_.isEmpty() && output_ && editors_.size() == 4 ? multi_->Inline(editors_[2], output_) : nullptr;
        const auto skipped = (detail ? detail->skippedLines_ : 0) + (resultDetail ? resultDetail->skippedLines_ : 0);
        inlineStatus_->setVisible(enabled && skipped != 0);
        inlineStatus_->setText(tr("문자 강조 일부 생략 (%1줄)").arg(skipped));
        inlineHighlights_->setToolTip(resultDetail ? tr("원본 비교 쌍과 BASE ↔ RESULT의 문자 차이를 공통 정렬 기준으로 표시합니다.") : tr("선택한 비교 쌍의 문자 차이를 표시합니다."));
        for (qsizetype index = 0; index < editors_.size(); ++index)
        {
            auto* editor = editors_[index];
            const auto current = editor->send(SCI_GETINDICATORCURRENT), value = editor->send(SCI_GETINDICATORVALUE);
            editor->send(SCI_SETINDICATORCURRENT, sourceDifferenceIndicator);
            editor->send(SCI_INDICATORCLEARRANGE, 0, editor->send(SCI_GETLENGTH));
            editor->send(SCI_INDICSETSTYLE, sourceDifferenceIndicator, INDIC_ROUNDBOX);
            editor->send(SCI_INDICSETFORE, sourceDifferenceIndicator, color(dark_ ? QColor("#ef947e") : QColor("#b84b32")));
            editor->send(SCI_INDICSETALPHA, sourceDifferenceIndicator, dark_ ? 70 : 50);
            editor->send(SCI_INDICSETOUTLINEALPHA, sourceDifferenceIndicator, 160);
            editor->send(SCI_INDICSETUNDER, sourceDifferenceIndicator, 1);
            editor->send(SCI_SETINDICATORVALUE, 1);
            int side = pair && index == pair->inputs_[0] ? 0 : pair && index == pair->inputs_[1] ? 1 : -1;
            const auto* decoration = detail;
            if (resultDetail && (index == 2 || editor == output_)) { decoration = resultDetail; side = index == 2 ? 0 : 1; }
            if (enabled && decoration && side >= 0)
                for (const auto& range : decoration->ranges_[std::size_t(side)])
                    editor->send(SCI_INDICATORFILLRANGE, uptr_t(range.begin_), sptr_t(range.end_ - range.begin_));
            editor->send(SCI_SETINDICATORCURRENT, uptr_t(current)); editor->send(SCI_SETINDICATORVALUE, uptr_t(value));
        }
    }
    void CompareWindow::applyAppearance()
    {
        auto& theme = fm::style::ThemeManager::instance(); theme.setScheme(dark_ ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        qApp->setFont(uiFont_); changes_->setFont(listFont_); find_->SetListFont(listFont_); directory_->SetListFont(listFont_); semantic_->SetListFont(listFont_);
        if (extensionView_) { extensionView_->SetListFont(listFont_); extensionView_->SetTextFont(textFont_); }
        const QColor background = dark_ ? QColor("#202632") : QColor("#fbfcff"), foreground = dark_ ? QColor("#e3e8ef") : QColor("#202630");
        const auto family = textFont_.family().toUtf8();
        for (auto* editor : editors_)
        {
            editor->send(SCI_STYLESETFORE, STYLE_DEFAULT, color(foreground)); editor->send(SCI_STYLESETBACK, STYLE_DEFAULT, color(background));
            editor->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(family.constData()));
            editor->send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, sptr_t(textFont_.pointSizeF() * 100)); editor->send(SCI_STYLECLEARALL);
            for (const auto style : {1, 2, 3}) editor->send(SCI_STYLESETFORE, style, color(dark_ ? QColor("#90b58b") : QColor("#51774c")));
            editor->send(SCI_STYLESETFORE, 5, color(dark_ ? QColor("#a6c7ff") : QColor("#255ba4")));
            editor->send(SCI_STYLESETFORE, 6, color(dark_ ? QColor("#edc38a") : QColor("#945410")));
            editor->send(SCI_STYLESETBACK, STYLE_LINENUMBER, color(background));
            editor->send(SCI_SETCARETFORE, color(foreground));
            // Search keeps focus in the find bar. Both active and inactive
            // selections must remain readable against Light/Dark syntax colors.
            for (const auto element : {SC_ELEMENT_SELECTION_TEXT, SC_ELEMENT_SELECTION_ADDITIONAL_TEXT,
                SC_ELEMENT_SELECTION_INACTIVE_TEXT, SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_TEXT})
                editor->send(SCI_SETELEMENTCOLOUR, element, color(dark_ ? QColor("#ffffff") : QColor("#142236")) | 0xff000000u);
            for (const auto element : {SC_ELEMENT_SELECTION_BACK, SC_ELEMENT_SELECTION_ADDITIONAL_BACK})
                editor->send(SCI_SETELEMENTCOLOUR, element, color(dark_ ? QColor("#395d83") : QColor("#bdd5f6")) | 0xff000000u);
            for (const auto element : {SC_ELEMENT_SELECTION_INACTIVE_BACK, SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_BACK})
                editor->send(SCI_SETELEMENTCOLOUR, element, color(dark_ ? QColor("#344252") : QColor("#dae5f3")) | 0xff000000u);
            editor->send(SCI_MARKERSETBACK, 8, color(dark_ ? QColor("#4d4635") : QColor("#fff0bb")));
            editor->send(SCI_COLOURISE, 0, -1);
        }
        applyInlineHighlights(); overview_->SetDark(dark_);
        QSettings settings; settings.setValue("theme/dark", dark_); settings.setValue("fonts/ui", uiFont_.toString());
        settings.setValue("fonts/list", listFont_.toString()); settings.setValue("fonts/text", textFont_.toString());
    }
    void CompareWindow::chooseFont(const QString& Role)
    {
        auto& font = Role == "ui" ? uiFont_ : Role == "list" ? listFont_ : textFont_; bool ok = false;
        const auto selected = QFontDialog::getFont(&ok, font, this, Role.toUpper()); if (ok) { font = selected; applyAppearance(); }
    }
    void CompareWindow::BindRequest(std::function<void(int)> Completed)
    {
        completed_ = std::move(Completed); openFiles_->setEnabled(false); openFolders_->setEnabled(false);
        setWindowTitle("HyperCompare — " + QFileInfo(request_.file1_).fileName() + " ↔ " + QFileInfo(request_.file2_).fileName());
    }
    bool CompareWindow::busy() const { return (directory_ && directory_->Busy()) || watcher_.isRunning() || workWatcher_.isRunning() || reportWatcher_.isRunning() || inputSaveWatcher_.isRunning() || (directoryMerge_ && directoryMerge_->Busy()) || (recoveryDialog_ && recoveryDialog_->Busy()); }
    void CompareWindow::CancelRemoteRequest() { canceled_->store(true); if (directory_) directory_->Cancel(); if (directoryMerge_) directoryMerge_->Cancel(); if (recoveryDialog_) recoveryDialog_->Cancel(); }
    void CompareWindow::closeEvent(QCloseEvent* Event)
    {
        if (busy()) { CancelRemoteRequest(); closing_ = true; Event->ignore(); return; }
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 닫으시겠습니까?")) != QMessageBox::Yes) { closing_ = false; Event->ignore(); return; }
        Event->accept(); if (auto completed = std::move(completed_)) completed(exitCode_);
    }
}
