#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace hc::rst {

struct DiagnosticEntry {
    QString uri;
    QString path;
    int line = 1;
    int character = 1;
    int severity = 0;
    QString message;
    QString source;
    int endLine = 1;
    int endCharacter = 2;
};

[[nodiscard]] QString pathToFileUri(const QString& path);
[[nodiscard]] QString fileUriToPath(const QString& uri);
[[nodiscard]] QString severityLabel(int severity);
[[nodiscard]] QString diagnosticTooltipText(const QVector<DiagnosticEntry>& entries, const QString& path, int line);
[[nodiscard]] QVector<DiagnosticEntry> parseLspDiagnostics(const QString& uri, const QJsonArray& diagnostics);
[[nodiscard]] QVector<DiagnosticEntry> parseSphinxBuildDiagnostics(const QString& output, const QString& workingDirectory);
[[nodiscard]] QVector<DiagnosticEntry> deduplicateDiagnostics(const QVector<DiagnosticEntry>& entries);

}  // namespace hc::rst

