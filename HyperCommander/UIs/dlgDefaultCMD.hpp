#pragma once

#include "cmnUiUtils.hpp"
#include "cmnTypeDefs.hpp"

#include "commandMgr_Defs.hpp"

#include "ui_dlgDefaultCMD.h"

class QDefaultCMDUI : public nsHC::QBaseUI
{
    Q_OBJECT
public:
    QDefaultCMDUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );

    TyHC_COMMAND                        GetSelectedCMD() const;

public slots:
    void                                on_btnClose_clicked( bool checked = false );
    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );

private:

    Ui::dlgDefaultCMD                   ui;
};