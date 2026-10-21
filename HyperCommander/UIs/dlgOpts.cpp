#include "stdafx.h"
#include "dlgOpts.hpp"
#include <fmfilelist/RecordPreview.h>
#include "thumbnailSettingsPage.hpp"
#include "menuBarSettingsPage.hpp"
#include "themeSettingsPage.hpp"
#include "keyboardSettingsPage.hpp"
#include <QStyleHints>
#include <QTabWidget>
#include "commonLibs/operations/fileOperation.hpp"
#include "fileColorRulesPage.hpp"
#include "permissionDialogs.hpp"
#include <fmwidgets/Card.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/WindowChrome.h>

#include "dlgDefaultCMD.hpp"
#include "Components/cmpView.hpp"
#include "cmpPanel.hpp"
#include "UniqueLibs/qtitanColumnAdapter.hpp"
#include "UniqueLibs/qtColumnProfileEditor.hpp"
#include "UniqueLibs/columnAutoSelection.hpp"
#include "UniqueLibs/fileJobUi.hpp"
#include "UniqueLibs/columnPluginFields.hpp"
#include "UniqueLibs/builtInFsModel.hpp"

#include "cmnHCUtils.hpp"
#include "Qt/cmnQtUtils.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/qtPluginManagementPage.hpp"

#include "cmnTypeDefs.hpp"
#include "fileSetMgr.hpp"

QMainOpts::QMainOpts( QWidget* parent, Qt::WindowFlags flags )
    : QBaseUI( parent, flags )
{
    committedSettings_ = hc::settings::AppSettings().Object();
    hc::settings::AppSettings().BeginEdit();
    connect(this, &QDialog::finished, this, [] {
        hc::settings::AppSettings().DiscardEdit();
        TyStColorSchemeMgr::GetInstance()->Refresh(); TyStFileSetMgr::GetInstance()->Refresh();
        TyStPlugInMgr::GetInstance()->Refresh(); TyStCommandMgr::GetInstance()->Refresh();
    });
    ui.setupUi( this );
    // Use the same Watercolor chrome as the main window. The legacy .ui frame
    // had no themed painting and a fixed-size literal X close button.
    ui.frmTitleBar->hide();
    ui.verticalLayout->setContentsMargins(0, 0, 0, 0);
    auto* Chrome = new fm::ui::WindowChrome(this, this, {false, true});
    Chrome->setObjectName("hc.settings.windowChrome");
    ui.verticalLayout->insertWidget(0, Chrome);
    SetMovableUI(false); SetResizableUI(true); setSizeGripEnabled(true);
    // The legacy display page used 22-pixel combo boxes at absolute positions.
    // Let font metrics drive every row and scroll the page on smaller screens.
    auto* DisplayLayout = new QVBoxLayout(ui.pgDisplay);
    auto* PanelTabs = new QTabWidget(ui.pgDisplay); PanelTabs->setObjectName("hc.settings.filePanelTabs");
    DisplayLayout->addWidget(PanelTabs);
    auto* DisplayScroll = new QScrollArea(PanelTabs); DisplayScroll->setWidgetResizable(true);
    DisplayScroll->setObjectName("hc.settings.filePanelGeneral");
    DisplayScroll->setFrameShape(QFrame::NoFrame); PanelTabs->addTab(DisplayScroll, tr("공통"));
    auto* IconScroll = new QScrollArea(PanelTabs); IconScroll->setWidgetResizable(true);
    IconScroll->setFrameShape(QFrame::NoFrame); PanelTabs->addTab(IconScroll, tr("아이콘"));
    auto* IconBody = new QWidget; auto* IconRows = new QVBoxLayout(IconBody); IconScroll->setWidget(IconBody);
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
    Icons->addWidget(ui.chkIsDimmedIconWhenHidden); IconRows->addWidget(ui.grpIconDisplay);
    auto* ShellOptions = new QGroupBox(tr("Windows 탐색기 아이콘"), IconBody);
    auto* ShellRows = new QVBoxLayout(ShellOptions);
    specialFolderIcons_ = new QCheckBox(tr("특별한 폴더용 아이콘 보기: 파일 시스템 (예: 휴지통)(&W)"), ShellOptions);
    specialFolderIcons_->setObjectName("hc.settings.specialFolderIcons");
    virtualFolderIcons_ = new QCheckBox(tr("특별한 폴더용 아이콘 보기: 가상 폴더 (예: 네트워크)(&V)"), ShellOptions);
    virtualFolderIcons_->setObjectName("hc.settings.virtualFolderIcons");
    explorerDefaultIcons_ = new QCheckBox(tr("탐색기의 기본 폴더/파일 아이콘 보기(&F)"), ShellOptions);
    explorerDefaultIcons_->setObjectName("hc.settings.explorerDefaultIcons");
    iconOverlays_ = new QCheckBox(tr("아이콘 오버레이 보기 (예: 바로 가기의 화살표)(&C)"), ShellOptions);
    iconOverlays_->setObjectName("hc.settings.iconOverlays");
    for (auto* Check : {specialFolderIcons_, virtualFolderIcons_, explorerDefaultIcons_, iconOverlays_}) ShellRows->addWidget(Check);
    specialFolderIcons_->setToolTip(tr("Windows에 등록된 폴더별 아이콘과 desktop.ini 사용자 지정 아이콘을 표시합니다."));
    explorerDefaultIcons_->setToolTip(tr("일반 폴더와 파일에 Windows 기본 아이콘을 사용합니다. 끄면 현재 UI 스타일의 기본 아이콘을 사용합니다."));
    IconRows->addWidget(ShellOptions); IconRows->addStretch();
    connect(iconModes_, &QButtonGroup::idToggled, this, [this, ShellOptions](int, bool) {
        const bool Enabled = iconModes_->checkedId() != 3;
        ShellOptions->setEnabled(Enabled); ui.chkIsDimmedIconWhenHidden->setEnabled(Enabled);
    });
    auto* Sizes = new QFormLayout;
    Sizes->addRow(ui.lblHeaderSizeDisplay, ui.cbxHeaderSizeDisplay);
    Sizes->addRow(ui.lblFooterSizeDisplay, ui.cbxFooterSizeDisplay); DisplayRows->addLayout(Sizes);
    DisplayRows->addWidget(ui.lblSortMethod);
    for (auto* Choice : {ui.chkSortCaseSensitive, ui.chkSortLocaleAware, ui.chkSortNumericAware}) DisplayRows->addWidget(Choice);
    auto* DirectoryRows = new QVBoxLayout;
    for (auto* Choice : {ui.radioButton, ui.radioButton_2, ui.radioButton_3}) DirectoryRows->addWidget(Choice);
    DisplayRows->addWidget(ui.lblDirectory); DisplayRows->addLayout(DirectoryRows);
    ui.widget_4->hide();
    DisplayRows->addStretch();
    ui.lstOptCate->setStyleSheet({});
    // Removing the legacy stylesheet can leave an explicit light palette on
    // the item view. Refresh it with the same tokens as the product theme.
    const auto RefreshCategories = [this] {
        const auto Palette = fm::style::ThemeManager::instance().colors().toPalette();
        ui.lstOptCate->setPalette(Palette); ui.lstOptCate->viewport()->setPalette(Palette);
    };
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, RefreshCategories);
    RefreshCategories();
    for (auto* Frame : {ui.frmTitleBar, ui.frmFooter}) {
        Frame->setFrameShape(QFrame::NoFrame);
        Frame->setMinimumHeight(0); Frame->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    for (auto* Combo : {ui.cbxHeaderSizeDisplay, ui.cbxFooterSizeDisplay})
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (auto* Child : ui.pgPluginManage->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) Child->hide();
    if (auto* Old = ui.pgPluginManage->layout()) { while (auto* Item = Old->takeAt(0)) delete Item; delete Old; }
    auto* PluginLayout = new QVBoxLayout(ui.pgPluginManage); PluginLayout->setContentsMargins(0, 0, 0, 0);
    pluginManagement_ = new QPluginManagementPage(ui.pgPluginManage); PluginLayout->addWidget(pluginManagement_);

    initialize();
    initializeDesignPages();
    // Scrollable settings pages must not make the initial window as tall as
    // their full content. Qt still enforces the font-dependent minimum size.
    resize(1100, 800);

    QTimer::singleShot(0, this, [this] {
        LoadSettings(); layout()->activate();
        const auto Available = screen()->availableGeometry().size() - QSize(24, 24);
        resize(QSize(1100, 800).boundedTo(Available).expandedTo(minimumSizeHint()));
    });
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
    if (fileColors_) {
        const auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
        fileColors_->Load(Options["fileColorRules"].toObject());
        if (columnEditor_) columnEditor_->SetAutoColors(fileColors_->Document());
        if (auto* Allow=findChild<QCheckBox*>("hc.permissions.allow")) Allow->setChecked(Options["allowElevation"].toBool(true));
        defaultConflict_->setCurrentIndex(qMax(0, QStringList{"ask", "replace", "skip"}.indexOf(Options["fileConflict"].toString("ask"))));
        findChild<QCheckBox*>("hc.settings.renameInDialog")->setChecked(Options["renameInDialog"].toBool(true));
        findChild<QCheckBox*>("hc.settings.renameSelectExtension")->setChecked(Options["renameSelectExtension"].toBool(false));
        findChild<QCheckBox*>("hc.settings.renameOnSlowClick")->setChecked(Options["renameOnSlowClick"].toBool(true));
        findChild<QCheckBox*>("hc.settings.folderSizeOnSpace")->setChecked(Options["folderSizeOnSpace"].toBool(true));
        findChild<QCheckBox*>("hc.settings.folderSizeBeforeOperations")->setChecked(Options["folderSizeBeforeOperations"].toBool(true));
        findChild<QCheckBox*>("hc.settings.folderSizeUseEverything")->setChecked(Options["folderSizeUseEverything"].toBool(true));
        findChild<QComboBox*>("hc.settings.folderSizeMode")->setCurrentIndex(qMax(0, QStringList{"manual", "local", "all", "everything"}.indexOf(Options["folderSizeMode"].toString("everything"))));
        defaultMetadata_->setChecked(Options["copyMetadata"].toBool(true)); defaultVerify_->setChecked(Options["copyVerify"].toBool(false));
        defaultAcl_->setChecked(Options["copyAcl"].toBool(false)); defaultStreams_->setChecked(Options["copyStreams"].toBool(false));
        findChild<QCheckBox*>("hc.settings.delayedFileWrites")->setChecked(Options["delayedFileWrites"].toBool(true));
        defaultPattern_->setText(Options["copyPattern"].toString("*")); defaultSpeed_->setValue(Options["copySpeedMiB"].toInt(0));
        defaultDetailed_->setChecked(Options["jobDetailed"].toBool(false)); defaultClose_->setChecked(Options["jobCloseOnSuccess"].toBool(true));
        findChild<QCheckBox*>("hc.settings.recycleDefault")->setChecked(Options["deleteToRecycle"].toBool(true));
        findChild<QCheckBox*>("hc.settings.deleteWhileListing")->setChecked(Options["deleteWhileListing"].toBool(true));
        findChild<QComboBox*>("hc.settings.jobNotification")->setCurrentIndex(qMax(0, QStringList{"none", "status", "attention", "sound"}.indexOf(Options["jobNotification"].toString("attention"))));
        findChild<QComboBox*>("hc.settings.queueOrder")->setCurrentIndex(Options["queueInsertion"] == "first" ? 1 : 0);
        findChild<QSpinBox*>("hc.settings.jobConcurrency")->setValue(Options["jobConcurrency"].toInt(2));
        findChild<QCheckBox*>("hc.settings.enableJobRecovery")->setChecked(Options["enableJobRecovery"].toBool(false));
        findChild<QCheckBox*>("hc.settings.saveJobProgress")->setEnabled(Options["enableJobRecovery"].toBool(false));
        findChild<QCheckBox*>("hc.settings.saveJobProgress")->setChecked(Options["saveJobProgress"].toBool(false));
        findChild<QCheckBox*>("hc.settings.showJobDetails")->setChecked(Options["showJobDetails"].toBool(false));
    }


    Value = hc::settings::AppSettings().SizeStyle(true);
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxHeaderSizeDisplay->setCurrentIndex( Value );

    Value = hc::settings::AppSettings().SizeStyle(false);
    if( Value > HC_SIZE_STYLE_BKM )
        Value = HC_SIZE_STYLE_BYTES;
    ui.cbxFooterSizeDisplay->setCurrentIndex( Value );


    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    const auto Panel = General.value("filePanel").toObject();
    const auto Presentation = hc::ui::ReadFilePanelSettings(Panel);
    filePanelSupported_ = !Panel.contains("version") || Panel.value("version") == QJsonValue(1);
    listCard_->setEnabled(filePanelSupported_);
    if (!filePanelSupported_) listCard_->setToolTip(tr("다른 버전의 파일 패널 설정을 보존합니다."));
    listDefault_->setCurrentIndex(QStringList{"one-line", "two-line", "auto", "thumbnails"}.indexOf(Presentation.mode_));
    listNameBelow_->setChecked(Presentation.appearance_.nameBelow);
    oneLineSeparator_->setCurrentIndex(int(Presentation.appearance_.oneLineSeparator));
    twoLineSeparator_->setCurrentIndex(int(Presentation.appearance_.twoLineSeparator));
    listDensity_->setCurrentIndex(int(Presentation.appearance_.density));
    listElide_->setCurrentIndex(int(Presentation.appearance_.nameElide));
    listAutoWidth_->setValue(Presentation.appearance_.autoWidth);
    listAutoPercent_->setValue(Presentation.appearance_.autoPercent);
    resetListModeDraft_ = false;
    iconModes_->button(qMax(0, QStringList{"all", "associated", "standard", "none"}.indexOf(General["iconMode"].toString())))->setChecked(true);
    directoryStyles_->button(qMax(0, QStringList{"plain", "brackets", "suffix"}.indexOf(General["directoryStyle"].toString())))->setChecked(true);
    ui.chkIsDimmedIconWhenHidden->setChecked(General["dimHiddenIcons"].toBool(true));
    specialFolderIcons_->setChecked(General["specialFolderIcons"].toBool(true));
    virtualFolderIcons_->setChecked(General["virtualFolderIcons"].toBool(true));
    explorerDefaultIcons_->setChecked(General["explorerDefaultIcons"].toBool(true));
    iconOverlays_->setChecked(General["iconOverlays"].toBool(true));
    ui.chkSortCaseSensitive->setChecked(General["sortCaseSensitive"].toBool());
    ui.chkSortLocaleAware->setChecked(General["sortLocaleAware"].toBool(true));
    ui.chkSortNumericAware->setChecked(General["sortNumericAware"].toBool(true));
    focusPreviousDirectory_->setChecked(General["focusPreviousDirectory"].toBool(true));
    const auto QuickFind = hc::quickfind::Settings::FromJson(General["quickFind"].toObject()).ToJson();
    for (auto It = QuickFind.begin(); It != QuickFind.end(); ++It) {
        const auto Name = "hc.navigation.quickFind." + It.key();
        if (auto* Check = findChild<QCheckBox*>(Name)) Check->setChecked(It.value().toBool());
        else if (auto* Choice = findChild<QComboBox*>(Name)) Choice->setCurrentIndex(Choice->findData(It.value().toString()));
        else if (auto* Interval = findChild<QSpinBox*>(Name)) Interval->setValue(It.value().toInt());
    }
    const auto Refresh = hc::refresh::Settings::FromJson(General["refresh"].toObject()).ToJson();
    for (auto It = Refresh.begin(); It != Refresh.end(); ++It) {
        const auto Name = "hc.navigation.refresh." + It.key();
        if (auto* Check = findChild<QCheckBox*>(Name)) Check->setChecked(It.value().toBool());
        else if (auto* Seconds = findChild<QSpinBox*>(Name)) Seconds->setValue(It.value().toInt());
        else if (auto* Text = findChild<QLineEdit*>(Name)) Text->setText(It.value().toString());
    }
    if (auto* Preset = findChild<QComboBox*>("hc.settings.shortcutPreset")) {
        const QSignalBlocker Block(Preset); Preset->setCurrentIndex(General["shortcutPreset"].toString("legacy") == "prototype" ? 1 : 0);
    }
    StColorSchemeMgr->Refresh();
    TyStFileSetMgr::GetInstance()->Refresh();

    const auto SelectedScheme = StColorSchemeMgr->GetCurrentColorScheme();
    RefreshColorScheme(SelectedScheme);
    thumbnailSettings_->Load(General.value("thumbnails").toObject());
    const auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
    themeSettings_->LoadLibrary(Appearance["watercolorPresets"].toObject());
    themeSettings_->Load(Appearance["watercolorTheme"].toObject());
    if (auto* Theme = findChild<QComboBox*>("hc.settings.theme")) { const QSignalBlocker Block(Theme); const int Mode = General["themeMode"] == "system" ? 2 : StColorSchemeMgr->GetColorScheme(SelectedScheme).IsDarkMode ? 1 : 0; Theme->setCurrentIndex(Mode); if (auto* Card = findChild<fm::ui::ThemeModeCard*>("hc.settings.themeCard" + QString::number(Mode))) Card->setChecked(true); }
    findChild<fm::ui::AccentPicker*>("hc.settings.accent")->setEnabled(themeSettings_->isEnabled());
    findChild<QCheckBox*>("hc.settings.systemAccent")->setEnabled(themeSettings_->isEnabled());
    findChild<fm::ui::AccentPicker*>("hc.settings.accent")->setCurrent(themeSettings_->Scheme().seeds.accent);
    { const QSignalBlocker Block(findChild<QCheckBox*>("hc.settings.systemAccent")); findChild<QCheckBox*>("hc.settings.systemAccent")->setChecked(themeSettings_->Scheme().seeds.useSystemAccent); }
    findChild<QCheckBox*>("hc.settings.darkTitle")->setChecked(General["darkTitleBar"].toBool(true));
    findChild<QCheckBox*>("hc.settings.showCommandLine")->setChecked(General["showCommandLine"].toBool(true));
    findChild<QCheckBox*>("hc.settings.showFunctionBar")->setChecked(General["showFunctionBar"].toBool(true));
    findChild<QComboBox*>("hc.settings.startup")->setCurrentIndex(General["restoreWorkspace"].toBool(true) ? 0 : 1);
    findChild<QCheckBox*>("hc.settings.singleInstance")->setChecked(General["singleInstance"].toBool(false));
    findChild<QComboBox*>("hc.settings.newTabPosition")->setCurrentIndex(qMax(0, QStringList{"last", "right", "left", "first"}.indexOf(General["newTabPosition"].toString("last"))));
    findChild<QFontComboBox*>("hc.settings.monoFont")->setCurrentFont(QFont(General["monoFontFamily"].toString("Cascadia Mono")));
    findChild<QSpinBox*>("hc.settings.monoSize")->setValue(General["monoFontPx"].toInt(12));


    RefreshPluginList();
    TyStCommandMgr::GetInstance()->Refresh();
    RefreshShortcuts();
    if (keyboardSettings_) keyboardSettings_->Load();
}

