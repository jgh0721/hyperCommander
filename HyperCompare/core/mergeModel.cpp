#include "stdafx.h"
#include "mergeModel.hpp"

namespace hc::compare
{
    namespace
    {
        std::uint64_t nextRevision()
        { static std::atomic_uint64_t value{0}; return value.fetch_add(1, std::memory_order_relaxed) + 1; }
        std::vector<std::size_t> offsets(std::string_view Text)
        {
            std::vector<std::size_t> result{0};
            for (const auto line : SplitLines(Text)) result.push_back(result.back() + line.size());
            return result;
        }
        std::size_t shift(std::size_t Value, std::size_t Removed, std::size_t Inserted)
        { return Inserted >= Removed ? Value + (Inserted - Removed) : Value - (Removed - Inserted); }
    }
    MergePlan TwoWayPlan(std::string_view Left, std::string_view Right, const std::vector<Edit>& Edits, Resolution Seed)
    {
        MergePlan result; const auto left = offsets(Left), right = offsets(Right);
        for (const auto& edit : Edits)
        {
            MergeBlock block; block.id_ = result.blocks_.size() + 1;
            block.leftLine_ = edit.begin_; block.rightLine_ = edit.otherBegin_;
            block.leftBegin_ = left.at(edit.begin_); block.rightBegin_ = right.at(edit.otherBegin_);
            block.left_ = Left.substr(block.leftBegin_, left.at(edit.end_) - block.leftBegin_);
            block.right_ = edit.replacement_;
            const auto start = Seed == Resolution::Right ? block.rightBegin_ : block.leftBegin_;
            const auto length = Seed == Resolution::Right ? block.right_.size() : block.left_.size();
            result.initial_.push_back({start, start + length, Seed}); result.blocks_.push_back(std::move(block));
        }
        return result;
    }
    MergePlan ThreeWayPlan(std::string_view Base, std::string_view Left, std::string_view Right, const MergeResult& Merged)
    {
        MergePlan result; result.hasBase_ = true;
        const auto base = offsets(Base), left = offsets(Left), right = offsets(Right);
        for (const auto& conflict : Merged.conflicts_)
        {
            MergeBlock block; block.id_ = result.blocks_.size() + 1;
            block.leftLine_ = conflict.leftBegin_; block.rightLine_ = conflict.rightBegin_; block.baseLine_ = conflict.baseBegin_;
            block.leftBegin_ = left.at(block.leftLine_); block.rightBegin_ = right.at(block.rightLine_); block.baseBegin_ = base.at(block.baseLine_);
            block.left_ = conflict.left_; block.right_ = conflict.right_; block.base_ = conflict.base_;
            if (Left.substr(block.leftBegin_, block.left_.size()) != block.left_ || Right.substr(block.rightBegin_, block.right_.size()) != block.right_)
                throw Failure("InvalidMergeLocation");
            result.initial_.push_back({conflict.resultBegin_, conflict.resultEnd_}); result.blocks_.push_back(std::move(block));
        }
        return result;
    }
    MergeModel::MergeModel(std::shared_ptr<const MergePlan> Plan) : plan_(std::move(Plan))
    {
        if (!plan_) throw Failure("MissingMergePlan");
        state_ = plan_->initial_; revision_ = nextRevision();
    }
    std::size_t MergeModel::index(std::uint64_t Id) const
    {
        if (!Id || Id > state_.size() || plan_->blocks_[Id - 1].id_ != Id) throw Failure("UnknownMergeBlock");
        return static_cast<std::size_t>(Id - 1);
    }
    std::size_t MergeModel::UnresolvedCount() const
    {
        return std::count_if(state_.begin(), state_.end(), [](const auto& block) { return block.resolution_ == Resolution::Unresolved; });
    }
    BlockEdit MergeModel::Prepare(std::uint64_t Id, std::uint64_t ExpectedRevision, Resolution Choice) const
    {
        if (ExpectedRevision != revision_) throw Failure("StaleMergeDecision");
        const auto at = index(Id); const auto& state = state_[at]; const auto& block = plan_->blocks_[at];
        if (!state.mapped_) throw Failure("AmbiguousMergeLocation");
        if (Choice == Resolution::Unresolved || (Choice == Resolution::Base && !plan_->hasBase_)) throw Failure("InvalidMergeChoice");
        BlockEdit result{Id, revision_, state.begin_, state.end_, Choice};
        switch (Choice)
        {
        case Resolution::Left: result.replacement_ = block.left_; break;
        case Resolution::Right: result.replacement_ = block.right_; break;
        case Resolution::Base: result.replacement_ = block.base_; break;
        case Resolution::LeftThenRight: result.replacement_ = block.left_ + block.right_; break;
        case Resolution::RightThenLeft: result.replacement_ = block.right_ + block.left_; break;
        case Resolution::Manual: break;
        default: throw Failure("InvalidMergeChoice");
        }
        return result;
    }
    void MergeModel::Apply(const BlockEdit& Edit)
    {
        const auto verified = Prepare(Edit.id_, Edit.revision_, Edit.resolution_);
        if (verified.begin_ != Edit.begin_ || verified.end_ != Edit.end_ || verified.replacement_ != Edit.replacement_) throw Failure("InvalidMergeDecision");
        const auto at = index(Edit.id_); auto& target = state_[at];
        if (Edit.resolution_ != Resolution::Manual)
        {
            const auto removed = target.end_ - target.begin_, inserted = Edit.replacement_.size();
            for (std::size_t i = at + 1; i < state_.size(); ++i)
            { state_[i].begin_ = shift(state_[i].begin_, removed, inserted); state_[i].end_ = shift(state_[i].end_, removed, inserted); }
            target.end_ = target.begin_ + inserted;
        }
        target.resolution_ = Edit.resolution_; revision_ = nextRevision();
    }
    void MergeModel::TextChanged(std::size_t Position, std::size_t Removed, std::size_t Inserted)
    {
        const auto end = Position + Removed;
        for (auto& block : state_)
        {
            if (end <= block.begin_ && !(Removed == 0 && Position == block.begin_))
            { block.begin_ = shift(block.begin_, Removed, Inserted); block.end_ = shift(block.end_, Removed, Inserted); }
            else if (Position >= block.end_ && !(Position == block.begin_ && block.begin_ == block.end_)) continue;
            else
            {
                block.resolution_ = Resolution::Unresolved;
                if (Position >= block.begin_ && end <= block.end_) block.end_ = shift(block.end_, Removed, Inserted);
                else
                {
                    block.mapped_ = false;
                    const auto map = [&](std::size_t Point, bool Right) {
                        if (Point < Position) return Point;
                        if (Point >= end) return shift(Point, Removed, Inserted);
                        return Position + (Right ? Inserted : 0);
                    };
                    block.begin_ = map(block.begin_, false); block.end_ = map(block.end_, true);
                }
            }
        }
        for (std::size_t i = 1; i < state_.size(); ++i)
            if (state_[i - 1].end_ > state_[i].begin_) { state_[i - 1].mapped_ = false; state_[i].mapped_ = false; }
        revision_ = nextRevision();
    }
    void MergeModel::SelectAll(Resolution Choice)
    {
        if (Choice != Resolution::Left && Choice != Resolution::Right && Choice != Resolution::Manual) throw Failure("InvalidMergeChoice");
        for (std::size_t i = 0; i < state_.size(); ++i)
        {
            auto& state = state_[i]; const auto& block = plan_->blocks_[i]; state.resolution_ = Choice;
            if (Choice == Resolution::Manual) continue;
            state.begin_ = Choice == Resolution::Left ? block.leftBegin_ : block.rightBegin_;
            state.end_ = state.begin_ + (Choice == Resolution::Left ? block.left_.size() : block.right_.size()); state.mapped_ = true;
        }
        revision_ = nextRevision();
    }
    void MergeModel::Restore(const std::vector<BlockState>& State)
    {
        if (State.size() != state_.size()) throw Failure("InvalidMergeHistory");
        state_ = State; revision_ = nextRevision();
    }
    void MergeModel::Invalidate()
    { for (auto& state : state_) { state.mapped_ = false; state.resolution_ = Resolution::Unresolved; } revision_ = nextRevision(); }
}
