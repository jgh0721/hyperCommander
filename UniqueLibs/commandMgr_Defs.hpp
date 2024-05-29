﻿#pragma once

#include <QtCore>

#include "Qt\cmnQtUtils.hpp"

struct TyHC_COMMAND
{
    QString                             Cate;
    QLatin1String                       Name;
    QKeyCombination                     Default;
    QString                             Desc;
};

using TySetCMDShortcut      = QSet< QKeyCombination >;
using TyMapShortcutToCMD    = QHash< QKeyCombination, TyHC_COMMAND >;
using TyMapShortcutToCMDStr = QHash< QKeyCombination, QLatin1String >;

#define DECLARE_CMD( Var, Cate, Name, Shortcut, Desc ) \
    const TyHC_COMMAND Var = { Cate, QLatin1String( Name ), QKeyCombination::fromCombined( Shortcut ), Desc };

#define DECLARE_HC_COMMAND( Name ) \
    Q_INVOKABLE void Name( const QModelIndex& CursorIndex )

#define DEFINE_HC_COMMAND( Class, Name ) \
void Class::##Name( const QModelIndex& CursorIndex )

DECLARE_CMD( BuiltInCMD_Tab_New,                    QObject::tr( "탭" ), "cm_NewTab",
             Qt::ControlModifier | Qt::Key_T,       QObject::tr( "새 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Prev,                   QObject::tr( "탭" ), "cm_PrevTab",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Backtab,     QObject::tr( "이전 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Next,                   QObject::tr( "탭" ), "cm_NextTab",
             Qt::ControlModifier | Qt::Key_Tab,     QObject::tr( "다음 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Close,                  QObject::tr( "탭" ), "cm_CloseTab",
             Qt::ControlModifier | Qt::Key_W,       QObject::tr( "탭 닫기" ) );

DECLARE_CMD( BuiltInCMD_OPs_Return,                 QObject::tr( "동작" ), "cm_Return",
             Qt::Key_Return,                        QObject::tr( "실행(엔터 키 입력)" ) );

DECLARE_CMD( BuiltInCMD_Sel_Inverse,                QObject::tr( "선택" ), "cm_SelInverse",
             Qt::Key_Space,                         QObject::tr( "파일 선택을 반전하고 다음으로 이동" ) );

DECLARE_CMD( BuiltInCMD_Etc_SwitchPanel,            QObject::tr( "기타" ), "cm_SwitchPanel",
             Qt::Key_Tab,                           QObject::tr( "반대편 패널로 전환" ) );

DECLARE_CMD( BuiltInCMD_Etc_ContextMenu,            QObject::tr( "기타" ), "cm_ContextMenu",
             Qt::ShiftModifier | Qt::Key_Return,    QObject::tr( "상황메뉴" ) );

//const TyHC_COMMAND BuiltInCMD_CopyToSame        = { QLatin1String( "cmd_CopyToSame" ) };
//const TyHC_COMMAND BuiltInCMD_CopyToAnother     = { QLatin1String( "cmd_CopyToAnother" ) };
//const TyHC_COMMAND BuiltInCMD_File_MultiRename  = { QLatin1String( "cmd_MultiRename" ) };
//const TyHC_COMMAND BuiltInCMD_View_SwitchHidSys = { QLatin1String( "cmd_SwitchHidSys" ), QObject::tr("숨김/시스템 파일 표시"), QKeySequence( Qt::Key_Control | Qt::Key_H ) };

// 해당 벡터를 순서대로 순회하며 환경설정 대화상자에 표시한다. 
const QVector< TyHC_COMMAND > GlobalBuiltInCMDs = {
    BuiltInCMD_Tab_New,
    BuiltInCMD_Tab_Prev,
    BuiltInCMD_Tab_Next,
    BuiltInCMD_Tab_Close,

    BuiltInCMD_OPs_Return,

    BuiltInCMD_Sel_Inverse,

    BuiltInCMD_Etc_SwitchPanel,
    BuiltInCMD_Etc_ContextMenu,
};
