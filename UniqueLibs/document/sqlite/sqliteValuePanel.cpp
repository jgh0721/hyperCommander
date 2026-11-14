#include "stdafx.h"
#include "sqliteValuePanel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteResultModel.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;
        enum Mode { TextMode, JsonMode, HexMode, ImageMode };

        QString HexDump(const QByteArray& Bytes)
        {
            // 16바이트씩: 위치 · 16진 · 글자.
            QString Out;
            for (int At = 0; At < Bytes.size(); At += 16) {
                const auto Line = Bytes.mid(At, 16);
                QString Ascii;
                for (const char Ch : Line) Ascii += (Ch >= 0x20 && Ch < 0x7F) ? QChar(Ch) : QChar(0x00B7);
                Out += QStringLiteral("%1  %2  %3\n").arg(At, 8, 16, QLatin1Char('0')).arg(QString::fromLatin1(Line.toHex(' ')).leftJustified(47), Ascii);
                if (Out.size() > 400000) { Out += QStringLiteral("…\n"); break; }
            }
            return Out;
        }

        // 16진 보기에서 고친 글을 바이트로(위치 · 글자 열은 무시하고 16진 숫자만).
        QByteArray ParseHex(const QString& Text, bool* Ok)
        {
            QByteArray Digits;
            for (const auto& Line : Text.split(QLatin1Char('\n'))) {
                auto Body = Line.trimmed();
                if (Body.isEmpty()) continue;
                // ‘00000010  89 50 …  .PNG’ 모양이면 가운데 부분만.
                const auto Parts = Body.split(QStringLiteral("  "), Qt::SkipEmptyParts);
                if (Parts.size() >= 2 && Parts[0].size() == 8) Body = Parts[1];
                for (const auto Ch : Body) if (Ch.isLetterOrNumber()) Digits += char(Ch.toLatin1());
            }
            if (Ok) *Ok = Digits.size() % 2 == 0 && QByteArray::fromHex(Digits).toHex() == Digits.toLower();
            return QByteArray::fromHex(Digits);
        }
    }

    ValuePanel::ValuePanel(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName(QStringLiteral("sqliteValuePanel"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(10, 6, 8, 8);
        Layout->setSpacing(6);
        auto* Head = new QHBoxLayout;
        title_ = new QLabel(this);
        title_->setObjectName(QStringLiteral("sqliteValueTitle"));
        Head->addWidget(title_, 1);
        auto* Close = new QToolButton(this);
        Close->setObjectName(QStringLiteral("sqliteValueClose"));
        Close->setAutoRaise(true);
        Close->setText(QStringLiteral("×"));
        Close->setToolTip(tr("값 패널 닫기"));
        connect(Close, &QToolButton::clicked, this, &ValuePanel::CloseRequested);
        Head->addWidget(Close);
        Layout->addLayout(Head);
        modes_ = new fm::ui::SegmentedControl(QStringList{tr("텍스트"), QStringLiteral("JSON"), tr("16진"), tr("이미지")}, this);
        modes_->setObjectName(QStringLiteral("sqliteValueModes"));
        modes_->setSegmentSize(fm::ui::SegmentedControl::Small);
        modes_->setExpanding(true);
        Layout->addWidget(modes_);
        meta_ = new QLabel(this);
        meta_->setObjectName(QStringLiteral("sqliteValueMeta"));
        meta_->setWordWrap(true);
        Layout->addWidget(meta_);
        pages_ = new QStackedWidget(this);
        editor_ = new QPlainTextEdit(pages_);
        editor_->setObjectName(QStringLiteral("sqliteValueEditor"));
        editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        editor_->installEventFilter(this);
        pages_->addWidget(editor_);
        image_ = new QLabel(pages_);
        image_->setObjectName(QStringLiteral("sqliteValueImage"));
        image_->setAlignment(Qt::AlignCenter);
        image_->setMinimumHeight(80);
        pages_->addWidget(image_);
        Layout->addWidget(pages_, 1);
        auto* Foot = new QHBoxLayout;
        null_ = new QPushButton(tr("NULL로 설정"), this);
        null_->setObjectName(QStringLiteral("sqliteValueNull"));
        apply_ = new QPushButton(tr("적용 (Ctrl+Enter)"), this);
        apply_->setObjectName(QStringLiteral("sqliteValueApply"));
        fm::style::setButtonRole(apply_, fm::style::ButtonRole::Primary);
        Foot->addWidget(null_);
        Foot->addStretch(1);
        Foot->addWidget(apply_);
        Layout->addLayout(Foot);
        connect(modes_, &fm::ui::SegmentedControl::currentIndexChanged, this, &ValuePanel::showMode);
        connect(null_, &QPushButton::clicked, this, [this] { if (editable_ && cell_.isValid()) emit ApplyRequested(cell_, QVariant()); });
        connect(apply_, &QPushButton::clicked, this, [this] {
            bool Ok = false;
            const auto Value = EditedValue(&Ok);
            if (Ok && editable_ && cell_.isValid()) emit ApplyRequested(cell_, Value);
        });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &ValuePanel::applyTheme);
        applyTheme();
        SetCell({});
    }

    void ValuePanel::SetMonoFont(const QFont& Font) { editor_->setFont(Font); }

    void ValuePanel::applyTheme()
    {
        auto Palette = meta_->palette();
        Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(this)[T::Fg3]);
        meta_->setPalette(Palette);
    }

    void ValuePanel::FocusEditor()
    {
        if (mode_ == ImageMode) modes_->setCurrentIndex(TextMode);
        editor_->setFocus(Qt::OtherFocusReason);
    }

    void ValuePanel::SetCell(const QModelIndex& Index)
    {
        cell_ = Index;
        value_ = Index.isValid() ? Index.data(ValueRole) : QVariant();
        editable_ = Index.isValid() && (Index.flags() & Qt::ItemIsEditable);
        const bool Null = !value_.isValid() || value_.isNull();
        const bool Blob = value_.typeId() == QMetaType::QByteArray;
        if (!Index.isValid()) title_->setText(tr("값"));
        else title_->setText(tr("값  %1행 · %2").arg(Index.row() + 1).arg(Index.model()->headerData(Index.column(), Qt::Horizontal).toString()));
        // 이미지 BLOB 은 이미지 보기로, 그 밖은 지난 보기를 지킨다(이미지 보기였으면 텍스트로).
        const bool Picture = Blob && !QImage::fromData(value_.toByteArray()).isNull();
        int Mode = mode_;
        if (Picture) Mode = ImageMode;
        else if (Blob) Mode = HexMode;
        else if (Mode == ImageMode || Mode == HexMode) Mode = TextMode;
        mode_ = -1;
        modes_->setCurrentIndex(Mode);
        showMode(Mode);
        null_->setEnabled(editable_ && !Null);
        apply_->setEnabled(editable_);
        editor_->setReadOnly(!editable_);
    }

    void ValuePanel::showMode(int Mode)
    {
        if (Mode == mode_) return;
        mode_ = Mode;
        refresh();
    }

    void ValuePanel::refresh()
    {
        const bool Null = !value_.isValid() || value_.isNull();
        const bool Blob = value_.typeId() == QMetaType::QByteArray;
        const auto Declared = cell_.isValid() ? cell_.data(DeclaredTypeRole).toString() : QString();
        const int Storage = cell_.isValid() ? cell_.data(StorageRole).toInt() : SQLITE_NULL;
        const QString StorageName = Storage == SQLITE_INTEGER ? QStringLiteral("INTEGER") : Storage == SQLITE_FLOAT ? QStringLiteral("REAL")
                                  : Storage == SQLITE_TEXT ? QStringLiteral("TEXT") : Storage == SQLITE_BLOB ? QStringLiteral("BLOB") : QStringLiteral("NULL");
        QStringList Meta;
        if (!Declared.isEmpty()) Meta.append(Declared);
        Meta.append(tr("저장 형식 %1").arg(StorageName));
        if (Blob) Meta.append(QLocale().formattedDataSize(value_.toByteArray().size(), 1, QLocale::DataSizeTraditionalFormat));
        else if (!Null) Meta.append(tr("%1자").arg(QLocale().toString(value_.toString().size())));
        if (!editable_ && cell_.isValid()) Meta.append(tr("읽기만"));
        meta_->setText(Meta.join(QStringLiteral(" · ")));
        if (mode_ == ImageMode) {
            const auto Picture = QImage::fromData(value_.toByteArray());
            pages_->setCurrentWidget(image_);
            if (Picture.isNull()) { image_->setPixmap({}); image_->setText(tr("이미지가 아닙니다.")); return; }
            image_->setText({});
            image_->setPixmap(QPixmap::fromImage(Picture.size().width() > 480 || Picture.size().height() > 480
                                                     ? Picture.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation) : Picture));
            meta_->setText(meta_->text() + QStringLiteral(" · %1×%2").arg(Picture.width()).arg(Picture.height()));
            return;
        }
        pages_->setCurrentWidget(editor_);
        if (mode_ == HexMode) {
            editor_->setPlainText(HexDump(Blob ? value_.toByteArray() : value_.toString().toUtf8()));
            return;
        }
        if (Null) { editor_->clear(); editor_->setPlaceholderText(QStringLiteral("NULL")); return; }
        const auto Text = Blob ? QString::fromUtf8(value_.toByteArray()) : value_.toString();
        if (mode_ == JsonMode) {
            QJsonParseError Error;
            const auto Document = QJsonDocument::fromJson(Text.toUtf8(), &Error);
            editor_->setPlainText(Error.error == QJsonParseError::NoError ? QString::fromUtf8(Document.toJson(QJsonDocument::Indented)) : Text);
            if (Error.error != QJsonParseError::NoError) meta_->setText(meta_->text() + tr(" · JSON 아님"));
            return;
        }
        editor_->setPlainText(Text);
    }

    QVariant ValuePanel::EditedValue(bool* Ok) const
    {
        if (Ok) *Ok = editable_ && mode_ != ImageMode;
        if (mode_ == HexMode) return ParseHex(editor_->toPlainText(), Ok);
        if (mode_ == JsonMode) {
            // 들여 쓴 JSON 은 줄여서 넣는다(원래 줄인 모양이었을 때가 많다). JSON 이 아니면 글 그대로.
            const auto Text = editor_->toPlainText();
            const auto Document = QJsonDocument::fromJson(Text.toUtf8());
            return Document.isNull() ? QVariant(Text) : QVariant(QString::fromUtf8(Document.toJson(QJsonDocument::Compact)));
        }
        return editor_->toPlainText();
    }

    bool ValuePanel::eventFilter(QObject* Watched, QEvent* Event)
    {
        if (Watched == editor_ && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            if ((Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) && (Key->modifiers() & Qt::ControlModifier)) {
                apply_->click();
                return true;
            }
        }
        return QWidget::eventFilter(Watched, Event);
    }
}
