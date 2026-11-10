#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <future>
#include <system_error>
#include <thread>

namespace hc::document
{
    /// Owns a parking window and the proxies of embedded helper processes, on a thread that only
    /// pumps messages.
    ///
    /// A helper's surface is a child of its proxy, so Windows joins the helper's input queue with
    /// the queue of the proxy's owner thread. Input is then handed out to both in arrival order,
    /// and a busy helper holds back the other thread's keys. A proxy owned by the GUI thread made
    /// every helper start (WebEngine initialisation) and every retired helper's teardown stall the
    /// editor's typing. A parked proxy joins only this idle thread; the GUI thread is joined only
    /// while the proxy is a child of a host widget.
    ///
    /// Proxies are created and destroyed here: a window can only be destroyed by its owner thread.
    /// Reparenting, showing, hiding and sizing them from the GUI thread are cross-thread calls this
    /// thread answers at once.
    class DocumentParkingThread final
    {
    public:
        DocumentParkingThread() = default;
        DocumentParkingThread(const DocumentParkingThread&) = delete;
        DocumentParkingThread& operator=(const DocumentParkingThread&) = delete;
        ~DocumentParkingThread() { Stop(); }

        /// The parking window, or nullptr when the thread or its window could not be created.
        HWND Start()
        {
            if(thread_.joinable()) return window_;
            // Attached proxies become children of host widgets: keep the GUI thread's DPI context.
            const auto Context = GetThreadDpiAwarenessContext();
            std::promise<HWND> Created; auto Window = Created.get_future();
            try {
                thread_ = std::thread([Context, Created = std::move(Created)]() mutable {
                    SetThreadDpiAwarenessContext(Context);
                    const auto* Class = windowClass();
                    const auto Parking = Class ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, Class, L"", WS_POPUP,
                        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
                    Created.set_value(Parking);
                    if(!Parking) return;
                    MSG Message{};
                    while(GetMessageW(&Message, nullptr, 0, 0) > 0) DispatchMessageW(&Message);
                });
            } catch(const std::system_error&) { return nullptr; }
            window_ = Window.get();
            if(!window_) thread_.join();
            else threadId_ = GetThreadId(thread_.native_handle());
            return window_;
        }
        void Stop()
        {
            if(!thread_.joinable()) return;
            if(!window_ || !SendMessageTimeoutW(window_, QuitMessage, 0, 0, SMTO_NORMAL, 5000, nullptr))
                PostThreadMessageW(GetThreadId(thread_.native_handle()), WM_QUIT, 0, 0);
            thread_.join(); window_ = nullptr; threadId_ = 0;
        }
        /// A hidden child of the parking window carrying Property = Value, or nullptr.
        HWND CreateProxy(int Width, int Height, const wchar_t* Property, HANDLE Value) const
        {
            ProxyRequest Request{Width, Height, Property, Value};
            DWORD_PTR Result = 0;
            if(!window_ || !SendMessageTimeoutW(window_, CreateProxyMessage, 0, reinterpret_cast<LPARAM>(&Request), SMTO_NORMAL, 5000, &Result))
                return nullptr;
            return reinterpret_cast<HWND>(Result);
        }
        /// Removes Property and destroys the proxy, when it still belongs to this thread.
        void DestroyProxy(HWND Proxy, const wchar_t* Property) const
        {
            if(window_) SendMessageTimeoutW(window_, DestroyProxyMessage, reinterpret_cast<WPARAM>(Proxy), reinterpret_cast<LPARAM>(Property), SMTO_NORMAL, 5000, nullptr);
        }
        /// True when Window belongs to this thread.
        bool Owns(HWND Window) const
        {
            return threadId_ && Window && IsWindow(Window) && GetWindowThreadProcessId(Window, nullptr) == threadId_;
        }

    private:
        static constexpr UINT CreateProxyMessage = WM_APP + 0x41, DestroyProxyMessage = WM_APP + 0x42, QuitMessage = WM_APP + 0x43;
        struct ProxyRequest { int width_ = 0, height_ = 0; const wchar_t* property_ = nullptr; HANDLE value_ = nullptr; };
        static const wchar_t* windowClass()
        {
            static constexpr const wchar_t* Name = L"HyperCommander.DocumentParking";
            static const bool Registered = [] {
                WNDCLASSW Class{}; Class.lpfnWndProc = windowProc; Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = Name;
                return RegisterClassW(&Class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
            }();
            return Registered ? Name : nullptr;
        }
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
        {
            switch(Message) {
            case CreateProxyMessage: {
                const auto& Request = *reinterpret_cast<const ProxyRequest*>(LParam);
                const auto Proxy = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                    0, 0, Request.width_, Request.height_, Window, nullptr, GetModuleHandleW(nullptr), nullptr);
                if(Proxy && !SetPropW(Proxy, Request.property_, Request.value_)) { DestroyWindow(Proxy); return 0; }
                return reinterpret_cast<LRESULT>(Proxy);
            }
            case DestroyProxyMessage: {
                const auto Proxy = reinterpret_cast<HWND>(WParam);
                // A recycled handle that now belongs to another thread is left alone.
                if(IsWindow(Proxy) && GetWindowThreadProcessId(Proxy, nullptr) == GetCurrentThreadId()) {
                    RemovePropW(Proxy, reinterpret_cast<const wchar_t*>(LParam)); DestroyWindow(Proxy);
                }
                return 0;
            }
            case QuitMessage: DestroyWindow(Window); return 0;
            case WM_CLOSE: return 0; // Only QuitMessage ends the parking window.
            case WM_DESTROY: PostQuitMessage(0); return 0;
            }
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
        std::thread thread_;
        HWND window_ = nullptr;
        DWORD threadId_ = 0;
    };
}
