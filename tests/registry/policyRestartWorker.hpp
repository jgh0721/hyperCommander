#pragma once
#include "UniqueLibs/registry/registryMutation.hpp"
#include "UniqueLibs/systemSessionService.hpp"
#include "commonLibs/system/processInfo.hpp"

// The owned VM runner copies this harness as HyperCommander.exe beside the
// unmodified product helper. This exercises its real caller-path validation;
// it is a test harness, not the product GUI executable.
inline int RegistryPolicyRestartWorker(const QString& Root, const QString& Phase)
{
    using namespace hc::registry;
    if (qEnvironmentVariable("HC_REGISTRY_TEST_LOCAL_GPO") != "task_4ceba737038e"
        || QSysInfo::machineHostName().compare("iMonAGR-Win11", Qt::CaseInsensitive) != 0
        || !Root.startsWith("C:\\HyperCommanderTestRegistryPolicy-", Qt::CaseInsensitive)) return ERROR_ACCESS_DENIED;
    HANDLE Token = nullptr; TOKEN_ELEVATION Elevation{}; DWORD Length = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) return GetLastError();
    const bool Elevated = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Length) && Elevation.TokenIsElevated;
    CloseHandle(Token); if (!Elevated) return ERROR_ELEVATION_REQUIRED; // Never request an interactive UAC prompt in this probe.
    const auto ReadId = [&](const QString& Name) { QFile File(QDir(Root).filePath(Name)); return File.open(QIODevice::ReadOnly) ? QString::fromUtf8(File.readAll()) : QString(); };
    RegistryChangeStore Store(QDir(Root).filePath("registry-backup")); WinRegistryBackend Backend;
    hc::admin::SystemSessionClient Client(QDir(Root).filePath("HyperCommanderAdmin.exe"));
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Request) { return Client.Request(Kind, Request, 120000); });
    RegistryMutation Mutation(Backend, Store, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()), &Remote);
    const auto Original = Store.Read(ReadId("journal-id.txt")); if (!Original) return int(Original.error);
    RegistryMutationResult Result;
    if (Phase == "undo") Result = Mutation.Undo(Original.value);
    else if (Phase == "redo") {
        const auto Undo = Store.Read(ReadId("undo-id.txt")); if (!Undo) return int(Undo.error);
        Result = Mutation.Redo(Original.value, Undo.value);
    } else return ERROR_INVALID_PARAMETER;
    Client.Close();
    if (Result.applied.error) { qWarning() << "policy restart" << Phase << "completed" << Result.applied.completed << "error" << Result.applied.error; return int(Result.applied.error); }
    if (Phase == "undo") {
        QFile File(QDir(Root).filePath("undo-id.txt"));
        if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly) || File.write(Result.record.plan.id.toUtf8()) < 0) return ERROR_WRITE_FAULT;
    }
    return 0;
}
