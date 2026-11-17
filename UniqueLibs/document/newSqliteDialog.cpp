#include "stdafx.h"
#include "newSqliteDialog.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqlite/sqliteRuntime.hpp"
#include "sqlite/sqliteSeeDialog.hpp"

/// 공통 라이브러리
#include "commonLibs/document/documentFormatSupport.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    NewSqliteDialog::NewSqliteDialog(QWidget* Parent, const QString& Folder, sqlite::KeyFormat Format, bool SaveKey)
        : QDialog(Parent)
    {
        setObjectName("newSqliteDialog");
        setWindowTitle(tr("새 SQLite 데이터베이스"));
        setAttribute(Qt::WA_DeleteOnClose);
        auto* Layout = new QVBoxLayout(this);
        auto* Form = new QFormLayout;
        Layout->addLayout(Form);

        auto* PathRow = new QHBoxLayout;
        path_ = new QLineEdit(QDir::toNativeSeparators(QDir(Folder).filePath(tr("새 데이터베이스.sqlite3"))), this);
        path_->setObjectName("newSqlite.path");
        auto* Browse = new QPushButton(tr("찾아보기…"), this);
        PathRow->addWidget(path_, 1);
        PathRow->addWidget(Browse);
        auto* PathLabel = new QLabel(tr("저장 경로(&P)"), this);
        PathLabel->setBuddy(path_);
        Form->addRow(PathLabel, PathRow);
        connect(Browse, &QPushButton::clicked, this, &NewSqliteDialog::choosePath);

        encrypt_ = new QCheckBox(tr("SEE (AES-256)로 암호화"), this);
        encrypt_->setObjectName("newSqlite.encrypt");
        Form->addRow(encrypt_);
        const auto See = sqlite::Runtime::See();
        if (See != sqlite::SeeState::Available) {
            encrypt_->setEnabled(false);
            auto* Reason = new QLabel(sqlite::Runtime::SeeText(See), this);
            Reason->setWordWrap(true);
            Form->addRow(Reason);
        }
        format_ = new QComboBox(this);
        format_->setObjectName("newSqlite.keyFormat");
        for (const auto Item : {sqlite::KeyFormat::Text, sqlite::KeyFormat::Raw, sqlite::KeyFormat::Hex})
            format_->addItem(sqlite::KeyFormatLabel(Item), int(Item));
        format_->setCurrentIndex(format_->findData(int(Format)));
        Form->addRow(tr("키 형식(&F)"), format_);

        key_ = new sqlite::SecretField(QStringLiteral("newSqlite.key"), this);
        confirm_ = new sqlite::SecretField(QStringLiteral("newSqlite.confirm"), this);
        auto* KeyLabel = new QLabel(tr("키(&K)"), this);
        KeyLabel->setBuddy(key_->Edit());
        key_->Edit()->setAccessibleName(tr("키"));
        Form->addRow(KeyLabel, key_);
        auto* ConfirmLabel = new QLabel(tr("키 확인(&V)"), this);
        ConfirmLabel->setBuddy(confirm_->Edit());
        confirm_->Edit()->setAccessibleName(tr("키 확인"));
        Form->addRow(ConfirmLabel, confirm_);
        save_ = new QCheckBox(tr("이 PC에 키 저장 (DPAPI · 현재 사용자)"), this);
        save_->setObjectName("newSqlite.saveKey");
        save_->setChecked(SaveKey);
        Form->addRow(save_);
        const auto ShowKeyFields = [this, Form](bool Show) {
            const QList<QWidget*> Fields{format_, key_, confirm_, save_};
            for (auto* Field : Fields)
                Form->setRowVisible(Field, Show);
            if (!Show) { key_->Wipe(); confirm_->Wipe(); }
            adjustSize();
        };
        connect(encrypt_, &QCheckBox::toggled, this, ShowKeyFields);

        message_ = new QLabel(this);
        message_->setObjectName("newSqlite.message");
        message_->setTextFormat(Qt::PlainText);
        message_->setWordWrap(true);
        Layout->addWidget(message_);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        create_ = Buttons->button(QDialogButtonBox::Ok);
        create_->setObjectName("newSqlite.create");
        create_->setText(tr("만들기(&O)"));
        Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소(&C)"));
        Layout->addWidget(Buttons);
        connect(Buttons, &QDialogButtonBox::accepted, this, &NewSqliteDialog::accept);
        connect(Buttons, &QDialogButtonBox::rejected, this, &NewSqliteDialog::reject);
        hc::ui::ApplyDialogFrame(this, Buttons, {{}, tr("저장 경로와 암호화 여부를 정합니다."), fm::ui::glyph::NewFile});
        setMinimumWidth(520);
        ShowKeyFields(false);
        path_->setFocus();
        const auto Name = QFileInfo(path_->text()).completeBaseName();
        path_->setSelection(path_->text().size() - QFileInfo(path_->text()).fileName().size(), Name.size());
    }

    void NewSqliteDialog::choosePath()
    {
        QStringList Patterns;
        for (const auto Suffix : SqliteDocumentExtensions)
            Patterns.append(QStringLiteral("*.") + QString::fromLatin1(Suffix.data(), qsizetype(Suffix.size())));
        auto* Dialog = new QFileDialog(this, tr("데이터베이스 저장 경로"), path_->text(),
                                       tr("SQLite 데이터베이스 (%1)").arg(Patterns.join(QLatin1Char(' '))));
        Dialog->setAcceptMode(QFileDialog::AcceptSave);
        Dialog->setOption(QFileDialog::DontConfirmOverwrite);
        Dialog->setDefaultSuffix(QStringLiteral("sqlite3"));
        Dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(Dialog, &QFileDialog::fileSelected, this, [this](const QString& Path) {
            path_->setText(QDir::toNativeSeparators(Path));
        });
        Dialog->open();
    }

    void NewSqliteDialog::ShowError(const QString& Error)
    {
        creating_ = false;
        setEnabled(true);
        message_->setStyleSheet(QStringLiteral("color: %1;").arg(
            fm::style::themeColorsFor(message_)[fm::style::Token::Danger].name()));
        message_->setText(Error);
    }

    void NewSqliteDialog::reject()
    {
        if (!creating_) QDialog::reject();
    }

    void NewSqliteDialog::accept()
    {
        if (creating_) return;
        auto Path = QDir::fromNativeSeparators(path_->text().trimmed());
        const QFileInfo File(Path);
        static const QRegularExpression Forbidden(QStringLiteral(R"([:*?"<>|\x00-\x1f])"));
        const auto Name = File.fileName();
        if (!File.isAbsolute() || Name.isEmpty() || Name.startsWith('.') || Name.endsWith('.') || Name.contains(Forbidden)) {
            ShowError(tr("파일 이름을 포함한 올바른 저장 경로를 입력하세요."));
            return;
        }
        if (File.suffix().isEmpty()) Path += QStringLiteral(".sqlite3");
        else {
            const auto Suffix = File.suffix().toLower().toLatin1();
            if (std::find(SqliteDocumentExtensions.begin(), SqliteDocumentExtensions.end(),
                          std::string_view(Suffix.constData(), Suffix.size())) == SqliteDocumentExtensions.end()) {
                ShowError(tr("SQLite 확장자를 사용하세요 (.sqlite3, .db, .sqlite, .db3, .s3db, .sl3)."));
                return;
            }
        }

        NewSqliteRequest Request;
        Request.path_ = QDir::cleanPath(Path);
        if (encrypt_->isChecked()) {
            if (key_->Edit()->text().isEmpty() || key_->Edit()->text() != confirm_->Edit()->text()) {
                ShowError(tr("키를 두 칸에 똑같이 입력하세요."));
                return;
            }
            Request.key_.format_ = sqlite::KeyFormat(format_->currentData().toInt());
            auto Key = key_->Take();
            confirm_->Wipe();
            if (Request.key_.format_ == sqlite::KeyFormat::Hex && !sqlite::ValidHexKey(Key)) {
                sqlite::WipeKey(Key);
                ShowError(tr("16진 키는 0–9 · A–F 글자를 짝수 개 넣어야 합니다."));
                return;
            }
            Request.key_.bytes_ = sqlite::ProtectKey(Key);
            sqlite::WipeKey(Key);
            if (Request.key_.bytes_.isEmpty()) {
                ShowError(tr("키를 보호하지 못했습니다(DPAPI)."));
                return;
            }
            Request.saveKey_ = save_->isChecked();
        }
        creating_ = true;
        message_->setStyleSheet({});
        message_->setText(tr("데이터베이스를 만드는 중입니다."));
        setEnabled(false);
        emit Requested(Request);
    }
}
