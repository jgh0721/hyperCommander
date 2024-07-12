#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"
#include "externalLibs/TotalCommander/wcxhead.h"

class CPackFSModel
{
    Q_DISABLE_COPY( CPackFSModel );
public:
    struct Node
    {
        QString             FileName;
        DWORD               Attributes = 0;
        int                 CRC;
        QDateTime           FileTime;

        quint64             CompressedSize = 0;
        quint64             UncompressedSize = 0;

        QVector< Node* >    Children;
    };

public:
    CPackFSModel();
    ~CPackFSModel();

    bool                            ChDir( const QString& Dir );
    void                            MkDir( const QString& Dir );

    void                            MakeFile( Node* Item );
    QVector< Node* >                FindFiles( const QString& Filter );

    quint32                         TotalCount() const;
    quint32                         TotalFileCount() const;
    quint32                         TotalDirectoryCount() const;


private:
public:
    QSet< Node* >                   allNodes() const;
    QSet< Node* >                   retrieveChildren( Node* Parent ) const ;
    Node*                           retrieveParentNode( const QString& FileFullPath );
    Node*                           search( const QStringList& PIDL, bool isSearchParent /* = false */ ) const;
    // 해당 경로의 부모 Node 반환
    // Path 가 / 또는 /blahblah 라면 루트 디렉토리 반환

    Node*                           ensureDirectoryTree( const QString& Path );

    Node*                           Root_ = nullptr;
    Node*                           Current_ = nullptr;

    quint32                         TotalCount_ = 0;
    quint32                         TotalFileCount_ = 0;
    quint32                         TotalDirectoryCount_ = 0;
};

struct TyPlugInWLXFuncs
{
    HMODULE hModule = nullptr;

    HWND    ( __stdcall* ListLoad )                 ( HWND ParentWin, char* FileToLoad, int ShowFlags ) = nullptr;
    HWND    ( __stdcall* ListLoadW )                ( HWND ParentWin, WCHAR* FileToLoad, int ShowFlags ) = nullptr;
    int     ( __stdcall* ListLoadNext )             ( HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags ) = nullptr;
    int     ( __stdcall* ListLoadNextW )            ( HWND ParentWin, HWND PluginWin, WCHAR* FileToLoad, int ShowFlags ) = nullptr;
    void    ( __stdcall* ListCloseWindow )          ( HWND ListWin ) = nullptr;
    void    ( __stdcall* ListGetDetectString )      ( char* DetectString, int maxlen ) = nullptr;
    int     ( __stdcall* ListSearchText )           ( HWND ListWin, char* SearchString, int SearchParameter ) = nullptr;
    int     ( __stdcall* ListSearchTextW )          ( HWND ListWin, WCHAR* SearchString, int SearchParameter ) = nullptr;
    int     ( __stdcall* ListSearchDialog )         ( HWND ListWin, int FindNext ) = nullptr;
    int     ( __stdcall* ListSendCommand )          ( HWND ListWin, int Command, int Parameter ) = nullptr;
    int     ( __stdcall* ListPrint )                ( HWND ListWin, char* FileToPrint, char* DefPrinter, int PrintFlags, RECT* Margins ) = nullptr;
    int     ( __stdcall* ListPrintW )               ( HWND ListWin, WCHAR* FileToPrint, WCHAR* DefPrinter, int PrintFlags, RECT* Margins ) = nullptr;
    int     ( __stdcall* ListNotificationReceived ) ( HWND ListWin, int Message, WPARAM wParam, LPARAM lParam ) = nullptr;
    void    ( __stdcall* ListSetDefaultParams )     ( PVOID dps ) = nullptr;
    HBITMAP ( __stdcall* ListGetPreviewBitmap )     ( char* FileToLoad, int width, int height, char* contentbuf, int contentbuflen ) = nullptr;
    HBITMAP ( __stdcall* ListGetPreviewBitmapW )    ( WCHAR* FileToLoad, int width, int height, char* contentbuf, int contentbuflen ) = nullptr;
};

struct TyPlugInWCXFuncs
{
    HMODULE hModule = nullptr;