void QMainOpts::SetListView(CGridView* View)
{
    listView_ = View;
    useListDefault_->setEnabled(View != nullptr);
    listScope_->setText(View ? tr("기본값은 이를 따르는 탭과 새 탭에 적용합니다. 현재 탭: %1")
        .arg(View->ListModeOverride() == "inherit" ? tr("설정 기본값 따름") : View->ListMode()) :
        tr("기본값은 이를 따르는 탭과 새 탭에 적용합니다. 패널 버튼에서 선택한 보기는 탭별로 유지합니다."));
    const auto* Model = View ? qobject_cast<CFSModel*>(View->BaseModel()) : nullptr;
    QStringList Selected = View ? View->CaptureSelection() : QStringList{};
    if (Selected.isEmpty() && View && !View->CaptureFocus().isEmpty()) Selected.append(View->CaptureFocus());
    columnEditor_->SetSamples(Model ? Model->ColumnSamples() : QVector<hc::columns::PreviewSample>{}, Model && !Selected.isEmpty() ? Model->ColumnSamples(Selected) : QVector<hc::columns::PreviewSample>{});
    // Follow the preview pane by default; an explicit simulated width remains
    // available in its options without inheriting the live panel's full width.
    columnEditor_->SetPreviewWidth(0);
    columnEditor_->SetContextFields(Model ? Model->AvailableColumns() : QVector<hc::columns::FieldDefinition>{});
    columnEditor_->Load(View ? View->ColumnProfile() : hc::columns::DefaultProfile(),
        hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject()["ColumnsV2/Profiles"].toObject(),
        View && View->ColumnWidths()->Writable());
    columnEditor_->LoadAutoSelection(hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject()[hc::columns::AutoSelectionKey].toObject(),
        fileColors_->Document(), View ? View->AutomaticColumns() : true);
}

void QMainOpts::ShowListSettingsPage()
{
    for (int Row = 0; Row < ui.lstOptCate->count(); ++Row) if (ui.lstOptCate->item(Row)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(ui.pgCustomColumns)) ui.lstOptCate->setCurrentRow(Row);
}

void QMainOpts::ShowPluginSettingsPage(const QString& Binary, bool Review)
{
    for (int Row = 0; Row < ui.lstOptCate->count(); ++Row)
        if (ui.lstOptCate->item(Row)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(ui.pgPluginManage)) ui.lstOptCate->setCurrentRow(Row);
    if (!Binary.isEmpty()) pluginManagement_->SelectPlugin(Binary, Review);
}

