#include "stdafx.h"
#include "workspacePaths.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "UniqueLibs/document/explorer/remoteDrive.hpp"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document::workspace
{
    namespace
    {
        bool isBareDrive( const QString& Text )
        {
            return Text.size() == 2 && Text.at( 0 ).isLetter() && Text.at( 1 ) == QLatin1Char( ':' );
        }

        QString caseKey( const QString& Normalized )
        {
#ifdef Q_OS_WIN
            return Normalized.toLower();
#else
            return Normalized;
#endif
        }

        // 키끼리의 포함 관계. 루트가 "d:/" 처럼 '/' 로 끝나면 그대로 앞머리로 쓴다.
        bool keyInside( const QString& PathKeyText, const QString& RootKeyText )
        {
            if( PathKeyText.isEmpty() || RootKeyText.isEmpty() )
                return false;
            if( PathKeyText == RootKeyText )
                return true;
            const QString Prefix = RootKeyText.endsWith( QLatin1Char( '/' ) ) ? RootKeyText : RootKeyText + QLatin1Char( '/' );
            return PathKeyText.startsWith( Prefix );
        }

        // 있으면 정규 경로(링크 · 접합 풀림), 없으면 문자열 정규형. MRE isPathInWorkspace 의 normalized 람다와 같은 뜻.
        QString diskKey( const QString& Path )
        {
            const QString Canonical = QFileInfo( Path ).canonicalFilePath();
            return PathKey( Canonical.isEmpty() ? Path : Canonical );
        }
    }

    QString NormalizePath( const QString& Path )
    {
        if( Path.trimmed().isEmpty() )
            return {};

        QString Text = QDir::fromNativeSeparators( Path.trimmed() );
        if( Text.startsWith( QLatin1String( "//?/UNC/" ), Qt::CaseInsensitive ) )
            Text = QLatin1String( "//" ) + Text.mid( 8 );
        else if( Text.startsWith( QLatin1String( "//?/" ) ) )
            Text = Text.mid( 4 );

        // "D:" 만 주면 QFileInfo 가 그 드라이브의 현재 폴더로 읽는다. 드라이브 루트로 본다.
        if( isBareDrive( Text ) )
            Text += QLatin1Char( '/' );

        // absoluteFilePath 는 절대 경로를 문자열로만 다룬다(GetFullPathName). 상대 경로만 현재 폴더를 붙인다.
        if( !Text.startsWith( QLatin1String( "//" ) ) )
            Text = QFileInfo( Text ).absoluteFilePath();
        Text = QDir::cleanPath( Text );

        if( Text.size() >= 2 && Text.at( 0 ).isLetter() && Text.at( 1 ) == QLatin1Char( ':' ) )
        {
            Text[ 0 ] = Text.at( 0 ).toUpper();
            if( Text.size() == 2 )
                Text += QLatin1Char( '/' );
        }
        return Text;
    }

    QString NormalizeRoot( const QString& Path )
    {
        return NormalizePath( Path );
    }

    QString PathKey( const QString& Path )
    {
        return caseKey( NormalizePath( Path ) );
    }

    bool SamePath( const QString& Left, const QString& Right )
    {
        const QString LeftKey = PathKey( Left );
        return !LeftKey.isEmpty() && LeftKey == PathKey( Right );
    }

    bool IsInside( const QString& Path, const QString& Root, bool AllowDiskAccess )
    {
        if( keyInside( PathKey( Path ), PathKey( Root ) ) )
            return true;
        if( !AllowDiskAccess || Path.isEmpty() || Root.isEmpty() )
            return false;
        return keyInside( diskKey( Path ), diskKey( Root ) );
    }

    QString RelativeTo( const QString& Root, const QString& Path )
    {
        const QString NormalizedRoot = NormalizeRoot( Root );
        const QString NormalizedPath = NormalizePath( Path );
        const QString RootKeyText = caseKey( NormalizedRoot );
        const QString PathKeyText = caseKey( NormalizedPath );
        if( RootKeyText.isEmpty() || PathKeyText.isEmpty() )
            return {};
        if( PathKeyText == RootKeyText )
            return QStringLiteral( "." );

        const QString Prefix = RootKeyText.endsWith( QLatin1Char( '/' ) ) ? RootKeyText : RootKeyText + QLatin1Char( '/' );
        if( !PathKeyText.startsWith( Prefix ) )
            return {};
        // 키는 소문자지만 돌려주는 것은 원래 대소문자다. 정규형끼리는 길이가 같다.
        return NormalizedPath.mid( Prefix.size() );
    }

    QString ToStored( const QString& Root, const QString& Path, bool* Outside )
    {
        const QString Relative = RelativeTo( Root, Path );
        if( Outside != nullptr )
            *Outside = Relative.isEmpty();
        return Relative.isEmpty() ? NormalizePath( Path ) : Relative;
    }

    QString FromStored( const QString& Root, const QString& Stored )
    {
        const QString Text = QDir::fromNativeSeparators( Stored.trimmed() );
        if( Text.isEmpty() )
            return {};

        const bool Absolute = Text.startsWith( QLatin1Char( '/' ) )
                              || ( Text.size() >= 2 && Text.at( 0 ).isLetter() && Text.at( 1 ) == QLatin1Char( ':' ) );
        if( Absolute )
            return NormalizePath( Text );

        // 상대 경로는 늘 지금 루트를 기준으로 푼다. 루트를 모르면 풀 수 없다(현재 폴더 기준으로 읽으면 엉뚱한 파일이 된다).
        const QString NormalizedRoot = NormalizeRoot( Root );
        if( NormalizedRoot.isEmpty() )
            return {};
        if( Text == QLatin1String( "." ) )
            return NormalizedRoot;
        return NormalizePath( NormalizedRoot + QLatin1Char( '/' ) + Text );
    }

    QStringList AncestorFolders( const QString& Folder )
    {
        QString Current = NormalizePath( Folder );
        if( Current.isEmpty() )
            return {};

        const QString DriveKey = PathKey( explorer::DriveRootOf( Current ) );
        QStringList Result;
        // 상한은 깨진 입력에서의 무한 반복을 막는다. Windows 경로 깊이는 이보다 얕다.
        for( int Guard = 0; Guard < 1024 && !Current.isEmpty(); ++Guard )
        {
            Result.append( Current );
            if( caseKey( Current ) == DriveKey )
                break;

            const qsizetype Slash = Current.lastIndexOf( QLatin1Char( '/' ) );
            if( Slash <= 0 )
                break;
            QString Parent = Current.left( Slash );
            if( isBareDrive( Parent ) )
                Parent += QLatin1Char( '/' );
            // UNC 는 공유 루트 위("//server")로 올라가지 않는다.
            if( Parent.startsWith( QLatin1String( "//" ) ) && Parent.indexOf( QLatin1Char( '/' ), 2 ) < 0 )
                break;
            if( Parent == Current )
                break;
            Current = Parent;
        }
        return Result;
    }
}
