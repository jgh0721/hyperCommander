#include "stdafx.h"
#include "cmpRegistryView.hpp"
#include "registryNameOverlay.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "commonLibs/system/processInfo.hpp"
#include <QSysInfo>
#include <QStackedWidget>
#include <QSortFilterProxyModel>

/// 모듈
/// UIs
#include "treeBranchStyle.hpp"
#include "registryLocationMenu.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/commandMgr.hpp"
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/BreadcrumbBar.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/FindBox.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/FlowLayout.h>
#include <fmwidgets/Menu.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QSplitter>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QClipboard>
#include <QScopedValueRollback>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryView, "hc.ui.registry")
using namespace hc::registry;

class RegistryPolicyFilter final : public QSortFilterProxyModel
{
public:
    explicit RegistryPolicyFilter(QObject* Parent) : QSortFilterProxyModel(Parent) { setRecursiveFilteringEnabled(true); setFilterCaseSensitivity(Qt::CaseInsensitive); }
    void SetHideMicrosoft(bool Hide) { beginFilterChange(); hideMicrosoft_ = Hide; endFilterChange(Direction::Rows); }
    void SetKnownOnly(bool Known) { beginFilterChange(); known_ = Known; endFilterChange(Direction::Rows); }
protected:
    bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override
    {
        const auto Index = sourceModel()->index(Row, 0, Parent);
        if (Index.data(RegistryResultModel::GroupRole).toBool()) return false;
        if (known_ && !Index.data(RegistryResultModel::PayloadRole).toMap().value("known").toBool()) return false;
        const auto Payload = Index.data(RegistryResultModel::PayloadRole).toMap();
        if (hideMicrosoft_ && Payload.value("microsoft").toBool() && !Payload.value("risk").toBool()) return false;
        auto Search = Index.data(RegistryResultModel::SearchRole).toString();
        for (auto Ancestor = Parent; Ancestor.isValid(); Ancestor = Ancestor.parent()) Search += '\n' + Ancestor.data().toString();
        return filterRegularExpression().match(Search).hasMatch();
    }
private:
    bool known_ = false, hideMicrosoft_ = false;
};
CRegistryView::CRegistryView(QWidget* Parent) : CViewT(Parent)
{
    viewMode_ = VM_SYSTEM; setObjectName("hc.registry.view");
    service_ = new RegistryService({}, this); keys_ = new RegistryKeyTreeModel(service_, this); items_ = new RegistryItemsModel(service_, this);
    rootProxy_ = new RegistryRootProxy(this); rootProxy_->setSourceModel(items_); rootProxy_->SetFlat(true);
    filterProxy_ = new RegistryFilterProxy(this); filterProxy_->setSourceModel(rootProxy_);
    navigation_ = new RegistryNavigation(service_, keys_, items_, this);
    descriptions_ = RegistryDescriptions::Shared(QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-cache/admx"));
    const auto UpdateDescriptions = [this] {
        items_->SetDescriptions([Descriptions = QPointer<RegistryDescriptions>(descriptions_)](const RegistryLocation& Location,
            const std::optional<QString>& Name, std::optional<quint32> Type) {
            return Descriptions ? Descriptions->Find(Location, Name, Type).title : QString();
        });
        if (policyMode_ && policies_) policies_->Reload(descriptions_->Policies());
        emit sigCursorChanged(); emit sigSummaryChanged();
    };
    connect(descriptions_, &RegistryDescriptions::Changed, this, UpdateDescriptions);
    UpdateDescriptions();
    auto* Outer = new QVBoxLayout(this); Outer->setContentsMargins(0, 4, 0, 0); Outer->setSpacing(4);
    headerScroll_ = new fm::ui::ScrollPanel(this); headerScroll_->setObjectName("hc.registry.headerScroll");
    headerContent_ = new QWidget; headerContent_->installEventFilter(this);
    auto* Layout = new QVBoxLayout(headerContent_); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(4);
    headerScroll_->setContentWidget(headerContent_); Outer->addWidget(headerScroll_);
    auto* Navigation = new QHBoxLayout; Navigation->setSpacing(3);
    const auto Button = [this, Navigation](const QString& Text, const QString& Name, auto Handler) {
        auto* Result = new fm::ui::Button(Text, this); Result->setCompact(true); Result->setObjectName(Name);
        connect(Result, &QPushButton::clicked, this, Handler); Navigation->addWidget(Result); return Result;
    };
    back_ = Button(QStringLiteral("←"), "hc.registry.back", [this] { Back(); });
    forward_ = Button(QStringLiteral("→"), "hc.registry.forward", [this] { Forward(); });
    Button(QStringLiteral("↑"), "hc.registry.up", [this] { Up(); });
    Button(tr("트리"), "hc.registry.toggleTree", [this] { ToggleTree(); });
    address_ = new fm::ui::BreadcrumbBar(this); address_->setObjectName("hc.registry.address"); Navigation->addWidget(address_, 1);
    address_->setSingleClickEdit(true); address_->setMiddleClickPaste(true);
    connect(address_, &fm::ui::BreadcrumbBar::pathEntered, this, [this](const QString& Path) { Navigate(Path); });
    connect(address_, &fm::ui::BreadcrumbBar::segmentClicked, this, [this](int Segment) {
        const auto Parts = navigation_->Path().split('\\'); Navigate(Segment < 0 ? QString() : Parts.mid(0, Segment + 1).join('\\'));
    });
    Button(QStringLiteral("☆"), "hc.registry.favorite", [this] { emit sigCommandRequested("hc.registry.favorite"); });
    Layout->addLayout(Navigation);
    auto* Bar = new fm::ui::FlowLayout(4, 4);
    auto* Modes = new fm::ui::SegmentedControl({tr("탐색"), tr("적용된 정책"), tr("자동 실행"), tr("변경 기록"), tr("찾기 결과")}, this);
    modes_ = Modes; Modes->setObjectName("hc.registry.modes"); Modes->setSegmentSize(fm::ui::SegmentedControl::Compact); Modes->setCurrentIndex(0);
    connect(Modes, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Index) {
        if (Index == 4) { ShowSearch(); return; }
        const QStringList Commands{"hc.registry.browse", "hc.registry.policies", "hc.registry.autoruns", "hc.registry.history"};
        if (Index >= 0 && Index < Commands.size()) emit sigCommandRequested(Commands[Index]);
    });
    Modes->button(4)->hide();
    Bar->addWidget(Modes);
    listMode_ = new fm::ui::SegmentedControl({tr("목록"), tr("펼쳐 보기")}, this); listMode_->setObjectName("hc.registry.listMode");
    listMode_->setSegmentSize(fm::ui::SegmentedControl::Compact); listMode_->setCurrentIndex(0);
    connect(listMode_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Index) { SetFlat(Index == 0); });
    Bar->addWidget(listMode_);
    filter_ = new fm::ui::FindBox(this); filter_->setObjectName("hc.registry.filter");
    filter_->setPlaceholderText(tr("이 목록에서 거르기")); filter_->setAccessibleName(tr("이 목록에서 거르기"));
    filter_->setMinimumWidth(140); filter_->setMaximumWidth(280); Bar->addWidget(filter_);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& Text) {
        if (searchMode_) { searchFilter_->setFilterFixedString(Text); expandSearchGroups(); emit sigSummaryChanged(); return; }
        if (autorunMode_) { autorunFilter_->setFilterFixedString(Text); QTimer::singleShot(0, this, &CRegistryView::expandAutorunGroups); emit sigSummaryChanged(); return; }
        if (policyMode_) { policyFilter_->setFilterFixedString(Text); QTimer::singleShot(0, this, &CRegistryView::expandPolicyGroups); emit sigSummaryChanged(); return; }
        if (historyMode_) { historyFilter_->setFilterFixedString(Text); emit sigSummaryChanged(); return; }
        saveCursor(); filterProxy_->SetText(Text); restoreCursor(); emit sigSummaryChanged();
    });
    auto* Known = new fm::ui::WrapCheckBox(tr("설명 있는 항목만"), this); Known->setObjectName("hc.registry.knownOnly");
    Bar->addWidget(Known);
    connect(Known, &QCheckBox::toggled, this, [this](bool Checked) {
        if (policyFilter_) { policyFilter_->SetKnownOnly(Checked); QTimer::singleShot(0, this, &CRegistryView::expandPolicyGroups); }
        saveCursor(); filterProxy_->SetKnownOnly(Checked); restoreCursor(); emit sigSummaryChanged();
    });
    auto* HideMicrosoft = new fm::ui::WrapCheckBox(tr("Microsoft 서명 항목 숨기기"), this);
    HideMicrosoft->setObjectName("hc.registry.hideMicrosoft"); Bar->addWidget(HideMicrosoft); HideMicrosoft->hide();
    connect(HideMicrosoft, &QCheckBox::toggled, this, [this](bool Hide) { autorunFilter_->SetHideMicrosoft(Hide); QTimer::singleShot(0, this, &CRegistryView::expandAutorunGroups); });
    bits_ = new fm::ui::ChoiceBox(this); bits_->setObjectName("hc.registry.bitView");
    bits_->addItem(tr("64비트 보기"), int(View::Native64)); bits_->addItem(tr("32비트 보기 (WOW6432Node)"), int(View::Wow32));
    bits_->setAccessibleName(tr("레지스트리 보기")); bits_->setMinimumContentsLength(18); Bar->addWidget(bits_);
    connect(bits_, &QComboBox::currentIndexChanged, this, [this](int Index) { SetRegistryView(Index == 1 ? View::Wow32 : View::Native64); });
    Layout->addLayout(Bar);
    auto* Locations = new fm::ui::Button(tr("즐겨찾기 · 알려진 위치"), this);
    Locations->setCompact(true); Locations->setObjectName("hc.registry.knownLocations");
    Bar->addWidget(Locations);
    const bool Elevated = hc::permissions::Elevated();
    permissionNotice_ = Elevated ? tr("관리자 권한으로 실행 중입니다.") : tr("표준 권한으로 실행 중입니다. HKLM과 다른 사용자 키의 변경은 저장할 때 관리자 도우미가 권한을 요청합니다.");
    auto* NoticeRow = new QHBoxLayout;
    notice_ = new fm::ui::Label(permissionNotice_, fm::ui::Label::Help, this);
    notice_->setObjectName("hc.registry.notice"); NoticeRow->addWidget(notice_, 1);
    auto* Helper = new fm::ui::Button(tr("관리자 도우미 미리 시작"), this); Helper->setCompact(true); Helper->setEnabled(!Elevated);
    Helper->setObjectName("hc.registry.adminHelper"); NoticeRow->addWidget(Helper);
    connect(Helper, &QPushButton::clicked, this, [this] { emit sigCommandRequested("hc.system.startAdminHelper"); });
    connect(&hc::sys::AdminEvents(), &hc::sys::AdminSessionEvents::sigConnected, this, [this] {
        permissionNotice_ = tr("관리자 도우미가 연결되었습니다. 변경 전 백업과 권한 확인을 수행합니다.");
        notice_->setText(permissionNotice_); notice_->setTone(fm::ui::Label::Default);
    });
    Layout->addLayout(NoticeRow);
    splitter_ = new QSplitter(this); splitter_->setObjectName("hc.registry.splitter");
    treeGrid_ = new Qtitan::TreeGrid(splitter_); itemPages_ = new QStackedWidget(splitter_);
    itemsGrid_ = new Qtitan::TreeGrid(itemPages_); historyGrid_ = new Qtitan::TreeGrid(itemPages_);
    itemPages_->addWidget(itemsGrid_); itemPages_->addWidget(historyGrid_); historyGrid_->setObjectName("hc.registry.historyGrid");
    history_ = new RegistryHistoryModel(this); historyFilter_ = new QSortFilterProxyModel(this);
    historyFilter_->setSourceModel(history_); historyFilter_->setRecursiveFilteringEnabled(true); historyFilter_->setAutoAcceptChildRows(true); historyFilter_->setFilterCaseSensitivity(Qt::CaseInsensitive); historyFilter_->setFilterKeyColumn(-1);
    historyGrid_->setViewType(Qtitan::TreeGrid::TreeView);
    historyView_ = historyGrid_->view<Qtitan::GridTreeView>();
    configureGrid(historyGrid_, historyView_, historyFilter_, false);
    static_cast<Qtitan::GridTableColumn*>(historyView_->getColumnByModelColumn(0))->setSortOrder(Qtitan::SortDescending);
    connect(historyView_, &Qtitan::GridViewBase::focusRowChanged, this, &CRegistryView::sigCursorChanged);
    connect(historyView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { emit sigCommandRequested("hc.registry.open"); });
    connect(history_, &RegistryHistoryModel::Reloaded, this, [this] {
        if (historyMode_ && history_->Error()) { notice_->setText(tr("일부 변경 기록을 읽지 못했습니다 (오류 %1). 원본 백업은 보존됩니다.").arg(history_->Error())); notice_->setTone(fm::ui::Label::Danger); }
        emit sigSummaryChanged(); if (historyMode_) emit sigCursorChanged();
    });
    policies_ = new RegistryPolicies(this); policyFilter_ = new RegistryPolicyFilter(this); policyFilter_->setSourceModel(policies_->Model());
    policyGrid_ = new Qtitan::TreeGrid(itemPages_); policyGrid_->setObjectName("hc.registry.policyGrid"); itemPages_->addWidget(policyGrid_);
    policyGrid_->setViewType(Qtitan::TreeGrid::TreeView); policyView_ = policyGrid_->view<Qtitan::GridTreeView>();
    configureGrid(policyGrid_, policyView_, policyFilter_, false);
    connect(policyView_, &Qtitan::GridViewBase::focusRowChanged, this, &CRegistryView::sigCursorChanged);
    connect(policyView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { emit sigCommandRequested("hc.registry.open"); });
    connect(policies_, &RegistryPolicies::Changed, this, [this] {
        if (!policyMode_) return;
        expandPolicyGroups();
        notice_->setText(policies_->Diagnostics().isEmpty() ? permissionNotice_ : policies_->Diagnostics().join('\n'));
        emit sigSummaryChanged(); emit sigCursorChanged();
    });
    autoruns_ = new RegistryAutoruns(this); autorunFilter_ = new RegistryPolicyFilter(this); autorunFilter_->setSourceModel(autoruns_->Model());
    autorunGrid_ = new Qtitan::TreeGrid(itemPages_); autorunGrid_->setObjectName("hc.registry.autorunGrid"); itemPages_->addWidget(autorunGrid_);
    autorunGrid_->setViewType(Qtitan::TreeGrid::TreeView); autorunView_ = autorunGrid_->view<Qtitan::GridTreeView>();
    configureGrid(autorunGrid_, autorunView_, autorunFilter_, false);
    connect(autorunView_, &Qtitan::GridViewBase::focusRowChanged, this, &CRegistryView::sigCursorChanged);
    connect(autorunView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { emit sigCommandRequested("hc.registry.open"); });
    connect(autoruns_, &RegistryAutoruns::Changing, this, [this] {
        if (autorunMode_) autorunState_ = RegistryGridState::Capture(autorunView_);
    });
    connect(autoruns_, &RegistryAutoruns::Changed, this, [this] {
        if (!autorunMode_) return; expandAutorunGroups();
        autorunState_.Restore(autorunView_, [this](const QString& Id) {
            std::function<QModelIndex(const QModelIndex&)> Find = [&](const QModelIndex& Parent) -> QModelIndex {
                for (int Row = 0; Row < autorunFilter_->rowCount(Parent); ++Row) {
                    const auto Index = autorunFilter_->index(Row, 0, Parent);
                    if (Index.data(RegistryTreeModel::IdRole).toString() == Id) return Index;
                    const auto Child = Find(Index); if (Child.isValid()) return Child;
                }
                return {};
            };
            return Find({});
        });
        notice_->setText(autoruns_->Diagnostics().join('\n')); emit sigSummaryChanged(); emit sigCursorChanged();
    });
    setupSearch(Layout);
    ReloadHistory();
    treeGrid_->setObjectName("hc.registry.keys"); itemsGrid_->setObjectName("hc.registry.items");
    treeGrid_->setViewType(Qtitan::TreeGrid::TreeView); itemsGrid_->setViewType(Qtitan::TreeGrid::TreeView);
    treeView_ = treeGrid_->view<Qtitan::GridTreeView>(); itemsView_ = itemsGrid_->view<Qtitan::GridTreeView>();
    configureGrid(treeGrid_, treeView_, keys_, true); configureGrid(itemsGrid_, itemsView_, filterProxy_, false);
    splitter_->setStretchFactor(0, 0); splitter_->setStretchFactor(1, 1); splitter_->setSizes({200, 600}); Outer->addWidget(splitter_, 1);
    connect(treeView_, &Qtitan::GridViewBase::focusRowChanged, this, [this] {
        if (syncing_ || modelBatchDepth_) return;
        const auto Row = treeView_->focusedRow(); const auto Key = keys_->Location(Row.isValid() ? Row.modelIndex(0) : QModelIndex());
        if (Key) Navigate(Key->Path());
    });
    nameEditor_ = new RegistryNameOverlay(itemsGrid_, itemsView_);
    treeNameEditor_ = new RegistryNameOverlay(treeGrid_, treeView_);
    connect(treeNameEditor_, &RegistryNameOverlay::CommitRequested, this, [this](const QString& Name) {
        if (!renameSource_.isValid()) { FinishRename(tr("항목이 바뀌었습니다. 다시 선택하세요.")); return; }
        emit sigRenameRequested(renameSource_, Name);
    });
    connect(nameEditor_, &RegistryNameOverlay::CommitRequested, this, [this](const QString& Name) {
        if (!renameSource_.isValid()) { FinishRename(tr("항목이 바뀌었습니다. 다시 선택하세요.")); return; }
        emit sigRenameRequested(renameSource_, Name);
    });
    connect(itemsView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { emit sigCommandRequested("hc.registry.open"); });
    connect(itemsView_, &Qtitan::GridViewBase::focusRowChanged, this, &CRegistryView::sigCursorChanged);
    connect(itemsView_, &Qtitan::GridViewBase::selectionChanged, this, &CRegistryView::sigSummaryChanged);
    connect(itemsView_, &Qtitan::GridViewBase::selectionChanged, this, [this] { saveCursor(); });
    connect(itemsView_, &Qtitan::GridViewBase::focusRowChanged, this, [this] { saveCursor(); });
    connect(navigation_, &RegistryNavigation::Navigated, this, &CRegistryView::navigated);
    connect(navigation_, &RegistryNavigation::Failed, this, [this](const QString& Path, quint32 Error) {
        if (!navigation_->Current()) navigated();
        notice_->setText(tr("%1을(를) 열 수 없습니다 (오류 %2).").arg(Path).arg(Error)); notice_->setTone(fm::ui::Label::Danger);
    });
    connect(items_, &RegistryTreeModel::BatchAboutToChange, this, [this] { saveCursor(); ++modelBatchDepth_; itemsView_->beginUpdate(); });
    connect(items_, &RegistryTreeModel::BatchChanged, this, [this] { itemsView_->endUpdate(); --modelBatchDepth_; restoreCursor(); emit sigSummaryChanged(); });
    connect(keys_, &RegistryTreeModel::BatchAboutToChange, this, [this] { saveCursor(); ++modelBatchDepth_; treeView_->beginUpdate(); });
    connect(keys_, &RegistryTreeModel::BatchChanged, this, [this] { treeView_->endUpdate(); --modelBatchDepth_; restoreCursor(); });
    connect(itemsView_, &Qtitan::GridViewBase::sortingChanged, this, &CRegistryView::applySort);
    connect(rootProxy_, &RegistryRootProxy::RootChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (navigation_->Pending()) return;
            const auto Root = items_->Location(rootProxy_->Root());
            const auto Current = navigation_->Current();
            if ((Root ? Root->Id() : QString()) != (Current ? Current->Id() : QString()))
                navigation_->Navigate(Root ? Root->Path() : QString(), false);
        });
    });
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { fitColumns(); });
    items_->SetSort(RegistryTreeModel::Name, Qt::AscendingOrder); keys_->SetSort(0, Qt::AscendingOrder);
    static_cast<Qtitan::GridTableColumn*>(itemsView_->getColumnByModelColumn(0))->setSortOrder(Qtitan::SortAscending);
    static_cast<Qtitan::GridTableColumn*>(treeView_->getColumnByModelColumn(0))->setSortOrder(Qtitan::SortAscending);
    navigated();
    Locations->setMenu(new RegistryLocationMenu(this, Locations));
}
CRegistryView::~CRegistryView() { CloseView(); }
void CRegistryView::configureGrid(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View, QAbstractItemModel* Model, bool Tree)
{
    View->options().setAutoCreateColumns(false); View->setModel(Model);
    auto& Options = View->options(); Options.setGroupsHeader(false); View->tableOptions().setColumnsHeader(!Tree);
    Options.setGridLines(Qtitan::LinesNone); Options.setDragEnabled(false); Options.setDropEnabled(false);
    Options.setFocusFollowsMouse(false); Options.setKeepFocusedRow(true); Options.setShowWaitCursor(false);
    View->tableOptions().setRowsQuickSelection(false); View->tableOptions().setColumnsQuickCustomization(false);
    View->tableOptions().setColumnsQuickMenuVisible(false); View->setFileManagerMode(true);
    for (int I = 0; I < Model->columnCount(); ++I) {
        auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal).toString()));
        Column->setWidth(I == 0 ? 210 : 160); Column->setMinWidth(60);
        Column->dataBinding()->setSortRole(Qt::ItemDataRole(RegistryTreeModel::GridSortRole));
    }
    auto* Style = new TreeBranchStyle(this); Grid->setStyle(Style);
    Grid->viewport()->setFocusPolicy(Qt::StrongFocus); Grid->installEventFilter(this); Grid->viewport()->installEventFilter(this);
    for (auto* Action : View->actions()) { (void)Action->disconnect(); Action->setShortcut({}); }
}
void CRegistryView::navigated()
{
    notice_->setText(permissionNotice_); notice_->setTone(fm::ui::Label::Default);
    const auto Key = navigation_->Current();
    { const QSignalBlocker Block(bits_); bits_->setCurrentIndex(navigation_->SelectedView() == View::Wow32 ? 1 : 0); }
    rootProxy_->SetRoot(Key ? items_->Find(Key->Id()) : QModelIndex());
    address_->setFullPath(navigation_->Path()); address_->setSegments(Key ? navigation_->Path().split('\\') : QStringList{tr("컴퓨터")});
    back_->setEnabled(navigation_->CanBack()); forward_->setEnabled(navigation_->CanForward());
    if (Key) {
        const auto Index = keys_->Find(Key->Id()); syncing_ = true;
        QModelIndexList Ancestors;
        for (auto Parent = Index.parent(); Parent.isValid(); Parent = Parent.parent()) Ancestors.prepend(Parent);
        treeState_.cursor = Key->Id();
        for (const auto& Parent : Ancestors) {
            const auto Id = Parent.data(RegistryTreeModel::IdRole).toString();
            if (!treeState_.expanded.contains(Id)) treeState_.expanded.append(Id);
            treeView_->expand(Parent);
        }
        const auto Row = treeView_->getRow(Index); if (Row.isValid()) treeView_->setFocusedRowIndex(Row.rowIndex());
        syncing_ = false;
    }
    restoreCursor(); updateWatched();
    fitColumns(); emit sigSummaryChanged(); emit sigCursorChanged();
}
bool CRegistryView::Navigate(const QString& Path, std::optional<QString> ValueName)
{
    ++navigationRevision_;
    if (historyMode_ || policyMode_ || autorunMode_ || searchMode_) ShowHistory(false);
    if (ValueName) {
        sourceTree_ = false;
        SetFilter({});
        if (auto* Known = findChild<QCheckBox*>("hc.registry.knownOnly")) Known->setChecked(false);
    }
    retainState_ = false; itemState_ = {}; cursorId_.clear(); treeState_ = RegistryGridState::Capture(treeView_);
    if (ValueName) if (const auto Location = RegistryLocation::Parse(Path, navigation_->SelectedView())) {
        itemState_.cursor = RegistryTreeModel::ValueId(*Location, *ValueName);
        itemState_.selected = {itemState_.cursor}; retainState_ = true;
    }
    return navigation_->Navigate(Path);
}
void CRegistryView::SetRegistryView(View SelectedView)
{
    if (SelectedView == navigation_->SelectedView()) return;
    ++navigationRevision_;
    retainState_ = false; itemState_ = {}; treeState_ = {}; cursorId_.clear();
    navigation_->SetView(SelectedView);
}
void CRegistryView::Back() { ++navigationRevision_; retainState_ = false; itemState_ = {}; cursorId_.clear(); navigation_->Back(); }
void CRegistryView::Forward() { ++navigationRevision_; retainState_ = false; itemState_ = {}; cursorId_.clear(); navigation_->Forward(); }
void CRegistryView::Up()
{
    ++navigationRevision_;
    if (!navigation_->Current()) return;
    retainState_ = false;
    itemState_ = {}; itemState_.cursor = navigation_->Current()->Id(); cursorId_ = itemState_.cursor;
    navigation_->Up();
}
void CRegistryView::EditAddress()
{
    emit sig_NotifyViewActivated();
    updateHeaderGeometry(); headerScroll_->ensureWidgetVisible(address_, 0, 0);
    address_->EditPath();
}
void CRegistryView::ToggleTree() { treeGrid_->setVisible(treeGrid_->isHidden()); fitColumns(); }
void CRegistryView::Refresh()
{
    if (autorunMode_) autoruns_->Reload();
    if (policyMode_) policies_->Reload(descriptions_->Policies());
    ReloadHistory();
    if (navigation_->Current()) {
        service_->Invalidate(*navigation_->Current(), true);
        service_->Request(*navigation_->Current());
    }
}
void CRegistryView::ReloadHistory()
{
    history_->Reload(QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-backup"),
        QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()));
}
void CRegistryView::ShowHistory(bool Show)
{
    if (searchMode_) ShowSearch(false);
    if (autorunMode_) ShowAutoruns(false);
    if (policyMode_) ShowPolicies(false);
    if (historyMode_ == Show) { if (Show) ReloadHistory(); return; }
    if (Show) saveCursor();
    historyMode_ = Show; itemPages_->setCurrentWidget(Show ? historyGrid_ : itemsGrid_);
    { const QSignalBlocker Block(modes_); modes_->setCurrentIndex(Show ? 3 : 0); }
    bits_->setEnabled(!Show); listMode_->setVisible(!Show);
    if (auto* Known = findChild<QCheckBox*>("hc.registry.knownOnly")) Known->setEnabled(!Show);
    if (Show) { historyFilter_->setFilterFixedString(filter_->text()); ReloadHistory(); }
    else { autoruns_->SetActive(false); filterProxy_->SetText(filter_->text()); restoreCursor(); }
    fitColumns(); emit sigSummaryChanged(); emit sigCursorChanged();
}
void CRegistryView::expandPolicyGroups()
{
    if (!policyMode_ || !policyFilter_ || !policyView_) return;
    const auto Expand = [&](auto&& Self, const QModelIndex& Parent) -> void {
        for (int Row = 0; Row < policyFilter_->rowCount(Parent); ++Row) {
            const auto Index = policyFilter_->index(Row, 0, Parent);
            if (!Index.data(RegistryResultModel::GroupRole).toBool()) continue;
            policyView_->expand(Index);
            if (!filter_->text().isEmpty()) Self(Self, Index);
        }
    };
    Expand(Expand, {});
    emit sigSummaryChanged();
}
void CRegistryView::ShowPolicies(bool Show)
{
    if (searchMode_) ShowSearch(false);
    if (autorunMode_) ShowAutoruns(false);
    if (historyMode_) ShowHistory(false);
    if (policyMode_ == Show) { if (Show) policies_->Reload(descriptions_->Policies()); return; }
    ++navigationRevision_;
    if (Show) saveCursor();
    policyMode_ = Show; itemPages_->setCurrentWidget(Show ? policyGrid_ : itemsGrid_);
    { const QSignalBlocker Block(modes_); modes_->setCurrentIndex(Show ? 1 : 0); }
    bits_->setEnabled(!Show); listMode_->setVisible(!Show);
    if (auto* Known = findChild<QCheckBox*>("hc.registry.knownOnly")) Known->setEnabled(true);
    if (Show) { policyFilter_->setFilterFixedString(filter_->text()); policies_->Reload(descriptions_->Policies()); }
    else { autoruns_->SetActive(false); filterProxy_->SetText(filter_->text()); restoreCursor(); }
    fitColumns(); emit sigSummaryChanged(); emit sigCursorChanged();
}
std::optional<RegistryPolicyEntry> CRegistryView::CurrentPolicy() const
{
    return policyMode_ ? policies_->Entry(CurrentSource().data(RegistryTreeModel::IdRole).toString()) : std::nullopt;
}
std::optional<RegistryHistoryRecord> CRegistryView::CurrentHistory() const
{
    return historyMode_ ? history_->Record(CurrentSource()) : std::nullopt;
}
void CRegistryView::SetFilter(const QString& Text) { filter_->setText(Text); }
QString CRegistryView::FilterText() const { return filter_->text(); }
void CRegistryView::FocusFilter() { filter_->setFocus(Qt::ShortcutFocusReason); filter_->selectAll(); }
bool CRegistryView::CanExecuteCommand(const QString& Command) const
{
    if (Command == "hc.registry.importPreview") return true;
    if (Command == "hc.registry.export" || Command == "hc.registry.compare") return !historyMode_ && (CurrentSource().isValid() || navigation_->Current().has_value());
    if (Command == "hc.registry.stopFind") return search_ && search_->Running();
    if (Command == "hc.registry.findNextBatch") return search_ && !search_->Running() && !search_->Status().done;
    if (Command == "hc.registry.replaceResults" || Command == "hc.registry.exportResults") return searchMode_ && !search_->Running() && !search_->Rows().isEmpty();
    if (searchMode_ && Command != "hc.registry.browse" && Command != "hc.registry.history"
        && Command != "hc.registry.policies" && Command != "hc.registry.autoruns" && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        if (Command == "hc.registry.open") return CurrentSource().isValid();
        if (Command == "hc.registry.edit" || Command == "hc.registry.describe" || Command == "hc.registry.copyPath" || Command == "hc.registry.follow") return CurrentSearchResult().has_value();
        return false;
    }
    if (Command == "hc.registry.history" || Command == "hc.registry.browse" || Command == "hc.registry.policies" || Command == "hc.registry.autoruns") return true;
    if (autorunMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        if (Command == "hc.registry.open") return CurrentSource().isValid();
        if (Command == "hc.registry.toggleAutorun") return CurrentAutorun() && CurrentAutorun()->canToggle;
        if (Command == "hc.registry.describe" || Command == "hc.registry.edit" || Command == "hc.registry.copyPath" || Command == "hc.registry.follow") return CurrentAutorun().has_value();
        return false;
    }
    if (policyMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        if (Command == "hc.registry.open") return CurrentSource().isValid();
        if (Command == "hc.registry.edit" || Command == "hc.registry.describe" || Command == "hc.registry.copyPath") return CurrentSource().isValid() && !CurrentSource().data(RegistryResultModel::GroupRole).toBool();
        return false;
    }
    if (historyMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        if (Command == "hc.registry.open" || Command == "hc.registry.describe" || Command == "hc.registry.copyPath") return CurrentHistory().has_value();
        return false;
    }
    if (Command.startsWith("hc.registry.favorite.")) {
        bool Valid = false; const int Digit = Command.section('.', -1).toInt(&Valid);
        const auto* Cache = service_->Cached(RegistryFavorites::SharedLocation());
        return Valid && Digit >= 0 && Digit <= 9 && Cache && Cache->loaded && Cache->snapshot
            && RegistryFavorites::Decode(Cache->snapshot.value, navigation_->SelectedView()).size() > (Digit ? Digit - 1 : 9);
    }
    if (Command == "hc.registry.address" || Command == "hc.registry.tree" || Command == "hc.registry.browse") return true;
    if (Command == "hc.registry.parent") return navigation_->Current().has_value();
    const auto Current = CurrentSource();
    const bool Value = Current.isValid() && Current.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value);
    const auto Location = RegistryLocation::Parse(Current.data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView());
    if (Command == "hc.registry.newKey" || Command == "hc.registry.newValue") return navigation_->Current().has_value();
    if (Command == "hc.registry.rename") return Location && (Value ? !Current.data(RegistryTreeModel::ValueNameRole).toString().isEmpty() : !Location->key.isEmpty());
    if (Command == "hc.registry.delete") return Location && (Value || !Location->key.isEmpty());
    if (Command == "hc.registry.copyName" || Command == "hc.registry.copyData") return Value;
    if (Command == "hc.registry.copyReg") return Value;
    if (Command == "hc.registry.follow") return CurrentSource().isValid();
    if (Command == "hc.registry.describe") return CurrentSource().isValid();
    if (Command == "hc.registry.edit") return CurrentSource().isValid() && CurrentSource().data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value);
    if (Command == "hc.registry.open") return CurrentSource().isValid() && CurrentSource().data(RegistryTreeModel::KindRole).toInt() != int(RegistryTreeModel::Kind::Status);
    if (Command == "hc.registry.copyPath") return navigation_->Current().has_value() || CurrentSource().isValid();
    return false;
}
bool CRegistryView::ExecuteCommand(const QString& Command)
{
    if (!CanExecuteCommand(Command)) return false;
    if (Command == "hc.registry.stopFind") { search_->Stop(); return true; }
    if (Command == "hc.registry.findNextBatch") { search_->Continue(); return true; }
    if (searchMode_ && (Command == "hc.registry.open" || Command == "hc.registry.copyPath")) {
        if (const auto Result = CurrentSearchResult(); Result && Result->location) {
            if (Command == "hc.registry.copyPath") QApplication::clipboard()->setText(Result->location->PhysicalPath());
            else { SetRegistryView(Result->location->view); Navigate(Result->location->Path(), Result->value ? std::optional<QString>(Result->value->name) : std::nullopt); }
        } else if (Command == "hc.registry.open") {
            const auto Row = searchView_->focusedRow();
            if (searchView_->modelController()->isRowExpanded(Row)) searchView_->collapseRow(Row); else searchView_->expandRow(Row);
        }
        return true;
    }
    if (Command == "hc.registry.autoruns") { ShowAutoruns(); return true; }
    if (Command == "hc.registry.policies") { ShowPolicies(); return true; }
    if (Command == "hc.registry.history" || Command == "hc.registry.browse") { ShowHistory(Command == "hc.registry.history"); return true; }
    if (autorunMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        const auto Entry = CurrentAutorun();
        if (Entry && Command == "hc.registry.copyPath") QApplication::clipboard()->setText(Entry->location.Path());
        else if (Command == "hc.registry.open" && !Entry) {
            const auto Row = autorunView_->focusedRow(); if (autorunView_->modelController()->isRowExpanded(Row)) autorunView_->collapseRow(Row); else autorunView_->expandRow(Row);
        }
        return true;
    }
    if (policyMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        const auto Index = CurrentSource();
        if (Command == "hc.registry.copyPath") QApplication::clipboard()->setText(Index.data(RegistryTreeModel::LocationRole).toString());
        if (Command == "hc.registry.open" && Index.data(RegistryResultModel::GroupRole).toBool()) {
            const auto Row = policyView_->focusedRow();
            if (policyView_->modelController()->isRowExpanded(Row)) policyView_->collapseRow(Row); else policyView_->expandRow(Row);
        }
        return true;
    }
    if (historyMode_ && Command != "hc.registry.address" && Command != "hc.registry.tree") {
        const auto Record = CurrentHistory();
        if (Record && !Record->plan.steps.isEmpty()) {
            const auto Step = history_->SelectedStep(CurrentSource()).value_or(Record->plan.steps.first());
            if (Command == "hc.registry.open") Navigate(Step.location.Path(), Step.kind == StepKind::Value ? std::optional<QString>(Step.name) : std::nullopt);
            else if (Command == "hc.registry.copyPath") QApplication::clipboard()->setText(Step.location.Path());
        }
        return true;
    }

    if (Command.startsWith("hc.registry.favorite.")) {
        const int Digit = Command.section('.', -1).toInt();
        const auto* Cache = service_->Cached(RegistryFavorites::SharedLocation());
        Navigate(RegistryFavorites::Decode(Cache->snapshot.value, navigation_->SelectedView()).at(Digit ? Digit - 1 : 9).location.Path());
    }
    else if (Command == "hc.registry.address") EditAddress();
    else if (Command == "hc.registry.parent") Up();
    else if (Command == "hc.registry.tree") {
        const bool TreeFocused = treeGrid_->isAncestorOf(QApplication::focusWidget());
        if (!treeGrid_->isHidden() && TreeFocused) { treeGrid_->hide(); EnsureKeyboardFocusOnView(); }
        else { treeGrid_->show(); treeGrid_->viewport()->setFocus(Qt::ShortcutFocusReason); }
    }
    else if (Command == "hc.registry.open") {
        const auto Location = RegistryLocation::Parse(CurrentSource().data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView()); if (Location) Navigate(Location->Path());
    }
    else if (Command == "hc.registry.copyPath") {
        const auto Location = RegistryLocation::Parse(CurrentSource().data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView());
        QApplication::clipboard()->setText(Location ? Location->Path() : navigation_->Path());
    }
    return true;
}
void CRegistryView::SetFlat(bool Flat)
{
    saveCursor(); rootProxy_->SetFlat(Flat); const QSignalBlocker Block(listMode_); listMode_->setCurrentIndex(Flat ? 0 : 1); restoreCursor();
    if (!Flat) { const auto Current = filterProxy_->mapFromSource(rootProxy_->mapFromSource(items_->Find(CurrentSource().data(RegistryTreeModel::IdRole).toString()))); if (Current.isValid()) itemsView_->expand(Current); }
}
void CRegistryView::CloseView() { if (search_) search_->Stop(); if (autoruns_) autoruns_->SetActive(false); if (service_) { service_->SetWatched({}); service_->Cancel(); } }
void CRegistryView::SetShown(bool Shown) { shown_ = Shown; if (!Shown) CloseView(); else { if (autorunMode_) autoruns_->SetActive(true); navigated(); Refresh(); } }
void CRegistryView::SetPanelActive(bool Active) { address_->SetPaneActive(Active); }
void CRegistryView::EnsureKeyboardFocusOnView() const { (searchMode_ ? searchGrid_ : autorunMode_ ? autorunGrid_ : policyMode_ ? policyGrid_ : historyMode_ ? historyGrid_ : itemsGrid_)->viewport()->setFocus(Qt::OtherFocusReason); }
void CRegistryView::ApplyColorScheme(const TyColorScheme& ColorScheme) { CViewT::ApplyColorScheme(ColorScheme); update(); }
QModelIndex CRegistryView::CurrentSource() const
{
    if (searchMode_) { const auto Row = searchView_->focusedRow(); return Row.isValid() ? searchFilter_->mapToSource(Row.modelIndex(0)) : QModelIndex(); }
    if (autorunMode_) { const auto Row = autorunView_->focusedRow(); return Row.isValid() ? autorunFilter_->mapToSource(Row.modelIndex(0)) : QModelIndex(); }
    if (policyMode_) { const auto Row = policyView_->focusedRow(); return Row.isValid() ? policyFilter_->mapToSource(Row.modelIndex(0)) : QModelIndex(); }
    if (historyMode_) { const auto Row = historyView_->focusedRow(); return Row.isValid() ? historyFilter_->mapToSource(Row.modelIndex(0)) : QModelIndex(); }
    if (sourceTree_) { const auto Row = treeView_->focusedRow(); return Row.isValid() ? Row.modelIndex(0) : QModelIndex(); }
    const auto Row = itemsView_->focusedRow(); return Row.isValid() ? rootProxy_->mapToSource(filterProxy_->mapToSource(Row.modelIndex(0))) : QModelIndex();
}
QModelIndexList CRegistryView::SelectedSources() const
{
    if (policyMode_ || autorunMode_ || searchMode_) return CurrentSource().isValid() ? QModelIndexList{CurrentSource()} : QModelIndexList{};
    if (sourceTree_ && !historyMode_) return CurrentSource().isValid() ? QModelIndexList{CurrentSource()} : QModelIndexList{};
    QModelIndexList Result;
    if (auto* Selection = itemsView_->selection())
        for (const auto& Index : Selection->selectedRowIndexes()) {
            const auto Source = rootProxy_->mapToSource(filterProxy_->mapToSource(Index.siblingAtColumn(0)));
            if (Source.isValid() && Source.data(RegistryTreeModel::KindRole).toInt() != int(RegistryTreeModel::Kind::Status)) Result.append(Source);
        }
    if (Result.isEmpty() && CurrentSource().isValid()) Result.append(CurrentSource());
    return Result;
}
bool CRegistryView::BeginRename()
{
    if (!CanExecuteCommand("hc.registry.rename")) return false;
    renameSource_ = CurrentSource(); renameTree_ = sourceTree_;
    const auto Visible = renameTree_ ? QModelIndex(renameSource_) : filterProxy_->mapFromSource(rootProxy_->mapFromSource(renameSource_));
    const auto Location = RegistryLocation::Parse(renameSource_.data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView());
    const auto Name = renameSource_.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value)
        ? renameSource_.data(RegistryTreeModel::ValueNameRole).toString() : Location->key.section('\\', -1);
    return (renameTree_ ? treeNameEditor_ : nameEditor_)->Begin(Visible, Name);
}
void CRegistryView::FinishRename(const QString& Error) { (renameTree_ ? treeNameEditor_ : nameEditor_)->Finish(Error); }
QString CRegistryView::PathText() const { return navigation_->Current() ? navigation_->Path() : tr("컴퓨터"); }
QString CRegistryView::SummaryText() const { if (searchMode_) return tr("찾기 결과 · %1개 · %2개 키 확인%3").arg(search_->Rows().size()).arg(search_->Status().scanned).arg(search_->Running() ? tr(" · 찾는 중…") : QString()); if (autorunMode_) return autoruns_->Loading() ? tr("자동 실행 · 불러오는 중…") : tr("자동 실행 · %1개 항목").arg(autorunView_->getRowCount()); if (policyMode_) return policies_->Loading() ? tr("적용된 정책 · 불러오는 중…") : tr("적용된 정책 · %1개 항목").arg(policyView_->getRowCount()); if (historyMode_) return tr("변경 기록 · %1개").arg(historyFilter_->rowCount()); return tr("레지스트리 · %1개 항목").arg(filterProxy_->rowCount()); }
QString CRegistryView::StatusLeft() const { return SummaryText(); }
QString CRegistryView::StatusRight() const { return SelectedRegistryView() == View::Native64 ? tr("64비트 보기") : tr("32비트 보기"); }
int CRegistryView::VisibleRowCount() const { return (searchMode_ ? searchView_ : searchMode_ ? searchView_ : autorunMode_ ? autorunView_ : policyMode_ ? policyView_ : historyMode_ ? historyView_ : itemsView_)->getRowCount(); }
void CRegistryView::saveCursor()
{
    if (historyMode_ || policyMode_ || autorunMode_ || searchMode_) return;
    if (navigation_->Pending() || restoringState_ || retainState_ || modelBatchDepth_) return;
    // Qtitan clears its selection during a sort before sortingChanged arrives.
    // Preserve the last stable ID snapshot across that notification window.
    const auto* Rules = itemsView_->modelController()->sortRules();
    const auto* Rule = Rules && Rules->sortRulesCount() ? Rules->sortRule(0) : nullptr;
    const int Column = Rule && Rule->dataBinding ? Rule->dataBinding->column() : 0;
    const auto Order = Rule && Rule->order == Qtitan::SortDescending ? Qt::DescendingOrder : Qt::AscendingOrder;
    if (sortColumn_ >= 0 && (Column != sortColumn_ || Order != sortOrder_)) return;
    itemState_ = RegistryGridState::Capture(itemsView_); treeState_ = RegistryGridState::Capture(treeView_); cursorId_ = itemState_.cursor;
}
void CRegistryView::restoreCursor()
{
    if (historyMode_ || policyMode_ || autorunMode_ || searchMode_) return;
    if (restoringState_ || navigation_->Pending() || modelBatchDepth_) return;
    const QScopedValueRollback<bool> Restoring(restoringState_, true), Sync(syncing_, true);
    const bool TreeReady = treeState_.Restore(treeView_, [this](const QString& Id) { return keys_->Find(Id); });
    const bool ItemsReady = itemState_.Restore(itemsView_, [this](const QString& Id) { return filterProxy_->mapFromSource(rootProxy_->mapFromSource(items_->Find(Id))); });
    if (TreeReady && ItemsReady) retainState_ = false;
    updateWatched();
}
void CRegistryView::applySort()
{
    const auto* Rules = itemsView_->modelController()->sortRules();
    const auto* Rule = Rules && Rules->sortRulesCount() ? Rules->sortRule(0) : nullptr;
    const int Column = Rule && Rule->dataBinding ? Rule->dataBinding->column() : 0;
    const auto Order = Rule && Rule->order == Qtitan::SortDescending ? Qt::DescendingOrder : Qt::AscendingOrder;
    if (Column == sortColumn_ && Order == sortOrder_) return;
    const QScopedValueRollback<bool> Retain(retainState_, true);
    sortColumn_ = Column; sortOrder_ = Order; items_->SetSort(Column, Order);
}
void CRegistryView::updateWatched()
{
    if (!shown_ || !navigation_->Current()) { service_->SetWatched({}); return; }
    QList<RegistryLocation> Locations{*navigation_->Current(), RegistryFavorites::SharedLocation()};
    for (int I = 0; I < itemsView_->getRowCount() && Locations.size() < 16; ++I) {
        const auto Row = itemsView_->getRow(I);
        if (!Row.isValid() || !itemsView_->modelController()->isRowExpanded(Row)) continue;
        const auto Key = RegistryLocation::Parse(Row.modelIndex(0).data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView());
        if (Key && !Locations.contains(*Key)) Locations.append(*Key);
    }
    service_->SetWatched(Locations);
}
QJsonObject CRegistryView::CaptureState() const
{
    auto State = navigation_->CaptureState(); State["kind"] = "registry"; State["flat"] = rootProxy_->Flat(); State["treeVisible"] = !treeGrid_->isHidden();
    State["showHistory"] = historyMode_; State["showPolicies"] = policyMode_; State["showAutoruns"] = autorunMode_; State["hideMicrosoft"] = findChild<QCheckBox*>("hc.registry.hideMicrosoft")->isChecked();
    State["cursor"] = RegistryGridState::Capture(itemsView_).cursor; State["splitter"] = QString::fromLatin1(splitter_->saveState().toBase64());
    State["items"] = (retainState_ ? itemState_ : RegistryGridState::Capture(itemsView_)).ToJson();
    State["keys"] = (retainState_ ? treeState_ : RegistryGridState::Capture(treeView_)).ToJson();
    State["filter"] = FilterText(); State["knownOnly"] = filterProxy_->KnownOnly();
    QJsonObject Find{{"text", findQuery_->text()}, {"type", findType_->currentIndex()},
        {"time", findTime_->currentIndex()}, {"scope", findScope_->currentIndex()}, {"visible", !findBar_->isHidden()}};
    for (const auto* Name : {"keys", "names", "data", "whole", "regex"})
        Find[Name] = findBar_->findChild<QCheckBox*>("hc.registry.find." + QString::fromLatin1(Name))->isChecked();
    State["find"] = Find;
    State["sortColumn"] = sortColumn_; State["sortDescending"] = sortOrder_ == Qt::DescendingOrder; return State;
}
void CRegistryView::RestoreState(const QJsonObject& State)
{
    ++navigationRevision_;
    const auto Find = State["find"].toObject(); findQuery_->setText(Find["text"].toString());
    findType_->setCurrentIndex(std::clamp(Find["type"].toInt(), 0, 3));
    findTime_->setCurrentIndex(std::clamp(Find["time"].toInt(), 0, 4));
    findScope_->setCurrentIndex(std::clamp(Find["scope"].toInt(), 0, 1));
    for (const auto* Name : {"keys", "names", "data", "whole", "regex"})
        if (Find.contains(Name)) findBar_->findChild<QCheckBox*>("hc.registry.find." + QString::fromLatin1(Name))->setChecked(Find[Name].toBool());
    findBar_->setVisible(Find["visible"].toBool());
    SetFlat(State["flat"].toBool(true)); treeGrid_->setVisible(State["treeVisible"].toBool(true));
    splitter_->restoreState(QByteArray::fromBase64(State["splitter"].toString().toLatin1()));
    itemState_ = RegistryGridState::FromJson(State["items"].toObject()); treeState_ = RegistryGridState::FromJson(State["keys"].toObject());
    if (itemState_.cursor.isEmpty()) itemState_.cursor = State["cursor"].toString();
    cursorId_ = itemState_.cursor; retainState_ = true;
    SetFilter(State["filter"].toString());
    if (auto* Known = findChild<QCheckBox*>("hc.registry.knownOnly")) Known->setChecked(State["knownOnly"].toBool());
    const auto Column = std::clamp(State["sortColumn"].toInt(), 0, 3);
    static_cast<Qtitan::GridTableColumn*>(itemsView_->getColumnByModelColumn(Column))->setSortOrder(State["sortDescending"].toBool() ? Qtitan::SortDescending : Qtitan::SortAscending);
    navigation_->RestoreState(State);
    ShowHistory(State["showHistory"].toBool());
    if (State["showPolicies"].toBool()) ShowPolicies();
    findChild<QCheckBox*>("hc.registry.hideMicrosoft")->setChecked(State["hideMicrosoft"].toBool());
    if (State["showAutoruns"].toBool()) ShowAutoruns();
    // Retry unloaded IDs on subsequent batches, without a wall-clock deadline.
    // Explicit navigation/input abandons this pending restoration.
}
void CRegistryView::fitColumns()
{
    if (!itemsView_) return;
    const int Available = std::max(400, itemPages_->width() - 24);
    if (searchView_) for (int I = 0; I < 4; ++I) static_cast<Qtitan::GridTableColumn*>(searchView_->getColumnByModelColumn(I))->setWidth(Available / 4);
    const int PolicyWeights[]{30, 15, 12, 43};
    if (policyView_) for (int I = 0; I < 4; ++I) static_cast<Qtitan::GridTableColumn*>(policyView_->getColumnByModelColumn(I))->setWidth(Available * PolicyWeights[I] / 100);
    const int AutoWeights[]{25, 15, 20, 40};
    if (autorunView_) for (int I = 0; I < 4; ++I) static_cast<Qtitan::GridTableColumn*>(autorunView_->getColumnByModelColumn(I))->setWidth(Available * AutoWeights[I] / 100);
    const int HistoryWeights[]{20, 18, 34, 28};
    for (int I = 0; I < 4; ++I) static_cast<Qtitan::GridTableColumn*>(historyView_->getColumnByModelColumn(I))->setWidth(Available * HistoryWeights[I] / 100);
    const int Weights[]{27, 18, 30, 25};
    for (int I = 0; I < 4; ++I) static_cast<Qtitan::GridTableColumn*>(itemsView_->getColumnByModelColumn(I))->setWidth(Available * Weights[I] / 100);
}
void CRegistryView::updateHeaderGeometry()
{
    if (!headerContent_ || !headerScroll_) return;
    // Keep the grid usable at high DPI / short windows. The fm scroll surface
    // preserves every control instead of allowing wrapped rows to overlap.
    const int Width = headerScroll_->viewport()->width();
    const int Natural = std::max(0, headerContent_->layout()->totalHeightForWidth(Width));
    if (headerContent_->minimumHeight() != Natural) headerContent_->setMinimumHeight(Natural);
    const int Height = std::min(Natural + 2, std::max(72, height() - 100));
    if (headerScroll_->height() != Height) headerScroll_->setFixedHeight(Height);
}
void CRegistryView::resizeEvent(QResizeEvent* Event) { CViewT::resizeEvent(Event); updateHeaderGeometry(); fitColumns(); }
bool CRegistryView::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == headerContent_ && (Event->type() == QEvent::Resize || Event->type() == QEvent::LayoutRequest))
        updateHeaderGeometry();
    // A nested height-for-width layout can otherwise be compressed to its
    // single-line minimum while the expanding grid consumes the remainder.
    if (Watched == findBar_ && (Event->type() == QEvent::Resize || Event->type() == QEvent::LayoutRequest || Event->type() == QEvent::Show)) {
        const int Height = findBar_->layout()->totalHeightForWidth(findBar_->width());
        if (Height >= 0 && findBar_->minimumHeight() != Height) findBar_->setMinimumHeight(Height);
    }
    if (auto* Input = qobject_cast<QWidget*>(Watched); Input && findBar_ && findBar_->isAncestorOf(Input) && Event->type() == QEvent::KeyPress) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if (Key->key() == Qt::Key_Escape) { findBar_->hide(); EnsureKeyboardFocusOnView(); return true; }
        return CViewT::eventFilter(Watched, Event);
    }
    if (Event->type() == QEvent::ContextMenu) {
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        fm::ui::Menu Menu(this); Menu.setObjectName("hc.registry.context");
        const auto Add = [&](QMenu* Target, const char* Id) {
            if (auto* Command = Bridge->Action(Id)) {
                auto* Item = Target->addAction(Command->text()); Item->setEnabled(Command->isEnabled());
                connect(Item, &QAction::triggered, this, [this, Id] { emit sigCommandRequested(QString::fromLatin1(Id)); });
            }
        };
        if (autorunMode_) Add(&Menu, "hc.registry.toggleAutorun");
        Add(&Menu, "hc.registry.open"); Add(&Menu, "hc.registry.edit"); Add(&Menu, "hc.registry.describe"); Add(&Menu, "hc.registry.follow");
        Menu.addSeparator(); Add(&Menu, "hc.registry.rename"); Add(&Menu, "hc.registry.newKey"); Add(&Menu, "hc.registry.newValue"); Add(&Menu, "hc.registry.delete");
        Menu.addSeparator(); auto* Copy = new fm::ui::Menu(tr("복사"), &Menu); Menu.addMenu(Copy);
        for (const auto* Id : {"hc.registry.copyPath", "hc.registry.copyName", "hc.registry.copyData", "hc.registry.copyReg"}) Add(Copy, Id);
        Add(&Menu, "hc.registry.importPreview"); Add(&Menu, "hc.registry.export"); Add(&Menu, "hc.registry.compare"); Add(&Menu, "hc.registry.favorite");
        Menu.addSeparator(); Add(&Menu, "hc.registry.undo"); Add(&Menu, "hc.registry.redo");
        Menu.exec(static_cast<QContextMenuEvent*>(Event)->globalPos()); return true;
    }
    if (Event->type() == QEvent::MouseButtonPress || Event->type() == QEvent::KeyPress) retainState_ = false;
    if (Event->type() == QEvent::MouseButtonRelease) QTimer::singleShot(0, this, [this] { saveCursor(); updateWatched(); });
    if (Event->type() == QEvent::FocusIn || Event->type() == QEvent::MouseButtonPress) {
        sourceTree_ = Watched == treeGrid_ || Watched == treeGrid_->viewport();
        emit sig_NotifyViewActivated(); emit sigCursorChanged();
    }
    if (Event->type() == QEvent::KeyPress) {
        auto* Key = static_cast<QKeyEvent*>(Event);
        const auto Modifiers = Key->modifiers() & ~Qt::KeypadModifier;
        if ((Key->key() == Qt::Key_Menu && Modifiers == Qt::NoModifier) || (Key->key() == Qt::Key_F10 && Modifiers == Qt::ShiftModifier)) {
            auto* Grid = sourceTree_ ? treeGrid_ : searchMode_ ? searchGrid_ : autorunMode_ ? autorunGrid_ : policyMode_ ? policyGrid_ : historyMode_ ? historyGrid_ : itemsGrid_;
            const auto Position = Grid->viewport()->rect().center();
            QContextMenuEvent Context(QContextMenuEvent::Keyboard, Position, Grid->viewport()->mapToGlobal(Position));
            QCoreApplication::sendEvent(Grid->viewport(), &Context); return true;
        }
        const bool InTree = Watched == treeGrid_ || Watched == treeGrid_->viewport();
        const bool Plain = Modifiers == Qt::NoModifier || (Key->key() == Qt::Key_Plus && Modifiers == Qt::ShiftModifier);
        if ((InTree || policyMode_ || historyMode_ || autorunMode_ || searchMode_ || !rootProxy_->Flat()) && Plain && (Key->key() == Qt::Key_Left || Key->key() == Qt::Key_Right || Key->key() == Qt::Key_Plus || Key->key() == Qt::Key_Minus)) {
            auto* View = InTree ? treeView_ : searchMode_ ? searchView_ : autorunMode_ ? autorunView_ : policyMode_ ? policyView_ : historyMode_ ? historyView_ : itemsView_; const auto Row = View->focusedRow();
            if (!Row.isValid()) return true;
            const auto Index = Row.modelIndex(0); const bool Open = View->modelController()->isRowExpanded(Row);
            const bool Children = View->modelController()->hasChildren(Row);
            if (Key->key() == Qt::Key_Plus || (Key->key() == Qt::Key_Right && !Open)) {
                if (Children) View->expandRow(Row);
            } else if (Key->key() == Qt::Key_Minus || (Key->key() == Qt::Key_Left && Open)) {
                if (Open) View->collapseRow(Row);
            } else {
                const auto Target = Key->key() == Qt::Key_Right ? Index.model()->index(0, 0, Index) : Index.parent();
                const auto Next = View->getRow(Target);
                if (Next.isValid()) { View->setFocusedRowIndex(Next.rowIndex()); View->scrollToIndex(Target, false); }
            }
            saveCursor(); updateWatched(); return true;
        }
        const auto ScopeKeys = TyStCommandMgr::GetInstance()->RetrieveForScope(hc::ui::InputScope::RegistryView);
        const auto Command = hc::ui::ShortcutCommand(*Key, ScopeKeys);
        if (!Command.isEmpty()) {
            if (Command == "hc.registry.open" && (Watched == treeGrid_ || Watched == treeGrid_->viewport())) {
                const auto Row = treeView_->focusedRow(); const auto Location = keys_->Location(Row.isValid() ? Row.modelIndex(0) : QModelIndex());
                if (Location) Navigate(Location->Path());
            } else emit sigCommandRequested(Command);
            // Even unavailable scoped commands consume the key; a hidden file
            // model must never receive delete, copy, undo or rename by fallback.
            return true;
        }
        if (Key->modifiers() == Qt::NoModifier && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter)) {
            const auto Index = CurrentSource();
            if (Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Key)) { const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), navigation_->SelectedView()); if (Location) Navigate(Location->Path()); }
            else emit sigCommandRequested("hc.registry.edit");
            return true;
        }
        if (Key->modifiers() == Qt::NoModifier && Key->key() == Qt::Key_Backspace) { Up(); return true; }
        const auto Global = hc::ui::ShortcutCommand(*Key, TyStCommandMgr::GetInstance()->Retrieve());
        if (!Global.isEmpty()) {
            if (hc::ui::IsSystemViewGlobalCommand(Global)) TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(Key, {});
            return true;
        }
    }
    return CViewT::eventFilter(Watched, Event);
}

