#pragma once

/// UIs
#include "cmpView.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
#include "UniqueLibs/registry/registryLinks.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPointer>

class CRegistryView;
class QStackedWidget;
namespace fm::ui { class Label; class SegmentedControl; class Button; }
namespace hc::registry { class RegistryValueEditor; class RegistryPolicyEditor; class RegistryLinkPane; }
class CRegistryDetailView final : public CViewT
{
    Q_OBJECT
public:
    explicit CRegistryDetailView(QWidget* Parent = nullptr);
    void SetSource(CRegistryView* Source);
    CRegistryView* Source() const;
    void Update();
    bool RequestClose();
    void CloseView() override;
    void EnsureKeyboardFocusOnView() const override;
    void ShowData(bool Focus = true);
    void ShowDescription();
    void ShowLinks(bool Follow = false);
    void SetBusy(bool Busy);
    void PolicySaveFinished(const QString& Error = {});
    hc::registry::RegistryPolicyEditor* PolicyEditor() const { return policyEditor_; }
    void SaveFinished(const hc::registry::RegistryValue& Value, const QString& Error = {});
    hc::registry::RegistryValueEditor* Editor() const { return editor_; }
    const std::optional<hc::registry::RegistryLocation>& Location() const { return location_; }
    void SetDescription(const QString& Text);
    void SetLinks(const QString& Text);
signals:
    void PolicySaveRequested();
    void PolicyCancelRequested();
    void LinkRequested(const hc::registry::RegistryLink& Link);
    void SaveRequested(const hc::registry::RegistryLocation& Location, const hc::registry::RegistryValue& Original,
        const hc::registry::RegistryValue& Draft);
private:
    QPointer<CRegistryView> source_;
    std::optional<hc::registry::RegistryLocation> location_;
    fm::ui::Label* title_;
    fm::ui::Label* subtitle_;
    fm::ui::Label* notice_;
    fm::ui::Label* description_;
    hc::registry::RegistryLinkPane* links_;
    fm::ui::Label* keyInfo_;
    fm::ui::SegmentedControl* tabs_;
    QStackedWidget* pages_;
    QStackedWidget* dataPages_;
    hc::registry::RegistryPolicyEditor* policyEditor_;
    hc::registry::RegistryValueEditor* editor_;
    QString shownId_;
    quint64 expansionGeneration_ = 0;
    bool busy_ = false;
};
