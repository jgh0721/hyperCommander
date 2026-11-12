#include "stdafx.h"
#include "rstPathIndex.hpp"

/// 모듈
#include "rstFileKinds.hpp"
#include "rstBackgroundWork.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QThreadPool>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

struct PathIndexPublicationState
{
    std::mutex                    mutex;
    std::deque< QStringList >     pendingBatches;
    qsizetype                     pendingFileCount = 0;
    qsizetype                     discoveredFileCount = 0;
    bool                          drainScheduled = false;
};

namespace {

/// 저장할 때마다 전 트리를 다시 훑지 않도록 하는 최소 간격.
constexpr qint64 kRescanThrottleMs = 10'000;
/// 결과가 아주 많아도 GUI 이벤트 큐와 메모리를 한 항목씩 소모하지 않도록 묶는다.
constexpr qsizetype kBatchSize = 200;
/// 한 번의 GUI 콜백이 처리할 최대 항목 수. 입력 이벤트에 제어권을 자주 돌려준다.
constexpr qsizetype kMaxFilesPerProgressDelivery = 1'000;
/// GUI가 소비하는 동안 워커가 추가로 쌓아 둘 진행 항목 수. 초과분은 최종
/// snapshot이 대신하므로 전체 인덱스 결과에는 영향을 주지 않는다.
constexpr qsizetype kMaxPendingProgressFiles = kMaxFilesPerProgressDelivery;
/// 연속 진행 알림은 20 Hz 이하로 제한한다.
constexpr auto kProgressDeliveryInterval = std::chrono::milliseconds{ 50 };

struct ScanResult
{
    QStringList paths;
    bool        cancelled = false;
};

QThreadPool& pathIndexRetirementPool()
{
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount( 1 );
        pool.setExpiryTimeout( 30'000 );
        pool.setThreadPriority( QThread::LowestPriority );
        return true;
    }();
    Q_UNUSED( configured );
    return pool;
}

void retirePathChunks( PathIndexChunks chunks )
{
    if( chunks.isEmpty() )
        return;

    pathIndexRetirementPool().start(
        [ chunks = std::move( chunks ) ]() mutable { chunks.clear(); } );
}

void retirePathList( QStringList paths )
{
    if( paths.isEmpty() )
        return;

    pathIndexRetirementPool().start(
        [ paths = std::move( paths ) ]() mutable { paths.clear(); } );
}

void retirePathBatches( std::deque< QStringList > batches )
{
    if( batches.empty() )
        return;
    pathIndexRetirementPool().start(
        [ batches = std::move( batches ) ]() mutable { batches.clear(); } );
}

template< typename ShouldCancel >
bool cancellablePathSort( QStringList& paths, ShouldCancel shouldCancel )
{
    constexpr qsizetype kRunSize = 2'048;
    constexpr qsizetype kCancelCheckStride = 1'024;
    const auto less = []( const QString& left, const QString& right ) {
        const int folded = QString::compare( left, right, Qt::CaseInsensitive );
        return folded != 0 ? folded < 0 : QString::compare( left, right, Qt::CaseSensitive ) < 0;
    };

    if( shouldCancel() )
        return false;

    for( qsizetype start = 0; start < paths.size(); start += kRunSize )
    {
        if( shouldCancel() )
            return false;
        const qsizetype end = (std::min)( paths.size(), start + kRunSize );
        std::sort( paths.begin() + start, paths.begin() + end, less );
    }

    QStringList scratch;
    scratch.resize( paths.size() );
    bool sourceIsPaths = true;
    for( qsizetype width = kRunSize; width < paths.size(); )
    {
        const qsizetype pairWidth = width > paths.size() - width
                                        ? paths.size()
                                        : width * 2;
        QStringList& source = sourceIsPaths ? paths : scratch;
        QStringList& target = sourceIsPaths ? scratch : paths;

        for( qsizetype start = 0; start < paths.size(); start += pairWidth )
        {
            qsizetype left = start;
            const qsizetype middle = (std::min)( paths.size(), start + width );
            qsizetype right = middle;
            const qsizetype end = (std::min)( paths.size(), start + pairWidth );
            qsizetype output = start;

            while( left < middle || right < end )
            {
                if( ( output - start ) % kCancelCheckStride == 0 && shouldCancel() )
                    return false;
                if( right >= end || ( left < middle && less( source.at( left ),
                                                               source.at( right ) ) ) )
                    target[ output++ ] = source.at( left++ );
                else
                    target[ output++ ] = source.at( right++ );
            }
        }

        sourceIsPaths = !sourceIsPaths;
        if( pairWidth >= paths.size() )
            break;
        width = pairWidth;
    }

    if( !sourceIsPaths )
        paths.swap( scratch );
    return !shouldCancel();
}

template< typename BatchReady, typename ShouldCancel >
ScanResult scanPathIndexImpl( const QString& root, const int limit, BatchReady batchReady,
                              ShouldCancel shouldCancel )
{
    ScanResult result;
    if( shouldCancel() )
    {
        result.cancelled = true;
        return result;
    }

    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() || !QFileInfo( base ).isDir() )
        return result;

