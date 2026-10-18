#pragma once
#include <QDir>
#include <QString>
#include <QStringList>

namespace hc::win
{
    // Minimal environment for restricted child processes: no inherited secrets, only system locations.
    // Shell stores known folders as "%SystemDrive%\ProgramData\..."; when SystemDrive is missing the
    // literal path stays relative and a "%SystemDrive%" folder appears under the child's working directory.
    inline QStringList RestrictedEnvironment( const QString& Path )
    {
        QStringList Result{ "PATH=" + Path, "TEMP=" + QDir::tempPath(), "TMP=" + QDir::tempPath() };
        for( const auto* Name : { "SystemRoot", "SystemDrive", "ProgramData" } )
        {
            const auto Value = qEnvironmentVariable( Name );
            if( !Value.isEmpty() ) Result.append( QString( Name ) + '=' + Value );
        }
        return Result;
    }
}
