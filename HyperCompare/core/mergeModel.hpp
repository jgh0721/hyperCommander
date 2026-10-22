#pragma once
#include "compare.hpp"
#include <memory>

namespace hc::compare
{
    enum class Resolution { Unresolved, Left, Right, Base, LeftThenRight, RightThenLeft, Manual };
    struct BlockState
    {
        std::size_t begin_ = 0, end_ = 0;
        Resolution resolution_ = Resolution::Unresolved;
        bool mapped_ = true;
        bool operator==(const BlockState&) const = default;
    };
    struct MergeBlock
    {
        std::uint64_t id_ = 0;
        std::size_t leftBegin_ = 0, rightBegin_ = 0, baseBegin_ = 0;
        std::size_t leftLine_ = 0, rightLine_ = 0, baseLine_ = 0;
        std::string left_, right_, base_;
    };
    struct MergePlan
    {
        bool hasBase_ = false;
        std::vector<MergeBlock> blocks_;
        std::vector<BlockState> initial_;
    };
    MergePlan TwoWayPlan(std::string_view Left, std::string_view Right, const std::vector<Edit>& Edits,
        Resolution Seed = Resolution::Unresolved);
    MergePlan ThreeWayPlan(std::string_view Base, std::string_view Left, std::string_view Right, const MergeResult& Merged);
    struct BlockEdit
    {
        std::uint64_t id_ = 0, revision_ = 0;
        std::size_t begin_ = 0, end_ = 0;
        Resolution resolution_ = Resolution::Unresolved;
        std::string replacement_;
    };
    // Locations are UTF-8 byte ranges. Undo restores state but never rewinds revision_.
    class MergeModel
    {
    public:
        explicit MergeModel(std::shared_ptr<const MergePlan> Plan);
        const MergePlan& Plan() const { return *plan_; }
        const std::vector<BlockState>& State() const { return state_; }
        std::uint64_t Revision() const { return revision_; }
        std::size_t UnresolvedCount() const;
        BlockEdit Prepare(std::uint64_t Id, std::uint64_t ExpectedRevision, Resolution Choice) const;
        void Apply(const BlockEdit& Edit);
        void TextChanged(std::size_t Position, std::size_t Removed, std::size_t Inserted);
        void SelectAll(Resolution Choice);
        void Restore(const std::vector<BlockState>& State);
        void Invalidate();
    private:
        std::size_t index(std::uint64_t Id) const;
        std::shared_ptr<const MergePlan> plan_;
        std::vector<BlockState> state_;
        std::uint64_t revision_ = 1;
    };
}
