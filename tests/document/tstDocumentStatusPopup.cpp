#include <QtTest>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include "UniqueLibs/document/documentStatusBar.hpp"
#include "UniqueLibs/document/documentStatusPopup.hpp"
#include "UniqueLibs/document/documentThemeAdapter.hpp"

using namespace hc::document;

namespace
{
    // 계획 5.4 · 시안 POP 의 상태표시줄 선택 항목과 팝업.
    QList< DocumentStatusField > Fields( bool Picker )
    {
        QList< DocumentStatusField > Result{ { "position", QString::fromUtf8( "줄 18 / 545 · 열 5" ), "18:5", {}, "text.goToLine" } };
        DocumentStatusField Encoding{ "encoding", "UTF-8 (BOM)", {}, {}, "text.encoding" };
        Encoding.picker_ = Picker;
        DocumentStatusField Eol{ "eol", "CRLF" };
        Eol.picker_ = Picker;
        Result << Encoding << Eol;
        Result << DocumentStatusField{ "mode", QString::fromUtf8( "편집" ), {}, {}, {}, true, true };
        return Result;
    }

    // 작업 영역과 같은 구조: 본문 아래에 상태표시줄.
    class Host final
    {
    public:
        QWidget             window_;
        QWidget*            body_ = nullptr;
        DocumentStatusBar*  bar_  = nullptr;

        Host()
        {
            window_.setGeometry( 120, 160, 900, 520 );
            auto* Layout = new QVBoxLayout( &window_ );
            Layout->setContentsMargins( 0, 0, 0, 0 );
            Layout->setSpacing( 0 );
            body_ = new QWidget;
            body_->setObjectName( "body" );
            body_->setFocusPolicy( Qt::StrongFocus );
            Layout->addWidget( body_, 1 );
            bar_ = new DocumentStatusBar;
            Layout->addWidget( bar_ );
            bar_->SetFields( Fields( true ) );
            window_.show();
        }

        QToolButton* Button( const QString& Id ) const { return bar_->findChild< QToolButton* >( "status." + Id ); }
        QRect Window() const { return { window_.mapToGlobal( QPoint( 0, 0 ) ), window_.size() }; }
        int BarTop() const { return bar_->mapToGlobal( QPoint( 0, 0 ) ).y(); }
    };

    QList< QRect > Geometries( const QWidget& Bar )
    {
        QList< QRect > Result;
        for( const auto* Item : Bar.findChildren< QWidget* >( Qt::FindDirectChildrenOnly ) ) Result << Item->geometry();
        return Result;
    }

    // 분수 배율에서 창 위치가 장치 픽셀로 반올림되는 만큼은 허용한다.
    bool Near( int Actual, int Expected ) { return qAbs( Actual - Expected ) <= 1; }
}

class DocumentStatusPopupTests final : public QObject
{
    Q_OBJECT
private slots:
    void pickerFieldRequestsPopover()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        QStringList Commands, Pickers;
        QWidget*    Anchor = nullptr;
        View.bar_->activated_       = [ & ]( const QString& Command ) { Commands << Command; };
        View.bar_->pickerRequested_ = [ & ]( const QString& Id, QWidget* At ) { Pickers << Id; Anchor = At; };

        auto* Encoding = View.Button( "encoding" );
        QVERIFY( Encoding );
        QCOMPARE( Encoding->accessibleDescription(), QString::fromUtf8( "눌러서 변경" ) );
        QTest::mouseClick( Encoding, Qt::LeftButton );
        QCOMPARE( Pickers, QStringList{ "encoding" } );
        QCOMPARE( Anchor, static_cast< QWidget* >( Encoding ) );
        QTest::keyClick( Encoding, Qt::Key_Return );
        QTest::keyClick( Encoding, Qt::Key_Enter, Qt::KeypadModifier );
        QTest::keyClick( Encoding, Qt::Key_Space );
        QCOMPARE( Pickers.size(), 4 );
        QVERIFY( Commands.isEmpty() );

        // 명령이 없는 선택 항목도 단추다.
        auto* Eol = View.Button( "eol" );
        QVERIFY( Eol );
        QTest::mouseClick( Eol, Qt::LeftButton );
        QCOMPARE( Pickers.last(), QString( "eol" ) );

