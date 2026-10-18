#pragma once
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>

namespace hc::settings {
// All product writers share this lock and merge only their changed leaves.
// Arrays are atomic: concurrent edits to the same list require explicit resolution.
class UnifiedConfig {
public:
    static constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    static QString FileName(const QString& Directory) { return QDir(Directory).filePath("HyperEditor.json"); }
    static QJsonValue Encode(const QVariant& Value)
    {
        if (Value.metaType().id() == QMetaType::QByteArray)
            return QJsonObject{{"$type", "QByteArray"}, {"base64", QString::fromLatin1(Value.toByteArray().toBase64())}};
        switch (Value.metaType().id()) {
        case QMetaType::Bool: case QMetaType::Int: case QMetaType::UInt: case QMetaType::Double:
        case QMetaType::QString: case QMetaType::QStringList:
            return QJsonValue::fromVariant(Value);
        default: break;
        }
        if (!Value.isValid()) return QJsonValue::Null;
        QByteArray Bytes; QDataStream Stream(&Bytes, QIODevice::WriteOnly); Stream.setVersion(QDataStream::Qt_6_0); Stream << Value;
        return QJsonObject{{"$type", "QVariant"}, {"base64", QString::fromLatin1(Bytes.toBase64())}};
    }
    static QVariant Decode(const QJsonValue& Value)
    {
        if (Value.isObject() && Value.toObject().contains("$type")) {
            const auto Record = Value.toObject(); const auto Bytes = QByteArray::fromBase64(Record["base64"].toString().toLatin1());
            if (Record["$type"] == "QByteArray") return Bytes;
            if (Record["$type"] == "QVariant") { QDataStream Stream(Bytes); Stream.setVersion(QDataStream::Qt_6_0); QVariant Result; Stream >> Result; return Result; }
        }
        return Value.toVariant();
    }
    static QJsonValue Value(const QJsonObject& Object, const QStringList& Path)
    {
        QJsonValue Result(Object);
        for (const auto& Key : Path) Result = Result.toObject().value(Key);
        return Result;
    }
    static void Set(QJsonObject& Object, const QStringList& Path, const QJsonValue& Value)
    {
        if (Path.isEmpty()) return;
        if (Path.size() == 1) { if (Value.isUndefined()) Object.remove(Path.first()); else Object[Path.first()] = Value; return; }
        auto Child = Object[Path.first()].toObject(); Set(Child, Path.mid(1), Value); Object[Path.first()] = Child;
    }
    static bool Load(const QString& Path, QJsonObject& Object, QString& Error, bool PersistMigration = true)
    {
        Error.clear();
        if (QFileInfo::exists(Path)) return read(Path, 2, Object, Error);
        const QDir Directory(QFileInfo(Path).absolutePath());
        bool Legacy = false;
        for (const auto* Name : {"settings.json", "workspace.json", "document-views.ini"}) Legacy |= QFileInfo::exists(Directory.filePath(Name));
        Object = {{"schemaVersion", 2}};
        if (!Legacy) return true;
        QLockFile Lock(Path + ".lock"); Lock.setStaleLockTime(0);
        if (PersistMigration && !Lock.tryLock(0)) { Error = "file-locked"; return false; }
        if (QFileInfo::exists(Path)) return read(Path, 2, Object, Error);
        QJsonObject Sources;
        for (const auto* Name : {"settings.json", "workspace.json", "document-views.ini"}) {
            const auto Source = Directory.filePath(Name); if (!QFileInfo::exists(Source)) continue;
            QFile File(Source);
            if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) { Error = Source + ": read-failed"; return false; }
            const auto Bytes = File.readAll();
            if (File.error() != QFileDevice::NoError) { Error = Source + ": read-failed"; return false; }
            Sources[Name] = QJsonObject{{"path", Source}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex())}};
            if (QString(Name) == "document-views.ini") {
                QSettings Ini(Source, QSettings::IniFormat); QJsonObject Views;
                for (const auto& Key : Ini.allKeys()) Set(Views, Key.split('/'), Encode(Ini.value(Key)));
                if (Ini.status() != QSettings::NoError) { Error = Source + ": invalid-ini"; return false; }
                Object["documentViews"] = Views;
            } else {
                QJsonObject Old; if (!read(Source, 1, Old, Error)) return false;
                if (QString(Name) == "settings.json") {
                    for (auto It = Old.begin(); It != Old.end(); ++It) if (It.key() != "schemaVersion") Object[It.key()] = It.value();
                } else {
                    if (Old.contains("workspace")) Object["workspace"] = Old["workspace"];
                    Old.remove("workspace"); Old.remove("schemaVersion");
                    if (!Old.isEmpty()) Object["legacyWorkspace"] = Old;
                }
            }
        }
        Object["schemaVersion"] = 2;
        Object["migration"] = QJsonObject{{"completed", true}, {"sources", Sources}};
        return !PersistMigration || write(Path, Object, Error);
    }
    static bool Save(const QString& Path, const QJsonObject& Base, const QJsonObject& Desired,
        QJsonObject& Result, QString& Error, bool ResolveConflicts = false)
    {
        QJsonObject Initial; if (!Load(Path, Initial, Error)) return false;
        if (!QDir().mkpath(QFileInfo(Path).absolutePath())) { Error = "create-directory-failed"; return false; }
        QLockFile Lock(Path + ".lock"); Lock.setStaleLockTime(0);
        if (!Lock.tryLock(0)) { Error = "file-locked"; return false; }
        QJsonObject Disk{{"schemaVersion", 2}};
        if (QFileInfo::exists(Path) && !read(Path, 2, Disk, Error)) return false;
        QStringList Conflicts;
        Result = merge(Base, Desired, Disk, {}, Conflicts, ResolveConflicts).toObject();
        if (!Conflicts.isEmpty()) { Error = "conflict: " + Conflicts.join(", "); return false; }
        Result["schemaVersion"] = 2;
        return write(Path, Result, Error);
    }
    static QJsonObject Changed(const QJsonObject& Base, const QJsonObject& Desired, const QJsonObject& Target)
    {
        QStringList Unused; return merge(Base, Desired, Target, {}, Unused, true).toObject();
    }
