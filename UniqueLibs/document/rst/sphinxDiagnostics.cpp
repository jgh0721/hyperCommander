#include "stdafx.h"
#include "sphinxDiagnostics.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

int intOrDefault(const QJsonValue& value, int fallback) {
    if (value.isDouble()) {
        return value.toInt(fallback);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

QString locationKey(const DiagnosticEntry& entry) {
    QString base = !entry.path.isEmpty() ? QFileInfo(entry.path).absoluteFilePath() : entry.uri;
#ifdef Q_OS_WIN
    base = base.toCaseFolded();
#endif
    return QStringLiteral("%1:%2:%3").arg(base).arg(qMax(1, entry.line)).arg(qMax(1, entry.character));
}

bool isEsbonio(const DiagnosticEntry& entry) {
    return entry.source.compare(QStringLiteral("esbonio"), Qt::CaseInsensitive) == 0;
}

bool isSphinxBuild(const DiagnosticEntry& entry) {
    return entry.source.compare(QStringLiteral("sphinx-build"), Qt::CaseInsensitive) == 0;
}

}  // namespace

QString pathToFileUri(const QString& path) {
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
}

QString fileUriToPath(const QString& uri) {
    const QUrl url(uri);
    if (!url.isLocalFile()) {
        return {};
    }
    return QFileInfo(url.toLocalFile()).absoluteFilePath();
}

/// LSP 의 숫자 severity 를 사람이 읽을 이름으로.
///
/// 진단 표의 "심각도" 열과 툴팁에만 쓰인다 — 비교나 파싱에는 쓰지 않으므로
/// 번역해도 안전하다. (외부 프로세스 출력에 들어 있는 WARNING/ERROR 문자열은
/// 아래 parseSphinxBuildDiagnostics() 의 정규식이 다루며, 그쪽은 번역 금지다.)
///
/// namespace 함수라 tr() 이 없다. 컨텍스트를 직접 적는다.
QString severityLabel(int severity) {
    switch (severity) {
    case 1:
        return QCoreApplication::translate("SphinxDiagnostics", "Error");
    case 2:
        return QCoreApplication::translate("SphinxDiagnostics", "Warning");
    case 3:
        return QCoreApplication::translate("SphinxDiagnostics", "Information");
    case 4:
        return QCoreApplication::translate("SphinxDiagnostics", "Hint");
    default:
        return QCoreApplication::translate("SphinxDiagnostics", "Unknown");
    }
}

QString diagnosticTooltipText(const QVector<DiagnosticEntry>& entries, const QString& path, int line) {
    if (path.trimmed().isEmpty() || line <= 0) {
        return {};
    }
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    QStringList messages;
    for (const DiagnosticEntry& entry : entries) {
        if (entry.path.isEmpty()) {
            continue;
        }
        if (QFileInfo(entry.path).absoluteFilePath().compare(normalizedPath, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const int startLine = qMax(1, entry.line);
        const int endLine = qMax(startLine, entry.endLine);
        if (line < startLine || line > endLine) {
            continue;
        }
        messages.push_back(QStringLiteral("%1 · %2: %3")
            .arg(severityLabel(entry.severity), entry.source.isEmpty() ? QStringLiteral("diagnostic") : entry.source, entry.message));
    }
    return messages.join(QStringLiteral("\n"));
}

QVector<DiagnosticEntry> parseLspDiagnostics(const QString& uri, const QJsonArray& diagnostics) {
    QVector<DiagnosticEntry> entries;
    const QString path = fileUriToPath(uri);
    entries.reserve(diagnostics.size());
    for (const QJsonValue& value : diagnostics) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject diagnostic = value.toObject();
        const QJsonObject range = diagnostic.value(QStringLiteral("range")).toObject();
        const QJsonObject start = range.value(QStringLiteral("start")).toObject();
        const QJsonObject end = range.value(QStringLiteral("end")).toObject();
        DiagnosticEntry entry;
        entry.uri = uri;
        entry.path = path;
        entry.line = intOrDefault(start.value(QStringLiteral("line")), 0) + 1;
        entry.character = intOrDefault(start.value(QStringLiteral("character")), 0) + 1;
        entry.endLine = intOrDefault(end.value(QStringLiteral("line")), entry.line - 1) + 1;
        entry.endCharacter = intOrDefault(end.value(QStringLiteral("character")), entry.character) + 1;
        if (std::pair(entry.endLine, entry.endCharacter) <= std::pair(entry.line, entry.character)) {
            entry.endLine = entry.line;
            entry.endCharacter = entry.character + 1;
        }
        entry.severity = diagnostic.value(QStringLiteral("severity")).isDouble() ? diagnostic.value(QStringLiteral("severity")).toInt() : 0;
        entry.message = diagnostic.value(QStringLiteral("message")).toString();
        entry.source = diagnostic.value(QStringLiteral("source")).toString(QStringLiteral("esbonio"));
        entries.push_back(entry);
    }
    return entries;
}

QVector<DiagnosticEntry> parseSphinxBuildDiagnostics(const QString& output, const QString& workingDirectory) {
    // ⚠ 이 패턴은 sphinx-build 가 뱉은 **외부 프로세스 출력**을 읽는다. 번역
    //   대상이 아니며, 서브프로세스에 LANG/LC_ALL 을 주입해서도 안 된다 —
    //   Sphinx 출력이 로컬라이즈되는 순간 진단이 하나도 잡히지 않는다.
    static const QRegularExpression re(QStringLiteral(R"(^(.+?):(\d+):\s*(WARNING|ERROR|CRITICAL):\s*(.*)$)"));
    QVector<DiagnosticEntry> entries;
    const QStringList lines = output.split(QRegularExpression(QStringLiteral("\r?\n")), Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        const QRegularExpressionMatch match = re.match(rawLine.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        QString path = match.captured(1);
        if (QDir::isRelativePath(path)) {
            path = QDir(workingDirectory).absoluteFilePath(path);
        }
        path = QFileInfo(path).absoluteFilePath();
        const QString level = match.captured(3);
        DiagnosticEntry entry;
        entry.path = path;
        entry.uri = pathToFileUri(path);
        entry.line = qMax(1, match.captured(2).toInt());
        entry.character = 1;
        entry.endLine = entry.line;
        entry.endCharacter = 2;
        entry.severity = (level == QStringLiteral("ERROR") || level == QStringLiteral("CRITICAL")) ? 1 : 2;
        entry.message = match.captured(4);
        entry.source = QStringLiteral("sphinx-build");
        entries.push_back(entry);
    }
    return entries;
}

QVector<DiagnosticEntry> deduplicateDiagnostics(const QVector<DiagnosticEntry>& entries) {
    QVector<DiagnosticEntry> result;
    QHash<QString, qsizetype> indexByLocation;
    for (const DiagnosticEntry& entry : entries) {
        const QString key = locationKey(entry);
        const auto it = indexByLocation.constFind(key);
        if (it == indexByLocation.constEnd()) {
            indexByLocation.insert(key, result.size());
            result.push_back(entry);
            continue;
        }
        DiagnosticEntry& existing = result[*it];
        if (isEsbonio(entry) && isSphinxBuild(existing)) {
            existing = entry;
        }
    }
    return result;
}

}  // namespace hc::rst


