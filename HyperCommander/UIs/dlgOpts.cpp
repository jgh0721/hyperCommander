#include "stdafx.h"
#include "dlgOpts.hpp"

#include "cmnHCUtils.hpp"
#include "Qt/cmnQtUtils.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"

#include "cmnTypeDefs.hpp"
#include "fileSetMgr.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QDialog( parent, flags )
{
    ui.setupUi( this );

    initialize();

    QMetaObject::invokeMethod( this, "LoadSettings", Qt::QueuedConnection );
}

void QMainOpts::LoadSettings()
{
    int Value = 0;
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    const auto StFileSetMgr = TyStFileSetMgr::GetInstance();

    StSettings->beginGroup( "Configuration" );

    Value = StSettings->value( "SizeInHeader" ).toInt();
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxHeaderSizeDisplay->setCurrentIndex( Value );

    Value = StSettings->value( "SizeInFooter" ).toInt();
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxFooterSizeDisplay->setCurrentIndex( Value );

    StSettings->endGroup();

    StColorSchemeMgr->Refresh();
    StFileSetMgr->Refresh();

    ui.cbxColorScheme->clear();
    ui.cbxColorScheme->addItems( StColorSchemeMgr->GetNames() );

    ui.lstFileSet->clear();
    ui.lstFileSet->addItems( StFileSetMgr->GetNames() );

    RefreshPluginList();
}

void QMainOpts::SaveSettings()
{
    StSettings->beginGroup( "Configuration" );
    StSettings->setValue( "SizeInHeader", ui.cbxHeaderSizeDisplay->currentIndex() );
    StSettings->setValue( "SizeInFooter", ui.cbxFooterSizeDisplay->currentIndex() );
    StSettings->endGroup();

    SaveSettings_ColorScheme();
    TyStFileSetMgr::GetInstance()->SaveSettings();
    SaveSettings_PluginList();

    emit NotifyColorSchemeChanged( TyStColorSchemeMgr::GetInstance()->GetColorScheme( ui.cbxColorScheme->currentText() ) );
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

    ColorScheme.Dialog_FGColor      = ui.btnDlgFGColor->text();
    ColorScheme.Dialog_BGColor      = ui.btnDlgBGColor->text();

    ColorScheme.FileList_FGColor    = ui.btnLstFGColor->text();
    ColorScheme.FileList_BGColor    = ui.btnLstBGColor->text();
    ColorScheme.FileList_Cursor     = ui.btnLstCursorColor->text();
    ColorScheme.FileList_Selected   = ui.btnLstSelectColor->text();

    for( int row = 0; row < ui.tblFileSetColor->rowCount(); ++row )
    {
        const auto FileSet = ui.tblFileSetColor->item( row, 0 )->text();
        const auto FGColor = static_cast< QPushButton* >( ui.tblFileSetColor->cellWidget( row, 1 ) )->text();
        const auto BGColor = static_cast< QPushButton* >( ui.tblFileSetColor->cellWidget( row, 2 ) )->text();

        ColorScheme.MapNameToColors[ FileSet ] = qMakePair( FGColor, BGColor );
    }

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    StColorSchemeMgr->UpsertColorScheme( ColorScheme, true );
}

