#include "nativeViewerHostService.hpp"
#include <QFileInfo>
#include <commctrl.h>
#include "wlxWindowMessage.hpp"

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        constexpr auto WindowClass = L"HyperCommander.PluginViewerSurface.1";
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"plugin-viewer"}}; }
        bool Dimension(const QJsonValue& Value)
        { const auto Number = Value.toInt(-1); return Value.isDouble() && Value.toDouble() == Number && Number >= 0 && Number <= 32768; }

    }
    NativeViewerHostService::NativeViewerHostService(HMODULE Module, DWORD ParentProcess, const PluginManifest& Manifest) : manifest_(Manifest), parentProcess_(ParentProcess)
    {
        if (Manifest.format_ == PluginFormat::DopusViewer) dopus_ = std::make_unique<DopusViewerBackend>(Module);
        else wlx_ = std::make_unique<WlxViewerBackend>(Module);
    }
    thread_local NativeViewerHostService* NativeViewerHostService::inputOwner_ = nullptr;
    NativeViewerHostService::~NativeViewerHostService() { close(); }
    Result<Unit> NativeViewerHostService::Initialize(const QString& DefaultIniPath)
    { return dopus_ ? dopus_->Initialize() : wlx_->Initialize(DefaultIniPath); }
    Result<QJsonObject> NativeViewerHostService::Probe()
    { return dopus_ ? dopus_->Probe() : wlx_->Probe(); }
    void NativeViewerHostService::close()
    {
        if (inputHook_) UnhookWindowsHookEx(std::exchange(inputHook_, nullptr));
        if (inputOwner_ == this) inputOwner_ = nullptr;
        const auto Viewer = std::exchange(viewer_, nullptr);
        if (dopus_) dopus_->Close();
        else if (Viewer && IsWindow(Viewer)) wlx_->Close(Viewer);
        if (container_) DestroyWindow(std::exchange(container_, nullptr));
    }
    LRESULT CALLBACK NativeViewerHostService::inputHook(int Code, WPARAM WParam, LPARAM LParam)
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
    LRESULT CALLBACK NativeViewerHostService::windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* Self = reinterpret_cast<NativeViewerHostService*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE)
        {
            Self = static_cast<NativeViewerHostService*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        // Plugins can request the background while constructing their window,
        // before Open has returned the child HWND.
        if (Self && Self->dopus_ && Message == WM_NOTIFY && LParam
            && reinterpret_cast<NMHDR*>(LParam)->code == UINT(0 - 2000)) return RGB(32, 32, 32);
        if (Self && Self->viewer_)
        {
            if (Message == WM_SIZE) { if (Self->dopus_) Self->dopus_->Resize(LOWORD(LParam), HIWORD(LParam)); else MoveWindow(Self->viewer_, 0, 0, LOWORD(LParam), HIWORD(LParam), TRUE); return 0; }
            if (Message == WM_SETFOCUS) { SetFocus(Self->viewer_); return 0; }
            if ((Message == WM_COMMAND || Message == WM_NOTIFY) && Self->wlx_)
                if (const auto Result = Self->wlx_->Notify(Self->viewer_, Message, WParam, LParam)) return *Result;
        }
        // The parent owns the session through IPC; the plugin cannot close the
        // application's surface by sending WM_CLOSE to its lister parent.
        if (Message == WM_CLOSE) return 0;
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
    Result<QJsonObject> NativeViewerHostService::Handle(WireKind Kind, const QJsonObject& Request)
    {
        if (Kind == WireKind::MetadataQuery || Kind == WireKind::ContentConvert)
        {
            const auto Required = Kind == WireKind::MetadataQuery ? Extension::Metadata : Extension::ContentConvert;
            if (!dopus_ || !manifest_.extensions_.contains(Required)) return Error{ErrorCategory::AccessDenied, "host-viewer-data-denied", {"plugin"}};
            return dopus_->Data(Kind, Request);
        }
        if (!manifest_.extensions_.contains(Extension::Viewer)) return Error{ErrorCategory::AccessDenied, "host-viewer-denied", {"plugin"}};
        if (Kind == WireKind::ViewerOpen)
        {
            if (container_) return Invalid("host-viewer-already-open");
            if ((Request.size() != 5 && Request.size() != 6) || (Request.size() == 6 && !Request["preview"].isBool())
                || !Request["parent"].isString() || !Request["file"].isString()
                || !Dimension(Request["width"]) || !Dimension(Request["height"])) return Invalid("host-viewer-open-fields");
            const auto Flags = Request["flags"].toInt(-1);
            if (!Request["flags"].isDouble() || Flags < 0 || Flags > 511 || Request["flags"].toDouble() != Flags) return Invalid("host-viewer-flags");
            // Width and height are physical pixels at this Win32 boundary.
            // The Qt presentation adapter performs the logical/DPR conversion.
            const auto File = Request["file"].toString();
            if (File.isEmpty() || File.size() > 32767 || File.contains(QChar(0)) || !QFileInfo(File).isAbsolute()) return Invalid("host-viewer-file");
            if (!QFileInfo(File).isFile()) return Error{ErrorCategory::NotFound, "host-viewer-file-missing", {"plugin-viewer"}};
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
            if (dopus_)
            {
                auto Opened = dopus_->Open(container_, File, Request["width"].toInt(), Request["height"].toInt(), Request["preview"].toBool());
                if (!Opened.HasValue()) { close(); return Opened.Failure(); } viewer_ = Opened.Value();
            }
            else viewer_ = wlx_->Open(container_, File, Flags);
            DWORD ViewerProcess = 0;
            if (!viewer_ || !GetWindowThreadProcessId(viewer_, &ViewerProcess) || ViewerProcess != GetCurrentProcessId() || !IsChild(container_, viewer_))
            {
                // Never pass an unvalidated, foreign HWND to plugin cleanup.
                viewer_ = nullptr; close(); return Error{ErrorCategory::Unsupported, "host-viewer-load-rejected", {"plugin-viewer"}};
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
            if (dopus_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"dopus-viewer"}};
            return wlx_->HandleCommand(viewer_, Request);
        }
        return Invalid("host-viewer-command");
    }
}
