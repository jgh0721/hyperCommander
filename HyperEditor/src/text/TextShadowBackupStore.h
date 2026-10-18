#pragma once

#include "text/ScintillaDocument.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

class TextShadowBackupStore
{
public:
    struct Snapshot
    {
        bool isUntitled = false;
        QString untitledId;
        QString displayTitle;
        QString originalFilePath;
        QString text;
        QString encoding;
        QString detectedEncoding;
        ScintillaDocument::LineEnding lineEnding = ScintillaDocument::LF;
        int caretPosition = 0;
        int firstVisibleLine = 0;
        qint64 originalSize = -1;
        qint64 originalLastModifiedUtcMs = 0;
        QDateTime savedAtUtc;
    };

    static QString backupDirectoryPath();
    static QString backupPathForFile(const QString& originalFilePath);
    static QString backupPathForUntitled(const QString& untitledId);
    static bool saveSnapshot(const Snapshot& snapshot, QString* errorMessage = nullptr);
    static bool loadSnapshot(const QString& originalFilePath, Snapshot* snapshot, QString* errorMessage = nullptr);
    static bool loadUntitledSnapshot(const QString& untitledId, Snapshot* snapshot, QString* errorMessage = nullptr);
    static QList<Snapshot> restorableSnapshots(bool includeText = false);
    static QStringList restorableFilePaths();
    static bool hasSnapshot(const QString& originalFilePath);
    static bool deleteUntitledSnapshot(const QString& untitledId, QString* errorMessage = nullptr);
    static bool deleteSnapshot(const QString& originalFilePath, QString* errorMessage = nullptr);
    static bool deleteAllBackups(QString* errorMessage = nullptr);
    static bool originalFileMatchesSnapshot(const Snapshot& snapshot);

private:
    static bool loadSnapshotFile(const QString& backupFilePath,
                                 Snapshot* snapshot,
                                 QString* errorMessage = nullptr,
                                 bool includeText = true);
    static QString backupPathForKey(const QString& key);
    static QString normalizedUntitledId(const QString& untitledId);
    static QString normalizedFilePath(const QString& filePath);
};




