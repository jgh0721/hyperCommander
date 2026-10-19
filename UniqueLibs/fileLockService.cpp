#include "stdafx.h"
#include "fileLockService.hpp"

/// Project headers
#include "nativeFileLocks.hpp"

/// Platform headers
#include <shellapi.h>
#include <sddl.h>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

namespace hc::filelocks {
namespace {
constexpr DWORD frameLimit = 65536;
struct Handle {
    HANDLE value_ = nullptr;
    explicit Handle(HANDLE Value = nullptr) : value_(Value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
};
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString pipeName(const QString& Nonce) { return "\\\\.\\pipe\\HyperCommander-fileLocks-" + Nonce; }
bool sameImage(const QString& First, const QString& Second) {
    return !First.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(First)).compare(QDir::cleanPath(QDir::fromNativeSeparators(Second)), Qt::CaseInsensitive) == 0;
}
QJsonObject transportError(bool Action, DWORD Code) {
    return {{"outcome", Action ? "Unknown" : "Unavailable"}, {"nativeCode", QString::number(Code)}, {"stage","transport"}, {"requiresRefresh", true}};
}
bool io(HANDLE Pipe, bool Write, QByteArray& Bytes, DWORD Timeout, const std::atomic_bool* Cancel = nullptr) {
    Handle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!Event.value_) return false;
    OVERLAPPED Pending{}; Pending.hEvent = Event.value_; DWORD Count = 0;
    const bool Done = Write ? WriteFile(Pipe, Bytes.constData(), DWORD(Bytes.size()), &Count, &Pending)
                            : ReadFile(Pipe, Bytes.data(), DWORD(Bytes.size()), &Count, &Pending);
    if (!Done) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        QElapsedTimer Timer; Timer.start();
        while ((!Cancel || !Cancel->load()) && Timer.elapsed() < Timeout) {
            if (WaitForSingleObject(Event.value_, 50) == WAIT_OBJECT_0) break;
        }
        if ((Cancel && Cancel->load()) || Timer.elapsed() >= Timeout) {
            CancelIoEx(Pipe, &Pending); GetOverlappedResult(Pipe, &Pending, &Count, TRUE); return false;
        }
        if (!GetOverlappedResult(Pipe, &Pending, &Count, FALSE)) return false;
    }
    if (Write) return Count == DWORD(Bytes.size());
    Bytes.resize(int(Count)); return Count > 0;
}
bool send(HANDLE Pipe, QJsonObject Message, const QString& Nonce, const std::atomic_bool* Cancel = nullptr) {
    Message["protocol"] = "hc.fileLocks"; Message["version"] = 1; Message["nonce"] = Nonce;
    auto Bytes = QJsonDocument(Message).toJson(QJsonDocument::Compact);
    return Bytes.size() <= frameLimit && io(Pipe, true, Bytes, 5000, Cancel);
}
std::optional<QJsonObject> receive(HANDLE Pipe, const QString& Nonce, DWORD Timeout, const std::atomic_bool* Cancel = nullptr) {
    QByteArray Bytes(frameLimit, '\0');
    if (!io(Pipe, false, Bytes, Timeout, Cancel)) return {};
    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error);
    if (Error.error != QJsonParseError::NoError || !Document.isObject()) return {};
    const auto Message = Document.object();
    if (Message["protocol"] != "hc.fileLocks" || Message["version"].toInt() != 1 || Message["nonce"].toString() != Nonce) return {};
    return Message;
}
QJsonObject publicRow(QJsonObject Row, const QString& Capability) {
    Row.remove("kernelPath");
    // Verification retains full native paths in the server, independently of
    // display truncation and the bounded transport frame.
    for (const auto& Key : {"path", "image", "user"}) if (Row[Key].toString().size() > 4096) Row[Key] = Row[Key].toString().left(4095) + QChar(0x2026);
    Row["capability"] = Capability; return Row;
}
}

