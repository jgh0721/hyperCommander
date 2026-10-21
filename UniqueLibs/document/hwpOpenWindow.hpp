#pragma once
#include "hwpProcessBackend.hpp"
#include <QFutureWatcher>
#include <functional>
#include <stop_token>
class QLabel;
class QPrinter;
class QPrintDialog;
namespace hc::document
{
    // Both installed entry points select the same fixed product runtime. No
    // environment override, parent search or package-local runtime fallback.
    WebProcessRuntime MakeProductHwpRuntime(const QString& ApplicationDirectory,
        QByteArray ApprovedInventorySha256);

    // One native open operation. Close through RequestClose and retain the window
    // until Closed; application Quit is deferred through preparation/retirement.
    class HwpOpenWindow final : public QWidget
    {
        Q_OBJECT
    public:
        using PrintDialogFactory = std::function<bool(QPrinter&, QWidget*)>;
        using BackendFactory = std::function<std::unique_ptr<HwpSdiBackend>()>;
        HwpOpenWindow(QString Path, QString CacheRoot, WebProcessRuntime Runtime,
            BackendFactory Factory = {}, bool Editable = false, bool OriginalWritable = true, PrintDialogFactory PrintDialog = {});
        ~HwpOpenWindow() override;
        void Open();
        bool SetRecovery(RecoverySnapshot Snapshot);
        void RequestClose();
        void Save(bool SaveAs = false);
        void Print();
        void SetPrintDialogFactory(PrintDialogFactory Factory) { printDialogFactory_ = std::move(Factory); }
        bool IsBusy() const { return preparing_ || cleaning_ || (closing_ && !closed_) || bool(printJob_) || (document_ && document_->IsBusy()); }
        QString Error() const { return error_; }
        bool IsClosed() const { return closed_; }
        bool IsClosing() const { return closing_; }
        bool IsPreparing() const { return preparing_; }
        HwpSdiWindow* Document() const { return document_; }
    signals:
        void StateChanged();
        void Closed();
    protected:
        void closeEvent(QCloseEvent* Event) override;
        bool eventFilter(QObject* Object, QEvent* Event) override;
    private:
        struct PrintJob;
        struct Prepared;
        std::shared_ptr<PrintJob> printJob_;
        QFutureWatcher<storage::Result<storage::Unit>> printing_;
        QPointer<QPrintDialog> printDialog_;
        PrintDialogFactory printDialogFactory_;
        bool closeAfterPrint_ = false;
        void finishPrint(storage::Result<storage::Unit> Result);
        struct SaveDestination;
        QFutureWatcher<std::shared_ptr<SaveDestination>> destinationPreparation_;
        std::shared_ptr<SaveDestination> saveDestination_;
        bool preparingDestination_ = false;
        void prepared();
        void cleanup();
        void finishClose();
        void fail(QString Error);
        QString path_, cacheRoot_, error_;
        WebProcessRuntime runtime_;
        BackendFactory factory_;
        std::optional<RecoverySnapshot> recovery_;
        std::stop_source stop_;
        QFutureWatcher<std::shared_ptr<Prepared>> preparation_;
        QFutureWatcher<QString> cleanup_;
        std::shared_ptr<Prepared> resources_;
        QLabel* status_;
        HwpSdiWindow* document_ = nullptr;
        bool editable_ = false, originalWritable_ = false, saveThenClose_ = false;
        QFutureWatcher<DocumentSaveResult> saving_;
        std::optional<SaveTicket> saveTicket_;
        bool quitPending_ = false, closeSyncPending_ = false, closeSynchronized_ = false, closeFrozen_ = false, closePrompt_ = false;
        bool opened_ = false, preparing_ = false, cleaning_ = false, closing_ = false, closed_ = false;
    };
}
