#include "stdafx.h"
#include "cmnPath.hpp"

#include <winioctl.h>

namespace nsCmn
{
    namespace nsCmnPath
    {
        int64_t CalculateDirSize( const std::wstring& Path )
        {
            int64_t Size = 0;
            WIN32_FIND_DATA data = {};
            // TODO: Path 가 Root 가 아니라면 \\?\ 를 덧붙인다. 
            std::wstring fname = Path + L"\\*.*";

            HANDLE h = FindFirstFileExW( fname.c_str(), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH );

            if( h == INVALID_HANDLE_VALUE )
                return Size;

            do
            {
                if( ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
                {
                    // make sure we skip "." and "..".  Have to use strcmp here because
                    // some file names can start with a dot, so just testing for the 
                    // first dot is not suffient.
                    if( wcscmp( data.cFileName, L"." ) != 0 && wcscmp( data.cFileName, L".." ) != 0 )
                    {
                        // We found a sub-directory, so get the files in it too
                        // recurrsion here!
                        Size += CalculateDirSize( Path + L"\\" + data.cFileName );
                    }
                }
                else
                {
                    LARGE_INTEGER sz = {};
                    // All we want here is the file size.  Since file sizes can be larger
                    // than 2 gig, the size is reported as two DWORD objects.  Below we
                    // combine them to make one 64-bit integer.
                    sz.LowPart = data.nFileSizeLow;
                    sz.HighPart = static_cast< LONG >( data.nFileSizeHigh );
                    Size += sz.QuadPart;
                }

            } while( FindNextFile( h, &data ) != 0 );

            FindClose( h );

            return Size;
        }

        QString GetFileExtension( const QString& FileName )
        {
            const auto Pos = FileName.indexOf( '.', -1 );
            if( Pos < 0 )
                return "";

            return FileName.mid( Pos + 1 );
        }

        QString GetReparsePointTo( const QString& Path )
        {
            WIN32_FIND_DATAW Wfd = { 0, };
            const auto P = Path.toStdWString();
            auto FindHandle = FindFirstFileW( P.c_str(), &Wfd );

            if( FindHandle == INVALID_HANDLE_VALUE )
                return "";

            if( FlagOn( Wfd.dwFileAttributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                if( FlagOn( Wfd.dwReserved0, IO_REPARSE_TAG_SYMLINK ) ||            // SYMLINKD
                    FlagOn( Wfd.dwReserved0, IO_REPARSE_TAG_MOUNT_POINT ) )         // JUNTION
                {
                    auto hFile =
                        CreateFileW( P.c_str(), 0,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL );

                    if( hFile != INVALID_HANDLE_VALUE )
                    {
                        DWORD cbData;
                        BYTE DataBuffer[ 16384 ] = { 0 };   // REPARSE_GUID_DATA_BUFFER 는 최대 16KB 이다. 

                        if( DeviceIoControl( hFile, FSCTL_GET_REPARSE_POINT, nullptr, 0, &DataBuffer, sizeof( DataBuffer ), &cbData, nullptr ) != FALSE )
                        {
                            CloseHandle( hFile );
                            const auto pRGDB = ( REPARSE_GUID_DATA_BUFFER* )&DataBuffer;
                            return QString::fromWCharArray( reinterpret_cast< const wchar_t* >( pRGDB->GenericReparseBuffer.DataBuffer ) );
                        }

                        CloseHandle( hFile );
                    }
                }
            }

            FindClose( FindHandle );

            return "";
        }
    } // nsCmnPath
} // nsCmn
