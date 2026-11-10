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
    connect(this, &CCommandMgr::ShortcutsChanged, this, &CCommandMgr::refreshGlobalShortcuts);
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
    const auto Current = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
    const auto Scoped = hc::ui::CommandLineShortcutOverrides(Current);
    if (!Scoped.isEmpty()) Overrides["commandLine"] = Scoped;
    // 보기 입력 범위의 사용자 지정은 전역 맵과 따로 저장되어 있다. 다시 만들 때 그대로 둔다.
    for (const auto Scope : {hc::ui::InputScope::ProcessView, hc::ui::InputScope::ServiceView}) {
        const auto View = Current[hc::ui::ScopeKey(Scope)].toObject();
        if (!View.isEmpty()) Overrides[hc::ui::ScopeKey(Scope)] = View;
    }
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

TyMapShortcutToCMDStr CCommandMgr::RetrieveForCommand(const QString& Id) const
{
    if (hc::ui::IsViewScopedCommand(Id)) return RetrieveForScope(hc::ui::HomeScope(Id));
    return hc::ui::IsCommandLineShortcut(Id)
        ? hc::ui::EffectiveCommandLineShortcuts(hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject())
        : Retrieve();
}

TyMapShortcutToCMDStr CCommandMgr::RetrieveForScope(hc::ui::InputScope Scope) const
{
    if (Scope == hc::ui::InputScope::Global) return Retrieve();
    return hc::ui::EffectiveScopeShortcuts(hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject(), Scope);
}

void CCommandMgr::SetShortcutInScope(QKeyCombination Key, const QString& CMD, hc::ui::InputScope Scope)
{
    if (Scope == hc::ui::InputScope::Global || Scope == hc::ui::InputScope::CommandLine) { SetShortcutWithCMD(Key, CMD); return; }
    if (GetDefaultCMD(CMD).Name.isEmpty() || !hc::ui::ScopeAccepts(Scope, CMD) || hc::ui::IsProtectedScopeKey(Key)) return;
    auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
    auto Scoped = Overrides[hc::ui::ScopeKey(Scope)].toObject();
    Scoped[QKeySequence(Key).toString(QKeySequence::PortableText)] = hc::ui::CommandId(CMD);
    Overrides[hc::ui::ScopeKey(Scope)] = Scoped;
    hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
    emit ShortcutsChanged();
}

void CCommandMgr::DelShortcutInScope(QKeyCombination Key, const QString& Id, hc::ui::InputScope Scope)
{
    if (Scope == hc::ui::InputScope::Global || Scope == hc::ui::InputScope::CommandLine) { DelShortcut(Key, Id); return; }
    auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
    auto Scoped = Overrides[hc::ui::ScopeKey(Scope)].toObject();
    // 기본 키를 지우는 경우를 포함해 null로 표시한다. 기본값에 없는 키의 null은 효과가 없다.
    Scoped[QKeySequence(Key).toString(QKeySequence::PortableText)] = QJsonValue::Null;
    Overrides[hc::ui::ScopeKey(Scope)] = Scoped;
    hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
    emit ShortcutsChanged();
}

void CCommandMgr::ResetScope(hc::ui::InputScope Scope)
{
    auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
    Overrides.remove(hc::ui::ScopeKey(Scope));
    hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
    emit ShortcutsChanged();
}

bool CCommandMgr::ProcessScopedKeyPress(QKeyEvent* KeyEvent, hc::ui::InputScope Scope, const QModelIndex& CursorIndex)
{
    const auto Id = hc::ui::ScopeShortcut(*KeyEvent, hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject(), Scope);
    if (Id.isEmpty() || !bridge_) return false;
    if (bridge_->ExecuteCommand(Id, CursorIndex)) { KeyEvent->accept(); return true; }
    return false;
}

void CCommandMgr::SetShortcutWithCMD( QKeyCombination Key, const QString& CMD )
{
    if (GetDefaultCMD(CMD).Name.isEmpty()) return;
    if (hc::ui::IsViewScopedCommand(CMD)) { SetShortcutInScope(Key, CMD, hc::ui::HomeScope(CMD)); return; }
    if (hc::ui::IsCommandLineShortcut(CMD)) {
        auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
        auto Scoped = hc::ui::CommandLineShortcutOverrides(Overrides);
        Scoped[QKeySequence(Key).toString(QKeySequence::PortableText)] = hc::ui::CommandId(CMD);
        Overrides["commandLine"] = Scoped;
        hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
        emit ShortcutsChanged(); return;
    }
    (*SpMapKeyToCMDText_)[Key] = GetDefaultCMD(CMD).Name; SaveSettings(); emit ShortcutsChanged();
}

