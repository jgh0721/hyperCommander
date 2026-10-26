#pragma once

#include "cmnUiUtils.hpp"
#include "solFavoriteDirMgr.hpp"

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
    void                            moveCurrent( int Offset );
    void                            arrangeLayout();

    void                            on_edtCommand_editingFinished();
    void                            on_edtPath_editingFinished();

    void                            on_btnOK_clicked( bool checked = false );
    void                            on_btnCancel_clicked( bool checked = false );

    void                            on_rdoDrivesOnNone_toggled( bool checked = false );
    void                            on_rdoDrivesOnTop_toggled( bool checked = false );
    void                            on_rdoDrivesOnBottom_toggled( bool checked = false );

private:
    QString                         promptName( const QString& Initial, int ExcludedRow = -1 );
    void                            refresh();
    void                            refreshItem( const QString& Name );

    QVector<CFavoriteDirMgr::TyFavoriteDir> items_;
    CFavoriteDirMgr::TyEnDrivesOn     drivesOn_ = CFavoriteDirMgr::DRIVES_ON_NONE;
    Ui::dlgFavoriteDir              ui;
    QLineEdit*                      nameView_ = nullptr;
    QLabel*                         status_ = nullptr;
    QPushButton*                    moveUp_ = nullptr;
    QPushButton*                    moveDown_ = nullptr;
    QPushButton*                    browse_ = nullptr;
};