#pragma once

#include "cmnUiUtils.hpp"

#include "ui_dlgFileMove.h"

class QFileMoveUI : public nsHC::QBaseUI
{
    Q_OBJECT
public:
    QFileMoveUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::FramelessWindowHint );

private:

    Ui::dlgFileMove                     ui;
};
