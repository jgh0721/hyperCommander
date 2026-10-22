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
        case_ = new QCheckBox(tr("대소문자"), controls_); case_->setObjectName("compareFindCase"); layout->addWidget(case_);
        word_ = new QCheckBox(tr("단어 단위"), controls_); word_->setObjectName("compareFindWord"); layout->addWidget(word_);
        QSettings settings; case_->setChecked(settings.value("find/caseSensitive", false).toBool());
        word_->setChecked(settings.value("find/wholeWord", false).toBool());
        feedback_ = new QLabel(controls_); feedback_->setObjectName("compareFindFeedback");
        feedback_->setTextFormat(Qt::PlainText); feedback_->setMinimumWidth(120); layout->addWidget(feedback_);
        auto* close = button(tr("닫기"), "compareFindClose"); close->setToolTip(tr("찾기 닫기 (Esc)"));
        addWidget(controls_);
        connect(previous, &QToolButton::clicked, this, [this] { Find(true); });
        connect(next, &QToolButton::clicked, this, [this] { Find(false); });
        connect(close, &QToolButton::clicked, this, &FindBar::Close);
        connect(query_, &QLineEdit::returnPressed, this, [this] { Find(false); });
        connect(query_, &QLineEdit::textChanged, this, &FindBar::clearFeedback);
        connect(scope_, &QComboBox::currentIndexChanged, this, &FindBar::clearFeedback);
        connect(case_, &QCheckBox::toggled, this, [this](bool Value) { QSettings{}.setValue("find/caseSensitive", Value); clearFeedback(); });
        connect(word_, &QCheckBox::toggled, this, [this](bool Value) { QSettings{}.setValue("find/wholeWord", Value); clearFeedback(); });
        query_->installEventFilter(this); hide();
    }
    ScintillaEditBase* FindBar::editor() const
    {
        const auto index = scope_->currentIndex();
        return index >= 0 && index < editors_.size() ? editors_[index].data() : nullptr;
    }
    void FindBar::clearFeedback() { timer_->stop(); search_.reset(); feedback_->clear(); }
    void FindBar::SetEditors(const QVector<ScintillaEditBase*>& Editors)
    {
        for (const auto& connection : connections_) disconnect(connection);
        connections_.clear(); editors_.clear();
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
        SetBusy(busy_);
    }
    void FindBar::SetBusy(bool Busy)
    {
        busy_ = Busy; controls_->setEnabled(!busy_ && editor());
        if (busy_ && search_) { clearFeedback(); feedback_->setText(tr("검색 중단")); }
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
        const bool restore = isVisible(); clearFeedback(); hide();
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
        clearFeedback(); search_.emplace(); auto& search = *search_;
        search.editor_ = target; search.text_ = query_->text().toUtf8(); search.backwards_ = Backwards;
        search.length_ = target->send(SCI_GETLENGTH);
        search.cursor_ = target->send(Backwards ? SCI_GETSELECTIONSTART : SCI_GETSELECTIONEND);
        search.anchor_ = target->send(SCI_GETANCHOR); search.caret_ = target->send(SCI_GETCURRENTPOS);
        search.flags_ = (case_->isChecked() ? SCFIND_MATCHCASE : 0) | (word_->isChecked() ? SCFIND_WHOLEWORD : 0);
        const bool ascii = std::all_of(search.text_.cbegin(), search.text_.cend(), [](char Byte) { return static_cast<unsigned char>(Byte) < 128; });
        search.overlap_ = search.text_.size() * ((search.flags_ & SCFIND_MATCHCASE) ? 1 : ascii ? 4 : 64) + 4;
        search.elapsed_.start(); show(); feedback_->setText(tr("검색 중…")); timer_->start();
    }
    void FindBar::step()
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
            const bool backwards = search.backwards_, wrapped = search.wrapped_; clearFeedback();
            target->send(SCI_SETSEL, uptr_t(backwards ? match.chrgText.cpMax : match.chrgText.cpMin),
                backwards ? match.chrgText.cpMin : match.chrgText.cpMax);
            target->send(SCI_SCROLLCARET);
            feedback_->setText(wrapped ? (backwards ? tr("끝에서 계속 찾음") : tr("처음부터 계속 찾음")) : tr("찾음"));
            return;
        }
        if (search.backwards_ ? boundary == 0 : boundary == search.length_)
        {
            // Search the whole document once after wrapping, including a match
            // that straddles the original caret. No full-document text copy.
            if (search.wrapped_) { clearFeedback(); feedback_->setText(tr("일치 없음")); return; }
            search.wrapped_ = true; search.cursor_ = search.backwards_ ? search.length_ : 0;
        }
        else search.cursor_ = target->send(search.backwards_ ? SCI_POSITIONBEFORE : SCI_POSITIONAFTER, uptr_t(boundary));
    }
}
