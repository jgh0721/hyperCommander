#pragma once
#include "pipeChannel.hpp"
#include "dopusAbi.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    // SDK calls and native resources stay on the host's window thread. The
    // common viewer host owns parenting, DPI, input routing and process grants.
    class DopusViewerBackend final
    {
    public:
        explicit DopusViewerBackend(HMODULE Module);
        ~DopusViewerBackend();
        storage::Result<storage::Unit> Initialize();
        storage::Result<QJsonObject> Probe();
        storage::Result<HWND> Open(HWND Parent, const QString& File, int Width, int Height, bool Preview);
        void Close();
        void Resize(int Width, int Height);
        storage::Result<QJsonObject> Data(WireKind Kind, const QJsonObject& Request);
    private:
        template<class Char> storage::Result<QJsonObject> identifyPlugin();
        template<class Char> storage::Result<QJsonObject> process(HWND Parent, const QString& File, int Width, int Height, bool View, bool Convert);
        template<class Char> storage::Result<QJsonObject> loadText(HWND Parent, Char* Name, IStream* Input, bool FromStream);
        storage::Result<HWND> showBitmap(HWND Parent, HBITMAP Bitmap, DWORD Flags, COLORREF Transparent);
        static LRESULT CALLBACK bitmapProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
        HMODULE module_;
        WinHandle abort_;
        bool preview_ = false;
        bool initialized_ = false, unicode_ = false, native_ = false, com_ = false;
        HWND window_ = nullptr;
        QJsonObject probe_;
        BITMAPINFO bitmapInfo_{};
        std::vector<DWORD> pixels_;
        // Some native viewers retain their stream until destruction.
        IStream* input_ = nullptr;
    };
}
