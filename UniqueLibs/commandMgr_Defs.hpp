#pragma once

#include <QtCore>

#include "Qt/cmnQtUtils.hpp"

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
void Class:: ##Name( const QModelIndex& CursorIndex )

DECLARE_CMD( BuiltInCMD_Tab_New,                    QObject::tr( "탭" ), "cm_NewTab",
             Qt::ControlModifier | Qt::Key_T,       QObject::tr( "새 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Prev,                   QObject::tr( "탭" ), "cm_PrevTab",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Backtab,     QObject::tr( "이전 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Next,                   QObject::tr( "탭" ), "cm_NextTab",
             Qt::ControlModifier | Qt::Key_Tab,     QObject::tr( "다음 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Close,                  QObject::tr( "탭" ), "cm_CloseTab",
             Qt::ControlModifier | Qt::Key_W,       QObject::tr( "탭 닫기" ) );

DECLARE_CMD( BuiltInCMD_OPs_FileCopy,               QObject::tr( "동작" ), "cm_CopyOtherPanel",
             Qt::ControlModifier | Qt::Key_C,       QObject::tr( "다른 패널로 복사" ) );
DECLARE_CMD( BuiltInCMD_OPs_RenameSingleFile,       QObject::tr( "동작" ), "cm_RenameSingleFile",
             Qt::Key_F2,                            QObject::tr( "커서 위치의 파일명 변경" ) );
DECLARE_CMD( BuiltInCMD_OPs_Lister,                 QObject::tr( "동작" ), "cm_List",
             Qt::Key_F3,                            QObject::tr( "내장 뷰어로 보기" ) );
DECLARE_CMD( BuiltInCMD_OPs_Return,                 QObject::tr( "동작" ), "cm_Return",
             Qt::Key_Return,                        QObject::tr( "실행(엔터 키 입력)" ) );
DECLARE_CMD( BuiltInCMD_OPs_Enter,                  QObject::tr( "동작" ), "cm_Return",
             Qt::KeypadModifier | Qt::Key_Enter,    QObject::tr( "실행(엔터 키 입력)" ) );
DECLARE_CMD( BuiltInCMD_OPs_MkDir,                  QObject::tr( "동작" ), "cm_MkDir",
             Qt::ControlModifier | Qt::Key_K,       QObject::tr( "새 폴더" ) );
DECLARE_CMD( BuiltInCMD_OPs_Delete,                 QObject::tr( "동작" ), "cm_Delete",
             Qt::Key_Delete,                        QObject::tr( "파일 삭제" ) );
DECLARE_CMD( BuiltInCMD_OPs_UnicodeNormalization,   QObject::tr( "동작" ), "cm_NameNormalization",
             Qt::ControlModifier | Qt::Key_U,       QObject::tr( "파일 이름 정규화(NFC) 교정" ) );

DECLARE_CMD( BuiltInCMD_Sel_Inverse,                QObject::tr( "선택" ), "cm_SelInverse",
             Qt::Key_Space,                         QObject::tr( "파일 선택을 반전하고 다음으로 이동" ) );

DECLARE_CMD( BuiltInCMD_Tool_MultiRenameFiles,      QObject::tr( "보기" ), "cm_MultiRenameFiles",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_R,       QObject::tr( "일괄 이름변경" ) );

DECLARE_CMD( BuiltInCMD_View_SwitchHidSys,          QObject::tr( "보기" ), "cm_SwitchHidSys",
             Qt::ControlModifier | Qt::Key_H,       QObject::tr( "숨김/시스템 파일 표시 전환" ) );
DECLARE_CMD( BuiltInCMD_View_RereadSource,          QObject::tr( "보기" ), "cm_RereadSource",
             Qt::Key_F5,                            QObject::tr( "원본 새로 고침" ) );

DECLARE_CMD( BuiltInCMD_Etc_SwitchPanel,            QObject::tr( "기타" ), "cm_SwitchPanel",
             Qt::Key_Tab,                           QObject::tr( "반대편 패널로 전환" ) );

DECLARE_CMD( BuiltInCMD_Etc_ContextMenu,            QObject::tr( "기타" ), "cm_ContextMenu",
             Qt::ShiftModifier | Qt::Key_Return,    QObject::tr( "상황메뉴" ) );

DECLARE_CMD( BuiltInCMD_Etc_ExternalMenu,           QObject::tr( "기타" ), "cm_ExternalEditorMenu",
             Qt::Key_F4,                            QObject::tr( "외부 편집기 메뉴" ) );

//const TyHC_COMMAND BuiltInCMD_CopyToSame        = { QLatin1String( "cmd_CopyToSame" ) };
//const TyHC_COMMAND BuiltInCMD_CopyToAnother     = { QLatin1String( "cmd_CopyToAnother" ) };

// 해당 벡터를 순서대로 순회하며 환경설정 대화상자에 표시한다. 
const QVector< TyHC_COMMAND > GlobalBuiltInCMDs = {
    BuiltInCMD_Tab_New,
    BuiltInCMD_Tab_Prev,
    BuiltInCMD_Tab_Next,
    BuiltInCMD_Tab_Close,

    BuiltInCMD_OPs_FileCopy,
    BuiltInCMD_OPs_RenameSingleFile,
    BuiltInCMD_OPs_Lister,
    BuiltInCMD_OPs_Return,
    BuiltInCMD_OPs_Enter,
    BuiltInCMD_OPs_MkDir,
    BuiltInCMD_OPs_Delete,
    BuiltInCMD_OPs_UnicodeNormalization,

    BuiltInCMD_Sel_Inverse,

    BuiltInCMD_Tool_MultiRenameFiles,

    BuiltInCMD_View_SwitchHidSys,
    BuiltInCMD_View_RereadSource,

    BuiltInCMD_Etc_SwitchPanel,
    BuiltInCMD_Etc_ContextMenu,
    BuiltInCMD_Etc_ExternalMenu,
};
