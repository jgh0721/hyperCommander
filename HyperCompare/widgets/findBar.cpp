#include "stdafx.h"
#include "findBar.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTimer>
#include <QDockWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QMainWindow>
#include <ScintillaEditBase.h>
#include <algorithm>

namespace hc::compare
{
    FindBar::FindBar(QWidget* Parent) : QToolBar(tr("찾기"), Parent)
    {
        setObjectName("compareFindBar"); setMovable(false); setFloatable(false);
        timer_ = new QTimer(this); timer_->setInterval(0); connect(timer_, &QTimer::timeout, this, &FindBar::step);
        controls_ = new QWidget(this); controls_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* layout = new QHBoxLayout(controls_); layout->setContentsMargins(6, 2, 6, 2);
        auto* label = new QLabel(tr("대상"), controls_); layout->addWidget(label);
        scope_ = new QComboBox(controls_); scope_->setObjectName("compareFindScope"); scope_->setAccessibleName(tr("검색 대상 패널"));
        label->setBuddy(scope_); layout->addWidget(scope_);
        query_ = new QLineEdit(controls_); query_->setObjectName("compareFindText"); query_->setMaxLength(256);
        query_->setToolTip(tr("일반 문자열 검색 · 최대 256자"));
        query_->setAccessibleName(tr("찾을 문자열")); query_->setPlaceholderText(tr("찾을 문자열 — Enter: 다음, Shift+Enter: 이전"));
        query_->setClearButtonEnabled(true); query_->setMinimumWidth(160); layout->addWidget(query_, 1);
        auto button = [this, layout](const QString& Text, const QString& Name) {
            auto* value = new QToolButton(controls_); value->setText(Text); value->setObjectName(Name); layout->addWidget(value); return value;
        };
        auto* previous = button(tr("이전"), "compareFindPrevious"); previous->setToolTip(tr("이전 찾기 (Shift+F3)"));
        auto* next = button(tr("다음"), "compareFindNext"); next->setToolTip(tr("다음 찾기 (F3)"));
        auto* all = button(tr("모두 찾기"), "compareFindAll"); all->setToolTip(tr("일치 구간 강조와 결과 목록 · 최대 1,000건"));
        case_ = new QCheckBox(tr("대소문자"), controls_); case_->setObjectName("compareFindCase"); layout->addWidget(case_);
        word_ = new QCheckBox(tr("단어 단위"), controls_); word_->setObjectName("compareFindWord"); layout->addWidget(word_);
        QSettings settings; case_->setChecked(settings.value("find/caseSensitive", false).toBool());
        word_->setChecked(settings.value("find/wholeWord", false).toBool());
        feedback_ = new QLabel(controls_); feedback_->setObjectName("compareFindFeedback");
        feedback_->setTextFormat(Qt::PlainText); feedback_->setMinimumWidth(120); layout->addWidget(feedback_);
        auto* close = button(tr("닫기"), "compareFindClose"); close->setToolTip(tr("찾기 닫기 (Esc)"));
        addWidget(controls_);
        connect(all, &QToolButton::clicked, this, &FindBar::FindAll);
        connect(previous, &QToolButton::clicked, this, [this] { Find(true); });
        connect(next, &QToolButton::clicked, this, [this] { Find(false); });
        connect(close, &QToolButton::clicked, this, &FindBar::Close);
        connect(query_, &QLineEdit::returnPressed, this, [this] { Find(false); });
        connect(query_, &QLineEdit::textChanged, this, &FindBar::clearFeedback);
        connect(scope_, &QComboBox::currentIndexChanged, this, &FindBar::clearFeedback);
        connect(case_, &QCheckBox::toggled, this, [this](bool Value) { QSettings{}.setValue("find/caseSensitive", Value); clearFeedback(); });
        connect(word_, &QCheckBox::toggled, this, [this](bool Value) { QSettings{}.setValue("find/wholeWord", Value); clearFeedback(); });
        if (auto* main = qobject_cast<QMainWindow*>(Parent))
        {
            resultsDock_ = new QDockWidget(tr("검색 결과"), main); resultsDock_->setObjectName("compareFindResultsDock");
            resultsDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
            results_ = new QTreeWidget(resultsDock_); results_->setObjectName("compareFindResults");
            results_->setAccessibleName(tr("검색 결과 목록")); results_->setHeaderLabels({tr("줄"), tr("열"), tr("내용")});
            results_->setRootIsDecorated(false); results_->setUniformRowHeights(true); results_->setSelectionMode(QAbstractItemView::SingleSelection);
            results_->setToolTip(tr("결과를 두 번 클릭하거나 Enter를 눌러 이동합니다."));
            results_->header()->setSectionResizeMode(0, QHeaderView::Interactive); results_->setColumnWidth(0, 75);
            results_->header()->setSectionResizeMode(1, QHeaderView::Interactive); results_->setColumnWidth(1, 75); results_->header()->setStretchLastSection(true);
            resultsDock_->setWidget(results_); main->addDockWidget(Qt::BottomDockWidgetArea, resultsDock_); resultsDock_->hide();
            connect(results_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* Item) {
                if (busy_ || search_ || !resultEditor_ || resultEditor_ != editor()) return;
                const auto begin = Item->data(0, Qt::UserRole).toLongLong(), end = Item->data(1, Qt::UserRole).toLongLong();
                resultEditor_->send(SCI_SETSEL, uptr_t(begin), sptr_t(end)); resultEditor_->send(SCI_SCROLLCARET); resultEditor_->setFocus();
            });
            connect(resultsDock_, &QDockWidget::visibilityChanged, this, [this](bool Visible) { if (!Visible) clearFeedback(); });
        }
        query_->installEventFilter(this); hide();
    }
    ScintillaEditBase* FindBar::editor() const
    {
        const auto index = scope_->currentIndex();
        return index >= 0 && index < editors_.size() ? editors_[index].data() : nullptr;
    }
    void FindBar::resetSearch()
    { timer_->stop(); search_.reset(); feedback_->clear(); if (results_) results_->setEnabled(!busy_); }
    void FindBar::clearMatches()
    {
        if (results_) results_->clear(); resultEditor_.clear();
        for (const auto& target : editors_) if (target)
        {
            const auto current = target->send(SCI_GETINDICATORCURRENT), value = target->send(SCI_GETINDICATORVALUE);
            target->send(SCI_SETINDICATORCURRENT, searchMatchIndicator); target->send(SCI_INDICATORCLEARRANGE, 0, target->send(SCI_GETLENGTH));
            target->send(SCI_SETINDICATORCURRENT, uptr_t(current)); target->send(SCI_SETINDICATORVALUE, uptr_t(value));
        }
    }
    void FindBar::clearFeedback() { resetSearch(); clearMatches(); }
    void FindBar::SetListFont(const QFont& Font) { if (results_) results_->setFont(Font); }
    void FindBar::styleMatches()
    {
        const auto color = palette().color(QPalette::Highlight);
        const auto rgb = color.red() | (color.green() << 8) | (color.blue() << 16);
        for (const auto& target : editors_) if (target)
        {
            target->send(SCI_INDICSETSTYLE, searchMatchIndicator, INDIC_ROUNDBOX);
            target->send(SCI_INDICSETFORE, searchMatchIndicator, rgb); target->send(SCI_INDICSETALPHA, searchMatchIndicator, 55);
            target->send(SCI_INDICSETOUTLINEALPHA, searchMatchIndicator, 220); target->send(SCI_INDICSETUNDER, searchMatchIndicator, 1);
        }
    }
    void FindBar::changeEvent(QEvent* Event)
    { QToolBar::changeEvent(Event); if (Event->type() == QEvent::PaletteChange) styleMatches(); }
    void FindBar::SetEditors(const QVector<ScintillaEditBase*>& Editors)
    {
        for (const auto& connection : connections_) disconnect(connection);
        clearFeedback(); connections_.clear(); editors_.clear();
        const QSignalBlocker blocker(scope_); scope_->clear(); clearFeedback();
        for (auto* value : Editors)
        {
            const int index = int(editors_.size()); editors_.push_back(value); scope_->addItem(value->objectName());
            connections_.push_back(connect(value, &ScintillaEditBase::focusChanged, this, [this, index](bool Focused) {
                if (Focused) scope_->setCurrentIndex(index);
            }));
            connections_.push_back(connect(value, &ScintillaEditBase::notifyChange, this, &FindBar::clearFeedback));
        }
        if (editors_.isEmpty()) hide();
        styleMatches(); SetBusy(busy_);
    }
    void FindBar::SetBusy(bool Busy)
    {
        busy_ = Busy; controls_->setEnabled(!busy_ && editor());
        if (busy_) { const bool searching = search_.has_value(); clearFeedback(); if (searching) feedback_->setText(tr("검색 중단")); }
    }
    void FindBar::Open()
    {
        auto* target = editor(); if (busy_ || !target) return;
        // Seed only bounded, single-line selections. Never copy whole documents
        // into the query, and never persist searched content to settings.
        const auto begin = target->send(SCI_GETSELECTIONSTART), end = target->send(SCI_GETSELECTIONEND);
        const auto size = end - begin;
        if (target->send(SCI_GETSELECTIONS) == 1 && !target->send(SCI_SELECTIONISRECTANGLE) && size > 0 && size <= 1024)
        {
            QByteArray text(qsizetype(size) + 1, '\0');
            // Read exactly the bounded document range. GETSELTEXT can include
            // virtual-space padding beyond the selected document byte count.
            Sci_TextRangeFull range{{begin, end}, text.data()};
            target->send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range)); text.chop(1);
            const auto selected = QString::fromUtf8(text);
            if (selected.size() <= query_->maxLength() && !selected.contains('\r') && !selected.contains('\n')) query_->setText(selected);
        }
        show(); query_->setFocus(Qt::ShortcutFocusReason); query_->selectAll();
    }
    void FindBar::Close()
    {
        const bool restore = isVisible(); clearFeedback(); if (resultsDock_) resultsDock_->hide(); hide();
        if (restore && editor()) editor()->setFocus(Qt::ShortcutFocusReason);
    }
    bool FindBar::eventFilter(QObject* Object, QEvent* Event)
    {
        if (Object == query_ && Event->type() == QEvent::KeyPress)
        {
            auto* key = static_cast<QKeyEvent*>(Event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers() == Qt::ShiftModifier)
            { Find(true); return true; }
        }
        return QToolBar::eventFilter(Object, Event);
    }
    void FindBar::Find(bool Backwards)
    {
        auto* target = editor(); if (busy_ || !target) return;
        if (query_->text().isEmpty()) { Open(); return; }
        if (query_->text().contains(QChar::Null)) { show(); feedback_->setText(tr("검색어 확인 필요")); return; }
        resetSearch(); search_.emplace(); auto& search = *search_;
        search.editor_ = target; search.text_ = query_->text().toUtf8(); search.backwards_ = Backwards;
        search.length_ = target->send(SCI_GETLENGTH);
        search.cursor_ = target->send(Backwards ? SCI_GETSELECTIONSTART : SCI_GETSELECTIONEND);
        search.anchor_ = target->send(SCI_GETANCHOR); search.caret_ = target->send(SCI_GETCURRENTPOS);
        search.flags_ = (case_->isChecked() ? SCFIND_MATCHCASE : 0) | (word_->isChecked() ? SCFIND_WHOLEWORD : 0);
        const bool ascii = std::all_of(search.text_.cbegin(), search.text_.cend(), [](char Byte) { return static_cast<unsigned char>(Byte) < 128; });
        search.overlap_ = search.text_.size() * ((search.flags_ & SCFIND_MATCHCASE) ? 1 : ascii ? 4 : 64) + 4;
        search.elapsed_.start(); show(); feedback_->setText(tr("검색 중…")); timer_->start();
    }
    void FindBar::FindAll()
    {
        if (busy_ || !editor()) return;
        clearFeedback(); Find(false); if (!search_) return;
        search_->all_ = true; search_->cursor_ = 0; resultEditor_ = editor(); styleMatches();
        if (results_) results_->setEnabled(false);
        if (resultsDock_)
        {
            const bool opening = !resultsDock_->isVisible(); resultsDock_->setWindowTitle(tr("검색 결과 — %1").arg(scope_->currentText())); resultsDock_->show();
            if (opening) if (auto* main = qobject_cast<QMainWindow*>(parentWidget()))
                main->resizeDocks({resultsDock_}, {std::clamp(main->height() / 4, 120, 220)}, Qt::Vertical);
        }
    }
    void FindBar::finishAll(bool Limited)
    {
        const auto count = search_ ? search_->count_ : 0; resetSearch();
        feedback_->setText(Limited ? tr("%1건 · 표시 제한").arg(count) : tr("%1건 찾음").arg(count));
    }
    void FindBar::step()
    {
        QElapsedTimer slice; slice.start(); if (results_) results_->setUpdatesEnabled(false);
        // Batch bounded hits to avoid one layout/paint for every result row.
        for (int i = 0; i < 64; ++i)
        {
            stepOnce();
            if (!search_ || !search_->all_ || slice.elapsed() >= 6) break;
        }
        if (results_) results_->setUpdatesEnabled(true);
    }
    void FindBar::stepOnce()
    {
        if (!search_) return;
        auto& search = *search_; auto* target = search.editor_.data();
        if (busy_ || !target || target != editor() || target->send(SCI_GETLENGTH) != search.length_ ||
            target->send(SCI_GETANCHOR) != search.anchor_ || target->send(SCI_GETCURRENTPOS) != search.caret_)
        { clearFeedback(); return; }
        if (search.elapsed_.elapsed() >= 5000) { clearFeedback(); feedback_->setText(tr("검색 시간 초과")); return; }
        // Yield between bounded candidate ranges. Scintilla reserves 16 folded
        // bytes per input byte; each matched document character can use 4 bytes.
        // ASCII queries have no folding expansion, so 4 bytes suffice there.
        // This conservative overlap retains Unicode case-fold matches across
        // chunk boundaries. Whole words are checked against the full document.
        const sptr_t chunk = std::max<sptr_t>(256, 262144 / search.text_.size());
        const auto boundary = search.backwards_ ? std::max<sptr_t>(0, search.cursor_ - chunk) : std::min(search.length_, search.cursor_ + chunk);
        const auto limit = search.backwards_ ? std::max<sptr_t>(0, boundary - search.overlap_) : std::min(search.length_, boundary + search.overlap_);
        Sci_TextToFindFull match{{search.cursor_, limit}, search.text_.constData(), {0, 0}};
        const auto found = target->send(SCI_FINDTEXTFULL, search.flags_, reinterpret_cast<sptr_t>(&match));
        if (found >= 0)
        {
            if (search.all_)
            {
                const auto begin = match.chrgText.cpMin, end = match.chrgText.cpMax;
                const auto current = target->send(SCI_GETINDICATORCURRENT), value = target->send(SCI_GETINDICATORVALUE);
                target->send(SCI_SETINDICATORCURRENT, searchMatchIndicator); target->send(SCI_SETINDICATORVALUE, 1);
                target->send(SCI_INDICATORFILLRANGE, uptr_t(begin), end - begin);
                target->send(SCI_SETINDICATORCURRENT, uptr_t(current)); target->send(SCI_SETINDICATORVALUE, uptr_t(value));
                if (results_)
                {
                    const auto line = target->send(SCI_LINEFROMPOSITION, uptr_t(begin));
                    const auto lineStart = target->send(SCI_POSITIONFROMLINE, uptr_t(line)), lineEnd = target->send(SCI_GETLINEENDPOSITION, uptr_t(line));
                    // Bound bytes first and then align both ends to UTF-8 boundaries.
                    // PositionRelative clamps an out-of-document request to zero.
                    const auto rawStart = std::max(lineStart, begin - 32);
                    const auto start = rawStart == lineStart ? lineStart : target->send(SCI_POSITIONAFTER, uptr_t(rawStart - 1));
                    const auto rawEnd = std::min(lineEnd, start + 128);
                    const auto finish = rawEnd == lineEnd ? lineEnd : target->send(SCI_POSITIONBEFORE, uptr_t(rawEnd + 1));
                    QByteArray text(qsizetype(finish - start) + 1, '\0'); Sci_TextRangeFull range{{start, finish}, text.data()};
                    target->send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<sptr_t>(&range)); text.chop(1);
                    auto* item = new QTreeWidgetItem(results_, {QString::number(line + 1), QString::number(target->send(SCI_GETCOLUMN, uptr_t(begin)) + 1), (start > lineStart ? QStringLiteral("…") : QString{}) + QString::fromUtf8(text) + (finish < lineEnd ? QStringLiteral("…") : QString{})});
                    item->setData(0, Qt::UserRole, qlonglong(begin)); item->setData(1, Qt::UserRole, qlonglong(end));
                }
                ++search.count_; search.cursor_ = end;
                if (end >= search.length_) finishAll(false);
                else if (search.count_ >= 1000) finishAll(true);
                else feedback_->setText(tr("검색 중… %1건").arg(search.count_));
                return;
            }
            const bool backwards = search.backwards_, wrapped = search.wrapped_; resetSearch();
            target->send(SCI_SETSEL, uptr_t(backwards ? match.chrgText.cpMax : match.chrgText.cpMin),
                backwards ? match.chrgText.cpMin : match.chrgText.cpMax);
            target->send(SCI_SCROLLCARET);
            feedback_->setText(wrapped ? (backwards ? tr("끝에서 계속 찾음") : tr("처음부터 계속 찾음")) : tr("찾음"));
            return;
        }
        if (search.backwards_ ? boundary == 0 : boundary == search.length_)
        {
            if (search.all_) { finishAll(false); return; }
            // Search the whole document once after wrapping, including a match
            // that straddles the original caret. No full-document text copy.
            if (search.wrapped_) { clearFeedback(); feedback_->setText(tr("일치 없음")); return; }
            search.wrapped_ = true; search.cursor_ = search.backwards_ ? search.length_ : 0;
        }
        else search.cursor_ = target->send(search.backwards_ ? SCI_POSITIONBEFORE : SCI_POSITIONAFTER, uptr_t(boundary));
    }
}
