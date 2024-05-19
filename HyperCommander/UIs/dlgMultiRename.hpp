#pragma once

#include "dlgMultiRename_Model.hpp"

#include "ui_dlgMultiRename.h"

class QMultiRenameUI : public QDialog
{
    Q_OBJECT
public:
    QMultiRenameUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::Dialog );

private:

    QMultiRenameModel*                  Model = nullptr;
    Qtitan::GridBandedTableView*        View = nullptr;
    Ui::dlgMultiRename                  Ui;
};