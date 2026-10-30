#include "stdafx.h"
#include "solTypeDefs_Ver.hpp"
#include "qtMessageDialog.hpp"
#include "builtinPluginCatalog.hpp"
#include "HyperCommander/UIs/dialogShell.hpp"
#include "HyperCommander/UIs/fileToolsDialogs.hpp"
#include "HyperCommander/UIs/permissionDialogs.hpp"
#include "archive/creationDialog.hpp"
#include "archive/creationJob.hpp"
#include "commonLibs/plugins/archiveVolumeNames.hpp"
#include <QCheckBox>
#include "commandUiBridge.hpp"
#include "folderSize.hpp"
#include "HyperCommander/UIs/itemSelectionController.hpp"
#include "navigationError.hpp"
#include "HyperCommander/UIs/dlgMain.hpp"
#include <fmwidgets/FindBox.h>
#include <fmwidgets/PanelTabStrip.h>
#include <fmstyle/ThemeManager.h>
#include <fmstyle/Glyphs.h>
#include "commandMgr.hpp"
#include "userMenu.hpp"
#include "commandUiWidgets.hpp"
#include <fmwidgets/FunctionKeyBar.h>
#include <QDesktopServices>
#include <QClipboard>
#include <QMimeData>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QEvent>
#include <QVBoxLayout>
#include <QShortcut>
#include <QScopedValueRollback>
#include <QToolButton>
#include "HyperCommander/UIs/dlgOpts.hpp"
#include "UIs/cmpPanel.hpp"
#include "builtInFsModel.hpp"
#include "fileJobUi.hpp"
#include "HyperCommander/UIs/fileAttributesDialog.hpp"
#include "nativeWritableStorage.hpp"
#include "storageProviders.hpp"
#include "qtSearch.hpp"
#include "quickFindController.hpp"
#include "qtAutomation.hpp"
#include "qtWorkspaceRestore.hpp"
#include "wcxMutation.hpp"
#include "wcxStorage.hpp"
#include "wcxBackend.hpp"
#include "archiveExtractJob.hpp"
#include "commonLibs/operations/providerInteractionTask.hpp"
#include "qtViewerHost.hpp"
#include <QStandardPaths>
#include "document/documentOpenController.hpp"
#include "HyperCommander/UIs/dlgExternalConfigure.hpp"
#include "externalEditorMgr.hpp"
#include "qtPluginHostSettings.hpp"
#include "qtPluginReview.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "nativeMaterializer.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include <QtConcurrent>
#include "commonLibs/storage/materializedContent.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"
#include <QFileDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include "UniqueLibs/qtDialogFrame.hpp"
#include <QtitanGrid.h>

using namespace hc::commands;
using namespace hc::storage;

namespace
{
    Result<hc::operations::FileOperationRequest> NativeFileRequest(const QList<QUrl>& Urls, bool Move, OperationContext Context)
    {
        hc::operations::FileOperationRequest Request;
        Request.operation_ = Move ? hc::operations::FileOperation::Move : hc::operations::FileOperation::Copy;
        std::set<std::string> Seen;
        std::map<LocatorKind, std::shared_ptr<IStorageProvider>> Readers;
        for (const auto& Url : Urls) {
            if (Context.stopToken_.stop_requested()) return Error{ErrorCategory::Cancelled, "file-import-cancelled"};
            if (!Url.isLocalFile()) return Error{ErrorCategory::InvalidRequest, "absolute-source-required"};
            const auto Path = QDir::toNativeSeparators(QDir::cleanPath(Url.toLocalFile()));
            if (!QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "absolute-source-required"};
            Locator Location{Path.startsWith("\\\\") && !Path.startsWith("\\\\?\\") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
            if (Path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive)) Location.kind_ = LocatorKind::Smb;
            if (!Seen.insert(Path.toCaseFolded().toStdString()).second) continue;
            auto& Reader = Readers[Location.kind_];
            if (!Reader) Reader = MakeNativeReadProvider(Location.kind_);
            auto Entry = Reader->Resolve(Location, Context).get();
            if (!Entry.HasValue()) return Entry.Failure();
            Request.sources_.push_back({std::move(Entry.Value()), Reader, MakeNativeWritableStorage(Location.kind_, Reader)});
        }
        if (Request.sources_.empty()) return Error{ErrorCategory::InvalidRequest, "file-selection-empty"};
        return Request;
    }

    class ShortcutDropTask final : public hc::operations::IJobTask
    {
    public:
        ShortcutDropTask(QStringList Paths, QString Destination) : paths_(std::move(Paths)), destination_(std::move(Destination)) {}
        hc::operations::JobStep Advance(hc::operations::JobStepContext Context) override
        {
            using namespace hc::operations;
            JobStep Step;
            const auto Fail = [&](const QString& Message) {
                Step.next_ = JobState::Failed; Step.error_ = Error{ErrorCategory::Unavailable, Message.toStdString()}; return Step;
            };
            if (Context.operation_.stopToken_.stop_requested()) { Step.next_ = JobState::Cancelled; return Step; }
            if (next_ >= paths_.size()) { Step.next_ = JobState::Completed; return Step; }
            const auto Path = paths_[next_];
            const auto Name = QFileInfo(Path).fileName();
            if (!QDir::isAbsolutePath(Path) || Path.contains(QChar::Null) || Name.isEmpty()) return Fail(QObject::tr("바로 가기 원본 경로가 올바르지 않습니다: %1").arg(Path));
            // Publish a complete .lnk with a non-replacing rename. Existing
            // shortcuts and files remain untouched, including concurrent creates.
            QTemporaryDir Staging(QDir(destination_).filePath(".hc-shortcuts-XXXXXX"));
            if (!Staging.isValid()) return Fail(QObject::tr("바로 가기를 만들 폴더에 쓸 수 없습니다: %1").arg(destination_));
            QFile Source(Path);
            const auto Temporary = Staging.filePath("shortcut.lnk");
            if (!Source.link(Temporary)) return Fail(QObject::tr("바로 가기를 만들 수 없습니다: %1\n%2").arg(Path, Source.errorString()));
            QFile Shortcut(Temporary);
            bool Created = false;
            for (int Suffix = 1; Suffix <= 10000; ++Suffix) {
                const auto Target = QDir(destination_).filePath(Name + (Suffix == 1 ? QString{} : QString(" (%1)").arg(Suffix)) + ".lnk");
                if (Shortcut.rename(Target)) { Created = true; break; }
                const QFileInfo Existing(Target);
                if (!Existing.exists() && !Existing.isSymbolicLink()) return Fail(QObject::tr("바로 가기를 저장할 수 없습니다: %1\n%2").arg(Target, Shortcut.errorString()));
            }
            if (!Created) return Fail(QObject::tr("사용 가능한 바로 가기 이름을 찾을 수 없습니다: %1").arg(Name));
            ++next_;
            Step.item_ = ItemOutcome{Path.toStdString(), ItemOutcome::Status::Completed, {}, true, false};
            JobProgress Progress; Progress.currentItem_ = Path.toStdString(); Progress.source_ = Path.toStdString();
            Progress.destination_ = destination_.toStdString(); Progress.itemsDone_ = next_; Progress.plannedItems_ = paths_.size(); Step.progress_ = std::move(Progress);
            return Step;
        }
        std::vector<Error> Finalize(hc::operations::JobState) override { return {}; }
    private:
        QStringList paths_;
        QString destination_;
        qsizetype next_ = 0;
    };

    QByteArray NativeClipboardToken(const QMimeData* Mime)
    {
        if (!Mime || !Mime->hasUrls()) return {};
        QJsonArray Urls;
        for (const auto& Url : Mime->urls()) {
            if (!Url.isLocalFile() || Url.toLocalFile().contains(QChar::Null)) return {};
            Urls.append(Url.toString(QUrl::FullyEncoded));
        }
        if (Urls.isEmpty()) return {};
        return QCryptographicHash::hash(QJsonDocument(QJsonObject{{"urls", Urls},
            {"effect", QString::fromLatin1(Mime->data("application/x-qt-windows-mime;value=\"Preferred DropEffect\"").toBase64())}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }
    template<class T> std::shared_ptr<T> WorkerOwned(std::shared_ptr<T> Value)
    {
        auto* Pointer = Value.get();
        return std::shared_ptr<T>(Pointer, [Owner = std::move(Value)](T*) mutable {
            (void)QtConcurrent::run([Owner = std::move(Owner)] {});
        });
    }
    QString MetadataText(const Result<hc::plugins::MetadataValue>& Value)
    {
        using namespace hc::plugins;
        if (!Value.HasValue()) return QString::fromStdString(Value.Failure().code_);
        if (Value.Value().status_ == MetadataStatus::Delayed) return QObject::tr("계산 지연");
        if (Value.Value().status_ == MetadataStatus::OnDemand) return QObject::tr("요청 시 계산");
        QString Text;
        std::visit([&Text](const auto& Item) {
            using Type = std::decay_t<decltype(Item)>;
            if constexpr (std::is_same_v<Type, std::string>) Text = QString::fromStdString(Item);
            else if constexpr (std::is_same_v<Type, bool>) Text = Item ? QObject::tr("예") : QObject::tr("아니요");
            else if constexpr (!std::is_same_v<Type, std::monostate>) Text = QString::number(Item);
        }, Value.Value().value_);
        return Text;
    }
    class QtExecutor final : public IExecutor
    {
    public:
        explicit QtExecutor(QObject* Owner) : owner_(Owner) {}
        bool Post(std::function<void()> Work) override
        {
            const auto App = QCoreApplication::instance();
            if (!App) return false;
            // The application is the stable delivery context. The owner guard is
            // examined on the GUI thread, so deleting a tab/window drops its work.
            return QMetaObject::invokeMethod(App, [Owner = owner_, Work = std::move(Work)] { if (Owner) Work(); }, Qt::QueuedConnection);
        }
    private:
        QPointer<QObject> owner_;
    };
    QModelIndex SourceIndex(QModelIndex Index)
    {
        while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
        return Index;
    }
    CmpPanel* PanelOf(QObject* Object)
    {
        for (auto* Parent = Object; Parent; Parent = Parent->parent())
            if (auto* Panel = qobject_cast<CmpPanel*>(Parent)) return Panel;
        return nullptr;
    }
    CFSModel* PanelModel(CmpPanel* Panel)
    {
        if (!Panel) return nullptr;
        const auto View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        return View ? qobject_cast<CFSModel*>(View->BaseModel()) : nullptr;
    }
}

std::shared_ptr<IExecutor> MakeQtExecutor(QObject* Owner) { return std::make_shared<QtExecutor>(Owner); }

CommandUiBridge::CommandUiBridge(QWidget* Main) : QObject(Main), main_(Main), executor_(MakeQtExecutor(this)),
    registry_(std::make_shared<CommandRegistry>()), events_(std::make_shared<EventHub>())
{
    dispatcher_ = std::make_unique<CommandDispatcher>(registry_, events_);
    activePanel_ = Main->findChild<CmpPanel*>("cmpLeftPanel");
    for (auto* Panel : Main->findChildren<CmpPanel*>())
        connect(Panel, &CmpPanel::sig_NotifyPanelActivated, this, [this, Panel] { activePanel_ = Panel; });
    const auto Commands = hc::defaults::BusCommands();
    for (const auto& Descriptor : Commands)
    {
        const auto Registered = registry_->Register(Descriptor, {
            [Guard = QPointer<CommandUiBridge>(this)](const CommandInvocation& Invocation) {
                const bool Available = Guard && (Guard->findModel(Invocation.context_.tabId_)
                    || Invocation.id_ == "hc.viewer.close" || (Invocation.id_ == "hc.viewer.quick" && Guard->quick_));
                return SupportDecision{Available ? Availability::Supported : Availability::Unsupported, Available ? "tab-available" : "tab-closed"};
            },
            [Guard = QPointer<CommandUiBridge>(this)](const CommandInvocation& Invocation) -> Result<CommandOutcome> {
                if (!Guard) return Error{ErrorCategory::Cancelled, "window-closed"};
                return Guard->execute(Invocation);
            }}, executor_);
        Q_ASSERT(Registered.HasValue());
    }
    auto Subscription = events_->Subscribe("command.failed", executor_, [Guard = QPointer<QWidget>(Main)](const EventEnvelope& Event) {
        if (auto* Window = qobject_cast<QMainWindow*>(Guard.data()))
            Window->statusBar()->showMessage(std::get<std::string>(Event.payload_.at("command")).starts_with("hc.navigate.")
                ? QObject::tr("폴더 이동을 완료하지 못했습니다. 경로와 연결 상태를 확인하세요.")
                : std::get<std::string>(Event.payload_.at("command")) == "hc.file.compare"
                ? QObject::tr("비교할 파일 2~3개 또는 폴더 2개를 선택하세요. 양쪽 패널에서는 각각 1개를 선택할 수 있습니다. 오류: %1").arg(QString::fromStdString(std::get<std::string>(Event.payload_.at("code"))))
                : QObject::tr("명령을 실행할 수 없습니다: %1").arg(QString::fromStdString(std::get<std::string>(Event.payload_.at("code")))), 6000);
    });
    errorSubscription_ = std::move(Subscription.Value());
    fileJobs_ = new FileJobUi(Main, events_);
    viewers_ = new QtViewerHost(Main, [] {
        const auto Base=QDir::tempPath()+"/hc-viewer-content";QDir().mkpath(Base);
        MaterializationLimits Limits;Limits.maxActiveFiles_=std::numeric_limits<std::size_t>::max();
        return MakeNativeMaterializer(Base.toStdString(),Limits);
    },
        [] { return QPluginHostSettings::ViewerChoices(hc::settings::AppSettings()); },
        [] { return QPluginHostSettings::ViewerSelection(hc::settings::AppSettings()); });
    viewers_->reviewPlugin_ = [this](const QString& Binary) {
        QMainOpts Settings(main_);
        QTimer::singleShot(0, &Settings, [&Settings, Binary] { Settings.ShowPluginSettingsPage(Binary, true); });
        Settings.exec();
    };
    // Product startup may retain one idle viewer; Qt test mode owns its own
    // session lifecycle and explicitly prepares viewers in the adoption tests.
    if (!QStandardPaths::isTestModeEnabled() && QCoreApplication::applicationName() == "HyperCommander") QTimer::singleShot(500, viewers_, [this] {
        const auto Catalog = hc::document::BuildDocumentHandlerCatalog(hc::settings::AppSettings(), QCoreApplication::applicationDirPath());
        const auto Found = Catalog.targets_.find("builtin.hyperviewer");
        if (Found != Catalog.targets_.end() && Found->second.descriptor_.available_) viewers_->PreparePlugin(Found->second.plugin_);
    });
    documents_ = new hc::document::DocumentOpenController(Main, [] {
        std::vector<hc::document::DocumentHandlerTarget> Plugins;
        for (const auto& Choice : QPluginHostSettings::ViewerChoices(hc::settings::AppSettings())) {
            Plugins.push_back(hc::document::BuildPluginViewerTarget(Choice.name_, Choice.options_, Choice.extensions_));
        }
        return hc::document::BuildDocumentHandlerCatalog(hc::settings::AppSettings(), QCoreApplication::applicationDirPath(), std::move(Plugins));
    }, [this](const auto& Target, auto Inputs, auto Completion) {
        if (Target.descriptor_.delivery_ == hc::document::FileDelivery::OnePerProcess) {
            auto Left = std::make_shared<std::size_t>(Inputs.size()); auto Failed = std::make_shared<bool>(false);
            const auto Ready = [Left, Failed, Completion](bool Success, const QString& Error) { if (*Failed) return; if (!Success) { *Failed = true; Completion(false, Error); } else if (--*Left == 0) Completion(true, {}); };
            for (const auto& Input : Inputs) if (!viewers_->OpenPlugin(Target.plugin_, {Input}, QString::fromStdString(Target.descriptor_.name_), Ready)) return false;
            return true;
        }
        return viewers_->OpenPlugin(Target.plugin_, std::move(Inputs), QString::fromStdString(Target.descriptor_.name_), std::move(Completion)) != nullptr;
    });
    connect(documents_, &hc::document::DocumentOpenController::ConfigureRequested, this, [this] {
        TyStExternalEditorMgr::GetInstance()->Refresh(); CExternalConfigure Dialog(main_); Dialog.exec();
    });
    connect(documents_, &hc::document::DocumentOpenController::Finished, this, [this](const QString&, const QString& Status, const QString& Reason) {
        if (auto* Window = qobject_cast<QMainWindow*>(main_.data())) {
            if (Status == "failed" || Status == "rejected") Window->statusBar()->showMessage(tr("문서를 열 수 없습니다: %1").arg(Reason), 8000);
            else if (Status == "cancelled") Window->statusBar()->showMessage(tr("문서 열기를 취소했습니다."), 3000);
            else Window->statusBar()->showMessage(Status == "ready" ? tr("문서를 열었습니다.") : Status == "started" ? tr("편집기를 실행했습니다.") : tr("문서를 준비하고 있습니다…"), 3000);
        }
    });
    auto* PreviewTimer = new QTimer(this); PreviewTimer->setInterval(120);
    connect(PreviewTimer, &QTimer::timeout, this, &CommandUiBridge::refreshPreview); PreviewTimer->start();
    fileJobs_->onFinished_ = [Guard = QPointer<CommandUiBridge>(this)](const hc::operations::JobResult& Result) {
        if (!Guard || !Guard->main_) return;
        if (auto Selection = Guard->jobSelections_.find(Result.snapshot_.id_); Selection != Guard->jobSelections_.end()) {
            const auto Saved = std::move(Selection->second); Guard->jobSelections_.erase(Selection);
            if (Saved.view_ && Saved.model_ && Saved.view_->BaseModel() == Saved.model_
                && Saved.location_ == Saved.model_->property("hc.snapshotLocation").toString()
                && Result.snapshot_.state_ == hc::operations::JobState::Completed && !Result.snapshot_.cancelRequested_
                && Result.snapshot_.completedItems_ > 0 && !Result.snapshot_.skippedItems_ && !Result.snapshot_.failedItems_
                && !Result.snapshot_.error_ && Result.cleanupErrors_.empty()) Saved.view_->RemoveSelection(Saved.keys_);
        }
        if (auto Move = Guard->clipboardMoves_.find(Result.snapshot_.id_); Move != Guard->clipboardMoves_.end()) {
            const auto Token = Move->second; Guard->clipboardMoves_.erase(Move);
            if (Token == Guard->clipboardToken_ && Result.snapshot_.state_ == hc::operations::JobState::Completed
                && Result.snapshot_.completedItems_ > 0 && !Result.snapshot_.skippedItems_ && !Result.snapshot_.failedItems_)
                Guard->CancelClipboard();
        }
        if (Result.snapshot_.error_) hc::plugins::OfferPluginReview(Guard->main_, QString::fromStdString(Result.snapshot_.error_->code_), {}, Guard->viewers_->reviewPlugin_);
        else for (const auto& Item : Result.items_) if (Item.error_ && hc::plugins::OfferPluginReview(Guard->main_, QString::fromStdString(Item.error_->code_), {}, Guard->viewers_->reviewPlugin_)) break;
        if (auto It = Guard->inlineRenameCompletions_.find(Result.snapshot_.correlationId_); It != Guard->inlineRenameCompletions_.end()) {
            auto Completion = std::move(It->second); Guard->inlineRenameCompletions_.erase(It);
            const bool Success = Result.snapshot_.state_ == hc::operations::JobState::Completed
                && Result.snapshot_.completedItems_ > 0 && Result.snapshot_.skippedItems_ == 0 && Result.snapshot_.failedItems_ == 0;
            // Deliver after the refresh below has entered its loading state.
            QTimer::singleShot(0, Guard, [Completion = std::move(Completion), Success] { Completion(Success); });
        }
        const auto Navigation = Guard->createdFolderNavigation_.find(Result.snapshot_.id_);
        if (Navigation != Guard->createdFolderNavigation_.end()) {
            if (Result.snapshot_.state_ != hc::operations::JobState::Completed) Guard->createdFolderNavigation_.erase(Navigation);
            else { Navigation->second.ready_ = true; Guard->finishCreatedFolderNavigation(); }
        }
        if (auto Focus = Guard->createdFileFocus_.find(Result.snapshot_.id_); Focus != Guard->createdFileFocus_.end()) {
            if (Result.snapshot_.state_ == hc::operations::JobState::Completed) Focus->second.ready_ = true;
            else Guard->createdFileFocus_.erase(Focus);
        }
        if(Result.snapshot_.metadata_.options_.starts_with("security-")) {
            auto Path=QDir::fromNativeSeparators(QString::fromStdString(Result.snapshot_.metadata_.source_));if(Path.startsWith("//?/"))Path=Path.mid(4);
            const auto Parent=QFileInfo(Path).absolutePath();
            for(auto* Model:Guard->main_->findChildren<CFSModel*>())if(Model->GetRoot()&&Model->ProviderEnabled()&&!Model->IsLoading()) {
                const auto Current=Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot());
                if(Current.kind_==LocatorKind::Local&&QDir::cleanPath(QString::fromStdString(Current.root_)).compare(Parent,Qt::CaseInsensitive)==0)Model->Refresh();
            }
        }
        if (Result.snapshot_.metadata_.options_.starts_with("rename-record=")) {
            const auto Folder = QDir::cleanPath(QString::fromStdString(Result.snapshot_.metadata_.source_));
            for (auto* Model : Guard->main_->findChildren<CFSModel*>()) if (Model->GetRoot() && Model->ProviderEnabled() && !Model->IsLoading()) {
                const auto Current = Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot());
                if ((Current.kind_ == LocatorKind::Local || Current.kind_ == LocatorKind::Smb)
                    && QDir::cleanPath(QString::fromStdString(Current.root_)).compare(Folder, Qt::CaseInsensitive) == 0) Model->Refresh();
            }
        }
        const auto It = Guard->affectedLocations_.find(Result.snapshot_.id_);
        if (It == Guard->affectedLocations_.end()) return;
        for (auto* Model : Guard->main_->findChildren<CFSModel*>())
            if (Model->GetRoot() && Model->ProviderEnabled() && !Model->IsLoading())
                for (const auto& At : It->second)
                {
                    if (Model->IsSearchResults()) {
                        bool Affected = false;
                        for (int Row = 0; Row < Model->rowCount(); ++Row) {
                            const auto Item = Model->GetFileInfo(Model->index(Row, 0));
                            if (!Item || !Item->storageEntry_ || Item->storageEntry_->locator_.kind_ != At.kind_) continue;
                            const auto Parent = QFileInfo(QString::fromStdString(Item->storageEntry_->locator_.root_)).absolutePath();
                            if (QDir::cleanPath(QString::fromStdString(At.root_)).compare(QDir::cleanPath(Parent), Qt::CaseInsensitive) == 0) { Affected = true; break; }
                        }
                        if (Affected) { Model->Refresh(); break; }
                        continue;
                    }
                    const auto Current = Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot());
                    if (Current.kind_ == At.kind_ && QDir::cleanPath(QString::fromStdString(At.root_)).compare(
                        QDir::cleanPath(QString::fromStdString(Current.root_)), Qt::CaseInsensitive) == 0)
                    { Model->Refresh(); break; }
                }
        Guard->affectedLocations_.erase(It);
    };
    fileJobs_->onResubmitted_ = [this](const std::string& Id, const hc::operations::FileOperationRequest& Request) {
        std::vector<Locator> Affected{Request.destination_};
        for (const auto& Source : Request.sources_) { auto Parent = Source.entry_.locator_; Parent.root_ = QFileInfo(QString::fromStdString(Parent.root_)).absolutePath().toStdString(); Affected.push_back(Parent); }
        affectedLocations_[Id] = std::move(Affected);
    };
    bindModels();
    createActions();
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &CommandUiBridge::updateClipboard);
    updateClipboard();
}