void QMainOpts::SaveSettings_PluginList()
{
    TyStPlugInMgr::GetInstance()->SaveSettings();
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

    ui.fntMnuFontBox->setCurrentFont( QFont( ColorScheme.Menu_Font ) );
    ui.spbMnuFontBox->setValue( ColorScheme.Menu_Font.pointSize() );

    ui.fntDlgFontBox->setCurrentFont( QFont( ColorScheme.Dialog_Font ) );
    ui.spbDlgFontBox->setValue( ColorScheme.Dialog_Font.pointSize() );

    ui.fntLstFontBox->setCurrentFont( QFont( ColorScheme.FileList_Font ) );
    ui.spbLstFontBox->setValue( ColorScheme.FileList_Font.pointSize() );

    setButtonColor( ui.btnDlgFGColor, ColorScheme.Dialog_FGColor );
    setButtonColor( ui.btnDlgBGColor, ColorScheme.Dialog_BGColor );

    setButtonColor( ui.btnLstFGColor, ColorScheme.FileList_FGColor );
    setButtonColor( ui.btnLstBGColor, ColorScheme.FileList_BGColor );
    setButtonColor( ui.btnLstCursorColor, ColorScheme.FileList_Cursor );
    setButtonColor( ui.btnLstSelectColor, ColorScheme.FileList_Selected );

    for( const auto& FileSetName : ColorScheme.MapNameToColors.keys() )
    {
        TyFileSet FileSet = { FileSetName, };
        const auto Colors = ColorScheme.MapNameToColors[ FileSetName ];
       
        oo_insertFileSetColorRow( &FileSet, Colors );
    }
}

void QMainOpts::RefreshFileSet( const QString& FileSetName )
{
    on_btnResetFileSet_clicked();

    if( FileSetName.isEmpty() == true )
        return;

    const auto& FileSet = TyStFileSetMgr::GetInstance()->GetFileSet( FileSetName );
    if( FileSet.Name.isEmpty() == true )
        return;

    ui.edtFileSetName->setText( FileSet.Name );

    if( FlagOn( FileSet.Flags, FILE_SET_NOR_EXT_FILTERS ) )
    {
        ui.chkFileSetExt->setChecked( true );
        ui.edtFileSetExt->setText( FileSet.VecExtFilters.join( '|' ) );
    }

    if( FlagOn( FileSet.Flags, FILE_SET_NOR_FILTERS ) )
    {
        ui.chkFileSetRegExp->setChecked( true );
        ui.edtFileSetRegExp->setText( FileSet.Filters.join( '|' ) );
    }

    if( FlagOn( FileSet.Flags, FILE_SET_EXP_ATTRIBUTE ) )
    {
        ui.grpFileSetAttr->setChecked( true );

        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_ARCHIVE ) )
            ui.chkFileSetAttr_Archive->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_HIDDEN ) )
            ui.chkFileSetAttr_Hidden->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_DIRECTORY ) )
            ui.chkFileSetAttr_Directory->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_ENCRYPTED ) )
            ui.chkFileSetAttr_Encrypted->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_READONLY ) )
            ui.chkFileSetAttr_ReadOnly->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_SYSTEM ) )
            ui.chkFileSetAttr_System->setChecked( true );
        if( FlagOn( FileSet.Flags, FILE_ATTRIBUTE_COMPRESSED ) )
            ui.chkFileSetAttr_Compressed->setChecked( true );
    }
}

