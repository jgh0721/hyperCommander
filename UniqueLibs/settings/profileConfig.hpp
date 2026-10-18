#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStringList>
#include "UniqueLibs/cmnTypeDefs_Name.hpp"

namespace hc::settings {
// HyperCommander.json holds the settings sections and "workspace" in one file.
// Legacy settings.json/workspace.json are copied once when the file is missing; sources stay untouched.
class ProfileConfig {
public:
    static constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    static QString FileName(const QString& Directory) { return QDir(Directory).filePath(MAIN_CONFIG_FILENAME); }
    // 이전 파일이 있는 첫 폴더만 사용한다. 설정과 작업공간을 다른 폴더에서 섞지 않는다.
    static QString LegacyDirectory(const QString& Directory, const QStringList& Legacy)
    {
        for (const auto& Candidate : QStringList{Directory} + Legacy) {
            if (Candidate.isEmpty()) continue;
            const QDir Folder(Candidate);
            if (QFileInfo::exists(Folder.filePath("settings.json")) || QFileInfo::exists(Folder.filePath("workspace.json"))) return Folder.absolutePath();
        }
        return {};
    }
    // 작업공간만 저장된 파일은 신규 설치로 보고, 읽을 수 없는 파일은 기존 설치로 본다.
    static bool HasSettings(const QString& Directory, const QStringList& Legacy)
    {
        QFile File(FileName(Directory));
        if (!File.exists()) {
            const auto Source = LegacyDirectory(Directory, Legacy);
            return !Source.isEmpty() && QFileInfo::exists(QDir(Source).filePath("settings.json"));
        }
        if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) return true;
        const auto Doc = QJsonDocument::fromJson(File.readAll());
        if (!Doc.isObject()) return true;
        const auto Root = Doc.object();
        for (auto It = Root.begin(); It != Root.end(); ++It) if (It.key() != "schemaVersion" && It.key() != "workspace") return true;
        return false;
    }
    static bool Migrate(const QString& Directory, const QStringList& Legacy, QString& Error)
    {
        Error.clear();
        const auto Path = FileName(Directory);
        if (QFileInfo::exists(Path)) return true;
        const auto Source = LegacyDirectory(Directory, Legacy);
        if (Source.isEmpty()) return true;
        if (!QDir().mkpath(Directory)) { Error = "create-directory-failed"; return false; }
        QLockFile Lock(Path + ".lock"); Lock.setStaleLockTime(0);
        if (!Lock.tryLock(0)) { Error = "file-locked"; return false; }
        if (QFileInfo::exists(Path)) return true;
        QJsonObject Settings, Workspace;
        if (!read(QDir(Source).filePath("settings.json"), Settings, Error) || !read(QDir(Source).filePath("workspace.json"), Workspace, Error)) return false;
        QJsonObject Root;
        for (auto It = Settings.begin(); It != Settings.end(); ++It) if (It.key() != "workspace") Root[It.key()] = It.value();
        if (Workspace.contains("workspace")) Root["workspace"] = Workspace["workspace"];
        Root["schemaVersion"] = 1;
        auto Bytes = QJsonDocument(Root).toJson(QJsonDocument::Indented); Bytes.replace("\n", "\r\n");
        if (Bytes.size() > MaxBytes) { Error = "legacy-document-too-large"; return false; }
        QSaveFile File(Path); File.setDirectWriteFallback(false);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) { Error = "legacy-migration-save-failed"; return false; }
        return true;
    }
private:
    // 없는 이전 파일은 빈 문서로 본다.
    static bool read(const QString& Path, QJsonObject& Object, QString& Error)
    {
        Object = {};
        if (!QFileInfo::exists(Path)) return true;
        QFile File(Path);
        if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) { Error = "legacy-read-failed: " + Path; return false; }
        QJsonParseError Parse; const auto Doc = QJsonDocument::fromJson(File.readAll(), &Parse);
        if (File.error() != QFileDevice::NoError || Parse.error != QJsonParseError::NoError || !Doc.isObject()) { Error = "legacy-invalid-json: " + Path; return false; }
        Object = Doc.object();
        if (Object.value("schemaVersion") != QJsonValue(1)) { Error = "legacy-unsupported-schema-version: " + Path; return false; }
        return true;
    }
};
}
