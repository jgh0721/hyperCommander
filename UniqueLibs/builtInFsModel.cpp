#include "stdafx.h"
#include "builtInFsModel.hpp"

#include <ObjectArray.h>

#include "commonLibs/cmnDateTime.hpp"

#include <QtConcurrent>

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

QString FSModel::GetFileFullPath( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid ) == false )
        return "";

    const auto& Item = VecNode.at( Index.row() );
    return QString( "%1%2\\%3" ).arg( Root ).arg( CurrentPath.length() == 1 ? "" : CurrentPath ).arg( Item.Name );
}

Node FSModel::GetFileInfo( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid ) == false )
        return {};

    return VecNode.at( Index.row() );
}

bool FSModel::IsRoot() const
{
    return CurrentPath.length() == 1;
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

void FSModel::Refresh()
{
    OleInitialize( 0 );

    WIN32_FIND_DATA wfd = { 0 };
    const auto Filter = CurrentPath.length() == 1 ? Root + CurrentPath + "*.*" : Root + CurrentPath + "\\*.*";
    HANDLE hFile = FindFirstFileExW( Filter.toStdWString().c_str(),
                                     FindExInfoBasic,
                                     &wfd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH );
    
    QVector< Node > Nodes;

    if( hFile != INVALID_HANDLE_VALUE )
    {
        if( CurrentPath.length() != 1  )
        {
            Node Item;

            Item.Attiributes = FILE_ATTRIBUTE_DIRECTORY;
            Item.Name = "..";
            Item.Size = 0;

            Nodes.push_back( Item );
        }

        do
        {
            if( wcscmp( wfd.cFileName, L"." ) == 0 ||
                wcscmp( wfd.cFileName, L".." ) == 0 )
                continue;

            Node Item;

            Item.Attiributes    = wfd.dwFileAttributes;
            Item.Name           = QString::fromWCharArray( wfd.cFileName );
            Item.Size           = ((int64_t)wfd.nFileSizeHigh << 32) | (wfd.nFileSizeLow);
            Item.Created        = nsCmn::ConvertTo( wfd.ftCreationTime, false );
            Item.Modified       = nsCmn::ConvertTo( wfd.ftLastWriteTime, false );

            if( Item.Name.compare( "." ) == 0 || Item.Name.compare( ".." ) == 0 || FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
            {
                if( QPixmapCache::find( "Directory", &Item.Icon ) == false )
                {
                    int a = 0;
                }
            }
            else
            {

                do
                {
                    SHFILEINFOW SHInfo = { 0, };
                    SHGetFileInfoW( Item.Name.toStdWString().c_str(), 0, &SHInfo, sizeof( SHFILEINFOW ),
                                    SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES | SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON );
                    Item.Icon = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
                    if( SHInfo.hIcon != Q_NULLPTR )
                        DestroyIcon( SHInfo.hIcon );

                } while( false );

            }
            Nodes.push_back( Item );

        } while( FindNextFileW( hFile, &wfd ) != 0 );

        FindClose( hFile );
    }


    //QtConcurrent::blockingMap( Nodes, []( Node& Item ) {

    //    if( Item.Name.compare( "." ) == 0 || Item.Name.compare( ".." ) == 0 || FlagOn( Item.Attiributes, FILE_ATTRIBUTE_DIRECTORY ) )
    //    {
    //        if( QPixmapCache::find( "Directory", &Item.Icon ) == false )
    //        {
    //            int a = 0;
    //        }
    //    }
    //    else
    //    {
    //        OleInitialize( 0 );

    //        do
    //        {
    //            //Item.Icon = 
    //            //QFileIconProvider().icon( QFileInfo( Item.Name ) ).pixmap( 24, 24 );
    //            SHFILEINFOW SHInfo = { 0, };
    //            SHGetFileInfoW( Item.Name.toStdWString().c_str(), 0, &SHInfo, sizeof( SHFILEINFOW ),
    //                            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES | SHGFI_ADDOVERLAYS | SHGFI_ICON | SHGFI_LARGEICON );
    //            Item.Icon = QPixmap::fromImage( QImage::fromHICON( SHInfo.hIcon ) );
    //            if( SHInfo.hIcon != Q_NULLPTR )
    //                DestroyIcon( SHInfo.hIcon );

    //        } while( false );

    //        CoUninitialize();
    //    }
    //} );

    beginResetModel();
    qSwap( Nodes, VecNode );
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
    if( index.isValid() == false )
        return {};

    const auto Row = index.row();
    const auto Col = index.column();

    if( Row < 0 || Row >= VecNode.size() )
        return {};

    if( Col < 0 || Col >= CurrentView.VecColumns.size() )
        return {};

    if( role == Qt::DisplayRole )
    {
        const auto& Item = VecNode[ Row ];
        const auto Def = CurrentView.VecColumns[ Col ];
        if( Def.Content.compare( "[=HC.name]", Qt::CaseInsensitive ) == 0 )
        {
            return Item.Name;
        }

        if( Def.Content.compare( "[=HC.size]", Qt::CaseInsensitive ) == 0 )
        {
            if( Item.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
                return {};

            return Item.Size;
        }

        if( Def.Content.compare( "[=HC.created]", Qt::CaseInsensitive ) == 0 )
        {
            return Item.Created;
        }
    }

    if( role == Qt::DecorationRole )
    {
        if( Col == 0 )
            return VecNode[ Row ].Icon;
    }

    if( role == Qt::ForegroundRole )
    {
        const auto& Item = VecNode[ Row ];

        if( Item.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
            return QColor( "red" );

        return QColor( "white" );
    }

    if( role == Qt::UserRole )
    {
        // 디렉토리 순으로 정렬하기 위해 디렉토리 이름 앞에 0 을 붙이고, 파일 이름 앞에는 1 을 붙인다.
        // 이로 인해 오름차순이나 내림차순으로 정렬하면 자연히 이에 따라 정렬된다.
        // QtitanDataGrid 에서 컬럼을 생성할 때 정렬에 사용할 Role 을 UserRole 로 지정한 컬럼만 적용된다.
        if( Col == 0 )
        {
            const auto& Item = VecNode[ Row ];

            if( Item.Name == ".." )
                return "0" + Item.Name;

            if( Item.Attiributes & FILE_ATTRIBUTE_DIRECTORY )
                return "1" + Item.Name;
            else
                return "2" + Item.Name;
        }
    }

    if( role == Qt::ForegroundRole )
    {
        return QColor( "white" );
    }

    return QVariant();
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

QMap<int, QVariant> FSModel::itemData( const QModelIndex& index ) const
{
    return QMap<int, QVariant>();
}

Qt::ItemFlags FSModel::flags( const QModelIndex& index ) const
{
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}