private:
    static QJsonValue merge(const QJsonValue& Base, const QJsonValue& Desired, const QJsonValue& Disk,
        const QString& Path, QStringList& Conflicts, bool Force)
    {
        if (Base == Desired) return Disk;
        if (Desired.isObject() && (Base.isObject() || Base.isUndefined()) && (Disk.isObject() || Disk.isUndefined())) {
            const auto Before = Base.toObject(), After = Desired.toObject(); auto Result = Disk.toObject();
            auto Keys = Before.keys(); for (const auto& Key : After.keys()) if (!Keys.contains(Key)) Keys.append(Key);
            for (const auto& Key : Keys) {
                const auto Value = merge(Before.value(Key), After.value(Key), Result.value(Key), Path.isEmpty() ? Key : Path + '.' + Key, Conflicts, Force);
                if (Value.isUndefined()) Result.remove(Key); else Result[Key] = Value;
            }
            return Result;
        }
        if (!Force && Disk != Base && Disk != Desired) { Conflicts.append(Path); return Disk; }
        return Desired;
    }
    static bool read(const QString& Path, int Version, QJsonObject& Object, QString& Error)
    {
        QFile File(Path);
        if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) { Error = Path + ": read-failed"; return false; }
        QJsonParseError Parse; const auto Doc = QJsonDocument::fromJson(File.readAll(), &Parse);
        if (File.error() != QFileDevice::NoError || Parse.error != QJsonParseError::NoError || !Doc.isObject()) { Error = Path + ": invalid-json"; return false; }
        Object = Doc.object();
        if (Object.value("schemaVersion") != QJsonValue(Version)) { Error = Path + ": unsupported-schema-version"; return false; }
        return true;
    }
    static bool write(const QString& Path, const QJsonObject& Object, QString& Error)
    {
        auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Indented); Bytes.replace("\n", "\r\n");
        if (Bytes.size() > MaxBytes) { Error = "document-too-large"; return false; }
        QSaveFile File(Path); File.setDirectWriteFallback(false);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) { Error = Path + ": atomic-save-failed"; return false; }
        Error.clear(); return true;
    }
};
}
