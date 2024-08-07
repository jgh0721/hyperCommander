﻿#pragma once

#include "UniqueLibs/commandMgr_Defs.hpp"

#include "ui_dlgMain.h"

class QSHChangeNotify;

class QMainUI : public QMainWindow
{
    Q_OBJECT
public:
    QMainUI( QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );

public slots:

    void                            OnDriveAdd( const QString& Root );
    void                            OnDriveRemoved( const QString& Root );
    void                            OnMediaInserted( const QString& ItemIDDisplayName );
    void                            OnMediaRemoved( const QString& Root );
    void                            OnColorSchemeChanged( const TyColorScheme& ColorScheme );

    ////////////////////////////////////////////////////////////////////////////
    /// 명령 핸들러

    DECLARE_HC_COMMAND( cm_NewTab );
    DECLARE_HC_COMMAND( cm_PrevTab );
    DECLARE_HC_COMMAND( cm_NextTab );
    DECLARE_HC_COMMAND( cm_CloseTab );

    DECLARE_HC_COMMAND( cm_SrcQuickView );

    DECLARE_HC_COMMAND( cm_CopyOtherPanel );
    DECLARE_HC_COMMAND( cm_RenameSingleFile );
    DECLARE_HC_COMMAND( cm_List );
    DECLARE_HC_COMMAND( cm_Return );
    DECLARE_HC_COMMAND( cm_MkDir );
    DECLARE_HC_COMMAND( cm_Delete );
    DECLARE_HC_COMMAND( cm_NameNormalization );
    DECLARE_HC_COMMAND( cm_SetAttrib );

    DECLARE_HC_COMMAND( cm_SelInverse );

    DECLARE_HC_COMMAND( cm_MultiRenameFiles );

    DECLARE_HC_COMMAND( cm_SwitchHidSys );
    DECLARE_HC_COMMAND( cm_RereadSource );

    DECLARE_HC_COMMAND( cm_DirectoryHotList );
    DECLARE_HC_COMMAND_EX( cm_GotoDrive );
    DECLARE_HC_COMMAND_EX( cm_ChangePath );

    DECLARE_HC_COMMAND( cm_SwitchPanel );
    DECLARE_HC_COMMAND( cm_ContextMenu );
    DECLARE_HC_COMMAND( cm_ExternalEditorMenu );

protected slots:

    void                                OnAddCurrentDirToFavorite();

    void                                on_acShowMainOpts_triggered( bool checked = false );

    void                                oo_notifyCurrentDirectory( const QString& CurrentPath );
    void                                oo_notifyPanelActivated();

protected:
    bool                                eventFilter( QObject* Object, QEvent* Event ) override;
    void                                closeEvent( QCloseEvent* Event ) override;

private:
    Q_INVOKABLE void                    initialize();

    CmpPanel*                           retrieveSrcPanel() const;
    CmpPanel*                           retrieveDstPanel() const;


    QSHChangeNotify*                    shlChangeNotify = nullptr;
    int                                 currentPanelIndex = 0; // 0 = Left, 1 = Right

    Ui::dlgMain                         ui;
}; 