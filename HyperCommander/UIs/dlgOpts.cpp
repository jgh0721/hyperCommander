#include "stdafx.h"
#include "dlgOpts.hpp"

#include "cmnHCUtils.hpp"
#include "Qt/cmnQtUtils.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"

#include "cmnTypeDefs.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QDialog( parent, flags )
{
    ui.setupUi( this );

    initialize();

    QMetaObject::invokeMethod( this, "LoadSettings", Qt::QueuedConnection );
}

void QMainOpts::LoadSettings()
{
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    StColorSchemeMgr->Refresh();

    ui.cbxColorScheme->clear();
    ui.cbxColorScheme->addItems( StColorSchemeMgr->GetNames() );

    //ui.ftCbxFileList->setCurrentFont( GetOPTValue( OPT_COLORS_LISTFONT ) );
    //ui.spbFileListFontSize->setValue( GetOPTValue( OPT_COLORS_LISTFONTSIZE ).toInt() );

    //setButtonColor( ui.btnFileListForground, QColor( GetOPTValue( OPT_COLORS_FORECOLOR ) ) );
    //setButtonColor( ui.btnFileListBackground, QColor( GetOPTValue( OPT_COLORS_BACKCOLOR ) ) );
}

void QMainOpts::SaveSettings()
{
    //SetOPTValue( OPT_COLORS_LISTFONT, ui.ftCbxFileList->currentFont().family() );
    //SetOPTValue( OPT_COLORS_LISTFONTSIZE, ui.spbFileListFontSize->value() );
    //SetOPTValue( OPT_COLORS_FORECOLOR, ui.btnFileListForground->text() );
    //SetOPTValue( OPT_COLORS_BACKCOLOR, ui.btnFileListBackground->text() );
}

void QMainOpts::SaveSettings_ColorScheme()
{
    TyColorScheme ColorScheme = {};

    ColorScheme.Name                = ui.edtColorSchemeName->text();
    ColorScheme.IsDarkMode          = ui.chkIsDarkMode->isChecked();
    ColorScheme.Menu_Font           = ui.fntMnuFontBox->currentFont();
    ColorScheme.Menu_Font.setPointSize( ui.spbMnuFontBox->value() );
    ColorScheme.Dialog_Font         = ui.fntDlgFontBox->currentFont();
    ColorScheme.Dialog_Font.setPointSize( ui.spbDlgFontBox->value() );
    ColorScheme.FileList_Font       = ui.fntLstFontBox->currentFont();
    ColorScheme.FileList_Font.setPointSize( ui.spbLstFontBox->value() );
    ColorScheme.FileList_FGColor    = ui.btnLstFGColor->text();
    ColorScheme.FileList_BGColor    = ui.btnLstBGColor->text();
    ColorScheme.FileList_Cursor     = ui.btnLstCursorColor->text();
    ColorScheme.FileList_Selected   = ui.btnLstSelectColor->text();

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    StColorSchemeMgr->UpsertColorScheme( ColorScheme, true );
}

void QMainOpts::RefreshColorScheme( const QString& SchemeName )
{
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    const auto ColorScheme = StColorSchemeMgr->GetColorScheme( SchemeName );
    resetColorScheme( ColorScheme.Name );

    if( ColorScheme.Name.isEmpty() == true )
        return;

    ui.edtColorSchemeName->setText( ColorScheme.Name );
    ui.chkIsDarkMode->setChecked( ColorScheme.IsDarkMode );

    ui.fntMnuFontBox->setFont( QFont( ColorScheme.Menu_Font ) );
    ui.spbMnuFontBox->setValue( ColorScheme.Menu_Font.pointSize() );

    ui.fntDlgFontBox->setFont( QFont( ColorScheme.Dialog_Font ) );
    ui.spbDlgFontBox->setValue( ColorScheme.Dialog_Font.pointSize() );

    ui.fntLstFontBox->setFont( QFont( ColorScheme.FileList_Font ) );
    ui.spbLstFontBox->setValue( ColorScheme.FileList_Font.pointSize() );

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
    UNREFERENCED_PARAMETER( Current );
    UNREFERENCED_PARAMETER( Previous );

    if( Current == nullptr )
        return;

    // NOTE: 일단 항목의 색인과 오른쪽 화면의 색인을 일치시켜둔다.
    ui.stkOpts->setCurrentIndex( ui.lstOptCate->row( Current ) );
}

