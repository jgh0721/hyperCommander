#include "stdafx.h"
#include "commandUiBridge.hpp"
#include "UIs/cmpPanel.hpp"
#include "builtInFsModel.hpp"
#include "fileJobUi.hpp"
#include "nativeWritableStorage.hpp"
#include "storageProviders.hpp"
#include "qtSearch.hpp"
#include "qtWorkspaceRestore.hpp"
#include "wcxMutation.hpp"
#include "qtViewerHost.hpp"
#include "qtPluginHostSettings.hpp"
#include "nativeMaterializer.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include <QtConcurrent>
#include "commonLibs/storage/materializedContent.hpp"
#include "BuiltInPlugin/IsoImage/isoJob.hpp"
#include "BuiltInPlugin/IsoImage/builtinIso.hpp"
#include <QFileDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QtitanGrid.h>

using namespace hc::commands;
using namespace hc::storage;

namespace
{
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
    const std::vector<CommandDescriptor> Commands{
        {"hc.navigate.path", 1, {{"path", {ParameterType::String}}}},
        {"hc.navigate.parent"}, {"hc.navigate.refresh"}, {"hc.navigate.cancel"}, {"hc.navigate.open"},
        {"hc.navigate.provider", 1, {{"enabled", {ParameterType::Boolean}}}},
        {"hc.tab.new"}, {"hc.tab.close"}, {"hc.tab.next"}, {"hc.tab.previous"}, {"hc.view.columns"},
        {"hc.viewer.open"}, {"hc.viewer.quick"}, {"hc.viewer.preview"}, {"hc.viewer.close"},
        {"hc.file.copy", 1, {{"destination", {ParameterType::String}}, {"destinationTab", {ParameterType::String, false}}, {"destinationLocation", {ParameterType::String, false}}}, "file-write"},
        {"hc.file.move", 1, {{"destination", {ParameterType::String}}, {"destinationTab", {ParameterType::String, false}}, {"destinationLocation", {ParameterType::String, false}}}, "file-write"},
        {"hc.file.rename", 1, {{"destination", {ParameterType::String}}}, "file-write"},
        {"hc.file.mkdir", 1, {{"destination", {ParameterType::String}}}, "file-write"},
        {"hc.file.delete", 1, {}, "file-write"}
    };
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
            Window->statusBar()->showMessage(QObject::tr("명령을 실행할 수 없습니다: %1").arg(QString::fromStdString(std::get<std::string>(Event.payload_.at("code")))), 6000);
    });
    errorSubscription_ = std::move(Subscription.Value());
    fileJobs_ = new FileJobUi(Main, events_);
    viewers_ = new QtViewerHost(Main, [Base = QDir::tempPath().toStdString()] { return MakeNativeMaterializer(Base); },
        [] { return QPluginHostSettings::ViewerChoices(*StSettings); },
        [] { return QPluginHostSettings::ViewerSelection(*StSettings); });
    auto* PreviewTimer = new QTimer(this); PreviewTimer->setInterval(120);
    connect(PreviewTimer, &QTimer::timeout, this, &CommandUiBridge::refreshPreview); PreviewTimer->start();
    fileJobs_->onFinished_ = [Guard = QPointer<CommandUiBridge>(this)](const hc::operations::JobResult& Result) {
        if (!Guard || !Guard->main_) return;
        const auto It = Guard->affectedLocations_.find(Result.snapshot_.id_);
        if (It == Guard->affectedLocations_.end()) return;
        for (auto* Model : Guard->main_->findChildren<CFSModel*>())
            if (Model->GetRoot() && Model->ProviderEnabled() && !Model->IsLoading())
                for (const auto& At : It->second)
                {
                    const auto Current = Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot());
                    if (Current.kind_ == At.kind_ && QDir::cleanPath(QString::fromStdString(At.root_)).compare(
                        QDir::cleanPath(QString::fromStdString(Current.root_)), Qt::CaseInsensitive) == 0)
                    { Model->Refresh(); break; }
                }
        Guard->affectedLocations_.erase(It);
    };
    bindModels();
    createActions();
}

