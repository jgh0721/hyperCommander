#pragma once

/// 고유 라이브러리
#include "UniqueLibs/ui/pathListEditor.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QWidget>

class QStackedWidget;
namespace fm::ui { class TextField; class MultilineField; class UnsignedNumberEdit; class HexBufferEditor; class DataInterpreter; class Label; class Button; }
namespace hc::registry {
// A draft owns its original bytes. Selection changes cannot silently overwrite
// it: SetValue refuses while modified/busy, until the caller resolves the draft.
class RegistryValueEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RegistryValueEditor(QWidget* Parent = nullptr);
    bool SetValue(const RegistryValue& Value, bool PathValue = false);
    RegistryResult<RegistryValue> Draft() const;
    bool IsModified() const;
    void SetBusy(bool Busy);
    void SetPathResolver(hc::ui::PathResolver Resolver);
    void SetExpandedText(const QString& Expanded, const QString& Warning = {});
    void Discard();
    void AcceptSaved(const RegistryValue& Saved);
    void ShowError(const QString& Error);
    void FocusEditor();
signals:
    void SaveRequested(const hc::registry::RegistryValue& Original, const hc::registry::RegistryValue& Draft);
    void CancelRequested();
    void ModifiedChanged(bool Modified);
    void ExpansionRequested(const QString& Raw);
private:
    void changed();
    RegistryValue original_;
    QStackedWidget* pages_;
    fm::ui::TextField* text_;
    fm::ui::MultilineField* multi_;
    fm::ui::UnsignedNumberEdit* number_;
    fm::ui::HexBufferEditor* hex_;
    fm::ui::DataInterpreter* interpreter_;
    hc::ui::PathListEditor* path_;
    fm::ui::Label* heading_;
    fm::ui::Label* warning_;
    fm::ui::Label* expanded_;
    fm::ui::Button* save_;
    fm::ui::Button* cancel_;
    enum Page { Text, Multi, Number, Binary, Path } page_ = Binary;
    bool loading_ = false, busy_ = false, loaded_ = false, pathValue_ = false;
};
}
