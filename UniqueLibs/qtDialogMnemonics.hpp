#pragma once

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTextEdit>
#include <utility>

namespace hc::ui {

// Button mnemonics also work without Alt when focus is outside text entry and
// type-ahead controls. Resolve live buttons so rebuilt prompts keep their keys.
class DialogMnemonics final : public QObject
{
public:
    static void Install(QDialog* Dialog)
    {
        if (!Dialog || Dialog->findChild<QObject*>("hc.dialog.mnemonics", Qt::FindDirectChildrenOnly)) return;
        // Message boxes attach their frame from inside the first show event.
        // Normalize existing buttons now as well as on subsequent shows.
        (new DialogMnemonics(Dialog))->addStandardMnemonics();
    }
    ~DialogMnemonics() override { if (qApp) qApp->removeEventFilter(this); }

protected:
    bool eventFilter(QObject* Object, QEvent* Event) override
    {
        if (!dialog_) return false;
        if (Object == dialog_ && Event->type() == QEvent::Show) addStandardMnemonics();
        if (Event->type() != QEvent::ShortcutOverride && Event->type() != QEvent::KeyPress) return false;
        auto* Target = qobject_cast<QWidget*>(Object);
        if (!Target || Target->window() != dialog_ || !dialog_->isVisible() || !dialog_->isEnabled()
            || QApplication::activePopupWidget()) return false;
        if (auto* Modal = QApplication::activeModalWidget(); Modal && Modal != dialog_) return false;
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if ((Key->modifiers() & ~Qt::ShiftModifier) != Qt::NoModifier || Key->key() < Qt::Key_A || Key->key() > Qt::Key_Z) return false;
        if (editing(Target) || editing(dialog_->focusWidget())) return false;
        QPushButton* Match = nullptr;
        for (auto* Button : dialog_->findChildren<QPushButton*>()) {
            if (Button->window() != dialog_ || !Button->isVisibleTo(dialog_) || !Button->isEnabled()) continue;
            const auto Mnemonic = QKeySequence::mnemonic(Button->text());
            if (Mnemonic.count() != 1 || Mnemonic[0].key() != Key->key()) continue;
            if (Match) return false; // An ambiguous key must not choose an action.
            Match = Button;
        }
        if (!Match) return false;
        Event->accept();
        if (Event->type() == QEvent::KeyPress && !Key->isAutoRepeat()) Match->click();
        return true;
    }

private:
    explicit DialogMnemonics(QDialog* Dialog) : QObject(Dialog), dialog_(Dialog)
    {
        setObjectName("hc.dialog.mnemonics");
        qApp->installEventFilter(this);
    }
    bool editing(QWidget* Widget) const
    {
        if (!Widget || Widget->window() != dialog_) return false;
        for (auto* At = Widget; At && At != dialog_; At = At->parentWidget()) {
            if (auto* Line = qobject_cast<QLineEdit*>(At)) { if (!Line->isReadOnly()) return true; continue; }
            if (auto* Text = qobject_cast<QTextEdit*>(At)) { if (!Text->isReadOnly()) return true; continue; }
            if (auto* Text = qobject_cast<QPlainTextEdit*>(At)) { if (!Text->isReadOnly()) return true; continue; }
            if (qobject_cast<QComboBox*>(At) || qobject_cast<QAbstractSpinBox*>(At) || qobject_cast<QAbstractItemView*>(At)
                || At->testAttribute(Qt::WA_InputMethodEnabled)) return true;
        }
        return false;
    }
    void addStandardMnemonics()
    {
        using Box = QDialogButtonBox;
        const std::pair<Box::StandardButton, char> Keys[] = {
            {Box::Ok, 'O'}, {Box::Open, 'O'}, {Box::Save, 'S'}, {Box::SaveAll, 'A'}, {Box::Cancel, 'C'}, {Box::Close, 'C'},
            {Box::Yes, 'Y'}, {Box::YesToAll, 'A'}, {Box::No, 'N'}, {Box::NoToAll, 'L'}, {Box::Abort, 'A'},
            {Box::Retry, 'R'}, {Box::Ignore, 'I'}, {Box::Apply, 'P'}, {Box::Reset, 'R'}, {Box::RestoreDefaults, 'D'}, {Box::Help, 'H'}};
        for (auto* Buttons : dialog_->findChildren<Box*>()) {
            if (Buttons->window() != dialog_) continue;
            for (const auto& [Which, Key] : Keys)
                if (auto* Button = Buttons->button(Which); Button && QKeySequence::mnemonic(Button->text()).isEmpty())
                    Button->setText(Button->text() + QString("(&%1)").arg(QChar::fromLatin1(Key)));
        }
    }
    QPointer<QDialog> dialog_;
};

} // namespace hc::ui
