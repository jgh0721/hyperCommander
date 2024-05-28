#pragma once

#include <QtCore>

#include "Qt\cmnQtUtils.hpp"

struct TyHC_COMMAND
{
    QString                             Cate;
    QLatin1String                       Name;
    QKeySequence                        Default;
    QString                             Desc;
};

using TySetCMDShortcut = QSet< QKeySequence >;
using TyMapShortcutToCMD = QHash< QKeySequence, TyHC_COMMAND >;

#define DECLARE_CMD( Var, Cate, Name, Shortcut, Desc ) \
    const TyHC_COMMAND Var = { Cate, QLatin1String( Name ), QKeySequence( Shortcut ), Desc };


DECLARE_CMD( BuiltInCMD_ContextMenu,                QObject::tr( "기타" ), "cm_ContextMenu",   
             Qt::ShiftModifier | Qt::Key_Enter,     QObject::tr( "상황메뉴" ) );

//const TyHC_COMMAND BuiltInCMD_Return            = { QLatin1String( "cmd_Return" ) };
//const TyHC_COMMAND BuiltInCMD_CopyToSame        = { QLatin1String( "cmd_CopyToSame" ) };
//const TyHC_COMMAND BuiltInCMD_CopyToAnother     = { QLatin1String( "cmd_CopyToAnother" ) };
//const TyHC_COMMAND BuiltInCMD_Tab_New           = { QLatin1String( "cmd_NewTab" ) };
//const TyHC_COMMAND BuiltInCMD_Tab_Prev          = { QLatin1String( "cmd_PrevTab" ) };
//const TyHC_COMMAND BuiltInCMD_Tab_Next          = { QLatin1String( "cmd_NextTab" ) };
//const TyHC_COMMAND BuiltInCMD_Tab_Close         = { QLatin1String( "cmd_CloseTab" ), QObject::tr("탭 닫기"), QKeySequence( Qt::Key_Control | Qt::Key_W ) };
//const TyHC_COMMAND BuiltInCMD_File_MultiRename  = { QLatin1String( "cmd_MultiRename" ) };
//const TyHC_COMMAND BuiltInCMD_View_SwitchHidSys = { QLatin1String( "cmd_SwitchHidSys" ), QObject::tr("숨김/시스템 파일 표시"), QKeySequence( Qt::Key_Control | Qt::Key_H ) };

// 해당 벡터를 순서대로 순회하며 환경설정 대화상자에 표시한다. 
const QVector< TyHC_COMMAND > GlobalBuiltInCMDs = {
    BuiltInCMD_ContextMenu
};