CommandUiBridge::~CommandUiBridge() { clipboardImportStop_.request_stop(); }

void CommandUiBridge::rememberJobSelection(const std::string& Id, const CommandContext& Context)
{
    auto* Model = findModel(Context.tabId_);
    if (!Model || !main_) return;
    std::set<std::string> Ids;
    for (const auto& Entry : Context.selection_) Ids.insert(Serialize(Entry));
    for (auto* View : main_->findChildren<CGridView*>()) if (View->BaseModel() == Model) {
        QStringList Keys;
        for (const auto& Index : View->SelectedSourceIndexes()) {
            const auto Entry = Model->GetFileInfo(Index);
            if (Entry && Entry->storageEntry_ && (Context.selectionManifest_ || Ids.contains(Serialize(Entry->storageEntry_->id_))))
                Keys.append(hc::workspace::SelectionKey(*Entry->storageEntry_));
        }
        if (!Keys.isEmpty()) jobSelections_[Id] = {View, Model, Model->property("hc.snapshotLocation").toString(), std::move(Keys)};
        return;
    }
}

void CommandUiBridge::bindModels()
{
    if (!main_) return;
    for (auto* Model : main_->findChildren<CFSModel*>())
    {
        Model->SetClipboardCut(cutKeys_, cutRoot_);
        if (Model->property("hc.tabId").isNull()) Model->setProperty("hc.tabId", QUuid::createUuid().toString(QUuid::Id128));
        if (Model->property("hc.commandSignals").toBool()) continue;
        Model->setProperty("hc.commandSignals", true);
        const auto Publish = [this, Model](bool Success, const QString& Code) {
            EventEnvelope Event;
            Event.id_ = Success ? "navigation.completed" : "navigation.failed";
            Event.producer_ = "qt-navigation";
            Event.correlationId_ = Model->property("hc.correlation").toString().toStdString();
            Model->setProperty("hc.lastNavigationCorrelation", QString::fromStdString(Event.correlationId_));
            Model->setProperty("hc.lastNavigationError", Success ? QString() : Code);
            Model->setProperty("hc.correlation", QVariant{});
            Event.causationId_ = Event.correlationId_;
            Event.payload_ = {{"tab", Model->property("hc.tabId").toString().toStdString()},
                {"path", Model->GetCurrentPathWithRoot().toStdString()}, {"code", Code.toStdString()}};
            events_->Publish(std::move(Event));
            // A request superseded by the next navigation reports its outcome
            // to automation only; the panel never entered a failed state.
            const bool Visible = !Success && Model->LastNavigationError();
            if (Visible)
                hc::plugins::OfferPluginReview(main_, Code, {}, viewers_->reviewPlugin_);
            if (Visible)
                if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
                {
                    const auto Failure = Model->LastNavigationError().value_or(hc::storage::Error{hc::storage::ErrorCategory::Unavailable, Code.toStdString()});
                    Window->statusBar()->showMessage(hc::ui::DescribeNavigationError(Failure, Model->GetCurrentPathWithRoot()).summary_, 6000);
                }
        };
        connect(Model, &CFsModelT::sigChangedDirectory, this, [Publish](const QString&) { Publish(true, {}); });
        connect(Model, &CFsModelT::sigNavigationFailed, this, [Publish](const QString& Code) { Publish(false, Code); });
    }
}

CommandContext CommandUiBridge::Capture(const QModelIndex& Index)
{
    Q_ASSERT(QThread::currentThread() == thread());
    bindModels();
    const auto Source = SourceIndex(Index);
    auto* Model = Source.isValid() ? qobject_cast<CFSModel*>(const_cast<QAbstractItemModel*>(Source.model())) : PanelModel(activePanel_);
    if (!Model) return {};
    CommandContext Context;
    Context.tabId_ = Model->property("hc.tabId").toString().toStdString();
    Context.location_ = Model->WorkspaceLocation();
    if (Source.isValid())
    {
        const auto Entry = Model->GetFileInfo(Source);
        if (Entry)
            Context.selection_.push_back(Entry->storageEntry_ ? Entry->storageEntry_->id_
                : ResourceId{{"legacy"}, Entry->Name_.toStdString(), IdentityScope::NavigationSession, {}});
    }
    return Context;
}

Task<CommandOutcome> CommandUiBridge::Dispatch(const QString& Id, const QVariantMap& Parameters, const QString& Source, const QModelIndex& Index)
{
    CommandInvocation Invocation;
    Invocation.id_ = Id.toStdString();
    Invocation.context_ = Id.startsWith("hc.clipboard.") && Id != "hc.clipboard.paste" ? captureFiles(Index, false)
        : Id.startsWith("hc.viewer.") ? captureFiles(Index, Id != "hc.viewer.open")
        : Id.startsWith("hc.file.") ? captureFiles(Index, Id == "hc.file.rename") : Capture(Index);
    Invocation.permissions_.insert("navigation");
    if (Id.startsWith("hc.file.") || Id == "hc.clipboard.paste") Invocation.permissions_.insert("file-write");
    Invocation.source_ = Source.toStdString();
    Invocation.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    for (auto It = Parameters.begin(); It != Parameters.end(); ++It)
    {
        if (It.value().metaType().id() == QMetaType::Bool) Invocation.parameters_[It.key().toStdString()] = It.value().toBool();
        else if (It.value().metaType().id() == QMetaType::QString) Invocation.parameters_[It.key().toStdString()] = It.value().toString().toStdString();
        else if (It.value().metaType().id() == QMetaType::Int || It.value().metaType().id() == QMetaType::LongLong)
            Invocation.parameters_[It.key().toStdString()] = std::int64_t(It.value().toLongLong());
        else return Ready<CommandOutcome>(Error{ErrorCategory::InvalidRequest, "unsupported-ui-parameter"});
    }
    if ((Id == "hc.viewer.open" || Id == "hc.file.edit") && !Invocation.parameters_.contains("requestToken")) {
        auto Token = captureDocument(Invocation, findModel(Invocation.context_.tabId_));
        if (!Token.HasValue()) {
            if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
                Window->statusBar()->showMessage(tr("문서를 열 수 없습니다: %1").arg(QString::fromStdString(Token.Failure().code_)), 8000);
            return Ready<CommandOutcome>(Token.Failure());
        }
        Invocation.parameters_["requestToken"] = Token.Value();
    }
    return DispatchInvocation(std::move(Invocation));
}

Task<CommandOutcome> CommandUiBridge::DispatchInvocation(CommandInvocation Invocation) { return dispatcher_->Dispatch(std::move(Invocation)); }

std::shared_ptr<hc::script::IScriptHostApi> CommandUiBridge::ScriptApi(std::set<std::string> Permissions)
{
    return std::make_shared<hc::script::CommandScriptApi>(AutomationHost(),std::move(Permissions));
}

hc::automation::Host CommandUiBridge::AutomationHost()
{
    static const std::set<std::string> Exposed{"hc.navigate.path", "hc.navigate.parent", "hc.navigate.refresh", "hc.navigate.forceRefresh", "hc.navigate.open",
        "hc.file.copy", "hc.file.move", "hc.file.rename", "hc.file.mkdir", "hc.file.delete", "hc.file.new"};
    const auto Initial = captureFiles({}, false);
    const auto Guard = QPointer<CommandUiBridge>(this);
    struct Binding {
        QPointer<CFSModel> model_;
        std::stop_source stop_;
        std::stop_source commandStop_;
        std::unique_ptr<std::stop_callback<std::function<void()>>> runStop_;
        std::unique_ptr<std::stop_callback<std::function<void()>>> ownerStop_;
        QMetaObject::Connection destroyed_;
        ~Binding() { QObject::disconnect(destroyed_); }
    };
    auto Lifetime = std::make_shared<Binding>(); Lifetime->model_ = findModel(Initial.tabId_);
    if (Lifetime->model_) Lifetime->destroyed_ = connect(Lifetime->model_, &QObject::destroyed,
        [Stop = Lifetime->stop_]() mutable { Stop.request_stop(); });
    hc::automation::Host Host;
    Host.script_ = std::make_shared<hc::script::Services>();
    Host.script_->events_ = events_;
    Host.script_->workspace_ = [Guard]() -> Result<hc::script::ScriptValue> {
        using Value = hc::script::ScriptValue;
        if (!Guard || !Guard->main_) return Error{ErrorCategory::Cancelled, "script-workspace-closed"};
        Value::Array Panes;
        for (auto* Panel : Guard->main_->findChildren<CmpPanel*>()) {
            const auto Snapshot = Panel->CaptureWorkspace(); Value::Array Tabs;
            for (const auto& Tab : Snapshot.tabs_) Tabs.emplace_back(Value::Object{{"id", Tab.id_.toStdString()},
                {"path", Tab.location_.root_}, {"location", Serialize(Tab.location_)}, {"provider", Tab.provider_.toStdString()}});
            Panes.emplace_back(Value::Object{{"active", Panel == Guard->activePanel_},
                {"activeTab", std::int64_t(Snapshot.active_ + 1)}, {"tabs", std::move(Tabs)}});
        }
        return Value(Value::Object{{"panes", std::move(Panes)}});
    };
    Host.script_->job_ = [Guard](const CommandInvocation& Invocation, const CommandOutcome& Outcome) -> Result<Parameters> {
        if (!Guard) return Error{ErrorCategory::Cancelled, "script-window-closed"};
        if (!Invocation.id_.starts_with("hc.file.")) return Parameters{{"state", std::string("running")}};
        auto Snapshot = Guard->fileJobs_->Scheduler().Snapshot(Outcome.jobId_); if (!Snapshot.HasValue()) return Snapshot.Failure();
        const auto& Job = Snapshot.Value();
        if (Job.correlationId_ != Invocation.correlationId_) return Error{ErrorCategory::StaleRevision, "script-job-changed"};
        return Parameters{{"state", std::string(hc::operations::StateName(Job.state_))}, {"bytes", std::int64_t(Job.progress_.bytesDone_)},
            {"completed", std::int64_t(Job.completedItems_)}, {"failed", std::int64_t(Job.failedItems_)},
            {"skipped", std::int64_t(Job.skippedItems_)}, {"waitingForUser", Job.interaction_.has_value()}};
    };
    Host.script_->viewer_ = [Guard, Lifetime](const CommandContext& Context, const ResourceId& Id) -> Result<hc::script::ViewerAccess> {
        using Value = hc::script::ScriptValue;
        auto* Model = Guard ? Guard->findModel(Context.tabId_) : nullptr;
        if (!Model || Model != Lifetime->model_ || Model->WorkspaceLocation() != Context.location_)
            return Error{ErrorCategory::StaleRevision, "script-context-changed"};
        auto Reader = Model->SnapshotProvider();
        if (Model->IsLoading() || !Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unavailable, "script-viewer-source"};
        std::optional<EntrySnapshot> Selected;
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
            if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Id) { Selected = *Entry->storageEntry_; break; }
        }
        if (!Selected || Selected->directory_) return Error{ErrorCategory::StaleRevision, "script-viewer-resource"};
        Reader = Model->ProviderFor(*Selected);
        QPointer<QDialog> Window = Guard->viewers_->OpenFull(std::make_shared<ProviderContentSource>(std::move(Reader), *Selected));
        QPointer<QtViewerSurface> Surface = Window->findChild<QtViewerSurface*>();
        if (!Surface) { Window->close(); return Error{ErrorCategory::Internal, "script-viewer-surface"}; }
        const auto Session = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        const auto Publish = [Events = Guard->events_, Session](std::string Id, std::string Code = {}) {
            EventEnvelope Event; Event.id_ = std::move(Id); Event.producer_ = "script-viewer"; Event.correlationId_ = Session;
            Event.payload_ = {{"session", Session}, {"code", std::move(Code)}}; Events->Publish(std::move(Event));
        };
        connect(Surface, &QtViewerSurface::Ready, Window, [Publish](const QString&) { Publish("viewer.ready"); });
        connect(Surface, &QtViewerSurface::Failed, Window, [Publish](const QString& Code) { Publish("viewer.failed", Code.toStdString()); });
        connect(Window, &QDialog::finished, Window, [Publish](int) { Publish("viewer.closed"); });
        hc::script::ViewerAccess Access;
        Access.snapshot_ = [Surface, Window, Session]() -> Result<Value> {
            if (!Surface || !Window || !Window->isVisible()) return Error{ErrorCategory::StaleRevision, "script-viewer-closed"};
            const auto Snapshot = Surface->Snapshot();
            const auto Status = Surface->IsReady() ? "ready" : Snapshot.status_ == hc::viewer::ViewerStatus::Failed ? "failed"
                : Snapshot.status_ == hc::viewer::ViewerStatus::Cancelled || Snapshot.status_ == hc::viewer::ViewerStatus::Closed ? "closed" : "loading";
            return Value(Value::Object{{"session", Session}, {"state", Status}, {"name", Snapshot.source_.name_},
                {"provider", Snapshot.provider_}, {"bytes", std::int64_t(Snapshot.bytesRead_)}, {"error", Snapshot.error_ ? Snapshot.error_->code_ : std::string{}}});
        };
        Access.execute_ = [Surface, Window](std::string_view Command) -> Result<Unit> {
            if (!Surface || !Window || !Window->isVisible()) return Error{ErrorCategory::StaleRevision, "script-viewer-closed"};
            if (!Surface->IsReady()) return Error{ErrorCategory::Unavailable, "script-viewer-not-ready"};
            Surface->Execute(QString::fromUtf8(Command.data(), Command.size())); return Unit{};
        };
        Access.close_ = [Window] { if (Window) Window->close(); };
        return Access;
    };
    Host.capture_ = [Guard, Initial, Lifetime]() -> Result<CommandContext> {
        auto* Model = Guard ? Guard->findModel(Initial.tabId_) : nullptr;
        if (!Model || Model != Lifetime->model_) return Error{ErrorCategory::Cancelled, "automation-tab-closed"};
        if (PanelModel(PanelOf(Model)) != Model) return Error{ErrorCategory::Cancelled, "automation-tab-not-active"};
        auto Context = Initial; Context.location_ = Model->WorkspaceLocation();
        // Selection handles are captured once, never rebound to a later UI selection.
        if (!(Context.location_ == Initial.location_)) Context.selection_.clear();
        return Context;
    };
    Host.dispatch_ = [Guard, Lifetime](CommandInvocation Invocation) -> Task<CommandOutcome> {
        if (!Guard) return Ready<CommandOutcome>(Error{ErrorCategory::Cancelled, "automation-window-closed"});
        if (!Lifetime->model_) return Ready<CommandOutcome>(Error{ErrorCategory::Cancelled, "automation-tab-closed"});
        // Interactive dialogs, tab switching and viewers need separate lifetime
        // facades. This first DSL stage exposes navigation and M4 file jobs only.
        if (!Exposed.contains(Invocation.id_)) return Ready<CommandOutcome>(Error{ErrorCategory::Unsupported, "automation-command-not-exposed"});
        // The saved workspace may recreate the same tab ID before a queued
        // dispatch executes. Cancel that dispatch when its original model dies.
        Lifetime->runStop_.reset(); Lifetime->ownerStop_.reset(); Lifetime->commandStop_ = std::stop_source{};
        Lifetime->runStop_ = std::make_unique<std::stop_callback<std::function<void()>>>(Invocation.operation_.stopToken_,
            [Stop = Lifetime->commandStop_]() mutable { Stop.request_stop(); });
        Lifetime->ownerStop_ = std::make_unique<std::stop_callback<std::function<void()>>>(Lifetime->stop_.get_token(),
            [Stop = Lifetime->commandStop_]() mutable { Stop.request_stop(); });
        Invocation.operation_.stopToken_ = Lifetime->commandStop_.get_token();
        return Guard->DispatchInvocation(std::move(Invocation));
    };
    Host.poll_ = [Guard, Lifetime](const CommandInvocation& Invocation, const CommandOutcome& Outcome) -> Result<bool> {
        if (!Guard) return Error{ErrorCategory::Cancelled, "automation-window-closed"};
        auto* Model = Guard->findModel(Invocation.context_.tabId_);
        if (!Model || Model != Lifetime->model_) return Error{ErrorCategory::Cancelled, "automation-tab-closed"};
        if (Invocation.id_.starts_with("hc.file.")) {
            const auto Job = Guard->fileJobs_->Scheduler().Snapshot(Outcome.jobId_);
            if (!Job.HasValue()) return Job.Failure();
            const auto& Snapshot = Job.Value();
            if (Snapshot.correlationId_ != Invocation.correlationId_) return Error{ErrorCategory::StaleRevision, "automation-job-changed"};
            if (!hc::operations::IsTerminal(Snapshot.state_)) return false;
            if (Snapshot.error_) return *Snapshot.error_;
            if (Snapshot.state_ == hc::operations::JobState::Cancelled) return Error{ErrorCategory::Cancelled, "automation-job-cancelled"};
            if (Snapshot.state_ != hc::operations::JobState::Completed || Snapshot.failedItems_ || Snapshot.skippedItems_)
                return Error{ErrorCategory::Internal, "automation-job-incomplete"};
            if (Guard->fileJobs_->IsUiPending(Outcome.jobId_) || Model->IsLoading()) return false;
            return true;
        }
        const auto Current = Model->property("hc.correlation").toString().toStdString();
        if (Model->IsLoading() && Current != Outcome.jobId_)
            return Error{ErrorCategory::Cancelled, "automation-navigation-replaced"};
        if (Model->property("hc.lastNavigationCorrelation").toString().toStdString() == Outcome.jobId_) {
            const auto Code = Model->property("hc.lastNavigationError").toString();
            if (!Code.isEmpty()) return Error{ErrorCategory::Unavailable, Code.toStdString()};
            return true;
        }
        if (Current != Outcome.jobId_)
            return Error{ErrorCategory::Cancelled, "automation-navigation-replaced"};
        return false;
    };
    Host.cancel_ = [Guard](const CommandInvocation& Invocation, const CommandOutcome& Outcome) {
        if (!Guard) return;
        if (Invocation.id_.starts_with("hc.file.") && !Outcome.jobId_.empty()) {
            const auto Job = Guard->fileJobs_->Scheduler().Snapshot(Outcome.jobId_);
            if (Job.HasValue() && Job.Value().correlationId_ == Invocation.correlationId_) Guard->fileJobs_->Scheduler().Cancel(Outcome.jobId_);
        } else if (Invocation.id_.starts_with("hc.navigate.")) {
            auto* Model = Guard->findModel(Invocation.context_.tabId_);
            if (Model && Model->property("hc.correlation").toString().toStdString() == Invocation.correlationId_) Model->CancelNavigation();
        }
    };
    Host.schema_ = [Registry = registry_](const std::string& Id) -> Result<CommandDescriptor> {
        if (!Exposed.contains(Id)) return Error{ErrorCategory::Unsupported, "automation-command-not-exposed"};
        const auto Registered = Registry->Resolve(Id); if (!Registered.HasValue()) return Registered.Failure();
        return Registered.Value()->descriptor_;
    };
    Host.output_ = [Guard, Lifetime](const CommandInvocation& Invocation, const CommandOutcome& Outcome) -> Result<Parameters> {
        if (!Guard || !Lifetime->model_) return Error{ErrorCategory::Cancelled, "automation-tab-closed"};
        Parameters Values{{"path", Lifetime->model_->WorkspaceLocation().root_}};
        if (const auto Destination = Invocation.parameters_.find("destination"); Destination != Invocation.parameters_.end()) Values["destination"] = Destination->second;
        if (Invocation.id_.starts_with("hc.file.") && !Outcome.jobId_.empty()) {
            const auto Job = Guard->fileJobs_->Scheduler().Snapshot(Outcome.jobId_); if (!Job.HasValue()) return Job.Failure();
            Values["completedItems"] = std::int64_t(Job.Value().completedItems_);
        }
        return Values;
    };
    return Host;
}

