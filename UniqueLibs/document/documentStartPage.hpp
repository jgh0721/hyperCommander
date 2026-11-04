#pragma once

/// 고유 라이브러리
#include "recentDocuments.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>

class QAbstractButton;
class QLabel;
class QMenu;
class QMimeData;
class QVBoxLayout;

namespace hc::document
{
    /// 문서가 없을 때 보이는 시작 화면(독립 HyperEditor 전용, 계획 6.1).
    /// 가운데 560 DIP 상자에 제목 · 안내 · [열기…] [새 문서] · 최근 항목 5행을 둔다.
    /// 행은 위 · 아래 · Enter, 숫자 1–5 로 연다. 로컬 파일 끌어다 놓기를 받는다.
    class DocumentStartPage final : public QWidget
    {
        Q_OBJECT
    public:
        static constexpr int MaxRows = 5, BoxWidth = 560;

        explicit DocumentStartPage( RecentDocumentStore* Store, QWidget* Parent = nullptr );
        ~DocumentStartPage() override;

        /// 보이는 최근 항목 행 수(0–5).
        int RowCount() const;
        /// 우클릭 메뉴를 만든다(호출한 쪽이 소유). Pos 가 행 위면 행 메뉴, 아니면 빈 영역 메뉴.
        QMenu* CreateContextMenu( const QPoint& Pos, QWidget* Parent = nullptr );
        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;

    signals:
        void OpenRequested( const hc::document::RecentDocument& Document );
        void OpenFileRequested();
        void NewDocumentRequested();
        void ShowAllRequested();
        void FilesDropped( const QStringList& Paths );

    protected:
        bool eventFilter( QObject* Watched, QEvent* Event ) override;
        void changeEvent( QEvent* Event ) override;
        void contextMenuEvent( QContextMenuEvent* Event ) override;
        void dragEnterEvent( QDragEnterEvent* Event ) override;
        void dragMoveEvent( QDragMoveEvent* Event ) override;
        void dropEvent( QDropEvent* Event ) override;
        void hideEvent( QHideEvent* Event ) override;
        void keyPressEvent( QKeyEvent* Event ) override;
        void paintEvent( QPaintEvent* Event ) override;
        void resizeEvent( QResizeEvent* Event ) override;
        void showEvent( QShowEvent* Event ) override;

    private:
        void rebuild();
        void relayout();
        void applyTheme();
        void applyFonts();
        void openRow( int Index );
        void focusRow( int Index );
        int  focusedRow() const;
        int  rowAt( const QPoint& Pos ) const;
        void refreshMissing( const QString& Path );
        void confirmClear();
        static QStringList localFiles( const QMimeData* Data );

        QPointer< RecentDocumentStore > store_;
        RecentPathProbe*                probe_      = nullptr;
        QWidget*                        box_        = nullptr;
        QLabel*                         title_      = nullptr;
        QLabel*                         hint_       = nullptr;
        QLabel*                         header_     = nullptr;
        QLabel*                         empty_      = nullptr;
        QAbstractButton*                open_       = nullptr;
        QAbstractButton*                new_        = nullptr;
        QAbstractButton*                showAll_    = nullptr;
        QList< QAbstractButton* >       rows_;
        QList< RecentDocument >         documents_;
        QPointer< QDialog >             confirm_;
    };
}
