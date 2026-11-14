#include "stdafx.h"
#include "sqliteCellDelegate.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteResultModel.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QCache>
#include <QImage>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>

namespace hc::sqlite
{
    namespace
    {
        // 머리 바이트로 이미지 형식을 알아본다(PNG · JPEG · GIF · WebP · BMP).
        QString ImageFormat(const QByteArray& Bytes)
        {
            if (Bytes.startsWith("\x89PNG\r\n\x1a\n")) return QStringLiteral("PNG");
            if (Bytes.startsWith("\xFF\xD8\xFF")) return QStringLiteral("JPEG");
            if (Bytes.startsWith("GIF87a") || Bytes.startsWith("GIF89a")) return QStringLiteral("GIF");
            if (Bytes.size() >= 12 && Bytes.startsWith("RIFF") && Bytes.mid(8, 4) == "WEBP") return QStringLiteral("WebP");
            if (Bytes.startsWith("BM") && Bytes.size() > 26) return QStringLiteral("BMP");
            return {};
        }

        struct Thumbnail { QPixmap pixmap_; QSize size_; };

        // 그릴 때마다 풀지 않게 담아 둔다. 열쇠는 크기와 앞 4 KiB 의 해시(같은 그림이 여러 행에 있어도 한 번).
        const Thumbnail* CachedThumbnail(const QByteArray& Bytes, int Height)
        {
            static QCache<QString, Thumbnail> Cache(256);
            if (Bytes.size() > 4 * 1024 * 1024 || Height < 8) return nullptr;
            const auto Key = QStringLiteral("%1:%2:%3").arg(Bytes.size()).arg(qHash(QByteArrayView(Bytes).first(qMin<qsizetype>(Bytes.size(), 4096)))).arg(Height);
            if (auto* Found = Cache.object(Key)) return Found->pixmap_.isNull() ? nullptr : Found;
            // 풀지 못한 것(머리만 맞는 잘린 그림)도 담아 둔다. 그릴 때마다 다시 풀지 않게.
            const auto Image = QImage::fromData(Bytes);
            auto* Made = Image.isNull() ? new Thumbnail{}
                                        : new Thumbnail{QPixmap::fromImage(Image.scaled(Height * 2, Height, Qt::KeepAspectRatio, Qt::SmoothTransformation)), Image.size()};
            Cache.insert(Key, Made);
            return Made->pixmap_.isNull() ? nullptr : Made;
        }
    }

    CellDelegate::CellDelegate(QObject* Parent) : QStyledItemDelegate(Parent) {}

    void CellDelegate::SetFonts(const QFont& Grid, const QFont& Mono, bool MonoValues)
    {
        grid_ = Grid;
        mono_ = Mono;
        hasGrid_ = true;
        monoValues_ = MonoValues;
    }

    QSize CellDelegate::sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const
    {
        QStyleOptionViewItem Opt(Option);
        if (hasGrid_) Opt.font = grid_;
        auto Size = QStyledItemDelegate::sizeHint(Opt, Index);
        Size.setHeight(qMax(Size.height(), QFontMetrics(Opt.font).height() + 8));
        Size.setWidth(qMin(Size.width(), 360));
        return Size;
    }

