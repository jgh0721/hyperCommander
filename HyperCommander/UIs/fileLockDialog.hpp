#pragma once

#include "dialogShell.hpp"
#include "UniqueLibs/fileLockService.hpp"
#include <QJsonArray>
#include <QDateTime>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;
namespace fm::ui { class Banner; class KeyValueCard; }

namespace hc::ui {
struct FileLockDialogRequest {
    QString source_, destination_, reason_;
    bool canRetry_ = true, canSkip_ = true;
    // Checks the original interaction ID, never only a recycled job selection.
    std::function<bool()> current_;
};
class FileLockDialog final : public DialogShell {
public:
    enum class Reply { None, Retry, Skip, CancelJob };
    using ClientFactory = std::function<std::shared_ptr<filelocks::FileLockClient>(const QStringList&, bool)>;
    explicit FileLockDialog(FileLockDialogRequest Request, QWidget* Parent = nullptr, ClientFactory Factory = {});
    ~FileLockDialog() override;
    Reply SelectedReply() const { return reply_; }
protected:
    void accept() override;
private:
    void query(bool Elevated);
    void updateDetails();
    void act(const QString& Action);
    bool current() const;
    void setBusy(bool Busy);
    FileLockDialogRequest request_;
    ClientFactory factory_;
    std::shared_ptr<filelocks::FileLockClient> client_;
    QJsonArray rows_;
    QString generation_;
    QDateTime queried_;
    bool busy_ = false, requiresRefresh_ = true;
    bool elevated_ = false;
    Reply reply_ = Reply::None;
    QTreeWidget* table_ = nullptr;
    fm::ui::Banner* banner_ = nullptr;
    fm::ui::KeyValueCard* details_ = nullptr;
    QPushButton *refresh_ = nullptr, *elevate_ = nullptr, *stop_ = nullptr, *close_ = nullptr, *terminate_ = nullptr, *skip_ = nullptr;
    QLabel *summary_ = nullptr, *restriction_ = nullptr;
};
}
