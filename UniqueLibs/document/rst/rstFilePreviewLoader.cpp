#include "stdafx.h"
#include "rstFilePreviewLoader.hpp"

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
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMetaObject>
#include <QPixmapCache>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

namespace {

/// 방향키를 꾹 누르면 반복 주기가 ~30ms 다. 이 문턱이 없으면 훑는 동안
/// 디스크를 계속 만진다. setScaledSize() 하나로는 못 막는다 — 파일 열기와
/// 헤더 읽기 자체가 네트워크 드라이브에서 수백 ms 다.
constexpr int kDebounceMs = 120;
/// 디코딩 **전에** 거를 픽셀 상한. QImageReader::size() 는 헤더만 읽는다.
constexpr qint64 kMaxPixels = 80'000'000;
/// 프리뷰 픽스맵 캐시 상한 (KB). 남의 한도를 낮추지는 않는다.
constexpr int kPixmapCacheKb = 20 * 1024;
/// 값싼 메타 캐시 항목 수. 문자열 몇 개라 넉넉히 잡아도 된다.
constexpr int kMetaCacheLimit = 256;

}   // namespace

FilePreviewLoader::FilePreviewLoader( QObject* parent )
    : QObject( parent )
    , debounce_( new QTimer( this ) )
{
    debounce_->setSingleShot( true );
    debounce_->setInterval( kDebounceMs );
    connect( debounce_, &QTimer::timeout, this, &FilePreviewLoader::fire );

    QPixmapCache::setCacheLimit( qMax( QPixmapCache::cacheLimit(), kPixmapCacheKb ) );
}

QString FilePreviewLoader::cacheKeyFor( const QString& path, const QSize& box,
                                        const qreal devicePixelRatio )
{
    // dpr 을 키에 넣어야 모니터를 옮겨도 흐려지지 않는다.
    return QStringLiteral( "mrst.preview:%1|%2x%3@%4" )
        .arg( path )
        .arg( box.width() )
        .arg( box.height() )
        .arg( qRound( devicePixelRatio * 100 ) );
}

void FilePreviewLoader::cancel()
{
    debounce_->stop();
    pendingToken_ = 0;
    pendingPath_.clear();
    latest_.store( 0, std::memory_order_relaxed );
}

void FilePreviewLoader::request( const quint64 token, const QString& absolutePath,
                                 const QSize& boxLogical, const qreal devicePixelRatio,
                                 const bool isDirectory )
{
    debounce_->stop();
    latest_.store( token, std::memory_order_relaxed );

    if( token == 0 || absolutePath.isEmpty() )
        return;

    const QString key = cacheKeyFor( absolutePath, boxLogical, devicePixelRatio );

    QPixmap cached;
    const auto meta = metaCache_.constFind( key );
    if( meta != metaCache_.constEnd()
        && ( isDirectory || QPixmapCache::find( key, &cached ) ) )
    {
        // 캐시 적중. 디스크를 만지지 않는다.
        Result result;
        result.token = token;
        result.pixmap = cached;
        result.hasAlpha = meta.value().hasAlpha;
        result.metaLine = meta.value().metaLine;
        result.note = meta.value().note;
        emit previewReady( result );
        return;
    }

    pendingToken_ = token;
    pendingPath_ = absolutePath;
    pendingBox_ = boxLogical;
    pendingDpr_ = devicePixelRatio;
    pendingIsDirectory_ = isDirectory;
    debounce_->start();
}

