#pragma once
/// 고유 라이브러리
#include "UniqueLibs/registry/registryLinks.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QWidget>
#include <functional>
class QVBoxLayout;
namespace hc::registry {
class RegistryLinkPane final : public QWidget
{
    Q_OBJECT
public:
    explicit RegistryLinkPane(QWidget* Parent = nullptr);
    void SetSelection(const RegistryLocation& Location, std::optional<RegistryValue> Value);
    void SetMessage(const QString& Text);
    void Activate();
    void SetCacheDirectory(const QString& Directory) { links_->SetCacheDirectory(Directory); }
    void Follow();
    void SetNavigator(std::function<void(const RegistryLink&)> Navigate) { navigate_ = std::move(Navigate); }
protected:
    void hideEvent(QHideEvent* Event) override;
    void showEvent(QShowEvent* Event) override;
private:
    void render();
    RegistryLinks* links_;
    QVBoxLayout* layout_;
    std::optional<RegistryLocation> location_;
    std::optional<RegistryValue> value_;
    std::function<void(const RegistryLink&)> navigate_;
    bool active_ = false, follow_ = false;
    int displayLimit_ = 200;
};
}
