#include "stdafx.h"
#include "semantic.hpp"
#include "workerHost.hpp"
#include "compareLanguage.hpp"
#include <QJsonArray>

namespace hc::compare
{
    namespace { constexpr std::uint64_t mib = 1024 * 1024; }
    std::uint64_t SourceTreeMemory(const QString& Language, qsizetype Bytes)
    {
        // Peak child commit per source byte while parsing and digesting (measured
        // 2026-10-28: 44 C/C++, 56 Python at 4-10 MiB): the tree is 30 to 36 times the
        // source, plus the decoded request and the function list.
        const std::uint64_t perByte = Language == "python" ? 58 : 48;
        return perByte * std::uint64_t(std::max<qsizetype>(Bytes, 0));
    }
    std::uint64_t SourceWorkerMemory(const QString& Language, qsizetype Bytes)
    {
        // The worker process itself commits about 7 MiB. The limit leaves 2.5 times the
        // typical tree for dense code; a tree beyond it aborts the worker, not the GUI.
        const auto typical = SourceTreeMemory(Language, Bytes);
        if (16 * mib + typical > 512 * mib) return 0;
        return std::clamp<std::uint64_t>(16 * mib + typical * 5 / 2, 64 * mib, 512 * mib);
    }
    bool ValidateFunctionList(const QJsonObject& Result, const QString& Language, qsizetype Bytes)
    {
        if (Result["language"] != Language || Result["functions"].toArray().size() > 10000) return false;
        int previous = -1; qsizetype tokenBytes = 0;
        for (const auto value : Result["functions"].toArray())
        {
            const auto function = value.toObject(); const int begin = function["begin"].toInt(-1), end = function["end"].toInt(-1);
            const int nameBegin = function["nameBegin"].toInt(-2), nameEnd = function["nameEnd"].toInt(-2);
            if (begin < previous || end < begin || end > Bytes || function["name"].toString().isEmpty() || function["name"].toString().size() > 16384 ||
                function["line"].toInt(-1) < 0 || function["endLine"].toInt(-1) < function["line"].toInt() ||
                !((nameBegin == -1 && nameEnd == -1) || (begin <= nameBegin && nameBegin < nameEnd && nameEnd <= end)) ||
                (function["renameSignature"].toString().size() != 0 && function["renameSignature"].toString().size() != 64) ||
                (!function["literalShape"].toString().isEmpty() && function["literalShape"].toString().size() != 64) ||
                function["numberTokens"].toInt(-1) < 0 || function["numberTokens"].toInt() > function["bodyTokens"].toInt() ||
                function["bodyTokens"].toInt(-1) < 0 || function["signature"].toString().size() != 64 || function["body"].toString().size() != 64) return false;
            if (!function["bodySequence"].isArray()) return false;
            const auto sequence = function["bodySequence"].toArray();
            if (!sequence.isEmpty() && (sequence.size() < 32 || sequence.size() > 256 || sequence.size() != function["bodyTokens"].toInt())) return false;
            if (Language == "python" && !sequence.isEmpty() && function["tokenStructure"].toString().size() != 64) return false;
            qsizetype functionBytes = 0;
            for (const auto& token : sequence)
            {
                if (!token.isString() || token.toString().toUtf8().size() > 128) return false;
                functionBytes += token.toString().toUtf8().size();
            }
            tokenBytes += functionBytes;
            if (functionBytes > 8192 || tokenBytes > 512 * 1024) return false;
            previous = end;
        }
        return true;
    }
    QJsonObject AnalyzeFunctionsAs(const CompareLanguage& Language, const QByteArray& Utf8, const Budget& Limits)
    {
        if (!Language.structure_) return {{"status", "Unsupported"}, {"reason", "Language"}};
        const auto& language = Language.id_;
        const auto unavailable = [&](const QString& Reason) { return QJsonObject{{"status", "Unavailable"}, {"reason", Reason}, {"language", language}}; };
        if (Utf8.size() > 16 * 1024 * 1024) return unavailable("InputLimit");
        if (Limits.canceled_ && Limits.canceled_->load()) return unavailable("Canceled");
        const auto now = std::chrono::steady_clock::now();
        if (now >= Limits.deadline_) return unavailable("TimeLimit");
        WorkerPolicy policy; policy.memoryBytes_ = SourceWorkerMemory(language, Utf8.size());
        // Rejected before spawning: tree-sitter aborts the process when an allocation fails.
        if (!policy.memoryBytes_) return unavailable("InputLimit");
        // Parsing and digests scale with the input (measured up to about 450 ms/MiB, more
        // for dense code); 10 s is the standard engine bound.
        policy.timeoutMs_ = int(std::clamp<qint64>(1000 + Utf8.size() * 800 / qsizetype(mib), 2500, 10000));
        // The worker stops before the host deadline, so it reports TimeLimit instead of being killed.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Limits.deadline_ - now).count();
        const auto deadline = int(std::clamp<qint64>(std::min<qint64>(policy.timeoutMs_, remaining) - 250, 1, 30000));
        auto result = RunWorker("source.tree-sitter", {{"language", language}, {"source", QString::fromLatin1(Utf8.toBase64())}, {"deadlineMs", deadline}}, Limits, policy);
        if (result["status"] == "Unavailable") { result["language"] = language; return result; }
        if (!ValidateFunctionList(result, language, Utf8.size())) return unavailable("WorkerProtocol");
        return result;
    }
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits)
    { return AnalyzeFunctionsAs(DetectCompareLanguage(Path, Utf8), Utf8, Limits); }
}
