#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QAbstractEventDispatcher>
#include <QTimer>
#include <atomic>
#include <cstdint>

namespace hc::plugins
{
    // Only the caller of Cycle may take command work. Neither the native
    // window, timers, hooks nor Qt callbacks execute work, including in a
    // foreign nested message loop entered from a toolbar notification.
    class ViewerUiDispatcher final
    {
    public:
        struct ProbeResult { bool acknowledged_ = false; DWORD error_ = 0; };
        class NativeCallback final
        {
        public:
            NativeCallback();
            ~NativeCallback();
        private:
            ViewerUiDispatcher* owner_;
        };
        class Request final
        {
        public:
            explicit Request(ViewerUiDispatcher& Owner);
            ~Request();
        private:
            ViewerUiDispatcher& owner_;
        };
        ViewerUiDispatcher();
        ~ViewerUiDispatcher();
        bool Initialize();
        void Cycle(bool Wait);
        void Wake();
        bool AtBoundary() const;
        bool QuitRequested() const { return quit_.load(); }
        std::uint64_t BoundarySerial() const { return boundarySerial_.load(); }
        ProbeResult Probe(std::uint64_t Serial) const;
        static void SetInputAttached(bool Attached);
        bool InputAttached() const { return inputAttached_.load(); }
    private:
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
        static LRESULT CALLBACK messageHook(int Code, WPARAM WParam, LPARAM LParam);
        static thread_local ViewerUiDispatcher* current_;
        QAbstractEventDispatcher* dispatcher_ = nullptr;
        HWND window_ = nullptr;
        HHOOK hook_ = nullptr;
        QTimer pulse_;
        DWORD thread_ = 0;
        std::uint32_t generation_ = 0;
        unsigned requestDepth_ = 0, callbackDepth_ = 0, messages_ = 0;
        bool processing_ = false, interrupted_ = false;
        std::atomic<bool> quit_{false};
        std::atomic<bool> inputAttached_{false};
        std::atomic<std::uint64_t> boundarySerial_{0}, acknowledged_{0};
    };
}
