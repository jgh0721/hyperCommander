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
    QSize sizeHint() const override { return {1040, 640}; }
    QSize minimumSizeHint() const override { return {760, 480}; }
    void Load(const hc::columns::ColumnProfile& Profile, const QJsonObject& Library, bool Writable = true);
    const hc::columns::ColumnProfile& Profile() const { return draft_; }
    QJsonObject Library() const;
    bool Dirty() const { return dirty_; }
    struct ApplyTargets { bool current_, opposite_, defaultThis_, defaultOpposite_, automatic_, sync_; };
    ApplyTargets Targets() const;
    bool TargetsTouched() const { return targetsTouched_; }
    void SetPanes(const QString& This, const QString& Opposite);
    void SetSyncCount(int Count) { syncCount_ = Count; updateApplySummary(); }
    bool ValidateDraft();
    void LoadAutoSelection(const QJsonObject& Policy, const QJsonObject& Colors, bool AutomaticTab);
    QJsonObject AutoSelection() const;
    void SetAutoColors(const QJsonObject& Colors);
    void SetCurrentLocation(const QString& Path) { currentLocation_ = Path; refreshAutoVerdict(); }
    bool AutomaticTab() const { return applyAuto_->isChecked(); }
    void SetPreviewWidth(int Width) { previewWidth_->setValue(Width); }
    void SetSamples(QVector<hc::columns::PreviewSample> Current, QVector<hc::columns::PreviewSample> Selected);
    void SetContextFields(QVector<hc::columns::FieldDefinition> Fields);
    void MarkSaved() { dirty_ = targetsTouched_ = false; updateApplySummary(); }
signals:
    void ProfileSelected();
protected:
    void resizeEvent(QResizeEvent* Event) override;
    bool eventFilter(QObject* Watched, QEvent* Event) override;
private:
    void rebuildList(int Row);
    void loadColumn(int Row);
    void storeColumn();
    void refreshColumnItem(int Row);
    void selectPreviewColumn(const QUuid& Id);
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
    bool updatingPreview_ = false;
    QVector<QUuid> previewColumnIds_;
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
    void updateApplySummary();
    bool targetsTouched_ = false;
    int syncCount_ = 0;
    QUuid currentProfileId_;
    QCheckBox *applyCurrent_, *applyOpposite_, *applyDefaultThis_, *applyDefaultOpposite_, *applySync_;
    QRadioButton *applyAuto_, *applyFixed_;
    QLabel *applySummary_, *applyTitle_;
    void appendAutoRule(const QJsonObject& Rule);
    enum AutoColumn { AutoEnabled, AutoProfile, AutoPath, AutoResolved, AutoGroup, AutoPercent };
    void refreshAutoVerdict();
    QString currentLocation_;
    QLabel* autoVerdict_ = nullptr;
    QTimer autoResolveTimer_;
    QVector<hc::columns::FieldDefinition> availableFields() const;
    QVector<hc::columns::FieldDefinition> contextFields_;
    void refreshSorts(int Row = 0);
    void loadGrouping();
    void storeGrouping();
    QComboBox *groupColumn_ = nullptr, *groupMode_ = nullptr, *groupOrder_ = nullptr;
    QButtonGroup* groupExpand_ = nullptr;
    QCheckBox* hideGroupedColumn_ = nullptr;
    QCheckBox *groupCount_ = nullptr, *groupSize_ = nullptr, *groupLatest_ = nullptr, *groupsHeader_ = nullptr;
    QVector<QComboBox*> groupColumns_, groupModes_, groupOrders_;
    QVector<QWidget*> extraGroupRows_;
    QPushButton* addGroup_ = nullptr;
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
    QComboBox *layout_, *field_, *alignment_, *sizeFormat_, *dateFormat_, *unit_, *widthMode_, *sort_, *sortOrder_;
    QLineEdit *profileTitle_, *title_, *template_;
    QComboBox* sortField_;
    QListWidget* sortRules_;
    QSpinBox* precision_;
    QDoubleSpinBox *width_, *minimum_, *maximum_;
    QCheckBox *primary_, *visible_, *splitNameExtension_;
    QCheckBox *autoSelection_;
    QTableWidget* autoRules_;
    QJsonObject autoPolicy_, autoColors_;
    QListWidget* columns_;
    QListWidget* profileList_ = nullptr;
    QTabWidget* pages_ = nullptr;
    QFormLayout* contentForm_ = nullptr;
    QWidget *templateRow_ = nullptr, *formatRow_ = nullptr;
    QLabel *columnTitle_ = nullptr, *summary_ = nullptr, *precisionLabel_ = nullptr;
    QTableWidget* preview_ = nullptr; // Owns the item model, rendered by the product Qtitan view.
    Qtitan::Grid* previewGrid_ = nullptr;
    QtitanColumnAdapter* previewAdapter_ = nullptr;
    QScrollArea* previewFrame_ = nullptr;
    QSpinBox* previewWidth_ = nullptr;
    QLabel* error_;
    QPushButton* deleteProfile_;
};
