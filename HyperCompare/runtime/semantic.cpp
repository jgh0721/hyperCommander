#include "stdafx.h"
#include "semantic.hpp"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <Windows.h>

namespace hc::compare
{
    QJsonObject AnalyzeFunctions(const QString& Path, const QByteArray& Utf8, const Budget& Limits)
    {
        const auto suffix = QFileInfo(Path).suffix().toLower();
        const QString language = suffix == "c" ? "c" : QStringList{"cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp"}.contains(suffix) ? "cpp" : "";
        if (language.isEmpty()) return {{"status", "Unsupported"}, {"reason", "Language"}};
        auto unavailable = [&](const QString& Reason) { return QJsonObject{{"status", "Unavailable"}, {"reason", Reason}, {"language", language}}; };
        if (Utf8.size() > 16 * 1024 * 1024) return unavailable("InputLimit");
        if (Limits.canceled_ && Limits.canceled_->load()) return unavailable("Canceled");
        const auto deadline = std::min(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(2500));
        if (std::chrono::steady_clock::now() >= deadline) return unavailable("TimeLimit");
        struct Job { HANDLE handle_ = CreateJobObjectW(nullptr, nullptr); ~Job() { if (handle_) CloseHandle(handle_); } } job;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.ProcessMemoryLimit = 256 * 1024 * 1024; info.BasicLimitInformation.ActiveProcessLimit = 1;
        if (!job.handle_ || !SetInformationJobObject(job.handle_, JobObjectExtendedLimitInformation, &info, sizeof(info))) return unavailable("WorkerIsolation");
        QProcess process; process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Args) { Args->flags |= CREATE_NO_WINDOW; });
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-internal-semantic"});
        if (!process.waitForStarted(500)) return unavailable("WorkerStart");
        struct ProcessGuard { QProcess& process_; ~ProcessGuard() { if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(1000); } } } guard{process};
        const HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        const bool assigned = child && AssignProcessToJobObject(job.handle_, child); if (child) CloseHandle(child);
        if (!assigned) return unavailable("WorkerIsolation");
        if (std::chrono::steady_clock::now() >= deadline) return unavailable("TimeLimit");
        if (Limits.canceled_ && Limits.canceled_->load()) return unavailable("Canceled");
        const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto request = QJsonDocument(QJsonObject{{"schema", 1}, {"nonce", nonce}, {"language", language}, {"source", QString::fromLatin1(Utf8.toBase64())}}).toJson(QJsonDocument::Compact);
        if (process.write(request) != request.size()) return unavailable("WorkerInput");
        process.closeWriteChannel(); QByteArray response;
        for (;;)
        {
            if (Limits.canceled_ && Limits.canceled_->load()) return unavailable("Canceled");
            if (std::chrono::steady_clock::now() >= deadline) return unavailable("TimeLimit");
            process.waitForFinished(20); response += process.readAllStandardOutput(); process.readAllStandardError();
            if (response.size() > 8 * 1024 * 1024) return unavailable("OutputLimit");
            if (process.state() == QProcess::NotRunning) break;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return unavailable("WorkerFailure");
        QJsonParseError error; auto result = QJsonDocument::fromJson(response, &error).object();
        if (error.error != QJsonParseError::NoError || result["schema"].toInt() != 1 || result["nonce"].toString() != nonce || result["language"] != language ||
            !QStringList{"Complete", "Partial", "Unavailable"}.contains(result["status"].toString()) || result["functions"].toArray().size() > 10000) return unavailable("WorkerProtocol");
        int previous = -1;
        for (const auto value : result["functions"].toArray())
        {
            const auto function = value.toObject(); const int begin = function["begin"].toInt(-1), end = function["end"].toInt(-1);
            if (begin < previous || end < begin || end > Utf8.size() || function["name"].toString().isEmpty() || function["name"].toString().size() > 16384 ||
                function["line"].toInt(-1) < 0 || function["endLine"].toInt(-1) < function["line"].toInt() ||
                function["signature"].toString().size() != 64 || function["body"].toString().size() != 64) return unavailable("WorkerProtocol");
            previous = end;
        }
        result.remove("nonce"); return result;
    }
}
