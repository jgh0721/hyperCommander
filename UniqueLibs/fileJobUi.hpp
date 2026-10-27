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
class QGridLayout;
class QBoxLayout;
namespace fm::ui { class TransferGraph; class DialogHeader; }
namespace hc::ui { class FileJobInteraction; }

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
    using PreparedRetry = std::function<hc::storage::Result<hc::operations::PreparedJob>(const hc::operations::JobResult&, hc::storage::OperationContext)>;
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::PreparedJob Job, const QString& Label,
        PreparedRetry Retry = {}, std::function<void(const std::string&)> Resubmitted = {});
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
    void updateProgressView(const hc::operations::JobSnapshot& Snapshot);
    void layoutProgressFooter();
    void fitProgressDialog();
    void loadResultPage(const std::string& Id);
    void answer(hc::operations::InteractionChoice Choice);
    void updateInteraction(const hc::operations::JobSnapshot* Snapshot);
    void replyInteraction(const std::string& Id, const std::string& Request, hc::operations::InteractionChoice Choice, const QString& Value);
    void showInteractionDetails(const hc::operations::JobSnapshot& Snapshot);
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
    QProgressBar* currentProgress_ = nullptr;
    QWidget *sourceBlock_ = nullptr, *destinationBlock_ = nullptr, *currentBlock_ = nullptr;
    QWidget* phaseBlock_ = nullptr;
    QWidget *axisBlock_ = nullptr, *pagesBlock_ = nullptr;
    QLabel *sourcePath_ = nullptr, *sourceName_ = nullptr, *destinationPath_ = nullptr, *destinationName_ = nullptr;
    QLabel *currentTitle_ = nullptr, *currentPercent_ = nullptr, *currentMetrics_ = nullptr;
    QLabel *overallTitle_ = nullptr, *overallPercent_ = nullptr, *overallMetrics_ = nullptr;
    QPushButton* graphToggle_ = nullptr;
    QComboBox* graphAxis_ = nullptr;
    QWidget *footerLeft_ = nullptr, *footerRight_ = nullptr;
    QGridLayout* progressFooter_ = nullptr;
    QBoxLayout* metricsLayout_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPointer<hc::ui::FileJobInteraction> interaction_;
    std::string interactionPasswordRequest_;
    std::string repliedInteraction_;
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
    fm::ui::TransferGraph* graph_ = nullptr;
    QPushButton* managerPause_ = nullptr;
    QPushButton* managerCancel_ = nullptr;
    QPushButton* managerStart_ = nullptr;
    QPushButton* managerRetry_ = nullptr;
    QPushButton* queueUp_ = nullptr;
    QPushButton* queueDown_ = nullptr;
    QSpinBox* concurrency_ = nullptr;
    std::map<std::string, hc::operations::FileOperationRequest> retryRequests_;
    struct PreparedRetryRecord { PreparedRetry build_; std::function<void(const std::string&)> submitted_; };
    std::map<std::string, PreparedRetryRecord> preparedRetries_;
    std::map<std::string, hc::storage::OperationContext> retryContexts_;
    QPushButton* managerOpen_ = nullptr;
    QPushButton* indicator_ = nullptr;
    QCheckBox* autoClose_ = nullptr;
    bool detailed_ = false;
    bool appliedShowRecords_ = false;
    QLabel* notice_ = nullptr;
    std::string graphId_;
    qint64 graphElapsed_ = -1;
    qint64 graphTotal_ = -1;
    struct Sample { qint64 elapsed_; qint64 bytes_; bool paused_; };
    std::map<std::string, std::vector<Sample>> samples_;
    std::map<std::string, int> sampleKinds_;
    std::map<std::string, hc::operations::JobPhase> samplePhases_;
    std::map<std::string, QString> endpoints_;
    std::map<std::string,QJsonObject> sizeFormats_;
    QPushButton* previousPage_ = nullptr;
    QPushButton* nextPage_ = nullptr;
    uint64_t resultOffset_ = 0, pageGeneration_ = 0;
    std::string resultPageId_;
    uint64_t loadedOffset_ = UINT64_MAX;
    std::vector<hc::operations::ItemOutcome> resultPage_;
    bool pageLoading_ = false;
    bool passwordDialogOpen_ = false;
    std::map<std::string, std::stop_source> passwordScopes_;
};
