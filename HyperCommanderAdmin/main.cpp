#include "UniqueLibs/permissionService.hpp"
#include "UniqueLibs/nativeFileElevation.hpp"
#include "UniqueLibs/fileLockService.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"
int main(int Argc, char** Argv) {
    if (!hc::update::StartupAllowed(hc::update::ExecutableRoot())) return ERROR_BUSY;
    QCoreApplication App(Argc, Argv);
    const auto Args = App.arguments();
    if (Args.size() != 3 || (Args[1] != "--request" && Args[1] != "--elevated-job" && Args[1] != "--elevated-list" && Args[1] != "--file-lock-session") || Args[2].size() > 24000) return ERROR_INVALID_PARAMETER;
    const auto Bytes = QByteArray::fromBase64(Args[2].toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error);
    if (Error.error != QJsonParseError::NoError || !Document.isObject()) return ERROR_INVALID_PARAMETER;
    if (Args[1] == "--file-lock-session") return hc::filelocks::ExecuteFileLockSession(Document.object());
    if (Args[1] == "--elevated-list") {
        if (!hc::permissions::Elevated()) return ERROR_ELEVATION_REQUIRED;
        return hc::permissions::ExecuteDirectoryElevation(Document.object());
    }
    if (Args[1] == "--elevated-job") {
        if (!hc::permissions::Elevated()) return ERROR_ELEVATION_REQUIRED;
        return hc::permissions::ExecuteFileElevation(Document.object());
    }
    if (const auto Code = hc::permissions::Validate(Document.object())) return Code;
    if (!hc::permissions::Elevated()) return ERROR_ELEVATION_REQUIRED;
    return hc::permissions::Execute(Document.object());
}