bool CommandUiBridge::DispatchFolder(const QModelIndex& Index)
{
    const auto Source = SourceIndex(Index);
    auto* Model = qobject_cast<CFSModel*>(const_cast<QAbstractItemModel*>(Source.model()));
    if (!Model || !Source.isValid()) return false;
    const auto Entry = Model->GetFileInfo(Source);
    if (!Entry || !(Entry->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) return false;
    Dispatch("hc.navigate.open", {}, "shortcut", Index);
    return true;
}

CFSModel* CommandUiBridge::findModel(const std::string& TabId) const
{
    if (!main_ || TabId.empty()) return nullptr;
    for (auto* Model : main_->findChildren<CFSModel*>())
        if (Model->property("hc.tabId").toString().toStdString() == TabId) return Model;
    return nullptr;
}

Result<CommandOutcome> CommandUiBridge::execute(const CommandInvocation& Invocation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (Invocation.id_ == "hc.viewer.close" || (Invocation.id_ == "hc.viewer.quick" && quick_))
    { closeQuick(); return CommandOutcome{}; }
    auto* Model = findModel(Invocation.context_.tabId_);
    if (!Model) return Error{ErrorCategory::Cancelled, "tab-closed"};
    if ((!Model->GetRoot() || Model->HasNavigationFallback())
        && (Invocation.id_.starts_with("hc.file.") || Invocation.id_.starts_with("hc.viewer.") || Invocation.id_.starts_with("hc.clipboard.")))
        return Error{ErrorCategory::Unavailable, "tab-not-ready"};
    if (!Model->GetRoot() && !Model->HasNavigationFallback()
        && (Invocation.id_ == "hc.navigate.parent" || Invocation.id_ == "hc.navigate.open"))
        return Error{ErrorCategory::Unavailable, "tab-not-ready"};
    auto* Panel = PanelOf(Model);
    if (!Panel || PanelModel(Panel) != Model) return Error{ErrorCategory::Cancelled, "tab-not-active"};
    const auto& Id = Invocation.id_;
    if (Id.starts_with("hc.clipboard.")) return executeClipboard(Invocation, Model);
    const bool DriveRoot = Id.starts_with("hc.navigate.gotoDrive");
    const bool NavigateToPath = Id == "hc.navigate.path" || DriveRoot;
    QString Path;
    if (DriveRoot) Path = QString(QChar::fromLatin1(Id.back())) + ":\\";
    else if (Id == "hc.navigate.path")
    {
        Path = QString::fromStdString(std::get<std::string>(Invocation.parameters_.at("path")));
        if (Path.size() == 2 && Path.endsWith(':')) Path += '\\';
        if (!QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "absolute-path-required"};
        Path = QDir::toNativeSeparators(QDir::cleanPath(Path));
    }
    if (!NavigateToPath && !(Invocation.context_.location_ == Model->WorkspaceLocation()))
        return Error{ErrorCategory::StaleRevision, "navigation-context-changed"};
    if (Id.starts_with("hc.navigate.") && Id != "hc.navigate.refresh" && Id != "hc.navigate.forceRefresh" && Id != "hc.navigate.cancel" && Model->IsSearchResults())
        Panel->StopEverythingSearch(false);
    if (Id == "hc.viewer.open" || Id == "hc.file.edit") return executeDocument(Invocation, Model);
    if (Id == "hc.file.compare") {
        const auto Arguments = compareArguments(Invocation.context_, Model);
        if (!Arguments.HasValue()) return Arguments.Failure();
        const auto Root = QCoreApplication::applicationDirPath();
        const auto Program = QDir(Root).filePath("HyperCompare.exe");
        if (!QFileInfo::exists(Program)) return Error{ErrorCategory::NotFound, "compare-program-not-installed"};
        if (!QProcess::startDetached(Program, Arguments.Value(), Root)) return Error{ErrorCategory::Unavailable, "compare-launch-failed"};
        return CommandOutcome{};
    }
    if (Id.starts_with("hc.file.")) return executeFile(Invocation, Model);
    if (Id.starts_with("hc.viewer.")) return executeViewer(Invocation, Model);
    if ((Id.starts_with("hc.navigate.") && Id != "hc.navigate.cancel") || Id == "hc.tab.close")
        if (Model->IsLoading()) Model->CancelNavigation(Id != "hc.tab.close");
    if (Id.starts_with("hc.navigate.") && Id != "hc.navigate.cancel")
        Model->setProperty("hc.correlation", QString::fromStdString(Invocation.correlationId_));
    if (NavigateToPath)
    {
        Panel->ChangeCurrentPath(Path, {});
        if (!Model->IsLoading() && Model->GetCurrentPathWithRoot().compare(Path, Qt::CaseInsensitive) != 0)
            return Error{ErrorCategory::NotFound, "path-not-available"};
    }
    else if (Id == "hc.navigate.refresh") { Model->setProperty("hc.explicitIconRefresh", true); Model->Refresh(); }
    else if (Id == "hc.navigate.forceRefresh") Model->ForceRefresh();
    else if (Id == "hc.navigate.parent") Model->ChangeDirectory("..");
    else if (Id == "hc.navigate.cancel") Model->CancelNavigation();
    else if (Id == "hc.navigate.provider")
    {
        if (Invocation.context_.location_.kind_ == LocatorKind::Plugin && !std::get<bool>(Invocation.parameters_.at("enabled")))
            return Error{ErrorCategory::Unsupported, "plugin-requires-provider"};
        Model->SetProviderEnabled(std::get<bool>(Invocation.parameters_.at("enabled")));
    }
    else if (Id == "hc.navigate.open" || Id == "hc.navigate.enterFile")
    {
        if (Invocation.context_.selection_.size() != 1) return Error{ErrorCategory::InvalidRequest, "single-selection-required"};
        const auto& Selected = Invocation.context_.selection_.front();
        QModelIndex Target;
        for (int Row = 0; Row < Model->rowCount(); ++Row)
        {
            const auto Index = Model->index(Row, 0);
            const auto Entry = Model->GetFileInfo(Index);
            if ((Entry->storageEntry_ && Entry->storageEntry_->id_ == Selected)
                || (!Entry->storageEntry_ && Selected.providerId_.value_ == "legacy" && Entry->Name_.toStdString() == Selected.key_))
            {
                if (!(Entry->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (Id != "hc.navigate.enterFile" || TyStPlugInMgr::GetInstance()->RetrieveWCXByExts(Entry->Ext_, Entry->Name_).isEmpty())
                        return Error{ErrorCategory::Unsupported, "file-container-not-supported"};
                }
                Target = Index;
                break;
            }
        }
        if (!Target.isValid()) return Error{ErrorCategory::StaleRevision, "selection-changed"};
        Model->ChangeDirectory(Target);
    }
    else if (Id == "hc.tab.new") { Panel->AddTab(); bindModels(); return CommandOutcome{}; }
    else if (Id == "hc.tab.close") { Panel->CloseTab(); return CommandOutcome{}; }
    else if (Id == "hc.tab.next") { Panel->NextTab(); return CommandOutcome{}; }
    else if (Id == "hc.tab.previous") { Panel->PrevTab(); return CommandOutcome{}; }
    else if (Id == "hc.view.columns")
    {
        if (const auto View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) {
            QMainOpts Settings(main_); Settings.SetListView(View); Settings.ShowListSettingsPage(); Settings.exec();
        }
        return CommandOutcome{};
    }
    else return Error{ErrorCategory::NotFound, "unknown-command"};
    return CommandOutcome{Model->IsLoading() ? CommandOutcome::Status::Accepted : CommandOutcome::Status::Completed,
        Model->IsLoading() ? Invocation.correlationId_ : std::string{}};
}

void CommandUiBridge::createActions()
{
    auto* Window = qobject_cast<QMainWindow*>(main_.data());
    if (!Window) return;
    auto* Menu = Window->findChild<QMenuBar*>("menubar")->addMenu(tr("탐색"));
    auto* Toolbar = Window->addToolBar(tr("탐색 및 파일 작업"));
    Toolbar->setObjectName("hc.navigationToolbar");
    Toolbar->setMovable(false); Toolbar->setFloatable(false); Toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    Toolbar->setIconSize(QSize(16, 16));
    Toolbar->setVisible(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["showMainToolbar"].toBool(false));
    const std::vector<std::pair<QString, QString>> Actions{
        {"hc.navigate.path", tr("경로 이동")}, {"hc.navigate.parent", tr("상위 폴더")},
        {"hc.navigate.forceRefresh", tr("강제 새로 고침")}, {"hc.navigate.refresh", tr("새로 고침")}, {"hc.navigate.cancel", tr("중지")},
        {"hc.navigate.enterFile", tr("파일 내부로 들어가기")}
    };
    for (const auto& [Id, Label] : Actions)
    {
            auto* Action = new QAction(Label, this);
            Action->setObjectName("hc.menu." + Id);
            Menu->addAction(Action); Toolbar->addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Id] {
                QVariantMap Parameters;
                if (Id == "hc.navigate.path")
                {
                    bool Accepted = false;
                    const auto Context = Capture();
                    const auto Path = hc::ui::InputDialog::getText(main_, tr("폴더 이동"), tr("경로"), QLineEdit::Normal,
                        QString::fromStdString(Context.location_.root_), &Accepted);
                    if (!Accepted) return;
                    Parameters["path"] = Path;
                }
                Dispatch(Id, Parameters, "ui", Id == "hc.navigate.enterFile" ? commandIndex() : QModelIndex{});
            });
    }
    auto* Compatibility = Menu->addAction(tr("호환 탐색"));
    Compatibility->setObjectName("hc.menu.compatibility");
    Compatibility->setCheckable(true);
    connect(Menu, &QMenu::aboutToShow, this, [this, Compatibility] {
        if (const auto Model = PanelModel(activePanel_)) Compatibility->setChecked(!Model->ProviderEnabled());
    });
    connect(Compatibility, &QAction::triggered, this, [this](bool Checked) {
        Dispatch("hc.navigate.provider", {{"enabled", !Checked}}, "menu");
    });
    auto* Files = Window->findChild<QMenuBar*>("menubar")->addMenu(tr("파일 작업"));
    auto* Editors = Files->addAction(tr("외부 편집기 설정…")); Editors->setObjectName("hc.document.configure");
    connect(Editors, &QAction::triggered, this, [this] { TyStExternalEditorMgr::GetInstance()->Refresh(); CExternalConfigure Dialog(main_); Dialog.exec(); });
    auto* Viewers = Window->findChild<QMenuBar*>("menubar")->addMenu(tr("뷰어"));
    for (const auto& Item : std::vector<std::pair<QString, QString>>{
        {"hc.viewer.open", tr("문서 보기 · 프로그램 선택")}, {"hc.file.edit", tr("문서 편집 · 프로그램 선택")}, {"hc.viewer.quick", tr("반대 패널 미리 보기")}, {"hc.viewer.close", tr("미리 보기 닫기")}})
    {
        auto* Action = Viewers->addAction(Item.second); Action->setObjectName("hc.menu." + Item.first);
        connect(Action, &QAction::triggered, this, [this, Id = Item.first] { Dispatch(Id, {}, "ui", commandIndex()); });
    }
    const std::vector<std::pair<QString, QString>> FileActions{
        {"hc.file.copy", tr("복사")}, {"hc.file.move", tr("이동")}, {"hc.file.rename", tr("이름 변경")},
        {"hc.file.mkdir", tr("새 폴더")}, {"hc.file.delete", tr("삭제")}
    };
    for (const auto& [Id, Label] : FileActions)
    {
        auto* Action = Files->addAction(Label);
        Action->setObjectName("hc.menu." + Id);
        connect(Action, &QAction::triggered, this, [this, Id] { PromptFileOperation(Id, "ui", commandIndex()); });
    }
    auto* NewFile = Files->addAction(tr("새 파일…")); NewFile->setObjectName("hc.menu.file.new");
    auto* RenameBatch = Files->addAction(tr("다중 이름 변경…")); RenameBatch->setObjectName("hc.menu.file.batchRename");
    connect(NewFile, &QAction::triggered, this, [this] { PromptFileTools(false); });
    connect(RenameBatch, &QAction::triggered, this, [this] { PromptFileTools(true); });
    auto* Permissions=Files->addAction(tr("선택 항목 권한 확인…")); Permissions->setObjectName("hc.menu.permissions");
    connect(Permissions,&QAction::triggered,this,[this] {
        const auto Context=captureFiles({},false); auto* Model=findModel(Context.tabId_); if(!Model||(!Model->IsSearchResults()&&Context.location_.kind_!=LocatorKind::Local))return;
        QStringList Paths; for(int Row=0;Row<Model->rowCount()&&Paths.size()<256;++Row){const auto E=Model->GetFileInfo(Model->index(Row,0));if(E&&E->storageEntry_&&std::find(Context.selection_.begin(),Context.selection_.end(),E->storageEntry_->id_)!=Context.selection_.end())Paths.append(Model->GetFileFullPath(Model->index(Row,0)));}
        if(Paths.isEmpty()&&!Model->IsSearchResults())Paths.append(QString::fromStdString(Context.location_.root_)); if(!Paths.isEmpty())hc::ui::ShowPermissionInspection(Paths,main_);
    });
    auto* Jobs = Files->addAction(tr("작업 목록"));
    Jobs->setObjectName("hc.menu.jobs.show");
    connect(Jobs, &QAction::triggered, this, [this] { fileJobs_->Show(); });
    Files->addSeparator();
    auto* CreateArchive = Files->addAction(tr("파일 압축…")); CreateArchive->setObjectName("hc.menu.archive.create");
    connect(CreateArchive, &QAction::triggered, this, [this] { openArchive(); });
    auto* ExtractArchive = Files->addAction(tr("파일 압축 해제…")); ExtractArchive->setObjectName("hc.menu.archive.extract");
    connect(ExtractArchive, &QAction::triggered, this, [this] { extractArchive(); });
    auto* CreateIso = Files->addAction(tr("선택 항목으로 ISO 생성…")); CreateIso->setObjectName("hc.menu.iso.create");
    auto* InspectIso = Files->addAction(tr("ISO 구조 확인…")); InspectIso->setObjectName("hc.menu.iso.inspect");
    connect(CreateIso, &QAction::triggered, this, [this] { openIso(false); });
    connect(InspectIso, &QAction::triggered, this, [this] { openIso(true); });
    auto* Plugins = Window->findChild<QMenuBar*>("menubar")->addMenu(tr("플러그인"));
    auto* Storage = Plugins->addAction(tr("파일 시스템 열기…")); Storage->setObjectName("hc.menu.wfx.open");
    auto* Metadata = Plugins->addAction(tr("선택 파일 정보…")); Metadata->setObjectName("hc.menu.wdx.query");
    auto* Search = Menu->addAction(tr("범위 검색…")); Search->setObjectName("hc.menu.search");
    connect(Search, &QAction::triggered, this, [this] { openSearch(); });
    hc::automation::Install(Window,hc::settings::AppSettings(),[Guard = QPointer<CommandUiBridge>(this)] { return Guard ? Guard->AutomationHost() : hc::automation::Host{}; });
    auto* DopusStorage = Plugins->addAction(tr("DOpus 파일 시스템 열기…")); DopusStorage->setObjectName("hc.menu.dopus.open");
    auto* DopusMetadata = Plugins->addAction(tr("DOpus 선택 파일 정보…")); DopusMetadata->setObjectName("hc.menu.dopus.query");
    connect(DopusStorage, &QAction::triggered, this, [this] { openDataPlugin(false, true); });
    connect(DopusMetadata, &QAction::triggered, this, [this] { openDataPlugin(true, true); });
    connect(Storage, &QAction::triggered, this, [this] { openDataPlugin(false); });
    connect(Metadata, &QAction::triggered, this, [this] { openDataPlugin(true); });
    createCommandUi();
}

QAction* CommandUiBridge::Action(const QString& Id) const
{
    return actions_.value(hc::ui::CommandId(Id));
}

QModelIndex CommandUiBridge::commandIndex() const
{
    if (commandIndex_.isValid()) return commandIndex_;
    const auto* Panel = activePanel_.data();
    auto* View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
    if (!View || !View->BaseView()) return {};
    const auto Row = View->BaseView()->focusedRow();
    return Row.isValid() && Row.type() == Qtitan::GridRow::DataRow ? Row.modelIndex(0) : QModelIndex{};
}

bool CommandUiBridge::ExecuteCommand(const QString& Id, const QModelIndex& Index)
{
    const QScopedValueRollback<QModelIndex> Context(commandIndex_, Index);
    if (auto* Command = Action(Id)) {
        if (Command->isEnabled()) Command->trigger();
        return true;
    }
    return false;
}

void CommandUiBridge::ShowShellMenu(const QModelIndex& Index)
{
    const auto Context = Capture(Index);
    const auto Model = findModel(Context.tabId_);
    const auto Panel = Model ? PanelOf(Model) : nullptr;
    const auto View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
    if (!View || !View->ShowShellMenu(Index))
        if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
            Window->statusBar()->showMessage(tr("이 위치 또는 선택 항목에서는 탐색기 메뉴를 열 수 없습니다."), 6000);
}

void CommandUiBridge::RefreshFunctionBar() {
    auto* Window = qobject_cast<QMainWindow*>(main_.data()); if (!Window) return;
    auto* Layout = qobject_cast<QVBoxLayout*>(Window->centralWidget()->layout()); if (!Layout) return;
    auto* Old = Window->findChild<fm::ui::FunctionKeyBar*>("hc.functionBar");
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    const auto Desired = hc::ui::FunctionCommands(General); QStringList Current;
    if (Old) for (auto* Button : Old->buttons()) Current.append(Button->property("hc.commandId").toString());
    if (Old && Current == Desired) {
        for (int Slot = 0; Slot < Desired.size(); ++Slot) {
            auto* Command = Action(Desired[Slot]);
            const auto Label = Command ? Command->property("hc.baseLabel").toString() : Desired[Slot];
            hc::ui::SetFunctionCaption(Old->buttons()[Slot], Command, hc::ui::FunctionCaption(General, Slot, Desired[Slot], Label));
        }
        Old->setVisible(General["showFunctionBar"].toBool(true)); return;
    }
    if (Old) { Layout->removeWidget(Old); delete Old; }
    auto* Bar = new fm::ui::FunctionKeyBar(Window->centralWidget()); Bar->setObjectName("hc.functionBar");
    for (int Slot = 0; Slot < Desired.size(); ++Slot) {
        const auto& Id = Desired[Slot];
        auto* Command = Action(Id); const auto Label = Command ? Command->property("hc.baseLabel").toString() : Id;
        auto* Button = Bar->addKey({}, hc::ui::FunctionCaption(General, Slot, Id, Label)); Button->setObjectName("hc.function." + Id); Button->setProperty("hc.commandId", Id);
        if (Command) hc::ui::BindFunctionButton(Button, Command); else Button->setEnabled(false);
    }
    Layout->addWidget(Bar); Bar->setVisible(hc::settings::AppSettings().Read(hc::settings::Section::General).toObject()["showFunctionBar"].toBool(true));
}

bool CommandUiBridge::ReloadMenu() { return !userMenu_ || userMenu_->Reload(); }

void CommandUiBridge::RefreshShortcutLabels()
{
    const auto Values = TyStCommandMgr::GetInstance()->Retrieve();
    for (auto It = actions_.begin(); It != actions_.end(); ++It) {
        if (!It.value()) continue;
        auto* Command = It.value().data();
        const auto Keys = hc::ui::CommandKeys(hc::ui::IsCommandLineShortcut(It.key())
            ? TyStCommandMgr::GetInstance()->RetrieveForCommand(It.key()) : Values, It.key());
        QStringList Labels;
        for (const auto& Key : Keys) Labels.append(Key.toString(QKeySequence::NativeText));
        const auto Label = Labels.value(0);
        const auto Base = Command->property("hc.baseLabel").toString();
        Command->setProperty("hc.keyLabel", Label);
        Command->setToolTip(Base + (Labels.isEmpty() ? tr(" · 단축키 없음") : " · " + Labels.join(", ")));
        Command->setText(Base + (Label.isEmpty() ? QString{} : "\t" + Label));
    }
    if (main_) main_->setProperty("hc.shortcutRevision", main_->property("hc.shortcutRevision").toInt() + 1);
    if (auto* Find = main_ ? main_->findChild<fm::ui::FindBox*>("hc.mainFind") : nullptr) {
        const auto* Focus = Action("hc.search.focus");
        const auto Key = Focus ? Focus->property("hc.keyLabel").toString() : QString{};
        Find->setPlaceholderText(tr("현재 폴더에서 찾기") + (Key.isEmpty() ? QString{} : "  " + Key));
    }
    if (const auto* NewTab = Action("hc.tab.new"))
        for (auto* Strip : main_->findChildren<fm::ui::PanelTabStrip*>()) Strip->newTabButton()->setToolTip(NewTab->toolTip());
}

void CommandUiBridge::finishCreatedFolderNavigation()
{
    for (auto It = createdFolderNavigation_.begin(); It != createdFolderNavigation_.end();) {
        const auto& Navigation = It->second;
        if (!Navigation.ready_) { ++It; continue; }
        auto* Model = findModel(Navigation.tab_);
        if (!Model || Model != Navigation.model_ || !Model->GetRoot() || PanelModel(PanelOf(Model)) != Model
            || Model->WorkspaceLocation() != Navigation.source_) { It = createdFolderNavigation_.erase(It); continue; }
        // A watcher refresh can overlap job completion. Keep the intent until
        // it settles, but never navigate a replaced tab/model or changed root.
        if (Model->IsLoading()) { ++It; continue; }
        const auto Target = Navigation.target_; It = createdFolderNavigation_.erase(It);
        PanelOf(Model)->ChangeCurrentPath(QString::fromStdString(Target.root_), {});
    }
}

void CommandUiBridge::finishCreatedFileFocus()
{
    for (auto It = createdFileFocus_.begin(); It != createdFileFocus_.end();) {
        const auto& Intent = It->second;
        if (!Intent.ready_) { ++It; continue; }
        auto* Model = findModel(Intent.tab_);
        if (!Model || Model != Intent.model_ || !Model->GetRoot() || Model->WorkspaceLocation() != Intent.source_
            || PanelModel(PanelOf(Model)) != Model) { It = createdFileFocus_.erase(It); continue; }
        if (Model->IsLoading()) { ++It; continue; }
        const auto Panel = PanelOf(Model); const auto View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        for (int Row = 0; View && Row < Model->rowCount(); ++Row) {
            const auto Source = Model->index(Row,0); const auto Entry = Model->GetFileInfo(Source);
            if (!Entry || Entry->Name_ != Intent.name_) continue;
            if (Entry->storageEntry_) {
                const auto Key = hc::workspace::SelectionKey(*Entry->storageEntry_); View->RestoreSelection({Key}, Key);
            }
            break;
        }
        It = createdFileFocus_.erase(It);
    }
}

void CommandUiBridge::refreshActionState()
{
    finishCreatedFolderNavigation();
    finishCreatedFileFocus();
    if (auto* Back = Action("hc.navigate.back")) Back->setEnabled(activePanel_ && activePanel_->CanNavigateHistory(-1));
    if (auto* Forward = Action("hc.navigate.forward")) Forward->setEnabled(activePanel_ && activePanel_->CanNavigateHistory(1));
    const auto* Model = PanelModel(activePanel_);
    if (auto* Compatibility = Action("hc.navigate.compatibility")) Compatibility->setChecked(Model && !Model->ProviderEnabled());
    const bool Ready = Model && Model->GetRoot() && !Model->HasNavigationFallback() && !Model->IsLoading();
    for (auto It = actions_.cbegin(); It != actions_.cend(); ++It)
        if (It.key().startsWith("hc.selection.") && It.value()) It.value()->setEnabled(Ready);
    if (auto* Command = Action("hc.file.folderSize")) Command->setEnabled(Ready && Model->ProviderEnabled()
        && (Model->IsSearchResults() || !hc::folders::IsNetwork(Model->WorkspaceLocation())));
    for (const auto* Id : {"hc.file.shellMenu", "hc.file.context"})
        if (auto* Command = Action(Id)) Command->setEnabled(Ready && (Model->WorkspaceLocation().kind_ == LocatorKind::Local || Model->WorkspaceLocation().kind_ == LocatorKind::Smb));
    for (const auto* Id : {"hc.file.copy", "hc.file.move", "hc.file.rename", "hc.file.mkdir", "hc.file.delete", "hc.file.new", "hc.file.batchRename",
        "hc.file.compare", "hc.file.edit", "hc.viewer.open", "hc.viewer.quick", "hc.file.permissions", "hc.archive.create", "hc.archive.extract", "hc.iso.create"})
        if (auto* Command = Action(Id)) Command->setEnabled(Ready);
    // These legacy handlers are still empty. Keep their IDs for old settings,
    // but don't advertise an operation which cannot change anything yet.
    for (const auto* Id : {"hc.file.normalize"}) if (auto* Command = Action(Id)) Command->setEnabled(false);
    for (const auto* Id : {"hc.file.new", "hc.file.mkdir"})
        if (auto* Command = Action(Id)) Command->setEnabled(Ready && !Model->IsSearchResults());
    const bool ClipboardSource = Ready && Model->ProviderEnabled() && Model->SnapshotProvider();
    if (auto* Command = Action("hc.clipboard.copy")) Command->setEnabled(ClipboardSource);
    if (auto* Command = Action("hc.clipboard.cut")) Command->setEnabled(ClipboardSource
        && (Model->IsSearchResults() || Model->WorkspaceLocation().kind_ == LocatorKind::Local
            || Model->WorkspaceLocation().kind_ == LocatorKind::Smb
            || (Model->WorkspaceLocation().kind_ == LocatorKind::Plugin && Model->GetRoot()->WriteProvider())));
    if (auto* Command = Action("hc.clipboard.paste")) Command->setEnabled(ClipboardSource && clipboard_
        && !Model->IsSearchResults() && (Model->WorkspaceLocation().kind_ == LocatorKind::Local
            || Model->WorkspaceLocation().kind_ == LocatorKind::Smb
            || (Model->WorkspaceLocation().kind_ == LocatorKind::Plugin && Model->GetRoot()->WriteProvider())));
    if (auto* Command = Action("hc.search.openLocation")) Command->setEnabled(Ready && Model->IsSearchResults());
    if (auto* Command = Action("hc.file.attributes")) {
        const bool Native = Ready && (Model->WorkspaceLocation().kind_ == LocatorKind::Local || Model->WorkspaceLocation().kind_ == LocatorKind::Smb);
        Command->setEnabled(Native);
        Command->setToolTip(Native ? tr("선택한 파일·폴더의 속성과 수정 시각을 변경합니다.") : tr("로컬 또는 SMB 파일·폴더에서 사용할 수 있습니다."));
    }
}

