#pragma once
#include "commonLibs/columns/columnContracts.hpp"

// 제품 기본값의 편집 지점. 이 헤더를 고친 뒤 다시 빌드하면 새 설치에 적용된다.
// settings.json에 저장된 값(빈 배열·단축키 해제 포함)은 기본값보다 우선한다.
// 명령 ID는 hc.*로 통일한다. 이전 설정의 cm_* ID는 CommandAliases()에서 변환한다.
// 새 명령은 이 목록 외에 실행 핸들러도 구현해야 한다.

#include <QtCore>
#include "commonLibs/commands/commandBus.hpp"

struct TyOpts
{
    // UI
    QString                             Cate;
    QString                             Desc;
    QString                             Tooltip;

    // Settings
    QString                             Section;        // Application option category
    QString                             Key;
    QString                             ValueType;
    QString                             DefaultValue;

    // Etc
    unsigned int                        Build = 0;
};

Q_DECLARE_METATYPE( TyOpts );

// 변수명, OPT_카테고리_키

const TyOpts OPT_COLOR_SCHEME_COUNT = { "", "", "",
                                        "ColorScheme", "", "", "", 1 };

const TyOpts OPT_COLORS_LISTFONT  = { QObject::tr( "색상" ), QObject::tr( "글꼴" ), QObject::tr( "파일 목록의 글꼴" ),
                                      "Colors", "FileListFontName", "FontName", "D2Coding ligature",
                                      1 };

const TyOpts OPT_COLORS_LISTFONTSIZE = { QObject::tr( "색상" ), QObject::tr( "글꼴 크기" ), QObject::tr( "파일 목록의 글꼴 크기" ),
                                      "Colors", "FileListFontSize", "int", "16",
                                      1 };

const TyOpts OPT_COLORS_FORECOLOR = { QObject::tr( "색상" ), QObject::tr( "글꼴 색상" ), QObject::tr( "파일 목록의 글꼴 색상" ),
                                      "Colors", "FileListForeColor", "Color(RGB)", "#DFDFDF",
                                      1 };
const TyOpts OPT_COLORS_BACKCOLOR = { QObject::tr( "색상" ), QObject::tr( "배경 색상" ), QObject::tr( "파일 목록의 배경 색상" ),
                                      "Colors", "FileListBackColor", "Color(RGB)", "#1F1F1F",
                                      1 };


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

// Enter/Shift+Enter are command-edit defaults, independent of panel shortcuts.
DECLARE_CMD( BuiltInCMD_CommandLine_Execute, QObject::tr("명령행"), "hc.commandLine.execute",
             Qt::Key_Return, QObject::tr("명령행 입력 실행") );
DECLARE_CMD( BuiltInCMD_CommandLine_KeepOpen, QObject::tr("명령행"), "hc.commandLine.keepOpen",
             Qt::ShiftModifier | Qt::Key_Return, QObject::tr("명령행 입력 실행 후 창 유지") );
DECLARE_CMD( BuiltInCMD_CommandLine_Activate, QObject::tr("명령행"), "hc.commandLine.activate",
             Qt::ControlModifier | Qt::Key_Slash, QObject::tr("명령행 입력 활성화") );

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

DECLARE_CMD( BuiltInCMD_Clipboard_Cut, QObject::tr("클립보드"), "hc.clipboard.cut",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_X, QObject::tr("잘라내기") );
DECLARE_CMD( BuiltInCMD_Clipboard_Copy, QObject::tr("클립보드"), "hc.clipboard.copy",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_C, QObject::tr("복사") );
DECLARE_CMD( BuiltInCMD_Clipboard_Paste, QObject::tr("클립보드"), "hc.clipboard.paste",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_V, QObject::tr("붙여넣기") );

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

DECLARE_CMD( BuiltInCMD_View_ForceRefresh, QObject::tr( "보기" ), "hc.navigate.forceRefresh",
             Qt::ControlModifier | Qt::Key_F5, QObject::tr( "아이콘을 포함하여 강제 새로 고침" ) );

DECLARE_CMD( BuiltInCMD_Nav_FavoriteDir,          QObject::tr( "탐색" ),    "hc.navigate.favorites",
             Qt::ControlModifier | Qt::Key_G,                           QObject::tr( "즐겨찾기 폴더 목록" ) );
DECLARE_CMD( BuiltInCMD_Nav_GotoDrive,            QObject::tr( "탐색" ),    "hc.navigate.path",
             Qt::Key_F12,                                               QObject::tr( "드라이브 전환" ) );

DECLARE_CMD( BuiltInCMD_Nav_SystemProcesses,      QObject::tr( "탐색" ),    "hc.navigate.systemProcesses",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_P,           QObject::tr( "시스템 도구: 프로세스로 이동" ) );
DECLARE_CMD( BuiltInCMD_Nav_SystemServices,       QObject::tr( "탐색" ),    "hc.navigate.systemServices",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_S,           QObject::tr( "시스템 도구: 서비스로 이동" ) );

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

DECLARE_CMD( BuiltInCMD_File_Compare,            QObject::tr( "동작" ),    "hc.file.compare",
             Qt::ControlModifier | Qt::Key_O,                           QObject::tr( "파일·폴더 비교" ) );
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

// 제목표시줄 오디오 미리 듣기와 재생 제어의 제품 기본 단축키.
DECLARE_CMD( BuiltInCMD_Audio_Preview,            QObject::tr( "오디오" ),  "hc.audio.preview",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_Up,         QObject::tr( "오디오 미리 듣기" ) );
DECLARE_CMD( BuiltInCMD_Audio_PreviewKeypad,      QObject::tr( "오디오" ),  "hc.audio.preview",
             Qt::Key_unknown,                                           QObject::tr( "오디오 미리 듣기" ) );
DECLARE_CMD( BuiltInCMD_Audio_PlayPause,          QObject::tr( "오디오" ),  "hc.audio.playPause",
             Qt::Key_unknown,                                           QObject::tr( "재생/일시정지" ) );
DECLARE_CMD( BuiltInCMD_Audio_Stop,               QObject::tr( "오디오" ),  "hc.audio.stop",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_Down,       QObject::tr( "정지" ) );
DECLARE_CMD( BuiltInCMD_Audio_Next,               QObject::tr( "오디오" ),  "hc.audio.next",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_Right,       QObject::tr( "다음 곡" ) );
DECLARE_CMD( BuiltInCMD_Audio_Prev,               QObject::tr( "오디오" ),  "hc.audio.prev",
             Qt::ControlModifier | Qt::AltModifier | Qt::Key_Left,       QObject::tr( "이전 곡" ) );
DECLARE_CMD( BuiltInCMD_Audio_SeekForward,        QObject::tr( "오디오" ),  "hc.audio.seekForward",
             Qt::Key_unknown,                                           QObject::tr( "5초 앞으로" ) );
DECLARE_CMD( BuiltInCMD_Audio_SeekBack,           QObject::tr( "오디오" ),  "hc.audio.seekBack",
             Qt::Key_unknown,                                           QObject::tr( "5초 뒤로" ) );
DECLARE_CMD( BuiltInCMD_Audio_Locate,             QObject::tr( "오디오" ),  "hc.audio.locate",
             Qt::Key_unknown,                                           QObject::tr( "재생 중인 파일 위치 보기" ) );

