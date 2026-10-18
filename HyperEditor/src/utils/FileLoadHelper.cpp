#include "utils/FileLoadHelper.h"

#include <QByteArray>
#include <QFile>

#include <algorithm>
#include <limits>

namespace FileLoadHelper {

int scaledProgressValue(qint64 value, qint64 maximum, int scale)
{
    if (scale <= 0 || maximum <= 0)
        return 0;

    const qint64 clampedValue = std::clamp(value, qint64(0), maximum);
    const long double ratio = static_cast<long double>(clampedValue) / static_cast<long double>(maximum);
    const long double scaled = ratio * static_cast<long double>(scale);
    return std::clamp(static_cast<int>(scaled), 0, scale);
}

ReadResult readFileInChunks(QFile& file,
                            qint64 bytesToRead,
                            QByteArray* outData,
                            const ByteProgressCallback& progress,
                            const CancelCheckCallback& shouldCancel,
                            qint64 chunkSize)
{
    if (!outData || !file.isOpen() || bytesToRead < 0)
        return ReadResult::Failed;

    outData->clear();
    outData->reserve(static_cast<int>(std::min<qint64>(bytesToRead,
                                                        std::numeric_limits<int>::max())));

    if (bytesToRead == 0) {
        if (progress)
            progress(0, 0);
        return ReadResult::Success;
    }

    const qint64 effectiveChunkSize = std::max<qint64>(1, chunkSize);
    qint64 bytesRead = 0;
    while (bytesRead < bytesToRead) {
        if (shouldCancel && shouldCancel())
            return ReadResult::Cancelled;

        const qint64 nextChunkSize = std::min(bytesToRead - bytesRead, effectiveChunkSize);
        const QByteArray chunk = file.read(nextChunkSize);
        if (chunk.isEmpty() && nextChunkSize > 0)
            return ReadResult::Failed;

        outData->append(chunk);
        bytesRead += chunk.size();

        if (progress)
            progress(bytesRead, bytesToRead);

        if (shouldCancel && shouldCancel())
            return ReadResult::Cancelled;
    }

    return bytesRead == bytesToRead ? ReadResult::Success : ReadResult::Failed;
}

} // namespace FileLoadHelper
