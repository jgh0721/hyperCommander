#pragma once
#include "pipeChannel.hpp"
#include <QJsonObject>
#include <QAbstractNativeEventFilter>
#include "dopusViewerBackend.hpp"
#include "wlxViewerBackend.hpp"

namespace hc::plugins
{
    // A dedicated host owns one native viewer session. All SDK calls, window
    // messages and destruction run on the same thread, before DLL unload.
    class NativeViewerHostService final : public QAbstractNativeEventFilter
    {
    public:
        NativeViewerHostService(HMODULE Module, DWORD ParentProcess, const PluginManifest& Manifest);
        ~NativeViewerHostService();
        storage::Result<storage::Unit> Initialize(const QString& DefaultIniPath);
        storage::Result<QJsonObject> Probe();
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request);
        void PrepareWork(WireKind Kind);
        storage::Result<HANDLE> ArmWork(WireKind Kind);
        void FinishWork(WireKind Kind, bool Cancelled);
        bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr* Result) override;
    private:
        static LRESULT CALLBACK windowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
        static LRESULT CALLBACK inputHook(int Code, WPARAM WParam, LPARAM LParam);
        bool filterInput(MSG* Message);
        void close();
        HMODULE module_;
        PluginManifest manifest_;
        std::unique_ptr<DopusViewerBackend> dopus_;
        std::unique_ptr<WlxViewerBackend> wlx_;
        DWORD parentProcess_;
        DWORD thread_ = GetCurrentThreadId();
        DWORD inputThread_ = 0;
        HWND container_ = nullptr, viewer_ = nullptr;
        HHOOK inputHook_ = nullptr;
        UINT closeMessage_ = 0;
        UINT returnMessage_ = 0, interactionMessage_ = 0;
        unsigned returnKey_ = 0, returnModifiers_ = 0;
        bool preview_ = false, interacted_ = false;
        static thread_local NativeViewerHostService* inputOwner_;

    };
}
