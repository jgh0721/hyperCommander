#include "pipeChannel.hpp"

namespace hc::plugins
{
    using namespace storage;
    Error HostError(const char* Code, DWORD NativeError)
    { return {ErrorCategory::Unavailable, NativeError ? std::string(Code) + ":" + std::to_string(NativeError) : Code, {"plugin-host"}}; }
    Result<Unit> WaitPipe(HANDLE Pipe, OVERLAPPED& Pending, HANDLE Peer, OperationContext Context, bool PumpMessages)
    {
        const auto Cancel = [&] {
            CancelIoEx(Pipe, &Pending); DWORD Count = 0;
            // Keep the OVERLAPPED and its buffer alive through cancellation completion.
            GetOverlappedResult(Pipe, &Pending, &Count, TRUE);
        };
        for (;;)
        {
            if (const auto Stop = Context.Check({"plugin-host"})) { Cancel(); return *Stop; }
            HANDLE Handles[]{Pending.hEvent, Peer};
            const DWORD HandleCount = Peer ? 2 : 1;
            const auto Wait = PumpMessages ? MsgWaitForMultipleObjectsEx(HandleCount, Handles, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                : WaitForMultipleObjects(HandleCount, Handles, FALSE, 20);
            if (Wait == WAIT_OBJECT_0) return Unit{};
            if (Peer && Wait == WAIT_OBJECT_0 + 1) { Cancel(); return HostError("host-exited"); }
            if (PumpMessages && Wait == WAIT_OBJECT_0 + HandleCount)
            {
                // Bounded batches prevent a message flood from starving IPC and
                // deadline checks. A hung window procedure is reaped by the parent.
                MSG Message{};
                for (unsigned Count = 0; Count < 64 && PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE); ++Count)
                {
                    if (Message.message == WM_QUIT) { Cancel(); return HostError("host-window-quit"); }
                    TranslateMessage(&Message); DispatchMessageW(&Message);
                }
                continue;
            }
            if (Wait != WAIT_TIMEOUT) { const auto Error = GetLastError(); Cancel(); return HostError("host-pipe-wait", Error); }
        }
    }
    namespace
    {
        Result<Unit> Transfer(HANDLE Pipe, HANDLE Peer, void* Data, DWORD Length, bool Write, OperationContext Context, bool PumpMessages = false)
        {
            DWORD Offset = 0; WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!Event.Valid()) return HostError("host-pipe-event", GetLastError());
            while (Offset < Length)
            {
                if (const auto Stop = Context.Check({"plugin-host"})) return *Stop;
                OVERLAPPED Pending{}; Pending.hEvent = Event.Get(); ResetEvent(Event.Get()); DWORD Count = 0;
                const auto Done = Write ? WriteFile(Pipe, static_cast<char*>(Data) + Offset, Length - Offset, &Count, &Pending)
                    : ReadFile(Pipe, static_cast<char*>(Data) + Offset, Length - Offset, &Count, &Pending);
                if (!Done)
                {
                    const auto Error = GetLastError(); if (Error != ERROR_IO_PENDING) return HostError("host-pipe-io", Error);
                    auto Wait = WaitPipe(Pipe, Pending, Peer, Context, PumpMessages); if (!Wait.HasValue()) return Wait.Failure();
                }
                if (!GetOverlappedResult(Pipe, &Pending, &Count, FALSE)) return HostError("host-pipe-result", GetLastError());
                if (!Count || Count > Length - Offset) return HostError("host-pipe-closed");
                Offset += Count;
            }
            return Unit{};
        }
    }
    Result<Unit> WriteFrame(HANDLE Pipe, HANDLE Peer, WireKind Kind, std::uint64_t Sequence, const QByteArray& Payload, OperationContext Context)
    {
        if (Payload.size() > WirePayloadLimit) return HostError("host-wire-budget");
        auto Header = EncodeHeader({{1, 0}, Kind, Sequence, static_cast<std::uint32_t>(Payload.size())});
        if (!Header.HasValue()) return Header.Failure();
        auto Sent = Transfer(Pipe, Peer, Header.Value().data(), static_cast<DWORD>(Header.Value().size()), true, Context);
        if (!Sent.HasValue()) return Sent.Failure();
        return Transfer(Pipe, Peer, const_cast<char*>(Payload.constData()), static_cast<DWORD>(Payload.size()), true, Context);
    }
    Result<WireFrame> ReadFrame(HANDLE Pipe, HANDLE Peer, OperationContext Context, bool PumpMessages)
    {
        std::vector<std::byte> Bytes(WireHeaderBytes);
        auto Read = Transfer(Pipe, Peer, Bytes.data(), static_cast<DWORD>(Bytes.size()), false, Context, PumpMessages); if (!Read.HasValue()) return Read.Failure();
        auto Header = DecodeHeader(Bytes); if (!Header.HasValue()) return Header.Failure();
        QByteArray Payload(Header.Value().length_, Qt::Uninitialized);
        Read = Transfer(Pipe, Peer, Payload.data(), Header.Value().length_, false, Context, PumpMessages); if (!Read.HasValue()) return Read.Failure();
        return WireFrame{Header.Value(), std::move(Payload)};
    }
}
