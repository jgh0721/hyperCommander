#include "stdafx.h"
#include "largeTextView.hpp"
#include "boundedEditor.hpp"
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
    }
    LargeTextView::LargeTextView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent)
        : ExtensionView(Parent), input_(std::move(Input)), value_(std::move(Value)), saved_(value_->documents_)
    {
        setObjectName("largeTextView"); auto* box = new QVBoxLayout(this); auto* actions = new QHBoxLayout(); box->addLayout(actions);
        const auto button = [&](const QString& Text, const QString& Name, auto Callback) {
            auto* b = new QPushButton(Text, this); b->setObjectName(Name); actions->addWidget(b); buttons_.append(b); connect(b, &QPushButton::clicked, this, Callback);
        };
        button(tr("페이지 편집 적용"), "largeApplyPages", [this] { applyPages(); });
        button(tr("왼쪽 → 오른쪽"), "largeMergeRight", [this] { merge(0); });
        button(tr("왼쪽 ← 오른쪽"), "largeMergeLeft", [this] { merge(1); });
        button(tr("실행 취소"), "largeUndo", [this] { undo(false); }); button(tr("다시 실행"), "largeRedo", [this] { undo(true); });
        button(tr("왼쪽 저장"), "largeSaveLeft", [this] { save(0); }); button(tr("오른쪽 저장"), "largeSaveRight", [this] { save(1); });
        auto* cancel = new QPushButton(tr("취소"), this); actions->addWidget(cancel); connect(cancel, &QPushButton::clicked, this, [this] { if (canceled_) canceled_->store(true); });
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
            if (Busy() || pageDirty_[0] || pageDirty_[1] || value_->changes_.empty()) return;
            const auto& edit = value_->changes_[std::size_t(change_->value() - 1)];
            pageStart_ = {}; lines_[0]->setValue(int(edit.begin_) + 1); lines_[1]->setValue(int(edit.otherBegin_) + 1); load();
        });
        auto* panes = new QHBoxLayout(); box->addLayout(panes, 1);
        for (int side = 0; side < 2; ++side) {
            auto* pane = new QVBoxLayout(); panes->addLayout(pane, 1);
            auto* label = new QLabel(value_->documents_[side].original_.path_, this); titles_[side] = label; label->setTextFormat(Qt::PlainText); pane->addWidget(label);
            auto* row = new QHBoxLayout(); pane->addLayout(row); row->addWidget(new QLabel(tr("시작 줄"), this));
            lines_[side] = new QSpinBox(this); lines_[side]->setObjectName(side ? "largeLineRight" : "largeLineLeft"); row->addWidget(lines_[side]);
            auto* previous = new QPushButton(tr("이전"), this); auto* next = new QPushButton(tr("다음"), this); auto* go = new QPushButton(tr("이동"), this);
            for (auto* b : {previous, next, go}) { row->addWidget(b); buttons_.append(b); }
            previous->setObjectName(side ? "largePreviousRight" : "largePreviousLeft"); next->setObjectName(side ? "largeNextRight" : "largeNextLeft");
            connect(previous, &QPushButton::clicked, this, [this, side] {
                if (pageDirty_[0] || pageDirty_[1]) return;
                const auto& offsets = value_->lines_[side]; const auto at = std::upper_bound(offsets.begin(), offsets.end(), begin_[side]) - offsets.begin() - 1;
                if (begin_[side] > offsets[at]) pageStart_[side] = std::max(offsets[at], begin_[side] > largeTextPageMaximum ? begin_[side] - largeTextPageMaximum : 0);
                else { pageStart_[side].reset(); lines_[side]->setValue(std::max(1, int(at) - 399)); } load();
            });
            connect(next, &QPushButton::clicked, this, [this, side] { if (pageDirty_[0] || pageDirty_[1]) return; pageStart_[side] = end_[side]; load(); });
            connect(go, &QPushButton::clicked, this, [this] { if (!pageDirty_[0] && !pageDirty_[1]) { pageStart_ = {}; load(); } });
            auto* editor = new BoundedEditor(this, input_.memoryMiB_, largeTextPageMaximum); editors_[side] = editor;
            editor->setObjectName(side ? "largeTextRight" : "largeTextLeft"); pane->addWidget(editor, 1); editor->send(SCI_SETCODEPAGE, SC_CP_UTF8); editor->EnableProtection(8 * 1024 * 1024);
            editor->blocked_ = [this](const QString& Reason) { status_->setText(Reason); };
            connect(editor, &ScintillaEditBase::modified, this, [this, side](Scintilla::ModificationFlags Flags) {
                if (!loading_ && (int(Flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) { pageDirty_[side] = true; controls(); if (stateChanged_) stateChanged_(); }
            });
        }
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
    void LargeTextView::SetTextFont(const QFont& Font)
    {
        const auto color = [](QColor Value) { return Value.red() | (Value.green() << 8) | (Value.blue() << 16); };
        for (auto* editor : editors_) {
            const auto name = Font.family().toUtf8(); editor->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>(name.constData()));
            editor->send(SCI_STYLESETSIZE, STYLE_DEFAULT, std::max(1, Font.pointSize())); editor->send(SCI_STYLESETFORE, STYLE_DEFAULT, color(dark_ ? QColor("#e5e5e5") : QColor("#142236")));
            editor->send(SCI_STYLESETBACK, STYLE_DEFAULT, color(dark_ ? QColor("#202020") : QColor("#fafbfc"))); editor->send(SCI_STYLECLEARALL);
            editor->send(SCI_SETCARETFORE, color(dark_ ? QColor("#ffffff") : QColor("#142236")));
            editor->send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_RTEXT); editor->send(SCI_SETMARGINWIDTHN, 0, 60); editor->send(SCI_SETMARGINWIDTHN, 1, 0);
            editor->send(SCI_STYLESETBACK, STYLE_LINENUMBER, color(dark_ ? QColor("#282828") : QColor("#f0f0f0")));
            editor->send(SCI_MARKERDEFINE, 8, SC_MARK_BACKGROUND); editor->send(SCI_MARKERSETBACK, 8, color(dark_ ? QColor("#4d4635") : QColor("#fff0bb")));
        }
    }
    void LargeTextView::controls()
    {
        for (auto* b : buttons_) b->setEnabled(!Busy());
        for (int side = 0; side < 2; ++side) {
            const bool source = merging() && value_->session_->pair_[side] != value_->session_->result_;
            lines_[side]->setRange(1, std::max(1, int(value_->lines_[side].size()))); lines_[side]->setEnabled(!Busy()); editors_[side]->send(SCI_SETREADONLY, Busy() || source);
            titles_[side]->setText((value_->session_ ? value_->session_->titles_[value_->session_->pair_[side]] + " — " : QString{}) + value_->documents_[side].original_.path_);
            findChild<QPushButton*>(side ? "largeSaveRight" : "largeSaveLeft")->setEnabled(!Busy() && !source);
            findChild<QPushButton*>(side ? "largeMergeRight" : "largeMergeLeft")->setEnabled(!Busy() && !source);
        }
        if (pairs_) pairs_->setEnabled(!Busy() && !pageDirty_[0] && !pageDirty_[1]);
        if (pairs_ && value_->session_) { const QSignalBlocker blocker(pairs_); for (int i = 0; i < pairs_->count(); ++i) { const auto data = pairs_->itemData(i).toList(); if (value_->session_->pair_ == std::array{data[0].toInt(), data[1].toInt()}) pairs_->setCurrentIndex(i); } }
        if (conflict_) { conflict_->setRange(1, std::max(1, int(value_->session_->conflicts_.size()))); conflict_->setEnabled(!Busy()); }
        change_->setRange(1, std::max(1, int(value_->changes_.size()))); change_->setEnabled(!Busy());
        if (pageDirty_[0] || pageDirty_[1]) status_->setText(tr("페이지 수정 중 — ‘페이지 편집 적용’ 후 탐색·병합·저장할 수 있습니다."));
    }
    void LargeTextView::run(std::function<std::shared_ptr<LargeTextComparison>(const Budget&)> Work, bool Mutation, int Saved)
    {
        if (Busy()) return; busy_ = true; canceled_ = std::make_shared<std::atomic_bool>(false);
        const auto starts = std::array{lines_[0]->value() - 1, lines_[1]->value() - 1};
        disconnect(&watcher_, nullptr, this, nullptr);
        connect(&watcher_, &QFutureWatcher<Response>::finished, this, [this, Mutation, Saved] {
            const auto response = watcher_.result(); busy_ = false;
            // A completed disk commit is not rolled back by a later page-load or
            // editor admission failure. Keep its new expected revision.
            if (Saved >= 0 && response.comparison_) { value_ = response.comparison_; saved_[Saved] = value_->documents_[Saved]; if (value_->session_) { const auto index = value_->session_->pair_[Saved]; savedAll_[index] = value_->documents_[Saved]; if (index == value_->session_->result_) resultNeverSaved_ = false; } }
            if (!response.error_.isEmpty()) { controls(); status_->setText(response.error_ + (response.recovery_.isEmpty() ? QString{} : tr(" · 복구 기록: ") + response.recovery_.first().toString())); if (stateChanged_) stateChanged_(); return; }
            std::vector<std::shared_ptr<void>> admissions;
            // Page replacement discards native page history; document Undo lives
            // in immutable disk revisions. Reserve storage without growing Undo.
            try { for (int side = 0; side < 2; ++side) admissions.push_back(static_cast<BoundedEditor*>(editors_[side])->BeginEdit(response.text_[side].size(), editors_[side]->send(SCI_GETLENGTH), true, false, true)); }
            catch (const std::exception& error) { controls(); status_->setText(QString::fromUtf8(error.what())); if (stateChanged_) stateChanged_(); return; }
            if (Mutation) { undo_.push_back(value_); if (undo_.size() > 32) undo_.erase(undo_.begin()); redo_.clear(); }
            if (Saved == -2) { undo_.pop_back(); redo_.push_back(value_); }
            if (Saved == -3) { redo_.pop_back(); undo_.push_back(value_); }
            value_ = response.comparison_; if (Saved >= 0) saved_[Saved] = value_->documents_[Saved];
            begin_ = response.begin_; end_ = response.end_; loading_ = true;
            for (int side = 0; side < 2; ++side) {
                auto* editor = editors_[side]; editor->send(SCI_SETREADONLY, 0); editor->send(SCI_CLEARALL);
                editor->send(SCI_ADDTEXT, response.text_[side].size(), reinterpret_cast<sptr_t>(response.text_[side].constData())); editor->send(SCI_EMPTYUNDOBUFFER);
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
            }
            loading_ = false; controls(); status_->setText(tr("전체 파일 비교: %1개 차이 · 화면은 최대 400줄/1 MiB · 편집·병합은 명시적 저장 전까지 디스크 원본에 반영되지 않습니다.").arg(value_->changes_.size()));
            if (merging()) status_->setText(tr("미해결 충돌: %1 · %2 · ").arg(LargeTextUnresolved(*value_)).arg(value_->session_->reviewRequired_ ? tr("RESULT 전체 수동 검토 필요") : tr("충돌 선택 가능")) + status_->text());
            if (!value_->saveWarnings_.isEmpty() || !value_->saveRecovery_.isEmpty()) status_->setText(status_->text() + tr(" · 저장 확인: ") + QString::fromUtf8(QJsonDocument(QJsonObject{{"warnings", value_->saveWarnings_}, {"recovery", value_->saveRecovery_}}).toJson(QJsonDocument::Compact)));
            if (stateChanged_) stateChanged_();
        });
        watcher_.setFuture(QtConcurrent::run([work = std::move(Work), starts, pageStart = pageStart_, canceled = canceled_, memory = input_.memoryMiB_] {
            Response result; Budget limits; limits.canceled_ = canceled.get(); limits.deadline_ = std::chrono::steady_clock::time_point::max();
            try {
                ConfigureMemoryBudget(limits, memory);
                result.comparison_ = work(limits);
                for (int side = 0; side < 2; ++side) {
                    const auto& document = result.comparison_->documents_[side]; const auto& lines = result.comparison_->lines_[side];
                    auto begin = pageStart[side] ? std::min(*pageStart[side], document.bytes_) : lines[std::min<std::size_t>(starts[side], lines.size() - 1)];
                    const auto continuation = [&](std::uint64_t At) { return At < document.bytes_ && (uchar(ReadLargeTextRange(document, At, At + 1, limits)[0]) & 0xc0) == 0x80; };
                    while (begin && continuation(begin)) --begin;
                    const auto start = std::size_t(std::upper_bound(lines.begin(), lines.end(), begin) - lines.begin() - 1);
                    auto lineEnd = std::min(start + 400, lines.size() - 1);
                    while (lineEnd > start && lines[lineEnd] - begin > largeTextPageMaximum) --lineEnd;
                    auto end = lineEnd > start ? lines[lineEnd] : std::min(begin + largeTextPageMaximum, document.bytes_);
                    while (end > begin && continuation(end)) --end;
                    result.begin_[side] = begin; result.end_[side] = end;
                    result.text_[side] = ReadLargeTextRange(document, begin, end, limits);
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
        std::array<QByteArray, 2> text;
        try {
            const auto a = static_cast<BoundedEditor*>(editors_[0])->ReserveSnapshot(), b = static_cast<BoundedEditor*>(editors_[1])->ReserveSnapshot();
            text = {contents(editors_[0]), contents(editors_[1])};
        } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); return; }
        run([value = value_, text, begin = begin_, end = end_, changed = pageDirty_](const Budget& Limits) {
            auto documents = value->documents_; for (int side = 0; side < 2; ++side) if (changed[side]) documents[side] = EditLargeText(documents[side], begin[side], end[side], text[side], Limits);
            return UpdateLargeTextSession(*value, std::move(documents), Limits);
        }, true);
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
    void LargeTextView::save(int Side)
    {
        if (Busy() || pageDirty_[0] || pageDirty_[1]) return;
        if (merging() && (LargeTextUnresolved(*value_) || value_->session_->reviewRequired_)) { status_->setText(tr("미해결 충돌을 선택하거나 RESULT 전체를 검토 완료한 뒤 저장하십시오.")); return; }
        const auto original = value_->session_ ? (merging() && resultNeverSaved_ ? value_->documents_[Side].original_ : savedAll_[value_->session_->pair_[Side]].original_) : saved_[Side].original_;
        QString output;
        if (merging() && value_->session_->pair_[Side] != value_->session_->result_) return;
        if (merging() && (original.path_.isEmpty() || (resultNeverSaved_ && original.exists_ && !input_.overwrite_))) {
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
        if (Busy() || pageDirty_[0] || pageDirty_[1] || !value_->session_) return;
        const auto pair = pairs_->itemData(Index).toList(); if (pair.size() != 2) return; const std::array selected{pair[0].toInt(), pair[1].toInt()};
        pageStart_ = {}; lines_[0]->setValue(1); lines_[1]->setValue(1);
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
}