void QMainOpts::RefreshPluginList()
{
    const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
    StPlugInMgr->Refresh();

    ui.tblWLX->clearContents();
    ui.tblWLX->setRowCount( 0 );

    for( uint32_t Idx = 0; Idx < StPlugInMgr->GetWLXCount(); ++Idx )
    {
        const auto Row = ui.tblWLX->rowCount();
        ui.tblWLX->setRowCount( Row + 1 );
        ui.tblWLX->setItem( Row, 0, new QTableWidgetItem( StPlugInMgr->GetWLXFilePath( Idx ) ) );
        ui.tblWLX->setItem( Row, 1, new QTableWidgetItem( StPlugInMgr->GetWLXDetect( Idx ) ) );
    }
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

    TyStColorSchemeMgr::GetInstance()->SetCurrentColorScheme( ui.cbxColorScheme->currentText() );
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

void QMainOpts::oo_refreshFileSetColor()
{
    ui.tblFileSetColor->clearContents();
    ui.tblFileSetColor->setRowCount( 0 );

    const auto SchemeName = ui.edtColorSchemeName->text();

    if( SchemeName.isEmpty() == false )
    {
        const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
        const auto StFileSetMgr = TyStFileSetMgr::GetInstance();
        for( const auto& Name : StFileSetMgr->GetNames() )
        {
            const auto& ColorScheme = StColorSchemeMgr->GetColorScheme( SchemeName );
            const auto FileSet = StFileSetMgr->GetFileSet( Name );

            if( ColorScheme.MapNameToColors.contains( FileSet.Name ) == true )
                oo_insertFileSetColorRow( &FileSet, ColorScheme.MapNameToColors[ FileSet.Name ] );
            else
                oo_insertFileSetColorRow( &FileSet, {} );
        }
    }

    oo_insertEmptyFileSetColorRow();
}

void QMainOpts::oo_insertFileSetColorRow( const TyFileSet* FileSet, const QPair< QColor, QColor >& FGWithBG )
{
    Q_ASSERT( FileSet != nullptr && FileSet->Name.isEmpty() == false );
    const auto Row = oo_insertEmptyFileSetColorRow();

    ui.tblFileSetColor->item( Row, 0 )->setText( FileSet->Name );
    setButtonColor( static_cast< QPushButton* >( ui.tblFileSetColor->cellWidget( Row, 1 ) ), FGWithBG.first );
    setButtonColor( static_cast< QPushButton* >( ui.tblFileSetColor->cellWidget( Row, 2 ) ), FGWithBG.second );
}

int QMainOpts::oo_insertEmptyFileSetColorRow()
{
    QPushButton* Button = nullptr;
    QTableWidgetItem* Item = nullptr;
    const auto Row = ui.tblFileSetColor->rowCount();

    ui.tblFileSetColor->setRowCount( Row + 1 );
    ui.tblFileSetColor->setItem( Row, 0, new QTableWidgetItem( "" ) );

    Button = new QPushButton;
    Button->setFlat( true );
    Button->setDefault( false );
    setButtonColor( Button, {} );
    connect( Button, &QPushButton::clicked, this, &QMainOpts::pickButtonColor );
    ui.tblFileSetColor->setItem( Row, 1, new QTableWidgetItem( "" ) );
    ui.tblFileSetColor->setCellWidget( Row, 1, Button );

    Button = new QPushButton;
    Button->setFlat( true );
    Button->setDefault( false );
    setButtonColor( Button, {} );
    connect( Button, &QPushButton::clicked, this, &QMainOpts::pickButtonColor );
    ui.tblFileSetColor->setItem( Row, 2, new QTableWidgetItem( "" ) );
    ui.tblFileSetColor->setCellWidget( Row, 2, Button );

    QWidget* Widget = new QWidget;
    QHBoxLayout* Layout = new QHBoxLayout;
    Layout->setContentsMargins( 9, 0, 9, 0 );
    Widget->setLayout( Layout );
    QPushButton* AddButton = new QPushButton;
    QPushButton* DelButton = new QPushButton;
    AddButton->setDefault( false );
    DelButton->setDefault( false );
    AddButton->setText( "+" );
    DelButton->setText( "-" );
    Layout->addWidget( AddButton );
    Layout->addWidget( DelButton );
    connect( AddButton, &QPushButton::clicked, this, &QMainOpts::oo_insertEmptyFileSetColorRow );
    connect( DelButton, &QPushButton::clicked, this, &QMainOpts::oo_removeFileSetColorRow );
    ui.tblFileSetColor->setItem( Row, 3, new QTableWidgetItem( "" ) );
    ui.tblFileSetColor->setCellWidget( Row, 3, Widget );

    return Row;
}

void QMainOpts::oo_removeFileSetColorRow()
{
    Q_ASSERT( sender() != nullptr );
    Q_ASSERT( qobject_cast< QPushButton* >( sender() ) != nullptr );

    const int Row = ui.tblFileSetColor->currentRow();
    if( Row < 0 )
        return;

    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();

    const auto ColorScheme = ui.edtColorSchemeName->text();
    const auto FileSet = ui.tblFileSetColor->item( Row, 0 )->text();

    if( StColorSchemeMgr->GetNames().contains( ColorScheme ) == false )
        return;

    StColorSchemeMgr->RemoveFileSetColor( ColorScheme, FileSet );
    ui.tblFileSetColor->removeRow( Row );
}

void QMainOpts::on_lstFileSet_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous )
{
    UNREFERENCED_PARAMETER( Previous );

    RefreshFileSet( Current != nullptr ? Current->text() : "" );
}

