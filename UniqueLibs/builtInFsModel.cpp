#include "stdafx.h"
#include "builtInFsModel.hpp"

#include "commonLibs/cmnDateTime.hpp"
#include "cmnHCUtils.hpp"
#include "UniqueLibs/columnMgr.hpp"

#include <QtConcurrent>
#include <ObjectArray.h>
#include <Shlobj.h>
#include <winioctl.h>

/// Everything-SDK
#include "Everything.h"

#include <QtConcurrent>
#include <numeric>
#include <Shlwapi.h>

#include "colorSchemeMgr.hpp"
#include "fileSetMgr.hpp"

#pragma comment( lib, "shlwapi" )

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

void CFSIconGatherer::run()
{
    QFileIconProvider Provider;

    for( int row = 0; row < vecNode_.size(); ++row )
    {
        if( isInterruptionRequested() == true )
            break;

        CCmnMutexLocker Locker( lock_ );
        const auto& Item = vecNode_[ row ];

        if( Item->Icon.isNull() == false )
            continue;

        if( FlagOn( Item->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            Item->Icon = Provider.icon( QAbstractFileIconProvider::Folder ).pixmap( 32, 32 );
        }
        else
        {
            Item->Icon = Provider.icon( QFileInfo( currentPathWithRoot_ + Item->Name_ ) ).pixmap( 32, 32 );
        }

        if( Item->Icon.isNull() == false )
            model_->dataChanged( model_->index( row, 0 ), model_->index( row, 0 ), QList<int>() << Qt::DecorationRole );
    }
}

////////////////////////////////////////////////////////////////////////////////
///

void CFSDirSizeGatherer::run()
{
    if( sizeCol_ < 0 )
        return;

    QMap< QString, qint64 > MapDirNameToSize;
    
    QtConcurrent::run( [&MapDirNameToSize, this]() {

        Everything_SetMatchPath( TRUE );
        Everything_SetMatchWholeWord( TRUE );
        Everything_SetRequestFlags( EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH | EVERYTHING_REQUEST_SIZE );
        Everything_SetSort( EVERYTHING_SORT_PATH_ASCENDING );

        Everything_SetReplyID( QRandomGenerator().generate() );
        Everything_SetSearchW( currentPathWithRoot_.toStdWString().c_str() );
        Everything_QueryW( TRUE );

        QVector< uint32_t > VecResults( Everything_GetNumResults() );
        std::iota( VecResults.begin(), VecResults.end(), 0 );

        std::vector< wchar_t > BaseBuffer( currentPathWithRoot_.length() + 1 );
        currentPathWithRoot_.toWCharArray( &BaseBuffer[ 0 ] );

        bool IsContinue = true;

        VecResults = QtConcurrent::blockingFiltered( VecResults, [BaseBuffer, &IsContinue, this]( uint32_t Row ) {
            if( isInterruptionRequested() == true )
                return false;

            if( IsContinue == false ) return false;

            if( Everything_IsFolderResult( Row ) == FALSE )
                return false;

            if( wcsstr( BaseBuffer.data(), Everything_GetResultPathW( Row ) ) == nullptr )
            {
                IsContinue = false;
                return false;
            }

            return true;
        } );

        if( isInterruptionRequested() == true )
            return;

        MapDirNameToSize = QtConcurrent::blockingMappedReduced<decltype( MapDirNameToSize )>( VecResults, [this]( uint32_t Row ) {
            QPair< QString, LARGE_INTEGER > Result;
            if( Everything_GetResultSize( Row, &Result.second ) != FALSE )
                Result.first = QString::fromWCharArray( Everything_GetResultFileNameW( Row ) );

            return Result;
        }, [this]( QMap< QString, qint64 >& Result, const QPair< QString, LARGE_INTEGER >& Value ) {
            if( Value.first.isEmpty() == false )
                Result[ Value.first ] = Value.second.QuadPart;
        } );

    } ).waitForFinished();

    if( isInterruptionRequested() == true )
        return;

    for( int row = 0; row < vecNode_.size(); ++row )
    {
        if( isInterruptionRequested() == true )
            break;

        {
            CCmnMutexLocker Locker( lock_ );
            const auto& Item = vecNode_[ row ];

            if( MapDirNameToSize.contains( Item->Name_ ) == true )
                Item->Size_ = MapDirNameToSize[ Item->Name_ ];

            ( ( CFSModel* )model_ )->createBuiltFsValues( Item, sizeCol_ );
        }

        model_->dataChanged( model_->index( row, sizeCol_ ), model_->index( row, sizeCol_ ), QList<int>() << Qt::DisplayRole << Qt::EditRole );
    }
}

////////////////////////////////////////////////////////////////////////////////
///

QString CFSModel::GetName( const QModelIndex& Index ) const
{
    if( Index.isValid() == false || Index.row() < 0 || Index.row() >= vecNode_.size() )
        return "";

    return vecNode_.at( Index.row() )->Name_;
}

QVector<nsHC::TySpFileSource> CFSModel::GetChildItems( const QString& RootWithPath, bool IncludeDotDot /* = false */ )
{
    WIN32_FIND_DATA Wfd = { 0, };
    QVector< nsHC::TySpFileSource > VecRet;

    HANDLE hFind = FindFirstFileExW( ( RootWithPath + "\\*.*" ).toStdWString().c_str(),
                                     FindExInfoBasic, &Wfd,
                                     FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH );

    if( hFind == INVALID_HANDLE_VALUE )
        return {};

    if( IncludeDotDot == true )
    {
        // NOTE: FindFirstFileExW 는 . 과 .. 를 반환하지 않는다.
        const auto Fs = std::make_shared< nsHC::CFileSourceT >();

        Fs->Name_       = "..";
        Fs->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;

        VecRet.emplace_back( Fs );
    }

    do
    {
        if( ( wcscmp( Wfd.cFileName, L"." ) == 0 ) ||
            ( wcscmp( Wfd.cFileName, L".." ) == 0 ) )
            continue;

        const auto Fs = std::make_shared< nsHC::CFileSourceT >();

        Fs->Name_           = QString::fromWCharArray( Wfd.cFileName );
        Fs->Ext_            = nsCmn::nsCmnPath::GetFileExtension( Fs->Name_ );
        // Item.IsNormalizedByNFD = IsNormalizedString( NormalizationD, wfd.cFileName, -1 );
        // NOTE: OS 의 IsNormalizedString 의 정상적인 Latin1 문자열도 TRUE 를 반환한다. 
        if( Fs->Name_ != Fs->Name_.normalized( QString::NormalizationForm_C ) )
            SetFlag( Fs->Flags_, nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD );

        Fs->Attributes_     = Wfd.dwFileAttributes;
        Fs->Reserved0_      = Wfd.dwReserved0;
        Fs->Size_           = (static_cast< int64_t >( Wfd.nFileSizeHigh ) << 32) | ( Wfd.nFileSizeLow );

        Fs->Created_        = nsCmn::ConvertTo( Wfd.ftCreationTime, true );
        Fs->Modified_       = nsCmn::ConvertTo( Wfd.ftLastWriteTime, true );

        VecRet.emplace_back( Fs );

        if( QThread::currentThread()->isInterruptionRequested() == true )
            break;

    } while( FindNextFileW( hFind, &Wfd ) != FALSE );

    FindClose( hFind );
    return VecRet;
}

void CFSModel::multiData( const QModelIndex& index, QModelRoleDataSpan roleDataSpan ) const
{
    if( index.isValid() == false )
        return;

    const auto Row = index.row();
    const auto Col = index.column();

    if( Row < 0 || Row >= rowCount() )
        return;

    if( Col < 0 || Col >= columnView_.VecColumns.size() )
        return;

    CCmnMutexLocker Locker( &lock );

    // TODO: 항목이름을 나타나는 컬럼을 찾아낼 방법이 필요하다. 지금은 0 으로 하드코딩... 
    for( QModelRoleData& RoleWith : roleDataSpan )
    {
        const int Role = RoleWith.role();
        const auto& Item = vecNode_[ Row ];

        switch( Role )
        {
            case Qt::DisplayRole:
            case Qt::EditRole: {
                if( Item->VecContent.isEmpty() == true || Col >= Item->VecContent.size() )
                    RoleWith.setData( Item->Name_ );
                else
                    RoleWith.setData( Item->VecContent[ Col ] );
            } break;

            case Qt::DecorationRole: {
                if( Col == 0 )
                    RoleWith.setData( vecNode_[ Row ]->Icon );
            } break;

            case Qt::ForegroundRole: {
                if( vecRowColors_[ Row ].first.isValid() == true )
                    RoleWith.setData( vecRowColors_[ Row ].first );
                else
                    RoleWith.setData( QColor( "#FFFFFF" ) );
            } break;
            case Qt::BackgroundRole: {
                if( vecRowColors_[ Row ].second.isValid() == true )
                    RoleWith.setData( vecRowColors_[ Row ].second );
                else
                    RoleWith.setData( QColor( 0, 0, 0 ) );
            } break;
            case Qt::FontRole: {
                RoleWith.setData( colorScheme_.FileList_Font );

                if( Col == 0 )
                {
                    if( FlagOn( Item->Flags_, nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD ) )
                    {
                        QFont under( colorScheme_.FileList_Font );
                        under.setUnderline( true );
                        RoleWith.setData( under );
                    }
                }
            } break;

            case CFsModelT::USR_ROLE_SORT: {
                // 디렉토리 순으로 정렬하기 위해 디렉토리 이름 앞에 0 을 붙이고, 파일 이름 앞에는 1 을 붙인다.
                // 이로 인해 오름차순이나 내림차순으로 정렬하면 자연히 이에 따라 정렬된다.
                // QtitanDataGrid 에서 컬럼을 생성할 때 정렬에 사용할 Role 을 UserRole 로 지정한 컬럼만 적용된다.
                if( Col == 0 )
                {
                    if( Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY )
                    {
                        if( Item->Name_ == ".." || Item->Name_ == "." )
                            RoleWith.setData( "0_" + Item->Name_ );
                        else
                            RoleWith.setData( "1_" + Item->Name_ );
                    }
                    else
                    {
                        RoleWith.setData( "2_" + Item->Name_ );
                    }
                }
            } break;
            case CFsModelT::USR_ROLE_ATTRIBUTE: {
                RoleWith.setData( static_cast< quint32 >( Item->Attributes_ ) );
            } break;
            case CFsModelT::USR_ROLE_HIDDENOPACITY: {
                if( FlagOn( Item->Attributes_, FILE_ATTRIBUTE_HIDDEN ) ||
                    FlagOn( Item->Attributes_, FILE_ATTRIBUTE_SYSTEM ) )
                    RoleWith.setData( 0.5f );
                else
                    RoleWith.setData( 1.0f );
            } break;
        }
    }
}

void CFSModel::doRefresh()
{
    // Root + CurrentPath 의 대상이 압축파일인지 확인
    const auto Root = GetRoot();
    const auto Base = retrieveRootWithPath();

    /*!
     * GetFileAttributes 함수
     *  Local 경로/파일 성공
     *  Remote(SMB) 경로/파일 성공
     *  Remote(SMB) \\Server 만 있을 때, ERROR_BAD_PATHNAME 반환
     *      => FindFirstFile 로 검색하면 Share 목록을 도출할 수 있음
     */

    int FileCount = 0;
    int DirectoryCount = 0;
    uint64_t TotalSize = 0;

    QVector< TyPrFGWithBG > VecRowColors;
    QVector< nsHC::TySpFileSource > VecItems;
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();
    auto Attr = Root->GetCate() == nsHC::FS_CATE_VIRUAL ? FILE_ATTRIBUTE_VIRTUAL : GetFileAttributesW( Base.toStdWString().c_str() );
    if( Attr == INVALID_FILE_ATTRIBUTES || FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) )
        Attr = 0;
    if( Root->GetCate() == nsHC::FS_CATE_REMOTE )
        Attr = FILE_ATTRIBUTE_DIRECTORY;
    
    // NOTE: 해당 스레드에서 기본적인 파일 목록을 완성한다.
    // NOTE: 추가적으로 폴더 크기, 아이콘 등은 배경 스레드를 통해 완성시킨다. 

    if( ( FlagOn( Attr, FILE_ATTRIBUTE_DIRECTORY ) ) ||
        ( FlagOn( Attr, FILE_ATTRIBUTE_VIRTUAL ) && Root->GetCate() == nsHC::FS_CATE_VIRUAL ) )
    {
        // 파일 목록을 얻은 후, 스레드를 통해 아이콘 등을 획득한다.
        if( Root->GetCate() == nsHC::FS_CATE_REMOTE )
        {
            // NOTE: 서버 주소만 있다면 다른 방식으로 열거해야 한다.
            if( Base.startsWith( "\\\\" ) == true && Base.count( '\\' ) == 2 )
            {
                // 서버 주소, Share 열거해야 함
                VecItems += ( ( nsHC::CFSSmb* )Root.get() )->GetChildItems( Base, false );
            }
            else
            {
                if( FlagOn( Attr, FILE_ATTRIBUTE_DIRECTORY ) )
                    VecItems += GetChildItems( Base, true );
            }
        }
        else
        {
            if( FlagOn( Attr, FILE_ATTRIBUTE_DIRECTORY ) )
                VecItems += GetChildItems( Base, true );
            if( FlagOn( Attr, FILE_ATTRIBUTE_VIRTUAL ) && Root->GetCate() == nsHC::FS_CATE_VIRUAL )
                VecItems += ( ( nsHC::CFSShell* )Root.get() )->GetChildItems( nullptr, false );
        }
    }
    else
    {
        // NOTE: 압축파일인지 확인6
        if( FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) )
        {
            const auto Model = ( ( nsHC::CFSPack* )Root.get() )->Model();

            // 압축 파일을 빠져나가기 위해 .. 추가
            const auto Fs = std::make_shared< nsHC::CFileSourceT >();
            Fs->Name_ = "..";
            Fs->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
            VecItems.push_back( Fs );

            Model->ChDir( Base );

            for( const auto& Item : Model->FindFiles( "*.*" ) )
            {
                const auto Fs = std::make_shared< nsHC::CFileSourceT >();

                Fs->Name_       = Item->FileName;
                Fs->Ext_        = nsCmn::nsCmnPath::GetFileExtension( Fs->Name_ );
                if( Fs->Name_ != Fs->Name_.normalized( QString::NormalizationForm_C ) )
                    SetFlag( Fs->Flags_, nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD );
                Fs->Attributes_ = Item->Attributes;
                Fs->Size_       = Item->UncompressedSize;

                VecItems.push_back( Fs );
            }
        }
    }

    VecRowColors.resize( VecItems.count(), qMakePair( colorScheme_.FileList_FGColor, colorScheme_.FileList_BGColor ) );
    createBuiltFsValues( VecItems );

    for( qsizetype idx = 0; idx < VecItems.size(); ++idx )
    {
        const auto& Item = VecItems[ idx ];

        if( FlagOn( Item->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
            DirectoryCount++;
        else
        {
            FileCount++;
            TotalSize += Item->Size_;
        }

        for( const auto& FileSet : vecFileSetColors_ )
        {
            if( StColorSchemeMgr->JudgeFileSet( FileSet.first, Item.get(), nullptr, nullptr ) == false )
                continue;

            VecRowColors[ idx ] = FileSet.second;
            break;
        }
    }

    {
        beginResetModel();
        if( FsIconGatherer_ != nullptr )
            FsIconGatherer_->requestInterruption();
        if( DirSizeGatherer_ != nullptr )
            DirSizeGatherer_->requestInterruption();
        qSwap( VecItems, vecNode_ );
        qSwap( VecRowColors, vecRowColors_ );
        qSwap( FileCount, fileCount_ );
        qSwap( DirectoryCount, directoryCount_ );
        qSwap( TotalSize, totalSize_ );
        endResetModel();
    }

    if( FsIconGatherer_ == nullptr )
    {
        FsIconGatherer_ = new CFSIconGatherer( this, vecNode_, GetCurrentPathWithRoot(), &lock );
        FsIconGatherer_->setAutoDelete( false );
    }

    FsIconGatherer_->clearInterruption();
    QThreadPool::globalInstance()->start( FsIconGatherer_ );

    if( !FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) )
    {
        if( DirSizeGatherer_ == nullptr )
        {
            int ColSize = -1;
            for( const auto& ColView : columnView_.VecColumns )
            {
                if( ColView.Content.contains( BUILTIN_COL_FS_SIZE ) == false )
                    continue;

                    ColSize = ColView.Index;
                    break;
            }

            DirSizeGatherer_ = new CFSDirSizeGatherer( this, vecNode_, GetCurrentPathWithRoot(), ColSize, &lock );
            DirSizeGatherer_->setAutoDelete( false );
        }

        DirSizeGatherer_->clearInterruption();
        DirSizeGatherer_->setCurrentPath( GetCurrentPathWithRoot() );
        QThreadPool::globalInstance()->start( DirSizeGatherer_ );
    }
}

