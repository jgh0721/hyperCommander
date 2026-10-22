#pragma once
#include "core/overviewMap.hpp"
#include <QWidget>
#include <QPointer>
#include <QVector>
#include <memory>

class ScintillaEditBase;
namespace hc::compare
{
    class OverviewBar final : public QWidget
    {
        Q_OBJECT
    public:
        explicit OverviewBar(QWidget* Parent);
        void SetSession(ScintillaEditBase* Left, ScintillaEditBase* Right, std::shared_ptr<const AlignmentMap> Map);
        void SetAligned(bool Aligned);
        void SetDark(bool Dark);
        void FocusSource();
        QSize sizeHint() const override;
    protected:
        void paintEvent(QPaintEvent* Event) override;
        void resizeEvent(QResizeEvent* Event) override;
        void changeEvent(QEvent* Event) override;
        void mousePressEvent(QMouseEvent* Event) override;
        void mouseMoveEvent(QMouseEvent* Event) override;
        void keyPressEvent(QKeyEvent* Event) override;
        void focusInEvent(QFocusEvent* Event) override;
        void focusOutEvent(QFocusEvent* Event) override;
    private:
        QRect track(unsigned Side) const;
        void rebuild();
        void navigate(int Y);
        void scrollTo(std::intptr_t Row);
        std::array<QPointer<ScintillaEditBase>, 2> editors_;
        QVector<QMetaObject::Connection> connections_;
        std::shared_ptr<const AlignmentMap> map_;
        OverviewRaster raster_;
        unsigned active_ = 0;
        bool aligned_ = false, dark_ = false;
    };
}
