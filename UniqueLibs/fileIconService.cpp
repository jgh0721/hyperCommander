#include "stdafx.h"
#include "fileIconService.hpp"

/// Modules
#include "nativeIconExtractor.hpp"

/// Platform/framework headers
#include <QApplication>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QPainter>
#include <QStyle>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace hc::icons {
QString Request::Family() const
{
    return QString::number(int(kind_)) + QChar(0)
        + (virtual_ ? QStringLiteral("shell:") + path_ : QDir::fromNativeSeparators(path_))
        + QChar(0) + extension_.toLower();
}
QString Request::Key() const { return Family() + QChar(0) + QString::number(pixels_) + QChar(0) + revision_; }
bool Request::Remote() const
{
    const auto Path = QDir::fromNativeSeparators(path_);
    if (remote_ || virtual_ || Path.startsWith("//?/UNC/", Qt::CaseInsensitive) || (Path.startsWith("//") && !Path.startsWith("//?/"))) return true;
    if (Path.size() >= 3 && Path[1] == ':') {
        const auto Root = QDir::toNativeSeparators(Path.left(3));
        return GetDriveTypeW(reinterpret_cast<LPCWSTR>(Root.utf16())) == DRIVE_REMOTE;
    }
    return false;
}
FileIconService::FileIconService(QObject* Parent, Loader Load, Clock Now)
    : QObject(Parent), loader_(Load ? std::move(Load) : Loader(ExtractNativeIcon)), now_(std::move(Now))
{
    age_.start(); if (!now_) now_ = [this] { return age_.elapsed(); };
    pool_.setMaxThreadCount(2);
    pumpTimer_.setSingleShot(true); changedTimer_.setSingleShot(true); changedTimer_.setInterval(16); expiryTimer_.setSingleShot(true);
    connect(&pumpTimer_, &QTimer::timeout, this, [this] { pump(); });
    connect(&expiryTimer_, &QTimer::timeout, this, [this] { CheckExpired(); });
    expirySchedule_.setSingleShot(true); connect(&expirySchedule_, &QTimer::timeout, this, [this] { updateExpiry(); });
    systemTimer_.setSingleShot(true); systemTimer_.setInterval(200);
    connect(&systemTimer_, &QTimer::timeout, this, [this] { InvalidateSystem(); });
    connect(&changedTimer_, &QTimer::timeout, this, [this] {
        const auto Keys = std::exchange(changed_, {});
        const auto Owners = subscribers_.keys();
        for (auto* Owner : Owners) {
            const auto It = subscribers_.constFind(Owner); if (It == subscribers_.cend()) continue;
            bool Interested = Keys.contains(QString{}); for (const auto& Key : Keys) if (It->keys_.contains(Key)) { Interested = true; break; }
            if (Interested) { const auto Changed = It->changed_; Changed(); }
        }
        scheduleExpiry();
    });
    const auto ChangedPath = [this](const QString& Path) { Invalidate(dependencies_.value(Path), 500); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, ChangedPath);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, ChangedPath);
    if (qApp) qApp->installEventFilter(this);
}
FileIconService::~FileIconService()
{
    // Only application shutdown drains workers. Closing a tab merely unsubscribes.
    if (qApp) qApp->removeEventFilter(this);
    if (shellRegistration_) SHChangeNotifyDeregister(shellRegistration_);
    if (shellWindow_) { qApp->removeNativeEventFilter(this); delete shellWindow_; }
}
FileIconService& FileIconService::Instance()
{
    static QPointer<FileIconService> Service;
    if (!Service) { Service = new FileIconService(qApp); Service->setObjectName("hc.fileIconService"); Service->enableNativeNotifications(); }
    return *Service;
}
void FileIconService::Subscribe(QObject* Owner, std::function<void()> Changed)
{
    if (!subscribers_.contains(Owner)) connect(Owner, &QObject::destroyed, this, [this, Owner] { Unsubscribe(Owner); });
    subscribers_[Owner].changed_ = std::move(Changed);
}
void FileIconService::Unsubscribe(QObject* Owner) { subscribers_.remove(Owner); scheduleExpiry(); }
void FileIconService::BeginRequests(QObject* Owner) { if (subscribers_.contains(Owner)) subscribers_[Owner].keys_.clear(); }
void FileIconService::SetAutomaticRefresh(QObject* Owner, bool Enabled)
{
    if (subscribers_.contains(Owner)) subscribers_[Owner].automatic_ = Enabled;
    scheduleExpiry();
}
Value FileIconService::Read(const Request& Query, QObject* Owner, bool Explicit)
{
    const auto Key = Query.Key();
    if (subscribers_.contains(Owner)) subscribers_[Owner].keys_.insert(Key);
    auto* Entry = cache_.object(Key);
    if (Entry && Entry->expires_ > now_()) { ++stats_.hits_; scheduleExpiry(); return Entry->value_; }
    const auto Previous = Entry ? Entry->value_ : Value{};
    if (Entry && !Entry->force_ && !Explicit && subscribers_.contains(Owner) && !subscribers_[Owner].automatic_) return Previous;
    enqueue(Query); scheduleExpiry(); return Previous;
}
QPixmap FileIconService::DefaultIcon(bool Directory, int Pixels, bool Explorer, QObject* Owner)
{
    const auto Key = QString::number(Directory) + ':' + QString::number(Pixels);
    auto* Fallback = defaults_.object(Key);
    if (!Fallback) {
        auto Pixmap = QApplication::style()->standardIcon(Directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon)
            .pixmap(QSize(Pixels, Pixels), 1.0).scaled(Pixels, Pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        defaults_.insert(Key, new QPixmap(Pixmap), std::max(1, Pixels * Pixels * 4 / 1024)); Fallback = defaults_.object(Key);
    }
    const auto Base = Fallback ? *Fallback : QPixmap{};
    if (!Explorer) return Base;
    Request Query; Query.kind_ = Directory ? Kind::Folder : Kind::File; Query.pixels_ = Pixels;
    const auto Value = Read(Query, Owner); return Value.pixmap_.isNull() ? Base : Value.pixmap_;
}
QPixmap FileIconService::Compose(const QPixmap& Base, const Value& Overlay)
{
    if (Base.isNull() || Overlay.pixmap_.isNull() || Overlay.state_ == State::NoOverlay) return Base;
    const auto Key = QString::number(Base.cacheKey()) + ':' + QString::number(Overlay.pixmap_.cacheKey());
    if (const auto* Found = composed_.object(Key)) return *Found;
    auto Pixmap = Base.copy(); QPainter Painter(&Pixmap); Painter.drawPixmap(0, 0, Overlay.pixmap_); Painter.end();
    composed_.insert(Key, new QPixmap(Pixmap), std::max(1, Pixmap.width() * Pixmap.height() * 4 / 1024));
    return Pixmap;
}
void FileIconService::enqueue(const Request& Query)
{
    const auto Key = Query.Key();
    if (pending_.contains(Key)) { ++stats_.merged_; return; }
    if (pending_.size() >= 1024) { queueBudgetHit_ = true; return; }
    Job Next{Query, ++serial_};
    Next.query_.knownCustomFolder_ = customFolders_.contains(Query.path_);
    pending_.insert(Key, Next); queue_.append(Key);
    stats_.maxPending_ = std::max(stats_.maxPending_, int(pending_.size()));
    if (!pumpTimer_.isActive()) pumpTimer_.start(0);
}
void FileIconService::pump()
{
    qint64 Delay = 1000;
    for (int At = 0; running_ < 2 && At < queue_.size();) {
        const auto Key = queue_[At]; const auto It = pending_.constFind(Key);
        if (It == pending_.cend()) { queue_.removeAt(At); continue; }
        const auto Next = *It;
        bool Needed = subscribers_.isEmpty();
        for (const auto& Owner : subscribers_) if (Owner.keys_.contains(Key)) { Needed = true; break; }
        if (!Needed) { pending_.remove(Key); queue_.removeAt(At); continue; }
        const auto* Cached = cache_.object(Key);
        if (Cached && Cached->defer_ > now_()) { Delay = std::min(Delay, Cached->defer_ - now_()); ++At; continue; }
        // Reserve a worker for local work when a disconnected share blocks in Shell.
        if (Next.query_.Remote() && remoteRunning_) { ++At; continue; }
        queue_.removeAt(At); ++running_; if (Next.query_.Remote()) ++remoteRunning_;
        ++stats_.loads_;
        using TimedResult = std::pair<Result, qint64>;
        auto* Watcher = new QFutureWatcher<TimedResult>(this);
        connect(Watcher, &QFutureWatcher<TimedResult>::finished, this, [this, Watcher, Next] {
            --running_; if (Next.query_.Remote()) --remoteRunning_;
            auto Output = Watcher->result(); Watcher->deleteLater(); publish(Next, std::move(Output.first), Output.second); pump();
        });
        const auto Load = loader_;
        Watcher->setFuture(QtConcurrent::run(&pool_, [Load, Next] {
            QElapsedTimer Timer; Timer.start(); Result Output;
            try { Output = Load(Next.query_); } catch (...) { Output.state_ = State::TransientFailure; }
            if (!Output.image_.isNull()) Output.digest_ = QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char*>(Output.image_.constBits()), Output.image_.sizeInBytes()), QCryptographicHash::Sha256);
            return TimedResult{std::move(Output), Timer.elapsed()};
        }));
    }
    if (!queue_.isEmpty() && running_ < 2) pumpTimer_.start(int(std::max<qint64>(1, Delay)));
    if (queueBudgetHit_ && pending_.size() < 1024) {
        queueBudgetHit_ = false;
        for (const auto& Owner : subscribers_) if (!Owner.keys_.isEmpty()) notify(*Owner.keys_.cbegin());
    }
}
void FileIconService::publish(const Job& Work, Result Output, qint64 Elapsed)
{
    const auto Key = Work.query_.Key(); const auto Pending = pending_.constFind(Key);
    if (Pending == pending_.cend() || Pending->serial_ != Work.serial_) { ++stats_.discarded_; return; }
    pending_.remove(Key); stats_.loadMs_ += Elapsed;
    if (Output.customFolder_) customFolders_.insert(Work.query_.path_, new bool(true));
    QElapsedTimer Apply; Apply.start();
    const auto* Previous = cache_.object(Key);
    auto* Entry = new Cached{Work.query_, {}, 0, 0, Previous ? Previous->attempts_ : 0};
    qint64 Lifetime = Work.query_.kind_ == Kind::Overlay || Work.query_.virtual_ ? 15000
        : Work.query_.kind_ == Kind::Item ? 60000 : 30 * 60 * 1000;
    Entry->value_.state_ = Output.state_;
    if (Output.state_ == State::TransientFailure) {
        if (Previous) Entry->value_.pixmap_ = Previous->value_.pixmap_;
        if (Previous) Entry->digest_ = Previous->digest_;
        constexpr int Retry[]{250, 750, 2000, 30000}; Lifetime = Retry[std::min(Entry->attempts_++, 3)];
    } else if (Output.state_ == State::NoEmbeddedIcon || Output.state_ == State::NoCustomIcon) {
        constexpr int Retry[]{2000, 5000, 15000, 30000}; Lifetime = Retry[std::min(Entry->attempts_++, 3)];
    } else {
        Entry->attempts_ = 0;
        if (!Output.image_.isNull()) {
            Entry->digest_ = Output.digest_;
            if (Previous && Previous->digest_ == Output.digest_) Entry->value_.pixmap_ = Previous->value_.pixmap_;
            else {
                const auto RasterKey = "raster:" + QString::fromLatin1(Output.digest_.toHex());
                if (const auto* Shared = composed_.object(RasterKey)) Entry->value_.pixmap_ = *Shared;
                else {
                    Entry->value_.pixmap_ = QPixmap::fromImage(Output.image_); ++stats_.conversions_;
                    composed_.insert(RasterKey, new QPixmap(Entry->value_.pixmap_), std::max(1, Output.image_.width() * Output.image_.height() * 4 / 1024));
                }
            }
        }
    }
    Entry->expires_ = now_() + Lifetime;
    const auto& Pixmap = Entry->value_.pixmap_;
    const int Cost = std::max(1, Pixmap.width() * Pixmap.height() * 4 / 1024);
    cache_.insert(Key, Entry, Cost);
    watch(Key, Output.dependencies_); notify(Key);
    stats_.maxApplyUs_ = std::max(stats_.maxApplyUs_, Apply.nsecsElapsed() / 1000);
    scheduleExpiry();
}
void FileIconService::Invalidate(const QSet<QString>& Keys, int SettleMs)
{
    QSet<QString> Families;
    for (const auto& Key : Keys) Families.insert(Key.section(QChar(0), 0, 2));
    auto Expanded = Keys;
    for (const auto& Key : cache_.keys()) if (Families.contains(cache_.object(Key)->query_.Family())) Expanded.insert(Key);
    for (auto It = pending_.cbegin(); It != pending_.cend(); ++It) if (Families.contains(It->query_.Family())) Expanded.insert(It.key());
    for (const auto& Key : Expanded) {
        if (auto* Entry = cache_.object(Key)) {
            Entry->expires_ = 0; Entry->defer_ = now_() + SettleMs; Entry->attempts_ = 0;
            Entry->force_ = SettleMs == 0;
        }
        pending_.remove(Key); queue_.removeAll(Key); notify(Key);
    }
    scheduleExpiry();
}
void FileIconService::InvalidatePaths(const QSet<QString>& Paths, int SettleMs)
{
    QSet<QString> Keys;
    for (const auto& Key : cache_.keys()) if (Paths.contains(cache_.object(Key)->query_.path_)) Keys.insert(Key);
    for (auto It = pending_.cbegin(); It != pending_.cend(); ++It) if (Paths.contains(It->query_.path_)) Keys.insert(It.key());
    Invalidate(Keys, SettleMs);
}
void FileIconService::InvalidateSystem()
{
    QSet<QString> Keys; for (const auto& Key : cache_.keys()) Keys.insert(Key);
    for (auto It = pending_.cbegin(); It != pending_.cend(); ++It) Keys.insert(It.key());
    defaults_.clear(); composed_.clear(); Invalidate(Keys);
}
void FileIconService::CheckExpired()
{
    QSet<QString> Keys; for (const auto& Owner : subscribers_) if (Owner.automatic_) Keys.unite(Owner.keys_);
    for (const auto& Key : Keys) if (const auto* Entry = cache_.object(Key); Entry && Entry->expires_ <= now_()) enqueue(Entry->query_);
    scheduleExpiry();
}
void FileIconService::scheduleExpiry()
{
    if (!expirySchedule_.isActive()) expirySchedule_.start(0);
}
void FileIconService::updateExpiry()
{
    qint64 Due = now_() + 60000; bool Any = false;
    for (const auto& Owner : subscribers_) if (Owner.automatic_) for (const auto& Key : Owner.keys_) {
        if (pending_.contains(Key)) continue;
        if (const auto* Entry = cache_.object(Key)) { Due = std::min(Due, std::max(Entry->expires_, Entry->defer_)); Any = true; }
    }
    if (Any) expiryTimer_.start(int(std::clamp<qint64>(Due - now_(), 1, 60000))); else expiryTimer_.stop();
}
void FileIconService::notify(const QString& Key)
{
    changed_.insert(Key); if (!changedTimer_.isActive()) changedTimer_.start();
}
void FileIconService::watch(const QString& Key, const QStringList& Paths)
{
    // Bounded local dependencies cover desktop.ini, external resources and atomic replacement.
    QSet<QString> Wanted;
    for (const auto& Path : Paths) { Wanted.insert(Path); Wanted.insert(QFileInfo(Path).absolutePath()); }
    for (auto It = dependencies_.begin(); It != dependencies_.end();) {
        if (!Wanted.contains(It.key())) It->remove(Key);
        if (It->isEmpty()) { watcher_.removePath(It.key()); It = dependencies_.erase(It); } else ++It;
    }
    if (now_() - dependencyPrune_ >= 1000) {
        dependencyPrune_ = now_();
        QSet<QString> Interested; for (const auto& Owner : subscribers_) Interested.unite(Owner.keys_);
        for (auto It = dependencies_.begin(); It != dependencies_.end();) {
            for (auto KeyIt = It->begin(); KeyIt != It->end();) {
                if (!cache_.contains(*KeyIt) || !Interested.contains(*KeyIt)) KeyIt = It->erase(KeyIt); else ++KeyIt;
            }
            if (It->isEmpty()) { watcher_.removePath(It.key()); It = dependencies_.erase(It); } else ++It;
        }
    }
    for (const auto& Source : Paths) for (const auto& Path : {Source, QFileInfo(Source).absolutePath()}) {
        Request Query; Query.path_ = Path;
        if (Query.Remote()) continue;
        if (dependencies_.contains(Path)) {
            dependencies_[Path].insert(Key);
            if (!watcher_.files().contains(Path) && !watcher_.directories().contains(Path) && QFileInfo::exists(Path)) watcher_.addPath(Path);
            continue;
        }
        if (dependencies_.size() >= 128 || !QFileInfo::exists(Path)) continue;
        if (watcher_.addPath(Path)) dependencies_[Path].insert(Key);
    }
}
bool FileIconService::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == qApp && (Event->type() == QEvent::ApplicationPaletteChange || Event->type() == QEvent::StyleChange)) {
        defaults_.clear(); composed_.clear(); notify(QString{});
    }
    return QObject::eventFilter(Watched, Event);
}
void FileIconService::enableNativeNotifications()
{
    shellWindow_ = new QWindow; shellWindow_->setFlags(Qt::Tool); shellWindow_->create();
    qApp->installNativeEventFilter(this);
    PIDLIST_ABSOLUTE Desktop = nullptr;
    if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &Desktop))) {
        const SHChangeNotifyEntry Entry{Desktop, TRUE};
        shellRegistration_ = SHChangeNotifyRegister(reinterpret_cast<HWND>(shellWindow_->winId()), SHCNRF_ShellLevel | SHCNRF_NewDelivery,
            SHCNE_ASSOCCHANGED, WM_APP + 171, 1, &Entry);
        CoTaskMemFree(Desktop);
    }
}
bool FileIconService::nativeEventFilter(const QByteArray&, void* Message, qintptr* Result)
{
    const auto* Msg = static_cast<MSG*>(Message);
    if (shellWindow_ && Msg->hwnd == reinterpret_cast<HWND>(shellWindow_->winId()) && Msg->message == WM_APP + 171) {
        PIDLIST_ABSOLUTE* Items = nullptr; long Event = 0;
        const auto Lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(Msg->wParam), DWORD(Msg->lParam), &Items, &Event);
        if (Lock) { SHChangeNotification_Unlock(Lock); if (!systemTimer_.isActive()) systemTimer_.start(); }
        if (Result) *Result = 0; return true;
    }
    if (Msg->message == WM_SETTINGCHANGE || Msg->message == WM_SYSCOLORCHANGE || Msg->message == WM_THEMECHANGED)
        if (!systemTimer_.isActive()) systemTimer_.start();
    return false;
}
}
