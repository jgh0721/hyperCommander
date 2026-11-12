#include "stdafx.h"
#include "uvDownloader.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <zlib.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>
#include <QtEndian>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::rst {
namespace {

/// 압축 파일은 15 MB 남짓이다. 이보다 크면 다른 것을 받고 있는 것이다.
constexpr qint64 MaxArchiveBytes = 128LL * 1024 * 1024;
/// uv.exe 는 60 MB 남짓이다.
constexpr qint64 MaxEntryBytes = 512LL * 1024 * 1024;
constexpr int TransferTimeoutMs = 30000;

QString markerPath( const QString& Target )
{
    return QFileInfo( Target ).absoluteDir().filePath( QStringLiteral( "uv.version" ) );
}

quint16 u16( const char* At ) { return qFromLittleEndian< quint16 >( At ); }
quint32 u32( const char* At ) { return qFromLittleEndian< quint32 >( At ); }

bool setError( QString* Error, const QString& Message )
{
    if( Error != nullptr )
        *Error = Message;
    return false;
}

}  // namespace

QStringList UvRelease::DownloadUrls()
{
    const QString Version = QString::fromLatin1( UvRelease::Version ), Name = QString::fromLatin1( UvRelease::ArchiveName );
    return { QStringLiteral( "https://releases.astral.sh/github/uv/releases/download/%1/%2" ).arg( Version, Name ),
             QStringLiteral( "https://github.com/astral-sh/uv/releases/download/%1/%2" ).arg( Version, Name ) };
}

QByteArray ExtractZipEntry( const QByteArray& Archive, const QString& EntryName, QString* Error )
{
    // 끝에서부터 중앙 디렉터리 끝 레코드( PK\5\6 )를 찾는다. 주석이 붙을 수 있어 고정 위치가 아니다.
    constexpr qsizetype EndSize = 22;
    qsizetype End = -1;
    for( qsizetype At = Archive.size() - EndSize; At >= 0 && At >= Archive.size() - EndSize - 65535; --At ) {
        if( u32( Archive.constData() + At ) == 0x06054b50u ) { End = At; break; }
    }
    if( End < 0 ) { setError( Error, QObject::tr( "ZIP 형식이 아닙니다." ) ); return {}; }

    const quint16 Count = u16( Archive.constData() + End + 10 );
    qsizetype At = u32( Archive.constData() + End + 16 );
    const QByteArray Wanted = EntryName.toUtf8();
    for( quint16 Index = 0; Index < Count; ++Index ) {
        constexpr qsizetype HeaderSize = 46;
        if( At < 0 || At + HeaderSize > Archive.size() || u32( Archive.constData() + At ) != 0x02014b50u ) break;
        const char* Header = Archive.constData() + At;
        const quint16 Method = u16( Header + 10 );
        const quint32 Crc = u32( Header + 16 ), Packed = u32( Header + 20 ), Size = u32( Header + 24 );
        const quint16 NameLength = u16( Header + 28 ), ExtraLength = u16( Header + 30 ), CommentLength = u16( Header + 32 );
        const quint32 LocalOffset = u32( Header + 42 );
        if( At + HeaderSize + NameLength > Archive.size() ) break;
        const QByteArray Name = Archive.mid( At + HeaderSize, NameLength );
        At += HeaderSize + NameLength + ExtraLength + CommentLength;
        // 보관 파일 안의 경로는 '/' 로 나뉜다. 마지막 요소만 본다.
        if( Name.mid( Name.lastIndexOf( '/' ) + 1 ).compare( Wanted, Qt::CaseInsensitive ) != 0 ) continue;

        constexpr qsizetype LocalSize = 30;
        if( qsizetype( LocalOffset ) + LocalSize > Archive.size() || u32( Archive.constData() + LocalOffset ) != 0x04034b50u ) {
            setError( Error, QObject::tr( "ZIP 항목 머리가 손상되었습니다." ) ); return {};
        }
        const char* Local = Archive.constData() + LocalOffset;
        const qsizetype Data = qsizetype( LocalOffset ) + LocalSize + u16( Local + 26 ) + u16( Local + 28 );
        if( Size > MaxEntryBytes || Data + qsizetype( Packed ) > Archive.size() ) {
            setError( Error, QObject::tr( "ZIP 항목 크기가 올바르지 않습니다." ) ); return {};
        }

        QByteArray Out;
        if( Method == 0 ) {
            Out = Archive.mid( Data, Packed );
        } else if( Method == 8 ) {
            Out.resize( Size );
            z_stream Stream{};
            // 음수 창 크기: zlib 머리 없는 raw deflate.
            if( inflateInit2( &Stream, -MAX_WBITS ) != Z_OK ) { setError( Error, QObject::tr( "압축을 풀 수 없습니다." ) ); return {}; }
            Stream.next_in = reinterpret_cast< Bytef* >( const_cast< char* >( Archive.constData() + Data ) );
            Stream.avail_in = Packed;
            Stream.next_out = reinterpret_cast< Bytef* >( Out.data() );
            Stream.avail_out = Size;
            const int Result = inflate( &Stream, Z_FINISH );
            const bool Complete = Result == Z_STREAM_END && Stream.total_out == Size;
            inflateEnd( &Stream );
            if( !Complete ) { setError( Error, QObject::tr( "압축을 풀 수 없습니다." ) ); return {}; }
        } else {
            setError( Error, QObject::tr( "지원하지 않는 ZIP 압축 방식입니다." ) ); return {};
        }
        if( quint32( Out.size() ) != Size
            || crc32( 0L, reinterpret_cast< const Bytef* >( Out.constData() ), uInt( Out.size() ) ) != Crc ) {
            setError( Error, QObject::tr( "ZIP 항목의 검사값이 맞지 않습니다." ) ); return {};
        }
        return Out;
    }
    setError( Error, QObject::tr( "ZIP 안에 %1 이(가) 없습니다." ).arg( EntryName ) );
    return {};
}

