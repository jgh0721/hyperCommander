#pragma once
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <functional>
#include "commandUiConfig.hpp"

namespace hc::ui {
struct MenuNode {
    QString id_, label_, command_, section_;
    bool separator_ = false;
    QList<MenuNode> items_;
};
struct MenuDefinition {
    QList<MenuNode> menus_;
    QString error_;
    QStringList issues_;
    static MenuDefinition Parse(const QByteArray& Bytes, const QSet<QString>& Commands, const QSet<QString>& Sections)
    {
        MenuDefinition Result;
        if (Bytes.size() > 1024 * 1024) { Result.error_ = "document-too-large"; return Result; }
        QJsonParseError Error;
        const auto Document = QJsonDocument::fromJson(Bytes, &Error);
        if (Error.error != QJsonParseError::NoError) {
            Result.error_ = QString("json-byte-%1: %2").arg(Error.offset).arg(Error.errorString()); return Result;
        }
        const auto Root = Document.object();
        if (!Document.isObject() || Root["schemaVersion"] != QJsonValue(1) || !Root["menus"].isArray()
            || Root.size() != 2 || Root["menus"].toArray().isEmpty()) { Result.error_ = "invalid-root-or-schema"; return Result; }
        QSet<QString> Ids;
        int Count = 0;
        const QRegularExpression IdPattern("^[A-Za-z0-9_.-]{1,128}$");
        const auto Mnemonic = [](const QString& Label) {
            for (int Index = 0; Index + 1 < Label.size(); ++Index) {
                if (Label[Index] != '&') continue;
                if (Label[Index + 1] == '&') { ++Index; continue; }
                return Label[Index + 1].toCaseFolded();
            }
            return QChar{};
        };
        std::function<bool(const QJsonArray&, QList<MenuNode>&, int, const QString&)> Read;
        Read = [&](const QJsonArray& Items, QList<MenuNode>& Nodes, int Depth, const QString& Path) {
            if (Depth > 4) { Result.error_ = Path + ": depth-limit"; return false; }
            QSet<QChar> Mnemonics;
            for (int Index = 0; Index < Items.size(); ++Index) {
                const auto Location = Path + QString("[%1]").arg(Index);
                const auto Fail = [&](const QString& Code) { Result.error_ = Location + ": " + Code; return false; };
                if (++Count > 512) return Fail("item-limit");
                if (!Items[Index].isObject()) return Fail("expected-object");
                const auto Object = Items[Index].toObject();
                for (auto It = Object.begin(); It != Object.end(); ++It)
                    if (!QStringList{"id", "label", "command", "section", "items", "type"}.contains(It.key())) return Fail("unknown-field-" + It.key());
                MenuNode Node;
                if (Object["type"].toString() == "separator") {
                    if (Depth == 0 || Object.size() != 1) return Fail("invalid-separator");
                    Node.separator_ = true; Nodes.append(Node); continue;
                }
                if (Object.contains("type")) return Fail("invalid-type");
                if (!Object["id"].isString() || !IdPattern.match(Object["id"].toString()).hasMatch()) return Fail("invalid-id");
                Node.id_ = Object["id"].toString();
                if (Ids.contains(Node.id_)) return Fail("duplicate-id-" + Node.id_);
                Ids.insert(Node.id_);
                if (Object.contains("label") && (!Object["label"].isString() || Object["label"].toString().trimmed().isEmpty()
                    || Object["label"].toString().size() > 256 || Object["label"].toString().contains(QRegularExpression("[\\x00-\\x1f]")))) return Fail("invalid-label");
                Node.label_ = Object["label"].toString();
                const int Kinds = int(Object.contains("command")) + int(Object.contains("section")) + int(Object.contains("items"));
                if (Kinds != 1 || (Depth == 0 && !Object.contains("items"))) return Fail("expected-one-item-kind");
                if (Object.contains("items")) {
                    if (Node.label_.isEmpty() || !Object["items"].isArray()) return Fail("invalid-submenu");
                    if (!Read(Object["items"].toArray(), Node.items_, Depth + 1, Location + ".items")) return false;
                } else if (Object.contains("command")) {
                    if (!Object["command"].isString() || !IdPattern.match(Object["command"].toString()).hasMatch()) return Fail("invalid-command");
                    Node.command_ = CommandId(Object["command"].toString());
                    if (!Commands.contains(Node.command_)) Result.issues_.append(Location + ": unavailable-command-" + Node.command_);
                } else {
                    if (!Object["section"].isString() || !Sections.contains(Object["section"].toString())) return Fail("unknown-section");
                    Node.section_ = Object["section"].toString();
                    // A dynamic menu is inserted once so that aboutToShow and
                    // plugin/script reload ownership stay with its original host.
                    const auto SectionKey = "$section." + Node.section_;
                    if (Ids.contains(SectionKey)) return Fail("duplicate-section");
                    Ids.insert(SectionKey);
                }
                const auto Key = Mnemonic(Node.label_);
                if (!Key.isNull() && Mnemonics.contains(Key)) return Fail("duplicate-mnemonic");
                if (!Key.isNull()) Mnemonics.insert(Key);
                Nodes.append(std::move(Node));
            }
            return true;
        };
        if (!Read(Root["menus"].toArray(), Result.menus_, 0, "menus")) Result.menus_.clear();
        return Result;
    }
};

// Config changes are prepared off the live menu bar. A failed reload preserves
// both the user's bytes and the last valid menu. QAction connections belong to
// the command owner and are never reconnected by reload.
class UserMenu : public QObject {
public:
    using Diagnostic = std::function<void(const QString&)>;
    UserMenu(QMenuBar* Bar, QString Path, QByteArray BuiltIn,
        QHash<QString, QPointer<QAction>> Commands, QHash<QString, QPointer<QMenu>> Sections, Diagnostic Report = {}, std::function<void()> BeforeShow = {})
        : QObject(Bar), bar_(Bar), path_(std::move(Path)), builtIn_(std::move(BuiltIn)), commands_(std::move(Commands)),
          sections_(std::move(Sections)), report_(std::move(Report)), beforeShow_(std::move(BeforeShow)) {}
    QString FileName() const { return path_; }
    QString Error() const { return error_; }
    bool Reload()
    {
        if (menus_.isEmpty() && !apply(builtIn_)) return false;
        if (!QFileInfo::exists(path_)) {
            // Never overwrite a concurrently created user file. Read-only
            // directories still retain a working built-in menu.
            if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return fail("create-directory-failed");
            QLockFile Lock(path_ + ".lock"); Lock.setStaleLockTime(0);
            if (!Lock.tryLock(0)) return fail("file-locked");
            if (!QFileInfo::exists(path_)) {
                QSaveFile Save(path_); Save.setDirectWriteFallback(false);
                if (!Save.open(QIODevice::WriteOnly) || Save.write(builtIn_) != builtIn_.size() || !Save.commit()) return fail("atomic-save-failed");
            }
        }
        QFile File(path_);
        if (!File.open(QIODevice::ReadOnly)) return fail("read-failed: " + File.errorString());
        if (File.size() > 1024 * 1024) return fail("document-too-large");
        const auto Bytes = File.read(1024 * 1024 + 1);
        if (File.error() != QFileDevice::NoError) return fail("read-failed: " + File.errorString());
        return apply(Bytes);
    }
private:
    bool fail(const QString& Error) { error_ = path_ + ": " + Error; if (report_) report_(error_); return false; }
    bool apply(const QByteArray& Bytes)
    {
        // Expand only the exact old stock selection section for display. The
        // user's file and any edited/deleted selection section remain intact.
        auto Root = QJsonDocument::fromJson(Bytes).object(); auto Menus = Root["menus"].toArray();
        const QJsonObject OldItem{{"id", "invert"}, {"command", "hc.selection.invert"}, {"label", QStringLiteral("선택 반전 · 다음 항목")}};
        const auto Defaults = QJsonDocument::fromJson(builtIn_).object()["menus"].toArray();
        for (qsizetype I = 0; I < Menus.size(); ++I) {
            const auto Menu = Menus[I].toObject();
            if (Menu["id"] != "select" || Menu["label"] != QStringLiteral("선택(&S)") || Menu["items"].toArray() != QJsonArray{OldItem}) continue;
            for (const auto& Default : Defaults) if (Default.toObject()["id"] == "select") Menus[I] = Default;
        }
        const bool HasMenus = Root["menus"].isArray();
        if (HasMenus) Root["menus"] = Menus;
        const auto Effective = HasMenus ? QJsonDocument(Root).toJson() : Bytes;
        const auto Definition = MenuDefinition::Parse(Effective, QSet<QString>(commands_.keyBegin(), commands_.keyEnd()), QSet<QString>(sections_.keyBegin(), sections_.keyEnd()));
        if (!Definition.error_.isEmpty()) return fail(Definition.error_);
        QList<QMenu*> Prepared;
        std::function<void(QMenu*, const QList<MenuNode>&)> Fill;
        Fill = [&](QMenu* Menu, const QList<MenuNode>& Nodes) {
            for (const auto& Node : Nodes) {
                if (Node.separator_) { Menu->addSeparator(); continue; }
                if (!Node.section_.isEmpty()) {
                    if (auto* Section = sections_.value(Node.section_).data()) Menu->addMenu(Section);
                    continue;
                }
                if (Node.command_.isEmpty()) {
                    auto* Submenu = Menu->addMenu(Node.label_); Submenu->setObjectName("hc.userMenu." + Node.id_); Fill(Submenu, Node.items_); continue;
                }
                auto* Action = Menu->addAction(Node.label_.isEmpty() ? Node.command_ : Node.label_);
                Action->setObjectName("hc.userMenu." + Node.id_); Action->setProperty("hc.commandId", Node.command_);
                const QPointer<QAction> Target = commands_.value(Node.command_);
                if (!Target) { Action->setEnabled(false); Action->setToolTip(QObject::tr("사용할 수 없는 명령: %1").arg(Node.command_)); continue; }
                const auto Sync = [Action, Target, Label = Node.label_] {
                    if (!Target) { Action->setEnabled(false); return; }
                    const auto Base = Label.isEmpty() ? Target->text().section('\t', 0, 0) : Label;
                    const auto Key = Target->property("hc.keyLabel").toString();
                    Action->setText(Base + (Key.isEmpty() ? QString{} : "\t" + Key));
                    Action->setEnabled(Target->isEnabled()); Action->setVisible(Target->isVisible());
                    Action->setCheckable(Target->isCheckable()); Action->setChecked(Target->isChecked()); Action->setToolTip(Target->toolTip());
                };
                Sync();
                connect(Target, &QAction::changed, Action, Sync);
                connect(Target, &QObject::destroyed, Action, [Action] { Action->setEnabled(false); });
                connect(Action, &QAction::triggered, Target, [Target] { if (Target) Target->trigger(); });
            }
        };
        for (const auto& Node : Definition.menus_) {
            auto* Menu = new QMenu(Node.label_, bar_); Menu->setObjectName("hc.userMenu." + Node.id_); Fill(Menu, Node.items_); Prepared.append(Menu);
            if (beforeShow_) connect(Menu, &QMenu::aboutToShow, this, [this] { beforeShow_(); });
        }
        // Remove borrowed dynamic sections before deleting their old containers.
        for (auto* Menu : menus_) for (auto* Child : Menu->findChildren<QMenu*>())
            for (const auto& Section : sections_) if (Section) Child->removeAction(Section->menuAction());
        for (auto* Menu : menus_) for (const auto& Section : sections_) if (Section) Menu->removeAction(Section->menuAction());
        for (auto* Action : bar_->actions()) bar_->removeAction(Action);
        qDeleteAll(menus_); menus_ = Prepared;
        for (auto* Menu : menus_) bar_->addMenu(Menu);
        error_.clear();
        if (!Definition.issues_.isEmpty() && report_) report_(path_ + ": " + Definition.issues_.join("; "));
        return true;
    }
    QPointer<QMenuBar> bar_;
    QString path_, error_;
    QByteArray builtIn_;
    QHash<QString, QPointer<QAction>> commands_;
    QHash<QString, QPointer<QMenu>> sections_;
    QList<QMenu*> menus_;
    Diagnostic report_;
    std::function<void()> beforeShow_;
};
} // namespace hc::ui
