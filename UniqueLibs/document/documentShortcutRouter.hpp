#pragma once
#include "documentShortcutRegistry.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
namespace hc::document {
// One owner arbitrates Qt shortcuts and direct editor/search-field key handling.
// QAction keeps its displayed key, but WidgetShortcut prevents a second dispatch.
class DocumentShortcutRouter final : public QObject {
public:
    explicit DocumentShortcutRouter(QWidget* Scope) : QObject(Scope), scope_(Scope)
    {
        timeout_.setSingleShot(true); timeout_.setInterval(1200);
        connect(&timeout_, &QTimer::timeout, this, [this] { pending_ = {}; });
        connect(qApp, &QApplication::focusChanged, this, [this] { pending_ = {}; timeout_.stop(); });
        qApp->installEventFilter(this);
    }
    void Register(const QString& Id, QAction* Action)
    { actions_[Id] = Action; Action->setShortcutContext(Qt::WidgetShortcut); }
    void SetEligibility(std::function<bool(const QString&, QWidget*)> Predicate) { eligible_ = std::move(Predicate); }
    QString Apply(const QJsonObject& Stored)
    {
        pending_ = {}; timeout_.stop();
        const auto Error = DocumentShortcutRegistry::Validate(Stored);
        bindings_ = DocumentShortcutRegistry::Bindings(Error.isEmpty() ? Stored : QJsonObject{});
        for (auto It = actions_.begin(); It != actions_.end(); ++It) if (It.value())
            It.value()->setShortcut(QKeySequence::fromString(bindings_[It.key()].toString(), QKeySequence::PortableText));
        return Error;
    }
    // Effective bindings after validation; defaults until the first Apply.
    QJsonObject Bindings() const { return bindings_.isEmpty() ? DocumentShortcutRegistry::Bindings({}) : bindings_; }
protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override
    {
        if (Event->type() != QEvent::ShortcutOverride && Event->type() != QEvent::KeyPress) return false;
        auto* Widget = qobject_cast<QWidget*>(Watched);
        if (!Widget || !scope_->isVisible() || (Widget != scope_ && !scope_->isAncestorOf(Widget))
            || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
        auto* Key = static_cast<QKeyEvent*>(Event);
        if (Key->key() == Qt::Key_Control || Key->key() == Qt::Key_Shift || Key->key() == Qt::Key_Alt || Key->key() == Qt::Key_Meta) return false;
        auto Combination = DocumentShortcutRegistry::NormalizeKey(Key->keyCombination());
        if (Combination.key() == Qt::Key_Backtab) Combination = QKeyCombination(Combination.keyboardModifiers() | Qt::ShiftModifier, Qt::Key_Tab);
        const auto Sequence = pending_.isEmpty() ? QKeySequence(Combination) : QKeySequence(pending_[0], Combination);
        QAction* Exact = nullptr; bool Partial = false, Owned = false;
        for (const auto& Item : DocumentShortcutRegistry::Commands()) {
            if (eligible_ && !eligible_(Item.id_, Widget)) continue;
            const auto Binding = DocumentShortcutRegistry::NormalizeNavigationKeys(QKeySequence::fromString(bindings_[Item.id_].toString(), QKeySequence::PortableText));
            const auto Match = Binding.isEmpty() ? QKeySequence::NoMatch : Sequence.matches(Binding);
            if (Match == QKeySequence::ExactMatch) { Exact = actions_.value(Item.id_); Owned = true; }
            if (Match == QKeySequence::PartialMatch) Partial = true;
            // An unbound or rebound command must not fall through to Scintilla's
            // control-character aliases or QTabWidget's built-in tab switching.
            if (pending_.isEmpty() && Sequence == DocumentShortcutRegistry::NormalizeNavigationKeys(QKeySequence(Item.key_))) Owned = true;
            const QMap<QString, QString> Aliases{{"edit.redo", "Ctrl+Shift+Z"}, {"edit.copy", "Ctrl+Insert"}, {"edit.cut", "Shift+Delete"}, {"edit.paste", "Shift+Insert"}};
            if (pending_.isEmpty() && Aliases.contains(Item.id_) && Sequence == QKeySequence(Aliases[Item.id_])) {
                Owned = true; if (Binding == QKeySequence(Item.key_)) Exact = actions_.value(Item.id_);
            }
        }
        if (!Owned && !Partial && pending_.isEmpty()) return false;
        if (Event->type() == QEvent::ShortcutOverride) { Event->accept(); return true; }
        if (Partial && !Exact) { pending_ = Sequence; timeout_.start(); }
        else { pending_ = {}; timeout_.stop(); if (Exact && Exact->isEnabled()) Exact->trigger(); }
        Event->accept(); return true;
    }
private:
    QWidget* scope_;
    QMap<QString, QPointer<QAction>> actions_;
    QJsonObject bindings_;
    QKeySequence pending_;
    QTimer timeout_;
    std::function<bool(const QString&, QWidget*)> eligible_;
};
}