void CommandUiBridge::createCommandUi()
{
    auto* Window = qobject_cast<QMainWindow*>(main_.data());
    if (!Window) return;
    // Adopt existing actions rather than reconnecting their operations. Dynamic
    // plugin and automation menus keep their hosts, including script reloads.
    const QHash<QString, QString> Names{
        {"hc.menu.file.new", "hc.file.new"}, {"hc.menu.file.batchRename", "hc.file.batchRename"},
        {"hc.menu.permissions", "hc.file.permissions"}, {"hc.menu.jobs.show", "hc.jobs.show"},
        {"hc.menu.archive.create", "hc.archive.create"}, {"hc.menu.archive.extract", "hc.archive.extract"}, {"hc.menu.iso.create", "hc.iso.create"}, {"hc.menu.iso.inspect", "hc.iso.inspect"},
        {"hc.menu.search", "hc.search.show"}, {"hc.menu.compatibility", "hc.navigate.compatibility"},
        {"hc.menu.wfx.open", "hc.plugin.wfx.open"}, {"hc.menu.wdx.query", "hc.plugin.wdx.query"},
        {"hc.menu.dopus.open", "hc.plugin.dopus.open"}, {"hc.menu.dopus.query", "hc.plugin.dopus.query"},
        {"hc.menu.automation", "hc.automation.show"}, {"acShowMainOpts", "hc.settings.show"}
    };
    for (auto* Existing : Window->findChildren<QAction*>()) {
        auto Id = Names.value(Existing->objectName());
        if (Id.isEmpty() && Existing->objectName().startsWith("hc.menu.hc.")) Id = Existing->objectName().mid(8);
        if (!Id.isEmpty()) actions_.insert(Id, Existing);
    }
    const auto Add = [this](const QString& Id, const QString& Label, std::function<void()> Invoke) {
        auto* Command = new QAction(Label, this); Command->setObjectName("hc.action." + Id); actions_.insert(Id, Command);
        Command->setProperty("hc.commandId", Id); Command->setProperty("hc.baseLabel", Label);
        connect(Command, &QAction::triggered, this, std::move(Invoke));
    };
    Add("hc.file.compare", tr("파일·폴더 비교"), [this] { Dispatch("hc.file.compare", {}, "ui", commandIndex()); });
    Add("hc.file.edit", tr("문서 편집"), [this] { Dispatch("hc.file.edit", {}, "ui", commandIndex()); });
    Add("hc.app.exit", tr("종료"), [Window] { Window->close(); });
    Add("hc.menu.reload", tr("메뉴 파일 다시 읽기"), [this] { QTimer::singleShot(0, this, [this] { if (userMenu_) userMenu_->Reload(); }); });
    Add("hc.menu.edit", tr("메뉴 파일 열기"), [this, Window] {
        if (userMenu_ && !QDesktopServices::openUrl(QUrl::fromLocalFile(userMenu_->FileName())))
            Window->statusBar()->showMessage(tr("메뉴 파일을 열 수 없습니다: %1").arg(userMenu_->FileName()));
    });
    Add("hc.app.about", tr("HyperCommander 정보"), [Window] {
        QDialog Box(Window); Box.setWindowTitle(QObject::tr("HyperCommander 정보")); Box.setObjectName("hc.about");
        // Reserve room for the title, application badge and caption controls.
        Box.setMinimumWidth(Box.fontMetrics().horizontalAdvance(Box.windowTitle()) + 208);
        auto* Layout = new QVBoxLayout(&Box);
        auto* Description = new QLabel(QObject::tr("HyperCommander\n버전 %1\n두 패널 파일 관리자 · Watercolor UI")
            .arg(QString::fromLatin1(nsHC::CLIENT_VERSION)), &Box);
        Description->setTextFormat(Qt::PlainText); Description->setTextInteractionFlags(Qt::TextSelectableByMouse); Layout->addWidget(Description);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Box); Layout->addWidget(Buttons);
        Buttons->button(QDialogButtonBox::Close)->setText(QObject::tr("닫기"));
        auto* Update = Buttons->addButton(QObject::tr("업데이트 확인…"), QDialogButtonBox::ActionRole); Update->setObjectName("hc.about.checkUpdate");
        Update->setEnabled(Window->findChild<QAction*>("hc.update.check") != nullptr);
        bool CheckRequested = false;
        QObject::connect(Update, &QPushButton::clicked, &Box, [&] { CheckRequested = true; Box.accept(); });
        QObject::connect(Buttons, &QDialogButtonBox::rejected, &Box, &QDialog::reject);
        hc::ui::ApplyDialogFrame(&Box, Buttons);
        if (Box.exec() == QDialog::Accepted && CheckRequested)
            if (auto* Action = Window->findChild<QAction*>("hc.update.check")) Action->trigger();
    });
    Add("hc.app.update", tr("업데이트 확인"), [Window] { if (auto* Action = Window->findChild<QAction*>("hc.update.check")) Action->trigger(); });
    Add("hc.file.shellMenu", tr("탐색기 메뉴"), [this] { ShowShellMenu(commandIndex()); });
    Add("hc.file.folderSize", tr("하위 폴더 점유 공간 계산"), [this] {
        if (auto* Model = PanelModel(activePanel_)) Model->RequestDirectorySizes();
    });
    Add("hc.file.context", tr("상황메뉴"), [this] { ShowShellMenu(commandIndex()); });
    Add("hc.commandLine.activate", tr("명령행 입력 활성화"), [Window] {
        QMetaObject::invokeMethod(Window, "ActivateCommandLine", Qt::DirectConnection);
    });
    Add("hc.commandLine.execute", tr("명령행 실행"), [Window] {
        QMetaObject::invokeMethod(Window, "SubmitCommandLine", Qt::DirectConnection, Q_ARG(bool, false));
    });
    Add("hc.commandLine.keepOpen", tr("명령행 실행 후 창 유지"), [Window] {
        QMetaObject::invokeMethod(Window, "SubmitCommandLine", Qt::DirectConnection, Q_ARG(bool, true));
    });
    for (const auto& Command : GlobalBuiltInCMDs) {
        const auto Id = hc::ui::CommandId(Command.Name);
        if (actions_.contains(Id)) continue;
        Add(Id, Command.Desc, [this, Id] {
            if (Id.startsWith("hc.clipboard.")) { Dispatch(Id, {}, "ui", commandIndex()); return; }
            if (Id.startsWith("hc.selection.") && Id != "hc.selection.invert") {
                auto* Panel = activePanel_.data();
                auto* View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
                if (auto* Selection = hc::ui::ItemSelectionController::For(View)) Selection->Execute(Id);
                return;
            }
            if (Id.startsWith("hc.navigate.gotoDrive")) { Dispatch(Id, {}, "ui"); return; }
            if (Id.startsWith("hc.tab.")) { Dispatch(Id, {}, "ui"); return; }
            if (Id == "hc.viewer.quick" || Id == "hc.viewer.open") { Dispatch(Id, {}, "ui"); return; }
            // These operations retain their list-local context and are
            // intentionally not installed as window-wide QAction shortcuts.
            TyStCommandMgr::GetInstance()->InvokeCommand(Id, commandIndex());
        });
    }
    if (auto* Settings = Action("hc.settings.show")) Settings->setText(tr("환경 설정"));
    for (auto It = actions_.begin(); It != actions_.end(); ++It) {
        It.value()->setProperty("hc.commandId", It.key());
        It.value()->setProperty("hc.baseLabel", It.value()->text().section('\t', 0, 0));
    }
    connect(TyStCommandMgr::GetInstance(), &CCommandMgr::ShortcutsChanged, this, [this] { RefreshShortcutLabels(); });
    RefreshShortcutLabels();
    Add("hc.navigate.back", tr("뒤로"), [this] { if (activePanel_) activePanel_->NavigateHistory(-1); });
    Add("hc.navigate.forward", tr("앞으로"), [this] { if (activePanel_) activePanel_->NavigateHistory(1); });
    auto* Toolbar = Window->findChild<QToolBar*>("hc.navigationToolbar"); Toolbar->clear();
    const QList<QPair<QString, fm::style::Glyph>> Tools{
        {"hc.navigate.back", fm::style::Glyph::Back}, {"hc.navigate.forward", fm::style::Glyph::Forward},
        {"hc.navigate.parent", fm::style::Glyph::ArrowUp}, {"hc.navigate.forceRefresh", fm::style::Glyph::Refresh}, {"hc.navigate.refresh", fm::style::Glyph::Refresh},
        {"hc.file.mkdir", fm::style::Glyph::NewFolder}, {"hc.file.new", fm::style::Glyph::NewFile},
        {"hc.file.copy", fm::style::Glyph::Copy}, {"hc.file.move", fm::style::Glyph::Move},
        {"hc.file.rename", fm::style::Glyph::Rename}, {"hc.file.delete", fm::style::Glyph::Trash},
        {"hc.file.batchRename", fm::style::Glyph::MultiRename}, {"hc.jobs.show", fm::style::Glyph::Clock}
    };
    const auto RefreshIcons = [this, Window, Tools] {
        const auto& Colors = fm::style::themeColorsFor(Window);
        for (const auto& [Id, Glyph] : Tools)
            if (auto* Command = Action(Id)) Command->setIcon(fm::style::glyphIcon(Glyph,
                Glyph == fm::style::Glyph::Trash ? Colors[fm::style::Token::Danger] : Colors[fm::style::Token::Fg], 16,
                Glyph == fm::style::Glyph::NewFolder ? Colors[fm::style::Token::Folder] : QColor{}));
    };
    RefreshIcons(); connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, RefreshIcons);
    for (int Index = 0; Index < Tools.size(); ++Index) {
        if (Index == 4 || Index == 6 || Index == 10) Toolbar->addSeparator();
        Toolbar->addAction(Action(Tools[Index].first));
    }
    auto* Spacer = new QWidget(Toolbar); Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); Toolbar->addWidget(Spacer);
    auto* Find = new fm::ui::FindBox(Window); Find->setObjectName("hc.mainFind"); Find->setMinimumWidth(120); Find->setMaximumWidth(300);
    Find->setToolTip(tr("현재 패널 검색 · name:문자 ext:확장자. 탭마다 검색을 유지합니다."));
    auto* QuickFind = new hc::ui::QuickFindController(Window, Find, [this] { return activePanel_.data(); }, [this](const QString& Path) {
        if (!activePanel_) return;
        const QFileInfo File(Path);
        activePanel_->SetSearch({});
        activePanel_->ChangeCurrentPath(File.isDir() ? File.absoluteFilePath() : File.absolutePath(), {});
    });
    QuickFind->setParent(this);
    const auto SyncFind = [QuickFind] { QuickFind->Synchronize(); };
    for (auto* Panel : Window->findChildren<CmpPanel*>()) connect(Panel, &CmpPanel::sigNavigationStateChanged, this, [this, SyncFind] {
        SyncFind(); refreshActionState();
    });
    if (auto* Main = qobject_cast<QMainUI*>(Window)) connect(Main, &QMainUI::sigActivePanelChanged, this, [this, SyncFind](CmpPanel* Panel) { activePanel_ = Panel; SyncFind(); refreshActionState(); });
    Add("hc.search.focus", tr("설정 범위에서 찾기"), [QuickFind] { QuickFind->FocusStandard(); });
    Add("hc.search.everything", tr("Everything에서 찾기"), [QuickFind] { QuickFind->FocusEverything(); });
    Add("hc.search.openLocation", tr("검색 결과의 원본 위치 열기"), [this] { if (activePanel_) activePanel_->OpenSearchLocation(); });
    for (const auto* Id : {"hc.search.focus", "hc.search.everything", "hc.navigate.back", "hc.navigate.forward", "hc.commandLine.activate"}) {
        auto* Key = new QShortcut(Window); Key->setContext(Qt::WindowShortcut);
        const auto Update = [Key, Id] { Key->setKeys(hc::ui::CommandKeys(TyStCommandMgr::GetInstance()->Retrieve(), Id)); };
        Update(); connect(TyStCommandMgr::GetInstance(), &CCommandMgr::ShortcutsChanged, Key, Update);
        connect(Key, &QShortcut::activated, this, [this, Id] { ExecuteCommand(Id); });
    }
    auto* FindButton = new QToolButton(Toolbar); FindButton->setText(tr("찾기")); Toolbar->addWidget(FindButton);
    connect(FindButton, &QToolButton::clicked, QuickFind, [QuickFind] { QuickFind->FocusStandard(); });
    RefreshFunctionBar();
    QHash<QString, QPointer<QMenu>> Sections;
    for (auto* Root : Window->findChild<QMenuBar*>("menubar")->actions()) {
        auto* Menu = Root->menu(); if (!Menu) continue;
        for (auto* Item : Menu->actions()) {
            if (Item->objectName() == "hc.menu.wfx.open") Sections.insert("plugins", Menu);
            if (Item->objectName() == "hc.menu.automation") Sections.insert("automation", Menu);
        }
    }
    RefreshShortcutLabels();
    QFile Default(":/HyperCommander/menu-bar.json");
    if (!Default.open(QIODevice::ReadOnly)) return;
    userMenu_ = new hc::ui::UserMenu(Window->findChild<QMenuBar*>("menubar"), QDir(hc::settings::Paths().directory_).filePath("menu-bar.json"), Default.readAll(), actions_, Sections,
        [Window](const QString& Diagnostic) { Window->statusBar()->showMessage(Diagnostic); qWarning().noquote() << Diagnostic; },
        [this] { refreshActionState(); });
    userMenu_->Reload();
    refreshActionState();
    auto* StateTimer = new QTimer(this); StateTimer->setInterval(250);
    connect(StateTimer, &QTimer::timeout, this, [this] { refreshActionState(); }); StateTimer->start();
    auto* RecoveryMenu = new QMenu(Window);
    RecoveryMenu->addAction(Action("hc.settings.show")); RecoveryMenu->addAction(Action("hc.menu.edit")); RecoveryMenu->addAction(Action("hc.menu.reload"));
    auto* MenuBar = Window->findChild<QMenuBar*>("menubar");
    auto* HeaderActions = new QWidget(MenuBar); HeaderActions->setObjectName("hc.headerActions");
    auto* HeaderLayout = new QHBoxLayout(HeaderActions);
    HeaderLayout->setContentsMargins(0, 0, 0, 0); HeaderLayout->setSpacing(4);
    // Let the surrounding row recalculate both dimensions after font/counter changes.
    HeaderActions->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    // Search must remain visible and focusable when the optional toolbar is hidden,
    // including the default first-run configuration.
    HeaderLayout->addWidget(Find);
    auto* AdminIndicator = new QLabel(tr("관리자 권한으로 실행 중"), HeaderActions);
    AdminIndicator->setObjectName("hc.adminIndicator");
    AdminIndicator->setToolTip(tr("HyperCommander 본체가 관리자 권한으로 실행 중입니다."));
    AdminIndicator->setAccessibleName(AdminIndicator->text());
    HeaderLayout->addWidget(AdminIndicator, 0, Qt::AlignVCenter);
    AdminIndicator->setVisible(hc::permissions::Elevated());
    if (auto* Indicator = Window->findChild<QPushButton*>("hc.jobs.indicator")) {
        Window->statusBar()->removeWidget(Indicator);
        Indicator->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        HeaderLayout->addWidget(Indicator);
        Indicator->show();
    }
    auto* SettingsButton = new QToolButton(HeaderActions); SettingsButton->setObjectName("hc.menuRecovery");
    SettingsButton->setText(tr("설정")); SettingsButton->setToolTip(tr("환경 설정 · 메뉴 파일 복구"));
    SettingsButton->setMenu(RecoveryMenu); SettingsButton->setPopupMode(QToolButton::InstantPopup);
    // Share the tallest button's size hint, including after font changes.
    SettingsButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    HeaderLayout->addWidget(SettingsButton);
    MenuBar->setCornerWidget(HeaderActions, Qt::TopRightCorner);
    HeaderActions->show();
    SettingsButton->show();
    // Recovery remains available when a user removes the Settings menu. This
    // binding follows the configurable key and is active only on the main frame.
    // File operation bindings remain in the list's existing key event handler.
    auto* Recovery = new QShortcut(Window); Recovery->setContext(Qt::WindowShortcut);
    const auto UpdateRecovery = [Recovery] {
        const auto Keys = hc::ui::CommandKeys(TyStCommandMgr::GetInstance()->Retrieve(), "hc.settings.show"); Recovery->setKeys(Keys);
    };
    UpdateRecovery(); connect(TyStCommandMgr::GetInstance(), &CCommandMgr::ShortcutsChanged, Recovery, UpdateRecovery);
    connect(Recovery, &QShortcut::activated, this, [this] { ExecuteCommand("hc.settings.show"); });
}

void CommandUiBridge::PromptArchive(bool Extract, const QModelIndex& Index) {
    const QScopedValueRollback<QModelIndex> Scope(commandIndex_, Index);
    if (Extract) extractArchive(); else openArchive();
}
void CommandUiBridge::extractArchive() {
    const auto Context = captureFiles(commandIndex(), false); auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->GetRoot() || !Model->ProviderEnabled() || Model->IsLoading()) return;
    const bool Inside = !Model->IsSearchResults() && Context.location_.kind_ == LocatorKind::Archive;
    if (!Model->IsSearchResults() && !Inside && Context.location_.kind_ != LocatorKind::Local && Context.location_.kind_ != LocatorKind::Smb) {
        hc::ui::MessageDialog::information(main_, tr("파일 압축 해제"), tr("로컬·SMB의 압축 파일 또는 열린 압축 파일 안의 항목을 선택하세요.")); return;
    }
    std::vector<hc::operations::OperationSource> Sources; QStringList Names; auto Reader = Model->SnapshotProvider();
    if (!Context.selectionManifest_) for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto Item = Model->GetFileInfo(Model->index(Row,0));
        if (Item && Item->storageEntry_ && std::find(Context.selection_.begin(), Context.selection_.end(), Item->storageEntry_->id_) != Context.selection_.end()) {
            if (!Inside && Item->storageEntry_->directory_) { hc::ui::MessageDialog::information(main_, tr("파일 압축 해제"), tr("압축 파일만 선택하세요.")); return; }
            Sources.push_back({*Item->storageEntry_, Model->ProviderFor(*Item->storageEntry_), {}}); Names.append(Item->Name_);
        }
    }
    if (Context.selectionManifest_) Names.append(tr("전체 선택 %1개").arg(Context.selectionManifest_->Count()));
    if (Sources.empty() && !Context.selectionManifest_) { hc::ui::MessageDialog::information(main_, tr("파일 압축 해제"), tr("압축 파일이나 내부 항목을 먼저 선택하세요.")); return; }
    auto Destination = QString::fromStdString(Context.location_.root_); if (Inside) Destination = QFileInfo(Destination).absolutePath();
    if (Model->IsSearchResults() && !Sources.empty()) Destination = QFileInfo(QString::fromStdString(Sources.front().entry_.locator_.root_)).absolutePath();
    const auto RelativeBase = Destination;
    for (auto* Other : main_->findChildren<CmpPanel*>()) if (Other != PanelOf(Model)) if (auto* Target = PanelModel(Other); Target && Target->GetRoot() && !Target->IsSearchResults()) {
        const auto Location = Target->WorkspaceLocation(); if (Location.kind_ == LocatorKind::Local || Location.kind_ == LocatorKind::Smb) Destination = QString::fromStdString(Location.root_);
    }
    const bool BrowseRoot = Inside && !Context.selectionManifest_ && Sources.size() == 1 && Sources.front().entry_.hints_.contains("virtualArchiveRoot");
    hc::ui::ArchiveExtractDialog Dialog(Names, Destination, Inside && !BrowseRoot, main_, RelativeBase); if (Dialog.exec() != QDialog::Accepted) return;
    const auto Path = Dialog.Destination(); Locator Target{(Path.startsWith("//") && (!Path.startsWith("//?/") || Path.startsWith("//?/UNC/", Qt::CaseInsensitive))) ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
    auto Native = MakeNativeReadProvider(Target.kind_); auto Writer = MakeNativeWritableStorage(Target.kind_, Native);
    const auto Conflict = Dialog.Conflict() == "replace" ? hc::operations::ConflictPolicy::Replace : Dialog.Conflict() == "skip" ? hc::operations::ConflictPolicy::Skip : hc::operations::ConflictPolicy::Ask;
    OperationContext Operation; Operation.passwordPrompt_ = hc::plugins::AskArchivePassword;
    Result<hc::operations::PreparedJob> Prepared = Error{ErrorCategory::Internal, "extract-prepare"};
    if (BrowseRoot) {
        hc::operations::ExtractRequest Request; Request.archives_ = Sources; Request.sourceRoot_ = Context.location_;
        Request.destination_ = Target; Request.writer_ = Writer; Request.conflict_ = Conflict; Request.separateFolders_ = Dialog.SeparateFolders(); Request.elevation_ = fileJobs_->ElevationService();
        Request.readers_ = [Reader = Sources.front().reader_](const EntrySnapshot&) { return std::vector<std::shared_ptr<IStorageProvider>>{Reader}; };
        Prepared = hc::operations::PrepareExtract(std::move(Request), Operation);
    } else if (Inside) {
        hc::operations::FileOperationRequest Copy; Copy.sources_ = std::move(Sources); Copy.selection_ = Context.selectionManifest_; Copy.selectionRoot_ = Context.location_;
        Copy.destination_ = Target; Copy.destinationWriter_ = Writer; Copy.conflict_ = Conflict; Copy.createParents_ = true; Copy.elevation_ = fileJobs_->ElevationService();
        Prepared = hc::operations::PrepareFileOperation(std::move(Copy), Operation);
        if (Prepared.HasValue()) Prepared.Value().task_ = std::make_unique<hc::operations::ProviderInteractionTask>(std::move(Prepared.Value().task_));
    } else {
        // Snapshot configured priority and review approvals before entering the
        // worker. A settings edit cannot redirect an already submitted job.
        struct Choice { QString path_; QStringList extensions_; std::shared_ptr<hc::plugins::IArchiveBackend> backend_; QJsonValue applicationRule_; bool sevenZip_; };
        std::vector<Choice> Choices;
        for (const auto& Spec : TyStPlugInMgr::GetInstance()->RetrieveWCX()) {
            auto Plugin = Spec.FileFullPath; Plugin.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
            if (QDir::isRelativePath(Plugin)) Plugin = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Plugin);
            if ((Spec.hostEnabled_ && Plugin.endsWith(".wcx", Qt::CaseInsensitive)) || (!Spec.hostEnabled_ && !Plugin.endsWith("64", Qt::CaseInsensitive) && QFileInfo::exists(Plugin+"64"))) Plugin += "64";
            std::shared_ptr<hc::plugins::IArchiveBackend> Backend;
            if (Spec.hostEnabled_) {
                hc::plugins::ApprovedWcxOptions Options; Options.executable_ = QCoreApplication::applicationDirPath()+"/hc-plugin-host-x64.exe";
                Options.binaryPath_ = Plugin; Options.manifestPath_ = Spec.hostManifest_; Options.approval_ = Spec.hostApproval_; Options.passwordPrompt_ = hc::plugins::AskArchivePassword;
                Backend = hc::plugins::MakeApprovedWcxBackend(std::move(Options));
            } else Backend = hc::plugins::MakeInProcessWcxBackend(Plugin.toStdString());
            Choices.push_back({Plugin, Spec.Exts, std::move(Backend), Spec.applicationRule_, Spec.hostEnabled_ && Spec.hostApproval_.reviewed_.format_ == hc::plugins::PluginFormat::SevenZip});
        }
        hc::operations::ExtractRequest Request; Request.archives_ = std::move(Sources); Request.selection_ = Context.selectionManifest_; Request.sourceRoot_ = Context.location_;
        Request.destination_ = Target; Request.writer_ = Writer; Request.conflict_ = Conflict; Request.separateFolders_ = Dialog.SeparateFolders(); Request.elevation_ = fileJobs_->ElevationService();
        Request.readers_ = [Choices = std::move(Choices), Temporary = Target.kind_==LocatorKind::Local ? Target.root_ : std::string{}](const EntrySnapshot& Entry) {
            std::vector<std::shared_ptr<IStorageProvider>> Readers; const auto Extension = QFileInfo(QString::fromStdString(Entry.name_)).suffix();
            for (const auto& Choice : Choices) {
                const auto Name = QString::fromStdString(Entry.name_);
                bool Supported = Choice.extensions_.contains(Extension, Qt::CaseInsensitive);
                for (const auto& Ext : Choice.extensions_) if (Name.endsWith('.' + Ext, Qt::CaseInsensitive)) Supported = true;
                if (!Supported && Choice.sevenZip_) Supported = hc::plugins::IsArchiveVolumeName(Name);
                if (!Supported) continue;
                const auto Rule = hc::plugins::PluginApplicationRule::Parse(Choice.applicationRule_);
                if (!Rule.HasValue() || !Rule.Value().NameOnly()) continue;
                const auto Match = Rule.Value().Matches(QString::fromStdString(Entry.name_));
                if (!Match.HasValue() || !Match.Value()) continue;
                if (Choice.sevenZip_ && !hc::plugins::BuiltinArchiveAllowed(Rule.Value(), Name)) continue;
                Readers.push_back(MakeWcxReadProvider(Choice.path_.toStdString(), {LocatorKind::Archive, Entry.locator_.root_, {{"wcx", "/"}}}, Temporary, Choice.backend_, {}, true));
            }
            return Readers;
        };
        Prepared = hc::operations::PrepareExtract(std::move(Request), Operation);
    }
    if (!Prepared.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("파일 압축 해제"), QString::fromStdString(Prepared.Failure().code_)); return; }
    Prepared.Value().request_.deferred_ = Dialog.Queued();
    Prepared.Value().request_.metadata_.source_ = Context.location_.root_; Prepared.Value().request_.metadata_.destination_ = Target.root_;
    Prepared.Value().request_.metadata_.kind_ = hc::operations::JobKind::Extract;
    auto Submitted = fileJobs_->Submit(std::move(Prepared.Value()), tr("파일 압축 해제"));
    if (!Submitted.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("파일 압축 해제"), QString::fromStdString(Submitted.Failure().code_)); return; }
    auto Parent = Target; Parent.root_ = QFileInfo(Path).absolutePath().toStdString();
    affectedLocations_[Submitted.Value().id_] = {Target, Parent};
    rememberJobSelection(Submitted.Value().id_, Context);
}

