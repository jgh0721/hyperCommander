#include "stdafx.h"
#include "qtWorkspaceRestore.hpp"
#include "builtInFsModel.hpp"
#include "qtPluginHostSettings.hpp"
#include "storageProviders.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include <QtConcurrent>

namespace {
using namespace hc::storage;
class WorkspaceNative final : public nsHC::CFileSystemT {
public:
    WorkspaceNative(const QString& Root, LocatorKind Kind) : CFileSystemT(Root) {
        cate_ = Kind == LocatorKind::Smb ? nsHC::FS_CATE_REMOTE : nsHC::FS_CATE_LOCAL;
        features_ = nsHC::TyEnFSFeatures(nsHC::FS_FEA_LIST) | nsHC::FS_FEA_READABLE | nsHC::FS_FEA_WRITABLE | nsHC::FS_FEA_MOVABLE
            | nsHC::FS_FEA_DELETABLE | nsHC::FS_FEA_RENAMABLE;
        remote_ = Kind == LocatorKind::Smb || GetDriveTypeW(qUtf16Printable(Root + "\\")) == DRIVE_REMOTE;
    }
    bool IsRemote() const override { return remote_; }
private:
    bool remote_;
};
class WorkspaceShell final : public nsHC::CFileSystemT {
public:
    explicit WorkspaceShell(const Locator& Location) : CFileSystemT({}), location_(Location) {
        cate_ = nsHC::FS_CATE_VIRTUAL; features_ = nsHC::FS_FEA_LIST;
        readProvider_ = MakeShellReadProvider(Location.root_);
    }
    Locator ReadLocation(const QString&) const override { return location_; }
private:
    Locator location_;
};
template<class T> std::shared_ptr<T> WorkerOwned(std::shared_ptr<T> Value) {
    auto* Pointer = Value.get();
    return std::shared_ptr<T>(Pointer, [Owner = std::move(Value)](T*) mutable { (void)QtConcurrent::run([Owner = std::move(Owner)] {}); });
}
void NativeRoot(CFSModel* Model, const Locator& Location, const nsHC::TySpFileSystem& Cached) {
    auto Path = QDir::toNativeSeparators(QString::fromStdString(Location.root_));
    const auto Root = Location.kind_ == LocatorKind::Smb ? QString("\\") : Path.left(2);
    Model->SetRoot(Cached && Cached->GetRoot().compare(Root, Qt::CaseInsensitive) == 0
        ? Cached : nsHC::TySpFileSystem(new WorkspaceNative(Root, Location.kind_)));
    Model->SetCurrentPath(Path.mid(Root.size()));
}
}