void CRegistryView::expandAutorunGroups()
{
    if (!autorunMode_) return;
    for (int Row = 0; Row < autorunFilter_->rowCount(); ++Row) autorunView_->expand(autorunFilter_->index(Row, 0));
    emit sigSummaryChanged();
}
void CRegistryView::ShowAutoruns(bool Show)
{
    if (searchMode_) ShowSearch(false);
    if (Show) { if (policyMode_) ShowPolicies(false); if (historyMode_) ShowHistory(false); }
    if (autorunMode_ == Show) { if (Show) autoruns_->Reload(); return; }
    ++navigationRevision_; if (Show) saveCursor(); autorunMode_ = Show;
    itemPages_->setCurrentWidget(Show ? autorunGrid_ : itemsGrid_);
    { const QSignalBlocker Block(modes_); modes_->setCurrentIndex(Show ? 2 : 0); }
    findChild<QCheckBox*>("hc.registry.hideMicrosoft")->setVisible(Show);
    findChild<QCheckBox*>("hc.registry.knownOnly")->setEnabled(!Show);
    bits_->setEnabled(!Show); listMode_->setVisible(!Show);
    if (Show) { autorunFilter_->setFilterFixedString(filter_->text()); autoruns_->SetActive(true); }
    else { autoruns_->SetActive(false); filterProxy_->SetText(filter_->text()); restoreCursor(); }
    fitColumns(); emit sigSummaryChanged(); emit sigCursorChanged();
}
std::optional<RegistryAutorunEntry> CRegistryView::CurrentAutorun() const
{
    return autorunMode_ ? autoruns_->Entry(CurrentSource().data(RegistryTreeModel::IdRole).toString()) : std::nullopt;
}


