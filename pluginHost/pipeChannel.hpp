#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "commonLibs/plugins/pluginWire.hpp"
#include <QByteArray>
#include <functional>

namespace hc::plugins
{
    class WinHandle
    {
    public:
        explicit WinHandle(HANDLE Value = nullptr) : value_(Value) {}
        ~WinHandle() { Reset(); }
        WinHandle(const WinHandle&) = delete;
        WinHandle& operator=(const WinHandle&) = delete;
        WinHandle(WinHandle&& Other) noexcept : value_(std::exchange(Other.value_, nullptr)) {}
        WinHandle& operator=(WinHandle&& Other) noexcept { if (this != &Other) { Reset(); value_ = std::exchange(Other.value_, nullptr); } return *this; }
        HANDLE Get() const { return value_; }
        bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
        void Reset(HANDLE Value = nullptr) { if (Valid()) CloseHandle(value_); value_ = Value; }
    private:
        HANDLE value_;
    };
    struct WireFrame { WireHeader header_; QByteArray payload_; };
    storage::Error HostError(const char* Code, DWORD NativeError = 0);
    storage::Result<storage::Unit> WaitPipe(HANDLE Pipe, OVERLAPPED& Pending, HANDLE Peer, storage::OperationContext& Context, bool PumpMessages = false,
        const std::function<bool()>& AwaitingInteraction = {});
    storage::Result<storage::Unit> WriteFrame(HANDLE Pipe, HANDLE Peer, WireKind Kind, std::uint64_t Sequence, const QByteArray& Payload, storage::OperationContext Context, ProtocolVersion Version = {});
    storage::Result<WireFrame> ReadFrame(HANDLE Pipe, HANDLE Peer, storage::OperationContext Context, bool PumpMessages = false,
        const std::function<bool()>& AwaitingInteraction = {});
}
