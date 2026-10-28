#include "stdafx.h"
#include "workerFrames.hpp"
#include <QJsonDocument>
#include <QtEndian>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        bool writeAll(HANDLE Pipe, const char* Data, qsizetype Bytes)
        {
            while (Bytes > 0)
            {
                DWORD written = 0;
                if (!WriteFile(Pipe, Data, DWORD(std::min<qsizetype>(Bytes, 1 << 20)), &written, nullptr) || !written) return false;
                Data += written; Bytes -= written;
            }
            return true;
        }
        bool readAll(HANDLE Pipe, char* Data, qsizetype Bytes)
        {
            while (Bytes > 0)
            {
                DWORD read = 0;
                if (!ReadFile(Pipe, Data, DWORD(std::min<qsizetype>(Bytes, 1 << 20)), &read, nullptr) || !read) return false;
                Data += read; Bytes -= read;
            }
            return true;
        }
        bool readLength(HANDLE Pipe, std::uint32_t Limit, std::uint32_t& Length)
        {
            char bytes[4];
            if (!readAll(Pipe, bytes, 4)) return false;
            Length = qFromLittleEndian<std::uint32_t>(bytes); return Length <= Limit;
        }
    }
    bool WriteWorkerFrame(void* Pipe, const QJsonObject& Header, const QByteArray& Body)
    {
        const auto header = QJsonDocument(Header).toJson(QJsonDocument::Compact);
        if (header.size() > qsizetype(workerFrameHeaderLimit) || Body.size() > qsizetype(workerFrameBodyLimit)) return false;
        char lengths[8]; qToLittleEndian<std::uint32_t>(std::uint32_t(header.size()), lengths); qToLittleEndian<std::uint32_t>(std::uint32_t(Body.size()), lengths + 4);
        return writeAll(Pipe, lengths, 4) && writeAll(Pipe, header.constData(), header.size()) && writeAll(Pipe, lengths + 4, 4) && writeAll(Pipe, Body.constData(), Body.size());
    }
    bool ReadWorkerFrame(void* Pipe, WorkerFrame& Frame)
    {
        std::uint32_t length = 0;
        if (!readLength(Pipe, workerFrameHeaderLimit, length)) return false;
        QByteArray header(qsizetype(length), Qt::Uninitialized);
        if (!readAll(Pipe, header.data(), header.size())) return false;
        QJsonParseError error; const auto document = QJsonDocument::fromJson(header, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        if (!readLength(Pipe, workerFrameBodyLimit, length)) return false;
        Frame.header_ = document.object(); Frame.body_ = QByteArray(qsizetype(length), Qt::Uninitialized);
        return readAll(Pipe, Frame.body_.data(), Frame.body_.size());
    }
}
