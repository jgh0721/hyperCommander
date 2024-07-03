#include "stdafx.h"
#include "uiComboBoxWidget.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

const std::string logger = "UIs/cmpRecipientWithFiles";

// based on https://stackoverflow.com/questions/61758354/remove-buttons-for-combobox-items

QComboBoxItemWidget::QComboBoxItemWidget( QWidget* Parent, Qt::WindowFlags f )
    : QWidget( Parent, f )
{
    setFixedHeight( 48 );

    const auto Horizontal = new QHBoxLayout;
    Horizontal->setContentsMargins( 0, 0, 0, 0 );
    setLayout( Horizontal );

    btnLeft = new QPushButton;
    btnLeft->setVisible( false );
    btnLeft->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    connect( btnLeft, &QPushButton::clicked, this, &QComboBoxItemWidget::buttonClicked );

    lblContent = new QLabel;
    lblContent->setVisible( false );
    lblContent->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );

    //btnContent = new QPushButton;
    //btnContent->setVisible( true );
    //auto btnContentLy = new QHBoxLayout;
    //btnContentLy->setContentsMargins( 9, 0, 0, 0 );
    //btnContentLy->setSpacing( 0 );
    //btnContent->setLayout( btnContentLy );
    //btnContent->setStyleSheet( R"(
    //    QPushButton { 
    //        background-color: transparent;
    //        border: none;
    //        text-align: left;
    //    }

    //    QPushButton::hover {
    //        background: transparent;
    //    }
    //)" );
    //btnContent->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
    //lblContent = new QLabel;
    //lblContent->setAttribute( Qt::WA_TranslucentBackground );
    //lblContent->setAttribute( Qt::WA_TransparentForMouseEvents );
    //btnContent->layout()->addWidget( lblContent );
    //connect( btnContent, &QPushButton::clicked, this, &QComboBoxItemWidget::contentClicked );

    btnRight = new QPushButton;
    btnRight->setVisible( false );
    btnRight->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Minimum );
    connect( btnRight, &QPushButton::clicked, this, &QComboBoxItemWidget::buttonClicked );

    Horizontal->addItem( new QSpacerItem( 9, 0, QSizePolicy::Minimum, QSizePolicy::Minimum ) );
    Horizontal->addWidget( btnLeft );
    //Horizontal->addItem( new QSpacerItem( 0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum ) );

    Horizontal->addWidget( lblContent );

    //Horizontal->addItem( new QSpacerItem( 0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum ) );
    Horizontal->addWidget( btnRight );
    Horizontal->addItem( new QSpacerItem( 9, 0, QSizePolicy::Minimum, QSizePolicy::Minimum ) );
}

void QComboBoxItemWidget::SetItem( QListWidgetItem* Item )
{
    widItem = Item;
}

QPushButton* QComboBoxItemWidget::AddButton( bool IsLeft, const QIcon& Icon, const QString& ToolTip )
{
    const auto Btn = IsLeft == true ? btnLeft : btnRight;
    Btn->setIcon( Icon );
    Btn->setToolTip( ToolTip );
    Btn->setVisible( true );

    updateGeometry();
    adjustSize();

    return Btn;
}

QPushButton* QComboBoxItemWidget::GetButton( bool IsLeft ) const
{
    return IsLeft == true ? btnLeft : btnRight;
}

QLabel* QComboBoxItemWidget::AddLabel()
{
    if( lblContent == nullptr )
    {
        lblContent = new QLabel;
        lblContent->setVisible( true );
        lblContent->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Minimum );
        const auto HBox = static_cast< QHBoxLayout* >( layout() );
        // lblContent->setStyleSheet( "color: black;" );

        // Spacer - Left Button - Spacer - Spacer - Right Button - Spacer
        HBox->insertWidget( 3, lblContent );
    }

    lblContent->setVisible( true );
    return lblContent;
}

QLabel* QComboBoxItemWidget::GetLabel() const
{
    return lblContent;
}

void QComboBoxItemWidget::SetText( const QString& Text )
{
    sText = Text;
    GetItem()->setText( Text );
}

QString QComboBoxItemWidget::GetText() const
{
    return GetItem()->text();
}

QVariant QComboBoxItemWidget::GetUserData( int Role ) const
{
    return GetItem()->data( Role );
}

void QComboBoxItemWidget::SetUserData( int Role, const QVariant& Data )
{
    GetItem()->setData( Role, Data );
}

///////////////////////////////////////////////////////////////////////////////

QComboBoxForWidget::QComboBoxForWidget( QWidget* parent )
    : QComboBox( parent )
{
    lstView = new QListWidget;
    lstView->setSpacing( 1 );
    lstView->setSortingEnabled( false );
    // lstView->setStyleSheet( "QListView::item { margin-left: 9px; } " );

    setModel( lstView->model() );
    setView( lstView );
    setEditable( false );
}

QComboBoxItemWidget* QComboBoxForWidget::CreateItem()
{
    QListWidgetItem* Item = new QListWidgetItem;
    QComboBoxItemWidget* ItemWidget = new QComboBoxItemWidget( this );

    Item->setSizeHint( ItemWidget->sizeHint() );
    ItemWidget->SetItem( Item );
    
    return ItemWidget;
}

void QComboBoxForWidget::AddItem( QListWidgetItem* Item, QComboBoxItemWidget* Widget )
{
    lstView->insertItem( 0, Item );
    adjustSize();
    Widget->updateGeometry();
    Item->setSizeHint( Widget->size() );
    lstView->setItemWidget( Item, Widget );
}

void QComboBoxForWidget::AppendItem( QListWidgetItem* Item, QComboBoxItemWidget* Widget )
{
    lstView->insertItem( lstView->count(), Item );
    adjustSize();
    Widget->updateGeometry();
    Item->setSizeHint( Widget->size() );
    lstView->setItemWidget( Item, Widget );
}

QComboBoxItemWidget* QComboBoxForWidget::GetItem( int Index ) const
{
    const auto Count = GetCount();
    if( Index < 0 || Index >= Count )
        return nullptr;

    const auto Item = lstView->item( Index );
    if( Item == nullptr )
        return nullptr;

    return static_cast< QComboBoxItemWidget* >( lstView->itemWidget( Item ) );
}

void QComboBoxForWidget::RemoveItem( QListWidgetItem* Item )
{
    if( Item == nullptr )
        return;

    delete lstView->itemWidget( Item );
    delete lstView->takeItem( lstView->row( Item ) );
}

int QComboBoxForWidget::GetCount() const
{
    return lstView->count();
}
