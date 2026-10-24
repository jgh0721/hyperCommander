#pragma once
#include "core/alignmentMap.hpp"
#include "core/inlineDiff.hpp"
#include <QObject>
#include <QFutureWatcher>
#include <QTimer>
#include <QVector>
#include <QByteArray>
#include <functional>
#include <memory>

class ScintillaEditBase;
namespace hc::compare
{
    class MultiPaneAlignment final : public QObject
    {
        Q_OBJECT
    public:
        MultiPaneAlignment(QVector<ScintillaEditBase*> Editors, QObject* Parent, int MemoryMiB = 2048);
        ~MultiPaneAlignment() override;
        void SetEnabled(bool Enabled);
        void Refresh();
        bool Ready() const { return bool(map_); }
        QString Error() const { return error_; }
        std::shared_ptr<const AlignmentMap> Pair(ScintillaEditBase* Left, ScintillaEditBase* Right) const;
        const InlineDiff* Inline(ScintillaEditBase* Left, ScintillaEditBase* Right) const;
        void Reveal(ScintillaEditBase* Editor, std::size_t Line);
        std::function<void()> changed_;
    private:
        void clear();
        void launch();
        void apply();
        void scrolled(qsizetype Side);
        void remember();
        struct View { std::intptr_t top_ = 0, x_ = 0; };
        using InlinePairs = std::array<InlineDiff, 16>;
        std::shared_ptr<const InlinePairs> inline_;
        struct Computed { std::shared_ptr<InlinePairs> inline_; std::shared_ptr<MultiAlignmentMap> map_; QString error_; std::uint64_t generation_ = 0; };
        QVector<ScintillaEditBase*> editors_;
        QVector<QByteArray> text_;
        QVector<View> known_;
        QVector<bool> dirty_;
        QTimer timer_;
        QFutureWatcher<Computed> watcher_;
        std::shared_ptr<std::atomic_bool> canceled_;
        std::shared_ptr<MultiAlignmentMap> map_;
        QString error_;
        std::uint64_t generation_ = 0;
        qsizetype master_ = 0;
        int memoryMiB_ = 2048;
        bool enabled_ = true, applying_ = false;
    };
}
