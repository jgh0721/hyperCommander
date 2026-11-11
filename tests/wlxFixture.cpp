#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <string>

namespace
{
    HINSTANCE instance_ = nullptr;
    struct Viewer { std::wstring file_; DWORD thread_; unsigned ticks_ = 0; };
    INT_PTR CALLBACK SettingsProc(HWND Window, UINT Message, WPARAM, LPARAM)
    {
        if (Message == WM_INITDIALOG)
        {
            SetWindowTextW(Window, L"WLX fixture settings");
            return TRUE;
        }
        if (Message == WM_APP + 81) { Sleep(INFINITE); return TRUE; }
        if (Message == WM_CLOSE) { EndDialog(Window, 0); return TRUE; }
        return FALSE;
    }
    void OpenSettings(HWND Window)
    {
        // The plugin owns a top-level modal parent. The application's root is
        // foreign to this restricted Job and cannot serve as its dialog owner.
        const auto Owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_OVERLAPPED,
            0, 0, 10, 10, nullptr, nullptr, instance_, nullptr);
        if (!Owner) return;
        struct Template { DLGTEMPLATE dialog_; WORD menu_ = 0, class_ = 0, title_ = 0; } Dialog{};
        Dialog.dialog_.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        Dialog.dialog_.cx = 180; Dialog.dialog_.cy = 80;
        DialogBoxIndirectParamW(instance_, &Dialog.dialog_, Owner, SettingsProc, 0);
        DestroyWindow(Owner);
    }
    LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* State = reinterpret_cast<Viewer*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE)
        {
            State = static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(State));
        }
        if (Message == WM_APP + 80) { OpenSettings(Window); return 0; }
        if (Message == WM_APP + 81) { SetWindowLongPtrW(Window, GWLP_ID, 81); Sleep(INFINITE); return 0; }
        if (Message == WM_APP + 83) { PostQuitMessage(0); return 0; }
        if (Message == WM_APP + 82) {
            const auto Menu = CreatePopupMenu(); AppendMenuW(Menu, MF_STRING, 1, L"Settings option");
            POINT Point{}; ClientToScreen(Window, &Point);
            TrackPopupMenu(Menu, TPM_RETURNCMD, Point.x, Point.y, 0, Window, nullptr);
            DestroyMenu(Menu); return 0;
        }
        if (Message == WM_TIMER && State)
        {
            if (++State->ticks_ >= 3 && State->file_.find(L"hang-message") != std::wstring::npos) Sleep(INFINITE);
            const auto Title = L"WLX fixture ready " + std::to_wstring(State->ticks_);
            SetWindowTextW(Window, Title.c_str()); InvalidateRect(Window, nullptr, TRUE); return 0;
        }
        if (Message == WM_PAINT)
        {
            PAINTSTRUCT Paint{}; auto Dc = BeginPaint(Window, &Paint); RECT Area{}; GetClientRect(Window, &Area);
            FillRect(Dc, &Area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            const auto Label = L"Hosted WLX Unicode viewer";
            TextOutW(Dc, 12, 12, Label, lstrlenW(Label)); EndPaint(Window, &Paint); return 0;
        }
        if (Message == WM_NCDESTROY) { delete State; SetWindowLongPtrW(Window, GWLP_USERDATA, 0); }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
}
extern "C" __declspec(dllexport) HWND __stdcall ListLoadW(HWND Parent, wchar_t* File, int)
{
    std::wstring Path(File);
    // Materialization preserves the extension, not the original basename.
    // Fault directives remain private to this test DLL.
    auto Content = CreateFileW(File, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (Content != INVALID_HANDLE_VALUE)
    {
        char Bytes[96]{}; DWORD Count = 0;
        if (ReadFile(Content, Bytes, sizeof(Bytes) - 1, &Count, nullptr))
        {
            const std::string Text(Bytes, Count);
            if (Text.starts_with("HC-WLX-FAULT:")) Path += std::wstring(Text.begin(), Text.end());
        }
        CloseHandle(Content);
    }
    if (Path.find(L"reject") != std::wstring::npos) return nullptr;
    if (Path.find(L"hang-load") != std::wstring::npos) Sleep(INFINITE);
    WNDCLASSW Class{}; Class.lpfnWndProc = WindowProc; Class.hInstance = instance_; Class.lpszClassName = L"HyperCommander.WlxFixture";
    if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    auto* State = new Viewer{Path, GetCurrentThreadId()};
    auto Window = CreateWindowExW(0, Class.lpszClassName, L"WLX fixture ready 0", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 10, 10, Parent, nullptr, instance_, State);
    if (!Window) return nullptr;
    SetTimer(Window, 1, 30, nullptr); return Window;
}
extern "C" __declspec(dllexport) void __stdcall ListCloseWindow(HWND Window)
{
    auto* State = reinterpret_cast<Viewer*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (State && State->file_.find(L"hang-close") != std::wstring::npos) Sleep(INFINITE);
    DestroyWindow(Window);
}
extern "C" __declspec(dllexport) int __stdcall ListSendCommand(HWND Window, int Command, int Parameter)
{
    auto* State = reinterpret_cast<Viewer*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (!State || State->thread_ != GetCurrentThreadId()) return 1;
    if (Command == 4 && Parameter == 78) { OpenSettings(Window); return 0; }
    if (Command == 4 && Parameter == 77) {
        const auto File = CreateFileW(State->file_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE) return 1;
        const char Text[] = "saved by WLX"; DWORD Written = 0;
        const bool Saved = WriteFile(File, Text, sizeof(Text) - 1, &Written, nullptr) && Written == sizeof(Text) - 1 && SetEndOfFile(File);
        CloseHandle(File); return Saved ? 0 : 1;
    }
    if (State->file_.find(L"crash-command") != std::wstring::npos) RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID)
{ if (Reason == DLL_PROCESS_ATTACH) instance_ = Instance; return TRUE; }
