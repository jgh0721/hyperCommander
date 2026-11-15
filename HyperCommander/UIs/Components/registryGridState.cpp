#include "stdafx.h"
#include "registryGridState.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/registry/registryModels.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonArray>
#include <QScrollBar>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryGridState, "hc.ui.registryState")

RegistryGridState RegistryGridState::Capture(Qtitan::GridTreeView* View)
{
    RegistryGridState State; if (!View) return State;
    constexpr int Id = hc::registry::RegistryTreeModel::IdRole;
    const auto Row = View->focusedRow();
    if (Row.isValid()) State.cursor = Row.modelIndex(0).data(Id).toString();
    if (auto* Selection = View->selection()) for (const auto& Index : Selection->selectedRowIndexes())
        if (State.selected.size() < 10000) State.selected.append(Index.data(Id).toString());
    for (int I = 0; I < View->getRowCount() && State.expanded.size() < 10000; ++I) {
        const auto Item = View->getRow(I);
        if (Item.isValid() && View->modelController()->isRowExpanded(Item)) State.expanded.append(Item.modelIndex(0).data(Id).toString());
    }
    if (auto* Bar = View->verticalScrollBar()) State.scroll = Bar->value();
    return State;
}
bool RegistryGridState::Restore(Qtitan::GridTreeView* View, const std::function<QModelIndex(const QString&)>& Lookup) const
{
    if (!View) return false;
    bool Complete = true;
    // IDs are captured in preorder, so parent expansions materialize children
    // before their turn. A later async batch retries IDs not yet loaded.
    for (const auto& Id : expanded) {
        const auto Index = Lookup(Id);
        if (Index.isValid()) View->expand(Index); else Complete = false;
    }
    const auto Cursor = Lookup(cursor); const auto Row = View->getRow(Cursor);
    if (Row.isValid()) View->setFocusedRowIndex(Row.rowIndex());
    else if (!cursor.isEmpty()) Complete = false;
    View->deselectAll();
    for (const auto& Id : selected) {
        const auto Index = Lookup(Id); const auto Item = View->getRow(Index);
        if (Item.isValid()) View->selectRow(Item.rowIndex());
        else Complete = false;
    }
    if (auto* Bar = View->verticalScrollBar()) Bar->setValue(scroll);
    return Complete;
}
QJsonObject RegistryGridState::ToJson() const
{
    return {{"cursor", cursor}, {"selected", QJsonArray::fromStringList(selected)},
        {"expanded", QJsonArray::fromStringList(expanded)}, {"scroll", scroll}};
}
RegistryGridState RegistryGridState::FromJson(const QJsonObject& Object)
{
    RegistryGridState State; State.cursor = Object["cursor"].toString();
    const auto Read = [](const QJsonValue& Value) {
        QStringList Result;
        for (const auto& Item : Value.toArray()) {
            if (Result.size() == 10000) break;
            const auto Id = Item.toString(); if (!Id.isEmpty() && Id.size() <= 65536) Result.append(Id);
        }
        return Result;
    };
    State.selected = Read(Object["selected"]); State.expanded = Read(Object["expanded"]);
    State.scroll = std::max(0, Object["scroll"].toInt()); return State;
}