QString CFSModel::retrieveRootWithPath() const
{
    const auto RootCount = GetRootCount();

    if( RootCount == 1 )
    {
        const auto Root = GetRoot();
        return Root->GetRoot() + GetCurrentPath();
    }

    return GetCurrentPath();
}

void CFSModel::createBuiltFsValues( QVector<nsHC::TySpFileSource>& Items )
{
    // 컬럼 서식 변환
    for( auto& Col : columnView_.VecColumns )
    {
        if( Col.Content_CVT.isEmpty() == true && Col.Content.isEmpty() == false )
        {
            Col.Content_CVT.resize( Col.Content.length() + 1 );
            Col.Content.toWCharArray( Col.Content_CVT.data() );
        }
    }

    // 컬럼에 대한 데이터 생성
    for( const auto& Item : Items )
    {
        for( const auto& Col : columnView_.VecColumns )
        {
            QString Content;
            bool IsParsed = false;
            auto Fmt = const_cast< wchar_t* >( Col.Content_CVT.data() );

            do
            {
                // TODO: ColumnParseResult 를 미리 만들어 두면 더욱 빨라질 수 있다.
                ColumnParseResult Result;
                IsParsed = CColumnMgr::Parse( Fmt, Result, Content );

                if( IsParsed == true )
                    CColumnMgr::CreateColumnContent( Result, Item.get(), Content);

            } while( IsParsed == true );

            Item->VecContent.push_back( Content );
        }
    }
}

