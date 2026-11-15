#pragma once
/// 고유 라이브러리
#include "UniqueLibs/registry/registryPolicies.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QWidget>
#include <functional>

class QVBoxLayout;
namespace fm::ui { class SegmentedControl; class Label; class Button; }
namespace hc::registry {
class RegistryPolicyEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RegistryPolicyEditor(QWidget* Parent = nullptr);
    bool SetPolicy(const RegistryPolicyEntry& Entry);
    bool IsModified() const;
    PolicyState State() const;
    QVariantMap Options() const;
    const RegistryPolicyEntry& Entry() const { return entry_; }
    void SetBusy(bool Busy);
    void ShowError(const QString& Error);
    void Discard();
signals:
    void SaveRequested();
    void CancelRequested();
    void ModifiedChanged(bool Modified);
private:
    void changed();
    void rebuild();
    RegistryPolicyEntry entry_;
    QVBoxLayout* layout_;
    QWidget* form_ = nullptr;
    QWidget* options_ = nullptr;
    fm::ui::SegmentedControl* states_ = nullptr;
    fm::ui::Label* error_;
    fm::ui::Button* save_;
    fm::ui::Button* cancel_;
    QHash<QString, std::function<QVariant()>> readers_;
    QVariantMap originalOptions_;
    bool loaded_ = false, loading_ = false, busy_ = false;
};
} // namespace hc::registry
