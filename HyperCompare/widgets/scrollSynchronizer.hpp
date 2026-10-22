#pragma once
#include "core/alignmentMap.hpp"
#include <QObject>
#include <memory>

class ScintillaEditBase;
namespace hc::compare
{
    class ScrollSynchronizer final : public QObject
    {
    public:
        ScrollSynchronizer(ScintillaEditBase* Left, ScintillaEditBase* Right,
            std::shared_ptr<const AlignmentMap> Map, QObject* Parent);
        void SetEnabled(bool Enabled);
        void SetMap(std::shared_ptr<const AlignmentMap> Map);
        void SetAligned(bool Aligned);
        void ClearPadding();
        void Synchronize(unsigned FromSide);
        void Reveal(std::size_t LeftLine, std::size_t RightLine);
    private:
        struct View { std::intptr_t top_ = 0, x_ = 0; };
        View view(unsigned Side) const;
        void remember();
        void changed(unsigned Side);
        void move(unsigned Side, std::size_t Line);
        void applyPadding();
        std::array<ScintillaEditBase*, 2> editors_;
        std::array<View, 2> known_{};
        std::shared_ptr<const AlignmentMap> map_;
        bool enabled_ = false, applying_ = false, aligned_ = false;
    };
}
