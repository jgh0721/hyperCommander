#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincrypt.h>
// The legacy SDK expects this alias from its original precompiled header.
using LPCBYTE = const BYTE*;
#include "plugin support.h"
#include "viewer plugins.h"
#include "viewerHostBridge.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include <cwchar>
#include <algorithm>
#include <type_traits>
#include <cstdint>
#include <vector>

namespace
{
    constexpr auto ClassName = L"HyperViewer.Container.P0";
    using Bridge = std::shared_ptr<hc::document::ViewerHostBridge>;
    LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* Owner = reinterpret_cast<Bridge*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_CREATE)
        {
            Owner = new Bridge(std::make_shared<hc::document::ViewerHostBridge>(Window));
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Owner)); (*Owner)->Start(); return 0;
        }
        if (Owner)
        {
            const auto Self = *Owner;
            if (Message == DVPLUGINMSG_LOADW && LParam) { return Self->Load(reinterpret_cast<const wchar_t*>(LParam)) ? TRUE : FALSE; }
            if (Message == DVPLUGINMSG_SETABORTEVENT) { Self->SetAbort(reinterpret_cast<HANDLE>(LParam)); return TRUE; }
            if (Message == WM_SIZE || Message == DVPLUGINMSG_RESIZE) return 0;
            if (Message == WM_SETFOCUS) { Self->Focus(); return 0; }
            if (Message == WM_PAINT)
            {
                PAINTSTRUCT Paint{}; auto Dc = BeginPaint(Window, &Paint); RECT Rect{}; GetClientRect(Window, &Rect);
                FillRect(Dc, &Rect, GetSysColorBrush(COLOR_WINDOW));
                DrawTextW(Dc, Self->Failed() ? L"HyperViewer failed to initialize." : L"HyperViewer is opening...", -1, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                EndPaint(Window, &Paint); return 0;
            }
            if (Message == WM_NCDESTROY) { SetWindowLongPtrW(Window, GWLP_USERDATA, 0); Self->Stop(); delete Owner; }
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
    void Copy(wchar_t* Buffer, UINT Capacity, const wchar_t* Text) { if (Buffer && Capacity) wcsncpy_s(Buffer, Capacity, Text, _TRUNCATE); }
    bool LooksLikeText(const wchar_t* Path, HANDLE Abort)
    {
        if (Abort && WaitForSingleObject(Abort, 0) == WAIT_OBJECT_0) return false;
        HANDLE File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE) return false;
        std::vector<char> Sample(64 * 1024); DWORD Count = 0;
        const bool Read = ReadFile(File, Sample.data(), DWORD(Sample.size()), &Count, nullptr);
        CloseHandle(File); if (!Read) return false;
        Sample.resize(Count);
        for (int Trim = 0; Trim <= 3 && Trim <= int(Sample.size()); ++Trim) {
            const int Length = int(Sample.size()) - Trim;
            const bool Le = Length >= 2 && BYTE(Sample[0]) == 0xff && BYTE(Sample[1]) == 0xfe;
            const bool Be = Length >= 2 && BYTE(Sample[0]) == 0xfe && BYTE(Sample[1]) == 0xff;
            std::wstring Text;
            if (Le || Be) {
                if (Length % 2) continue;
                Text.resize(Length / 2);
                for (int I = 0; I < Length / 2; ++I)
                    Text[I] = wchar_t(Le ? (BYTE(Sample[2 * I + 1]) << 8) | BYTE(Sample[2 * I])
                        : (BYTE(Sample[2 * I]) << 8) | BYTE(Sample[2 * I + 1]));
            } else {
                UINT CodePage = CP_UTF8;
                int WideLength = MultiByteToWideChar(CodePage, MB_ERR_INVALID_CHARS, Sample.data(), Length, nullptr, 0);
                if (!WideLength && Length) { CodePage = 949; WideLength = MultiByteToWideChar(CodePage, MB_ERR_INVALID_CHARS, Sample.data(), Length, nullptr, 0); }
                if (!WideLength && Length) continue;
                Text.resize(WideLength);
                if (WideLength && MultiByteToWideChar(CodePage, MB_ERR_INVALID_CHARS, Sample.data(), Length, Text.data(), WideLength) != WideLength) continue;
            }
            return std::all_of(Text.begin(), Text.end(), [](wchar_t Ch) {
                return Ch >= 32 || Ch == L'\t' || Ch == L'\r' || Ch == L'\n' || Ch == L'\f';
            });
        }
        return false;
    }
}
extern "C" BOOL DVP_InitEx(LPDVPINITEXDATA Data) { return Data && Data->cbSize >= sizeof(DVPINITEXDATA); }
extern "C" void DVP_Uninit() {}
extern "C" BOOL DVP_IdentifyW(LPVIEWERPLUGININFOW Info)
{
    if (!Info || Info->cbSize < sizeof(VIEWERPLUGININFOW)) return FALSE;
    Info->dwFlags = DVPFIF_ExtensionsOnlyIfSlow | DVPFIF_ZeroBytesOk | DVPFIF_NoThumbnails;
    std::wstring Extensions;
    for (const auto Item : hc::document::DocumentExtensions) if (!Item.empty()) {
        if (!Extensions.empty()) Extensions += L';'; Extensions += L'.'; Extensions.append(Item.begin(), Item.end());
    }
    Copy(Info->lpszHandleExts, Info->cchHandleExtsMax, Extensions.c_str());
    Copy(Info->lpszName, Info->cchNameMax, L"HyperViewer");
    Copy(Info->lpszDescription, Info->cchDescriptionMax, L"HyperViewer PDF, text and image workspace");
    Info->dwVersionHigh = 0; Info->dwVersionLow = 6; Info->uiMajorFileType = DVPMajorType_Other;
    Info->idPlugin = {0x5095a16b, 0xe90f, 0x4b39, {0xa0, 0x70, 0x7d, 0xdd, 0x52, 0x65, 0x62, 0x01}};
    return TRUE;
}
extern "C" BOOL DVP_IdentifyFileW(HWND, LPWSTR File, LPVIEWERPLUGINFILEINFOW Info, HANDLE Abort)
{
    if (!File || !Info || Info->cbSize < sizeof(VIEWERPLUGINFILEINFOW) || (Abort && WaitForSingleObject(Abort, 0) == WAIT_OBJECT_0)) return FALSE;
    const auto Slash = std::max(wcsrchr(File, L'\\'), wcsrchr(File, L'/'));
    const auto Dot = wcsrchr(File, L'.'); const auto Ext = Dot && (!Slash || Dot > Slash) ? Dot : L"";
    bool Supported = false;
    for (const auto Item : hc::document::DocumentExtensions) {
        if (Item.empty()) continue;
        std::wstring Candidate; if (!Item.empty()) { Candidate = L"."; Candidate.append(Item.begin(), Item.end()); }
        Supported |= _wcsicmp(Ext, Candidate.c_str()) == 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA Attributes{};
    if (!GetFileAttributesExW(File, GetFileExInfoStandard, &Attributes) || (Attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    bool Image = false;
    for(const auto* Candidate:{L".png",L".jpg",L".jpeg",L".bmp",L".gif",L".tif",L".tiff",L".webp",L".ico",L".svg"})
        Image |= _wcsicmp(Ext,Candidate)==0;
    if (!Supported && !Image && !(Supported = LooksLikeText(File, Abort))) return FALSE;
    const auto Bytes = (std::uint64_t(Attributes.nFileSizeHigh) << 32) | Attributes.nFileSizeLow;
    if (!Image && Bytes > hc::document::DocumentByteLimit) return FALSE;
    Info->dwFlags = DVPFIF_CanReturnViewer; Info->wMajorType = _wcsicmp(Ext,L".pdf")==0?DVPMajorType_Other:Image?DVPMajorType_Image:DVPMajorType_Text;
    Copy(Info->lpszInfo, Info->cchInfoMax, L"PDF / text / image"); return TRUE;
}
extern "C" HWND DVP_CreateViewer(HWND Parent, LPRECT Rect, DWORD)
{
    if (!Parent || !IsWindow(Parent) || !Rect) return nullptr;
    HMODULE Module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WindowProc), &Module);
    WNDCLASSW Class{}; Class.hInstance = Module; Class.lpszClassName = ClassName; Class.lpfnWndProc = WindowProc; Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowExW(0, ClassName, L"HyperViewer", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        Rect->left, Rect->top, Rect->right - Rect->left, Rect->bottom - Rect->top, Parent, nullptr, Module, nullptr);
}
extern "C" BOOL HC_DocumentCommand(HWND Window, const char* Input, UINT Size, char* Output, UINT* Capacity)
{
    wchar_t Name[128]{};
    if (!Window || GetClassNameW(Window, Name, 128) == 0 || wcscmp(Name, ClassName) || !Capacity || !Output) return FALSE;
    auto* Owner = reinterpret_cast<Bridge*>(GetWindowLongPtrW(Window, GWLP_USERDATA)); if (!Owner) return FALSE;
    const auto Self = *Owner; std::string Reply;
    if (!Self->Exchange(Input, Size, Reply)) Reply = Self->Diagnostic();
    if (Reply.size() > *Capacity) return FALSE;
    std::copy(Reply.begin(), Reply.end(), Output); *Capacity = UINT(Reply.size()); return TRUE;
}

static_assert(std::is_same_v<decltype(&DVP_InitEx), PFNDVPINITEX>);
static_assert(std::is_same_v<decltype(&DVP_Uninit), PFNDVPUNINIT>);
static_assert(std::is_same_v<decltype(&DVP_IdentifyW), PFNDVPIDENTIFYW>);
static_assert(std::is_same_v<decltype(&DVP_IdentifyFileW), PFNDVPIDENTIFYFILEW>);
static_assert(std::is_same_v<decltype(&DVP_CreateViewer), PFNDVPCREATEVIEWER>);
