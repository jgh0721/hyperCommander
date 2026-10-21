#pragma once
#include "passwordStore.hpp"
#include "commonLibs/storage/storage.hpp"
#include <QDialog>
#include <QWidget>

class QTableWidget;
class QCheckBox;
class QPushButton;
class QLabel;
class QComboBox;
class QLineEdit;
class QButtonGroup;

namespace hc::passwords
{
    Store& RuntimeStore();
    bool UnlockStore(Store& Value, QWidget* Parent);

    class SettingsPage final : public QWidget
    {
        Q_OBJECT
    public:
        explicit SettingsPage(QWidget* Parent = nullptr);
        void Load();
        bool Apply();
    signals:
        void Changed();
    private:
        void refresh();
        void edit(bool Add);
        void master(bool Enable);
        Store draft_;
        QTableWidget* table_;
        QCheckBox* automatic_;
        QCheckBox* master_;
        QPushButton* add_;
        QPushButton* edit_;
        QPushButton* remove_;
        QPushButton* changeMaster_;
        QPushButton* unlock_;
        QLabel* status_;
        bool dirty_ = false;
    };

    class ArchivePasswordDialog final : public QDialog
    {
    public:
        ArchivePasswordDialog(QString Archive, unsigned Attempt, QWidget* Parent = nullptr);
        ~ArchivePasswordDialog() override;
        QString Value() const;
        int Scope() const;
        QString EntryName() const;
    private:
        void populate();
        void accept() override;
        QString archive_;
        QComboBox* saved_;
        QLineEdit* value_;
        QLineEdit* name_;
        QButtonGroup* scope_;
        QPushButton* unlock_;
        QLabel* error_;
    };

    hc::storage::Result<hc::storage::Password> AcquireArchivePassword(const std::string& Archive, unsigned Attempt,
        const hc::storage::OperationContext& Context, QWidget* Parent = nullptr);
    std::optional<hc::storage::Password> TryAutomaticArchivePassword(const std::string& Archive, unsigned Attempt,
        const hc::storage::OperationContext& Context);
    void ForgetSessionPasswords();
}