void CommandUiBridge::openArchive(std::string InitialFormat)
{
    const auto Context = captureFiles(commandIndex(), false);
    auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->ProviderEnabled() || Model->IsLoading()) return;
    auto Reader = Model->SnapshotProvider();
    std::vector<hc::operations::OperationSource> Sources;
    if (!Context.selectionManifest_) for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto Item = Model->GetFileInfo(Model->index(Row, 0));
        if (Item && Item->storageEntry_ && std::find(Context.selection_.begin(), Context.selection_.end(), Item->storageEntry_->id_) != Context.selection_.end())
            Sources.push_back({*Item->storageEntry_, Model->ProviderFor(*Item->storageEntry_), {}});
    }
    const auto Count = Context.selectionManifest_ ? Context.selectionManifest_->Count() : Sources.size();
    if (!Count) { hc::ui::MessageDialog::information(main_, tr("파일 압축"), tr("압축할 파일이나 폴더를 먼저 선택하십시오.")); return; }
    auto Directory = QString::fromStdString(Context.location_.root_);
    if (Context.location_.kind_ == LocatorKind::Archive) Directory = QFileInfo(Directory).absolutePath();
    else if (Context.location_.kind_ != LocatorKind::Local && Context.location_.kind_ != LocatorKind::Smb) Directory = QDir::homePath();
    auto SourceLocation = Context.location_;
    if (Model->IsSearchResults() && !Sources.empty()) {
        SourceLocation = Sources.front().entry_.locator_;
        Directory = QFileInfo(QString::fromStdString(SourceLocation.root_)).absolutePath(); SourceLocation.root_ = Directory.toStdString();
    }
    hc::archive::CreationDialog Dialog(hc::settings::AppSettings(), SourceLocation, Directory, Count, main_, InitialFormat);
    if (Dialog.exec() != QDialog::Accepted) return;
    hc::archive::CreateJobRequest Request; Request.value_ = Dialog.Request(); Request.creator_ = Dialog.Creator();
    Request.sources_ = std::move(Sources); Request.selection_ = Context.selectionManifest_;
    for (const auto& Source : Request.sources_) Request.value_.sources_.push_back(Source.entry_);
    const auto Saved = Dialog.SavePreferences();
    if (!Saved.HasValue()) hc::ui::MessageDialog::warning(main_, tr("압축 설정 저장"), QString::fromStdString(Saved.Failure().code_));
    OperationContext Operation; Operation.passwordPrompt_ = hc::plugins::AskArchivePassword;
    auto Prepared = hc::archive::PrepareCreation(Request, Operation);
    if (!Prepared.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("파일 압축"), QString::fromStdString(Prepared.Failure().code_)); return; }
    auto Affected = Request.value_.output_; const auto Path = QString::fromStdString(Affected.root_);
    if (!Request.value_.composition_.separateArchives_) Affected.root_ = QFileInfo(Path).absolutePath().toStdString();
    const auto Label = Request.value_.composition_.separateArchives_ ? tr("압축 파일 개별 생성") : tr("압축 파일 생성 · %1").arg(QFileInfo(Path).fileName());
    auto Submitted = fileJobs_->Submit(std::move(Prepared.Value()), Label,
        [Request](const hc::operations::JobResult& Result, OperationContext RetryContext) mutable {
            Request = hc::archive::RetryCreation(std::move(Request), Result);
            return hc::archive::PrepareCreation(Request, std::move(RetryContext));
        }, [Guard = QPointer<CommandUiBridge>(this), Affected](const std::string& Id) { if (Guard) Guard->affectedLocations_[Id] = {Affected}; });
    if (!Submitted.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("파일 압축"), QString::fromStdString(Submitted.Failure().code_)); return; }
    affectedLocations_[Submitted.Value().id_] = {Affected};
    rememberJobSelection(Submitted.Value().id_, Context);
}

void CommandUiBridge::openIso(bool Inspect)
{
    if (!Inspect) { openArchive("iso"); return; }
    const auto Context = captureFiles({}, false);
    auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->ProviderEnabled() || Model->IsLoading()) return;
    auto Reader = Model->SnapshotProvider();
    std::vector<hc::operations::OperationSource> Sources;
    for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto Item = Model->GetFileInfo(Model->index(Row, 0));
        if (Item && Item->storageEntry_ && std::find(Context.selection_.begin(), Context.selection_.end(), Item->storageEntry_->id_) != Context.selection_.end())
            Sources.push_back({*Item->storageEntry_, Model->ProviderFor(*Item->storageEntry_), {}});
    }
    const auto CurrentDirectory = Context.location_.kind_ == LocatorKind::Archive ? QFileInfo(QString::fromStdString(Context.location_.root_)).absolutePath() : QString::fromStdString(Context.location_.root_);
    if (Inspect) {
        QString Path;
        if (Sources.size() == 1 && !Sources[0].entry_.directory_ && (Sources[0].entry_.locator_.kind_ == LocatorKind::Local || Sources[0].entry_.locator_.kind_ == LocatorKind::Smb)) Path = QString::fromStdString(Sources[0].entry_.locator_.root_);
        if (Path.isEmpty()) Path = QFileDialog::getOpenFileName(main_, tr("ISO 구조 확인"), CurrentDirectory, tr("ISO 이미지 (*.iso)"));
        if (Path.isEmpty()) return;
        auto* Dialog = new QDialog(main_); Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setObjectName("hc.iso.inspect"); Dialog->setWindowTitle(tr("ISO 구조 확인")); Dialog->resize(780, 520);
        auto* Layout = new QVBoxLayout(Dialog);
        auto* FileLabel = new QLabel(QDir::toNativeSeparators(Path), Dialog); FileLabel->setTextFormat(Qt::PlainText); FileLabel->setWordWrap(true); Layout->addWidget(FileLabel);
        auto* Status = new QLabel(tr("구조를 확인하고 있습니다…"), Dialog); Status->setTextFormat(Qt::PlainText); Status->setWordWrap(true); Status->setObjectName("hc.iso.inspect.status"); Layout->addWidget(Status);
        auto* Table = new QTableWidget(0, 3, Dialog); Table->setObjectName("hc.iso.inspect.entries"); Table->setHorizontalHeaderLabels({tr("경로"), tr("종류"), tr("크기")}); Table->setEditTriggers(QAbstractItemView::NoEditTriggers); Table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); Layout->addWidget(Table, 1);
        auto* Note = new QLabel(tr("ISO에는 파일 내용의 기준 체크섬이 없습니다. 전체 읽기 검사는 읽기 오류를 확인하며, 원본과의 일치 여부를 판정하지 않습니다."), Dialog); Note->setWordWrap(true); Layout->addWidget(Note);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, Dialog); Buttons->button(QDialogButtonBox::Close)->setText(tr("닫기")); auto* ReadAll = Buttons->addButton(tr("전체 파일 읽기 검사"), QDialogButtonBox::ActionRole); ReadAll->setObjectName("hc.iso.inspect.readAll"); Layout->addWidget(Buttons);
        auto Stop = std::make_shared<std::stop_source>(); connect(Dialog, &QDialog::finished, Dialog, [Stop] { Stop->request_stop(); }); connect(Buttons, &QDialogButtonBox::rejected, Dialog, &QDialog::close);
        const auto Run = [Dialog, Table, Status, ReadAll, Stop, Path](bool Full) {
            ReadAll->setEnabled(false); Status->setText(Full ? QObject::tr("전체 파일을 읽고 있습니다…") : QObject::tr("구조를 확인하고 있습니다…"));
            using Inspection = Result<hc::plugins::BuiltinIsoInspection>;
            auto* Watcher = new QFutureWatcher<Inspection>(Dialog);
            connect(Watcher, &QFutureWatcher<Inspection>::finished, Dialog, [Watcher, Table, Status, ReadAll, Full] {
                const auto Result = Watcher->result(); Watcher->deleteLater(); ReadAll->setEnabled(true);
                if (!Result.HasValue()) { Status->setText(QObject::tr("확인하지 못했습니다: %1").arg(QString::fromStdString(Result.Failure().code_))); return; }
                const auto& Value = Result.Value();
                auto Text = QObject::tr("%1 · 볼륨 %2\n%3\n파일 %4개 · 폴더 %5개 · %6바이트")
                    .arg(Full ? QObject::tr("전체 읽기 검사 완료") : QObject::tr("구조 확인 완료"), Value.volumeLabel_, Value.namespaces_.join(" + "))
                    .arg(QLocale().toString(Value.files_), QLocale().toString(Value.directories_), QLocale().toString(Value.bytes_));
                if (Value.files_ + Value.directories_ > Value.preview_.size()) Text += QObject::tr("\n목록은 처음 5,000개 항목을 표시합니다. 전체 탐색은 파일 패널에서 ISO를 여십시오.");
                Status->setText(Text); Table->setRowCount(int(Value.preview_.size()));
                for (int I = 0; I < int(Value.preview_.size()); ++I) { const auto& Entry = Value.preview_[size_t(I)]; Table->setItem(I, 0, new QTableWidgetItem(QString::fromStdString(Entry.name_))); Table->setItem(I, 1, new QTableWidgetItem(Entry.directory_ ? QObject::tr("폴더") : QObject::tr("파일"))); Table->setItem(I, 2, new QTableWidgetItem(Entry.directory_ ? QString{} : QLocale().toString(Entry.size_))); }
            });
            Watcher->setFuture(QtConcurrent::run([Path, Stop, Full] { OperationContext Operation; Operation.stopToken_ = Stop->get_token(); return hc::plugins::InspectBuiltinIso(Path, Full, Operation); }));
        };
        connect(ReadAll, &QPushButton::clicked, Dialog, [Run] { Run(true); });
        FileLabel->hide(); hc::ui::ApplyDialogFrame(Dialog, Buttons, {{}, QDir::toNativeSeparators(Path), fm::ui::glyph::Drive});
        Dialog->show(); Run(false); return;
    }
}

void CommandUiBridge::openSearch()
{
    auto* Model = PanelModel(activePanel_);
    if (!Model || Model->IsLoading() || !Model->SnapshotProvider()) return;
    hc::workspace::TabState State; State.location_ = Model->WorkspaceLocation();
    State.provider_ = Model->property("hc.workspaceProvider").toString(); State.nativeRoot_ = Model->property("hc.workspaceNativeRoot").toString();
    hc::search::Show(main_, WorkerOwned(Model->SnapshotProvider()), State.location_, Model->SearchMetadata(),
        [Model = QPointer<CFSModel>(Model), State](const EntrySnapshot& Entry) mutable {
            if (!Model || PanelModel(PanelOf(Model)) != Model) return;
            State.location_ = Entry.locator_;
            if (!Entry.directory_) {
                auto& Path = State.location_.nesting_.empty() ? State.location_.root_ : State.location_.nesting_.back().entry_;
                const auto Last = Path.find_last_of("/\\"); if (Last == std::string::npos) return; Path.resize(Last + 1);
            }
            RestoreWorkspaceLocation(Model, State);
        });
}

void CommandUiBridge::openDataPlugin(bool Metadata, bool Dopus)
{
    using namespace hc::plugins;
    const auto Choices = QPluginHostSettings::DataChoices(hc::settings::AppSettings(), Dopus ? (Metadata ? PluginFormat::DopusViewer : PluginFormat::DopusVfs) : (Metadata ? PluginFormat::Wdx : PluginFormat::Wfx));
    if (Choices.empty()) { hc::ui::MessageDialog::information(main_, tr("플러그인"), tr("설정의 플러그인 관리에서 플러그인을 추가하고 실행을 승인해 주세요.")); return; }
    const auto Context = captureFiles({}, true);
    QPointer<CmpPanel> Panel = activePanel_;
    QPointer<CFSModel> Model = findModel(Context.tabId_);
    const auto MetadataGeneration = Model ? Model->MetadataGeneration() : 0;
    std::shared_ptr<IContentSource> Source;
    if (Metadata)
    {
        if (!Model || Model->IsLoading() || !Model->ProviderEnabled() || !Model->SnapshotProvider() || Context.selection_.size() != 1) return;
        for (int Row = 0; Row < Model->rowCount(); ++Row)
        {
            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
            if (Entry && Entry->storageEntry_ && !Entry->storageEntry_->directory_ && Entry->storageEntry_->id_ == Context.selection_.front())
                Source = std::make_shared<ProviderContentSource>(Model->SnapshotProvider(), *Entry->storageEntry_);
        }
        if (!Source) return;
    }
    QStringList Names; for (const auto& Choice : Choices) Names << Choice.id_ + " — " + Choice.name_;
    bool Accepted = false; const auto Name = hc::ui::InputDialog::getItem(main_, tr("플러그인 선택"), tr("플러그인"), Names, 0, false, &Accepted);
    if (!Accepted || !Panel) return;
    const auto Options = Choices[Names.indexOf(Name)].options_;
    QString NativeRoot;
    if (Dopus && !Metadata)
    {
        if (Context.location_.kind_ == LocatorKind::Local && Context.location_.nesting_.empty()) NativeRoot = QString::fromStdString(Context.location_.root_);
        if (Model && Context.selection_.size() == 1)
            for (int Row = 0; Row < Model->rowCount(); ++Row)
            {
                const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
                if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Context.selection_.front()
                    && Entry->storageEntry_->locator_.kind_ == LocatorKind::Local && Entry->storageEntry_->locator_.nesting_.empty())
                    NativeRoot = QString::fromStdString(Entry->storageEntry_->locator_.root_);
            }
        NativeRoot = hc::ui::InputDialog::getText(main_, tr("DOpus 파일 시스템"), tr("열 파일 또는 폴더의 전체 경로"), QLineEdit::Normal, NativeRoot, &Accepted);
        if (!Accepted || NativeRoot.isEmpty() || !Panel) return;
    }
    const auto Cache = QDir::tempPath().toStdString();
    const auto Session = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    auto* Dialog = new QDialog(main_); Dialog->setObjectName("hc.dataPluginDialog"); Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setWindowTitle(Name); Dialog->resize(600, 380);
    auto* Layout = new QVBoxLayout(Dialog); auto* Status = new QLabel(tr("플러그인 연결 중…"), Dialog); Layout->addWidget(Status);
    auto* Table = new QTableWidget(0, 2, Dialog); Table->setHorizontalHeaderLabels({tr("항목"), tr("값")}); Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); Layout->addWidget(Table); Table->setVisible(Metadata);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, Dialog); Layout->addWidget(Buttons); connect(Buttons, &QDialogButtonBox::rejected, Dialog, &QDialog::close);
    hc::ui::ApplyDialogFrame(Dialog, Buttons, {{}, Metadata ? tr("플러그인이 알려 준 파일 정보입니다.") : tr("플러그인 파일 시스템에 연결합니다."), fm::ui::glyph::Info});
    auto Stop = std::make_shared<std::stop_source>(); connect(Dialog, &QObject::destroyed, this, [Stop] { Stop->request_stop(); });
    struct OpenResult { HostedStorage storage_; std::vector<std::pair<QString, QString>> values_; std::shared_ptr<IMetadataProvider> metadata_; std::vector<int> fields_; QMap<QString, QString> searchable_; };
    auto* Watcher = new QFutureWatcher<Result<OpenResult>>(Dialog);
    connect(Watcher, &QFutureWatcher<Result<OpenResult>>::finished, Dialog, [this, Watcher, Dialog, Status, Table, Panel, Metadata, Source, Stop, Model, MetadataGeneration, Options, NativeRoot] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (!Result.HasValue()) {
            const auto Code = QString::fromStdString(Result.Failure().code_); Status->setText(tr("연결 실패: %1").arg(Code));
            hc::plugins::OfferPluginReview(main_, Code, Options.binaryPath_, viewers_->reviewPlugin_); return;
        }
        if (Metadata)
        {
            if (Model) Model->PutMetadata(Source->Describe(), Result.Value().searchable_, MetadataGeneration);
            Status->setText(Source->Describe().locator_.kind_ == LocatorKind::Local
                ? tr("파일 정보 — 항목을 두 번 클릭하면 지연된 값도 계산합니다.")
                : tr("가상·원격 파일의 임시 복사본 기준 정보입니다. 두 번 클릭하면 지연된 값도 계산합니다."));
            for (const auto& [Field, Value] : Result.Value().values_)
            { const auto Row = Table->rowCount(); Table->insertRow(Row); Table->setItem(Row, 0, new QTableWidgetItem(Field)); Table->setItem(Row, 1, new QTableWidgetItem(Value)); }
            connect(Table, &QTableWidget::cellDoubleClicked, Dialog, [Table, Dialog, Source, Stop, Model, MetadataGeneration, Provider = Result.Value().metadata_, Fields = Result.Value().fields_](int Row, int) {
                if (Row < 0 || std::size_t(Row) >= Fields.size()) return;
                Table->setEnabled(false); auto* Query = new QFutureWatcher<hc::storage::Result<MetadataValue>>(Dialog);
                connect(Query, &QFutureWatcher<hc::storage::Result<MetadataValue>>::finished, Dialog, [Query, Table, Row, Model, Source, MetadataGeneration] {
                    const auto Value = Query->result(); const auto Text = MetadataText(Value);
                    Table->item(Row, 1)->setText(Text); Table->setEnabled(true); Query->deleteLater();
                    if (Model && Value.HasValue() && Value.Value().status_ == MetadataStatus::Value)
                        Model->PutMetadata(Source->Describe(), {{Table->item(Row, 0)->text(), Text}}, MetadataGeneration);
                });
                Query->setFuture(QtConcurrent::run([Provider, Source, Stop, Field = Fields[Row]] {
                    OperationContext Operation; Operation.stopToken_ = Stop->get_token(); Operation.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                    return Provider->Query(Source, {Field, 0, false}, Operation).get();
                }));
            });
        }
        else if (Panel)
        {
            Panel->AddTab(false); bindModels();
            if (auto* Target = PanelModel(Panel))
            {
                const auto& Storage = Result.Value().storage_;
                Target->setProperty("hc.workspaceProvider", Options.manifestPath_);
                Target->setProperty("hc.workspaceNativeRoot", NativeRoot);
                Target->SetRoot(nsHC::TySpFileSystem(new nsHC::CFSPlugin(Storage.root_, Storage.reader_, Storage.writer_)));
                Target->SetCurrentPath("\\"); Target->SetProviderEnabled(true); Target->Refresh();
            }
            Dialog->close();
        }
    });
    Watcher->setFuture(QtConcurrent::run([Options, Cache, Source, Metadata, Dopus, NativeRoot, Stop, Session]() -> Result<OpenResult> {
        OperationContext Operation; Operation.stopToken_ = Stop->get_token(); Operation.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        if (Metadata) {
            auto Rule = PluginApplicationRule::Parse(Options.applicationRule_); if (!Rule.HasValue()) return Rule.Failure();
            auto Match = Rule.Value().MatchesSource(*Source, Operation); if (!Match.HasValue()) return Match.Failure();
            if (!Match.Value()) return Error{ErrorCategory::Unsupported, "plugin-rule-excluded"};
        }
        auto Inspected = InspectManifestFile(Options.manifestPath_, Operation); if (!Inspected.HasValue()) return Inspected.Failure();
        if (QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive)) return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch"};
        auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, {}, Operation, Options.defaultIniPath_);
        if (!Started.HasValue()) return Started.Failure();
        // Provider ownership may end when a GUI tab closes. Reap its host on a
        // worker; active requests retain shared ownership until they return.
        auto Process = std::shared_ptr<PluginProcess>(Started.Value().release(), [](PluginProcess* Value) {
            (void)QtConcurrent::run([Value] { delete Value; });
        });
        OpenResult Result;
        if (!Metadata)
        {
            auto Store = MakeNativeContentStore(Cache); if (!Store.HasValue()) return Store.Failure();
            auto Storage = Dopus ? MakeHostedDopusVfsProvider(Process, {Session}, Store.Value(), Options.approval_.granted_.contains(Permission::WriteContent), NativeRoot)
                : MakeHostedWfxProvider(Process, {Session}, Store.Value(), Options.approval_.granted_.contains(Permission::WriteContent));
            if (!Storage.HasValue()) return Storage.Failure(); Result.storage_ = std::move(Storage.Value());
            Result.storage_.reader_ = WorkerOwned(std::move(Result.storage_.reader_));
            if (Result.storage_.writer_) Result.storage_.writer_ = WorkerOwned(std::move(Result.storage_.writer_));
        }
        else
        {
            auto Materializer = MakeNativeMaterializer(Cache); if (!Materializer.HasValue()) return Materializer.Failure();
            auto Provider = Dopus ? MakeHostedDopusMetadataProvider(Process, Materializer.Value()) : MakeHostedWdxProvider(Process, Materializer.Value()); if (!Provider.HasValue()) return Provider.Failure();
            Result.metadata_ = WorkerOwned(Provider.Value());
            auto Fields = Provider.Value()->Fields(Operation).get(); if (!Fields.HasValue()) return Fields.Failure();
            for (const auto& Field : Fields.Value())
            {
                if (const auto Failure = Operation.Check({})) return *Failure;
                auto Value = Provider.Value()->Query(Source, {Field.index_, 0, true}, Operation).get();
                Result.values_.emplace_back(QString::fromStdString(Field.name_), MetadataText(Value)); Result.fields_.push_back(Field.index_);
                if (Value.HasValue() && Value.Value().status_ == MetadataStatus::Value)
                    Result.searchable_.insert(QString::fromStdString(Field.name_), MetadataText(Value));
            }
        }
        return Result;
    }));
    Dialog->show();
}