void QMainOpts::on_btnResetFileSet_clicked( bool checked )
{
    ui.edtFileSetName->clear();

    ui.chkFileSetExt->setChecked( false );
    ui.edtFileSetExt->clear();
    ui.chkFileSetRegExp->setChecked( false );
    ui.edtFileSetRegExp->clear();

    ui.grpFileSetAttr->setChecked( false );
    ui.chkFileSetAttr_Archive->setChecked( false );
    ui.chkFileSetAttr_Hidden->setChecked( false );
    ui.chkFileSetAttr_Directory->setChecked( false );
    ui.chkFileSetAttr_Encrypted->setChecked( false );
    ui.chkFileSetAttr_ReadOnly->setChecked( false );
    ui.chkFileSetAttr_System->setChecked( false );
    ui.chkFileSetAttr_Compressed->setChecked( false );
}

void QMainOpts::on_btnAddFileSet_clicked( bool checked )
{
    TyFileSet FileSet;

    FileSet.Name = ui.edtFileSetName->text();

    if( ui.edtFileSetExt->text().isEmpty() == false )
    {
        SetFlag( FileSet.Flags, FILE_SET_NOR_EXT_FILTERS );
        FileSet.VecExtFilters = ui.edtFileSetExt->text().split( '|', Qt::SkipEmptyParts );
    }

    if( ui.edtFileSetRegExp->text().isEmpty() == false )
    {
        SetFlag( FileSet.Flags, FILE_SET_NOR_FILTERS );
        FileSet.Filters = ui.edtFileSetRegExp->text().split( '|', Qt::SkipEmptyParts );
    }

    if( ui.grpFileSetAttr->isChecked() == true )
    {
        SetFlag( FileSet.Flags, FILE_SET_EXP_ATTRIBUTE );
        if( ui.chkFileSetAttr_Archive->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_ARCHIVE );
        if( ui.chkFileSetAttr_Hidden->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_HIDDEN );
        if( ui.chkFileSetAttr_Directory->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_DIRECTORY );
        if( ui.chkFileSetAttr_Encrypted->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_ENCRYPTED );
        if( ui.chkFileSetAttr_ReadOnly->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_READONLY );
        if( ui.chkFileSetAttr_System->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_SYSTEM );
        if( ui.chkFileSetAttr_Compressed->isChecked() == true )
            SetFlag( FileSet.Attributes, FILE_ATTRIBUTE_COMPRESSED );
    }

    UNREFERENCED_PARAMETER( FileSet.Size );

    TyStFileSetMgr::GetInstance()->SetFileSet( FileSet );
    if( ui.lstFileSet->findItems( FileSet.Name, Qt::MatchFixedString ).isEmpty() == true )
        ui.lstFileSet->addItem( FileSet.Name );
}

void QMainOpts::on_btnRemoveFileSet_clicked( bool checked )
{
    const auto Name = ui.edtFileSetName->text();

    if( Name.isEmpty() == true )
        return;

    TyStFileSetMgr::GetInstance()->RemoveFileSet( Name );

    for( const auto& Item : ui.lstFileSet->findItems( Name, Qt::MatchFixedString ) )
    {
        ui.lstFileSet->takeItem( ui.lstFileSet->row( Item ) );
        delete Item;
    }
}

