#pragma once
#include "pipeChannel.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    // TC ABI translation only. The common host validates and owns HWNDs.
    class WlxViewerBackend final
    {
    public:
        explicit WlxViewerBackend(HMODULE Module);
        storage::Result<storage::Unit> Initialize(const QString& DefaultIniPath);
        storage::Result<QJsonObject> Probe();
        HWND Open(HWND Parent, const QString& File, int Flags);
        void Close(HWND Viewer);
        std::optional<LRESULT> Notify(HWND Viewer, UINT Message, WPARAM WParam, LPARAM LParam);
        storage::Result<QJsonObject> HandleCommand(HWND Viewer, const QJsonObject& Request);
    private:
        HMODULE module_;
        using Load = HWND(__stdcall*)(HWND, wchar_t*, int);
        using CloseWindow = void(__stdcall*)(HWND);
        using Command = int(__stdcall*)(HWND, int, int);
        using Notification = int(__stdcall*)(HWND, int, WPARAM, LPARAM);
        Load load_ = nullptr;
        CloseWindow close_ = nullptr;
        Command command_ = nullptr;
        Notification notification_ = nullptr;
        bool initialized_ = false;
        std::optional<QByteArray> detectString_;
    };
}
