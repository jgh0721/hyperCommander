#pragma once
#include <QtWidgets>
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"

// A settings draft: this widget never mutates the live view or the repository.
class QtColumnProfileEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit QtColumnProfileEditor(QWidget* Parent = nullptr);
    void Load(const hc::columns::ColumnProfile& Profile, const QJsonObject& Library, bool Writable = true);
    const hc::columns::ColumnProfile& Profile() const { return draft_; }
    QJsonObject Library() const;
    bool Dirty() const { return dirty_; }
    bool NewTabDefault() const;
    bool ValidateDraft();
    void SetContextFields(QVector<hc::columns::FieldDefinition> Fields);
    void MarkSaved() { dirty_ = false; }
protected:
    void resizeEvent(QResizeEvent* Event) override;
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void rebuildList(int Row);
    void loadColumn(int Row);
    void storeColumn();
    void updatePreview();
    void cloneProfile();
    void refreshProfiles();
    QVector<hc::columns::FieldDefinition> availableFields() const;
    QVector<hc::columns::FieldDefinition> contextFields_;
    void refreshSorts(int Row = 0);
    void refreshFields();
    void refreshScripts(int Row = 0);
    void loadScript();
    void storeScript();
    QComboBox* scripts_;
    QLineEdit *scriptTitle_, *dependencies_;
    QPlainTextEdit *scriptSource_, *scriptOutputs_;
    QCheckBox* scriptEnabled_;
    hc::columns::ColumnProfile draft_;
    QJsonObject library_;
    bool loading_ = false;
    bool dirty_ = false;
    bool writable_ = true;
    int row_ = -1;
    QComboBox *profiles_, *scope_, *layout_, *field_, *alignment_, *sizeFormat_, *dateFormat_, *unit_, *widthMode_, *sort_, *sortOrder_;
    QLineEdit *profileTitle_, *title_, *template_;
    QComboBox* sortField_;
    QListWidget* sortRules_;
    QSpinBox* precision_;
    QDoubleSpinBox *width_, *minimum_, *maximum_;
    QCheckBox *primary_, *visible_;
    QListWidget* columns_;
    QTableWidget* preview_ = nullptr;
    QLabel* error_;
    QPushButton* deleteProfile_;
};
