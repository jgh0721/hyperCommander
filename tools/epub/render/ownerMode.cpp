#include "stdafx.h"
#include "ownerMode.hpp"

/// Platform/framework headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace hc::epub::poc
{
    struct OwnerSurface::State
    {
        HWND window = nullptr;
        OwnerMode owner;
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
        {
            auto* Self = reinterpret_cast<State*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
            if(Message == WM_NCCREATE) {
                Self = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
                Self->window = Window; SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
            }
            if(Message == WM_NCDESTROY && Self) { Self->window = nullptr; SetWindowLongPtrW(Window, GWLP_USERDATA, 0); }
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
    };
    OwnerSurface::OwnerSurface() : state_(std::make_unique<State>()) {}
    OwnerSurface::~OwnerSurface() { if(state_->window) DestroyWindow(state_->window); }
    bool OwnerSurface::Create(const OwnerMode& Owner)
    {
        if(state_->window) return false;
        state_->owner = Owner;
        DWORD Pid = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(Owner.parentWindow), &Pid);
        if(Pid != Owner.processId) return false;
        WNDCLASSW Class{}; Class.lpfnWndProc = State::windowProc; Class.hInstance = GetModuleHandleW(nullptr);
        Class.lpszClassName = L"HyperCommander.EpubPocSurface";
        if(!RegisterClassW(&Class)) return false;
        if(!CreateWindowExW(WS_EX_NOPARENTNOTIFY, Class.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, reinterpret_cast<HWND>(Owner.parentWindow), nullptr, Class.hInstance, state_.get())) return false;
        if(!Refresh()) return false;
        ShowWindow(state_->window, SW_SHOWNOACTIVATE); return true;
    }
    std::uintptr_t OwnerSurface::Window() const { return reinterpret_cast<std::uintptr_t>(state_->window); }
    QSize OwnerSurface::Size() const
    { RECT Rect{}; return state_->window && GetClientRect(state_->window, &Rect) ? QSize(Rect.right, Rect.bottom) : QSize(); }
    bool OwnerSurface::Refresh(std::uintptr_t Reader)
    {
        if(!state_->window || !RefreshOwnerWindow(Window(), state_->owner)) return false;
        if(!Reader) return true;
        DWORD Pid = 0; const auto Child = reinterpret_cast<HWND>(Reader); GetWindowThreadProcessId(Child, &Pid);
        return Pid == GetCurrentProcessId() && GetParent(Child) == state_->window;
    }
    OwnerMode ParseOwnerMode()
    {
        int Count = 0; auto* Raw = CommandLineToArgvW(GetCommandLineW(), &Count);
        if(!Raw) throw QStringLiteral("Cannot read native arguments");
        QStringList Args; for(int Index = 0; Index < Count; ++Index) Args.append(QString::fromWCharArray(Raw[Index]));
        LocalFree(Raw);
        OwnerMode Mode;
        if(!Args.contains("--web-owner")) return Mode;
        if(Args.size() != 10 || Args[1] != "--web-owner" || Args[2] != "--pipe" || Args[4] != "--connection"
            || Args[6] != "--parent-process" || Args[8] != "--parent"
            || !Args[3].startsWith(QStringLiteral(R"(\\.\pipe\hc-web-owner-)"))) throw QStringLiteral("Invalid owner arguments");
        Mode.enabled = true; Mode.pipe = Args[3]; Mode.connection = Args[5].toLatin1();
        bool PidOk = false, ParentOk = false;
        Mode.processId = Args[7].toUInt(&PidOk); const auto Parent = Args[9].toULongLong(&ParentOk);
        if(!PidOk || !Mode.processId || !ParentOk || !Parent || Parent > UINTPTR_MAX
            || QString::number(Mode.processId) != Args[7] || QString::number(Parent) != Args[9]
            || Mode.connection.size() != 32 || QByteArray::fromHex(Mode.connection).toHex() != Mode.connection)
            throw QStringLiteral("Invalid owner identity");
        Mode.parentWindow = static_cast<std::uintptr_t>(Parent);
        BOOL InJob = FALSE; JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{}; JOBOBJECT_EXTENDED_LIMIT_INFORMATION Job{};
        if(!IsProcessInJob(GetCurrentProcess(), nullptr, &InJob) || !InJob
            || !QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &Ui, sizeof(Ui), nullptr) || Ui.UIRestrictionsClass
            || !QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &Job, sizeof(Job), nullptr)
            || !(Job.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
            || (Job.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)))
            throw QStringLiteral("Incompatible owner Job policy");
        DWORD Pid = 0; const auto ParentHandle = reinterpret_cast<HWND>(Mode.parentWindow);
        GetWindowThreadProcessId(ParentHandle, &Pid);
        if(Pid != Mode.processId) throw QStringLiteral("Invalid owner window");
        SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(ParentHandle));
        return Mode;
    }
    bool RefreshOwnerWindow(std::uintptr_t Child, const OwnerMode& Owner)
    {
        const auto Window = reinterpret_cast<HWND>(Child), Parent = reinterpret_cast<HWND>(Owner.parentWindow);
        DWORD ParentPid = 0, ChildPid = 0;
        GetWindowThreadProcessId(Parent, &ParentPid); GetWindowThreadProcessId(Window, &ChildPid);
        RECT Rect{};
        return ParentPid == Owner.processId && ChildPid == GetCurrentProcessId() && GetParent(Window) == Parent
            && GetClientRect(Parent, &Rect)
            && SetWindowPos(Window, nullptr, 0, 0, Rect.right, Rect.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
    }
}