    HANDLE  ( __stdcall* OpenArchive )              ( tOpenArchiveData* ArchiveData ) = nullptr;
    HANDLE  ( __stdcall* OpenArchiveW )             ( tOpenArchiveDataW* ArchiveData ) = nullptr;
    int     ( __stdcall* ReadHeader )               ( HANDLE hArcData, tHeaderData* HeaderData ) = nullptr;
    int     ( __stdcall* ProcessFile )              ( HANDLE hArcData, int Operation, char* DestPath, char* DestName) = nullptr;
    int     ( __stdcall* ProcessFileW )             ( HANDLE hArcData, int Operation, WCHAR* DestPath, WCHAR* DestName) = nullptr;
    int     ( __stdcall* CloseArchive )             ( HANDLE hArcData ) = nullptr;
    void    ( __stdcall* SetChangeVolProc )         ( HANDLE hArcData, tChangeVolProc pChangeVolProc1 ) = nullptr;
    void    ( __stdcall* SetChangeVolProcW )        ( HANDLE hArcData, tChangeVolProcW pChangeVolProc1 ) = nullptr;
    void    ( __stdcall* SetProcessDataProc )       ( HANDLE hArcData, tProcessDataProc pProcessDataProc ) = nullptr;
    void    ( __stdcall* SetProcessDataProcW )      ( HANDLE hArcData, tProcessDataProcW pProcessDataProc ) = nullptr;

    int     ( __stdcall* PackFiles )                ( char* PackedFile, char* SubPath, char* SrcPath, char* AddList, int Flags ) = nullptr;
    int     ( __stdcall* PackFilesW )               ( WCHAR* PackedFile, WCHAR* SubPath, WCHAR* SrcPath, WCHAR* AddList, int Flags ) = nullptr;
    int     ( __stdcall* DeleteFiles )              ( char* PackedFile, char* DeleteList) = nullptr;
    int     ( __stdcall* DeleteFilesW )             ( WCHAR* PackedFile, WCHAR* DeleteList) = nullptr;
    int     ( __stdcall* GetPackerCaps )            () = nullptr;
    void    ( __stdcall* ConfigurePacker )          ( HWND Parent, HINSTANCE DllInstance ) = nullptr;
    int     ( __stdcall* StartMemPack )             ( int Options, char* FileName) = nullptr;
    int     ( __stdcall* StartMemPackW )            ( int Options, char* FileName) = nullptr;
    int     ( __stdcall* PackToMem )                ( int hMemPack, char* BufIn, int InLen, int* Taken, char* BufOut, int OutLen, int* Written, int SeekBy ) = nullptr;
    int     ( __stdcall* DoneMemPack )              ( int hMemPack ) = nullptr;
    BOOL    ( __stdcall* CanYouHandleThisFile )     ( char* FileName ) = nullptr;
    BOOL    ( __stdcall* CanYouHandleThisFileW )    ( WCHAR* FileName ) = nullptr;
    void    ( __stdcall* PackSetDefaultParams )     ( PackDefaultParamStruct* dps );
    void    ( __stdcall* PkSetCryptCallback )       ( tPkCryptProc pPkCryptProc, int CryptoNr, int Flags );
    int     ( __stdcall* ReadHeaderEx )             ( HANDLE hArcData, tHeaderDataEx* HeaderDataEx ) = nullptr;
    int     ( __stdcall* ReadHeaderExW )            ( HANDLE hArcData, tHeaderDataExW* HeaderDataEx ) = nullptr;
    int     ( __stdcall* GetBackgroundFlags )       ();

    ///* Definition of callback functions called by the DLL Ask to swap disk for multi-volume archive */
    //int( __stdcall* PackChangeVolProc )( char* ArcName, int Mode );
    //int( __stdcall* PackChangeVolProcW )( WCHAR* ArcName, int Mode );
    ///* Notify that data is processed - used for progress dialog */
    //int( __stdcall* PackProcessDataProc )( char* FileName, int Size );
    //int( __stdcall* PackProcessDataProcW )( WCHAR* FileName, int Size );
    //int( __stdcall* PackPkCryptProc )( int CryptoNr, int Mode, char* ArcName, char* Password, int MaxLen );
    //int( __stdcall* PackPkCryptProcW )( int CryptoNr, int Mode, WCHAR* ArcName, WCHAR* Password, int MaxLen );
};

struct TyTCPlugIn
{
    int                 Index = 0;
    QString             FileFullPath;
    bool                IsContain64Bit = false;

    QString             FilePath;       // 파일이름 제외;
    QString             FileName;       // 플러그인 이름( 32비트 기준, 확장자 포함, 64비트 플러그인은 해당 이름에 64를 붙인다 )
};

Q_DECLARE_METATYPE( TyTCPlugIn );

struct TyPlugInWLX : TyTCPlugIn
{
    QString             Detect;
    QVector< QString >  Exts;

    TyPlugInWLXFuncs    Funcs;
};

Q_DECLARE_METATYPE( TyPlugInWLX );

