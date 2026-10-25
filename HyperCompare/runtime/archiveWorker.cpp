#include "stdafx.h"
#include "zipArchive.hpp"
#include <QJsonArray>
#include <QMap>

namespace hc::compare
{
    QJsonObject ArchiveWorkerRequest(const QJsonObject& Payload)
    {
        const auto resources = Payload["resources"].toArray(); if (resources.isEmpty() || resources.size() > 2) throw Failure("ArchiveArityUnsupported");
        Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        QJsonArray documents; QVector<QMap<QString, QJsonObject>> maps;
        for (const auto& value : resources)
        {
            limits.Check(); if (value.toString().size() > 24 * 1024 * 1024) throw Failure("ArchiveInputLimit");
            auto decoded = QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) throw Failure("InvalidInputEncoding"); ZipArchive archive(std::move(decoded.decoded), limits);
            QJsonArray entries; QMap<QString, QJsonObject> paths;
            for (const auto& entry : archive.Entries())
            {
                limits.Check(); const auto hash = entry.directory_ ? QString{} : archive.Hash(entry.ordinal_, limits);
                const QJsonObject item{{"path", entry.path_}, {"ordinal", entry.ordinal_}, {"directory", entry.directory_}, {"size", QString::number(entry.size_)},
                    {"sha256", hash}, {"location", archive.Location(entry.ordinal_, hash)}};
                entries.append(item); paths.insert(entry.path_, item);
            }
            documents.append(QJsonObject{{"rawSha256", archive.Digest()}, {"entryCount", entries.size()}, {"entries", resources.size() == 1 ? entries : QJsonArray{}}}); maps.push_back(std::move(paths));
        }
        QJsonArray comparisons; bool contentsEqual = true;
        if (maps.size() == 2)
        {
            QMap<QString, bool> names; for (const auto& map : maps) for (auto it = map.cbegin(); it != map.cend(); ++it) names.insert(it.key(), true);
            for (auto it = names.cbegin(); it != names.cend(); ++it)
            {
                limits.Check(); const auto left = maps[0].value(it.key()), right = maps[1].value(it.key());
                const QString state = left.isEmpty() ? "RightOnly" : right.isEmpty() ? "LeftOnly" : left["directory"] != right["directory"] ? "TypeChanged" :
                    left["size"] == right["size"] && left["sha256"] == right["sha256"] ? "Equal" : "Changed";
                contentsEqual &= state == "Equal"; comparisons.append(QJsonObject{{"path", it.key()}, {"state", state}, {"left", left}, {"right", right}});
            }
        }
        return {{"status", "Complete"}, {"payloadSchema", "archive.entries.v1"}, {"documents", documents}, {"entries", comparisons}, {"contentsEqual", contentsEqual},
            {"rawEqual", documents.size() == 1 || documents[0].toObject()["rawSha256"] == documents[1].toObject()["rawSha256"]},
            {"coverage", "all explicit entries; case-sensitive names; verified uncompressed bytes; no filesystem extraction"}, {"merge", false}};
    }
}
