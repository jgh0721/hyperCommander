#pragma once
#include "core/mergeModel.hpp"
#include <QWidget>
#include <array>
#include <functional>
#include <vector>

class ScintillaEditBase;
class QToolButton;
namespace hc::compare
{
    struct GutterChunk
    {
        std::uint64_t id_ = 0;
        std::array<std::size_t, 2> begin_{}, end_{};
        bool enabled_ = true, resolved_ = false, conflict_ = false;
        bool operator==(const GutterChunk&) const = default;
    };
    // Only visible chunks have controls; positions are UTF-8 byte boundaries.
    class ChunkGutter final : public QWidget
    {
        Q_OBJECT
    public:
        ChunkGutter(ScintillaEditBase* Left, ScintillaEditBase* Right, int SourceSide, QWidget* Parent);
        void SetChunks(std::vector<GutterChunk> Chunks, std::uint64_t Revision);
        void Refresh();
        int SourceSide() const { return sourceSide_; }
        std::function<void(std::uint64_t, std::uint64_t, Resolution)> choose_;
    protected:
        void paintEvent(QPaintEvent*) override;
        void resizeEvent(QResizeEvent*) override;
        void changeEvent(QEvent*) override;
        bool eventFilter(QObject*, QEvent*) override;
    private:
        void arrange();
        int y(unsigned Side, std::size_t Position) const;
        std::array<ScintillaEditBase*, 2> editors_;
        std::vector<GutterChunk> chunks_;
        struct Row { std::size_t index_; int leftTop_, leftBottom_, rightTop_, rightBottom_; };
        std::vector<Row> rows_;
        std::vector<std::array<QToolButton*, 3>> buttons_;
        std::uint64_t revision_ = 0;
        int sourceSide_ = -1;
        bool queued_ = false;
    };
}
