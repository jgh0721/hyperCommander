﻿#include "stdafx.h"
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

#include "colorSchemeMgr.hpp"
#include "fileSetMgr.hpp"

QModelIndex FSModel::index( int row, int column, const QModelIndex& parent ) const
{
    if( row < 0 || column < 0 || row >= rowCount( parent ) || column >= columnCount( parent ) )
        return QModelIndex();

    return createIndex( row, column, ( PVOID )&VecNode[ row ] );
}

void FSModel::SetRoot( const QString& Root )
{
    this->Root = Root;
}

void FSModel::SetCurrentPath( const QString& Path )
{
    CurrentPath = Path;
}

QString FSModel::GetCurrentPath() const
{
    return CurrentPath;
}

void FSModel::SetColumnView( const ColumnView& CV)
{
    CurrentView = CV;
}

QString FSModel::GetName( const QModelIndex& Index ) const
{
    if( Index.isValid() == false || Index.row() < 0 || Index.row() >= VecNode.size() )
        return "";

    return VecNode.at( Index.row() ).Name;
}

QString FSModel::GetFileFullPath( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid ) == false )
        return "";

    const auto& Item = VecNode.at( Index.row() );
    return QString( "%1%2\\%3" ).arg( Root ).arg( CurrentPath.length() == 1 ? "" : CurrentPath ).arg( Item.Name );
}

QString FSModel::GetFileFullPath( const QString& Name ) const
{
    return QString( "%1%2\\%3" ).arg( Root ).arg( CurrentPath.length() == 1 ? "" : CurrentPath ).arg( Name );
}

Node FSModel::GetFileInfo( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid | CheckIndexOption::DoNotUseParent ) == false )
        return {};

    return VecNode.at( Index.row() );
}

bool FSModel::IsRoot() const
{
    return CurrentPath.length() == 1;
}

void FSModel::SetColorScheme( const TyColorScheme& ColorScheme )
{
    this->ColorScheme = ColorScheme;
}

void FSModel::ChangeDirectory( const QString& Child )
{
    if( IsRoot() == true )
        SetCurrentPath( QDir::toNativeSeparators( QDir::cleanPath( GetCurrentPath() + Child ) ) );
    else
        SetCurrentPath( QDir::toNativeSeparators( QDir::cleanPath( GetCurrentPath() + "\\" + Child ) ) );

    Refresh();

    emit sigChangedDirectory( QDir::toNativeSeparators( Root + GetCurrentPath() ) );
}

