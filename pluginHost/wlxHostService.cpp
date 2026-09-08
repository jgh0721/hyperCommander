#include "wlxHostService.hpp"
#include <QFileInfo>
#include "wlxWindowMessage.hpp"

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        constexpr auto WindowClass = L"HyperCommander.WlxSurface.1";
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
        bool Dimension(const QJsonValue& Value)
        { const auto Number = Value.toInt(-1); return Value.isDouble() && Value.toDouble() == Number && Number >= 0 && Number <= 32768; }
    }
    WlxHostService::WlxHostService(HMODULE Module, DWORD ParentProcess) : module_(Module), parentProcess_(ParentProcess)
    {
        load_ = reinterpret_cast<Load>(GetProcAddress(module_, "ListLoadW"));
        close_ = reinterpret_cast<Close>(GetProcAddress(module_, "ListCloseWindow"));
        command_ = reinterpret_cast<Command>(GetProcAddress(module_, "ListSendCommand"));
        notification_ = reinterpret_cast<Notification>(GetProcAddress(module_, "ListNotificationReceived"));
    }
    thread_local WlxHostService* WlxHostService::inputOwner_ = nullptr;
    WlxHostService::~WlxHostService() { close(); }
    Result<QJsonObject> WlxHostService::Probe() const
    {
        if (!load_) return Error{ErrorCategory::Unsupported, "host-wlx-unicode-exports", {"wlx"}};
        return QJsonObject{{"format", "wlx"}, {"unicode", true}, {"commands", command_ != nullptr}, {"processId", qint64(GetCurrentProcessId())}};
    }
    void WlxHostService::close()
    {
        if (inputHook_) UnhookWindowsHookEx(std::exchange(inputHook_, nullptr));
        if (inputOwner_ == this) inputOwner_ = nullptr;
        const auto Viewer = std::exchange(viewer_, nullptr);
        if (Viewer && IsWindow(Viewer)) { if (close_) close_(Viewer); else DestroyWindow(Viewer); }
        if (container_) DestroyWindow(std::exchange(container_, nullptr));
    }
    LRESULT CALLBACK WlxHostService::inputHook(int Code, WPARAM WParam, LPARAM LParam)
    {
        auto* Self = inputOwner_;
        if (Code == HC_ACTION && WParam == PM_REMOVE && Self && Self->viewer_)
        {
            auto* Message = reinterpret_cast<MSG*>(LParam);
            if (Message->message == WM_KEYDOWN && (Message->hwnd == Self->viewer_ || IsChild(Self->viewer_, Message->hwnd)))
            {
                const bool Control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool Modified = (GetKeyState(VK_MENU) & 0x8000) != 0 || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool Close = !Modified && ((!Control && Message->wParam == VK_ESCAPE) || (Control && Message->wParam == 'Q'));
                if (Close)
                {
                    // Post to the one granted native parent. Queuing avoids a
                    // synchronous call into the GUI while it retires this host.
                    const bool Repeated = (Message->lParam & (1LL << 30)) != 0;
                    if (!Repeated) PostMessageW(GetParent(Self->container_), Self->closeMessage_, reinterpret_cast<WPARAM>(Self->viewer_), 0);
                    Message->message = WM_NULL;
                }
            }
        }
        return CallNextHookEx(nullptr, Code, WParam, LParam);
    }
    LRESULT CALLBACK WlxHostService::windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* Self = reinterpret_cast<WlxHostService*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE)
        {
            Self = static_cast<WlxHostService*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        if (Self && Self->viewer_)
        {
            if (Message == WM_SIZE) { MoveWindow(Self->viewer_, 0, 0, LOWORD(LParam), HIWORD(LParam), TRUE); return 0; }
            if (Message == WM_SETFOCUS) { SetFocus(Self->viewer_); return 0; }
            if ((Message == WM_COMMAND || Message == WM_NOTIFY) && Self->notification_)
                return Self->notification_(Self->viewer_, Message, WParam, LParam);
        }
        // The parent owns the session through IPC; the plugin cannot close the
        // application's surface by sending WM_CLOSE to its lister parent.
        if (Message == WM_CLOSE) return 0;
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
    Result<QJsonObject> WlxHostService::Handle(WireKind Kind, const QJsonObject& Request)
    {
        if (Kind == WireKind::ViewerOpen)
        {
            if (container_) return Invalid("host-viewer-already-open");
            if (!load_) return Probe();
            if (Request.size() != 5 || !Request["parent"].isString() || !Request["file"].isString()
                || !Dimension(Request["width"]) || !Dimension(Request["height"])) return Invalid("host-viewer-open-fields");
            const auto Flags = Request["flags"].toInt(-1);
            if (!Request["flags"].isDouble() || Flags < 0 || Flags > 511 || Request["flags"].toDouble() != Flags) return Invalid("host-viewer-flags");
            // Width and height are physical pixels at this Win32 boundary.
            // The Qt presentation adapter performs the logical/DPR conversion.
            const auto File = Request["file"].toString();
            if (File.isEmpty() || File.size() > 32767 || File.contains(QChar(0)) || !QFileInfo(File).isAbsolute()) return Invalid("host-viewer-file");
            if (!QFileInfo(File).isFile()) return Error{ErrorCategory::NotFound, "host-viewer-file-missing", {"wlx"}};
            bool Parsed = false; const auto ParentValue = Request["parent"].toString().toULongLong(&Parsed, 16);
            const auto Parent = reinterpret_cast<HWND>(static_cast<quintptr>(ParentValue)); DWORD Process = 0;
            if (!Parsed || !Parent || !GetWindowThreadProcessId(Parent, &Process) || Process != parentProcess_) return Invalid("host-viewer-parent");
            const auto Dpi = GetWindowDpiAwarenessContext(Parent);
            if (!Dpi || !SetThreadDpiAwarenessContext(Dpi)) return HostError("host-viewer-dpi", GetLastError());
            WNDCLASSW Class{}; Class.lpfnWndProc = windowProc; Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = WindowClass;
            if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HostError("host-viewer-window-class", GetLastError());
            container_ = CreateWindowExW(0, WindowClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                0, 0, Request["width"].toInt(), Request["height"].toInt(), Parent, nullptr, Class.hInstance, this);
            if (!container_) return HostError("host-viewer-container", GetLastError());
            auto NativePath = File.toStdWString(); viewer_ = load_(container_, NativePath.data(), Flags);
            DWORD ViewerProcess = 0;
            if (!viewer_ || !GetWindowThreadProcessId(viewer_, &ViewerProcess) || ViewerProcess != GetCurrentProcessId() || !IsChild(container_, viewer_))
            {
                // Never pass an unvalidated, foreign HWND to plugin cleanup.
                viewer_ = nullptr; close(); return Error{ErrorCategory::Unsupported, "host-viewer-load-rejected", {"wlx"}};
            }
            MoveWindow(viewer_, 0, 0, Request["width"].toInt(), Request["height"].toInt(), TRUE);
            closeMessage_ = RegisterWindowMessageW(WlxCloseMessage); inputOwner_ = this;
            inputHook_ = SetWindowsHookExW(WH_GETMESSAGE, inputHook, nullptr, GetCurrentThreadId());
            if (!closeMessage_ || !inputHook_) { const auto Error = GetLastError(); close(); return HostError("host-viewer-input-hook", Error); }
            ShowWindow(viewer_, SW_SHOW); ShowWindow(container_, SW_SHOW);
            return QJsonObject{{"window", QString::number(reinterpret_cast<quintptr>(container_), 16)},
                {"viewer", QString::number(reinterpret_cast<quintptr>(viewer_), 16)}, {"processId", qint64(GetCurrentProcessId())}};
        }
        if (Kind == WireKind::ViewerClose)
        { if (!Request.isEmpty()) return Invalid("host-viewer-close-fields"); close(); return QJsonObject{{"closed", true}}; }
        if (!container_ || !viewer_ || !IsWindow(container_) || !IsWindow(viewer_)) return HostError("host-viewer-not-open");
        if (Kind == WireKind::ViewerResize)
        {
            if (Request.size() != 2 || !Dimension(Request["width"]) || !Dimension(Request["height"])) return Invalid("host-viewer-size");
            if (!MoveWindow(container_, 0, 0, Request["width"].toInt(), Request["height"].toInt(), TRUE)) return HostError("host-viewer-resize", GetLastError());
            return QJsonObject{{"resized", true}};
        }
        if (Kind == WireKind::ViewerFocus)
        { if (!Request.isEmpty()) return Invalid("host-viewer-focus-fields"); SetFocus(viewer_); return QJsonObject{{"focused", GetFocus() == viewer_ || IsChild(viewer_, GetFocus())}}; }
        if (Kind == WireKind::ViewerCommand)
        {
            const auto Command = Request["command"].toInt(-1), Parameter = Request["parameter"].toInt(-1);
            if (Request.size() != 2 || !Request["command"].isDouble() || !Request["parameter"].isDouble()
                || Request["command"].toDouble() != Command || Request["parameter"].toDouble() != Parameter
                || Command < 1 || Command > 4 || Parameter < 0 || Parameter > (Command == 4 ? 100 : 511)) return Invalid("host-viewer-command-fields");
            if (!command_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"wlx"}};
            return QJsonObject{{"result", command_(viewer_, Command, Parameter)}};
        }
        return Invalid("host-viewer-command");
    }
}
