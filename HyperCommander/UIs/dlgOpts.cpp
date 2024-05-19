#include "stdafx.h"
#include "dlgOpts.hpp"

#include "Qt/cmnQtUtils.hpp"

#include "cmnTypeDefs.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QDialog( parent, flags )
{
    ui.setupUi( this );

    initialize();

    nsCmn::nsCmnQt::InvokeMethod( this, "LoadSettings", Qt::QueuedConnection );
}

void QMainOpts::LoadSettings()
{
    ui.ftCbxFileList->setCurrentFont( GetOPTValue( OPT_COLORS_LISTFONT ) );
    ui.spbFileListFontSize->setValue( GetOPTValue( OPT_COLORS_LISTFONTSIZE ).toInt() );

    setButtonColor( ui.btnFileListForground, QColor( GetOPTValue( OPT_COLORS_FORECOLOR ) ) );
    setButtonColor( ui.btnFileListBackground, QColor( GetOPTValue( OPT_COLORS_BACKCOLOR ) ) );
}

void QMainOpts::SaveSettings()
{
    SetOPTValue( OPT_COLORS_LISTFONT, ui.ftCbxFileList->currentFont().family() );
    SetOPTValue( OPT_COLORS_LISTFONTSIZE, ui.spbFileListFontSize->value() );
    SetOPTValue( OPT_COLORS_FORECOLOR, ui.btnFileListForground->text() );
    SetOPTValue( OPT_COLORS_BACKCOLOR, ui.btnFileListBackground->text() );
}

///////////////////////////////////////////////////////////////////////////////
/// TitleBar

void QMainOpts::on_btnClose_clicked( bool checked /* = false */ )
{
    on_btnCancel_clicked( checked );
}

///////////////////////////////////////////////////////////////////////////////
/// Left

void QMainOpts::on_lstOptCate_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous )
{
    if( Current == nullptr )
        return;

    // NOTE: 일단 항목의 색인과 오른쪽 화면의 색인을 일치시켜둔다.
    ui.stkOpts->setCurrentIndex( ui.lstOptCate->row( Current ) );
}

///////////////////////////////////////////////////////////////////////////////
/// Content

void QMainOpts::on_btnFileListForground_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnFileListForground, Selected );
}

void QMainOpts::on_btnFileListBackground_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnFileListBackground, Selected );
}

///////////////////////////////////////////////////////////////////////////////
/// Footer

void QMainOpts::on_btnOK_clicked( bool checked )
{
    SaveSettings();
    accept();
}

void QMainOpts::on_btnCancel_clicked( bool checked )
{
    reject();
}

void QMainOpts::on_btnApply_clicked( bool checked )
{
    SaveSettings();
}

void QMainOpts::initialize()
{
    ui.lstOptCate->addItem( PAGE_DISPLAY.second );
    ui.lstOptCate->addItem( PAGE_DISPLAY_FONTCOLOR.second );
    ui.lstOptCate->addItem( PAGE_SHORTCUT.second );

    ui.lstOptCate->setResizeMode( QListView::Adjust );
    ui.lstOptCate->setSpacing( 6 );
    ui.lstOptCate->setUniformItemSizes( false );

    ui.lstOptCate->setSizeAdjustPolicy( QAbstractScrollArea::AdjustToContents );
    ui.lstOptCate->setFixedWidth( ui.lstOptCate->sizeHintForColumn( 0 ) + ui.lstOptCate->lineWidth() * 2 + ui.lstOptCate->frameWidth() * 2 );
}

void QMainOpts::setButtonColor( QPushButton* Button, const QColor& Color )
{
    QPixmap px( 40, 40 );
    px.fill( Color );
    Button->setIcon( px );
    Button->setAutoFillBackground( true );
    // Button->setPalette( QPalette( Selected ) );
    Button->setText( Color.name() );
}
