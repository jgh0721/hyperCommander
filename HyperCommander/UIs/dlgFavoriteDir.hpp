#pragma once

#include "cmnUiUtils.hpp"

#include "ui_dlgFavoriteDir.h"

class QFavoriteDirConfigure : public nsHC::QBaseUI
{
    Q_OBJECT
public:
    QFavoriteDirConfigure( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );

protected slots:

    void                            on_btnClose_clicked( bool checked = false );
    void                            on_lstDir_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous );

    void                            on_btnAdd_clicked( bool checked = false );
    void                            on_btnRemove_clicked( bool checked = false );
    void                            on_btnRename_clicked( bool checked = false );

    void                            on_btnOK_clicked( bool checked = false );
    void                            on_btnCancel_clicked( bool checked = false );

    void                            on_rdoDrivesOnNone_toggled( bool checked = false );
    void                            on_rdoDrivesOnTop_toggled( bool checked = false );
    void                            on_rdoDrivesOnBottom_toggled( bool checked = false );

private:
    void                            refresh();
    void                            refreshItem( const QString& Name );

    Ui::dlgFavoriteDir              ui;
};