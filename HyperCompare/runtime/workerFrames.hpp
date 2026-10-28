#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <cstdint>

namespace hc::compare
{
    // Resident worker framing: [u32 header bytes][compact JSON object][u32 body bytes][body].
    // Bodies carry source bytes as is, so no base64 or JSON string copies are made.
    inline constexpr std::uint32_t workerFrameHeaderLimit = 64 * 1024, workerFrameBodyLimit = 24 * 1024 * 1024;
    struct WorkerFrame { QJsonObject header_; QByteArray body_; };
    // Blocking I/O on Win32 pipe HANDLEs; each side reads on a dedicated thread.
    // False on EOF, I/O failure or a bound violation.
    bool WriteWorkerFrame(void* Pipe, const QJsonObject& Header, const QByteArray& Body = {});
    bool ReadWorkerFrame(void* Pipe, WorkerFrame& Frame);
}