FileLockClient::FileLockClient(QStringList Paths, bool Elevated, QString Helper, Launcher Launch)
    : paths_(std::move(Paths)), elevated_(Elevated), helper_(std::move(Helper)), nonce_(id()), launcher_(std::move(Launch)) {
    if (helper_.isEmpty()) helper_ = QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommanderAdmin.exe");
}
FileLockClient::~FileLockClient() {
    if (pipe_) CloseHandle(std::exchange(pipe_, nullptr));
    if (process_) {
        // This process is an isolated diagnostic instance, never another file job.
        if (WaitForSingleObject(process_, 100) == WAIT_TIMEOUT) TerminateProcess(process_, ERROR_CANCELLED);
        CloseHandle(process_);
    }
}
void FileLockClient::Cancel() { cancelled_ = true; }
bool FileLockClient::start() {
    if (pipe_ || cancelled_ || paths_.isEmpty() || paths_.size() > 32 || !QFileInfo::exists(helper_)) return false;
    for (const auto& Path : paths_) if (!IsLocalLockPath(Path)) return false;
    const auto Sid = ProcessUserSid(GetCurrentProcess());
    if (Sid.isEmpty()) return false;
    const auto Sddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + Sid + ')';
    PSECURITY_DESCRIPTOR Descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()), SDDL_REVISION_1, &Descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES Security{sizeof(Security), Descriptor, FALSE};
    const auto Name = pipeName(nonce_);
    pipe_ = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(Name.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, frameLimit, frameLimit, 0, &Security);
    LocalFree(Descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) { pipe_ = nullptr; return false; }
    QJsonArray Targets; for (const auto& Path : paths_) Targets.append(Path);
    const QJsonObject Bootstrap{{"version",1},{"nonce",nonce_},{"pid",int(GetCurrentProcessId())},{"created",ProcessCreationTime(GetCurrentProcess())},
        {"callerSid",Sid},{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"paths",Targets},{"elevated",elevated_}};
    const auto Payload = QJsonDocument(Bootstrap).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (Payload.size() > 24000) return false;
    const auto Arguments = "--file-lock-session " + QString::fromLatin1(Payload);
    if (launcher_) process_ = launcher_(helper_, Arguments, elevated_);
    else if (elevated_) {
        const auto Program = QDir::toNativeSeparators(helper_);
        SHELLEXECUTEINFOW Info{}; Info.cbSize = sizeof(Info); Info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        Info.lpVerb = L"runas"; Info.lpFile = reinterpret_cast<LPCWSTR>(Program.utf16()); Info.lpParameters = reinterpret_cast<LPCWSTR>(Arguments.utf16()); Info.nShow = SW_HIDE;
        if (ShellExecuteExW(&Info)) process_ = Info.hProcess;
    } else {
        auto Command = ('"' + QDir::toNativeSeparators(helper_) + "\" " + Arguments).toStdWString();
        STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Process{};
        if (CreateProcessW(reinterpret_cast<LPCWSTR>(helper_.utf16()), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process)) {
            process_ = Process.hProcess; CloseHandle(Process.hThread);
        }
    }
    if (!process_) return false;
    Handle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED Pending{}; Pending.hEvent = Event.value_;
    bool Connected = ConnectNamedPipe(pipe_, &Pending); const auto Error = Connected ? ERROR_SUCCESS : GetLastError();
    if (!Connected && Error == ERROR_PIPE_CONNECTED) Connected = true;
    if (!Connected && Error == ERROR_IO_PENDING) {
        QElapsedTimer Timer; Timer.start();
        while (!cancelled_ && Timer.elapsed() < 30000 && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            if (WaitForSingleObject(Event.value_, 50) == WAIT_OBJECT_0) { DWORD Bytes = 0; Connected = GetOverlappedResult(pipe_, &Pending, &Bytes, FALSE); break; }
        }
        if (!Connected) { CancelIoEx(pipe_, &Pending); DWORD Bytes = 0; GetOverlappedResult(pipe_, &Pending, &Bytes, TRUE); }
    }
    ULONG Peer = 0;
    if (!Connected || !GetNamedPipeClientProcessId(pipe_, &Peer) || Peer != GetProcessId(process_)
        || !sameImage(ProcessImage(process_), QFileInfo(helper_).absoluteFilePath())) return false;
    return true;
}
QJsonObject FileLockClient::exchange(QJsonObject Message, unsigned long Timeout) {
    const bool Action = Message["kind"] == "action";
    if (cancelled_) return transportError(false, ERROR_CANCELLED);
    if ((!pipe_ && !start()) || !process_) {
        const DWORD Error = GetLastError(); cancelled_ = true;
        return transportError(false, Error);
    }
    const auto RequestId = Message["requestId"].toString();
    if (!send(pipe_, Message, nonce_, &cancelled_)) return transportError(Action, ERROR_BROKEN_PIPE);
    auto Reply = receive(pipe_, nonce_, Timeout, &cancelled_);
    if (!Reply || (*Reply)["requestId"].toString() != RequestId) {
        cancelled_ = true;
        return transportError(Action, ERROR_TIMEOUT);
    }
    return *Reply;
}
QJsonObject FileLockClient::Query() {
    std::lock_guard Lock(mutex_);
    auto Reply = exchange({{"kind","query"},{"requestId",id()}}, 30000);
    QJsonArray Rows = Reply["rows"].toArray();
    const auto Generation = Reply["generation"].toString();
    int Next = Reply["next"].toInt(-1);
    while (Next >= 0 && !cancelled_ && Rows.size() < 2000) {
        auto Page = exchange({{"kind","page"},{"requestId",id()},{"generation",Generation},{"offset",Next}}, 5000);
        if (Page["outcome"] != "Completed" || Page["generation"].toString() != Generation) { Reply["partial"] = true; Reply["outcome"] = "Unavailable"; break; }
        for (const auto& Row : Page["rows"].toArray()) Rows.append(Row);
        const int Previous = Next; Next = Page["next"].toInt(-1);
        if (Next >= 0 && Next <= Previous) { Reply["partial"] = true; break; }
    }
    Reply["rows"] = Rows; return Reply;
}
QJsonObject FileLockClient::Act(const QString& Capability, const QString& Generation, const QString& Action, const QString& RequestId) {
    std::lock_guard Lock(mutex_);
    return exchange({{"kind","action"},{"requestId",RequestId},{"capability",Capability},{"generation",Generation},{"action",Action},{"approved",true}}, 10000);
}

