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

const TyHC_COMMAND BuiltInCMD_ContextMenu   = { QLatin1String( "cmd_ContextMenu" ) };
const TyHC_COMMAND BuiltInCMD_Return        = { QLatin1String( "cmd_Return" ) };
const TyHC_COMMAND BuiltInCMD_CopyToSame    = { QLatin1String( "cmd_CopyToSame" ) };
const TyHC_COMMAND BuiltInCMD_CopyToAnother = { QLatin1String( "cmd_CopyToAnother" ) };

const QVector< TyHC_COMMAND > Global = {
    BuiltInCMD_ContextMenu
};