    const bool limited = limit > 0;
    const QDir baseDirectory( base );

    // QDirIterator::Subdirectories 를 쓰지 않는다. 그것은 다 훑은 뒤에 걸러내는
    // 방식이라 .git 과 .venv 안까지 전부 들어간다. 명시적인 스택으로
    // 가지치기를 하면 제외 디렉터리 자체에 들어가지 않는다.
    QStringList pending = { base };
    QStringList batch;
    batch.reserve( kBatchSize );

    while( !pending.isEmpty()
           && ( !limited || result.paths.size() < static_cast< qsizetype >( limit ) ) )
    {
        if( shouldCancel() )
        {
            result.paths.clear();
            result.cancelled = true;
            return result;
        }

        const QString directory = pending.takeLast();
        // Hidden 을 넣어야 Unix 의 dot 파일과 Windows 의 숨김 속성 파일도 나온다.
        // 이름이 점으로 시작한다는 이유만으로 버리지 않고, 디렉터리만 중앙 제외
        // 목록으로 가지치기한다.
        QDirIterator entries( directory,
                              QDir::Dirs | QDir::Files | QDir::Hidden
                                  | QDir::NoDotAndDotDot,
                              QDirIterator::NoIteratorFlags );

        while( entries.hasNext() )
        {
            if( shouldCancel() )
            {
                result.paths.clear();
                result.cancelled = true;
                return result;
            }

            entries.next();
            const QFileInfo entry = entries.fileInfo();
            const QString name = entry.fileName();
            // 링크를 따라 워크스페이스 밖의 파일을 빠른 열기 후보로 노출하거나
            // 부모를 가리키는 디렉터리 링크에서 순환하지 않게 모두 제외한다.
            // 선택 시에도 canonical 경계를 다시 확인해 스캔 뒤 교체를 막는다.
            if( entry.isSymLink() || entry.isJunction() )
                continue;

            if( entry.isDir() )
            {
                if( !filekinds::isExcludedDirectoryName( name ) )
                    pending << entry.absoluteFilePath();
                continue;
            }

            const QString relative =
                QDir::fromNativeSeparators( baseDirectory.relativeFilePath( entry.absoluteFilePath() ) );
            result.paths << relative;
            batch << relative;

            if( batch.size() >= kBatchSize )
            {
                batchReady( std::move( batch ) );
                batch.clear();
                batch.reserve( kBatchSize );
            }

            if( limited && result.paths.size() >= static_cast< qsizetype >( limit ) )
                break;
        }
    }

    if( !batch.isEmpty() )
        batchReady( std::move( batch ) );

    if( !cancellablePathSort( result.paths, shouldCancel ) )
    {
        result.paths.clear();
        result.cancelled = true;
    }
    return result;
}

}   // namespace

QStringList scanPathIndex( const QString& root, const int limit )
{
    ScanResult result = scanPathIndexImpl(
        root, limit, []( QStringList ) {}, [] { return isShuttingDown(); } );
    return result.cancelled ? QStringList{} : std::move( result.paths );
}