void CFSModel::createBuiltFsValues( const nsHC::TySpFileSource& Item, int Column )
{
    if( Column < 0 || Column >= columnView_.VecColumns.size() )
        return;

    const auto& Col = columnView_.VecColumns[ Column ];

    QString Content;
    bool IsParsed = false;
    auto Fmt = const_cast< wchar_t* >( Col.Content_CVT.data() );

    do
    {
        // TODO: ColumnParseResult 를 미리 만들어 두면 더욱 빨라질 수 있다.
        ColumnParseResult Result;
        IsParsed = CColumnMgr::Parse( Fmt, Result, Content );

        if( IsParsed == true )
            CColumnMgr::CreateColumnContent( Result, Item.get(), Content );

    } while( IsParsed == true );

    Item->VecContent[ Column ] = Content;
}


//DWORD FSModel::Rename( const QModelIndex& Index, const QString& NewName )
//{
//    const auto& Old = GetFileFullPath( Index ).toStdWString();
//    const auto& New = GetFileFullPath( NewName ).toStdWString();
//
//    const auto Ret = MoveFileExW( Old.c_str(), New.c_str(), 0 );
//    return Ret != FALSE ? ERROR_SUCCESS : GetLastError();
//}
//
//DWORD FSModel::MakeDirectory( const QString& NewName )
//{
//    const auto New = GetFileFullPath( NewName ).toStdWString();
//    const auto Ret = SHCreateDirectoryExW( nullptr, New.c_str(), nullptr );
//
//    if( Ret == ERROR_SUCCESS )
//    {
//        // TODO: Refresh 함수에서 파일 추가, 디렉토리 추가를 별도 함수로 만든 후 이곳에 추가한다.
//        Node Item;
//
//        Item.Attiributes    = FILE_ATTRIBUTE_DIRECTORY;
//        Item.Name           = NewName.section( '\\', 0 );
//        Item.Created        = QDateTime::currentDateTime();
//        Item.Icon           = Icon_Directory;
//
//        // 컬럼에 대한 데이터 생성
//        for( const auto& Col : CurrentView.VecColumns )
//        {
//            const auto Def = Col.Content.toStdWString();
//
//            auto Fmt = const_cast< wchar_t* >( Def.c_str() );
//            bool IsParsed = false;
//            QString Content;
//
//            do
//            {
//                ColumnParseResult Result;
//                IsParsed = CColumnMgr::Parse( Fmt, Result, Content );
//
//                if( IsParsed == true )
//                    CColumnMgr::CreateColumnContent( Result, &Item, Content );
//
//            } while( IsParsed == true );
//
//            Item.VecContent.push_back( Content );
//        }
//
//        const auto Row = rowCount();
//
//        beginInsertRows( QModelIndex(), Row, Row );
//        VecNode.push_back( Item );
//        endInsertRows();
//        DirectoryCount++;
//    }
//
//    return Ret;
//}
//
//void FSModel::Refresh()
//{
//    HRESULT hRet = OleInitialize( 0 );
//    
//    int FileCount_Refresh = 0;
//    int DirectoryCount_Refresh = 0;
//    int64_t TotalSize_Refresh = 0;
//    QVector< TyPrFBWithBG > VecRowColors_Refresh;
//    const auto VecFileSet = TyStFileSetMgr::GetInstance()->GetNames();
//    TyStColorSchemeMgr::GetInstance()->SetCurrentColorScheme( "Default" );
//
//    if( Icon_Directory.isNull() == true )
//    {
//        SHFILEINFOW SHInfo = { 0, };
//        if( SHGetFileInfoW( L"룽슈멜", FILE_ATTRIBUTE_DIRECTORY, &SHInfo, sizeof( SHFILEINFOW ), SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME ) != FALSE )
//            Icon_Directory = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
//    }
//
//    const auto Base = CurrentPath.length() == 1 ? Root + CurrentPath : Root + CurrentPath + "\\";
//