void FSModel::ChangeDirectory( const QModelIndex& Child )
{
    const auto& Node = GetFileInfo( Child );
    if( Node.Name.isEmpty() == true )
        ChangeDirectory( "" );
    else
    {
        if( FlagOn( Node.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            if( FlagOn( Node.Attiributes, FILE_ATTRIBUTE_REPARSE_POINT ) )
            {
                if( FlagOn( Node.Reserved0, IO_REPARSE_TAG_SYMLINK ) ||            // SYMLINKD
                    FlagOn( Node.Reserved0, IO_REPARSE_TAG_MOUNT_POINT ) )         // JUNTION
                {
                    const auto s = nsCmn::nsCmnPath::GetReparsePointTo( GetFileFullPath( Child ) );

                    if( s.length() > 2 && s.contains( ':' ) == true )
                    {
                        SetRoot( s.left( 2 ) );
                        SetCurrentPath( s.mid( 2 ) );

                        Refresh();

                        emit sigChangedDirectory( QDir::toNativeSeparators( Root + GetCurrentPath() ) );
                        return;
                    }
                    else
                    {
                        // TODO: 오류 처리
                    }
                }
            }

            ChangeDirectory( Node.Name );
        }
    }
}

int FSModel::GetFileCount() const
{
    return FileCount;
}

int FSModel::GetDirectoryCount() const
{
    return DirectoryCount;
}

int64_t FSModel::GetTotalSize() const
{
    return TotalSize;
}

DWORD FSModel::Rename( const QModelIndex& Index, const QString& NewName )
{
    const auto& Old = GetFileFullPath( Index ).toStdWString();
    const auto& New = GetFileFullPath( NewName ).toStdWString();

    const auto Ret = MoveFileExW( Old.c_str(), New.c_str(), 0 );
    return Ret != FALSE ? ERROR_SUCCESS : GetLastError();
}

DWORD FSModel::MakeDirectory( const QString& NewName )
{
    const auto New = GetFileFullPath( NewName ).toStdWString();
    const auto Ret = SHCreateDirectoryExW( nullptr, New.c_str(), nullptr );

    if( Ret == ERROR_SUCCESS )
    {
        // TODO: Refresh 함수에서 파일 추가, 디렉토리 추가를 별도 함수로 만든 후 이곳에 추가한다.
        Node Item;

        Item.Attiributes    = FILE_ATTRIBUTE_DIRECTORY;
        Item.Name           = NewName.section( '\\', 0 );
        Item.Created        = QDateTime::currentDateTime();
        Item.Icon           = Icon_Directory;

        // 컬럼에 대한 데이터 생성
        for( const auto& Col : CurrentView.VecColumns )
        {
            const auto Def = Col.Content.toStdWString();

            auto Fmt = const_cast< wchar_t* >( Def.c_str() );
            bool IsParsed = false;
            QString Content;

            do
            {
                ColumnParseResult Result;
                IsParsed = CColumnMgr::Parse( Fmt, Result, Content );

                if( IsParsed == true )
                    CColumnMgr::CreateColumnContent( Result, &Item, Content );

            } while( IsParsed == true );

            Item.VecContent.push_back( Content );
        }

        const auto Row = rowCount();

        beginInsertRows( QModelIndex(), Row, Row );
        VecNode.push_back( Item );
        endInsertRows();
        DirectoryCount++;
    }

    return Ret;
}

QVector<Node> FSModel::GetChildItems( const QString& Path )
{
    QVector< Node > Ret;
    WIN32_FIND_DATA Wfd = { 0, };
    HANDLE hFind = FindFirstFileExW( ( Path + "\\*.*" ).toStdWString().c_str(),
                                     FindExInfoBasic, &Wfd,
                                     FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH );

    if( hFind == INVALID_HANDLE_VALUE )
        return {};

    do
    {
        if( wcscmp( Wfd.cFileName, L"." ) == 0 ||
            wcscmp( Wfd.cFileName, L".." ) == 0 )
            continue;

        Node Item;

        Item.Attiributes    = Wfd.dwFileAttributes;
        Item.Reserved0      = Wfd.dwReserved0;
        Item.Name           = QString::fromWCharArray( Wfd.cFileName );
        Item.IsNormalizedByNFD = IsNormalizedString( NormalizationD, Wfd.cFileName, -1 );
        Item.Ext            = nsCmn::nsCmnPath::GetFileExtension( Item.Name );
        Item.Size           = (static_cast< int64_t >( Wfd.nFileSizeHigh ) << 32) | ( Wfd.nFileSizeLow );
        Item.Created        = nsCmn::ConvertTo( Wfd.ftCreationTime, false );
        Item.Modified       = nsCmn::ConvertTo( Wfd.ftLastWriteTime, false );

        Ret.emplace_back( Item );

        if( QThread::currentThread()->isInterruptionRequested() == true )
            break;

    } while( FindNextFileW( hFind, &Wfd ) != FALSE );

    FindClose( hFind );
    return Ret;
}

void FSModel::Refresh()
{
    HRESULT hRet = OleInitialize( 0 );
    
    int FileCount_Refresh = 0;
    int DirectoryCount_Refresh = 0;
    int64_t TotalSize_Refresh = 0;
    QVector< TyPrFBWithBG > VecRowColors_Refresh;
    const auto VecFileSet = TyStFileSetMgr::GetInstance()->GetNames();
    TyStColorSchemeMgr::GetInstance()->SetCurrentColorScheme( "Default" );

    if( Icon_Directory.isNull() == true )
    {
        SHFILEINFOW SHInfo = { 0, };
        if( SHGetFileInfoW( L"룽슈멜", FILE_ATTRIBUTE_DIRECTORY, &SHInfo, sizeof( SHFILEINFOW ), SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME ) != FALSE )
            Icon_Directory = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
    }

    const auto Base = CurrentPath.length() == 1 ? Root + CurrentPath : Root + CurrentPath + "\\";

    QMap< QString, qint64 > MapDirNameToSize;

    //QtConcurrent::run( [&MapDirNameToSize, Base]() {

    //    Everything_SetMatchPath( TRUE );
    //    Everything_SetMatchWholeWord( TRUE );
    //    Everything_SetRequestFlags( EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH | EVERYTHING_REQUEST_SIZE );
    //    Everything_SetSort( EVERYTHING_SORT_PATH_ASCENDING );

    //    Everything_SetSearchW( Base.toStdWString().c_str() );
    //    Everything_QueryW( TRUE );

    //    QVector< uint32_t > VecResults( Everything_GetNumResults() );
    //    std::iota( VecResults.begin(), VecResults.end(), 0 );

    //    std::vector< wchar_t > BaseBuffer( Base.length() + 1 );
    //    Base.toWCharArray( &BaseBuffer[ 0 ] );

    //    bool IsContinue = true;
    //    VecResults = QtConcurrent::blockingFiltered( VecResults, [BaseBuffer, &IsContinue]( uint32_t Row ) {
    //        if( IsContinue == false ) return false;

    //        if( Everything_IsFolderResult( Row ) == FALSE )
    //            return false;

    //        if( wcsstr( BaseBuffer.data(), Everything_GetResultPathW( Row ) ) == nullptr )
    //        {
    //            IsContinue = false;
    //            return false;
    //        }

    //        return true;
    //    } );

    //    MapDirNameToSize = QtConcurrent::blockingMappedReduced<decltype( MapDirNameToSize )>( VecResults, [Base]( uint32_t Row ) {
    //        QPair< QString, LARGE_INTEGER > Result;
    //        if( Everything_GetResultSize( Row, &Result.second ) != FALSE )
    //            Result.first = QString::fromWCharArray( Everything_GetResultFileNameW( Row ) );

    //        return Result;
    //    }, [Base]( QMap< QString, qint64 >& Result, const QPair< QString, LARGE_INTEGER >& Value ) {
    //        if( Value.first.isEmpty() == false )
    //            Result[ Value.first ] = Value.second.QuadPart;
    //    } );

    //} ).waitForFinished();

    QVector< Node > Nodes;

    if( CurrentPath.length() != 1 )
    {
        Node Item;

        Item.Attiributes = FILE_ATTRIBUTE_DIRECTORY;
        Item.Name = "..";
        Item.Size = 0;
        if( Icon_Directory.isNull() == false )
            Item.Icon = Icon_Directory;

        Nodes.push_back( Item );

        TyPrFBWithBG Colors;
        for( const auto& FileSetName : VecFileSet )
        {
            if( TyStColorSchemeMgr::GetInstance()->JudgeFileSet( TyStFileSetMgr::GetInstance()->GetFileSet( FileSetName ), Item, &Colors.first, &Colors.second ) == false )
                continue;

            break;
        }

        VecRowColors_Refresh.push_back( Colors );
    }

    WIN32_FIND_DATA wfd = { 0 };
    const auto Filter = Base + "*.*";
    HANDLE hFile = FindFirstFileExW( Filter.toStdWString().c_str(),
                                     FindExInfoBasic,
                                     &wfd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH );

    QFileIconProvider Provider;
    
    if( hFile != INVALID_HANDLE_VALUE )
    {
        do
        {
            if( wcscmp( wfd.cFileName, L"." ) == 0 ||
                wcscmp( wfd.cFileName, L".." ) == 0 )
                continue;

            Node Item;

            Item.Attiributes    = wfd.dwFileAttributes;
            Item.Reserved0      = wfd.dwReserved0;
            Item.Name           = QString::fromWCharArray( wfd.cFileName );
            // Item.IsNormalizedByNFD = IsNormalizedString( NormalizationD, wfd.cFileName, -1 );
            // NOTE: OS 의 IsNormalizedString 의 정상적인 Latin1 문자열도 TRUE 를 반환한다. 
            Item.IsNormalizedByNFD = Item.Name != Item.Name.normalized( QString::NormalizationForm_C );
            Item.Ext            = nsCmn::nsCmnPath::GetFileExtension( Item.Name );
            if( FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                if( MapDirNameToSize.contains( Item.Name ) == true )
                    Item.Size = MapDirNameToSize[ Item.Name ];
                else
                    Item.Size = 0;
            }
            else
            {
                Item.Size       = (static_cast< int64_t >( wfd.nFileSizeHigh ) << 32) | (wfd.nFileSizeLow);
            }
            Item.Created        = nsCmn::ConvertTo( wfd.ftCreationTime, false );
            Item.Modified       = nsCmn::ConvertTo( wfd.ftLastWriteTime, false );

            if( FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                if( Icon_Directory.isNull() == false )
                    Item.Icon = Icon_Directory;

                DirectoryCount_Refresh++;
            }
            else
            {
                Item.Icon = Provider.icon( QFileInfo( Base + Item.Name ) ).pixmap( 24, 24 );

                //do
                //{
                //    SHFILEINFOW SHInfo = { 0, };
                //    const auto File = Item.Name.toStdWString();
                //    SHGetFileInfoW( File.c_str(), FILE_ATTRIBUTE_NORMAL, &SHInfo, sizeof( SHFILEINFOW ),
                //                    SHGFI_USEFILEATTRIBUTES | SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON );

                //    if( SHInfo.hIcon != Q_NULLPTR )
                //    {
                //        Item.Icon = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
                //        DestroyIcon( SHInfo.hIcon );
                //    }

                //} while( false );

                FileCount_Refresh++;
                TotalSize_Refresh += Item.Size;
            }

            // 컬럼에 대한 데이터 생성
            for( const auto& Col : CurrentView.VecColumns )
            {
                const auto Def = Col.Content.toStdWString();

                auto Fmt = const_cast< wchar_t* >( Def.c_str() );
                bool IsParsed = false;
                QString Content;

                do
                {
                    // TODO: ColumnParseResult 를 미리 만들어 두면 더욱 빨라질 수 있다.
                    ColumnParseResult Result;
                    IsParsed = CColumnMgr::Parse( Fmt, Result, Content );

                    if( IsParsed == true )
                        CColumnMgr::CreateColumnContent( Result, &Item, Content );

                } while( IsParsed == true );

                Item.VecContent.push_back( Content );
            }

            Nodes.push_back( Item );

            TyPrFBWithBG Colors;
            for( const auto& FileSetName : VecFileSet )
            {
                if( TyStColorSchemeMgr::GetInstance()->JudgeFileSet( TyStFileSetMgr::GetInstance()->GetFileSet( FileSetName ), Item, &Colors.first, &Colors.second ) == false )
                    continue;

                break;
            }

            VecRowColors_Refresh.push_back( Colors );

        } while( FindNextFileW( hFile, &wfd ) != 0 );

        FindClose( hFile );
    }

    beginResetModel();
    qSwap( Nodes, VecNode );
    FileCount = FileCount_Refresh;
    DirectoryCount = DirectoryCount_Refresh;
    TotalSize = TotalSize_Refresh;
    qSwap( VecRowColors_Refresh, VecRowColors );
    endResetModel();

    CoUninitialize();
}


QModelIndex FSModel::parent( const QModelIndex& child ) const
{
    if (child.isValid() == true)
        return QModelIndex();

    return QModelIndex();
}

int FSModel::rowCount( const QModelIndex& parent ) const
{
    if( parent.isValid() == true )
        return 0;

    return VecNode.size();
}

int FSModel::columnCount( const QModelIndex& parent ) const
{
    if( parent.isValid() == true )
        return 0;

    return CurrentView.VecColumns.size();
}

QVariant FSModel::data( const QModelIndex& index, int role ) const
{
    QModelRoleData roleData( role );
    multiData( index, roleData );
    return roleData.data();
}

bool FSModel::setData( const QModelIndex& index, const QVariant& value, int role )
{
    if( role == Qt::EditRole )
    {
        if( index.isValid() == false )
            return QAbstractItemModel::setData( index, value, role );

        const auto Row = index.row();
        const auto Col = index.column();

        if( Row < 0 || Row >= VecNode.size() )
            return QAbstractItemModel::setData( index, value, role );

        if( Col < 0 || Col >= CurrentView.VecColumns.size() )
            return QAbstractItemModel::setData( index, value, role );

        if( Col == 0 )
        {
            auto& Node = VecNode[ Row ];
            Node.Name = value.toString();
            Node.IsNormalizedByNFD = FALSE;
            Node.VecContent[ Col ] = value;
            emit dataChanged( index, index, QList<int>() << Qt::DisplayRole << Qt::EditRole << Qt::FontRole );
            return true;
        }
    }

    return QAbstractItemModel::setData( index, value, role );
}

QVariant FSModel::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if( orientation != Qt::Horizontal )
        return {};

    if( role != Qt::DisplayRole )
        return {};
    if( section < 0 || section >= CurrentView.VecColumns.size() )
        return {};

    return CurrentView.VecColumns[ section ].Name;
}