bool QMainOpts::SaveSettings()
{
    if (pluginManagement_ && !pluginManagement_->Validate()) return false;
    const auto PreviousSettings = committedSettings_;
    if (menuSettings_ && !menuSettings_->Prepare()) {
        for (int Row = 0; Row < ui.lstOptCate->count(); ++Row) if (ui.lstOptCate->item(Row)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(menuSettings_)) ui.lstOptCate->setCurrentRow(Row);
        return false;
    }
    const auto CancelMenu = qScopeGuard([this] { if (menuSettings_) menuSettings_->CancelPrepared(); });
    if (fileColors_ && !fileColors_->Validate()) {
        for (int Row = 0; Row < ui.lstOptCate->count(); ++Row) if (ui.lstOptCate->item(Row)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(fileColors_)) ui.lstOptCate->setCurrentRow(Row);
        return false;
    }
    if (!hc::operations::IsValidCopyPattern(defaultPattern_->text().trimmed().toStdString())) {
        auto* Jobs = findChild<QWidget*>("hc.settings.fileOperations");
        for (int Row = 0; Row < ui.lstOptCate->count(); ++Row)
            if (ui.lstOptCate->item(Row)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(Jobs)) ui.lstOptCate->setCurrentRow(Row);
        findChild<QTabWidget*>("hc.settings.fileOperationsTabs")->setCurrentIndex(1);
        defaultCopyError_->setVisible(true); defaultPattern_->setFocus();
        findChild<QScrollArea*>("hc.settings.fileOperationsCopy")->ensureWidgetVisible(defaultCopyError_);
        return false;
    }
    defaultCopyError_->setVisible(false);
    auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
    Options["fileColorRules"] = fileColors_->Document();
    if (auto* Allow=findChild<QCheckBox*>("hc.permissions.allow")) Options["allowElevation"]=Allow->isChecked();
    Options["fileConflict"] = QStringList{"ask", "replace", "skip"}.value(defaultConflict_->currentIndex(), "ask");
    Options["renameInDialog"] = findChild<QCheckBox*>("hc.settings.renameInDialog")->isChecked();
    Options["renameSelectExtension"] = findChild<QCheckBox*>("hc.settings.renameSelectExtension")->isChecked();
    Options["renameOnSlowClick"] = findChild<QCheckBox*>("hc.settings.renameOnSlowClick")->isChecked();
    for (const auto* Key : {"folderSizeOnSpace", "folderSizeBeforeOperations", "folderSizeUseEverything"})
        Options[Key] = findChild<QCheckBox*>(QString("hc.settings.") + Key)->isChecked();
    Options["folderSizeMode"] = QStringList{"manual", "local", "all", "everything"}.value(findChild<QComboBox*>("hc.settings.folderSizeMode")->currentIndex(), "everything");
    Options["copyMetadata"] = defaultMetadata_->isChecked(); Options["copyVerify"] = defaultVerify_->isChecked();
    Options["copyAcl"] = defaultAcl_->isChecked(); Options["copyStreams"] = defaultStreams_->isChecked();
    Options["delayedFileWrites"] = findChild<QCheckBox*>("hc.settings.delayedFileWrites")->isChecked();
    Options["copyPattern"] = defaultPattern_->text().trimmed(); Options["copySpeedMiB"] = defaultSpeed_->value();
    Options["jobDetailed"] = defaultDetailed_->isChecked(); Options["jobCloseOnSuccess"] = defaultClose_->isChecked();
    Options["deleteToRecycle"] = findChild<QCheckBox*>("hc.settings.recycleDefault")->isChecked();
    Options["deleteWhileListing"] = findChild<QCheckBox*>("hc.settings.deleteWhileListing")->isChecked();
    Options["jobNotification"] = QStringList{"none", "status", "attention", "sound"}.value(findChild<QComboBox*>("hc.settings.jobNotification")->currentIndex(), "attention");
    Options["queueInsertion"] = findChild<QComboBox*>("hc.settings.queueOrder")->currentIndex() == 1 ? "first" : "last";
    Options["jobConcurrency"] = findChild<QSpinBox*>("hc.settings.jobConcurrency")->value();
    Options["enableJobRecovery"] = findChild<QCheckBox*>("hc.settings.enableJobRecovery")->isChecked();
    Options["saveJobProgress"] = findChild<QCheckBox*>("hc.settings.saveJobProgress")->isChecked();
    Options["showJobDetails"] = findChild<QCheckBox*>("hc.settings.showJobDetails")->isChecked();
    hc::settings::AppSettings().Put(hc::settings::Section::Options, Options);
    const bool ColumnsChanged = listView_ && columnEditor_->Dirty();
    QVector<QPointer<CGridView>> Targets; QVector<hc::columns::ColumnProfile> PreviousProfiles;
    if (ColumnsChanged && columnEditor_->ApplyCurrent()) {
        Targets.append(listView_);
        if (columnEditor_->BothPanels() && parentWidget()) for (auto* Panel : parentWidget()->window()->findChildren<CmpPanel*>()) {
            auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
            if (View && View != listView_) Targets.append(View);
        }
        for (const auto& View : Targets) {
            if (!View || !View->BaseModel() || !View->ColumnWidths()->Writable()) { ShowListSettingsPage(); return false; }
            PreviousProfiles.append(View->ColumnProfile());
        }
    }
    if (ColumnsChanged) {
        columnEditor_->SetAutoColors(fileColors_->Document());
        if (!columnEditor_->ValidateDraft()) { ShowListSettingsPage(); return false; }
        auto Columns = hc::settings::AppSettings().Read(hc::settings::Section::Columns).toObject();
        Columns["ColumnsV2/Profiles"] = columnEditor_->Library();
        Columns[hc::columns::AutoSelectionKey] = columnEditor_->AutoSelection();
        if (columnEditor_->NewTabDefault()) Columns["ColumnsV2/Defaults/" + listView_->ColumnWidths()->PaneKey()] = hc::columns::EncodeProfile(columnEditor_->Profile());
        hc::settings::AppSettings().Put(hc::settings::Section::Columns, Columns);
    }
    auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    General["sizeInHeader"] = hc::settings::SizeFormats().value(ui.cbxHeaderSizeDisplay->currentIndex(), "bytes");
    General["sizeInFooter"] = hc::settings::SizeFormats().value(ui.cbxFooterSizeDisplay->currentIndex(), "bytes");
    General["iconMode"] = QStringList{"all", "associated", "standard", "none"}.value(iconModes_->checkedId(), "standard");
    General["directoryStyle"] = QStringList{"plain", "brackets", "suffix"}.value(directoryStyles_->checkedId(), "plain");
    General["dimHiddenIcons"] = ui.chkIsDimmedIconWhenHidden->isChecked();
    General["specialFolderIcons"] = specialFolderIcons_->isChecked();
    General["virtualFolderIcons"] = virtualFolderIcons_->isChecked();
    General["explorerDefaultIcons"] = explorerDefaultIcons_->isChecked();
    General["iconOverlays"] = iconOverlays_->isChecked();
    General["sortCaseSensitive"] = ui.chkSortCaseSensitive->isChecked();
    General["sortLocaleAware"] = ui.chkSortLocaleAware->isChecked();
    General["sortNumericAware"] = ui.chkSortNumericAware->isChecked();
    General["focusPreviousDirectory"] = focusPreviousDirectory_->isChecked();
    auto QuickFind = General["quickFind"].toObject();
    const auto QuickDefaults = hc::quickfind::Settings{}.ToJson();
    for (auto It = QuickDefaults.begin(); It != QuickDefaults.end(); ++It) {
        const auto Name = "hc.navigation.quickFind." + It.key();
        if (auto* Check = findChild<QCheckBox*>(Name)) QuickFind[It.key()] = Check->isChecked();
        else if (auto* Choice = findChild<QComboBox*>(Name)) QuickFind[It.key()] = Choice->currentData().toString();
        else if (auto* Interval = findChild<QSpinBox*>(Name)) QuickFind[It.key()] = Interval->value();
    }
    General["quickFind"] = QuickFind;
    auto Refresh = General["refresh"].toObject();
    const auto RefreshDefaults = hc::refresh::Settings{}.ToJson();
    for (auto It = RefreshDefaults.begin(); It != RefreshDefaults.end(); ++It) {
        const auto Name = "hc.navigation.refresh." + It.key();
        if (auto* Check = findChild<QCheckBox*>(Name)) Refresh[It.key()] = Check->isChecked();
        else if (auto* Seconds = findChild<QSpinBox*>(Name)) Refresh[It.key()] = Seconds->value();
        else if (auto* Text = findChild<QLineEdit*>(Name)) Refresh[It.key()] = Text->text().trimmed();
    }
    General["refresh"] = Refresh;
    if (filePanelSupported_) {
        auto Panel = General.value("filePanel").toObject();
        Panel["version"] = 1; Panel["defaultMode"] = QStringList{"one-line", "two-line", "auto", "thumbnails"}.value(listDefault_->currentIndex(), "one-line");
        Panel["nameBelow"] = listNameBelow_->isChecked();
        const QStringList Separators{"none", "zebra", "line", "space", "tint"};
        Panel["oneLineSeparator"] = Separators.value(oneLineSeparator_->currentIndex(), "none");
        Panel["twoLineSeparator"] = Separators.value(twoLineSeparator_->currentIndex(), "line");
        Panel["density"] = QStringList{"compact", "normal", "relaxed"}.value(listDensity_->currentIndex(), "normal");
        Panel["nameElide"] = QStringList{"keep-extension", "end", "middle"}.value(listElide_->currentIndex(), "keep-extension");
        Panel["autoWidthDip"] = listAutoWidth_->value(); Panel["autoClippedPercent"] = listAutoPercent_->value();
        General["filePanel"] = Panel;
    }
    if (keyboardSettings_ && !keyboardSettings_->SaveFunctions(General)) return false;
    General["themeMode"] = QStringList{"light", "dark", "system"}.value(findChild<QComboBox*>("hc.settings.theme")->currentIndex(), "light");
    General["darkTitleBar"] = findChild<QCheckBox*>("hc.settings.darkTitle")->isChecked();
    General["showCommandLine"] = findChild<QCheckBox*>("hc.settings.showCommandLine")->isChecked();
    General["showFunctionBar"] = findChild<QCheckBox*>("hc.settings.showFunctionBar")->isChecked();
    General["restoreWorkspace"] = findChild<QComboBox*>("hc.settings.startup")->currentIndex() == 0;
    General["singleInstance"] = findChild<QCheckBox*>("hc.settings.singleInstance")->isChecked();
    General["newTabPosition"] = QStringList{"last", "right", "left", "first"}.value(findChild<QComboBox*>("hc.settings.newTabPosition")->currentIndex(), "last");
    General["monoFontFamily"] = findChild<QFontComboBox*>("hc.settings.monoFont")->currentFont().family(); General["monoFontPx"] = findChild<QSpinBox*>("hc.settings.monoSize")->value();
    General["thumbnails"] = thumbnailSettings_->Save();
    hc::settings::AppSettings().Put(hc::settings::Section::General, General);
    SaveSettings_ColorScheme();
    auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject(); Appearance["watercolorTheme"] = themeSettings_->Save(); Appearance["watercolorPresets"] = themeSettings_->SaveLibrary(); hc::settings::AppSettings().Put(hc::settings::Section::Appearance, Appearance);
    TyStColorSchemeMgr::GetInstance()->Refresh();
    RefreshColorScheme(selectedScheme_);
    TyStFileSetMgr::GetInstance()->SaveSettings(); SaveSettings_PluginList();
    TyStCommandMgr::GetInstance()->SaveSettings();
    if (!hc::settings::AppSettings().Commit()) {
        QMessageBox Message(QMessageBox::Warning, tr("설정 저장 실패"),
            tr("편집 내용은 창에 유지됩니다. 다시 읽으면 디스크의 설정으로 돌아갑니다.\n%1").arg(hc::settings::AppSettings().Error()), QMessageBox::Close, this);
        auto* Reload = Message.addButton(tr("디스크에서 다시 읽기"), QMessageBox::ResetRole);
        Message.exec();
        if (Message.clickedButton() == Reload) { hc::settings::AppSettings().Reload(); committedSettings_ = hc::settings::AppSettings().Object(); hc::settings::AppSettings().BeginEdit(); SetListView(listView_); LoadSettings(); }
        return false;
    }
    if (ColumnsChanged) {
        for (int Index = 0; Index < Targets.size(); ++Index) if (!Targets[Index] || !Targets[Index]->ApplyColumnProfile(columnEditor_->Profile())) {
            for (int Restore = 0; Restore < Index; ++Restore) if (Targets[Restore]) Targets[Restore]->ApplyColumnProfile(PreviousProfiles[Restore]);
            const bool Restored = hc::settings::AppSettings().Replace(PreviousSettings) && hc::settings::AppSettings().Commit();
            QMessageBox::warning(this, tr("컬럼 적용 실패"), Restored ? tr("기존 설정을 복구했습니다. 편집 내용을 확인한 후 다시 적용하세요.") : tr("패널 적용 및 설정 복구에 실패했습니다. 디스크 설정을 다시 읽어 주세요.")); return false;
        }
        for (const auto& View : Targets) if (View) { View->SetAutomaticColumns(columnEditor_->AutomaticTab()); emit View->sigViewSettingsChanged(); }

    }
    if (menuSettings_ && !menuSettings_->CommitPrepared()) {
        for (int I = 0; I < Targets.size(); ++I) if (Targets[I]) Targets[I]->ApplyColumnProfile(PreviousProfiles[I]);
        const bool Restored = hc::settings::AppSettings().Replace(PreviousSettings) && hc::settings::AppSettings().Commit();
        QMessageBox::warning(this, tr("메뉴 저장 실패"), Restored ? tr("기존 설정과 메뉴를 유지했습니다. 메뉴 페이지에서 오류를 확인하세요.") : tr("메뉴 저장 및 설정 복구에 실패했습니다. 디스크 설정을 확인하세요.")); return false;
    }
    if (auto* Bridge = TyStCommandMgr::GetInstance()->Bridge()) { Bridge->ReloadMenu(); Bridge->RefreshShortcutLabels(); Bridge->RefreshFunctionBar(); }
    if (ColumnsChanged) columnEditor_->MarkSaved();
    if (resetListModeDraft_ && listView_) { listView_->SetListMode("inherit"); resetListModeDraft_ = false; }
    if (thumbnailSettings_->ClearRequested()) {
        for (auto* View : parentWidget()->window()->findChildren<CGridView*>()) View->ClearThumbnailCache();
        thumbnailSettings_->MarkApplied();
    }
    hc::columns::PluginFields::Instance().RefreshIfChanged();
    const auto Scheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme(selectedScheme_);
    emit TyStColorSchemeMgr::GetInstance()->sigColorSchemeChanged(Scheme);
    emit NotifyColorSchemeChanged(Scheme);
    if (auto* Bridge = TyStCommandMgr::GetInstance()->Bridge()) Bridge->FileJobs()->ApplyDefaults();
    if (auto* Owner = parentWidget()) for (auto* Model : Owner->findChildren<CFSModel*>()) {
        Model->ApplyDirectorySizeSettings(); Model->ApplyRefreshSettings();
        Model->SetEverythingDisplayInterval(hc::quickfind::Settings::FromJson(
            hc::settings::AppSettings().CommittedObject()["general"].toObject()["quickFind"].toObject()).everythingDisplayIntervalMs_);
    }
    committedSettings_ = hc::settings::AppSettings().Object();
    if (auto* Status = findChild<QLabel*>("hc.settings.saveStatus")) Status->setText(tr("적용 완료 · 이후 변경은 다시 적용하거나 취소하세요."));
    return true;
}

