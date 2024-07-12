#include "stdafx.h"
#include "builtInVolumeModel.hpp"

QVector< nsW32API::VOLUME_INFO > GetVolumeInfos()
{
    WCHAR wszVolumeName[ MAX_PATH ] = { 0, };
    HANDLE hVolumeSearch = INVALID_HANDLE_VALUE;
    QVector< nsW32API::VOLUME_INFO > vecVolumeInfos;

    static auto VOLUME_GUID_PREFIX = QStringLiteral( "\\\\?\\Volume{" );

    do
    {
        hVolumeSearch = FindFirstVolumeW( wszVolumeName, MAX_PATH );
        if( hVolumeSearch == INVALID_HANDLE_VALUE )
            break;

        do
        {
            nsW32API::VOLUME_INFO volumeInfo;

            volumeInfo.sGUID = QString::fromWCharArray( wszVolumeName );
            if( volumeInfo.sGUID.startsWith( VOLUME_GUID_PREFIX, Qt::CaseInsensitive ) == true )
                volumeInfo.sGUID = volumeInfo.sGUID.mid( VOLUME_GUID_PREFIX.size(), 36 /* GUID Length */ );

            {
                DWORD  CharCount = MAX_PATH + 1;
                PWCHAR Names = NULL;
                PWCHAR NameIdx = NULL;
                BOOL   Success = FALSE;

                for( ;; )
                {
                    //  Allocate a buffer to hold the paths.
                    Names = ( PWCHAR ) new BYTE[ CharCount * sizeof( WCHAR ) ];

                    if( !Names )
                    {
                        //  If memory can't be allocated, return.
                        break;
                    }

                    //  Obtain all of the paths
                    //  for this volume.
                    Success = GetVolumePathNamesForVolumeNameW( wszVolumeName, Names, CharCount, &CharCount );

                    if( Success )
                        break;

                    if( GetLastError() != ERROR_MORE_DATA )
                    {
                        break;
                    }

                    //
                    //  Try again with the
                    //  new suggested size.
                    delete[] Names;
                    Names = NULL;
                }

                if( Success )
                {
                    //  Display the various paths.
                    for( NameIdx = Names; NameIdx[ 0 ] != L'\0'; NameIdx += wcslen( NameIdx ) + 1 )
                    {
                        if( wcslen( NameIdx ) == 3 )
                            volumeInfo.sDriveLetter = QString::fromWCharArray( NameIdx );
                        else if( wcslen( NameIdx ) > 3 )
                            volumeInfo.vecMountPoint.push_back( QString::fromWCharArray( NameIdx ) );
                    }
                }

                if( Names != NULL )
                {
                    delete[] Names;
                    Names = NULL;
                }
            }

            if( volumeInfo.sDriveLetter.isEmpty() == false )
            {
                WCHAR volumeName[ MAX_PATH + 1 ] = { 0 };
                WCHAR fileSystemName[ MAX_PATH + 1 ] = { 0 };

                BOOL bRet = GetVolumeInformationW( volumeInfo.sDriveLetter.toStdWString().c_str(),
                                                   volumeName, sizeof( volumeName ),
                                                   ( DWORD* )&volumeInfo.dwSerialNo,
                                                   ( DWORD* )&volumeInfo.dwMaxComponentLen, ( DWORD* )&volumeInfo.dwFileSystemFlags,
                                                   fileSystemName, sizeof( fileSystemName ) );

                if( bRet == FALSE )
                    break;

                volumeInfo.sName = QString::fromWCharArray( volumeName );
                volumeInfo.sFileSystem = QString::fromWCharArray( fileSystemName );

                volumeInfo.dwDriveType = GetDriveTypeW( volumeInfo.sDriveLetter.toStdWString().c_str() );
            }

            vecVolumeInfos.push_back( cmnMove( volumeInfo ) );

        } while( FindNextVolumeW( hVolumeSearch, wszVolumeName, MAX_PATH ) != FALSE );

    } while( false );

    if( hVolumeSearch != INVALID_HANDLE_VALUE )
        FindVolumeClose( hVolumeSearch );

    return vecVolumeInfos;
}
