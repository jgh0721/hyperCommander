#pragma once

#include "cmnTypeDefs_Opts.hpp"
#include "cmnUiUtils.hpp"

#include "ui_dlgOpts.h"

struct TyFileSet;
struct TyColorScheme;
class QPluginManagementPage;
class CGridView;
class QtColumnProfileEditor;
namespace hc::ui { class FileColorRulesPage; class ThumbnailSettingsPage; class MenuBarSettingsPage; class ThemeSettingsPage; class KeyboardSettingsPage; }


class QMainOpts : public nsHC::QBaseUI
{
    Q_OBJECT
public:
    QMainOpts( QWidget* parent = Q_NULLPTR, Qt::WindowFlags flags = Qt::FramelessWindowHint );

    ~QMainOpts() override;
    void SetListView(CGridView* View);
    void ShowListSettingsPage();
    void ShowPluginSettingsPage(const QString& Binary = {}, bool Review = false);

    Q_INVOKABLE void                    LoadSettings();
    Q_INVOKABLE bool                    SaveSettings();
    Q_INVOKABLE void                    SaveSettings_ColorScheme();
    Q_INVOKABLE void                    SaveSettings_PluginList();
    Q_INVOKABLE void                    RefreshColorScheme( const QString& SchemeName );
    Q_INVOKABLE void                    RefreshPluginList();
    Q_INVOKABLE void                    RefreshShortcuts();

signals:
    void                                NotifyColorSchemeChanged( const TyColorScheme& ColorScheme );

protected slots:

    ///////////////////////////////////////////////////////////////////////////////
    /// TitleBar

    void                                on_btnClose_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Left

    void                                on_lstOptCate_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous );

    ///////////////////////////////////////////////////////////////////////////////
    /// Content

    
    /// Page = pgShortcut
    void                                on_btnAddShortcutCMD_clicked( bool checked = false );
    void                                on_btnDelShortcutCMD_clicked( bool checked = false );

    /// Page = pgPluginManage

    void                                on_btnAddWLX_clicked( bool checked = false );
    void                                on_btnRemoveWLX_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Footer

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );
    void                                on_btnApply_clicked( bool checked = false );

private:
    void                                initialize();
    void initializeDesignPages();
    QJsonObject committedSettings_;
    hc::ui::ThemeSettingsPage* themeSettings_ = nullptr;
    hc::ui::KeyboardSettingsPage* keyboardSettings_ = nullptr;

    hc::ui::MenuBarSettingsPage* menuSettings_ = nullptr;
    hc::ui::ThumbnailSettingsPage* thumbnailSettings_ = nullptr;
    hc::ui::FileColorRulesPage* fileColors_ = nullptr;
    QComboBox* defaultConflict_ = nullptr;
    QCheckBox* defaultDetailed_ = nullptr;
    QCheckBox* defaultClose_ = nullptr;
    QCheckBox* defaultMetadata_ = nullptr;
    QCheckBox* defaultVerify_ = nullptr;
    QCheckBox* defaultAcl_ = nullptr;
    QCheckBox* defaultStreams_ = nullptr;
    QLineEdit* defaultPattern_ = nullptr;
    QSpinBox* defaultSpeed_ = nullptr;
    QLabel* defaultCopyError_ = nullptr;

    QString selectedScheme_;
    QCheckBox* inverseCursor_ = nullptr;
    QCheckBox* inverseSelection_ = nullptr;

    Ui::dlgOpts                         ui;
    QPointer<CGridView> listView_;
    QComboBox* listDefault_ = nullptr;
    QComboBox* oneLineSeparator_ = nullptr;
    QComboBox* twoLineSeparator_ = nullptr;
    QComboBox* listDensity_ = nullptr;
    QComboBox* listElide_ = nullptr;
    QCheckBox* listNameBelow_ = nullptr;
    QSpinBox* listAutoWidth_ = nullptr;
    QSpinBox* listAutoPercent_ = nullptr;
    QLabel* listScope_ = nullptr;
    QWidget* listCard_ = nullptr;
    QPushButton* useListDefault_ = nullptr;
    bool resetListModeDraft_ = false;
    bool filePanelSupported_ = true;
    QtColumnProfileEditor* columnEditor_ = nullptr;
    QCheckBox* focusPreviousDirectory_ = nullptr;
    QButtonGroup* iconModes_ = nullptr;
    QCheckBox* specialFolderIcons_ = nullptr;
    QCheckBox* virtualFolderIcons_ = nullptr;
    QCheckBox* explorerDefaultIcons_ = nullptr;
    QCheckBox* iconOverlays_ = nullptr;
    QButtonGroup* directoryStyles_ = nullptr;
    QPluginManagementPage* pluginManagement_ = nullptr;
};