void CRegistryView::setupSearch(QVBoxLayout* Layout)
{
    search_ = new RegistrySearch(this); searchFilter_ = new RegistryPolicyFilter(this);
    searchFilter_->setSourceModel(search_->Model());
    searchGrid_ = new Qtitan::TreeGrid(itemPages_); searchGrid_->setObjectName("hc.registry.searchGrid"); itemPages_->addWidget(searchGrid_);
    searchGrid_->setViewType(Qtitan::TreeGrid::TreeView); searchView_ = searchGrid_->view<Qtitan::GridTreeView>();
    configureGrid(searchGrid_, searchView_, searchFilter_, false);
    connect(searchView_, &Qtitan::GridViewBase::focusRowChanged, this, &CRegistryView::sigCursorChanged);
    connect(searchView_, &Qtitan::GridViewBase::rowDblClicked, this, [this] { emit sigCommandRequested("hc.registry.open"); });
    const auto SearchState = std::make_shared<RegistryGridState>();
    connect(search_, &RegistrySearch::BatchStarting, this, [this, SearchState] {
        *SearchState = RegistryGridState::Capture(searchView_); searchView_->beginUpdate();
    });
    connect(search_, &RegistrySearch::BatchFinished, this, [this, SearchState] {
        searchView_->endUpdate(); expandSearchGroups();
        if (!SearchState->cursor.isEmpty()) SearchState->Restore(searchView_, [this](const QString& Id) {
            return searchFilter_->mapFromSource(search_->Model()->Find(Id));
        });
    });
    connect(search_, &RegistrySearch::Changed, this, [this] {
        if (!searchMode_) return;
        expandSearchGroups();
        const auto& Status = search_->Status();
        QString Message = SummaryText();
        if (!Status.error.isEmpty()) Message += "\n" + Status.error;
        if (Status.limited) Message += "\n" + tr("결과 보관 한도에 도달했습니다. 범위를 좁혀 다시 찾으세요.");
        if (Status.denied) Message += "\n" + tr("읽지 못한 키 %1개").arg(Status.denied);
        if (!search_->Running() && !Status.done) Message += tr(" · 일시 중지");
        notice_->setText(Message); emit sigSummaryChanged();
    });
    findBar_ = new QWidget(this); findBar_->setObjectName("hc.registry.findBar"); findBar_->installEventFilter(this);
    auto* Rows = new QVBoxLayout(findBar_); Rows->setContentsMargins(4, 4, 4, 4);
    Rows->addWidget(new fm::ui::Label(tr("레지스트리에서 찾기"), fm::ui::Label::Body, findBar_));
    auto* Flow = new fm::ui::FlowLayout(4, 4); Rows->addLayout(Flow);
    findQuery_ = new fm::ui::FindBox(findBar_); findQuery_->setObjectName("hc.registry.findQuery"); findQuery_->setMinimumWidth(220);
    Flow->addWidget(findQuery_); findQuery_->installEventFilter(this);
    const QStringList Labels{tr("키"), tr("값 이름"), tr("데이터"), tr("전체 일치"), tr("정규식")};
    const QStringList Names{"keys", "names", "data", "whole", "regex"};
    for (int I = 0; I < Names.size(); ++I) {
        auto* Check = new fm::ui::WrapCheckBox(Labels[I], findBar_); Check->setObjectName("hc.registry.find." + Names[I]); Check->setChecked(I < 3); Flow->addWidget(Check);
    }
    findType_ = new fm::ui::ChoiceBox(findBar_); findType_->addItems({tr("모든 형식"), tr("문자열"), tr("숫자"), tr("이진")}); Flow->addWidget(findType_);
    findTime_ = new fm::ui::ChoiceBox(findBar_); findTime_->addItems({tr("시각 제한 없음"), tr("최근 1시간"), tr("오늘"), tr("최근 7일"), tr("최근 30일")}); Flow->addWidget(findTime_);
    findScope_ = new fm::ui::ChoiceBox(findBar_); findScope_->addItems({tr("현재 키 아래"), tr("전체")}); Flow->addWidget(findScope_);
    const auto Add = [&](const QString& Text, const QString& Name, auto Handler) {
        auto* Button = new fm::ui::Button(Text, findBar_); Button->setCompact(true); Button->setObjectName(Name);
        Flow->addWidget(Button); connect(Button, &QPushButton::clicked, this, Handler);
    };
    Add(tr("찾기"), "hc.registry.find.start", [this] { startSearch(); });
    Add(tr("중지"), "hc.registry.find.stop", [this] { search_->Stop(); });
    Add(tr("계속 찾기"), "hc.registry.find.continue", [this] { search_->Continue(); });
    Add(tr("치환…"), "hc.registry.find.replace", [this] { emit sigCommandRequested("hc.registry.replaceResults"); });
    Add(tr("결과 내보내기…"), "hc.registry.find.export", [this] { emit sigCommandRequested("hc.registry.exportResults"); });
    Add(tr("닫기"), "hc.registry.find.close", [this] { findBar_->hide(); EnsureKeyboardFocusOnView(); });
    connect(findQuery_, &QLineEdit::returnPressed, this, &CRegistryView::startSearch);
    for (auto* Input : findBar_->findChildren<QWidget*>()) Input->installEventFilter(this);
    findTime_->setAccessibleName(tr("키 마지막 쓴 시각"));
    findTime_->setToolTip(tr("값별 시각이 아닌, 값이 속한 키의 마지막 쓴 시각으로 찾습니다."));
    Layout->addWidget(findBar_); findBar_->hide();
}
void CRegistryView::OpenFind()
{
    findBar_->show(); updateHeaderGeometry(); findQuery_->setFocus(Qt::ShortcutFocusReason); findQuery_->selectAll();
    QTimer::singleShot(0, this, [this] { headerScroll_->ensureWidgetVisible(findQuery_); });
}
void CRegistryView::startSearch()
{
    RegistrySearchOptions Options; Options.text = findQuery_->text(); Options.callerSid = QString::fromStdWString(hc::system::CurrentUserSid());
    Options.keys = findBar_->findChild<QCheckBox*>("hc.registry.find.keys")->isChecked();
    Options.names = findBar_->findChild<QCheckBox*>("hc.registry.find.names")->isChecked();
    Options.data = findBar_->findChild<QCheckBox*>("hc.registry.find.data")->isChecked();
    Options.whole = findBar_->findChild<QCheckBox*>("hc.registry.find.whole")->isChecked();
    Options.regex = findBar_->findChild<QCheckBox*>("hc.registry.find.regex")->isChecked();
    Options.type = RegistrySearchOptions::Type(findType_->currentIndex());
    if (findScope_->currentIndex() == 0 && navigation_->Current()) Options.roots = {*navigation_->Current()};
    else for (auto Root : {Hive::ClassesRoot, Hive::CurrentUser, Hive::LocalMachine, Hive::Users, Hive::CurrentConfig})
        Options.roots.append(RegistryLocation{Root, {}, navigation_->SelectedView()});
    if (findTime_->currentIndex()) {
        auto Since = QDateTime::currentDateTime();
        switch (findTime_->currentIndex()) {
        case 1: Since = Since.addSecs(-3600); break;
        case 2: Since = Since.date().startOfDay(); break;
        case 3: Since = Since.addDays(-7); break;
        default: Since = Since.addDays(-30); break;
        }
        Options.since = quint64(Since.toMSecsSinceEpoch() + 11644473600000LL) * 10000;
    }
    ShowSearch(); search_->Start(std::move(Options));
}
void CRegistryView::ShowSearch(bool Show)
{
    if (Show) { if (autorunMode_) ShowAutoruns(false); if (policyMode_) ShowPolicies(false); if (historyMode_) ShowHistory(false); }
    if (searchMode_ == Show) return;
    if (Show) saveCursor(); searchMode_ = Show; ++navigationRevision_;
    if (Show) modes_->button(4)->show();
    itemPages_->setCurrentWidget(Show ? searchGrid_ : itemsGrid_);
    { const QSignalBlocker Block(modes_); modes_->setCurrentIndex(Show ? 4 : 0); }
    bits_->setEnabled(!Show); listMode_->setVisible(!Show); findChild<QCheckBox*>("hc.registry.knownOnly")->setEnabled(!Show);
    if (Show) { searchFilter_->setFilterFixedString(filter_->text()); expandSearchGroups(); }
    else { filterProxy_->SetText(filter_->text()); restoreCursor(); }
    fitColumns(); emit sigSummaryChanged(); emit sigCursorChanged();
}
void CRegistryView::expandSearchGroups()
{
    if (!searchMode_) return;
    for (int Row = 0; Row < searchFilter_->rowCount(); ++Row) searchView_->expand(searchFilter_->index(Row, 0));
}
std::optional<RegistryResultRow> CRegistryView::CurrentSearchResult() const
{
    return searchMode_ ? search_->Model()->Row(CurrentSource()) : std::nullopt;
}