        // 명령 항목은 그대로 activated_ 를 부른다.
        auto* Position = View.Button( "position" );
        QVERIFY( Position->accessibleDescription().isEmpty() );
        QTest::mouseClick( Position, Qt::LeftButton );
        QTest::keyClick( Position, Qt::Key_Return );
        QCOMPARE( Commands, ( QStringList{ "text.goToLine", "text.goToLine" } ) );
        QCOMPARE( Pickers.size(), 5 );

        // 팝업을 받지 않는 곳에서는 선택 항목이 명령으로 돌아간다.
        View.bar_->pickerRequested_ = nullptr;
        QTest::mouseClick( Encoding, Qt::LeftButton );
        QCOMPARE( Commands.last(), QString( "text.encoding" ) );
    }

    void pickerCueKeepsWidth()
    {
        DocumentStatusBar Bar;
        Bar.resize( 900, 32 );
        Bar.show();
        QVERIFY( QTest::qWaitForWindowExposed( &Bar ) );
        Bar.SetFields( Fields( false ) );
        QCoreApplication::processEvents();
        auto*     Plain      = Bar.findChild< QToolButton* >( "status.encoding" );
        const int PlainWidth = Plain->width();
        QVERIFY( !Bar.findChild< QToolButton* >( "status.eol" ) );

        Bar.SetFields( Fields( true ) );
        QCoreApplication::processEvents();
        auto* Picker = dynamic_cast< DocumentStatusButton* >( Bar.findChild< QToolButton* >( "status.encoding" ) );
        QVERIFY( Picker );
        QCOMPARE( static_cast< QToolButton* >( Picker ), Plain );
        // '⌄' 자리는 보이지 않을 때도 폭에 들어 있다.
        QCOMPARE( Picker->width(), PlainWidth + DocumentStatusButton::PickerReserve( Bar.font() ) );
        QVERIFY( !Picker->IsGlyphVisible() );
        const auto Before = Geometries( Bar );

        Picker->setAttribute( Qt::WA_UnderMouse, true );
        Picker->update();
        QCoreApplication::processEvents();
        QVERIFY( Picker->IsGlyphVisible() );
        QCOMPARE( Geometries( Bar ), Before );
        Picker->setAttribute( Qt::WA_UnderMouse, false );
        QVERIFY( !Picker->IsGlyphVisible() );

        Bar.activateWindow();
        // 포커스 쪽은 창이 앞에 올 때만 확인한다(Windows 전경 잠금).
        if( QTest::qWaitForWindowActive( &Bar ) )
        {
            Picker->setFocus( Qt::TabFocusReason );
            QVERIFY( Picker->IsGlyphVisible() );
            QCOMPARE( Geometries( Bar ), Before );
        }
    }

    void popupSitsAboveStatusBar()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        auto*         Anchor = View.Button( "encoding" );
        const QString Title  = QString::fromUtf8( "인코딩 — UTF-8 (BOM)" );
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( Anchor, Title );
        QCOMPARE( Popup->AnchorWidget(), static_cast< QWidget* >( Anchor ) );
        Popup->AddNote( nullptr, QString::fromUtf8( "자동 감지: UTF-8 · 신뢰도 높음" ) );
        Popup->AddChips( nullptr, { "2", "4", "8" }, 1, {} );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QCOMPARE( Popup->accessibleName(), Title );
        QCOMPARE( Popup->Title(), Title );

        const QRect Panel = Popup->PanelGeometry();
        QVERIFY( Near( Panel.width(), DocumentStatusPopup::PanelWidth ) );
        QVERIFY( Near( Panel.y() + Panel.height() + DocumentStatusPopup::BarGap, View.BarTop() ) );
        QVERIFY( Near( Panel.x(), Anchor->mapToGlobal( QPoint( 0, 0 ) ).x() ) );
        QVERIFY( View.Window().contains( Panel ) );
        Popup->close();
        QTRY_VERIFY( !Popup );

        // 오른쪽 끝 항목은 창 안으로 당긴다(오른쪽 4 DIP).
        auto* Mode = View.bar_->findChild< QWidget* >( "status.mode" );
        Popup = DocumentStatusPopup::Open( Mode, "mode" );
        Popup->AddNote( nullptr, "right" );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        const QRect Right = Popup->PanelGeometry();
        QVERIFY( View.Window().contains( Right ) );
        QVERIFY( Near( Right.x() + Right.width(), View.Window().x() + View.Window().width() - 4 ) );
        QVERIFY( Right.x() < Mode->mapToGlobal( QPoint( 0, 0 ) ).x() );
        Popup->close();
        QTRY_VERIFY( !Popup );

        // 창 안 위쪽에 자리가 없으면 상태표시줄 아래로 띄운다.
        View.window_.resize( 900, 80 );
        QTRY_VERIFY( View.window_.height() == 80 );
        Popup = DocumentStatusPopup::Open( Anchor, Title );
        for( int I = 0; I < 4; ++I ) Popup->AddNote( nullptr, QString::fromUtf8( "파일 바이트는 그대로 두고 해석만 바꿉니다." ) );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QVERIFY( Popup->PanelGeometry().y() > View.BarTop() );
        Popup->close();
        QTRY_VERIFY( !Popup );
    }

    void tabsSwitchPages()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( View.Button( "encoding" ), "UTF-8" );
        const auto Pages = Popup->AddTabs( { QString::fromUtf8( "다시 읽기 (해석만)" ), QString::fromUtf8( "변환하여 저장" ) }, 0 );
        QCOMPARE( Pages.size(), 2 );
        auto* First  = Popup->AddNote( Pages[ 0 ], "first" );
        auto* Second = Popup->AddNote( Pages[ 1 ], "second" );
        Popup->AddWarning( Pages[ 1 ], QString::fromUtf8( "표현할 수 없는 문자가 있으면 저장 전에 위치를 표시합니다." ) );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );

        const auto Tabs = Popup->findChildren< QAbstractButton* >( "statusPopupTab" );
        QCOMPARE( Tabs.size(), 2 );
        QVERIFY( Tabs[ 0 ]->isChecked() );
        QVERIFY( First->isVisible() );
        QVERIFY( !Second->isVisible() );
        const int ShortHeight = Popup->PanelGeometry().height();

        QTest::mouseClick( Tabs[ 1 ], Qt::LeftButton );
        QVERIFY( Tabs[ 1 ]->isChecked() );
        QVERIFY( !Tabs[ 0 ]->isChecked() );
        QVERIFY( Second->isVisible() );
        QVERIFY( !First->isVisible() );
        // 카드가 커져도 아래 끝은 상태표시줄 위에 있다.
        QVERIFY( Popup->PanelGeometry().height() > ShortHeight );
        QVERIFY( Near( Popup->PanelGeometry().y() + Popup->PanelGeometry().height() + DocumentStatusPopup::BarGap, View.BarTop() ) );

        // 방향키는 탭을 옮기며 쪽도 바꾼다.
        QTest::keyClick( Tabs[ 1 ], Qt::Key_Left );
        QVERIFY( Tabs[ 0 ]->isChecked() );
        QVERIFY( First->isVisible() );
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( Tabs[ 0 ] ) );
        Popup->close();
        QTRY_VERIFY( !Popup );
    }

    void chipsAreExclusive()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        QList< int > Activated;
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( View.Button( "eol" ), QString::fromUtf8( "들여쓰기 — 공백 4" ) );
        Popup->AddChips( nullptr, { "2", "4", "8" }, 1, [ & ]( int Index ) { Activated << Index; } );
        Popup->AddChips( nullptr, { QString::fromUtf8( "첫 쪽" ), QString::fromUtf8( "마지막 쪽" ) }, -1, {} );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );

        const auto Chips = Popup->findChildren< QAbstractButton* >( "statusPopupChip" );
        QCOMPARE( Chips.size(), 5 );
        QVERIFY( !Chips[ 0 ]->isChecked() && Chips[ 1 ]->isChecked() && !Chips[ 2 ]->isChecked() );
        QVERIFY( !Chips[ 3 ]->isChecked() && !Chips[ 4 ]->isChecked() );
        // 묶음마다 Tab 이 들르는 칩은 하나다(고른 칩, 없으면 첫째).
        QCOMPARE( Chips[ 1 ]->focusPolicy(), Qt::StrongFocus );
        QCOMPARE( Chips[ 0 ]->focusPolicy(), Qt::ClickFocus );
        QCOMPARE( Chips[ 3 ]->focusPolicy(), Qt::StrongFocus );

        QTest::mouseClick( Chips[ 2 ], Qt::LeftButton );
        QCOMPARE( Activated, QList< int >{ 2 } );
        QVERIFY( Chips[ 2 ]->isChecked() && !Chips[ 1 ]->isChecked() );
        // 고른 칩을 다시 눌러도 고른 채로 남는다.
        QTest::mouseClick( Chips[ 2 ], Qt::LeftButton );
        QVERIFY( Chips[ 2 ]->isChecked() );
        QCOMPARE( Activated.size(), 2 );

        // 방향키는 포커스만 옮기고(끝에서 돌아간다), Enter · Space 가 고른다.
        Chips[ 2 ]->setFocus( Qt::TabFocusReason );
        QTest::keyClick( Chips[ 2 ], Qt::Key_Right );
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( Chips[ 0 ] ) );
        QCOMPARE( Activated.size(), 2 );
        QVERIFY( Chips[ 2 ]->isChecked() );
        QTest::keyClick( Chips[ 0 ], Qt::Key_Return );
        QCOMPARE( Activated.last(), 0 );
        QVERIFY( Chips[ 0 ]->isChecked() && !Chips[ 2 ]->isChecked() );
        QTest::keyClick( Chips[ 0 ], Qt::Key_Left );
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( Chips[ 2 ] ) );
        QTest::keyClick( Chips[ 2 ], Qt::Key_Space );
        QCOMPARE( Activated.last(), 2 );
        QCOMPARE( Activated.size(), 4 );
        // 다른 묶음은 그대로다. 칩은 팝업을 닫지 않는다.
        QVERIFY( !Chips[ 3 ]->isChecked() && !Chips[ 4 ]->isChecked() );
        QVERIFY( Popup->isVisible() );
        Popup->close();
        QTRY_VERIFY( !Popup );
    }

    void listFiltersAndActivates()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        QList< int > Activated;
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( View.Button( "encoding" ), QString::fromUtf8( "인코딩" ) );
        auto* Search = Popup->AddSearch( nullptr, QString::fromUtf8( "인코딩 검색" ) );
        auto* List   = Popup->AddList( nullptr,
                                       { { "UTF-8", QString::fromUtf8( "유니코드" ) }, { "UTF-16 LE", {} }, { "EUC-KR", QString::fromUtf8( "한국어" ) },
                                         { "Shift_JIS", QString::fromUtf8( "일본어" ) }, { "windows-1252", QString::fromUtf8( "서유럽어" ) } },
                                       0, [ & ]( int Index ) { Activated << Index; }, Search );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( Search ) );
        QCOMPARE( List->currentRow(), 0 );
        QVERIFY( List->height() <= 170 );

        // 오른쪽 설명으로도 거른다. 고른 행이 숨으면 처음 보이는 행으로 옮긴다.
        Search->setText( QString::fromUtf8( "한국" ) );
        QVERIFY( List->isRowHidden( 0 ) );
        QVERIFY( !List->isRowHidden( 2 ) );
        QCOMPARE( List->currentRow(), 2 );
        QTest::keyClick( Search, Qt::Key_Return );
        QCOMPARE( Activated, QList< int >{ 2 } );

        Search->clear();
        QVERIFY( !List->isRowHidden( 0 ) );
        QTest::keyClicks( Search, "utf" );
        QCOMPARE( Search->text(), QString( "utf" ) );
        QVERIFY( !List->isRowHidden( 0 ) && !List->isRowHidden( 1 ) );
        QVERIFY( List->isRowHidden( 2 ) && List->isRowHidden( 3 ) && List->isRowHidden( 4 ) );
        QCOMPARE( List->currentRow(), 0 );
        // 검색 칸의 위아래 키는 보이는 행 사이를 움직인다.
        QTest::keyClick( Search, Qt::Key_Down );
        QCOMPARE( List->currentRow(), 1 );
        QTest::keyClick( Search, Qt::Key_Down );
        QCOMPARE( List->currentRow(), 1 );
        QTest::keyClick( Search, Qt::Key_Enter, Qt::KeypadModifier );
        QCOMPARE( Activated.last(), 1 );

        // 목록의 Enter · Space · 누름. 거른 뒤에도 Items 의 순번을 받는다.
        Search->clear();
        List->setFocus( Qt::TabFocusReason );
        List->setCurrentRow( 3 );
        QTest::keyClick( List, Qt::Key_Return );
        QCOMPARE( Activated.last(), 3 );
        QTest::keyClick( List, Qt::Key_Space );
        QCOMPARE( Activated.size(), 4 );
        QTest::mouseClick( List->viewport(), Qt::LeftButton, {}, List->visualItemRect( List->item( 4 ) ).center() );
        QCOMPARE( Activated.last(), 4 );
        QCOMPARE( List->currentRow(), 4 );
        // 목록 선택은 팝업을 닫지 않는다.
        QVERIFY( Popup->isVisible() );
        Popup->close();
        QTRY_VERIFY( !Popup );
    }

    void inputAcceptsOrStays()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        QStringList Seen;
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( View.Button( "position" ), QString::fromUtf8( "줄로 이동" ) );
        auto* Input = Popup->AddInput( nullptr, "18:5", QString::fromUtf8( "줄[:열] · 1–545" ), [ & ]( const QString& Text ) {
            Seen << Text;
            return Text == "20";
        } );
        Popup->AddNote( nullptr, QString::fromUtf8( "Enter로 이동, Esc로 본문 복귀." ) );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QCOMPARE( QApplication::focusWidget(), static_cast< QWidget* >( Input ) );
        QCOMPARE( Input->selectedText(), QString( "18:5" ) );

        // 받지 않으면 남고 글자를 골라 둔다.
        QTest::keyClick( Input, Qt::Key_Return );
        QCOMPARE( Seen, QStringList{ "18:5" } );
        QVERIFY( Popup && Popup->isVisible() );
        QCOMPARE( Input->selectedText(), QString( "18:5" ) );
        QTest::keyClicks( Input, "20" );
        QCOMPARE( Input->text(), QString( "20" ) );
        QTest::keyClick( Input, Qt::Key_Return );
        QCOMPARE( Seen.last(), QString( "20" ) );
        QTRY_VERIFY( !Popup );
    }

    void actionClosesBeforeRunning()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        bool          Ran = false, OpenWhileRunning = true;
        QList< bool > Toggles;
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( View.Button( "eol" ), QString::fromUtf8( "줄 끝 — CRLF" ) );
        auto* Check  = Popup->AddCheck( nullptr, QString::fromUtf8( "BOM 포함" ), true, [ & ]( bool On ) { Toggles << On; } );
        auto* Action = Popup->AddAction( nullptr, QString::fromUtf8( "모든 줄을 LF로 변환" ), "Ctrl+Shift+L", [ & ] {
            Ran              = true;
            OpenWhileRunning = Popup && Popup->isVisible();
        } );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QTest::keyClick( Check, Qt::Key_Space );
        QCOMPARE( Toggles, QList< bool >{ false } );
        QVERIFY( Popup->isVisible() );
        QTest::keyClick( Action, Qt::Key_Return );
        QVERIFY( Ran );
        QVERIFY( !OpenWhileRunning );
        QTRY_VERIFY( !Popup );
    }

    void escapeRestoresFocus()
    {
        Host View;
        QVERIFY( QTest::qWaitForWindowExposed( &View.window_ ) );
        auto* Anchor = View.Button( "encoding" );
        QPointer< DocumentStatusPopup > Popup = DocumentStatusPopup::Open( Anchor, "esc" );
        Popup->AddChips( nullptr, { "a", "b" }, 0, {} );
        Popup->AddCheck( nullptr, "check", true, {} );
        Popup->AddAction( nullptr, "action", {}, {} );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );

        // Tab 은 팝업 안을 돈다.
        QVERIFY( Popup->isAncestorOf( QApplication::focusWidget() ) );
        for( int I = 0; I < 5; ++I )
        {
            QTest::keyClick( QApplication::focusWidget(), Qt::Key_Tab );
            QVERIFY( Popup->isAncestorOf( QApplication::focusWidget() ) );
        }
        QTest::keyClick( QApplication::focusWidget(), Qt::Key_Escape );
        QTRY_VERIFY( !Popup );
        QCOMPARE( View.window_.focusWidget(), static_cast< QWidget* >( Anchor ) );

        // 돌아갈 곳을 정하면 그리로 간다.
        Popup = DocumentStatusPopup::Open( Anchor, "esc" );
        Popup->AddChips( nullptr, { "a", "b" }, 0, {} );
        Popup->SetFocusReturn( View.body_ );
        Popup->ShowAbove();
        QVERIFY( QTest::qWaitForWindowExposed( Popup.data() ) );
        QTest::keyClick( QApplication::focusWidget(), Qt::Key_Escape );
        QTRY_VERIFY( !Popup );
        QCOMPARE( View.window_.focusWidget(), View.body_ );
    }
};

int main( int Argc, char** Argv )
{
    QApplication App( Argc, Argv );
    // 제품과 같은 스타일 · 팔레트 · 글꼴.
    DocumentThemeAdapter::Install( App );
    DocumentStatusPopupTests Tests;
    return QTest::qExec( &Tests, Argc, Argv );
}

#include "tstDocumentStatusPopup.moc"
