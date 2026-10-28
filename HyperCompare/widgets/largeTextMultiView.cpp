#include "stdafx.h"
#include "largeTextMultiView.hpp"
#include "comparePalette.hpp"
#include "compareSyntax.hpp"
#include "largeTextSyntax.hpp"
#include "boundedEditor.hpp"
#include "chunkGutter.hpp"
#include "runtime/controllerMemory.hpp"
#include <QtConcurrentRun>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QToolButton>
#include <QPushButton>
#include <QTimer>
#include <QSignalBlocker>
#include <ILexer.h>
#include <Lexilla.h>
#include <algorithm>

namespace hc::compare
{
    class LargeTextSourcePane final : public QWidget
    {
    public:
        int document_;
        std::function<void()> changed_, copy_;
        bool busy_ = false;
        QString error_;
        LargeTextSourcePane(int Document, int Memory, QWidget* Parent) : QWidget(Parent), document_(Document), memory_(Memory) {
            setObjectName(QString("largeInputPreview%1").arg(Document)); auto* column = new QVBoxLayout(this); column->setContentsMargins(0, 0, 0, 0);
            title_ = new QLabel(this); title_->setTextFormat(Qt::PlainText); title_->setMinimumWidth(0); title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); column->addWidget(title_);
            auto* controls = new QHBoxLayout(); column->addLayout(controls); line_ = new QSpinBox(this); line_->setRange(1, 1); line_->setMaximumWidth(100); line_->setObjectName(QString("largeInputLine%1").arg(Document)); controls->addWidget(line_);
            const auto button = [&](const QString& Text, auto Function) { auto* button = new QToolButton(this); button->setText(Text); controls->addWidget(button); connect(button, &QToolButton::clicked, this, Function); return button; };
            button("‹", [this] { if (!busy_ && value_) { const auto& lines = value_->lines_[0]; const auto at = std::upper_bound(lines.begin(), lines.end(), begin_) - lines.begin() - 1; load(lines[std::max<std::ptrdiff_t>(0, at - 400)]); } });
            button("›", [this] { if (!busy_ && value_) load(end_); });
            button("↵", [this] { if (!busy_ && value_) { const auto& lines = value_->lines_[0]; load(lines[std::min<std::size_t>(line_->value() - 1, lines.size() - 1)]); } });
            copyButton_ = button(tr("선택 → RESULT"), [this] { if (!busy_ && copy_) copy_(); }); copyButton_->setObjectName(QString("largeInput%1ToResult").arg(Document));
            editor_ = new BoundedEditor(this, Memory, largeTextPageMaximum); editor_->setObjectName(QString("largeTextInput%1").arg(Document)); editor_->send(SCI_SETCODEPAGE, SC_CP_UTF8); editor_->EnableProtection(8 * 1024 * 1024); editor_->send(SCI_SETREADONLY, 1); column->addWidget(editor_, 1);
            status_ = new QLabel(this); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); column->addWidget(status_);
            editor_->blocked_ = [this](const QString& Reason) { status_->setText(Reason); };
            connect(&watcher_, &QFutureWatcher<Response>::finished, this, [this] {
                const auto response = watcher_.result(); busy_ = false; error_ = response.error_;
                if (!error_.isEmpty()) status_->setText(error_);
                else if (response.match_ && !response.match_->found_) { match_ = {}; status_->setText(tr("전체 파일에 일치 항목이 없습니다.")); }
                else {
                    try {
                        const auto admission = editor_->BeginEdit(response.text_.size(), editor_->send(SCI_GETLENGTH), true, false, true);
                        value_ = response.value_; begin_ = response.begin_; end_ = response.end_; syntax_ = response.syntax_; editor_->send(SCI_SETREADONLY, 0); editor_->send(SCI_CLEARALL); editor_->send(SCI_ADDTEXT, response.text_.size(), reinterpret_cast<sptr_t>(response.text_.constData())); editor_->send(SCI_EMPTYUNDOBUFFER); editor_->send(SCI_SETILEXER, 0, 0); editor_->send(SCI_STARTSTYLING, 0); editor_->send(SCI_SETSTYLINGEX, response.styles_.size(), reinterpret_cast<sptr_t>(response.styles_.constData()));
                        const auto& offsets = value_->lines_[0]; const auto first = std::size_t(std::upper_bound(offsets.begin(), offsets.end(), begin_) - offsets.begin() - 1);
                        editor_->send(SCI_MARGINTEXTCLEARALL); for (sptr_t row = 0; row < editor_->send(SCI_GETLINECOUNT); ++row) { const auto number = QByteArray::number(qulonglong(first + row + 1)); editor_->send(SCI_MARGINSETTEXT, row, reinterpret_cast<sptr_t>(number.constData())); editor_->send(SCI_MARGINSETSTYLE, row, STYLE_LINENUMBER); }
                        const auto last = first + std::size_t(editor_->send(SCI_GETLINECOUNT));
                        ClearCompareMarks(editor_); auto edit = std::lower_bound(value_->changes_.begin(), value_->changes_.end(), first, [](const auto& Edit, auto Line) { return Edit.end_ < Line; });
                        for (; edit != value_->changes_.end() && edit->begin_ < last; ++edit) for (auto row = std::max(first, edit->begin_); row < std::min(edit->end_, last); ++row) editor_->send(SCI_MARKERADD, row - first, CompareEditMarker(edit->begin_, edit->end_, edit->otherBegin_, edit->otherEnd_));
                        const auto& session = *value_->session_; const auto& source = session.documents_[document_]; title_->setText(session.titles_[document_] + " — " + source.original_.path_); title_->setToolTip(title_->text()); line_->setRange(1, std::max(1, int(offsets.size()))); line_->setValue(int(first) + 1);
                        lexerId_ = CompareLexerId(languages_(source.original_.path_)); Appearance(font_, dark_); editor_->send(SCI_SETREADONLY, 1); status_->setText(tr("%1 · 읽기 전용 · %2–%3 바이트").arg(source.original_.encoding_).arg(begin_).arg(end_));
                        if (response.match_) { match_ = *response.match_; editor_->send(SCI_SETSEL, match_.begin_ - begin_, std::min(match_.end_, end_) - begin_); editor_->send(SCI_SCROLLCARET); status_->setText(match_.wrapped_ ? tr("문서 처음/끝으로 돌아가 찾았습니다.") : tr("전체 파일에서 찾았습니다.")); }
                    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); editor_->send(SCI_SETREADONLY, 1); status_->setText(error_); }
                }
                if (changed_) changed_();
            });
        }
        ~LargeTextSourcePane() override { Stop(); editor_->blocked_ = {}; }
        void Stop() { canceled_->store(true); watcher_.waitForFinished(); }
        void Cancel() { canceled_->store(true); }
        void Appearance(const QFont& Font, bool Dark) { font_ = Font; dark_ = Dark; ApplyLargeTextEditorAppearance(editor_, Font, Dark, lexerId_); }
        // Takes effect with the next Bind; the cached styles belong to the previous language.
        void SetLanguages(CompareLanguageResolver Languages) { languages_ = std::move(Languages); syntax_.reset(); }
        void EnableCopy(bool Enabled, bool Visible) { copyButton_->setVisible(Visible); copyButton_->setEnabled(Enabled && !busy_ && error_.isEmpty()); }
        QString Message() const { return status_->text(); }
        std::uint64_t Cursor() const { return begin_ + std::uint64_t(editor_->send(SCI_GETCURRENTPOS)); }
        std::optional<std::pair<std::uint64_t, std::uint64_t>> Selection() const {
            if (editor_->send(SCI_GETSELECTIONS) != 1 || editor_->send(SCI_GETSELECTIONMODE) == SC_SEL_RECTANGLE) throw Failure("LargeTextSelectionUnsupported");
            const auto a = editor_->send(SCI_GETSELECTIONSTART), b = editor_->send(SCI_GETSELECTIONEND); if (a == b) return {}; return std::pair{begin_ + std::uint64_t(a), begin_ + std::uint64_t(b)};
        }
        void Bind(std::shared_ptr<const LargeTextSession> Session, std::uint64_t Anchor) {
            if (busy_) return; auto retained = value_;
            start([Session, retained, Anchor, document = document_](const Budget& Limits) {
                auto session = *Session;
                if (retained) for (const auto& cache : retained->session_->pairCache_) if (cache->indexes_[0] == session.documents_[cache->pair_[0]].index_ && cache->indexes_[1] == session.documents_[cache->pair_[1]].index_ && std::none_of(session.pairCache_.begin(), session.pairCache_.end(), [&](const auto& Existing) { return Existing->pair_ == cache->pair_ && Existing->indexes_ == cache->indexes_; })) session.pairCache_.push_back(cache);
                const auto reference = session.result_ >= 0 ? session.result_ : 1; auto value = PairLargeTextSession(std::move(session), {document, reference}, Limits);
                const auto& target = value->lines_[1]; const auto line = std::int64_t(std::upper_bound(target.begin(), target.end(), std::min(Anchor, value->documents_[1].bytes_)) - target.begin() - 1);
                std::int64_t sourceLine = line, delta = 0; for (const auto& edit : value->changes_) { if (line < std::int64_t(edit.otherBegin_)) { sourceLine = line + delta; break; } if (line <= std::int64_t(edit.otherEnd_)) { sourceLine = std::int64_t(edit.begin_) + std::min<std::int64_t>(line - edit.otherBegin_, edit.end_ - edit.begin_); break; } delta = std::int64_t(edit.end_) - std::int64_t(edit.otherEnd_); sourceLine = line + delta; }
                sourceLine = std::clamp<std::int64_t>(sourceLine, 0, std::int64_t(value->lines_[0].size() - 1)); Response response; response.value_ = value;
                const auto sourceBegin = value->lines_[0][sourceLine], sourceEnd = std::size_t(sourceLine + 1) < value->lines_[0].size() ? value->lines_[0][sourceLine + 1] : value->documents_[0].bytes_;
                response.begin_ = sourceBegin + std::min(Anchor - target[line], sourceEnd - sourceBegin); return response;
            });
        }
        void Find(const QString& Query, bool Backwards, Qt::CaseSensitivity Case, bool Regex) {
            if (busy_ || !value_ || Query.isEmpty()) return; const bool repeat = query_ == Query && case_ == Case && regex_ == Regex && match_.found_;
            auto from = repeat ? (Backwards ? match_.begin_ : match_.end_) : Cursor(); const bool advance = repeat && match_.begin_ == match_.end_ && !Backwards; query_ = Query; case_ = Case; regex_ = Regex;
            start([value = value_, Query, Backwards, Case, Regex, from, advance](const Budget& Limits) mutable { const auto& document = value->documents_[0]; if (advance) { if (from == document.bytes_) from = 0; else do { ++from; } while (from < document.bytes_ && (uchar(ReadLargeTextRange(document, from, from + 1, Limits)[0]) & 0xc0) == 0x80); } Response response; response.value_ = value; response.match_ = FindLargeText(document, Query, from, Backwards, Case, Limits, Regex); response.begin_ = response.match_->begin_; return response; });
        }
    private:
        struct Response { std::shared_ptr<LargeTextComparison> value_; QByteArray text_, styles_; std::shared_ptr<LargeTextSyntax> syntax_; std::uint64_t begin_ = 0, end_ = 0; std::optional<LargeTextMatch> match_; QString error_; };
        void load(std::uint64_t Begin) { start([value = value_, Begin](const Budget&) { Response response; response.value_ = value; response.begin_ = Begin; return response; }); }
        void start(std::function<Response(const Budget&)> Work) {
            if (busy_) return; busy_ = true; canceled_ = std::make_shared<std::atomic_bool>(false); if (changed_) changed_();
            watcher_.setFuture(QtConcurrent::run([Work = std::move(Work), canceled = canceled_, syntax = syntax_, languages = languages_, memory = memory_]() {
                Response response; try { Budget limits; limits.deadline_ = std::chrono::steady_clock::time_point::max(); limits.canceled_ = canceled.get(); ConfigureMemoryBudget(limits, memory); response = Work(limits); if (response.match_ && !response.match_->found_) return response;
                    const auto& document = response.value_->documents_[0]; const auto& lines = response.value_->lines_[0]; auto begin = std::min(response.begin_, document.bytes_); const auto continuation = [&](std::uint64_t At) { return At < document.bytes_ && (uchar(ReadLargeTextRange(document, At, At + 1, limits)[0]) & 0xc0) == 0x80; }; while (begin && continuation(begin)) --begin;
                    const auto first = std::size_t(std::upper_bound(lines.begin(), lines.end(), begin) - lines.begin() - 1); auto last = std::min(first + 400, lines.size() - 1); while (last > first && lines[last] - begin > largeTextPageMaximum) --last; auto end = last > first ? lines[last] : std::min(document.bytes_, begin + largeTextPageMaximum); while (end > begin && continuation(end)) --end;
                    response.begin_ = begin; response.end_ = end; response.text_ = ReadLargeTextRange(document, begin, end, limits); response.syntax_ = syntax; response.styles_ = StyleLargeText(document, languages(document.original_.path_), begin, end, response.syntax_, limits);
                } catch (const std::exception& error) { response.error_ = QString::fromUtf8(error.what()); } return response;
            }));
        }
        CompareLanguageResolver languages_;
        int memory_, lexerId_ = 0; bool dark_ = false; QFont font_{"D2Coding ligature", 14}; QLabel* title_ = nullptr; QLabel* status_ = nullptr; QSpinBox* line_ = nullptr; QToolButton* copyButton_ = nullptr; BoundedEditor* editor_ = nullptr;
        std::shared_ptr<LargeTextComparison> value_; std::shared_ptr<LargeTextSyntax> syntax_; std::uint64_t begin_ = 0, end_ = 0; QString query_; Qt::CaseSensitivity case_ = Qt::CaseInsensitive; bool regex_ = false; LargeTextMatch match_;
        QFutureWatcher<Response> watcher_; std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
    };
    LargeTextMultiView::LargeTextMultiView(Request Input, std::shared_ptr<LargeTextComparison> Value, QWidget* Parent) : LargeTextView(std::move(Input), std::move(Value), Parent)
    {
        auto* row = new QHBoxLayout(); static_cast<QVBoxLayout*>(layout())->insertLayout(0, row); row->addWidget(new QLabel(tr("보기"), this)); layoutMode_ = new QComboBox(this); layoutMode_->setObjectName("largeLayoutMode"); layoutMode_->addItems({tr("비교 쌍 편집"), merging() ? tr("%1입력 + RESULT 동시 보기").arg(value_->session_->result_) : tr("3파일 동시 보기(읽기 전용)")}); layoutMode_->setCurrentIndex(1); row->addWidget(layoutMode_); row->addStretch();
        for (int document = 0; document < int(value_->session_->documents_.size()); ++document) if (document != 0 && document != (merging() ? value_->session_->result_ : 1)) {
            auto* pane = new LargeTextSourcePane(document, input_.memoryMiB_, this); sources_.push_back(pane); if (merging()) panes_->insertWidget(int(sources_.size()), pane, 1); else panes_->addWidget(pane, 1);
            pane->changed_ = [this] { changed(); if (stateChanged_) stateChanged_(); }; pane->copy_ = [this, pane] { applySource(pane); }; pane->SetLanguages(languages_);
        }
        stateChanged_ = [this] { changed(); }; connect(layoutMode_, &QComboBox::currentIndexChanged, this, [this](int Index) { const QSignalBlocker blocker(layoutMode_); layoutMode_->setCurrentIndex(all_ ? 1 : 0); if (Busy()) return; const auto action = [this, Index] { setAll(Index == 1); }; if (!deferPages(action)) action(); });
        setAll(true);
    }
    LargeTextMultiView::~LargeTextMultiView() { for (auto* pane : sources_) { pane->changed_ = {}; pane->copy_ = {}; pane->Stop(); } }
    bool LargeTextMultiView::Busy() const { if (LargeTextView::Busy()) return true; if (all_) for (auto* pane : sources_) if (pane->busy_) return true; return false; }
    int LargeTextMultiView::ExitCode() const { if (all_) for (auto* pane : sources_) if (!pane->error_.isEmpty()) return pane->error_ == "Canceled" ? 3 : 2; return LargeTextView::ExitCode(); }
    void LargeTextMultiView::Cancel() { LargeTextView::Cancel(); for (auto* pane : sources_) pane->Cancel(); }
    void LargeTextMultiView::SetTextFont(const QFont& Font) { LargeTextView::SetTextFont(Font); for (auto* pane : sources_) pane->Appearance(Font, dark_); }
    void LargeTextMultiView::SetDark(bool Dark) { LargeTextView::SetDark(Dark); for (auto* pane : sources_) pane->Appearance(textFont_, Dark); }
    void LargeTextMultiView::SetSyntax(const QString& Override)
    { LargeTextView::SetSyntax(Override); for (auto* pane : sources_) pane->SetLanguages(languages_); displayed_.reset(); changed(); }
    void LargeTextMultiView::setAll(bool All) {
        all_ = All; displayReadOnly_ = all_ && !merging(); { const QSignalBlocker blocker(layoutMode_); layoutMode_->setCurrentIndex(all_ ? 1 : 0); }
        pairs_->setVisible(!all_); gutter_->setVisible(!all_); for (auto* pane : sources_) pane->setVisible(all_);
        for (const auto& name : {"largePreviousLeft", "largePreviousRight", "largeNextLeft", "largeNextRight"}) {
            auto* button = findChild<QPushButton*>(name); const bool previous = QString(name).contains("Previous"); button->setText(all_ ? (previous ? "‹" : "›") : (previous ? tr("이전") : tr("다음"))); button->setToolTip(previous ? tr("이전 페이지") : tr("다음 페이지")); button->setMaximumWidth(all_ ? 30 : QWIDGETSIZE_MAX); button->setMinimumWidth(0);
        }
        if (merging()) findChild<QPushButton*>("largeMergeRight")->setText(all_ ? tr("FILE 1 → RESULT") : tr("왼쪽 → 오른쪽"));
        { const QSignalBlocker blocker(searchScope_); searchScope_->clear(); if (all_) for (int i = 0; i < value_->session_->titles_.size(); ++i) searchScope_->addItem(value_->session_->titles_[i], i); else searchScope_->addItems({tr("왼쪽 전체 파일"), tr("오른쪽 전체 파일")}); }
        displayed_.reset();
        const std::array canonical{0, merging() ? value_->session_->result_ : 1};
        if (all_ && value_->session_->pair_ != canonical) { pageStart_ = {}; run([session = *value_->session_, canonical](const Budget& Limits) { return PairLargeTextSession(session, canonical, Limits); }, false); }
        else changed();
    }
    void LargeTextMultiView::changed() {
        if (synchronizing_) return; synchronizing_ = true;
        if (all_ && !LargeTextView::Busy() && !deferredContinuation_ && !pageDirty_[0] && !pageDirty_[1] && value_->session_ && (displayed_ != value_->session_ || resultAnchor_ != begin_[1])) {
            displayed_ = value_->session_; resultAnchor_ = begin_[1]; for (auto* pane : sources_) pane->Bind(displayed_, resultAnchor_);
        }
        controls(); layoutMode_->setEnabled(!Busy()); for (auto* pane : sources_) pane->EnableCopy(!Busy(), merging());
        if (!Busy() && (pageDirty_[0] || pageDirty_[1])) applyTimer_->start();
        if (searching_ && !searching_->busy_) { status_->setText(searching_->Message()); searching_ = nullptr; }
        synchronizing_ = false;
    }
    void LargeTextMultiView::find(bool Backwards) {
        if (!all_) { LargeTextView::find(Backwards); return; } if (Busy()) return;
        if (deferPages([this, Backwards] { find(Backwards); })) return;
        const auto document = searchScope_->currentData().toInt();
        for (auto* pane : sources_) if (pane->document_ == document) { searching_ = pane; pane->Find(query_->text(), Backwards, searchCase_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive, searchRegex_->isChecked()); return; }
        const QSignalBlocker blocker(searchScope_); const auto selected = searchScope_->currentIndex(); searchScope_->setCurrentIndex(document == 0 ? 0 : 1); LargeTextView::find(Backwards); searchScope_->setCurrentIndex(selected);
    }
    void LargeTextMultiView::applySource(LargeTextSourcePane* Pane) {
        if (Busy() || !merging()) return;
        try {
            const auto selection = Pane->Selection(); const auto cursor = Pane->Cursor();
            const auto action = [this, document = Pane->document_, selection, cursor] {
                run([current = value_, document, selection, cursor](const Budget& Limits) {
                    auto pair = PairLargeTextSession(*current->session_, {document, current->session_->result_}, Limits); auto documents = pair->documents_; bool applied = false;
                    for (std::size_t index = pair->changes_.size(); index-- > 0;) { const auto& edit = pair->changes_[index]; const auto begin = pair->lines_[0][edit.begin_], end = pair->lines_[0][edit.end_]; const bool contains = selection ? (begin == end ? selection->first <= begin && begin < selection->second : std::max(begin, selection->first) < std::min(end, selection->second)) : (begin == end ? cursor == begin : begin <= cursor && cursor < end);
                        if (!contains) continue; pair->documents_[1] = documents[1]; documents[1] = MergeLargeTextChange(*pair, 0, index, begin == end ? std::nullopt : selection, Limits); applied = true; if (!selection) break;
                    }
                    if (!applied) throw Failure("LargeTextNoChangeAtSelection"); const auto next = UpdateLargeTextSession(*pair, std::move(documents), Limits); return PairLargeTextSession(*next->session_, current->session_->pair_, Limits);
                }, true);
            }; if (!deferPages(action)) action();
        } catch (const std::exception& error) { status_->setText(QString::fromUtf8(error.what())); }
    }
}