unsigned long ExecuteFileLockSession(const QJsonObject& Bootstrap, const QString& ExpectedCaller) {
    const auto Nonce = Bootstrap["nonce"].toString(); const auto Stamp = QDateTime::fromString(Bootstrap["utc"].toString(), Qt::ISODateWithMs);
    const auto Age = Stamp.msecsTo(QDateTime::currentDateTimeUtc());
    if (Bootstrap["version"].toInt() != 1 || QUuid(Nonce).isNull() || !Stamp.isValid() || Age < -5000 || Age > 120000
        || Bootstrap["paths"].toArray().isEmpty() || Bootstrap["paths"].toArray().size() > 32) return ERROR_INVALID_PARAMETER;
    QStringList Paths;
    for (const auto& Value : Bootstrap["paths"].toArray()) {
        if (!Value.isString() || !IsLocalLockPath(Value.toString())) return ERROR_INVALID_PARAMETER;
        Paths.append(Value.toString());
    }
    const auto Pid = DWORD(Bootstrap["pid"].toInt());
    Handle Parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Pid));
    const auto Trusted = ExpectedCaller.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommander.exe") : ExpectedCaller;
    if (!Parent.value_ || ProcessCreationTime(Parent.value_) != Bootstrap["created"].toString()
        || ProcessUserSid(Parent.value_) != Bootstrap["callerSid"].toString()
        || !sameImage(ProcessImage(Parent.value_), QFileInfo(Trusted).absoluteFilePath())) return ERROR_ACCESS_DENIED;
    if (Bootstrap["elevated"].toBool()) {
        HANDLE Raw = nullptr; TOKEN_ELEVATION Elevation{}; DWORD Size = 0;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw)) return ERROR_ELEVATION_REQUIRED;
        Handle Token(Raw);
        if (!GetTokenInformation(Raw, TokenElevation, &Elevation, sizeof(Elevation), &Size) || !Elevation.TokenIsElevated) return ERROR_ELEVATION_REQUIRED;
    } else if (ProcessUserSid(GetCurrentProcess()) != Bootstrap["callerSid"].toString()) return ERROR_ACCESS_DENIED;
    const auto Name = pipeName(Nonce);
    if (!WaitNamedPipeW(reinterpret_cast<LPCWSTR>(Name.utf16()), 5000)) return GetLastError();
    Handle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(Name.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    ULONG Peer = 0;
    if (Pipe.value_ == INVALID_HANDLE_VALUE || !GetNamedPipeServerProcessId(Pipe.value_, &Peer) || Peer != Pid) return ERROR_ACCESS_DENIED;
    DWORD Mode = PIPE_READMODE_MESSAGE; if (!SetNamedPipeHandleState(Pipe.value_, &Mode, nullptr, nullptr)) return GetLastError();
    QJsonArray Rows; QHash<QString,QJsonObject> Capabilities, Receipts;
    QString Generation; QDateTime Queried; bool Partial = false; QStringList Reasons;
    while (WaitForSingleObject(Parent.value_, 0) == WAIT_TIMEOUT) {
        const auto Request = receive(Pipe.value_, Nonce, 120000);
        if (!Request) return ERROR_BROKEN_PIPE;
        const auto Kind = (*Request)["kind"].toString(), RequestId = (*Request)["requestId"].toString();
        if (QUuid(RequestId).isNull()) return ERROR_INVALID_PARAMETER;
        QJsonObject Reply{{"requestId",RequestId},{"outcome","Rejected"}};
        if (Kind == "query") {
            Capabilities.clear(); Rows = {}; Generation = id();
            auto Found = Scan(Paths, Pid, [&]{return WaitForSingleObject(Parent.value_,0) != WAIT_TIMEOUT;});
            Partial = Found.partial_; Reasons = Found.reasons_; Queried = QDateTime::currentDateTimeUtc();
            for (const auto& Value : Found.rows_) { const auto Capability = id(); Capabilities.insert(Capability, Value.toObject()); Rows.append(publicRow(Value.toObject(),Capability)); }
        }
        if (Kind == "query" || (Kind == "page" && (*Request)["generation"].toString() == Generation)) {
            int Offset = Kind == "query" ? 0 : (*Request)["offset"].toInt(-1);
            if (Offset >= 0 && Offset <= Rows.size()) {
                QJsonArray Page;
                while (Offset < Rows.size() && Page.size() < 128) {
                    auto Candidate = Page; Candidate.append(Rows[Offset]);
                    if (QJsonDocument(Candidate).toJson(QJsonDocument::Compact).size() > 58000) break;
                    Page = Candidate; ++Offset;
                }
                Reply["outcome"] = "Completed"; Reply["rows"] = Page; Reply["generation"] = Generation;
                Reply["next"] = Offset < Rows.size() ? Offset : -1; Reply["partial"] = Partial;
                Reply["reasons"] = QJsonArray::fromStringList(Reasons);
            }
        } else if (Kind == "action") {
            if (Receipts.contains(RequestId)) Reply = Receipts.value(RequestId);
            else if (Receipts.size() < 128 && (*Request)["approved"].isBool() && (*Request)["approved"].toBool()
                && (*Request)["generation"].toString() == Generation && !Queried.isNull() && Queried.msecsTo(QDateTime::currentDateTimeUtc()) <= 120000
                && Capabilities.contains((*Request)["capability"].toString())) {
                const auto Row = Capabilities.value((*Request)["capability"].toString());
                Capabilities.clear(); // Any attempt consumes every capability from this snapshot.
                Reply = ExecuteAction(Row, (*Request)["action"].toString(), Pid); Reply["requestId"] = RequestId;
                Receipts.insert(RequestId, Reply);
            }
        }
        if (!send(Pipe.value_, Reply, Nonce)) return ERROR_BROKEN_PIPE;
    }
    return ERROR_SUCCESS;
}
}
