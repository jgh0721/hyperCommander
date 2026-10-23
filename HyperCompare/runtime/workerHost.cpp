#include "stdafx.h"
#include "workerHost.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <Windows.h>

namespace hc::compare
{
    QJsonObject RunWorker(const QString& Engine, const QJsonObject& Payload, const Budget& Limits, const WorkerPolicy& Policy)
    {
        const auto unavailable = [&](const QString& Reason) { return QJsonObject{{"status", "Unavailable"}, {"reason", Reason}, {"engine", Engine}}; };
        if (Policy.timeoutMs_ <= 0 || Policy.timeoutMs_ > 30000 || Policy.requestBytes_ <= 0 || Policy.requestBytes_ > 64 * 1024 * 1024 ||
            Policy.responseBytes_ <= 0 || Policy.responseBytes_ > 64 * 1024 * 1024 || Policy.memoryBytes_ > 512 * 1024 * 1024 || !Policy.memoryBytes_) return unavailable("WorkerPolicy");
        const auto deadline = std::min(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(Policy.timeoutMs_));
        const auto stopped = [&]() -> QString {
            if (Limits.canceled_ && Limits.canceled_->load()) return "Canceled";
            if (std::chrono::steady_clock::now() >= deadline) return "TimeLimit";
            return {};
        };
        if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
        // Two named mutex slots bound workers across GUI windows and CLI processes
        // in this logon session. Abandoned ownership is recoverable after a crash.
        struct Slots
        {
            HANDLE handles_[2]{}; int held_ = -1;
            ~Slots() { if (held_ >= 0) ReleaseMutex(handles_[held_]); for (auto handle : handles_) if (handle) CloseHandle(handle); }
        } workerSlots;
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex());
        for (int i = 0; i < 2; ++i)
        {
            const auto name = QString("Local\\HyperCompare.Worker.%1.%2").arg(owner).arg(i);
            workerSlots.handles_[i] = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
            if (!workerSlots.handles_[i]) return unavailable("WorkerCapacity");
        }
        while (workerSlots.held_ < 0)
        {
            if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
            const auto wait = WaitForMultipleObjects(2, workerSlots.handles_, FALSE, 20);
            if (wait < WAIT_OBJECT_0 + 2) workerSlots.held_ = int(wait - WAIT_OBJECT_0);
            else if (wait >= WAIT_ABANDONED_0 && wait < WAIT_ABANDONED_0 + 2) workerSlots.held_ = int(wait - WAIT_ABANDONED_0);
            else if (wait != WAIT_TIMEOUT) return unavailable("WorkerCapacity");
        }
        struct Job { HANDLE handle_ = CreateJobObjectW(nullptr, nullptr); ~Job() { if (handle_) CloseHandle(handle_); } } job;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        info.ProcessMemoryLimit = Policy.memoryBytes_; info.BasicLimitInformation.ActiveProcessLimit = 1;
        if (!job.handle_ || !SetInformationJobObject(job.handle_, JobObjectExtendedLimitInformation, &info, sizeof(info))) return unavailable("WorkerIsolation");
        QProcess process; process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Args) { Args->flags |= CREATE_NO_WINDOW; });
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-internal-worker"});
        if (!process.waitForStarted(500)) return unavailable("WorkerStart");
        struct ProcessGuard { QProcess& process_; ~ProcessGuard() { if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(1000); } } } guard{process};
        const HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        const bool assigned = child && AssignProcessToJobObject(job.handle_, child); if (child) CloseHandle(child);
        if (!assigned) return unavailable("WorkerIsolation");
        if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
        const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(Payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        const auto request = QJsonDocument(QJsonObject{{"schema", 1}, {"nonce", nonce}, {"engine", Engine}, {"digest", digest}, {"request", Payload}}).toJson(QJsonDocument::Compact);
        if (request.size() > Policy.requestBytes_) return unavailable("WorkerInputLimit");
        if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
        if (process.write(request) != request.size()) return unavailable("WorkerInput");
        process.closeWriteChannel(); QByteArray response; qsizetype stderrBytes = 0;
        for (;;)
        {
            if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
            process.waitForFinished(20); response += process.readAllStandardOutput(); stderrBytes += process.readAllStandardError().size();
            if (response.size() > Policy.responseBytes_ || stderrBytes > 1024 * 1024) return unavailable("WorkerOutputLimit");
            if (process.state() == QProcess::NotRunning) break;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return unavailable("WorkerFailure");
        QJsonParseError error; auto result = QJsonDocument::fromJson(response, &error).object();
        if (error.error != QJsonParseError::NoError || result["schema"].toInt() != 1 || result["nonce"] != nonce || result["engine"] != Engine || result["digest"] != digest ||
            !QStringList{"Complete", "Partial", "Unavailable"}.contains(result["status"].toString())) return unavailable("WorkerProtocol");
        result.remove("nonce"); return result;
    }
}
