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
            const auto Pos = FileName.lastIndexOf( '.' );
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

        TyOsValue< int64_t > GetFileSize( HANDLE hFile )
        {
            LARGE_INTEGER Li = { 0, };
            if( GetFileSizeEx( hFile, &Li ) != FALSE )
                return { Li.QuadPart };

            return MAKE_WIN32_LAST_ERROR;
        }

        TyOsValue<int64_t> GetFileSize( const std::wstring& FilePath )
        {
            WIN32_FIND_DATA Wfd = {0,};
            HANDLE hFind = ::FindFirstFileW( FilePath.c_str(), &Wfd );

            if( hFind != INVALID_HANDLE_VALUE )
            {
                ::FindClose( hFind );
                return MAKE_WIN32_VALUE( MAKEINT64( Wfd.nFileSizeLow, Wfd.nFileSizeHigh ) );
            }

            return MAKE_WIN32_LAST_ERROR;
        }

        TyOsValue<uint32_t> GetHardLinkCount( HANDLE hFile )
        {
            BY_HANDLE_FILE_INFORMATION FileInformation = {0,};

            if( ::GetFileInformationByHandle( hFile, &FileInformation ) != FALSE )
                return MAKE_WIN32_VALUE( FileInformation.nNumberOfLinks );

            return MAKE_WIN32_LAST_ERROR;
        }

        TyOsValue<bool> IsFileExists( const std::wstring& FilePath )
        {
            const auto Ret = ::GetFileAttributesW( FilePath.c_str() );
            if( Ret == INVALID_FILE_ATTRIBUTES )
                return MAKE_WIN32_LAST_ERROR;

            return MAKE_WIN32_VALUE( !FlagOn( Ret, FILE_ATTRIBUTE_DIRECTORY ) );
        }

        TyOsValue<bool> CopyACL( const std::wstring& Src, const std::wstring& Dst, SECURITY_INFORMATION Si )
        {
            DWORD Cb = 0;
            std::vector< char > Buffer;
            PSECURITY_DESCRIPTOR SD = nullptr;
            BOOL bRet = GetFileSecurityW( Src.c_str(), Si, SD, 0, &Cb );
            if( bRet == FALSE && Cb == 0 )
                return { false, { TyOsError::OS_WIN32_ERROR, ::GetLastError() } };

            Buffer.resize( Cb );
            SD = Buffer.data();
            bRet = GetFileSecurityW( Src.c_str(), Si, SD, Buffer.size(), &Cb );
            if( bRet == FALSE )
                return { false, { TyOsError::OS_WIN32_ERROR, ::GetLastError() } };

            bRet = SetFileSecurityW( Dst.c_str(), Si, SD );
            return { bRet != FALSE,{ TyOsError::OS_WIN32_ERROR, ::GetLastError() } };
        }
    } // nsCmnPath
} // nsCmn
