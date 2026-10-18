#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Windows x64 Unicode ABI described by the official UnEGG package.
// Interface declarations only; no vendor implementation or SDK is redistributed.
// License: https://secure.altools.co.kr/support/License.aspx (EGG section).
namespace hc::plugins::egg {
using Handle = size_t;
using Callback = UINT(__cdecl*)(void*, UINT, void*);
inline constexpr UINT Success = 1, Skip = 2, Cancel = 3;
struct Header {
    size_t size_; const wchar_t* name_; const wchar_t* comment_;
    uint64_t packed_, unpacked_; uint32_t attributes_, crc_, method_;
    int64_t modified_; BOOL encrypted_;
};
struct Extract { size_t index_; const wchar_t* name_; wchar_t path_[MAX_PATH]; };
struct Progress { short total_, current_; };
struct Start { UINT code_; };
struct Finish { UINT code_, result_; };
struct ActivityStart { size_t index_; UINT code_; };
struct ActivityFinish { size_t index_; UINT code_, result_; };
struct Password { size_t index_, attempt_; wchar_t value_[1024]; };
struct Error { size_t index_; UINT code_; };
struct Test { size_t index_; const wchar_t* name_; BOOL skip_; };
using Create = UINT(__cdecl*)(Handle*, Callback, void*);
using Destroy = UINT(__cdecl*)(Handle*);
using Validate = UINT(__cdecl*)(const wchar_t*, int*);
using Open = UINT(__cdecl*)(Handle, const wchar_t*);
using Action = UINT(__cdecl*)(Handle);
using Count = UINT(__cdecl*)(Handle, size_t*);
using GetHeader = UINT(__cdecl*)(Handle, size_t, Header*);
static_assert(sizeof(void*) == 8 && sizeof(Header) == 72 && offsetof(Header, modified_) == 56);
static_assert(sizeof(Extract) == 536 && sizeof(Password) == 2064 && sizeof(Test) == 24);
}
