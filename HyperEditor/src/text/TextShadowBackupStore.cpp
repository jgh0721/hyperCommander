#include "text/TextShadowBackupStore.h"

#include "core/AppSettings.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtGlobal>

#include <utility>

namespace {

constexpr quint32 kMagic = 0x4D565442; // MVTB
constexpr quint32 kVersion = 2;

void setError(QString* errorMessage, const QString& message)
{
    if (errorMessage)
        *errorMessage = message;
}

} // namespace

QString TextShadowBackupStore::backupDirectoryPath()
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (basePath.isEmpty())
        basePath = QDir::tempPath() + QStringLiteral("/MultiViewer");

    return QDir(basePath).filePath(QStringLiteral("TextHotExit"));
}

QString TextShadowBackupStore::backupPathForFile(const QString& originalFilePath)
{
    const QString normalizedPath = normalizedFilePath(originalFilePath);
    if (normalizedPath.isEmpty())
        return {};

    return backupPathForKey(normalizedPath);
}

QString TextShadowBackupStore::backupPathForUntitled(const QString& untitledId)
{
    const QString id = normalizedUntitledId(untitledId);
    if (id.isEmpty())
        return {};

    return backupPathForKey(QStringLiteral("untitled:%1").arg(id));
}

bool TextShadowBackupStore::saveSnapshot(const Snapshot& snapshot, QString* errorMessage)
{
    AppSettings settings;
    if (!settings.value("textView/hotExitEnabled", true).toBool())
        return true;

    const QString untitledId = normalizedUntitledId(snapshot.untitledId);
    const QString normalizedPath = normalizedFilePath(snapshot.originalFilePath);
    if (snapshot.isUntitled && untitledId.isEmpty()) {
        setError(errorMessage, QObject::tr("제목없음 문서 백업 ID가 비어 있습니다."));
        return false;
    }
    if (!snapshot.isUntitled && normalizedPath.isEmpty()) {
        setError(errorMessage, QObject::tr("원본 파일 경로가 비어 있습니다."));
        return false;
    }

    QDir backupDir(backupDirectoryPath());
    if (!backupDir.exists() && !backupDir.mkpath(QStringLiteral("."))) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 폴더를 만들 수 없습니다: %1").arg(backupDir.absolutePath()));
        return false;
    }

    Snapshot effective = snapshot;
    effective.isUntitled = snapshot.isUntitled;
    effective.untitledId = snapshot.isUntitled ? untitledId : QString();
    effective.originalFilePath = snapshot.isUntitled ? QString() : normalizedPath;
    if (!effective.savedAtUtc.isValid())
        effective.savedAtUtc = QDateTime::currentDateTimeUtc();

    const QString path = effective.isUntitled
        ? backupPathForUntitled(effective.untitledId)
        : backupPathForFile(effective.originalFilePath);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일을 열 수 없습니다: %1").arg(path));
        return false;
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic
        << kVersion
        << effective.isUntitled
        << effective.untitledId
        << effective.displayTitle
        << effective.originalFilePath
        << effective.encoding
        << effective.detectedEncoding
        << static_cast<qint32>(effective.lineEnding)
        << static_cast<qint32>(effective.caretPosition)
        << static_cast<qint32>(effective.firstVisibleLine)
        << static_cast<qint64>(effective.originalSize)
        << static_cast<qint64>(effective.originalLastModifiedUtcMs)
        << effective.savedAtUtc
        << effective.text;

    if (out.status() != QDataStream::Ok) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 직렬화에 실패했습니다: %1").arg(path));
        return false;
    }

    if (!file.commit()) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일을 저장할 수 없습니다: %1").arg(path));
        return false;
    }

    return true;
}

bool TextShadowBackupStore::loadSnapshot(const QString& originalFilePath, Snapshot* snapshot, QString* errorMessage)
{
    if (!snapshot) {
        setError(errorMessage, QObject::tr("스냅샷 출력 포인터가 비어 있습니다."));
        return false;
    }

    const QString path = backupPathForFile(originalFilePath);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    return loadSnapshotFile(path, snapshot, errorMessage);
}

bool TextShadowBackupStore::loadUntitledSnapshot(const QString& untitledId, Snapshot* snapshot, QString* errorMessage)
{
    if (!snapshot) {
        setError(errorMessage, QObject::tr("스냅샷 출력 포인터가 비어 있습니다."));
        return false;
    }

    const QString path = backupPathForUntitled(untitledId);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    return loadSnapshotFile(path, snapshot, errorMessage);
}

QList<TextShadowBackupStore::Snapshot> TextShadowBackupStore::restorableSnapshots(bool includeText)
{
    QList<Snapshot> snapshots;
    QDir backupDir(backupDirectoryPath());
    if (!backupDir.exists())
        return snapshots;

    const QFileInfoList files = backupDir.entryInfoList({QStringLiteral("*.mvtextbak")}, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& fileInfo : files) {
        Snapshot snapshot;
        if (!loadSnapshotFile(fileInfo.absoluteFilePath(), &snapshot, nullptr, includeText))
            continue;
        if (!snapshot.isUntitled && !originalFileMatchesSnapshot(snapshot))
            continue;
        snapshots.append(snapshot);
    }

    return snapshots;
}

QStringList TextShadowBackupStore::restorableFilePaths()
{
    QStringList paths;
    for (const Snapshot& snapshot : restorableSnapshots(false)) {
        if (snapshot.isUntitled)
            continue;
        if (!paths.contains(snapshot.originalFilePath, Qt::CaseInsensitive))
            paths.append(snapshot.originalFilePath);
    }

    return paths;
}