PathIndex::PathIndex( QObject* parent )
    : QObject( parent )
    , rescanTimer_( new QTimer( this ) )
{
    rescanTimer_->setObjectName( QStringLiteral( "pathIndexRescanTimer" ) );
    rescanTimer_->setSingleShot( true );
    connect( rescanTimer_, &QTimer::timeout, this, &PathIndex::runPendingInvalidation );
}

PathIndex::~PathIndex()
{
    if( scanCancellation_ )
        scanCancellation_->store( true, std::memory_order_relaxed );
}

bool PathIndex::isReadyFor( const QString& root ) const
{
    return !root.isEmpty() && indexedRoot_ == QDir::cleanPath( root );
}

bool PathIndex::isScanningFor( const QString& root ) const
{
    return !root.isEmpty() && scanningRoot_ == QDir::cleanPath( root );
}

void PathIndex::ensure( const QString& root )
{
    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() || indexedRoot_ == base || scanningRoot_ == base )
        return;
    startScan( base );
}

void PathIndex::invalidate( const QString& root )
{
    const QString base = QDir::cleanPath( root );
    if( base.isEmpty() )
        return;

    if( scanningRoot_ == base )
    {
        pendingInvalidationRoot_ = base;
        return;
    }

    if( indexedRoot_ == base && sinceLastScan_.isValid()
        && sinceLastScan_.elapsed() < kRescanThrottleMs )
    {
        pendingInvalidationRoot_ = base;
        const qint64 remaining = kRescanThrottleMs - sinceLastScan_.elapsed();
        rescanTimer_->start( static_cast< int >( (std::max)( qint64{ 1 }, remaining ) ) );
        return;
    }
    startScan( base );
}

void PathIndex::clear()
{
    if( scanCancellation_ )
        scanCancellation_->store( true, std::memory_order_relaxed );
    scanCancellation_.reset();

    ++generation_;
    rescanTimer_->stop();
    pendingInvalidationRoot_.clear();
    indexedRoot_.clear();
    scanningRoot_.clear();
    QStringList retiredPaths = std::move( paths_ );
    paths_.clear();
    PathIndexChunks retiredChunks = std::move( partialPathChunks_ );
    partialPathChunks_.clear();
    partialPathCount_ = 0;
    scannedPathCount_ = 0;
    retirePathChunks( std::move( retiredChunks ) );
    retirePathList( std::move( retiredPaths ) );
    sinceLastScan_.invalidate();
}

