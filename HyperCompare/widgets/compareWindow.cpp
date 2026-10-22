#include "stdafx.h"
#include "compareWindow.hpp"
#include "mergeController.hpp"
#include "scrollSynchronizer.hpp"
#include "findBar.hpp"
#include "overviewBar.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QLabel>
#include <QJsonDocument>
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
        auto* sources = new QWidget(layout); auto* row = new QHBoxLayout(sources); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
        panes_ = new QSplitter(Qt::Horizontal, sources); overview_ = new OverviewBar(sources);
        row->addWidget(panes_, 1); row->addWidget(overview_);
        changes_ = new QTreeWidget(layout); layout->addWidget(sources); layout->addWidget(changes_);
        panes_->setChildrenCollapsible(false); layout->setChildrenCollapsible(false);
        changes_->setHeaderLabels({tr("차이 / 경로"), tr("왼쪽"), tr("오른쪽")});
        changes_->setObjectName("compareChanges");
        changes_->setRootIsDecorated(false); changes_->setUniformRowHeights(true);
        layout->setStretchFactor(0, 4); layout->setStretchFactor(1, 1); layout->setSizes({560, 140}); setCentralWidget(layout);
        auto* tools = addToolBar(tr("비교"));
        tools->addAction(tr("파일 열기"), this, &CompareWindow::openInputs);
        tools->addAction(tr("다시 비교"), this, [this] { start(true); });
        cancel_ = tools->addAction(tr("취소"), this, [this] { canceled_->store(true); state_->setText(tr("취소 요청 중")); });
        cancel_->setEnabled(false);
        save_ = tools->addAction(tr("결과 저장"), this, &CompareWindow::save); save_->setEnabled(false);
        pairSelector_ = new QComboBox(tools); pairSelector_->setObjectName("comparePairSelector");
        pairSelector_->setAccessibleName(tr("비교 쌍"));
        pairSelector_->addItems({tr("FILE 1 ↔ FILE 2"), tr("FILE 1 ↔ FILE 3"), tr("FILE 2 ↔ FILE 3")});
        pairSelector_->setToolTip(tr("선택한 두 원본에 차이 강조·정렬·개요·탐색을 적용합니다. 열린 입력 내용으로 전환하며 파일을 다시 읽으려면 ‘다시 비교’를 사용합니다."));
        pairSelectorAction_ = tools->addWidget(pairSelector_); pairSelectorAction_->setVisible(false);
        connect(pairSelector_, &QComboBox::currentIndexChanged, this, [this](int Index) {
            if (busy() || result_.operation_ != "compare3" || Index < 0 || Index > 2 || !result_.pairViews_[std::size_t(Index)]) return;
            pairIndex_ = Index; applyPairView(); applyAppearance(); updateMerge();
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
            {tr("RIGHT → LEFT"), Resolution::RightThenLeft}, {tr("블록 수동 완료"), Resolution::Manual}})
        {
            auto* action = mergeTools->addAction(choice.first, this, [this, value = choice.second] { chooseBlock(value); });
            blockActions_.push_back(action); action->setEnabled(false);
            if (choice.second == Resolution::Base) baseChoice_ = action;
        }
        auto* fileMenu = menuBar()->addMenu(tr("파일"));
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
        saveWork_->setShortcut(QKeySequence("Ctrl+Shift+S")); loadWork_->setShortcut(QKeySequence("Ctrl+Shift+O"));
        saveWork_->setEnabled(false); loadWork_->setEnabled(false);
        auto* editMenu = menuBar()->addMenu(tr("편집"));
        undo_ = editMenu->addAction(tr("실행 취소"), this, [this] { if (output_ && !busy()) output_->send(SCI_UNDO); });
        redo_ = editMenu->addAction(tr("다시 실행"), this, [this] { if (output_ && !busy()) output_->send(SCI_REDO); });
        undo_->setShortcut(QKeySequence::Undo); redo_->setShortcut(QKeySequence::Redo); undo_->setEnabled(false); redo_->setEnabled(false);
        find_ = new FindBar(this); addToolBar(Qt::BottomToolBarArea, find_);
        editMenu->addSeparator();
        auto* find = editMenu->addAction(tr("찾기"), find_, &FindBar::Open); find->setShortcut(QKeySequence::Find);
        auto* findNext = editMenu->addAction(tr("다음 찾기"), this, [this] { find_->Find(false); }); findNext->setShortcut(QKeySequence("F3"));
        auto* findPrevious = editMenu->addAction(tr("이전 찾기"), this, [this] { find_->Find(true); }); findPrevious->setShortcut(QKeySequence("Shift+F3"));
        findActions_ = {find, findNext, findPrevious}; for (auto* action : findActions_) action->setEnabled(false);
        auto* closeFind = new QAction(this); closeFind->setShortcut(QKeySequence(Qt::Key_Escape)); closeFind->setEnabled(false); addAction(closeFind);
        connect(closeFind, &QAction::triggered, find_, &FindBar::Close);
        connect(find_, &QToolBar::visibilityChanged, closeFind, &QAction::setEnabled);
        auto* viewMenu = menuBar()->addMenu(tr("보기"));
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
                scroll_->SetEnabled(Enabled && !busy());
                if (Enabled) scroll_->Synchronize(editors_[currentPair()->inputs_[1]]->hasFocus() ? 1 : 0);
            }
        });
        inlineHighlights_ = viewMenu->addAction(tr("줄 내부 차이 강조")); inlineHighlights_->setCheckable(true);
        inlineHighlights_->setChecked(settings.value("view/inlineHighlights", true).toBool());
        inlineHighlights_->setToolTip(tr("선택한 두 원본의 변경 줄을 순서대로 짝지어 문자 차이를 표시합니다. 긴 줄은 줄 단위 표시를 유지합니다."));
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
        connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
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
                    workPath_ = transfer.path_; workExpected_ = transfer.file_.expected_;
                }
                catch (const std::exception& error) { transfer.error_ = QString::fromUtf8(error.what()); }
            }
            updateMerge();
            if (!transfer.error_.isEmpty()) state_->setText(tr("작업 파일 처리 중단: ") + transfer.error_);
            else if (transfer.loading_) state_->setText(tr("작업 복원 완료 — 실행 취소로 이전 RESULT 복원 가능"));
            else state_->setText(transfer.file_.warnings_.isEmpty() && transfer.file_.recovery_.isEmpty() ?
                tr("작업 파일 저장 완료 — RESULT 저장 상태 유지") : tr("작업 파일 저장 완료 — 정리·파일 변경 확인 필요"));
            state_->setToolTip(transfer.path_ + '\n' + QString::fromUtf8(QJsonDocument(QJsonObject{
                {"warnings", transfer.file_.warnings_}, {"recovery", transfer.file_.recovery_}}).toJson()));
            if (closing_) close();
        });
        connect(changes_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Item) {
            if (!Item || editors_.size() < 2 || result_.operation_ == "directory2") return;
            selectBlock(); if (merge_) updateMerge();
        });
        applyAppearance();
        if (!request_.file1_.isEmpty()) QTimer::singleShot(0, this, [this] { start(); });
        else state_->setText(tr("파일 열기 또는 -file1/-file2로 비교를 시작하십시오."));
    }
    CompareWindow::~CompareWindow() { canceled_->store(true); watcher_.waitForFinished(); workWatcher_.waitForFinished(); }
    void CompareWindow::openInputs()
    {
        if (busy()) return;
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 파일을 여시겠습니까?")) != QMessageBox::Yes) return;
        const auto left = QFileDialog::getOpenFileName(this, tr("왼쪽 파일")); if (left.isEmpty()) return;
        const auto right = QFileDialog::getOpenFileName(this, tr("오른쪽 파일")); if (right.isEmpty()) return;
        request_.file1_ = left; request_.file2_ = right; request_.base_.clear(); request_.file3_.clear();
        request_.output_.clear(); request_.mode_ = "compare"; start();
    }
    void CompareWindow::start(bool Preserve)
    {
        if (busy() || request_.file1_.isEmpty()) return;
        canceled_ = std::make_shared<std::atomic_bool>(false);
        refreshing_ = Preserve && merge_;
        if (!refreshing_) { dirty_ = false; exitCode_ = 3; }
        save_->setEnabled(false); state_->setText(tr("비교 중…"));
        cancel_->setEnabled(true);
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
        if (changed)
        {
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
            showBlocks(); merge_->Refresh(result_.plan_); applyAppearance();
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
        }
        if (changes_->topLevelItemCount()) changes_->setCurrentItem(changes_->topLevelItem(0));
    }
    ScintillaEditBase* CompareWindow::addPane(const QString& Title, const QString& Path, const QByteArray& Text, bool ReadOnly)
    {
        auto* pane = new QWidget(panes_); auto* box = new QVBoxLayout(pane); box->setContentsMargins(2, 2, 2, 2);
        auto* label = new QLabel(Title + " — " + QFileInfo(Path).fileName(), pane); label->setTextFormat(Qt::PlainText); label->setToolTip(Path);
        label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); box->addWidget(label);
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
        if (result_.operation_ != "compare3") pairIndex_ = 0;
        { const QSignalBlocker blocker(pairSelector_); pairSelector_->setCurrentIndex(pairIndex_); }
        workPath_.clear(); workExpected_.reset();
        find_->SetEditors({});
        overview_->SetSession(nullptr, nullptr, {}); overview_->hide();
        delete scroll_; scroll_ = nullptr;
        delete merge_; merge_ = nullptr; output_ = nullptr; editors_.clear(); changes_->clear();
        inlineStatus_->hide(); inlineHighlights_->setEnabled(false);
        changes_->setHeaderLabels({tr("차이 / 경로"), tr("왼쪽"), tr("오른쪽"), tr("해결")});
        while (panes_->count()) delete panes_->widget(0);
        exitCode_ = result_.exitCode_; unresolved_ = !result_.merged_.conflicts_.empty() || result_.error_ == "DecisionsRequired";
        if (result_.completion_ != "Complete") { updateMerge(); state_->setText(result_.error_); return; }
        if (result_.operation_ == "directory2")
        {
            for (const auto& value : result_.entries_)
            { const auto entry = value.toObject(); new QTreeWidgetItem(changes_, {entry["path"].toString(), entry["status"].toString()}); }
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
        if (request_.mode_ == "merge") exitCode_ = 3;
        QList<int> widths; for (int i = 0; i < panes_->count(); ++i) widths.push_back(1);
        panes_->setSizes(widths);
        auto* layout = qobject_cast<QSplitter*>(centralWidget()); layout->setSizes({560, 140});
        find_->SetEditors(editors_);
        applyAppearance();
        updateMerge();
    }
    void CompareWindow::applyPairView()
    {
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
        const bool searchable = !editors_.isEmpty() && !busy();
        const bool pairSelection = result_.operation_ == "compare3" && result_.completion_ == "Complete";
        pairSelectorAction_->setVisible(pairSelection); pairSelector_->setEnabled(pairSelection && !busy());
        find_->SetBusy(busy()); for (auto* action : findActions_) action->setEnabled(searchable);
        syncScroll_->setEnabled(scroll_ && !busy());
        alignRows_->setEnabled(scroll_ && !busy());
        showOverview_->setEnabled(scroll_ != nullptr);
        overview_->setVisible(scroll_ && showOverview_->isChecked()); overview_->setEnabled(scroll_ && !busy());
        if (scroll_) scroll_->SetEnabled(syncScroll_->isChecked() && !busy());
        const bool ready = merge_ && !busy();
        saveWork_->setEnabled(ready); loadWork_->setEnabled(ready);
        for (auto* action : blockActions_) action->setEnabled(false);
        for (auto* action : wholeActions_) action->setEnabled(ready);
        undo_->setEnabled(ready && output_->send(SCI_CANUNDO)); redo_->setEnabled(ready && output_->send(SCI_CANREDO));
        if (!merge_) return;
        const auto& model = merge_->Model();
        for (std::size_t i = 0; i < model.State().size(); ++i)
        {
            auto* item = changes_->topLevelItem(int(i)); const auto& block = model.State()[i];
            const auto label = block.resolution_ == Resolution::Unresolved ? tr("미해결") : block.resolution_ == Resolution::Left ? "LEFT" :
                block.resolution_ == Resolution::Right ? "RIGHT" : block.resolution_ == Resolution::Base ? "BASE" :
                block.resolution_ == Resolution::LeftThenRight ? "LEFT → RIGHT" : block.resolution_ == Resolution::RightThenLeft ? "RIGHT → LEFT" : tr("수동 완료");
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
    void CompareWindow::selectBlock()
    {
        if (busy() || !changes_->currentItem()) return;
        if (!merge_)
        {
            const auto* item = changes_->currentItem();
            if (scroll_) scroll_->Reveal(item->data(1, Qt::UserRole).toULongLong(), item->data(2, Qt::UserRole).toULongLong());
            return;
        }
        const auto at = changes_->indexOfTopLevelItem(changes_->currentItem()); if (at < 0) return;
        const auto& block = merge_->Model().Plan().blocks_[std::size_t(at)];
        const auto& state = merge_->Model().State()[std::size_t(at)];
        if (scroll_) scroll_->Reveal(block.leftLine_, block.rightLine_);
        if (state.mapped_) { output_->send(SCI_SETSEL, state.begin_, sptr_t(state.end_)); output_->send(SCI_SCROLLCARET); }
    }
    void CompareWindow::navigateBlock(int Direction)
    {
        const auto count = changes_->topLevelItemCount(); if (!count || busy()) return;
        const auto at = changes_->indexOfTopLevelItem(changes_->currentItem());
        auto* target = changes_->topLevelItem(at < 0 ? (Direction > 0 ? 0 : count - 1) : (at + Direction + count) % count);
        if (target == changes_->currentItem()) selectBlock(); else changes_->setCurrentItem(target);
    }
    void CompareWindow::save()
    {
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
    void CompareWindow::transferWork(const QString& Path, bool Loading, bool ReplaceExisting)
    {
        if (!merge_ || busy() || Path.isEmpty()) return;
        const auto absolute = QFileInfo(Path).absoluteFilePath();
        const auto expected = absolute.compare(workPath_, Qt::CaseInsensitive) == 0 ? workExpected_ : std::nullopt;
        const MergeWork work = Loading ? MergeWork{} : MergeWork{contents(output_), merge_->Model().State(), merge_->Model().ReviewRequired()};
        canceled_ = std::make_shared<std::atomic_bool>(false);
        output_->send(SCI_SETREADONLY, 1); cancel_->setEnabled(true);
        workWatcher_.setFuture(QtConcurrent::run([path = absolute, input = request_, session = result_, work, expected, Loading, ReplaceExisting, cancel = canceled_] {
            WorkTransfer transfer; transfer.path_ = path; transfer.loading_ = Loading;
            Budget limits; limits.canceled_ = cancel.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(input.timeoutMs_);
            try { transfer.file_ = Loading ? LoadMergeWorkFile(path, input, session, limits) : SaveMergeWorkFile(path, input, session, work, expected, ReplaceExisting, limits); }
            catch (const std::exception& error) { transfer.error_ = QString::fromUtf8(error.what()); }
            return transfer;
        }));
        updateMerge(); state_->setText(Loading ? tr("작업 불러오는 중…") : tr("작업 저장 중…"));
    }
    void CompareWindow::applyInlineHighlights()
    {
        const auto* pair = currentPair();
        const bool enabled = inlineHighlights_->isChecked() && pair;
        inlineHighlights_->setEnabled(pair != nullptr);
        inlineStatus_->setVisible(enabled && pair->inline_.skippedLines_ != 0);
        inlineStatus_->setText(tr("문자 강조 일부 생략 (%1줄)").arg(pair ? pair->inline_.skippedLines_ : 0));
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
            const int side = pair && index == pair->inputs_[0] ? 0 : pair && index == pair->inputs_[1] ? 1 : -1;
            if (enabled && side >= 0)
                for (const auto& range : pair->inline_.ranges_[std::size_t(side)])
                    editor->send(SCI_INDICATORFILLRANGE, uptr_t(range.begin_), sptr_t(range.end_ - range.begin_));
            editor->send(SCI_SETINDICATORCURRENT, uptr_t(current)); editor->send(SCI_SETINDICATORVALUE, uptr_t(value));
        }
    }
    void CompareWindow::applyAppearance()
    {
        auto& theme = fm::style::ThemeManager::instance(); theme.setScheme(dark_ ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
        qApp->setFont(uiFont_); changes_->setFont(listFont_);
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
    void CompareWindow::closeEvent(QCloseEvent* Event)
    {
        if (busy()) { canceled_->store(true); closing_ = true; Event->ignore(); return; }
        if (dirty_ && QMessageBox::question(this, tr("수정 결과"), tr("저장하지 않은 결과를 버리고 닫으시겠습니까?")) != QMessageBox::Yes) { closing_ = false; Event->ignore(); return; }
        Event->accept();
    }
}
