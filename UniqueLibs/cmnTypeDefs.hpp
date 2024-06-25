#pragma once

#include <QString>

#include "cmnTypeDefs_Name.hpp"

constexpr long long KILO_BYTE = 1000;
constexpr long long MEGA_BYTE = KILO_BYTE * KILO_BYTE;
constexpr long long GIGA_BYTE = MEGA_BYTE * KILO_BYTE;
constexpr long long TERA_BYTE = GIGA_BYTE * KILO_BYTE;

constexpr long long KIBI_BYTE = 1024;
constexpr long long MEBI_BYTE = KIBI_BYTE * KIBI_BYTE;
constexpr long long GIBI_BYTE = MEBI_BYTE * KIBI_BYTE;
constexpr long long TEBI_BYTE = GIBI_BYTE * KIBI_BYTE;

Q_GLOBAL_STATIC_WITH_ARGS( QSettings, StSettings, ( QString( "%1/%2" ).arg( qApp->applicationDirPath() ).arg( MAIN_CONFIG_FILENAME ), QSettings::IniFormat ));

////////////////////////////////////////////////////////////////////////////////

/*!
    Index => 0..N

    [Configuration]
    SizeInHeader=
    SizeInFooter=
    UseBuiltInShortcut=True

    [ColorScheme]
    Count=N
    Selected=N
    N_Name=XXXX

    [ColorScheme_XXXX]
    IsDarkMode=false

    Menu_Font_Family=
    Menu_Font_Size=Npt or Npx
    Dialog_Font_Family=
    Dialog_Font_Size=Npt or Npx
    Dialog_FGColor=
    Dialog_BGColor=
    List_Font_Family=
    List_Font_Size=Npt or Npx
    List_FGColor=
    List_BGColor=
    List_CursorColor=
    List_SelectColor=

    FileSet_XXX_FGColor=
    FileSet_XXX_BGColor=
        Color => #RGB, #RRGGBB, #AARRGGBB, #RRRGGGBBB, #RRRRGGGGBBBB, SVG Color Keyword

    [FileSet]
    Count=N
    0_Name=XXXX
    0_Flags=XXXX
    0_Filters=X
    0_Exts=XXX|XXX
    0_Attributes=Z
    0_Size=Z

    // 내장 뷰어
    [Viewer]

    [ListerPlugins]
    Count=N
    0_Path=XXX                  wlx or wlx64
    0_Detect=XXXX
    0_Support64Bit=True|False

    [PacketPlugins]
    Count=N
    0_Path=Caps,XXX
    0_Exts=
    0_Support64Bit=True|False

    // Num( NumpadModifier ), Ctrl, Alt, Shift 
    // UseBuiltInShortcut 값이 True 이면 해당 값을 먼저 가져온 후, 이후에 사용자가 설정한 값으로 재정의한다. 
    [Shortcut]

 */

////////////////////////////////////////////////////////////////////////////////

struct TyConfiguration
{
    int SizeInHeader = 0;
    int SizeInFooter = 0;
};

Q_DECLARE_METATYPE( TyConfiguration );

////////////////////////////////////////////////////////////////////////////////

struct Node
{
    QString                             Name;
    BOOL                                IsNormalizedByNFD = FALSE;
    QString                             Ext;

    DWORD                               Attiributes = 0;
    DWORD                               Reserved0 = 0;
    int64_t                             Size = 0;

    QDateTime                           Created;
    QDateTime                           Modified;

    ///////////////////////////////////////////////////////////////////////////

    QPixmap                             Icon;
    QVector< QVariant >                 VecContent;
};

Q_DECLARE_METATYPE( Node );

constexpr int FILE_SET_NOR_FILTERS      = 0x1;
constexpr int FILE_SET_NOR_EXT_FILTERS  = 0x2;

constexpr int FILE_SET_EXP_ATTRIBUTE    = 0x100;

constexpr int FILE_SET_EXP_SIZE_EQUAL   = 0x1000;
constexpr int FILE_SET_EXP_SIZE_LESS    = 0x2000;
constexpr int FILE_SET_EXP_SIZE_MORE    = 0x4000;

