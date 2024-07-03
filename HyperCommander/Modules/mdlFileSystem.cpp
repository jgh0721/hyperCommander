#include "stdafx.h"
#include "mdlFileSystem.hpp"

#include "cmnTypeDefs_Name.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

namespace nsHC
{
    CFileSystemT::CFileSystemT( const QString& Root )
    {
        root_ = Root;
    }

    CFileSystemT* CFileSystemT::MakeFileSystem( const QString& Root, TyOsError* Error )
    {
        CFileSystemT* Ret = nullptr;
        if( Error != nullptr )
            *Error = TyOsError();

        // Root 경로명을 분석하여 LOCAL, REMOTE 등을 판정한다.

        QString s = Root;
        s = s.replace( "/", "\\" );

        bool IsNTPath = false;
        bool IsUNCPath = false;
        bool IsVFS = false;
        qsizetype Base = 0;

        if( s.startsWith( NTPATH_UNC, Qt::CaseInsensitive ) == true )
        {
            IsNTPath = true;
            IsUNCPath = true;
            Base = NTPATH_UNC.length();
        }
        else if( s.startsWith( NTPATH ) == true )
        {
            IsNTPath = true;
            IsUNCPath = false;
            Base = NTPATH.length();
        }
        else
        {
            if( s.startsWith( VFSPATH ) == true )
            {
                // VFS
                IsVFS = true;
            }
            else
            {
                // LOCAL, REMOTE

                if( s.startsWith( R"(\\)" ) == true )
                {
                    IsUNCPath = true;
                    Base = 2;
                }
            }
        }

        if( IsVFS == true )
        {

            return nullptr;
        }

        if( IsUNCPath == true )
        {

            return nullptr;
        }

        if( IsUNCPath == false )
        {
            // LOCAL
            const auto Pos = s.indexOf( ':' );

            if( Pos < 0 )
            {
                if( Error != nullptr )
                    *Error = TyOsError( TyOsError::OS_WIN32_ERROR, ERROR_BAD_PATHNAME );

                return nullptr;
            }

            QString RootPath;
            if( IsNTPath == true )
            {
                if( IsUNCPath == true )
                    RootPath = NTPATH_UNC;
                else
                    RootPath = NTPATH;
            }
            else
            {
                if( IsUNCPath == true )
                    RootPath = R"(\\)";
            }

            RootPath = s.mid( Base, 3 );

            // 반드시 \\ 로 끝나야 한다. 

            WCHAR FsName[ MAX_PATH ] = { 0, };
            if( GetVolumeInformationW( RootPath.toStdWString().c_str(), nullptr, 0, nullptr, nullptr, nullptr, FsName, MAX_PATH ) == FALSE )
            {
                if( Error != nullptr )
                    *Error = TyOsError( TyOsError::OS_WIN32_ERROR, ::GetLastError() );

                return nullptr;
            }

            if( stricmp( FsName, L"NTFS" ) == 0 )
                Ret = new nsHC::CFSNtfs( RootPath.left(2) );
            else
                Ret = new nsHC::CFSLocal( RootPath.left(2) );
        }

        return Ret;
    }

    TyEnFSCate CFileSystemT::GetCate() const
    {
        return cate_;
    }

    TyEnFSFeatures CFileSystemT::GetFeatures() const
    {
        return features_;
    }

    TyOsError CFileSystemT::SetVolumeName( const QString& VolumeLabel )
    {
        return { TyOsError::OS_WIN32_ERROR, ERROR_SUCCESS };
    }

    void CFileSystemT::readFsInformation()
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSLocal::CFSLocal( const QString& Root )
        : CFileSystemT( Root )
    {
        cate_ = FS_CATE_LOCAL;
        readFsInformation();
    }

    bool CFSLocal::IsRemovable()
    {
        return GetDriveTypeW( GetRoot().toStdWString().c_str() ) == DRIVE_REMOVABLE;
    }

    quint64 CFSLocal::GetTotalSize()
    {
        ULARGE_INTEGER Li = { 0 };
        if( GetDiskFreeSpaceExW( GetRoot().toStdWString().c_str(), nullptr, &Li, nullptr ) == FALSE )
            return 0;

        return Li.QuadPart;
    }

    quint64 CFSLocal::GetFreeSize()
    {
        ULARGE_INTEGER Li = { 0 };
        if( GetDiskFreeSpaceExW( GetRoot().toStdWString().c_str(), &Li, nullptr, nullptr ) == FALSE )
            return 0;

        return Li.QuadPart;
    }

    void CFSLocal::readFsInformation()
    {
        DWORD FsFlags = 0;
        if( GetVolumeInformationW( GetRoot().toStdWString().c_str(), nullptr, 0, nullptr, &maxComponentLength_, &FsFlags, nullptr, 0 ) == FALSE )
            return;

        features_ |= FS_FEA_LIST;
        features_ |= FS_FEA_READABLE;

        if( !FlagOn( FsFlags, FILE_READ_ONLY_VOLUME ) )
            features_ = static_cast< TyEnFSFeatures >( features_.toInt() | (
                                                           FS_FEA_WRITABLE | FS_FEA_MOVABLE | FS_FEA_DELETABLE | FS_FEA_RENAMABLE ) );

        if( FlagOn( FsFlags, FILE_PERSISTENT_ACLS ) )
            features_ |= FS_FEA_SECURITY;

        if( FlagOn( FsFlags, FILE_NAMED_STREAMS ) )
            features_ |= FS_FEA_STREAMS;

        features_ |= FS_FEA_PACK;
    }

    ////////////////////////////////////////////////////////////////////////////////
    ///

    CFSNtfs::CFSNtfs( const QString& Root )
        : CFSLocal( Root )
    {

    }

    QString CFSNtfs::GetVolumeName()
    {
        QVector< wchar_t > VecBuffer( MAX_PATH );

        if( GetVolumeInformationW( GetRoot().toStdWString().c_str(), VecBuffer.data(), VecBuffer.size(), nullptr, nullptr, nullptr, nullptr, 0 ) == FALSE )
            return "";

        return QString::fromWCharArray( VecBuffer.data() );
    }

} // nsHC
