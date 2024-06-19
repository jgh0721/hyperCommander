#pragma once

#include "cmnTypeDefs_Opts.hpp"

#include "ui_dlgOpts.h"

struct TyFileSet;
struct TyColorScheme;

const QPair< int, QString > PAGE_DISPLAY                    = { 0, QObject::tr("화면") };
const QPair< int, QString > PAGE_DISPLAY_FONTCOLOR          = { 1, QObject::tr("  글꼴 및 색상") };
const QPair< int, QString > PAGE_FILESET                    = { 2, QObject::tr("파일집합 정의") };
const QPair< int, QString > PAGE_SHORTCUT                   = { 3, QObject::tr("단축키") };
const QPair< int, QString > PAGE_CUSTOM_COLUMNS             = { 4, QObject::tr("사용자정의 컬럼") };
const QPair< int, QString > PAGE_PLUGIN_MGR                 = { 5, QObject::tr("플러그인 관리") };

class QMainOpts : public QDialog
{
    Q_OBJECT
public:
    QMainOpts( QWidget* parent = Q_NULLPTR, Qt::WindowFlags flags = Qt::FramelessWindowHint );

    Q_INVOKABLE void                    LoadSettings();
    Q_INVOKABLE void                    SaveSettings();
    Q_INVOKABLE void                    SaveSettings_ColorScheme();
    Q_INVOKABLE void                    SaveSettings_PluginList();
    Q_INVOKABLE void                    RefreshColorScheme( const QString& SchemeName );
    Q_INVOKABLE void                    RefreshFileSet( const QString& FileSetName );
    Q_INVOKABLE void                    RefreshPluginList();

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

    
    /// Page = pgDisplay_ColorScheme
    void                                on_cbxColorScheme_currentIndexChanged( int index );
    void                                on_btnAddColorScheme_clicked( bool checked = false );

    void                                on_btnLstFGColor_clicked( bool checked = false );
    void                                on_btnLstBGColor_clicked( bool checked = false );
    void                                on_btnLstCursorColor_clicked( bool checked = false );
    void                                on_btnLstSelectColor_clicked( bool checked = false );

    void                                oo_refreshFileSetColor();
    void                                oo_insertFileSetColorRow( const TyFileSet* FileSet, const QPair< QColor, QColor >& FGWithBG );
    int                                 oo_insertEmptyFileSetColorRow();
    void                                oo_removeFileSetColorRow();

    /// Page = pgFileSet
    void                                on_lstFileSet_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous );

    void                                on_btnResetFileSet_clicked( bool checked = false );
    void                                on_btnAddFileSet_clicked( bool checked = false );
    void                                on_btnRemoveFileSet_clicked( bool checked = false );

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

    void                                setButtonColor( QPushButton* Button, const QColor& Color );
    void                                resetColorScheme( const QString& Name );
    Q_INVOKABLE void                    pickButtonColor( bool checked = false );

    Ui::dlgOpts                         ui;
};