#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QPointer>
#include <QVariant>
#include <functional>
#include <atomic>
#include <memory>
#include "registryImport.hpp"
namespace fm::ui { class Button; }

class CmpPanel;
class CRegistryView;
class CRegistryDetailView;
class CommandUiBridge;
class QMainWindow;
namespace hc::registry {
struct RegistryLocation;
struct RegistryValue;
class RegistryMutation;
struct RegistryMutationResult;
class RegistryCommands final : public QObject
{
    Q_OBJECT
public:
    using PanelAccessor = std::function<CmpPanel*()>;
    RegistryCommands(QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active);
    static bool Handles(const QString& Command);
    bool Execute(const QString& Command, const QVariant& Argument = {});
    CRegistryView* Open(const QVariant& Argument = {});
    void RefreshState();
    void OpenImport(const QString& Path, std::shared_ptr<void> KeepAlive = {}, std::function<void()> Editor = {}, std::function<bool()> SourceUnchanged = {}, RegistryImportLoader Loader = {});
    CRegistryView* ActiveView() const;
    bool DetailOpen() const;
    bool Active() const;
    void ToggleDetail();
    bool CloseDetail();
    bool Busy() const { return busy_; }
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void exportData(CRegistryView* View, bool Results);
    void compare(CRegistryView* View);
    int compareProcesses_ = 0;
    void favorite(CRegistryView* View);
    void rename(CRegistryView* View, const QModelIndex& Index, const QString& Name);
    void create(CRegistryView* View, bool Key);
    void remove(CRegistryView* View);
    void undoRedo(CRegistryView* View, bool Redo);
    void runMutation(CRegistryView* View, std::function<RegistryMutationResult(RegistryMutation&)> Work,
        std::function<void(CRegistryView*, const QString&)> Done = {});
    CmpPanel* currentDetailPanel() const;
    CRegistryDetailView* openDetail(CRegistryView* View);
    void savePolicy(CRegistryDetailView* Detail);
    void saveValue(CRegistryDetailView* Detail, const RegistryLocation& Location, const RegistryValue& Original, const RegistryValue& Draft);
    QPointer<QMainWindow> main_;
    CommandUiBridge* bridge_;
    PanelAccessor active_;
    std::shared_ptr<std::atomic_bool> policyCancel_;
    fm::ui::Button* policyCancelButton_ = nullptr;
    bool busy_ = false;
};
} // namespace hc::registry
