#include "stdafx.h"
#include "fileSetMgr.hpp"

void CFileSetMgr::Refresh()
{
    decltype( mapNameToFileSet ) Values;

    StSettings->beginGroup( "FileSet" );
    const auto Count = StSettings->value( "Count", 0 ).toInt();
    for( int idx = 0; idx < Count; ++idx )
    {
        TyFileSet FileSet = {
            StSettings->value( QString( "%1_Name" ).arg( idx ) ).toString(),
            StSettings->value( QString( "%1_Flags" ).arg( idx ) ).toUInt(),

            StSettings->value( QString( "%1_Filters" ).arg( idx ) ).toString().split( '|', Qt::SkipEmptyParts ),
            StSettings->value( QString( "%1_Exts" ).arg( idx ) ).toString().split( '|', Qt::SkipEmptyParts ).toVector(),
            StSettings->value( QString( "%1_Attributes" ).arg( idx ) ).toString().toUInt(),
            StSettings->value( QString( "%1_Size" ).arg( idx ) ).toString().toLongLong(),
        };

        if( FileSet.Name.isEmpty() == true )
            continue;

        Values[ FileSet.Name ] = FileSet;
    }
    StSettings->endGroup();

    //StSettings->beginGroup( "FileSet" );
    //Count = StSettings->value( "Count" ).toInt();
    //for( int Idx = 0; Idx < Count; ++Idx )
    //{
    //    TyFileSet FileSet;
    //    FileSet.Name = StSettings->value( QString( "%1_Name" ) ).toString();
    //    FileSet.Flags = StSettings->value( QString( "%1_Flags" ) ).toUInt();

    //    FileSet.Filters = StSettings->value( QString( "%1_Filters" ) ).toString().split( '|', Qt::SkipEmptyParts );
    //    FileSet.VecExtFilters = StSettings->value( QString( "%1_Exts" ) ).toString().split( '|', Qt::SkipEmptyParts );
    //    FileSet.Attributes = StSettings->value( QString( "%1_Attributes" ) ).toUInt();
    //    FileSet.Size = StSettings->value( QString( "%1_Size" ) ).toLongLong();
    //    mapNameToFileSet[ FileSet.Name ] = FileSet;
    //}
    //StSettings->endGroup();

    qSwap( Values, mapNameToFileSet );
}

void CFileSetMgr::SaveSettings()
{
    StSettings->beginGroup( "FileSet" );
    StSettings->setValue( "Count", mapNameToFileSet.count() );
    int idx = 0;
    for( const auto& Name : mapNameToFileSet.keys() )
    {
        const auto& FileSet = mapNameToFileSet[ Name ];
        StSettings->setValue( QString( "%1_Name" ).arg( idx ), FileSet.Name );
        StSettings->setValue( QString( "%1_Flags" ).arg( idx ), FileSet.Flags );

        StSettings->setValue( QString( "%1_Filters" ).arg( idx ), FileSet.Filters.join( '|' ) );
        StSettings->setValue( QString( "%1_Exts" ).arg( idx ), FileSet.VecExtFilters.join( '|' ) );
        StSettings->setValue( QString( "%1_Attributes" ).arg( idx ), FileSet.Attributes );
        StSettings->setValue( QString( "%1_Size" ).arg( idx ), FileSet.Size );

        idx++;
    }
    StSettings->endGroup();
}

QVector<QString> CFileSetMgr::GetNames() const
{
    return mapNameToFileSet.keys();
}

void CFileSetMgr::SetFileSet( const TyFileSet& FileSet )
{
    Q_ASSERT( FileSet.Name.isEmpty() == false );
    if( FileSet.Name.isEmpty() == true )
        return;

    mapNameToFileSet[ FileSet.Name ] = FileSet;
    SaveSettings();
}

TyFileSet CFileSetMgr::GetFileSet( const QString& FileSetName ) const
{
    if( mapNameToFileSet.contains( FileSetName ) == false )
        return {};

    return mapNameToFileSet[ FileSetName ];
}

void CFileSetMgr::RemoveFileSet( const QString& FileSetName )
{
    if( mapNameToFileSet.contains( FileSetName ) == false )
        return;

    mapNameToFileSet.remove( FileSetName );
    SaveSettings();
}