void CommandUiBridge::closeQuick()
{
    if (!quick_) return;
    auto* Quick = quick_.data(); Quick->CloseView();
    bool Restored = false;
    if (quickPanel_)
        if (auto* Tabs = quickPanel_->findChild<QTabWidget*>("tabWidget"))
            for (int Index = 0; Index < Tabs->count(); ++Index)
                if (quickPanel_->GetFocusView(Index) == Quick)
                { quickPanel_->CloseQuickView(Index); Restored = true; break; }
    if (!Restored) Quick->deleteLater();
    quick_.clear(); quickPanel_.clear(); quickSource_.clear(); quickKey_.clear();
}

void CommandUiBridge::refreshPreview()
{
    if (!quick_ || !quickSource_ || !quickPanel_ || !quick_->isVisible()) return;
    if (quickPanel_->GetFocusView(quickPanel_->CurrentTabIndex()) != quick_) return;
    auto* Model = PanelModel(quickSource_);
    auto* View = qobject_cast<CGridView*>(quickSource_->GetFocusView(quickSource_->CurrentTabIndex()));
    if (!Model || !View || Model->IsLoading() || !Model->ProviderEnabled())
    { quick_->CloseView(); quickKey_.clear(); return; }
    const auto Focused = View->BaseView()->focusedRow();
    const auto Index = Focused.isValid() && Focused.type() == Qtitan::GridRow::DataRow ? SourceIndex(Focused.modelIndex(0)) : QModelIndex{};
    const auto Entry = Index.isValid() ? Model->GetFileInfo(Index) : nsHC::TySpFileSource{};
    if (!Entry || !Entry->storageEntry_ || Entry->storageEntry_->directory_)
    { quick_->CloseView(); quickKey_.clear(); return; }
    const auto Key = Model->property("hc.tabId").toString() + QString::fromStdString(Serialize(Entry->storageEntry_->id_) + Serialize(Entry->storageEntry_->locator_));
    if (quickKey_ == Key) return;
    quickKey_ = Key;
    Dispatch("hc.viewer.preview", {}, "preview", Index);
}

Result<std::string> CommandUiBridge::captureDocument(const CommandInvocation& Invocation, CFSModel* Model)
{
    if (!Model || Model->IsLoading()) return Error{ErrorCategory::Unavailable, "document-navigation-in-progress"};
    auto Reader = Model->SnapshotProvider();
    if (!Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unsupported, "document-provider-unsupported"};
    if (Invocation.context_.selectionManifest_ || Invocation.context_.selection_.size() > 16)
        return Error{ErrorCategory::BudgetExceeded, "document-selection-limit-16"};
    hc::document::DocumentOpenRequest Request;
    Request.intent_ = Invocation.id_ == "hc.file.edit" ? hc::document::DocumentIntent::Edit : hc::document::DocumentIntent::View;
    const auto Kind = Invocation.context_.location_.kind_;
    auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider()
        : Kind == LocatorKind::Local || Kind == LocatorKind::Smb ? MakeNativeWritableStorage(Kind, Reader) : nullptr;
    for (const auto& Id : Invocation.context_.selection_) {
        bool Found = false;
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
            if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Id) {
                auto EntryReader = Model->ProviderFor(*Entry->storageEntry_);
                auto EntryWriter = Model->IsSearchResults() ? MakeNativeWritableStorage(Entry->storageEntry_->locator_.kind_, EntryReader) : Writer;
                Request.documents_.push_back({std::make_shared<ProviderContentSource>(EntryReader, *Entry->storageEntry_), EntryWriter}); Found = true; break;
            }
        }
        if (!Found) return Error{ErrorCategory::StaleRevision, "document-selection-changed"};
    }
    const auto Context = Invocation.context_;
    Request.stillCurrent_ = [Guard = QPointer<CFSModel>(Model), Context, Reader] {
        const auto Panel = Guard ? PanelOf(Guard) : nullptr;
        return Guard && !Guard->IsLoading() && Panel && PanelModel(Panel) == Guard
            && Guard->property("hc.tabId").toString().toStdString() == Context.tabId_
            && Guard->WorkspaceLocation() == Context.location_ && Guard->SnapshotProvider() == Reader;
    };
    return documents_->Capture(std::move(Request)).toStdString();
}
Result<CommandOutcome> CommandUiBridge::executeDocument(const CommandInvocation& Invocation, CFSModel* Model)
{
    QString Token;
    if (const auto At = Invocation.parameters_.find("requestToken"); At != Invocation.parameters_.end()) Token = QString::fromStdString(std::get<std::string>(At->second));
    else { auto Captured = captureDocument(Invocation, Model); if (!Captured.HasValue()) return Captured.Failure(); Token = QString::fromStdString(Captured.Value()); }
    if (!documents_->Open(Token)) return Error{ErrorCategory::Cancelled, "document-request-expired"};
    return CommandOutcome{CommandOutcome::Status::Accepted};
}
Result<CommandOutcome> CommandUiBridge::executeViewer(const CommandInvocation& Invocation, CFSModel* Model)
{
    if (Model->IsLoading()) return Error{ErrorCategory::Unavailable, "navigation-in-progress"};
    auto Reader = Model->SnapshotProvider();
    if (!Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unsupported, "provider-snapshot-required"};
    if (Invocation.context_.selection_.size() != 1) return Error{ErrorCategory::InvalidRequest, "single-selection-required"};
    std::optional<EntrySnapshot> Selected;
    for (int Row = 0; Row < Model->rowCount(); ++Row)
    {
        const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
        if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Invocation.context_.selection_.front())
        { Selected = *Entry->storageEntry_; break; }
    }
    if (!Selected) return Error{ErrorCategory::StaleRevision, "selection-changed"};
    if (Selected->directory_) return Error{ErrorCategory::Unsupported, "viewer-directory"};
    Reader = Model->ProviderFor(*Selected);
    auto Content = std::make_shared<ProviderContentSource>(std::move(Reader), *Selected);
    if (Invocation.id_ == "hc.viewer.open") { viewers_->OpenFull(std::move(Content)); return CommandOutcome{}; }
    auto* SourcePanel = PanelOf(Model);
    if (Invocation.id_ == "hc.viewer.quick")
    {
        CmpPanel* Target = nullptr;
        for (auto* Panel : main_->findChildren<CmpPanel*>()) if (Panel != SourcePanel) { Target = Panel; break; }
        if (!Target || !qobject_cast<CGridView*>(Target->GetFocusView(Target->CurrentTabIndex())))
            return Error{ErrorCategory::Unavailable, "preview-panel-unavailable"};
        Target->OpenQuickView(Target->CurrentTabIndex(), {});
        quick_ = qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex()));
        if (!quick_) return Error{ErrorCategory::Internal, "preview-surface-unavailable"};
        quickPanel_ = Target; quickSource_ = SourcePanel;
        connect(quick_, &CQuickView::CloseRequested, this, [this] { Dispatch("hc.viewer.close", {}, "shortcut"); });
    }
    else if (Invocation.id_ != "hc.viewer.preview") return Error{ErrorCategory::Unsupported, "viewer-command-unavailable"};
    if (!quick_ || quickSource_ != SourcePanel || !quickPanel_
        || quickPanel_->GetFocusView(quickPanel_->CurrentTabIndex()) != quick_)
        return Error{ErrorCategory::Cancelled, "preview-closed"};
    quickKey_ = Model->property("hc.tabId").toString() + QString::fromStdString(Serialize(Selected->id_) + Serialize(Selected->locator_));
    quick_->SetContent(*viewers_, std::move(Content));
    if (Invocation.id_ == "hc.viewer.quick")
        if (auto* Grid = qobject_cast<CGridView*>(SourcePanel->GetFocusView(SourcePanel->CurrentTabIndex()))) Grid->EnsureKeyboardFocusOnView();
    return CommandOutcome{};
}

Result<QStringList> CommandUiBridge::CompareArguments(const QModelIndex& Index)
{
    const auto Context = captureFiles(Index, false);
    return compareArguments(Context, findModel(Context.tabId_));
}

Result<QStringList> CommandUiBridge::compareArguments(const CommandContext& Context, CFSModel* Model)
{
    struct Item { QString path_; bool directory_; };
    const auto Collect = [](const CommandContext& Captured, CFSModel* Source) -> Result<QList<Item>> {
        if (!Source || !Source->GetRoot() || Source->IsLoading() || !(Captured.location_ == Source->WorkspaceLocation()))
            return Error{ErrorCategory::Unavailable, "compare-panel-not-ready"};
        if (Captured.selectionManifest_ || Captured.selection_.empty() || Captured.selection_.size() > 3)
            return Error{ErrorCategory::InvalidRequest, "compare-select-two-or-three-files"};
        auto* Panel = PanelOf(Source);
        auto* View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
        if (!View || View->BaseModel() != Source) return Error{ErrorCategory::Unavailable, "compare-panel-not-ready"};
        auto Candidates = View->SelectedSourceIndexes();
        const auto Row = View->BaseView()->focusedRow();
        const auto Focused = Row.isValid() && Row.type() == Qtitan::GridRow::DataRow ? SourceIndex(Row.modelIndex(0)) : QModelIndex{};
        if (Focused.isValid()) Candidates.append(Focused);
        QList<Item> Items;
        for (const auto& Id : Captured.selection_) {
            bool Found = false;
            for (const auto& Index : Candidates) {
                if (!Index.isValid() || Index.model() != Source) continue;
                const auto Entry = Source->GetFileInfo(Index);
                if (!Entry || Entry->Name_ == "..") continue;
                const auto Current = Entry->storageEntry_ ? Entry->storageEntry_->id_ : ResourceId{{"legacy"}, Entry->Name_.toStdString(), IdentityScope::NavigationSession, {}};
                if (!(Current == Id)) continue;
                const auto Location = Entry->storageEntry_ ? Entry->storageEntry_->locator_ : Source->WorkspaceLocation();
                if (Location.kind_ != LocatorKind::Local && Location.kind_ != LocatorKind::Smb)
                    return Error{ErrorCategory::Unsupported, "compare-native-path-required"};
                const auto Path = QDir::toNativeSeparators(Source->GetFileFullPath(Index));
                if (!QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "compare-invalid-path"};
                Items.append({Path, bool(Entry->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)}); Found = true; break;
            }
            if (!Found) return Error{ErrorCategory::StaleRevision, "compare-selection-changed"};
        }
        return Items;
    };
    auto Selected = Collect(Context, Model); if (!Selected.HasValue()) return Selected.Failure();
    auto Items = Selected.Value();
    if (Items.size() == 1) {
        const auto* Active = PanelOf(Model); CmpPanel* Other = nullptr;
        for (auto* Panel : main_->findChildren<CmpPanel*>())
            if (Panel != Active && (Panel->objectName() == "cmpLeftPanel" || Panel->objectName() == "cmpRightPanel")) { Other = Panel; break; }
        auto* OtherModel = PanelModel(Other);
        if (!OtherModel) return Error{ErrorCategory::Unavailable, "compare-other-panel-not-ready"};
        // Passing the other model's index captures that panel's selection/cursor,
        // without activating it or changing the user's focus.
        auto* OtherView = qobject_cast<CGridView*>(Other->GetFocusView(Other->CurrentTabIndex()));
        if (!OtherView) return Error{ErrorCategory::Unavailable, "compare-other-panel-not-ready"};
        const auto Row = OtherView->BaseView()->focusedRow();
        const auto Cursor = Row.isValid() && Row.type() == Qtitan::GridRow::DataRow ? SourceIndex(Row.modelIndex(0)) : QModelIndex{};
        if (!Cursor.isValid()) return Error{ErrorCategory::InvalidRequest, "compare-other-panel-selection-required"};
        const auto OtherContext = captureFiles(Cursor, false);
        auto Opposite = Collect(OtherContext, OtherModel); if (!Opposite.HasValue()) return Opposite.Failure();
        if (Opposite.Value().size() != 1) return Error{ErrorCategory::InvalidRequest, "compare-one-item-per-panel"};
        if (Active && Active->objectName() == "cmpRightPanel") Items.prepend(Opposite.Value().front());
        else Items.append(Opposite.Value().front());
    }
    for (int i = 0; i < Items.size(); ++i) {
        if (Items[i].directory_ != Items[0].directory_) return Error{ErrorCategory::InvalidRequest, "compare-mixed-files-and-folders"};
        for (int j = 0; j < i; ++j)
            if (QDir::cleanPath(Items[i].path_).compare(QDir::cleanPath(Items[j].path_), Qt::CaseInsensitive) == 0)
                return Error{ErrorCategory::InvalidRequest, "compare-same-path"};
    }
    if (Items[0].directory_ && Items.size() != 2) return Error{ErrorCategory::InvalidRequest, "compare-select-two-folders"};
    QStringList Arguments{"-mode", Items[0].directory_ ? "directory" : "compare"};
    for (int i = 0; i < Items.size(); ++i) Arguments << QString("-file%1").arg(i + 1) << Items[i].path_;
    return Arguments;
}

CommandContext CommandUiBridge::captureFiles(const QModelIndex& Index, bool Single)
{
    auto Context = Capture(Index);
    auto* Model = findModel(Context.tabId_);
    if (!Model) return Context;
    const auto Panel = PanelOf(Model);
    const auto View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
    if (!View || View->BaseModel() != Model) return Context;
    const auto Grid = View->BaseView();
    if (!Single && Model->UsesIndexedOrder() && Model->property("hc.selectAllEntries").toBool()) {
        auto Manifest = Model->SelectAllEntries();
        if (Manifest.HasValue()) { Context.selectionManifest_ = Manifest.Value(); Context.selection_.clear(); return Context; }
    }
    QList<QModelIndex> Selected;
    if (!Single)
        for (const auto& SelectedIndex : View->SelectedSourceIndexes()) Selected.push_back(SelectedIndex);
    if (Selected.isEmpty())
    {
        const auto Focused = Grid->focusedRow();
        const auto Cursor = Index.isValid() ? SourceIndex(Index) : Focused.isValid() && Focused.type() == Qtitan::GridRow::DataRow ? SourceIndex(Focused.modelIndex(0)) : QModelIndex{};
        if (Cursor.isValid()) Selected.push_back(Cursor);
    }
    Context.selection_.clear();
    for (const auto& At : Selected)
    {
        if (At.model() != Model || !At.isValid()) continue;
        const auto Entry = Model->GetFileInfo(At);
        if (!Entry || Entry->Name_ == "..") continue;
        Context.selection_.push_back(Entry->storageEntry_ ? Entry->storageEntry_->id_
            : ResourceId{{"legacy"}, Entry->Name_.toStdString(), IdentityScope::NavigationSession, {}});
    }
    return Context;
}

void CommandUiBridge::PromptFileOperation(const QString& Id, const QString& Source, const QModelIndex& Index)
{
    // Capture before showing a dialog: a nested Qt event loop can change tabs.
    CommandInvocation Invocation;
    Invocation.id_ = Id.toStdString(); Invocation.source_ = Source.toStdString();
    Invocation.permissions_.insert("file-write");
    Invocation.context_ = captureFiles(Index, Id == "hc.file.rename");
    Invocation.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    auto* Model = findModel(Invocation.context_.tabId_);
    if (!Model || !Model->GetRoot()) return;
    if (Model->IsSearchResults() && Id == "hc.file.mkdir") {
        hc::ui::MessageDialog::information(main_, tr("새 폴더"), tr("검색 결과에는 기본 목적지가 없습니다. 실제 폴더를 연 뒤 실행하세요.")); return;
    }
    if (Id == "hc.file.rename") {
        const auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
        if (Source == "cell-click" && !Options["renameOnSlowClick"].toBool(true)) return;
        const auto Panel = PanelOf(Model);
        const auto View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
        if (!Model->IsSearchResults() && (Source == "cell-click" || !Options["renameInDialog"].toBool(true)) && View && !View->IsThumbnailMode()) {
            View->BeginRename(Index, Options["renameSelectExtension"].toBool(false),
                [Guard = QPointer<CommandUiBridge>(this)](const QModelIndex& At, const QString& Name, CGridView::RenameCompletion Completion) {
                    if (Guard) Guard->submitInlineRename(At, Name, std::move(Completion));
                    else Completion(false);
                });
            return;
        }
    }
    hc::ui::FileOperationDialogRequest DialogRequest;
    DialogRequest.command_ = Id;
    DialogRequest.source_ = QString::fromStdString(Invocation.context_.location_.root_);
    DialogRequest.manifestSelection_ = bool(Invocation.context_.selectionManifest_);
    DialogRequest.nativeSource_ = Invocation.context_.location_.kind_ == LocatorKind::Local || Invocation.context_.location_.kind_ == LocatorKind::Smb;
    if (Model->IsSearchResults()) DialogRequest.nativeSource_ = true;
    for (const auto& Selected : Invocation.context_.selection_)
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
            if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Selected) {
                if (Model->IsSearchResults() && DialogRequest.names_.isEmpty())
                    DialogRequest.source_ = QFileInfo(QString::fromStdString(Entry->storageEntry_->locator_.root_)).absolutePath();
                DialogRequest.names_.append(Model->IsSearchResults() && Id != "hc.file.rename"
                    ? Model->GetFileFullPath(Model->index(Row, 0)) : Entry->Name_); break;
            }
        }
    if (Id == "hc.file.delete") {
        hc::ui::FileOperationDialog Dialog(DialogRequest, main_);
        if (Dialog.exec() != QDialog::Accepted) return;
        Invocation.parameters_["deleteMode"] = std::string(Dialog.Recycle() ? "recycle" : "permanent");
        Invocation.parameters_["allowReadOnly"] = Dialog.AllowReadOnly();
        Invocation.parameters_["confirmed"] = true;
    }
    if (Id != "hc.file.delete")
    {
        const bool NameOnly = Id == "hc.file.rename" || Id == "hc.file.mkdir";
        auto Destination = Invocation.context_.location_;
        if (Model->IsSearchResults()) {
            const auto Native = QDir::toNativeSeparators(DialogRequest.source_);
            Destination = {Native.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, Native.toStdString()};
        }
        std::string DestinationTab;
        if (!NameOnly)
            for (auto* Other : main_->findChildren<CmpPanel*>())
                if (Other != PanelOf(Model))
                    if (auto* Target = PanelModel(Other); Target && Target->GetRoot() && !Target->IsSearchResults())
                    {
                        Destination = Target->GetRoot()->ReadLocation(Target->GetCurrentPathWithRoot());
                        DestinationTab = Target->property("hc.tabId").toString().toStdString();
                    }
        const bool Archive = Destination.kind_ == LocatorKind::Archive;
        DialogRequest.copyEngine_ = !Archive && (Invocation.context_.location_.kind_ != LocatorKind::Archive || Id == "hc.file.copy");
        auto Default = Archive ? QString::fromStdString(Destination.nesting_.back().entry_) : QString::fromStdString(Destination.root_);
        if (NameOnly) Default = Id == "hc.file.mkdir" ? tr("새 폴더") : DialogRequest.names_.value(0);
        DialogRequest.initialValue_ = Default;
        DialogRequest.relativeBase_ = DialogRequest.nativeSource_ ? DialogRequest.source_
            : Invocation.context_.location_.kind_ == LocatorKind::Archive ? QFileInfo(DialogRequest.source_).absolutePath() : Default;
        DialogRequest.nativeDestination_ = (Destination.kind_ == LocatorKind::Local || Destination.kind_ == LocatorKind::Smb);
        DialogRequest.providerKey_ = Destination.kind_ == LocatorKind::Local ? "local" : Destination.kind_ == LocatorKind::Smb ? "smb" : "";
        hc::ui::FileOperationDialog Dialog(DialogRequest, main_);
        if (auto* Policy = Dialog.findChild<QComboBox*>("hc.operation.conflict")) {
            const auto DefaultPolicy = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["fileConflict"].toString("ask");
            Policy->setCurrentIndex(qMax(0, Policy->findData(DefaultPolicy)));
        }
        if (Dialog.exec() != QDialog::Accepted) return;
        const auto Input = Dialog.Input();
        Invocation.parameters_["createParents"] = Dialog.CreateParents();
        if (!NameOnly) Invocation.parameters_["queued"] = Dialog.Queued();
        if (!NameOnly && DialogRequest.copyEngine_) {
            Invocation.parameters_["copyMetadata"] = Dialog.PreserveMetadata(); Invocation.parameters_["copyVerify"] = Dialog.VerifyCopy();
            Invocation.parameters_["copyAcl"] = Dialog.CopyAcl(); Invocation.parameters_["copyStreams"] = Dialog.CopyStreams();
            Invocation.parameters_["copyPattern"] = Dialog.IncludePattern().toStdString(); Invocation.parameters_["copySpeed"] = std::int64_t(Dialog.SpeedLimit());
        }
        Invocation.parameters_["targetIsName"] = Dialog.TargetIsName();
        Invocation.parameters_["openCreated"] = Dialog.OpenCreatedFolder();

        if (!NameOnly) Invocation.parameters_["conflict"] = Dialog.Conflict().toStdString();
        if (Destination.kind_ == LocatorKind::Plugin)
        {
            auto Target = Destination;
            auto Path = QDir::toNativeSeparators(Input);
            const auto PrefixEnd = Destination.root_.find('\\');
            const auto Prefix = QString::fromStdString(Destination.root_.substr(0, PrefixEnd));
            if (NameOnly)
            {
                if (Path.contains('\\') || Path.contains(':') || Path == "." || Path == ".." || Path.contains(QChar::Null)) return;
                Target.root_ += (Target.root_.ends_with('\\') ? "" : "\\") + Path.toStdString();
            }
            else
            {
                if (Path.startsWith('\\')) Path = Prefix + Path;
                if (!Path.startsWith(Prefix + "\\")) return;
                Target.root_ = Path.toStdString();
                Invocation.parameters_["destinationTab"] = DestinationTab;
                Invocation.parameters_["destinationLocation"] = Serialize(Destination);
            }
            Invocation.parameters_["destination"] = Serialize(Target);
        }
        else if (Archive)
        {
            auto Target = Destination;
            const auto Base = QString::fromStdString(Destination.nesting_.back().entry_);
            const auto Internal = QDir::fromNativeSeparators(Input);
            Target.nesting_.back().entry_ = QDir::cleanPath(Internal.startsWith('/') ? Internal : QDir(Base).filePath(Internal)).toStdString();
            Invocation.parameters_["destination"] = Serialize(Target);
            if (!NameOnly)
            {
                Invocation.parameters_["destinationTab"] = DestinationTab;
                Invocation.parameters_["destinationLocation"] = Serialize(Destination);
            }
        }
        else {
            const auto Target = Dialog.ResolvedTarget();
            Invocation.parameters_["destination"] = Target.toStdString();
            if (!NameOnly) {
                const bool Unc = Target.startsWith("\\\\") && (!Target.startsWith("\\\\?\\") || Target.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive));
                Invocation.parameters_["recentProvider"] = std::string(Unc ? "smb" : "local");
            }
        }
    }
    DispatchInvocation(std::move(Invocation));
}

