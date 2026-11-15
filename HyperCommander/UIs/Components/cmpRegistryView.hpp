#pragma once
#include <fmwidgets/ScrollPanel.h>

/// UIs
#include "cmpView.hpp"
#include "registryGridState.hpp"
/// 고유 라이브러리
#include "UniqueLibs/registry/registryNavigation.hpp"
#include "UniqueLibs/registry/registryRootProxy.hpp"
#include "UniqueLibs/registry/registryFilterProxy.hpp"
#include "UniqueLibs/registry/registryHistoryModel.hpp"
#include "UniqueLibs/registry/registryDescriptions.hpp"
#include "UniqueLibs/registry/registryPolicies.hpp"
#include "UniqueLibs/registry/registryAutoruns.hpp"
#include "UniqueLibs/registry/registrySearch.hpp"

namespace fm::ui { class BreadcrumbBar; class Button; class SegmentedControl; class FindBox; class Label; class ChoiceBox; }
namespace Qtitan { class TreeGrid; class GridTreeView; }
class TreeBranchStyle;
class QSplitter;
class QVBoxLayout;
class QStackedWidget;
class QSortFilterProxyModel;
class RegistryNameOverlay;
class RegistryPolicyFilter;

class CRegistryView final : public CViewT
{
    Q_OBJECT
public:
    explicit CRegistryView(QWidget* Parent = nullptr);
    ~CRegistryView() override;
    void ApplyColorScheme(const TyColorScheme& ColorScheme) override;
    void EnsureKeyboardFocusOnView() const override;
    void CloseView() override;
    void SetPanelActive(bool Active);
    void SetShown(bool Shown);
    bool Navigate(const QString& Path, std::optional<QString> ValueName = {});
    void SetRegistryView(hc::registry::View SelectedView);
    hc::registry::View SelectedRegistryView() const { return autorunMode_ && CurrentAutorun() ? CurrentAutorun()->location.view : policyMode_ ? hc::registry::View::Native64 : navigation_->SelectedView(); }
    quint64 NavigationRevision() const { return navigationRevision_; }
    void Refresh();
    void ShowHistory(bool Show = true);
    void ShowPolicies(bool Show = true);
    void ShowAutoruns(bool Show = true);
    bool AutorunsShown() const { return autorunMode_; }
    std::optional<hc::registry::RegistryAutorunEntry> CurrentAutorun() const;
    hc::registry::RegistryAutoruns* Autoruns() const { return autoruns_; }
    bool PoliciesShown() const { return policyMode_; }
    std::optional<hc::registry::RegistryPolicyEntry> CurrentPolicy() const;
    hc::registry::RegistryPolicies* Policies() const { return policies_; }
    void ReloadHistory();
    bool HistoryShown() const { return historyMode_; }
    hc::registry::RegistryHistoryModel* History() const { return history_; }
    std::optional<hc::registry::RegistryHistoryRecord> CurrentHistory() const;
    void SetFlat(bool Flat);
    void ToggleTree();
    void EditAddress();
    void Back();
    void Forward();
    void Up();
    bool CanBack() const { return navigation_->CanBack(); }
    bool CanForward() const { return navigation_->CanForward(); }
    void SetFilter(const QString& Text);
    QString FilterText() const;
    void FocusFilter();
    void OpenFind();
    void ShowSearch(bool Show = true);
    bool SearchShown() const { return searchMode_; }
    hc::registry::RegistrySearch* Search() const { return search_; }
    std::optional<hc::registry::RegistryResultRow> CurrentSearchResult() const;
    bool ExecuteCommand(const QString& Command);
    bool CanExecuteCommand(const QString& Command) const;
    QString PathText() const;
    QString SummaryText() const;
    QString StatusLeft() const;
    QString StatusRight() const;
    QJsonObject CaptureState() const;
    void RestoreState(const QJsonObject& State);
    QModelIndex CurrentSource() const;
    QModelIndexList SelectedSources() const;
    bool BeginRename();
    void FinishRename(const QString& Error = {});
    int VisibleRowCount() const;
    hc::registry::RegistryDescriptions* Descriptions() const { return descriptions_; }
    hc::registry::RegistryService* Service() const { return service_; }
signals:
    void sigRenameRequested(const QModelIndex& Source, const QString& Name);
    void sigCursorChanged();
    void sigSummaryChanged();
    void sigCommandRequested(const QString& Command);
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
    void resizeEvent(QResizeEvent* Event) override;
private:
    void updateHeaderGeometry();
    QWidget* headerContent_ = nullptr;
    fm::ui::ScrollPanel* headerScroll_ = nullptr;
    void setupSearch(QVBoxLayout* Layout);
    void startSearch();
    void expandSearchGroups();
    hc::registry::RegistrySearch* search_ = nullptr;
    RegistryPolicyFilter* searchFilter_ = nullptr;
    Qtitan::TreeGrid* searchGrid_ = nullptr;
    Qtitan::GridTreeView* searchView_ = nullptr;
    QWidget* findBar_ = nullptr;
    fm::ui::FindBox* findQuery_ = nullptr;
    fm::ui::ChoiceBox* findType_ = nullptr;
    fm::ui::ChoiceBox* findTime_ = nullptr;
    fm::ui::ChoiceBox* findScope_ = nullptr;
    bool searchMode_ = false;
    void expandPolicyGroups();
    void expandAutorunGroups();
    hc::registry::RegistryAutoruns* autoruns_ = nullptr;
    RegistryPolicyFilter* autorunFilter_ = nullptr;
    Qtitan::TreeGrid* autorunGrid_ = nullptr;
    Qtitan::GridTreeView* autorunView_ = nullptr;
    bool autorunMode_ = false;
    void configureGrid(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View, QAbstractItemModel* Model, bool Tree);
    void navigated();
    void fitColumns();
    void saveCursor();
    void restoreCursor();
    void applySort();
    void updateWatched();
    hc::registry::RegistryDescriptions* descriptions_ = nullptr;
    hc::registry::RegistryService* service_ = nullptr;
    hc::registry::RegistryKeyTreeModel* keys_ = nullptr;
    hc::registry::RegistryItemsModel* items_ = nullptr;
    hc::registry::RegistryRootProxy* rootProxy_ = nullptr;
    hc::registry::RegistryFilterProxy* filterProxy_ = nullptr;
    hc::registry::RegistryNavigation* navigation_ = nullptr;
    Qtitan::TreeGrid* treeGrid_ = nullptr;
    Qtitan::TreeGrid* itemsGrid_ = nullptr;
    Qtitan::GridTreeView* treeView_ = nullptr;
    Qtitan::GridTreeView* itemsView_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QStackedWidget* itemPages_ = nullptr;
    Qtitan::TreeGrid* historyGrid_ = nullptr;
    Qtitan::GridTreeView* historyView_ = nullptr;
    hc::registry::RegistryHistoryModel* history_ = nullptr;
    QSortFilterProxyModel* historyFilter_ = nullptr;
    fm::ui::SegmentedControl* modes_ = nullptr;
    bool historyMode_ = false, policyMode_ = false;
    hc::registry::RegistryPolicies* policies_ = nullptr;
    Qtitan::TreeGrid* policyGrid_ = nullptr;
    Qtitan::GridTreeView* policyView_ = nullptr;
    RegistryPolicyFilter* policyFilter_ = nullptr;
    fm::ui::BreadcrumbBar* address_ = nullptr;
    fm::ui::FindBox* filter_ = nullptr;
    fm::ui::ChoiceBox* bits_ = nullptr;
    fm::ui::SegmentedControl* listMode_ = nullptr;
    fm::ui::Label* notice_ = nullptr;
    fm::ui::Button* back_ = nullptr;
    fm::ui::Button* forward_ = nullptr;
    RegistryNameOverlay* nameEditor_ = nullptr;
    RegistryNameOverlay* treeNameEditor_ = nullptr;
    bool sourceTree_ = false, renameTree_ = false;
    QPersistentModelIndex renameSource_;
    QString cursorId_;
    QString permissionNotice_;
    RegistryGridState itemState_, treeState_, autorunState_;
    bool restoringState_ = false, retainState_ = false;
    quint64 navigationRevision_ = 0;
    int modelBatchDepth_ = 0;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    bool shown_ = true, syncing_ = false;
};