void CCommandMgr::DelShortcut( QKeyCombination Key, const QString& Id )
{
    if (hc::ui::IsViewScopedCommand(Id)) { DelShortcutInScope(Key, Id, hc::ui::HomeScope(Id)); return; }
    if (hc::ui::IsCommandLineShortcut(Id)) {
        auto Overrides = hc::settings::AppSettings().Read(hc::settings::Section::Shortcuts).toObject();
        auto Scoped = hc::ui::CommandLineShortcutOverrides(Overrides);
        Scoped[QKeySequence(Key).toString(QKeySequence::PortableText)] = QJsonValue::Null;
        Overrides["commandLine"] = Scoped;
        hc::settings::AppSettings().Put(hc::settings::Section::Shortcuts, Overrides);
        emit ShortcutsChanged(); return;
    }
    SpMapKeyToCMDText_->remove(Key); SaveSettings(); emit ShortcutsChanged();
}

QWidget* CCommandMgr::GetMainUI() const
{
    return MainUI_;
}

void CCommandMgr::SetMainUI( QWidget* MainUI )
{
    clearGlobalShortcuts(); if (qApp) qApp->removeNativeEventFilter(this);
    MainUI_ = MainUI;
    if (bridge_) delete bridge_.data();
    if (MainUI) {
        bridge_ = new CommandUiBridge(MainUI);
        qApp->installNativeEventFilter(this);
        connect(MainUI, &QObject::destroyed, this, [this] { clearGlobalShortcuts(); MainUI_ = nullptr; });
        Refresh();
    }
}

bool CCommandMgr::ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex )
{
    // Esc 는 패널에 걸린 일시 상태를 한 번에 하나씩 끝낸다. 방금 켠 미리 듣기가 클립보드 표시보다 먼저다.
    if (bridge_ && KeyEvent->key() == Qt::Key_Escape && KeyEvent->modifiers() == Qt::NoModifier
        && (bridge_->CancelAudioPreview() || bridge_->CancelClipboard() || bridge_->CancelFlatListing())) {
        KeyEvent->accept(); return true;
    }
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
        { QLatin1String( "hc.file.ads.allowInternetBlock" ), &CCommandMgr::AllowInternetBlock },
        { QLatin1String( "hc.selection.invert" ),      &CCommandMgr::SelectionInvert },
        { QLatin1String( "hc.file.batchRename" ),      &CCommandMgr::FileBatchRename },
        { QLatin1String( "hc.file.new" ),              &CCommandMgr::FileNew },
        { QLatin1String( "hc.archive.create" ),        &CCommandMgr::ArchiveCreate },
        { QLatin1String( "hc.archive.extract" ),       &CCommandMgr::ArchiveExtract },
        { QLatin1String( "hc.view.hidden" ),           &CCommandMgr::ViewHidden },
        { QLatin1String( "hc.navigate.refresh" ),      &CCommandMgr::NavigateRefresh },
        { QLatin1String( "hc.navigate.favorites" ),    &CCommandMgr::NavigateFavorites },
        { QLatin1String( "hc.panel.switch" ),          &CCommandMgr::PanelSwitch },
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
    if (bridge_) bridge_->PromptFileOperation("hc.file.mkdir", "shortcut", CursorIndex);
}

DEFINE_HC_COMMAND( CCommandMgr, FileDelete )
{
    if (bridge_) bridge_->PromptFileOperation("hc.file.delete", "shortcut", CursorIndex);
}