void QMainOpts::on_btnAddWLX_clicked( bool checked )
{
    const auto FileFullPath = QFileDialog::getOpenFileName( this, tr( "WLX 플러그인 열기" ), QApplication::applicationDirPath(),
                                                        tr( "WLX (*.wlx;*.wlx64)" ) );

    if( FileFullPath.isEmpty() == true )
        return;

    QFileInfo FileInfo( FileFullPath );

    TyPlugInWLX WLX;
    TyPlugInWLXFuncs WLXFuncs;
    CTCPlugInWLX::RetrieveWLXFuncs( FileFullPath, &WLXFuncs );

    if( WLXFuncs.hModule == nullptr )
    {
        ShowMSGBox( this, QMessageBox::Information, tr( "" ) );
        return;
    }

    if( WLXFuncs.ListGetDetectString == nullptr )
    {
        ShowMSGBox( this, QMessageBox::Information, tr( "" ) );
        return;
    }
    
    std::vector< char > Detect( MAX_PATH );
    WLXFuncs.ListGetDetectString( Detect.data(), MAX_PATH );
    
    WLX.FileFullPath = FileFullPath;
    WLX.IsContain64Bit = true;          // TODO: 64bit OS 라면 무조건 true
    WLX.Detect = QString::fromUtf8( Detect.data() );

    const auto Row = ui.tblWLX->rowCount();
    ui.tblWLX->setRowCount( Row + 1 );
    ui.tblWLX->setItem( Row, 0, new QTableWidgetItem( FileFullPath ) );
    ui.tblWLX->setItem( Row, 1, new QTableWidgetItem( WLX.Detect ) );

    TyStPlugInMgr::GetInstance()->SetWLXOpts( UINT_MAX, WLX );

}

void QMainOpts::on_btnRemoveWLX_clicked( bool checked )
{
    QSet< int > SelectedRows;
    for( auto& Item : ui.tblWLX->selectedItems() )
        SelectedRows.insert( Item->row() );

    const auto StPlugInMgr = TyStPlugInMgr::GetInstance();

    // TODO: 여러 개라면 큰 수에서 작은 수로 진행하도록 뒤집어야 한다. 
    for( auto& Row : SelectedRows )
    {
        ui.tblWLX->removeRow( Row );
        StPlugInMgr->DelWLXOpts( Row );
    }
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

    ui.btnApply->setEnabled( false );
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

    // TyEnSizeStyle 과 순서가 일치해야 한다.
    const auto SizeInStyles = QStringList() << "Bytes" << "KBytes"
        << "유동적( x.xxx k/M/G )"
        << "유동적( x.xx  k/M/G )"
        << "유동적( x.x   k/M/G )"
        << "유동적( x     k/M/G )"
        << "유동적( x.xxx k/M/G/T )"
        << "유동적( x.xx  k/M/G/T )"
        << "유동적( x.x   k/M/G/T )"
        << "유동적( x     k/M/G/T )"
        << "유동적( x.x   k/M )"
        << "유동적( x     k/M )"
        ;

    ui.cbxHeaderSizeDisplay->clear();
    ui.cbxHeaderSizeDisplay->addItems( SizeInStyles );
    ui.cbxHeaderSizeDisplay->setEditable( true );
    ui.cbxHeaderSizeDisplay->lineEdit()->setReadOnly( true );
    ui.cbxHeaderSizeDisplay->lineEdit()->setAlignment( Qt::AlignCenter );
    ui.cbxFooterSizeDisplay->clear();
    ui.cbxFooterSizeDisplay->addItems( SizeInStyles );
    ui.cbxFooterSizeDisplay->setEditable( true );
    ui.cbxFooterSizeDisplay->lineEdit()->setReadOnly( true );
    ui.cbxFooterSizeDisplay->lineEdit()->setAlignment( Qt::AlignCenter );

    oo_insertEmptyFileSetColorRow();

    on_btnResetFileSet_clicked();
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

void QMainOpts::pickButtonColor( bool checked )
{
    Q_ASSERT( sender() != nullptr );
    Q_ASSERT( qobject_cast< QPushButton* >( sender() ) != nullptr );

    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( static_cast< QPushButton* >( sender() ), Selected );

    SaveSettings();
}

