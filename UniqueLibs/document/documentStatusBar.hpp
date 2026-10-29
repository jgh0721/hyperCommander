#pragma once
#include <QWidget>
#include <QLabel>
#include <QToolButton>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QStringList>
#include <QList>
#include <functional>

namespace hc::document
{
    struct DocumentStatusField
    {
        QString id_, text_, compact_, detail_, command_;
        bool essential_ = true, trailing_ = false;
        bool operator==(const DocumentStatusField&) const = default;
    };
    // A measured layout: optional fields are ordered from highest to lowest priority.
    // Core fields never disappear; exceptionally narrow windows use another row.
    class DocumentStatusBar final : public QWidget
    {
    public:
        explicit DocumentStatusBar(QWidget* Parent = nullptr) : QWidget(Parent)
        {
            setObjectName("documentStatusBar");
            setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            setMinimumHeight(30);
        }
        std::function<void(const QString&)> activated_;
        std::function<void()> returnFocus_;
        void SetFields(const QList<DocumentStatusField>& Fields)
        {
            if (fields_ == Fields) return;
            fields_ = Fields;
            // Reuse controls so cursor-only updates preserve keyboard focus.
            while (items_.size() > Fields.size()) delete items_.takeLast();
            for (qsizetype I = 0; I < Fields.size(); ++I) {
                const auto& Field = Fields[I];
                const bool Button = !Field.command_.isEmpty();
                if (I < items_.size() && bool(qobject_cast<QToolButton*>(items_[I])) != Button) {
                    delete items_[I]; items_[I] = nullptr;
                }
                if (I == items_.size()) items_.append(nullptr);
                if (!items_[I]) {
                    if (Button) {
                        auto* Control = new QToolButton(this); Control->setAutoRaise(true);
                        Control->setFocusPolicy(Qt::StrongFocus);
                        connect(Control, &QToolButton::clicked, this, [this, Control] {
                            const auto Command = Control->property("statusCommand").toString();
                            if (activated_) activated_(Command);
                        });
                        items_[I] = Control;
                    } else {
                        auto* Label = new QLabel(this); Label->setTextFormat(Qt::PlainText);
                        Label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft); items_[I] = Label;
                    }
                    items_[I]->installEventFilter(this);
                }
                auto* Item = items_[I]; Item->setObjectName("status." + Field.id_);
                Item->setProperty("statusCommand", Field.command_);
                Item->setAccessibleName(Field.text_);
                Item->setToolTip(Field.detail_.isEmpty() ? Field.text_ : Field.detail_);
            }
            QStringList Details; for (const auto& Field : Fields) Details << (Field.detail_.isEmpty() ? Field.text_ : Field.detail_);
            setToolTip(Details.join("\n")); arrange();
        }
        QList<DocumentStatusField> Fields() const { return fields_; }
        QSize sizeHint() const override { return {640, height()}; }
    protected:
        void resizeEvent(QResizeEvent* Event) override { QWidget::resizeEvent(Event); arrange(); }
        void changeEvent(QEvent* Event) override
        {
            QWidget::changeEvent(Event);
            if (Event->type() == QEvent::FontChange || Event->type() == QEvent::StyleChange) arrange();
        }
        bool eventFilter(QObject* Object, QEvent* Event) override
        {
            if (Event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape) {
                if (returnFocus_) returnFocus_(); return true;
            }
            return QWidget::eventFilter(Object, Event);
        }
        void paintEvent(QPaintEvent*) override
        {
            QPainter Painter(this); Painter.setPen(palette().color(QPalette::Mid));
            Painter.drawLine(0, 0, width(), 0);
            for (auto* Item : items_) if (!Item->isHidden())
                Painter.drawLine(Item->geometry().right(), Item->y() + 6, Item->geometry().right(), Item->geometry().bottom() - 6);
        }
    private:
        void arrange()
        {
            if (arranging_) return;
            arranging_ = true;
            const auto Metrics = fontMetrics(); const int Row = qMax(30, Metrics.height() + 12), Available = qMax(1, width() - 12);
            QList<int> Widths; QStringList Texts; QList<bool> Visible;
            auto Measure = [&](bool Compact, bool LimitNames) {
                Widths.clear(); Texts.clear(); Visible.clear(); int Total = 0;
                for (const auto& Field : fields_) {
                    auto Text = Compact && !Field.compact_.isEmpty() ? Field.compact_ : Field.text_;
                    if (LimitNames && !Field.essential_) Text = Metrics.elidedText(Text, Qt::ElideMiddle, 160);
                    const int Width = Metrics.horizontalAdvance(Text) + 18;
                    Texts << Text; Widths << Width; Visible << true; Total += Width;
                }
                return Total;
            };
            int Total = Measure(false, false);
            if (Total > Available) Total = Measure(false, true);
            if (Total > Available) Total = Measure(true, true);
            for (qsizetype I = fields_.size(); I-- > 0 && Total > Available;)
                if (!fields_[I].essential_) { Visible[I] = false; Total -= Widths[I]; }
            int X = 6, Y = 1;
            for (qsizetype I = 0; I < items_.size(); ++I) {
                auto* Item = items_[I];
                if (!Visible[I]) {
                    if (Item->hasFocus() && returnFocus_) returnFocus_();
                    Item->hide(); continue;
                }
                if (fields_[I].trailing_ && Total <= Available) {
                    int Tail = 0; for (qsizetype J = I; J < items_.size(); ++J) if (Visible[J]) Tail += Widths[J];
                    X = qMax(X, width() - 6 - Tail);
                }
                if (X > 6 && X + Widths[I] > width() - 6) { X = 6; Y += Row; }
                if (auto* Button = qobject_cast<QToolButton*>(Item)) Button->setText(Texts[I]);
                else static_cast<QLabel*>(Item)->setText(Texts[I]);
                Item->setGeometry(X, Y, Widths[I], Row - 1); Item->show(); X += Widths[I];
            }
            const int Height = Y + Row;
            if (minimumHeight() != Height) { setFixedHeight(Height); updateGeometry(); }
            arranging_ = false; update();
        }
        QList<DocumentStatusField> fields_;
        QList<QWidget*> items_;
        bool arranging_ = false;
    };
}
