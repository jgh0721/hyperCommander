#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFrame>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace hc::document
{
    /// 상태표시줄 선택 항목이 여는 팝업(계획 5.4, 시안 POP). 호출부는 내용을 위에서 아래로 쌓은 뒤 ShowAbove() 를 부른다.
    ///
    /// - Page 가 nullptr 이면 Root() 에 넣는다. AddTabs() 가 돌려준 쪽에 넣으면 그 탭을 고를 때만 보인다.
    /// - 칩 · 목록 · 체크는 선택만 바꾸고 팝업을 닫지 않는다. 닫으려면 콜백에서 close() 를 부른다.
    ///   동작 단추는 팝업을 닫은 뒤 콜백을 부르고, 입력 칸은 OnAccept 가 true 를 돌려주면 닫는다.
    /// - Esc 는 닫고 SetFocusReturn() 위젯(기본: 기준 항목)으로 포커스를 돌린다. Tab 은 팝업 안을 돈다.
    ///   방향키는 탭 · 칩 · 목록 안을 움직이고, Enter · Space 는 포커스가 있는 선택을 실행한다.
    /// - 닫히면 스스로 지워진다(WA_DeleteOnClose).
    class DocumentStatusPopup final : public QFrame
    {
        Q_OBJECT
    public:
        /// 카드 폭과 상태표시줄 위 간격(DIP).
        static constexpr int PanelWidth = 340;
        static constexpr int BarGap     = 6;

        explicit DocumentStatusPopup( QWidget* Anchor );
        ~DocumentStatusPopup() override;

        /// 팝업을 만들어 돌려준다. 호출부가 내용을 넣고 ShowAbove() 를 부른다.
        static DocumentStatusPopup* Open( QWidget* Anchor, const QString& Heading );

        void                SetTitle( const QString& Heading );
        QString             Title() const;
        /// 탭 없이 쓰는 내용의 쪽.
        QWidget*            Root() const;
        QWidget*            AnchorWidget() const;

        /// 흐린 11.5 px 설명(줄바꿈).
        QLabel*             AddNote( QWidget* Page, const QString& Text );
        /// 세그먼트 탭. 탭마다 쪽 하나를 돌려준다. 탭을 바꾸면 그 쪽만 보이고 카드 높이를 다시 맞춘다.
        QList< QWidget* >   AddTabs( const QStringList& Labels, int Current );
        /// 배타 칩 한 줄(넘치면 줄바꿈). Current 가 -1 이면 고른 칩이 없다. OnActivate 는 칩 순번을 받는다.
        void                AddChips( QWidget* Page, const QStringList& Labels, int Current, std::function< void( int ) > OnActivate );
        QLineEdit*          AddSearch( QWidget* Page, const QString& Placeholder );
        /// Items 는 ( 글자, 오른쪽 흐린 설명 ). Filter 를 주면 그 칸의 글자로 거르고, 그 칸의 Enter · 위아래 키도 목록에 간다.
        /// OnActivate 는 Items 의 순번을 받는다(거른 뒤에도 같다).
        QListWidget*        AddList( QWidget* Page, const QList< QPair< QString, QString > >& Items, int Current,
                                     std::function< void( int ) > OnActivate, QLineEdit* Filter = nullptr );
        QPushButton*        AddAction( QWidget* Page, const QString& Text, const QString& ShortcutText, std::function< void() > OnTrigger );
        QCheckBox*          AddCheck( QWidget* Page, const QString& Text, bool Checked, std::function< void( bool ) > OnToggle );
        QLabel*             AddWarning( QWidget* Page, const QString& Text );
        /// Enter 로 OnAccept 를 부른다. true 면 닫고, false 면 칸을 오류 모양으로 두고 남는다. Hint 는 칸 안 오른쪽에 흐리게.
        QLineEdit*          AddInput( QWidget* Page, const QString& Text, const QString& Hint, std::function< bool( const QString& ) > OnAccept );

        void                SetFocusReturn( QWidget* Widget );
        /// 카드의 아래 끝을 상태표시줄 위 BarGap 에 맞추고, 왼쪽은 기준 항목에 맞춰 창 안으로 보정해 띄운다.
        /// 창 안 위쪽에 자리가 없으면 상태표시줄 아래에 띄운다.
        void                ShowAbove();
        /// 그린 카드의 화면 좌표(창에는 그림자 여백이 더 있다).
        QRect               PanelGeometry() const;

    protected:
        void                paintEvent( QPaintEvent* Event ) override;
        void                keyPressEvent( QKeyEvent* Event ) override;
        void                mousePressEvent( QMouseEvent* Event ) override;
        bool                eventFilter( QObject* Object, QEvent* Event ) override;

    private:
        struct PageState
        {
            QVBoxLayout*    layout_ = nullptr;
            int             bottom_ = 0;        // 마지막 항목의 아래 여백 — 다음 항목의 위 여백과 겹친다(CSS 여백 겹침)
        };

        QWidget*            makePage( int Bottom );
        QWidget*            pageOf( QWidget* Page ) const;
        void                place( QWidget* Page, QWidget* Item, int Top, int Bottom );
        void                setTail( PageState& State );
        void                watch( QWidget* Widget );
        void                applyTheme();
        void                dismiss();
        void                focusFirst();
        void                fit();
        void                selectTab( QWidget* Strip, int Index );
        QRect               panelRect() const;

        QPointer< QWidget > anchor_;
        QPointer< QWidget > focusReturn_;
        QLabel*             title_ = nullptr;
        QWidget*            root_  = nullptr;
        QHash< QWidget*, PageState >                    pages_;
        QHash< QObject*, std::function< void() > >     activators_;  // Enter(목록은 Space 도)로 부를 동작
        QHash< QObject*, QListWidget* >                 filters_;     // 검색 칸 → 거르는 목록
    };
}
