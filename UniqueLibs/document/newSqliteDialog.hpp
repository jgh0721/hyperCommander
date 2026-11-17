#pragma once

/// 고유 라이브러리
#include "sqlite/sqliteKeyStore.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace hc::sqlite { class SecretField; }

namespace hc::document
{
    struct NewSqliteRequest
    {
        QString path_;
        sqlite::ProtectedKey key_;
        bool saveKey_ = false;
    };

    class NewSqliteDialog final : public QDialog
    {
        Q_OBJECT
    public:
        NewSqliteDialog(QWidget* Parent, const QString& Folder, sqlite::KeyFormat Format, bool SaveKey);
        void ShowError(const QString& Error);

    signals:
        void Requested(const hc::document::NewSqliteRequest& Request);

    public slots:
        void accept() override;
        void reject() override;

    private:
        void choosePath();

        QLineEdit* path_ = nullptr;
        QCheckBox* encrypt_ = nullptr;
        QComboBox* format_ = nullptr;
        sqlite::SecretField* key_ = nullptr;
        sqlite::SecretField* confirm_ = nullptr;
        QCheckBox* save_ = nullptr;
        QLabel* message_ = nullptr;
        QPushButton* create_ = nullptr;
        bool creating_ = false;
    };
}
