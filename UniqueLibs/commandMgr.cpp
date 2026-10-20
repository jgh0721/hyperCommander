#include "stdafx.h"
#include "commandMgr.hpp"
#include "commandUiConfig.hpp"

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
    SpMapKeyToCMDText_ = std::make_shared<TyMapShortcutToCMDStr>(hc::ui::EffectiveShortcuts(
        hc::settings::AppSettings().Read(hc::settings::Section::General).toObject(),
        hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject()));
    emit ShortcutsChanged();
}

void CCommandMgr::SaveSettings()
{
    QJsonObject Overrides;
    const auto Values = Retrieve();
    const auto Defaults = hc::ui::DefaultShortcuts(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject());
    for (auto It = Defaults.cbegin(); It != Defaults.cend(); ++It)
        if (!Values.contains(It.key())) Overrides[QKeySequence(It.key()).toString(QKeySequence::PortableText)] = QJsonValue::Null;
    for (auto It = Values.begin(); It != Values.end(); ++It)
        Overrides[QKeySequence(It.key()).toString(QKeySequence::PortableText)] = hc::ui::CommandId(It.value());
    hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
}

TyMapShortcutToCMDStr CCommandMgr::Retrieve() const
{
    return *( retrieve() );
}

TyHC_COMMAND CCommandMgr::GetDefaultCMD( const QString& Command )
{
    return hc::ui::FindCommand(Command);
}

void CCommandMgr::SetShortcutWithCMD( QKeyCombination Key, const QString& CMD )
{
    if (GetDefaultCMD(CMD).Name.isEmpty()) return;
    (*SpMapKeyToCMDText_)[Key] = GetDefaultCMD(CMD).Name; SaveSettings(); emit ShortcutsChanged();
}

void CCommandMgr::DelShortcut( QKeyCombination Key )
{
    SpMapKeyToCMDText_->remove(Key); SaveSettings(); emit ShortcutsChanged();
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
    const bool Executed = hc::ui::InvokeShortcut(*KeyEvent, *retrieve(), [this, &CursorIndex](const QString& Id) {
        if (bridge_ && bridge_->ExecuteCommand(Id, CursorIndex)) return true;
        return InvokeCommand( Id, CursorIndex );
    });
    if (Executed) KeyEvent->accept();
    return Executed;
}

bool CCommandMgr::InvokeCommand( const QString& Id, const QModelIndex& CursorIndex )
{
    using TyHandler = void ( CCommandMgr::* )( const QModelIndex& );
    struct TyCommandHandler
    {
        QLatin1String commandId_;
        TyHandler     handler_;
    };
    static const TyCommandHandler Handlers[] = {
        { QLatin1String( "hc.tab.new" ),               &CCommandMgr::TabNew },
        { QLatin1String( "hc.tab.previous" ),          &CCommandMgr::TabPrevious },
        { QLatin1String( "hc.tab.next" ),              &CCommandMgr::TabNext },
        { QLatin1String( "hc.tab.close" ),             &CCommandMgr::TabClose },
        { QLatin1String( "hc.viewer.quick" ),          &CCommandMgr::ViewerQuick },
        { QLatin1String( "hc.file.copy" ),             &CCommandMgr::FileCopy },
        { QLatin1String( "hc.file.rename" ),           &CCommandMgr::FileRename },
        { QLatin1String( "hc.viewer.open" ),           &CCommandMgr::ViewerOpen },
        { QLatin1String( "hc.navigate.open" ),         &CCommandMgr::NavigateOpen },
        { QLatin1String( "hc.file.mkdir" ),            &CCommandMgr::FileMkdir },
        { QLatin1String( "hc.file.delete" ),           &CCommandMgr::FileDelete },
        { QLatin1String( "hc.file.normalize" ),        &CCommandMgr::FileNormalize },
        { QLatin1String( "hc.file.attributes" ),       &CCommandMgr::FileAttributes },
        { QLatin1String( "hc.selection.invert" ),      &CCommandMgr::SelectionInvert },
        { QLatin1String( "hc.file.batchRename" ),      &CCommandMgr::FileBatchRename },
        { QLatin1String( "hc.file.new" ),              &CCommandMgr::FileNew },
        { QLatin1String( "hc.archive.create" ),        &CCommandMgr::ArchiveCreate },
        { QLatin1String( "hc.archive.extract" ),       &CCommandMgr::ArchiveExtract },
        { QLatin1String( "hc.view.hidden" ),           &CCommandMgr::ViewHidden },
        { QLatin1String( "hc.navigate.refresh" ),      &CCommandMgr::NavigateRefresh },
        { QLatin1String( "hc.navigate.favorites" ),    &CCommandMgr::NavigateFavorites },
        { QLatin1String( "hc.panel.switch" ),          &CCommandMgr::PanelSwitch },
        { QLatin1String( "hc.file.context" ),          &CCommandMgr::FileContext },
        { QLatin1String( "hc.file.edit" ),             &CCommandMgr::FileEdit },
    };
    const auto CommandId = hc::ui::CommandId( Id );
    for( const auto& Handler : Handlers )
    {
        if( Handler.commandId_ != CommandId ) continue;
        ( this->*Handler.handler_ )( CursorIndex );
        return true;
    }
    return false;
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

            NavigatePath( QModelIndex(), QStringList() << Arg << Path );
            return;
        }
    }

    Q_ASSERT( false );
}