//    QVector< Node > Nodes;
//
//    if( CurrentPath.length() != 1 )
//    {
//        Node Item;
//
//        Item.Attiributes = FILE_ATTRIBUTE_DIRECTORY;
//        Item.Name = "..";
//        Item.Size = 0;
//        if( Icon_Directory.isNull() == false )
//            Item.Icon = Icon_Directory;
//
//        Nodes.push_back( Item );
//
//        TyPrFBWithBG Colors;
//        for( const auto& FileSetName : VecFileSet )
//        {
//            if( TyStColorSchemeMgr::GetInstance()->JudgeFileSet( TyStFileSetMgr::GetInstance()->GetFileSet( FileSetName ), Item, &Colors.first, &Colors.second ) == false )
//                continue;
//
//            break;
//        }
//
//        VecRowColors_Refresh.push_back( Colors );
//    }
//
//    WIN32_FIND_DATA wfd = { 0 };
//    const auto Filter = Base + "*.*";
//    HANDLE hFile = FindFirstFileExW( Filter.toStdWString().c_str(),
//                                     FindExInfoBasic,
//                                     &wfd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH );
//
//    QFileIconProvider Provider;
//    
//    if( hFile != INVALID_HANDLE_VALUE )
//    {
//        do
//        {
//            if( wcscmp( wfd.cFileName, L"." ) == 0 ||
//                wcscmp( wfd.cFileName, L".." ) == 0 )
//                continue;
//
//            Node Item;
//
//            Item.Attiributes    = wfd.dwFileAttributes;
//            Item.Reserved0      = wfd.dwReserved0;
//            Item.Name           = QString::fromWCharArray( wfd.cFileName );
//            // Item.IsNormalizedByNFD = IsNormalizedString( NormalizationD, wfd.cFileName, -1 );
//            // NOTE: OS 의 IsNormalizedString 의 정상적인 Latin1 문자열도 TRUE 를 반환한다. 
//            Item.IsNormalizedByNFD = Item.Name != Item.Name.normalized( QString::NormalizationForm_C );
//            Item.Ext            = nsCmn::nsCmnPath::GetFileExtension( Item.Name );
//            if( FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
//            {
//                if( MapDirNameToSize.contains( Item.Name ) == true )
//                    Item.Size = MapDirNameToSize[ Item.Name ];
//                else
//                    Item.Size = 0;
//            }
//            else
//            {
//                Item.Size       = (static_cast< int64_t >( wfd.nFileSizeHigh ) << 32) | (wfd.nFileSizeLow);
//            }
//            Item.Created        = nsCmn::ConvertTo( wfd.ftCreationTime, false );
//            Item.Modified       = nsCmn::ConvertTo( wfd.ftLastWriteTime, false );
//
//            if( FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
//            {
//                if( Icon_Directory.isNull() == false )
//                    Item.Icon = Icon_Directory;
//
//                DirectoryCount_Refresh++;
//            }
//            else
//            {
//                Item.Icon = Provider.icon( QFileInfo( Base + Item.Name ) ).pixmap( 24, 24 );
//
//                //do
//                //{
//                //    SHFILEINFOW SHInfo = { 0, };
//                //    const auto File = Item.Name.toStdWString();
//                //    SHGetFileInfoW( File.c_str(), FILE_ATTRIBUTE_NORMAL, &SHInfo, sizeof( SHFILEINFOW ),
//                //                    SHGFI_USEFILEATTRIBUTES | SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON );
//
//                //    if( SHInfo.hIcon != Q_NULLPTR )
//                //    {
//                //        Item.Icon = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
//                //        DestroyIcon( SHInfo.hIcon );
//                //    }
//
//                //} while( false );
//
//                FileCount_Refresh++;
//                TotalSize_Refresh += Item.Size;
//            }
//
//            Nodes.push_back( Item );
//
//            TyPrFBWithBG Colors;
//            for( const auto& FileSetName : VecFileSet )
//            {
//                if( TyStColorSchemeMgr::GetInstance()->JudgeFileSet( TyStFileSetMgr::GetInstance()->GetFileSet( FileSetName ), Item, &Colors.first, &Colors.second ) == false )
//                    continue;
//
//                break;
//            }
//
//            VecRowColors_Refresh.push_back( Colors );
//
//        } while( FindNextFileW( hFile, &wfd ) != 0 );
//
//        FindClose( hFile );
//    }
//
//    beginResetModel();
//    qSwap( Nodes, VecNode );
//    FileCount = FileCount_Refresh;
//    DirectoryCount = DirectoryCount_Refresh;
//    TotalSize = TotalSize_Refresh;
//    qSwap( VecRowColors_Refresh, VecRowColors );
//    endResetModel();
//
//    CoUninitialize();
//}
//
//
//bool FSModel::setData( const QModelIndex& index, const QVariant& value, int role )
//{
//    if( role == Qt::EditRole )
//    {
//        if( index.isValid() == false )
//            return QAbstractItemModel::setData( index, value, role );
//
//        const auto Row = index.row();
//        const auto Col = index.column();
//
//        if( Row < 0 || Row >= VecNode.size() )
//            return QAbstractItemModel::setData( index, value, role );
//
//        if( Col < 0 || Col >= CurrentView.VecColumns.size() )
//            return QAbstractItemModel::setData( index, value, role );
//
//        if( Col == 0 )
//        {
//            auto& Node = VecNode[ Row ];
//            Node.Name = value.toString();
//            Node.IsNormalizedByNFD = FALSE;
//            Node.VecContent[ Col ] = value;
//            emit dataChanged( index, index, QList<int>() << Qt::DisplayRole << Qt::EditRole << Qt::FontRole );
//            return true;
//        }
//    }
//
//    return QAbstractItemModel::setData( index, value, role );
//}

////////////////////////////////////////////////////////////////////////////////
///

void CFSProxyModel::SetHiddenSystem( bool IsShow )
{
    IsShowHiddenSystem_ = IsShow;
    
    invalidateFilter();
}

bool CFSProxyModel::GetHiddenSystem() const
{
    return IsShowHiddenSystem_;
}

bool CFSProxyModel::filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const
{
    QModelIndex Index = sourceModel()->index( source_row, 0, source_parent );

    if( Index.isValid() && !source_parent.isValid() )
    {
        const auto Attr = Index.data( CFSModel::USR_ROLE_ATTRIBUTE ).toUInt();
        if( ( FlagOn( Attr, FILE_ATTRIBUTE_HIDDEN ) ) || ( FlagOn( Attr, FILE_ATTRIBUTE_SYSTEM ) ) )
            return IsShowHiddenSystem_;
    }

    return QSortFilterProxyModel::filterAcceptsRow( source_row, source_parent );
}