struct TyFileSet
{
    QString                             Name;
    uint32_t                            Flags = 0;

    // 일반
    QStringList                         Filters;
    QVector<QString>                    VecExtFilters;          // 모두 소문자

    // 확장
    uint32_t                            Attributes = 0;
    int64_t                             Size = 0;
};

Q_DECLARE_METATYPE( TyFileSet );

using TyPrFBWithBG = QPair< QColor, QColor >;
Q_DECLARE_METATYPE( TyPrFBWithBG );

struct TyColorScheme
{
    QString             Name;
    bool                IsDarkMode = false;

    // Main
    QFont               Menu_Font;
    QColor              Menu_FGColor;
    QColor              Menu_BGColor;

    QFont               Dialog_Font;
    QColor              Dialog_FGColor;
    QColor              Dialog_BGColor;

    // FileList
    QFont               FileList_Font;
    QColor              FileList_FGColor;
    QColor              FileList_BGColor;
    QColor              FileList_Selected;
    QColor              FileList_Cursor;

    bool                IsInverseCursor = false;
    bool                IsInverseSelect = false;

    // key = FileSet's Name, value.first = FG, value.second = BG
    QMap< QString, TyPrFBWithBG > MapNameToColors;
};

Q_DECLARE_METATYPE( TyColorScheme );

struct TyPlugInWLXFuncs
{
    HMODULE hModule = nullptr;

    HWND( __stdcall*    ListLoad )( HWND ParentWin, char* FileToLoad, int ShowFlags );
    HWND( __stdcall*    ListLoadW )( HWND ParentWin, WCHAR* FileToLoad, int ShowFlags );
    int( __stdcall*     ListLoadNext )( HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags );
    int( __stdcall*     ListLoadNextW )( HWND ParentWin, HWND PluginWin, WCHAR* FileToLoad, int ShowFlags );
    void( __stdcall*    ListCloseWindow )( HWND ListWin );
    void( __stdcall*    ListGetDetectString )( char* DetectString, int maxlen );
    int( __stdcall*     ListSearchText )( HWND ListWin, char* SearchString, int SearchParameter );
    int( __stdcall*     ListSearchTextW )( HWND ListWin, WCHAR* SearchString, int SearchParameter );
    int( __stdcall*     ListSearchDialog )( HWND ListWin, int FindNext );
    int( __stdcall*     ListSendCommand )( HWND ListWin, int Command, int Parameter );
    int( __stdcall*     ListPrint )( HWND ListWin, char* FileToPrint, char* DefPrinter, int PrintFlags, RECT* Margins );
    int( __stdcall*     ListPrintW )( HWND ListWin, WCHAR* FileToPrint, WCHAR* DefPrinter, int PrintFlags, RECT* Margins );
    int( __stdcall*     ListNotificationReceived )( HWND ListWin, int Message, WPARAM wParam, LPARAM lParam );
    void( __stdcall*    ListSetDefaultParams )( PVOID dps );
    HBITMAP( __stdcall* ListGetPreviewBitmap )( char* FileToLoad, int width, int height, char* contentbuf, int contentbuflen );
    HBITMAP( __stdcall* ListGetPreviewBitmapW )( WCHAR* FileToLoad, int width, int height, char* contentbuf, int contentbuflen );
};

struct TyPlugInWLX
{
    int                 Index = 0;
    QString             FileFullPath;
    QString             Detect;
    bool                IsContain64Bit = false;

    QString             FilePath;       // 파일이름 제외;
    QString             FileName;       // 플러그인 이름( 32비트 기준, 확장자 포함, 64비트 플러그인은 해당 이름에 64를 붙인다 )

    TyPlugInWLXFuncs    Funcs;
};

Q_DECLARE_METATYPE( TyPlugInWLX );

struct TyExternalEditor
{
    QString             Name;
    QIcon               Icon;
    QString             IconPath;

    QString             FilePath;
    QString             CMDLine;

    QString             Detect;             // force | ext= ...
    QVector<QString>    VecExtensions;      // 확장자 목록, 소문자
};

Q_DECLARE_METATYPE( TyExternalEditor );
