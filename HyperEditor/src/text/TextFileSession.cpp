#include "text/TextFileSession.h"

#include "utils/EncodingDetector.h"
#include "utils/FileLoadHelper.h"

#include <QFile>

bool TextFileSession::open(const QString& filePath,
                           OpenMode mode,
                           const ProgressCallback& progress,
                           const CancelCallback& cancel,
                           bool* wasCanceled)
{
    clear();

    if (wasCanceled)
        *wasCanceled = false;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    m_filePath = filePath;
    m_fileSize = file.size();
    m_openMode = mode;

    const qint64 sampleBytes = qMin(m_fileSize, DetectionSampleLimit);
    m_detectionSample = file.peek(sampleBytes);
    m_detectedEncoding = EncodingDetector::detectFromSample(m_detectionSample);
    m_hasBom = EncodingDetector::hasBom(m_detectionSample, m_detectedEncoding);

    const qint64 bytesToRead = (mode == OpenMode::Full)
        ? m_fileSize
        : qMin(m_fileSize, PreviewByteLimit);

    const FileLoadHelper::ReadResult readResult = FileLoadHelper::readFileInChunks(file,
                                                                                   bytesToRead,
                                                                                   &m_rawData,
                                                                                   progress,
                                                                                   cancel);
    if (readResult != FileLoadHelper::ReadResult::Success) {
        if (readResult == FileLoadHelper::ReadResult::Cancelled && wasCanceled)
            *wasCanceled = true;
        clear();
        return false;
    }

    m_truncated = bytesToRead < m_fileSize;
    return true;
}

QString TextFileSession::decodeWithEncoding(const QString& encoding) const
{
    return EncodingDetector::decode(m_rawData, encoding);
}

bool TextFileSession::saveText(const QString& filePath,
                               const QString& text,
                               const QString& encoding,
                               ScintillaDocument::LineEnding lineEnding,
                               bool includeBom,
                               const ProgressCallback& progress)
{
    Q_UNUSED(filePath); Q_UNUSED(text); Q_UNUSED(encoding); Q_UNUSED(lineEnding); Q_UNUSED(includeBom); Q_UNUSED(progress); return false;
}

void TextFileSession::clear()
{
    m_filePath.clear();
    m_rawData.clear();
    m_detectionSample.clear();
    m_detectedEncoding.clear();
    m_fileSize = 0;
    m_hasBom = false;
    m_truncated = false;
    m_openMode = OpenMode::Full;
}

bool TextFileSession::shouldPromptForLargeFile(qint64 fileSize)
{
    return fileSize >= LargeFilePromptThresholdBytes;
}

QString TextFileSession::normalizeLineEndings(QString text, ScintillaDocument::LineEnding lineEnding)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    switch (lineEnding) {
        case ScintillaDocument::CRLF:
            text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
            break;
        case ScintillaDocument::CR:
            text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
            break;
        case ScintillaDocument::LF:
        default:
            break;
    }

    return text;
}

