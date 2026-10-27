#pragma once
#include "commonLibs/operations/jobScheduler.hpp"
#include <QDialog>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QLayout;

namespace hc::ui
{
    class FileJobInteraction final : public QDialog
    {
    public:
        using Reply = std::function<void(operations::InteractionChoice, const QString&)>;
        FileJobInteraction(operations::JobSnapshot Snapshot, QString Operation, Reply Answer,
            std::function<void()> SavedPasswords, std::function<void()> Details, QWidget* Parent);
        bool Matches(const operations::JobSnapshot& Snapshot) const;
        void Dismiss();
        void SetError(const QString& Message);
    protected:
        void reject() override;
        void resizeEvent(QResizeEvent* Event) override;
        void showEvent(QShowEvent* Event) override;
        void closeEvent(QCloseEvent* Event) override;
        bool eventFilter(QObject* Object, QEvent* Event) override;
    private:
        bool has(operations::InteractionChoice Choice) const;
        void endpoint(const QString& Role, const std::string& Value, const char* Name);
        void rebuildActions();
        QPushButton* addButton(QLayout* Row, const QString& Text, const char* Name, std::function<void()> Clicked);
        void choose(operations::InteractionChoice Choice);
        void editName();
        void validate();
        void focusAction();
        void fitToContents();
        operations::JobSnapshot snapshot_;
        Reply answer_;
        std::function<void()> savedPasswords_;
        std::function<void()> details_;
        QVBoxLayout* body_ = nullptr;
        QWidget *content_ = nullptr, *actions_ = nullptr;
        QVBoxLayout* footer_ = nullptr;
        QLabel *heading_ = nullptr, *message_ = nullptr, *validation_ = nullptr;
        QLineEdit* editor_ = nullptr;
        QPushButton* apply_ = nullptr;
        QString originalName_, headingText_, messageText_;
        bool rename_ = false, dismissed_ = false, replied_ = false, fitting_ = false;
    };
}