void QMainOpts::SaveSettings_ColorScheme()
{
    auto* Manager = TyStColorSchemeMgr::GetInstance();
    auto Scheme = Manager->GetColorScheme(selectedScheme_);
    if (Scheme.Name.isEmpty()) return;
    // Start with the complete saved scheme so removing the legacy editor never
    // discards its colors, override intent, fonts or file-set mappings.
    const int Mode = findChild<QComboBox*>("hc.settings.theme")->currentIndex();
    Scheme.IsDarkMode = Mode == 1 || (Mode == 2 && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    Scheme.IsInverseCursor = inverseCursor_->isChecked();
    Scheme.IsInverseSelect = inverseSelection_->isChecked();
    Scheme.Menu_Font = findChild<QFontComboBox*>("hc.settings.menuFont")->currentFont();
    Scheme.Menu_Font.setPointSize(findChild<QSpinBox*>("hc.settings.menuFontSize")->value());
    Scheme.Dialog_Font = findChild<QFontComboBox*>("hc.settings.dialogFont")->currentFont();
    Scheme.Dialog_Font.setPointSize(findChild<QSpinBox*>("hc.settings.dialogFontSize")->value());
    Scheme.FileList_Font = findChild<QFontComboBox*>("hc.settings.listFont")->currentFont();
    Scheme.FileList_Font.setPointSize(findChild<QSpinBox*>("hc.settings.listFontSize")->value());
    Manager->UpsertColorScheme(Scheme, true);
}

void QMainOpts::SaveSettings_PluginList()
{
    pluginManagement_->Save(hc::settings::AppSettings());
    TyStPlugInMgr::GetInstance()->Refresh();
}

void QMainOpts::RefreshColorScheme(const QString& SchemeName)
{
    const auto Scheme = TyStColorSchemeMgr::GetInstance()->GetColorScheme(SchemeName);
    if (Scheme.Name.isEmpty()) return;
    selectedScheme_ = Scheme.Name;
    inverseCursor_->setChecked(Scheme.IsInverseCursor);
    inverseSelection_->setChecked(Scheme.IsInverseSelect);
    const QFont Fonts[]{Scheme.Menu_Font, Scheme.Dialog_Font, Scheme.FileList_Font};
    const QStringList Keys{"menu", "dialog", "list"};
    for (int I = 0; I < 3; ++I) {
        findChild<QFontComboBox*>("hc.settings." + Keys[I] + "Font")->setCurrentFont(Fonts[I]);
        findChild<QSpinBox*>("hc.settings." + Keys[I] + "FontSize")->setValue(Fonts[I].pointSize());
    }
}

void QMainOpts::RefreshPluginList()
{
    pluginManagement_->Load(hc::settings::AppSettings());
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
    auto Keys = Map.keys();
    std::sort(Keys.begin(), Keys.end(), [](auto Left, auto Right) { return QKeySequence(Left).toString() < QKeySequence(Right).toString(); });
    for( const auto Key : Keys )
    {
        const auto CMD = Map[ Key ];
        const auto CMD_Default = StCommandMgr->GetDefaultCMD( CMD );

        const auto Row = ui.tblShortCut->rowCount();

        ui.tblShortCut->setRowCount( Row + 1 );
        ui.tblShortCut->setItem( Row, 0, new QTableWidgetItem( CMD ) );
        const auto Defaults = hc::ui::DefaultShortcuts(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject());
        const bool IsDefault = Defaults.value(Key) == CMD;
        ui.tblShortCut->setItem(Row, 1, new QTableWidgetItem(IsDefault ? QKeySequence(Key).toString() : QString{}));
        ui.tblShortCut->setItem(Row, 2, new QTableWidgetItem(IsDefault ? QString{} : QKeySequence(Key).toString()));
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
    ui.stkOpts->setCurrentIndex(Current->data(Qt::UserRole).isValid() ? Current->data(Qt::UserRole).toInt() : ui.lstOptCate->row(Current));
}

///////////////////////////////////////////////////////////////////////////////
/// Content

/// Page = pdDisplay_ColorScheme
void QMainOpts::on_btnAddShortcutCMD_clicked( bool checked )
{
    const auto StCommandMgr = TyStCommandMgr::GetInstance();
    const auto Input = ui.keySequenceEdit->keySequence();
    if( Input.isEmpty() == true )
    {
        QMessageBox::information(this, tr("단축키"), tr("먼저 단축키를 입력하세요."));
        return;
    }
    if (Input.count() != 1) { QMessageBox::information(this, tr("단축키"), tr("한 번의 키 조합만 사용할 수 있습니다.")); return; }

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
    const auto Existing = StCommandMgr->Retrieve().value(Input[0]);
    if (!Existing.isEmpty() && Existing != Selected.Name && QMessageBox::question(this, tr("단축키 충돌"),
        tr("%1은(는) 현재 '%2'에 연결되어 있습니다. '%3'으로 변경하시겠습니까?").arg(Input.toString(), StCommandMgr->GetDefaultCMD(Existing).Desc, Selected.Desc)) != QMessageBox::Yes) return;

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

    // A hidden column editor still contributes its full minimum height to the
    // stacked layout. The viewport keeps that height local to this page.
    auto* ColumnsScroll = new QScrollArea(ui.pgCustomColumns); ColumnsScroll->setObjectName("hc.settings.columnsScroll");
    ColumnsScroll->setWidgetResizable(true); ColumnsScroll->setFrameShape(QFrame::NoFrame);
    columnEditor_ = new QtColumnProfileEditor; ColumnsScroll->setWidget(columnEditor_);
    ui.pgCustomColumns->layout()->addWidget(ColumnsScroll);

    auto* NavigationPage = new QWidget(ui.stkOpts); NavigationPage->setObjectName("hc.settings.navigation");
    ui.stkOpts->addWidget(NavigationPage); ui.lstOptCate->addItem(tr("탐색"));
    auto* NavigationLayout = new QVBoxLayout(NavigationPage);
    auto* NavigationHeader = new fm::ui::DialogHeader(NavigationPage);
    NavigationHeader->setTitle(tr("탐색")); NavigationHeader->setSubtitle(tr("폴더 이동, 빠른 찾기와 자동 새로고침 동작을 설정합니다."));
    NavigationLayout->addWidget(NavigationHeader);
    auto* NavigationTabs = new QTabWidget(NavigationPage); NavigationTabs->setObjectName("hc.navigation.tabs");
    NavigationTabs->setAccessibleName(tr("탐색 설정 범주")); NavigationLayout->addWidget(NavigationTabs, 1);
    auto* NavigationGeneral = new QWidget(NavigationTabs); auto* NavigationGeneralLayout = new QVBoxLayout(NavigationGeneral);
    NavigationTabs->addTab(NavigationGeneral, tr("폴더 이동"));
    focusPreviousDirectory_ = new QCheckBox(tr("상위 폴더(..)로 이동할 때 방금 나온 폴더에 커서 놓기"), NavigationGeneral);
    focusPreviousDirectory_->setObjectName("hc.navigation.focusPreviousDirectory");
    NavigationGeneralLayout->addWidget(focusPreviousDirectory_);
    auto* NavigationNote = new QLabel(tr("좌우 패널에 모두 적용합니다. 끄면 상위 폴더 목록의 첫 항목에 커서를 놓습니다."), NavigationPage);
    NavigationNote->setWordWrap(true); NavigationGeneralLayout->addWidget(NavigationNote); NavigationGeneralLayout->addStretch();
    auto* QuickPage = new QWidget(NavigationTabs); NavigationTabs->addTab(QuickPage, tr("빠른 찾기"));
    auto* QuickLayout = new QVBoxLayout(QuickPage);
    auto* QuickEnabled = new QCheckBox(tr("빠른 찾기 기능 사용(&Q)"), QuickPage);
    QuickEnabled->setObjectName("hc.navigation.quickFind.enabled"); QuickLayout->addWidget(QuickEnabled);
    auto* QuickOptions = new QGroupBox(tr("빠른 찾기 동작"), QuickPage); QuickLayout->addWidget(QuickOptions);
    auto* QuickForm = new QFormLayout(QuickOptions);
    const auto QuickChoice = [QuickOptions, QuickForm](const char* Key, const QString& Caption, const QList<QPair<QString, QString>>& Items) {
        auto* Choice = new QComboBox(QuickOptions); Choice->setObjectName("hc.navigation.quickFind." + QString::fromLatin1(Key));
        for (const auto& Item : Items) Choice->addItem(Item.first, Item.second);
        QuickForm->addRow(Caption, Choice);
    };
    QuickChoice("activation", tr("활성화 방법(&A):"), {{tr("Ctrl+Alt+글자"), "ctrl-alt"}, {tr("글자 입력"), "typing"}});
    QuickChoice("scope", tr("검색 범위(&S):"), {{tr("현재 패널의 파일 목록"), "panel"}, {tr("Everything 검색"), "everything"}});
    QuickChoice("match", tr("검색 방법(&M):"), {{tr("검색어로 시작"), "starts"}, {tr("검색어 포함"), "contains"}, {tr("검색어로 끝남"), "ends"}});
    QuickChoice("result", tr("결과 표시(&R):"), {{tr("필터링"), "filter"}, {tr("커서 이동 (기본값)"), "cursor"}});
    auto* QuickHighlight = new QCheckBox(tr("일치하는 부분 문자열 강조 표시(&H)"), QuickOptions);
    QuickHighlight->setObjectName("hc.navigation.quickFind.highlight"); QuickForm->addRow(QuickHighlight);
    auto* EverythingOptions = new QGroupBox(tr("Everything 결과 표시"), QuickPage); QuickLayout->addWidget(EverythingOptions);
    auto* EverythingForm = new QFormLayout(EverythingOptions);
    auto* EverythingInterval = new QSpinBox(EverythingOptions);
    EverythingInterval->setObjectName("hc.navigation.quickFind.everythingDisplayIntervalMs");
    EverythingInterval->setRange(hc::quickfind::Settings::MinimumDisplayInterval, hc::quickfind::Settings::MaximumDisplayInterval);
    EverythingInterval->setSingleStep(50); EverythingInterval->setSuffix(tr(" ms")); EverythingInterval->setValue(500);
    EverythingInterval->setToolTip(tr("응답을 받은 뒤 파일 패널에 결과를 추가하는 간격입니다. 첫 결과와 마지막 결과는 준비되는 대로 표시합니다. 검색 요청 간격이나 Everything 응답 대기 시간에는 영향을 주지 않습니다."));
    EverythingForm->addRow(tr("Everything 응답 표시 주기(&E):"), EverythingInterval);
    auto* QuickNote = new QLabel(tr("파일 목록에서 입력을 시작하면 상단 검색창으로 이동하여 첫 글자부터 이어서 입력합니다.\n커서 이동은 전체 목록을 유지하며 첫 일치 항목으로 이동합니다. 위·아래 화살표로 이전·다음 일치 항목을 찾습니다.\n검색창에서 Ctrl+S를 누르면 검색어를 유지한 채 필터링과 커서 이동을 전환합니다.\n결과 표시 방식은 현재 패널 목록 검색에 적용합니다. Everything 검색은 파일 패널에 결과를 누적 표시합니다. Ctrl+F는 설정 범위, Ctrl+Alt+F는 Everything에서 검색합니다.\n한글 등 IME 조합 입력을 지원하며, 이름 변경 중에는 활성화하지 않습니다.\nEverything 검색에는 실행 중인 Everything이 필요합니다. Esc를 누르면 빠른 찾기를 닫고 목록으로 돌아갑니다."), QuickPage);
    QuickNote->setWordWrap(true); QuickLayout->addWidget(QuickNote); QuickLayout->addStretch();
    connect(QuickEnabled, &QCheckBox::toggled, QuickOptions, &QWidget::setEnabled);
    auto* RefreshScroll = new QScrollArea(NavigationTabs); RefreshScroll->setWidgetResizable(true); RefreshScroll->setFrameShape(QFrame::NoFrame);
    NavigationTabs->addTab(RefreshScroll, tr("새로고침"));
    auto* RefreshBody = new QWidget; RefreshScroll->setWidget(RefreshBody); auto* RefreshLayout = new QVBoxLayout(RefreshBody);
    auto* Notifications = new QGroupBox(tr("파일시스템 변경 통지로 자동 새로고침"), RefreshBody);
    auto* NotificationLayout = new QVBoxLayout(Notifications); RefreshLayout->addWidget(Notifications);
    const auto RefreshCheck = [](QVBoxLayout* Layout, const char* Key, const QString& Caption) {
        auto* Check = new QCheckBox(Caption, Layout->parentWidget()); Check->setObjectName("hc.navigation.refresh." + QString::fromLatin1(Key));
        Layout->addWidget(Check); return Check;
    };
    RefreshCheck(NotificationLayout, "names", tr("파일 생성·삭제·이름 변경 시 새로고침(&C)"));
    RefreshCheck(NotificationLayout, "metadata", tr("파일 크기·날짜·속성 변경 시 새로고침(&S)"));
    RefreshCheck(NotificationLayout, "appendNewEntries", tr("추가된 파일을 목록 맨 뒤에 표시(&P)"));
    auto* OrderNote = new QLabel(tr("기본값은 현재 정렬 순서 적용입니다. 맨 뒤에 표시한 항목은 수동 새로고침이나 정렬 변경 시 다시 정렬합니다."), Notifications);
    OrderNote->setWordWrap(true); NotificationLayout->addWidget(OrderNote);
    RefreshCheck(NotificationLayout, "pauseInBackground", tr("백그라운드일 때 새로고침 보류(&R)"));
    RefreshCheck(NotificationLayout, "updateFooter", tr("하단 정보 새로고침 · 파일 개수와 크기(&F)"));
    RefreshCheck(NotificationLayout, "updateHeader", tr("상단 정보 새로고침 · 디스크 여유 공간(&H)"));
    auto* Exclusions = new QFormLayout; NotificationLayout->addLayout(Exclusions);
    auto* Excluded = new QLineEdit(Notifications); Excluded->setObjectName("hc.navigation.refresh.excludedDrives");
    Excluded->setPlaceholderText(tr("예: C:;D:;\\\\server\\share")); Excluded->setAccessibleName(tr("자동 새로고침 제외 드라이브"));
    Exclusions->addRow(tr("자동 새로고침 제외(&I):"), Excluded);
    auto* ExcludedNote = new QLabel(tr("세미콜론으로 구분합니다. 제외 위치에는 통지와 주기 새로고침을 모두 적용하지 않습니다. 수동 새로고침은 사용할 수 있습니다."), Notifications);
    ExcludedNote->setWordWrap(true); NotificationLayout->addWidget(ExcludedNote);
    auto* Fallback = new QGroupBox(tr("파일시스템 변경 통지를 사용할 수 없는 위치"), RefreshBody);
    auto* FallbackLayout = new QVBoxLayout(Fallback); RefreshLayout->addWidget(Fallback);
    auto* Poll = RefreshCheck(FallbackLayout, "fallbackEnabled", tr("변경 통지 미지원 시 일정 주기로 자동 새로고침(&A)"));
    auto* IntervalForm = new QFormLayout; FallbackLayout->addLayout(IntervalForm);
    auto* Interval = new QSpinBox(Fallback); Interval->setObjectName("hc.navigation.refresh.fallbackSeconds");
    Interval->setRange(1, 86400); Interval->setSuffix(tr(" 초")); IntervalForm->addRow(tr("새로고침 주기(&T):"), Interval);
    connect(Poll, &QCheckBox::toggled, Interval, &QWidget::setEnabled); Interval->setEnabled(false);
    auto* FallbackNote = new QLabel(tr("기본값: 꺼짐 · 주기 15초. 네트워크·플러그인 등 변경 통지를 사용할 수 없는 활성 탭에 적용합니다. 통지를 사용할 수 있는 위치는 주기적으로 다시 읽지 않습니다."), Fallback);
    FallbackNote->setWordWrap(true); FallbackLayout->addWidget(FallbackNote); RefreshLayout->addStretch();
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


}

void QMainOpts::initializeDesignPages()
{
    const auto Page = [this](const QString& Title, const QString& Description) {
        auto* Widget = new QWidget(ui.stkOpts); auto* Layout = new QVBoxLayout(Widget);
        auto* Header = new fm::ui::DialogHeader(Widget); Header->setTitle(Title); Header->setSubtitle(Description); Layout->addWidget(Header);
        ui.stkOpts->addWidget(Widget); return Widget;
    };
    const auto DisabledCombo = [](QFormLayout* Form, const QString& Title, const QStringList& Choices, const QString& Reason) {
        auto* Combo = new QComboBox; Combo->addItems(Choices); Combo->setEnabled(false); Combo->setToolTip(Reason); Combo->setAccessibleDescription(Reason); Form->addRow(Title, Combo);
    };
    themeSettings_ = new hc::ui::ThemeSettingsPage(ui.stkOpts); ui.stkOpts->addWidget(themeSettings_);
    auto* General = Page(tr("일반 · 모양"), tr("테마·강조색·글꼴과 시작 동작을 설정합니다. 적용 전 변경은 초안에 유지됩니다."));
    auto* GeneralLayout = qobject_cast<QVBoxLayout*>(General->layout());
    auto* GeneralScroll = new QScrollArea(General); GeneralScroll->setWidgetResizable(true); GeneralScroll->setFrameShape(QFrame::NoFrame); GeneralLayout->addWidget(GeneralScroll, 1);
    auto* GeneralBody = new QWidget; GeneralScroll->setWidget(GeneralBody); auto* GeneralGrid = new QGridLayout(GeneralBody); GeneralGrid->setColumnStretch(0, 1); GeneralGrid->setColumnStretch(1, 1);
    auto* ThemeCard = new fm::ui::Card(GeneralBody); auto* ThemeForm = new QVBoxLayout(ThemeCard); GeneralGrid->addWidget(ThemeCard, 0, 0);
    ThemeForm->addWidget(new QLabel(tr("테마"), ThemeCard)); auto* Modes = new QHBoxLayout; ThemeForm->addLayout(Modes);
    auto* Theme = new QComboBox(General); Theme->setObjectName("hc.settings.theme"); Theme->addItems({tr("라이트"), tr("다크"), tr("시스템")}); Theme->hide();
    auto* Group = new QButtonGroup(this); Group->setExclusive(true);
    for (int I : {2, 0, 1}) {
        auto* Mode = new fm::ui::ThemeModeCard(ThemeCard); Mode->setObjectName("hc.settings.themeCard" + QString::number(I));
        Mode->setMode(I == 2 ? fm::ui::ThemeModeCard::System : I == 1 ? fm::ui::ThemeModeCard::Dark : fm::ui::ThemeModeCard::Light);
        Mode->setText(I == 2 ? tr("시스템(&Y)") : I == 1 ? tr("다크(&K)") : tr("라이트(&L)"));
        Mode->setCheckable(true); Group->addButton(Mode, I); Modes->addWidget(Mode);
    }
    connect(Group, &QButtonGroup::idClicked, Theme, &QComboBox::setCurrentIndex);
    connect(Theme, &QComboBox::currentIndexChanged, this, [this, Group, Theme](int Value) {
        if (auto* B = Group->button(Value)) B->setChecked(true);
        const bool Dark = Value == 1 || (Value == 2 && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        RefreshColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"));
    });
    auto* Accent = new fm::ui::AccentPicker(ThemeCard); Accent->setObjectName("hc.settings.accent"); ThemeForm->addWidget(new QLabel(tr("강조색 · 버튼·진행·선택"), ThemeCard)); ThemeForm->addWidget(Accent);
    connect(Accent, &fm::ui::AccentPicker::accentChosen, this, [this](const auto& Color) { themeSettings_->SetAccent(Color); });
    connect(Accent, &fm::ui::AccentPicker::customRequested, this, [this, Accent] { const auto Color = QColorDialog::getColor(Accent->current().value_or(QColor("#1F5FD1")), this); if (Color.isValid()) { Accent->setCurrent(Color); themeSettings_->SetAccent(Color); } });
    auto* SystemAccent = new QCheckBox(tr("Windows 강조색 사용"), ThemeCard); SystemAccent->setObjectName("hc.settings.systemAccent"); ThemeForm->addWidget(SystemAccent);
    connect(SystemAccent, &QCheckBox::toggled, this, [this](bool On) { themeSettings_->SetSystemAccent(On); });
    themeSettings_->onChanged_ = [this, Accent, SystemAccent, Group] {
        Accent->setCurrent(themeSettings_->Scheme().seeds.accent); const QSignalBlocker Block(SystemAccent); SystemAccent->setChecked(themeSettings_->Scheme().seeds.useSystemAccent);
        for (auto* B : Group->buttons()) static_cast<fm::ui::ThemeModeCard*>(B)->setAccent(themeSettings_->Scheme().seeds.accent.value_or(QColor("#1F5FD1")));
    };
    auto* DarkTitle = new QCheckBox(tr("다크 테마에서 제목 표시줄도 어둡게"), ThemeCard); DarkTitle->setObjectName("hc.settings.darkTitle"); ThemeForm->addWidget(DarkTitle);
    auto* ColorEdit = new QPushButton(tr("색 편집(&T)..."), ThemeCard); ThemeForm->addWidget(ColorEdit); connect(ColorEdit, &QPushButton::clicked, this, [this] {
        for (int R = 0; R < ui.lstOptCate->count(); ++R) if (ui.lstOptCate->item(R)->data(Qt::UserRole).toInt() == ui.stkOpts->indexOf(themeSettings_)) ui.lstOptCate->setCurrentRow(R);
    });
    auto* Fonts = new fm::ui::Card(GeneralBody); auto* FontForm = new QFormLayout(Fonts); FontForm->setRowWrapPolicy(QFormLayout::WrapLongRows); GeneralGrid->addWidget(Fonts, 1, 0);
    const QStringList FontKeys{"list", "menu", "dialog"};
    const QStringList FontLabels{tr("목록 글꼴(&F)"), tr("메뉴 글꼴"), tr("대화상자 글꼴")};
    for (int I = 0; I < 3; ++I) {
        auto* Row = new QWidget(Fonts); auto* Fields = new QHBoxLayout(Row); Fields->setContentsMargins(0, 0, 0, 0);
        auto* Font = new QFontComboBox(Row); Font->setObjectName("hc.settings." + FontKeys[I] + "Font"); Fields->addWidget(Font, 1);
        auto* Size = new QSpinBox(Row); Size->setObjectName("hc.settings." + FontKeys[I] + "FontSize"); Size->setRange(6, 72); Size->setSuffix(tr(" pt")); Fields->addWidget(Size);
        FontForm->addRow(FontLabels[I], Row);
    }
    auto* Mono = new QFontComboBox(Fonts); Mono->setObjectName("hc.settings.monoFont"); Mono->setFontFilters(QFontComboBox::MonospacedFonts); FontForm->addRow(tr("고정폭 글꼴(&C)"), Mono);
    auto* MonoSize = new QSpinBox(Fonts); MonoSize->setObjectName("hc.settings.monoSize"); MonoSize->setRange(9, 36); FontForm->addRow(tr("고정폭 글자 크기 · px"), MonoSize);
    DisabledCombo(FontForm, tr("화면 배율"), {tr("Windows 시스템 설정 따름")}, tr("화면 배율은 Windows 디스플레이 설정에서 변경합니다."));
    auto* Startup = new fm::ui::Card(GeneralBody); auto* StartupForm = new QFormLayout(Startup); StartupForm->setRowWrapPolicy(QFormLayout::WrapLongRows); GeneralGrid->addWidget(Startup, 0, 1);
    auto* StartMode = new QComboBox(Startup); StartMode->setObjectName("hc.settings.startup"); StartMode->addItems({tr("마지막 탭과 폴더 복원"), tr("기본 작업공간으로 시작")}); StartupForm->addRow(tr("시작할 때"), StartMode);
    auto* Line = new QCheckBox(tr("명령줄 표시"), Startup); Line->setObjectName("hc.settings.showCommandLine"); StartupForm->addRow(Line);
    auto* FunctionBar = new QCheckBox(tr("기능 키 막대 표시"), Startup); FunctionBar->setObjectName("hc.settings.showFunctionBar"); StartupForm->addRow(FunctionBar);
    auto* Single = new QCheckBox(tr("창 하나만 실행 · 기존 창에 새 탭"), Startup); Single->setObjectName("hc.settings.singleInstance"); Single->setToolTip(tr("다음 실행부터 적용합니다. 같은 사용자·설정 폴더의 기존 창에 경로를 새 탭으로 전달합니다.")); StartupForm->addRow(Single);
    auto* Tabs = new fm::ui::Card(GeneralBody); auto* TabForm = new QFormLayout(Tabs); GeneralGrid->addWidget(Tabs, 1, 1);
    auto* TabPosition = new QComboBox(Tabs); TabPosition->setObjectName("hc.settings.newTabPosition"); TabPosition->addItems({tr("마지막"), tr("현재 탭 오른쪽"), tr("현재 탭 왼쪽"), tr("처음")}); TabForm->addRow(tr("새 탭 위치"), TabPosition);
    auto* TabNote = new QLabel(tr("보기·열 설정은 탭별로 기억합니다. 전체 기본값과 현재 탭의 예외는 ‘파일 패널’에서 구분합니다."), Tabs); TabNote->setWordWrap(true); TabForm->addRow(TabNote);
    GeneralGrid->setRowStretch(2, 1);
    auto* DisplayBody = ui.pgDisplay->findChild<QScrollArea*>("hc.settings.filePanelGeneral")->widget();
    auto* DisplayRows = qobject_cast<QVBoxLayout*>(DisplayBody->layout());
    listCard_ = new fm::ui::Card(DisplayBody); listCard_->setObjectName("hc.settings.listPresentation");
    auto* ListColumns = new QHBoxLayout(listCard_); auto* ListControls = new QWidget(listCard_); ListColumns->addWidget(ListControls, 1);
    auto* ListForm = new QFormLayout(ListControls); ListForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* PreviewCard = new fm::ui::Card(listCard_); ListColumns->addWidget(PreviewCard, 1); auto* PreviewRows = new QVBoxLayout(PreviewCard);
    PreviewRows->addWidget(new QLabel(tr("미리보기 · 적용 전 예제 · 1줄/2줄"), PreviewCard));
    auto* One = new fm::filelist::RecordPreview(PreviewCard); auto* Two = new fm::filelist::RecordPreview(PreviewCard); One->setObjectName("hc.settings.oneLinePreview"); Two->setObjectName("hc.settings.twoLinePreview");
    const QList<fm::filelist::RecordPreview::Row> PreviewFiles{{"Projects", tr("폴더")}, {tr("긴 파일 이름과 확장자를 보존하는 자료.tar.gz"), "7.2 MB", true}, {"MainWindow.cpp", "24.1 KB", false, true}};
    One->SetRows(PreviewFiles); Two->SetRows(PreviewFiles); PreviewRows->addWidget(One); PreviewRows->addWidget(Two); PreviewRows->addStretch();
    auto* Header = new fm::ui::DialogHeader(ui.pgDisplay); Header->setTitle(tr("파일 패널")); Header->setSubtitle(tr("공통 탭에서 목록 표시를, 아이콘 탭에서 폴더·파일 아이콘을 설정합니다.")); qobject_cast<QVBoxLayout*>(ui.pgDisplay->layout())->insertWidget(0, Header);
    auto* StateCard = new fm::ui::Card(PreviewCard); auto* StateRows = new QVBoxLayout(StateCard); PreviewRows->insertWidget(1, StateCard);
    StateRows->addWidget(new QLabel(tr("커서 · 선택"), StateCard));
    inverseCursor_ = new QCheckBox(StateCard); inverseCursor_->setObjectName("hc.settings.inverseCursor");
    inverseSelection_ = new QCheckBox(StateCard); inverseSelection_->setObjectName("hc.settings.inverseSelection");
    inverseCursor_->setText(tr("커서 색상 반전")); inverseSelection_->setText(tr("선택 색상 반전")); StateRows->addWidget(inverseCursor_); StateRows->addWidget(inverseSelection_);
    auto* Title = new QLabel(tr("목록 보기"), listCard_); ListForm->addRow(Title);
    const auto Choice = [this, ListForm](const QString& Name, const QString& Label, const QStringList& Items) {
        auto* Combo = new QComboBox(listCard_); Combo->setObjectName(Name); Combo->addItems(Items); ListForm->addRow(Label, Combo); return Combo;
    };
    listDefault_ = Choice("hc.settings.listDefault", tr("기본 보기"), {tr("1줄"), tr("2줄"), tr("자동"), tr("썸네일")});
    oneLineSeparator_ = Choice("hc.settings.oneLineSeparator", tr("1줄 레코드 구분"), {tr("없음"), tr("교차 배경"), tr("구분선"), tr("여백"), tr("메타데이터 틴트")});
    twoLineSeparator_ = Choice("hc.settings.twoLineSeparator", tr("2줄 레코드 구분"), {tr("없음"), tr("교차 배경"), tr("구분선"), tr("여백"), tr("메타데이터 틴트")});
    listDensity_ = Choice("hc.settings.listDensity", tr("행 밀도"), {tr("조밀"), tr("보통"), tr("여유")});
    listElide_ = Choice("hc.settings.listElide", tr("긴 이름 표시"), {tr("확장자를 보존하는 가운데 줄임"), tr("끝 줄임"), tr("가운데 줄임")});
    listNameBelow_ = new QCheckBox(tr("2줄 보기에서 이름을 아래 줄에 표시"), listCard_); listNameBelow_->setObjectName("hc.settings.listNameBelow"); ListForm->addRow(listNameBelow_);
    listAutoWidth_ = new QSpinBox(listCard_); listAutoWidth_->setObjectName("hc.settings.listAutoWidth"); listAutoWidth_->setRange(100, 4000); listAutoWidth_->setSuffix(tr(" DIP"));
    listAutoPercent_ = new QSpinBox(listCard_); listAutoPercent_->setObjectName("hc.settings.listAutoPercent"); listAutoPercent_->setRange(5, 100); listAutoPercent_->setSuffix("%");
    ListForm->addRow(tr("자동: 이 폭보다 좁으면 2줄"), listAutoWidth_); ListForm->addRow(tr("자동: 잘린 이름 비율 이상이면 2줄"), listAutoPercent_);
    auto* AutoHelp = new QLabel(tr("표시 중인 목록에서 최대 2,048개 이름을 고르게 측정합니다. 2줄에서 1줄로 복귀할 때는 폭 32 DIP·비율 5%의 안정 구간을 적용합니다."), listCard_); AutoHelp->setWordWrap(true); ListForm->addRow(AutoHelp);
    listScope_ = new QLabel(tr("기본값은 이를 따르는 탭과 새 탭에 적용합니다."), listCard_); listScope_->setWordWrap(true); ListForm->addRow(listScope_);
    useListDefault_ = new QPushButton(tr("현재 탭도 설정 기본값 따르기(&D)"), listCard_); useListDefault_->setObjectName("hc.settings.useListDefault"); useListDefault_->setEnabled(false); ListForm->addRow(useListDefault_);
    connect(useListDefault_, &QPushButton::clicked, this, [this] { resetListModeDraft_ = true; listScope_->setText(tr("적용/확인을 누르면 현재 탭이 설정 기본값을 따릅니다.")); ui.btnApply->setEnabled(true); });
    const auto UpdatePreview = [this, One, Two] {
        fm::filelist::ListAppearance A; A.invertCursor = inverseCursor_->isChecked(); A.invertSelection = inverseSelection_->isChecked(); A.nameBelow = listNameBelow_->isChecked(); A.density = fm::filelist::RowDensity(listDensity_->currentIndex()); A.nameElide = fm::filelist::NameElide(listElide_->currentIndex());
        A.oneLineSeparator = fm::filelist::RecordSeparator(oneLineSeparator_->currentIndex()); A.twoLineSeparator = fm::filelist::RecordSeparator(twoLineSeparator_->currentIndex());
        One->SetAppearance(A, false); Two->SetAppearance(A, true);
    };
    for (auto* C : {oneLineSeparator_, twoLineSeparator_, listDensity_, listElide_}) connect(C, &QComboBox::currentIndexChanged, this, UpdatePreview);
    connect(inverseCursor_, &QCheckBox::toggled, this, UpdatePreview); connect(inverseSelection_, &QCheckBox::toggled, this, UpdatePreview);
    connect(listNameBelow_, &QCheckBox::toggled, this, UpdatePreview); UpdatePreview();
    DisplayRows->insertWidget(0, listCard_);
    thumbnailSettings_ = new hc::ui::ThumbnailSettingsPage(ui.stkOpts);
    auto* Thumbnails = thumbnailSettings_; ui.stkOpts->addWidget(Thumbnails);
    // Keep height-for-width changes in this page from increasing the dialog's minimum height.
    auto* FileColorsScroll = new QScrollArea(ui.stkOpts); FileColorsScroll->setObjectName("hc.settings.fileColorsScroll");
    FileColorsScroll->setWidgetResizable(true); FileColorsScroll->setFrameShape(QFrame::NoFrame);
    fileColors_ = new hc::ui::FileColorRulesPage;
    FileColorsScroll->setWidget(fileColors_); ui.stkOpts->addWidget(FileColorsScroll);
    auto* Jobs = Page(tr("파일 작업"), tr("공통 동작과 작업별 기본값을 설정합니다. 변경한 값은 새 작업부터 적용합니다."));
    Jobs->setObjectName("hc.settings.fileOperations");
    auto* JobsTabs = new QTabWidget(Jobs); JobsTabs->setObjectName("hc.settings.fileOperationsTabs"); JobsTabs->setAccessibleName(tr("파일 작업 범주"));
    qobject_cast<QVBoxLayout*>(Jobs->layout())->addWidget(JobsTabs, 1);
    // Each tab owns a viewport so long copy options cannot raise the dialog's
    // minimum height. Create controls in visual order for keyboard navigation.
    const auto JobTab = [JobsTabs](const QString& Title, const QString& Name) {
        auto* Scroll = new QScrollArea(JobsTabs); Scroll->setObjectName(Name);
        Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        auto* Body = new QWidget; auto* Rows = new QVBoxLayout(Body);
        Scroll->setWidget(Body); JobsTabs->addTab(Scroll, Title); return Rows;
    };
    const auto JobGroup = [](QVBoxLayout* Rows, const QString& Title) {
        auto* Card = new fm::ui::Card(Rows->parentWidget()); auto* Form = new QFormLayout(Card);
        Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        Form->addRow(new QLabel(Title, Card)); Rows->addWidget(Card); return Form;
    };
    auto* CommonRows = JobTab(tr("공통"), "hc.settings.fileOperationsCommon");
    auto* FolderForm = JobGroup(CommonRows, tr("하위 폴더 점유 공간 계산"));
    const auto AddFolderCheck = [&](const QString& Text, const char* Key) {
        auto* Check = new QCheckBox(Text, FolderForm->parentWidget()); Check->setObjectName(QString("hc.settings.") + Key);
        FolderForm->addRow(Check); return Check;
    };
    AddFolderCheck(tr("Space로 폴더를 선택할 때(&W)"), "folderSizeOnSpace");
    AddFolderCheck(tr("복사·이동·삭제를 시작하기 전에(&B)"), "folderSizeBeforeOperations")
        ->setToolTip(tr("끄면 전체 크기 사전 계산을 생략합니다. 휴지통 삭제와 압축 파일의 순차 읽기에 필요한 항목 확인은 유지합니다."));
    AddFolderCheck(tr("Everything 사용(&E)"), "folderSizeUseEverything");
    auto* FolderMode = new QComboBox(FolderForm->parentWidget()); FolderMode->setObjectName("hc.settings.folderSizeMode");
    FolderMode->addItems({tr("수동: Alt+Shift+Enter"), tr("자동: 로컬 드라이브만"), tr("자동: 모두(네트워크 제외)"), tr("자동: Everything으로만")});
    FolderForm->addRow(tr("자동 계산 범위(&A)"), FolderMode);
    auto* FolderHelp = new QLabel(tr("폴더 안의 파일 크기를 합산해 목록에 표시합니다. 링크·정션의 대상은 따라가지 않습니다.\nUNC 경로와 매핑된 네트워크 드라이브는 자동·수동·작업 전 용량 계산에서 제외합니다.\nEverything을 사용할 수 없으면 직접 계산합니다. ‘자동: Everything으로만’에서는 직접 탐색하지 않습니다. 수동 계산 단축키는 키보드 설정에서 변경할 수 있습니다."), FolderForm->parentWidget());
    FolderHelp->setWordWrap(true); FolderForm->addRow(FolderHelp);
    auto* ProgressForm = JobGroup(CommonRows, tr("진행창 · 완료 알림"));
    auto* Progress = ProgressForm->parentWidget();
    defaultDetailed_ = new QCheckBox(tr("처음 열 때 그래프 보이기"), Progress); defaultDetailed_->setObjectName("hc.settings.jobDetailed"); ProgressForm->addRow(defaultDetailed_);
    defaultClose_ = new QCheckBox(tr("성공하면 진행창 닫기"), Progress); defaultClose_->setObjectName("hc.settings.jobCloseOnSuccess"); ProgressForm->addRow(defaultClose_);
    auto* Notification = new QComboBox(Progress); Notification->setObjectName("hc.settings.jobNotification"); Notification->addItems({tr("알리지 않음"), tr("상태 표시줄"), tr("상태 표시줄 · 작업 표시줄"), tr("상태 표시줄 · 작업 표시줄 · 소리")}); ProgressForm->addRow(tr("백그라운드 작업 완료 알림"), Notification);
    auto* QueueForm = JobGroup(CommonRows, tr("대기열 · 동시 실행"));
    auto* Queue = QueueForm->parentWidget();
    auto* QueueOrder = new QComboBox(Queue); QueueOrder->setObjectName("hc.settings.queueOrder"); QueueOrder->addItems({tr("새 작업을 마지막에 추가"), tr("새 작업을 처음에 추가")}); QueueForm->addRow(tr("대기열 기본 순서"), QueueOrder);
    auto* Concurrency = new QSpinBox(Queue); Concurrency->setObjectName("hc.settings.jobConcurrency"); Concurrency->setRange(1, 2); QueueForm->addRow(tr("동시 실행 수 기본값"), Concurrency);
    auto* JobsNote = new QLabel(tr("순서와 알림은 새로 접수하는 작업에 적용합니다. 동시 실행 수는 적용 시 작업관리자에 반영합니다. 실행 중인 작업은 중단하지 않습니다."), Queue); JobsNote->setWordWrap(true); QueueForm->addRow(JobsNote);
    CommonRows->addStretch();

    auto* CopyRows = JobTab(tr("복사·이동"), "hc.settings.fileOperationsCopy");
    auto* ConflictForm = JobGroup(CopyRows, tr("같은 이름의 파일"));
    defaultConflict_ = new QComboBox(ConflictForm->parentWidget()); defaultConflict_->addItems({tr("매번 묻기"), tr("덮어쓰기"), tr("건너뛰기")}); defaultConflict_->setObjectName("hc.settings.fileConflict"); ConflictForm->addRow(tr("같은 이름이 있을 때"), defaultConflict_);
    auto* PreserveForm = JobGroup(CopyRows, tr("속성 보존 · 검증"));
    auto* Preserve = PreserveForm->parentWidget();
    defaultMetadata_ = new QCheckBox(tr("파일 속성 및 수정 시각 유지"), Preserve); defaultMetadata_->setObjectName("hc.settings.copyMetadata"); PreserveForm->addRow(defaultMetadata_);
    defaultVerify_ = new QCheckBox(tr("SHA-256 검증 후 게시"), Preserve); defaultVerify_->setObjectName("hc.settings.copyVerify"); PreserveForm->addRow(defaultVerify_);
    defaultAcl_ = new QCheckBox(tr("파일 접근 권한(DACL) 복사"), Preserve); defaultAcl_->setObjectName("hc.settings.copyAcl"); PreserveForm->addRow(defaultAcl_);
    defaultStreams_ = new QCheckBox(tr("파일 대체 데이터 스트림(ADS) 복사"), Preserve); defaultStreams_->setObjectName("hc.settings.copyStreams"); PreserveForm->addRow(defaultStreams_);
    auto* CopyHelp = new QLabel(tr("공급자가 지원하는 옵션만 적용됩니다. DACL·ADS는 파일에 적용하며, 폴더 권한과 링크 보존은 지원하지 않습니다."), Preserve); CopyHelp->setWordWrap(true); PreserveForm->addRow(CopyHelp);
    auto* TransferForm = JobGroup(CopyRows, tr("파일 필터 · 속도 · 쓰기 방식"));
    auto* Transfer = TransferForm->parentWidget();
    defaultPattern_ = new QLineEdit("*", Transfer); defaultPattern_->setObjectName("hc.settings.copyPattern"); TransferForm->addRow(tr("포함할 파일 (* · ? · ; 구분)"), defaultPattern_);
    defaultCopyError_ = new QLabel(tr("파일명 패턴에는 경로·빈 패턴을 넣을 수 없습니다."), Transfer); defaultCopyError_->setObjectName("hc.settings.copyError"); defaultCopyError_->hide(); TransferForm->addRow(defaultCopyError_);
    defaultSpeed_ = new QSpinBox(Transfer); defaultSpeed_->setObjectName("hc.settings.copySpeed"); defaultSpeed_->setRange(0, 1048576); defaultSpeed_->setSuffix(" MiB/s"); defaultSpeed_->setSpecialValueText(tr("제한 없음")); TransferForm->addRow(tr("새 복사·이동의 속도 제한"), defaultSpeed_);
    auto* DelayedWrites = new QCheckBox(tr("파일 내용 지연 쓰기"), Transfer); DelayedWrites->setObjectName("hc.settings.delayedFileWrites"); TransferForm->addRow(DelayedWrites);
    auto* DelayedWritesNote = new QLabel(tr("켜면 파일마다 디스크 강제 저장을 생략해 OS 쓰기 캐시를 사용합니다. 전원 장애나 시스템 중단 직후에는 최근에 완료한 파일의 내용이 손실될 수 있습니다. 끄면 파일마다 강제 저장합니다. 새 작업부터 적용됩니다."), Transfer); DelayedWritesNote->setWordWrap(true); TransferForm->addRow(DelayedWritesNote);
    CopyRows->addStretch();

    auto* RenameRows = JobTab(tr("이름 변경"), "hc.settings.fileOperationsRename");
    auto* RenameForm = JobGroup(RenameRows, tr("이름 변경 시작"));
    auto* Rename = RenameForm->parentWidget();
    auto* RenameDialog = new QCheckBox(tr("이름 변경 키를 눌렀을 때 별도 대화상자 표시"), Rename);
    RenameDialog->setObjectName("hc.settings.renameInDialog"); RenameForm->addRow(RenameDialog);
    auto* RenameExtension = new QCheckBox(tr("이름 변경을 시작할 때 확장자까지 기본 선택"), Rename);
    RenameExtension->setObjectName("hc.settings.renameSelectExtension"); RenameForm->addRow(RenameExtension);
    auto* RenameClick = new QCheckBox(tr("같은 파일 이름을 천천히 두 번 클릭하여 이름 변경"), Rename);
    RenameClick->setObjectName("hc.settings.renameOnSlowClick"); RenameForm->addRow(RenameClick);
    RenameClick->setToolTip(tr("더블클릭은 파일·폴더를 엽니다. 끄면 이름 변경 키와 명령으로만 이름 변경을 시작합니다."));
    auto* RenameNote = new QLabel(tr("대화상자 표시를 끄면 목록 안에서 이름을 변경합니다. 마지막 점(.) 뒤를 확장자로 구분합니다.\n목록 편집 중 이름 변경 단축키를 다시 누르면 전체 이름과 확장자를 제외한 이름 선택이 전환됩니다. 위·아래 화살표는 변경을 완료한 뒤 이전·다음 항목을 편집하며, Esc는 편집을 취소합니다."), Rename);
    RenameNote->setWordWrap(true); RenameForm->addRow(RenameNote); RenameRows->addStretch();

    auto* DeleteRows = JobTab(tr("삭제"), "hc.settings.fileOperationsDelete");
    auto* DeleteForm = JobGroup(DeleteRows, tr("휴지통 · 영구 삭제"));
    auto* Delete = DeleteForm->parentWidget();
    auto* RecycleDefault = new QCheckBox(tr("삭제할 때 휴지통을 기본 선택"), Delete); RecycleDefault->setObjectName("hc.settings.recycleDefault"); DeleteForm->addRow(RecycleDefault);
    auto* IncrementalDelete = new QCheckBox(tr("파일 목록을 확인하는 중에도 삭제 시작(&L)"), Delete);
    IncrementalDelete->setObjectName("hc.settings.deleteWhileListing"); DeleteForm->addRow(IncrementalDelete);
    IncrementalDelete->setToolTip(tr("기본값은 켜짐입니다. 확인한 파일부터 처리하며, 전체 목록 확인이 끝나기 전까지 총개수는 집계 중으로 표시합니다.\n끄면 전체 목록 확인 후 삭제합니다. 휴지통 삭제는 복원 구조를 유지하도록 선택 항목별로 확인 후 이동합니다.\n압축 파일 내부는 압축 파일을 갱신하는 일괄 작업으로 처리합니다."));
    auto* DeleteNote = new QLabel(tr("휴지통을 지원하지 않는 위치는 영구 삭제를 직접 선택해야 합니다."), Delete); DeleteNote->setWordWrap(true); DeleteForm->addRow(DeleteNote);
    DeleteRows->addStretch();
    auto* Laboratory = Page(tr("실험실"), tr("실험적인 기능을 선택합니다. 적용 시점은 각 항목의 설명을 확인하세요."));
    Laboratory->setObjectName("hc.settings.laboratory");
    auto* RecoveryCard = new fm::ui::Card(Laboratory); auto* RecoveryLayout = new QVBoxLayout(RecoveryCard);
    auto* EnableRecovery = new QCheckBox(tr("작업 복구"), RecoveryCard); EnableRecovery->setObjectName("hc.settings.enableJobRecovery"); RecoveryLayout->addWidget(EnableRecovery);
    auto* RecoveryNote = new QLabel(tr("기본값은 꺼짐입니다. 켜면 작업 결과와 파일 쓰기 단계를 저장하고, 비정상 종료 후 다시 실행할 때 중단된 쓰기를 확인하고 기존 대상 파일을 복원합니다.\n끄면 새 작업의 복구 기록과 재실행 후 작업 이력을 저장하지 않습니다. 현재 세션의 진행률·결과·실행 취소는 유지됩니다. 압축 해제를 중단 위치부터 자동으로 이어가는 기능은 아닙니다.\n덮어쓰기 도중 종료되면 기존 파일이 .hc-backup-* 이름으로 남을 수 있으며, 수동 확인과 복원이 필요합니다.\n변경은 새 작업부터 적용합니다. 이전 기록은 삭제하지 않으며, 켠 상태로 다시 실행하면 확인할 수 있습니다."), RecoveryCard); RecoveryNote->setWordWrap(true); RecoveryLayout->addWidget(RecoveryNote);
    qobject_cast<QVBoxLayout*>(Laboratory->layout())->addWidget(RecoveryCard);
    auto* ProgressCard = new fm::ui::Card(Laboratory); auto* ProgressLayout = new QVBoxLayout(ProgressCard);
    auto* SaveProgress = new QCheckBox(tr("진행률 저장"), ProgressCard); SaveProgress->setObjectName("hc.settings.saveJobProgress"); ProgressLayout->addWidget(SaveProgress);
    auto* ProgressNote = new QLabel(tr("작업 중 처리량과 현재 파일의 진행 위치를 디스크에 저장합니다. 기본값은 꺼짐이며, 켜면 반복 저장으로 작업 속도가 낮아질 수 있습니다.\n작업 복구가 켜져 있을 때 사용할 수 있습니다. 진행률 저장을 꺼도 복구에 필요한 작업 결과·파일 쓰기 단계는 저장합니다. 실시간 화면 갱신은 두 설정과 관계없이 유지됩니다."), ProgressCard);
    connect(EnableRecovery, &QCheckBox::toggled, SaveProgress, &QCheckBox::setEnabled);
    ProgressNote->setWordWrap(true); ProgressLayout->addWidget(ProgressNote);
    qobject_cast<QVBoxLayout*>(Laboratory->layout())->addWidget(ProgressCard);
    auto* DetailsCard = new fm::ui::Card(Laboratory); auto* DetailsLayout = new QVBoxLayout(DetailsCard);
    auto* ShowDetails = new QCheckBox(tr("상세 기록 보이기"), DetailsCard); ShowDetails->setObjectName("hc.settings.showJobDetails"); DetailsLayout->addWidget(ShowDetails);
    auto* DetailsNote = new QLabel(tr("작업 진행 창에 파일별 처리 결과와 진단 기록을 표시합니다. 기본값은 꺼짐입니다. 오류 안내와 확인·암호 입력 요청은 이 설정과 관계없이 표시합니다. 작업 복구·진행률 저장과 독립적으로 사용합니다."), DetailsCard);
    DetailsNote->setWordWrap(true); DetailsLayout->addWidget(DetailsNote); qobject_cast<QVBoxLayout*>(Laboratory->layout())->addWidget(DetailsCard);
    qobject_cast<QVBoxLayout*>(Laboratory->layout())->addStretch();
    auto* Keyboard = ui.pgShortcut;
    ui.keySequenceEdit->setMaximumSequenceLength(1);
    auto* KeyOptions = new QWidget(Keyboard); auto* KeyForm = new QFormLayout(KeyOptions);
    auto* Preset = new QComboBox(KeyOptions); Preset->setObjectName("hc.settings.shortcutPreset");
    Preset->addItems({tr("기존 기본값 (F5 새로 고침)"), tr("시안 기본값 (F5 복사 · F6 이동 · F7 새 폴더 · F8 삭제)")});
    KeyForm->addRow(tr("기본 단축키"), Preset);
    auto* ResetKeys = new QPushButton(tr("선택한 기본값으로 단축키 초기화"), KeyOptions); ResetKeys->setObjectName("hc.settings.resetShortcuts"); KeyForm->addRow(ResetKeys);
    auto* KeyHelp = new QLabel(tr("사용자가 지정한 키는 기본값보다 우선합니다. 변경 내용은 적용/확인으로 저장하고, 취소하면 되돌립니다.\n기능바와 메뉴에는 현재 연결된 키가 표시됩니다."), KeyOptions); KeyHelp->setWordWrap(true); KeyForm->addRow(KeyHelp);
    if (auto* Layout = qobject_cast<QVBoxLayout*>(Keyboard->layout())) Layout->insertWidget(0, KeyOptions); else Keyboard->layout()->addWidget(KeyOptions);
    connect(Preset, &QComboBox::currentIndexChanged, this, [this](int Index) {
        auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject(); General["shortcutPreset"] = Index == 1 ? "prototype" : "legacy";
        hc::settings::AppSettings().Put(hc::settings::Section::General, General); RefreshShortcuts();
    });
    connect(ResetKeys, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("단축키 초기화"), tr("사용자 지정 단축키를 지우고 선택한 기본값으로 되돌리시겠습니까?")) != QMessageBox::Yes) return;
        hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, QJsonObject{}); RefreshShortcuts();
    });
    keyboardSettings_ = new hc::ui::KeyboardSettingsPage(ui.stkOpts); ui.stkOpts->addWidget(keyboardSettings_);
    // Resolve page indices from widgets so removing a page cannot retarget navigation.
    menuSettings_ = new hc::ui::MenuBarSettingsPage(ui.stkOpts); ui.stkOpts->addWidget(menuSettings_);
    auto* Permissions=hc::ui::PermissionSettingsPage(ui.stkOpts); ui.stkOpts->addWidget(Permissions);
    const QSignalBlocker Block(ui.lstOptCate); ui.lstOptCate->clear();
    const QList<QPair<QString, int>> Pages{{tr("일반 · 모양"), ui.stkOpts->indexOf(General)}, {tr("테마 색상"), ui.stkOpts->indexOf(themeSettings_)}, {tr("파일 패널"), ui.stkOpts->indexOf(ui.pgDisplay)},
        {tr("썸네일 보기"), ui.stkOpts->indexOf(Thumbnails)}, {tr("파일 그룹 · 색상"), ui.stkOpts->indexOf(FileColorsScroll)}, {tr("열 · 사용자 정의 열"), ui.stkOpts->indexOf(ui.pgCustomColumns)},
        {tr("파일 작업"), ui.stkOpts->indexOf(Jobs)}, {tr("키보드"), ui.stkOpts->indexOf(keyboardSettings_)}, {tr("관리자 권한"), ui.stkOpts->indexOf(Permissions)}, {tr("플러그인 관리"), ui.stkOpts->indexOf(ui.pgPluginManage)}, {tr("탐색"), ui.stkOpts->indexOf(findChild<QWidget*>("hc.settings.navigation"))}, {tr("메뉴바"), ui.stkOpts->indexOf(menuSettings_)}, {tr("실험실"), ui.stkOpts->indexOf(Laboratory)}};
    for (const auto& Entry : Pages) { auto* Item = new QListWidgetItem(Entry.first, ui.lstOptCate); Item->setData(Qt::UserRole, Entry.second); }
    auto* SaveStatus = new QLabel(this); SaveStatus->setObjectName("hc.settings.saveStatus"); SaveStatus->setTextFormat(Qt::PlainText); SaveStatus->setWordWrap(true); SaveStatus->setContentsMargins(12, 4, 12, 4);
    SaveStatus->setText(tr("초안 · 적용/확인으로 저장 · 취소로 이전 설정 유지\n%1").arg(hc::settings::AppSettings().FileName())); ui.verticalLayout->insertWidget(ui.verticalLayout->count() - 1, SaveStatus);
    auto* Sidebar = new QWidget(this); auto* SidebarLayout = new QVBoxLayout(Sidebar); SidebarLayout->setContentsMargins(0, 0, 0, 0);
    Sidebar->setObjectName("hc.settings.sidebar");
    Sidebar->setFixedWidth(qBound(210, ui.lstOptCate->sizeHintForColumn(0) + 32, 260));
    ui.lstOptCate->setMinimumWidth(0); ui.lstOptCate->setMaximumWidth(QWIDGETSIZE_MAX);
    ui.lstOptCate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Constrain the entire navigation column, including its search field.
    // Constraining only the list lets its parent consume unused horizontal space.
    delete ui.horizontalLayout_2->replaceWidget(ui.lstOptCate, Sidebar);
    ui.horizontalLayout_2->setStretch(0, 0); ui.horizontalLayout_2->setStretch(1, 1);
    auto* Search = new QLineEdit(Sidebar); Search->setObjectName("hc.settings.search"); Search->setClearButtonEnabled(true); Search->setPlaceholderText(tr("설정 찾기 · Ctrl+F")); Search->setAccessibleName(tr("설정 페이지 찾기")); SidebarLayout->addWidget(Search); SidebarLayout->addWidget(ui.lstOptCate, 1);
    auto* Find = new QShortcut(QKeySequence::Find, this); connect(Find, &QShortcut::activated, Search, qOverload<>(&QWidget::setFocus));
    connect(Search, &QLineEdit::textChanged, this, [this](const QString& Text) { for (int Row = 0; Row < ui.lstOptCate->count(); ++Row) {
        auto* Item = ui.lstOptCate->item(Row); auto* Page = ui.stkOpts->widget(Item->data(Qt::UserRole).toInt()); QString Content = Item->text();
        for (auto* Label : Page->findChildren<QLabel*>()) Content += " " + Label->text(); for (auto* Button : Page->findChildren<QAbstractButton*>()) Content += " " + Button->text();
        for (auto* Combo : Page->findChildren<QComboBox*>()) for (int I = 0; I < Combo->count(); ++I) Content += " " + Combo->itemText(I);
        for (auto* Tabs : Page->findChildren<QTabWidget*>()) for (int I = 0; I < Tabs->count(); ++I) Content += " " + Tabs->tabText(I);
        Item->setHidden(!Content.contains(Text.trimmed(), Qt::CaseInsensitive));
    } });
    ui.lstOptCate->setCurrentRow(0); ui.stkOpts->setCurrentWidget(General);
}