void PathIndex::startScan( const QString& root )
{
    if( scanCancellation_ )
        scanCancellation_->store( true, std::memory_order_relaxed );

    rescanTimer_->stop();
    pendingInvalidationRoot_.clear();
    scanningRoot_ = root;
    PathIndexChunks retiredChunks = std::move( partialPathChunks_ );
    partialPathChunks_.clear();
    partialPathCount_ = 0;
    scannedPathCount_ = 0;
    retirePathChunks( std::move( retiredChunks ) );
    const quint64 generation = ++generation_;
    const auto cancellation = std::make_shared< std::atomic_bool >( false );
    const auto publication = std::make_shared< PathIndexPublicationState >();
    scanCancellation_ = cancellation;
    QPointer< PathIndex > guard( this );
    QCoreApplication* const dispatcher = QCoreApplication::instance();
    emit scanStarted( root );

    if( dispatcher == nullptr )
    {
        applyCancelled( root, generation );
        return;
    }

    // 디스크 순회와 정렬은 워커에 남기고, 작은 배치와 완성 결과만 GUI 스레드로
    // 되돌린다. 부분 캐시는 GUI 스레드에서만 바뀌므로 UI가 잠금 없이 읽을 수 있다.
    QThreadPool::globalInstance()->start( [ guard, dispatcher, root, generation, cancellation,
                                            publication ] {
        const auto publishBatch = [ guard, dispatcher, root, generation, cancellation,
                                    publication ]( QStringList batch ) mutable {
            if( isShuttingDown() || cancellation->load( std::memory_order_relaxed ) )
                return;

            bool scheduleDrain = false;
            {
                const std::scoped_lock lock( publication->mutex );
                publication->discoveredFileCount += batch.size();
                if( publication->pendingFileCount + batch.size()
                    <= kMaxPendingProgressFiles )
                {
                    publication->pendingFileCount += batch.size();
                    publication->pendingBatches.push_back( std::move( batch ) );
                }
                if( !publication->pendingBatches.empty()
                    && !publication->drainScheduled )
                {
                    publication->drainScheduled = true;
                    scheduleDrain = true;
                }
            }

            if( scheduleDrain )
            {
                QMetaObject::invokeMethod(
                    dispatcher,
                    [ guard, root, generation, cancellation, publication ] {
                        if( guard.isNull() || isShuttingDown()
                            || cancellation->load( std::memory_order_relaxed ) )
                            return;
                        guard->drainPublishedBatches( root, generation, publication,
                                                      cancellation );
                    },
                    Qt::QueuedConnection );
            }
        };

        ScanResult result = scanPathIndexImpl(
            root, 0, publishBatch,
            [ cancellation ] {
                return isShuttingDown()
                       || cancellation->load( std::memory_order_relaxed );
            } );

        if( result.cancelled || isShuttingDown()
            || cancellation->load( std::memory_order_relaxed ) )
        {
            // 취소된 세대의 대기열은 워커에서 해제해 GUI 이벤트가 오래된 문자열
            // 수만 개를 파괴하느라 멈추지 않게 한다.
            {
                std::deque< QStringList > discarded;
                const std::scoped_lock lock( publication->mutex );
                discarded.swap( publication->pendingBatches );
                publication->pendingFileCount = 0;
                publication->drainScheduled = false;
            }

            // 실제 종료 중에는 QCoreApplication 해체 경계로 새 이벤트를 보내지
            // 않는다. 사용자가 종료를 취소하면 MainWindow가 clear+ensure로 새
            // 세대를 시작한다. 일반 세대 취소일 때만 GUI 상태 전이를 게시한다.
            if( isShuttingDown() )
                return;
            QMetaObject::invokeMethod(
                dispatcher,
                [ guard, root, generation ] {
                    if( !guard.isNull() )
                        guard->applyCancelled( root, generation );
                },
                Qt::QueuedConnection );
            return;
        }

        QMetaObject::invokeMethod(
            dispatcher,
            [ guard, root, found = std::move( result.paths ), generation,
              cancellation, publication ]() mutable {
                // 즉시 게시된 첫 진행 콜백에는 먼저 실행할 기회를 주되, 50 ms
                // 타이머 뒤에 남은 배치는 최종 snapshot으로 대체한다. deque swap은
                // 짧게 끝내고 실제 문자열 참조 해제는 워커에 맡긴다.
                std::deque< QStringList > discarded;
                {
                    const std::scoped_lock lock( publication->mutex );
                    discarded.swap( publication->pendingBatches );
                    publication->pendingFileCount = 0;
                    publication->drainScheduled = false;
                }
                retirePathBatches( std::move( discarded ) );

                if( guard.isNull() )
                {
                    retirePathList( std::move( found ) );
                    return;
                }
                if( isShuttingDown()
                    || cancellation->load( std::memory_order_relaxed ) )
                {
                    retirePathList( std::move( found ) );
                    guard->applyCancelled( root, generation );
                    return;
                }
                guard->apply( root, std::move( found ), generation );
            },
            Qt::QueuedConnection );
    } );
}