bool TextShadowBackupStore::loadSnapshotFile(const QString& backupFilePath, Snapshot* snapshot, QString* errorMessage, bool includeText)
{
    if (!snapshot) {
        setError(errorMessage, QObject::tr("스냅샷 출력 포인터가 비어 있습니다."));
        return false;
    }

    const QString path = QDir::cleanPath(backupFilePath);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일을 열 수 없습니다: %1").arg(path));
        return false;
    }

    quint32 magic = 0;
    quint32 version = 0;
    qint32 lineEnding = static_cast<qint32>(ScintillaDocument::LF);
    qint32 caretPosition = 0;
    qint32 firstVisibleLine = 0;
    Snapshot loaded;

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    in >> magic
       >> version;

    if (magic != kMagic || (version != 1 && version != kVersion)) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일 형식이 올바르지 않습니다: %1").arg(path));
        return false;
    }

    if (version >= 2) {
        in >> loaded.isUntitled
           >> loaded.untitledId
           >> loaded.displayTitle;
    }

    in >> loaded.originalFilePath
       >> loaded.encoding
       >> loaded.detectedEncoding
       >> lineEnding
       >> caretPosition
       >> firstVisibleLine
       >> loaded.originalSize
       >> loaded.originalLastModifiedUtcMs
       >> loaded.savedAtUtc;

    if (includeText)
        in >> loaded.text;

    if (in.status() != QDataStream::Ok) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일 형식이 올바르지 않습니다: %1").arg(path));
        return false;
    }

    loaded.untitledId = normalizedUntitledId(loaded.untitledId);
    loaded.originalFilePath = loaded.isUntitled ? QString() : normalizedFilePath(loaded.originalFilePath);
    loaded.lineEnding = static_cast<ScintillaDocument::LineEnding>(lineEnding);
    loaded.caretPosition = qMax(0, caretPosition);
    loaded.firstVisibleLine = qMax(0, firstVisibleLine);
    *snapshot = std::move(loaded);
    return true;
}

bool TextShadowBackupStore::hasSnapshot(const QString& originalFilePath)
{
    const QString path = backupPathForFile(originalFilePath);
    return !path.isEmpty() && QFileInfo::exists(path);
}

bool TextShadowBackupStore::deleteSnapshot(const QString& originalFilePath, QString* errorMessage)
{
    const QString path = backupPathForFile(originalFilePath);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;

    if (!QFile::remove(path)) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일을 삭제할 수 없습니다: %1").arg(path));
        return false;
    }

    return true;
}

bool TextShadowBackupStore::deleteUntitledSnapshot(const QString& untitledId, QString* errorMessage)
{
    const QString path = backupPathForUntitled(untitledId);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return true;

    if (!QFile::remove(path)) {
        setError(errorMessage, QObject::tr("핫 엑시트 백업 파일을 삭제할 수 없습니다: %1").arg(path));
        return false;
    }

    return true;
}

bool TextShadowBackupStore::deleteAllBackups(QString* errorMessage)
{
    QDir backupDir(backupDirectoryPath());
    if (!backupDir.exists())
        return true;

    bool ok = true;
    const QFileInfoList files = backupDir.entryInfoList({QStringLiteral("*.mvtextbak")}, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& fileInfo : files) {
        if (!QFile::remove(fileInfo.absoluteFilePath()))
            ok = false;
    }

    if (!ok)
        setError(errorMessage, QObject::tr("일부 핫 엑시트 백업 파일을 삭제할 수 없습니다: %1").arg(backupDir.absolutePath()));

    return ok;
}

bool TextShadowBackupStore::originalFileMatchesSnapshot(const Snapshot& snapshot)
{
    if (snapshot.isUntitled)
        return true;

    const QString normalizedPath = normalizedFilePath(snapshot.originalFilePath);
    if (normalizedPath.isEmpty())
        return false;

    const QFileInfo info(normalizedPath);
    if (!info.exists() || !info.isFile())
        return false;

    if (snapshot.originalSize >= 0 && info.size() != snapshot.originalSize)
        return false;

    if (snapshot.originalLastModifiedUtcMs > 0) {
        const qint64 current = info.lastModified().toUTC().toMSecsSinceEpoch();
        if (qAbs(current - snapshot.originalLastModifiedUtcMs) > 2000)
            return false;
    }

    return true;
}

QString TextShadowBackupStore::backupPathForKey(const QString& key)
{
    const QString normalizedKey = key.trimmed();
    if (normalizedKey.isEmpty())
        return {};

    const QByteArray hash = QCryptographicHash::hash(normalizedKey.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(backupDirectoryPath()).filePath(QString::fromLatin1(hash) + QStringLiteral(".mvtextbak"));
}

QString TextShadowBackupStore::normalizedUntitledId(const QString& untitledId)
{
    QString id = untitledId.trimmed();
    if (id.startsWith(QLatin1Char('{')) && id.endsWith(QLatin1Char('}')) && id.size() > 2)
        id = id.mid(1, id.size() - 2);
    return id;
}

QString TextShadowBackupStore::normalizedFilePath(const QString& filePath)
{
    if (filePath.trimmed().isEmpty())
        return {};

    const QFileInfo info(filePath);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);

    return QDir::cleanPath(info.absoluteFilePath());
}






