#pragma once

// 제품 기본값의 편집 지점. 이 헤더를 고친 뒤 다시 빌드하면 새 설치에 적용된다.
// settings.json에 저장된 값(빈 배열·단축키 해제 포함)은 기본값보다 우선한다.
// 명령 ID는 hc.*로 통일한다. 이전 설정의 cm_* ID는 CommandAliases()에서 변환한다.
// 새 명령은 이 목록 외에 실행 핸들러도 구현해야 한다.

#include <QtCore>
#include "directoryRefreshSettings.hpp"
#include "quickFindSettings.hpp"
#include "commonLibs/commands/commandBus.hpp"

struct TyHC_COMMAND
{
    QString         Cate;
    QLatin1String   Name;
    QKeyCombination Default;
    QString         Desc;
};

using TySetCMDShortcut      = QSet<QKeyCombination>;
using TyMapShortcutToCMD    = QHash<QKeyCombination, TyHC_COMMAND>;
using TyMapShortcutToCMDStr = QHash<QKeyCombination, QLatin1String>;

#define DECLARE_CMD( Var, Cate, Name, Shortcut, Desc ) \
    inline const TyHC_COMMAND Var = { Cate, QLatin1String( Name ), QKeyCombination::fromCombined( Shortcut ), Desc };

DECLARE_CMD( BuiltInCMD_Tab_New,                  QObject::tr( "탭" ),     "hc.tab.new",
             Qt::ControlModifier | Qt::Key_T,                           QObject::tr( "새 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Prev,                 QObject::tr( "탭" ),     "hc.tab.previous",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Backtab, QObject::tr( "이전 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Next,                 QObject::tr( "탭" ),     "hc.tab.next",
             Qt::ControlModifier | Qt::Key_Tab,                         QObject::tr( "다음 탭" ) );
DECLARE_CMD( BuiltInCMD_Tab_Close,                QObject::tr( "탭" ),     "hc.tab.close",
             Qt::ControlModifier | Qt::Key_W,                           QObject::tr( "탭 닫기" ) );

DECLARE_CMD( BuiltInCMD_Src_QuickView,            QObject::tr( "원본" ),    "hc.viewer.quick",
             Qt::ControlModifier | Qt::Key_Q,                           QObject::tr( "빠른 보기 창" ) );

DECLARE_CMD( BuiltInCMD_OPs_FileCopy,             QObject::tr( "동작" ),    "hc.file.copy",
             Qt::ControlModifier | Qt::Key_C,                           QObject::tr( "다른 패널로 복사" ) );
DECLARE_CMD( BuiltInCMD_OPs_RenameSingleFile,     QObject::tr( "동작" ),    "hc.file.rename",
             Qt::Key_F2,                                                QObject::tr( "이름 변경 · 대화상자 또는 목록에서 편집" ) );
DECLARE_CMD( BuiltInCMD_OPs_Lister,               QObject::tr( "동작" ),    "hc.viewer.open",
             Qt::Key_F3,                                                QObject::tr( "문서 보기 · 프로그램 선택" ) );
DECLARE_CMD( BuiltInCMD_OPs_Return,               QObject::tr( "동작" ),    "hc.navigate.open",
             Qt::Key_Return,                                            QObject::tr( "실행(엔터 키 입력)" ) );
DECLARE_CMD( BuiltInCMD_OPs_Enter,                QObject::tr( "동작" ),    "hc.navigate.open",
             Qt::KeypadModifier | Qt::Key_Enter,                        QObject::tr( "실행(엔터 키 입력)" ) );
DECLARE_CMD( BuiltInCMD_OPs_MkDir,                QObject::tr( "동작" ),    "hc.file.mkdir",
             Qt::ControlModifier | Qt::Key_K,                           QObject::tr( "새 폴더" ) );
DECLARE_CMD( BuiltInCMD_OPs_Delete,               QObject::tr( "동작" ),    "hc.file.delete",
             Qt::Key_Delete,                                            QObject::tr( "파일 삭제" ) );
DECLARE_CMD( BuiltInCMD_OPs_DeleteOnKeypad,       QObject::tr( "동작" ),    "hc.file.delete",
             Qt::KeypadModifier | Qt::Key_Delete,                       QObject::tr( "파일 삭제" ) );
// 이름 정규화는 명령 ID만 유지하며 실행 기능은 아직 제공하지 않는다.
DECLARE_CMD( BuiltInCMD_OPs_UnicodeNormalization, QObject::tr( "동작" ),    "hc.file.normalize",
             Qt::ControlModifier | Qt::Key_U,                           QObject::tr( "파일 이름 정규화(NFC) 교정" ) );
DECLARE_CMD( BuiltInCMD_OPs_SetAttrib,            QObject::tr( "동작" ),    "hc.file.attributes",
             Qt::ControlModifier | Qt::Key_Z,                           QObject::tr( "속성 변경" ) );

DECLARE_CMD( BuiltInCMD_Sel_Inverse,              QObject::tr( "선택" ),    "hc.selection.invert",
             Qt::Key_Space,                                             QObject::tr( "파일 선택을 반전하고 다음으로 이동" ) );
DECLARE_CMD( BuiltInCMD_Sel_Select, QObject::tr("선택"), "hc.selection.select",
             Qt::ControlModifier | Qt::Key_Equal, QObject::tr("항목 선택…") );
DECLARE_CMD( BuiltInCMD_Sel_SelectKeypad, QObject::tr("선택"), "hc.selection.select",
             Qt::KeypadModifier | Qt::Key_Plus, QObject::tr("항목 선택…") );
DECLARE_CMD( BuiltInCMD_Sel_Deselect, QObject::tr("선택"), "hc.selection.deselect",
             Qt::ControlModifier | Qt::Key_Minus, QObject::tr("항목 선택 해제…") );
DECLARE_CMD( BuiltInCMD_Sel_DeselectKeypad, QObject::tr("선택"), "hc.selection.deselect",
             Qt::KeypadModifier | Qt::Key_Minus, QObject::tr("항목 선택 해제…") );
DECLARE_CMD( BuiltInCMD_Sel_All, QObject::tr("선택"), "hc.selection.all",
             Qt::ControlModifier | Qt::KeypadModifier | Qt::Key_Plus, QObject::tr("전체 선택") );
DECLARE_CMD( BuiltInCMD_Sel_Clear, QObject::tr("선택"), "hc.selection.clear",
             Qt::ControlModifier | Qt::KeypadModifier | Qt::Key_Minus, QObject::tr("전체 선택 해제") );
DECLARE_CMD( BuiltInCMD_Sel_InvertAll, QObject::tr("선택"), "hc.selection.invertAll",
             Qt::KeypadModifier | Qt::Key_Asterisk, QObject::tr("전체 선택 반전") );
DECLARE_CMD( BuiltInCMD_Sel_SameExtension, QObject::tr("선택"), "hc.selection.sameExtension",
             Qt::AltModifier | Qt::KeypadModifier | Qt::Key_Plus, QObject::tr("현재 항목과 같은 확장자 선택") );
DECLARE_CMD( BuiltInCMD_Sel_ClearExtension, QObject::tr("선택"), "hc.selection.clearExtension",
             Qt::AltModifier | Qt::KeypadModifier | Qt::Key_Minus, QObject::tr("같은 확장자 선택 해제") );
DECLARE_CMD( BuiltInCMD_Sel_Restore, QObject::tr("선택"), "hc.selection.restore",
             Qt::KeypadModifier | Qt::Key_Slash, QObject::tr("이전 선택 복원") );
DECLARE_CMD( BuiltInCMD_Sel_Remember, QObject::tr("선택"), "hc.selection.remember",
             Qt::Key_unknown, QObject::tr("현재 선택 기억") );
DECLARE_CMD( BuiltInCMD_Sel_Recall, QObject::tr("선택"), "hc.selection.recall",
             Qt::Key_unknown, QObject::tr("기억한 선택 불러오기") );

DECLARE_CMD( BuiltInCMD_Archive_Create,           QObject::tr( "동작" ),    "hc.archive.create",
             Qt::ControlModifier | Qt::Key_A,                           QObject::tr( "파일 압축" ) );
DECLARE_CMD( BuiltInCMD_FolderSize,               QObject::tr( "동작" ),    "hc.file.folderSize",
             Qt::AltModifier | Qt::ShiftModifier | Qt::Key_Return,       QObject::tr( "하위 폴더 점유 공간 계산" ) );
DECLARE_CMD( BuiltInCMD_FolderSizeKeypad,         QObject::tr( "동작" ),    "hc.file.folderSize",
             Qt::AltModifier | Qt::ShiftModifier | Qt::KeypadModifier | Qt::Key_Enter, QObject::tr( "하위 폴더 점유 공간 계산" ) );
DECLARE_CMD( BuiltInCMD_Archive_Extract,          QObject::tr( "동작" ),    "hc.archive.extract",
             Qt::ControlModifier | Qt::Key_X,                           QObject::tr( "파일 압축 해제" ) );

DECLARE_CMD( BuiltInCMD_EnterFile,                QObject::tr( "탐색" ),    "hc.navigate.enterFile",
             Qt::ControlModifier | Qt::Key_PageDown,                    QObject::tr( "파일 내부로 들어가기" ) );

DECLARE_CMD( BuiltInCMD_OPs_NewFile,              QObject::tr( "동작" ),    "hc.file.new",
             Qt::ShiftModifier | Qt::Key_F4,                            QObject::tr( "새 파일" ) );
DECLARE_CMD( BuiltInCMD_Tool_MultiRenameAlias,    QObject::tr( "보기" ),    "hc.file.batchRename",
             Qt::ControlModifier | Qt::Key_M,                           QObject::tr( "다중 이름 변경" ) );

DECLARE_CMD( BuiltInCMD_Tool_MultiRenameFiles,    QObject::tr( "보기" ),    "hc.file.batchRename",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_R,         QObject::tr( "일괄 이름변경" ) );

DECLARE_CMD( BuiltInCMD_View_SwitchHidSys,        QObject::tr( "보기" ),    "hc.view.hidden",
             Qt::ControlModifier | Qt::Key_H,                           QObject::tr( "숨김/시스템 파일 표시 전환" ) );
DECLARE_CMD( BuiltInCMD_View_RereadSource,        QObject::tr( "보기" ),    "hc.navigate.refresh",
             Qt::Key_F5,                                                QObject::tr( "원본 새로 고침" ) );

DECLARE_CMD( BuiltInCMD_Nav_FavoriteDir,          QObject::tr( "탐색" ),    "hc.navigate.favorites",
             Qt::ControlModifier | Qt::Key_G,                           QObject::tr( "즐겨찾기 폴더 목록" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDrive,            QObject::tr( "탐색" ),    "hc.navigate.path",
             Qt::Key_F12,                                               QObject::tr( "드라이브 전환" ) );

DECLARE_CMD( BuiltInCMD_Nav_GotoDriveA, QObject::tr( "탐색" ), "hc.navigate.gotoDriveA",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_A, QObject::tr( "A 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveB, QObject::tr( "탐색" ), "hc.navigate.gotoDriveB",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_B, QObject::tr( "B 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveC, QObject::tr( "탐색" ), "hc.navigate.gotoDriveC",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_C, QObject::tr( "C 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveD, QObject::tr( "탐색" ), "hc.navigate.gotoDriveD",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_D, QObject::tr( "D 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveE, QObject::tr( "탐색" ), "hc.navigate.gotoDriveE",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_E, QObject::tr( "E 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveF, QObject::tr( "탐색" ), "hc.navigate.gotoDriveF",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_F, QObject::tr( "F 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveG, QObject::tr( "탐색" ), "hc.navigate.gotoDriveG",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_G, QObject::tr( "G 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveH, QObject::tr( "탐색" ), "hc.navigate.gotoDriveH",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_H, QObject::tr( "H 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveI, QObject::tr( "탐색" ), "hc.navigate.gotoDriveI",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_I, QObject::tr( "I 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveJ, QObject::tr( "탐색" ), "hc.navigate.gotoDriveJ",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_J, QObject::tr( "J 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveK, QObject::tr( "탐색" ), "hc.navigate.gotoDriveK",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_K, QObject::tr( "K 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveL, QObject::tr( "탐색" ), "hc.navigate.gotoDriveL",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_L, QObject::tr( "L 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveM, QObject::tr( "탐색" ), "hc.navigate.gotoDriveM",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_M, QObject::tr( "M 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveN, QObject::tr( "탐색" ), "hc.navigate.gotoDriveN",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_N, QObject::tr( "N 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveO, QObject::tr( "탐색" ), "hc.navigate.gotoDriveO",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_O, QObject::tr( "O 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveP, QObject::tr( "탐색" ), "hc.navigate.gotoDriveP",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_P, QObject::tr( "P 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveQ, QObject::tr( "탐색" ), "hc.navigate.gotoDriveQ",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Q, QObject::tr( "Q 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveR, QObject::tr( "탐색" ), "hc.navigate.gotoDriveR",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_R, QObject::tr( "R 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveS, QObject::tr( "탐색" ), "hc.navigate.gotoDriveS",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_S, QObject::tr( "S 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveT, QObject::tr( "탐색" ), "hc.navigate.gotoDriveT",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T, QObject::tr( "T 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveU, QObject::tr( "탐색" ), "hc.navigate.gotoDriveU",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_U, QObject::tr( "U 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveV, QObject::tr( "탐색" ), "hc.navigate.gotoDriveV",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_V, QObject::tr( "V 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveW, QObject::tr( "탐색" ), "hc.navigate.gotoDriveW",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_W, QObject::tr( "W 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveX, QObject::tr( "탐색" ), "hc.navigate.gotoDriveX",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_X, QObject::tr( "X 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveY, QObject::tr( "탐색" ), "hc.navigate.gotoDriveY",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Y, QObject::tr( "Y 드라이브 루트로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDriveZ, QObject::tr( "탐색" ), "hc.navigate.gotoDriveZ",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Z, QObject::tr( "Z 드라이브 루트로 이동" ) );

DECLARE_CMD( BuiltInCMD_Etc_SwitchPanel,          QObject::tr( "기타" ),    "hc.panel.switch",
             Qt::Key_Tab,                                               QObject::tr( "반대편 패널로 전환" ) );

DECLARE_CMD( BuiltInCMD_Etc_ContextMenu,          QObject::tr( "기타" ),    "hc.file.context",
             Qt::Key_unknown,                                          QObject::tr( "상황메뉴" ) );
DECLARE_CMD( BuiltInCMD_OPs_ShellMenu,            QObject::tr( "동작" ),    "hc.file.shellMenu",
             Qt::ShiftModifier | Qt::Key_Return,                        QObject::tr( "탐색기 메뉴" ) );
DECLARE_CMD( BuiltInCMD_OPs_ShellMenuOnKeypad,    QObject::tr( "동작" ),    "hc.file.shellMenu",
             Qt::ShiftModifier | Qt::KeypadModifier | Qt::Key_Enter,    QObject::tr( "탐색기 메뉴" ) );

DECLARE_CMD( BuiltInCMD_Etc_ExternalMenu,         QObject::tr( "기타" ),    "hc.file.edit",
             Qt::Key_F4,                                                QObject::tr( "문서 편집 · 프로그램 선택" ) );

DECLARE_CMD( BuiltInCMD_OPs_Move,                 QObject::tr( "동작" ),    "hc.file.move",
             Qt::Key_unknown,                                           QObject::tr( "다른 패널로 이동" ) );
DECLARE_CMD( BuiltInCMD_Jobs,                     QObject::tr( "기타" ),    "hc.jobs.show",
             Qt::Key_unknown,                                           QObject::tr( "작업관리자" ) );
DECLARE_CMD( BuiltInCMD_Settings,                 QObject::tr( "기타" ),    "hc.settings.show",
             Qt::ControlModifier | Qt::Key_Comma,                       QObject::tr( "환경 설정" ) );
DECLARE_CMD( BuiltInCMD_ReloadMenu,               QObject::tr( "기타" ),    "hc.menu.reload",
             Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_F12,     QObject::tr( "메뉴 파일 다시 읽기" ) );

DECLARE_CMD( BuiltInCMD_Back,                     QObject::tr( "탐색" ),    "hc.navigate.back",
             Qt::AltModifier | Qt::Key_Left,                            QObject::tr( "방문 기록 뒤로" ) );
DECLARE_CMD( BuiltInCMD_Forward,                  QObject::tr( "탐색" ),    "hc.navigate.forward",
             Qt::AltModifier | Qt::Key_Right,                           QObject::tr( "방문 기록 앞으로" ) );
DECLARE_CMD( BuiltInCMD_FindCurrent,              QObject::tr( "탐색" ),    "hc.search.focus",
             Qt::ControlModifier | Qt::Key_F,                           QObject::tr( "설정 범위에서 찾기" ) );
DECLARE_CMD( BuiltInCMD_FindEverything,           QObject::tr( "탐색" ),    "hc.search.everything",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_F,         QObject::tr( "Everything에서 찾기" ) );
DECLARE_CMD( BuiltInCMD_SearchLocation,           QObject::tr( "탐색" ),    "hc.search.openLocation",
             Qt::Key_unknown,                                          QObject::tr( "검색 결과의 원본 위치 열기" ) );

// 추가 메뉴 명령도 동일한 목록에서 관리한다. 기본 단축키는 지정하지 않는다.
DECLARE_CMD( BuiltInCMD_OPs_Permissions,          QObject::tr( "동작" ),    "hc.file.permissions",
             Qt::Key_unknown,                                           QObject::tr( "선택 항목 권한 확인" ) );
DECLARE_CMD( BuiltInCMD_Iso_Create,               QObject::tr( "동작" ),    "hc.iso.create",
             Qt::Key_unknown,                                           QObject::tr( "선택 항목으로 ISO 생성" ) );
DECLARE_CMD( BuiltInCMD_Iso_Inspect,              QObject::tr( "동작" ),    "hc.iso.inspect",
             Qt::Key_unknown,                                           QObject::tr( "ISO 구조 확인" ) );
DECLARE_CMD( BuiltInCMD_Search_Show,              QObject::tr( "탐색" ),    "hc.search.show",
             Qt::Key_unknown,                                           QObject::tr( "범위 검색" ) );
DECLARE_CMD( BuiltInCMD_Nav_Compatibility,        QObject::tr( "탐색" ),    "hc.navigate.compatibility",
             Qt::Key_unknown,                                           QObject::tr( "호환 탐색 전환" ) );
DECLARE_CMD( BuiltInCMD_Plugin_Wfx_Open,          QObject::tr( "플러그인" ),  "hc.plugin.wfx.open",
             Qt::Key_unknown,                                           QObject::tr( "파일 시스템 열기" ) );
DECLARE_CMD( BuiltInCMD_Plugin_Wdx_Query,         QObject::tr( "플러그인" ),  "hc.plugin.wdx.query",
             Qt::Key_unknown,                                           QObject::tr( "선택 파일 정보" ) );
DECLARE_CMD( BuiltInCMD_Plugin_Dopus_Open,        QObject::tr( "플러그인" ),  "hc.plugin.dopus.open",
             Qt::Key_unknown,                                           QObject::tr( "DOpus 파일 시스템 열기" ) );
DECLARE_CMD( BuiltInCMD_Plugin_Dopus_Query,       QObject::tr( "플러그인" ),  "hc.plugin.dopus.query",
             Qt::Key_unknown,                                           QObject::tr( "DOpus 선택 파일 정보" ) );
DECLARE_CMD( BuiltInCMD_Automation_Show,          QObject::tr( "기타" ),    "hc.automation.show",
             Qt::Key_unknown,                                           QObject::tr( "자동화 관리" ) );
DECLARE_CMD( BuiltInCMD_Viewer_Close,             QObject::tr( "보기" ),    "hc.viewer.close",
             Qt::Key_unknown,                                           QObject::tr( "미리 보기 닫기" ) );
DECLARE_CMD( BuiltInCMD_Menu_Edit,                QObject::tr( "기타" ),    "hc.menu.edit",
             Qt::Key_unknown,                                           QObject::tr( "메뉴 파일 열기" ) );
DECLARE_CMD( BuiltInCMD_App_Exit,                 QObject::tr( "기타" ),    "hc.app.exit",
             Qt::Key_unknown,                                           QObject::tr( "종료" ) );
DECLARE_CMD( BuiltInCMD_App_About,                QObject::tr( "기타" ),    "hc.app.about",
             Qt::Key_unknown,                                           QObject::tr( "HyperCommander 정보" ) );
DECLARE_CMD( BuiltInCMD_App_Update,               QObject::tr( "기타" ),    "hc.app.update",
             Qt::Key_unknown,                                           QObject::tr( "업데이트 확인" ) );

// 해당 벡터를 순서대로 순회하며 환경설정 대화상자에 표시한다.
inline const QVector<TyHC_COMMAND> GlobalBuiltInCMDs = {
    BuiltInCMD_Tab_New,
    BuiltInCMD_Tab_Prev,
    BuiltInCMD_Tab_Next,
    BuiltInCMD_Tab_Close,

    BuiltInCMD_Src_QuickView,

    BuiltInCMD_OPs_FileCopy,
    BuiltInCMD_OPs_RenameSingleFile,
    BuiltInCMD_OPs_Lister,
    BuiltInCMD_OPs_Return,
    BuiltInCMD_OPs_Enter,
    BuiltInCMD_OPs_MkDir,
    BuiltInCMD_OPs_Delete,
    BuiltInCMD_OPs_DeleteOnKeypad,
    BuiltInCMD_OPs_UnicodeNormalization,
    BuiltInCMD_OPs_SetAttrib,

    BuiltInCMD_Sel_Inverse,
    BuiltInCMD_Sel_Select, BuiltInCMD_Sel_SelectKeypad,
    BuiltInCMD_Sel_Deselect, BuiltInCMD_Sel_DeselectKeypad,
    BuiltInCMD_Sel_All, BuiltInCMD_Sel_Clear, BuiltInCMD_Sel_InvertAll,
    BuiltInCMD_Sel_SameExtension, BuiltInCMD_Sel_ClearExtension,
    BuiltInCMD_Sel_Restore, BuiltInCMD_Sel_Remember, BuiltInCMD_Sel_Recall,
    BuiltInCMD_FolderSize,
    BuiltInCMD_FolderSizeKeypad,

    BuiltInCMD_Archive_Create,
    BuiltInCMD_Archive_Extract,
    BuiltInCMD_EnterFile,
    BuiltInCMD_OPs_NewFile,
    BuiltInCMD_Tool_MultiRenameAlias,
    BuiltInCMD_Tool_MultiRenameFiles,

    BuiltInCMD_View_SwitchHidSys,
    BuiltInCMD_View_RereadSource,

    BuiltInCMD_Nav_FavoriteDir,
    BuiltInCMD_Nav_GotoDrive,
    BuiltInCMD_Nav_GotoDriveA,
    BuiltInCMD_Nav_GotoDriveB,
    BuiltInCMD_Nav_GotoDriveC,
    BuiltInCMD_Nav_GotoDriveD,
    BuiltInCMD_Nav_GotoDriveE,
    BuiltInCMD_Nav_GotoDriveF,
    BuiltInCMD_Nav_GotoDriveG,
    BuiltInCMD_Nav_GotoDriveH,
    BuiltInCMD_Nav_GotoDriveI,
    BuiltInCMD_Nav_GotoDriveJ,
    BuiltInCMD_Nav_GotoDriveK,
    BuiltInCMD_Nav_GotoDriveL,
    BuiltInCMD_Nav_GotoDriveM,
    BuiltInCMD_Nav_GotoDriveN,
    BuiltInCMD_Nav_GotoDriveO,
    BuiltInCMD_Nav_GotoDriveP,
    BuiltInCMD_Nav_GotoDriveQ,
    BuiltInCMD_Nav_GotoDriveR,
    BuiltInCMD_Nav_GotoDriveS,
    BuiltInCMD_Nav_GotoDriveT,
    BuiltInCMD_Nav_GotoDriveU,
    BuiltInCMD_Nav_GotoDriveV,
    BuiltInCMD_Nav_GotoDriveW,
    BuiltInCMD_Nav_GotoDriveX,
    BuiltInCMD_Nav_GotoDriveY,
    BuiltInCMD_Nav_GotoDriveZ,

    BuiltInCMD_Etc_SwitchPanel,
    BuiltInCMD_Etc_ContextMenu,
    BuiltInCMD_OPs_ShellMenu,
    BuiltInCMD_OPs_ShellMenuOnKeypad,
    BuiltInCMD_Etc_ExternalMenu,
    BuiltInCMD_OPs_Move,
    BuiltInCMD_Jobs,
    BuiltInCMD_Settings,
    BuiltInCMD_ReloadMenu,
    BuiltInCMD_Back,
    BuiltInCMD_Forward,
    BuiltInCMD_FindCurrent,
    BuiltInCMD_FindEverything,
    BuiltInCMD_SearchLocation,

    BuiltInCMD_OPs_Permissions,
    BuiltInCMD_Iso_Create,
    BuiltInCMD_Iso_Inspect,
    BuiltInCMD_Search_Show,
    BuiltInCMD_Nav_Compatibility,
    BuiltInCMD_Plugin_Wfx_Open,
    BuiltInCMD_Plugin_Wdx_Query,
    BuiltInCMD_Plugin_Dopus_Open,
    BuiltInCMD_Plugin_Dopus_Query,
    BuiltInCMD_Automation_Show,
    BuiltInCMD_Viewer_Close,
    BuiltInCMD_Menu_Edit,
    BuiltInCMD_App_Exit,
    BuiltInCMD_App_About,
    BuiltInCMD_App_Update,
};

namespace hc::defaults
{
    // 신규 설치용 키 배치와 이전 설치에서 빠진 필드의 호환 기본값.
    inline constexpr auto FreshShortcutPreset    = "prototype";
    inline constexpr auto ExistingShortcutPreset = "legacy";

    // 구식 ID는 저장된 설정·메뉴를 읽는 호환 별칭으로만 사용한다.
    inline QHash<QString, QString> CommandAliases()
    {
        return {
            { "cm_NewTab",               "hc.tab.new" },
            { "cm_PrevTab",              "hc.tab.previous" },
            { "cm_NextTab",              "hc.tab.next" },
            { "cm_CloseTab",             "hc.tab.close" },
            { "cm_SrcQuickView",         "hc.viewer.quick" },
            { "cm_CopyOtherPanel",       "hc.file.copy" },
            { "cm_MoveOtherPanel",       "hc.file.move" },
            { "cm_RenameSingleFile",     "hc.file.rename" },
            { "cm_List",                 "hc.viewer.open" },
            { "cm_Return",               "hc.navigate.open" },
            { "cm_MkDir",                "hc.file.mkdir" },
            { "cm_Delete",               "hc.file.delete" },
            { "cm_NameNormalization",    "hc.file.normalize" },
            { "cm_SetAttrib",            "hc.file.attributes" },
            { "cm_SelInverse",           "hc.selection.invert" },
            { "cm_MultiRenameFiles",     "hc.file.batchRename" },
            { "cm_EnterFile",            "hc.navigate.enterFile" },
            { "cm_PackFiles",            "hc.archive.create" },
            { "cm_UnpackFiles",          "hc.archive.extract" },
            { "cm_NewFile",              "hc.file.new" },
            { "cm_SwitchHidSys",         "hc.view.hidden" },
            { "cm_RereadSource",         "hc.navigate.refresh" },
            { "cm_DirectoryHotList",     "hc.navigate.favorites" },
            { "cm_GotoDrive",            "hc.navigate.path" },
            { "cm_SwitchPanel",          "hc.panel.switch" },
            { "cm_ContextMenu",          "hc.file.context" },
            { "cm_ExternalEditorMenu",   "hc.file.edit" },
            { "cm_NavigateBack",         "hc.navigate.back" },
            { "cm_NavigateForward",      "hc.navigate.forward" },
            { "cm_FindCurrent",          "hc.search.focus" },
            { "cm_ShowJobs",             "hc.jobs.show" },
            { "cm_ShowSettings",         "hc.settings.show" },
            { "cm_ReloadMenu",           "hc.menu.reload" },
            { "cm_ChangePath",           "hc.navigate.path" },
        };
    }

    inline const QVector<TyHC_COMMAND>& Commands()
    {
        return GlobalBuiltInCMDs;
    }

    inline QString CommandId(const QString& Name)
    {
        static const auto Aliases = CommandAliases();
        for (auto It = Aliases.cbegin(); It != Aliases.cend(); ++It)
            if (It.key().compare(Name, Qt::CaseInsensitive) == 0 || It.value().compare(Name, Qt::CaseInsensitive) == 0) return It.value();
        for( const auto& Command : Commands() )
            if( QString( Command.Name ).compare( Name, Qt::CaseInsensitive ) == 0 ) return Command.Name;
        return Name;
    }

    // Convert stored command references without changing keys, disabled bindings,
    // captions or unknown extension commands. Saving the settings persists hc.* IDs.
    inline void MigrateCommandSettings( QJsonObject& Document )
    {
        if( Document["shortcuts"].isObject() )
        {
            auto Shortcuts = Document["shortcuts"].toObject();
            for( auto It = Shortcuts.begin(); It != Shortcuts.end(); ++It )
                if( It.value().isString() ) It.value() = CommandId( It.value().toString() );
            Document["shortcuts"] = Shortcuts;
        }
        if( Document["general"].isObject() )
        {
            auto General = Document["general"].toObject();
            if( General["functionCommands"].isArray() )
            {
                auto Commands = General["functionCommands"].toArray();
                for( int Index = 0; Index < Commands.size(); ++Index )
                    if( Commands[Index].isString() ) Commands[Index] = CommandId( Commands[Index].toString() );
                General["functionCommands"] = Commands;
                Document["general"] = General;
            }
        }
    }

    // 명령 버스가 받는 매개변수·버전·필수 권한. 실행 코드는 CommandUiBridge에 둔다.
    inline std::vector<hc::commands::CommandDescriptor> BusCommands()
    {
        using namespace hc::commands;
        std::vector<CommandDescriptor> Descriptors{ { "hc.navigate.path", 1, { { "path", { ParameterType::String } } } },
                 { "hc.navigate.parent" },
                 { "hc.navigate.refresh" },
                 { "hc.navigate.cancel" },
                 { "hc.navigate.open" },
                 { "hc.navigate.enterFile" },
                 { "hc.navigate.provider",    1, { { "enabled", { ParameterType::Boolean } } } },
                 { "hc.tab.new" },
                 { "hc.tab.close" },
                 { "hc.tab.next" },
                 { "hc.tab.previous" },
                 { "hc.view.columns" },
                 { "hc.viewer.open",          1, { { "requestToken", { ParameterType::String, false } } } },
                 { "hc.file.edit",            1, { { "requestToken", { ParameterType::String, false } } } },
                 { "hc.viewer.quick" },
                 { "hc.viewer.preview" },
                 { "hc.viewer.close" },
                 { "hc.file.copy",
                   1,
                   { { "destination", { ParameterType::String } },
                     { "destinationTab",          { ParameterType::String, false } },
                     { "destinationLocation",     { ParameterType::String, false } },
                     { "conflict",                { ParameterType::String, false } },
                     { "createParents",           { ParameterType::Boolean, false } },
                     { "targetIsName",            { ParameterType::Boolean, false } },
                     { "openCreated",             { ParameterType::Boolean, false } },
                     { "recentProvider",          { ParameterType::String, false } },
                     { "queued",                  { ParameterType::Boolean, false } },
                     { "copyMetadata",            { ParameterType::Boolean, false } },
                     { "copyVerify",              { ParameterType::Boolean, false } },
                     { "copyAcl",                 { ParameterType::Boolean, false } },
                     { "copyStreams",             { ParameterType::Boolean, false } },
                     { "copyPattern",             { ParameterType::String, false } },
                     { "copySpeed",               { ParameterType::Integer, false } } },
                   "file-write" },
                 { "hc.file.move",
                   1,
                   { { "destination", { ParameterType::String } },
                     { "destinationTab",          { ParameterType::String, false } },
                     { "destinationLocation",     { ParameterType::String, false } },
                     { "conflict",                { ParameterType::String, false } },
                     { "createParents",           { ParameterType::Boolean, false } },
                     { "targetIsName",            { ParameterType::Boolean, false } },
                     { "openCreated",             { ParameterType::Boolean, false } },
                     { "recentProvider",          { ParameterType::String, false } },
                     { "queued",                  { ParameterType::Boolean, false } },
                     { "copyMetadata",            { ParameterType::Boolean, false } },
                     { "copyVerify",              { ParameterType::Boolean, false } },
                     { "copyAcl",                 { ParameterType::Boolean, false } },
                     { "copyStreams",             { ParameterType::Boolean, false } },
                     { "copyPattern",             { ParameterType::String, false } },
                     { "copySpeed",               { ParameterType::Integer, false } } },
                   "file-write" },
                 { "hc.file.rename",
                   1,
                   { { "destination", { ParameterType::String } },
                     { "createParents",           { ParameterType::Boolean, false } },
                     { "targetIsName",            { ParameterType::Boolean, false } },
                     { "openCreated",             { ParameterType::Boolean, false } } },
                   "file-write" },
                 { "hc.file.mkdir",
                   1,
                   { { "destination", { ParameterType::String } },
                     { "createParents",           { ParameterType::Boolean, false } },
                     { "targetIsName",            { ParameterType::Boolean, false } },
                     { "openCreated",             { ParameterType::Boolean, false } } },
                   "file-write" },
                 { "hc.file.new",
                   1,
                   { { "name", { ParameterType::String } },
                     { "template",                { ParameterType::Integer, false } },
                     { "bom",                     { ParameterType::Boolean, false } },
                     { "queued",                  { ParameterType::Boolean, false } } },
                   "file-write" },
                 { "hc.file.delete",
                   1,
                   { { "deleteMode", { ParameterType::String, false } },
                     { "allowReadOnly",           { ParameterType::Boolean, false } },
                     { "confirmed",               { ParameterType::Boolean, false } } },
                   "file-write" } };
        for (const auto& Command : Commands())
            if (QString(Command.Name).startsWith("hc.navigate.gotoDrive"))
                Descriptors.push_back({QString(Command.Name).toStdString()});
        return Descriptors;
    }

    // 빈 명령 ID는 해당 키 해제. 사용자 지정 단축키는 이 프리셋 위에 적용한다.
    inline QHash<QKeyCombination, QString> ShortcutPresetChanges( const QString& Preset )
    {
        if( Preset != "prototype" )
            return {};
        return { { QKeyCombination( Qt::ControlModifier, Qt::Key_C ), {} },
                 { QKeyCombination( Qt::ControlModifier, Qt::Key_K ), {} },
                 { QKeyCombination( Qt::Key_F5 ), "hc.file.copy" },
                 { QKeyCombination( Qt::Key_F6 ), "hc.file.move" },
                 { QKeyCombination( Qt::Key_F7 ), "hc.file.mkdir" },
                 { QKeyCombination( Qt::Key_F8 ), "hc.file.delete" },
                 { QKeyCombination( Qt::ControlModifier, Qt::Key_R ), "hc.navigate.refresh" } };
    }
    inline QStringList FunctionCommands()
    {
        return { "hc.file.rename", "hc.viewer.open", "hc.file.edit", "hc.file.copy",       "hc.file.move",
                 "hc.file.mkdir",  "hc.file.delete", "hc.file.new",  "hc.file.batchRename" };
    }
    inline QHash<QString, QString> FunctionCaptions()
    {
        return { { "hc.file.rename", QObject::tr( "이름 변경" ) },
                 { "hc.viewer.open",          QObject::tr( "보기" ) },
                 { "hc.file.edit",            QObject::tr( "편집" ) },
                 { "hc.file.copy",            QObject::tr( "복사" ) },
                 { "hc.file.move",            QObject::tr( "이동" ) },
                 { "hc.file.mkdir",           QObject::tr( "새 폴더" ) },
                 { "hc.file.delete",          QObject::tr( "삭제" ) },
                 { "hc.file.new",             QObject::tr( "새 파일" ) },
                 { "hc.file.batchRename",     QObject::tr( "다중 이름 변경" ) } };
    }

    // 현재 제품은 파일 그룹을 자동 등록하지 않는다. 아래 예제 팔레트는 설정의
    // '예제 추가'에서도 사용한다. FileGroups()의 groups에 이를 지정하면 설치 기본값이 된다.
    inline QJsonArray ExampleFileGroups()
    {
        // 조건 선언: C(Type, Op, Value[, Unit]). 아래 키는 대소문자를 구분한다.
        // Value는 숫자·참/거짓도 문자열로 쓴다. 빈 값은 허용하지 않으며 최대 512자다.
        // Unit은 size와 modified/within에서 필수이고, 나머지 조건에서는 생략한다.
        // 숫자는 0 이상 10진 정수이며, 단위 환산 결과까지 quint64 범위 안이어야 한다.
        // 지원 형식은 FileColorRules::Validate()/Evaluate()의 버전 2 규칙과 같다.
        //
        // Type="extension" (파일만): Op="in"(목록에 포함), "notIn"(목록에 없음).
        //   Value="cpp,hpp,txt"처럼 확장자를 나열한다. 쉼표·세미콜론·|·공백으로 구분하며
        //   대소문자는 무시하고 앞의 점은 생략 가능하다. 무확장자는 "<none>"이다.
        //   확장자는 문자·숫자·_·+·-만 허용한다. *.txt 같은 패턴은 pattern을 사용한다.
        //   파일명의 마지막 확장자를 비교하므로 tar.gz는 "gz"로 지정한다.
        //   예: C("extension", "in", "cpp,hpp,<none>").
        // Type="folder": Op="is", Value="true"(폴더) 또는 "false"(파일).
        //   예: C("folder", "is", "true").
        // Type="normalization": Op="is", Value="decomposed"(자소 분리 있음) 또는 "composed"(NFC).
        //   원래 파일명을 NFC로 조합했을 때 달라지면 decomposed다. NFD 및 NFC/NFD 혼합을
        //   포함하고 영문·숫자처럼 조합 전후가 같은 이름은 composed다. 파일·폴더 모두 적용한다.
        //   기존 FS_FLAG_UNICODE_NFD 판별과 같으며 파일명 자체를 변경하지 않는다.
        //   예: C("normalization", "is", "decomposed").
        // Type="attributes": Op="any"(비트 중 하나 이상), "all"(비트 전부).
        //   Value는 Windows 파일 속성 비트 마스크의 10진 문자열이다.
        //   읽기 전용=1, 숨김=2, 시스템=4, 폴더=16, 보관=32, 재분석 지점=1024,
        //   압축=2048, 암호화=16384. 여러 속성은 비트 OR한 수를 쓴다.
        //   예: C("attributes", "any", "6")은 숨김 또는 시스템(2 | 4),
        //       C("attributes", "all", "6")은 숨김이면서 시스템인 항목이다.
        // Type="size" (파일만): Op="gt"(>), "ge"(>=), "lt"(<), "le"(<=),
        //   "eq"(=), "ne"(!=). Value는 크기, Unit="B"/"KiB"/"MiB"/"GiB".
        //   KiB=1024 B, MiB=1024^2 B, GiB=1024^3 B이며 소수는 허용하지 않는다.
        //   예: C("size", "ge", "100", "MiB"), C("size", "eq", "0", "B").
        // Type="modified": Op="within"(현재 기준 최근 기간, 양 끝 포함).
        //   Value는 기간, Unit="minutes"/"hours"/"days"(1일=24시간).
        //   예: C("modified", "within", "7", "days"). 미래의 수정 시각은 제외한다.
        //   Op="before"(<)/"after"(>)는 Value에 시간대를 포함한 ISO 8601 시각을 쓴다.
        //   예: C("modified", "after", "2026-10-01T00:00:00+09:00").
        //   UTC는 "2026-10-01T00:00:00Z"처럼 쓴다. 기준 시각과 같으면 불일치한다.
        //   수정 시각을 제공하지 않는 항목은 modified 조건에 일치하지 않는다.
        // Type="link": Op="is", Value="shortcut"(.lnk/.url 파일),
        //   "symbolic"(확인된 심볼릭 링크·정션), "any"(둘 중 하나), "none"(링크 아님).
        //   재분석 태그가 불명인 항목은 symbolic/none으로 판정하지 않는다.
        //   예: C("link", "is", "any").
        // Type="pattern": Op="matches"(패턴 중 하나와 일치),
        //   "notMatches"(모든 패턴과 불일치). 파일·폴더 이름 전체를 대소문자 무시로 비교한다.
        //   Value는 경로 없는 와일드카드(*, ?, [abc] 등) 문자열의 QJsonArray이며,
        //   패턴은 각각 1~512자, 배열은 1~32개다. 정규식이나 경로 구분자는 쓰지 않는다.
        //   C는 문자열 전용이므로 pattern은 아래처럼 QJsonObject로 직접 선언한다.
        //   QJsonObject{ { "type", "pattern" }, { "op", "matches" },
        //                { "value", QJsonArray{ "CMakeLists.txt", "*.cmake" } } }
        const auto C = []( const char* Type, const char* Op, const QString& Value, const char* Unit = "" )
        {
            QJsonObject Result{ { "type", Type }, { "op", Op }, { "value", Value } };
            if( *Unit )
                Result[ "unit" ] = Unit;
            return Result;
        };
        QJsonArray Result;
        // 그룹 선언: Add(Id, Name, Conditions, Foreground[, Font[, Background]]).
        // Id는 고유한 접미사이며 저장되는 ID는 "example." + Id다. Name도 중복 없이 지정한다.
        // Conditions는 위 조건 객체를 1~32개 담은 배열이다. 이 Add는 join="any"(OR)로
        // 등록한다. AND가 필요하면 해당 그룹의 join을 "all"로 지정하도록 바꾼다.
        // Foreground/Background는 "#RRGGBB" 또는 "#AARRGGBB" 색상이며, {}는 지정 생략이다.
        // Font는 { { "bold", true }, { "italic", true }, { "underline", true },
        //           { "strike", true } } 중 필요한 효과만 지정한다(false로 명시적 해제 가능).
        // 그룹은 활성화된 예제로 등록되며, 다크 색상은 라이트 색상에서 자동 생성한다.
        // FileGroups()의 mode="first"에서는 먼저 일치한 그룹만 적용하므로 순서도 고려한다.
        // mode="merge"에서는 일치한 그룹을 합치되 색상·글꼴 효과별 첫 지정값이 우선한다.
        // 예: Add("log", QObject::tr("로그"), { C("extension", "in", "log") },
        //         "#667085", { { "italic", true } });
        const auto Add = [ & ]( const char*    Id,
                                const QString& Name,
                                QJsonArray     Conditions,
                                const QString& Foreground,
                                QJsonObject    Font       = {},
                                const QString& Background = {} )
        {
            QJsonObject Light;
            if( !Foreground.isEmpty() )
                Light[ "foreground" ] = Foreground;
            if( !Background.isEmpty() )
                Light[ "background" ] = Background;
            Result.append( QJsonObject{ { "id", QString( "example." ) + Id },
                                        { "name",                    Name },
                                        { "enabled",                 true },
                                        { "example",                 true },
                                        { "join",                    "any" },
                                        { "conditions",              Conditions },
                                        { "light",                   Light },
                                        { "autoDark",                true },
                                        { "font",                    Font } } );
        };
        Add( "hidden", QObject::tr( "숨김 · 시스템" ), { C( "attributes", "any", "6" ) }, "#667085", { { "italic", true } } );
        Add( "executable", QObject::tr( "실행 파일" ), { C( "extension", "in", "exe,msi,cmd,bat,ps1" ) }, "#2563eb" );
        Add( "document", QObject::tr( "문서" ), { C( "extension", "in", "pdf,doc,docx,xls,xlsx,ppt,pptx,hwp,txt" ) }, "#b54738" );
        Add( "media", QObject::tr( "이미지 · 영상" ), { C( "extension", "in", "png,jpg,jpeg,gif,webp,heic,mp4,mkv,mov" ) }, "#087f8c" );
        Add( "archive", QObject::tr( "압축 파일" ), { C( "extension", "in", "zip,7z,rar,tar,gz,zst" ) }, "#7c3aed" );
        Add( "source",
             QObject::tr( "소스 코드" ),
             { C( "extension", "in", "cpp,c,cc,cxx,h,hpp,inl,ixx,ui,qrc" ),
               QJsonObject{ { "type", "pattern" }, { "op", "matches" }, { "value", QJsonArray{ "CMakeLists.txt", "*.cmake" } } } },
             "#9a5b00",
             { { "bold", true } } );
        Add( "link", QObject::tr( "링크 · 바로 가기" ), { C( "link", "is", "any" ) }, {}, { { "underline", true } } );
        Add( "large", QObject::tr( "큰 파일" ), { C( "size", "ge", "1", "GiB" ) }, "#b83280" );
        Add( "empty", QObject::tr( "빈 파일" ), { C( "size", "eq", "0", "B" ) }, {}, { { "strike", true } } );
        Add( "recent", QObject::tr( "최근 24시간에 바뀜" ), { C( "modified", "within", "24", "hours" ) }, {}, {}, "#fff2c6" );
        return Result;
    }

    inline QJsonObject FileGroups()
    {
        // groups: id, name, enabled, join(any/all), conditions, light/dark, autoDark, font.
        // 예: QJsonArray Groups = ExampleFileGroups(); 기존 사용자 배열은 덮어쓰지 않는다.
        const QJsonArray Groups;
        return { { "version", 2 }, { "mode", "first" }, { "groups", Groups } };
    }
    inline QJsonArray LegacyFileSets()
    {
        // name, flags, filters[], extensions[], attributes, size 형식의 구형 파일 집합.
        return {};
    }
    inline QJsonObject LegacyFileSetColors( bool Dark )
    {
        // 집합 이름 -> {foreground: "#RRGGBB", background: "#RRGGBB"}.
        Q_UNUSED( Dark );
        return {};
    }
    inline QJsonObject Appearance()
    {
        QJsonObject Schemes;
        for( const bool Dark : { false, true } )
        {
            const QJsonObject Font{ { "family", "Malgun Gothic" }, { "pointSize", 11 } };
            const QJsonObject ListFont{ { "family", "Malgun Gothic" }, { "pointSize", 12 } };
            Schemes[ Dark ? QStringLiteral( "기본 다크" ) : QStringLiteral( "기본 라이트" ) ] =
                QJsonObject{ { "dark", Dark },
                             { "inverseCursor",           false },
                             { "inverseSelection",        false },
                             { "fonts",                   QJsonObject{ { "menu", Font }, { "dialog", Font }, { "list", ListFont } } },
                             { "colors",
                               QJsonObject{ { "dialogText", Dark ? "#E8EDF5" : "#20242B" },
                                            { "dialogBackground",        Dark ? "#20252E" : "#F4F6FA" },
                                            { "listText",                Dark ? "#E8EDF5" : "#20242B" },
                                            { "listBackground",          Dark ? "#181C23" : "#FFFFFF" },
                                            { "listCursor",              Dark ? "#34445C" : "#E0EBFF" },
                                            { "listSelection",           Dark ? "#405575" : "#C9D9F1" } } },
                             { "fileSets",                LegacyFileSetColors( Dark ) } };
        }
        return { { "selected", QStringLiteral( "기본 라이트" ) }, { "schemes", Schemes } };
    }
    inline QJsonObject FilePanel()
    {
        return { { "version", 1 },
                 { "defaultMode",             "one-line" },
                 { "oneLineSeparator",        "none" },
                 { "twoLineSeparator",        "line" },
                 { "nameBelow",               false },
                 { "autoWidthDip",            640 },
                 { "autoClippedPercent",      25 },
                 { "density",                 "normal" },
                 { "nameElide",               "keep-extension" } };
    }
    inline QJsonObject Thumbnails()
    {
        return { { "version", 1 },     { "size", 96 },          { "nameLines", 2 },   { "info", 1 },    { "fill", false },
                 { "badges",                  true },   { "method", 2 },         { "targets", 15 },    { "threads", 2 }, { "cacheMb", 16 },
                 { "skipSlow",                true }, { "autoSwitch", false }, { "autoPercent", 70 } };
    }
    inline QJsonObject General( bool ExistingInstallation )
    {
        return { { "sizeInHeader", "adaptive2" },
                 { "sizeInFooter",            "adaptive2" },
                 { "useBuiltInShortcuts",     true },
                 { "shortcutPreset",          ExistingInstallation ? ExistingShortcutPreset : FreshShortcutPreset },
                 { "iconMode",                "standard" },
                 { "dimHiddenIcons",          true },
                 { "specialFolderIcons",      true },
                 { "virtualFolderIcons",      true },
                 { "explorerDefaultIcons",    true },
                 { "iconOverlays",            true },
                 { "sortCaseSensitive",       false },
                 { "sortLocaleAware",         true },
                 { "sortNumericAware",        true },
                 { "directoryStyle",          "plain" },
                 { "focusPreviousDirectory",  true },
                 { "quickFind",               hc::quickfind::Settings{}.ToJson() },
                 { "refresh",                 hc::refresh::Settings{}.ToJson() },
                 { "darkTitleBar",            true },
                 { "showCommandLine",         true },
                 { "showFunctionBar",         true },
                 { "restoreWorkspace",        true },
                 { "singleInstance",          false },
                 { "newTabPosition",          "last" },
                 { "monoFontFamily",          "Cascadia Mono" },
                 { "monoFontPx",              12 },
                 // 빈 themeMode는 저장된 선택 색상표의 light/dark를 따른다(기존 설치 호환).
                 { "themeMode",               "" },
                 { "functionCommands",        QJsonArray::fromStringList( FunctionCommands() ) },
                 { "functionCaptions",        QJsonArray{} },
                 { "filePanel",               FilePanel() },
                 { "thumbnails",              Thumbnails() } };
    }
    inline QJsonObject Options()
    {
        return { { "allowElevation", true },
                 { "fileConflict",            "ask" },
                 { "renameInDialog",          true },
                 { "renameSelectExtension",   false },
                 { "renameOnSlowClick",       true },
                 { "folderSizeOnSpace",       true },
                 { "folderSizeBeforeOperations", true },
                 { "folderSizeUseEverything", true },
                 { "folderSizeMode",          "everything" },
                 { "copyMetadata",            true },
                 { "copyVerify",              false },
                 { "copyAcl",                 false },
                 { "copyStreams",             false },
                 { "copyPattern",             "*" },
                 { "copySpeedMiB",            0 },
                 { "delayedFileWrites",       true },
                 { "jobDetailed",             false },
                 { "jobCloseOnSuccess",       true },
                 { "deleteToRecycle",         true },
                 { "deleteWhileListing",      true },
                 { "jobNotification",         "attention" },
                 { "queueInsertion",          "last" },
                 { "jobConcurrency",          2 },
                 { "enableJobRecovery",       false },
                 { "saveJobProgress",         false },
                 { "showJobDetails",          false },
                 { "fileColorRules",          FileGroups() } };
    }
    inline QJsonObject DocumentHandlers()
    {
        return { { "schemaVersion", 1 },
                 { "handlers",                QJsonArray{} },
                 { "defaultViewHandlerId",    "builtin.hyperviewer" },
                 { "defaultEditHandlerId",    "builtin.hypereditor" },
                 { "selectionPolicy",         "askWhenMultiple" },
                 { "fileTypeOverrides",       QJsonObject{} } };
    }
    inline QJsonObject Settings( bool ExistingInstallation = false )
    {
        return { { "schemaVersion", 1 },
                 { "general",                 General( ExistingInstallation ) },
                 { "appearance",              Appearance() },
                 // shortcuts는 사용자 차이만 저장한다. 제품 키는 위 명령·프리셋에서 설정한다.
                 { "shortcuts",               QJsonObject{} },
                 { "favorites",               QJsonObject{ { "drivesOn", "none" }, { "items", QJsonArray{} } } },
                 { "fileSets",                LegacyFileSets() },
                 { "editors",                 QJsonObject{ { "items", QJsonArray{} }, { "documentHandlers", DocumentHandlers() } } },
                 { "plugins",
                   QJsonObject{ { "viewerSelection", "manual" },
                                { "wlx",                     QJsonArray{} },
                                { "wcx",                     QJsonArray{} },
                                { "wfx",                     QJsonArray{} },
                                { "wdx",                     QJsonArray{} },
                                { "dopusVfs",                QJsonArray{} },
                                { "dopusViewer",             QJsonArray{} } } },
                 { "columns",                 QJsonObject{} },
                 { "automation",              QJsonObject{ { "version", 2 }, { "scripts", QJsonArray{} } } },
                 { "options",                 Options() } };
    }
} // namespace hc::defaults
