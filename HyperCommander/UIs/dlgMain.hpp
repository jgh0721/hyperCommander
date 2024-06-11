#pragma once

#include "UniqueLibs/commandMgr_Defs.hpp"

#include "ui_dlgMain.h"

class QMainUI : public QMainWindow
{
    Q_OBJECT
public:
    QMainUI( QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );

public slots:

    ////////////////////////////////////////////////////////////////////////////
    /// 명령 핸들러

    DECLARE_HC_COMMAND( cm_NewTab );
    DECLARE_HC_COMMAND( cm_PrevTab );
    DECLARE_HC_COMMAND( cm_NextTab );
    DECLARE_HC_COMMAND( cm_CloseTab );

    DECLARE_HC_COMMAND( cm_RenameSingleFile );
    DECLARE_HC_COMMAND( cm_List );
    DECLARE_HC_COMMAND( cm_Return );
    DECLARE_HC_COMMAND( cm_MkDir );

    DECLARE_HC_COMMAND( cm_SelInverse );

    DECLARE_HC_COMMAND( cm_MultiRenameFiles );

    DECLARE_HC_COMMAND( cm_SwitchHidSys );

    DECLARE_HC_COMMAND( cm_SwitchPanel );
    DECLARE_HC_COMMAND( cm_ContextMenu );
    DECLARE_HC_COMMAND( cm_ExternalEditorMenu );

protected slots:

    void                                on_acShowMainOpts_triggered( bool checked = false );

    void                                oo_notifyCurrentDirectory( const QString& CurrentPath );
    void                                oo_notifyPanelActivated();

protected:
    bool                                eventFilter( QObject* Object, QEvent* Event ) override;
    void                                closeEvent( QCloseEvent* Event ) override;

private:
    Q_INVOKABLE void                    initialize();

    CmpPanel*                           currentFocusPanel() const;
    int                                 currentPanelIndex = 0; // 0 = Left, 1 = Right
    Ui::dlgMain                         ui;
}; 