void RestoreWorkspaceLocation(CFSModel* Model, const hc::workspace::TabState& Tab, nsHC::TySpFileSystem Native)
{
    using namespace hc::storage;
    using namespace hc::plugins;
    // A new navigation invalidates a pending host connection without retaining
    // widget/model pointers in its worker. Failed roots remain explicit tabs.
    const auto Ticket = QUuid::createUuid().toString();
    Model->setProperty("hc.workspaceProvider", Tab.provider_); Model->setProperty("hc.workspaceNativeRoot", Tab.nativeRoot_);
    Model->SetPlaceholder(Tab.location_, QObject::tr("위치 연결 중…"));
    Model->setProperty("hc.restoreTicket", Ticket);
    const auto& Location = Tab.location_;
    if (Location.kind_ == LocatorKind::Local || Location.kind_ == LocatorKind::Smb) {
        const auto Path = QString::fromStdString(Location.root_);
        if (!QDir::isAbsolutePath(Path)) { Model->SetPlaceholder(Location, QObject::tr("전체 경로가 필요합니다.")); return; }
        if (!Location.nesting_.empty() || (Location.kind_ == LocatorKind::Local && (Path.size() < 3 || Path[1] != ':' || (Path[2] != '/' && Path[2] != '\\')))
            || (Location.kind_ == LocatorKind::Smb && !Path.startsWith("\\\\"))) {
            Model->SetPlaceholder(Location, QObject::tr("저장된 경로를 확인하십시오.")); return;
        }
        NativeRoot(Model, Location, Native); Model->ClearPlaceholder(); Model->Refresh(); return;
    }
    if (Location.kind_ == LocatorKind::Shell && Location.nesting_.empty()) {
        Model->SetRoot(nsHC::TySpFileSystem(new WorkspaceShell(Location))); Model->SetCurrentPath("\\");
        Model->ClearPlaceholder(); Model->Refresh(); return;
    }
    if (Location.kind_ == LocatorKind::Archive && !Location.nesting_.empty() && Location.nesting_.size() <= 8
        && std::ranges::all_of(Location.nesting_, [](const ContainerHop& Hop) { return Hop.provider_ == "wcx"; })) {
        const auto Path = QString::fromStdString(Location.root_);
        if (!QDir::isAbsolutePath(Path)) { Model->SetPlaceholder(Location, QObject::tr("전체 경로가 필요합니다.")); return; }
        std::vector<TySpWCX> Plugins; QString LastManifest;
        for (std::size_t Level = 0; Level < Location.nesting_.size(); ++Level) {
            const auto Name = Level ? QString::fromStdString(Location.nesting_[Level - 1].entry_) : Path;
            const auto Choices = TyStPlugInMgr::GetInstance()->RetrieveWCXByExts(QFileInfo(Name).suffix());
            for (const auto& Choice : Choices) {
                if (!Choice.hostEnabled_ || (Level + 1 == Location.nesting_.size() && !Tab.provider_.isEmpty() && Choice.hostManifest_.compare(Tab.provider_, Qt::CaseInsensitive))) continue;
                const auto Wcx = TySpWCX(new CTCPlugInWCX);
                if (Wcx->LoadWCX(Choice).ErrorCode != ERROR_SUCCESS) continue;
                Plugins.push_back(Wcx); LastManifest = Choice.hostManifest_; break;
            }
            if (Plugins.size() != Level + 1) break;
        }
        if (Plugins.size() == Location.nesting_.size()) {
            auto Parent = Location; Parent.kind_ = Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local;
            Parent.nesting_.clear(); Parent.root_ = QFileInfo(Path).absolutePath().toStdString(); NativeRoot(Model, Parent, Native);
            nsHC::CFSPack* Previous = nullptr; QString Display = Path;
            for (std::size_t Level = 0; Level < Plugins.size(); ++Level) {
                auto* Pack = new nsHC::CFSPack(Display, Plugins[Level]); Pack->SetParent(Model->GetCurrentPath());
                auto Root = Location; Root.nesting_.resize(Level + 1); Root.nesting_.back().entry_ = "/";
                if (Previous) Pack->SetArchiveChain(std::move(Root), Previous->ArchiveBackends());
                Model->AppendRoot(nsHC::TySpFileSystem(Pack));
                const auto Internal = QString::fromStdString(Location.nesting_[Level].entry_);
                Model->SetCurrentPath(Level + 1 == Plugins.size() ? Internal : QFileInfo(Internal).path());
                Display += Internal; Previous = Pack;
            }
            Model->setProperty("hc.workspaceProvider", LastManifest); Model->ClearPlaceholder(); Model->Refresh(); return;
        }
    }
    if (Location.kind_ != LocatorKind::Plugin || Tab.provider_.isEmpty() || !Location.nesting_.empty()) {
        Model->SetPlaceholder(Location, QObject::tr("위치를 제공하는 플러그인을 사용할 수 없습니다. 설정 확인 후 다시 시도하십시오.")); return;
    }
    if (hc::workspace::PluginPath(Location).isEmpty()) { Model->SetPlaceholder(Location, QObject::tr("플러그인 내부 경로를 확인하십시오.")); return; }
    for (const auto Format : {PluginFormat::Wfx, PluginFormat::DopusVfs})
        for (const auto& Choice : QPluginHostSettings::DataChoices(*StSettings, Format)) {
            if (Choice.options_.manifestPath_.compare(Tab.provider_, Qt::CaseInsensitive)) continue;
            const auto Options = Choice.options_;
            using OpenResult = Result<HostedStorage>;
            auto* Watcher = new QFutureWatcher<OpenResult>(Model);
            Model->setProperty("hc.restorePending", true);
            auto Stop = std::make_shared<std::stop_source>();
            QObject::connect(Watcher, &QObject::destroyed, [Stop] { Stop->request_stop(); });
            QObject::connect(Model, &CFSModel::NavigationCancelled, Watcher, [Stop] { Stop->request_stop(); });
            QObject::connect(Watcher, &QFutureWatcher<OpenResult>::finished, Model, [Model, Watcher, Ticket, Tab] {
                const auto Result = Watcher->result(); Watcher->deleteLater();
                if (Model->property("hc.restoreTicket").toString() != Ticket || !Model->HasPlaceholder()) return;
                Model->setProperty("hc.restorePending", false);
                if (!Result.HasValue()) { Model->SetPlaceholder(Tab.location_, QString::fromStdString(Result.Failure().code_)); return; }
                const auto& Storage = Result.Value();
                Model->SetRoot(nsHC::TySpFileSystem(new nsHC::CFSPlugin(Storage.root_, Storage.reader_, Storage.writer_)));
                const auto Path = hc::workspace::PluginPath(Tab.location_);
                Model->SetCurrentPath(QDir::toNativeSeparators(Path)); Model->ClearPlaceholder(); Model->Refresh();
            });
            Watcher->setFuture(QtConcurrent::run([Options, Format, Native = Tab.nativeRoot_, Stop]() -> OpenResult {
                OperationContext Operation; Operation.stopToken_ = Stop->get_token(); Operation.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                auto Manifest = InspectManifestFile(Options.manifestPath_, Operation); if (!Manifest.HasValue()) return Manifest.Failure();
                if (QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Manifest.Value().binaryPath_, Qt::CaseInsensitive)) return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch"};
                auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, {}, Operation, Options.defaultIniPath_);
                if (!Started.HasValue()) return Started.Failure();
                auto Process = std::shared_ptr<PluginProcess>(Started.Value().release(), [](PluginProcess* Value) { (void)QtConcurrent::run([Value] { delete Value; }); });
                auto Store = MakeNativeContentStore(QDir::tempPath().toStdString()); if (!Store.HasValue()) return Store.Failure();
                const ProviderId Session{QUuid::createUuid().toString(QUuid::Id128).toStdString()};
                auto Storage = Format == PluginFormat::DopusVfs ? MakeHostedDopusVfsProvider(Process, Session, Store.Value(), Options.approval_.granted_.contains(Permission::WriteContent), Native)
                    : MakeHostedWfxProvider(Process, Session, Store.Value(), Options.approval_.granted_.contains(Permission::WriteContent));
                if (!Storage.HasValue()) return Storage.Failure();
                Storage.Value().reader_ = WorkerOwned(std::move(Storage.Value().reader_));
                if (Storage.Value().writer_) Storage.Value().writer_ = WorkerOwned(std::move(Storage.Value().writer_));
                return Storage;
            }));
            return;
        }
    Model->SetPlaceholder(Location, QObject::tr("승인된 플러그인을 찾을 수 없습니다. 설정 확인 후 다시 시도하십시오."));
}