struct TyPlugInWCX : TyTCPlugIn
{
    enum TyEnWCX_CAPS
    {
        WCX_CAPS_NEW        = 0x1,          // Can create new archives 
        WCX_CAPS_MODIFY     = 0x2,          // Can modify existing archives
        WCX_CAPS_MULTIPLE   = 0x4,          // Archive can contain multiple files
        WCX_CAPS_DELETE     = 0x8,          // Can delete files
        WCX_CAPS_OPTIONS    = 0x10,         // Has options dialog
        WCX_CAPS_MEMPACK    = 0x20,         // Supports packing in memory
        WCX_CAPS_BY_CONTENT = 0x40,         // Detect archive type by content
        WCX_CAPS_SEARCHTEXT = 0x80,         // Allow searching for text in archives created with this plugin
        WCX_CAPS_HIDE       = 0x100,        // Don't show packer icon, don't open with Enter but with Ctrl+PgDn
        WCX_CAPS_ENCRYPT    = 0x200         // Plugin supports encryption.
    };

    enum TyEnWCX_Background
    {
        WCX_BACKGROUND_UNPACK   = 0x1,      // Calls to OpenArchive, ReadHeader(Ex), ProcessFile and CloseArchive are thread-safe (unpack in background)
        WCX_BACKGROUND_PACK     = 0x2,      // Calls to PackFiles are thread-safe (pack in background)
        WCX_BACKGROUND_MEMPACK  = 0x4,      // Calls to StartMemPack, PackToMem and DoneMemPack are thread-safe
    };

    QVector< QString >  Exts;
    int                 PackerCaps = 0;     // TyEnWCX_CAPS BitWise OR
    TyPlugInWCXFuncs    Funcs;
};

Q_DECLARE_METATYPE( TyPlugInWCX );

class CTCPlugInWLX
{
public:
    ~CTCPlugInWLX();

    bool                                LoadWLX( const QString& FilePath );
    void                                SetWinId( HWND ListWin );
    void                                CloseWLX();

    static void                         RetrieveWLXFuncs( const QString& PlugIn, TyPlugInWLXFuncs* TC );
    TyPlugInWLXFuncs                    Funcs() const;

private:

    HWND                                wnd_ = nullptr;
    TyPlugInWLX                         wlx_;
};

Q_DECLARE_METATYPE( CTCPlugInWLX );

using TySpWLX = QSharedPointer< CTCPlugInWLX >;

class CTCPlugInWCX
{
    Q_DISABLE_COPY( CTCPlugInWCX );

public:
    CTCPlugInWCX() = default;
    ~CTCPlugInWCX();

    TyOsError                           LoadWCX( const TyPlugInWCX& WCX );

    TyOsValue<bool>                     OpenArchive( const QString& FileFullPath, int Mode = PK_OM_LIST );
    TyOsValue<CPackFSModel*>            CreatePackModel() const;
    // Src = 압축 파일 내에서의 경로 디렉토리 구분자는 /, \ 
    void                                ExtractFile( const QString& Src, const QString& Dst );
    void                                CloseArchive();

    void                                CloseWCX();

    static void                         RetrieveWCXFuncs( const QString& PlugIn, TyPlugInWCXFuncs* TC );

private:
    TyOsValue<HANDLE>                   open( const QString& FileFullPath, int Mode = PK_OM_LIST );
    int                                 close( HANDLE Archive );

    static int __stdcall                onProgressA( char* ArcName, int Mode );
    static int __stdcall                onProgressW( WCHAR* ArcName, int Mode );

    HANDLE                              Arc = nullptr;
    QString                             ArchiveFile;

    TyPlugInWCX                         wcx_;
};

Q_DECLARE_METATYPE( CTCPlugInWCX );

using TySpWCX = QSharedPointer< CTCPlugInWCX >;

class CTCPlugInMgr : public QObject
{
    Q_OBJECT
public:
    CTCPlugInMgr( QObject* Parent = nullptr );

    void                                Refresh();
    void                                SaveSettings();

    quint32                             GetWLXCount() const;
    QString                             GetWLXFilePath( quint32 Idx ) const;
    QString                             GetWLXDetect( quint32 Idx ) const;
    void                                SetWLXOpts( quint32 Idx, const TyPlugInWLX& WLX );
    void                                DelWLXOpts( quint32 Idx );

    /*!
     * @brief
     * @param Menu
     * @param FileFullPath
     * @return 해당 파일에 적용되는 뷰어 수량
     */
    quint32                             ConstructWLXMenu( QMenu* Menu, const QString& FileFullPath );

    QVector< TyPlugInWCX >              RetrieveWCXByExts( const QString& Ext ) const;

private:

    void                                Save_WLX();

    QVector< TyPlugInWLX >              vecWLX;
    QVector< TyPlugInWCX >              vecWCX;
};

using TyStPlugInMgr = nsCmn::nsConcurrent::TSingleton<CTCPlugInMgr>;