DEFINE_HC_COMMAND( CCommandMgr, FileNormalize )
{
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

DEFINE_HC_COMMAND( CCommandMgr, AllowInternetBlock )
{
    if (bridge_) bridge_->AllowInternetBlock(CursorIndex);
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

DEFINE_HC_COMMAND( CCommandMgr, FileEdit )
{
    if (bridge_) { bridge_->Dispatch("hc.file.edit", {}, "shortcut", CursorIndex); return; }
    QMetaObject::invokeMethod( GetMainUIPtr(), __FUNCNAME__, Qt::QueuedConnection, Q_ARG( const QModelIndex&, CursorIndex ) );
}

CCommandMgr::TySpMapKeyToCMDStr CCommandMgr::retrieve() const
{
    return SpMapKeyToCMDText_;
}

CCommandMgr::~CCommandMgr()
{
    clearGlobalShortcuts();
    if (qApp) qApp->removeNativeEventFilter(this);
}

bool CCommandMgr::GlobalShortcutEnabled(const QString& Id) const
{
    const auto Values = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["globalShortcuts"].toObject();
    const bool Default = Id == "hc.audio.next" || Id == "hc.audio.prev" || Id == "hc.audio.playPause"
        || Id == "hc.audio.stop";
    return Values.value(Id).toBool(Default);
}

void CCommandMgr::SetGlobalShortcutEnabled(const QString& Id, bool Enabled)
{
    auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    auto Values = General["globalShortcuts"].toObject(); Values[Id] = Enabled; General["globalShortcuts"] = Values;
    hc::settings::AppSettings().Put(hc::settings::Section::General, General);
    emit ShortcutsChanged();
}

QString CCommandMgr::GlobalShortcutError(const QString& Id) const { return globalErrors_.value(Id); }

void CCommandMgr::clearGlobalShortcuts()
{
    for (auto It = globalKeys_.cbegin(); It != globalKeys_.cend(); ++It) UnregisterHotKey(nullptr, It.key());
    globalKeys_.clear();
}

void CCommandMgr::refreshGlobalShortcuts()
{
    clearGlobalShortcuts(); globalErrors_.clear();
    if (!MainUI_) return;
    int NextId = 0x4000;
    for (auto It = SpMapKeyToCMDText_->cbegin(); It != SpMapKeyToCMDText_->cend(); ++It) {
        const auto Id = hc::ui::CommandId(It.value());
        if (!GlobalShortcutEnabled(Id)) continue;
        const auto Key = It.key(); const int Code = Key.key(); UINT Vk = 0, CharacterModifiers = 0;
        if ((Code >= Qt::Key_A && Code <= Qt::Key_Z) || (Code >= Qt::Key_0 && Code <= Qt::Key_9)) Vk = Code;
        else if (Code >= Qt::Key_F1 && Code <= Qt::Key_F24) Vk = VK_F1 + Code - Qt::Key_F1;
        else switch (Code) {
        case Qt::Key_Space: Vk = VK_SPACE; break; case Qt::Key_Tab: Vk = VK_TAB; break;
        case Qt::Key_Return: case Qt::Key_Enter: Vk = VK_RETURN; break;
        case Qt::Key_Escape: Vk = VK_ESCAPE; break; case Qt::Key_Backspace: Vk = VK_BACK; break;
        case Qt::Key_Delete: Vk = VK_DELETE; break; case Qt::Key_Insert: Vk = VK_INSERT; break;
        case Qt::Key_Home: Vk = VK_HOME; break; case Qt::Key_End: Vk = VK_END; break;
        case Qt::Key_PageUp: Vk = VK_PRIOR; break; case Qt::Key_PageDown: Vk = VK_NEXT; break;
        case Qt::Key_Left: Vk = VK_LEFT; break; case Qt::Key_Right: Vk = VK_RIGHT; break;
        case Qt::Key_Up: Vk = VK_UP; break; case Qt::Key_Down: Vk = VK_DOWN; break;
        case Qt::Key_MediaNext: Vk = VK_MEDIA_NEXT_TRACK; break; case Qt::Key_MediaPrevious: Vk = VK_MEDIA_PREV_TRACK; break;
        case Qt::Key_MediaPlay: case Qt::Key_MediaTogglePlayPause: Vk = VK_MEDIA_PLAY_PAUSE; break;
        case Qt::Key_MediaStop: Vk = VK_MEDIA_STOP; break;
        default:
            if (Code > 0 && Code < 0x10000) {
                const auto Native = VkKeyScanW(wchar_t(Code));
                if (Native != -1) {
                    Vk = LOBYTE(Native);
                    if (HIBYTE(Native) & 1) CharacterModifiers |= MOD_SHIFT;
                    if (HIBYTE(Native) & 2) CharacterModifiers |= MOD_CONTROL;
                    if (HIBYTE(Native) & 4) CharacterModifiers |= MOD_ALT;
                }
            }
            break;
        }
        const auto Mods = Key.keyboardModifiers(); UINT Flags = MOD_NOREPEAT | CharacterModifiers;
        if (Mods.testFlag(Qt::ControlModifier)) Flags |= MOD_CONTROL;
        if (Mods.testFlag(Qt::AltModifier)) Flags |= MOD_ALT;
        if (Mods.testFlag(Qt::ShiftModifier)) Flags |= MOD_SHIFT;
        if (Mods.testFlag(Qt::MetaModifier)) Flags |= MOD_WIN;
        if (!Vk || Mods.testFlag(Qt::KeypadModifier)) {
            globalErrors_[Id] += tr("%1: 글로벌 등록을 지원하지 않는 키입니다. ").arg(QKeySequence(Key).toString()); continue;
        }
        const int Number = NextId++;
        if (RegisterHotKey(nullptr, Number, Flags, Vk)) globalKeys_.insert(Number, {Id, Key, Flags & ~MOD_NOREPEAT, Vk});
        else globalErrors_[Id] += tr("%1: Windows 등록 실패(%2). 다른 앱의 사용 여부를 확인하세요. ").arg(QKeySequence(Key).toString()).arg(GetLastError());
    }
}

bool CCommandMgr::nativeEventFilter(const QByteArray&, void* Message, qintptr* Result)
{
    const auto* Event = static_cast<MSG*>(Message);
    if (Event->message != WM_HOTKEY || !globalKeys_.contains(int(Event->wParam))) return false;
    const auto Binding = globalKeys_.value(int(Event->wParam));
    if (LOWORD(Event->lParam) != Binding.modifiers_ || HIWORD(Event->lParam) != Binding.virtualKey_) return false;
    // Thread hotkeys arrive as windows_dispatcher_MSG without result storage.
    if (Result) *Result = 0;
    // 등록된 키도 설정의 키 캡처와 모달 대화상자에서는 정상 입력으로 전달한다.
    if (QApplication::activeModalWidget()) {
        if (auto* Focus = QApplication::focusWidget()) {
            QKeyEvent Key(QEvent::KeyPress, Binding.key_.key(), Binding.key_.keyboardModifiers());
            QCoreApplication::sendEvent(Focus, &Key);
        }
        return true;
    }
    if (bridge_) bridge_->ExecuteCommand(Binding.command_, {});
    return true;
}
