#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include "cmnTypeDefs.hpp"

namespace nsHC
{
    class QBaseUI : public QDialog
    {
        Q_OBJECT
    public:
        QBaseUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::Dialog );

        void                            SetMovableUI( bool isMovable );
        bool                            GetMovableUI();

        void                            SetResizableUI( bool isResizable );
        bool                            GetResizableUI() const;

    public slots:
        virtual void                    OnColorSchemeChanged( const TyColorScheme& ColorScheme );
        virtual void                    OnLanguageChanged();

    signals:
        void                            NotifyUiReady();

    protected:
        void                            mousePressEvent( QMouseEvent* ev ) override;
        void                            mouseMoveEvent( QMouseEvent* ev ) override;
        void                            mouseReleaseEvent( QMouseEvent* ev ) override;
        void                            showEvent( QShowEvent* event ) override;

    private:
        QFrame*                         titleBar_ = nullptr;
        QLabel*                         titleBarIcon_ = nullptr;
        QLabel*                         titleBarText_ = nullptr;

        // 현재, 창을 끌기 중 인지 여부
        bool                            isDragWindow_ = false;
        // 창 끌기 가능 여부
        bool                            isDragableWindow_ = true;
        // 창 크기 조절 가능 여부
        bool                            isResizableWindow_ = false;
        QPoint                          ptDragPosition;
        // 프레임이 없는 창에서 창을 끌 때 제목표시줄로 사용할 높이, 단위 : px, 기본값 : 70
        int                             nDragableHeightPx_ = 48;

    };

} // nsHC