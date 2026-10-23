#include "stdafx.h"
#include "multiPaneAlignment.hpp"
#include "scintillaPadding.hpp"
#include <ScintillaEditBase.h>
#include <QtConcurrentRun>

namespace hc::compare
{
    MultiPaneAlignment::MultiPaneAlignment(QVector<ScintillaEditBase*> Editors, QObject* Parent)
        : QObject(Parent), editors_(std::move(Editors)), text_(editors_.size()), known_(editors_.size()), dirty_(editors_.size(), true)
    {
        timer_.setSingleShot(true); timer_.setInterval(120);
        connect(&timer_, &QTimer::timeout, this, &MultiPaneAlignment::launch);
        for (qsizetype side = 0; side < editors_.size(); ++side)
        {
            auto* editor = editors_[side]; editor->send(SCI_SETENDATLASTLINE, 0);
            connect(editor, &ScintillaEditBase::verticalScrolled, this, [this, side] { scrolled(side); });
            connect(editor, &ScintillaEditBase::horizontalScrolled, this, [this, side] { scrolled(side); });
            connect(editor, &ScintillaEditBase::updateUi, this, [this, side] { scrolled(side); });
            connect(editor, &ScintillaEditBase::modified, this, [this, side](Scintilla::ModificationFlags Type) {
                // Remove document-line keyed padding before line insertion/deletion
                // changes Scintilla's contraction state; neither operation edits bytes.
                if (int(Type) & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE)) { clear(); return; }
                if (!(int(Type) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;
                dirty_[side] = true; master_ = side; Refresh();
            });
        }
        connect(&watcher_, &QFutureWatcher<Computed>::finished, this, [this] {
            const auto result = watcher_.result();
            if (result.generation_ != generation_) { timer_.start(); return; }
            map_ = result.map_; error_ = result.error_;
            if (map_) apply();
            if (changed_) changed_();
        });
        remember(); Refresh();
    }
    MultiPaneAlignment::~MultiPaneAlignment()
    {
        timer_.stop(); if (canceled_) canceled_->store(true); watcher_.waitForFinished(); clear();
    }
    void MultiPaneAlignment::remember()
    {
        for (qsizetype i = 0; i < editors_.size(); ++i) known_[i] = {editors_[i]->send(SCI_GETFIRSTVISIBLELINE), editors_[i]->send(SCI_GETXOFFSET)};
    }
    void MultiPaneAlignment::clear()
    {
        applying_ = true;
        for (auto* editor : editors_)
        {
            const auto line = editor->send(SCI_DOCLINEFROMVISIBLE, editor->send(SCI_GETFIRSTVISIBLELINE));
            editor->send(ClearPaddingRows);
            editor->send(SCI_SETFIRSTVISIBLELINE, std::max<sptr_t>(0, editor->send(SCI_VISIBLEFROMDOCLINE, line)));
        }
        map_.reset(); remember(); applying_ = false;
    }
    void MultiPaneAlignment::Refresh()
    {
        ++generation_; if (canceled_) canceled_->store(true);
        clear(); error_.clear(); timer_.start(); if (changed_) changed_();
    }
    void MultiPaneAlignment::launch()
    {
        if (watcher_.isRunning()) return;
        for (qsizetype i = 0; i < editors_.size(); ++i)
        {
            if (!dirty_[i]) continue;
            const auto length = editors_[i]->send(SCI_GETLENGTH);
            text_[i].resize(qsizetype(length) + 1); editors_[i]->send(SCI_GETTEXT, uptr_t(text_[i].size()), reinterpret_cast<sptr_t>(text_[i].data())); text_[i].resize(length); dirty_[i] = false;
        }
        canceled_ = std::make_shared<std::atomic_bool>(false);
        watcher_.setFuture(QtConcurrent::run([texts = text_, generation = generation_, canceled = canceled_] {
            Computed result; result.generation_ = generation;
            try
            {
                Budget limits; limits.canceled_ = canceled.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                std::vector<std::string_view> views; for (const auto& text : texts) views.emplace_back(text.constData(), std::size_t(text.size()));
                auto map = MultiAlignmentMap::Build(views, limits);
                auto decoration = limits;
                decoration.deadline_ = std::min(limits.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
                result.map_ = std::make_shared<MultiAlignmentMap>(map.WithSimilarLines(views, decoration));
            }
            catch (const std::exception& error) { result.error_ = QString::fromUtf8(error.what()); }
            return result;
        }));
    }
    void MultiPaneAlignment::apply()
    {
        applying_ = true;
        const auto top = editors_[master_]->send(SCI_GETFIRSTVISIBLELINE);
        const auto line = editors_[master_]->send(SCI_DOCLINEFROMVISIBLE, top);
        for (qsizetype side = 0; side < editors_.size(); ++side)
        {
            auto* editor = editors_[side]; std::size_t leading = 0;
            std::vector<std::pair<std::size_t, std::size_t>> gaps;
            for (const auto& run : map_->Runs())
            {
                const auto count = run.rowEnd_ - run.rowBegin_ - (run.end_[side] - run.begin_[side]); if (!count) continue;
                const auto end = run.end_[side];
                if (!end) leading += count;
                else if (!gaps.empty() && gaps.back().first == end - 1) gaps.back().second += count;
                else gaps.emplace_back(end - 1, count);
            }
            const auto lines = map_->LineCount(side);
            if (lines && std::size_t(editor->send(SCI_GETLINECOUNT)) == lines)
            {
                if (!gaps.empty() && gaps.back().first == lines - 1) ++gaps.back().second;
                else gaps.emplace_back(lines - 1, 1);
            }
            for (const auto& gap : gaps) editor->send(SetTrailingRows, gap.first, gap.second);
            editor->send(SetLeadingRows, leading);
        }
        const auto projection = map_->Pair(master_, master_ == 0 ? 1 : 0);
        const auto row = top == 0 ? 0 : projection.DisplayRow(0, std::min(std::size_t(std::max<sptr_t>(0, line)), map_->LineCount(master_)));
        if (enabled_) for (auto* editor : editors_) editor->send(SCI_SETFIRSTVISIBLELINE, row);
        remember(); applying_ = false;
    }
    void MultiPaneAlignment::SetEnabled(bool Enabled)
    {
        if (enabled_ == Enabled) return;
        enabled_ = Enabled; remember();
        if (enabled_ && map_) { known_[master_] = {-1, -1}; scrolled(master_); }
    }
    void MultiPaneAlignment::scrolled(qsizetype Side)
    {
        if (applying_) return;
        const View current{editors_[Side]->send(SCI_GETFIRSTVISIBLELINE), editors_[Side]->send(SCI_GETXOFFSET)};
        const auto old = known_[Side]; known_[Side] = current;
        if (!map_ || !enabled_ || (current.top_ == old.top_ && current.x_ == old.x_)) return;
        applying_ = true; master_ = Side;
        for (qsizetype i = 0; i < editors_.size(); ++i)
        {
            if (i == Side) continue;
            if (current.top_ != old.top_) editors_[i]->send(SCI_SETFIRSTVISIBLELINE, current.top_);
            if (current.x_ != old.x_) editors_[i]->send(SCI_SETXOFFSET, current.x_);
        }
        remember(); applying_ = false;
    }
    std::shared_ptr<const AlignmentMap> MultiPaneAlignment::Pair(ScintillaEditBase* Left, ScintillaEditBase* Right) const
    {
        const auto a = editors_.indexOf(Left), b = editors_.indexOf(Right);
        if (!map_ || a < 0 || b < 0 || a == b) return {};
        return std::make_shared<AlignmentMap>(map_->Pair(a, b));
    }
    void MultiPaneAlignment::Reveal(ScintillaEditBase* Editor, std::size_t Line)
    {
        const auto side = editors_.indexOf(Editor); if (!map_ || side < 0) return;
        const auto pair = map_->Pair(side, side == 0 ? 1 : 0);
        const auto row = pair.DisplayRow(0, std::min(Line, map_->LineCount(side))); applying_ = true;
        for (auto* editor : editors_) editor->send(SCI_SETFIRSTVISIBLELINE, row);
        master_ = side; remember(); applying_ = false;
    }
}
