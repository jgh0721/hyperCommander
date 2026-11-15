#pragma once
#include "UniqueLibs/registry/registryImport.hpp"
#include <fmwidgets/ContentDialog.h>
#include <functional>
namespace hc::registry { class RegistryDescriptions; }
namespace fm::ui { class Label; class ChoiceBox; class MultilineField; class ContentStack; }
class QCheckBox;
namespace Qtitan { class TreeGrid; class GridTreeView; }
class RegistryImportDialog final : public fm::ui::ContentDialog
{
public:
    RegistryImportDialog(QString Path, hc::registry::RegistryDescriptions* Descriptions, QWidget* Parent = nullptr);
    ~RegistryImportDialog() override;
    const hc::registry::RegistryImportPreview& Preview() const { return preview_; }
    QByteArray SourceHash() const { return hash_; }
    QString SourcePath() const { return input_.path.isEmpty() ? path_ : input_.path; }
    const hc::registry::RegistryImportSource& Source() const { return input_; }
    hc::registry::RegistryImportLoader loadSource;
    std::function<void(const hc::registry::RegistryLocation&)> openKey;
    std::function<void(const QString&)> openEditor;
private:
    void reload(bool ReadFile);
    void render();
    QString path_;
    hc::registry::RegistryImportSource input_;
    QByteArray bytes_, hash_;
    hc::registry::RegistryImportPreview preview_;
    hc::registry::RegistryDescriptions* descriptions_;
    hc::registry::RegistryResultModel* model_;
    Qtitan::TreeGrid* grid_;
    Qtitan::GridTreeView* view_;
    fm::ui::Label* notice_;
    fm::ui::ChoiceBox* bits_;
    fm::ui::ChoiceBox* encoding_;
    fm::ui::ChoiceBox* classes_;
    fm::ui::MultilineField* source_;
    QCheckBox* backup_;
    QSet<int> excluded_;
    std::shared_ptr<std::atomic_bool> cancel_;
    quint64 generation_ = 0;
    bool tree_ = false, loading_ = false;
};