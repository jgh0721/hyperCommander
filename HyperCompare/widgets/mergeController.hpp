#pragma once
#include "core/mergeModel.hpp"
#include <QObject>
#include <QByteArray>
#include <functional>
#include <map>

class ScintillaEditBase;
namespace hc::compare
{
    class MergeController final : public QObject
    {
    public:
        MergeController(ScintillaEditBase* Editor, std::shared_ptr<const MergePlan> Plan, QObject* Parent,
            std::size_t HistoryBytes = 64 * 1024 * 1024);
        const MergeModel& Model() const { return model_; }
        void Choose(std::uint64_t Id, std::uint64_t Revision, Resolution Choice);
        void ChooseAll(Resolution Choice, const QByteArray& Text = {});
        void Refresh(std::shared_ptr<const MergePlan> Plan);
        void MarkSaved();
        bool Dirty() const;
        bool Saved() const;
        std::function<void()> changed_;
    private:
        void modified(int Type, std::size_t Position, std::size_t Length);
        void remember();
        void replace(std::size_t Begin, std::size_t End, const QByteArray& Text, MergeModel Next, bool TextChange);
        ScintillaEditBase* editor_;
        MergeModel model_;
        struct History { std::vector<BlockState> state_; bool reviewRequired_ = false; };
        std::map<int, History> history_;
        std::size_t historyBytes_ = 0;
        std::size_t historyLimit_;
        bool applying_ = false, saved_ = false;
    };
}
