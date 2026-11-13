#include "stdafx.h"
#include "remoteDrive.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <lm.h>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document::explorer
{
    namespace
    {
        // NetUseGetInfo 는 기억된 매핑이면 연결이 끊겨도 정보를 준다. WNetGetConnection 은 그 경우에도
        // 성공하므로 쓰지 않는다 — net use 와 같은 상태값(ui1_status)으로 실제 세션 상태를 본다.
        // MRE FileTreeFilterProxy.cpp:22-42.
        bool isDisconnectedMappedDrive( const QString& DriveName )
        {
            // 오래된 시그니처가 입력 문자열도 LPWSTR 로 받는다. 원본을 const_cast 하지 않고 호출 전용 버퍼를 만든다.
            QString MutableName = DriveName;
            LPBYTE Buffer = nullptr;
            const NET_API_STATUS Result = ::NetUseGetInfo( nullptr, reinterpret_cast< LPWSTR >( MutableName.data() ), 1, &Buffer );
            if( Result != NERR_Success || Buffer == nullptr )
            {
                if( Buffer != nullptr )
                    ::NetApiBufferFree( Buffer );
                return false;
            }

            const auto* UseInfo = reinterpret_cast< const USE_INFO_1* >( Buffer );
            const bool Disconnected = UseInfo->ui1_status != USE_OK;
            ::NetApiBufferFree( Buffer );
            return Disconnected;
        }

        // "\\?\C:\x" · "\\?\UNC\server\share" 의 긴 경로 머리를 걷어 낸다. 구분자는 '/' 로 맞춘 뒤 부른다.
        QString stripLongPrefix( const QString& Path )
        {
            if( Path.startsWith( QLatin1String( "//?/UNC/" ), Qt::CaseInsensitive ) )
                return QLatin1String( "//" ) + Path.mid( 8 );
            if( Path.startsWith( QLatin1String( "//?/" ) ) || Path.startsWith( QLatin1String( "//./" ) ) )
                return Path.mid( 4 );
            return Path;
        }
    }

    bool IsDisconnectedRemoteDrivePath( const QString& Path )
    {
        // 파일 시스템 API 로 정규화하지 않는다. 끊긴 드라이브라면 정규화 자체가 재연결을 기다릴 수 있으므로
        // 문자열에서 드라이브 이름만 뽑는다. MRE FileTreeFilterProxy.cpp:44-56.
        const QString Plain = stripLongPrefix( QString( Path ).replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) ) );
        if( Plain.size() < 2 || !Plain.at( 0 ).isLetter() || Plain.at( 1 ) != QLatin1Char( ':' ) )
            return false;
        return isDisconnectedMappedDrive( Plain.left( 2 ).toUpper() );
    }

    bool IsUncPath( const QString& Path )
    {
        const QString Plain = stripLongPrefix( QString( Path ).replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) ) );
        return Plain.startsWith( QLatin1String( "//" ) ) && Plain.size() > 2 && Plain.at( 2 ) != QLatin1Char( '/' );
    }

    QString DriveRootOf( const QString& Path )
    {
        const QString Plain = stripLongPrefix( QString( Path ).replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) ) );
        if( Plain.size() >= 2 && Plain.at( 0 ).isLetter() && Plain.at( 1 ) == QLatin1Char( ':' ) )
            return Plain.left( 1 ).toUpper() + QLatin1String( ":/" );

        if( !IsUncPath( Plain ) )
            return {};

        // UNC 는 공유 루트까지가 드라이브다. 서버만 있고 공유 이름이 없으면 루트를 정할 수 없다.
        const QStringList Parts = Plain.mid( 2 ).split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
        if( Parts.size() < 2 )
            return {};
        return QLatin1String( "//" ) + Parts.at( 0 ) + QLatin1Char( '/' ) + Parts.at( 1 ) + QLatin1Char( '/' );
    }
}
