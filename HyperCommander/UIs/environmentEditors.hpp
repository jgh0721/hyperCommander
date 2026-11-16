#pragma once
#include "UniqueLibs/system/environmentStorage.hpp"
#include "UniqueLibs/ui/pathListEditor.hpp"
#include <QWidget>
class QStackedWidget;
namespace fm::ui { class TextField; class ChoiceBox; class PathEdit; class ExpandableValueEdit; class MaskedValueEdit; class OrderedTokenEdit; class Label; class Button; }
namespace hc::environment {
class EnvironmentValueEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit EnvironmentValueEditor(QWidget* Parent = nullptr);
    bool SetValue(const Value& Original, bool ReadOnly = false, bool Masked = false, const QStringList& KnownValues = {}, bool FolderHint = false);
    Value Draft() const;
    bool IsModified() const;
    void SetBusy(bool Busy);
    void SetVariableNames(const QStringList& Names);
    void SetResolver(hc::ui::PathResolver Resolver);
    void Discard();
    void AcceptSaved(const Value& Saved);
    void FocusEditor();
    void ShowError(const QString& Error);
signals:
    void SaveRequested(const hc::environment::Value& Original, const hc::environment::Value& Draft);
    void CancelRequested();
    void ModifiedChanged(bool Modified);
    void ExpansionRequested(const QString& Raw);
private:
    enum Page { Text, Folder, Expanded, List, Tokens, Known, Masked };
    void changed();
    void resolveFolder();
    void setDraft(const QString& Raw);
    Page page_ = Text;
    Value original_;
    bool loaded_ = false, loading_ = false, readOnly_ = false, busy_ = false;
    QStackedWidget* pages_;
    fm::ui::TextField* text_;
    fm::ui::PathEdit* folder_;
    fm::ui::ExpandableValueEdit* expanded_;
    hc::ui::PathListEditor* list_;
    fm::ui::OrderedTokenEdit* tokens_;
    fm::ui::ChoiceBox* known_;
    fm::ui::MaskedValueEdit* masked_;
    fm::ui::ChoiceBox* type_;
    fm::ui::Label* message_;
    fm::ui::Label* folderStatus_;
    QTimer folderTimer_;
    hc::ui::PathResolver resolver_;
    quint64 folderGeneration_ = 0;
    fm::ui::Button* save_;
};
}