void CommandUiBridge::submitInlineRename(const QModelIndex& Index, const QString& Name, std::function<void(bool)> Completion)
{
    const auto Error = hc::filetools::NameError(Name);
    if (!Error.isEmpty()) {
        if (auto* Window = qobject_cast<QMainWindow*>(main_.data())) Window->statusBar()->showMessage(Error, 6000);
        Completion(false); return;
    }
    CommandInvocation Invocation;
    Invocation.id_ = "hc.file.rename"; Invocation.source_ = "inline";
    Invocation.permissions_.insert("file-write"); Invocation.context_ = captureFiles(Index, true);
    Invocation.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    auto Destination = Invocation.context_.location_;
    if (Destination.kind_ == LocatorKind::Plugin)
        Destination.root_ += (Destination.root_.ends_with('\\') ? "" : "\\") + Name.toStdString();
    else if (Destination.kind_ == LocatorKind::Archive && !Destination.nesting_.empty())
        Destination.nesting_.back().entry_ = QDir(QString::fromStdString(Destination.nesting_.back().entry_)).filePath(Name).toStdString();
    else Destination.root_ = QDir(QString::fromStdString(Destination.root_)).filePath(Name).toStdString();
    Invocation.parameters_["destination"] = Serialize(Destination);
    const auto Correlation = Invocation.correlationId_;
    inlineRenameCompletions_[Correlation] = std::move(Completion);
    auto Future = std::make_shared<Task<CommandOutcome>>(DispatchInvocation(std::move(Invocation)));
    auto* Poll = new QTimer(this); Poll->setInterval(10);
    connect(Poll, &QTimer::timeout, this, [this, Poll, Future, Correlation] {
        if (Future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto Result = Future->get(); Poll->stop(); Poll->deleteLater();
        if (!Result.HasValue()) {
            if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
                Window->statusBar()->showMessage(tr("이름을 변경할 수 없습니다: %1").arg(QString::fromStdString(Result.Failure().code_)), 8000);
        }
        if (!Result.HasValue() || Result.Value().jobId_.empty()) {
            if (auto It = inlineRenameCompletions_.find(Correlation); It != inlineRenameCompletions_.end()) {
                auto Done = std::move(It->second); inlineRenameCompletions_.erase(It); Done(false);
            }
        }
    });
    Poll->start();
}

bool CommandUiBridge::ownsFileClipboard() const
{
    const auto* Mime = QApplication::clipboard()->mimeData();
    return !clipboardToken_.isEmpty() && Mime && (clipboardExternal_ ? NativeClipboardToken(Mime) == clipboardToken_
        : Mime->data("application/x-hypercommander-files") == clipboardToken_);
}

void CommandUiBridge::updateClipboard()
{
    if (HasFileClipboard() && ownsFileClipboard()) { refreshActionState(); return; }
    jobSelections_.erase("clipboard");
    clipboardImportStop_.request_stop(); clipboardImportStop_ = std::stop_source{};
    const auto Generation = ++clipboardGeneration_;
    clipboard_.reset(); clipboardToken_.clear(); clipboardLocations_.clear(); cutKeys_.clear(); cutRoot_.clear();
    clipboardExternal_ = false; clipboardImporting_ = false;
    refreshClipboardVisuals(); refreshActionState();
    const auto* Mime = QApplication::clipboard()->mimeData();
    const auto Token = NativeClipboardToken(Mime);
    if (Token.isEmpty()) return;
    const auto Urls = Mime->urls();
    const auto Effect = Mime->data("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
    const bool Cut = Effect == QByteArray("\x02\0\0\0", 4);
    clipboardToken_ = Token; clipboardExternal_ = true; clipboardImporting_ = true;
    OperationContext Context; Context.stopToken_ = clipboardImportStop_.get_token();
    using ClipboardResult = Result<hc::operations::FileOperationRequest>;
    auto* Watcher = new QFutureWatcher<ClipboardResult>(this);
    connect(Watcher, &QFutureWatcher<ClipboardResult>::finished, this, [this, Watcher, Generation, Cut] {
        auto Loaded = Watcher->result(); Watcher->deleteLater();
        if (Generation != clipboardGeneration_ || !clipboardImporting_ || !ownsFileClipboard()) return;
        clipboardImporting_ = false;
        if (!Loaded.HasValue()) {
            clipboardToken_.clear(); clipboardExternal_ = false;
            if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
                Window->statusBar()->showMessage(tr("클립보드 파일을 확인할 수 없습니다: %1").arg(QString::fromStdString(Loaded.Failure().code_)), 6000);
            refreshActionState(); return;
        }
        clipboard_ = std::move(Loaded.Value());
        for (const auto& Source : clipboard_->sources_) {
            if (Cut) cutKeys_.insert(hc::workspace::SelectionKey(Source.entry_));
            auto Parent = Source.entry_.locator_; Parent.root_ = QFileInfo(QString::fromStdString(Parent.root_)).absolutePath().toStdString();
            clipboardLocations_.push_back(std::move(Parent));
        }
        refreshClipboardVisuals(); refreshActionState();
    });
    // Resolve the native clipboard's immutable URL list away from the GUI thread.
    // A generation and stop token prevent a replaced/cancelled import from returning.
    Watcher->setFuture(QtConcurrent::run([Urls, Cut, Context]() -> ClipboardResult {
        return NativeFileRequest(Urls, Cut, Context);
    }));
}

void CommandUiBridge::DropFiles(const QStringList& Paths, const QString& Destination, Qt::DropAction Action)
{
    using namespace hc::operations;
    if (Paths.isEmpty() || Destination.contains(QChar::Null) || !QDir::isAbsolutePath(Destination)
        || (Action != Qt::CopyAction && Action != Qt::MoveAction && Action != Qt::LinkAction)) return;
    const auto Target = QDir::toNativeSeparators(QDir::cleanPath(Destination));
    const auto Kind = Target.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive) || (Target.startsWith("\\\\") && !Target.startsWith("\\\\?\\"))
        ? LocatorKind::Smb : LocatorKind::Local;
    const Locator At{Kind, Target.toStdString()};
    if (Action == Qt::LinkAction) {
        PreparedJob Job; Job.request_.maxRetries_ = 0; Job.request_.resourceLanes_ = {FileOperationLane(At)};
        Job.request_.metadata_.source_ = Paths.front().toStdString(); Job.request_.metadata_.destination_ = At.root_;
        Job.task_ = std::make_unique<ShortcutDropTask>(Paths, Target);
        auto Started = fileJobs_->Submit(std::move(Job), tr("바로 가기 만들기"));
        if (Started.HasValue()) affectedLocations_[Started.Value().id_] = {At};
        else hc::ui::MessageDialog::warning(main_, tr("끌어다 놓기"), QString::fromStdString(Started.Failure().code_));
        return;
    }
    const auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
    QList<QUrl> Urls; for (const auto& Path : Paths) Urls.append(QUrl::fromLocalFile(Path));
    using ImportResult = Result<FileOperationRequest>;
    auto* Watcher = new QFutureWatcher<ImportResult>(this);
    connect(Watcher, &QFutureWatcher<ImportResult>::finished, this, [this, Watcher, At, Options] {
        auto Loaded = Watcher->result(); Watcher->deleteLater();
        if (!Loaded.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("끌어다 놓기"), tr("파일을 확인할 수 없습니다: %1").arg(QString::fromStdString(Loaded.Failure().code_))); return; }
        auto Request = std::move(Loaded.Value()); Request.destination_ = At;
        Request.destinationWriter_ = MakeNativeWritableStorage(At.kind_, MakeNativeReadProvider(At.kind_));
        Request.copyOptions_.metadata_ = Options["copyMetadata"].toBool(true); Request.copyOptions_.verify_ = Options["copyVerify"].toBool(false);
        Request.copyOptions_.acl_ = Options["copyAcl"].toBool(false); Request.copyOptions_.streams_ = Options["copyStreams"].toBool(false);
        Request.bytesPerSecondLimit_ = quint64(qBound(0, Options["copySpeedMiB"].toInt(0), 1048576)) * 1024 * 1024;
        std::vector<Locator> Affected{At};
        for (const auto& Source : Request.sources_) {
            auto Parent = Source.entry_.locator_; Parent.root_ = QFileInfo(QString::fromStdString(Parent.root_)).absolutePath().toStdString(); Affected.push_back(std::move(Parent));
        }
        auto Started = fileJobs_->Submit(std::move(Request), OperationContext{});
        if (Started.HasValue()) affectedLocations_[Started.Value().id_] = std::move(Affected);
        else hc::ui::MessageDialog::warning(main_, tr("끌어다 놓기"), QString::fromStdString(Started.Failure().code_));
    });
    // Resolve immutable source paths away from the GUI thread; navigation and
    // clipboard changes during import must not redirect this drop.
    Watcher->setFuture(QtConcurrent::run([Urls, Action] { return NativeFileRequest(Urls, Action == Qt::MoveAction, {}); }));
}

void CommandUiBridge::refreshClipboardVisuals()
{
    if (main_) for (auto* Model : main_->findChildren<CFSModel*>()) Model->SetClipboardCut(cutKeys_, cutRoot_);
}

bool CommandUiBridge::CancelClipboard()
{
    if (!HasFileClipboard()) return false;
    jobSelections_.erase("clipboard");
    const bool Owned = ownsFileClipboard();
    clipboardImportStop_.request_stop(); ++clipboardGeneration_; clipboardImporting_ = false; clipboardExternal_ = false;
    clipboard_.reset(); clipboardToken_.clear(); cutKeys_.clear(); cutRoot_.clear(); clipboardLocations_.clear();
    if (Owned) QApplication::clipboard()->clear();
    refreshClipboardVisuals(); refreshActionState();
    return true;
}

Result<CommandOutcome> CommandUiBridge::executeClipboard(const CommandInvocation& Invocation, CFSModel* Model)
{
    using namespace hc::operations;
    if (Model->IsLoading() || !Model->ProviderEnabled() || !Model->SnapshotProvider())
        return Error{ErrorCategory::Unavailable, "clipboard-provider-not-ready"};
    if (Invocation.context_.location_ != Model->WorkspaceLocation())
        return Error{ErrorCategory::StaleRevision, "navigation-context-changed"};
    const auto Kind = Invocation.context_.location_.kind_;
    if (Invocation.id_ == "hc.clipboard.paste") {
        if (!clipboard_ || !ownsFileClipboard()) return Error{ErrorCategory::Unavailable, "file-clipboard-empty"};
        if (Model->IsSearchResults()) return Error{ErrorCategory::InvalidRequest, "search-results-have-no-destination"};
        if (Kind != LocatorKind::Local && Kind != LocatorKind::Smb && Kind != LocatorKind::Plugin)
            return Error{ErrorCategory::Unsupported, "clipboard-destination-not-writable"};
        if (std::any_of(clipboardMoves_.begin(), clipboardMoves_.end(), [this](const auto& Move) { return Move.second == clipboardToken_; }))
            return Error{ErrorCategory::Unavailable, "clipboard-move-in-progress"};
        auto Request = *clipboard_;
        Request.destination_ = Invocation.context_.location_;
        Request.destinationWriter_ = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider()
            : MakeNativeWritableStorage(Kind, MakeNativeReadProvider(Kind));
        if (!Request.destinationWriter_) return Error{ErrorCategory::Unsupported, "clipboard-destination-not-writable"};
        Request.correlationId_ = Invocation.correlationId_;
        const auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
        Request.copyOptions_.metadata_ = Options["copyMetadata"].toBool(true);
        Request.copyOptions_.verify_ = Options["copyVerify"].toBool(false);
        Request.copyOptions_.acl_ = Options["copyAcl"].toBool(false);
        Request.copyOptions_.streams_ = Options["copyStreams"].toBool(false);
        Request.bytesPerSecondLimit_ = quint64(qBound(0, Options["copySpeedMiB"].toInt(0), 1048576)) * 1024 * 1024;
        auto Started = fileJobs_->Submit(std::move(Request), Invocation.operation_);
        if (!Started.HasValue()) return Started.Failure();
        auto Affected = clipboardLocations_; Affected.push_back(Invocation.context_.location_);
        affectedLocations_[Started.Value().id_] = std::move(Affected);
        if (const auto Selection = jobSelections_.find("clipboard"); Selection != jobSelections_.end()) jobSelections_[Started.Value().id_] = Selection->second;
        if (clipboard_->operation_ == hc::operations::FileOperation::Move) clipboardMoves_[Started.Value().id_] = clipboardToken_;
        return CommandOutcome{CommandOutcome::Status::Accepted, Started.Value().id_};
    }
    const bool Cut = Invocation.id_ == "hc.clipboard.cut";
    if (!Cut && Invocation.id_ != "hc.clipboard.copy") return Error{ErrorCategory::Unsupported, "clipboard-command-unknown"};
    if (Cut && Kind == LocatorKind::Archive) return Error{ErrorCategory::Unsupported, "clipboard-archive-cut-unavailable"};
    FileOperationRequest Request; Request.operation_ = Cut ? hc::operations::FileOperation::Move : hc::operations::FileOperation::Copy;
    const auto Reader = Model->SnapshotProvider();
    const auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider()
        : Kind == LocatorKind::Local || Kind == LocatorKind::Smb ? MakeNativeWritableStorage(Kind, Reader) : nullptr;
    QSet<QString> Keys;
    QList<QUrl> Urls;
    std::vector<Locator> Locations;
    if (!Model->IsSearchResults()) Locations.push_back(Invocation.context_.location_);
    if (Invocation.context_.selectionManifest_) {
        Request.selection_ = Invocation.context_.selectionManifest_;
        if (Request.selection_->Provider() != Reader) return Error{ErrorCategory::StaleRevision, "selection-provider-changed"};
        Request.selectionRoot_ = Invocation.context_.location_; Request.selectionWriter_ = Writer;
        if (!Request.selection_->Count()) return Error{ErrorCategory::InvalidRequest, "clipboard-selection-empty"};
        if (Cut && !Writer) return Error{ErrorCategory::Unsupported, "clipboard-source-not-writable"};
    } else {
        // Match all IDs in one pass; retaining snapshots keeps the clipboard valid
        // when the source tab changes folder or closes before paste.
        std::set<std::string> Selected;
        for (const auto& Id : Invocation.context_.selection_) Selected.insert(Serialize(Id));
        if (Selected.empty()) return Error{ErrorCategory::InvalidRequest, "clipboard-selection-empty"};
        for (int Row = 0; Row < Model->rowCount(); ++Row) {
            const auto Item = Model->GetFileInfo(Model->index(Row, 0));
            if (!Item || Item->Name_ == ".." || !Item->storageEntry_ || !Selected.contains(Serialize(Item->storageEntry_->id_))) continue;
            const auto& Entry = *Item->storageEntry_;
            auto SourceReader = Model->ProviderFor(Entry);
            auto SourceWriter = Model->IsSearchResults() ? MakeNativeWritableStorage(Entry.locator_.kind_, SourceReader) : Writer;
            if (!SourceReader || (Cut && !SourceWriter)) return Error{ErrorCategory::Unsupported, "clipboard-source-not-writable"};
            Request.sources_.push_back({Entry, SourceReader, SourceWriter});
            Keys.insert(hc::workspace::SelectionKey(Entry));
            if (Entry.locator_.kind_ == LocatorKind::Local || Entry.locator_.kind_ == LocatorKind::Smb) {
                Urls.append(QUrl::fromLocalFile(QString::fromStdString(Entry.locator_.root_)));
                if (Model->IsSearchResults()) {
                    auto Parent = Entry.locator_; Parent.root_ = QFileInfo(QString::fromStdString(Parent.root_)).absolutePath().toStdString();
                    Locations.push_back(std::move(Parent));
                }
            }
        }
        if (Request.sources_.empty()) return Error{ErrorCategory::InvalidRequest, "clipboard-selection-empty"};
        if (Request.sources_.size() != Selected.size()) return Error{ErrorCategory::StaleRevision, "selection-changed"};
    }
    auto* Mime = new QMimeData;
    const auto Token = QUuid::createUuid().toByteArray();
    Mime->setData("application/x-hypercommander-files", Token);
    if (!Urls.isEmpty() && Urls.size() == Request.sources_.size()) {
        Mime->setUrls(Urls);
        // Qt's native MIME bridge publishes Explorer's preferred copy/move effect.
        QByteArray Effect(4, '\0'); Effect[0] = Cut ? 2 : 1;
        Mime->setData("application/x-qt-windows-mime;value=\"Preferred DropEffect\"", Effect);
    }
    clipboardImportStop_.request_stop(); ++clipboardGeneration_; clipboardImporting_ = false; clipboardExternal_ = false;
    clipboard_ = std::move(Request); clipboardToken_ = Token; clipboardLocations_ = std::move(Locations);
    cutKeys_ = Cut ? std::move(Keys) : QSet<QString>{};
    cutRoot_ = Cut && clipboard_->selection_ ? hc::workspace::LocationKey(Invocation.context_.location_) : QString{};
    jobSelections_.erase("clipboard"); rememberJobSelection("clipboard", Invocation.context_);
    QApplication::clipboard()->setMimeData(Mime);
    if (!ownsFileClipboard()) {
        CancelClipboard(); return Error{ErrorCategory::Unavailable, "clipboard-write-failed"};
    }
    refreshClipboardVisuals(); refreshActionState();
    return CommandOutcome{};
}

