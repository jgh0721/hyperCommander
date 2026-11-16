#pragma once
#include "UniqueLibs/system/environmentProfiles.hpp"
#include "cmpView.hpp"
#include "UniqueLibs/system/environmentSession.hpp"
#include "UniqueLibs/system/environmentModel.hpp"
#include "UniqueLibs/registry/registryHistoryModel.hpp"
#include "commonLibs/system/processEnvironment.hpp"
#include <QPointer>
class QSplitter;
class QStandardItemModel;
class QSortFilterProxyModel;
namespace Qtitan { class TreeGrid; class GridTreeView; }
namespace fm::ui { class FindBox; class Label; class SegmentedControl; class BreadcrumbBar; class WrapCheckBox; }
class CEnvironmentView final : public CViewT
{
    Q_OBJECT
public:
    explicit CEnvironmentView(QWidget* Parent = nullptr, hc::environment::EnvironmentSession* Session = nullptr);
    ~CEnvironmentView() override;
    void ApplyColorScheme(const TyColorScheme& Colors) override;
    void EnsureKeyboardFocusOnView() const override;
    void CloseView() override;
    void SetPanelActive(bool Active);
    void SetShown(bool Shown);
    void Refresh();
    void SetScope(hc::environment::EnvironmentViewScope Scope, bool Remember = true);
    hc::environment::EnvironmentViewScope Scope() const { return scope_; }
    hc::environment::EnvironmentSession* Session() const { return session_; }
    hc::environment::EnvironmentModel* Model() const { return model_; }
    const hc::environment::EnvironmentModel::Row* Current() const;
    QModelIndex CurrentSource() const;
    void Select(const QString& Name, std::optional<int> Entry = {});
    void FocusFilter();
    void SetFilter(const QString& Text);
    QString FilterText() const;
    void ToggleTree();
    void Back();
    void Forward();
    bool CanBack() const { return historyPosition_ > 0; }
    bool CanForward() const { return historyPosition_ + 1 < scopes_.size(); }
    void SetProcess(const hc::system::ProcessKey& Key, const hc::environment::ProcessEnvironment& Process);
    std::optional<hc::system::ProcessKey> ProcessKey() const { return processKey_; }
    void SetProfile(QString Id, const std::vector<hc::environment::Value>& Values);
    const hc::environment::ValueMap& OtherEnvironment() const { return other_; }
    const hc::environment::ValueMap& ProcessValues() const { return processValues_; }
    const std::optional<std::wstring>& ProcessDirectory() const { return processDirectory_; }
    QString ProfileId() const { return profileId_; }
    void SetProfiles(QList<hc::environment::EnvironmentProfile> Profiles);
    std::optional<hc::environment::EnvironmentProfile> CurrentProfile() const;
    void SetHistory(QList<hc::registry::RegistryHistoryRecord> Records);
    hc::registry::RegistryHistoryModel* History() const { return historyModel_; }
    std::optional<hc::registry::RegistryHistoryRecord> CurrentHistory() const;
    std::optional<hc::registry::RegistryStep> CurrentHistoryStep() const;
    std::shared_ptr<hc::environment::IPathProbe> PathProbe() const { return pathProbe_; }
    bool ExecuteCommand(const QString& Command);
    bool CanExecuteCommand(const QString& Command) const;
    QString PathText() const;
    QString SummaryText() const;
    QString StatusLeft() const { return SummaryText(); }
    QString StatusRight() const;
    QJsonObject CaptureState() const;
    void RestoreState(const QJsonObject& State);
    void ShowNotice(const QString& Text);
signals:
    void sigCursorChanged();
    void sigSummaryChanged();
    void sigCommandRequested(const QString& Command);
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void rebuild();
    void restoreItems();
    void analyzePaths();
    void configure(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View, QAbstractItemModel* Model, bool Tree);
    std::shared_ptr<hc::environment::IPathProbe> pathProbe_;
    quint64 pathRevision_ = 0;
    std::shared_ptr<std::atomic_bool> pathCancel_;
    QHash<QString, std::vector<hc::environment::PathEntry>> pathResults_;
    QPointer<hc::environment::EnvironmentSession> session_;
    hc::environment::EnvironmentModel* model_;
    QSortFilterProxyModel* filterProxy_;
    QStandardItemModel* scopeModel_;
    hc::registry::RegistryHistoryModel* historyModel_;
    Qtitan::TreeGrid* treeGrid_;
    Qtitan::TreeGrid* itemsGrid_;
    Qtitan::TreeGrid* historyGrid_;
    Qtitan::GridTreeView* treeView_;
    Qtitan::GridTreeView* itemsView_;
    Qtitan::GridTreeView* historyView_;
    QSplitter* splitter_;
    fm::ui::FindBox* filter_;
    fm::ui::Label* notice_;
    fm::ui::SegmentedControl* modes_;
    fm::ui::BreadcrumbBar* address_;
    hc::environment::EnvironmentViewScope scope_ = hc::environment::EnvironmentViewScope::User;
    hc::environment::ValueMap other_, processValues_;
    std::optional<hc::system::ProcessKey> processKey_;
    QString profileId_, processError_, pendingSelection_;
    QList<hc::environment::EnvironmentProfile> profiles_;
    std::optional<std::wstring> processDirectory_;
    QVector<hc::environment::EnvironmentViewScope> scopes_;
    QVector<QJsonObject> visits_;
    QJsonObject pendingItems_;
    int historyPosition_ = -1;
    bool expanded_ = false, shown_ = true, active_ = false, syncing_ = false;
};
