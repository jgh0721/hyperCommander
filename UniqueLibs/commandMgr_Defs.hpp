#pragma once

#include <QtCore>

#include "Qt\cmnQtUtils.hpp"

struct TyHC_COMMAND
{
    QLatin1String                       Name;
    QString                             Desc;
    QKeySequence                        DefaultBinding;

    TySpInvokeInfo                      SpInvokeInfo;
};

const TyHC_COMMAND BuiltInCMD_ContextMenu       = { QLatin1String( "cmd_ContextMenu" ) };
const TyHC_COMMAND BuiltInCMD_Return            = { QLatin1String( "cmd_Return" ) };
const TyHC_COMMAND BuiltInCMD_CopyToSame        = { QLatin1String( "cmd_CopyToSame" ) };
const TyHC_COMMAND BuiltInCMD_CopyToAnother     = { QLatin1String( "cmd_CopyToAnother" ) };
const TyHC_COMMAND BuiltInCMD_Tab_New           = { QLatin1String( "cmd_NewTab" ) };
const TyHC_COMMAND BuiltInCMD_Tab_Close         = { QLatin1String( "cmd_CloseTab" ), QObject::tr("탭 닫기"), QKeySequence( Qt::Key_Control | Qt::Key_W ) };
const TyHC_COMMAND BuiltInCMD_File_MultiRename  = { QLatin1String( "cmd_MultiRename" ) };

const QVector< TyHC_COMMAND > Global = {
    BuiltInCMD_ContextMenu
};
