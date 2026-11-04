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
#include <QCoreApplication>
#include <QStyleOption>
#include <QStylePainter>
#include <functional>

namespace hc::document
{
    struct DocumentStatusField
    {
        QString id_, text_, compact_, detail_, command_;
        bool essential_ = true, trailing_ = false;
        // A picker opens a popover through DocumentStatusBar::pickerRequested_ instead of running command_.
        // Kept last so existing aggregate initialisations keep their meaning.
        bool picker_ = false;
        bool operator==(const DocumentStatusField&) const = default;
    };
    // A status field that runs a command or opens a picker. A picker keeps room for its '⌄' cue in
    // the measured width, so showing the cue on hover or focus never moves another field.
    class DocumentStatusButton final : public QToolButton
    {
    public:
        using QToolButton::QToolButton;
        static constexpr char16_t PickerGlyph = u'⌄';
        // Horizontal padding on each side; DocumentStatusBar measures text + 2 * Padding.
        static constexpr int Padding = 9;
        // The prototype draws the cue at 10 px beside 12 px text (.sf.pick::after).
        static QFont GlyphFont(QFont Font)
        {
            if (Font.pointSizeF() > 0) Font.setPointSizeF(Font.pointSizeF() * 10.0 / 12.0);
            else Font.setPixelSize(qMax(1, Font.pixelSize() * 10 / 12));
            return Font;
        }
        // Width a picker adds to its field: the 4 px gap and the cue.
        static int PickerReserve(const QFont& Font) { return 4 + QFontMetrics(GlyphFont(Font)).horizontalAdvance(QChar(PickerGlyph)); }
        bool IsPicker() const { return picker_; }
        void SetPicker(bool Picker) { if (picker_ == Picker) return; picker_ = Picker; update(); }
        bool IsGlyphVisible() const { return picker_ && isEnabled() && (underMouse() || hasFocus()); }
    protected:
        void paintEvent(QPaintEvent* Event) override
        {
            if (!picker_) { QToolButton::paintEvent(Event); return; }
            QStylePainter Painter(this);
            QStyleOptionToolButton Option; initStyleOption(&Option);
            // The panel (hover, focus) covers the whole field; the label stays centred left of the cue.
            auto Panel = Option; Panel.text.clear(); Panel.icon = QIcon();
            Painter.drawComplexControl(QStyle::CC_ToolButton, Panel);
            const int Frame = style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &Option, this);
            auto Label = Option; Label.rect = rect().adjusted(Frame, Frame, -Frame - PickerReserve(font()), -Frame);
            Painter.drawControl(QStyle::CE_ToolButtonLabel, Label);
            if (!IsGlyphVisible()) return;
            const QFont Glyph = GlyphFont(font()); const QFontMetricsF Metrics(Glyph); const QString Text{QChar(PickerGlyph)};
            // Centre the ink, not the line box: the arrowhead sits low in its cell (the prototype lifts it 5 px).
            const QRectF Ink = Metrics.tightBoundingRect(Text);
            auto Color = Option.palette.color(QPalette::ButtonText); Color.setAlphaF(Color.alphaF() * 0.8);
            Painter.setFont(Glyph); Painter.setPen(Color);
            Painter.drawText(QPointF(width() - Padding - Metrics.horizontalAdvance(Text), height() / 2.0 - Ink.center().y()), Text);
        }
    private:
        bool picker_ = false;
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
        // Picker fields: the field id and its button, which anchors the popover.
        std::function<void(const QString& Id, QWidget* Anchor)> pickerRequested_;
        std::function<void()> returnFocus_;
        void SetFields(const QList<DocumentStatusField>& Fields)
        {
            if (fields_ == Fields) return;
            fields_ = Fields;
            // Reuse controls so cursor-only updates preserve keyboard focus.
            while (items_.size() > Fields.size()) delete items_.takeLast();
            for (qsizetype I = 0; I < Fields.size(); ++I) {
                const auto& Field = Fields[I];
                const bool Button = !Field.command_.isEmpty() || Field.picker_;
                if (I < items_.size() && bool(qobject_cast<QToolButton*>(items_[I])) != Button) {
                    delete items_[I]; items_[I] = nullptr;
                }
                if (I == items_.size()) items_.append(nullptr);
                if (!items_[I]) {
                    if (Button) {
                        auto* Control = new DocumentStatusButton(this); Control->setAutoRaise(true);
                        Control->setFocusPolicy(Qt::StrongFocus);
                        connect(Control, &QToolButton::clicked, this, [this, Control] { trigger(Control); });
                        items_[I] = Control;
                    } else {
                        auto* Label = new QLabel(this); Label->setTextFormat(Qt::PlainText);
                        Label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft); items_[I] = Label;
                    }
                    items_[I]->installEventFilter(this);
                }
                auto* Item = items_[I]; Item->setObjectName("status." + Field.id_);
                Item->setProperty("statusId", Field.id_);
                Item->setProperty("statusCommand", Field.command_);
                if (auto* Control = dynamic_cast<DocumentStatusButton*>(Item)) Control->SetPicker(Field.picker_);
                Item->setAccessibleName(Field.text_);
                Item->setAccessibleDescription(Field.picker_
                    ? QCoreApplication::translate("hc::document::DocumentStatusBar", "눌러서 변경") : QString());
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
            if (Event->type() == QEvent::KeyPress) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (Key->key() == Qt::Key_Escape) {
                    if (returnFocus_) returnFocus_(); return true;
                }
                // Enter runs a command field or opens a picker, as Space and a click do.
                if ((Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) && !(Key->modifiers() & ~Qt::KeypadModifier))
                    if (auto* Control = qobject_cast<QToolButton*>(Object)) { if (!Key->isAutoRepeat()) Control->click(); return true; }
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
        void trigger(DocumentStatusButton* Control)
        {
            if (Control->IsPicker() && pickerRequested_) { pickerRequested_(Control->property("statusId").toString(), Control); return; }
            // A host that does not offer pickers still gets the field's command, so the field never goes dead.
            const auto Command = Control->property("statusCommand").toString();
            if (activated_ && !Command.isEmpty()) activated_(Command);
        }
        void arrange()
        {
            if (arranging_) return;
            arranging_ = true;
            const auto Metrics = fontMetrics(); const int Row = qMax(30, Metrics.height() + 12), Available = qMax(1, width() - 12);
            const int Reserve = DocumentStatusButton::PickerReserve(font());
            QList<int> Widths; QStringList Texts; QList<bool> Visible;
            auto Measure = [&](bool Compact, bool LimitNames) {
                Widths.clear(); Texts.clear(); Visible.clear(); int Total = 0;
                for (const auto& Field : fields_) {
                    auto Text = Compact && !Field.compact_.isEmpty() ? Field.compact_ : Field.text_;
                    if (LimitNames && !Field.essential_) Text = Metrics.elidedText(Text, Qt::ElideMiddle, 160);
                    // A picker always reserves its cue, shown or not, so hover and focus keep the layout.
                    const int Width = Metrics.horizontalAdvance(Text) + 2 * DocumentStatusButton::Padding + (Field.picker_ ? Reserve : 0);
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
