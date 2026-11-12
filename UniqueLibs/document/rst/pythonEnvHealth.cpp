#include "stdafx.h"
#include "pythonEnvHealth.hpp"

/// 모듈
#include "rstLineUtils.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

QString stripCfgQuotes( QString value )
{
    value = value.trimmed();
    if( value.size() >= 2
        && ( ( value.front() == QLatin1Char( '"' ) && value.back() == QLatin1Char( '"' ) )
             || ( value.front() == QLatin1Char( '\'' ) && value.back() == QLatin1Char( '\'' ) ) ) )
    {
        value = value.mid( 1, value.size() - 2 ).trimmed();
    }
    return value;
}

}  // namespace

QString pythonVenvDamageReason( const QString& venvDir )
{
    const QString cfgPath = QDir( venvDir ).filePath( QStringLiteral( "pyvenv.cfg" ) );
    QFile cfg( cfgPath );
    if( !cfg.open( QIODevice::ReadOnly | QIODevice::Text ) )
        return QCoreApplication::translate( "PythonEnvironment",
                                            "가상환경 설정 파일을 읽을 수 없습니다: %1" )
            .arg( QDir::toNativeSeparators( cfgPath ) );

#ifdef Q_OS_WIN
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "bin/python" ) );
#endif
    if( !QFileInfo::exists( launcherPath ) )
    {
        return QCoreApplication::translate( "PythonEnvironment",
                                            "가상환경 Python 실행 파일을 찾을 수 없습니다: %1" )
            .arg( QDir::toNativeSeparators( launcherPath ) );
    }

    QString home;
    const QStringList lines = rstline::fromUtf8File( cfg.readAll() ).split( QLatin1Char( '\n' ) );
    for( const QString& line : lines )
    {
        const qsizetype equal = line.indexOf( QLatin1Char( '=' ) );
        if( equal < 0 || line.left( equal ).trimmed().compare( QStringLiteral( "home" ),
                                                               Qt::CaseInsensitive ) != 0 )
            continue;
        home = stripCfgQuotes( line.mid( equal + 1 ) );
        break;
    }

#ifdef Q_OS_WIN
    if( !home.isEmpty() )
    {
        if( QDir::isRelativePath( home ) )
            home = QDir( venvDir ).absoluteFilePath( home );
        const QString basePython = QDir( home ).filePath( QStringLiteral( "python.exe" ) );
        if( !QFileInfo::exists( basePython ) )
        {
            return QCoreApplication::translate( "PythonEnvironment",
                                                "가상환경의 기반 Python을 찾을 수 없습니다: %1" )
                .arg( QDir::toNativeSeparators( basePython ) );
        }
    }
#else
    Q_UNUSED( home );
#endif
    return {};
}

bool pythonFailureIndicatesBrokenEnvironment( const int exitCode, const bool crashed,
                                              const QString& output )
{
    if( output.contains( QStringLiteral( "No Python at" ), Qt::CaseInsensitive ) )
        return true;
    if( exitCode < 0 && crashed )
        return true;
#ifdef Q_OS_WIN
    // Windows venvlauncher 는 pyvenv.cfg 의 기반 인터프리터가 없으면 103을 돌려준다.
    return exitCode == 103;
#else
    return false;
#endif
}

}  // namespace hc::rst
