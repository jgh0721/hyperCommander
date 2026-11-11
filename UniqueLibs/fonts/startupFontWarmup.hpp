#pragma once

#include <memory>

namespace hc::fonts
{
    struct StartupFontWarmupState;

    // Construct first in main(), before QApplication. Destruction requests
    // cancellation without joining: an IPC-only secondary process must exit fast.
    class StartupFontWarmup final
    {
    public:
        StartupFontWarmup(int Argc, char** Argv) noexcept;
        ~StartupFontWarmup();
        StartupFontWarmup(const StartupFontWarmup&) = delete;
        StartupFontWarmup& operator=(const StartupFontWarmup&) = delete;
        // Call once after QApplication exists. Also handles a window already
        // shown by an application-subclass constructor.
        void WatchFirstWindow();
    private:
        std::shared_ptr<StartupFontWarmupState> state_;
    };
}
