#include "stdafx.h"
#include "compareWindow.hpp"
#include "chunkGutter.hpp"
#include "scrollSynchronizer.hpp"
#include "multiPaneAlignment.hpp"
#include "overviewBar.hpp"
#include "semanticView.hpp"
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QTabWidget>
#include <QJsonDocument>
#include <QtConcurrentRun>
#include <ScintillaEditBase.h>

namespace hc::compare
{
    namespace
    {
        QByteArray bytes(ScintillaEditBase* Editor)
        {
            QByteArray value(qsizetype(Editor->send(SCI_GETLENGTH)) + 1, '\0');
            Editor->send(SCI_GETTEXT, uptr_t(value.size()), reinterpret_cast<sptr_t>(value.data())); value.chop(1); return value;
        }
    }
    void CompareWindow::setupInputEditing()
    {
        bufferTimer_.setSingleShot(true); bufferTimer_.setInterval(120);
        connect(&bufferTimer_, &QTimer::timeout, this, &CompareWindow::compareInputBuffers);
        save_->setShortcut(QKeySequence::Save);
        auto* menu = menuBar()->actions().front()->menu(); menu->addSeparator();
        for (int i = 0; i < 3; ++i)
        {
            auto* action = menu->addAction(tr("FILE %1 저장").arg(i + 1), this, [this, i] { saveInput(i); });
            action->setObjectName(QString("saveInput%1").arg(i + 1)); action->setVisible(false); inputSaveActions_.push_back(action);
        }
        connect(&bufferWatcher_, &QFutureWatcher<BufferComparison>::finished, this, [this] {
            const auto computed = bufferWatcher_.result();
            if (!sourceEditing_) return;
            if (computed.generation_ != bufferGeneration_) { bufferTimer_.start(); return; }
            sourcePending_ = false; sourceValid_ = computed.result_.completion_ == "Complete"; inputError_ = computed.result_.error_;
            if (sourceValid_)
            {
                // Only working-text metadata is replaced; disk revisions/encoding
                // remain the independently saved snapshots for each input.
                for (int i = 0; i < result_.inputs_.size(); ++i) result_.inputs_[i].utf8_ = computed.result_.inputs_[i].utf8_;
                result_.pairViews_ = computed.result_.pairViews_; result_.edits_ = computed.result_.edits_;
                result_.semantic_ = computed.result_.semantic_; result_.textComparison_ = computed.result_.textComparison_;
                result_.outcome_ = computed.result_.outcome_; result_.exitCode_ = computed.result_.exitCode_;
                result_.comparisons_ = computed.result_.comparisons_;
                QVector<std::pair<sptr_t, sptr_t>> views;
                for (auto* editor : editors_) views.push_back({editor->send(SCI_DOCLINEFROMVISIBLE, editor->send(SCI_GETFIRSTVISIBLELINE)), editor->send(SCI_GETXOFFSET)});
                semantic_->SetResult(result_.semantic_, !request_.base_.isEmpty()); details_->setTabVisible(1, !result_.semantic_["pairs"].toArray().isEmpty());
                applyPairView(); rebuildMultiAlignment(); applyAppearance();
                if (scroll_) scroll_->SetEnabled(false);
                for (int i = 0; i < editors_.size(); ++i)
                {
                    editors_[i]->send(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, editors_[i]->send(SCI_VISIBLEFROMDOCLINE, views[i].first)));
                    editors_[i]->send(SCI_SETXOFFSET, views[i].second);
                }
            }
            updateMerge();
        });
        connect(&inputSaveWatcher_, &QFutureWatcher<InputSaveResult>::finished, this, [this] {
            const auto saved = inputSaveWatcher_.result(); const auto side = savingInput_; savingInput_ = -1;
            for (int i = 0; i < editors_.size(); ++i) editors_[i]->send(SCI_SETREADONLY, i == 2 && !request_.base_.isEmpty());
            if (saved.error_.isEmpty())
            {
                result_.inputs_[side] = saved.snapshot_; editors_[side]->send(SCI_SETSAVEPOINT);
                inputError_.clear(); invalidateInputComparison();
            }
            else inputSaveError_ = tr("FILE %1 저장 실패: %2").arg(side + 1).arg(saved.error_);
            updateMerge();
            state_->setToolTip(QString::fromUtf8(QJsonDocument(QJsonObject{{"error", saved.error_}, {"committed", saved.committed_}, {"warnings", saved.warnings_}, {"recovery", saved.recovery_}}).toJson()));
            if (saved.error_.isEmpty() && (!saved.warnings_.isEmpty() || !saved.recovery_.isEmpty())) state_->setText(tr("저장 완료 — 복구 기록·임시 파일 정리 확인 필요"));
            if (closing_) close();
        });
    }
    void CompareWindow::bindInputEditors()
    {
        sourceEditing_ = !merge_ && result_.extension_.isEmpty() && !result_.operation_.startsWith("directory") && result_.inputs_.size() >= 2;
        sourcePending_ = false; sourceValid_ = true; activeInput_ = 0; inputError_.clear(); inputSaveError_.clear(); ++bufferGeneration_;
        if (!sourceEditing_) return;
        for (int i = 0; i < editors_.size(); ++i)
        {
            auto* editor = editors_[i]; editor->send(SCI_SETREADONLY, i == 2 && !request_.base_.isEmpty());
            connect(editor, &ScintillaEditBase::focusChanged, this, [this, i](bool Focused) {
                if (Focused && sourceEditing_) { activeInput_ = i; updateMerge(); }
            });
            connect(editor, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags Flags) {
                if (!sourceEditing_) return;
                if (int(Flags) & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE))
                {
                    if (scroll_) { scroll_->SetEnabled(false); scroll_->ClearPadding(); }
                }
                if (int(Flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) invalidateInputComparison();
            });
            connect(editor, &ScintillaEditBase::updateUi, this, [this] { if (sourceEditing_) updateInputState(); });
        }
    }
    void CompareWindow::invalidateInputComparison()
    {
        ++bufferGeneration_; if (bufferCanceled_) bufferCanceled_->store(true);
        sourcePending_ = true; inputError_.clear(); inputSaveError_.clear();
        if (scroll_) { scroll_->SetEnabled(false); scroll_->ClearPadding(); }
        overview_->SetSession(nullptr, nullptr, {});
        for (auto* editor : editors_) { editor->send(SCI_MARKERDELETEALL, 8); editor->send(SCI_SETINDICATORCURRENT, sourceDifferenceIndicator); editor->send(SCI_INDICATORCLEARRANGE, 0, editor->send(SCI_GETLENGTH)); }
        for (auto* gutter : gutters_) gutter->SetChunks({}, bufferGeneration_);
        details_->setTabVisible(1, false); bufferTimer_.start(); updateInputState();
    }
    void CompareWindow::compareInputBuffers()
    {
        if (!sourceEditing_ || !sourcePending_) return;
        if (bufferWatcher_.isRunning()) return;
        QVector<QByteArray> texts; for (auto* editor : editors_) texts.push_back(bytes(editor));
        bufferCanceled_ = std::make_shared<std::atomic_bool>(false);
        bufferWatcher_.setFuture(QtConcurrent::run([input = request_, previous = result_, texts, cancel = bufferCanceled_, generation = bufferGeneration_] {
            Budget limits; limits.canceled_ = cancel.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return BufferComparison{CompareOpenBuffers(input, previous, texts, limits), generation};
        }));
    }
    void CompareWindow::updateInputState()
    {
        if (!sourceEditing_ || editors_.isEmpty()) return;
        bool dirty = false;
        for (int i = 0; i < editors_.size(); ++i)
        {
            const bool changed = editors_[i]->send(SCI_GETMODIFY); dirty |= changed;
            if (i < inputSaveActions_.size()) inputSaveActions_[i]->setEnabled(!busy() && changed);
            if (i < formatLabels_.size())
            {
                const auto& input = result_.inputs_[i];
                formatLabels_[i]->setText(QString("%1 · %2%3").arg(input.encoding_, input.bom_ ? tr("BOM 있음") : tr("BOM 없음"), changed ? tr(" · 수정됨 *") : QString{}));
            }
        }
        dirty_ = dirty; auto* active = editors_[activeInput_];
        save_->setText(tr("FILE %1 저장").arg(activeInput_ + 1)); save_->setEnabled(!busy() && active->send(SCI_GETMODIFY));
        undo_->setEnabled(!busy() && active->send(SCI_CANUNDO)); redo_->setEnabled(!busy() && active->send(SCI_CANREDO));
        pairSelector_->setEnabled(!busy() && !sourcePending_ && sourceValid_);
        exportReport_->setEnabled(!busy() && !sourcePending_ && sourceValid_ && result_.completion_ == "Complete");
        alignRows_->setEnabled(scroll_ && !busy() && !sourcePending_ && sourceValid_ && !multi_);
        alignAll_->setEnabled(currentPair() && editors_.size() >= 3 && !busy());
        for (auto* action : encodingActions_) if (dirty_) action->setEnabled(false);
        exitCode_ = !inputError_.isEmpty() || !inputSaveError_.isEmpty() ? 2 : dirty_ || sourcePending_ ? 3 : result_.exitCode_;
        if (savingInput_ >= 0) state_->setText(tr("FILE %1 저장 중…").arg(savingInput_ + 1));
        else if (!inputSaveError_.isEmpty()) state_->setText(inputSaveError_);
        else if (!inputError_.isEmpty()) state_->setText(inputError_);
        else if (sourcePending_) state_->setText(tr("화면에 반영됨 — 차이 다시 계산 중…"));
        else if (dirty_) state_->setText(tr("수정됨 — FILE별 저장 또는 Ctrl+S로 현재 문서 저장"));
        else if (result_.textComparison_["encodingOnly"].toBool()) state_->setText(tr("내용 동일 — 인코딩/BOM만 다름"));
        else state_->setText(tr("비교 — %1").arg(result_.outcome_));
    }
    void CompareWindow::applyInputChunk(std::uint64_t Id, std::uint64_t Revision, Resolution Choice)
    {
        const auto* pair = currentPair(); if (!sourceEditing_ || !pair || Revision != bufferGeneration_ || !Id || Id > pair->changes_.size()) return;
        if (Choice != Resolution::Left && Choice != Resolution::Right) return;
        const auto change = pair->changes_[Id - 1]; const int from = Choice == Resolution::Left ? 0 : 1, to = 1 - from;
        auto* source = editors_[pair->inputs_[from]]; auto* target = editors_[pair->inputs_[to]];
        if (target->send(SCI_GETREADONLY)) return;
        const auto position = [](ScintillaEditBase* Editor, std::size_t Line) { const auto at = Editor->send(SCI_POSITIONFROMLINE, Line); return at < 0 ? Editor->send(SCI_GETLENGTH) : at; };
        const auto a = position(source, from ? change.otherBegin_ : change.begin_), b = position(source, from ? change.otherEnd_ : change.end_);
        const auto begin = position(target, to ? change.otherBegin_ : change.begin_), end = position(target, to ? change.otherEnd_ : change.end_);
        const auto replacement = bytes(source).mid(a, b - a); activeInput_ = pair->inputs_[to];
        target->send(SCI_BEGINUNDOACTION); target->send(SCI_SETTARGETRANGE, begin, end);
        target->send(SCI_REPLACETARGET, uptr_t(replacement.size()), reinterpret_cast<sptr_t>(replacement.constData())); target->send(SCI_ENDUNDOACTION);
        target->send(SCI_SETSEL, begin, begin + replacement.size()); target->setFocus(Qt::OtherFocusReason); updateMerge();
    }
    void CompareWindow::saveInput(int Index)
    {
        if (!sourceEditing_ || busy() || Index < 0 || Index >= result_.inputs_.size() || !editors_[Index]->send(SCI_GETMODIFY)) return;
        savingInput_ = Index; activeInput_ = Index; inputSaveError_.clear();
        const auto text = bytes(editors_[Index]);
        for (auto* editor : editors_) editor->send(SCI_SETREADONLY, 1);
        inputSaveWatcher_.setFuture(QtConcurrent::run([original = result_.inputs_[Index], text, timeout = request_.timeoutMs_] {
            Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            return SaveInputBuffer(original, text, limits);
        }));
        updateMerge();
    }
    void CompareWindow::inputUndo(bool Redo)
    {
        if (busy()) return;
        auto* editor = sourceEditing_ && !editors_.isEmpty() ? editors_[activeInput_] : output_;
        if (editor) editor->send(Redo ? SCI_REDO : SCI_UNDO);
    }
}
