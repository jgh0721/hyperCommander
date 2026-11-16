#include "stdafx.h"
#include "environmentPathProbe.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <AclAPI.h>
#include <future>
#include <thread>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentPathProbe, "hc.environment.pathProbe")
namespace hc::environment {
namespace {
std::atomic_int Outstanding{0};
DirectoryProbe InspectDirectory(const std::wstring& Path)
{
    DirectoryProbe Result;
    const DWORD Attributes = GetFileAttributesW(Path.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
        const auto Error = GetLastError();
        Result.status_ = Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND || Error == ERROR_INVALID_NAME ? ProbeStatus::Missing : ProbeStatus::Denied;
        return Result;
    }
    if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY)) return Result;
    Result.status_ = ProbeStatus::Exists;
    const auto Start = std::chrono::steady_clock::now();
    WIN32_FIND_DATAW Entry{};
    auto Pattern = Path; if (!Pattern.empty() && Pattern.back() != L'\\') Pattern += L'\\'; Pattern += L'*';
    const HANDLE Find = FindFirstFileExW(Pattern.c_str(), FindExInfoBasic, &Entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (Find == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) Result.status_ = ProbeStatus::Denied;
    } else {
        size_t Count = 0;
        do {
            if (++Count > 16384 || std::chrono::steady_clock::now() - Start > std::chrono::milliseconds(200)) { Result.status_ = ProbeStatus::Timeout; break; }
            if (!(Entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) Result.files_.emplace_back(Entry.cFileName);
            if ((Entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && Entry.dwReserved0 == IO_REPARSE_TAG_APPEXECLINK) Result.appAliases_.insert(Entry.cFileName);
        } while (FindNextFileW(Find, &Entry));
        const auto Error = GetLastError(); FindClose(Find);
        if (Result.status_ == ProbeStatus::Exists && Error != ERROR_NO_MORE_FILES) Result.status_ = ProbeStatus::Denied;
    }
    if (Result.status_ != ProbeStatus::Exists) { Result.files_.clear(); Result.appAliases_.clear(); return Result; }
    PACL Dacl = nullptr; PSECURITY_DESCRIPTOR Security = nullptr;
    const auto Error = GetNamedSecurityInfoW(Path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Dacl, nullptr, &Security);
    if (Error == ERROR_SUCCESS) {
        if (!Dacl) Result.writableByUsers_ = true;
        else {
            bool Complete = true, Writable = false;
            for (auto Kind : {WinWorldSid, WinBuiltinUsersSid, WinAuthenticatedUserSid}) {
                BYTE Buffer[SECURITY_MAX_SID_SIZE]; DWORD Size = sizeof(Buffer);
                if (!CreateWellKnownSid(Kind, nullptr, Buffer, &Size)) { Complete = false; continue; }
                TRUSTEE_W Trustee{}; BuildTrusteeWithSidW(&Trustee, Buffer);
                ACCESS_MASK Rights = 0;
                if (GetEffectiveRightsFromAclW(Dacl, &Trustee, &Rights) != ERROR_SUCCESS) { Complete = false; continue; }
                Writable |= (Rights & (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | GENERIC_WRITE | GENERIC_ALL)) != 0;
            }
            if (Complete || Writable) Result.writableByUsers_ = Writable;
        }
    }
    if (Security) LocalFree(Security);
    return Result;
}
}
DirectoryProbe NativePathProbe::Inspect(const std::wstring& Path)
{
    const auto Now = std::chrono::steady_clock::now();
    {
        std::lock_guard Lock(mutex_);
        const auto Found = cache_.find(Path);
        if (Found != cache_.end() && Now - Found->second.time_ < std::chrono::seconds(15)) return Found->second.value_;
    }
    // Without an explicit target working directory, relative paths cannot be
    // meaningfully checked against this process's unrelated current directory.
    const bool Absolute = Path.size() >= 3 && Path[1] == L':' && (Path[2] == L'\\' || Path[2] == L'/');
    if (!Absolute && !Path.starts_with(L"\\\\")) return {ProbeStatus::Denied};
    int Active = Outstanding.load();
    while (Active < 4 && !Outstanding.compare_exchange_weak(Active, Active + 1)) {}
    if (Active >= 4) return {ProbeStatus::Timeout};
    auto Promise = std::make_shared<std::promise<DirectoryProbe>>(); auto Future = Promise->get_future();
    std::thread Thread;
    try {
        Thread = std::thread([Path, Promise] {
            try { Promise->set_value(InspectDirectory(Path)); } catch (...) { Promise->set_value({ProbeStatus::Denied}); }
            --Outstanding;
        });
    } catch (...) { --Outstanding; return {ProbeStatus::Denied}; }
    DirectoryProbe Result;
    if (Future.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready) { Result = Future.get(); Thread.join(); }
    else {
        CancelSynchronousIo(Thread.native_handle());
        Thread.detach(); Result.status_ = ProbeStatus::Timeout;
    }
    { std::lock_guard Lock(mutex_); if (cache_.size() >= 512) cache_.clear(); cache_[Path] = {Result, Now}; }
    return Result;
}
void NativePathProbe::Invalidate() { std::lock_guard Lock(mutex_); cache_.clear(); }
}