UvDownloader::UvDownloader( QObject* Parent )
    : QObject( Parent ), network_( new QNetworkAccessManager( this ) )
{
}

UvDownloader::~UvDownloader()
{
    if( reply_ ) reply_->abort();
}

QString UvDownloader::InstalledVersion( const QString& Target )
{
    QFile Marker( markerPath( Target ) );
    if( !QFileInfo::exists( Target ) || !Marker.open( QIODevice::ReadOnly ) ) return {};
    return QJsonDocument::fromJson( Marker.readAll() ).object().value( QStringLiteral( "version" ) ).toString();
}

bool UvDownloader::IsInstalled( const QString& Target )
{
    return InstalledVersion( Target ) == QLatin1String( UvRelease::Version );
}

bool UvDownloader::IsRunning() const
{
    return !reply_.isNull();
}

void UvDownloader::Start( const QString& Target )
{
    if( IsRunning() ) return;
    target_ = Target;
    urls_ = UvRelease::DownloadUrls();
    lastError_.clear();
    cancelled_ = false;
    tryNext();
}

void UvDownloader::Cancel()
{
    cancelled_ = true;
    if( reply_ ) reply_->abort();
}

void UvDownloader::tryNext()
{
    if( urls_.isEmpty() ) {
        emit Finished( false, lastError_.isEmpty() ? tr( "uv를 내려받을 수 없습니다." ) : lastError_ );
        return;
    }
    const QUrl Url( urls_.takeFirst() );
    emit Log( tr( "uv %1 내려받기: %2" ).arg( QLatin1String( UvRelease::Version ), Url.toString() ) );
    QNetworkRequest Request( Url );
    // 배포처가 저장소로 넘겨줄 수 있다. https 에서 http 로 내려가는 이동은 따르지 않는다.
    Request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy );
    Request.setTransferTimeout( TransferTimeoutMs );
    Request.setHeader( QNetworkRequest::UserAgentHeader, QStringLiteral( "HyperEditor" ) );
    reply_ = network_->get( Request );
    connect( reply_, &QNetworkReply::downloadProgress, this, [ this ]( qint64 Received, qint64 Total ) {
        if( Received > MaxArchiveBytes && reply_ ) { lastError_ = tr( "내려받은 파일이 너무 큽니다." ); reply_->abort(); return; }
        emit Progress( Received, Total );
    } );
    connect( reply_, &QNetworkReply::finished, this, &UvDownloader::onFinished );
}

void UvDownloader::fail( const QString& Message )
{
    lastError_ = Message;
    emit Log( Message );
    tryNext();
}

void UvDownloader::onFinished()
{
    QNetworkReply* Reply = reply_;
    reply_.clear();
    if( Reply == nullptr ) return;
    Reply->deleteLater();

    if( cancelled_ ) { emit Finished( false, tr( "내려받기를 취소했습니다." ) ); return; }
    if( Reply->error() != QNetworkReply::NoError ) {
        fail( lastError_.isEmpty() ? tr( "내려받기 실패: %1" ).arg( Reply->errorString() ) : lastError_ );
        return;
    }
    const QByteArray Archive = Reply->readAll();
    const QByteArray Hash = QCryptographicHash::hash( Archive, QCryptographicHash::Sha256 ).toHex();
    if( Hash != QByteArray( UvRelease::ArchiveSha256 ) ) {
        // 다른 주소에서 다시 받는다. 두 곳 모두 다르면 쓰지 않는다.
        fail( tr( "내려받은 uv의 SHA-256이 제품에 기록된 값과 다릅니다. 받은 값: %1" ).arg( QString::fromLatin1( Hash ) ) );
        return;
    }

    QString Error;
    const QByteArray Executable = ExtractZipEntry( Archive, QStringLiteral( "uv.exe" ), &Error );
    if( Executable.isEmpty() ) { emit Finished( false, Error ); return; }

    QDir().mkpath( QFileInfo( target_ ).absolutePath() );
    QSaveFile Out( target_ );
    if( !Out.open( QIODevice::WriteOnly ) || Out.write( Executable ) != Executable.size() || !Out.commit() ) {
        emit Finished( false, tr( "uv를 저장할 수 없습니다: %1" ).arg( QDir::toNativeSeparators( target_ ) ) );
        return;
    }
    QSaveFile Marker( markerPath( target_ ) );
    const QJsonObject Record{ { QStringLiteral( "version" ), QLatin1String( UvRelease::Version ) },
                              { QStringLiteral( "archiveSha256" ), QLatin1String( UvRelease::ArchiveSha256 ) },
                              { QStringLiteral( "source" ), Reply->url().toString() } };
    if( !Marker.open( QIODevice::WriteOnly ) || Marker.write( QJsonDocument( Record ).toJson() ) < 0 || !Marker.commit() ) {
        emit Finished( false, tr( "uv 버전 표식을 쓸 수 없습니다." ) );
        return;
    }
    emit Log( tr( "uv %1 준비 완료 (SHA-256 확인됨)" ).arg( QLatin1String( UvRelease::Version ) ) );
    emit Finished( true, {} );
}

}  // namespace hc::rst
