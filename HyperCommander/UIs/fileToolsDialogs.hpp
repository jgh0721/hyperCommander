#pragma once
#include "dialogShell.hpp"
#include "UniqueLibs/fileBatchOperations.hpp"
#include "UniqueLibs/batchRenameJournal.hpp"
#include "UniqueLibs/renameJobController.hpp"
#include <QtWidgets>
#include <QFutureWatcher>
#include <functional>
#include "commonLibs/storage/entryIndex.hpp"
namespace fm::ui { class RenameComparisonView; }
namespace hc::ui {
class NewFileDialog final : public DialogShell {
public:
    explicit NewFileDialog(const QString& Folder, bool Supported, QWidget* Parent = nullptr);
    QString CreatedPath() const { return createdPath_; }
    bool OpenEditor() const;
    QString EditorProgram() const;
    QStringList EditorArguments() const;
    void SetProvider(std::shared_ptr<storage::IWritableStorage> Writer, storage::Locator Location,
        std::function<storage::Result<operations::JobHandle>(operations::PreparedJob)> Submit,
        std::function<void(const std::string&)> Cancel);
protected:
    void done(int Result) override;
    void accept() override;
    void reject() override;
    void closeEvent(QCloseEvent* Event) override;
private:
    void validate();
    QString folder_, createdPath_;
    bool supported_, busy_ = false;
    int type_ = 0;
    QLineEdit* name_;
    QCheckBox *bom_, *open_;
    QWidget* controls_;
    QComboBox* editor_;
    std::shared_ptr<storage::IWritableStorage> writer_;
    storage::Locator location_;
    std::function<storage::Result<operations::JobHandle>(operations::PreparedJob)> submit_;
    std::function<void(const std::string&)> cancel_;
    std::string jobId_;
};
class BatchRenameDialog final : public DialogShell {
public:
    explicit BatchRenameDialog(const QStringList& Paths, bool Supported, QWidget* Parent = nullptr,
        std::shared_ptr<storage::ISelectionManifest> Selection = {}, filetools::RenameJobController* Jobs = nullptr);
    ~BatchRenameDialog() override;
    std::function<void()> onChanged_;
protected:
    void accept() override;
    void reject() override;
    void closeEvent(QCloseEvent* Event) override;
private:
    void preview();
    void showPage();
    void loadDates();
    void done(int Result) override;
    void run(bool Undo);
    void finish(const filetools::BatchResult& Result, const QList<filetools::RenameRow>& Plan, bool Undo);
    filetools::Rules rules() const;
    QList<filetools::Entry> sources_;
    QList<filetools::RenameRow> plan_, undo_;
    QSet<QString> existing_;
    QLineEdit *mask_, *extension_, *find_, *replace_, *date_;
    QCheckBox *regex_, *sensitive_, *includeExtension_;
    QComboBox *nameCase_, *extensionCase_, *preset_, *dateSource_;
    QSpinBox *start_, *step_, *digits_, *dateOffset_;
    QWidget* controls_;
    QPushButton *undoButton_, *redoButton_;
    QLabel* summary_;
    QLabel* pageLabel_;
    QPushButton *previousPage_, *nextPage_;
    int page_ = 0;
    quint64 generation_ = 0;
    std::stop_source scanStop_, planStop_, metadataStop_;
    filetools::RenameJobController* jobs_;
    QPointer<QObject> jobOwner_;
    QString recordId_, pendingRecordId_;
    std::string jobId_;
    QCheckBox* queued_;
    QPushButton* background_;
    bool cancelWhenReady_ = false;
    bool metadataLoading_ = false;
    QTimer* previewTimer_;
    fm::ui::RenameComparisonView* comparison_;
    bool ready_ = false, busy_ = false, undone_ = false, allowExecute_ = true;
};
class RenameHistoryDialog final : public DialogShell {
public:
    explicit RenameHistoryDialog(filetools::RenameJobController* Jobs, QWidget* Parent = nullptr);
private:
    void reload();
    void select();
    void submit(int Action);
    bool active(const QString& Id) const;
    std::optional<operations::JobSnapshot> activeJob(const QString& Id) const;
    filetools::RenameJobController* jobs_;
    QPointer<QObject> jobOwner_;
    QTreeWidget* list_;
    fm::ui::RenameComparisonView* comparison_;
    QPushButton *undo_, *resume_, *restore_, *refresh_;
    QCheckBox* queued_;
    QLabel* selectionSummary_;
    QList<filetools::RenameRecord> records_;
    quint64 generation_ = 0;
    bool loading_ = false;
};
}
