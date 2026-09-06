#pragma once
#include <QObject>
#include <QPointer>
#include "commonLibs/operations/archiveOperation.hpp"

class QWidget;
class QDialog;
class QComboBox;
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QTimer;

class FileJobUi : public QObject
{
public:
    FileJobUi(QWidget* Main, std::shared_ptr<hc::commands::EventHub> Events);
    ~FileJobUi() override;
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::FileOperationRequest Request,
        hc::storage::OperationContext Context);
    hc::storage::Result<hc::operations::JobHandle> Submit(hc::operations::ArchiveOperationRequest Request,
        hc::storage::OperationContext Context);
    hc::operations::JobScheduler& Scheduler() { return scheduler_; }
    void Show();
    std::function<void(const hc::operations::JobResult&)> onFinished_;
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    hc::storage::Result<hc::operations::JobHandle> track(hc::storage::Result<hc::operations::JobHandle> Submitted,
        hc::operations::FileOperation Operation, const std::string& Label);
    void updateJobs();
    void updateSelection();
    void answer(hc::operations::InteractionChoice Choice);
    std::string selectedId() const;
    hc::operations::JobScheduler scheduler_;
    std::map<std::string, hc::operations::JobHandle> pending_;
    std::map<std::string, hc::operations::JobResult> results_;
    QPointer<QWidget> main_;
    QDialog* dialog_ = nullptr;
    QComboBox* jobs_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QLineEdit* renamed_ = nullptr;
    QPushButton* pause_ = nullptr;
    QPushButton* cancel_ = nullptr;
    std::map<hc::operations::InteractionChoice, QPushButton*> choices_;
    QTimer* timer_ = nullptr;
    bool closing_ = false;
};
