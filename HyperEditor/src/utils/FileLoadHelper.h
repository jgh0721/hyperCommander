#pragma once

#include <QString>

#include <functional>

class QByteArray;
class QFile;

namespace FileLoadHelper {

using ByteProgressCallback = std::function<void(qint64 bytesRead, qint64 totalBytes)>;
using CancelCheckCallback = std::function<bool()>;

enum class ReadResult {
    Success,
    Cancelled,
    Failed
};

int scaledProgressValue(qint64 value, qint64 maximum, int scale = 1000);
ReadResult readFileInChunks(QFile& file,
                            qint64 bytesToRead,
                            QByteArray* outData,
                            const ByteProgressCallback& progress = {},
                            const CancelCheckCallback& shouldCancel = {},
                            qint64 chunkSize = 512LL * 1024LL);

} // namespace FileLoadHelper