void PathIndex::drainPublishedBatches(
    const QString& root, const quint64 generation,
    const std::shared_ptr< PathIndexPublicationState >& publication,
    const std::shared_ptr< std::atomic_bool >& cancellation )
{
    if( generation != generation_ || scanningRoot_ != root || isShuttingDown()
        || cancellation->load( std::memory_order_relaxed ) )
    {
        std::deque< QStringList > discarded;
        {
            const std::scoped_lock lock( publication->mutex );
            discarded.swap( publication->pendingBatches );
            publication->pendingFileCount = 0;
            publication->drainScheduled = false;
        }
        retirePathBatches( std::move( discarded ) );
        return;
    }

    QStringList batch;
    qsizetype scannedCount = 0;
    {
        const std::scoped_lock lock( publication->mutex );
        if( publication->pendingBatches.empty() )
        {
            publication->drainScheduled = false;
            return;
        }

        qsizetype fileCount = 0;
        scannedCount = publication->discoveredFileCount;
        while( !publication->pendingBatches.empty() )
        {
            const QStringList& next = publication->pendingBatches.front();
            if( fileCount > 0 && fileCount + next.size() > kMaxFilesPerProgressDelivery )
                break;

            fileCount += next.size();
            publication->pendingFileCount -= next.size();
            batch.append( std::move( publication->pendingBatches.front() ) );
            publication->pendingBatches.pop_front();
        }
    }

    if( generation != generation_ || scanningRoot_ != root || isShuttingDown()
        || cancellation->load( std::memory_order_relaxed ) )
    {
        retirePathList( std::move( batch ) );
        return;
    }

    applyBatch( root, std::move( batch ), scannedCount, generation );

    // pending이 잠시 비었더라도 cooldown 동안 scheduled 상태를 유지한다. 그
    // 사이 워커가 넣은 배치는 새 GUI 이벤트를 만들지 않고 이 타이머가 가져간다.
    QTimer::singleShot(
        kProgressDeliveryInterval, this,
        [this, root, generation, publication, cancellation] {
            drainPublishedBatches( root, generation, publication, cancellation );
        } );
}

void PathIndex::applyBatch( const QString& root, QStringList batch,
                            const qsizetype scannedCount, const quint64 generation )
{
    if( generation != generation_ || scanningRoot_ != root || batch.isEmpty() )
        return;

    const auto chunk = std::make_shared< const QStringList >( std::move( batch ) );
    partialPathCount_ += chunk->size();
    scannedPathCount_ = (std::max)( scannedPathCount_, scannedCount );
    partialPathChunks_.append( chunk );
    emit progress( root, *chunk, scannedPathCount_ );
}

void PathIndex::apply( const QString& root, QStringList paths, const quint64 generation )
{
    if( generation != generation_ )
    {
        retirePathList( std::move( paths ) );
        return;   // 그사이 다른 루트를 훑기 시작했다
    }

    scanningRoot_.clear();
    indexedRoot_ = root;
    QStringList retiredPaths = std::move( paths_ );
    paths_ = std::move( paths );
    PathIndexChunks retiredChunks = std::move( partialPathChunks_ );
    partialPathChunks_.clear();
    partialPathCount_ = 0;
    scannedPathCount_ = paths_.size();
    retirePathChunks( std::move( retiredChunks ) );
    retirePathList( std::move( retiredPaths ) );
    scanCancellation_.reset();
    sinceLastScan_.start();

    emit logMessage( tr( "경로 인덱스: %1 개 [%2]" ).arg( paths_.size() ).arg( root ) );
    emit ready( root, paths_.size() );

    // 스캔 중 들어온 저장 알림은 이미 지나간 디렉터리를 바꿨을 수 있다.
    // 한 번으로 합쳐 즉시 다시 훑어 최종 스냅샷에서 영구 누락되지 않게 한다.
    if( pendingInvalidationRoot_ == root && !isShuttingDown() )
        startScan( root );
}

void PathIndex::applyCancelled( const QString& root, const quint64 generation )
{
    if( generation != generation_ || scanningRoot_ != root )
        return;

    scanningRoot_.clear();
    PathIndexChunks retiredChunks = std::move( partialPathChunks_ );
    partialPathChunks_.clear();
    partialPathCount_ = 0;
    scannedPathCount_ = 0;
    retirePathChunks( std::move( retiredChunks ) );
    scanCancellation_.reset();
}

void PathIndex::runPendingInvalidation()
{
    if( pendingInvalidationRoot_.isEmpty() || isShuttingDown() )
        return;

    const QString root = pendingInvalidationRoot_;
    startScan( root );
}

}   // namespace hc::rst
