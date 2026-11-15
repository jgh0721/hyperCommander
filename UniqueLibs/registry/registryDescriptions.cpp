#include "stdafx.h"
#include "registryDescriptions.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QLocale>
#include <QtConcurrentRun>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryDescriptions, "hc.registry.descriptions")
namespace hc::registry {
RegistryDescriptions::RegistryDescriptions(QString Directory, QString Language, QString CacheDirectory, QObject* Parent)
    : QObject(Parent), directory_(std::move(Directory)), language_(std::move(Language)), cacheDirectory_(std::move(CacheDirectory))
{
    debounce_.setSingleShot(true); debounce_.setInterval(500);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { debounce_.start(); });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { debounce_.start(); });
    connect(&debounce_, &QTimer::timeout, this, &RegistryDescriptions::Reload);
    Reload();
}
RegistryDescriptions::~RegistryDescriptions() { if (cancel_) cancel_->store(true); }
RegistryDescriptions* RegistryDescriptions::Shared(const QString& CacheDirectory)
{
    static QPointer<RegistryDescriptions> Instance;
    if (!Instance) Instance = new RegistryDescriptions(QDir(qEnvironmentVariable("WINDIR")).filePath("PolicyDefinitions"),
        QString(QLocale::system().name()).replace('_', '-'), CacheDirectory, QCoreApplication::instance());
    return Instance;
}
void RegistryDescriptions::Reload()
{
    if (loading_) { reloadRequested_ = true; return; }
    loading_ = true; const auto Generation = ++generation_; cancel_ = std::make_shared<std::atomic_bool>(false);
    struct Loaded { AdmxCatalog admx; BuiltinRegistryCatalog builtin; QStringList files, directories; };
    auto* Future = new QFutureWatcher<Loaded>(this);
    connect(Future, &QFutureWatcher<Loaded>::finished, this, [this, Future, Generation] {
        const auto Result = Future->result(); Future->deleteLater();
        if (generation_ != Generation) return;
        policies_ = std::make_shared<AdmxCatalog>(Result.admx); builtin_ = Result.builtin; lookup_.clear(); loading_ = false;
        if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
        if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
        if (!Result.files.isEmpty()) watcher_.addPaths(Result.files);
        if (!Result.directories.isEmpty()) watcher_.addPaths(Result.directories);
        emit Changed();
        if (std::exchange(reloadRequested_, false)) Reload();
    });
    Future->setFuture(QtConcurrent::run([Directory = directory_, Language = language_, Cache = cacheDirectory_, Cancel = cancel_] {
        Loaded Result; Result.admx = AdmxCatalog::Load(Directory, Language, Cache, Cancel.get());
        if (Cancel->load()) return Result;
        Result.builtin = BuiltinRegistryCatalog::Bundled();
        for (const auto& Dir : QStringList{Directory, QDir(Directory).filePath(Language), QDir(Directory).filePath("en-US")}) {
            if (!QDir(Dir).exists() || Result.directories.contains(Dir)) continue;
            Result.directories.append(Dir);
            for (const auto& File : QDir(Dir).entryInfoList({"*.admx", "*.adml"}, QDir::Files))
                if (Result.files.size() < 10000) Result.files.append(File.absoluteFilePath());
        }
        return Result;
    }));
}
RegistryExplanation RegistryDescriptions::Find(const RegistryLocation& Location, const std::optional<QString>& Name, std::optional<quint32> Type) const
{
    const auto Id = Location.Id() + '|' + (Name ? "value:" + QString::fromLatin1(Name->toCaseFolded().toUtf8().toHex()) : QString("key"))
        + '|' + (Type ? QString::number(*Type) : QString());
    if (const auto Found = lookup_.constFind(Id); Found != lookup_.cend()) return *Found;
    RegistryExplanation Result;
    auto BuiltinLocation = Location;
    if ((Location.hive == Hive::CurrentUser || Location.hive == Hive::LocalMachine)
        && Location.key.startsWith("Software\\Classes\\", Qt::CaseInsensitive)) {
        BuiltinLocation.hive = Hive::ClassesRoot; BuiltinLocation.key = Location.key.mid(17);
    }
    Result.builtin = builtin_.Find(BuiltinLocation, Name, Type, language_);
    if (Result.builtin) {
        Result.title = Result.builtin.title; Result.text = Result.builtin.text;
        Result.sources = Result.builtin.sourceUrls; Result.warnings = Result.builtin.warnings;
    }
    if (policies_) Result.policyIndexes = policies_->Find(Location, Name);
    if (!Result.policyIndexes.isEmpty()) {
        QStringList Titles, Texts;
        for (const auto Index : Result.policyIndexes) {
            const auto& Policy = policies_->policies[Index]; Titles.append(Policy.title);
            Texts.append(Policy.title + '\n' + Policy.explanation + (Policy.supportedOn.isEmpty() ? QString() : '\n' + Policy.supportedOn));
            Result.sources.append(Policy.sourcePath);
        }
        Result.title = Titles.join(" / "); Result.text = Texts.join("\n\n");
    }
    // Bound hot-row lookup memory; clearing affects only derived descriptions.
    if (lookup_.size() >= 10000) lookup_.clear();
    lookup_.insert(Id, Result); return Result;
}
} // namespace hc::registry
