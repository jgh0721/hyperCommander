#pragma once
#include "dialogShell.hpp"
#include "UniqueLibs/fileAttributes.hpp"
#include <QFutureWatcher>
#include <QDateTimeEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <mutex>

namespace hc::ui
{
    class FileAttributesDialog final : public DialogShell
    {
    public:
        FileAttributesDialog(std::vector<storage::EntrySnapshot> Entries,
            std::shared_ptr<storage::IStorageProvider> Reader,
            std::shared_ptr<const storage::ISelectionManifest> Manifest, QWidget* Parent = nullptr);
        ~FileAttributesDialog() override;
        bool Changed() const { return changed_; }
    protected:
        void accept() override;
        void reject() override;
    private:
        struct State
        {
            std::stop_source stop_;
            std::mutex mutex_;
            std::optional<std::pair<storage::EntrySnapshot, bool>> question_;
            std::optional<attributes::Decision> answer_;
            QString progress_;
            attributes::Plan plan_;
            attributes::Outcome outcome_;
            bool allowElevation_ = true;
        };
        void collect();
        void complete();
        void update();
        void poll();
        void updateTargetSummary(const std::vector<storage::EntrySnapshot>& Entries);
        storage::MetadataPatch patch() const;
        static attributes::Approval approval(std::shared_ptr<State> StateValue);
        std::vector<storage::EntrySnapshot> roots_;
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::shared_ptr<const storage::ISelectionManifest> manifest_;
        std::shared_ptr<State> state_;
        QFutureWatcher<void>* watcher_;
        QTimer* timer_;
        QCheckBox *recursive_, *changeTime_;
        QDateTimeEdit* timestamp_;
        QComboBox* values_[4];
        QLabel* current_[4];
        QLabel *summary_, *scope_, *targetPath_;
        QPlainTextEdit* details_;
        QPushButton *retry_, *now_, *showDetails_;
        bool busy_ = false, applying_ = false, ready_ = false, result_ = false, changed_ = false, asking_ = false;
    };
}
