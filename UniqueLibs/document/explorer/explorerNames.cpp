#include "stdafx.h"
#include "explorerNames.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document::explorer
{
    namespace
    {
        QString tr( const char* Text )
        {
            return QCoreApplication::translate( "hc::document::explorer::Names", Text );
        }

        // 확장자 앞에서 이름을 가른다. 점으로 시작하는 이름(".gitignore")은 통째로 이름이다.
        void splitName( const QString& Name, QString* Base, QString* Suffix )
        {
            const qsizetype Dot = Name.lastIndexOf( QLatin1Char( '.' ) );
            if( Dot <= 0 )
            {
                *Base = Name;
                Suffix->clear();
                return;
            }
            *Base = Name.left( Dot );
            *Suffix = Name.mid( Dot );
        }

        bool isReservedDeviceName( const QString& Name )
        {
            // Windows 는 "CON" 뿐 아니라 "con.txt" 도 장치로 연다. 확장자 앞부분으로 본다.
            const QString Stem = Name.section( QLatin1Char( '.' ), 0, 0 ).trimmed().toUpper();
            static const QStringList Fixed = { QStringLiteral( "CON" ), QStringLiteral( "PRN" ), QStringLiteral( "AUX" ),
                                               QStringLiteral( "NUL" ) };
            if( Fixed.contains( Stem ) )
                return true;
            if( Stem.size() == 4 && ( Stem.startsWith( QLatin1String( "COM" ) ) || Stem.startsWith( QLatin1String( "LPT" ) ) ) )
                return Stem.at( 3 ) >= QLatin1Char( '1' ) && Stem.at( 3 ) <= QLatin1Char( '9' );
            return false;
        }
    }

    bool IsSafeEntryName( const QString& Name, QString* Reason )
    {
        const auto Fail = [ Reason ]( const QString& Text ) {
            if( Reason != nullptr )
                *Reason = Text;
            return false;
        };

        // MRE MainWindow.cpp:4656-4669. ".." 을 막는 것이 핵심이다 — 사용자가 고른 폴더 밖에 파일을 만드는 길이 된다.
        if( Name.isEmpty() )
            return Fail( tr( "이름을 입력하세요." ) );
        if( Name == QLatin1String( "." ) || Name == QLatin1String( ".." ) )
            return Fail( tr( "'.' 과 '..' 은 이름으로 쓸 수 없습니다." ) );
        // Windows 가 끝의 공백과 점을 조용히 잘라 낸다. 사용자가 친 이름과 만든 이름이 달라진다.
        if( Name.endsWith( QLatin1Char( ' ' ) ) || Name.endsWith( QLatin1Char( '.' ) ) )
            return Fail( tr( "이름 끝에 공백이나 점을 둘 수 없습니다." ) );

        static const QString Forbidden = QStringLiteral( R"(<>:"/\|?*)" );
        for( const QChar Ch : Name )
        {
            if( Ch.unicode() < 0x20 )
                return Fail( tr( "이름에 제어 문자를 쓸 수 없습니다." ) );
            if( Forbidden.contains( Ch ) )
                return Fail( tr( "이름에 \\ / : * ? \" < > | 를 쓸 수 없습니다." ) );
        }

        if( isReservedDeviceName( Name ) )
            return Fail( tr( "Windows 가 장치 이름으로 쓰는 이름입니다(CON, NUL, COM1 등)." ) );

        if( Reason != nullptr )
            Reason->clear();
        return true;
    }

    QString EntryNameProblem( const QString& Folder, const QString& Name, const QString& RenamingFrom )
    {
        QString Reason;
        if( !IsSafeEntryName( Name, &Reason ) )
            return Reason;

        // 대소문자만 바꾸는 이름 바꾸기는 같은 항목이다. 파일 시스템이 대소문자를 가리지 않으므로 "이미 있음" 으로 막으면 안 된다.
        // RenamingFrom 은 이름이든 경로든 받는다(끝 이름만 비교한다).
        if( !RenamingFrom.isEmpty() && Name.compare( QFileInfo( RenamingFrom ).fileName(), Qt::CaseInsensitive ) == 0 )
            return {};

        if( QFileInfo::exists( QDir( Folder ).filePath( Name ) ) )
            return tr( "같은 이름의 항목이 이미 있습니다." );
        return {};
    }

    QString UniqueEntryName( const QString& Folder, const QString& Name )
    {
        const QDir Directory( Folder );
        if( !QFileInfo::exists( Directory.filePath( Name ) ) )
            return Name;

        QString Base, Suffix;
        splitName( Name, &Base, &Suffix );
        // 상한은 무한 반복을 막는 안전장치다. 같은 이름이 만 개 있는 폴더는 사람이 만들지 않는다.
        for( int Number = 2; Number < 10000; ++Number )
        {
            const QString Candidate = QStringLiteral( "%1 (%2)%3" ).arg( Base ).arg( Number ).arg( Suffix );
            if( !QFileInfo::exists( Directory.filePath( Candidate ) ) )
                return Candidate;
        }
        return Name;
    }

    QString DefaultNewFileName( const QString& Folder, bool InsideSphinx )
    {
        return UniqueEntryName( Folder, InsideSphinx ? tr( "새 문서.rst" ) : tr( "새 문서.md" ) );
    }

    QString DefaultNewFolderName( const QString& Folder )
    {
        return UniqueEntryName( Folder, tr( "새 폴더" ) );
    }
}
