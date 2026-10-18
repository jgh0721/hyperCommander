#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QSet>
#include "UniqueLibs/productDefaults.hpp"
namespace hc::document {
inline QJsonObject DefaultDocumentHandlers() {
    return hc::defaults::DocumentHandlers();
}
inline QJsonObject MigrateEditors(QJsonValue Value) {
    auto Result = Value.isObject() ? Value.toObject() : QJsonObject{{"items", Value.toArray()}};
    if (!Result.contains("documentHandlers")) Result["documentHandlers"] = DefaultDocumentHandlers();
    QJsonArray Items; QSet<QString> Ids;
    for (const auto& Value : Result["items"].toArray()) {
        auto Item = Value.toObject(); auto Id = Item["stableId"].toString();
        if (Id.isEmpty() || Ids.contains(Id) || Id.startsWith("builtin.")) {
            const auto Digest = QCryptographicHash::hash((Item["program"].toString() + '\n' + Item["name"].toString()).toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
            const auto Base = "external." + QString::fromLatin1(Digest); Id = Base; int Suffix = 1;
            while (Ids.contains(Id)) Id = Base + '.' + QString::number(Suffix++);
            Item["stableId"] = Id;
        }
        Ids.insert(Id); Items.append(Item);
    }
    Result["items"] = Items; return Result;
}
inline QJsonArray EditorItems(const QJsonValue& Value) { return Value.isArray() ? Value.toArray() : Value.toObject()["items"].toArray(); }
}
