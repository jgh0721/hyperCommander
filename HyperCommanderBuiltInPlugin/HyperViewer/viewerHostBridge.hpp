#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <memory>
#include <string>
#include "commonLibs/document/documentHostProtocol.hpp"

namespace hc::document
{
    class ViewerHostBridge final
    {
    public:
        explicit ViewerHostBridge(HWND Container);
        ~ViewerHostBridge();
        void Start();
        void Stop();
        bool Load(const wchar_t* Path);
        void Focus();
        void SetAbort(HANDLE Event);
        bool Exchange(const char* Input, unsigned Size, std::string& Output);
        HWND Child() const;
        bool Failed() const;
        std::string Diagnostic() const;
    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
