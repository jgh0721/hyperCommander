#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTabBar>

namespace hc::document
{
    // Chapter title and filename share one native tab, including its close button and keyboard navigation.
    class EpubSourceTabs final : public QTabBar
    {
    public:
        using QTabBar::QTabBar;
        QRect PrimaryTextRect(int Index) const;
        QFont FilenameFont() const;

    protected:
        QSize tabSizeHint(int Index) const override;
        QSize minimumTabSizeHint(int Index) const override { return tabSizeHint(Index); }
        void paintEvent(QPaintEvent* Event) override;
    };
}