////////////////////////////////////////////////////////////////////////////
/// 명령 핸들러

DEFINE_HC_COMMAND( CCommandMgr, TabNew )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.new", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, TabPrevious )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.previous", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, TabNext )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.next", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, TabClose )
{
    if (bridge_) { bridge_->Dispatch("hc.tab.close", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, ViewerQuick )
{
    if (bridge_) { bridge_->Dispatch("hc.viewer.quick", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileCopy )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.copy", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileRename )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.rename", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, ViewerOpen )
{
    if (bridge_) { bridge_->Dispatch("hc.viewer.open", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, NavigateOpen )
{
    if (bridge_ && bridge_->DispatchFolder(CursorIndex)) return;
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND(CCommandMgr, ArchiveCreate) { if (bridge_) bridge_->PromptArchive(false, CursorIndex); }
DEFINE_HC_COMMAND(CCommandMgr, ArchiveExtract) { if (bridge_) bridge_->PromptArchive(true, CursorIndex); }

DEFINE_HC_COMMAND( CCommandMgr, FileMkdir )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.mkdir", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileDelete )
{
    if (bridge_) { bridge_->PromptFileOperation("hc.file.delete", "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileNormalize )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileAttributes )
{
    if (bridge_) { bridge_->PromptFileAttributes(CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}
DEFINE_HC_COMMAND( CCommandMgr, SelectionInvert )
{
    // Complete the cursor move before a queued selection repeat resolves its index.
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::AutoConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileNew )
{
    if (bridge_) bridge_->PromptFileTools(false, CursorIndex);
}

DEFINE_HC_COMMAND( CCommandMgr, FileBatchRename )
{
    if (bridge_) { bridge_->PromptFileTools(true, CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, ViewHidden )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, NavigateRefresh )
{
    if (bridge_) { bridge_->Dispatch("hc.navigate.refresh", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, NavigateFavorites )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND_EX( CCommandMgr, NavigateDrive )
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

DEFINE_HC_COMMAND_EX( CCommandMgr, NavigatePath )
{
    if (bridge_)
    {
        const auto Paths = Parameter.toStringList();
        bridge_->Dispatch("hc.navigate.path", {{"path", Paths.isEmpty() ? Parameter.toString() : Paths.front()}}, "legacy", CursorIndex);
        return;
    }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ), Q_ARG( const QVariant&, Parameter ) );
}

DEFINE_HC_COMMAND( CCommandMgr, PanelSwitch )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileContext )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, FileEdit )
{
    if (bridge_) { bridge_->Dispatch("hc.file.edit", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

CCommandMgr::TySpMapKeyToCMDStr CCommandMgr::retrieve() const
{
    return SpMapKeyToCMDText_;
}
