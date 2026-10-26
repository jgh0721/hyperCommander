#include "stdafx.h"
#include "passwordUi.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"

/// Product settings
#include "UniqueLibs/settings/appSettings.hpp"
#include <fmwidgets/WindowChrome.h>
#include "UniqueLibs/qtDialogFrame.hpp"
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <fmstyle/StyleProps.h>

namespace hc::passwords
{
    namespace
    {
        QVector<QPointer<SettingsPage>>& pages() { static QVector<QPointer<SettingsPage>> Values; return Values; }
        void clear(QString& Text) { Text.fill(QChar::Null); Text.clear(); }
        void closeWithPrompt(QDialog& Dialog)
        {
            for (auto* Parent = Dialog.parentWidget(); Parent; Parent = Parent->parentWidget())
                if (Parent->objectName() == "hc.archive.password") {
                    if (auto* Prompt = qobject_cast<QDialog*>(Parent)) QObject::connect(Prompt, &QDialog::finished, &Dialog, &QDialog::reject);
                    break;
                }
        }
        QVBoxLayout* dialogRows(QDialog& Dialog)
        {
            closeWithPrompt(Dialog);
            Dialog.setWindowFlag(Qt::FramelessWindowHint);
            auto* Outer = new QVBoxLayout(&Dialog); Outer->setContentsMargins(0, 0, 0, 0); Outer->setSpacing(0);
            Outer->addWidget(new fm::ui::WindowChrome(&Dialog, &Dialog, {false, true}));
            auto* Body = new QWidget(&Dialog); Outer->addWidget(Body);
            return new QVBoxLayout(Body);
        }
        class SecretEdit final : public QLineEdit
        {
        public:
            explicit SecretEdit(QWidget* Parent) : QLineEdit(Parent), reveal_(new QToolButton(this))
            {
                reveal_->setText(QObject::tr("표시")); reveal_->setCheckable(true); reveal_->setAutoRaise(true);
                reveal_->setAccessibleName(QObject::tr("비밀번호 표시"));
                QObject::connect(reveal_, &QToolButton::toggled, this, [this](bool On) {
                    setEchoMode(On ? QLineEdit::Normal : QLineEdit::Password); reveal_->setText(On ? QObject::tr("숨김") : QObject::tr("표시"));
                    reveal_->setAccessibleName(On ? QObject::tr("비밀번호 숨김") : QObject::tr("비밀번호 표시"));
                });
            }
        protected:
            void resizeEvent(QResizeEvent* Event) override
            {
                QLineEdit::resizeEvent(Event); const int Width = fontMetrics().horizontalAdvance(QObject::tr("표시")) + 16;
                const int Frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth); setTextMargins(0, 0, Width + Frame, 0);
                reveal_->setGeometry(width() - Width - Frame, Frame, Width, height() - Frame * 2);
            }
        private:
            QToolButton* reveal_;
        };
        // Footer band under the body of a chrome dialog: buttons at the right, the accept button Primary.
        void footer(QDialog& Dialog, QDialogButtonBox* Buttons)
        {
            auto* Outer = qobject_cast<QVBoxLayout*>(Dialog.layout()); if (!Outer) return;
            auto* Band = new fm::ui::DialogFooter(&Dialog); auto* Row = new QHBoxLayout(Band);
            Row->setContentsMargins(20, 12, 20, 12); Row->setSpacing(8); Row->addStretch(1); Outer->addWidget(Band);
            hc::ui::MoveToFooter(Row, Buttons);
        }
        QDialogButtonBox* buttons(QDialogButtonBox::StandardButtons Which, QWidget* Parent)
        {
            auto* Box = new QDialogButtonBox(Which, Parent);
            if (auto* Ok = Box->button(QDialogButtonBox::Ok)) Ok->setText(QObject::tr("확인"));
            if (auto* Cancel = Box->button(QDialogButtonBox::Cancel)) Cancel->setText(QObject::tr("취소"));
            if (auto* Apply = Box->button(QDialogButtonBox::Apply)) Apply->setText(QObject::tr("적용"));
            return Box;
        }
        QLineEdit* secretInput(QWidget* Parent, const QString& Name)
        {
            auto* Edit = new SecretEdit(Parent); Edit->setObjectName(Name);
            Edit->setEchoMode(QLineEdit::Password); Edit->setMaxLength(4096);
            Edit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
            return Edit;
        }
        void wipe(QLineEdit* Edit) { auto Text = Edit->text(); Edit->clear(); clear(Text); }
        void showError(QWidget* Parent, const Store& Value) { hc::ui::MessageDialog::warning(Parent, QObject::tr("비밀번호 관리"), Value.Error()); }
        void manage(QWidget* Parent, const std::function<void()>& Finished)
        {
            auto* Dialog = new QDialog(Parent); Dialog->setAttribute(Qt::WA_DeleteOnClose);
            Dialog->setObjectName("hc.passwords.manager"); Dialog->setWindowTitle(QObject::tr("비밀번호 관리")); Dialog->resize(820, 660);
            auto* Rows = dialogRows(*Dialog); auto* Page = new SettingsPage(Dialog); Rows->addWidget(Page);
            auto* Buttons = buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, Dialog); Rows->addWidget(Buttons); footer(*Dialog, Buttons);
            QObject::connect(Buttons, &QDialogButtonBox::accepted, Dialog, [=] { if (Page->Apply()) Dialog->accept(); });
            QObject::connect(Buttons, &QDialogButtonBox::rejected, Dialog, &QDialog::reject);
            QObject::connect(Buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, Page, [Page] { Page->Apply(); });
            QObject::connect(Dialog, &QDialog::finished, Parent, [Finished] { Finished(); });
            Dialog->setWindowModality(Qt::WindowModal); Dialog->open();
        }
        struct Remembered
        {
            QString archive_;
            QByteArray protectedValue_;
            std::stop_token stop_;
            bool session_ = false;
        };
        QVector<Remembered>& memory() { static QVector<Remembered> Values; return Values; }
        QVector<QByteArray> candidates(const std::string& Archive, const hc::storage::OperationContext& Context)
        {
            RuntimeStore().ReloadIfChanged(); auto& Cache = memory();
            Cache.removeIf([](const Remembered& Value) { return !Value.session_ && Value.stop_.stop_requested(); });
            QVector<QByteArray> Result;
            if (QFileInfo(QString::fromStdString(Archive)).size() <= 0) return Result;
            const auto Path = QDir::cleanPath(QFileInfo(QString::fromStdString(Archive)).absoluteFilePath()).toCaseFolded();
            for (const auto& Value : Cache) if (Value.archive_ == Path && (Value.session_ || Value.stop_ == Context.stopToken_)) Result.append(Value.protectedValue_);
            const auto& Store = RuntimeStore();
            if (!Store.Locked() && Store.Automatic()) for (const auto& Entry : Store.Entries()) if (Entry.automatic_ && Result.size() < 101) Result.append(Entry.protectedValue_);
            return Result;
        }
        std::optional<hc::storage::Password> candidateValue(const QVector<QByteArray>& Values, unsigned Attempt)
        {
            if (!Attempt || Attempt > unsigned(Values.size())) return {};
            auto Bytes = Store::Reveal(Values[int(Attempt) - 1]); if (Bytes.isEmpty()) return {};
            hc::storage::Password Result(std::string(Bytes.constData(), size_t(Bytes.size()))); SecureZeroMemory(Bytes.data(), size_t(Bytes.size())); return Result;
        }
    }

    Store& RuntimeStore()
    {
        static Store Value(QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("HyperCommander.passwords"));
        static const bool Loaded = Value.Load(); Q_UNUSED(Loaded);
        return Value;
    }
    void ForgetSessionPasswords() { memory().clear(); }

    bool UnlockStore(Store& Value, QWidget* Parent)
    {
        if (!Value.Locked()) return true;
        if (!Value.Error().isEmpty() && !Value.HasMaster()) { showError(Parent, Value); return false; }
        QDialog Dialog(Parent); Dialog.setObjectName("hc.passwords.unlock"); Dialog.setWindowTitle(QObject::tr("비밀번호 저장소 잠금 해제"));
        auto* Rows = dialogRows(Dialog);
        auto* Note = new QLabel(QObject::tr("저장된 비밀번호를 사용하려면 마스터 비밀번호를 입력하세요."), &Dialog); Note->setWordWrap(true); Rows->addWidget(Note);
        auto* Label = new QLabel(QObject::tr("마스터 비밀번호(&M)"), &Dialog); Rows->addWidget(Label);
        auto* Input = secretInput(&Dialog, "hc.passwords.masterInput"); Label->setBuddy(Input); Rows->addWidget(Input);
        auto* Error = new QLabel(&Dialog); Error->setWordWrap(true); Error->hide(); Rows->addWidget(Error);
        auto* Buttons = buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
        Buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("잠금 해제")); Rows->addWidget(Buttons); footer(Dialog, Buttons);
        QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
        QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&] {
            auto Master = Input->text(); const bool Opened = Value.Unlock(Master); clear(Master); wipe(Input);
            if (Opened) Dialog.accept(); else { Error->setText(Value.Error()); Error->show(); Input->setFocus(); }
        });
        Dialog.resize(480, Dialog.sizeHint().height()); Input->setFocus(); const bool Accepted = Dialog.exec() == QDialog::Accepted; wipe(Input); return Accepted;
    }

    SettingsPage::SettingsPage(QWidget* Parent) : QWidget(Parent)
    {
        pages().removeIf([](const auto& Page) { return Page.isNull(); }); pages().append(this);
        setObjectName("hc.settings.passwords");
        auto* Rows = new QVBoxLayout(this);
        // Board Set-07: title row with the list actions; list card at the left, options at the right.
        auto* Toolbar = new QHBoxLayout; Rows->addLayout(Toolbar);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("비밀번호 관리")); Header->setSubtitle(tr("압축 파일에 사용할 비밀번호를 저장하고 관리합니다.")); Toolbar->addWidget(Header, 1);
        add_ = new QPushButton(tr("비밀번호 추가(&N)"), this); edit_ = new QPushButton(tr("편집(&E)"), this); remove_ = new QPushButton(tr("삭제(&D)"), this);
        add_->setObjectName("hc.passwords.add"); edit_->setObjectName("hc.passwords.edit"); remove_->setObjectName("hc.passwords.remove");
        fm::style::setButtonRole(add_, fm::style::ButtonRole::Primary);
        Toolbar->addWidget(add_, 0, Qt::AlignTop); Toolbar->addWidget(edit_, 0, Qt::AlignTop); Toolbar->addWidget(remove_, 0, Qt::AlignTop);
        auto* Reload = new QPushButton(tr("다시 읽기"), this); Reload->setObjectName("hc.passwords.reload"); Toolbar->addWidget(Reload, 0, Qt::AlignTop);
        auto* Columns = new QHBoxLayout; Columns->setSpacing(12); Rows->addLayout(Columns, 1);
        auto* ListCard = new fm::ui::Card(tr("저장된 비밀번호"), this); auto* ListRows = new QVBoxLayout(ListCard); ListRows->setContentsMargins(0, 0, 0, 0);
        Columns->addWidget(ListCard, 3);
        auto* Side = new QVBoxLayout; Side->setSpacing(12); Columns->addLayout(Side, 2);
        connect(Reload, &QPushButton::clicked, this, [this] {
            if (dirty_ && hc::ui::MessageDialog::question(this, tr("비밀번호 다시 읽기"), tr("저장하지 않은 변경을 버리고 다시 읽을까요?")) != QMessageBox::Yes) return;
            RuntimeStore().Load(); Load();
        });
        table_ = new QTableWidget(0, 3, this); table_->setObjectName("hc.passwords.table"); table_->setAccessibleName(tr("저장된 비밀번호 목록"));
        table_->setHorizontalHeaderLabels({tr("이름"), tr("비밀번호"), tr("자동 시도")}); table_->verticalHeader()->hide();
        table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch); table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table_->setFrameShape(QFrame::NoFrame); ListRows->addWidget(table_, 1);
        auto* AutoCard = new fm::ui::Card(tr("자동 시도"), this); auto* AutoRows = new QVBoxLayout(AutoCard); AutoRows->setContentsMargins(14, 10, 14, 12); Side->addWidget(AutoCard);
        automatic_ = new QCheckBox(tr("저장된 비밀번호 자동 시도(&A)"), this); automatic_->setObjectName("hc.passwords.automatic");
        // Caption at the left, switch at the right; the checkbox object and its mnemonic stay.
        auto* AutoLine = new QHBoxLayout; auto* AutoCaption = new QLabel(automatic_->text(), AutoCard); AutoCaption->setBuddy(automatic_);
        automatic_->setAccessibleName(tr("저장된 비밀번호 자동 시도")); automatic_->setText({}); fm::style::setSwitch(automatic_);
        AutoLine->addWidget(AutoCaption, 1); AutoLine->addWidget(automatic_); AutoRows->addLayout(AutoLine);
        auto* AutoNote = new QLabel(tr("자동 시도 항목을 목록 순서대로 사용합니다. 암호화 압축 생성 시에는 직접 선택합니다."), this); AutoNote->setWordWrap(true); AutoRows->addWidget(AutoNote);
        auto* Protection = new QGroupBox(tr("저장소 보호"), this); auto* ProtectionRows = new QVBoxLayout(Protection); ProtectionRows->setContentsMargins(14, 10, 14, 12);
        master_ = new QCheckBox(tr("마스터 비밀번호 사용 (선택)"), Protection); master_->setObjectName("hc.passwords.master"); ProtectionRows->addWidget(master_);
        auto* MasterNote = new QLabel(tr("설정하지 않아도 비밀번호를 저장하고 사용할 수 있습니다.\n기본 보호: 현재 Windows 계정 · 다른 계정이나 PC로 복사하면 사용할 수 없습니다."), Protection); MasterNote->setWordWrap(true); ProtectionRows->addWidget(MasterNote);
        auto* Actions = new QHBoxLayout; changeMaster_ = new QPushButton(tr("마스터 비밀번호 설정…"), Protection); changeMaster_->setObjectName("hc.passwords.changeMaster");
        unlock_ = new QPushButton(tr("잠금 해제…"), Protection); unlock_->setObjectName("hc.passwords.unlock"); Actions->addWidget(changeMaster_); Actions->addWidget(unlock_); Actions->addStretch(); ProtectionRows->addLayout(Actions); Side->addWidget(Protection); Side->addStretch();
        status_ = new QLabel(this); status_->setObjectName("hc.passwords.status"); status_->setWordWrap(true); Rows->addWidget(status_);
        connect(add_, &QPushButton::clicked, this, [this] { edit(true); }); connect(edit_, &QPushButton::clicked, this, [this] { edit(false); });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this] { edit(false); });
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { const bool Selected = table_->currentRow() >= 0 && !draft_.Locked(); edit_->setEnabled(Selected); remove_->setEnabled(Selected); });
        connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* Item) {
            if (Item->column() != 2) return; const int Row = Item->row(); auto Secret = draft_.Value(Row);
            if (draft_.Edit(Row, draft_.Entries()[Row].name_, Secret, Item->checkState() == Qt::Checked)) { dirty_ = true; emit Changed(); } clear(Secret);
        });
        connect(remove_, &QPushButton::clicked, this, [this] {
            const int Row = table_->currentRow(); if (Row < 0) return;
            if (hc::ui::MessageDialog::question(this, tr("비밀번호 삭제"), tr("‘%1’ 항목을 삭제할까요? 적용 또는 확인을 누르면 저장됩니다.").arg(draft_.Entries()[Row].name_)) != QMessageBox::Yes) return;
            draft_.Remove(Row); dirty_ = true; refresh(); emit Changed();
        });
        connect(automatic_, &QCheckBox::toggled, this, [this](bool On) { draft_.SetAutomatic(On); dirty_ = true; emit Changed(); });
        connect(master_, &QCheckBox::toggled, this, &SettingsPage::master);
        connect(changeMaster_, &QPushButton::clicked, this, [this] { master(true); });
        connect(unlock_, &QPushButton::clicked, this, [this] {
            if (draft_.Locked()) { if (UnlockStore(draft_, this)) { RuntimeStore() = draft_; refresh(); } }
            else if (!dirty_) {
                for (const auto& Page : pages()) if (Page && Page->dirty_) {
                    hc::ui::MessageDialog::information(this, tr("비밀번호 저장소 잠금"), tr("다른 비밀번호 관리 창에 저장하지 않은 변경이 있습니다. 적용하거나 취소한 뒤 잠그세요.")); return;
                }
                RuntimeStore().Lock(); ForgetSessionPasswords();
                for (const auto& Page : pages()) if (Page) Page->Load();
            }
        });
        Load();
    }
    void SettingsPage::Load() { RuntimeStore().ReloadIfChanged(); draft_ = RuntimeStore(); dirty_ = false; refresh(); }
    void SettingsPage::refresh()
    {
        const QSignalBlocker TableBlock(table_), AutoBlock(automatic_), MasterBlock(master_);
        const int Selected = table_->currentRow(); table_->setRowCount(int(draft_.Entries().size()));
        for (int Row = 0; Row < table_->rowCount(); ++Row) {
            table_->setItem(Row, 0, new QTableWidgetItem(draft_.Entries()[Row].name_));
            table_->setItem(Row, 1, new QTableWidgetItem(QString(8, QChar(0x2022))));
            auto* Automatic = new QTableWidgetItem; Automatic->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            Automatic->setCheckState(draft_.Entries()[Row].automatic_ ? Qt::Checked : Qt::Unchecked); table_->setItem(Row, 2, Automatic);
        }
        if (table_->rowCount()) table_->selectRow(qBound(0, Selected, table_->rowCount() - 1));
        automatic_->setChecked(draft_.Automatic()); master_->setChecked(draft_.HasMaster());
        const bool Open = !draft_.Locked(); table_->setEnabled(Open); automatic_->setEnabled(Open); master_->setEnabled(Open); add_->setEnabled(Open);
        edit_->setEnabled(Open && table_->currentRow() >= 0); remove_->setEnabled(edit_->isEnabled());
        changeMaster_->setText(draft_.HasMaster() ? tr("마스터 비밀번호 변경…") : tr("마스터 비밀번호 설정…")); changeMaster_->setEnabled(Open && draft_.HasMaster());
        unlock_->setVisible(draft_.HasMaster() || draft_.Locked()); unlock_->setText(Open ? tr("지금 잠금") : tr("잠금 해제…")); unlock_->setEnabled(!Open || !dirty_);
        status_->setText(!draft_.Error().isEmpty() ? draft_.Error() : draft_.Locked() ? tr("저장소가 잠겨 있습니다. 잠금을 해제하거나 압축 파일 비밀번호를 직접 입력하세요.")
            : dirty_ ? tr("변경 사항은 적용 또는 확인을 누르면 저장됩니다.") : tr("저장된 비밀번호 %1개 · 적용/확인으로 저장 · 취소로 이전 내용 유지").arg(draft_.Entries().size()));
    }
    bool SettingsPage::Apply()
    {
        if (!dirty_) return true;
        if (!draft_.Save()) { showError(this, draft_); return false; }
        RuntimeStore() = draft_; ForgetSessionPasswords(); dirty_ = false; refresh(); return true;
    }
    void SettingsPage::edit(bool Add)
    {
        const int Row = table_->currentRow(); if (!Add && Row < 0) return;
        QDialog Dialog(this); Dialog.setObjectName("hc.passwords.entry"); Dialog.setWindowTitle(Add ? tr("비밀번호 추가") : tr("비밀번호 편집"));
        closeWithPrompt(Dialog);
        auto* Form = new QFormLayout(&Dialog); auto* Name = new QLineEdit(&Dialog); Name->setMaxLength(100); Name->setObjectName("hc.passwords.entryName");
        auto* Value = secretInput(&Dialog, "hc.passwords.entryValue"); auto* Automatic = new QCheckBox(tr("압축 파일을 열 때 자동 시도"), &Dialog); Automatic->setChecked(true);
        if (!Add) { Name->setText(draft_.Entries()[Row].name_); auto Text = draft_.Value(Row); Value->setText(Text); clear(Text); Automatic->setChecked(draft_.Entries()[Row].automatic_); }
        Form->addRow(tr("이름(&N)"), Name); Form->addRow(tr("비밀번호(&P)"), Value); Form->addRow(Automatic);
        auto* Error = new QLabel(&Dialog); Error->setWordWrap(true); Form->addRow(Error);
        auto* Buttons = buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog); Form->addRow(Buttons);
        hc::ui::ApplyDialogFrame(&Dialog, Buttons, {{}, tr("압축 파일을 열 때 시도할 비밀번호를 저장합니다."), fm::ui::glyph::LockKeyhole});
        connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
        connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&] {
            auto Secret = Value->text(); const bool Done = Add ? draft_.Add(Name->text(), Secret, Automatic->isChecked()) : draft_.Edit(Row, Name->text(), Secret, Automatic->isChecked()); clear(Secret);
            if (Done) Dialog.accept(); else Error->setText(draft_.Error());
        });
        Dialog.resize(460, Dialog.sizeHint().height()); if (Dialog.exec() == QDialog::Accepted) { dirty_ = true; refresh(); emit Changed(); } wipe(Value);
    }
    void SettingsPage::master(bool Enable)
    {
        QDialog Dialog(this); Dialog.setWindowTitle(Enable ? tr("마스터 비밀번호 설정") : tr("마스터 비밀번호 해제"));
        closeWithPrompt(Dialog);
        auto* Form = new QFormLayout(&Dialog); QLineEdit* Current = nullptr; QLineEdit* Next = nullptr; QLineEdit* Confirm = nullptr;
        if (draft_.HasMaster() && RuntimeStore().HasMaster()) { Current = secretInput(&Dialog, "hc.passwords.oldMaster"); Form->addRow(tr("현재 비밀번호"), Current); }
        if (Enable) { Next = secretInput(&Dialog, "hc.passwords.newMaster"); Confirm = secretInput(&Dialog, "hc.passwords.confirmMaster"); Form->addRow(tr("새 비밀번호"), Next); Form->addRow(tr("새 비밀번호 확인"), Confirm); }
        auto* Note = new QLabel(Enable ? tr("마스터 비밀번호를 잊으면 저장된 비밀번호를 복구할 수 없습니다.") : tr("해제 후에도 현재 Windows 계정으로 저장소를 보호합니다."), &Dialog); Note->setWordWrap(true); Form->addRow(Note);
        auto* Error = new QLabel(&Dialog); Error->setWordWrap(true); Form->addRow(Error);
        auto* Buttons = buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog); Form->addRow(Buttons);
        // The warning was never placed in the form; it is the header subtitle now.
        Note->hide(); hc::ui::ApplyDialogFrame(&Dialog, Buttons, {{}, Note->text(), fm::ui::glyph::Lock});
        connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
        connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&] {
            if (Current) {
                // Reauthenticate against the committed envelope before changing protection.
                auto Check = RuntimeStore(); Check.Lock(); auto Secret = Current->text(); const bool Valid = Check.Unlock(Secret); clear(Secret);
                if (!Valid) { Error->setText(tr("현재 마스터 비밀번호가 맞지 않습니다.")); return; }
            }
            auto Secret = Next ? Next->text() : QString{}; auto Confirmation = Confirm ? Confirm->text() : QString{};
            auto Clear = qScopeGuard([&] { clear(Secret); clear(Confirmation); });
            if (Enable && (Secret.isEmpty() || Secret != Confirmation)) { Error->setText(tr("새 비밀번호와 확인이 일치하는지 확인하세요.")); return; }
            if (!draft_.SetMaster(Secret)) { Error->setText(draft_.Error()); return; } Dialog.accept();
        });
        Dialog.resize(470, Dialog.sizeHint().height()); const bool Accepted = Dialog.exec() == QDialog::Accepted;
        for (auto* Input : {Current, Next, Confirm}) if (Input) wipe(Input);
        if (Accepted) { dirty_ = true; emit Changed(); } refresh();
    }

    ArchivePasswordDialog::ArchivePasswordDialog(QString Archive, unsigned Attempt, QWidget* Parent) : QDialog(Parent), archive_(std::move(Archive))
    {
        setObjectName("hc.archive.password"); setWindowTitle(tr("압축 파일 비밀번호"));
        auto* Rows = dialogRows(*this); Rows->setSpacing(12);
        auto* Title = new QLabel(Attempt > 1 ? tr("비밀번호를 다시 입력하세요") : tr("비밀번호를 입력하세요"), this); auto Font = Title->font(); Font.setBold(true); Title->setFont(Font); Rows->addWidget(Title);
        auto* File = new QLabel(QFileInfo(archive_).fileName(), this); File->setTextFormat(Qt::PlainText); File->setWordWrap(true); Rows->addWidget(File);
        auto* Path = new QLabel(this); Path->setTextFormat(Qt::PlainText); Path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        Path->setText(QDir::toNativeSeparators(archive_)); Path->setWordWrap(true); Path->setMaximumWidth(520); Rows->addWidget(Path);
        auto* Form = new QFormLayout; saved_ = new QComboBox(this); saved_->setObjectName("hc.archive.savedPassword"); Form->addRow(tr("저장된 비밀번호(&S)"), saved_);
        value_ = secretInput(this, "hc.archive.passwordValue"); Form->addRow(tr("비밀번호(&P)"), value_); Rows->addLayout(Form);
        unlock_ = new QPushButton(tr("저장소 잠금 해제…"), this); Rows->addWidget(unlock_);
        scope_ = new QButtonGroup(this);
        int Index = 0; for (const auto& Text : {tr("이번 작업에만 사용"), tr("이번 실행 동안 기억"), tr("비밀번호 관리자에 저장")}) {
            auto* Radio = new QRadioButton(Text, this); Radio->setObjectName("hc.archive.passwordScope" + QString::number(Index)); scope_->addButton(Radio, Index++); Rows->addWidget(Radio);
        }
        scope_->button(0)->setChecked(true); name_ = new QLineEdit(this); name_->setObjectName("hc.archive.passwordName"); name_->setPlaceholderText(tr("저장할 이름")); name_->setText(QFileInfo(archive_).completeBaseName()); name_->setMaxLength(100); name_->hide(); Rows->addWidget(name_);
        auto* Manage = new QPushButton(tr("비밀번호 관리…"), this); Manage->setObjectName("hc.archive.managePasswords"); Manage->setFlat(true); Rows->addWidget(Manage, 0, Qt::AlignLeft);
        error_ = new QLabel(this); error_->setObjectName("hc.archive.passwordError"); error_->setWordWrap(true); error_->hide(); Rows->addWidget(error_);
        if (Attempt > 1) { error_->setText(tr("비밀번호가 맞지 않습니다. 다시 입력하거나 다른 항목을 선택하세요.")); error_->show(); }
        auto* Buttons = buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this); Buttons->button(QDialogButtonBox::Ok)->setText(tr("열기")); Rows->addWidget(Buttons); footer(*this, Buttons);
        connect(Buttons, &QDialogButtonBox::accepted, this, &ArchivePasswordDialog::accept); connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(scope_, &QButtonGroup::idToggled, this, [this](int Id, bool Checked) { if (Checked) name_->setVisible(Id == 2); });
        connect(saved_, &QComboBox::currentIndexChanged, this, [this](int Index) {
            if (Index <= 0) return; auto Text = RuntimeStore().Value(Index - 1); value_->setText(Text); clear(Text);
        });
        connect(unlock_, &QPushButton::clicked, this, [this] { if (UnlockStore(RuntimeStore(), this)) populate(); });
        connect(Manage, &QPushButton::clicked, this, [this] { manage(this, [this] { populate(); }); });
        connect(value_, &QLineEdit::textEdited, this, [this] { const QSignalBlocker Block(saved_); saved_->setCurrentIndex(0); });
        populate(); resize(560, sizeHint().height()); value_->setFocus();
        connect(this, &QDialog::finished, this, [this](int Result) { if (Result != QDialog::Accepted) wipe(value_); });
    }
    void ArchivePasswordDialog::populate()
    {
        const QSignalBlocker Block(saved_); saved_->clear(); saved_->addItem(tr("선택하세요"));
        for (const auto& Entry : RuntimeStore().Entries()) saved_->addItem(Entry.name_);
        saved_->setEnabled(!RuntimeStore().Locked() && !RuntimeStore().Entries().isEmpty()); unlock_->setVisible(RuntimeStore().Locked());
    }
    QString ArchivePasswordDialog::Value() const { return value_->text(); }
    ArchivePasswordDialog::~ArchivePasswordDialog() { wipe(value_); }
    int ArchivePasswordDialog::Scope() const { return scope_->checkedId(); }
    QString ArchivePasswordDialog::EntryName() const { return name_->text().trimmed(); }
    void ArchivePasswordDialog::accept()
    {
        auto Text = Value(); auto Clear = qScopeGuard([&] { clear(Text); });
        if (Text.isEmpty()) { error_->setText(tr("비밀번호를 입력하거나 저장된 항목을 선택하세요.")); error_->show(); value_->setFocus(); return; }
        if (Scope() == 2) {
            auto& Store = RuntimeStore(); if (!UnlockStore(Store, this)) return;
            if (!Store.Error().isEmpty()) { showError(this, Store); return; }
            auto Draft = Store;
            if (!Draft.Add(EntryName(), Text) || !Draft.Save()) { showError(this, Draft); return; }
            Store = Draft;
        }
        QDialog::accept();
    }
    hc::storage::Result<hc::storage::Password> AcquireArchivePassword(const std::string& Archive, unsigned Attempt,
        const hc::storage::OperationContext& Context, QWidget* Parent)
    {
        using namespace hc::storage;
        if (const auto Stop = Context.Check({"passwords"})) return *Stop;
        if (Context.workload_.IsAutomatic()) return Error{ErrorCategory::AccessDenied, "archive-password-required", {"passwords"}};
        const auto Path = QDir::cleanPath(QFileInfo(QString::fromStdString(Archive)).absoluteFilePath()).toCaseFolded();
        auto& Cache = memory(); const auto Candidates = candidates(Archive, Context);
        if (auto Value = candidateValue(Candidates, Attempt)) return std::move(*Value);
        if (Attempt > unsigned(Candidates.size()) + 3)
            return Error{ErrorCategory::AccessDenied, "archive-password-attempts", {"passwords"}};
        ArchivePasswordDialog Dialog(QString::fromStdString(Archive), Attempt > 1 ? 2 : 1, Parent);
        QTimer Timer; QObject::connect(&Timer, &QTimer::timeout, &Dialog, [&] { if (Context.Check({"passwords"})) Dialog.reject(); }); Timer.start(50);
        if (Dialog.exec() != QDialog::Accepted) return Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"passwords"}};
        if (const auto Stop = Context.Check({"passwords"})) return *Stop;
        auto Text = Dialog.Value(); auto Clear = qScopeGuard([&] { clear(Text); });
        auto Bytes = Text.toUtf8(); auto ClearBytes = qScopeGuard([&] { SecureZeroMemory(Bytes.data(), size_t(Bytes.size())); });
        if (Dialog.Scope() == 1 || (Dialog.Scope() == 0 && Context.stopToken_.stop_possible())) {
            Cache.removeIf([&](const Remembered& Value) { return Value.archive_ == Path && (Value.session_ || Value.stop_ == Context.stopToken_); });
            if (Cache.size() >= 256) Cache.removeFirst();
            Cache.append({Path, Store::Protect(Bytes), Context.stopToken_, Dialog.Scope() == 1});
        }
        return Password(std::string(Bytes.constData(), size_t(Bytes.size())));
    }
    std::optional<hc::storage::Password> TryAutomaticArchivePassword(const std::string& Archive, unsigned Attempt,
        const hc::storage::OperationContext& Context)
    {
        if (Context.Check({"passwords"}) || Context.workload_.IsAutomatic()) return {};
        return candidateValue(candidates(Archive, Context), Attempt);
    }
}
