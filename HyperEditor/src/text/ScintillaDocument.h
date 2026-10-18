#pragma once

#include <QString>

class ScintillaDocument
{
public:
    enum LineEnding {
        LF,
        CRLF,
        CR
    };

    struct Metrics
    {
        int lineCount = 1;
        int currentLine = 1;
        int currentColumn = 1;
        int characterCount = 0;
        int selectedCharacterCount = 0;
    };

    QString language() const { return m_language; }
    void setLanguage(const QString& language) { m_language = language.isEmpty() ? QStringLiteral("None") : language; }

    LineEnding lineEnding() const { return m_lineEnding; }
    void setLineEnding(LineEnding lineEnding) { m_lineEnding = lineEnding; }

    bool isPreviewOnly() const { return m_previewOnly; }
    void setPreviewOnly(bool previewOnly) { m_previewOnly = previewOnly; }

    bool isTruncated() const { return m_truncated; }
    void setTruncated(bool truncated) { m_truncated = truncated; }

    const Metrics& metrics() const { return m_metrics; }
    void setMetrics(const Metrics& metrics) { m_metrics = metrics; }

private:
    QString    m_language = QStringLiteral("None");
    LineEnding m_lineEnding = CRLF;
    bool       m_previewOnly = false;
    bool       m_truncated = false;
    Metrics    m_metrics;
};

