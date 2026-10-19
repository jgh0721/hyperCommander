#pragma once
#include <QObject>
#include "permissionJobController.hpp"
#include "renameJobController.hpp"
#include <QPointer>
#include <QJsonObject>
#include "commonLibs/operations/moveOperation.hpp"

class QWidget;
class QDialog;
class QComboBox;
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QCheckBox;
class QSpinBox;
namespace fm::ui { class TransferGraph; class DialogHeader; }

class FileJobUi : public QObject, public hc::permissions::JobController, public hc::filetools::RenameJobController
{
public:
    FileJobUi(QWidget* Main, std::shared_ptr<hc::commands::EventHub> Events);
    ~FileJobUi() override;
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::FileOperationRequest Request,
        hc::storage::OperationContext Context, bool Deferred = false);
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::ArchiveOperationRequest Request,
        hc::storage::OperationContext Context, bool Deferred = false);
    hc::operations::JobScheduler& Scheduler() { return scheduler_; }
    bool IsUiPending(const std::string& Id) const { return pending_.contains(Id); }
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::PreparedJob Job, const QString& Label);
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::MoveOperationRequest Request,
        hc::storage::OperationContext Context, bool Deferred = false);
    std::shared_ptr<hc::operations::IFileElevationService> ElevationService() const;
    QString SubmitSecurityJob(const QString& Path,bool Grant,const QJsonObject& Restore={}) override;
    QObject* RenameOwner() override { return this; }
    hc::operations::JobScheduler& RenameScheduler() override { return scheduler_; }
    hc::storage::Result<hc::operations::JobHandle> SubmitRename(hc::operations::PreparedJob Job, const QString& Label) override { return Submit(std::move(Job), Label); }
    void Show();
    void ApplyDefaults();
    std::function<void(const std::string&, const hc::operations::FileOperationRequest&)> onResubmitted_;
    std::function<void(const hc::operations::JobResult&)> onFinished_;
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    hc::storage::Result<hc::operations::JobHandle> track(hc::storage::Result<hc::operations::JobHandle> Submitted,
        hc::operations::FileOperation Operation, const std::string& Label);
    void controlAll(hc::operations::JobControl Control);
    void reorderSelected(int Direction);
    void retrySelected();
    bool canRetry(const std::string& Id) const;
    double measuredRate(const hc::operations::JobSnapshot& Snapshot) const;
    bool deletionJob(const hc::operations::JobSnapshot& Snapshot) const;
    bool acceptsControl(const hc::operations::JobSnapshot& Snapshot, hc::operations::JobControl Control) const;
    void updateManagerSelection(const hc::operations::JobSnapshot* Snapshot);
    void updateJobs();
    void updateManager();
    void showProgress();
    void controlSelected(bool Pause);
    void updateGraph(const hc::operations::JobSnapshot& Snapshot);
    void recordSamples();
    void updateSelection();
    void loadResultPage(const std::string& Id);
    void answer(hc::operations::InteractionChoice Choice);
    std::string selectedId() const;
    std::shared_ptr<hc::operations::IJobJournal> journal_;
    hc::operations::JobScheduler scheduler_;
    std::map<std::string, hc::operations::JobHandle> pending_;
    std::map<std::string, QString> notifications_;
    int appliedConcurrency_ = 2;
    std::map<std::string, hc::operations::JobResult> results_;
    QPointer<QWidget> main_;
    QPointer<QObject> previousPermissionController_;
    QDialog* dialog_ = nullptr;
    QComboBox* jobs_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QLineEdit* renamed_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* cancel_ = nullptr;
    std::map<hc::operations::InteractionChoice, QPushButton*> choices_;
    QPushButton* permission_ = nullptr;
    QTimer* timer_ = nullptr;
    bool closing_ = false;
    QDialog* manager_ = nullptr;
    QTreeWidget* jobList_ = nullptr;
    QComboBox* filter_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel *queueSummary_ = nullptr, *batchSummary_ = nullptr, *selectionSummary_ = nullptr, *historySummary_ = nullptr;
    QPushButton* clearHistory_ = nullptr;
    bool clearingHistory_ = false;
    std::string preparingRetry_;
    QLabel* metrics_ = nullptr;
    fm::ui::DialogHeader* header_ = nullptr;
    fm::ui::TransferGraph* graph_ = nullptr;
    QPushButton* managerPause_ = nullptr;
    QPushButton* managerCancel_ = nullptr;
    QPushButton* managerStart_ = nullptr;
    QPushButton* managerRetry_ = nullptr;
    QPushButton* queueUp_ = nullptr;
    QPushButton* queueDown_ = nullptr;
    QSpinBox* concurrency_ = nullptr;
    std::map<std::string, hc::operations::FileOperationRequest> retryRequests_;
    std::map<std::string, hc::storage::OperationContext> retryContexts_;
    QPushButton* managerOpen_ = nullptr;
    QPushButton* indicator_ = nullptr;
    QCheckBox* autoClose_ = nullptr;
    bool detailed_ = true;
    std::string graphId_;
    qint64 graphElapsed_ = -1;
    qint64 graphTotal_ = -1;
    struct Sample { qint64 elapsed_; qint64 bytes_; bool paused_; };
    std::map<std::string, std::vector<Sample>> samples_;
    std::map<std::string, QString> endpoints_;
    std::map<std::string,QJsonObject> sizeFormats_;
    QPushButton* previousPage_ = nullptr;
    QPushButton* nextPage_ = nullptr;
    uint64_t resultOffset_ = 0, pageGeneration_ = 0;
    std::string resultPageId_;
    uint64_t loadedOffset_ = UINT64_MAX;
    std::vector<hc::operations::ItemOutcome> resultPage_;
    bool pageLoading_ = false;
};
