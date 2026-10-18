#pragma once
#include <QtWidgets>
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/commandMgr.hpp"

namespace hc::ui {
// The same stable command IDs are used by menus, keyboard and function buttons.
class CommandSelector final : public QComboBox {
public:
    explicit CommandSelector(QWidget* Parent = nullptr) : QComboBox(Parent) {
        setEditable(true); setInsertPolicy(QComboBox::NoInsert); setMinimumContentsLength(14);
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        QMap<QString, QString> Commands;
        for (const auto& Command : hc::defaults::Commands()) Commands[CommandId(Command.Name)] = Command.Desc;
        if (auto* Main = TyStCommandMgr::GetInstance()->GetMainUI()) for (auto* Action : Main->findChildren<QAction*>()) {
            const auto Id = Action->property("hc.commandId").toString();
            if (!Id.isEmpty()) Commands[Id] = Action->property("hc.baseLabel").toString().isEmpty() ? Action->text().section('\t', 0, 0) : Action->property("hc.baseLabel").toString();
        }
        for (auto It = Commands.begin(); It != Commands.end(); ++It) addItem(It.value() + " · " + It.key(), It.key());
        completer()->setFilterMode(Qt::MatchContains); completer()->setCaseSensitivity(Qt::CaseInsensitive);
        setAccessibleName(tr("명령 이름 또는 ID 찾기"));
    }
    QString Command() const { return currentText() == itemText(currentIndex()) ? currentData().toString() : currentText().trimmed(); }
    void SetCommand(const QString& Id) { const int Index = findData(Id); if (Index >= 0) setCurrentIndex(Index); else setEditText(Id); }
    QSet<QString> Commands() const { QSet<QString> Result; for (int I = 0; I < count(); ++I) Result.insert(itemData(I).toString()); return Result; }
};
}
