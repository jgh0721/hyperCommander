#include "stdafx.h"
#include "sqliteKeyManager.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <Windows.h>
#include <NTSecAPI.h>
#include <wincred.h>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;

        std::function<bool(QWidget*, const QString&)>& LoginCheck()
        {
            static std::function<bool(QWidget*, const QString&)> Check;
            return Check;
        }

        void Wipe(void* Data, size_t Size) { if (Data && Size) SecureZeroMemory(Data, Size); }

        // CredUI 가 돌려준 묶음을 LSA 로 확인한다(암호 · PIN · 생체 모두). 안 되면 사용자 이름 · 암호로 LogonUser.
        bool VerifyPackedCredential(ULONG Package, void* Buffer, ULONG Size)
        {
            HANDLE Lsa = nullptr;
            if (LsaConnectUntrusted(&Lsa) == 0) {
                LSA_STRING Origin{};
                char OriginName[] = "HyperCommander";
                Origin.Buffer = OriginName;
                Origin.Length = USHORT(sizeof(OriginName) - 1);
                Origin.MaximumLength = USHORT(sizeof(OriginName));
                TOKEN_SOURCE Source{};
                memcpy(Source.SourceName, "HyperCmd", 8);
                AllocateLocallyUniqueId(&Source.SourceIdentifier);
                void* Profile = nullptr;
                ULONG ProfileSize = 0;
                LUID LogonId{};
                HANDLE Token = nullptr;
                QUOTA_LIMITS Quotas{};
                NTSTATUS SubStatus = 0;
                const NTSTATUS Status = LsaLogonUser(Lsa, &Origin, Network, Package, Buffer, Size, nullptr, &Source, &Profile, &ProfileSize, &LogonId, &Token, &Quotas, &SubStatus);
                if (Profile) LsaFreeReturnBuffer(Profile);
                if (Token) CloseHandle(Token);
                LsaDeregisterLogonProcess(Lsa);
                if (Status == 0) return true;
            }
            wchar_t User[CREDUI_MAX_USERNAME_LENGTH + 1]{}, Domain[CREDUI_MAX_DOMAIN_TARGET_LENGTH + 1]{}, Password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
            DWORD UserSize = DWORD(std::size(User)), DomainSize = DWORD(std::size(Domain)), PasswordSize = DWORD(std::size(Password));
            bool Ok = false;
            if (CredUnPackAuthenticationBufferW(CRED_PACK_PROTECTED_CREDENTIALS, Buffer, Size, User, &UserSize, Domain, &DomainSize, Password, &PasswordSize)) {
                wchar_t Name[CREDUI_MAX_USERNAME_LENGTH + 1]{}, Realm[CREDUI_MAX_DOMAIN_TARGET_LENGTH + 1]{};
                if (CredUIParseUserNameW(User, Name, DWORD(std::size(Name)), Realm, DWORD(std::size(Realm))) != NO_ERROR) wcsncpy_s(Name, User, _TRUNCATE);
                HANDLE Token = nullptr;
                Ok = LogonUserW(Name, Realm[0] ? Realm : nullptr, Password, LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &Token) != 0;
                if (Token) CloseHandle(Token);
            }
            Wipe(Password, sizeof(Password));
            return Ok;
        }

        bool PromptWindowsLogin(QWidget* Parent, const QString& Reason)
        {
            const auto Caption = QObject::tr("HyperCommander — 저장한 키 보기").toStdWString();
            const auto Message = Reason.toStdWString();
            CREDUI_INFOW Info{};
            Info.cbSize = sizeof(Info);
            Info.hwndParent = Parent ? reinterpret_cast<HWND>(Parent->window()->winId()) : nullptr;
            Info.pszCaptionText = Caption.c_str();
            Info.pszMessageText = Message.c_str();
            for (int Attempt = 0; Attempt < 3; ++Attempt) {
                ULONG Package = 0;
                void* Out = nullptr;
                ULONG OutSize = 0;
                BOOL Save = FALSE;
                const DWORD Rc = CredUIPromptForWindowsCredentialsW(&Info, Attempt == 0 ? 0 : ERROR_LOGON_FAILURE, &Package, nullptr, 0, &Out, &OutSize, &Save,
                                                                    CREDUIWIN_ENUMERATE_CURRENT_USER);
                if (Rc != ERROR_SUCCESS) return false;      // 취소
                const bool Ok = VerifyPackedCredential(Package, Out, OutSize);
                Wipe(Out, OutSize);
                CoTaskMemFree(Out);
                if (Ok) return true;
            }
            return false;
        }

        QString UsedText(const QDateTime& When)
        {
            if (!When.isValid()) return QObject::tr("—");
            const auto Local = When.toLocalTime();
            if (Local.date() == QDate::currentDate()) return QObject::tr("오늘 %1").arg(Local.toString(QStringLiteral("HH:mm")));
            return Local.toString(QStringLiteral("yyyy-MM-dd"));
        }
    }

    void SetLoginCheck(std::function<bool(QWidget*, const QString&)> Check) { LoginCheck() = std::move(Check); }

    bool ConfirmWindowsLogin(QWidget* Parent, const QString& Reason)
    {
        if (LoginCheck()) return LoginCheck()(Parent, Reason);
        return PromptWindowsLogin(Parent, Reason);
    }

    KeyManagerPage::KeyManagerPage(const QString& Directory, QWidget* Parent) : QWidget(Parent), store_(Directory)
    {
        setObjectName(QStringLiteral("sqliteKeyManager"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(8);
        auto* Head = new QHBoxLayout;
        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("sqliteKeysTitle"));
        auto TitleFont = title_->font();
        TitleFont.setWeight(QFont::DemiBold);
        title_->setFont(TitleFont);
        Head->addWidget(title_, 1);
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("sqliteKeysSearch"));
        search_->setPlaceholderText(tr("파일 이름 · 경로 검색"));
        search_->setClearButtonEnabled(true);
        search_->setMinimumWidth(200);
        search_->setMaximumWidth(260);
        Head->addWidget(search_);
        Layout->addLayout(Head);

        list_ = new QTreeWidget(this);
        list_->setObjectName(QStringLiteral("sqliteKeysList"));
        list_->setRootIsDecorated(false);
        list_->setUniformRowHeights(false);
        list_->setAlternatingRowColors(false);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHeaderLabels({tr("데이터베이스"), tr("키 형식"), tr("마지막으로 연 때"), tr("상태")});
        list_->header()->setStretchLastSection(false);
        list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int Column = 1; Column < 4; ++Column) list_->header()->setSectionResizeMode(Column, QHeaderView::ResizeToContents);
        list_->setMinimumHeight(150);
        Layout->addWidget(list_, 1);

        auto* Buttons = new QHBoxLayout;
        const auto Make = [this, Buttons](const QString& Text, const QString& Name) {
            auto* Button = new QPushButton(Text, this);
            Button->setObjectName(Name);
            Buttons->addWidget(Button);
            return Button;
        };
        reveal_ = Make(tr("키 보기…"), QStringLiteral("sqliteKeysReveal"));
        move_ = Make(tr("경로 바꾸기…"), QStringLiteral("sqliteKeysMove"));
        remove_ = Make(tr("삭제"), QStringLiteral("sqliteKeysRemove"));
        Buttons->addStretch(1);
        clean_ = Make(tr("없는 파일 정리"), QStringLiteral("sqliteKeysClean"));
        clear_ = Make(tr("모두 삭제…"), QStringLiteral("sqliteKeysClear"));
        Layout->addLayout(Buttons);

        connect(search_, &QLineEdit::textChanged, this, &KeyManagerPage::Refresh);
        connect(list_, &QTreeWidget::itemSelectionChanged, this, &KeyManagerPage::refreshButtons);
        connect(reveal_, &QPushButton::clicked, this, &KeyManagerPage::RevealSelected);
        connect(move_, &QPushButton::clicked, this, [this] {
            const auto Current = selectedPath();
            const auto Path = QFileDialog::getOpenFileName(this, tr("새 경로 — %1").arg(QFileInfo(Current).fileName()), QFileInfo(Current).absolutePath(),
                                                           tr("SQLite 데이터베이스 (*.db *.sqlite *.sqlite3 *.db3 *.s3db);;모든 파일 (*)"));
            if (!Path.isEmpty()) ChangeSelectedPath(Path);
        });
        connect(remove_, &QPushButton::clicked, this, [this] { RemoveSelected(true); });
        connect(clean_, &QPushButton::clicked, this, [this] {
            const int Removed = RemoveMissing();
            QMessageBox::information(this, tr("없는 파일 정리"), Removed > 0 ? tr("없는 파일의 키 %1개를 지웠습니다. 연결 안 된 네트워크 경로는 그대로 둡니다.").arg(Removed)
                                                                             : tr("지울 항목이 없습니다. 연결 안 된 네트워크 경로는 정리하지 않습니다."));
        });
        connect(clear_, &QPushButton::clicked, this, [this] { RemoveAll(true); });
        Refresh();
    }

    QString KeyManagerPage::selectedPath() const
    {
        const auto Items = list_->selectedItems();
        return Items.isEmpty() ? QString() : Items.first()->data(0, Qt::UserRole).toString();
    }

    void KeyManagerPage::refreshButtons()
    {
        const bool Selected = !selectedPath().isEmpty();
        reveal_->setEnabled(Selected);
        move_->setEnabled(Selected);
        remove_->setEnabled(Selected);
        clear_->setEnabled(list_->topLevelItemCount() > 0 || !search_->text().isEmpty());
    }

    void KeyManagerPage::Refresh()
    {
        const auto Selected = selectedPath();
        const auto Entries = store_.Entries();
        const auto Filter = search_->text().trimmed();
        const auto& Colors = fm::style::themeColorsFor(this);
        list_->clear();
        for (const auto& Entry : Entries) {
            const QFileInfo Info(Entry.path_);
            const auto Folder = QDir::toNativeSeparators(Info.absolutePath());
            if (!Filter.isEmpty() && !Info.fileName().contains(Filter, Qt::CaseInsensitive) && !Folder.contains(Filter, Qt::CaseInsensitive)) continue;
            auto* Item = new QTreeWidgetItem(list_);
            Item->setText(0, Info.fileName() + QLatin1Char('\n') + Folder);
            Item->setToolTip(0, QDir::toNativeSeparators(Entry.path_));
            Item->setData(0, Qt::UserRole, Entry.path_);
            Item->setText(1, KeyFormatLabel(Entry.format_));
            Item->setText(2, UsedText(Entry.used_.isValid() ? Entry.used_ : Entry.saved_));
            const auto State = KeyStore::StateOf(Entry);
            Item->setText(3, State == KeyStore::State::Ok ? tr("확인됨") : State == KeyStore::State::Unreachable ? tr("연결 안 됨") : tr("파일 없음"));
            Item->setForeground(3, Colors[State == KeyStore::State::Ok ? T::Ok : State == KeyStore::State::Unreachable ? T::Warn : T::Danger]);
            if (Entry.path_ == Selected) Item->setSelected(true);
        }
        title_->setText(tr("저장한 키 %1개").arg(Entries.size()));
        refreshButtons();
    }

    void KeyManagerPage::RevealSelected()
    {
        const auto Path = selectedPath();
        if (Path.isEmpty()) return;
        if (!ConfirmWindowsLogin(this, tr("%1 의 저장한 키를 보려면 Windows 계정을 확인하세요.").arg(QFileInfo(Path).fileName()))) return;
        const auto Entry = store_.Find(Path);
        auto Key = Entry ? RevealKey(Entry->protected_) : QByteArray();
        if (Key.isEmpty()) {
            QMessageBox::warning(this, tr("키 보기"), tr("키를 풀지 못했습니다. 다른 사용자 계정이나 다른 PC 에서 저장한 키일 수 있습니다."));
            return;
        }
        auto* Dialog = new QDialog(this);
        Dialog->setObjectName(QStringLiteral("sqliteKeyRevealDialog"));
        Dialog->setAttribute(Qt::WA_DeleteOnClose);
        Dialog->setWindowTitle(tr("저장한 키 — %1").arg(QFileInfo(Path).fileName()));
        auto* Form = new QFormLayout(Dialog);
        auto* Value = new QLineEdit(QString::fromUtf8(Key), Dialog);
        WipeKey(Key);
        Value->setObjectName(QStringLiteral("sqliteKeyRevealValue"));
        Value->setReadOnly(true);
        Value->setMinimumWidth(320);
        Form->addRow(tr("키"), Value);
        Form->addRow(tr("키 형식"), new QLabel(KeyFormatLabel(Entry->format_), Dialog));
        auto* Box = new QDialogButtonBox(QDialogButtonBox::Close, Dialog);
        auto* Copy = Box->addButton(tr("복사"), QDialogButtonBox::ActionRole);
        connect(Copy, &QPushButton::clicked, Value, [Value] { QApplication::clipboard()->setText(Value->text()); });
        connect(Box, &QDialogButtonBox::rejected, Dialog, &QDialog::reject);
        connect(Dialog, &QDialog::finished, Value, [Value] { Value->setText(QString(Value->text().size(), QChar(u' '))); Value->clear(); });
        Form->addRow(Box);
        Dialog->open();
    }

    void KeyManagerPage::ChangeSelectedPath(const QString& Path)
    {
        const auto Current = selectedPath();
        if (Current.isEmpty() || Path.isEmpty()) return;
        if (!store_.ChangePath(Current, Path)) QMessageBox::warning(this, tr("경로 바꾸기"), tr("키 저장소에 쓰지 못했습니다."));
        Refresh();
        emit Changed();
    }

    void KeyManagerPage::RemoveSelected(bool Ask)
    {
        const auto Path = selectedPath();
        if (Path.isEmpty()) return;
        if (Ask && QMessageBox::question(this, tr("키 삭제"), tr("%1 의 저장한 키를 지울까요? 다음에 열 때 키를 다시 넣어야 합니다.").arg(QFileInfo(Path).fileName()),
                                         QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        store_.Remove(Path);
        Refresh();
        emit Changed();
    }

    int KeyManagerPage::RemoveMissing()
    {
        const int Removed = store_.RemoveMissing();
        Refresh();
        if (Removed > 0) emit Changed();
        return Removed;
    }

    void KeyManagerPage::RemoveAll(bool Ask)
    {
        const auto Count = store_.Entries().size();
        if (Count == 0) return;
        if (Ask && QMessageBox::warning(this, tr("모두 삭제"), tr("저장한 키 %1개를 모두 지울까요? 되돌릴 수 없습니다.").arg(Count),
                                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        store_.RemoveAll();
        Refresh();
        emit Changed();
    }
}
