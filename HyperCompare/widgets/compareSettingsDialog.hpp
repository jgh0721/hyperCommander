#pragma once
#include "compareSettings.hpp"
#include <QDialog>
#include <functional>
class QTabWidget;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QTableWidget;
class QLabel;
class QPushButton;
class QDialogButtonBox;
namespace hc::compare
{
    class CompareSettingsDialog final : public QDialog
    {
    public:
        CompareSettingsDialog(CompareSettings Settings, QWidget* Parent);
        std::function<void(const CompareSettings&)> applied_;
    private:
        void changed();
        bool apply();
        void fillKeys();
        void selectCommand();
        void checkKey();
        void fillColors();
        void refreshPreview();
        CompareSettings draft_, saved_;
        QTabWidget* tabs_;
        QDialogButtonBox* buttons_;
        QLabel* status_;
        QLineEdit* search_;
        QComboBox* category_;
        QCheckBox* modified_;
        QTableWidget* keys_;
        QLineEdit* capture_;
        QLabel* commandLabel_;
        QLabel* commandInfo_;
        QLabel* conflict_;
        QPushButton* assign_;
        QComboBox* colorMode_;
        QComboBox* colorGroup_;
        QCheckBox* link_;
        QTableWidget* colors_;
        QWidget* preview_;
        QLabel* contrast_;
        QString selected_;
        bool filling_ = false;
    };
}