void FilePreviewLoader::fire()
{
    const quint64 token = pendingToken_;
    const QString path = pendingPath_;
    const QSize box = pendingBox_;
    const qreal dpr = pendingDpr_;
    const bool isDirectory = pendingIsDirectory_;
    if( token == 0 || path.isEmpty() )
        return;

    const QString key = cacheKeyFor( path, box, dpr );
    const QSize boxDevice = ( QSizeF( box ) * dpr ).toSize();
    QPointer< FilePreviewLoader > guard( this );

    QThreadPool::globalInstance()->start( [ guard, token, path, boxDevice, dpr, isDirectory, key ] {
        // 디코딩 **전** 첫 검사. 그사이 사용자가 다른 항목으로 갔으면 그만둔다.
        if( guard.isNull() || isShuttingDown()
            || guard->latest_.load( std::memory_order_relaxed ) != token )
        {
            return;
        }

        Probe probe;
        const QFileInfo info( path );
        probe.exists = info.exists();
        probe.isDirectory = isDirectory || info.isDir();
        probe.bytes = probe.isDirectory ? 0 : info.size();

        if( probe.exists && !probe.isDirectory && filekinds::isImageFile( path ) )
        {
            QImageReader reader( path );
            reader.setAutoTransform( true );   // EXIF 회전
            probe.format = reader.format();
            probe.sourceSize = reader.size();   // 헤더만 읽는다
            probe.frameCount = qMax( 1, reader.imageCount() );

            const qint64 pixels = static_cast< qint64 >( probe.sourceSize.width() )
                                  * probe.sourceSize.height();
            if( !probe.sourceSize.isValid() )
            {
                probe.unreadable = true;
            }
            else if( pixels > kMaxPixels )
            {
                probe.tooBig = true;
            }
            else
            {
                const bool vector =
                    probe.format.compare( QByteArrayLiteral( "svg" ), Qt::CaseInsensitive ) == 0;
                // 벡터는 상자 크기까지 키워도 되고, 래스터는 줄이기만 한다.
                QSize target = probe.sourceSize;
                if( vector || target.width() > boxDevice.width()
                    || target.height() > boxDevice.height() )
                {
                    target = target.scaled( boxDevice, Qt::KeepAspectRatio );
                }
                if( target.isValid() )
                    reader.setScaledSize( target );

                probe.image = reader.read();
                // 플러그인이 setScaledSize 를 무시했을 수 있다.
                if( !probe.image.isNull() && target.isValid() && probe.image.size() != target )
                {
                    probe.image = probe.image.scaled( target, Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation );
                }
                probe.unreadable = probe.image.isNull();
            }
        }

        // 송신 **전** 둘째 검사.
        if( guard.isNull() || isShuttingDown()
            || guard->latest_.load( std::memory_order_relaxed ) != token )
        {
            return;
        }

        QMetaObject::invokeMethod(
            guard,
            [ guard, token, key, probe = std::move( probe ), dpr ]() mutable {
                if( guard.isNull() )
                    return;
                guard->deliver( token, key, probe, dpr );
            },
            Qt::QueuedConnection );
    } );
}

void FilePreviewLoader::deliver( const quint64 token, const QString& cacheKey, const Probe& probe,
                                 const qreal devicePixelRatio )
{
    if( latest_.load( std::memory_order_relaxed ) != token )
        return;

    Result result;
    result.token = token;

    // QPixmap 은 GUI 스레드에서만 만든다. 워커는 QImage 까지다.
    if( !probe.image.isNull() )
    {
        result.pixmap = QPixmap::fromImage( probe.image );
        result.pixmap.setDevicePixelRatio( devicePixelRatio );
        result.hasAlpha = probe.image.hasAlphaChannel();
        QPixmapCache::insert( cacheKey, result.pixmap );
    }

    QStringList parts;
    if( probe.isDirectory )
    {
        parts << tr( "디렉터리" );
    }
    else
    {
        if( !probe.format.isEmpty() )
            parts << QString::fromLatin1( probe.format ).toUpper();
        if( probe.sourceSize.isValid() )
        {
            parts << QStringLiteral( "%1×%2" )
                        .arg( probe.sourceSize.width() )
                        .arg( probe.sourceSize.height() );
        }
        if( probe.frameCount > 1 )
            parts << tr( "%1 프레임" ).arg( probe.frameCount );
        if( probe.bytes > 0 )
        {
            parts << QLocale().formattedDataSize( probe.bytes, 1,
                                                 QLocale::DataSizeTraditionalFormat );
        }
    }
    result.metaLine = parts.join( QStringLiteral( " · " ) );

    if( !probe.exists )
        result.note = tr( "파일을 읽을 수 없습니다" );
    else if( probe.tooBig )
        result.note = tr( "이미지가 너무 큽니다" );
    else if( probe.unreadable )
    {
        // SVG 플러그인이 없는 배포에서도 여기로 온다. 하드 실패는 만들지 않는다.
        result.note = tr( "미리보기를 만들 수 없습니다" );
    }

    Meta meta;
    meta.hasAlpha = result.hasAlpha;
    meta.metaLine = result.metaLine;
    meta.note = result.note;
    if( metaCache_.size() >= kMetaCacheLimit )
        metaCache_.clear();
    metaCache_.insert( cacheKey, meta );

    emit previewReady( result );
}

}  // namespace hc::rst