///////////////////////////////////////////////////////////////////////////////
/// Content

/// Page = pdDisplay_ColorScheme
void QMainOpts::on_cbxColorScheme_currentIndexChanged( int index )
{
    RefreshColorScheme( ui.cbxColorScheme->currentText() );
}

void QMainOpts::on_btnAddColorScheme_clicked( bool checked )
{
    const auto Written = QInputDialog::getText( this, tr( "색 구성표 추가" ), tr( "새 구성표 이름:" ) );
    if( ui.cbxColorScheme->findText( Written ) >= 0 )
    {
        // TODO: 중복된 이름
        ShowMSGBox( this, QMessageBox::Information, LANG_OPT_DUPLICATE_SCHEME, "HyperCommander" );
        return;
    }

    QSignalBlocker Blocker( ui.cbxColorScheme );
    {
        ui.cbxColorScheme->addItem( Written );
        ui.cbxColorScheme->setCurrentText( Written );
    }
    resetColorScheme( Written );
    SaveSettings_ColorScheme();
}

void QMainOpts::on_btnLstFGColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnLstFGColor, Selected );
}

void QMainOpts::on_btnLstBGColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnLstBGColor, Selected );
}

void QMainOpts::on_btnLstCursorColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnLstCursorColor, Selected );
}

void QMainOpts::on_btnLstSelectColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnLstSelectColor, Selected );
}
//
//void QMainOpts::on_btnFileListForground_clicked( bool checked )
//{
//    const auto Selected = QColorDialog::getColor( Qt::white, this );
//
//    if( Selected.isValid() == true )
//        setButtonColor( ui.btnFileListForground, Selected );
//}
//
//void QMainOpts::on_btnFileListBackground_clicked( bool checked )
//{
//    const auto Selected = QColorDialog::getColor( Qt::white, this );
//
//    if( Selected.isValid() == true )
//        setButtonColor( ui.btnFileListBackground, Selected );
//}

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
    ui.lstOptCate->clear();

    ui.lstOptCate->addItem( PAGE_DISPLAY.second );
    ui.lstOptCate->addItem( PAGE_DISPLAY_FONTCOLOR.second );
    ui.lstOptCate->addItem( PAGE_FILESET.second );
    ui.lstOptCate->addItem( PAGE_SHORTCUT.second );
    ui.lstOptCate->addItem( PAGE_CUSTOM_COLUMNS.second );

    ui.lstOptCate->setResizeMode( QListView::Adjust );
    ui.lstOptCate->setSpacing( 6 );
    ui.lstOptCate->setUniformItemSizes( true );

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

void QMainOpts::resetColorScheme( const QString& Name )
{
    ui.edtColorSchemeName->setText( Name );
    ui.chkIsDarkMode->setChecked( false );

    ui.fntMnuFontBox->setCurrentFont( {} );
    ui.spbMnuFontBox->setValue( 9 );
    ui.fntDlgFontBox->setCurrentFont( {} );
    ui.spbDlgFontBox->setValue( 9 );
    ui.fntLstFontBox->setCurrentFont( {} );
    ui.spbLstFontBox->setValue( 14 );

    setButtonColor( ui.btnLstFGColor, {} );
    setButtonColor( ui.btnLstBGColor, {} );
    setButtonColor( ui.btnLstCursorColor, {} );
    setButtonColor( ui.btnLstSelectColor, {} );
}

