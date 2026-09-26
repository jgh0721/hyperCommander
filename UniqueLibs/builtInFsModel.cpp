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
            Item->Icon = Provider.icon( QAbstractFileIconProvider::Folder ).pixmap( 32, 32 ).scaledToWidth( 32, Qt::SmoothTransformation );
        }
        else
        {
            Item->Icon = Provider.icon( QFileInfo( currentPathWithRoot_ + Item->Name_ ) ).pixmap( 32, 32 ).scaledToWidth( 32, Qt::SmoothTransformation );
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

TyOsError CFSModel::InsertChildItem( const QString& NewName )
{
    QString Name = NewName;

    // 하위 디렉토리 생성이 포함되었다 하더라도 현재 디렉토리 기준으로 1단계 밑의 디렉토리가 화면에 표시되므로 해당 디렉토리 이름을 추출한다. 
    if( IsContainPathSeps( NewName ) == true )
        Name = NewName.split( QRegularExpression( "(\\|/)" ), Qt::SkipEmptyParts )[ 0 ];

    // TODO: Refresh 함수에서 파일 추가, 디렉토리 추가를 별도 함수로 만든 후 이곳에 추가한다.
    const auto Item = nsHC::TySpFileSource( new nsHC::CFileSourceT );

    Item->Name_         = Name;
    Item->Attributes_   = FILE_ATTRIBUTE_DIRECTORY;
    Item->Created_      = QDateTime::currentDateTime();

    // 컬럼에 대한 데이터 생성
    createBuiltFsValues( QVector< nsHC::TySpFileSource >() << Item );
    
    const auto Row = rowCount();
    const auto StColorSchemeMgr = TyStColorSchemeMgr::GetInstance();

    beginInsertRows( QModelIndex(), Row, Row );

    // 기본 전경/배경 
    vecRowColors_.push_back( QPair< QColor, QColor >() );

    // 확장자 등에 의한 색상 설정
    for( const auto& FileSet : vecFileSetColors_ )
    {
        if( StColorSchemeMgr->JudgeFileSet( FileSet.first, Item.get(), nullptr, nullptr ) == false )
            continue;

        vecRowColors_[ Row - 1 ] = FileSet.second;
        break;
    }

    vecNode_.push_back( Item );
    directoryCount_++;

    if( FsIconGatherer_ != nullptr )
        FsIconGatherer_->requestInterruption();

    endInsertRows();

    if( FsIconGatherer_ == nullptr )
    {
        FsIconGatherer_ = new CFSIconGatherer( this, vecNode_, GetCurrentPathWithRoot(), &lock );
        FsIconGatherer_->setAutoDelete( false );
    }

    FsIconGatherer_->clearInterruption();
    QThreadPool::globalInstance()->start( FsIconGatherer_ );

    return { TyOsError::OS_WIN32_ERROR, ERROR_SUCCESS };
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
            case Qt::ToolTipRole: {
                if (Item->storageEntry_) {
                    QStringList Lines; const auto Values = index_.Metadata(*Item->storageEntry_);
                    for (auto It = Values.begin(); It != Values.end(); ++It) Lines.append(It.key() + ": " + It.value());
                    RoleWith.setData(Lines.join('\n'));
                }
            } break;
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
    if (placeholder_ && !GetRoot()) { emit RetryRequested(); return; }
    placeholder_.reset();
    if (const auto Root = GetRoot()) Root->RefreshReadProvider();
    if (providerEnabled_)
    {
        refreshProvider();
        return;
    }
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
    auto Attr = Root->GetCate() == nsHC::FS_CATE_VIRTUAL ? FILE_ATTRIBUTE_VIRTUAL : GetFileAttributesW( Base.toStdWString().c_str() );
    if( Attr == INVALID_FILE_ATTRIBUTES || FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) )
        Attr = 0;
    if( Root->GetCate() == nsHC::FS_CATE_REMOTE )
        Attr = FILE_ATTRIBUTE_DIRECTORY;
    
    // NOTE: 해당 스레드에서 기본적인 파일 목록을 완성한다.
    // NOTE: 추가적으로 폴더 크기, 아이콘 등은 배경 스레드를 통해 완성시킨다. 

    if( ( FlagOn( Attr, FILE_ATTRIBUTE_DIRECTORY ) ) ||
        ( FlagOn( Attr, FILE_ATTRIBUTE_VIRTUAL ) && Root->GetCate() == nsHC::FS_CATE_VIRTUAL ) )
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
            if( FlagOn( Attr, FILE_ATTRIBUTE_VIRTUAL ) && Root->GetCate() == nsHC::FS_CATE_VIRTUAL )
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

            // A removed or damaged archive must still allow navigation out.
            if (Model)
            {
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

CFSModel::CFSModel()
{
    connect(&tracker_, &QtDirectoryTracker::RefreshRequired, this, [this] {
        if (!loading_ && watchEnabled_ && providerEnabled_ && !HasPlaceholder()) Refresh();
    });
}

hc::storage::Locator CFSModel::WorkspaceLocation() const
{
    if (placeholder_) return *placeholder_;
    const auto Root = GetRoot();
    return Root ? Root->ReadLocation(GetCurrentPathWithRoot()) : hc::storage::Locator{};
}

void CFSModel::SetWatchEnabled(bool Enabled)
{
    watchEnabled_ = Enabled;
    const auto Location = WorkspaceLocation();
    const auto Root = GetRoot();
    // Registering a native watcher on an unreachable server can block the GUI.
    const auto Native = Root && !Root->IsRemote() && Location.kind_ == hc::storage::LocatorKind::Local
        && Location.nesting_.empty() ? QString::fromStdString(Location.root_) : QString{};
    tracker_.SetLocation(Enabled && Root && providerEnabled_ ? hc::workspace::LocationKey(Location) : QString{}, Native);
}

void CFSModel::SetPlaceholder(const hc::storage::Locator& Location, const QString& Reason)
{
    CancelNavigation(); tracker_.SetLocation({});
    beginResetModel(); vecNode_.clear(); vecRowColors_.clear(); fileCount_ = directoryCount_ = 0; totalSize_ = 0;
    vecFs_.clear(); acceptedRoots_.clear(); acceptedProvider_.reset(); path_.clear(); acceptedPath_.clear();
    placeholder_ = Location; index_.Publish({}); setProperty("hc.snapshotLocation", QString{});
    endResetModel();
    navigationError_ = hc::storage::Error{hc::storage::ErrorCategory::Unavailable, Reason.toStdString(), {}, {}, true, "retry"};
    emit sigNavigationFailed(Reason); emit sigRefreshCompleted();
}

void CFSModel::ClearPlaceholder() { placeholder_.reset(); navigationError_.reset(); }

bool CFSModel::PutMetadata(const hc::storage::EntrySnapshot& Entry, const QMap<QString, QString>& Values, quint64 Generation)
{
    if (!index_.Put(Entry, Values, Generation)) return false;
    emit MetadataChanged(); return true;
}

CFSModel::~CFSModel()
{
    navigationStop_.request_stop();
    if (FsIconGatherer_ || DirSizeGatherer_)
    {
        if (FsIconGatherer_) FsIconGatherer_->requestInterruption();
        if (DirSizeGatherer_) DirSizeGatherer_->requestInterruption();
        QThreadPool::globalInstance()->waitForDone();
        delete FsIconGatherer_;
        delete DirSizeGatherer_;
    }
}

void CFSModel::CancelNavigation()
{
    setProperty("hc.restoreTicket", QString{}); setProperty("hc.restorePending", false);
    emit NavigationCancelled();
    navigationStop_.request_stop();
    ++generation_;
    if (loading_)
    {
        loading_ = false;
        navigationError_ = hc::storage::Error{hc::storage::ErrorCategory::Cancelled, "cancelled"};
        emit sigNavigationFailed("cancelled");
        emit sigRefreshCompleted();
    }
}

void CFSModel::SetProviderEnabled(bool Enabled)
{
    if (providerEnabled_ == Enabled) return;
    CancelNavigation();
    // Legacy gatherers refer to model-owned rows. Drain them before changing modes.
    if (FsIconGatherer_ || DirSizeGatherer_)
    {
        if (FsIconGatherer_) FsIconGatherer_->requestInterruption();
        if (DirSizeGatherer_) DirSizeGatherer_->requestInterruption();
        QThreadPool::globalInstance()->waitForDone();
    }
    providerEnabled_ = Enabled;
    // The compatibility path performs synchronous legacy I/O. Automatic
    // rescans belong only to the asynchronous provider path.
    index_.Publish({}); SetWatchEnabled(watchEnabled_);
    acceptedRoots_ = vecFs_;
    acceptedPath_ = path_;
    if (GetRoot()) Refresh();
}

void CFSModel::refreshProvider()
{
    using namespace hc::storage;
    navigationStop_.request_stop();
    navigationStop_ = std::stop_source{};
    const auto Generation = ++generation_;
    const auto TargetRoots = vecFs_;
    const auto TargetPath = path_;
    const auto Root = GetRoot();
    if (!Root) return;
    const auto Location = Root->ReadLocation(retrieveRootWithPath());
    const auto Provider = Root->ReadProvider();
    if (!acceptedRoots_.isEmpty())
    {
        vecFs_ = acceptedRoots_;
        path_ = acceptedPath_;
    }
    navigationError_.reset();
    loading_ = true;
    OperationContext Context;
    Context.requestId_ = std::to_string(Generation);
    Context.stopToken_ = navigationStop_.get_token();
    Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Context.freshEnumerationMetadata_ = true;
    Context.allowUnavailableChildMetadata_ = true;
    using ReadResult = Result<std::vector<EntrySnapshot>>;
    auto* Watcher = new QFutureWatcher<ReadResult>(this);
    connect(Watcher, &QFutureWatcher<ReadResult>::finished, this, [this, Watcher, Generation, TargetRoots, TargetPath, Provider, Location] {
        const auto Result = Watcher->result();
        Watcher->deleteLater();
        if (Generation != generation_) return;
        loading_ = false;
        if (!Result.HasValue())
        {
            navigationError_ = Result.Failure();
            emit sigNavigationFailed(QString::fromStdString(Result.Failure().code_));
            emit sigRefreshCompleted();
            return;
        }
        vecFs_ = TargetRoots;
        path_ = TargetPath;
        acceptedRoots_ = TargetRoots;
        acceptedPath_ = TargetPath;
        acceptedProvider_ = Provider;
        publishProvider(Result.Value());
        setProperty("hc.snapshotLocation", hc::workspace::LocationKey(Location));
        SetWatchEnabled(watchEnabled_);
        emit sigRefreshCompleted();
        const auto Notify = IsRoot() && (GetRoot()->GetFeatures() & nsHC::FS_FEA_PACK)
            ? GetRoot()->GetRoot() : GetCurrentPathWithRoot();
        emit sigChangedDirectory(QDir::toNativeSeparators(QDir::cleanPath(Notify)));
    });
    Watcher->setFuture(QtConcurrent::run([Provider, Location, Context]() -> ReadResult {
        // Only provider ownership and immutable values enter this worker.
        try
        {
            if (!Provider) return Error{ErrorCategory::Unsupported, "provider-not-available"};
            auto Cursor = Provider->Enumerate(Location, Context).get();
            if (!Cursor.HasValue()) return Cursor.Failure();
            std::vector<EntrySnapshot> Entries;
            for (;;)
            {
                auto Page = Cursor.Value()->Next(Context.maxPageItems_, Context).get();
                if (!Page.HasValue()) return Page.Failure();
                if (Page.Value().entries_.size() > Context.maxEnumerationEntries_ - Entries.size())
                    return Error{ErrorCategory::BudgetExceeded, "entry-limit", Provider->Id()};
                if (Page.Value().entries_.empty() && !Page.Value().end_)
                    return Error{ErrorCategory::Internal, "cursor-no-progress", Provider->Id()};
                Entries.insert(Entries.end(), Page.Value().entries_.begin(), Page.Value().entries_.end());
                if (Page.Value().end_) break;
            }
            if (const auto Failure = Context.Check(Provider->Id())) return *Failure;
            return Entries;
        }
        catch (const std::exception&) { return Error{ErrorCategory::Internal, "provider-exception"}; }
    }));
}

void CFSModel::publishProvider(std::vector<hc::storage::EntrySnapshot> Entries)
{
    index_.Publish(Entries);
    QVector<nsHC::TySpFileSource> Items;
    if (GetRoot()->ReadLocation(GetCurrentPathWithRoot()).kind_ != hc::storage::LocatorKind::Shell || GetRootCount() > 1)
    {
        auto Parent = std::make_shared<nsHC::CFileSourceT>();
        Parent->Name_ = "..";
        Parent->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
        Items.push_back(Parent);
    }
    QFileIconProvider Icons;
    for (auto& Entry : Entries)
    {
        nsHC::TySpFileSource Item(new nsHC::CFileSourceT, [](nsHC::CFileSourceT* Value) { if (Value->PIDL) CoTaskMemFree(Value->PIDL); delete Value; });
        Item->Name_ = QString::fromStdString(Entry.name_);
        Item->Ext_ = nsCmn::nsCmnPath::GetFileExtension(Item->Name_);
        Item->Attributes_ = Entry.attributes_;
        Item->Size_ = Entry.size_;
        if (Entry.createdUtcMs_) Item->Created_ = QDateTime::fromMSecsSinceEpoch(Entry.createdUtcMs_);
        if (Entry.modifiedUtcMs_) Item->Modified_ = QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
        if (Item->Name_ != Item->Name_.normalized(QString::NormalizationForm_C)) Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD;
        if (Entry.hints_.contains("reparseTag")) Item->Reserved0_ = QString::fromStdString(Entry.hints_.at("reparseTag")).toUInt();
        if (Entry.hints_.contains("shellPidl"))
        {
            const auto Bytes = QByteArray::fromBase64(QByteArray::fromStdString(Entry.hints_.at("shellPidl")));
            Item->PIDL = static_cast<LPITEMIDLIST>(CoTaskMemAlloc(Bytes.size()));
            if (Item->PIDL) memcpy(Item->PIDL, Bytes.data(), Bytes.size());
            Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_PIDL;
        }
        // Generic icons avoid filesystem/network calls on the GUI thread.
        Item->Icon = Icons.icon(Entry.directory_ ? QFileIconProvider::Folder : QFileIconProvider::File).pixmap(32, 32);
        Item->storageEntry_ = std::move(Entry);
        Items.push_back(std::move(Item));
    }
    createBuiltFsValues(Items);
    QVector<TyPrFGWithBG> Colors;
    int Files = 0, Directories = 0;
    std::uint64_t Size = 0;
    const auto ColorMgr = TyStColorSchemeMgr::GetInstance();
    for (const auto& Item : Items)
    {
        auto Color = qMakePair(colorScheme_.FileList_FGColor, colorScheme_.FileList_BGColor);
        for (const auto& Set : vecFileSetColors_)
            if (ColorMgr->JudgeFileSet(Set.first, Item.get(), nullptr, nullptr)) { Color = Set.second; break; }
        Colors.push_back(Color);
        if (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ++Directories;
        else { ++Files; Size += Item->Size_; }
    }
    beginResetModel();
    vecNode_ = std::move(Items);
    vecRowColors_ = std::move(Colors);
    fileCount_ = Files;
    directoryCount_ = Directories;
    totalSize_ = Size;
    endResetModel();
}

void CFSModel::SetColorScheme(const TyColorScheme& Scheme)
{
    CFsModelT::SetColorScheme(Scheme);
    // Recolor the existing snapshot without an I/O refresh or selection reset.
    const auto Manager = TyStColorSchemeMgr::GetInstance();
    vecRowColors_.clear(); vecRowColors_.reserve(vecNode_.size());
    for (const auto& Item : vecNode_) {
        auto Color = qMakePair(Scheme.FileList_FGColor, Scheme.FileList_BGColor);
        for (const auto& Set : vecFileSetColors_)
            if (Manager->JudgeFileSet(Set.first, Item.get(), nullptr, nullptr)) { Color = Set.second; break; }
        vecRowColors_.push_back(Color);
    }
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
        {Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole});
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
        if (!IsShowHiddenSystem_ && (FlagOn(Attr, FILE_ATTRIBUTE_HIDDEN) || FlagOn(Attr, FILE_ATTRIBUTE_SYSTEM))) return false;
        const auto Model = qobject_cast<const CFSModel*>(sourceModel());
        const auto Item = Model ? Model->GetFileInfo(Index) : nsHC::TySpFileSource{};
        if (Item && Item->storageEntry_ && !Model->MatchesSearch(*Item->storageEntry_, search_)) return false;
    }

    return QSortFilterProxyModel::filterAcceptsRow( source_row, source_parent );
}
