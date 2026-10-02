#pragma once
#include <QtWidgets>
#include "columnPreview.hpp"
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/columnProfileStorage.hpp"

namespace Qtitan { class Grid; }
class QtitanColumnAdapter;

// A settings draft: this widget never mutates the live view or the repository.
class QtColumnProfileEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit QtColumnProfileEditor(QWidget* Parent = nullptr);
    ~QtColumnProfileEditor() override;
    QSize sizeHint() const override { return {760, 580}; }
    void Load(const hc::columns::ColumnProfile& Profile, const QJsonObject& Library, bool Writable = true);
    const hc::columns::ColumnProfile& Profile() const { return draft_; }
    QJsonObject Library() const;
    bool Dirty() const { return dirty_; }
    bool NewTabDefault() const;
    bool ApplyCurrent() const { return scope_->currentIndex() != 3; }
    bool BothPanels() const { return scope_->currentIndex() == 2; }
    bool ValidateDraft();
    void SetPreviewWidth(int Width) { previewWidth_->setValue(Width); }
    void SetSamples(QVector<hc::columns::PreviewSample> Current, QVector<hc::columns::PreviewSample> Selected);
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
    void layoutPreview();
    void startPreview();
    void stopPreview();
    void loadManualSamples();
    hc::columns::CellValue readSample(const hc::columns::PreviewSample& Sample, const QString& Field);
    QVector<hc::columns::PreviewSample> currentSamples_, selectedSamples_, manualSamples_;
    std::unique_ptr<hc::columns::QueryEngine> previewQueries_;
    QByteArray previewSignature_;
    bool calculating_ = false, previewComputed_ = false;
    quint64 manualGeneration_ = 0;
    std::stop_source manualStop_;
    QString previewRetry_;
    QComboBox *sampleMode_, *fieldCategory_;
    QLineEdit *samplePath_, *fieldSearch_;
    QLabel *sampleStatus_, *valueDetail_, *fieldInfo_;
    QCheckBox *extractSamples_, *grouping_, *plusSign_, *utc_, *providerDisplay_;
    QVector<hc::columns::PreviewSample> previewSamples() const;
    hc::columns::QueryEngine::Loader previewLoader() const;
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
    QTableWidget* preview_ = nullptr; // Owns the item model, rendered by the product Qtitan view.
    Qtitan::Grid* previewGrid_ = nullptr;
    QtitanColumnAdapter* previewAdapter_ = nullptr;
    QScrollArea* previewFrame_ = nullptr;
    QSpinBox* previewWidth_ = nullptr;
    QLabel* error_;
    QPushButton* deleteProfile_;
};
