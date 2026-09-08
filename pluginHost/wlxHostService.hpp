#pragma once
#include "pipeChannel.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    // A dedicated host owns one native viewer session. All SDK calls, window
    // messages and destruction run on the same thread, before DLL unload.
    class WlxHostService final
    {
    public:
        WlxHostService(HMODULE Module, DWORD ParentProcess);
        ~WlxHostService();
        storage::Result<storage::Unit> Initialize(const QString& DefaultIniPath);
        storage::Result<QJsonObject> Probe();
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request);
    private:
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
        static LRESULT CALLBACK inputHook(int Code, WPARAM WParam, LPARAM LParam);
        void close();
        HMODULE module_;
        DWORD parentProcess_;
        HWND container_ = nullptr, viewer_ = nullptr;
        HHOOK inputHook_ = nullptr;
        UINT closeMessage_ = 0;
        static thread_local WlxHostService* inputOwner_;
        using Load = HWND(__stdcall*)(HWND, wchar_t*, int);
        using Close = void(__stdcall*)(HWND);
        using Command = int(__stdcall*)(HWND, int, int);
        using Notification = int(__stdcall*)(HWND, int, WPARAM, LPARAM);
        Load load_ = nullptr;
        Close close_ = nullptr;
        Command command_ = nullptr;
        Notification notification_ = nullptr;
        bool initialized_ = false;
        std::optional<QByteArray> detectString_;
    };
}
