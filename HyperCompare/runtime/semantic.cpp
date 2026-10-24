#include "stdafx.h"
#include "semantic.hpp"
#include "workerHost.hpp"
#include <QFileInfo>
#include <QJsonArray>

namespace hc::compare
{
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits)
    {
        const auto suffix = QFileInfo(Path).suffix().toLower();
        const QString language = suffix == "c" ? "c" : QStringList{"cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp"}.contains(suffix) ? "cpp" : "";
        if (language.isEmpty()) return {{"status", "Unsupported"}, {"reason", "Language"}};
        const auto unavailable = [&](const QString& Reason) { return QJsonObject{{"status", "Unavailable"}, {"reason", Reason}, {"language", language}}; };
        if (Utf8.size() > 16 * 1024 * 1024) return unavailable("InputLimit");
        if (Limits.canceled_ && Limits.canceled_->load()) return unavailable("Canceled");
        if (std::chrono::steady_clock::now() >= Limits.deadline_) return unavailable("TimeLimit");
        auto result = RunWorker("source.tree-sitter", {{"language", language}, {"source", QString::fromLatin1(Utf8.toBase64())}}, Limits);
        if (result["status"] == "Unavailable") { result["language"] = language; return result; }
        if (result["language"] != language || result["functions"].toArray().size() > 10000) return unavailable("WorkerProtocol");
        int previous = -1; qsizetype tokenBytes = 0;
        for (const auto value : result["functions"].toArray())
        {
            const auto function = value.toObject(); const int begin = function["begin"].toInt(-1), end = function["end"].toInt(-1);
            if (begin < previous || end < begin || end > Utf8.size() || function["name"].toString().isEmpty() || function["name"].toString().size() > 16384 ||
                function["line"].toInt(-1) < 0 || function["endLine"].toInt(-1) < function["line"].toInt() ||
                (function["renameSignature"].toString().size() != 0 && function["renameSignature"].toString().size() != 64) ||
                (!function["literalShape"].toString().isEmpty() && function["literalShape"].toString().size() != 64) ||
                function["numberTokens"].toInt(-1) < 0 || function["numberTokens"].toInt() > function["bodyTokens"].toInt() ||
                function["bodyTokens"].toInt(-1) < 0 || function["signature"].toString().size() != 64 || function["body"].toString().size() != 64) return unavailable("WorkerProtocol");
            if (!function["bodySequence"].isArray()) return unavailable("WorkerProtocol");
            const auto sequence = function["bodySequence"].toArray();
            if (!sequence.isEmpty() && (sequence.size() < 32 || sequence.size() > 256 || sequence.size() != function["bodyTokens"].toInt())) return unavailable("WorkerProtocol");
            qsizetype functionBytes = 0;
            for (const auto& token : sequence)
            {
                if (!token.isString() || token.toString().toUtf8().size() > 128) return unavailable("WorkerProtocol");
                functionBytes += token.toString().toUtf8().size();
            }
            tokenBytes += functionBytes;
            if (functionBytes > 8192 || tokenBytes > 512 * 1024) return unavailable("WorkerProtocol");
            previous = end;
        }
        return result;
    }
}
