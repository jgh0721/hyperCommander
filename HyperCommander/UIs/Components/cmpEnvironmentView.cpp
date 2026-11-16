#include "stdafx.h"
#include "cmpEnvironmentView.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/registry/registryCatalog.hpp"
#include <QLocale>
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/system/environmentStorage.hpp"
#include "UniqueLibs/system/environmentPathProbe.hpp"
#include "UniqueLibs/system/environmentApplication.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/environmentSensitivity.hpp"
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/FindBox.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/BreadcrumbBar.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/FlowLayout.h>
#include <fmwidgets/Menu.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QScrollBar>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentView, "hc.ui.environment")
using namespace hc::environment;
namespace {
const QStringList ScopeNames{QObject::tr("사용자"), QObject::tr("시스템"), QObject::tr("적용 결과"),
    QObject::tr("HyperCommander"), QObject::tr("프로세스 비교"), QObject::tr("프로필"), QObject::tr("변경 기록")};
class EnvironmentFilter final : public QSortFilterProxyModel
{
public:
    explicit EnvironmentFilter(QObject* Parent) : QSortFilterProxyModel(Parent) { setRecursiveFilteringEnabled(true); setAutoAcceptChildRows(true); setFilterCaseSensitivity(Qt::CaseInsensitive); setFilterKeyColumn(-1); }
    void SetProblems(bool Only) { beginFilterChange(); onlyProblems_ = Only; endFilterChange(Direction::Rows); }
protected:
    bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override {
        if (onlyProblems_ && !sourceModel()->index(Row, 0, Parent).data(EnvironmentModel::ProblemRole).toBool()) return false;
        return QSortFilterProxyModel::filterAcceptsRow(Row, Parent);
    }
private:
    bool onlyProblems_ = false;
};
}
CEnvironmentView::CEnvironmentView(QWidget* Parent, EnvironmentSession* Session) : CViewT(Parent), session_(Session)
{
    viewMode_ = VM_SYSTEM; setObjectName("hc.environment.view");
    if (!Session && EnvironmentApplication::Shared()->Native()) pathProbe_ = std::make_shared<NativePathProbe>();
    if (!session_) session_ = EnvironmentApplication::Shared()->Session();
    model_ = new EnvironmentModel(this); filterProxy_ = new EnvironmentFilter(this); filterProxy_->setSourceModel(model_);
    scopeModel_ = new QStandardItemModel(this);
    for (int I = 0; I < ScopeNames.size(); ++I) { auto* Item = new QStandardItem(ScopeNames[I]); Item->setEditable(false); scopeModel_->appendRow(Item); }
    historyModel_ = new hc::registry::RegistryHistoryModel(this);
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 4, 0, 0); Layout->setSpacing(4);
    auto* Navigation = new QHBoxLayout; Layout->addLayout(Navigation);
    const auto AddButton = [this, Navigation](const QString& Text, auto Callback) {
        auto* Button = new fm::ui::Button(Text, this); Button->setCompact(true); connect(Button, &QPushButton::clicked, this, Callback); Navigation->addWidget(Button); return Button;
    };
    AddButton(QStringLiteral("←"), [this] { Back(); }); AddButton(QStringLiteral("→"), [this] { Forward(); });
    AddButton(tr("범위 트리"), [this] { ToggleTree(); });
    address_ = new fm::ui::BreadcrumbBar(this); address_->setObjectName("hc.environment.address"); Navigation->addWidget(address_, 1);
    auto* Bar = new fm::ui::FlowLayout(4, 4); Layout->addLayout(Bar);
    modes_ = new fm::ui::SegmentedControl({tr("원문"), tr("확장값")}, this); modes_->setSegmentSize(fm::ui::SegmentedControl::Compact); modes_->setCurrentIndex(0); Bar->addWidget(modes_);
    connect(modes_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Index) { expanded_ = Index == 1; rebuild(); });
    filter_ = new fm::ui::FindBox(this); filter_->setObjectName("hc.environment.filter"); filter_->setPlaceholderText(tr("이름 · 값 거르기")); filter_->setMinimumWidth(140); Bar->addWidget(filter_);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& Text) { filterProxy_->setFilterFixedString(Text); emit sigSummaryChanged(); });
    auto* Problems = new fm::ui::WrapCheckBox(tr("문제만"), this); Problems->setObjectName("hc.environment.problemsOnly"); Bar->addWidget(Problems);
    connect(Problems, &QCheckBox::toggled, this, [this](bool Checked) { static_cast<EnvironmentFilter*>(filterProxy_)->SetProblems(Checked); emit sigSummaryChanged(); });
    for (const auto& Pair : {qMakePair(tr("새 변수"), "hc.systemEnvironment.new"), qMakePair(tr("명령 찾기"), "hc.systemEnvironment.findCommand"),
            qMakePair(tr("옛 환경 찾기"), "hc.environment.oldProcesses"), qMakePair(tr("새 프로필"), "hc.environment.newProfile"), qMakePair(tr("가져오기"), "hc.environment.import"), qMakePair(tr("내보내기"), "hc.environment.export")}) {
        auto* Button = new fm::ui::Button(Pair.first, this); Button->setCompact(true); Bar->addWidget(Button);
        connect(Button, &QPushButton::clicked, this, [this, Command = QString::fromLatin1(Pair.second)] { emit sigCommandRequested(Command); });
    }
    notice_ = new fm::ui::Label(tr("저장 전 변경 내용을 확인합니다. 이미 실행 중인 프로그램의 환경은 자동으로 바뀌지 않습니다."), fm::ui::Label::Help, this);
    notice_->setWordWrap(true); notice_->setObjectName("hc.environment.notice"); Layout->addWidget(notice_);
    splitter_ = new QSplitter(this); Layout->addWidget(splitter_, 1);
    treeGrid_ = new Qtitan::TreeGrid(splitter_); itemsGrid_ = new Qtitan::TreeGrid(splitter_); historyGrid_ = new Qtitan::TreeGrid(splitter_);
    treeGrid_->setObjectName("hc.environment.scopes"); itemsGrid_->setObjectName("hc.environment.variables"); historyGrid_->setObjectName("hc.environment.history");
    for (auto* Grid : {treeGrid_, itemsGrid_, historyGrid_}) Grid->setViewType(Qtitan::TreeGrid::TreeView);
    treeView_ = treeGrid_->view<Qtitan::GridTreeView>(); itemsView_ = itemsGrid_->view<Qtitan::GridTreeView>(); historyView_ = historyGrid_->view<Qtitan::GridTreeView>();
    configure(treeGrid_, treeView_, scopeModel_, true); configure(itemsGrid_, itemsView_, filterProxy_, false); configure(historyGrid_, historyView_, historyModel_, false);
    historyGrid_->hide(); splitter_->setSizes({180, 600, 600});
    connect(treeView_, &Qtitan::GridViewBase::focusRowChanged, this, [this] {
        if (syncing_) return; const auto Row = treeView_->focusedRow();
        if (Row.isValid()) {
            const auto Index = Row.modelIndex(0); const auto Id = Index.data(Qt::UserRole + 1).toString();
            if (!Id.isEmpty()) {
                for (const auto& Profile : profiles_) if (Profile.id_ == Id) { SetProfile(Id, Profile.values_); break; }
            } else SetScope(EnvironmentViewScope(Index.row()));
        }
    });
    connect(itemsView_, &Qtitan::GridViewBase::focusRowChanged, this, &CEnvironmentView::sigCursorChanged);
    connect(historyView_, &Qtitan::GridViewBase::focusRowChanged, this, &CEnvironmentView::sigCursorChanged);
    connect(itemsView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { ExecuteCommand("hc.environment.activate"); });
    connect(session_, &EnvironmentSession::Changed, this, &CEnvironmentView::rebuild);
    connect(session_, &EnvironmentSession::LoadingChanged, this, &CEnvironmentView::sigSummaryChanged);
    SetScope(EnvironmentViewScope::User); session_->Refresh();
}
CEnvironmentView::~CEnvironmentView() { if (pathCancel_) pathCancel_->store(true); }
void CEnvironmentView::configure(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View, QAbstractItemModel* Model, bool Tree)
{
    View->options().setAutoCreateColumns(false); View->setModel(Model);
    View->options().setGroupsHeader(false); View->tableOptions().setColumnsHeader(!Tree);
    View->options().setGridLines(Qtitan::LinesNone); View->options().setDragEnabled(false); View->options().setDropEnabled(false);
    View->options().setFocusFollowsMouse(false); View->options().setKeepFocusedRow(true);
    View->tableOptions().setRowsQuickSelection(false); View->tableOptions().setColumnsQuickCustomization(false);
    View->tableOptions().setColumnsQuickMenuVisible(false); View->setFileManagerMode(true);
    for (int I = 0; I < Model->columnCount(); ++I) {
        auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal).toString()));
        Column->setWidth(I == 0 ? 190 : 230);
        if (Model == filterProxy_) Column->dataBinding()->setSortRole(Qt::ItemDataRole(EnvironmentModel::GridSortRole));
    }
    for (auto* Action : View->actions()) { Action->disconnect(); Action->setShortcut({}); }
    Grid->viewport()->setFocusPolicy(Qt::StrongFocus); Grid->installEventFilter(this); Grid->viewport()->installEventFilter(this);
}
void CEnvironmentView::rebuild()
{
    if (!session_) return;
    QSet<QString> MaskExceptions;
    for (const auto& Name : hc::settings::AppSettings().Object()["options"].toObject()["environmentEditor"].toObject()["maskExceptions"].toArray())
        if (Name.isString()) MaskExceptions.insert(Name.toString().toCaseFolded());
    model_->SetMaskExceptions(std::move(MaskExceptions));
    const auto* Previous = Current(); const auto Id = !pendingSelection_.isEmpty() ? pendingSelection_ : Previous ? Previous->id_ : QString();
    if (scope_ == EnvironmentViewScope::Host) other_ = EnvironmentApplication::Shared()->OwnEnvironment();
    if (scope_ == EnvironmentViewScope::Process) other_ = processValues_;
    if (scope_ == EnvironmentViewScope::Process && !processError_.isEmpty()) model_->Clear();
    else if (scope_ == EnvironmentViewScope::Profile) {
        auto Snapshot = session_->Snapshot();
        if (const auto Profile = CurrentProfile()) {
            auto Input = Snapshot.input_;
            for (const auto& Value : Profile->values_) {
                std::erase_if(Input.user_, [&](const auto& Item) { return SameName(Item.name_, Value.name_); }); Input.user_.push_back(Value);
                Snapshot.input_.user_.push_back(Value); // Retain dependencies for transitive masking of preview values.
            }
            other_ = EffectiveEnvironment(BuildEnvironment(Input));
        }
        model_->SetSnapshot(Snapshot, scope_, other_, expanded_);
    } else model_->SetSnapshot(session_->Snapshot(), scope_, other_, expanded_);
    static const auto Catalog = hc::registry::BuiltinRegistryCatalog::Bundled();
    QHash<QString, QString> Descriptions;
    for (int I = 0; I < model_->rowCount(); ++I) {
        const auto* Item = model_->Item(model_->index(I, 0)); if (!Item || !Item->value_) continue;
        const auto Description = Catalog.Find(EnvironmentLocation(Item->value_->scope_ == Scope::System ? Scope::System : Scope::User),
            Item->name_, quint32(Item->value_->type_), QLocale().name().replace('_', '-'));
        if (Description) Descriptions.insert(Item->name_.toCaseFolded(), Description.title);
    }
    model_->SetDescriptions(Descriptions);
    for (auto It = pathResults_.cbegin(); It != pathResults_.cend(); ++It) model_->SetProblems(It.value(), It.key().toStdWString());
    analyzePaths();
    const auto Source = model_->Find(Id), Proxy = filterProxy_->mapFromSource(Source);
    const auto Row = itemsView_->getRow(Proxy);
    if (Row.isValid()) { itemsView_->setFocusedRowIndex(Row.rowIndex()); pendingSelection_.clear(); }
    else if (filterProxy_->rowCount()) itemsView_->setFocusedRowIndex(0);
    for (int I = 0; I < filterProxy_->rowCount(); ++I) {
        const auto Index = filterProxy_->index(I, 0);
        if (Index.data(EnvironmentModel::NameRole).toString().compare("Path", Qt::CaseInsensitive) == 0) itemsView_->expand(Index);
    }
    restoreItems();
    emit sigCursorChanged(); emit sigSummaryChanged();
}
void CEnvironmentView::analyzePaths()
{
    if (!pathProbe_ || !session_->Ready() || pathRevision_ == session_->Revision()) return;
    const auto Revision = pathRevision_ = session_->Revision();
    pathResults_.clear();
    if (pathCancel_) pathCancel_->store(true);
    const auto Cancel = pathCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto Snapshot = session_->Snapshot(); const auto Probe = pathProbe_;
    hc::sys::RunAsync(this, [Snapshot, Probe, Cancel] {
        QHash<QString, std::vector<PathEntry>> Results;
        BudgetedPathProbe Bounded(*Probe, Cancel.get());
        auto Definitions = Snapshot.input_.system_; Definitions.insert(Definitions.end(), Snapshot.input_.user_.begin(), Snapshot.input_.user_.end());
        const auto Sensitive = SensitiveVariables(Definitions);
        for (const auto* Name : {L"Path", L"PSModulePath"}) {
            if (Sensitive.contains(Name)) continue;
            AnalysisInput Input; Input.executablePath_ = SameName(Name, L"Path");
            Input.systemExpansion_ = Snapshot.merged_.afterSystemStrings_; Input.userExpansion_ = Snapshot.merged_.beforeUserExpansion_;
            for (const auto& [Key, Value] : Snapshot.merged_.values_) Input.allDefinitions_[Key] = Value.value_;
            for (const auto& Item : Snapshot.input_.system_) if (SameName(Item.name_, Name)) { Input.system_ = ParseList(Item.raw_).entries_; Input.systemType_ = Item.type_; }
            for (const auto& Item : Snapshot.input_.user_) if (SameName(Item.name_, Name)) { Input.user_ = ParseList(Item.raw_).entries_; Input.userType_ = Item.type_; }
            Results.insert(QString::fromWCharArray(Name), AnalyzePath(Input, Bounded, Cancel.get()));
        }
        return Results;
    }, [this, Revision](const QHash<QString, std::vector<PathEntry>>& Results) {
        if (!session_ || session_->Revision() != Revision) return;
        pathResults_ = Results;
        for (auto It = Results.cbegin(); It != Results.cend(); ++It) model_->SetProblems(It.value(), It.key().toStdWString());
        emit sigSummaryChanged();
    });
}
void CEnvironmentView::SetScope(EnvironmentViewScope Scope, bool Remember)
{
    if (int(Scope) < 0 || int(Scope) >= ScopeNames.size()) return;
    if (Remember && historyPosition_ >= 0 && historyPosition_ < visits_.size()) visits_[historyPosition_] = CaptureState();
    if (Remember && (scopes_.isEmpty() || scopes_[historyPosition_] != Scope)) {
        scopes_.resize(historyPosition_ + 1); visits_.resize(historyPosition_ + 1);
        scopes_.append(Scope); visits_.append(QJsonObject{}); historyPosition_ = scopes_.size() - 1;
    }
    scope_ = Scope; syncing_ = true;
    const auto Row = treeView_->getRow(scopeModel_->index(int(Scope), 0)); if (Row.isValid()) treeView_->setFocusedRowIndex(Row.rowIndex());
    syncing_ = false;
    address_->setSegments({tr("환경 변수"), ScopeNames[int(scope_)]}); address_->setFullPath(PathText());
    itemsGrid_->setVisible(Scope != EnvironmentViewScope::History); historyGrid_->setVisible(Scope == EnvironmentViewScope::History);
    rebuild();
    if (Scope == EnvironmentViewScope::History) emit sigCommandRequested("hc.environment.loadHistory");
    if (Scope == EnvironmentViewScope::Profile && Remember) emit sigCommandRequested("hc.environment.loadProfiles");
}
QModelIndex CEnvironmentView::CurrentSource() const { const auto Row = itemsView_->focusedRow(); return Row.isValid() ? filterProxy_->mapToSource(Row.modelIndex(0)) : QModelIndex(); }
const EnvironmentModel::Row* CEnvironmentView::Current() const { return scope_ == EnvironmentViewScope::History ? nullptr : model_->Item(CurrentSource()); }
void CEnvironmentView::Select(const QString& Name, std::optional<int> Entry)
{
    auto Id = QString::number(int(scope_)) + '/' + Name.toCaseFolded(); if (Entry) Id += '/' + QString::number(*Entry);
    pendingSelection_ = Id;
    const auto Source = model_->Find(Id); if (Source.parent().isValid()) itemsView_->expand(filterProxy_->mapFromSource(Source.parent()));
    const auto Proxy = filterProxy_->mapFromSource(Source), RowIndex = Proxy;
    const auto Row = itemsView_->getRow(RowIndex); if (Row.isValid()) { itemsView_->setFocusedRowIndex(Row.rowIndex()); itemsView_->scrollToIndex(Proxy, false); }
}
void CEnvironmentView::SetProcess(const hc::system::ProcessKey& Key, const ProcessEnvironment& Process)
{
    processKey_ = Key; processValues_ = Process.values_; other_ = processValues_; processDirectory_ = Process.currentDirectory_;
    processError_ = Process.status_ == ProcessReadStatus::Ok ? QString() : tr("프로세스 환경을 읽지 못했습니다 (상태 %1, 오류 %2).").arg(int(Process.status_)).arg(Process.nativeError_);
    SetScope(EnvironmentViewScope::Process); if (!processError_.isEmpty()) ShowNotice(processError_);
}
void CEnvironmentView::SetProfile(QString Id, const std::vector<Value>& Values)
{
    profileId_ = std::move(Id);
    auto Preview = session_->Snapshot().input_;
    for (const auto& Value : Values) {
        std::erase_if(Preview.user_, [&](const auto& Item) { return SameName(Item.name_, Value.name_); }); Preview.user_.push_back(Value);
    }
    other_ = EffectiveEnvironment(BuildEnvironment(Preview)); SetScope(EnvironmentViewScope::Profile, false);
    syncing_ = true;
    auto* Parent = scopeModel_->item(int(EnvironmentViewScope::Profile));
    for (int I = 0; I < Parent->rowCount(); ++I) if (Parent->child(I)->data(Qt::UserRole + 1).toString() == profileId_) {
        treeView_->expand(Parent->index()); const auto Row = treeView_->getRow(Parent->child(I)->index());
        if (Row.isValid()) treeView_->setFocusedRowIndex(Row.rowIndex()); break;
    }
    syncing_ = false;
}
void CEnvironmentView::SetProfiles(QList<EnvironmentProfile> Profiles)
{
    profiles_ = std::move(Profiles); syncing_ = true;
    auto* Parent = scopeModel_->item(int(EnvironmentViewScope::Profile)); Parent->removeRows(0, Parent->rowCount());
    for (const auto& Profile : profiles_) {
        auto* Item = new QStandardItem((Profile.active_ ? tr("켜짐 · ") : tr("꺼짐 · ")) + Profile.name_
            + (Profile.pendingId_.isEmpty() ? QString() : tr(" · 복구 확인 필요")));
        Item->setEditable(false); Item->setData(Profile.id_, Qt::UserRole + 1); Parent->appendRow(Item);
    }
    syncing_ = false;
    auto Selected = CurrentProfile();
    if (!Selected && !profiles_.empty()) Selected = profiles_.front();
    if (Selected) SetProfile(Selected->id_, Selected->values_);
    else { profileId_.clear(); other_.clear(); SetScope(EnvironmentViewScope::Profile, false); ShowNotice(tr("새 프로필을 만들어 사용자 변수의 묶음을 저장하세요.")); }
}
std::optional<EnvironmentProfile> CEnvironmentView::CurrentProfile() const
{
    for (const auto& Profile : profiles_) if (Profile.id_ == profileId_) return Profile;
    return {};
}
void CEnvironmentView::SetHistory(QList<hc::registry::RegistryHistoryRecord> Records) { historyModel_->SetRecords(std::move(Records)); }
std::optional<hc::registry::RegistryHistoryRecord> CEnvironmentView::CurrentHistory() const
{
    const auto Row = historyView_->focusedRow(); return Row.isValid() ? historyModel_->Record(Row.modelIndex(0)) : std::nullopt;
}
std::optional<hc::registry::RegistryStep> CEnvironmentView::CurrentHistoryStep() const
{
    const auto Row = historyView_->focusedRow(); return Row.isValid() ? historyModel_->SelectedStep(Row.modelIndex(0)) : std::nullopt;
}
void CEnvironmentView::Back() { if (CanBack()) { visits_[historyPosition_] = CaptureState(); RestoreState(visits_[--historyPosition_]); } }
void CEnvironmentView::Forward() { if (CanForward()) { visits_[historyPosition_] = CaptureState(); RestoreState(visits_[++historyPosition_]); } }
void CEnvironmentView::FocusFilter() { filter_->setFocus(Qt::ShortcutFocusReason); filter_->selectAll(); }
void CEnvironmentView::SetFilter(const QString& Text) { filter_->setText(Text); }
QString CEnvironmentView::FilterText() const { return filter_->text(); }
void CEnvironmentView::ToggleTree() { treeGrid_->setVisible(!treeGrid_->isVisible()); if (treeGrid_->isVisible()) treeGrid_->viewport()->setFocus(); }
void CEnvironmentView::Refresh() { if (session_) session_->Refresh(); if (scope_ == EnvironmentViewScope::Process) emit sigCommandRequested("hc.environment.refreshProcess"); }
void CEnvironmentView::SetShown(bool Shown) { shown_ = Shown; if (session_ && session_->parent() == this) session_->SetWatching(Shown); }
void CEnvironmentView::SetPanelActive(bool Active) { active_ = Active; setProperty("panelActive", Active); }
void CEnvironmentView::ApplyColorScheme(const TyColorScheme& Colors) { CViewT::ApplyColorScheme(Colors); update(); }
void CEnvironmentView::EnsureKeyboardFocusOnView() const { (scope_ == EnvironmentViewScope::History ? historyGrid_ : itemsGrid_)->viewport()->setFocus(Qt::OtherFocusReason); }
void CEnvironmentView::CloseView() { if (pathCancel_) pathCancel_->store(true); SetShown(false); }
QString CEnvironmentView::PathText() const { return QStringLiteral("system://environment/") + QString::number(int(scope_)); }
QString CEnvironmentView::SummaryText() const { return tr("환경 변수 · %1 · %2개%3").arg(ScopeNames[int(scope_)]).arg(filterProxy_->rowCount()).arg(session_ && session_->Pending() ? tr(" · 읽는 중") : QString()); }
QString CEnvironmentView::StatusRight() const { return scope_ == EnvironmentViewScope::User ? QStringLiteral("HKCU\\Environment") : scope_ == EnvironmentViewScope::System ? QStringLiteral("HKLM · 관리자 도우미로 저장") : tr("읽기 전용"); }
void CEnvironmentView::ShowNotice(const QString& Text) { notice_->setText(Text); }
QJsonObject CEnvironmentView::CaptureState() const
{
    const auto* Row = Current(); QJsonArray Sizes; for (int Size : splitter_->sizes()) Sizes.append(Size);
    QJsonObject State{{"kind", "environment"}, {"scope", int(scope_)}, {"cursor", Row ? Row->id_ : QString()}, {"expanded", expanded_}, {"tree", !treeGrid_->isHidden()}, {"sizes", Sizes}, {"profile", profileId_}};
    QJsonArray Widths, Open;
    for (int I = 0; I < itemsView_->getColumnCount(); ++I) Widths.append(itemsView_->getColumn(I)->width());
    for (int I = 0; I < filterProxy_->rowCount(); ++I) {
        const auto Index = filterProxy_->index(I, 0);
        if (itemsView_->modelController()->isExpanded(Index)) Open.append(Index.data(EnvironmentModel::IdRole).toString());
    }
    State["columns"] = Widths; State["open"] = Open; State["scroll"] = itemsView_->verticalScrollBar()->value();
    if (processKey_) { State["pid"] = qint64(processKey_->pid); State["created"] = QString::number(processKey_->createTime); }
    return State; // No values, drafts or secret-bearing filter text in workspace data.
}
void CEnvironmentView::RestoreState(const QJsonObject& State)
{
    pendingItems_ = State;
    profileId_ = State["profile"].toString(); pendingSelection_ = State["cursor"].toString();
    SetScope(EnvironmentViewScope(State["scope"].toInt()), false); modes_->setCurrentIndex(State["expanded"].toBool() ? 1 : 0);
    treeGrid_->setVisible(State["tree"].toBool(true)); QList<int> Sizes; for (const auto& Value : State["sizes"].toArray()) Sizes.append(Value.toInt()); if (Sizes.size() == 3) splitter_->setSizes(Sizes);
    pendingItems_ = State; restoreItems();
    if (scope_ == EnvironmentViewScope::Profile) emit sigCommandRequested("hc.environment.loadProfiles");
    if (State.contains("pid")) { processKey_ = hc::system::ProcessKey{quint32(State["pid"].toInteger()), State["created"].toString().toULongLong()}; emit sigCommandRequested("hc.environment.refreshProcess"); }
}
void CEnvironmentView::restoreItems()
{
    if (pendingItems_.isEmpty()) return;
    const auto Widths = pendingItems_["columns"].toArray();
    for (int I = 0; I < Widths.size() && I < itemsView_->getColumnCount(); ++I)
        itemsView_->getColumn(I)->setWidth(std::clamp(Widths[I].toInt(160), 40, 2000));
    if (pendingItems_.contains("open")) {
        const auto Open = pendingItems_["open"].toArray();
        for (int I = 0; I < filterProxy_->rowCount(); ++I) {
            const auto Index = filterProxy_->index(I, 0);
            if (Open.contains(Index.data(EnvironmentModel::IdRole).toString())) itemsView_->expand(Index);
            else itemsView_->collapse(Index);
        }
    }
    itemsView_->verticalScrollBar()->setValue(pendingItems_["scroll"].toInt());
    if (session_->Ready() && !session_->Pending()) pendingItems_ = {};
}
bool CEnvironmentView::CanExecuteCommand(const QString& Command) const
{
    if (scope_ == EnvironmentViewScope::Profile) {
        if (Command == "hc.systemEnvironment.new") return true;
        if (Command == "hc.systemEnvironment.edit" || Command == "hc.systemEnvironment.rename"
            || Command == "hc.systemEnvironment.delete" || Command == "hc.systemEnvironment.profileToggle") return CurrentProfile().has_value();
    }
    if (Command == "hc.systemEnvironment.profileToggle") return false;
    const auto* Row = Current(); const bool Editable = scope_ == EnvironmentViewScope::User || scope_ == EnvironmentViewScope::System;
    if (Command == "hc.systemEnvironment.new") return Editable;
    if (Command == "hc.systemEnvironment.moveUp" || Command == "hc.systemEnvironment.moveDown") return Editable && Row && Row->entry_;
    if (Command == "hc.systemEnvironment.rename" || Command == "hc.systemEnvironment.moveScope") return Editable && Row && !Row->entry_;
    if (Command == "hc.systemEnvironment.delete" || Command == "hc.systemEnvironment.edit") return Editable && Row;
    if (Command == "hc.systemEnvironment.addEntry") return Editable && Row && Row->value_ && EnvironmentModel::ListVariable(Row->value_->name_);
    if (Command == "hc.systemEnvironment.describe" || Command == "hc.systemEnvironment.follow" || Command == "hc.systemEnvironment.copy") return Row;
    return true;
}
bool CEnvironmentView::ExecuteCommand(const QString& Command)
{
    if (Command == "hc.environment.activate") {
        const auto Source = CurrentSource(); const auto Proxy = filterProxy_->mapFromSource(Source);
        if (model_->rowCount(Source)) {
            if (itemsView_->modelController()->isExpanded(Proxy)) itemsView_->collapse(Proxy); else itemsView_->expand(Proxy);
        } else if (scope_ == EnvironmentViewScope::Effective || scope_ == EnvironmentViewScope::Host || scope_ == EnvironmentViewScope::Process) emit sigCommandRequested("hc.environment.openOrigin");
        else emit sigCommandRequested("hc.systemEnvironment.edit");
        return true;
    }
    if (Command == "hc.search.focus") { emit sigCommandRequested("hc.systemEnvironment.findCommand"); return true; }
    if (Command == "hc.navigate.refresh" || Command == "hc.navigate.forceRefresh") { Refresh(); return true; }
    if (Command == "hc.navigate.back") { Back(); return true; }
    if (Command == "hc.navigate.forward") { Forward(); return true; }
    if (Command == "hc.systemEnvironment.scopeTree") { ToggleTree(); return true; }
    if (Command == "hc.systemEnvironment.copy") { const auto* Row = Current(); if (Row) QApplication::clipboard()->setText(Row->name_ + '=' + Row->text_); return true; }
    if (Command.startsWith("hc.systemEnvironment.") || Command.startsWith("hc.environment.") || Command == "hc.viewer.quick") { emit sigCommandRequested(Command); return true; }
    return false;
}
bool CEnvironmentView::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Event->type() == QEvent::FocusIn || Event->type() == QEvent::MouseButtonPress) emit sig_NotifyViewActivated();
    if (Event->type() == QEvent::ContextMenu) {
        fm::ui::Menu Menu(this);
        if (const auto* Row = Current(); Row && Row->value_ && !Row->entry_) {
            const auto Name = Row->name_.toCaseFolded();
            auto* Reveal = Menu.addAction(tr("이 변수의 자동 가림 예외"));
            Reveal->setCheckable(true);
            const auto Existing = hc::settings::AppSettings().Object()["options"].toObject()["environmentEditor"].toObject()["maskExceptions"].toArray();
            Reveal->setChecked(Existing.contains(Name));
            connect(Reveal, &QAction::triggered, this, [this, Name](bool Checked) {
                auto& Store = hc::settings::AppSettings(); auto Document = Store.Object();
                auto Options = Document["options"].toObject(), Settings = Options["environmentEditor"].toObject();
                QJsonArray Names;
                for (const auto& Existing : Settings["maskExceptions"].toArray()) if (Existing.toString() != Name) Names.append(Existing);
                if (Checked) Names.append(Name);
                Settings["maskExceptions"] = Names; Options["environmentEditor"] = Settings; Document["options"] = Options;
                if (Store.Replace(Document)) rebuild(); else ShowNotice(tr("가림 예외 설정을 저장하지 못했습니다."));
            });
            Menu.addSeparator();
        }
        if (scope_ == EnvironmentViewScope::Process && !processError_.isEmpty()) {
            auto* Retry = Menu.addAction(tr("관리자 도우미로 환경 다시 읽기"));
            connect(Retry, &QAction::triggered, this, [this] { emit sigCommandRequested("hc.environment.elevateProcess"); });
            Menu.addSeparator();
        }
        if (scope_ == EnvironmentViewScope::Profile) {
            for (const auto& Entry : {qMakePair(tr("프로필 편집"), "hc.environment.editProfile"), qMakePair(tr("프로필 삭제"), "hc.environment.deleteProfile")}) {
                auto* Item = Menu.addAction(Entry.first); Item->setEnabled(CurrentProfile().has_value());
                connect(Item, &QAction::triggered, this, [this, Command = QString::fromLatin1(Entry.second)] { ExecuteCommand(Command); });
            }
            auto* Toggle = Menu.addAction(tr("프로필 켜기 / 끄기")); Toggle->setEnabled(CurrentProfile().has_value());
            connect(Toggle, &QAction::triggered, this, [this] { ExecuteCommand("hc.systemEnvironment.profileToggle"); });
        }
        for (const auto* Command : {"hc.systemEnvironment.describe", "hc.systemEnvironment.edit", "hc.systemEnvironment.new", "hc.systemEnvironment.rename", "hc.systemEnvironment.moveScope", "hc.systemEnvironment.addEntry", "hc.systemEnvironment.delete", "hc.systemEnvironment.follow", "hc.systemEnvironment.copy", "hc.systemEnvironment.undo", "hc.systemEnvironment.redo"}) {
            if (auto* Action = TyStCommandMgr::GetInstance()->Bridge()->Action(Command)) {
                auto* Item = Menu.addAction(Action->text()); Item->setEnabled(CanExecuteCommand(Command)); connect(Item, &QAction::triggered, this, [this, Command] { ExecuteCommand(Command); });
            }
        }
        Menu.exec(static_cast<QContextMenuEvent*>(Event)->globalPos()); return true;
    }
    if (Event->type() == QEvent::KeyPress) {
        auto* Key = static_cast<QKeyEvent*>(Event);
        const auto Command = hc::ui::ShortcutCommand(*Key, TyStCommandMgr::GetInstance()->RetrieveForScope(hc::ui::InputScope::SystemEnvironmentView));
        if (!Command.isEmpty()) { ExecuteCommand(Command); return true; }
        if (Key->modifiers() == Qt::NoModifier && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter)) { ExecuteCommand("hc.environment.activate"); return true; }
        const auto Global = hc::ui::ShortcutCommand(*Key, TyStCommandMgr::GetInstance()->Retrieve());
        if (!Global.isEmpty()) { if (!ExecuteCommand(Global) && hc::ui::IsSystemViewGlobalCommand(Global)) TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(Key, {}); return true; }
    }
    return CViewT::eventFilter(Watched, Event);
}
