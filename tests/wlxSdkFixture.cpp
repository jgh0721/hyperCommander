#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
extern "C"
{
#include "externalLibs/TotalCommander/tc_listplug.h"
}
#include <cstring>

namespace
{
    DWORD sdkThread_ = 0;
    unsigned defaultsCount_ = 0, detectCount_ = 0;
    bool valid_ = false;
    char mode_[64]{};
    char expression_[2048]{};
    int setting_ = 0;
    void Crash() { RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr); }
}
extern "C" void __stdcall ListSetDefaultParams(ListDefaultParamStruct* Parameters)
{
    ++defaultsCount_; sdkThread_ = GetCurrentThreadId();
    valid_ = Parameters && Parameters->size == sizeof(ListDefaultParamStruct)
        && Parameters->PluginInterfaceVersionHi == 2 && Parameters->PluginInterfaceVersionLow == 10
        && std::memchr(Parameters->DefaultIniName, '\0', MAX_PATH);
    if (!valid_) return;
    GetPrivateProfileStringA("fixture", "mode", "normal", mode_, sizeof(mode_), Parameters->DefaultIniName);
    setting_ = GetPrivateProfileIntA("fixture", "value", -1, Parameters->DefaultIniName);
    GetPrivateProfileStringA("fixture", "expression", "EXT=\"TXT\"", expression_, sizeof(expression_), Parameters->DefaultIniName);
    if (std::strcmp(mode_, "hang-defaults") == 0) Sleep(INFINITE);
    if (std::strcmp(mode_, "crash-defaults") == 0) Crash();
}
extern "C" void __stdcall ListGetDetectString(char* Buffer, int Capacity)
{
    ++detectCount_;
    if (std::strcmp(mode_, "hang-detect") == 0) Sleep(INFINITE);
    if (std::strcmp(mode_, "crash-detect") == 0) Crash();
    if (std::strcmp(mode_, "unterminated") == 0) { std::memset(Buffer, 'X', Capacity); return; }
    if (std::strcmp(mode_, "maximum") == 0) { std::memset(Buffer, 'X', Capacity - 1); Buffer[Capacity - 1] = '\0'; return; }
    if (std::strcmp(mode_, "empty") == 0) { Buffer[0] = '\0'; return; }
    if (std::strcmp(mode_, "ansi") == 0) { const char Bytes[] = {'[', '0', ']', '=', '"', char(0xe9), '"', '\0'}; memcpy_s(Buffer, Capacity, Bytes, sizeof(Bytes)); return; }
    const auto Expression = valid_ && defaultsCount_ == 1 && detectCount_ == 1 && sdkThread_ == GetCurrentThreadId()
        && setting_ == 123 ? expression_ : "SDK_ORDER_ERROR";
    strcpy_s(Buffer, Capacity, Expression);
}
extern "C" HWND __stdcall ListLoadW(HWND Parent, wchar_t*, int Flags)
{
    if (!valid_ || defaultsCount_ != 1 || setting_ != 123 || sdkThread_ != GetCurrentThreadId()) return nullptr;
    if (std::strcmp(mode_, "reject") == 0) return nullptr;
    if (std::strcmp(mode_, "require-force") == 0 && !(Flags & lcp_forceshow)) return nullptr;
    return CreateWindowExW(0, L"STATIC", L"WLX SDK initialized", WS_CHILD | WS_VISIBLE,
        0, 0, 100, 40, Parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
extern "C" void __stdcall ListCloseWindow(HWND Window) { DestroyWindow(Window); }
extern "C" int __stdcall ListSearchTextW(HWND Window, wchar_t* Text, int Flags)
{
    if (!IsWindow(Window) || sdkThread_ != GetCurrentThreadId() || Flags < 0 || Flags > 15) return LISTPLUGIN_ERROR;
    if (wcscmp(Text, L"crash-search") == 0) Crash();
    if (wcscmp(Text, L"hang-search") == 0) Sleep(INFINITE);
    if (wcscmp(Text, L"한글") != 0) return LISTPLUGIN_ERROR;
    SetWindowTextW(Window, Text); return LISTPLUGIN_OK;
}
