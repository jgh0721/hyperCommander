#pragma once

#include "text/ScintillaDocument.h"

#include <QByteArray>
#include <QString>

#include <functional>

class TextFileSession
{
public:
    using ProgressCallback = std::function<void(qint64 bytesRead, qint64 totalBytes)>;
    using CancelCallback = std::function<bool()>;

    enum class OpenMode {
        Full,
        LimitedPreview
    };

    static constexpr qint64 LargeFilePromptThresholdBytes = 8LL * 1024LL * 1024LL;
    static constexpr qint64 PreviewByteLimit = 2LL * 1024LL * 1024LL;
    static constexpr qint64 DetectionSampleLimit = 256LL * 1024LL;

    bool open(const QString& filePath,
              OpenMode mode,
              const ProgressCallback& progress = {},
              const CancelCallback& cancel = {},
              bool* wasCanceled = nullptr);
    QString decodeWithEncoding(const QString& encoding) const;
    bool saveText(const QString& filePath,
                  const QString& text,
                  const QString& encoding,
                  ScintillaDocument::LineEnding lineEnding,
                  bool includeBom,
                  const ProgressCallback& progress = {});
    void clear();

    static bool shouldPromptForLargeFile(qint64 fileSize);

    QString filePath() const { return m_filePath; }
    qint64 fileSize() const { return m_fileSize; }
    bool isTruncated() const { return m_truncated; }
    bool hasFullContents() const { return !m_truncated; }
    bool hasBom() const { return m_hasBom; }
    QString detectedEncoding() const { return m_detectedEncoding; }
    OpenMode openMode() const { return m_openMode; }
    const QByteArray& rawData() const { return m_rawData; }

private:
    static QString normalizeLineEndings(QString text, ScintillaDocument::LineEnding lineEnding);

    QString    m_filePath;
    QByteArray m_rawData;
    QByteArray m_detectionSample;
    QString    m_detectedEncoding;
    qint64     m_fileSize = 0;
    bool       m_hasBom = false;
    bool       m_truncated = false;
    OpenMode   m_openMode = OpenMode::Full;
};

