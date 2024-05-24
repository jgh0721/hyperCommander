#include "stdafx.h"
#include "cmnPath.hpp"

int64_t nsCmn::nsCmnPath::CalculateDirSize( const std::wstring& Path )
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
            sz.LowPart  = data.nFileSizeLow;
            sz.HighPart = static_cast< LONG >( data.nFileSizeHigh );
            Size        += sz.QuadPart;
        }

    } while( FindNextFile( h, &data ) != 0 );

    FindClose( h );

    return Size;
}