    void CellDelegate::paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const
    {
        QStyleOptionViewItem Opt(Option);
        initStyleOption(&Opt, Index);
        if (hasGrid_) Opt.font = grid_;
        QString Text = Opt.text;
        Opt.text.clear();
        const auto& Colors = fm::style::themeColorsFor(Opt.widget);
        using T = fm::style::Token;
        const auto Pending = TableModel::PendingState(Index.data(PendingRole).toInt());
        const bool Selected = Opt.state.testFlag(QStyle::State_Selected);
        // 보류 편집의 바탕(선택되지 않은 칸만): 수정 칸 노랑, 새 행 초록, 지울 행 빨강.
        if (!Selected && Pending != TableModel::PendingState::None) {
            Opt.backgroundBrush = Colors[Pending == TableModel::PendingState::Modified ? T::WarnBg
                                         : Pending == TableModel::PendingState::Inserted ? T::OkBg : T::DangerBg];
        }
        auto* Style = Opt.widget ? Opt.widget->style() : QApplication::style();
        Style->drawControl(QStyle::CE_ItemViewItem, &Opt, Painter, Opt.widget);

        const bool Active = Selected && Opt.state.testFlag(QStyle::State_Active) && Opt.widget && Opt.widget->hasFocus();
        const bool Null = Index.data(NullRole).toBool();
        const bool Unset = Index.data(UnsetRole).toBool();
        const int Storage = Index.data(StorageRole).toInt();
        const bool Numeric = Index.data(NumericRole).toBool() && display_.alignNumbers_;
        if (Null && !Unset) Text = display_.nullText_;

        QFont Font = Opt.font;
        QColor Color = Active ? Opt.palette.color(QPalette::HighlightedText) : Colors[T::Fg];
        if (Null || Unset) { Font.setItalic(true); if (!Active) Color = Colors[T::Fg3]; }
        else if (Storage == SQLITE_BLOB) { if (!Active) Color = Colors[T::Fg2]; if (monoValues_) Font = mono_; }
        else if (Numeric && monoValues_) Font = mono_;
        if (Pending == TableModel::PendingState::Deleted) { Font.setStrikeOut(true); if (!Active) Color = Colors[T::Fg3]; }

        QRect TextRect = Style->subElementRect(QStyle::SE_ItemViewItemText, &Opt, Opt.widget).adjusted(4, 0, -4, 0);
        Painter->save();
        if (Storage == SQLITE_BLOB && display_.blobPreview_) {
            // 이미지 BLOB: 작은 그림 + ‘PNG 24×24 · 1.2 KB’(시안 ‘부품’ 의 셀 표시).
            const auto Bytes = Index.data(ValueRole).toByteArray();
            if (const auto Format = ImageFormat(Bytes); !Format.isEmpty()) {
                if (const auto* Thumb = CachedThumbnail(Bytes, Opt.rect.height() - 6)) {
                    const QRect Target(TextRect.left(), Opt.rect.top() + (Opt.rect.height() - Thumb->pixmap_.height() / int(Thumb->pixmap_.devicePixelRatio())) / 2,
                                       Thumb->pixmap_.width(), Thumb->pixmap_.height());
                    Painter->drawPixmap(Target.topLeft(), Thumb->pixmap_);
                    TextRect.setLeft(Target.right() + 6);
                    Text = QStringLiteral("%1 %2×%3 · %4").arg(Format).arg(Thumb->size_.width()).arg(Thumb->size_.height())
                               .arg(QLocale().formattedDataSize(Bytes.size(), 1, QLocale::DataSizeTraditionalFormat));
                }
            }
        }
        Painter->setFont(Font);
        Painter->setPen(Color);
        const auto Align = Qt::AlignVCenter | (Numeric && !Null && !Unset ? Qt::AlignRight : Qt::AlignLeft);
        Painter->drawText(TextRect, int(Align), QFontMetrics(Font).elidedText(Text, Qt::ElideRight, TextRect.width()));
        if (display_.mismatch_ && Index.data(MismatchRole).toBool()) {
            // 선언 형식과 저장 형식이 다르다 — 오른쪽 위 작은 삼각형(시안 ‘부품’ 보드의 셀 표시).
            const QRectF Cell = Opt.rect;
            QPainterPath Corner;
            Corner.moveTo(Cell.right() - 7, Cell.top());
            Corner.lineTo(Cell.right(), Cell.top());
            Corner.lineTo(Cell.right(), Cell.top() + 7);
            Corner.closeSubpath();
            Painter->setRenderHint(QPainter::Antialiasing);
            Painter->fillPath(Corner, Colors[T::Warn]);
        }
        if (Pending == TableModel::PendingState::Modified && !Selected) {
            // 수정 칸 왼쪽의 진한 선(시안).
            Painter->fillRect(QRect(Opt.rect.left(), Opt.rect.top(), 2, Opt.rect.height()), Colors[T::WarnLine]);
        }
        if (Index.data(ConflictRole).toBool()) {
            Painter->setRenderHint(QPainter::Antialiasing, false);
            Painter->setPen(QPen(Colors[T::Danger], 1.5));
            Painter->setBrush(Qt::NoBrush);
            Painter->drawRect(QRectF(Opt.rect).adjusted(0.75, 0.75, -0.75, -0.75));
        }
        Painter->restore();
    }

    bool CellDelegate::NeedsPanel(const QModelIndex& Index) const
    {
        if (Index.data(StorageRole).toInt() == SQLITE_BLOB) return true;
        const auto Value = Index.data(ValueRole);
        if (!Value.isValid() || Value.isNull()) return false;
        const auto Text = Value.toString();
        return Text.size() > display_.longText_ || Text.contains(QLatin1Char('\n')) || Text.contains(QLatin1Char('\r'));
    }

    QWidget* CellDelegate::createEditor(QWidget* Parent, const QStyleOptionViewItem& Option, const QModelIndex& Index) const
    {
        Q_UNUSED(Option);
        if (NeedsPanel(Index)) { emit PanelEditRequested(Index); return nullptr; }
        auto* Editor = new QLineEdit(Parent);
        Editor->setObjectName(QStringLiteral("sqliteCellEditor"));
        Editor->setFrame(false);
        if (hasGrid_) Editor->setFont(Index.data(NumericRole).toBool() && monoValues_ ? mono_ : grid_);
        Editor->setPlaceholderText(display_.nullText_);
        return Editor;
    }

    void CellDelegate::setEditorData(QWidget* Editor, const QModelIndex& Index) const
    {
        auto* Line = qobject_cast<QLineEdit*>(Editor);
        if (!Line) return;
        const auto Value = Index.data(ValueRole);
        const bool Null = !Value.isValid() || Value.isNull();
        Line->setProperty("sqliteWasNull", Null);
        Line->setText(Null ? QString() : DisplayText(Value));
        Line->selectAll();
    }

    void CellDelegate::setModelData(QWidget* Editor, QAbstractItemModel* Model, const QModelIndex& Index) const
    {
        auto* Line = qobject_cast<QLineEdit*>(Editor);
        if (!Line) return;
        // NULL 칸을 비운 채 두면 NULL 그대로(빈 글로 바꾸려면 값 패널, NULL 로 바꾸려면 Ctrl+Shift+N).
        if (Line->text().isEmpty() && Line->property("sqliteWasNull").toBool()) { Model->setData(Index, QVariant(), Qt::EditRole); return; }
        Model->setData(Index, Line->text(), Qt::EditRole);
    }
}
