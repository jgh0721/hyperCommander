#pragma once
#include "runtime/session.hpp"
#include <QDialog>
#include <QFutureWatcher>
#include <functional>

class QTreeWidget;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCloseEvent;
namespace hc::compare
{
    class SaveRecoveryDialog final : public QDialog
    {
    public:
        explicit SaveRecoveryDialog(Request Context, QWidget* Parent = nullptr);
        ~SaveRecoveryDialog() override;
        void Refresh();
        void OpenJournal(const QString& Path);
        void Recover();
        void Cancel();
        bool Busy() const { return active_; }
        const Result& LastResult() const { return last_; }
        std::function<void()> activityChanged_;
    protected:
        void closeEvent(QCloseEvent* Event) override;
        void reject() override;
    private:
        enum class Task { List, Preview, Recover };
        void run(Task Kind, const QString& Path = {});
        void updateControls();
        void showResult();
        void clearPreview();
        Request context_;
        Result last_;
        Task task_ = Task::List;
        QString selectedPath_, previewPath_;
        QByteArray previewHash_;
        std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
        QFutureWatcher<Result> watcher_;
        QTreeWidget* journals_ = nullptr;
        QLineEdit *destination_ = nullptr, *backup_ = nullptr, *stage_ = nullptr;
        QLabel *phase_ = nullptr, *state_ = nullptr;
        QPlainTextEdit* details_ = nullptr;
        QPushButton *refresh_ = nullptr, *browse_ = nullptr, *preview_ = nullptr, *recover_ = nullptr, *close_ = nullptr;
        bool active_ = false, closing_ = false;
    };
}
