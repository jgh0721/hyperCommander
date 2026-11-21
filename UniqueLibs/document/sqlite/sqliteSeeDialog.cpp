#include "stdafx.h"
#include "sqliteSeeDialog.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteKeyStore.hpp"
#include "sqliteRuntime.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <Windows.h>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;

        const QStringList& FormatLabels()
        {
            static const QStringList Labels{QObject::tr("암호문 (textkey)"), QObject::tr("문자열 (key)"), QObject::tr("16진 (hexkey)")};
            return Labels;
        }
        int FormatIndex(KeyFormat Format) { return Format == KeyFormat::Raw ? 1 : Format == KeyFormat::Hex ? 2 : 0; }
        KeyFormat FormatAt(int Index) { return Index == 1 ? KeyFormat::Raw : Index == 2 ? KeyFormat::Hex : KeyFormat::Text; }
        QString FormatShort(KeyFormat Format)
        {
            return Format == KeyFormat::Raw ? QObject::tr("문자열") : Format == KeyFormat::Hex ? QObject::tr("16진") : QObject::tr("암호문");
        }

        void Tone(QWidget* Widget, T Token)
        {
            auto Palette = Widget->palette();
            Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(Widget)[Token]);
            Widget->setPalette(Palette);
        }

        fm::ui::SegmentedControl* FormatSegments(QWidget* Parent, KeyFormat Format)
        {
            auto* Segments = new fm::ui::SegmentedControl(FormatLabels(), Parent);
            Segments->setObjectName(QStringLiteral("sqliteKeyFormat"));
            Segments->setSegmentSize(fm::ui::SegmentedControl::Small);
            Segments->setCurrentIndex(FormatIndex(Format));
            return Segments;
        }

        // 머리(아이콘 칸 · 제목 · 설명) — 시안의 열쇠 그림 칸.
        QWidget* Head(QWidget* Parent, const QString& Glyph, T IconTone, const QString& Title, QLabel** Subtitle)
        {
            auto* Box = new QWidget(Parent);
            auto* Row = new QHBoxLayout(Box);
            Row->setContentsMargins(0, 0, 0, 0);
            Row->setSpacing(12);
            auto* Icon = new QLabel(Box);
            Icon->setFixedSize(36, 36);
            Icon->setAlignment(Qt::AlignCenter);
            const auto Paint = [Icon, Glyph, IconTone] {
                const auto& Colors = fm::style::themeColorsFor(Icon);
                QPixmap Tile(36 * 2, 36 * 2);
                Tile.setDevicePixelRatio(2);
                Tile.fill(Qt::transparent);
                QPainter Painter(&Tile);
                Painter.setRenderHint(QPainter::Antialiasing);
                Painter.setPen(Qt::NoPen);
                Painter.setBrush(Colors[T::AccentSoft]);
                Painter.drawRoundedRect(QRectF(0, 0, 36, 36), 8, 8);
                Painter.drawPixmap(9, 9, hc::sqlite::Glyph(Glyph, Colors[IconTone], 18).pixmap(18, 18));
                Painter.end();
                Icon->setPixmap(Tile);
            };
            Paint();
            QObject::connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, Icon, Paint);
            Row->addWidget(Icon, 0, Qt::AlignTop);
            auto* Texts = new QVBoxLayout;
            Texts->setSpacing(3);
            auto* Main = new QLabel(Title, Box);
            auto Font = Resized(Main->font(), 2);
            Font.setWeight(QFont::DemiBold);
            Main->setFont(Font);
            Texts->addWidget(Main);
            auto* Sub = new QLabel(Box);
            Sub->setWordWrap(true);
            Tone(Sub, T::Fg2);
            Texts->addWidget(Sub);
            Row->addLayout(Texts, 1);
            if (Subtitle) *Subtitle = Sub;
            return Box;
        }

        QLabel* InfoBox(QWidget* Parent, const QString& Text)
        {
            auto* Label = new QLabel(Text, Parent);
            Label->setWordWrap(true);
            Label->setObjectName(QStringLiteral("sqliteSeeInfo"));
            Label->setContentsMargins(10, 8, 10, 8);
            Label->setFrameShape(QFrame::StyledPanel);
            Label->setAutoFillBackground(true);
            auto Palette = Label->palette();
            const auto& Colors = fm::style::themeColorsFor(Parent);
            Palette.setColor(QPalette::Window, Colors[T::Alt]);
            Palette.setColor(QPalette::WindowText, Colors[T::Fg2]);
            Label->setPalette(Palette);
            return Label;
        }
    }

    // ── 키 칸 ──────────────────────────────────────────────────────────────

    SecretField::SecretField(const QString& Name, QWidget* Parent) : QWidget(Parent)
    {
        auto* Row = new QHBoxLayout(this);
        Row->setContentsMargins(0, 0, 0, 0);
        Row->setSpacing(4);
        edit_ = new QLineEdit(this);
        edit_->setObjectName(Name);
        edit_->setAccessibleName(Name);
        edit_->setEchoMode(QLineEdit::Password);
        // 키는 실행 취소 기록 · 끌어 놓기 · 입력기 예측에 남기지 않는다.
        edit_->setAttribute(Qt::WA_InputMethodEnabled, false);
        edit_->setDragEnabled(false);
        Row->addWidget(edit_, 1);
        eye_ = new QToolButton(this);
        eye_->setObjectName(Name + QStringLiteral(".reveal"));
        eye_->setCheckable(true);
        eye_->setToolTip(tr("키 보기"));
        eye_->setAccessibleName(tr("키 보기"));
        eye_->setFocusPolicy(Qt::TabFocus);
        const auto Paint = [this] { eye_->setIcon(Glyph(eye_->isChecked() ? QStringLiteral("eyeoff") : QStringLiteral("view"), fm::style::themeColorsFor(this)[T::Fg2], 14)); };
        connect(eye_, &QToolButton::toggled, this, [this, Paint](bool On) {
            edit_->setEchoMode(On ? QLineEdit::Normal : QLineEdit::Password);
            eye_->setToolTip(On ? tr("키 숨기기") : tr("키 보기"));
            eye_->setAccessibleName(eye_->toolTip());
            Paint();
        });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, Paint);
        Paint();
        Row->addWidget(eye_);
    }

    SecretField::~SecretField() { Wipe(); }

    QByteArray SecretField::Take()
    {
        auto Text = edit_->text();
        QByteArray Bytes = Text.toUtf8();
        Text.fill(QChar(0));
        Wipe();
        return Bytes;
    }

    void SecretField::Wipe()
    {
        if (!edit_) return;
        // QLineEdit 의 글을 같은 길이의 빈 글로 덮고 지운다(실행 취소 기록도 함께).
        edit_->setText(QString(edit_->text().size(), QChar(u' ')));
        edit_->clear();
        edit_->setModified(false);
    }

    void SecretField::SetError(bool Error)
    {
        fm::style::setInvalid(edit_, Error);
    }

    // ── 키 대화 ────────────────────────────────────────────────────────────

    SeeKeyDialog::SeeKeyDialog(const QString& Path, KeyFormat Format, bool Save, bool ReadOnly, Attempt Try, QWidget* Parent)
        : QDialog(Parent), try_(std::move(Try)), fileName_(QFileInfo(Path).fileName())
    {
        setObjectName(QStringLiteral("sqliteSeeKeyDialog"));
        setWindowTitle(tr("암호화된 데이터베이스 열기"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(20, 16, 20, 12);
        Layout->setSpacing(11);
        Layout->addWidget(Head(this, QStringLiteral("lock"), T::AccentFg, tr("키를 넣어 주세요"), &subtitle_));
        subtitle_->setText(tr("%1 는 SQLite 머리말이 없고 16–23 바이트가 SQLite 규칙과 맞습니다. SEE 로 암호화한 파일로 보입니다.").arg(fileName_));

        error_ = new QWidget(this);
        error_->setObjectName(QStringLiteral("sqliteSeeError"));
        auto* ErrorRow = new QHBoxLayout(error_);
        ErrorRow->setContentsMargins(10, 8, 10, 8);
        ErrorRow->setSpacing(8);
        auto* ErrorIcon = new QLabel(error_);
        ErrorIcon->setPixmap(Glyph(QStringLiteral("err"), fm::style::themeColorsFor(this)[T::Danger], 14).pixmap(14, 14));
        ErrorRow->addWidget(ErrorIcon, 0, Qt::AlignTop);
        errorText_ = new QLabel(error_);
        errorText_->setObjectName(QStringLiteral("sqliteSeeErrorText"));
        errorText_->setWordWrap(true);
        errorText_->setTextFormat(Qt::RichText);
        ErrorRow->addWidget(errorText_, 1);
        error_->setAutoFillBackground(true);
        {
            auto Palette = error_->palette();
            Palette.setColor(QPalette::Window, fm::style::themeColorsFor(this)[T::DangerBg]);
            error_->setPalette(Palette);
        }
        error_->hide();
        Layout->addWidget(error_);

        auto* Form = new QGridLayout;
        Form->setHorizontalSpacing(10);
        Form->setVerticalSpacing(9);
        Form->setColumnMinimumWidth(0, 70);
        const auto Name = [this](const QString& Text) { auto* Label = new QLabel(Text, this); Tone(Label, T::Fg2); return Label; };
        key_ = new SecretField(QStringLiteral("sqliteSeeKey"), this);
        Form->addWidget(Name(tr("키")), 0, 0);
        Form->addWidget(key_, 0, 1);
        format_ = FormatSegments(this, Format);
        Form->addWidget(Name(tr("키 형식")), 1, 0);
        Form->addWidget(format_, 1, 1);
        auto* FormatNote = new QLabel(tr("암호문은 해시로 키를 만듭니다(textkey). 같은 글자라도 형식이 다르면 다른 키입니다."), this);
        FormatNote->setWordWrap(true);
        Tone(FormatNote, T::Fg3);
        Form->addWidget(FormatNote, 2, 1);
        auto* Algorithm = new QPushButton(tr("AES-256 (이 SEE 는 한 가지만 씀)"), this);
        Algorithm->setObjectName(QStringLiteral("sqliteSeeAlgorithm"));
        Algorithm->setEnabled(false);
        Form->addWidget(Name(tr("알고리즘")), 3, 0);
        Form->addWidget(Algorithm, 3, 1);
        Layout->addLayout(Form);

        auto* Checks = new QVBoxLayout;
        Checks->setContentsMargins(80, 0, 0, 0);
        Checks->setSpacing(7);
        save_ = new QCheckBox(tr("키를 이 PC 에 저장 (DPAPI · 현재 사용자)"), this);
        save_->setObjectName(QStringLiteral("sqliteSeeSave"));
        save_->setChecked(Save);
        readOnly_ = new QCheckBox(tr("읽기 전용으로 열기"), this);
        readOnly_->setObjectName(QStringLiteral("sqliteSeeReadOnly"));
        readOnly_->setChecked(ReadOnly);
        Checks->addWidget(save_);
        Checks->addWidget(readOnly_);
        Layout->addLayout(Checks);
        Layout->addStretch(1);

        auto* Foot = new QHBoxLayout;
        Foot->addStretch(1);
        auto* Cancel = new QPushButton(tr("취소"), this);
        open_ = new QPushButton(tr("열기"), this);
        open_->setObjectName(QStringLiteral("sqliteSeeOpen"));
        open_->setDefault(true);
        fm::style::setButtonRole(open_, fm::style::ButtonRole::Primary);
        Foot->addWidget(Cancel);
        Foot->addWidget(open_);
        Layout->addLayout(Foot);
        connect(Cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(open_, &QPushButton::clicked, this, &SeeKeyDialog::accept);
        connect(key_->Edit(), &QLineEdit::textEdited, this, [this] { key_->SetError(false); });
        connect(format_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this] { key_->SetError(false); });
        connect(this, &QDialog::finished, this, [this] { key_->Wipe(); });
        resize(520, 470);
        key_->Edit()->setFocus();
    }

    KeyFormat SeeKeyDialog::Format() const { return FormatAt(format_->currentIndex()); }
    void SeeKeyDialog::SetFormat(KeyFormat Format) { format_->setCurrentIndex(FormatIndex(Format)); }
    bool SeeKeyDialog::SaveKey() const { return save_->isChecked(); }
    bool SeeKeyDialog::ReadOnly() const { return readOnly_->isChecked(); }
    bool SeeKeyDialog::ShowingError() const { return !error_->isHidden(); }

    QByteArray SeeKeyDialog::TakeKey()
    {
        auto Key = accepted_;
        WipeKey(accepted_);
        return Key;
    }

    void SeeKeyDialog::showError(const Outcome& Result, KeyFormat Format)
    {
        subtitle_->setText(tr("%1 — SEE 암호화 추정").arg(fileName_));
        if (Result.wrongKey_)
            errorText_->setText(tr("<b>키가 맞지 않습니다.</b> %1 형식 · AES-256 으로 열지 못했습니다. 키를 만든 프로그램이 다른 형식(암호문 · 문자열 · 16진)을 썼다면 "
                                   "형식을 바꿔 보세요. AES-256 이 아닌 SEE 알고리즘으로 만든 파일이면 열지 못할 수 있습니다.").arg(FormatShort(Format)));
        else errorText_->setText(tr("<b>열지 못했습니다.</b> %1").arg(Result.error_.toHtmlEscaped()));
        error_->show();
        key_->SetError(true);
        open_->setText(tr("다시 시도"));
        key_->Edit()->selectAll();
        key_->Edit()->setFocus();
    }

    void SeeKeyDialog::accept()
    {
        const auto Format = this->Format();
        auto Key = key_->Edit()->text().toUtf8();
        if (Key.isEmpty()) { showError({false, false, tr("키를 넣어 주세요.")}, Format); return; }
        if (Format == KeyFormat::Hex && !ValidHexKey(Key)) {
            WipeKey(Key);
            showError({false, false, tr("16진 키는 0–9 · A–F 글자를 짝수 개 넣어야 합니다.")}, Format);
            return;
        }
        const auto Result = try_ ? try_(Key, Format, ReadOnly()) : Outcome{true};
        if (!Result.ok_) { WipeKey(Key); showError(Result, Format); return; }
        accepted_ = Key;
        WipeKey(Key);
        key_->Wipe();
        QDialog::accept();
    }

    // ── 암호화 대화 ────────────────────────────────────────────────────────

    SeeEncryptDialog::SeeEncryptDialog(const QString& Path, bool Encrypted, KeyFormat Current, KeyFormat DefaultFormat, bool Backup, QWidget* Parent)
        : QDialog(Parent), encrypted_(Encrypted), backupPath_(BackupPathFor(Path))
    {
        setObjectName(QStringLiteral("sqliteSeeEncryptDialog"));
        const QFileInfo Info(Path);
        setWindowTitle(tr("암호화 — %1").arg(Info.fileName()));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(20, 16, 20, 12);
        Layout->setSpacing(11);
        QLabel* Subtitle = nullptr;
        Layout->addWidget(Head(this, QStringLiteral("lock"), T::AccentFg, Encrypted ? tr("암호화 설정") : tr("암호화하기"), &Subtitle));
        Subtitle->setText(Encrypted ? tr("현재: SEE · AES-256 · %1").arg(KeyFormatLabel(Current)) : tr("현재: 암호화 없음 (평문 파일)"));

        auto* Options = new QVBoxLayout;
        Options->setSpacing(6);
        const auto Option = [&](const QString& Label, const QString& Hint, const QString& Name) {
            auto* Radio = new QRadioButton(QStringLiteral("%1 — %2").arg(Label, Hint), this);
            Radio->setObjectName(Name);
            Options->addWidget(Radio);
            return Radio;
        };
        if (Encrypted) {
            change_ = Option(tr("키 바꾸기"), tr("같은 파일을 새 키로 다시 씀(rekey)"), QStringLiteral("sqliteSeeChange"));
            decrypt_ = Option(tr("암호화 해제"), tr("빈 키로 rekey, 평문 파일이 됨"), QStringLiteral("sqliteSeeDecrypt"));
            change_->setChecked(true);
            Layout->addLayout(Options);
        }

        auto* Form = new QGridLayout;
        Form->setHorizontalSpacing(8);
        Form->setVerticalSpacing(9);
        Form->setColumnMinimumWidth(0, 70);
        const auto Name = [this](const QString& Text) { auto* Label = new QLabel(Text, this); Tone(Label, T::Fg2); return Label; };
        key_ = new SecretField(QStringLiteral("sqliteSeeNewKey"), this);
        confirm_ = new SecretField(QStringLiteral("sqliteSeeConfirmKey"), this);
        key_->Edit()->setPlaceholderText(tr("새 키"));
        confirm_->Edit()->setPlaceholderText(tr("새 키 확인"));
        Form->addWidget(Name(tr("새 키")), 0, 0);
        Form->addWidget(key_, 0, 1);
        Form->addWidget(confirm_, 0, 2);
        format_ = FormatSegments(this, DefaultFormat);
        Form->addWidget(Name(tr("키 형식")), 1, 0);
        Form->addWidget(format_, 1, 1, 1, 2);
        Layout->addLayout(Form);
        backup_ = new QCheckBox(tr("먼저 백업 사본 만들기 (%1)").arg(QFileInfo(backupPath_).fileName()), this);
        backup_->setObjectName(QStringLiteral("sqliteSeeBackup"));
        backup_->setChecked(Backup);
        auto* BackupRow = new QHBoxLayout;
        BackupRow->setContentsMargins(78, 0, 0, 0);
        BackupRow->addWidget(backup_);
        Layout->addLayout(BackupRow);
        Layout->addWidget(InfoBox(this, tr("모든 페이지를 다시 씁니다(%1). 커밋하지 않은 변경은 먼저 커밋해야 하고, 다른 프로그램이 파일을 열고 있으면 실패합니다. "
                                           "저장한 키도 새 키로 바꾸고, 암호화를 해제하면 저장한 키를 지웁니다.")
                                            .arg(QLocale().formattedDataSize(Info.size(), 1, QLocale::DataSizeTraditionalFormat))));
        problem_ = new QLabel(this);
        problem_->setObjectName(QStringLiteral("sqliteSeeProblem"));
        problem_->setWordWrap(true);
        Tone(problem_, T::Danger);
        Layout->addWidget(problem_);
        Layout->addStretch(1);

        auto* Foot = new QHBoxLayout;
        Foot->addStretch(1);
        auto* Cancel = new QPushButton(tr("취소"), this);
        apply_ = new QPushButton(tr("적용"), this);
        apply_->setObjectName(QStringLiteral("sqliteSeeApply"));
        apply_->setDefault(true);
        fm::style::setButtonRole(apply_, fm::style::ButtonRole::Primary);
        Foot->addWidget(Cancel);
        Foot->addWidget(apply_);
        Layout->addLayout(Foot);
        connect(Cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(apply_, &QPushButton::clicked, this, &SeeEncryptDialog::accept);
        for (auto* Radio : {change_, decrypt_}) if (Radio) connect(Radio, &QRadioButton::toggled, this, &SeeEncryptDialog::refresh);
        for (auto* Field : {key_, confirm_}) connect(Field->Edit(), &QLineEdit::textChanged, this, &SeeEncryptDialog::refresh);
        connect(format_, &fm::ui::SegmentedControl::currentIndexChanged, this, &SeeEncryptDialog::refresh);
        connect(this, &QDialog::finished, this, [this] { key_->Wipe(); confirm_->Wipe(); });
        refresh();
        resize(520, 470);
        key_->Edit()->setFocus();
    }

    SeeEncryptDialog::Operation SeeEncryptDialog::Chosen() const
    {
        if (!encrypted_) return Operation::Encrypt;
        return decrypt_->isChecked() ? Operation::Decrypt : Operation::Change;
    }

    void SeeEncryptDialog::Choose(Operation Value)
    {
        if (!encrypted_) return;
        (Value == Operation::Decrypt ? decrypt_ : change_)->setChecked(true);
        refresh();
    }

    KeyFormat SeeEncryptDialog::Format() const { return FormatAt(format_->currentIndex()); }
    void SeeEncryptDialog::SetFormat(KeyFormat Format) { format_->setCurrentIndex(FormatIndex(Format)); refresh(); }
    bool SeeEncryptDialog::Backup() const { return backup_->isChecked(); }

    QString SeeEncryptDialog::Problem() const
    {
        if (Chosen() == Operation::Decrypt) return {};
        const auto Key = key_->Edit()->text(), Confirm = confirm_->Edit()->text();
        if (Key.isEmpty()) return tr("새 키를 넣어 주세요.");
        if (Key != Confirm) return Confirm.isEmpty() ? tr("새 키를 한 번 더 넣어 주세요.") : tr("두 칸의 키가 다릅니다.");
        if (Format() == KeyFormat::Hex && !ValidHexKey(Key.toUtf8())) return tr("16진 키는 0–9 · A–F 글자를 짝수 개 넣어야 합니다.");
        return {};
    }

    void SeeEncryptDialog::refresh()
    {
        const bool Decrypt = Chosen() == Operation::Decrypt;
        key_->setEnabled(!Decrypt);
        confirm_->setEnabled(!Decrypt);
        format_->setEnabled(!Decrypt);
        const auto Text = Problem();
        // 입력 중에는 빈 칸을 오류로 보이지 않는다(적용을 눌렀을 때만).
        problem_->setText(key_->Edit()->text().isEmpty() && confirm_->Edit()->text().isEmpty() ? QString() : Text);
        apply_->setText(Decrypt ? tr("해제") : tr("적용"));
    }

    QByteArray SeeEncryptDialog::TakeKey()
    {
        auto Key = taken_;
        WipeKey(taken_);
        return Key;
    }

    void SeeEncryptDialog::accept()
    {
        if (const auto Text = Problem(); !Text.isEmpty()) { problem_->setText(Text); return; }
        if (Chosen() != Operation::Decrypt) taken_ = key_->Take();
        key_->Wipe();
        confirm_->Wipe();
        QDialog::accept();
    }

    // ── 도우미 ─────────────────────────────────────────────────────────────

    bool ValidHexKey(const QByteArray& Key)
    {
        const auto Trimmed = Key.trimmed();
        if (Trimmed.isEmpty() || Trimmed.size() % 2 != 0) return false;
        for (const char Ch : Trimmed) if (!std::isxdigit(static_cast<unsigned char>(Ch))) return false;
        return true;
    }

    bool ChangeEncryption(Connection& Db, const QByteArray& Key, KeyFormat Format, QString* Error)
    {
        if (Runtime::See() != SeeState::Available) { if (Error) *Error = Runtime::SeeText(Runtime::See()); return false; }
        if (sqlite3_get_autocommit(Db.Handle()) == 0) { if (Error) *Error = QObject::tr("열린 트랜잭션이 있습니다. 먼저 커밋하거나 롤백하세요."); return false; }
        const int Rc = Db.Rekey(Key, Format);
        if (Rc != SQLITE_OK) {
            if (Error) *Error = Rc == SQLITE_BUSY || Rc == SQLITE_LOCKED ? QObject::tr("다른 연결 · 프로그램이 파일을 쓰고 있어 바꾸지 못했습니다.")
                                                                         : QString::fromUtf8(sqlite3_errstr(Rc)) + QStringLiteral(" — ") + Db.ErrorMessage();
            return false;
        }
        // 바꾼 키로 다시 읽히는지 본다.
        QString Problem;
        if (Db.Probe(&Problem) != SQLITE_OK) { if (Error) *Error = Problem; return false; }
        // WAL 이면 다시 쓴 페이지가 아직 -wal 에만 있고 본 파일에는 옛 페이지(평문 · 옛 키)가 남는다(2026-11-14 확인).
        // 곧바로 본 파일로 옮기고 -wal 을 비운다.
        Statement Mode(Db, QStringLiteral("PRAGMA main.journal_mode"));
        if (Mode.Valid() && Mode.Step() == SQLITE_ROW && Mode.Value(0).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0) {
            Statement Checkpoint(Db, QStringLiteral("PRAGMA main.wal_checkpoint(TRUNCATE)"));
            if (!Checkpoint.Valid() || Checkpoint.Step() != SQLITE_ROW || Checkpoint.Value(0).toInt() != 0) {
                if (Error) *Error = QObject::tr("키는 바꿨지만 WAL 체크포인트를 끝내지 못했습니다. 다른 연결이 닫힌 뒤 데이터베이스 정보 › WAL 체크포인트를 실행하세요.");
                return true;
            }
        }
        return true;
    }

    QString BackupPathFor(const QString& Path)
    {
        const auto Base = QFileInfo(Path).absoluteFilePath() + QStringLiteral(".bak");
        if (!QFileInfo::exists(Base)) return Base;
        for (int Index = 2; Index < 1000; ++Index)
            if (const auto Next = Base + QString::number(Index); !QFileInfo::exists(Next)) return Next;
        return Base + QString::number(QDateTime::currentMSecsSinceEpoch());
    }

    bool CopyDatabaseFile(Connection& Db, const QString& Target, QString* Error)
    {
        // WAL 의 내용을 본 파일로 옮긴 뒤 바이트 그대로 복사한다(암호화된 파일이면 같은 키로 열린다).
        {
            Statement Mode(Db, QStringLiteral("PRAGMA main.journal_mode"));
            if (Mode.Valid() && Mode.Step() == SQLITE_ROW && Mode.Value(0).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0) {
                Statement Checkpoint(Db, QStringLiteral("PRAGMA main.wal_checkpoint(TRUNCATE)"));
                if (Checkpoint.Valid()) Checkpoint.Step();
            }
        }
        const auto Source = QDir::toNativeSeparators(QFileInfo(Db.Path()).absoluteFilePath()).toStdWString();
        const auto Destination = QDir::toNativeSeparators(QFileInfo(Target).absoluteFilePath()).toStdWString();
        if (!CopyFileW(Source.c_str(), Destination.c_str(), TRUE)) {
            if (Error) *Error = QObject::tr("백업 사본을 만들지 못했습니다: %1").arg(qt_error_string(int(GetLastError())));
            return false;
        }
        return true;
    }
}
