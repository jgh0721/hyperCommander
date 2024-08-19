#include "stdafx.h"
#include "commandMgr.hpp"

/// UIs
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"

/// 공통 라이브러리
#include "cmnBase.hpp"

/// 외부 라이브러리
#include <externalLibs/QtitanDataGrid/src/src/grid/QtnGridBase.h>

#include "builtInFsModel.hpp"

#include "ShlObj.h"

#define GetMainUIPtr() ( ( QMainUI* )MainUI_ )

using namespace nsCmn;
using namespace nsCmn::nsCmnQt;

////////////////////////////////////////////////////////////////////////////////

CCommandMgr::CCommandMgr( QObject* parent )
    : QObject( parent )
{
    SpMapKeyToCMDText_ = std::make_shared< TyMapShortcutToCMDStr >();
}

void CCommandMgr::Refresh()
{
    auto SpMapKeyToCMDText = std::make_shared< TyMapShortcutToCMDStr >();

    StSettings->beginGroup( "Configuration" );
    const auto IsUseBuiltInShortcut = StSettings->value( "UseBuiltInShortcut", QVariant( true ) ).toBool();
    StSettings->endGroup();


    // 기본값을 먼저 적용한 후, 사용자가 적용한 
    if( IsUseBuiltInShortcut == true )
    {
        for( const auto& Key : GlobalBuiltInCMDs )
            ( *SpMapKeyToCMDText )[ Key.Default ] = Key.Name;
    }

    StSettings->beginGroup( "Shortcut" );
    for( const auto& Shortcut : StSettings->allKeys() )
    {
        const auto Command = StSettings->value( Shortcut ).toString();
        const auto It = std::find_if( GlobalBuiltInCMDs.begin(), GlobalBuiltInCMDs.end(), [ Command ]( const TyHC_COMMAND& CMD) {
            return CMD.Name.compare( Command, Qt::CaseInsensitive ) == 0;
        } );

        if( It == GlobalBuiltInCMDs.end() )
            continue;
        
        const auto Key = QKeySequence::fromString( Command );
        if( Key.isEmpty() == true )
            continue;
        
        ( *SpMapKeyToCMDText )[ Key[ 0 ] ] = It->Name;
    }
    StSettings->endGroup();

    std::swap( SpMapKeyToCMDText, SpMapKeyToCMDText_ );
}

void CCommandMgr::SaveSettings()
{
    StSettings->beginGroup( "Configuration" );
    const auto IsUseBuiltInShortcut = StSettings->value( "UseBuiltInShortcut", QVariant( true ) ).toBool();
    StSettings->endGroup();

    const auto MapKeyToCMDText = Retrieve();
    StSettings->beginGroup( "Shortcut" );
    for( const auto Key : MapKeyToCMDText.keys() )
    {
        const auto Command = MapKeyToCMDText[ Key ];

        // 기본값 설정을 사용 중이고, 기본값과 일치한다면 해당 값은 무시한다.  
        if( IsUseBuiltInShortcut == true )
        {
            const auto It = std::find_if( GlobalBuiltInCMDs.begin(), GlobalBuiltInCMDs.end(), [Command, Key]( const TyHC_COMMAND& CMD ) {
                return CMD.Name.compare( Command, Qt::CaseInsensitive ) == 0 && CMD.Default.toCombined() == Key;
            } );

            if( It != GlobalBuiltInCMDs.end() )
                continue;
        }

        const auto CMD_Key = QKeySequence( Key ).toString();
        const auto CMD_Value = Command;

        StSettings->setValue( CMD_Key, CMD_Value );
    }
    StSettings->endGroup();
}

TyMapShortcutToCMDStr CCommandMgr::Retrieve() const
{
    return *( retrieve() );
}

TyHC_COMMAND CCommandMgr::GetDefaultCMD( const QString& Command )
{
    const auto It = std::find_if( GlobalBuiltInCMDs.begin(), GlobalBuiltInCMDs.end(), [Command]( const TyHC_COMMAND& CMD ) {
        return CMD.Name.compare( Command, Qt::CaseInsensitive ) == 0;
    } );

    return It == GlobalBuiltInCMDs.end() ? TyHC_COMMAND() : *It;
}

void CCommandMgr::SetShortcutWithCMD( QKeyCombination Key, const QString& CMD )
{

}

void CCommandMgr::DelShortcut( QKeyCombination Key )
{

}

QWidget* CCommandMgr::GetMainUI() const
{
    return MainUI_;
}

void CCommandMgr::SetMainUI( QWidget* MainUI )
{
    MainUI_ = MainUI;
}

bool CCommandMgr::ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex )
{
    auto KtoCMD = retrieve();
    const auto KCombined = KeyEvent->keyCombination();
    if( KtoCMD->contains( KCombined ) == false )
        return false;

    const auto CMD = KtoCMD->value( KCombined );
    Q_ASSERT( CMD.isEmpty() == false );
    KeyEvent->setAccepted( QMetaObject::invokeMethod( this, CMD.data(), Qt::DirectConnection, CursorIndex ) );

    return KeyEvent->isAccepted();
}

void CCommandMgr::ProcessFavoriteDir( const QString& Command, const QString& Path )
{
    if( Command.startsWith( "cd ", Qt::CaseInsensitive ) == true )
    {
        auto Arg = Command.mid( 3 );

        if( Arg.startsWith( R"(\\\)" ) == true )
        {
            // VFS 플러그인
        }
        else
        {
            if( Arg.startsWith( "\"" ) == true && Arg.endsWith( "\"" ) == true )
                Arg = Arg.mid( 1, Arg.size() - 1 );

            cm_ChangePath( QModelIndex(), QStringList() << Arg << Path );
            return;
        }
    }

    Q_ASSERT( false );
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( CCommandMgr, cm_NewTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_PrevTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_NextTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_CloseTab )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SrcQuickView )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_CopyOtherPanel )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_RenameSingleFile )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_List )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_Return )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_MkDir )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_Delete )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_NameNormalization )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SetAttrib )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}
DEFINE_HC_COMMAND( CCommandMgr, cm_SelInverse )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_MultiRenameFiles )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SwitchHidSys )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_RereadSource )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_DirectoryHotList )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND_EX( CCommandMgr, cm_GotoDrive )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ), Q_ARG( const QVariant&, Parameter ) );
}

DEFINE_HC_COMMAND_EX( CCommandMgr, cm_ChangePath )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ), Q_ARG( const QVariant&, Parameter ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SwitchPanel )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_ContextMenu )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_ExternalEditorMenu )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

CCommandMgr::TySpMapKeyToCMDStr CCommandMgr::retrieve() const
{
    return SpMapKeyToCMDText_;
}