Result<CommandOutcome> CommandUiBridge::executeFile(const CommandInvocation& Invocation, CFSModel* Model)
{
    using namespace hc::operations;
    if (Model->IsLoading()) return Error{ErrorCategory::InvalidRequest, "navigation-in-progress"};
    if (Model->IsSearchResults() && (Invocation.id_ == "hc.file.new" || Invocation.id_ == "hc.file.mkdir"))
        return Error{ErrorCategory::InvalidRequest, "search-results-have-no-destination"};
    if (Invocation.id_ == "hc.file.new") {
        const auto Kind = Invocation.context_.location_.kind_; auto Reader = Model->SnapshotProvider();
        if (!Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unsupported, "provider-snapshot-required"};
        auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider()
            : Kind == LocatorKind::Local || Kind == LocatorKind::Smb ? MakeNativeWritableStorage(Kind, Reader) : nullptr;
        const auto Name = QString::fromStdString(std::get<std::string>(Invocation.parameters_.at("name")));
        const auto Type = Invocation.parameters_.contains("template") ? std::get<std::int64_t>(Invocation.parameters_.at("template")) : 0;
        if (Type < 0 || Type >= hc::filetools::Templates().size()) return Error{ErrorCategory::InvalidRequest, "invalid-file-template"};
        const bool Bom = Invocation.parameters_.contains("bom") && std::get<bool>(Invocation.parameters_.at("bom"));
        auto Prepared = hc::filetools::PrepareCreate(Writer, Invocation.context_.location_, Name, int(Type), Bom, Invocation.operation_, fileJobs_->ElevationService(), !Invocation.operation_.workload_.IsAutomatic());
        if (!Prepared.HasValue()) return Prepared.Failure();
        Prepared.Value().request_.correlationId_ = Invocation.correlationId_;
        Prepared.Value().request_.deferred_ = Invocation.parameters_.contains("queued") && std::get<bool>(Invocation.parameters_.at("queued"));
        auto Submitted = fileJobs_->Submit(std::move(Prepared.Value()), tr("새 파일 · %1").arg(Name)); if (!Submitted.HasValue()) return Submitted.Failure();
        affectedLocations_[Submitted.Value().id_] = {Invocation.context_.location_};
        createdFileFocus_[Submitted.Value().id_] = {Invocation.context_.tabId_, Invocation.context_.location_, Model, Name, false};
        CommandOutcome Outcome; Outcome.jobId_ = Submitted.Value().id_; return Outcome;
    }
    FileOperationRequest Request;
    Request.operation_ = Invocation.id_ == "hc.file.copy" ? hc::operations::FileOperation::Copy : Invocation.id_ == "hc.file.move" ? hc::operations::FileOperation::Move
        : Invocation.id_ == "hc.file.rename" ? hc::operations::FileOperation::Rename : Invocation.id_ == "hc.file.mkdir" ? hc::operations::FileOperation::MakeDirectory : hc::operations::FileOperation::Delete;
    Request.correlationId_ = Invocation.correlationId_;
    const auto Flag = [&](const char* Name) {
        const auto It = Invocation.parameters_.find(Name);
        return It != Invocation.parameters_.end() && std::holds_alternative<bool>(It->second) && std::get<bool>(It->second);
    };
    Request.copyOptions_.metadata_ = !Invocation.parameters_.contains("copyMetadata") || Flag("copyMetadata");
    Request.copyOptions_.verify_ = Flag("copyVerify"); Request.copyOptions_.acl_ = Flag("copyAcl"); Request.copyOptions_.streams_ = Flag("copyStreams");
    if (const auto It = Invocation.parameters_.find("copyPattern"); It != Invocation.parameters_.end()) Request.includePattern_ = std::get<std::string>(It->second);
    if (const auto It = Invocation.parameters_.find("copySpeed"); It != Invocation.parameters_.end()) {
        const auto Speed = std::get<std::int64_t>(It->second); if (Speed < 0 || Speed > (1ll << 40)) return Error{ErrorCategory::InvalidRequest, "invalid-copy-speed"};
        Request.bytesPerSecondLimit_ = std::uint64_t(Speed);
    }
    if (!IsValidCopyPattern(Request.includePattern_)) return Error{ErrorCategory::InvalidRequest, "invalid-copy-pattern"};
    Request.createParents_ = Flag("createParents");
    Request.confirmDelete_ = !Flag("confirmed");
    Request.readOnly_ = Flag("allowReadOnly") ? ReadOnlyPolicy::Allow : ReadOnlyPolicy::Ask;
    if (const auto Mode = Invocation.parameters_.find("deleteMode"); Mode != Invocation.parameters_.end()) {
        const auto* Value = std::get_if<std::string>(&Mode->second);
        if (!Value || (*Value != "recycle" && *Value != "permanent")) return Error{ErrorCategory::InvalidRequest, "invalid-delete-mode"};
        Request.deleteMode_ = *Value == "recycle" ? DeleteMode::Recycle : DeleteMode::Permanent;
    }
    if (const auto It = Invocation.parameters_.find("conflict"); It != Invocation.parameters_.end()) {
        const auto& Policy = std::get<std::string>(It->second);
        if (Policy != "ask" && Policy != "replace" && Policy != "skip")
            return Error{ErrorCategory::InvalidRequest, "invalid-conflict-policy"};
        Request.conflict_ = Policy == "replace" ? ConflictPolicy::Replace : Policy == "skip" ? ConflictPolicy::Skip : ConflictPolicy::Ask;
    }
    const auto Kind = Model->IsSearchResults() ? LocatorKind::Local : Invocation.context_.location_.kind_;
    if (Kind != LocatorKind::Local && Kind != LocatorKind::Smb && Kind != LocatorKind::Archive && Kind != LocatorKind::Plugin)
        return Error{ErrorCategory::Unsupported, "file-operation-provider-required"};
    auto Reader = Model->SnapshotProvider();
    if (!Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unsupported, "provider-snapshot-required"};
    auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider() : Kind == LocatorKind::Archive ? nullptr : MakeNativeWritableStorage(Kind, Reader);
    if (Invocation.context_.selectionManifest_) {
        if (Invocation.context_.selectionManifest_->Provider() != Reader) return Error{ErrorCategory::StaleRevision, "selection-provider-changed"};
        Request.selection_ = Invocation.context_.selectionManifest_; Request.selectionRoot_ = Invocation.context_.location_; Request.selectionWriter_ = Writer;
    }
    if (Request.operation_ != hc::operations::FileOperation::MakeDirectory)
        for (const auto& Selected : Invocation.context_.selection_)
        {
            bool Found = false;
            for (int Row = 0; Row < Model->rowCount(); ++Row)
            {
                const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
                if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Selected)
                {
                    auto EntryReader = Model->ProviderFor(*Entry->storageEntry_);
                    auto EntryWriter = Model->IsSearchResults() ? MakeNativeWritableStorage(Entry->storageEntry_->locator_.kind_, EntryReader) : Writer;
                    Request.sources_.push_back({*Entry->storageEntry_, EntryReader, EntryWriter}); Found = true; break;
                }
            }
            if (!Found) return Error{ErrorCategory::StaleRevision, "selection-changed-or-provider-disabled"};
        }
    if (Request.operation_ != hc::operations::FileOperation::Delete)
    {
        const auto Text = std::get<std::string>(Invocation.parameters_.at("destination"));
        if (Text.starts_with("hcl1"))
        {
            auto Parsed = ParseLocator(Text); if (!Parsed.HasValue()) return Parsed.Failure();
            Request.destination_ = std::move(Parsed.Value());
        }
        else if (Kind == LocatorKind::Archive && (Request.operation_ == hc::operations::FileOperation::Rename || Request.operation_ == hc::operations::FileOperation::MakeDirectory))
        {
            Request.destination_ = Invocation.context_.location_;
            const auto Path = QDir::fromNativeSeparators(QString::fromStdString(Text));
            Request.destination_.nesting_.back().entry_ = Path.startsWith('/') ? Path.toStdString()
                : QDir(QString::fromStdString(Request.destination_.nesting_.back().entry_)).filePath(Path).toStdString();
        }
        else
        {
            auto Path = QDir::toNativeSeparators(QString::fromStdString(Text));
            if (Path.isEmpty() || Path.contains(QChar::Null) || !QDir::isAbsolutePath(Path)) return Error{ErrorCategory::InvalidRequest, "absolute-destination-required"};
            Request.destination_ = {Path.startsWith("\\\\") && !Path.startsWith("\\\\?\\") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
            if (Path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive)) Request.destination_.kind_ = LocatorKind::Smb;
        }
    }
    const auto FullTarget = Request.destination_;
    const bool NativeTarget = Request.destination_.kind_ == LocatorKind::Local || Request.destination_.kind_ == LocatorKind::Smb;
    const auto SourceParent = Model->IsSearchResults() && Request.sources_.size() == 1
        ? QFileInfo(QString::fromStdString(Request.sources_[0].entry_.locator_.root_)).absolutePath() : QString::fromStdString(Invocation.context_.location_.root_);
    const bool RenameToOtherFolder = Request.operation_ == hc::operations::FileOperation::Rename && NativeTarget
        && QDir::cleanPath(QFileInfo(QString::fromStdString(Request.destination_.root_)).absolutePath()).compare(QDir::cleanPath(SourceParent), Qt::CaseInsensitive) != 0;
    if (NativeTarget && (Flag("targetIsName") || RenameToOtherFolder)) {
        if (Request.selection_ || Request.sources_.size() != 1) return Error{ErrorCategory::InvalidRequest, "single-source-name-required"};
        const QFileInfo Target(QString::fromStdString(Request.destination_.root_));
        Request.destinationName_ = Target.fileName().toStdString(); Request.destination_.root_ = QDir::toNativeSeparators(Target.absolutePath()).toStdString();
        if (RenameToOtherFolder) Request.operation_ = hc::operations::FileOperation::Move;
    }
    if (Request.deleteMode_ == DeleteMode::Recycle && Kind != LocatorKind::Local)
        return Error{ErrorCategory::Unsupported, "recycle-requires-fixed-local-volume"};
    const auto RecentTarget = Request.destination_;
    const bool Remember = Invocation.parameters_.contains("recentProvider");
    const auto RecentProvider = Remember ? std::get<std::string>(Invocation.parameters_.at("recentProvider")) : std::string{};
    if (Remember && ((!NativeTarget) || RecentProvider != (Request.destination_.kind_ == LocatorKind::Smb ? "smb" : "local")))
        return Error{ErrorCategory::InvalidRequest, "recent-provider-mismatch"};
    std::vector<Locator> Affected;
    if (!Model->IsSearchResults()) Affected.push_back(Invocation.context_.location_);
    else if (Request.operation_ != hc::operations::FileOperation::Copy)
        for (const auto& Source : Request.sources_) {
            auto Parent = Source.entry_.locator_; Parent.root_ = QFileInfo(QString::fromStdString(Parent.root_)).absolutePath().toStdString(); Affected.push_back(std::move(Parent));
        }
    if (Request.operation_ != hc::operations::FileOperation::Delete)
    {
        auto Destination = Request.destination_;
        if (Destination.kind_ != LocatorKind::Archive && (Request.operation_ == hc::operations::FileOperation::Rename || Request.operation_ == hc::operations::FileOperation::MakeDirectory))
            Destination.root_ = QFileInfo(QString::fromStdString(Destination.root_)).path().toStdString();
        Affected.push_back(std::move(Destination));
    }
    const auto Endpoint = [](CFSModel* ArchiveModel) -> Result<ArchiveMoveEndpoint> {
        if (!ArchiveModel || !ArchiveModel->ProviderEnabled() || ArchiveModel->IsLoading() || !ArchiveModel->SnapshotProvider() || !ArchiveModel->GetRoot())
            return Error{ErrorCategory::Unsupported, "archive-destination-not-open"};
        const auto Pack = dynamic_cast<nsHC::CFSPack*>(ArchiveModel->GetRoot().get());
        if (!Pack || !Pack->GetWCX()) return Error{ErrorCategory::Unsupported, "archive-plugin-required"};
        const auto Policy = Pack->GetWCX()->WritePolicy(); if (!Policy.HasValue()) return Policy.Failure();
        ArchiveMoveEndpoint Value{Pack->ReadLocation("/"), ArchiveModel->SnapshotProvider()};
        const auto Plugin = Pack->GetWCX()->PluginPath().toStdString();
        if (QDir::cleanPath(Pack->GetWCX()->PluginPath()).compare(QDir::cleanPath(hc::plugins::BuiltinIsoOptions().binaryPath_), Qt::CaseInsensitive) == 0) {
            Value.limits_.maxInputBytes_ = 64ull * 1024 * 1024 * 1024;
            Value.limits_.maxArchiveBytes_ = 128ull * 1024 * 1024 * 1024;
        }
        Value.begin_ = [Plugin, Root = Value.root_, Reader = Value.reader_, Limits = Value.limits_, Backend = Pack->GetWCX()->Backend()](OperationContext Context) {
            return BeginWcxMutation(Plugin, Root, Reader, {}, Limits, std::move(Context), {}, Backend);
        };
        return Value;
    };
    const auto DestinationModel = [&]() -> Result<CFSModel*> {
        CFSModel* Target = nullptr;
        if (Invocation.parameters_.contains("destinationTab"))
        {
            Target = findModel(std::get<std::string>(Invocation.parameters_.at("destinationTab")));
            if (!Target || PanelModel(PanelOf(Target)) != Target || Target->IsLoading() || !Target->GetRoot())
                return Error{ErrorCategory::StaleRevision, "archive-destination-tab-changed"};
            const auto Current = Target->GetRoot()->ReadLocation(Target->GetCurrentPathWithRoot());
            if (Invocation.parameters_.contains("destinationLocation"))
            {
                auto Expected = ParseLocator(std::get<std::string>(Invocation.parameters_.at("destinationLocation")));
                if (!Expected.HasValue() || Expected.Value() != Current)
                    return Error{ErrorCategory::StaleRevision, "archive-destination-location-changed"};
            }
            if (!SameArchiveContainer(Current, Request.destination_))
                return Error{ErrorCategory::StaleRevision, "archive-destination-container-changed"};
        }
        else
            for (auto* Candidate : main_->findChildren<CFSModel*>())
                if (Candidate->GetRoot() && Candidate->GetRoot()->ReadLocation(Candidate->GetCurrentPathWithRoot()).kind_ == LocatorKind::Archive
                    && SameArchiveContainer(Candidate->GetRoot()->ReadLocation("/"), Request.destination_))
                { Target = Candidate; break; }
        if (!Target) return Error{ErrorCategory::Unsupported, "archive-destination-not-open"};
        return Target;
    };
    const auto DestinationWriter = [&]() -> Result<std::shared_ptr<IWritableStorage>> {
            if (Request.destination_.kind_ == LocatorKind::Plugin)
            {
                auto* Target = Model;
                if (Invocation.parameters_.contains("destinationTab"))
                    Target = findModel(std::get<std::string>(Invocation.parameters_.at("destinationTab")));
                if (!Target || PanelModel(PanelOf(Target)) != Target || Target->IsLoading() || !Target->ProviderEnabled() || !Target->GetRoot()) return Error{ErrorCategory::StaleRevision, "plugin-destination-tab-changed"};
                const auto Current = Target->GetRoot()->ReadLocation(Target->GetCurrentPathWithRoot());
                if (Invocation.parameters_.contains("destinationLocation") && Serialize(Current) != std::get<std::string>(Invocation.parameters_.at("destinationLocation")))
                    return Error{ErrorCategory::StaleRevision, "plugin-destination-location-changed"};
                if (Current.kind_ != LocatorKind::Plugin || Current.root_.substr(0, Current.root_.find('\\')) != Request.destination_.root_.substr(0, Request.destination_.root_.find('\\')))
                    return Error{ErrorCategory::InvalidRequest, "plugin-destination-session-mismatch"};
                auto Writer = Target->GetRoot()->WriteProvider();
                if (!Writer) return Error{ErrorCategory::Unsupported, "plugin-write-not-approved"};
                return Writer;
            }
            else
            {
                if (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)
                    return Error{ErrorCategory::Unsupported, "native-destination-required"};
                return MakeNativeWritableStorage(Request.destination_.kind_, MakeNativeReadProvider(Request.destination_.kind_));
            }
    };
    Result<JobHandle> Started = Error{ErrorCategory::Unsupported, "file-operation-unavailable"};
    const bool CrossMove = Request.operation_ == hc::operations::FileOperation::Move && (Kind == LocatorKind::Archive || Request.destination_.kind_ == LocatorKind::Archive)
        && !SameArchiveContainer(Request.destination_, Invocation.context_.location_);
    const bool ArchiveMutation = Request.destination_.kind_ == LocatorKind::Archive
        || (Kind == LocatorKind::Archive && Request.operation_ != hc::operations::FileOperation::Copy);
    if ((CrossMove || ArchiveMutation) && (!Request.copyOptions_.metadata_ || Request.copyOptions_.verify_ || Request.copyOptions_.acl_ || Request.copyOptions_.streams_
        || Request.includePattern_ != "*" || Request.bytesPerSecondLimit_)) return Error{ErrorCategory::Unsupported, "archive-copy-options-pending"};
    if (CrossMove)
    {
        MoveOperationRequest Move;
        Move.conflict_ = Request.conflict_;
        Move.createParents_ = Request.createParents_; Move.destinationName_ = Request.destinationName_;
        Move.selection_ = Request.selection_; Move.selectionRoot_ = Request.selectionRoot_;
        Move.sourceReader_ = Reader; Move.sourceWriter_ = Writer; Move.destination_ = Request.destination_; Move.correlationId_ = Request.correlationId_;
        for (const auto& Source : Request.sources_) Move.sources_.push_back(Source.entry_);
        if (Kind == LocatorKind::Archive)
        {
            auto Source = Endpoint(Model); if (!Source.HasValue()) return Source.Failure(); Move.sourceArchive_ = std::move(Source.Value());
        }
        if (Request.destination_.kind_ == LocatorKind::Archive)
        {
            auto Target = DestinationModel(); if (!Target.HasValue()) return Target.Failure();
            auto Destination = Endpoint(Target.Value()); if (!Destination.HasValue()) return Destination.Failure(); Move.destinationArchive_ = std::move(Destination.Value());
        }
        else
        {
            auto Writer = DestinationWriter(); if (!Writer.HasValue()) return Writer.Failure(); Move.destinationWriter_ = Writer.Value();
        }
        Move.maxBytes_ = std::min(Move.sourceArchive_ ? Move.sourceArchive_->limits_.maxInputBytes_ : UINT64_MAX,
            Move.destinationArchive_ ? Move.destinationArchive_->limits_.maxInputBytes_ : UINT64_MAX);
        Started = fileJobs_->Submit(std::move(Move), Invocation.operation_, Flag("queued"));
    }
    else if (ArchiveMutation)
    {
        CFSModel* ArchiveModel = Model;
        if (Request.operation_ == hc::operations::FileOperation::Copy || Invocation.parameters_.contains("destinationTab"))
        {
            auto Target = DestinationModel(); if (!Target.HasValue()) return Target.Failure();
            if (Request.operation_ == hc::operations::FileOperation::Copy) ArchiveModel = Target.Value();
        }
        auto Target = Endpoint(ArchiveModel); if (!Target.HasValue()) return Target.Failure();
        if (Request.operation_ != hc::operations::FileOperation::Delete && !SameArchiveContainer(Request.destination_, Target.Value().root_))
            return Error{ErrorCategory::InvalidRequest, "archive-destination-container-mismatch"};
        ArchiveOperationRequest Archive;
        if (Request.createParents_ || !Request.destinationName_.empty()) return Error{ErrorCategory::Unsupported, "archive-nested-target-policy-pending"};
        Archive.conflict_ = Request.conflict_; Archive.confirmDelete_ = Request.confirmDelete_;
        Archive.selection_ = Request.selection_; Archive.selectionRoot_ = Request.selectionRoot_;
        Archive.operation_ = Request.operation_; Archive.archive_ = Target.Value().root_; Archive.destination_ = Request.destination_;
        Archive.limits_ = Target.Value().limits_;
        Archive.sources_ = std::move(Request.sources_); Archive.reader_ = Target.Value().reader_; Archive.begin_ = std::move(Target.Value().begin_); Archive.correlationId_ = Request.correlationId_;
        Started = fileJobs_->Submit(std::move(Archive), Invocation.operation_, Flag("queued"));
    }
    else
    {
        if (Request.operation_ != hc::operations::FileOperation::Delete)
        {
            auto Writer = DestinationWriter(); if (!Writer.HasValue()) return Writer.Failure(); Request.destinationWriter_ = Writer.Value();
        }
        Started = fileJobs_->Submit(std::move(Request), Invocation.operation_, Flag("queued"));
    }
    if (!Started.HasValue()) return Started.Failure();
    affectedLocations_.emplace(Started.Value().id_, std::move(Affected));
    rememberJobSelection(Started.Value().id_, Invocation.context_);
    if (Flag("openCreated") && Invocation.id_ == "hc.file.mkdir" && NativeTarget)
        createdFolderNavigation_[Started.Value().id_] = {Invocation.context_.tabId_, Invocation.context_.location_, FullTarget, Model};
    else if (Invocation.id_ == "hc.file.mkdir") {
        const auto& Source = Invocation.context_.location_;
        const bool ArchiveTarget = FullTarget.kind_ == LocatorKind::Archive && SameArchiveContainer(FullTarget, Source);
        if ((NativeTarget && FullTarget.kind_ == Source.kind_) || ArchiveTarget) {
            const auto SourcePath = QString::fromStdString(ArchiveTarget ? Source.nesting_.back().entry_ : Source.root_);
            const auto TargetPath = QString::fromStdString(ArchiveTarget ? FullTarget.nesting_.back().entry_ : FullTarget.root_);
            const auto Relative = QDir::fromNativeSeparators(QDir(SourcePath).relativeFilePath(TargetPath));
            // Nested creation focuses the child visible in the original panel.
            if (!QDir::isAbsolutePath(Relative) && Relative != "." && Relative != ".." && !Relative.startsWith("../"))
                createdFileFocus_[Started.Value().id_] = {Invocation.context_.tabId_, Source, Model, Relative.section('/', 0, 0), false};
        }
    }
    if (Remember && !hc::ui::FileOperationDialog::RememberTarget(QString::fromStdString(RecentProvider), QString::fromStdString(RecentTarget.root_)))
        if (auto* Window = qobject_cast<QMainWindow*>(main_.data())) Window->statusBar()->showMessage(tr("작업을 제출했으나 최근 대상 경로를 저장하지 못했습니다."), 6000);
    return CommandOutcome{CommandOutcome::Status::Accepted, Started.Value().id_};
}


void CommandUiBridge::PromptFileTools(bool Rename, const QModelIndex& Index)
{
    const auto Context = captureFiles(Index, false);
    auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->GetRoot() || Model->IsLoading()) return;
    if (!Rename && Model->IsSearchResults()) return;
    const auto Folder = QString::fromStdString(Context.location_.root_);
    const bool Supported = Model->IsSearchResults() || Context.location_.kind_ == LocatorKind::Local || Context.location_.kind_ == LocatorKind::Smb;
    // The location and selection are immutable across the nested dialog loop.
    const auto Refresh = [Guard = QPointer<CommandUiBridge>(this), Folder, Kind = Context.location_.kind_] {
        if (!Guard || !Guard->main_) return;
        for (auto* M : Guard->main_->findChildren<CFSModel*>())
            if (M->GetRoot() && !M->IsLoading() && M->GetRoot()->ReadLocation(M->GetCurrentPathWithRoot()).kind_ == Kind
                && QDir::cleanPath(M->GetCurrentPathWithRoot()).compare(QDir::cleanPath(Folder), Qt::CaseInsensitive) == 0) M->Refresh();
    };
    if (!Rename) {
        auto Reader = Model->SnapshotProvider(); const auto Kind = Context.location_.kind_;
        if (Supported && !Reader) Reader = MakeNativeReadProvider(Kind);
        auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider() : Supported ? MakeNativeWritableStorage(Kind, Reader) : nullptr;
        hc::ui::NewFileDialog Dialog(Folder, bool(Writer), main_);
        if (Writer) Dialog.SetProvider(Writer, Context.location_, [this, Context, ModelGuard = QPointer<CFSModel>(Model)](hc::operations::PreparedJob Job) {
            const auto Name = QFileInfo(QString::fromStdString(Job.request_.metadata_.destination_)).fileName();
            auto Submitted = fileJobs_->Submit(std::move(Job), tr("새 파일 · %1").arg(Name));
            if (Submitted.HasValue()) { affectedLocations_[Submitted.Value().id_] = {Context.location_}; createdFileFocus_[Submitted.Value().id_] = {Context.tabId_, Context.location_, ModelGuard, Name, false}; }
            return Submitted;
        }, [this](const std::string& Id) { fileJobs_->Scheduler().Cancel(Id); });
        if (Dialog.exec() != QDialog::Accepted) return;
        Refresh();
        if (Dialog.OpenEditor()) {
            // Never shell-open a new executable/script template. The explicit text
            // editor receives the successfully created filename as one argument.
            if (!QProcess::startDetached(Dialog.EditorProgram(), Dialog.EditorArguments(), QFileInfo(Dialog.CreatedPath()).absolutePath()))
                hc::ui::MessageDialog::warning(main_, tr("편집기 열기 실패"), tr("파일은 생성되었으나 텍스트 편집기를 시작하지 못했습니다."));
        }
        return;
    }
    QStringList Paths;
    for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto E = Model->GetFileInfo(Model->index(Row, 0)); if (!E || E->Name_ == "..") continue;
        const auto Id = E->storageEntry_ ? E->storageEntry_->id_ : ResourceId{{"legacy"}, E->Name_.toStdString(), IdentityScope::NavigationSession, {}};
        if (std::find(Context.selection_.begin(), Context.selection_.end(), Id) != Context.selection_.end()) Paths.append(Model->GetFileFullPath(Model->index(Row, 0)));
    }
    hc::ui::BatchRenameDialog Dialog(Paths, Supported, main_, Context.selectionManifest_, fileJobs_); Dialog.onChanged_ = Refresh; Dialog.exec();
}

void CommandUiBridge::PromptFileAttributes(const QModelIndex& Index)
{
    const auto Context = captureFiles(Index, false);
    auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->GetRoot() || Model->IsLoading()) return;
    if (!Model->IsSearchResults() && Context.location_.kind_ != LocatorKind::Local && Context.location_.kind_ != LocatorKind::Smb) {
        hc::ui::MessageDialog::information(main_, tr("속성 변경"), tr("이 저장소에서는 속성 변경을 지원하지 않습니다.")); return;
    }
    std::vector<EntrySnapshot> Entries;
    if (!Context.selectionManifest_) for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
        if (!Entry || !Entry->storageEntry_ || Entry->Name_ == "..") continue;
        if (std::find(Context.selection_.begin(), Context.selection_.end(), Entry->storageEntry_->id_) != Context.selection_.end()) Entries.push_back(*Entry->storageEntry_);
    }
    if (Entries.empty() && !Context.selectionManifest_) { hc::ui::MessageDialog::information(main_, tr("속성 변경"), tr("변경할 파일이나 폴더를 선택하세요.")); return; }
    auto Reader = Model->SnapshotProvider(); if (!Reader) Reader = MakeNativeReadProvider(Context.location_.kind_);
    if (Model->IsSearchResults() && !Entries.empty()) {
        Reader = Model->ProviderFor(Entries.front());
        for (const auto& Entry : Entries) if (Model->ProviderFor(Entry) != Reader) {
            hc::ui::MessageDialog::information(main_, tr("속성 변경"), tr("로컬 파일과 네트워크 파일의 속성은 각각 선택하여 변경하세요.")); return;
        }
    }
    const QPointer<CmpPanel> Panel = PanelOf(Model);
    hc::ui::FileAttributesDialog Dialog(std::move(Entries), Reader, Context.selectionManifest_, main_);
    Dialog.exec();
    if (Dialog.Changed()) {
        const auto Folder = hc::attributes::Key(Context.location_);
        for (auto* M : main_->findChildren<CFSModel*>()) if (M->GetRoot() && !M->IsLoading()) {
            const auto Location = M->WorkspaceLocation(); const auto Path = hc::attributes::Key(Location);
            if (Location.kind_ == Context.location_.kind_ && (Path == Folder || Path.startsWith(Folder + '/'))) M->Refresh();
        }
    }
    if (Panel) if (auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) View->EnsureKeyboardFocusOnView();
}
