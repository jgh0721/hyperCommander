#include "stdafx.h"
#include "viewerUiDispatcher.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QThread>
#include <QScopeGuard>

namespace hc::plugins
{
    namespace { constexpr UINT ProbeMessage = WM_APP + 0x61; }
    thread_local ViewerUiDispatcher* ViewerUiDispatcher::current_ = nullptr;
    ViewerUiDispatcher::NativeCallback::NativeCallback() : owner_(current_)
    { if (owner_) ++owner_->callbackDepth_; }
    ViewerUiDispatcher::NativeCallback::~NativeCallback()
    { if (owner_) --owner_->callbackDepth_; }
    ViewerUiDispatcher::Request::Request(ViewerUiDispatcher& Owner) : owner_(Owner)
    { Q_ASSERT(Owner.AtBoundary()); ++owner_.requestDepth_; }
    ViewerUiDispatcher::Request::~Request() { --owner_.requestDepth_; }
    ViewerUiDispatcher::ViewerUiDispatcher() : thread_(GetCurrentThreadId()) {}
    ViewerUiDispatcher::~ViewerUiDispatcher()
    {
        Q_ASSERT(GetCurrentThreadId() == thread_);
        pulse_.stop();
        if (hook_) UnhookWindowsHookEx(hook_);
        if (window_) DestroyWindow(window_);
        if (current_ == this) current_ = nullptr;
    }
    bool ViewerUiDispatcher::Initialize()
    {
        Q_ASSERT(!current_ && GetCurrentThreadId() == thread_);
        dispatcher_ = QAbstractEventDispatcher::instance();
        if (!dispatcher_) return false;
        WNDCLASSW Class{}; Class.lpfnWndProc = windowProc;
        Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = L"HyperCommander.ViewerUiProbe.1";
        if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        // This identity stays fixed until both I/O threads have stopped.
        generation_ = GetTickCount() ^ thread_ ^ GetCurrentProcessId();
        if (!generation_) generation_ = 1;
        window_ = CreateWindowExW(0, Class.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, Class.hInstance, this);
        if (!window_) return false;
        current_ = this;
        // Qt consumes WM_QUIT before native event filters and QApplication::quit
        // is ineffective without exec(). WH_GETMESSAGE observes it without
        // altering the plugin's message or depending on a private Qt dispatcher.
        hook_ = SetWindowsHookExW(WH_GETMESSAGE, messageHook, nullptr, thread_);
        if (!hook_) return false;
        QObject::connect(&pulse_, &QTimer::timeout, &pulse_, [] {});
        pulse_.start(50); // A lost/coalesced wake changes latency, never correctness.
        return true;
    }
    bool ViewerUiDispatcher::AtBoundary() const
    {
        return GetCurrentThreadId() == thread_ && !processing_ && !requestDepth_ && !callbackDepth_
            && QThread::currentThread()->loopLevel() == 0;
    }
    void ViewerUiDispatcher::Cycle(bool Wait)
    {
        Q_ASSERT(AtBoundary());
        messages_ = 0; interrupted_ = false; processing_ = true;
        dispatcher_->processEvents(Wait ? QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents : QEventLoop::AllEvents);
        processing_ = false;
        ++boundarySerial_;
        if (!quit_ && AtBoundary()) QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void ViewerUiDispatcher::Wake() { dispatcher_->wakeUp(); }
    void ViewerUiDispatcher::SetInputAttached(bool Attached)
    { if (current_) current_->inputAttached_ = Attached; }
    LRESULT CALLBACK ViewerUiDispatcher::messageHook(int Code, WPARAM WParam, LPARAM LParam)
    {
        auto* Self = current_;
        if (Self && Code >= 0 && WParam == PM_REMOVE)
        {
            const auto* Message = reinterpret_cast<const MSG*>(LParam);
            if (Message->message == WM_QUIT) Self->quit_ = true;
            // interrupt() wakes the dispatcher. Repeating it for its own wake
            // messages can fill a modal loop's queue. Interrupt only once per
            // outer cycle, and leave a nested Qt exec() to its own dispatcher.
            // No hook/timer drains command work in either kind of modal loop.
            if (Self->processing_ && !Self->interrupted_ && QThread::currentThread()->loopLevel() == 0 && ++Self->messages_ >= 64) {
                Self->interrupted_ = true; Self->dispatcher_->interrupt();
            }
        }
        return CallNextHookEx(nullptr, Code, WParam, LParam);
    }
    LRESULT CALLBACK ViewerUiDispatcher::windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* Self = reinterpret_cast<ViewerUiDispatcher*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            Self = static_cast<ViewerUiDispatcher*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        if (Self && Message == ProbeMessage && WParam == Self->generation_) {
            Self->acknowledged_.store(std::uint64_t(LParam)); return 1;
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
    ViewerUiDispatcher::ProbeResult ViewerUiDispatcher::Probe(std::uint64_t Serial) const
    {
        Q_ASSERT(GetCurrentThreadId() != thread_);
        DWORD_PTR Reply = 0;
        SetLastError(ERROR_SUCCESS);
        const auto Ok = SendMessageTimeoutW(window_, ProbeMessage, generation_, LPARAM(Serial),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 100, &Reply);
        const auto Error = Ok ? ERROR_SUCCESS : GetLastError();
        return {Ok && Reply == 1 && acknowledged_.load() == Serial, Error};
    }
}