Qt::ItemFlags FSModel::flags( const QModelIndex& index ) const
{
    const auto Def = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return index.column() == 0 ? Def | Qt::ItemIsEditable : Def;
}

void FSModel::multiData( const QModelIndex& index, QModelRoleDataSpan roleDataSpan ) const
{
    if( index.isValid() == false )
        return;

    const auto Row = index.row();
    const auto Col = index.column();

    if( Row < 0 || Row >= VecNode.size() )
        return;

    if( Col < 0 || Col >= CurrentView.VecColumns.size() )
        return;

    // TODO: 항목이름을 나타나는 컬럼을 찾아낼 방법이 필요하다. 지금은 0 으로 하드코딩... 
    for( QModelRoleData& RoleWith : roleDataSpan )
    {
        const int Role = RoleWith.role();
        const auto& Item = VecNode[ Row ];

        switch( Role )
        {
            case Qt::DisplayRole:
            case Qt::EditRole: {
                if( Item.VecContent.isEmpty() == true || Col >= Item.VecContent.size() )
                    RoleWith.setData( Item.Name );
                else
                    RoleWith.setData( Item.VecContent[ Col ] );
            } break;

            case Qt::DecorationRole: {
                if( Col == 0 )
                    RoleWith.setData( VecNode[ Row ].Icon );
            } break;

            case Qt::ForegroundRole: {
                if( VecRowColors[ Row ].first.isValid() == true )
                    RoleWith.setData( VecRowColors[ Row ].first );
                else
                    RoleWith.setData( QColor( "#FFFFFF" ) );
            } break;
            case Qt::BackgroundRole: {
                if( VecRowColors[ Row ].second.isValid() == true )
                    RoleWith.setData( VecRowColors[ Row ].second );
                else
                    RoleWith.setData( QColor( 0, 0, 0 ) );
            } break;
            case Qt::FontRole: {
                if( Col == 0 )
                {
                    if( Item.IsNormalizedByNFD == TRUE )
                    {
                        QFont a;
                        a.setUnderline( true );
                        RoleWith.setData( a );
                    }
                }

            } break;

            case FSModel::USR_ROLE_SORT: {
                // 디렉토리 순으로 정렬하기 위해 디렉토리 이름 앞에 0 을 붙이고, 파일 이름 앞에는 1 을 붙인다.
                // 이로 인해 오름차순이나 내림차순으로 정렬하면 자연히 이에 따라 정렬된다.
                // QtitanDataGrid 에서 컬럼을 생성할 때 정렬에 사용할 Role 을 UserRole 로 지정한 컬럼만 적용된다.
                if( Col == 0 )
                {
                    if( Item.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
                    {
                        if( Item.Name == ".." || Item.Name == "." )
                            RoleWith.setData( "0_" + Item.Name );
                        else
                            RoleWith.setData( "1_" + Item.Name );
                    }
                    else
                    {
                        RoleWith.setData( "2_" + Item.Name );
                    }
                }
            } break;
            case FSModel::USR_ROLE_ATTRIBUTE: {
                RoleWith.setData( static_cast< quint32 >( Item.Attiributes ) );
            } break;
            case FSModel::USR_ROLE_HIDDENOPACITY: {
                if( FlagOn( Item.Attiributes, FILE_ATTRIBUTE_HIDDEN ) || 
                    FlagOn( Item.Attiributes, FILE_ATTRIBUTE_SYSTEM ) )
                    RoleWith.setData( 0.5f );
                else
                    RoleWith.setData( 1.0f );
            } break;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
///

void FSProxyModel::SetHiddenSystem( bool IsShow )
{
    IsShowHiddenSystem_ = IsShow;
    
    invalidateFilter();
}

bool FSProxyModel::GetHiddenSystem() const
{
    return IsShowHiddenSystem_;
}

bool FSProxyModel::filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const
{
    QModelIndex Index = sourceModel()->index( source_row, 0, source_parent );

    if( Index.isValid() && !source_parent.isValid() )
    {
        const auto Attr = Index.data( FSModel::USR_ROLE_ATTRIBUTE ).toUInt();
        if( ( FlagOn( Attr, FILE_ATTRIBUTE_HIDDEN ) ) || ( FlagOn( Attr, FILE_ATTRIBUTE_SYSTEM ) ) )
            return IsShowHiddenSystem_;
    }

    return QSortFilterProxyModel::filterAcceptsRow( source_row, source_parent );
}
