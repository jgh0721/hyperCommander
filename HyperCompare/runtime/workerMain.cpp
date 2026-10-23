#include "stdafx.h"
#include "workerHost.hpp"
#include "semantic.hpp"
#include "extensionEngine.hpp"
#include "zipArchive.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>

namespace hc::compare
{
    namespace
    {
        QJsonObject searchText(const QJsonObject& Input)
        {
            const auto raw = QByteArray::fromBase64(Input["text"].toString().toLatin1());
            const auto query = Input["query"].toString();
            if (raw.size() > 16 * 1024 * 1024 || query.isEmpty() || query.size() > 256) throw Failure("SearchInputLimit");
            QStringDecoder decoder(QStringConverter::Utf8); const QString text = decoder(raw);
            if (decoder.hasError()) throw Failure("SearchInvalidUtf8");
            auto pattern = Input["regex"].toBool() ? query : QRegularExpression::escape(query);
            if (Input["word"].toBool()) pattern = "(?<!\\w)(?:" + pattern + ")(?!\\w)";
            auto flags = QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::MultilineOption;
            if (!Input["case"].toBool()) flags |= QRegularExpression::CaseInsensitiveOption;
            const QRegularExpression expression(pattern, flags);
            if (!expression.isValid()) return {{"status", "Unavailable"}, {"reason", "InvalidSearchExpression"}, {"detail", expression.errorString()}};
            QJsonArray matches; auto it = expression.globalMatch(text); qsizetype previous = 0, bytePosition = 0;
            const bool all = Input["all"].toBool(true); const auto cursor = Input["cursor"].toInteger();
            QJsonObject firstHit, lastHit, before, after;
            const auto maximum = std::clamp(Input["maximum"].toInt(1000), 1, 1000);
            while (it.hasNext())
            {
                const auto match = it.next(); const auto begin = match.capturedStart(), end = match.capturedEnd();
                bytePosition += QStringView(text).mid(previous, begin - previous).toUtf8().size(); const auto first = bytePosition;
                bytePosition += QStringView(text).mid(begin, end - begin).toUtf8().size(); previous = end;
                const QJsonObject hit{{"begin", qint64(first)}, {"end", qint64(bytePosition)}};
                if (all)
                {
                    matches.append(hit); if (matches.size() >= maximum) return {{"status", "Partial"}, {"matches", matches}};
                }
                else
                {
                    if (firstHit.isEmpty()) firstHit = hit; lastHit = hit;
                    if (bytePosition <= cursor) before = hit;
                    if (first >= cursor && after.isEmpty()) after = hit;
                }
            }
            if (!all) for (const auto& hit : {firstHit, before, after, lastHit})
                if (!hit.isEmpty() && (matches.isEmpty() || matches.last().toObject() != hit)) matches.append(hit);
            return {{"status", "Complete"}, {"matches", matches}};
        }
    }
    int WorkerMain()
    {
        QFile input, output; if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 2;
        const auto bytes = input.read(64 * 1024 * 1024 + 1); if (bytes.size() > 64 * 1024 * 1024) return 2;
        QJsonParseError error; const auto envelope = QJsonDocument::fromJson(bytes, &error).object();
        const auto nonce = envelope["nonce"].toString(), engine = envelope["engine"].toString(), digest = envelope["digest"].toString();
        const auto payload = envelope["request"].toObject();
        if (error.error != QJsonParseError::NoError || envelope["schema"].toInt() != 1 || nonce.size() != 36 || digest.size() != 64 ||
            digest != QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) return 2;
        QJsonObject result;
        try
        {
            if (engine == "search.text") result = searchText(payload);
            else if (engine == "source.tree-sitter") result = SemanticWorkerRequest(payload);
            else if (engine == "image.png") result = PngWorkerRequest(payload);
            else if (engine == "container.zip") result = ArchiveWorkerRequest(payload);
            else if (engine == "office.docx") result = DocxWorkerRequest(payload);
            else throw Failure("WorkerEngineUnsupported");
        }
        catch (const std::exception& failure) { result = {{"status", "Unavailable"}, {"reason", QString::fromUtf8(failure.what())}}; }
        result["schema"] = 1; result["nonce"] = nonce; result["engine"] = engine; result["digest"] = digest;
        const auto response = QJsonDocument(result).toJson(QJsonDocument::Compact);
        if (response.size() > 64 * 1024 * 1024 || output.write(response) != response.size() || !output.flush()) return 2;
        return 0;
    }
}
