#include "nativeIsoRuntime.hpp"
#include "builtinIso.hpp"
#include <QCryptographicHash>
#include <QFileInfo>

namespace hc::iso {
using namespace storage;
Runtime::~Runtime()
{
    if (module_) FreeLibrary(module_);
    for (auto Pin : pins_) CloseHandle(Pin);
}
Error EngineError(int Status)
{
    const auto Category = Status == HC_ISO_CANCELLED ? ErrorCategory::Cancelled
        : Status == HC_ISO_UNSUPPORTED ? ErrorCategory::Unsupported
        : Status == HC_ISO_END ? ErrorCategory::NotFound
        : Status == HC_ISO_INVALID ? ErrorCategory::InvalidRequest : ErrorCategory::Unavailable;
    return {Category, "iso-engine-" + std::to_string(Status), {"iso"}};
}
Result<std::shared_ptr<Runtime>> LoadRuntime(const OperationContext& Context)
{
    if (auto Error = Context.Check({"iso"})) return *Error;
    static std::mutex Mutex;
    static std::weak_ptr<Runtime> Cached;
    std::lock_guard Lock(Mutex);
    if (auto Existing = Cached.lock()) return Existing;
    auto Value = std::make_shared<Runtime>();
    const auto Options = plugins::BuiltinIsoOptions();
    const auto Expected = Options.approval_.reviewed_.dependencies_.find("hc_iso_core.dll");
    if (Expected == Options.approval_.reviewed_.dependencies_.end())
        return Error{ErrorCategory::AccessDenied, "iso-core-pin-missing", {"iso"}};
    const auto Path = QFileInfo(Options.binaryPath_).absolutePath() + "/hc_iso_core.dll";
    auto Directory = QFileInfo(Path).absolutePath();
    for (;;) {
        const auto Pin = CreateFileW(reinterpret_cast<LPCWSTR>(Directory.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (Pin == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "iso-core-directory-pin", {"iso"}};
        Value->pins_.push_back(Pin);
        const auto Parent = QFileInfo(Directory).absolutePath();
        if (Parent == Directory) break;
        Directory = Parent;
    }
    const auto File = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "iso-core-file-pin", {"iso"}};
    Value->pins_.push_back(File);
    QCryptographicHash Hash(QCryptographicHash::Sha256);
    char Buffer[65536]; DWORD Read = 0;
    for (;;) {
        if (auto Error = Context.Check({"iso"})) return *Error;
        if (!ReadFile(File, Buffer, sizeof(Buffer), &Read, nullptr))
            return Error{ErrorCategory::Unavailable, "iso-core-hash-read", {"iso"}};
        if (!Read) break;
        Hash.addData(QByteArrayView(Buffer, Read));
    }
    if (Hash.result().toHex().toStdString() != Expected->second)
        return Error{ErrorCategory::AccessDenied, "iso-core-hash-mismatch", {"iso"}};
    Value->module_ = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Path.utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Value->module_) return Error{ErrorCategory::Unavailable, "iso-core-load", {"iso"}};
#define HC_ISO_RESOLVE(Member, Name) \
    Value->Member = reinterpret_cast<decltype(Value->Member)>(GetProcAddress(Value->module_, #Name)); \
    if (!Value->Member) return Error{ErrorCategory::Unsupported, "iso-core-abi", {"iso"}}
    HC_ISO_RESOLVE(openInput_, hcIsoOpenInput);
    HC_ISO_RESOLVE(close_, hcIsoClose);
    HC_ISO_RESOLVE(info_, hcIsoGetInfo);
    HC_ISO_RESOLVE(entry_, hcIsoGetEntry);
    HC_ISO_RESOLVE(find_, hcIsoFindEntry);
    HC_ISO_RESOLVE(child_, hcIsoGetChild);
    HC_ISO_RESOLVE(read_, hcIsoRead);
    HC_ISO_RESOLVE(build_, hcIsoBuild);
#undef HC_ISO_RESOLVE
    Cached = Value;
    return Value;
}
}
