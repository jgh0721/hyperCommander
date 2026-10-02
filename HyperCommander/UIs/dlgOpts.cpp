#include "stdafx.h"
#include "dlgOpts.hpp"

#include "dlgDefaultCMD.hpp"
#include "Components/cmpView.hpp"
#include "UniqueLibs/qtitanColumnAdapter.hpp"
#include "UniqueLibs/qtColumnProfileEditor.hpp"
#include "UniqueLibs/columnPluginFields.hpp"
#include "UniqueLibs/builtInFsModel.hpp"

#include "cmnHCUtils.hpp"
#include "Qt/cmnQtUtils.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/qtPluginHostSettings.hpp"

#include "cmnTypeDefs.hpp"
#include "fileSetMgr.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QBaseUI( parent, flags )
{
    hc::settings::AppSettings().BeginEdit();
    connect(this, &QDialog::finished, this, [] {
        hc::settings::AppSettings().DiscardEdit();
        TyStColorSchemeMgr::GetInstance()->Refresh(); TyStFileSetMgr::GetInstance()->Refresh();
        TyStPlugInMgr::GetInstance()->Refresh(); TyStCommandMgr::GetInstance()->Refresh();
    });
    ui.setupUi( this );
    // The legacy display page used 22-pixel combo boxes at absolute positions.
    // Let font metrics drive every row and scroll the page on smaller screens.
    auto* DisplayLayout = new QVBoxLayout(ui.pgDisplay);
    auto* DisplayScroll = new QScrollArea(ui.pgDisplay); DisplayScroll->setWidgetResizable(true);
    DisplayScroll->setFrameShape(QFrame::NoFrame); DisplayLayout->addWidget(DisplayScroll);
    auto* DisplayBody = new QWidget; auto* DisplayRows = new QVBoxLayout(DisplayBody);
    DisplayScroll->setWidget(DisplayBody);
    auto* Icons = new QVBoxLayout(ui.grpIconDisplay);
    for (auto* Choice : {ui.radioButton_7, ui.radioButton_6, ui.radioButton_5, ui.radioButton_4}) Icons->addWidget(Choice);
    iconModes_ = new QButtonGroup(this);
    int IconIndex = 0;
    for (auto* Choice : {ui.radioButton_7, ui.radioButton_6, ui.radioButton_5, ui.radioButton_4}) iconModes_->addButton(Choice, IconIndex++);
    ui.radioButton_7->setText(tr("모든 연결된 파일 + EXE/LNK (느림)"));
    directoryStyles_ = new QButtonGroup(this);
    int DirectoryIndex = 0;
    for (auto* Choice : {ui.radioButton, ui.radioButton_2, ui.radioButton_3}) directoryStyles_->addButton(Choice, DirectoryIndex++);
    Icons->addWidget(ui.chkIsDimmedIconWhenHidden); DisplayRows->addWidget(ui.grpIconDisplay);
    auto* Sizes = new QFormLayout;
    Sizes->addRow(ui.lblHeaderSizeDisplay, ui.cbxHeaderSizeDisplay);
    Sizes->addRow(ui.lblFooterSizeDisplay, ui.cbxFooterSizeDisplay); DisplayRows->addLayout(Sizes);
    DisplayRows->addWidget(ui.lblSortMethod);
    for (auto* Choice : {ui.chkSortCaseSensitive, ui.chkSortLocaleAware, ui.chkSortNumericAware}) DisplayRows->addWidget(Choice);
    auto* DirectoryRows = new QVBoxLayout;
    for (auto* Choice : {ui.radioButton, ui.radioButton_2, ui.radioButton_3}) DirectoryRows->addWidget(Choice);
    DisplayRows->addWidget(ui.lblDirectory); DisplayRows->addLayout(DirectoryRows);
    ui.widget_4->hide();
    auto* FolderSizes = new QVBoxLayout(ui.groupBox);
    FolderSizes->addWidget(ui.checkBox); FolderSizes->addWidget(ui.comboBox); DisplayRows->addWidget(ui.groupBox);
    DisplayRows->addStretch();
    ui.lstOptCate->setStyleSheet({});
    ui.horizontalSpacer_4->changeSize(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    ui.cbxColorScheme->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    for (auto* Frame : {ui.frmTitleBar, ui.frmFooter}) {
        Frame->setMinimumHeight(0); Frame->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    for (auto* Button : {ui.btnLstFGColor, ui.btnLstBGColor, ui.btnLstCursorColor, ui.btnLstSelectColor}) {
        Button->setMinimumWidth(0); Button->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    for (auto* Combo : {ui.cbxColorScheme, ui.cbxHeaderSizeDisplay, ui.cbxFooterSizeDisplay})
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    ui.groupBox_3->setTitle(tr("플러그인 실행 설정"));
    auto* WcxLayout = new QVBoxLayout(ui.groupBox_3);
    auto* PluginTabs = new QTabWidget(ui.groupBox_3); PluginTabs->setObjectName("pluginHostTabs"); WcxLayout->addWidget(PluginTabs);
    wcxHostSettings_ = new QPluginHostSettings(PluginTabs); PluginTabs->addTab(wcxHostSettings_, tr("압축 (WCX)"));
    wlxHostSettings_ = new QPluginHostSettings(hc::plugins::PluginFormat::Wlx, PluginTabs); PluginTabs->addTab(wlxHostSettings_, tr("뷰어 선택·WLX"));

    wfxHostSettings_ = new QPluginHostSettings(hc::plugins::PluginFormat::Wfx, PluginTabs); PluginTabs->addTab(wfxHostSettings_, tr("파일 시스템 (WFX)"));
    wdxHostSettings_ = new QPluginHostSettings(hc::plugins::PluginFormat::Wdx, PluginTabs); PluginTabs->addTab(wdxHostSettings_, tr("파일 정보 (WDX)"));

    dopusVfsSettings_ = new QPluginHostSettings(hc::plugins::PluginFormat::DopusVfs, PluginTabs); PluginTabs->addTab(dopusVfsSettings_, tr("DOpus 파일 시스템"));
    dopusViewerSettings_ = new QPluginHostSettings(hc::plugins::PluginFormat::DopusViewer, PluginTabs); PluginTabs->addTab(dopusViewerSettings_, tr("DOpus 뷰어·정보"));

    initialize();

    QMetaObject::invokeMethod( this, "LoadSettings", Qt::QueuedConnection );
}

QMainOpts::~QMainOpts()
{
    if (hc::settings::AppSettings().Editing()) {
        hc::settings::AppSettings().DiscardEdit();
        TyStColorSchemeMgr::GetInstance()->Refresh(); TyStFileSetMgr::GetInstance()->Refresh();
        TyStPlugInMgr::GetInstance()->Refresh(); TyStCommandMgr::GetInstance()->Refresh();
    }
}

void QMainOpts::LoadSettings()
{
    int Value = 0;
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    const auto StFileSetMgr = TyStFileSetMgr::GetInstance();


    Value = hc::settings::AppSettings().SizeStyle(true);
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxHeaderSizeDisplay->setCurrentIndex( Value );

    Value = hc::settings::AppSettings().SizeStyle(false);
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxFooterSizeDisplay->setCurrentIndex( Value );


    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    iconModes_->button(qMax(0, QStringList{"all", "associated", "standard", "none"}.indexOf(General["iconMode"].toString())))->setChecked(true);
    directoryStyles_->button(qMax(0, QStringList{"plain", "brackets", "suffix"}.indexOf(General["directoryStyle"].toString())))->setChecked(true);
    ui.chkIsDimmedIconWhenHidden->setChecked(General["dimHiddenIcons"].toBool(true));
    ui.chkSortCaseSensitive->setChecked(General["sortCaseSensitive"].toBool());
    ui.chkSortLocaleAware->setChecked(General["sortLocaleAware"].toBool(true));
    ui.chkSortNumericAware->setChecked(General["sortNumericAware"].toBool(true));
    focusPreviousDirectory_->setChecked(General["focusPreviousDirectory"].toBool(true));
    StColorSchemeMgr->Refresh();
    StFileSetMgr->Refresh();

    const auto SelectedScheme = StColorSchemeMgr->GetCurrentColorScheme();
    {
        const QSignalBlocker Block(ui.cbxColorScheme);
        ui.cbxColorScheme->clear(); ui.cbxColorScheme->addItems(StColorSchemeMgr->GetNames());
        ui.cbxColorScheme->setCurrentText(SelectedScheme);
    }
    RefreshColorScheme(SelectedScheme);

    ui.lstFileSet->clear();
    ui.lstFileSet->addItems( StFileSetMgr->GetNames() );

    RefreshPluginList();
    TyStCommandMgr::GetInstance()->Refresh();
    RefreshShortcuts();
}

void QMainOpts::SetListView(CGridView* View)
{
    listView_ = View;
    const auto* Model = View ? qobject_cast<CFSModel*>(View->BaseModel()) : nullptr;
    columnEditor_->SetContextFields(Model ? Model->AvailableColumns() : QVector<hc::columns::FieldDefinition>{});
    columnEditor_->Load(View ? View->ColumnProfile() : hc::columns::DefaultProfile(),
        hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject()["ColumnsV2/Profiles"].toObject(),
        View && View->ColumnWidths()->Writable());
}

void QMainOpts::ShowListSettingsPage()
{
    ui.lstOptCate->setCurrentRow(PAGE_CUSTOM_COLUMNS.first);
}

bool QMainOpts::SaveSettings()
{
    const bool ColumnsChanged = listView_ && columnEditor_->Dirty();
    if (ColumnsChanged) {
        if (!columnEditor_->ValidateDraft()) { ShowListSettingsPage(); return false; }
        auto Columns = hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject();
        Columns["ColumnsV2/Profiles"] = columnEditor_->Library();
        if (columnEditor_->NewTabDefault()) Columns["ColumnsV2/Defaults/" + listView_->ColumnWidths()->PaneKey()] = hc::columns::EncodeProfile(columnEditor_->Profile());
        hc::settings::AppSettings().Put(hc::settings::Section::Columns, Columns);
    }
    auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    General["sizeInHeader"] = hc::settings::SizeFormats().value(ui.cbxHeaderSizeDisplay->currentIndex(), "bytes");
    General["sizeInFooter"] = hc::settings::SizeFormats().value(ui.cbxFooterSizeDisplay->currentIndex(), "bytes");
    General["iconMode"] = QStringList{"all", "associated", "standard", "none"}.value(iconModes_->checkedId(), "standard");
    General["directoryStyle"] = QStringList{"plain", "brackets", "suffix"}.value(directoryStyles_->checkedId(), "plain");
    General["dimHiddenIcons"] = ui.chkIsDimmedIconWhenHidden->isChecked();
    General["sortCaseSensitive"] = ui.chkSortCaseSensitive->isChecked();
    General["sortLocaleAware"] = ui.chkSortLocaleAware->isChecked();
    General["sortNumericAware"] = ui.chkSortNumericAware->isChecked();
    General["focusPreviousDirectory"] = focusPreviousDirectory_->isChecked();
    hc::settings::AppSettings().Put(hc::settings::Section::General, General);
    SaveSettings_ColorScheme(); TyStFileSetMgr::GetInstance()->SaveSettings(); SaveSettings_PluginList();
    TyStCommandMgr::GetInstance()->SaveSettings();
    if (!hc::settings::AppSettings().Commit()) {
        QMessageBox Message(QMessageBox::Warning, tr("설정 저장 실패"),
            tr("편집 내용은 창에 유지됩니다. 다시 읽으면 디스크의 설정으로 돌아갑니다.\n%1").arg(hc::settings::AppSettings().Error()), QMessageBox::Close, this);
        auto* Reload = Message.addButton(tr("디스크에서 다시 읽기"), QMessageBox::ResetRole);
        Message.exec();
        if (Message.clickedButton() == Reload) { hc::settings::AppSettings().Reload(); hc::settings::AppSettings().BeginEdit(); SetListView(listView_); LoadSettings(); }
        return false;
    }
    hc::columns::PluginFields::Instance().Refresh();
    if (ColumnsChanged) {
        listView_->ApplyColumnProfile(columnEditor_->Profile());
        emit listView_->sigViewSettingsChanged();
        columnEditor_->MarkSaved();
    }
    const auto Scheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme(ui.cbxColorScheme->currentText());
    emit TyStColorSchemeMgr::GetInstance()->sigColorSchemeChanged(Scheme);
    emit NotifyColorSchemeChanged(Scheme);
    return true;
}

void QMainOpts::SaveSettings_ColorScheme()
{
    TyColorScheme ColorScheme = {};

    ColorScheme.Name                = ui.edtColorSchemeName->text();
    if (ColorScheme.Name.trimmed().isEmpty()) return;
    ColorScheme.IsDarkMode          = ui.chkIsDarkMode->isChecked();
    ColorScheme.IsInverseCursor     = ui.chkInverseCursor->isChecked();
    ColorScheme.IsInverseSelect     = ui.chkInverseSelection->isChecked();

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
    wcxHostSettings_->Save(hc::settings::AppSettings());
    wlxHostSettings_->Save(hc::settings::AppSettings());
    wfxHostSettings_->Save(hc::settings::AppSettings());
    wdxHostSettings_->Save(hc::settings::AppSettings());
    dopusVfsSettings_->Save(hc::settings::AppSettings()); dopusViewerSettings_->Save(hc::settings::AppSettings());
    TyStPlugInMgr::GetInstance()->Refresh();
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
    ui.chkInverseCursor->setChecked(ColorScheme.IsInverseCursor);
    ui.chkInverseSelection->setChecked(ColorScheme.IsInverseSelect);

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
    wcxHostSettings_->Load(hc::settings::AppSettings());
    wlxHostSettings_->Load(hc::settings::AppSettings());
    wfxHostSettings_->Load(hc::settings::AppSettings());
    wdxHostSettings_->Load(hc::settings::AppSettings());
    dopusVfsSettings_->Load(hc::settings::AppSettings()); dopusViewerSettings_->Load(hc::settings::AppSettings());
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

void QMainOpts::RefreshShortcuts()
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    StCommandMgr->Refresh();

    ui.tblShortCut->clearContents();
    ui.tblShortCut->setRowCount( 0 );

    const auto Map = StCommandMgr->Retrieve();
    for( const auto Key : Map.keys() )
    {
        const auto CMD = Map[ Key ];
        const auto CMD_Default = StCommandMgr->GetDefaultCMD( CMD );

        const auto Row = ui.tblShortCut->rowCount();

        ui.tblShortCut->setRowCount( Row + 1 );
        ui.tblShortCut->setItem( Row, 0, new QTableWidgetItem( CMD ) );
        ui.tblShortCut->setItem( Row, 1, new QTableWidgetItem( QKeySequence( CMD_Default.Default ).toString() ) );
        if( Key == CMD_Default.Default )
            ui.tblShortCut->setItem( Row, 2, new QTableWidgetItem() );
        else
            ui.tblShortCut->setItem( Row, 2, new QTableWidgetItem( QKeySequence( Key ).toString() ) );
        ui.tblShortCut->setItem( Row, 3, new QTableWidgetItem( CMD_Default.Desc ) );
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

void QMainOpts::on_btnDlgFGColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnDlgFGColor, Selected );
}

void QMainOpts::on_btnDlgBGColor_clicked( bool checked )
{
    const auto Selected = QColorDialog::getColor( Qt::white, this );

    if( Selected.isValid() == true )
        setButtonColor( ui.btnDlgBGColor, Selected );
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

void QMainOpts::on_btnAddShortcutCMD_clicked( bool checked )
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto Input = ui.keySequenceEdit->keySequence();
    if( Input.isEmpty() == true )
    {
        // TODO: 안내 메시지
        return;
    }

    QDefaultCMDUI Ui;

    const auto Ret = Ui.exec();
    if( Ret == Rejected )
        return;

    const auto Selected = Ui.GetSelectedCMD();
    if( Selected.Name.isEmpty() == true )
    {
        // TODO: 안내 메시지
        return;
    }

    StCommandMgr->SetShortcutWithCMD( Input[ 0 ], Selected.Name );
    const auto Row = ui.tblShortCut->rowCount();
    ui.tblShortCut->setRowCount( Row + 1 );
    ui.tblShortCut->setItem( Row, 0, new QTableWidgetItem( Selected.Name ) );
    ui.tblShortCut->setItem( Row, 1, new QTableWidgetItem( QKeySequence( Selected.Default ).toString() ) );
    ui.tblShortCut->setItem( Row, 2, new QTableWidgetItem( Input.toString() ) );
    ui.tblShortCut->setItem( Row, 3, new QTableWidgetItem( Selected.Desc ) );

    RefreshShortcuts();
}

void QMainOpts::on_btnDelShortcutCMD_clicked( bool checked )
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    QSet< int > SelectedRows = nsHC::SelectedRowsFromTbl( ui.tblShortCut );

    for( const auto Row : SelectedRows )
    {
        const auto Default_Key = ui.tblShortCut->item( Row, 1 )->text();
        const auto Override_Key = ui.tblShortCut->item( Row, 2 )->text();

        const QString Key = Override_Key.isEmpty() == false ? Override_Key : Default_Key;
        StCommandMgr->DelShortcut( QKeySequence( Key )[0] );
    }

    RefreshShortcuts();
}

void QMainOpts::on_btnAddWLX_clicked( bool checked )
{
    const auto FileFullPath = QFileDialog::getOpenFileName( this, tr( "WLX 플러그인 열기" ), QApplication::applicationDirPath(),
                                                        tr( "WLX (*.wlx *.wlx64)" ) );

    if( FileFullPath.isEmpty() == true )
        return;

    TyPlugInWLX WLX;
    // Registration records a path only. The manifest review inspects the DLL
    // asynchronously, and SDK exports are called only by the approved host.
    WLX.FileFullPath = FileFullPath;
    WLX.IsContain64Bit = FileFullPath.endsWith("64", Qt::CaseInsensitive);
    WLX.hostEnabled_ = true;

    TyStPlugInMgr::GetInstance()->SetWLXOpts( UINT_MAX, WLX );
    RefreshPluginList();
}

void QMainOpts::on_btnRemoveWLX_clicked( bool checked )
{
    QSet< int > SelectedRows;
    for( auto& Item : ui.tblWLX->selectedItems() )
        SelectedRows.insert( Item->row() );

    const auto StPlugInMgr = TyStPlugInMgr::GetInstance();

    auto Rows = SelectedRows.values(); std::sort(Rows.begin(), Rows.end(), std::greater<int>{});
    for( auto Row : Rows )
    {
        ui.tblWLX->removeRow( Row );
        StPlugInMgr->DelWLXOpts( Row );
    }
    RefreshPluginList();
}

///////////////////////////////////////////////////////////////////////////////
/// Footer

void QMainOpts::on_btnOK_clicked( bool checked )
{
    if (SaveSettings()) accept();
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
    ui.lstOptCate->addItem( PAGE_PLUGIN_MGR.second );
    columnEditor_ = new QtColumnProfileEditor(ui.pgCustomColumns);
    ui.pgCustomColumns->layout()->addWidget(columnEditor_);

    auto* NavigationPage = new QWidget(ui.stkOpts); NavigationPage->setObjectName("hc.settings.navigation");
    ui.stkOpts->addWidget(NavigationPage); ui.lstOptCate->addItem(tr("탐색"));
    auto* NavigationLayout = new QVBoxLayout(NavigationPage);
    focusPreviousDirectory_ = new QCheckBox(tr("상위 폴더(..)로 이동할 때 방금 나온 폴더에 커서 놓기"), NavigationPage);
    focusPreviousDirectory_->setObjectName("hc.navigation.focusPreviousDirectory");
    NavigationLayout->addWidget(focusPreviousDirectory_);
    auto* NavigationNote = new QLabel(tr("좌우 패널에 모두 적용합니다. 끄면 상위 폴더 목록의 첫 항목에 커서를 놓습니다."), NavigationPage);
    NavigationNote->setWordWrap(true); NavigationLayout->addWidget(NavigationNote); NavigationLayout->addStretch();
    ui.lstOptCate->setResizeMode( QListView::Adjust );
    ui.lstOptCate->setSpacing( 6 );
    ui.lstOptCate->setUniformItemSizes( true );

    ui.lstOptCate->setSizeAdjustPolicy( QAbstractScrollArea::AdjustToContents );
    ui.lstOptCate->setFixedWidth( ui.lstOptCate->sizeHintForColumn( 0 ) + ui.lstOptCate->lineWidth() * 2 + ui.lstOptCate->frameWidth() * 2
        + ui.lstOptCate->spacing() * 2 + ui.lstOptCate->fontMetrics().height() * 2 );

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
    ui.chkInverseCursor->setChecked(false);
    ui.chkInverseSelection->setChecked(false);

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

    ui.btnApply->setEnabled(true);
}

