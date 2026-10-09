#include "stdafx.h"
#include "commandMgr.hpp"

/// UIs
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"

/// 공통 라이브러리
#include "cmnBase.hpp"

/// 외부 라이브러리
#include <QtnGridBase.h>

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
    auto Values = std::make_shared<TyMapShortcutToCMDStr>();
    if (hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["useBuiltInShortcuts"].toBool(true))
        for (const auto& Command : GlobalBuiltInCMDs) (*Values)[Command.Default] = Command.Name;
    const auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
    for (auto It = Overrides.begin(); It != Overrides.end(); ++It) {
        const auto Key = QKeySequence::fromString(It.key(), QKeySequence::PortableText);
        if (Key.count() != 1 || Key[0].key() == Qt::Key_unknown) continue;
        if (It.value().isNull()) { Values->remove(Key[0]); continue; }
        const auto Command = GetDefaultCMD(It.value().toString());
        if (!Command.Name.isEmpty()) (*Values)[Key[0]] = Command.Name;
    }
    SpMapKeyToCMDText_ = std::move(Values);
}

void CCommandMgr::SaveSettings()
{
    QJsonObject Overrides;
    const auto Values = Retrieve();
    if (hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["useBuiltInShortcuts"].toBool(true))
        for (const auto& Command : GlobalBuiltInCMDs)
            if (!Values.contains(Command.Default)) Overrides[QKeySequence(Command.Default).toString(QKeySequence::PortableText)] = QJsonValue::Null;
    for (auto It = Values.begin(); It != Values.end(); ++It)
        Overrides[QKeySequence(It.key()).toString(QKeySequence::PortableText)] = It.value();
    hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
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
    if (GetDefaultCMD(CMD).Name.isEmpty()) return;
    (*SpMapKeyToCMDText_)[Key] = GetDefaultCMD(CMD).Name; SaveSettings();
}

void CCommandMgr::DelShortcut( QKeyCombination Key )
{
    SpMapKeyToCMDText_->remove(Key); SaveSettings();
}

QWidget* CCommandMgr::GetMainUI() const
{
    return MainUI_;
}

void CCommandMgr::SetMainUI( QWidget* MainUI )
{
    MainUI_ = MainUI;
    if (bridge_) delete bridge_.data();
    if (MainUI) bridge_ = new CommandUiBridge(MainUI);
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
                Arg = Arg.mid( 1, Arg.size() - 2 );

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
    if (bridge_) { bridge_->Dispatch("hc.tab.new", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_PrevTab )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.previous", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_NextTab )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.next", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_CloseTab )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.close", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_SrcQuickView )
{
    if (bridge_) { bridge_->Dispatch("hc.viewer.quick", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_CopyOtherPanel )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.copy", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_RenameSingleFile )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.rename", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_List )
{
    if (bridge_) { bridge_->Dispatch("hc.viewer.open", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_Return )
{
    if (bridge_ && bridge_->DispatchFolder(CursorIndex)) return;
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_MkDir )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.mkdir", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_Delete )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.delete", "shortcut", CursorIndex); return; }
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
    if (bridge_) { bridge_->Dispatch("hc.navigate.refresh", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, cm_DirectoryHotList )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND_EX( CCommandMgr, cm_GotoDrive )
{
    if (bridge_)
    {
        auto Path = Parameter.toString();
        if (Path.size() == 1 && Path[0].isLetter()) Path += ":\\";
        bridge_->Dispatch("hc.navigate.path", {{"path", Path}}, "shortcut", CursorIndex);
        return;
    }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ), Q_ARG( const QVariant&, Parameter ) );
}

DEFINE_HC_COMMAND_EX( CCommandMgr, cm_ChangePath )
{
    if (bridge_)
    {
        const auto Paths = Parameter.toStringList();
        bridge_->Dispatch("hc.navigate.path", {{"path", Paths.isEmpty() ? Parameter.toString() : Paths.front()}}, "legacy", CursorIndex);
        return;
    }
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
