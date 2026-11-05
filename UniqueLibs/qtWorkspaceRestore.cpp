#include "stdafx.h"
#include "qtWorkspaceRestore.hpp"
#include "builtInFsModel.hpp"
#include "qtPluginHostSettings.hpp"
#include "storageProviders.hpp"
#include "fileContainerOpen.hpp"
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
    Model->SetPlaceholder(Tab.location_, QObject::tr("위치 연결 중…"), true);
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
        auto Candidates = hc::navigation::FileContainerCandidates();
        std::erase_if(Candidates, [](const auto& Choice) { return Choice.dopus_; });
        using OpenResult = Result<std::vector<hc::navigation::OpenedContainer>>;
        auto* Watcher = new QFutureWatcher<OpenResult>(Model);
        auto Stop = std::make_shared<std::stop_source>(); Model->setProperty("hc.restorePending", true);
        QObject::connect(Watcher, &QObject::destroyed, [Stop] { Stop->request_stop(); });
        QObject::connect(Model, &CFSModel::NavigationCancelled, Watcher, [Stop] { Stop->request_stop(); });
        QObject::connect(Watcher, &QFutureWatcher<OpenResult>::finished, Model, [Model, Watcher, Ticket, Tab, Native, Path] {
            auto Result = Watcher->future().takeResult(); Watcher->deleteLater();
            if (Model->property("hc.restoreTicket").toString() != Ticket || !Model->HasPlaceholder()) return;
            Model->setProperty("hc.restorePending", false);
            if (!Result.HasValue()) { Model->SetPlaceholder(Tab.location_, QString::fromStdString(Result.Failure().code_)); return; }
            auto Parent = Tab.location_; Parent.kind_ = Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local;
            Parent.nesting_.clear(); Parent.root_ = QFileInfo(Path).absolutePath().toStdString(); NativeRoot(Model, Parent, Native);
            QString Display = Path;
            for (std::size_t Level = 0; Level < Result.Value().size(); ++Level) {
                auto& Opened = Result.Value()[Level]; const auto Wcx = TySpWCX(new CTCPlugInWCX);
                Wcx->LoadWCX(Opened.choice_.wcx_, Opened.backend_); Wcx->OpenArchive(QString::fromStdString(Opened.location_.root_));
                auto* Pack = new nsHC::CFSPack(Display, Wcx); Pack->SetParent(Model->GetCurrentPath());
                Pack->SetArchiveChain(Opened.location_, Opened.ancestors_); Pack->UseReadProvider(Opened.reader_);
                Model->AppendRoot(nsHC::TySpFileSystem(Pack));
                const auto Internal = QString::fromStdString(Tab.location_.nesting_[Level].entry_);
                Model->SetCurrentPath(Level + 1 == Result.Value().size() ? Internal : QFileInfo(Internal).path()); Display += Internal;
            }
            Model->ClearPlaceholder(); Model->Refresh();
        });
        Watcher->setFuture(QtConcurrent::run([Candidates = std::move(Candidates), Tab, Stop]() mutable -> OpenResult {
            OperationContext Context; Context.stopToken_ = Stop->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            std::vector<hc::navigation::OpenedContainer> Opened;
            for (std::size_t Level = 0; Level < Tab.location_.nesting_.size(); ++Level) {
                hc::navigation::ContainerRequest Request; Request.candidates_ = Candidates;
                if (Level + 1 == Tab.location_.nesting_.size() && !Tab.provider_.isEmpty())
                    std::erase_if(Request.candidates_, [&](const auto& Choice) { return Choice.options_.manifestPath_.compare(Tab.provider_, Qt::CaseInsensitive) != 0; });
                if (!Level) {
                    const auto Path = QString::fromStdString(Tab.location_.root_);
                    Request.entry_.locator_ = {Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Tab.location_.root_};
                    Request.source_ = MakeNativeReadProvider(Request.entry_.locator_.kind_);
                } else {
                    Request.source_ = Opened.back().reader_; Request.entry_.locator_ = Opened.back().location_;
                    Request.entry_.locator_.nesting_.back().entry_ = Tab.location_.nesting_[Level - 1].entry_;
                    Request.ancestors_ = Opened.back().ancestors_; Request.ancestors_.push_back(Opened.back().backend_);
                }
                auto Result = hc::navigation::OpenFileContainer(std::move(Request), Context); if (!Result.HasValue()) return Result.Failure();
                Opened.push_back(std::move(Result.Value()));
            }
            return Opened;
        }));
        return;
    }
    if (Location.kind_ != LocatorKind::Plugin || Tab.provider_.isEmpty() || !Location.nesting_.empty()) {
        Model->SetPlaceholder(Location, QObject::tr("위치를 제공하는 플러그인을 사용할 수 없습니다. 설정 확인 후 다시 시도하십시오.")); return;
    }
    if (hc::workspace::PluginPath(Location).isEmpty()) { Model->SetPlaceholder(Location, QObject::tr("플러그인 내부 경로를 확인하십시오.")); return; }
    for (const auto Format : {PluginFormat::Wfx, PluginFormat::DopusVfs})
        for (const auto& Choice : QPluginHostSettings::DataChoices(hc::settings::AppSettings(), Format, Format == PluginFormat::DopusVfs)) {
            if (Choice.options_.manifestPath_.compare(Tab.provider_, Qt::CaseInsensitive)) continue;
            const auto Options = Choice.options_;
            using OpenResult = Result<HostedStorage>;
            auto* Watcher = new QFutureWatcher<OpenResult>(Model);
            auto FileMount = std::make_shared<std::atomic_bool>(false);
            Model->setProperty("hc.restorePending", true);
            auto Stop = std::make_shared<std::stop_source>();
            QObject::connect(Watcher, &QObject::destroyed, [Stop] { Stop->request_stop(); });
            QObject::connect(Model, &CFSModel::NavigationCancelled, Watcher, [Stop] { Stop->request_stop(); });
            QObject::connect(Watcher, &QFutureWatcher<OpenResult>::finished, Model, [Model, Watcher, Ticket, Tab, FileMount] {
                const auto Result = Watcher->result(); Watcher->deleteLater();
                if (Model->property("hc.restoreTicket").toString() != Ticket || !Model->HasPlaceholder()) return;
                Model->setProperty("hc.restorePending", false);
                if (!Result.HasValue()) { Model->SetPlaceholder(Tab.location_, QString::fromStdString(Result.Failure().code_)); return; }
                const auto& Storage = Result.Value();
                auto Root = nsHC::TySpFileSystem(new nsHC::CFSPlugin(Storage.root_, Storage.reader_, Storage.writer_));
                if (FileMount->load()) {
                    const auto Parent = QFileInfo(Tab.nativeRoot_).absolutePath();
                    NativeRoot(Model, {Parent.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Parent.toStdString()}, {});
                    Root->setProperty("hc.fileContainer", true); Root->setProperty("hc.containerParentPath", Model->GetCurrentPath());
                    Model->AppendRoot(Root);
                } else Model->SetRoot(Root);
                const auto Path = hc::workspace::PluginPath(Tab.location_);
                Model->SetCurrentPath(QDir::toNativeSeparators(Path)); Model->ClearPlaceholder(); Model->Refresh();
            });
            Watcher->setFuture(QtConcurrent::run([Options, Format, Native = Tab.nativeRoot_, Stop, FileMount]() -> OpenResult {
                OperationContext Operation; Operation.stopToken_ = Stop->get_token(); Operation.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                if (Format == PluginFormat::DopusVfs && QFileInfo(Native).isFile()) {
                    hc::navigation::ContainerCandidate Candidate; Candidate.dopus_ = Candidate.content_ = true; Candidate.application_ = Options.applicationRule_;
                    Candidate.options_ = {Options.executable_, Options.manifestPath_, Options.binaryPath_, Options.approval_, Options.limits_, Options.defaultIniPath_};
                    hc::navigation::ContainerRequest Request;
                    Request.entry_.locator_ = {Native.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Native.toStdString()};
                    Request.source_ = MakeNativeReadProvider(Request.entry_.locator_.kind_); Request.candidates_.push_back(std::move(Candidate));
                    auto Opened = hc::navigation::OpenFileContainer(std::move(Request), Operation); if (!Opened.HasValue()) return Opened.Failure();
                    FileMount->store(true); return HostedStorage{Opened.Value().reader_, Opened.Value().writer_, Opened.Value().location_};
                }
                if (Options.applicationRule_.toObject().value("mode").toString("all") != "all") return Error{ErrorCategory::Unsupported, "plugin-application-scope"};
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