// 프로세스·서비스 보기. hc.process.*·hc.service.*의 기본 키는 전역 맵이 아니라 보기 입력 범위
// (processView·serviceView)에 둔다. 범위별 기본 키는 hc::defaults::ScopeShortcutDefaults()다.
DECLARE_CMD( BuiltInCMD_System_Processes,         QObject::tr( "시스템" ),  "hc.system.processes.open",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 보기 열기" ) );
DECLARE_CMD( BuiltInCMD_System_Services,          QObject::tr( "시스템" ),  "hc.system.services.open",
             Qt::Key_unknown,                                           QObject::tr( "서비스 보기 열기" ) );
DECLARE_CMD( BuiltInCMD_System_AdminHelper,       QObject::tr( "시스템" ),  "hc.system.startAdminHelper",
             Qt::Key_unknown,                                           QObject::tr( "관리자 도우미 미리 시작" ) );
DECLARE_CMD( BuiltInCMD_Panel_LocationLeft,       QObject::tr( "탐색" ),    "hc.panel.locationMenuLeft",
             Qt::AltModifier | Qt::Key_F1,                              QObject::tr( "왼쪽 패널 위치 메뉴" ) );
DECLARE_CMD( BuiltInCMD_Panel_LocationRight,      QObject::tr( "탐색" ),    "hc.panel.locationMenuRight",
             Qt::AltModifier | Qt::Key_F2,                              QObject::tr( "오른쪽 패널 위치 메뉴" ) );
DECLARE_CMD( BuiltInCMD_System_RefreshPause,      QObject::tr( "시스템" ),  "hc.system.refreshPause",
             Qt::Key_unknown,                                           QObject::tr( "갱신 일시 정지·재개" ) );
DECLARE_CMD( BuiltInCMD_System_Follow,            QObject::tr( "시스템" ),  "hc.system.followLocation",
             Qt::Key_unknown,                                           QObject::tr( "파일 위치 따라가기" ) );
DECLARE_CMD( BuiltInCMD_Process_Properties,       QObject::tr( "프로세스" ), "hc.process.properties",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 속성" ) );
DECLARE_CMD( BuiltInCMD_Process_Location,         QObject::tr( "프로세스" ), "hc.process.openLocation",
             Qt::Key_unknown,                                           QObject::tr( "파일 위치 열기(반대 패널)" ) );
DECLARE_CMD( BuiltInCMD_Process_LocationTab,      QObject::tr( "프로세스" ), "hc.process.openLocationNewTab",
             Qt::Key_unknown,                                           QObject::tr( "새 탭에서 파일 위치 열기" ) );
DECLARE_CMD( BuiltInCMD_Process_Window,           QObject::tr( "프로세스" ), "hc.process.switchToWindow",
             Qt::Key_unknown,                                           QObject::tr( "창으로 전환" ) );
DECLARE_CMD( BuiltInCMD_Process_Suspend,          QObject::tr( "프로세스" ), "hc.process.suspendToggle",
             Qt::Key_unknown,                                           QObject::tr( "일시 중단·재개" ) );
DECLARE_CMD( BuiltInCMD_Process_Priority,         QObject::tr( "프로세스" ), "hc.process.priority",
             Qt::Key_unknown,                                           QObject::tr( "우선순위 메뉴" ) );
DECLARE_CMD( BuiltInCMD_Process_Efficiency,       QObject::tr( "프로세스" ), "hc.process.efficiencyMode",
             Qt::Key_unknown,                                           QObject::tr( "효율 모드 전환" ) );
DECLARE_CMD( BuiltInCMD_Process_Run,              QObject::tr( "프로세스" ), "hc.process.run",
             Qt::Key_unknown,                                           QObject::tr( "새 작업 실행" ) );
DECLARE_CMD( BuiltInCMD_Process_Terminate,        QObject::tr( "프로세스" ), "hc.process.terminate",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 끝내기" ) );
DECLARE_CMD( BuiltInCMD_Process_TerminateTree,    QObject::tr( "프로세스" ), "hc.process.terminateTree",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 트리 끝내기" ) );
DECLARE_CMD( BuiltInCMD_Process_Restart,          QObject::tr( "프로세스" ), "hc.process.restart",
             Qt::Key_unknown,                                           QObject::tr( "다시 실행" ) );
DECLARE_CMD( BuiltInCMD_Process_Copy,             QObject::tr( "프로세스" ), "hc.process.copy",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 정보 복사" ) );
DECLARE_CMD( BuiltInCMD_Process_Search,           QObject::tr( "프로세스" ), "hc.process.searchOnline",
             Qt::Key_unknown,                                           QObject::tr( "온라인에서 검색" ) );
DECLARE_CMD( BuiltInCMD_Process_Menu,             QObject::tr( "프로세스" ), "hc.process.contextMenu",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 바로 가기 메뉴" ) );
DECLARE_CMD( BuiltInCMD_Process_ViewMode,         QObject::tr( "프로세스" ), "hc.process.viewMode",
             Qt::Key_unknown,                                           QObject::tr( "트리·목록 전환" ) );
DECLARE_CMD( BuiltInCMD_Process_Services,         QObject::tr( "프로세스" ), "hc.process.showServices",
             Qt::Key_unknown,                                           QObject::tr( "관련 서비스 보기" ) );
DECLARE_CMD( BuiltInCMD_Service_Properties,       QObject::tr( "서비스" ),  "hc.service.properties",
             Qt::Key_unknown,                                           QObject::tr( "서비스 속성" ) );
DECLARE_CMD( BuiltInCMD_Service_Location,         QObject::tr( "서비스" ),  "hc.service.openLocation",
             Qt::Key_unknown,                                           QObject::tr( "실행 파일 위치(반대 패널)" ) );
DECLARE_CMD( BuiltInCMD_Service_LocationTab,      QObject::tr( "서비스" ),  "hc.service.openLocationNewTab",
             Qt::Key_unknown,                                           QObject::tr( "새 탭에서 실행 파일 위치 열기" ) );
DECLARE_CMD( BuiltInCMD_Service_Start,            QObject::tr( "서비스" ),  "hc.service.start",
             Qt::Key_unknown,                                           QObject::tr( "서비스 시작" ) );
DECLARE_CMD( BuiltInCMD_Service_Stop,             QObject::tr( "서비스" ),  "hc.service.stop",
             Qt::Key_unknown,                                           QObject::tr( "서비스 중지" ) );
DECLARE_CMD( BuiltInCMD_Service_Restart,          QObject::tr( "서비스" ),  "hc.service.restart",
             Qt::Key_unknown,                                           QObject::tr( "서비스 다시 시작" ) );
DECLARE_CMD( BuiltInCMD_Service_Pause,            QObject::tr( "서비스" ),  "hc.service.pauseToggle",
             Qt::Key_unknown,                                           QObject::tr( "서비스 일시 중지·계속" ) );
DECLARE_CMD( BuiltInCMD_Service_StartType,        QObject::tr( "서비스" ),  "hc.service.startType",
             Qt::Key_unknown,                                           QObject::tr( "시작 유형 메뉴" ) );
DECLARE_CMD( BuiltInCMD_Service_Process,          QObject::tr( "서비스" ),  "hc.service.showProcess",
             Qt::Key_unknown,                                           QObject::tr( "프로세스 보기로 이동" ) );
DECLARE_CMD( BuiltInCMD_Service_Drivers,          QObject::tr( "서비스" ),  "hc.service.toggleDrivers",
             Qt::Key_unknown,                                           QObject::tr( "드라이버 포함 전환" ) );
DECLARE_CMD( BuiltInCMD_Service_Copy,             QObject::tr( "서비스" ),  "hc.service.copy",
             Qt::Key_unknown,                                           QObject::tr( "서비스 정보 복사" ) );
DECLARE_CMD( BuiltInCMD_Service_Menu,             QObject::tr( "서비스" ),  "hc.service.contextMenu",
             Qt::Key_unknown,                                           QObject::tr( "서비스 바로 가기 메뉴" ) );

// 해당 벡터를 순서대로 순회하며 환경설정 대화상자에 표시한다.
inline const QVector<TyHC_COMMAND> GlobalBuiltInCMDs = {
    BuiltInCMD_Tab_New,
    BuiltInCMD_Tab_Prev,
    BuiltInCMD_Tab_Next,
    BuiltInCMD_Tab_Close,

    BuiltInCMD_Src_QuickView,

    BuiltInCMD_Clipboard_Cut,
    BuiltInCMD_Clipboard_Copy,
    BuiltInCMD_Clipboard_Paste,
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
    BuiltInCMD_View_ForceRefresh,

    BuiltInCMD_Nav_FavoriteDir,
    BuiltInCMD_Nav_GotoDrive,
    BuiltInCMD_Nav_SystemProcesses,
    BuiltInCMD_Nav_SystemServices,
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
    BuiltInCMD_File_Compare,
    BuiltInCMD_CommandLine_Execute,
    BuiltInCMD_CommandLine_KeepOpen,
    BuiltInCMD_CommandLine_Activate,
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
    BuiltInCMD_Audio_Preview,
    BuiltInCMD_Audio_PreviewKeypad,
    BuiltInCMD_Audio_PlayPause,
    BuiltInCMD_Audio_Stop,
    BuiltInCMD_Audio_Next,
    BuiltInCMD_Audio_Prev,
    BuiltInCMD_Audio_SeekForward,
    BuiltInCMD_Audio_SeekBack,
    BuiltInCMD_Audio_Locate,
    BuiltInCMD_System_Processes,
    BuiltInCMD_System_Services,
    BuiltInCMD_System_AdminHelper,
    BuiltInCMD_Panel_LocationLeft,
    BuiltInCMD_Panel_LocationRight,
    BuiltInCMD_System_RefreshPause,
    BuiltInCMD_System_Follow,
    BuiltInCMD_Process_Properties,
    BuiltInCMD_Process_Location,
    BuiltInCMD_Process_LocationTab,
    BuiltInCMD_Process_Window,
    BuiltInCMD_Process_Suspend,
    BuiltInCMD_Process_Priority,
    BuiltInCMD_Process_Efficiency,
    BuiltInCMD_Process_Run,
    BuiltInCMD_Process_Terminate,
    BuiltInCMD_Process_TerminateTree,
    BuiltInCMD_Process_Restart,
    BuiltInCMD_Process_Copy,
    BuiltInCMD_Process_Search,
    BuiltInCMD_Process_Menu,
    BuiltInCMD_Process_ViewMode,
    BuiltInCMD_Process_Services,
    BuiltInCMD_Service_Properties,
    BuiltInCMD_Service_Location,
    BuiltInCMD_Service_LocationTab,
    BuiltInCMD_Service_Start,
    BuiltInCMD_Service_Stop,
    BuiltInCMD_Service_Restart,
    BuiltInCMD_Service_Pause,
    BuiltInCMD_Service_StartType,
    BuiltInCMD_Service_Process,
    BuiltInCMD_Service_Drivers,
    BuiltInCMD_Service_Copy,
    BuiltInCMD_Service_Menu,
};

namespace hc::defaults
{
    // 신규 설치용 키 배치와 이전 설치에서 빠진 필드의 호환 기본값.
    inline constexpr auto FreshShortcutPreset    = "product";
    inline constexpr auto ExistingShortcutPreset = "legacy";
    inline QStringList ShortcutPresetIds() { return { "legacy", "prototype", "product" }; }

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
                 { "hc.navigate.forceRefresh" },
                 { "hc.navigate.cancel" },
                 { "hc.navigate.open" },
                 { "hc.navigate.enterFile" },
                 { "hc.navigate.systemProcesses" },
                 { "hc.navigate.systemServices" },
                 { "hc.navigate.provider",    1, { { "enabled", { ParameterType::Boolean } } } },
                 { "hc.tab.new" },
                 { "hc.tab.close" },
                 { "hc.tab.next" },
                 { "hc.tab.previous" },
                 { "hc.view.columns" },
                 { "hc.viewer.open",          1, { { "requestToken", { ParameterType::String, false } } } },
                 { "hc.file.edit",            1, { { "requestToken", { ParameterType::String, false } } } },
                 { "hc.file.compare" },
                 { "hc.clipboard.cut" },
                 { "hc.clipboard.copy" },
                 { "hc.clipboard.paste", 1, {}, "file-write" },
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
        // Keep saved legacy/prototype bindings stable when fresh-install keys change.
        if( Preset == "product" )
            return { { QKeyCombination( Qt::ControlModifier, Qt::Key_C ), "hc.file.copy" },
                     { QKeyCombination( Qt::Key_F2 ), "hc.file.rename" },
                     { QKeyCombination( Qt::ControlModifier, Qt::Key_R ), "hc.file.rename" },
                     { QKeyCombination( Qt::ControlModifier, Qt::Key_M ), "hc.file.move" },
                     { QKeyCombination( Qt::ControlModifier | Qt::AltModifier, Qt::Key_R ), "hc.file.batchRename" },
                     { QKeyCombination( Qt::ControlModifier, Qt::Key_K ), "hc.file.mkdir" },
                     { QKeyCombination( Qt::ControlModifier, Qt::Key_A ), "hc.archive.create" },
                     { QKeyCombination( Qt::ControlModifier, Qt::Key_X ), "hc.archive.extract" } };
        if( Preset != "prototype" )
            return {};
        return { { QKeyCombination( Qt::ControlModifier, Qt::Key_C ), {} },
                 { QKeyCombination( Qt::ControlModifier, Qt::Key_K ), {} },
                 { QKeyCombination( Qt::Key_F5 ), "hc.navigate.refresh" },
                 { QKeyCombination( Qt::Key_F6 ), "hc.file.move" },
                 { QKeyCombination( Qt::Key_F7 ), "hc.file.mkdir" },
                 { QKeyCombination( Qt::Key_F8 ), "hc.file.delete" },
                 { QKeyCombination( Qt::ControlModifier, Qt::Key_R ), "hc.file.rename" } };
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
                 { "hc.file.batchRename",     QObject::tr( "다중 이름 변경" ) },
                 { "hc.navigate.refresh",     QObject::tr( "새로 고침" ) },
                 { "hc.viewer.quick",         QObject::tr( "빠른 보기" ) },
                 { "hc.process.properties",   QObject::tr( "속성" ) },
                 { "hc.process.openLocation", QObject::tr( "파일 위치" ) },
                 { "hc.process.priority",     QObject::tr( "우선순위" ) },
                 { "hc.process.run",          QObject::tr( "새 작업" ) },
                 { "hc.process.terminate",    QObject::tr( "끝내기" ) },
                 { "hc.process.terminateTree", QObject::tr( "트리 끝내기" ) },
                 { "hc.process.suspendToggle", QObject::tr( "일시 중단" ) },
                 { "hc.service.properties",   QObject::tr( "속성" ) },
                 { "hc.service.openLocation", QObject::tr( "파일 위치" ) },
                 { "hc.service.restart",      QObject::tr( "다시 시작" ) },
                 { "hc.service.start",        QObject::tr( "시작" ) },
                 { "hc.service.stop",         QObject::tr( "중지" ) },
                 { "hc.service.pauseToggle",  QObject::tr( "일시 중지" ) },
                 { "hc.service.startType",    QObject::tr( "시작 유형" ) } };
    }

    // 보기별 기능 키 막대. 키 표시는 해당 입력 범위의 키 맵을 따른다.
    inline QStringList ProcessFunctionCommands()
    {
        return { "hc.process.properties", "hc.process.openLocation", "hc.navigate.refresh", "hc.process.priority", "hc.process.run",
                 "hc.process.terminate", "hc.process.terminateTree", "hc.process.suspendToggle", "hc.viewer.quick" };
    }
    inline QStringList ServiceFunctionCommands()
    {
        return { "hc.service.properties", "hc.service.openLocation", "hc.navigate.refresh", "hc.service.restart", "hc.service.start",
                 "hc.service.stop", "hc.service.pauseToggle", "hc.service.startType", "hc.viewer.quick" };
    }

    // 보기 입력 범위의 기본 키(설계 7.5). F5는 제품 전체의 새로 고침이므로 범위에서 덮지 않는다.
    inline QList<QPair<QKeyCombination, QString>> ScopeShortcutDefaults( const QString& Scope )
    {
        using K = QKeyCombination;
        if( Scope == "processView" )
            return { { K( Qt::Key_F3 ), "hc.process.properties" },
                     { K( Qt::Key_F4 ), "hc.process.openLocation" },
                     { K( Qt::Key_F6 ), "hc.process.priority" },
                     { K( Qt::Key_F7 ), "hc.process.run" },
                     { K( Qt::Key_F8 ), "hc.process.terminate" },
                     { K( Qt::Key_Delete ), "hc.process.terminate" },
                     { K( Qt::KeypadModifier, Qt::Key_Delete ), "hc.process.terminate" },
                     { K( Qt::ShiftModifier, Qt::Key_F8 ), "hc.process.terminateTree" },
                     { K( Qt::ShiftModifier, Qt::Key_Delete ), "hc.process.terminateTree" },
                     { K( Qt::Key_F9 ), "hc.process.suspendToggle" },
                     { K( Qt::ShiftModifier, Qt::Key_Return ), "hc.process.contextMenu" },
                     { K( Qt::ControlModifier, Qt::Key_Return ), "hc.process.switchToWindow" },
                     { K( Qt::Key_Pause ), "hc.system.refreshPause" } };
        if( Scope == "serviceView" )
            return { { K( Qt::Key_F3 ), "hc.service.properties" },
                     { K( Qt::Key_F4 ), "hc.service.openLocation" },
                     { K( Qt::Key_F6 ), "hc.service.restart" },
                     { K( Qt::Key_F7 ), "hc.service.start" },
                     { K( Qt::Key_F8 ), "hc.service.stop" },
                     { K( Qt::Key_Delete ), "hc.service.stop" },
                     { K( Qt::KeypadModifier, Qt::Key_Delete ), "hc.service.stop" },
                     { K( Qt::Key_F9 ), "hc.service.pauseToggle" },
                     { K( Qt::ShiftModifier, Qt::Key_F4 ), "hc.service.startType" },
                     { K( Qt::ShiftModifier, Qt::Key_Return ), "hc.service.contextMenu" },
                     { K( Qt::ControlModifier, Qt::Key_Return ), "hc.service.showProcess" },
                     { K( Qt::Key_Pause ), "hc.system.refreshPause" } };
        return {};
    }

    // 프로세스·서비스 보기 설정(설계 6.5, 7.9)
    inline QJsonObject ProcessService()
    {
        return { { "version",                       1 },
                 { "refreshMs",                     1000 },
                 { "pauseWhenHidden",               true },
                 { "highlightCycles",               2 },
                 { "confirmSingleTerminate",        true },
                 { "gracefulClose",                 true },
                 { "gracefulWaitSec",               5 },
                 { "cpuMethod",                     "auto" },
                 { "showInterrupts",                true },
                 { "treeLines",                     true },
                 { "followDebounceMs",              300 },
                 { "serviceProblemExcludesDelayed", true },
                 { "helperIdleMinutes",             10 },
                 { "autoStartHelper",               true },
                 { "allUsers",                      true },
                 { "colors",                        QJsonObject{} } };
    }
    inline QJsonObject FunctionBars()
    {
        return { { "processView", QJsonObject{ { "commands", QJsonArray::fromStringList( ProcessFunctionCommands() ) }, { "captions", QJsonArray{} } } },
                 { "serviceView", QJsonObject{ { "commands", QJsonArray::fromStringList( ServiceFunctionCommands() ) }, { "captions", QJsonArray{} } } } };
    }

    // 아래 예제 팔레트는 설정의 '기본 예제 그룹 추가'에서 사용한다.
    // 신규 설치에 등록하는 그룹은 DefaultFileGroups()다.
    inline QJsonArray ExampleFileGroups()
    {
        // 조건 선언: C(Type, Op, Value[, Unit]). 아래 키는 대소문자를 구분한다.
        // Value는 숫자·참/거짓도 문자열로 쓴다. 빈 값은 허용하지 않으며 최대 512자다.
        // Unit은 size와 modified/within에서 필수이고, 나머지 조건에서는 생략한다.
        // 숫자는 0 이상 10진 정수이며, 단위 환산 결과까지 quint64 범위 안이어야 한다.
        // 아래 기본 예제는 버전 2이며, 버전 3 비교 문법은 아래 설명을 참고한다.
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
        // v3에서는 extension/pattern에 syntax="literal"/"wildcard"/"regex"를 지정한다.
        // extension literal: 기존 in/notIn 목록 문자열, pattern literal: contains/notContains 부분 문자열.
        // wildcard: matches/notMatches와 문자열 배열. 전용 glob 비교기로 전체 이름 또는 점 없는 suffix를 비교한다.
        // regex: matches/notMatches와 원문 문자열 하나. 부분 일치이며 전체 비교는 \A...\z를 사용한다.
        // 예: {"type":"extension","syntax":"wildcard","op":"matches","value":["jp*","h??"]}
        // 예: {"type":"pattern","syntax":"regex","op":"matches","value":"\\Areport_\\d+\\.txt\\z"}
        // 정규식의 세미콜론/세로줄/공백/역슬래시는 보존한다. 무확장자는 패턴 방식에서 빈 suffix다.
        // v1/v2 또는 syntax 없는 기존 조건은 아래 의미를 그대로 유지한다.
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
        // mode="first"에서는 먼저 일치한 그룹만 적용하므로 순서도 고려한다.
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

    // 신규 설치에 등록하는 파일 그룹. 병합(merge) 모드에서는 위 그룹이 글자색·배경·글꼴 효과를
    // 효과별로 먼저 가져가므로 순서가 곧 우선순위다. 확장자는 그룹 사이에 겹치지 않는다.
    // 라이트·다크 색을 따로 두며, 글자색은 목록 바탕·교차 행·배경 지정 그룹 위에서 4.5:1 이상이다.
    // 배경은 선택 행과 헷갈리지 않도록 위장 실행파일·인증서·온라인 전용·대용량에만 쓴다.
    inline QJsonArray DefaultFileGroups()
    {
        // 조건 선언은 ExampleFileGroups()와 같다. E는 확장자 목록, P는 파일명 와일드카드 목록이다.
        const auto C = []( const char* Type, const char* Op, const QString& Value, const char* Unit = "" )
        {
            QJsonObject Result{ { "type", Type }, { "op", Op }, { "value", Value } };
            if( *Unit )
                Result[ "unit" ] = Unit;
            return Result;
        };
        const auto E = [ & ]( const char* Extensions ) { return C( "extension", "in", Extensions ); };
        const auto P = []( const QStringList& Patterns )
        { return QJsonObject{ { "type", "pattern" }, { "op", "matches" }, { "value", QJsonArray::fromStringList( Patterns ) } }; };
        // 색 선언: Colors(Foreground[, Background]). ""는 지정 생략이다.
        const auto Colors = []( const char* Foreground, const char* Background = "" )
        {
            QJsonObject Result;
            if( *Foreground )
                Result[ "foreground" ] = Foreground;
            if( *Background )
                Result[ "background" ] = Background;
            return Result;
        };
        QJsonArray Result;
        // 그룹 선언: Add(Id, Name, Conditions, Light, Dark[, Font[, Enabled]]). 저장 ID는 "default." + Id다.
        // 다크 색을 직접 지정하므로 autoDark=false로 등록하고, 조건은 join="any"(OR)로 결합한다.
        const auto Add = [ & ]( const char*        Id,
                                const QString&     Name,
                                QJsonArray         Conditions,
                                const QJsonObject& Light,
                                const QJsonObject& Dark,
                                QJsonObject        Font    = {},
                                bool               Enabled = true )
        {
            Result.append( QJsonObject{ { "id", QString( "default." ) + Id },
                                        { "name",                    Name },
                                        { "enabled",                 Enabled },
                                        { "join",                    "any" },
                                        { "conditions",              Conditions },
                                        { "light",                   Light },
                                        { "dark",                    Dark },
                                        { "autoDark",                false },
                                        { "font",                    Font } } );
        };
        const QJsonObject Bold{ { "bold", true } }, Italic{ { "italic", true } };

        // 문서처럼 보이는 실행 파일: 이중 확장자, 확장자 앞 공백, 글자 방향을 뒤집는 U+202E(RLO).
        // 패턴은 조건당 32개 상한에 맞춘 흔한 조합이다. 맨 위에서 다른 그룹의 표시를 모두 덮는다.
        // U+202E는 RTL 문자 조건으로 잡는다. 이 조건은 아랍어·히브리어 글자가 든 이름도 함께 표시한다.
        Add( "disguisedExecutable",
             QObject::tr( "위장 실행파일" ),
             { P( { "*.pdf.exe", "*.doc.exe", "*.docx.exe", "*.xls.exe", "*.xlsx.exe", "*.ppt.exe", "*.pptx.exe", "*.hwp.exe",
                    "*.hwpx.exe", "*.txt.exe", "*.jpg.exe", "*.png.exe", "*.mp4.exe", "*.zip.exe", "*.pdf.scr", "*.doc.scr",
                    "*.docx.scr", "*.hwp.scr", "*.jpg.scr", "*.txt.scr", "*.pdf.js", "*.pdf.vbs", "*.pdf.lnk", "*.jpg.lnk",
                    "*.pdf.bat", "*.pdf.cmd", "*.pdf.hta", "*.doc.js", "*  .exe", "*  .scr", "*.pdf.com", "*.jpg.com" } ),
               QJsonObject{ { "type", "rtl" }, { "op", "contains" } } },
             Colors( "#B8231A", "#FBE9E7" ),
             Colors( "#FF8A80", "#3A1E1B" ),
             Bold );
        // 다음 다섯 그룹은 글자색·굵게·기울임·배경 중 하나만 써서 종류 색과 함께 보인다.
        Add( "hidden", QObject::tr( "숨김·시스템" ), { C( "attributes", "any", "6" ) }, Colors( "#6C6961" ), Colors( "#97948B" ), { { "dim", true } } );
        Add( "recent", QObject::tr( "최근 변경" ), { C( "modified", "within", "24", "hours" ) }, {}, {}, Bold );
        Add( "link", QObject::tr( "링크·바로가기" ), { C( "link", "is", "any" ) }, {}, {}, Italic );
        // 오프라인(0x1000) · 열 때 회수(0x40000) · 읽을 때 회수(0x400000). OneDrive 등의 자리표시 파일이다.
        Add( "onlineOnly",
             QObject::tr( "온라인 전용 파일" ),
             { C( "attributes", "any", "4460544" ) },
             Colors( "", "#E6EEF9" ),
             Colors( "", "#1E2A3D" ) );
        Add( "large", QObject::tr( "대용량 파일" ), { C( "size", "ge", "1", "GiB" ) }, Colors( "", "#FBEAE6" ), Colors( "", "#3A2320" ) );
        // key는 Keynote가 아니라 개인 키로 본다. .env는 뜻이 넓은 env 확장자 대신 파일명으로 잡는다.
        Add( "certificate",
             QObject::tr( "인증서·키" ),
             { E( "cer,pem,crt,der,csr,pfx,p12,key,jks,p7b,ppk,pgp,gpg,asc,kdbx,keystore,keytab,p7c,p7s,p8,spc" ),
               P( { ".env", ".env.*", "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519" } ) },
             Colors( "#8F4A05", "#FFF3D6" ),
             Colors( "#F2BA52", "#3A3318" ),
             Bold );
        Add( "folder", QObject::tr( "폴더" ), { C( "folder", "is", "true" ) }, Colors( "#C42B1C" ), Colors( "#FF7B72" ) );
        // 마지막 점 뒤를 비교하므로 .gitignore는 gitignore로 잡힌다.
        Add( "document",
             QObject::tr( "문서" ),
             { E( "md,rst,json,csv,xml,yaml,yml,toml,ini,cfg,conf,tsv,jsonl,ndjson,jsonc,json5,xsd,xsl,xslt,plist,inf,"
                  "properties,gitignore,gitattributes,editorconfig,clang-format,clang-tidy,adoc,tex,bib" ) },
             Colors( "#00639A" ),
             Colors( "#5CC3F5" ) );
        Add( "office",
             QObject::tr( "문서(오피스)" ),
             { E( "xls,xlsx,ppt,pptx,hwp,hwpx,doc,docx,pdf,rtf,odt,ods,odp,cell,show,docm,dot,dotx,dotm,xlsm,xlsb,xltx,xltm,"
                  "pptm,potx,pps,ppsx,odg,hwt,xps,oxps,one,vsd,vsdx,mpp,pages,numbers" ) },
             Colors( "#AD4407" ),
             Colors( "#FFA25C" ) );
        // ts는 TypeScript로 본다(MPEG-TS는 동영상의 m2ts·mts).
        Add( "source",
             QObject::tr( "소스코드" ),
             { E( "c,cpp,cxx,cc,h,hpp,hxx,hh,js,py,ts,cs,java,rs,go,lua,qml,ui,qrc,html,css,inl,ipp,ixx,cmake,c++,cppm,m,mm,"
                  "swift,kt,kts,dart,php,rb,pl,r,scala,sql,asm,vue,jsx,tsx,mjs,cjs,scss,sass,less,vb,fs,zig,proto,idl,def,rc,"
                  "natvis,glsl,hlsl,ipynb,vcxproj,filters,props,targets,csproj,vbproj,pro,pri,qbs,gradle,mk,mak,ninja,slnf,"
                  "sln,slnx" ),
               P( { "CMakeLists.txt", "Makefile", "GNUmakefile", "Dockerfile", "meson.build", "BUILD.bazel" } ) },
             Colors( "#1D5BC7" ),
             Colors( "#7AA7FF" ) );
        Add( "text", QObject::tr( "텍스트" ), { E( "txt,log,nfo,diz" ) }, Colors( "#5C5850" ), Colors( "#BDB7A9" ) );
        Add( "executable", QObject::tr( "실행파일" ), { E( "exe,com,scr,cpl,pif,appref-ms" ) }, Colors( "#18793A" ), Colors( "#56D364" ) );
        Add( "installer",
             QObject::tr( "설치 패키지" ),
             { E( "msi,msp,msix,msixbundle,appx,appxbundle,deb,rpm,pkg,apk,aab,vsix,nupkg,whl" ) },
             Colors( "#527000" ),
             Colors( "#B3D35A" ) );
        // reg·hta는 더블클릭으로 시스템을 바꾸므로 문서가 아니라 스크립트에 둔다.
        Add( "script",
             QObject::tr( "스크립트" ),
             { E( "bat,cmd,ps1,psm1,psd1,sh,vbs,wsf,reg,hta,vbe,jse,ahk,au3,nsi,nsh,iss,bash,zsh,fish" ) },
             Colors( "#856000" ),
             Colors( "#E8C14A" ) );
        // jar·war·ear는 실행할 수 있지만 내용이 zip이므로 압축파일에 둔다.
        Add( "archive",
             QObject::tr( "압축파일" ),
             { E( "zip,7z,rar,lzh,gz,tar,tgz,bz2,xz,zst,lz4,z,zipx,cab,alz,egg,arj,lzma,tbz,tbz2,txz,tzst,jar,war,ear,cpio" ) },
             Colors( "#9C27A6" ),
             Colors( "#EE8CF0" ) );
        Add( "diskImage",
             QObject::tr( "디스크 이미지" ),
             { E( "iso,img,vhd,vhdx,vmdk,wim,esd,dmg,vdi,qcow2,nrg,cue,swm" ) },
             Colors( "#6D3FB5" ),
             Colors( "#B99AF5" ) );
        Add( "video",
             QObject::tr( "동영상" ),
             { E( "avi,mp4,mkv,mov,wmv,webm,m4v,flv,mpg,mpeg,m2ts,3gp,ogv,rm,rmvb,asf,vob,divx,f4v,mts" ) },
             Colors( "#00706A" ),
             Colors( "#3DD6C3" ) );
        Add( "subtitle",
             QObject::tr( "자막 파일" ),
             { E( "smi,sami,srt,ass,ssa,vtt,sub,idx,sup,lrc,ttml,dfxp,sbv" ) },
             Colors( "#3A716A" ),
             Colors( "#8FC9C0" ),
             Italic );
        Add( "image",
             QObject::tr( "이미지" ),
             { E( "jpg,jpeg,gif,png,tif,bmp,webp,heic,heif,avif,ico,svg,psd,tiff,jfif,jxl,jp2,apng,dng,cr2,cr3,nef,arw,raf,"
                  "orf,rw2,tga,dds,exr,hdr,xcf,kra,ai,eps,emf,wmf,cur,pcx" ) },
             Colors( "#C2185B" ),
             Colors( "#FF79B0" ) );
        Add( "ebook",
             QObject::tr( "전자책·만화" ),
             { E( "epub,mobi,azw,azw3,kfx,fb2,djvu,cbz,cbr,cb7,cbt" ) },
             Colors( "#8E3B5E" ),
             Colors( "#F0A3C4" ) );
        // mod는 go.mod가 오디오로 잡히므로 넣지 않는다.
        Add( "audio",
             QObject::tr( "오디오" ),
             { E( "mp3,flac,aac,wav,ac3,dts,m4a,ogg,opus,wma,mid,midi,aif,aiff,ape,wv,mka,amr,mp2,caf,dsf,dff,mpc,xm,s3m" ) },
             Colors( "#7A4B1E" ),
             Colors( "#D7A574" ) );
        Add( "font", QObject::tr( "글꼴" ), { E( "ttf,otf,ttc,otc,woff,woff2,fon,fnt,pfb,pfm,eot" ) }, Colors( "#6B5A2E" ), Colors( "#D2BE8C" ) );
        // mdf는 Alcohol 디스크 이미지가 아니라 SQL Server 데이터베이스로 본다.
        Add( "database",
             QObject::tr( "데이터베이스" ),
             { E( "db,db-wal,db-shm,sqlite,sqlite3,mdb,accdb,mdf,ldf,ndf,frm,ibd,sdf,fdb,realm,duckdb,parquet,db-journal,"
                  "sqlite-wal,sqlite-shm" ) },
             Colors( "#4B4FB8" ),
             Colors( "#A3A8FF" ) );
        // 필요할 때 켜도록 꺼 둔다. obj는 빌드 산출물과 겹치므로 넣지 않는다.
        Add( "cad",
             QObject::tr( "3D·CAD" ),
             { E( "stl,3mf,fbx,gltf,glb,blend,dwg,dxf,step,stp,iges,igs,skp,usd,usdz,ply,3ds,dae" ) },
             Colors( "#3B6475" ),
             Colors( "#8CC2D6" ),
             {},
             false );
        // qm은 번역 원본 ts를 컴파일한 결과이므로 여기에 둔다.
        Add( "build",
             QObject::tr( "라이브러리·빌드·진단" ),
             { E( "dll,sys,lib,obj,pdb,ilk,exp,idb,ipch,pch,tlog,iobj,ipdb,res,tlb,winmd,ocx,ax,so,dylib,a,o,class,pyc,pyo,"
                  "qm,dmp,mdmp,hdmp,etl,evtx" ) },
             Colors( "#5F6573" ),
             Colors( "#9AA1AC" ) );
        // 오피스(~$*)·LibreOffice(.~lock.*#) 잠금 파일과 편집기 백업(*~)을 포함한다.
        Add( "temporary",
             QObject::tr( "임시 파일" ),
             { E( "tmp,bak,old,swp,part,crdownload,temp,cache,orig,rej" ), P( { "~$*", "*~", ".~lock.*#" } ) },
             Colors( "#6C6961" ),
             Colors( "#97948B" ),
             Italic );
        // 밑줄만 쓰므로 순서와 상관없이 다른 그룹 표시에 더해진다.
        Add( "normalization", QObject::tr( "파일명 정규화" ), { C( "normalization", "is", "decomposed" ) }, {}, {}, { { "underline", true } } );
        return Result;
    }

    // 신규 설치에만 DefaultFileGroups()를 등록한다. 기존 설치의 누락 키는 예전처럼 빈 문서로
    // 채우며, 저장된 groups 배열은 MergeDefaults가 덮어쓰지 않으므로 사용자의 삭제·편집이 유지된다.
    inline QJsonObject FileGroups( bool ExistingInstallation = false )
    {
        // groups: id, name, enabled, join(any/all), conditions, light/dark, autoDark, font.
        if( ExistingInstallation )
            return { { "version", 2 }, { "mode", "first" }, { "groups", QJsonArray{} } };
        return { { "version", 2 }, { "mode", "merge" }, { "groups", DefaultFileGroups() } };
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
    inline QJsonObject Appearance( bool ExistingInstallation = false )
    {
        QJsonObject Schemes;
        for( const bool Dark : { false, true } )
        {
            const QJsonObject Font{ { "family", "Malgun Gothic" }, { "pointSize", 11 } };
            const QJsonObject ListFont{ { "family", "D2Coding ligature" }, { "pointSize", 16 } };
            Schemes[ Dark ? QStringLiteral( "기본 다크" ) : QStringLiteral( "기본 라이트" ) ] =
                QJsonObject{ { "dark", Dark },
                             { "inverseCursor",           false },
                             { "inverseSelection",        !ExistingInstallation },
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
        return { { "selected", ExistingInstallation ? QStringLiteral( "기본 라이트" ) : QStringLiteral( "기본 다크" ) }, { "schemes", Schemes } };
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
                 { "nameElide",               "keep-extension" },
                 // Field conditions of file color rules and item selection on UNC paths and mapped
                 // network drives: "off", "builtIn" (no plugin fields) or "all".
                 { "networkFieldConditions",  "builtIn" } };
    }
    inline QJsonObject Thumbnails()
    {
        return { { "version", 1 },     { "size", 96 },          { "nameLines", 2 },   { "info", 1 },    { "fill", false },
                 { "badges",                  true },   { "method", 2 },         { "targets", 15 },    { "threads", 2 }, { "cacheMb", 16 },
                 { "skipSlow",                true }, { "autoSwitch", false }, { "autoPercent", 70 } };
    }
    // 제목표시줄 오디오 미리 듣기. 확장자 목록은 audioQueue.hpp의 기본값과 같다.
    inline QJsonObject AudioPreview()
    {
        return { { "version", 1 },
                 { "extensions",              "mp3,m4a,aac,flac,wav,ogg,oga,opus,wma,ape,wv,aif,aiff,mka,mp2,ac3,dts,amr,caf,mpc,dsf,dff" },
                 { "volume",                  70 },
                 { "muted",                   false },
                 { "autoNext",                true },
                 { "repeat",                  "off" },
                 { "shuffle",                 false },
                 { "animateNowPlaying",       true },
                 { "materializeLimitMiB",     1024 } };
    }
    inline QJsonObject DirectoryRefresh()
    {
        return {{"names", true}, {"metadata", true}, {"appendNewEntries", false}, {"pauseInBackground", true},
            {"updateFooter", true}, {"updateHeader", true}, {"excludedDrives", ""},
            {"fallbackEnabled", false}, {"fallbackSeconds", 15}};
    }
    inline QJsonObject QuickFind()
    {
        return {{"enabled", true}, {"highlight", true}, {"everythingKorean", true},
            {"everythingDisplayIntervalMs", 500}, {"activation", "ctrl-alt"}, {"scope", "panel"},
            {"match", "starts"}, {"result", "cursor"}};
    }
    inline QJsonObject General( bool ExistingInstallation )
    {
        return { { "sizeInHeader", "adaptive2" },
                 { "sizeInFooter",            "adaptive2" },
                 { "useBuiltInShortcuts",     true },
                 { "shortcutPreset",          ExistingInstallation ? ExistingShortcutPreset : FreshShortcutPreset },
                 { "iconMode",                ExistingInstallation ? "standard" : "all" },
                 { "specialFolderIcons",      true },
                 { "virtualFolderIcons",      true },
                 { "explorerDefaultIcons",    true },
                 { "iconOverlays",            true },
                 { "sortCaseSensitive",       false },
                 { "sortLocaleAware",         true },
                 { "sortNumericAware",        true },
                 { "directoryStyle",          ExistingInstallation ? "plain" : "brackets" },
                 { "focusPreviousDirectory",  true },
                 { "quickFind",               QuickFind() },
                 { "refresh",                 DirectoryRefresh() },
                 { "darkTitleBar",            true },
                 { "showMainToolbar",         false },
                 { "showCommandLine",         true },
                 { "showFunctionBar",         true },
                 { "restoreWorkspace",        true },
                 { "singleInstance",          false },
                 { "newTabPosition",          "last" },
                 { "monoFontFamily",          "Cascadia Mono" },
                 { "monoFontPx",              12 },
                 // 빈 themeMode는 저장된 선택 색상표의 light/dark를 따른다(기존 설치 호환).
                 { "themeMode",               ExistingInstallation ? "" : "dark" },
                 { "functionCommands",        QJsonArray::fromStringList( FunctionCommands() ) },
                 { "functionCaptions",        QJsonArray{} },
                 { "filePanel",               FilePanel() },
                 { "thumbnails",              Thumbnails() },
                 { "audioPreview",            AudioPreview() },
                 { "functionBars",            FunctionBars() },
                 { "processService",          ProcessService() },
                 // 한 번만 적용하는 사용자 메뉴 파일 이관 표식
                 { "menuMigrations",          QJsonArray{} } };
    }
    inline QJsonObject Options( bool ExistingInstallation = false )
    {
        return { { "allowElevation", true },
                 { "fileConflict",            "ask" },
                 { "renameInDialog",          ExistingInstallation },
                 { "renameSelectExtension",   false },
                 { "renameOnSlowClick",       true },
                 // 켜면 Enter·두 번 클릭으로 오디오를 제목표시줄에서 재생한다. 모든 설치에서 기본으로 끈다.
                 { "audioOpenInternally",     false },
                 { "folderSizeOnSpace",       true },
                 { "folderSizeBeforeOperations", true },
                 { "folderSizeUseEverything", true },
                 { "folderSizeMode",          "everything" },
                 { "copyMetadata",            true },
                 { "copyVerify",              false },
                 { "copyAcl",                 false },
                 { "copyStreams",             !ExistingInstallation },
                 { "copyPattern",             "*" },
                 { "copySpeedMiB",            0 },
                 { "delayedFileWrites",       true },
                 { "jobDetailed",             false },
                 { "jobCloseOnSuccess",       true },
                 { "deleteToRecycle",         ExistingInstallation },
                 { "deleteWhileListing",      true },
                 { "jobNotification",         "attention" },
                 { "queueInsertion",          "last" },
                 { "jobConcurrency",          2 },
                 { "enableJobRecovery",       false },
                 { "saveJobProgress",         false },
                 { "showJobDetails",          false },
                 { "fileColorRules",          FileGroups( ExistingInstallation ) } };
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
                 { "appearance",              Appearance( ExistingInstallation ) },
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
                 { "options",                 Options( ExistingInstallation ) } };
    }
    // File locations and compatibility recovery values are product policy as well.
    inline constexpr auto ConfigFileName = "HyperCommander.json";
    inline constexpr auto LegacySettingsFileName = "settings.json";
    inline constexpr auto LegacyWorkspaceFileName = "workspace.json";
    inline QString ConfigDirectory(const QString& ExecutableDirectory) { return ExecutableDirectory; }
    inline QString UserConfigDirectory() { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation); }
    inline QString LegacyConfigDirectory(const QString& Directory) { return QDir(Directory).filePath("config"); }
    inline QString SettingsFile(const QString& Directory) { return QDir(Directory).filePath(ConfigFileName); }
    inline QJsonObject Policy()
    {
        return {
            {"selection", QJsonObject{{"operation", 0}, {"syntax", 0}, {"scope", 0}, {"pattern", "*"}, {"exclude", ""},
                {"groupId", ""}, {"caseSensitive", false}, {"advanced", false}, {"all", true}, {"size", ""}, {"minimumSize", true}, {"modified", ""}}},
            {"columnAutoSelection", QJsonObject{{"version", 1}, {"enabled", false}, {"rules", QJsonArray{}}}},
            {"columnAutoRule", QJsonObject{{"enabled", true}, {"percent", 50}}},
            {"columnPresentation", QJsonObject{{"listMode", "details"}, {"precision", 1}, {"splitNameExtension", false}, {"providerDisplay", true}}},
            {"update", QJsonObject{{"automaticCheck", true}, {"checkIntervalDays", 7}, {"disabledIntervalDays", 0}}},
            {"pluginRecord", QJsonObject{{"hostEnabled", false}, {"invalidHostEnabled", true}, {"hostReviewed", QJsonObject{}}}},
            {"builtinApplication", QJsonObject{{"enabled", true}, {"mode", "all"}}},
            {"fileColorGroup", QJsonObject{{"enabled", true}, {"autoDark", false}}},
            // Rows whose field conditions are still being read. Warm neutrals: distinct from the list
            // backgrounds (#FFFFFF / #181C23) and from the blue cursor and selection colors.
            {"fileColorPending", QJsonObject{{"light", QJsonObject{{"background", "#F3F1EC"}, {"marker", "#B7791F"}}},
                {"dark", QJsonObject{{"background", "#2A2924"}, {"marker", "#D9A44A"}}}}},
            {"fileColorRules", QJsonObject{{"version", 1}, {"mode", "first"}, {"groups", QJsonArray{}}}},
            {"workspace", QJsonObject{{"defaultExpanded", true}, {"keepAncestors", true}, {"columnVisible", true}}},
            {"rename", QJsonObject{{"name", "[N]"}, {"extension", "[E]"}, {"find", ""}, {"replace", ""},
                {"regex", false}, {"sensitive", false}, {"nameCase", 0}, {"extensionCase", 0}, {"start", 1},
                {"step", 1}, {"digits", 3}, {"date", "yyyy-MM-dd"}, {"dateSource", 0}, {"dateOffset", 0}, {"includeExtension", false}}},
            {"archiveCreation", QJsonObject{{"schemaVersion", 1}, {"lastFormat", "7z"},
                {"composition", QJsonObject{{"preservePaths", true}, {"includeSubdirectories", true},
                    {"separateArchives", false}, {"omitSelectedFolderRoot", false}}}}}
        };
    }
    inline QJsonValue DefaultValue(const QString& Path, bool ExistingInstallation = true)
    {
        static const auto Existing = Settings(true), Fresh = Settings(false), Extra = Policy();
        const auto Find = [&Path](const QJsonObject& Object) {
            QJsonValue Value(Object);
            for (const auto& Key : Path.split('/')) Value = Value.toObject().value(Key);
            return Value;
        };
        const auto Value = Find(ExistingInstallation ? Existing : Fresh);
        return Value.isUndefined() ? Find(Extra) : Value;
    }
    inline QJsonObject FontDefaults(const QString& Role, bool Recovery = false)
    {
        return {{"family", Role == "list" ? "D2Coding ligature" : "Malgun Gothic"},
            {"pointSize", Role == "list" ? 16 : Recovery ? 9 : 11}};
    }
    inline QString InvalidColor(const QString& Role)
    { return Role.contains("Background") ? "#FFFFFF" : "#202020"; }
    template<typename Design> inline constexpr Design ProductDesign = Design::PaperFluent;

    inline QJsonObject StoreDefaults() { return {{"schemaVersion", 1}}; }
    template<typename Settings> inline Settings SevenZipCreation(bool Zip)
    {
        using Method = decltype(std::declval<Settings>().method_);
        using Solid = decltype(std::declval<Settings>().solid_);
        return Settings{5, Method::Automatic, {}, {}, {}, Zip ? Solid::Off : Solid::All, {}, false, true};
    }
    template<typename Settings> inline Settings IsoCreation()
    {
        using Profile = decltype(std::declval<Settings>().profile_);
        return Settings{Profile::UdfBridge, "HYPERCOMMANDER", false, 946684800, 128ull * 1024 * 1024 * 1024};
    }

    namespace columnPolicy {
        using namespace hc::columns;
    // URLZONE_LOCAL_MACHINE … URLZONE_UNTRUSTED. One list for the ADS zone column and its condition choices.
    inline QStringList ZoneNames()
    {
        return {QObject::tr("내 컴퓨터"), QObject::tr("로컬 인트라넷"), QObject::tr("신뢰할 수 있는 사이트"), QObject::tr("인터넷"), QObject::tr("제한된 사이트")};
    }
    // Picker order: groups as listed, fields inside a group as listed. Identity is the id alone,
    // so regrouping or reordering never changes a saved profile.
    inline QVector<hc::columns::FieldDefinition> ColumnFields()
    {
        QVector<FieldDefinition> Result;
        const auto Group = [&Result](const QString& Title, std::initializer_list<FieldDefinition> Fields) {
            for (auto Field : Fields) { Field.group_ = Title; Result.append(std::move(Field)); }
        };
        constexpr auto Slow = EvaluationCost::Background;
        Group(QObject::tr("이름·형식"), {{"file.name", QObject::tr("이름 (확장자 포함)"), FieldType::Text},
            {"file.stem", QObject::tr("이름 (확장자 제외)"), FieldType::Text},
            {"file.extension", QObject::tr("확장자"), FieldType::Text},
            {"file.type", QObject::tr("파일 형식 설명"), FieldType::Text},
            {"file.kind", QObject::tr("항목 종류"), FieldType::Text},
            {"file.isDirectory", QObject::tr("폴더 여부"), FieldType::Boolean}});
        Group(QObject::tr("크기"), {{"file.size", QObject::tr("크기"), FieldType::UnsignedInteger},
            {"file.allocatedSize", QObject::tr("디스크 할당 크기"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow}});
        Group(QObject::tr("날짜·시간"), {{"file.created", QObject::tr("만든 날짜"), FieldType::Timestamp},
            {"file.modified", QObject::tr("수정 날짜"), FieldType::Timestamp},
            {"file.accessed", QObject::tr("접근 시각"), FieldType::Timestamp}});
        Group(QObject::tr("경로"), {{"file.path", QObject::tr("전체 경로"), FieldType::Text},
            {"file.parentPath", QObject::tr("폴더 경로"), FieldType::Text},
            {"file.parentName", QObject::tr("부모 폴더명"), FieldType::Text},
            {"file.pathLength", QObject::tr("경로 길이"), FieldType::UnsignedInteger}});
        Group(QObject::tr("속성"), {{"file.attributes", QObject::tr("속성"), FieldType::UnsignedInteger},
            {"file.readOnly", QObject::tr("읽기 전용"), FieldType::Boolean},
            {"file.hidden", QObject::tr("숨김"), FieldType::Boolean},
            {"file.system", QObject::tr("시스템"), FieldType::Boolean},
            {"file.archive", QObject::tr("보관"), FieldType::Boolean},
            {"file.compressed", QObject::tr("압축"), FieldType::Boolean},
            {"file.encrypted", QObject::tr("암호화"), FieldType::Boolean},
            {"file.sparse", QObject::tr("희소 파일"), FieldType::Boolean},
            {"file.offline", QObject::tr("오프라인"), FieldType::Boolean}});
        // file.linkTarget is whichever of the symbolic-link and junction targets exists.
        Group(QObject::tr("링크"), {{"file.linkTarget", QObject::tr("링크 대상"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.softlink.target", QObject::tr("SL 대상"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.junction.target", QObject::tr("JC 대상"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.hardlink.count", QObject::tr("HL 수량"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow}});
        // Named streams only: the unnamed ::$DATA stream is the file body.
        Group(QObject::tr("대체 데이터 스트림"), {{"file.alternateDataStream.count", QObject::tr("ADS 수량"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow},
            {"file.alternateDataStream.names", QObject::tr("ADS 목록"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.alternateDataStream.totalSize", QObject::tr("ADS 크기"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow},
            {"file.alternateDataStream.zone", QObject::tr("ADS 인터넷 존"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow}});
        Group(QObject::tr("버전 정보"), {{"file.fileVersion", QObject::tr("파일 버전"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.productVersion", QObject::tr("제품 버전"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.productName", QObject::tr("제품 이름"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.company", QObject::tr("회사"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.description", QObject::tr("파일 설명"), FieldType::Text, ProviderKind::BuiltIn, Slow}});
        Group(QObject::tr("이미지"), {{"file.imageWidth", QObject::tr("이미지 가로"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow},
            {"file.imageHeight", QObject::tr("이미지 세로"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow},
            {"file.imageDimensions", QObject::tr("이미지 규격"), FieldType::Text, ProviderKind::BuiltIn, Slow},
            {"file.imageBitDepth", QObject::tr("이미지 비트 깊이"), FieldType::UnsignedInteger, ProviderKind::BuiltIn, Slow}});
        const QStringList Bytes{"file.size", "file.allocatedSize", "file.alternateDataStream.totalSize"};
        const QStringList Pixels{"file.imageWidth", "file.imageHeight"};
        const QStringList Counts{"file.hardlink.count", "file.alternateDataStream.count"};
        for (auto& Field : Result) {
            if (Bytes.contains(Field.id_)) Field.unit_ = FieldUnit::Bytes;
            else if (Pixels.contains(Field.id_)) Field.unit_ = FieldUnit::Pixels;
            else if (Counts.contains(Field.id_)) Field.unit_ = FieldUnit::Count;
            else if (Field.id_ == "file.pathLength") Field.unit_ = FieldUnit::Characters;
            else if (Field.id_ == "file.alternateDataStream.zone") {
                const auto Names = ZoneNames();
                for (int Zone = 0; Zone < Names.size(); ++Zone) Field.choices_.append({QString::number(Zone), QStringLiteral("%1 (%2)").arg(Names[Zone]).arg(Zone)});
            }
        }
        return Result;
    }
    inline const QUuid BuiltInProfileId{"a4ea8e7c-b621-4db7-bc79-b37dab6e7b8d"};
    inline hc::columns::ColumnProfile DefaultColumnProfile()
    {
        ColumnProfile Profile;
        Profile.id_ = BuiltInProfileId;
        Profile.title_ = QObject::tr("자세히");
        // Assigned once; never derive identity from field expressions or indices.
        const std::array<const char*, 8> Ids{{
            "519c8fbd-8d4a-4160-b742-bfeaf5ea9918", "81404f28-d586-4c19-99e7-f0bcabac3299",
            "e09a576b-14c3-4680-9bea-a0745bcdb9e3", "5753b771-d0a5-4a8b-a00e-f41ba091275a",
            "db962c8f-412a-442b-8cc8-a46f3cd4e7f8", "050d3fe3-b4ce-4a03-b42c-b78d207d3623",
            "0c00423a-e4b7-4e90-8cf2-706b0f2b0a6e", "128c32c8-1a12-4c9b-bb90-fad63f676f25"}};
        // Named, not indexed: ColumnFields() is ordered for the picker, not for this profile.
        const std::array<const char*, 8> FieldIds{{"file.name", "file.size", "file.created", "file.attributes",
            "file.modified", "file.extension", "file.stem", "file.type"}};
        const auto Fields = ColumnFields();
        const auto TitleOf = [&Fields](const QString& Id) {
            for (const auto& Field : Fields) if (Field.id_ == Id) return Field.title_;
            return QString{};
        };
        for (int Index = 0; Index < int(Ids.size()); ++Index) {
            ColumnDefinition Column;
            Column.id_ = QUuid(Ids[Index]);
            Column.fieldId_ = FieldIds[Index];
            Column.title_ = Index == 0 ? QObject::tr("이름") : Index == 7 ? QObject::tr("종류") : TitleOf(Column.fieldId_);
            Column.alignment_ = Index == 1 ? Qt::AlignRight : Index == 2 || Index == 3 || Index == 4 ? Qt::AlignCenter : Qt::AlignLeft;
            if (Index == 1) Column.format_.size_ = SizeFormat::Adaptive;
            hc::view::ColumnWidthPolicy Width;
            if (Index < 5) Width = {hc::view::WidthUnit::Percent, hc::view::WidthMode::Flexible,
                Index == 0 ? 46.0 : Index == 1 ? 15.0 : Index == 3 ? 9.0 : 30.0};
            Width.hidden_ = Index >= 4;
            Column.widths_.fill(Width);
            if (!Width.hidden_) {
                Width.value_ = Index == 0 ? 100 : std::round(Width.value_ * 100 / 54 * 1000000) / 1000000;
                Column.widths_[1] = Column.widths_[2] = Width;
            }
            if (Index != 0) {
                auto Two = Width;
                Two.unit_ = hc::view::WidthUnit::Percent; Two.mode_ = hc::view::WidthMode::Flexible;
                Two.hidden_ = Index != 1 && Index != 3 && Index != 4 && Index != 7;
                Two.value_ = Index == 1 ? 18 : Index == 3 ? 12 : Index == 4 ? 36 : 34;
                Column.widths_[1] = Column.widths_[2] = Two;
            }
            Profile.columns_.append(Column);
        }
        Profile.primaryNameId_ = Profile.columns_.front().id_;
        Profile.sorts_.append({Profile.primaryNameId_, Qt::AscendingOrder});
        return Profile;
    }
    inline const QUuid ReferenceProfileId{"5ff35fcb-93ae-47ac-9a4b-c4e723991e38"};
    inline const QUuid ReferenceTwoLineProfileId{"7dac5cda-e87a-43c7-af0b-58c6a1c8e5da"};
    inline hc::columns::ColumnProfile ReferenceColumnProfile(bool TwoLine = false)
    {
        ColumnProfile Profile;
        Profile.id_ = TwoLine ? ReferenceTwoLineProfileId : ReferenceProfileId;
        Profile.title_ = TwoLine ? QObject::tr("시안 자세히 · 2줄") : QObject::tr("시안 자세히 · 1줄");
        Profile.mode_ = TwoLine ? LayoutMode::NameFirst : LayoutMode::Details;
        Profile.splitNameExtension_ = true;
        const std::array<const char*, 6> Ids{{"d4203a5f-a52a-49da-a849-a5cdbe69fc23",
            "07a00643-8716-4dfe-909a-5f0d02950c7f", "ae7cd1be-0be6-45b0-adc5-5e01c6b91c34",
            "ac66b249-a810-42bc-a8a9-57e67c6c922b", "aa3cac42-1715-4942-a4b7-305d78c42e7f",
            "91fa28bf-c77b-4564-a927-ab979f1797da"}};
        const QStringList Fields{"file.name", "file.type", "file.extension", "file.size", "file.modified", "file.attributes"};
        const QStringList Titles{QObject::tr("이름"), QObject::tr("종류"), QObject::tr("확장자"),
            QObject::tr("크기"), QObject::tr("수정한 날짜"), QObject::tr("속성")};
        const std::array<double, 6> One{{46, 0, 9, 12, 25, 8}}, Two{{100, 34, 0, 18, 36, 12}};
        for (int Index = 0; Index < int(Ids.size()); ++Index) {
            ColumnDefinition Column;
            Column.id_ = QUuid(Ids[Index]); Column.fieldId_ = Fields[Index]; Column.title_ = Titles[Index];
            Column.alignment_ = Index == 3 ? Qt::AlignRight : Index == 2 || Index == 5 ? Qt::AlignCenter : Qt::AlignLeft;
            if (Index == 3) { Column.format_.size_ = SizeFormat::Adaptive; Column.format_.precision_ = 1; }
            if (Index == 4) Column.format_.datePattern_ = "yyyy-MM-dd HH:mm";
            for (int Mode = 0; Mode < 3; ++Mode) {
                auto& Width = Column.widths_[Mode];
                Width.unit_ = hc::view::WidthUnit::Percent; Width.mode_ = hc::view::WidthMode::Flexible;
                Width.value_ = Mode == 0 ? One[Index] : Two[Index];
                Width.hidden_ = Mode == 0 ? Index == 1 : Index == 2;
            }
            Profile.columns_.append(Column);
        }
        Profile.primaryNameId_ = Profile.columns_.front().id_;
        Profile.sorts_.append({Profile.primaryNameId_, Qt::AscendingOrder});
        return Profile;
    }
    }

    inline QString MenuBarFile(const QString& Directory) { return QDir(Directory).filePath("menu-bar.json"); }
    inline QString PluginIniFile(const QString& Directory, const QString& Format, const QString& Id = {})
    {
        return QDir(Directory).filePath(Format == "wcx" ? "archive-plugins/" + Id + "/pkplugin.ini"
            : Format == "wlx" ? "lsplugin.ini" : Format == "wfx" ? "fsplugin.ini" : "contentplugin.ini");
    }

    inline QKeyCombination AuxiliaryShortcut(const QString& Id, int Index = 0)
    {
        if (Id == "indexedChoice") return QKeyCombination(Qt::AltModifier, Qt::Key(Qt::Key_1 + Index));
        if (Id == "externalEditor") return QKeyCombination::fromCombined(int(Qt::ControlModifier) | int(Qt::Key_1) | Index);
        static const QHash<QString, QKeyCombination> Keys{
            {"conflict.renameIncoming", Qt::ALT | Qt::Key_F}, {"conflict.renameExisting", Qt::ALT | Qt::Key_G},
            {"fileColorGroup.add", Qt::ALT | Qt::Key_W}, {"background", Qt::Key_F2},
            {"path.edit", Qt::Key_F2}, {"path.editAlternate", Qt::CTRL | Qt::Key_L},
            {"path.complete", Qt::CTRL | Qt::Key_Space}, {"quickFind.toggleMode", Qt::CTRL | Qt::Key_S}, {"window.systemMenu", Qt::ALT | Qt::Key_Space}};
        if (Id == "quickView.close") return BuiltInCMD_Src_QuickView.Default;
        return Keys.value(Id, QKeyCombination::fromCombined(0));
    }
    template<typename Sequence> inline Sequence SettingsFindShortcut() { return Sequence(Sequence::Find); }
    template<typename Sequence> inline Sequence ViewerSurfaceShortcut(const QString& Command)
    {
        if (Command == "find") return Sequence(Sequence::Find);
        if (Command == "zoom-in") return Sequence(Sequence::ZoomIn);
        if (Command == "zoom-out") return Sequence(Sequence::ZoomOut);
        return {};
    }

    inline QString LegacyPluginIniFile(bool Viewer, bool Archive)
    { return QDir(UserConfigDirectory()).filePath(Viewer ? "lsplugin.ini" : Archive ? "pkplugin.ini" : "tc-data.ini"); }
    inline QString PluginHelperIniFile(const QString& Directory) { return QDir(Directory).filePath("wincmd.ini"); }

    inline QByteArray MenuBarDefaults()
    {
        return QByteArrayLiteral(
            "{\n"
            "  \"schemaVersion\": 1,\n"
            "  \"menus\": [\n"
            "    {\n"
            "      \"id\": \"file\",\n"
            "      \"label\": \"파일(&F)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"view\",\n"
            "          \"command\": \"hc.viewer.open\",\n"
            "          \"label\": \"보기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"edit\",\n"
            "          \"command\": \"hc.file.edit\",\n"
            "          \"label\": \"편집\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"compare\",\n"
            "          \"command\": \"hc.file.compare\",\n"
            "          \"label\": \"파일·폴더 비교\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"copy\",\n"
            "          \"command\": \"hc.file.copy\",\n"
            "          \"label\": \"복사\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"move\",\n"
            "          \"command\": \"hc.file.move\",\n"
            "          \"label\": \"이동\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"rename\",\n"
            "          \"command\": \"hc.file.rename\",\n"
            "          \"label\": \"이름 변경\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"mkdir\",\n"
            "          \"command\": \"hc.file.mkdir\",\n"
            "          \"label\": \"새 폴더\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"newfile\",\n"
            "          \"command\": \"hc.file.new\",\n"
            "          \"label\": \"새 파일\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"delete\",\n"
            "          \"command\": \"hc.file.delete\",\n"
            "          \"label\": \"삭제\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"batch\",\n"
            "          \"command\": \"hc.file.batchRename\",\n"
            "          \"label\": \"다중 이름 변경\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"permissions\",\n"
            "          \"command\": \"hc.file.permissions\",\n"
            "          \"label\": \"권한 확인\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"archives\",\n"
            "          \"label\": \"압축 · 이미지\",\n"
            "          \"items\": [\n"
            "            {\n"
            "              \"id\": \"zip\",\n"
            "              \"command\": \"hc.archive.create\",\n"
            "              \"label\": \"파일 압축…\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"archive-extract\",\n"
            "              \"command\": \"hc.archive.extract\",\n"
            "              \"label\": \"파일 압축 해제…\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"iso\",\n"
            "              \"command\": \"hc.iso.create\",\n"
            "              \"label\": \"ISO 생성\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"inspectiso\",\n"
            "              \"command\": \"hc.iso.inspect\",\n"
            "              \"label\": \"ISO 구조 확인\"\n"
            "            }\n"
            "          ]\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"jobs\",\n"
            "          \"command\": \"hc.jobs.show\",\n"
            "          \"label\": \"작업관리자\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"exit\",\n"
            "          \"command\": \"hc.app.exit\",\n"
            "          \"label\": \"종료\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"edit-menu\",\n"
            "      \"label\": \"편집(&E)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"editfile\",\n"
            "          \"command\": \"hc.file.edit\",\n"
            "          \"label\": \"편집\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"renameedit\",\n"
            "          \"command\": \"hc.file.rename\",\n"
            "          \"label\": \"이름 변경\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"select\",\n"
            "      \"label\": \"선택(&S)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"select-items\",\n"
            "          \"command\": \"hc.selection.select\",\n"
            "          \"label\": \"항목 선택…\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"deselect-items\",\n"
            "          \"command\": \"hc.selection.deselect\",\n"
            "          \"label\": \"항목 선택 해제…\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"select-all\",\n"
            "          \"command\": \"hc.selection.all\",\n"
            "          \"label\": \"전체 선택\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"deselect-all\",\n"
            "          \"command\": \"hc.selection.clear\",\n"
            "          \"label\": \"전체 선택 해제\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"invert-all\",\n"
            "          \"command\": \"hc.selection.invertAll\",\n"
            "          \"label\": \"전체 선택 반전\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"same-extension\",\n"
            "          \"command\": \"hc.selection.sameExtension\",\n"
            "          \"label\": \"현재 항목과 같은 확장자 선택\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"clear-extension\",\n"
            "          \"command\": \"hc.selection.clearExtension\",\n"
            "          \"label\": \"같은 확장자 선택 해제\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"restore-selection\",\n"
            "          \"command\": \"hc.selection.restore\",\n"
            "          \"label\": \"이전 선택 복원\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"remember-selection\",\n"
            "          \"command\": \"hc.selection.remember\",\n"
            "          \"label\": \"현재 선택 기억\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"recall-selection\",\n"
            "          \"command\": \"hc.selection.recall\",\n"
            "          \"label\": \"기억한 선택 불러오기\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"invert\",\n"
            "          \"command\": \"hc.selection.invert\",\n"
            "          \"label\": \"현재 항목 선택 전환 · 다음 항목\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"show\",\n"
            "      \"label\": \"보기(&V)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"quick\",\n"
            "          \"command\": \"hc.viewer.quick\",\n"
            "          \"label\": \"반대 패널 미리 보기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"closequick\",\n"
            "          \"command\": \"hc.viewer.close\",\n"
            "          \"label\": \"미리 보기 닫기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"hidden\",\n"
            "          \"command\": \"hc.view.hidden\",\n"
            "          \"label\": \"숨김·시스템 파일 표시 전환\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"navigate\",\n"
            "      \"label\": \"명령(&C)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"back\",\n"
            "          \"command\": \"hc.navigate.back\",\n"
            "          \"label\": \"뒤로\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"forward\",\n"
            "          \"command\": \"hc.navigate.forward\",\n"
            "          \"label\": \"앞으로\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"find\",\n"
            "          \"command\": \"hc.search.focus\",\n"
            "          \"label\": \"설정 범위에서 찾기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"find-everything\",\n"
            "          \"command\": \"hc.search.everything\",\n"
            "          \"label\": \"Everything에서 찾기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"search-location\",\n"
            "          \"command\": \"hc.search.openLocation\",\n"
            "          \"label\": \"검색 결과의 원본 위치 열기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"path\",\n"
            "          \"command\": \"hc.navigate.path\",\n"
            "          \"label\": \"경로 이동\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"parent\",\n"
            "          \"command\": \"hc.navigate.parent\",\n"
            "          \"label\": \"상위 폴더\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"refresh\",\n"
            "          \"command\": \"hc.navigate.refresh\",\n"
            "          \"label\": \"새로 고침\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"cancel\",\n"
            "          \"command\": \"hc.navigate.cancel\",\n"
            "          \"label\": \"중지\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"favorites\",\n"
            "          \"command\": \"hc.navigate.favorites\",\n"
            "          \"label\": \"즐겨찾기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"search\",\n"
            "          \"command\": \"hc.search.show\",\n"
            "          \"label\": \"범위 검색\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"compatibility\",\n"
            "          \"command\": \"hc.navigate.compatibility\",\n"
            "          \"label\": \"호환 탐색\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"enter-file\",\n"
            "          \"command\": \"hc.navigate.enterFile\",\n"
            "          \"label\": \"파일 내부로 들어가기\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"tabs\",\n"
            "      \"label\": \"탭(&T)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"newtab\",\n"
            "          \"command\": \"hc.tab.new\",\n"
            "          \"label\": \"새 탭\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"closetab\",\n"
            "          \"command\": \"hc.tab.close\",\n"
            "          \"label\": \"탭 닫기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"prevtab\",\n"
            "          \"command\": \"hc.tab.previous\",\n"
            "          \"label\": \"이전 탭\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"nexttab\",\n"
            "          \"command\": \"hc.tab.next\",\n"
            "          \"label\": \"다음 탭\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"tools\",\n"
            "      \"label\": \"도구(&U)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"plugins\",\n"
            "          \"section\": \"plugins\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"automation\",\n"
            "          \"section\": \"automation\"\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"audio\",\n"
            "          \"label\": \"오디오 미리 듣기\",\n"
            "          \"items\": [\n"
            "            {\n"
            "              \"id\": \"audioPreview\",\n"
            "              \"command\": \"hc.audio.preview\",\n"
            "              \"label\": \"미리 듣기\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioPlayPause\",\n"
            "              \"command\": \"hc.audio.playPause\",\n"
            "              \"label\": \"재생/일시정지\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioStop\",\n"
            "              \"command\": \"hc.audio.stop\",\n"
            "              \"label\": \"정지\"\n"
            "            },\n"
            "            {\n"
            "              \"type\": \"separator\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioPrev\",\n"
            "              \"command\": \"hc.audio.prev\",\n"
            "              \"label\": \"이전 곡\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioNext\",\n"
            "              \"command\": \"hc.audio.next\",\n"
            "              \"label\": \"다음 곡\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioSeekBack\",\n"
            "              \"command\": \"hc.audio.seekBack\",\n"
            "              \"label\": \"5초 뒤로\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioSeekForward\",\n"
            "              \"command\": \"hc.audio.seekForward\",\n"
            "              \"label\": \"5초 앞으로\"\n"
            "            },\n"
            "            {\n"
            "              \"type\": \"separator\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"audioLocate\",\n"
            "              \"command\": \"hc.audio.locate\",\n"
            "              \"label\": \"재생 중인 파일 위치 보기\"\n"
            "            }\n"
            "          ]\n"
            "        },\n"
            "        {\n"
            "          \"type\": \"separator\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"system\",\n"
            "          \"label\": \"프로세스·서비스\",\n"
            "          \"items\": [\n"
            "            {\n"
            "              \"id\": \"systemProcesses\",\n"
            "              \"command\": \"hc.system.processes.open\",\n"
            "              \"label\": \"프로세스 보기\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"systemServices\",\n"
            "              \"command\": \"hc.system.services.open\",\n"
            "              \"label\": \"서비스 보기\"\n"
            "            },\n"
            "            {\n"
            "              \"type\": \"separator\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"systemRun\",\n"
            "              \"command\": \"hc.process.run\",\n"
            "              \"label\": \"새 작업 실행…\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"systemFollow\",\n"
            "              \"command\": \"hc.system.followLocation\",\n"
            "              \"label\": \"실행 파일 위치 따라가기\"\n"
            "            },\n"
            "            {\n"
            "              \"id\": \"systemHelper\",\n"
            "              \"command\": \"hc.system.startAdminHelper\",\n"
            "              \"label\": \"관리자 도우미 미리 시작\"\n"
            "            }\n"
            "          ]\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"settings\",\n"
            "      \"label\": \"설정(&O)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"options\",\n"
            "          \"command\": \"hc.settings.show\",\n"
            "          \"label\": \"환경 설정\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"editmenu\",\n"
            "          \"command\": \"hc.menu.edit\",\n"
            "          \"label\": \"메뉴 파일 열기\"\n"
            "        },\n"
            "        {\n"
            "          \"id\": \"reloadmenu\",\n"
            "          \"command\": \"hc.menu.reload\",\n"
            "          \"label\": \"메뉴 파일 다시 읽기\"\n"
            "        }\n"
            "      ]\n"
            "    },\n"
            "    {\n"
            "      \"id\": \"help\",\n"
            "      \"label\": \"도움말(&H)\",\n"
            "      \"items\": [\n"
            "        {\n"
            "          \"id\": \"about\",\n"
            "          \"command\": \"hc.app.about\",\n"
            "          \"label\": \"HyperCommander 정보\"\n"
            "        }\n"
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}");
    }

    inline QList<QKeyCombination> ProtectedScopeKeys()
    {
        return {Qt::Key_Tab, Qt::Key_Escape, Qt::Key_F12, Qt::CTRL | Qt::Key_Tab,
            Qt::CTRL | Qt::Key_F, Qt::CTRL | Qt::Key_Q, Qt::CTRL | Qt::SHIFT | Qt::Key_Tab,
            Qt::ALT | Qt::Key_F1, Qt::ALT | Qt::Key_F2, Qt::ALT | Qt::Key_Left, Qt::ALT | Qt::Key_Right};
    }

} // namespace hc::defaults
