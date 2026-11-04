#include "stdafx.h"
#include "cmnPath.hpp"

#include <winioctl.h>
#include <vector>

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

#if defined(USE_QT_SUPPORT)
        QString GetFileExtension( const QString& FileName )
        {
            const auto Pos = FileName.lastIndexOf( '.' );
            if( Pos < 0 )
                return "";

            return FileName.mid( Pos + 1 );
        }
#endif

#if defined(USE_QT_SUPPORT)
        QString GetReparsePointTo( const QString& Path )
        {
            // 태그는 비트 집합이 아니므로 FlagOn 으로 가리지 않는다. 판별은 ReadReparseLink 가 맡는다.
            const auto hFile = CreateFileW( Path.toStdWString().c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr );
            if( hFile == INVALID_HANDLE_VALUE )
                return "";

            const auto Link = ReadReparseLink( hFile );
            CloseHandle( hFile );

            if( !Link.Value || Link.Value->Kind == TyReparseLink::REPARSE_LINK_OTHER )
                return "";

            return QString::fromStdWString( Link.Value->Target );
        }

        bool IsContainPathSeps( const QString& Path )
        {
            return Path.contains( '/' ) || Path.contains( '\\' );
        }
#endif
        TyOsValue<uint64_t> GetFreeDiskSpace( const std::wstring& Directory )
        {
            ULARGE_INTEGER Free = { 0, };
            if( GetDiskFreeSpaceExW( Directory.c_str(), &Free, nullptr, nullptr ) == FALSE )
                return MAKE_WIN32_LAST_ERROR;

            return MAKE_WIN32_VALUE( Free.QuadPart );
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

        TyOsValue< TyReparseLink > ReadReparseLink( HANDLE hFile )
        {
            // REPARSE_DATA_BUFFER 는 ntifs.h 에만 있다. 오프셋을 직접 읽는다.
            //   0 Tag(4) · 4 DataLength(2) · 8 SubstituteOffset · 10 SubstituteLength · 12 PrintOffset · 14 PrintLength
            //   심볼릭 링크는 16 Flags(4) 뒤 20 에서, 접합 지점은 16 에서 경로 버퍼가 시작한다.
            std::vector< BYTE > Buffer( MAXIMUM_REPARSE_DATA_BUFFER_SIZE );
            DWORD Bytes = 0;
            if( DeviceIoControl( hFile, FSCTL_GET_REPARSE_POINT, nullptr, 0, Buffer.data(), DWORD( Buffer.size() ), &Bytes, nullptr ) == FALSE )
                return MAKE_WIN32_LAST_ERROR;

            const TyOsValue< TyReparseLink > Invalid{ std::nullopt, { TyOsError::OS_WIN32_ERROR, ERROR_INVALID_REPARSE_DATA } };
            const auto Word = [ &Buffer ]( size_t Offset ) { WORD Value = 0; memcpy( &Value, Buffer.data() + Offset, sizeof( Value ) ); return size_t( Value ); };
            if( Bytes < 8 )
                return Invalid;

            TyReparseLink Link;
            memcpy( &Link.Tag, Buffer.data(), sizeof( Link.Tag ) );
            if( Word( 4 ) + 8 > Bytes )
                return Invalid;

            size_t PathBuffer = 0;
            if( Link.Tag == IO_REPARSE_TAG_SYMLINK )
            {
                Link.Kind = TyReparseLink::REPARSE_LINK_SYMLINK;
                PathBuffer = 20;
            }
            else if( Link.Tag == IO_REPARSE_TAG_MOUNT_POINT )
            {
                Link.Kind = TyReparseLink::REPARSE_LINK_JUNCTION;
                PathBuffer = 16;
            }
            else
                return MAKE_WIN32_VALUE( Link );

            if( Bytes < PathBuffer )
                return Invalid;

            const auto Name = [ & ]( size_t Field, std::wstring& Out ) {
                const auto Offset = Word( Field ), Length = Word( Field + 2 );
                if( ( Offset | Length ) & 1 || PathBuffer + Offset + Length > Bytes )
                    return false;
                Out.assign( reinterpret_cast< const wchar_t* >( Buffer.data() + PathBuffer + Offset ), Length / sizeof( wchar_t ) );
                return true;
            };

            std::wstring Substitute, Print;
            if( !Name( 8, Substitute ) || !Name( 12, Print ) )
                return Invalid;

            if( Link.Kind == TyReparseLink::REPARSE_LINK_SYMLINK )
            {
                ULONG Flags = 0;
                memcpy( &Flags, Buffer.data() + 16, sizeof( Flags ) );
                Link.Relative = ( Flags & 1 ) != 0;     // SYMLINK_FLAG_RELATIVE
            }

            // 표시 이름이 사용자가 만든 경로다. 볼륨 탑재 지점처럼 표시 이름이 없으면 NT 경로를 Win32 꼴로 바꾼다.
            if( !Print.empty() )
                Link.Target = Print;
            else if( Substitute.rfind( L"\\??\\", 0 ) == 0 )
            {
                const auto Rest = Substitute.substr( 4 );
                if( Rest.rfind( L"UNC\\", 0 ) == 0 )
                    Link.Target = L"\\\\" + Rest.substr( 4 );
                else if( Rest.size() >= 2 && Rest[ 1 ] == L':' )
                    Link.Target = Rest;
                else
                    Link.Target = L"\\\\?\\" + Rest;
            }
            else
                Link.Target = Substitute;

            return MAKE_WIN32_VALUE( Link );
        }

        TyOsValue<bool> IsDirExists( const std::wstring& DirPath )
        {
            const auto Ret = ::GetFileAttributesW( DirPath.c_str() );
            if( Ret == INVALID_FILE_ATTRIBUTES )
                return MAKE_WIN32_LAST_ERROR;

            return MAKE_WIN32_VALUE( FlagOn( Ret, FILE_ATTRIBUTE_DIRECTORY ) );
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