void CommandUiBridge::bindModels()
{
    if (!main_) return;
    for (auto* Model : main_->findChildren<CFSModel*>())
    {
        if (Model->property("hc.tabId").isNull()) Model->setProperty("hc.tabId", QUuid::createUuid().toString(QUuid::Id128));
        if (Model->property("hc.commandSignals").toBool()) continue;
        Model->setProperty("hc.commandSignals", true);
        const auto Publish = [this, Model](bool Success, const QString& Code) {
            EventEnvelope Event;
            Event.id_ = Success ? "navigation.completed" : "navigation.failed";
            Event.producer_ = "qt-navigation";
            Event.correlationId_ = Model->property("hc.correlation").toString().toStdString();
            Model->setProperty("hc.correlation", QVariant{});
            Event.causationId_ = Event.correlationId_;
            Event.payload_ = {{"tab", Model->property("hc.tabId").toString().toStdString()},
                {"path", Model->GetCurrentPathWithRoot().toStdString()}, {"code", Code.toStdString()}};
            events_->Publish(std::move(Event));
            if (!Success)
                if (auto* Window = qobject_cast<QMainWindow*>(main_.data()))
                    Window->statusBar()->showMessage(tr("폴더를 열 수 없습니다: %1").arg(Code), 6000);
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
    Invocation.context_ = Id.startsWith("hc.viewer.") ? captureFiles(Index, true)
        : Id.startsWith("hc.file.") ? captureFiles(Index, Id == "hc.file.rename") : Capture(Index);
    Invocation.permissions_.insert("navigation");
    if (Id.startsWith("hc.file.")) Invocation.permissions_.insert("file-write");
    Invocation.source_ = Source.toStdString();
    Invocation.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    for (auto It = Parameters.begin(); It != Parameters.end(); ++It)
    {
        if (It.value().metaType().id() == QMetaType::Bool) Invocation.parameters_[It.key().toStdString()] = It.value().toBool();
        else if (It.value().metaType().id() == QMetaType::QString) Invocation.parameters_[It.key().toStdString()] = It.value().toString().toStdString();
        else return Ready<CommandOutcome>(Error{ErrorCategory::InvalidRequest, "unsupported-ui-parameter"});
    }
    return DispatchInvocation(std::move(Invocation));
}

Task<CommandOutcome> CommandUiBridge::DispatchInvocation(CommandInvocation Invocation) { return dispatcher_->Dispatch(std::move(Invocation)); }

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
    if (!Model->GetRoot() && (Invocation.id_.starts_with("hc.file.") || Invocation.id_.starts_with("hc.viewer.")
        || Invocation.id_ == "hc.navigate.parent" || Invocation.id_ == "hc.navigate.open"))
        return Error{ErrorCategory::Unavailable, "tab-not-ready"};
    auto* Panel = PanelOf(Model);
    if (!Panel || PanelModel(Panel) != Model) return Error{ErrorCategory::Cancelled, "tab-not-active"};
    const auto& Id = Invocation.id_;
    QString Path;
    if (Id == "hc.navigate.path")
    {
        Path = QString::fromStdString(std::get<std::string>(Invocation.parameters_.at("path")));
        if (Path.size() == 2 && Path.endsWith(':')) Path += '\\';
        if (!QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "absolute-path-required"};
        Path = QDir::toNativeSeparators(QDir::cleanPath(Path));
    }
    if (Id != "hc.navigate.path" && !(Invocation.context_.location_ == Model->WorkspaceLocation()))
        return Error{ErrorCategory::StaleRevision, "navigation-context-changed"};
    if (Id.starts_with("hc.file.")) return executeFile(Invocation, Model);
    if (Id.starts_with("hc.viewer.")) return executeViewer(Invocation, Model);
    if ((Id.starts_with("hc.navigate.") && Id != "hc.navigate.cancel") || Id == "hc.tab.close")
        if (Model->IsLoading()) Model->CancelNavigation();
    if (Id.starts_with("hc.navigate.") && Id != "hc.navigate.cancel")
        Model->setProperty("hc.correlation", QString::fromStdString(Invocation.correlationId_));
    if (Id == "hc.navigate.path")
    {
        Panel->ChangeCurrentPath(Path, {});
        if (!Model->IsLoading() && Model->GetCurrentPathWithRoot().compare(Path, Qt::CaseInsensitive) != 0)
            return Error{ErrorCategory::NotFound, "path-not-available"};
    }
    else if (Id == "hc.navigate.refresh") Model->Refresh();
    else if (Id == "hc.navigate.parent") Model->ChangeDirectory("..");
    else if (Id == "hc.navigate.cancel") Model->CancelNavigation();
    else if (Id == "hc.navigate.provider")
    {
        if (Invocation.context_.location_.kind_ == LocatorKind::Plugin && !std::get<bool>(Invocation.parameters_.at("enabled")))
            return Error{ErrorCategory::Unsupported, "plugin-requires-provider"};
        Model->SetProviderEnabled(std::get<bool>(Invocation.parameters_.at("enabled")));
    }
    else if (Id == "hc.navigate.open")
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
                if (!(Entry->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) return Error{ErrorCategory::Unsupported, "directory-required"};
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
        if (const auto View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) View->ConfigureColumns();
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
    auto* Menu = Window->menuBar()->addMenu(tr("탐색"));
    auto* Toolbar = Window->addToolBar(tr("탐색"));
    Toolbar->setObjectName("hc.navigationToolbar");
    const std::vector<std::pair<QString, QString>> Actions{
        {"hc.navigate.path", tr("경로 이동")}, {"hc.navigate.parent", tr("상위 폴더")},
        {"hc.navigate.refresh", tr("새로 고침")}, {"hc.navigate.cancel", tr("중지")}, {"hc.view.columns", tr("컬럼 너비")}
    };
    for (const auto& [Id, Label] : Actions)
    {
        for (const auto& Source : {QStringLiteral("menu"), QStringLiteral("toolbar")})
        {
            auto* Action = new QAction(Label, this);
            Action->setObjectName("hc." + Source + "." + Id);
            if (Source == "menu") Menu->addAction(Action); else Toolbar->addAction(Action);
            connect(Action, &QAction::triggered, this, [this, Id, Source] {
                QVariantMap Parameters;
                if (Id == "hc.navigate.path")
                {
                    bool Accepted = false;
                    const auto Context = Capture();
                    const auto Path = QInputDialog::getText(main_, tr("폴더 이동"), tr("경로"), QLineEdit::Normal,
                        QString::fromStdString(Context.location_.root_), &Accepted);
                    if (!Accepted) return;
                    Parameters["path"] = Path;
                }
                Dispatch(Id, Parameters, Source);
            });
        }
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
    auto* Files = Window->menuBar()->addMenu(tr("파일 작업"));
    auto* Viewers = Window->menuBar()->addMenu(tr("뷰어"));
    for (const auto& Item : std::vector<std::pair<QString, QString>>{
        {"hc.viewer.open", tr("내장 뷰어로 열기")}, {"hc.viewer.quick", tr("반대 패널 미리 보기")}, {"hc.viewer.close", tr("미리 보기 닫기")}})
    {
        auto* Action = Viewers->addAction(Item.second); Action->setObjectName("hc.menu." + Item.first);
        connect(Action, &QAction::triggered, this, [this, Id = Item.first] { Dispatch(Id, {}, "menu"); });
    }
    const std::vector<std::pair<QString, QString>> FileActions{
        {"hc.file.copy", tr("복사")}, {"hc.file.move", tr("이동")}, {"hc.file.rename", tr("이름 변경")},
        {"hc.file.mkdir", tr("새 폴더")}, {"hc.file.delete", tr("영구 삭제")}
    };
    for (const auto& [Id, Label] : FileActions)
    {
        auto* Action = Files->addAction(Label);
        Action->setObjectName("hc.menu." + Id);
        connect(Action, &QAction::triggered, this, [this, Id] { PromptFileOperation(Id, "menu"); });
    }
    auto* Jobs = Files->addAction(tr("작업 목록"));
    connect(Jobs, &QAction::triggered, this, [this] { fileJobs_->Show(); });
    Files->addSeparator();
    auto* CreateIso = Files->addAction(tr("선택 항목으로 ISO 생성…")); CreateIso->setObjectName("hc.menu.iso.create");
    auto* InspectIso = Files->addAction(tr("ISO 구조 확인…")); InspectIso->setObjectName("hc.menu.iso.inspect");
    connect(CreateIso, &QAction::triggered, this, [this] { openIso(false); });
    connect(InspectIso, &QAction::triggered, this, [this] { openIso(true); });
    auto* Plugins = Window->menuBar()->addMenu(tr("플러그인"));
    auto* Storage = Plugins->addAction(tr("파일 시스템 열기…")); Storage->setObjectName("hc.menu.wfx.open");
    auto* Metadata = Plugins->addAction(tr("선택 파일 정보…")); Metadata->setObjectName("hc.menu.wdx.query");
    auto* Search = Menu->addAction(tr("범위 검색…")); Search->setObjectName("hc.menu.search");
    connect(Search, &QAction::triggered, this, [this] { openSearch(); });
    auto* DopusStorage = Plugins->addAction(tr("DOpus 파일 시스템 열기…")); DopusStorage->setObjectName("hc.menu.dopus.open");
    auto* DopusMetadata = Plugins->addAction(tr("DOpus 선택 파일 정보…")); DopusMetadata->setObjectName("hc.menu.dopus.query");
    connect(DopusStorage, &QAction::triggered, this, [this] { openDataPlugin(false, true); });
    connect(DopusMetadata, &QAction::triggered, this, [this] { openDataPlugin(true, true); });
    connect(Storage, &QAction::triggered, this, [this] { openDataPlugin(false); });
    connect(Metadata, &QAction::triggered, this, [this] { openDataPlugin(true); });
}

void CommandUiBridge::openIso(bool Inspect)
{
    const auto Context = captureFiles({}, false);
    auto* Model = findModel(Context.tabId_);
    if (!Model || !Model->ProviderEnabled() || Model->IsLoading()) return;
    auto Reader = Model->SnapshotProvider();
    std::vector<hc::operations::OperationSource> Sources;
    for (int Row = 0; Row < Model->rowCount(); ++Row) {
        const auto Item = Model->GetFileInfo(Model->index(Row, 0));
        if (Item && Item->storageEntry_ && std::find(Context.selection_.begin(), Context.selection_.end(), Item->storageEntry_->id_) != Context.selection_.end())
            Sources.push_back({*Item->storageEntry_, Reader, {}});
    }
    const auto CurrentDirectory = QString::fromStdString(Context.location_.root_);
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
        connect(ReadAll, &QPushButton::clicked, Dialog, [Run] { Run(true); }); Dialog->show(); Run(false); return;
    }
    if (Sources.empty()) { QMessageBox::information(main_, tr("ISO 생성"), tr("이미지에 넣을 파일이나 폴더를 먼저 선택하십시오.")); return; }
    QDialog Dialog(main_); Dialog.setObjectName("hc.iso.create"); Dialog.setWindowTitle(tr("선택 항목으로 ISO 생성")); Dialog.resize(650, 300);
    auto* Layout = new QVBoxLayout(&Dialog); auto* Form = new QFormLayout; Layout->addLayout(Form);
    auto* Selection = new QLabel(tr("선택한 %1개 항목과 하위 파일을 이미지에 넣습니다.").arg(Sources.size()), &Dialog); Selection->setWordWrap(true); Form->addRow(Selection);
    auto* OutputRow = new QWidget(&Dialog); auto* OutputLayout = new QHBoxLayout(OutputRow); OutputLayout->setContentsMargins(0, 0, 0, 0);
    auto* Output = new QLineEdit(QDir(CurrentDirectory).filePath("image.iso"), OutputRow); Output->setObjectName("hc.iso.create.path"); Output->setAccessibleName(tr("저장할 ISO 경로"));
    auto* Browse = new QPushButton(tr("찾아보기…"), OutputRow); OutputLayout->addWidget(Output, 1); OutputLayout->addWidget(Browse); Form->addRow(tr("저장 위치(&P)"), OutputRow);
    auto* Profile = new QComboBox(&Dialog); Profile->setObjectName("hc.iso.create.profile"); Profile->addItems({tr("ISO 9660"), tr("ISO 9660 + Joliet"), tr("ISO 9660 + Joliet + UDF 1.02"), tr("ISO 9660 + UDF 1.02 (긴 파일 이름)")}); Profile->setCurrentIndex(2); Form->addRow(tr("파일 시스템(&F)"), Profile);
    auto* Label = new QLineEdit("HYPERCOMMANDER", &Dialog); Label->setObjectName("hc.iso.create.label"); Label->setMaxLength(16); Form->addRow(tr("볼륨 이름(&V)"), Label);
    auto* Help = new QLabel(&Dialog); Help->setWordWrap(true); Layout->addWidget(Help);
    const auto UpdateHelp = [Profile, Label, Help] { Label->setMaxLength(Profile->currentIndex() == 0 ? 32 : Profile->currentIndex() == 3 ? 30 : 16); Help->setText(Profile->currentIndex() == 0
        ? QObject::tr("파일 이름을 ISO용 영문 대문자·숫자·밑줄로 변환합니다. 볼륨 이름은 ASCII 문자로 입력하십시오.")
        : Profile->currentIndex() == 3 ? QObject::tr("긴 파일 이름을 UDF에 보존합니다. 이름 한 부분은 UDF 인코딩 255바이트까지 지원하며, 볼륨 이름은 ASCII 30자까지 입력할 수 있습니다.")
        : QObject::tr("한글 파일 이름을 보존합니다. 이름 한 부분은 64 UTF-16 단위까지 지원하며, 볼륨 이름은 ASCII 16자까지 입력할 수 있습니다.")); };
    connect(Profile, &QComboBox::currentIndexChanged, &Dialog, UpdateHelp); UpdateHelp();
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog); Buttons->button(QDialogButtonBox::Ok)->setText(tr("생성")); Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소")); Layout->addWidget(Buttons);
    connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept); connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    connect(Browse, &QPushButton::clicked, &Dialog, [this, Output] { const auto Path = QFileDialog::getSaveFileName(main_, tr("ISO 저장 위치"), Output->text(), tr("ISO 이미지 (*.iso)"), nullptr, QFileDialog::DontConfirmOverwrite); if (!Path.isEmpty()) Output->setText(Path); });
    Output->setFocus(); Output->selectAll(); if (Dialog.exec() != QDialog::Accepted) return;
    auto Path = QDir::fromNativeSeparators(Output->text()); if (!Path.endsWith(".iso", Qt::CaseInsensitive)) Path += ".iso";
    hc::iso::CreateRequest Request; Request.sources_ = std::move(Sources); Request.destination_ = {Path.startsWith("//") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
    Request.profile_ = unsigned(Profile->currentIndex()); Request.volumeLabel_ = Label->text(); Request.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    const auto Destination = Request.destination_; auto Prepared = hc::iso::PrepareCreate(std::move(Request));
    if (!Prepared.HasValue()) { QMessageBox::warning(main_, tr("ISO 생성"), tr("설정을 확인하십시오: %1").arg(QString::fromStdString(Prepared.Failure().code_))); return; }
    auto Submitted = fileJobs_->Submit(std::move(Prepared.Value()), tr("ISO 생성 · %1").arg(QFileInfo(Path).fileName()));
    if (!Submitted.HasValue()) { QMessageBox::warning(main_, tr("ISO 생성"), QString::fromStdString(Submitted.Failure().code_)); return; }
    auto Parent = Destination; Parent.root_ = QFileInfo(Path).absolutePath().toStdString(); affectedLocations_[Submitted.Value().id_] = {Parent};
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
    const auto Choices = QPluginHostSettings::DataChoices(*StSettings, Dopus ? (Metadata ? PluginFormat::DopusViewer : PluginFormat::DopusVfs) : (Metadata ? PluginFormat::Wdx : PluginFormat::Wfx));
    if (Choices.empty()) { QMessageBox::information(main_, tr("플러그인"), tr("설정의 플러그인 실행 설정에서 플러그인을 추가하고 승인해 주세요.")); return; }
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
    bool Accepted = false; const auto Name = QInputDialog::getItem(main_, tr("플러그인 선택"), tr("플러그인"), Names, 0, false, &Accepted);
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
        NativeRoot = QInputDialog::getText(main_, tr("DOpus 파일 시스템"), tr("열 파일 또는 폴더의 전체 경로"), QLineEdit::Normal, NativeRoot, &Accepted);
        if (!Accepted || NativeRoot.isEmpty() || !Panel) return;
    }
    const auto Cache = QDir::tempPath().toStdString();
    const auto Session = QUuid::createUuid().toString(QUuid::Id128).toStdString();
    auto* Dialog = new QDialog(main_); Dialog->setObjectName("hc.dataPluginDialog"); Dialog->setAttribute(Qt::WA_DeleteOnClose); Dialog->setWindowTitle(Name); Dialog->resize(600, 380);
    auto* Layout = new QVBoxLayout(Dialog); auto* Status = new QLabel(tr("플러그인 연결 중…"), Dialog); Layout->addWidget(Status);
    auto* Table = new QTableWidget(0, 2, Dialog); Table->setHorizontalHeaderLabels({tr("항목"), tr("값")}); Table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); Layout->addWidget(Table); Table->setVisible(Metadata);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, Dialog); Layout->addWidget(Buttons); connect(Buttons, &QDialogButtonBox::rejected, Dialog, &QDialog::close);
    auto Stop = std::make_shared<std::stop_source>(); connect(Dialog, &QObject::destroyed, this, [Stop] { Stop->request_stop(); });
    struct OpenResult { HostedStorage storage_; std::vector<std::pair<QString, QString>> values_; std::shared_ptr<IMetadataProvider> metadata_; std::vector<int> fields_; QMap<QString, QString> searchable_; };
    auto* Watcher = new QFutureWatcher<Result<OpenResult>>(Dialog);
    connect(Watcher, &QFutureWatcher<Result<OpenResult>>::finished, Dialog, [this, Watcher, Dialog, Status, Table, Panel, Metadata, Source, Stop, Model, MetadataGeneration, Options, NativeRoot] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (!Result.HasValue()) { Status->setText(tr("연결 실패: %1").arg(QString::fromStdString(Result.Failure().code_))); return; }
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
    const auto Index = Focused.isValid() ? SourceIndex(Focused.modelIndex(0)) : QModelIndex{};
    const auto Entry = Index.isValid() ? Model->GetFileInfo(Index) : nsHC::TySpFileSource{};
    if (!Entry || !Entry->storageEntry_ || Entry->storageEntry_->directory_)
    { quick_->CloseView(); quickKey_.clear(); return; }
    const auto Key = Model->property("hc.tabId").toString() + QString::fromStdString(Serialize(Entry->storageEntry_->id_) + Serialize(Entry->storageEntry_->locator_));
    if (quickKey_ == Key) return;
    quickKey_ = Key;
    Dispatch("hc.viewer.preview", {}, "preview", Index);
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

CommandContext CommandUiBridge::captureFiles(const QModelIndex& Index, bool Single)
{
    auto Context = Capture(Index);
    auto* Model = findModel(Context.tabId_);
    if (!Model) return Context;
    const auto Panel = PanelOf(Model);
    const auto View = Panel ? qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) : nullptr;
    if (!View || View->BaseModel() != Model) return Context;
    const auto Grid = View->BaseView();
    QList<QModelIndex> Selected;
    if (!Single)
        for (const auto Row : Grid->selection()->selectedRowIndexes()) Selected.push_back(SourceIndex(Row));
    if (Selected.isEmpty())
    {
        const auto Focused = Grid->focusedRow();
        const auto Cursor = Index.isValid() ? SourceIndex(Index) : Focused.isValid() ? SourceIndex(Focused.modelIndex(0)) : QModelIndex{};
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
    if (Id != "hc.file.delete")
    {
        const bool NameOnly = Id == "hc.file.rename" || Id == "hc.file.mkdir";
        auto Destination = Invocation.context_.location_;
        std::string DestinationTab;
        if (!NameOnly)
            for (auto* Other : main_->findChildren<CmpPanel*>())
                if (Other != PanelOf(Model))
                    if (auto* Target = PanelModel(Other); Target && Target->GetRoot())
                    {
                        Destination = Target->GetRoot()->ReadLocation(Target->GetCurrentPathWithRoot());
                        DestinationTab = Target->property("hc.tabId").toString().toStdString();
                    }
        const bool Archive = Destination.kind_ == LocatorKind::Archive;
        auto Default = Archive ? QString::fromStdString(Destination.nesting_.back().entry_) : QString::fromStdString(Destination.root_);
        if (NameOnly) Default = Id == "hc.file.mkdir" ? tr("새 폴더") : QString{};
        auto Prompt = NameOnly ? tr("새 이름") : Archive ? tr("목적지 폴더 (%1 내부)").arg(QFileInfo(QString::fromStdString(Destination.root_)).fileName()) : tr("목적지 폴더");
        bool Accepted = false;
        const auto Input = QInputDialog::getText(main_, Id == "hc.file.move" ? tr("이동") : tr("파일 작업"), Prompt, QLineEdit::Normal, Default, &Accepted);
        if (!Accepted || Input.isEmpty()) return;
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
        else Invocation.parameters_["destination"] = NameOnly ? QDir(QString::fromStdString(Destination.root_)).filePath(Input).toStdString() : Input.toStdString();
    }
    DispatchInvocation(std::move(Invocation));
}

Result<CommandOutcome> CommandUiBridge::executeFile(const CommandInvocation& Invocation, CFSModel* Model)
{
    using namespace hc::operations;
    if (Model->IsLoading()) return Error{ErrorCategory::InvalidRequest, "navigation-in-progress"};
    FileOperationRequest Request;
    Request.operation_ = Invocation.id_ == "hc.file.copy" ? hc::operations::FileOperation::Copy : Invocation.id_ == "hc.file.move" ? hc::operations::FileOperation::Move
        : Invocation.id_ == "hc.file.rename" ? hc::operations::FileOperation::Rename : Invocation.id_ == "hc.file.mkdir" ? hc::operations::FileOperation::MakeDirectory : hc::operations::FileOperation::Delete;
    Request.correlationId_ = Invocation.correlationId_;
    const auto Kind = Invocation.context_.location_.kind_;
    if (Kind != LocatorKind::Local && Kind != LocatorKind::Smb && Kind != LocatorKind::Archive && Kind != LocatorKind::Plugin)
        return Error{ErrorCategory::Unsupported, "file-operation-provider-required"};
    auto Reader = Model->SnapshotProvider();
    if (!Model->ProviderEnabled() || !Reader) return Error{ErrorCategory::Unsupported, "provider-snapshot-required"};
    auto Writer = Kind == LocatorKind::Plugin ? Model->GetRoot()->WriteProvider() : Kind == LocatorKind::Archive ? nullptr : MakeNativeWritableStorage(Kind, Reader);
    if (Request.operation_ != hc::operations::FileOperation::MakeDirectory)
        for (const auto& Selected : Invocation.context_.selection_)
        {
            bool Found = false;
            for (int Row = 0; Row < Model->rowCount(); ++Row)
            {
                const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
                if (Entry && Entry->storageEntry_ && Entry->storageEntry_->id_ == Selected)
                { Request.sources_.push_back({*Entry->storageEntry_, Reader, Writer}); Found = true; break; }
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
    std::vector<Locator> Affected{Invocation.context_.location_};
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
            if (Current.kind_ != LocatorKind::Archive || Current.root_ != Request.destination_.root_)
                return Error{ErrorCategory::StaleRevision, "archive-destination-container-changed"};
        }
        else
            for (auto* Candidate : main_->findChildren<CFSModel*>())
                if (Candidate->GetRoot() && Candidate->GetRoot()->ReadLocation(Candidate->GetCurrentPathWithRoot()).kind_ == LocatorKind::Archive
                    && Candidate->GetRoot()->GetRoot().toStdString() == Request.destination_.root_)
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
        && !(Kind == LocatorKind::Archive && Request.destination_.kind_ == LocatorKind::Archive && Request.destination_.root_ == Invocation.context_.location_.root_);
    const bool ArchiveMutation = Request.destination_.kind_ == LocatorKind::Archive
        || (Kind == LocatorKind::Archive && Request.operation_ != hc::operations::FileOperation::Copy);
    if (CrossMove)
    {
        MoveOperationRequest Move;
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
        Move.maxBytes_ = std::min(Move.sourceArchive_ ? Move.sourceArchive_->limits_.maxInputBytes_ : 64ull * 1024 * 1024 * 1024,
            Move.destinationArchive_ ? Move.destinationArchive_->limits_.maxInputBytes_ : 64ull * 1024 * 1024 * 1024);
        Started = fileJobs_->Submit(std::move(Move), Invocation.operation_);
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
        if (Request.operation_ != hc::operations::FileOperation::Delete && Request.destination_.root_ != Target.Value().root_.root_)
            return Error{ErrorCategory::InvalidRequest, "archive-destination-container-mismatch"};
        ArchiveOperationRequest Archive;
        Archive.operation_ = Request.operation_; Archive.archive_ = Target.Value().root_; Archive.destination_ = Request.destination_;
        Archive.limits_ = Target.Value().limits_;
        Archive.sources_ = std::move(Request.sources_); Archive.reader_ = Target.Value().reader_; Archive.begin_ = std::move(Target.Value().begin_); Archive.correlationId_ = Request.correlationId_;
        Started = fileJobs_->Submit(std::move(Archive), Invocation.operation_);
    }
    else
    {
        if (Request.operation_ != hc::operations::FileOperation::Delete)
        {
            auto Writer = DestinationWriter(); if (!Writer.HasValue()) return Writer.Failure(); Request.destinationWriter_ = Writer.Value();
        }
        Started = fileJobs_->Submit(std::move(Request), Invocation.operation_);
    }
    if (!Started.HasValue()) return Started.Failure();
    affectedLocations_.emplace(Started.Value().id_, std::move(Affected));
    return CommandOutcome{CommandOutcome::Status::Accepted, Started.Value().id_};
}
