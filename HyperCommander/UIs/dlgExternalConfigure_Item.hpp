#pragma once

#include <QtGui>
#include <QtWidgets>

#include "ui_dlgExternalConfigure_Item.h"

#include "cmnTypeDefs.hpp"

class CExternalConfigureItem : public QDialog
{
    Q_OBJECT
public:
    CExternalConfigureItem( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Dialog );

    void                                    SetItem( const TyExternalEditor& Item );
    TyExternalEditor                        GetItem() const;


protected slots:
    void                                    on_btnClose_clicked( bool checked = false );

    void                                    on_btnBrowseIcon_clicked( bool checked = false );
    void                                    on_btnBrowseFile_clicked( bool checked = false );

    void                                    on_btnOK_clicked( bool checked = false );
    void                                    on_btnCancel_clicked( bool checked = false );

private:

    bool                                    setProgramIcon( QLabel* lbl, const QString& IconPath );

    Ui::dlgExternalConfigureItem            ui;
};