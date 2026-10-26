#include "stdafx.h"
#include "largeTextView.hpp"
#include <fmstyle/ThemeManager.h>
#include "largeTextSyntax.hpp"
#include "boundedEditor.hpp"
#include "chunkGutter.hpp"
#include "runtime/controllerMemory.hpp"
#include "runtime/saveRecovery.hpp"
#include <QtConcurrent/QtConcurrentRun>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QComboBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QShortcut>
#include <QFileInfo>
#include <QTimer>
#include <ILexer.h>
#include <SciLexer.h>
#include <Lexilla.h>
#include <set>

namespace hc::compare
{
    namespace {
        bool same(const LargeTextDocument& A, const LargeTextDocument& B) {
            if (!A.contentHash_.isEmpty() && !B.contentHash_.isEmpty()) return A.bytes_ == B.bytes_ && A.contentHash_ == B.contentHash_;
            if (A.bytes_ != B.bytes_ || A.pieces_.size() != B.pieces_.size()) return false;
            for (std::size_t i = 0; i < A.pieces_.size(); ++i) {
                const auto& a = A.pieces_[i]; const auto& b = B.pieces_[i];
                if (a.backing_ != b.backing_ || a.begin_ != b.begin_ || a.bytes_ != b.bytes_) return false;
            }
            return true;
        }
        QByteArray contents(ScintillaEditBase* Editor) {
            const auto size = Editor->send(SCI_GETLENGTH); QByteArray result(size + 1, '\0');
            Editor->send(SCI_GETTEXT, result.size(), reinterpret_cast<sptr_t>(result.data())); result.resize(size); return result;
        }
        const char* lexer(const QString& Path) {
            const auto ext = QFileInfo(Path).suffix().toLower();
            if (QStringList{"c", "cpp", "cc", "cxx", "h", "hpp", "cs", "java", "js", "ts"}.contains(ext)) return "cpp";
            if (QStringList{"py", "pyw", "pyi"}.contains(ext)) return "python";
            if (ext == "json") return "json"; if (ext == "xml" || ext == "html") return "xml"; return "null";
        }
    }
    const char* LargeTextLexer(const QString& Path) { return lexer(Path); }
    LargeTextView::LargeTextView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent)
        : ExtensionView(Parent), input_(std::move(Input)), value_(std::move(Value)), saved_(value_->documents_)
    {
        setObjectName("largeTextView"); auto* box = new QVBoxLayout(this); auto* actions = new QHBoxLayout(); box->addLayout(actions);
        applyTimer_ = new QTimer(this); applyTimer_->setSingleShot(true); applyTimer_->setInterval(700);
        connect(applyTimer_, &QTimer::timeout, this, [this] { applyPages(); });
        const auto button = [&](const QString& Text, const QString& Name, auto Callback) {
            auto* b = new QPushButton(Text, this); b->setObjectName(Name); actions->addWidget(b); buttons_.append(b); connect(b, &QPushButton::clicked, this, Callback);
        };
        button(tr("페이지 편집 적용"), "largeApplyPages", [this] { applyPages(); });
        button(tr("왼쪽 → 오른쪽"), "largeMergeRight", [this] { merge(0); });
        button(tr("왼쪽 ← 오른쪽"), "largeMergeLeft", [this] { merge(1); });
        button(tr("실행 취소"), "largeUndo", [this] { Undo(false); }); button(tr("다시 실행"), "largeRedo", [this] { Undo(true); });
        button(tr("왼쪽 저장"), "largeSaveLeft", [this] { save(0); }); button(tr("오른쪽 저장"), "largeSaveRight", [this] { save(1); });
        auto* cancel = new QPushButton(tr("취소"), this); actions->addWidget(cancel); connect(cancel, &QPushButton::clicked, this, [this] { Cancel(); });
        auto* navigation = new QHBoxLayout(); box->addLayout(navigation); navigation->addWidget(new QLabel(tr("차이 번호"), this));
        if (value_->session_) {
            savedAll_ = value_->session_->documents_; resultNeverSaved_ = merging();
            pairs_ = new QComboBox(this); pairs_->setObjectName("largePairSelector"); navigation->insertWidget(0, pairs_);
            const auto& session = *value_->session_; int selected = 0;
            for (int a = 0; a < int(session.documents_.size()); ++a) for (int b = a + 1; b < int(session.documents_.size()); ++b) {
                if (session.result_ >= 0 && b != session.result_) continue;
                if (session.pair_ == std::array{a, b}) selected = pairs_->count();
                pairs_->addItem(session.titles_[a] + " ↔ " + session.titles_[b], QVariantList{a, b});
            }
            pairs_->setCurrentIndex(selected); connect(pairs_, &QComboBox::currentIndexChanged, this, [this](int Index) { switchPair(Index); });
        }
        change_ = new QSpinBox(this); change_->setObjectName("largeChange"); navigation->addWidget(change_);
        auto* jump = new QPushButton(tr("차이로 이동"), this); navigation->addWidget(jump); buttons_.append(jump);
        connect(jump, &QPushButton::clicked, this, [this] {
            const auto navigate = [this] { if (value_->changes_.empty()) return; const auto& edit = value_->changes_[std::size_t(std::min<int>(change_->value(), int(value_->changes_.size())) - 1)]; pageStart_ = {}; lines_[0]->setValue(int(edit.begin_) + 1); lines_[1]->setValue(int(edit.otherBegin_) + 1); load(); };
            if (!Busy() && !deferPages(navigate)) navigate();
        });
        auto* tools = new QHBoxLayout(); box->addLayout(tools);
        const auto tool = [&](const QString& Text, const QString& Name, auto Callback) { auto* b = new QPushButton(Text, this); b->setObjectName(Name); tools->addWidget(b); buttons_.append(b); connect(b, &QPushButton::clicked, this, Callback); };
        tool(tr("작업 저장…"), "largeSaveWork", [this] { const auto path = QFileDialog::getSaveFileName(this, tr("대용량 작업 저장"), {}, "HyperCompare Large Work (*.hclarge)"); if (!path.isEmpty()) SaveWork(path, true); });
        tool(tr("작업 복구…"), "largeLoadWork", [this] { const auto path = QFileDialog::getOpenFileName(this, tr("대용량 작업 복구"), {}, "HyperCompare Large Work (*.hclarge)"); if (!path.isEmpty()) LoadWork(path); });
        if (merging()) tool(tr("이전 RESULT 복구…"), "largeRecoverResult", [this] { const auto path = QFileDialog::getOpenFileName(this, tr("변경된 원본의 RESULT 복구"), {}, "HyperCompare Large Work (*.hclarge)"); if (!path.isEmpty()) RecoverWork(path); });
        if (merging()) tool(tr("RESULT 다른 이름으로…"), "largeSaveResultAs", [this] { save(1, true); });
        tool(tr("보고서…"), "largeExportReport", [this] { const auto path = QFileDialog::getSaveFileName(this, tr("비교 보고서"), {}, "JSON (*.json)"); if (!path.isEmpty()) ExportReport(path, true); });
        searchScope_ = new QComboBox(this); searchScope_->setObjectName("largeSearchScope"); searchScope_->addItems({tr("왼쪽 전체 파일"), tr("오른쪽 전체 파일")}); tools->addWidget(searchScope_);
        query_ = new QLineEdit(this); query_->setObjectName("largeSearchQuery"); query_->setPlaceholderText(tr("전체 파일에서 텍스트 검색")); query_->setMaxLength(4096); tools->addWidget(query_, 1);
        searchCase_ = new QCheckBox(tr("대소문자"), this); searchCase_->setObjectName("largeSearchCase"); tools->addWidget(searchCase_);
        searchRegex_ = new QCheckBox(tr("정규식"), this); searchRegex_->setObjectName("largeSearchRegex"); tools->addWidget(searchRegex_);
        searchRegex_->setToolTip(tr("전체 파일 정규식 검색: 여러 줄·Unicode 지원, 실행 시간과 메모리 제한 적용"));
        tool(tr("이전 찾기"), "largeFindPrevious", [this] { find(true); }); tool(tr("다음 찾기"), "largeFindNext", [this] { find(false); });
        connect(query_, &QLineEdit::returnPressed, this, [this] { find(false); });
        auto* findKey = new QShortcut(QKeySequence::Find, this); connect(findKey, &QShortcut::activated, this, [this] { query_->setFocus(); query_->selectAll(); });
        auto* nextKey = new QShortcut(QKeySequence::FindNext, this); connect(nextKey, &QShortcut::activated, this, [this] { find(false); });
        auto* previousKey = new QShortcut(QKeySequence::FindPrevious, this); connect(previousKey, &QShortcut::activated, this, [this] { find(true); });
        auto* panes = new QHBoxLayout(); panes_ = panes; box->addLayout(panes, 1);
        for (int side = 0; side < 2; ++side) {
            auto* paneWidget = new QWidget(this); paneWidgets_[side] = paneWidget; auto* pane = new QVBoxLayout(paneWidget); pane->setContentsMargins(0, 0, 0, 0); panes->addWidget(paneWidget, 1);
            auto* label = new QLabel(value_->documents_[side].original_.path_, this); titles_[side] = label; label->setTextFormat(Qt::PlainText); label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); pane->addWidget(label);
            auto* row = new QHBoxLayout(); pane->addLayout(row); row->addWidget(new QLabel(tr("시작 줄"), this));
            lines_[side] = new QSpinBox(this); lines_[side]->setObjectName(side ? "largeLineRight" : "largeLineLeft"); row->addWidget(lines_[side]);
            auto* previous = new QPushButton(tr("이전"), this); auto* next = new QPushButton(tr("다음"), this); auto* go = new QPushButton(tr("이동"), this);
            for (auto* b : {previous, next, go}) { row->addWidget(b); buttons_.append(b); }
            previous->setObjectName(side ? "largePreviousRight" : "largePreviousLeft"); next->setObjectName(side ? "largeNextRight" : "largeNextLeft");
            connect(previous, &QPushButton::clicked, this, [this, side] {
                const auto navigate = [this, side] {
                const auto& offsets = value_->lines_[side]; const auto at = std::upper_bound(offsets.begin(), offsets.end(), begin_[side]) - offsets.begin() - 1;
                if (begin_[side] > offsets[at]) pageStart_[side] = std::max(offsets[at], begin_[side] > largeTextPageMaximum ? begin_[side] - largeTextPageMaximum : 0);
                else { pageStart_[side].reset(); lines_[side]->setValue(std::max(1, int(at) - 399)); } load();
                }; if (!Busy() && !deferPages(navigate)) navigate();
            });
            connect(next, &QPushButton::clicked, this, [this, side] { const auto navigate = [this, side] { pageStart_[side] = end_[side]; load(); }; if (!Busy() && !deferPages(navigate)) navigate(); });
            connect(go, &QPushButton::clicked, this, [this] { const auto requested = std::array{lines_[0]->value(), lines_[1]->value()}; const auto navigate = [this, requested] { pageStart_ = {}; for (int side = 0; side < 2; ++side) lines_[side]->setValue(requested[side]); load(); }; if (!Busy() && !deferPages(navigate)) navigate(); });
            auto* editor = new BoundedEditor(this, input_.memoryMiB_, largeTextPageMaximum); editors_[side] = editor;
            editor->installEventFilter(this);
            editor->setObjectName(side ? "largeTextRight" : "largeTextLeft"); pane->addWidget(editor, 1); editor->send(SCI_SETCODEPAGE, SC_CP_UTF8); editor->EnableProtection(8 * 1024 * 1024);
            editor->blocked_ = [this](const QString& Reason) { status_->setText(Reason); };
            connect(editor, &ScintillaEditBase::modified, this, [this, side](Scintilla::ModificationFlags Flags) {
                if (!loading_ && (int(Flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) { pageDirty_[side] = true; applyTimer_->start(); controls(); if (stateChanged_) stateChanged_(); }
            });
        }
        gutter_ = new ChunkGutter(editors_[0], editors_[1], merging() ? 0 : -1, this); gutter_->setObjectName("largeChunkGutter"); panes->insertWidget(1, gutter_);
        gutter_->choose_ = [this](std::uint64_t Id, std::uint64_t Revision, Resolution Choice) { chooseChunk(Id, Revision, Choice); };
        if (merging()) {
            auto* row = new QHBoxLayout(); box->addLayout(row); row->addWidget(new QLabel(tr("충돌"), this)); conflict_ = new QSpinBox(this); conflict_->setObjectName("largeConflict"); row->addWidget(conflict_);
            const auto resolveButton = [&](const QString& Text, const QString& Name, Resolution Choice) { auto* b = new QPushButton(Text, this); b->setObjectName(Name); row->addWidget(b); buttons_.append(b); connect(b, &QPushButton::clicked, this, [this, Choice] { resolve(Choice); }); };
            resolveButton("LEFT", "largeResolveLeft", Resolution::Left); resolveButton("RIGHT", "largeResolveRight", Resolution::Right);
            if (!input_.base_.isEmpty()) resolveButton("BASE", "largeResolveBase", Resolution::Base);
            resolveButton(tr("제거"), "largeResolveOmit", Resolution::Omit); resolveButton("LEFT + RIGHT", "largeResolveBoth", Resolution::LeftThenRight);
            auto* complete = new QPushButton(tr("전체 수동 검토 완료"), this); complete->setObjectName("largeCompleteReview"); row->addWidget(complete); buttons_.append(complete); connect(complete, &QPushButton::clicked, this, [this] { completeReview(); });
        }
        status_ = new QLabel(this); status_->setObjectName("largeStatus"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); box->addWidget(status_); controls(); load();
    }
    LargeTextView::~LargeTextView() { if (canceled_) canceled_->store(true); watcher_.waitForFinished(); }
    bool LargeTextView::eventFilter(QObject* Object, QEvent* Event)
    { if (Event->type() == QEvent::FocusIn) for (int side = 0; side < 2; ++side) if (Object == editors_[side]) activeSide_ = side; return ExtensionView::eventFilter(Object, Event); }
    void LargeTextView::SaveActive() { save(merging() ? 1 : activeSide_); }
    bool LargeTextView::CanUndo(bool Redo) const
    { if (Busy()) return false; return pageDirty_[0] || pageDirty_[1] ? editors_[activeSide_]->send(Redo ? SCI_CANREDO : SCI_CANUNDO) : !(Redo ? redo_ : undo_).empty(); }
    void LargeTextView::Undo(bool Redo)
    { if (Busy()) return; if (pageDirty_[0] || pageDirty_[1]) editors_[activeSide_]->send(Redo ? SCI_REDO : SCI_UNDO); else undo(Redo); }
    bool LargeTextView::Dirty() const {
        if (pageDirty_[0] || pageDirty_[1] || resultNeverSaved_) return true;
        if (merging()) return value_->session_->reviewRequired_ || LargeTextUnresolved(*value_) || !same(value_->session_->documents_[value_->session_->result_], savedAll_[value_->session_->result_]);
        if (value_->session_) { for (std::size_t i = 0; i < savedAll_.size(); ++i) if (!same(value_->session_->documents_[i], savedAll_[i])) return true; return false; }
        return !same(value_->documents_[0], saved_[0]) || !same(value_->documents_[1], saved_[1]);
    }
    int LargeTextView::ExitCode() const {
        if (merging()) return Dirty() ? 3 : 0;
        if (input_.textEquality_ == "bytes") { if (Dirty()) return 1; for (std::size_t i = 1; i < savedAll_.size(); ++i) if (savedAll_[0].rawHash_ != savedAll_[i].rawHash_) return 1; return savedAll_.empty() && saved_[0].rawHash_ != saved_[1].rawHash_ ? 1 : 0; }
        if (value_->session_) { for (const auto& pair : value_->session_->comparisons_) if (pair.toObject()["changeCount"].toDouble()) return 1; return 0; }
        return value_->changes_.empty() ? 0 : 1;
    }
    void ApplyLargeTextEditorAppearance(ScintillaEditBase* Editor, const QFont& Font, bool Dark, int LexerId)
    {
        const auto color = [](QColor Value) { return Value.red() | (Value.green() << 8) | (Value.blue() << 16); };
        // Surface, text, caret, gutter and the changed-line marker follow the product tokens; syntax hues stay per light/dark.
        const auto& Tokens = fm::style::ThemeManager::instance().colors();
            const auto name = Font.family().toUtf8(); Editor->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(name.constData()));
            Editor->send(SCI_STYLESETSIZE, STYLE_DEFAULT, std::max(1, Font.pointSize())); Editor->send(SCI_STYLESETFORE, STYLE_DEFAULT, color(Tokens[fm::style::Token::Fg]));
            Editor->send(SCI_STYLESETBACK, STYLE_DEFAULT, color(Tokens[fm::style::Token::Surface])); Editor->send(SCI_STYLECLEARALL);
            Editor->send(SCI_SETCARETFORE, color(Tokens[fm::style::Token::Fg]));
            Editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_RTEXT); Editor->send(SCI_SETMARGINWIDTHN, 0, 60); Editor->send(SCI_SETMARGINWIDTHN, 1, 0);
            Editor->send(SCI_STYLESETBACK, STYLE_LINENUMBER, color(Tokens[fm::style::Token::Alt]));
            Editor->send(SCI_MARKERDEFINE, 8, SC_MARK_BACKGROUND); Editor->send(SCI_MARKERSETBACK, 8, color(Tokens[fm::style::Token::WarnBg]));
            const auto paint = [&](std::initializer_list<int> Styles, const QColor& LightColor, const QColor& DarkColor) { for (const auto style : Styles) Editor->send(SCI_STYLESETFORE, style, color(Dark ? DarkColor : LightColor)); };
            switch (LexerId) {
            case SCLEX_CPP:
                paint({SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC}, QColor("#51774c"), QColor("#90b58b"));
                paint({SCE_C_WORD, SCE_C_WORD2}, QColor("#803da1"), QColor("#cb9fda")); paint({SCE_C_STRING, SCE_C_CHARACTER, SCE_C_VERBATIM, SCE_C_STRINGRAW}, QColor("#9b5f28"), QColor("#d7b080"));
                paint({SCE_C_NUMBER}, QColor("#3769a0"), QColor("#92bfdd")); break;
            case SCLEX_PYTHON:
                paint({SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK}, QColor("#51774c"), QColor("#90b58b")); paint({SCE_P_WORD, SCE_P_WORD2}, QColor("#803da1"), QColor("#cb9fda"));
                paint({SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE}, QColor("#9b5f28"), QColor("#d7b080")); paint({SCE_P_DEFNAME, SCE_P_CLASSNAME}, QColor("#3769a0"), QColor("#92bfdd")); break;
            case SCLEX_JSON:
                paint({SCE_JSON_STRING}, QColor("#9b5f28"), QColor("#d7b080")); paint({SCE_JSON_PROPERTYNAME}, QColor("#3769a0"), QColor("#92bfdd"));
                paint({SCE_JSON_KEYWORD}, QColor("#803da1"), QColor("#cb9fda")); paint({SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT}, QColor("#51774c"), QColor("#90b58b")); break;
            case SCLEX_XML:
                paint({SCE_H_TAG, SCE_H_TAGEND}, QColor("#3769a0"), QColor("#92bfdd")); paint({SCE_H_COMMENT}, QColor("#51774c"), QColor("#90b58b"));
                paint({SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_CDATA}, QColor("#9b5f28"), QColor("#d7b080")); break;
            }
    }
    void LargeTextView::SetTextFont(const QFont& Font)
    {
        textFont_ = Font; for (int side = 0; side < 2; ++side) ApplyLargeTextEditorAppearance(editors_[side], Font, dark_, lexerIds_[side]);
    }
    void LargeTextView::controls()
    {
        for (auto* b : buttons_) b->setEnabled(!Busy());
        for (int side = 0; side < 2; ++side) {
            const bool source = merging() && value_->session_->pair_[side] != value_->session_->result_;
            lines_[side]->setRange(1, std::max(1, int(value_->lines_[side].size()))); lines_[side]->setEnabled(!Busy()); editors_[side]->send(SCI_SETREADONLY, Busy() || source || displayReadOnly_);
            titles_[side]->setText((value_->session_ ? value_->session_->titles_[value_->session_->pair_[side]] + " — " : QString{}) + value_->documents_[side].original_.path_ + "\n" + value_->documents_[side].original_.encoding_ + (value_->documents_[side].original_.bom_ ? tr(" · BOM 있음") : tr(" · BOM 없음")));
            findChild<QPushButton*>(side ? "largeSaveRight" : "largeSaveLeft")->setEnabled(!Busy() && !source && !displayReadOnly_);
            findChild<QPushButton*>(side ? "largeMergeRight" : "largeMergeLeft")->setEnabled(!Busy() && !source && !displayReadOnly_);
        }
        if (pairs_) pairs_->setEnabled(!Busy());
        if (pairs_ && value_->session_) { const QSignalBlocker blocker(pairs_); for (int i = 0; i < pairs_->count(); ++i) { const auto data = pairs_->itemData(i).toList(); if (value_->session_->pair_ == std::array{data[0].toInt(), data[1].toInt()}) pairs_->setCurrentIndex(i); } }
        if (conflict_) { conflict_->setRange(1, std::max(1, int(value_->session_->conflicts_.size()))); conflict_->setEnabled(!Busy()); }
        change_->setRange(1, std::max(1, int(value_->changes_.size()))); change_->setEnabled(!Busy());
        if (pageDirty_[0] || pageDirty_[1]) status_->setText(tr("편집 내용은 잠시 후 자동 반영됩니다. 탐색·저장 시 먼저 반영하며 원본 쓰기는 저장할 때 수행합니다."));
        if (gutter_) { gutter_->setEnabled(!Busy() && !pageDirty_[0] && !pageDirty_[1]); if (pageDirty_[0] || pageDirty_[1]) gutter_->SetChunks({}, ++generation_); }
        if (query_) { query_->setEnabled(!Busy()); searchScope_->setEnabled(!Busy()); searchCase_->setEnabled(!Busy()); searchRegex_->setEnabled(!Busy()); }
    }
    void LargeTextView::run(std::function<Task(const Budget&)> Work, bool Mutation, int Saved)
    {
        if (Busy()) return; busy_ = true; canceled_ = std::make_shared<std::atomic_bool>(false);
        const auto starts = std::array{lines_[0]->value() - 1, lines_[1]->value() - 1};
        disconnect(&watcher_, nullptr, this, nullptr);
        connect(&watcher_, &QFutureWatcher<Response>::finished, this, [this, Mutation, Saved] {
            const auto response = watcher_.result(); busy_ = false;
            auto pending = std::move(pendingAction_); pendingAction_ = {};
            const auto position = std::exchange(pendingPosition_, std::nullopt);
            // A completed disk commit is not rolled back by a later page-load or
            // editor admission failure. Keep its new expected revision.
            if (Saved >= 0 && response.comparison_) { value_ = response.comparison_; saved_[Saved] = value_->documents_[Saved]; if (value_->session_) { const auto index = value_->session_->pair_[Saved]; savedAll_[index] = value_->documents_[Saved]; if (index == value_->session_->result_) resultNeverSaved_ = false; } }
            if (!response.error_.isEmpty()) { controls(); status_->setText(response.error_ + (response.recovery_.isEmpty() ? QString{} : tr(" · 복구 기록: ") + response.recovery_.first().toString())); if (stateChanged_) stateChanged_(); return; }
            if (response.match_ && !response.match_->found_) { lastMatch_ = {}; controls(); status_->setText(tr("전체 파일에서 일치하는 텍스트가 없습니다.")); if (stateChanged_) stateChanged_(); return; }
            std::vector<std::shared_ptr<void>> admissions;
            // Page replacement discards native page history; document Undo lives
            // in immutable disk revisions. Reserve storage without growing Undo.
            try { for (int side = 0; side < 2; ++side) admissions.push_back(static_cast<BoundedEditor*>(editors_[side])->BeginEdit(response.text_[side].size(), editors_[side]->send(SCI_GETLENGTH), true, false, true)); }
            catch (const std::exception& error) { controls(); status_->setText(QString::fromUtf8(error.what())); if (stateChanged_) stateChanged_(); return; }
            if (Mutation) { undo_.push_back(value_); if (undo_.size() > 32) undo_.erase(undo_.begin()); redo_.clear(); }
            if (Mutation || Saved == -2 || Saved == -3) { lastQuery_.clear(); lastMatch_ = {}; }
            if (Saved == -2) { undo_.pop_back(); redo_.push_back(value_); }
            if (Saved == -3) { redo_.pop_back(); undo_.push_back(value_); }
            value_ = response.comparison_; if (Saved >= 0) saved_[Saved] = value_->documents_[Saved];
            begin_ = response.begin_; end_ = response.end_; loading_ = true;
            for (int side = 0; side < 2; ++side) {
                auto* editor = editors_[side]; editor->send(SCI_SETREADONLY, 0); editor->send(SCI_CLEARALL);
                editor->send(SCI_ADDTEXT, response.text_[side].size(), reinterpret_cast<sptr_t>(response.text_[side].constData())); editor->send(SCI_EMPTYUNDOBUFFER);
                auto path = value_->documents_[side].original_.path_; if (merging() && value_->session_->pair_[side] == value_->session_->result_) path = value_->session_->documents_[0].original_.path_;
                auto* descriptor = CreateLexer(lexer(path)); lexerIds_[side] = descriptor ? descriptor->GetIdentifier() : 0; if (descriptor) descriptor->Release();
                editor->send(SCI_SETILEXER, 0, 0);
                editor->send(SCI_STARTSTYLING, 0); editor->send(SCI_SETSTYLINGEX, response.styles_[side].size(), reinterpret_cast<sptr_t>(response.styles_[side].constData()));
                syntax_[side] = response.syntax_[side];
                const auto& offsets = value_->lines_[side]; const auto first = std::size_t(std::upper_bound(offsets.begin(), offsets.end(), begin_[side]) - offsets.begin() - 1);
                const auto last = std::size_t(std::lower_bound(offsets.begin(), offsets.end(), end_[side]) - offsets.begin());
                editor->send(SCI_MARGINTEXTCLEARALL);
                for (sptr_t row = 0; row < editor->send(SCI_GETLINECOUNT); ++row) {
                    const auto number = QByteArray::number(qulonglong(first + row + 1));
                    editor->send(SCI_MARGINSETTEXT, row, reinterpret_cast<sptr_t>(number.constData())); editor->send(SCI_MARGINSETSTYLE, row, STYLE_LINENUMBER);
                }
                editor->send(SCI_MARKERDELETEALL, 8);
                auto at = std::lower_bound(value_->changes_.begin(), value_->changes_.end(), first, [side](const Edit& Edit, std::size_t Line) { return (side ? Edit.otherEnd_ : Edit.end_) < Line; });
                for (; at != value_->changes_.end(); ++at) {
                    const auto a = side ? at->otherBegin_ : at->begin_, b = side ? at->otherEnd_ : at->end_;
                    if (a > last) break;
                    for (auto row = std::max(a, first); row < std::min(b, last); ++row) editor->send(SCI_MARKERADD, row - first, 8);
                }
                pageStart_[side] = begin_[side]; lines_[side]->setRange(1, std::max(1, int(offsets.size()))); lines_[side]->setValue(int(first) + 1);
                pageDirty_[side] = false;
                if (position) { const auto size = editor->send(SCI_GETLENGTH); editor->send(SCI_SETSEL, std::min((*position)[side].anchor_, size), std::min((*position)[side].current_, size)); editor->send(SCI_SETFIRSTVISIBLELINE, (*position)[side].first_); }
            }
            loading_ = false; SetTextFont(textFont_); controls(); status_->setText(tr("전체 파일 비교: %1개 차이 · 최대 400줄/1 MiB씩 표시 · 앞 페이지 구문 문맥 유지 · 저장 전 원본 보존").arg(value_->changes_.size()));
            if (merging()) status_->setText(tr("미해결 충돌: %1 · %2 · ").arg(LargeTextUnresolved(*value_)).arg(value_->session_->reviewRequired_ ? tr("RESULT 전체 수동 검토 필요") : tr("충돌 선택 가능")) + status_->text());
            if (!value_->saveWarnings_.isEmpty() || !value_->saveRecovery_.isEmpty()) status_->setText(status_->text() + tr(" · 저장 확인: ") + QString::fromUtf8(QJsonDocument(QJsonObject{{"warnings", value_->saveWarnings_}, {"recovery", value_->saveRecovery_}}).toJson(QJsonDocument::Compact)));
            if (response.match_) {
                if (response.match_->found_) { lastMatch_ = *response.match_; const auto side = response.matchSide_; editors_[side]->send(SCI_SETSEL, response.match_->begin_ - begin_[side], std::min(response.match_->end_, end_[side]) - begin_[side]); editors_[side]->send(SCI_SCROLLCARET); status_->setText(response.match_->wrapped_ ? tr("문서 끝에서 처음으로 돌아가 찾았습니다.") : tr("전체 파일에서 찾았습니다.")); }
                else { lastMatch_ = {}; status_->setText(tr("전체 파일에서 일치하는 텍스트가 없습니다.")); }
                if (response.match_->found_ && response.match_->end_ > end_[response.matchSide_]) status_->setText(status_->text() + tr(" · 일치 범위가 페이지를 넘어 현재 표시 부분만 선택했습니다."));
            }
            if (!response.note_.isEmpty()) status_->setText(response.note_ + " · " + status_->text()); updateGutter();
            deferredContinuation_ = bool(pending);
            if (stateChanged_) stateChanged_();
            if (pending) QTimer::singleShot(0, this, [this, pending = std::move(pending)] { deferredContinuation_ = false; pending(); });
        });
        watcher_.setFuture(QtConcurrent::run([work = std::move(Work), starts, pageStart = pageStart_, syntax = syntax_, canceled = canceled_, memory = input_.memoryMiB_] {
            Response result; Budget limits; limits.canceled_ = canceled.get(); limits.deadline_ = std::chrono::steady_clock::time_point::max();
            try {
                ConfigureMemoryBudget(limits, memory);
                const auto task = work(limits); result.comparison_ = task.comparison_; result.match_ = task.match_; result.matchSide_ = task.side_; result.note_ = task.note_;
                result.syntax_ = syntax;
                for (int side = 0; side < 2; ++side) {
                    const auto& document = result.comparison_->documents_[side]; const auto& lines = result.comparison_->lines_[side];
                    auto begin = task.match_ && task.match_->found_ && task.side_ == side ? task.match_->begin_ : pageStart[side] ? std::min(*pageStart[side], document.bytes_) : lines[std::min<std::size_t>(starts[side], lines.size() - 1)];
                    const auto continuation = [&](std::uint64_t At) { return At < document.bytes_ && (uchar(ReadLargeTextRange(document, At, At + 1, limits)[0]) & 0xc0) == 0x80; };
                    while (begin && continuation(begin)) --begin;
                    const auto start = std::size_t(std::upper_bound(lines.begin(), lines.end(), begin) - lines.begin() - 1);
                    auto lineEnd = std::min(start + 400, lines.size() - 1);
                    while (lineEnd > start && lines[lineEnd] - begin > largeTextPageMaximum) --lineEnd;
                    auto end = lineEnd > start ? lines[lineEnd] : std::min(begin + largeTextPageMaximum, document.bytes_);
                    while (end > begin && continuation(end)) --end;
                    result.begin_[side] = begin; result.end_[side] = end;
                    result.text_[side] = ReadLargeTextRange(document, begin, end, limits);
                    auto path = document.original_.path_;
                    if (result.comparison_->session_ && result.comparison_->session_->result_ == result.comparison_->session_->pair_[side]) path = result.comparison_->session_->documents_[0].original_.path_;
                    result.styles_[side] = StyleLargeText(document, lexer(path), begin, end, result.syntax_[side], limits);
                }
            } catch (const SaveRecoveryFailure& error) { result.error_ = QString::fromUtf8(error.what()); result.recovery_ = error.recovery_; }
            catch (const std::exception& error) { result.error_ = QString::fromUtf8(error.what()); }
            return result;
        })); controls(); status_->setText(tr("전체 파일 작업 중… 취소할 수 있습니다.")); if (stateChanged_) stateChanged_();
    }
    void LargeTextView::load(bool Mutation, std::shared_ptr<LargeTextComparison> Value)
    { run([value = Value ? Value : value_](const Budget&) { return value; }, Mutation); }
    void LargeTextView::applyPages()
    {
        if (Busy() || (!pageDirty_[0] && !pageDirty_[1])) return;
        applyTimer_->stop();
        std::array<QByteArray, 2> text;
        try {
            const auto a = static_cast<BoundedEditor*>(editors_[0])->ReserveSnapshot(), b = static_cast<BoundedEditor*>(editors_[1])->ReserveSnapshot();
            text = {contents(editors_[0]), contents(editors_[1])};
        } catch (const std::exception& error) { pendingAction_ = {}; status_->setText(QString::fromUtf8(error.what())); return; }
        std::array<EditorPosition, 2> positions; for (int side = 0; side < 2; ++side) positions[side] = {editors_[side]->send(SCI_GETANCHOR), editors_[side]->send(SCI_GETCURRENTPOS), editors_[side]->send(SCI_GETFIRSTVISIBLELINE)};
        pendingPosition_ = positions;
        run([value = value_, text, begin = begin_, end = end_, changed = pageDirty_](const Budget& Limits) {
            auto documents = value->documents_; for (int side = 0; side < 2; ++side) if (changed[side]) documents[side] = EditLargeText(documents[side], begin[side], end[side], text[side], Limits);
            return UpdateLargeTextSession(*value, std::move(documents), Limits);
        }, true);
    }
    bool LargeTextView::deferPages(std::function<void()> Action)
    {
        if (!pageDirty_[0] && !pageDirty_[1]) return false;
        pendingAction_ = std::move(Action); applyPages(); return true;
    }
    void LargeTextView::merge(int Source)
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1] || value_->changes_.empty() || (merging() && value_->session_->pair_[1 - Source] != value_->session_->result_)) return;
        if (editors_[Source]->send(SCI_GETSELECTIONS) != 1 || editors_[Source]->send(SCI_SELECTIONISRECTANGLE)) { status_->setText(tr("범위 병합은 연속된 텍스트 선택을 사용하십시오.")); return; }
        const auto change = std::size_t(change_->value() - 1); const int target = 1 - Source;
        const auto a = editors_[Source]->send(SCI_GETSELECTIONSTART), b = editors_[Source]->send(SCI_GETSELECTIONEND);
        std::optional<std::pair<std::uint64_t, std::uint64_t>> selection; if (a != b) selection = std::pair{begin_[Source] + a, begin_[Source] + b};
        run([value = value_, Source, target, change, selection](const Budget& Limits) {
            auto documents = value->documents_; documents[target] = MergeLargeTextChange(*value, Source, change, selection, Limits);
            return UpdateLargeTextSession(*value, std::move(documents), Limits);
        }, true);
    }
    void LargeTextView::save(int Side, bool ChoosePath)
    {
        if (!Busy() && deferPages([this, Side, ChoosePath] { save(Side, ChoosePath); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        if (merging() && (LargeTextUnresolved(*value_) || value_->session_->reviewRequired_)) { status_->setText(tr("미해결 충돌을 선택하거나 RESULT 전체를 검토 완료한 뒤 저장하십시오.")); return; }
        const auto original = value_->session_ ? (merging() && resultNeverSaved_ ? value_->documents_[Side].original_ : savedAll_[value_->session_->pair_[Side]].original_) : saved_[Side].original_;
        QString output;
        if (merging() && value_->session_->pair_[Side] != value_->session_->result_) return;
        if (merging() && (ChoosePath || original.path_.isEmpty() || (resultNeverSaved_ && original.exists_ && !input_.overwrite_))) {
            output = QFileDialog::getSaveFileName(this, tr("대용량 RESULT 저장"), original.path_); if (output.isEmpty()) return;
        }
        run([value = value_, input = input_, Side, original, output](const Budget& Limits) {
            const auto bytes = (value->lines_[0].size() + value->lines_[1].size()) * sizeof(std::uint64_t) + value->changes_.size() * sizeof(Edit);
            const auto reservation = Limits.memoryReserve_ ? Limits.memoryReserve_(bytes) : std::shared_ptr<void>{};
            auto next = output.isEmpty() ? std::make_shared<LargeTextComparison>(*value) : BindLargeTextOutput(*value, output, true, input, Limits);
            auto document = next->documents_[Side]; if (output.isEmpty()) document.original_ = original;
            auto protectedInput = input;
            if (next->session_ && next->session_->result_ >= 0) { ValidateLargeTextInputs(*next->session_, Limits); for (int i = 0; i < next->session_->result_; ++i) protectedInput.protectedInputs_.append(next->session_->documents_[i].original_.path_); }
            auto session = next->session_ ? std::make_shared<LargeTextSession>(*next->session_) : std::shared_ptr<LargeTextSession>{};
            const auto saved = SaveLargeText(protectedInput, document, Limits); next->documents_[Side].original_ = saved.snapshot_;
            next->documents_[Side].rawHash_ = saved.rawHash_;
            if (session) { session->documents_[session->pair_[Side]] = next->documents_[Side]; next->session_ = std::move(session); }
            next->saveWarnings_ = saved.warnings_; next->saveRecovery_ = saved.recovery_; return next;
        }, false, Side);
    }
    void LargeTextView::undo(bool Redo)
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        auto& from = Redo ? redo_ : undo_; if (from.empty()) return;
        run([next = from.back()](const Budget&) { return next; }, false, Redo ? -3 : -2);
    }
    void LargeTextView::switchPair(int Index)
    {
        if (!Busy() && deferPages([this, Index] { switchPair(Index); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1] || !value_->session_) return;
        const auto pair = pairs_->itemData(Index).toList(); if (pair.size() != 2) return; const std::array selected{pair[0].toInt(), pair[1].toInt()};
        pageStart_ = {}; lastQuery_.clear(); lastMatch_ = {}; lines_[0]->setValue(1); lines_[1]->setValue(1);
        run([session = *value_->session_, selected](const Budget& Limits) { return PairLargeTextSession(session, selected, Limits); }, false);
    }
    void LargeTextView::resolve(Resolution Choice)
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1] || !merging()) return;
        const auto index = std::size_t(conflict_->value() - 1);
        run([value = value_, index, Choice](const Budget& Limits) { return ResolveLargeTextConflict(*value, index, Choice, Limits); }, true);
    }
    void LargeTextView::completeReview()
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1] || !merging()) return;
        run([value = value_](const Budget& Limits) { auto session = *value->session_; if (session.reviewRequired_) session.conflicts_.clear(); session.reviewRequired_ = false; for (auto& conflict : session.conflicts_) conflict.choice_ = Resolution::Manual; return PairLargeTextSession(std::move(session), value->session_->pair_, Limits); }, true);
    }
    void LargeTextView::find(bool Backwards)
    {
        if (!Busy() && deferPages([this, Backwards] { find(Backwards); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return; const auto query = query_->text(); if (query.isEmpty()) { query_->setFocus(); return; }
        const auto side = searchScope_->currentIndex(), document = value_->session_ ? value_->session_->pair_[side] : side;
        const auto regex = searchRegex_->isChecked(), matchCase = searchCase_->isChecked();
        const bool repeat = lastMatch_.found_ && query == lastQuery_ && document == lastSearchDocument_ && regex == lastRegex_ && matchCase == lastCase_;
        const auto start = repeat ? (Backwards ? lastMatch_.begin_ : lastMatch_.end_) : begin_[side] + editors_[side]->send(Backwards ? SCI_GETSELECTIONSTART : SCI_GETSELECTIONEND);
        const bool emptyRepeat = repeat && lastMatch_.begin_ == lastMatch_.end_;
        lastQuery_ = query; lastSearchDocument_ = document; lastRegex_ = regex; lastCase_ = matchCase; const auto sensitivity = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
        run([value = value_, side, query, start, Backwards, sensitivity, regex, emptyRepeat](const Budget& Limits) {
            auto from = start; const auto& document = value->documents_[side];
            if (emptyRepeat && !Backwards) {
                if (from >= document.bytes_) from = 0;
                else { ++from; while (from < document.bytes_ && (uchar(ReadLargeTextRange(document, from, from + 1, Limits)[0]) & 0xc0) == 0x80) ++from; }
            }
            Task task(value); task.side_ = side; task.match_ = FindLargeText(document, query, from, Backwards, sensitivity, Limits, regex);
            if (emptyRepeat && !Backwards && start == document.bytes_ && task.match_->found_) task.match_->wrapped_ = true; return task;
        }, false);
    }
    void LargeTextView::SaveWork(const QString& Path, bool ReplaceExisting)
    {
        if (!Busy() && deferPages([this, Path, ReplaceExisting] { SaveWork(Path, ReplaceExisting); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        run([value = value_, input = input_, Path, ReplaceExisting](const Budget& Limits) { const auto saved = SaveLargeTextWork(Path, input, *value, ReplaceExisting, Limits); auto next = std::make_shared<LargeTextComparison>(*value); next->saveWarnings_ = saved.warnings_; next->saveRecovery_ = saved.recovery_; Task task(next); task.note_ = tr("작업을 저장했습니다. 원본 파일은 별도 저장 전까지 보존됩니다."); return task; }, false);
    }
    void LargeTextView::LoadWork(const QString& Path)
    {
        if (!Busy() && deferPages([this, Path] { LoadWork(Path); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        run([value = value_, input = input_, Path](const Budget& Limits) { Task task(LoadLargeTextWork(Path, input, *value, Limits)); task.note_ = tr("작업을 복구했습니다. 실행 취소로 이전 작업에 돌아갈 수 있습니다."); return task; }, true);
    }
    void LargeTextView::RecoverWork(const QString& Path)
    {
        if (!Busy() && deferPages([this, Path] { RecoverWork(Path); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1] || !merging()) return;
        run([input = input_, Path](const Budget& Limits) { const auto fresh = OpenLargeTextSession(input, Limits); Task task(LoadLargeTextWork(Path, input, *fresh, Limits, true)); task.note_ = tr("최신 원본에 이전 RESULT를 복구했습니다. 전체 수동 검토 후 저장하십시오."); return task; }, true);
    }
    void LargeTextView::ExportReport(const QString& Path, bool ReplaceExisting)
    {
        if (!Busy() && deferPages([this, Path, ReplaceExisting] { ExportReport(Path, ReplaceExisting); })) return;
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        run([value = value_, input = input_, Path, ReplaceExisting](const Budget& Limits) { const auto saved = SaveLargeTextReport(Path, input, *value, ReplaceExisting, Limits); auto next = std::make_shared<LargeTextComparison>(*value); next->saveWarnings_ = saved.warnings_; next->saveRecovery_ = saved.recovery_; Task task(next); task.note_ = tr("현재 작업 상태의 비교 보고서를 저장했습니다."); return task; }, false);
    }
    void LargeTextView::updateGutter()
    {
        if (!gutter_ || Busy() || pageDirty_[0] || pageDirty_[1]) return; std::set<std::size_t> indexes;
        for (int side = 0; side < 2; ++side) {
            auto at = std::lower_bound(value_->changes_.begin(), value_->changes_.end(), begin_[side], [&](const Edit& Edit, std::uint64_t Begin) { return value_->lines_[side][side ? Edit.otherEnd_ : Edit.end_] < Begin; });
            for (; at != value_->changes_.end() && indexes.size() < 256; ++at) { if (value_->lines_[side][side ? at->otherBegin_ : at->begin_] > end_[side]) break; indexes.insert(std::size_t(at - value_->changes_.begin())); }
        }
        std::vector<GutterChunk> chunks;
        for (const auto index : indexes) { const auto& edit = value_->changes_[index]; GutterChunk chunk; chunk.id_ = index + 1; chunk.enabled_ = true;
            for (int side = 0; side < 2; ++side) { const auto a = value_->lines_[side][side ? edit.otherBegin_ : edit.begin_], b = value_->lines_[side][side ? edit.otherEnd_ : edit.end_]; chunk.begin_[side] = std::clamp(a, begin_[side], end_[side]) - begin_[side]; chunk.end_[side] = std::clamp(b, begin_[side], end_[side]) - begin_[side]; } chunks.push_back(chunk);
        }
        gutter_->SetChunks(std::move(chunks), ++generation_);
    }
    void LargeTextView::chooseChunk(std::uint64_t Id, std::uint64_t Revision, Resolution Choice)
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1] || Revision != generation_ || !Id || Id > value_->changes_.size()) return;
        change_->setValue(int(Id)); if (Choice == Resolution::Left) { merge(0); return; } if (Choice == Resolution::Right) { merge(1); return; }
        if (!merging()) return;
        run([value = value_, index = std::size_t(Id - 1), Choice](const Budget& Limits) { const auto& edit = value->changes_[index]; auto documents = value->documents_; const auto a = value->lines_[1][edit.otherBegin_], b = value->lines_[1][edit.otherEnd_];
            if (Choice == Resolution::Omit) documents[1] = EditLargeText(documents[1], a, b, {}, Limits);
            else if (Choice == Resolution::LeftThenRight || Choice == Resolution::RightThenLeft) {
                auto replacement = ReplaceLargeText({}, 0, 0, documents[0], value->lines_[0][edit.begin_], value->lines_[0][edit.end_], Limits);
                if (Choice == Resolution::LeftThenRight) replacement = ReplaceLargeText(replacement, replacement.bytes_, replacement.bytes_, documents[1], a, b, Limits);
                else replacement = ReplaceLargeText(replacement, 0, 0, documents[1], a, b, Limits);
                documents[1] = ReplaceLargeText(documents[1], a, b, replacement, 0, replacement.bytes_, Limits);
            }
            return UpdateLargeTextSession(*value, std::move(documents), Limits);
        }, true);
    }
}
