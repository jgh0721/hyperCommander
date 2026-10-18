#include "core/ExternalChangeWatcher.h"

#include "core/AppSettings.h"
#include <chrono>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

namespace mrst {

namespace {

constexpr auto kActionKey    = "textView/externalChangeAction";
constexpr auto kDetectionKey = "textView/externalChangeDetection";
constexpr auto kPollKey      = "textView/externalChangePollSeconds";

constexpr int kDefaultPollSeconds = 3;
constexpr int kMinPollSeconds     = 1;
constexpr int kMaxPollSeconds     = 3600;

/// 알림 한 번과 그것을 확인하는 사이의 간격. 짧으면 반쯤 쓰인 파일을 보고,
/// 길면 사용자가 기다린다. 250ms 는 사람이 즉시로 느끼는 상한(~300ms) 안쪽이면서,
/// 임시 파일 + rename 저장이 끝나기에는 넉넉한 값이다.
constexpr int kDefaultSettleMs = 250;

/// 자기 쓰기 구간의 최대 수명. 저장 완료 신호를 못 받는 경로가 남아 있어도
/// 감시가 그 파일을 영원히 놓지 않도록 못을 박아 둔다.
constexpr qint64 kSelfWriteMaxMs = 30 * 1000;

} // namespace

ExternalChangeWatcher::ExternalChangeWatcher( QObject* parent )
    : QObject( parent )
{
    settleTimer_ = new QTimer( this );
    settleTimer_->setSingleShot( true );
    settleTimer_->setInterval( kDefaultSettleMs );
    connect( settleTimer_, &QTimer::timeout, this, &ExternalChangeWatcher::onSettleTick );

    // 폴링 타이머는 만들어만 두고 필요할 때 돌린다. Notify 모드에서도 알림을
    // 걸지 못한 항목(pollFallback)이 생기면 이것이 유일한 그물이 된다.
    pollTimer_ = new QTimer( this );
    pollTimer_->setSingleShot( false );
    connect( pollTimer_, &QTimer::timeout, this, &ExternalChangeWatcher::onPollTick );

    action_      = configuredAction();
    detection_   = configuredDetection();
    pollSeconds_ = configuredPollSeconds();
}

ExternalChangeWatcher::~ExternalChangeWatcher() = default;

// ═══════════════════════════════════════════════════════════
// 설정
// ═══════════════════════════════════════════════════════════
ExternalChangeWatcher::Action ExternalChangeWatcher::configuredAction()
{
    AppSettings settings;
    const int raw = settings.value( QString::fromLatin1( kActionKey ),
                                    static_cast< int >( Action::Reload ) ).toInt();
    switch( raw )
    {
        case static_cast< int >( Action::Ignore ): return Action::Ignore;
        case static_cast< int >( Action::Ask ):    return Action::Ask;
        default:                                   return Action::Reload;
    }
}

ExternalChangeWatcher::Detection ExternalChangeWatcher::configuredDetection()
{
    AppSettings settings;
    const int raw = settings.value( QString::fromLatin1( kDetectionKey ),
                                    static_cast< int >( Detection::Notify ) ).toInt();
    return raw == static_cast< int >( Detection::Poll ) ? Detection::Poll : Detection::Notify;
}

int ExternalChangeWatcher::configuredPollSeconds()
{
    AppSettings settings;
    return qBound( kMinPollSeconds,
                   settings.value( QString::fromLatin1( kPollKey ), kDefaultPollSeconds ).toInt(),
                   kMaxPollSeconds );
}

int ExternalChangeWatcher::defaultPollSeconds() { return kDefaultPollSeconds; }
int ExternalChangeWatcher::minPollSeconds()     { return kMinPollSeconds; }
int ExternalChangeWatcher::maxPollSeconds()     { return kMaxPollSeconds; }

ExternalChangeWatcher::Fingerprint ExternalChangeWatcher::fingerprintOf( const QString& filePath )
{
    Fingerprint fingerprint;
    if( filePath.trimmed().isEmpty() )
        return fingerprint;

    // QFileInfo 는 값을 캐시한다. 매번 새로 만들어야 방금 바뀐 것이 보인다.
    const QFileInfo info( filePath );
    if( !info.exists() || !info.isFile() )
        return fingerprint;

    fingerprint.exists            = true;
    fingerprint.size              = info.size();
    fingerprint.lastModifiedUtcMs = info.lastModified().toUTC().toMSecsSinceEpoch();
    return fingerprint;
}

void ExternalChangeWatcher::reloadSettings()
{
    const bool wasEnabled = isEnabled();

    action_      = configuredAction();
    detection_   = configuredDetection();
    pollSeconds_ = configuredPollSeconds();

    if( !wasEnabled && isEnabled() )
        rebaselineAll();
    applyDetection();
}

void ExternalChangeWatcher::setAction( Action action )
{
    if( action_ == action )
        return;

    const bool wasEnabled = isEnabled();
    action_ = action;
    if( !wasEnabled && isEnabled() )
        rebaselineAll();
    applyDetection();
}

void ExternalChangeWatcher::setDetection( Detection detection )
{
    if( detection_ == detection )
        return;

    detection_ = detection;
    applyDetection();
}

void ExternalChangeWatcher::setPollSeconds( int seconds )
{
    const int bounded = qBound( kMinPollSeconds, seconds, kMaxPollSeconds );
    if( pollSeconds_ == bounded )
        return;

    pollSeconds_ = bounded;
    updatePollTimerState();
}

void ExternalChangeWatcher::setSettleInterval( int milliseconds )
{
    settleMs_ = qMax( 1, milliseconds );
    settleTimer_->setInterval( settleMs_ );
}

// ═══════════════════════════════════════════════════════════
// 감시 대상
// ═══════════════════════════════════════════════════════════
QString ExternalChangeWatcher::keyFor( const QString& filePath )
{
    const QString absolute = QDir::cleanPath( QFileInfo( filePath ).absoluteFilePath() );
#ifdef Q_OS_WIN
    return absolute.toLower();
#else
    return absolute;
#endif
}

QStringList ExternalChangeWatcher::watchedFiles() const
{
    QStringList paths;
    paths.reserve( entries_.size() );
    for( const Entry& entry : entries_ )
        paths.append( entry.path );
    paths.sort();
    return paths;
}

void ExternalChangeWatcher::setWatchedFiles( const QStringList& filePaths )
{
    QHash< QString, QString > wanted;       // key -> 절대 경로
    for( const QString& path : filePaths )
    {
        if( path.trimmed().isEmpty() )
            continue;
        wanted.insert( keyFor( path ), QDir::cleanPath( QFileInfo( path ).absoluteFilePath() ) );
    }

    for( auto it = entries_.begin(); it != entries_.end(); )
    {
        if( wanted.contains( it.key() ) )
        {
            ++it;
            continue;
        }

        detachOsWatch( it.value() );
        pending_.remove( it.key() );
        it = entries_.erase( it );
    }

    for( auto it = wanted.constBegin(); it != wanted.constEnd(); ++it )
    {
        if( entries_.contains( it.key() ) )
            continue;

        Entry entry;
        entry.path     = it.value();
        entry.baseline = fingerprintOf( entry.path );
        entries_.insert( it.key(), entry );
        if( isEnabled() && detection_ == Detection::Notify )
            attachOsWatch( entries_[ it.key() ] );

    }

    updatePollTimerState();
}

void ExternalChangeWatcher::markSynchronized( const QString& filePath )
{
    const QString key = keyFor( filePath );
    auto it = entries_.find( key );
    if( it == entries_.end() )
        return;

    it->baseline = fingerprintOf( it->path );
    it->probing  = false;
    pending_.remove( key );
    // 지우고 다시 만드는 저장을 지나오면 OS 알림이 풀려 있다.
    if( isEnabled() && detection_ == Detection::Notify )
        attachOsWatch( *it );
}

bool ExternalChangeWatcher::consumeSelfWrite( Entry& entry )
{
    if( entry.selfWrites <= 0 )
        return false;

    if( std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() <= entry.selfWriteDeadlineMs )
        return true;

    entry.selfWrites = 0;       // 짝이 어긋났다. 감시를 되살린다
    return false;
}

void ExternalChangeWatcher::beginSelfWrite( const QString& filePath )
{
    auto it = entries_.find( keyFor( filePath ) );
    if( it == entries_.end() )
        return;

    ++it->selfWrites;
    it->selfWriteDeadlineMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + kSelfWriteMaxMs;
}

void ExternalChangeWatcher::endSelfWrite( const QString& filePath, bool synchronized )
{
    const QString key = keyFor( filePath );
    auto it = entries_.find( key );
    if( it == entries_.end() )
        return;

    if( it->selfWrites > 0 )
        --it->selfWrites;
    if( it->selfWrites == 0 ) {
        if( synchronized ) markSynchronized( it->path );
        else scheduleProbe( key ); // Failed/conflicting saves must not hide another writer's change.
    }
}

void ExternalChangeWatcher::recheckAll()
{
    if( !isEnabled() || entries_.isEmpty() )
        return;

    for( auto it = entries_.constBegin(); it != entries_.constEnd(); ++it )
        scheduleProbe( it.key() );
}

// ═══════════════════════════════════════════════════════════
// 발견
// ═══════════════════════════════════════════════════════════
void ExternalChangeWatcher::onWatchedFileChanged( const QString& filePath )
{
    scheduleProbe( keyFor( filePath ) );
}

void ExternalChangeWatcher::onWatchedDirectoryChanged( const QString& dirPath )
{
    // 디렉터리 알림은 그 안에서 무언가 일어났다는 것까지만 알려 준다. 어느
    // 파일인지는 말해 주지 않으므로 그 디렉터리에 속한 항목을 모두 확인한다.
    // (그 디렉터리를 보는 이유는 파일 감시가 삭제로 풀리는 것 때문이다 —
    // 다른 편집기의 임시 파일 + rename 저장이 정확히 그 경로다.)
    const QString dirKey = keyFor( dirPath );
    for( auto it = entries_.constBegin(); it != entries_.constEnd(); ++it )
    {
        if( keyFor( QFileInfo( it.value().path ).absolutePath() ) == dirKey )
            scheduleProbe( it.key() );
    }
}

void ExternalChangeWatcher::scheduleProbe( const QString& key )
{
    if( !isEnabled() || !entries_.contains( key ) )
        return;

    pending_.insert( key );

    // 이미 돌고 있으면 다시 시작하지 않는다. 계속 덧붙여 쓰이는 파일(빌드 로그
    // 등)에서 타이머를 매번 되돌리면 확인이 영원히 밀린다.
    if( !settleTimer_->isActive() )
        settleTimer_->start();
}

void ExternalChangeWatcher::onSettleTick()
{
    if( inSettleTick_ )
        return;         // 신호를 받은 쪽의 모달 이벤트 루프에서 재진입했다

    inSettleTick_ = true;

    const QSet< QString > checking = pending_;
    pending_.clear();

    QStringList changed;
    QStringList vanished;

    for( const QString& key : checking )
    {
        auto it = entries_.find( key );
        if( it == entries_.end() )
            continue;

        Entry& entry = *it;
        if( consumeSelfWrite( entry ) )
            continue;                       // 우리가 쓰는 중이다. 알림을 버린다

        const Fingerprint now = fingerprintOf( entry.path );
        if( now == entry.baseline )
        {
            entry.probing = false;          // 우리가 아는 상태로 돌아왔다
            continue;
        }

        if( !entry.probing || now != entry.probe )
        {
            // 처음 본 변화이거나 아직 값이 움직이는 중이다. 한 번 더 잰다.
            entry.probing = true;
            entry.probe   = now;
            pending_.insert( key );
            continue;
        }

        // 간격을 두고 두 번 같은 값이 나왔다. 쓰기가 끝났다고 본다.
        entry.probing  = false;
        entry.baseline = now;
        if( isEnabled() && detection_ == Detection::Notify )
            attachOsWatch( entry );

        if( now.exists )
            changed.append( entry.path );
        else
            vanished.append( entry.path );
    }

    // 신호는 상태를 모두 정리한 뒤에 낸다. 받는 쪽이 setWatchedFiles() 로
    // 되돌아오면 위에서 잡은 Entry& 가 무효가 된다.
    for( const QString& path : changed )
    {

        emit sigFileChanged( path );
    }
    for( const QString& path : vanished )
    {

        emit sigFileVanished( path );
    }

    inSettleTick_ = false;

    if( !pending_.isEmpty() && !settleTimer_->isActive() )
        settleTimer_->start();
}

void ExternalChangeWatcher::onPollTick()
{
    if( !isEnabled() )
        return;

    const bool pollEverything = detection_ == Detection::Poll;
    for( auto it = entries_.begin(); it != entries_.end(); ++it )
    {
        if( !pollEverything && !it->pollFallback )
            continue;
        if( consumeSelfWrite( *it ) )
            continue;
        if( fingerprintOf( it->path ) != it->baseline )
            scheduleProbe( it.key() );
    }
}

// ═══════════════════════════════════════════════════════════
// OS 알림 / 폴링 전환
// ═══════════════════════════════════════════════════════════
void ExternalChangeWatcher::rebaselineAll()
{
    for( auto it = entries_.begin(); it != entries_.end(); ++it )
    {
        it->baseline = fingerprintOf( it->path );
        it->probing  = false;
    }
    pending_.clear();
}

void ExternalChangeWatcher::applyDetection()
{
    const bool useOsWatch = isEnabled() && detection_ == Detection::Notify;

    if( !useOsWatch )
    {
        for( auto it = entries_.begin(); it != entries_.end(); ++it )
        {
            detachOsWatch( *it );
            it->pollFallback = false;
        }
        if( !isEnabled() )
        {
            pending_.clear();
            settleTimer_->stop();
        }
    }
    else
    {
        for( auto it = entries_.begin(); it != entries_.end(); ++it )
            attachOsWatch( *it );
    }

    updatePollTimerState();
}

void ExternalChangeWatcher::attachOsWatch( Entry& entry )
{
    if( osWatcher_ == nullptr )
    {
        osWatcher_ = new QFileSystemWatcher( this );
        connect( osWatcher_, &QFileSystemWatcher::fileChanged,
                 this, &ExternalChangeWatcher::onWatchedFileChanged );
        connect( osWatcher_, &QFileSystemWatcher::directoryChanged,
                 this, &ExternalChangeWatcher::onWatchedDirectoryChanged );
    }

    // 파일이 지워지면 Qt 가 감시 목록에서 스스로 뺀다(qfilesystemwatcher.cpp).
    // 그래서 이미 걸었다는 것을 플래그로 들고 있으면 안 되고 매번 물어봐야 한다.
    const bool fileWatched = osWatcher_->files().contains( entry.path );
    if( fileWatched )
        entry.pollFallback = false;
    else if( QFileInfo::exists( entry.path ) )
        entry.pollFallback = !osWatcher_->addPath( entry.path );
    else
        entry.pollFallback = true;      // 아직 없는 파일. 디렉터리 알림으로 본다

    if( entry.pollFallback )


    // 파일 감시가 풀리는 순간을 알려 주는 것은 디렉터리 감시뿐이다.
    if( !entry.holdsDirRef )
    {
        retainDirectory( QFileInfo( entry.path ).absolutePath() );
        entry.holdsDirRef = true;
    }
    updatePollTimerState();
}

void ExternalChangeWatcher::detachOsWatch( Entry& entry )
{
    if( osWatcher_ == nullptr )
        return;

    if( osWatcher_->files().contains( entry.path ) )
        osWatcher_->removePath( entry.path );
    if( entry.holdsDirRef )
    {
        releaseDirectory( QFileInfo( entry.path ).absolutePath() );
        entry.holdsDirRef = false;
    }
}

void ExternalChangeWatcher::retainDirectory( const QString& dirPath )
{
    if( osWatcher_ == nullptr || dirPath.trimmed().isEmpty() )
        return;

    const QString dirKey = keyFor( dirPath );
    const int refs = dirRefs_.value( dirKey, 0 );
    if( refs == 0 && !osWatcher_->addPath( dirPath ) )
        return;                         // 디렉터리를 못 걸었다. 참조도 세지 않는다

    dirRefs_.insert( dirKey, refs + 1 );
}

void ExternalChangeWatcher::releaseDirectory( const QString& dirPath )
{
    if( osWatcher_ == nullptr || dirPath.trimmed().isEmpty() )
        return;

    const QString dirKey = keyFor( dirPath );
    const int refs = dirRefs_.value( dirKey, 0 );
    if( refs <= 0 )
        return;

    if( refs == 1 )
    {
        dirRefs_.remove( dirKey );
        osWatcher_->removePath( dirPath );
    }
    else
    {
        dirRefs_.insert( dirKey, refs - 1 );
    }
}

void ExternalChangeWatcher::updatePollTimerState()
{
    bool needsPolling = false;
    if( isEnabled() )
    {
        if( detection_ == Detection::Poll )
        {
            needsPolling = !entries_.isEmpty();
        }
        else
        {
            for( auto it = entries_.constBegin(); it != entries_.constEnd(); ++it )
            {
                if( it.value().pollFallback )
                {
                    needsPolling = true;
                    break;
                }
            }
        }
    }

    if( !needsPolling )
    {
        pollTimer_->stop();
        return;
    }

    const int intervalMs = pollSeconds_ * 1000;
    if( pollTimer_->interval() != intervalMs )
        pollTimer_->setInterval( intervalMs );
    if( !pollTimer_->isActive() )
        pollTimer_->start();
}

} // namespace mrst
