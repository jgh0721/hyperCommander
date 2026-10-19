#include "stdafx.h"
#include "builtInFsModel.hpp"
#include "quickFindSettings.hpp"
#include "directoryRefreshOrder.hpp"
#include "columnNativeFields.hpp"
#include "folderSize.hpp"
#include "nativeFileElevation.hpp"
#include <QPromise>
#include <QScopeGuard>
#include "columnPluginFields.hpp"
#include "commonLibs/columns/luaColumns.hpp"
#include "commonLibs/columns/columnSort.hpp"

#include "commonLibs/cmnDateTime.hpp"
#include "cmnHCUtils.hpp"
#include "UniqueLibs/columnMgr.hpp"

#include <QtConcurrent>
#include <ObjectArray.h>
#include <Shlobj.h>
#include <commoncontrols.h>
#include <winioctl.h>


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
        if( hc::settings::FileColorRules::HasDecomposedName(Fs->Name_) )
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
    refreshNameSortRanks();
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

void CFSModel::SetColumnView(const ColumnView& View)
{
    columnQueries_->SetLoader(hc::columns::WithLuaColumns(hc::columns::PluginFields::Instance().Loader(), View.profile_), false);
    beginResetModel();
    {
        CCmnMutexLocker Locker(&lock);
        columnView_ = View; columnSorts_.clear(); scriptRevisions_.clear();
        for (const auto& Script : View.profile_.scripts_) {
            QJsonArray Outputs; for (const auto& Output : Script.outputs_) Outputs.append(QJsonArray{Output.name_, int(Output.type_)});
            const auto Revision = QJsonDocument(QJsonObject{{"source", Script.source_}, {"dependencies", QJsonArray::fromStringList(Script.dependencies_)}, {"outputs", Outputs}}).toJson(QJsonDocument::Compact);
            scriptRevisions_[Script.id_] = QString::fromLatin1(QCryptographicHash::hash(Revision, QCryptographicHash::Sha256).toHex());
        }
        createBuiltFsValues(vecNode_);
        refreshNameSortRanks();
    }
    endResetModel();
}

// Visibility changes must request newly exposed lazy fields without resetting
// entries, selection or the directory snapshot.
void CFSModel::SetColumnLayoutMode(hc::columns::LayoutMode Mode)
{
    {
        CCmnMutexLocker Locker(&lock);
        if (columnView_.profile_.mode_ == Mode) return;
        columnView_.profile_.mode_ = Mode;
    }
    if (rowCount() && columnCount())
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole});
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
    const auto& RuleItem = vecNode_[Row];
    hc::settings::FileRuleInput RuleInput{RuleItem->Name_, RuleItem->Attributes_, quint64(qMax<qint64>(0, RuleItem->Size_)), colorScheme_.IsDarkMode};
    if (RuleItem->Modified_.isValid()) RuleInput.modifiedUtcMs_ = hc::settings::FileColorRules::MetadataTime(RuleItem->Modified_.toMSecsSinceEpoch());
    if (RuleItem->Reserved0_) RuleInput.reparseTag_ = RuleItem->Reserved0_;
    const auto RuleKey = RuleItem->Name_ + QChar::Null + QString::number(RuleItem->Attributes_) + ':' + QString::number(RuleItem->Size_)
        + ':' + (RuleInput.modifiedUtcMs_ ? QString::number(*RuleInput.modifiedUtcMs_) : QStringLiteral("unknown")) + ':' + QString::number(RuleItem->Reserved0_);
    auto* Cached = fileColorCache_.object(RuleKey);
    if (Cached && Cached->style_.validUntilUtcMs_ && RuleInput.referenceUtcMs_ >= *Cached->style_.validUntilUtcMs_) { fileColorCache_.remove(RuleKey); Cached = nullptr; }
    if (!Cached) { fileColorCache_.insert(RuleKey, new CachedFileRule{fileColorRules_.Evaluate(RuleInput), Row}); Cached = fileColorCache_.object(RuleKey); }
    Cached->row_ = Row;
    if (Cached->style_.validUntilUtcMs_) scheduleFileRuleExpiry(*Cached->style_.validUntilUtcMs_);
    const auto& RuleStyle = Cached->style_;
    for( QModelRoleData& RoleWith : roleDataSpan )
    {
        const int Role = RoleWith.role();
        const auto& Item = vecNode_[ Row ];

        switch( Role )
        {
            case GridSortRole: {
                if (columnSortRanks_.size() == vecNode_.size() && !columnSorts_.isEmpty()) {
                    const auto Id = columnView_.VecColumns[Col].definition_.id_;
                    const auto It = std::find_if(columnSorts_.begin(), columnSorts_.end(), [&](const auto& Sort) { return Sort.columnId_ == Id; });
                    RoleWith.setData(It != columnSorts_.end() && It->order_ == Qt::DescendingOrder ? -columnSortRanks_[Row] : columnSortRanks_[Row]);
                } else RoleWith.setData(Row);
            } break;
            case ColumnStateRole: { RoleWith.setData(int(evaluateColumn(Item, columnView_.VecColumns[Col], false).State())); } break;
            case Qt::ToolTipRole: {
                const auto Value = evaluateColumn(Item, columnView_.VecColumns[Col], false);
                if (!Value.Error().isEmpty()) { RoleWith.setData(Value.Error()); break; }
                if (Item->storageEntry_) {
                    QStringList Lines; const auto Values = index_.Metadata(*Item->storageEntry_);
                    for (auto It = Values.begin(); It != Values.end(); ++It) Lines.append(It.key() + ": " + It.value());
                    RoleWith.setData(Lines.join('\n'));
                }
            } break;
            case Qt::DisplayRole:
            case Qt::EditRole: {
                if (Col == 0 && Role == Qt::EditRole) { RoleWith.setData(Item->Name_); break; }
                if (Col == 0 && Role == Qt::DisplayRole && (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) && Item->Name_ != "..") {
                    const auto Style = displaySettings_["directoryStyle"].toString();
                    if (Style == "brackets") { RoleWith.setData("[" + Item->Name_ + "]"); break; }
                    if (Style == "suffix") { RoleWith.setData(Item->Name_ + "\\"); break; }
                }
                auto Binding = columnView_.VecColumns[Col];
                Binding.definition_.fieldId_ = hc::columns::DisplayField(columnView_.profile_, Binding.definition_);
                // A folder whose earlier size is known keeps showing it while it is re-measured.
                if (Role == Qt::DisplayRole && Binding.definition_.fieldId_ == "file.size" && Item->storageEntry_ && !Item->directorySize_
                    && directorySizePending_.contains(QString::fromStdString(hc::storage::Serialize(Item->storageEntry_->id_)))) {
                    RoleWith.setData(tr("계산 중…")); break;
                }
                const bool Visible = !Binding.definition_.widths_[int(columnView_.profile_.mode_)].hidden_;
                RoleWith.setData(CColumnMgr::FormatValue(Binding.definition_, evaluateColumn(Item, Binding, Visible && !loading_), Item.get()));
            } break;

            case Qt::DecorationRole: {
                if (Col == 0 && displaySettings_["iconMode"].toString() != "none") RoleWith.setData(vecNode_[Row]->Icon);
                else RoleWith.clearData();
            } break;

            case FileColorOverrideRole: {
                RoleWith.setData(RuleStyle.matched_ ||
                    (vecRowColors_[Row].first.isValid() && vecRowColors_[Row].first != colorScheme_.FileList_FGColor) ||
                    (vecRowColors_[Row].second.isValid() && vecRowColors_[Row].second != colorScheme_.FileList_BGColor));
            } break;
            case Qt::ForegroundRole: {
                if (RuleStyle.matched_) { RoleWith.setData(RuleStyle.foreground_.isValid() ? RuleStyle.foreground_ : colorScheme_.FileList_FGColor); break; }
                if( vecRowColors_[ Row ].first.isValid() == true )
                    RoleWith.setData( vecRowColors_[ Row ].first );
                else
                    RoleWith.setData( QColor( "#FFFFFF" ) );
            } break;
            case Qt::BackgroundRole: {
                if (RuleStyle.matched_) { RoleWith.setData(RuleStyle.background_.isValid() ? RuleStyle.background_ : colorScheme_.FileList_BGColor); break; }
                if( vecRowColors_[ Row ].second.isValid() == true )
                    RoleWith.setData( vecRowColors_[ Row ].second );
                else
                    RoleWith.setData( QColor( 0, 0, 0 ) );
            } break;
            case Qt::FontRole: {
                RoleWith.setData( RuleStyle.Font(colorScheme_.FileList_Font) );

                if( Col == 0 )
                {
                    if( FlagOn( Item->Flags_, nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD ) )
                    {
                        QFont under( RuleStyle.Font(colorScheme_.FileList_Font) );
                        under.setUnderline( true );
                        RoleWith.setData( under );
                    }
                }
            } break;

            case CFsModelT::USR_ROLE_SORT: {
                if (Col != 0) {
                    RoleWith.setData(evaluateColumn(Item, columnView_.VecColumns[Col], false, true).Raw());
                    break;
                }
                if (Col == 0 && nameSortRanks_.size() == vecNode_.size()) {
                    RoleWith.setData(nameSortRanks_[Row]);
                    break;
                }
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
                if (displaySettings_["dimHiddenIcons"].toBool(true) &&
                    (FlagOn(Item->Attributes_, FILE_ATTRIBUTE_HIDDEN) || FlagOn(Item->Attributes_, FILE_ATTRIBUTE_SYSTEM)))
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
                    if( hc::settings::FileColorRules::HasDecomposedName(Fs->Name_) )
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
        qSwap( VecItems, vecNode_ );
        qSwap( VecRowColors, vecRowColors_ );
        qSwap( FileCount, fileCount_ );
        qSwap( DirectoryCount, directoryCount_ );
        qSwap( TotalSize, totalSize_ );
        refreshNameSortRanks();
        ++iconGeneration_; iconStop_.request_stop();
        endResetModel();
    }

    if( FsIconGatherer_ == nullptr )
    {
        FsIconGatherer_ = new CFSIconGatherer( this, vecNode_, GetCurrentPathWithRoot(), &lock );
        FsIconGatherer_->setAutoDelete( false );
    }

    FsIconGatherer_->clearInterruption();
    QThreadPool::globalInstance()->start( FsIconGatherer_ );


}

CFSModel::CFSModel()
{
    columnQueries_ = std::make_unique<hc::columns::QueryEngine>(hc::columns::WithLuaColumns(hc::columns::PluginFields::Instance().Loader(), columnView_.profile_));
    columnQueries_->Changed = [this](const QSet<QString>& Paths) { columnResultsReady(Paths); };
    connect(&hc::columns::PluginFields::Instance(), &hc::columns::PluginFields::Changed, this, [this] {
        columnQueries_->SetLoader(hc::columns::WithLuaColumns(hc::columns::PluginFields::Instance().Loader(), columnView_.profile_));
        if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        refreshColumnSortRanks();
    });
    displaySettings_ = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    connect(&tracker_, &QtDirectoryTracker::RefreshRequired, this, [this] {
        if (!loading_ && watchEnabled_ && providerEnabled_ && !HasPlaceholder()) {
            carryDirectorySizes_ = true; autoRefreshRequested_ = true; Refresh();
        }
    });
    connect(this, &CFSModel::sigRefreshCompleted, this, [this] { tracker_.SetBusy(loading_); });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) {
        const auto Settings = hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject());
        tracker_.SetSuspended(Settings.pauseInBackground && State != Qt::ApplicationActive);
    });
    ApplyRefreshSettings();
}

void CFSModel::ApplyRefreshSettings()
{
    const auto Settings = hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject());
    tracker_.Configure(Settings);
    tracker_.SetSuspended(Settings.pauseInBackground && QGuiApplication::applicationState() != Qt::ApplicationActive);
    SetWatchEnabled(watchEnabled_);
}

hc::storage::Locator CFSModel::WorkspaceLocation() const
{
    if (placeholder_) return *placeholder_;
    const auto Root = GetRoot();
    return Root ? Root->ReadLocation(GetCurrentPathWithRoot()) : hc::storage::Locator{};
}

void CFSModel::NotifyNetworkChange()
{
    const auto Location = WorkspaceLocation();
    if ((Location.kind_ == hc::storage::LocatorKind::Smb && Location.root_ == "\\")
        || (Location.kind_ == hc::storage::LocatorKind::Shell && Location.root_ == "::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}"))
        tracker_.Invalidate();
}

void CFSModel::SetWatchEnabled(bool Enabled)
{
    watchEnabled_ = Enabled;
    const auto Location = WorkspaceLocation();
    const auto Root = GetRoot();
    // Registering a native watcher on an unreachable server can block the GUI.
    const auto Native = Root && !Root->IsRemote() && Location.kind_ == hc::storage::LocatorKind::Local
        && Location.nesting_.empty() ? QString::fromStdString(Location.root_) : QString{};
    tracker_.SetLocation(Enabled && Root && providerEnabled_ ? hc::workspace::LocationKey(Location) : QString{}, Native,
        QString::fromStdString(Location.root_));
}

void CFSModel::SetPlaceholder(const hc::storage::Locator& Location, const QString& Reason, bool Pending)
{
    automaticRefresh_ = autoRefreshRequested_ = false;
    elevatedDirectorySession_.reset();
    CancelNavigation(); tracker_.SetLocation({});
    columnQueries_->Reset();
    beginResetModel(); vecNode_.clear(); vecRowColors_.clear(); fileCount_ = directoryCount_ = 0; totalSize_ = 0;
    vecFs_.clear(); acceptedRoots_.clear(); acceptedProvider_.reset(); path_.clear(); acceptedPath_.clear();
    placeholder_ = Location; index_.Publish({}); setProperty("hc.snapshotLocation", QString{});
    pageSource_.reset(); pageView_.reset();
    endResetModel();
    emit PageChanged();
    // Connection preparation is not a terminal failure. Publishing it would
    // consume the command correlation before the actual navigation completes.
    if (Pending) { navigationError_.reset(); return; }
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
    navigationStop_.request_stop(); iconStop_.request_stop(); pageStop_.request_stop(); directorySizeStop_.request_stop();
    if (FsIconGatherer_)
    {
        if (FsIconGatherer_) FsIconGatherer_->requestInterruption();
        QThreadPool::globalInstance()->waitForDone();
        delete FsIconGatherer_;
    }
}

void CFSModel::CancelNavigation()
{
    automaticRefresh_ = autoRefreshRequested_ = false;
    failedNavigationRetry_ = {};
    directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    setProperty("hc.restoreTicket", QString{}); setProperty("hc.restorePending", false);
    emit NavigationCancelled();
    navigationStop_.request_stop();
    ++generation_;
    pageStop_.request_stop(); ++pageGeneration_; pageBusy_ = false;
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
    if (FsIconGatherer_)
    {
        if (FsIconGatherer_) FsIconGatherer_->requestInterruption();
        QThreadPool::globalInstance()->waitForDone();
    }
    providerEnabled_ = Enabled;
    pageSource_.reset(); pageView_.reset(); emit PageChanged();
    // The compatibility path performs synchronous legacy I/O. Automatic
    // rescans belong only to the asynchronous provider path.
    index_.Publish({}); SetWatchEnabled(watchEnabled_);
    acceptedRoots_ = vecFs_;
    acceptedPath_ = path_;
    if (GetRoot()) Refresh();
}

bool CFSModel::RetryFailedNavigation(bool Elevated)
{
    auto Retry = std::exchange(failedNavigationRetry_, {});
    if (!Retry) return false;
    elevatedDirectoryRequest_ = Elevated;
    Retry();
    return true;
}

void CFSModel::DismissNavigationError()
{
    failedNavigationRetry_ = {};
    navigationError_.reset();
    emit sigRefreshCompleted();
}

void CFSModel::refreshProvider()
{
    automaticRefresh_ = std::exchange(autoRefreshRequested_, false);
    const bool Elevated = std::exchange(elevatedDirectoryRequest_, false);
    failedNavigationRetry_ = {};
    directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    pageStop_.request_stop(); ++pageGeneration_; pageBusy_ = false;
    requestOnlyValues_ = false; columnQueries_->SetLoader(hc::columns::WithLuaColumns(hc::columns::PluginFields::Instance().Loader(), columnView_.profile_));
    using namespace hc::storage;
    navigationStop_.request_stop();
    navigationStop_ = std::stop_source{};
    const auto Generation = ++generation_;
    const auto TargetRoots = vecFs_;
    const auto TargetPath = path_;
    const auto TargetFullPath = retrieveRootWithPath();
    const auto Root = GetRoot();
    if (!Root) return;
    const auto Location = Root->ReadLocation(retrieveRootWithPath());
    const auto Provider = Root->ReadProvider();
    const auto Session = Location.kind_ == LocatorKind::Local && elevatedDirectorySession_
        && elevatedDirectorySession_->Contains(QString::fromStdString(Location.root_)) ? elevatedDirectorySession_ : nullptr;
    if (!acceptedRoots_.isEmpty())
    {
        vecFs_ = acceptedRoots_;
        path_ = acceptedPath_;
    }
    navigationError_.reset();
    loading_ = true;
    tracker_.SetBusy(true);
    OperationContext Context;
    Context.requestId_ = std::to_string(Generation);
    Context.stopToken_ = navigationStop_.get_token();
    Context.freshEnumerationMetadata_ = true;
    Context.allowUnavailableChildMetadata_ = true;
    struct Listing { std::shared_ptr<PagedEntryIndex> source_, view_; EntryPage page_; std::shared_ptr<hc::permissions::DirectoryElevationSession> session_; };
    using ReadResult = Result<Listing>;
    auto Compare = pageComparison(TargetFullPath); const bool Fallback = !Compare.HasValue();
    if (Fallback) Compare = pageComparison(TargetFullPath, true);
    const auto Sorts = columnSorts_; const auto Display = displaySettings_;
    const auto RefreshSettings = hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject());
    const bool Append = automaticRefresh_ && RefreshSettings.appendNewEntries && pageSource_
        && property("hc.snapshotLocation").toString() == hc::workspace::LocationKey(Location);
    auto Previous = Append && UsesIndexedOrder() ? pageSource_ : std::shared_ptr<PagedEntryIndex>{};
    const bool SortPrevious = Previous && !appendOrder_;
    std::vector<EntrySnapshot> PreviousRows;
    if (Append && !Previous) {
        QVector<int> Order;
        for (int Row = 0; Row < vecNode_.size(); ++Row) if (vecNode_[Row]->storageEntry_) Order.append(Row);
        const auto& Ranks = columnSortRanks_.size() == vecNode_.size() ? columnSortRanks_ : nameSortRanks_;
        if (Ranks.size() == vecNode_.size()) std::stable_sort(Order.begin(), Order.end(), [&](int A, int B) { return Ranks[A] < Ranks[B]; });
        for (const auto Row : Order) PreviousRows.push_back(*vecNode_[Row]->storageEntry_);
    }
    const auto Query = pageQuery_; const auto Match = pageMatch_; const auto ShowHidden = pageShowHidden_;
    auto* Watcher = new QFutureWatcher<ReadResult>(this);
    connect(Watcher, &QFutureWatcher<ReadResult>::finished, this, [this, Watcher, Generation, TargetRoots, TargetPath, Provider, Location, Query, Match, ShowHidden, Sorts, Fallback, Display, Append] {
        const auto Result = Watcher->result();
        Watcher->deleteLater();
        if (Generation != generation_) return;
        const auto EndAutomatic = qScopeGuard([this] { automaticRefresh_ = false; });
        loading_ = false;
        if (!Result.HasValue())
        {
            navigationError_ = Result.Failure();
            failedNavigationRetry_ = [this, TargetRoots, TargetPath] {
                vecFs_ = TargetRoots;
                path_ = TargetPath;
                Refresh();
            };
            emit sigNavigationFailed(QString::fromStdString(Result.Failure().code_));
            emit sigRefreshCompleted();
            if (Result.Failure().category_ != ErrorCategory::Cancelled)
                emit DirectoryReadFailed(QString::fromStdString(Location.root_), Location.kind_ == LocatorKind::Local);
            return;
        }
        vecFs_ = TargetRoots;
        path_ = TargetPath;
        acceptedRoots_ = TargetRoots;
        acceptedPath_ = TargetPath;
        acceptedProvider_ = Provider;
        // A tab retains its approved subtree session across refreshes and child
        // navigation. Publishing a location outside it releases the helper.
        elevatedDirectorySession_ = Result.Value().session_;
        pageSource_ = Result.Value().source_; pageView_ = Result.Value().view_;
        completedSorts_ = Sorts;
        appendOrder_ = Append;
        if (UsesIndexedOrder() && Fallback) { columnSorts_.clear(); completedSorts_.clear(); emit ColumnSortCancelled(); }
        publishProvider(Result.Value().page_.entries_);
        setProperty("hc.snapshotLocation", hc::workspace::LocationKey(Location));
        SetWatchEnabled(watchEnabled_);
        emit PageChanged();
        emit sigRefreshCompleted();
        const auto Notify = IsRoot() && (GetRoot()->GetFeatures() & nsHC::FS_FEA_PACK)
            ? GetRoot()->GetRoot() : GetCurrentPathWithRoot();
        emit sigChangedDirectory(QDir::toNativeSeparators(QDir::cleanPath(Notify)));
        if (UsesIndexedOrder() && (Query != pageQuery_ || Match != pageMatch_ || ShowHidden != pageShowHidden_ || (!Fallback && Sorts != columnSorts_) || Display != displaySettings_)) requestPage(0, true);
        else if (UsesIndexedOrder() && Fallback) emit ColumnQueryStatus(tr("이 컬럼의 전체 목록 정렬은 아직 지원하지 않아 이름순으로 표시합니다."));
    });
    Watcher->setFuture(QtConcurrent::run([Provider, Location, Context, Compare, Query, Match, ShowHidden, Elevated, Session, Append, Previous, SortPrevious, PreviousRows = std::move(PreviousRows)]() mutable -> ReadResult {
        // Only provider ownership and immutable values enter this worker.
        try
        {
            if (!Provider) return Error{ErrorCategory::Unsupported, "provider-not-available"};
            auto ActiveSession = Session;
            if (Elevated && Location.kind_ == LocatorKind::Local && !ActiveSession) {
                auto Open = hc::permissions::OpenElevatedDirectorySession(QString::fromStdString(Location.root_), Context);
                if (!Open.HasValue()) return Open.Failure();
                ActiveSession = std::move(Open.Value());
            }
            auto Cursor = ActiveSession
                ? ActiveSession->Enumerate(QString::fromStdString(Location.root_), Context, Provider)
                : Provider->Enumerate(Location, Context).get();
            if (!Cursor.HasValue()) return Cursor.Failure();
            auto Source = PagedEntryIndex::Build(*Cursor.Value(), Context);
            if (!Source.HasValue()) return Source.Failure();
            if (Append) {
                if (SortPrevious) {
                    if (!Compare.HasValue()) return Compare.Failure();
                    auto Sorted = Previous->Project(Compare.Value(), [](const EntrySnapshot&) { return true; }, Context, {});
                    if (!Sorted.HasValue()) return Sorted.Failure();
                    Previous = Sorted.Value();
                }
                Source = hc::refresh::AppendNewEntries(Source.Value(), Previous, std::move(PreviousRows), Context);
                if (!Source.HasValue()) return Source.Failure();
            }
            auto View = Source;
            if (Source.Value()->Count() > IndexedOrderThreshold) {
                if (!Compare.HasValue()) return Compare.Failure();
                View = Source.Value()->Project(Append ? hc::refresh::ArrivalOrder() : Compare.Value(), [Query, Match, ShowHidden](const EntrySnapshot& Entry) {
                    return (ShowHidden || !(Entry.attributes_ & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) &&
                        (Match.isEmpty() ? hc::workspace::DirectoryIndex{}.Matches(Entry, Query) : hc::quickfind::Matches(QString::fromStdString(Entry.name_), Query, Match));
                }, Context, {});
                if (!View.HasValue()) return View.Failure();
            }
            auto Page = View.Value()->Read(0, (std::numeric_limits<std::size_t>::max)(), Context); if (!Page.HasValue()) return Page.Failure();
            if (const auto Failure = Context.Check(Provider->Id())) return *Failure;
            return Listing{Source.Value(), View.Value(), std::move(Page.Value()), std::move(ActiveSession)};
        }
        catch (const std::exception&) { return Error{ErrorCategory::Internal, "provider-exception"}; }
    }));
}

hc::storage::Result<hc::storage::PagedEntryIndex::Less> CFSModel::pageComparison(const QString& ParentPath, bool NameOnly) const
{
    if (appendOrder_ && ParentPath.isEmpty() && !NameOnly) return hc::refresh::ArrivalOrder();
    using namespace hc::columns;
    QVector<Column> Bindings; QVector<Qt::SortOrder> Orders;
    for (const auto& Sort : columnSorts_) {
        if (NameOnly) break;
        for (const auto& Binding : columnView_.VecColumns) if (Binding.definition_.id_ == Sort.columnId_) {
            auto Fields = Binding.compiled_.dependencies_;
            if (Binding.definition_.template_.isEmpty()) Fields = {Binding.definition_.fieldId_};
            if (!Binding.definition_.sortField_.isEmpty()) Fields = {Binding.definition_.sortField_};
            for (const auto& Field : Fields) if (!QueryFamily(Field).isEmpty() || IsVfsColumn(Field))
                return hc::storage::Error{hc::storage::ErrorCategory::Unsupported, "paged-sort-needs-snapshot-values"};
            Bindings.push_back(Binding); Orders.push_back(Sort.order_);
        }
    }
    if (Bindings.isEmpty()) { Column Name; Name.definition_.fieldId_ = "file.name"; Bindings.push_back(Name); Orders.push_back(Qt::AscendingOrder); }
    const auto& First = Bindings.front().definition_;
    if (Bindings.size() == 1 && (First.sortField_ == "file.name" ||
        (First.sortField_.isEmpty() && First.template_.isEmpty() && First.fieldId_ == "file.name"))) {
        const auto TextOrder = TextCompare(displaySettings_["sortLocaleAware"].toBool(true), displaySettings_["sortNumericAware"].toBool(true),
            displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive);
        const auto Direction = Orders.front();
        // Name sorting needs no display object, template evaluation or identity
        // encoding except on a tie. Keep the exact shared text comparison rules.
        return hc::storage::PagedEntryIndex::Less([TextOrder, Direction](const auto& A, const auto& B) {
            if (A.directory_ != B.directory_) return A.directory_;
            const auto Order = TextOrder(QString::fromStdString(A.name_), QString::fromStdString(B.name_));
            if (Order) return Direction == Qt::AscendingOrder ? Order < 0 : Order > 0;
            return QString::compare(hc::workspace::SelectionKey(A), hc::workspace::SelectionKey(B), Qt::CaseSensitive) < 0;
        });
    }
    const auto Compare = SortLess(Orders, displaySettings_["sortLocaleAware"].toBool(true), displaySettings_["sortNumericAware"].toBool(true),
        displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    const auto Parent = ParentPath.isEmpty() ? GetCurrentPathWithRoot() : ParentPath;
    const auto MakeRow = [Bindings, Parent](const hc::storage::EntrySnapshot& Entry) {
        FileSnapshot Item; Item.name_ = QString::fromStdString(Entry.name_); Item.attributes_ = Entry.attributes_;
        Item.directory_ = Entry.directory_; Item.parentPath_ = Parent;
        if (!Entry.directory_) Item.size_ = Entry.size_;
        if (Entry.createdUtcMs_) Item.created_ = QDateTime::fromMSecsSinceEpoch(Entry.createdUtcMs_);
        if (Entry.modifiedUtcMs_) Item.modified_ = QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
        if (const auto It = Entry.hints_.find("accessedUtcMs"); It != Entry.hints_.end()) {
            bool Ok = false; const auto Time = QString::fromStdString(It->second).toLongLong(&Ok);
            if (Ok) Item.accessed_ = QDateTime::fromMSecsSinceEpoch(Time);
        }
        if (const auto It = Entry.hints_.find("typeDescription"); It != Entry.hints_.end()) Item.typeDescription_ = QString::fromStdString(It->second);
        SortRow Row; Row.directory_ = Entry.directory_; Row.id_ = hc::workspace::SelectionKey(Entry);
        for (const auto& Binding : Bindings) {
            const auto Resolve = [&](const QString& Field) { return hc::columns::Resolve(Field, Item); };
            const auto& Def = Binding.definition_;
            Row.values_.push_back(!Def.sortField_.isEmpty() ? Resolve(Def.sortField_) : Def.template_.isEmpty() ? Resolve(Def.fieldId_) : RenderTemplate(Binding.compiled_, Def.format_, Resolve));
        }
        return Row;
    };
    return hc::storage::PagedEntryIndex::Less([Compare, MakeRow](const auto& A, const auto& B) { return Compare(MakeRow(A), MakeRow(B)); });
}

void CFSModel::SetPageFilter(const QString& Query, bool ShowHidden, const QString& Match)
{
    if (pageQuery_ == Query && pageShowHidden_ == ShowHidden && pageMatch_ == Match) return;
    pageQuery_ = Query; pageShowHidden_ = ShowHidden; pageMatch_ = Match;
    if (UsesIndexedOrder() && !loading_) requestPage(0, true);
}

hc::storage::Result<std::shared_ptr<hc::storage::ISelectionManifest>> CFSModel::SelectAllEntries() const
{
    if (!pageView_) return hc::storage::Error{hc::storage::ErrorCategory::Unavailable, "no-directory-snapshot"};
    auto Selected = pageView_->SelectAll(); if (!Selected.HasValue()) return Selected.Failure();
    class BoundSelection final : public hc::storage::ISelectionManifest
    {
        std::shared_ptr<hc::storage::ISelectionManifest> selection_;
        std::shared_ptr<hc::storage::IStorageProvider> provider_;
    public:
        BoundSelection(std::shared_ptr<hc::storage::ISelectionManifest> Selection, std::shared_ptr<hc::storage::IStorageProvider> Provider)
            : selection_(std::move(Selection)), provider_(std::move(Provider)) {}
        std::uint64_t Count() const override { return selection_->Count(); }
        std::shared_ptr<hc::storage::IStorageProvider> Provider() const override { return provider_; }
        hc::storage::Result<hc::storage::EntryPage> Read(std::uint64_t Offset, std::size_t MaxItems, const hc::storage::OperationContext& Context) const override
        { return selection_->Read(Offset, MaxItems, Context); }
    };
    return std::shared_ptr<hc::storage::ISelectionManifest>(new BoundSelection(Selected.Value(), acceptedProvider_));
}

void CFSModel::requestPage(std::uint64_t Offset, bool Rebuild)
{
    using namespace hc::storage;
    auto Compare = pageComparison();
    if (Rebuild && !Compare.HasValue()) {
        columnSorts_ = completedSorts_; emit ColumnSortCancelled();
        emit ColumnQueryStatus(tr("이 컬럼은 전체 목록의 정렬 값 수집을 아직 지원하지 않습니다.")); return;
    }
    pageStop_.request_stop(); pageStop_ = std::stop_source{};
    pageBusy_ = true;
    const auto Generation = ++pageGeneration_; const auto Navigation = generation_;
    const auto Source = pageSource_; const auto Current = pageView_;
    const auto Query = pageQuery_; const auto Match = pageMatch_; const auto ShowHidden = pageShowHidden_; const auto Metadata = index_;
    const auto Sorts = columnSorts_;
    OperationContext Context; Context.stopToken_ = pageStop_.get_token();
    struct Listing { std::shared_ptr<PagedEntryIndex> view_; EntryPage page_; };
    using PageResult = Result<Listing>;
    auto* Watcher = new QFutureWatcher<PageResult>(this);
    emit ColumnQueryStatus(tr("목록을 준비하고 있습니다…"));
    connect(Watcher, &QFutureWatcher<PageResult>::finished, this, [this, Watcher, Generation, Navigation, Offset, Sorts] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation != pageGeneration_ || Navigation != generation_) return;
        pageBusy_ = false;
        if (!Result.HasValue()) {
            columnSorts_ = completedSorts_; emit ColumnSortCancelled();
            emit ColumnQueryStatus(QString::fromStdString(Result.Failure().code_)); return;
        }
        pageView_ = Result.Value().view_; completedSorts_ = Sorts;
        publishProvider(Result.Value().page_.entries_);
        emit PageChanged(); emit sigRefreshCompleted(); emit ColumnQueryStatus({});
    });
    Watcher->setFuture(QtConcurrent::run([Source, Current, Offset, Rebuild, Compare, Query, Match, ShowHidden, Metadata, Context]() -> PageResult {
        try {
            auto View = Rebuild ? Source->Project(Compare.Value(), [Query, Match, ShowHidden, Metadata](const EntrySnapshot& Entry) {
                return (ShowHidden || !(Entry.attributes_ & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) && (Match.isEmpty() ? Metadata.Matches(Entry, Query) : hc::quickfind::Matches(QString::fromStdString(Entry.name_), Query, Match));
            }, Context, {}) : Result<std::shared_ptr<PagedEntryIndex>>(Current);
            if (!View.HasValue()) return View.Failure();
            auto Page = View.Value()->Read(0, (std::numeric_limits<std::size_t>::max)(), Context); if (!Page.HasValue()) return Page.Failure();
            return Listing{View.Value(), std::move(Page.Value())};
        } catch (const std::exception&) { return Error{ErrorCategory::Internal, "page-worker-exception"}; }
    }));
}

namespace {
// SHGFI_ICON only offers 16/32 px. Larger list icons come from the system image lists
// (16/32/48/256) by image index and are scaled once to the drawn physical size.
QImage ShellIconImage(const std::wstring& Name, bool UseAttributes, int Pixels, DWORD Attributes = FILE_ATTRIBUTE_NORMAL)
{
    SHFILEINFOW Info{};
    if (!SHGetFileInfoW(Name.c_str(), Attributes, &Info, sizeof(Info), SHGFI_SYSICONINDEX | (UseAttributes ? SHGFI_USEFILEATTRIBUTES : 0)))
        return {};
    const auto Read = [&Info](int List) {
        QImage Image; IImageList* Images = nullptr;
        if (FAILED(SHGetImageList(List, IID_PPV_ARGS(&Images))) || !Images) return Image;
        HICON Icon = nullptr;
        if (SUCCEEDED(Images->GetIcon(Info.iIcon, ILD_TRANSPARENT, &Icon)) && Icon) { Image = QImage::fromHICON(Icon); DestroyIcon(Icon); }
        Images->Release(); return Image;
    };
    // Icons without a 256 px image sit in the top-left 48 px of the jumbo list.
    const auto Padded = [](const QImage& Image) {
        const auto Argb = Image.convertToFormat(QImage::Format_ARGB32);
        for (int Y = 0; Y < Argb.height(); ++Y) {
            const auto* Line = reinterpret_cast<const QRgb*>(Argb.constScanLine(Y));
            for (int X = 0; X < Argb.width(); ++X) if (qAlpha(Line[X]) && (X >= 48 || Y >= 48)) return false;
        }
        return true;
    };
    QImage Image;
    for (const int List : {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO}) {
        auto Next = Read(List);
        if (Next.isNull()) continue;
        if (List == SHIL_JUMBO && Padded(Next)) break;
        Image = std::move(Next);
        if (Image.width() >= Pixels) break;
    }
    if (!Image.isNull() && Image.width() != Pixels) Image = Image.scaled(Pixels, Pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return Image;
}
// Generic folder/file icons at exactly Pixels; QFileIconProvider stops at 32 px.
QPixmap GenericIcon(bool Directory, int Pixels)
{
    const auto Image = ShellIconImage(Directory ? L"hc-folder" : L"hc-file", true, Pixels,
        Directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    if (!Image.isNull()) return QPixmap::fromImage(Image);
    return QFileIconProvider().icon(Directory ? QFileIconProvider::Folder : QFileIconProvider::File).pixmap(QSize(Pixels, Pixels), 1.0);
}
}

void CFSModel::publishProvider(std::vector<hc::storage::EntrySnapshot> Entries)
{
    directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    requestOnlyValues_ = false; columnQueries_->Reset(); columnSortRanks_.clear();
    // The directory tracker re-lists the open folder on changes and on its periodic rescan. Keep each folder's
    // measured size on screen while it is re-measured instead of turning every folder back to "calculating".
    // An explicit refresh or navigation starts without sizes.
    QHash<QString, quint64> KnownSizes;
    if (std::exchange(carryDirectorySizes_, false))
        for (const auto& Old : vecNode_)
            if (Old->storageEntry_ && Old->directorySize_) KnownSizes.insert(QString::fromStdString(hc::storage::Serialize(Old->storageEntry_->id_)), *Old->directorySize_);
    directorySizeStale_.clear();
    index_.Publish(Entries);
    QVector<nsHC::TySpFileSource> Items;
    if (GetRoot()->ReadLocation(GetCurrentPathWithRoot()).kind_ != hc::storage::LocatorKind::Shell || GetRootCount() > 1)
    {
        auto Parent = std::make_shared<nsHC::CFileSourceT>();
        Parent->Name_ = "..";
        Parent->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
        Items.push_back(Parent);
    }
    const auto FolderIcon = GenericIcon(true, iconPixels_);
    const auto FileIcon = GenericIcon(false, iconPixels_);
    for (auto& Entry : Entries)
    {
        nsHC::TySpFileSource Item(new nsHC::CFileSourceT, [](nsHC::CFileSourceT* Value) { if (Value->PIDL) CoTaskMemFree(Value->PIDL); delete Value; });
        Item->Name_ = QString::fromStdString(Entry.name_);
        Item->Ext_ = nsCmn::nsCmnPath::GetFileExtension(Item->Name_);
        Item->Attributes_ = Entry.attributes_;
        Item->Size_ = Entry.size_;
        if (Entry.createdUtcMs_) Item->Created_ = QDateTime::fromMSecsSinceEpoch(Entry.createdUtcMs_);
        if (Entry.modifiedUtcMs_) Item->Modified_ = QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
        if (hc::settings::FileColorRules::HasDecomposedName(Item->Name_)) Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD;
        if (Entry.hints_.contains("reparseTag")) Item->Reserved0_ = QString::fromStdString(Entry.hints_.at("reparseTag")).toUInt();
        if (Entry.hints_.contains("shellPidl"))
        {
            const auto Bytes = QByteArray::fromBase64(QByteArray::fromStdString(Entry.hints_.at("shellPidl")));
            Item->PIDL = static_cast<LPITEMIDLIST>(CoTaskMemAlloc(Bytes.size()));
            if (Item->PIDL) memcpy(Item->PIDL, Bytes.data(), Bytes.size());
            Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_PIDL;
        }
        // Generic icons avoid filesystem/network calls on the GUI thread.
        Item->Icon = Entry.directory_ ? FolderIcon : FileIcon;
        Item->storageEntry_ = std::move(Entry);
        if (Item->storageEntry_->directory_) {
            const auto Key = QString::fromStdString(hc::storage::Serialize(Item->storageEntry_->id_));
            if (const auto Known = KnownSizes.constFind(Key); Known != KnownSizes.cend()) {
                Item->directorySize_ = *Known; Item->Size_ = qint64(qMin<quint64>(*Known, (std::numeric_limits<qint64>::max)()));
                directorySizeStale_.insert(Key);
            }
        }
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
        // Measured folder sizes count toward the total, as when a measurement arrives.
        if (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) { ++Directories; if (Item->directorySize_) Size += *Item->directorySize_; }
        else { ++Files; Size += Item->Size_; }
    }
    beginResetModel();
    vecNode_ = std::move(Items);
    vecRowColors_ = std::move(Colors);
    fileCount_ = Files;
    directoryCount_ = Directories;
    totalSize_ = Size;
    refreshNameSortRanks();
    // Reset observers query sort roles synchronously. Publish snapshot ranks
    // before endResetModel so descending grid order keeps the worker's order.
    if (UsesIndexedOrder() || appendOrder_) {
        columnSortRanks_.resize(vecNode_.size()); std::iota(columnSortRanks_.begin(), columnSortRanks_.end(), 0);
    }
    endResetModel();
    refreshDisplayIcons();
    refreshColumnSortRanks();
    RequestDirectorySizes({}, true);
}

void CFSModel::ApplyDisplaySettings()
{
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    const bool IconsChanged = displaySettings_["iconMode"] != General["iconMode"];
    displaySettings_ = General;
    if (UsesIndexedOrder() && !loading_) requestPage(0, true);
    refreshNameSortRanks();
    if (IconsChanged) refreshDisplayIcons();
    refreshColumnSortRanks();
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
        {Qt::DisplayRole, Qt::DecorationRole, CFsModelT::USR_ROLE_HIDDENOPACITY, CFsModelT::USR_ROLE_SORT});
    RequestDirectorySizes({}, true);
}

void CFSModel::ApplyDirectorySizeSettings()
{
    directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    RequestDirectorySizes({}, true);
}

void CFSModel::RequestDirectorySizes(const QModelIndexList& Entries, bool Automatic)
{
    using namespace hc::storage;
    const auto Provider = SnapshotProvider();
    if (loading_ || !providerEnabled_ || !Provider) return;
    // An explicit request takes priority over an automatic index-only batch.
    if (!Automatic && !directorySizePending_.isEmpty()) {
        directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    }
    const auto Options = hc::settings::AppSettings().CommittedObject()["options"].toObject();
    const auto Mode = Options["folderSizeMode"].toString("everything");
    const auto Location = WorkspaceLocation();
    if (Automatic && !hc::folders::Automatic(Mode, Location)) return;
    const bool UseIndex = Options["folderSizeUseEverything"].toBool(true)
        && (Location.kind_ == LocatorKind::Local || Location.kind_ == LocatorKind::Smb);
    const bool IndexOnly = Automatic && Mode == "everything";
    if (IndexOnly && !UseIndex) return;
    std::vector<EntrySnapshot> Targets;
    QHash<QString, int> Rows;
    bool HadSizes = false;
    for (int Row = 0; Row < rowCount(); ++Row) {
        if (!Entries.isEmpty() && !Entries.contains(index(Row, 0))) continue;
        const auto& Item = vecNode_[Row];
        if (!Item->storageEntry_ || Item->Name_ == ".." || !Item->storageEntry_->directory_
            || (Item->Attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const auto Key = QString::fromStdString(Serialize(Item->storageEntry_->id_));
        // Automatic passes re-measure only carried-over sizes, and keep showing them meanwhile.
        const bool Refresh = Automatic && directorySizeStale_.contains(Key);
        if ((Automatic && Item->directorySize_ && !Refresh) || directorySizePending_.contains(Key)) continue;
        if (Item->directorySize_ && !Refresh) {
            totalSize_ -= *Item->directorySize_; Item->directorySize_.reset(); Item->Size_ = 0;
            createBuiltFsValues(Item, -1); HadSizes = true;
        }
        directorySizePending_.insert(Key); Targets.push_back(*Item->storageEntry_); Rows.insert(Key, Row);
    }
    if (Targets.empty()) {
        if (!Automatic && rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        return;
    }
    const auto Generation = generation_;
    const auto Token = directorySizeStop_.get_token();
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole});
    if (HadSizes) emit DirectorySizesChanged();
    struct SizeResult { ResourceId id_; Result<std::uint64_t> bytes_; };
    auto* Watcher = new QFutureWatcher<SizeResult>(this);
    connect(Watcher, &QFutureWatcher<SizeResult>::resultReadyAt, this, [this, Watcher, Generation, Token, Rows](int ResultIndex) {
        if (Generation != generation_ || Token.stop_requested()) return;
        const auto Result = Watcher->resultAt(ResultIndex);
        const auto Key = QString::fromStdString(Serialize(Result.id_));
        directorySizePending_.remove(Key); directorySizeStale_.remove(Key);
        const auto Row = Rows.value(Key, -1);
        if (Row >= 0 && Row < vecNode_.size()) {
            const auto& Item = vecNode_[Row];
            if (!Item->storageEntry_ || Item->storageEntry_->id_ != Result.id_) return;
            if (!Result.bytes_.HasValue()) { emit dataChanged(index(Row, 0), index(Row, columnCount() - 1)); return; }
            const auto Previous = Item->directorySize_.value_or(0);
            Item->directorySize_ = Result.bytes_.Value();
            totalSize_ -= Previous;
            totalSize_ += qMin<quint64>(Result.bytes_.Value(), (std::numeric_limits<quint64>::max)() - totalSize_);
            Item->Size_ = qint64(qMin<quint64>(Result.bytes_.Value(), (std::numeric_limits<qint64>::max)()));
            createBuiltFsValues(Item, -1);
            emit dataChanged(index(Row, 0), index(Row, columnCount() - 1));
            // Coalesce fast index results so one batch does not repeatedly sort
            // the full grid or recount the same selected rows on the GUI thread.
            if (!directorySizeNotifyPending_) {
                directorySizeNotifyPending_ = true;
                QTimer::singleShot(30, this, [this] {
                    directorySizeNotifyPending_ = false; refreshColumnSortRanks(); emit DirectorySizesChanged();
                });
            }
        }
    });
    connect(Watcher, &QFutureWatcher<SizeResult>::finished, this, [this, Watcher, Generation, Token, Automatic] {
        Watcher->deleteLater();
        if (!Automatic && Generation == generation_ && !Token.stop_requested()) RequestDirectorySizes({}, true);
    });
    const auto Parent = QString::fromStdString(Location.root_);
    Watcher->setFuture(QtConcurrent::run([Targets = std::move(Targets), Provider, Parent, Token, UseIndex, IndexOnly, Automatic](QPromise<SizeResult>& Promise) {
        OperationContext Context; Context.stopToken_ = Token;
        if (Automatic) { Context.maxEnumerationEntries_ = 1000000; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2); }
        Result<QMap<QString, quint64>> Indexed = Error{ErrorCategory::Unavailable, "folder-size-index-disabled"};
        if (UseIndex) Indexed = hc::folders::Indexed(Parent, Context);
        for (const auto& Entry : Targets) {
            if (Token.stop_requested()) break;
            Result<std::uint64_t> Bytes = Error{ErrorCategory::Unavailable, "folder-size-not-indexed"};
            const auto Name = QString::fromStdString(Entry.name_);
            try {
                if (Indexed.HasValue() && Indexed.Value().contains(Name)) Bytes = Indexed.Value()[Name];
                else if (!IndexOnly) Bytes = hc::folders::Measure(*Provider, Entry, Context);
            } catch (const std::exception&) { Bytes = Error{ErrorCategory::Internal, "folder-size-provider-exception"}; }
            Promise.addResult(SizeResult{Entry.id_, std::move(Bytes)});
        }
    }));
}

void CFSModel::refreshNameSortRanks()
{
    // Supply numeric ranks to the grid so locale and natural-number options
    // remain independent, without relying on its locale-only numeric comparator.
    const bool Locale = displaySettings_["sortLocaleAware"].toBool(true);
    const bool Numeric = displaySettings_["sortNumericAware"].toBool(true);
    const auto Case = displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QCollator Collator; Collator.setCaseSensitivity(Case); Collator.setNumericMode(Numeric);
    const auto Compare = [&](const QString& Left, const QString& Right) -> int {
        if (Locale) return Collator.compare(Left, Right);
        if (!Numeric) return QString::compare(Left, Right, Case);
        const auto Digit = [](QChar Char) { return Char >= QChar('0') && Char <= QChar('9'); };
        qsizetype L = 0, R = 0;
        while (L < Left.size() && R < Right.size()) {
            auto LE = L, RE = R;
            if (Digit(Left[L]) && Digit(Right[R])) {
                while (LE < Left.size() && Digit(Left[LE])) ++LE;
                while (RE < Right.size() && Digit(Right[RE])) ++RE;
                while (L < LE && Left[L] == '0') ++L;
                while (R < RE && Right[R] == '0') ++R;
                if (LE - L != RE - R) return LE - L < RE - R ? -1 : 1;
                const auto Compared = QString::compare(Left.mid(L, LE - L), Right.mid(R, RE - R), Qt::CaseSensitive);
                if (Compared) return Compared;
            } else {
                // Compare one character when only one side begins a number.
                if (Digit(Left[L]) != Digit(Right[R])) { ++LE; ++RE; }
                else {
                    while (LE < Left.size() && !Digit(Left[LE])) ++LE;
                    while (RE < Right.size() && !Digit(Right[RE])) ++RE;
                }
                const auto Compared = QString::compare(Left.mid(L, LE - L), Right.mid(R, RE - R), Case);
                if (Compared) return Compared;
            }
            L = LE; R = RE;
        }
        return L < Left.size() ? 1 : R < Right.size() ? -1 : 0;
    };
    QVector<int> Order; Order.reserve(vecNode_.size());
    for (int Row = 0; Row < vecNode_.size(); ++Row) Order.push_back(Row);
    std::stable_sort(Order.begin(), Order.end(), [&](int Left, int Right) {
        const auto Group = [&](int Row) { return vecNode_[Row]->Name_ == ".." ? 0 : (vecNode_[Row]->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2; };
        if (Group(Left) != Group(Right)) return Group(Left) < Group(Right);
        const auto Name = [&](int Row) { const auto& Item = vecNode_[Row];
            return Item->columnValues_.isEmpty() ? Item->Name_ : Item->columnValues_.front().Raw().toString(); };
        return Compare(Name(Left), Name(Right)) < 0;
    });
    nameSortRanks_.resize(Order.size());
    for (int Rank = 0; Rank < Order.size(); ++Rank) nameSortRanks_[Order[Rank]] = Rank;
}

void CFSModel::SetIconPixels(int Pixels)
{
    Pixels = std::clamp(Pixels, 16, 512);
    if (iconPixels_ == Pixels) return;
    iconPixels_ = Pixels;
    if (!vecNode_.isEmpty()) refreshDisplayIcons();
}

void CFSModel::refreshDisplayIcons()
{
    iconStop_.request_stop(); iconStop_ = std::stop_source{};
    const auto Generation = ++iconGeneration_;
    const auto Mode = displaySettings_["iconMode"].toString("standard");
    const int Pixels = iconPixels_;
    // Device ratio 1: the pixmap holds exactly Pixels physical pixels and the view paints it at the logical size.
    const auto FolderIcon = GenericIcon(true, Pixels);
    const auto FileIcon = GenericIcon(false, Pixels);
    struct IconRequest { QString name_; QString path_; bool directory_; };
    QVector<IconRequest> Requests;
    for (auto& Item : vecNode_) {
        const bool Directory = Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY;
        Item->Icon = Mode == "none" || Item->Name_ == ".." ? QPixmap{} : Directory ? FolderIcon : FileIcon;
        QString Path;
        if (Item->storageEntry_ && Item->storageEntry_->locator_.nesting_.empty()
            && (Item->storageEntry_->locator_.kind_ == hc::storage::LocatorKind::Local || Item->storageEntry_->locator_.kind_ == hc::storage::LocatorKind::Smb))
            Path = QString::fromStdString(Item->storageEntry_->locator_.root_);
        Requests.push_back({Item->Name_, Path, Directory});
    }
    if (Mode != "all" && Mode != "associated") return;
    auto* Watcher = new QFutureWatcher<QVector<QImage>>(this);
    connect(Watcher, &QFutureWatcher<QVector<QImage>>::finished, this, [this, Watcher, Generation] {
        const auto Images = Watcher->result(); Watcher->deleteLater();
        if (Generation != iconGeneration_ || Images.size() != vecNode_.size()) return;
        for (int Row = 0; Row < Images.size(); ++Row) if (!Images[Row].isNull()) vecNode_[Row]->Icon = QPixmap::fromImage(Images[Row]);
        if (rowCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DecorationRole});
    });
    const auto Stop = iconStop_.get_token();
    Watcher->setFuture(QtConcurrent::run([Requests, Mode, Stop, Pixels] {
        const auto Com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto Release = qScopeGuard([&] { if (SUCCEEDED(Com)) CoUninitialize(); });
        QVector<QImage> Images(Requests.size()); QHash<QString, QImage> Cache;
        for (int Row = 0; Row < Requests.size() && !Stop.stop_requested(); ++Row) {
            const auto& Request = Requests[Row];
            if (Request.directory_) continue;
            const auto Extension = QFileInfo(Request.name_).suffix().toLower();
            const bool Specific = Mode == "all" && !Request.path_.isEmpty() && (Extension == "exe" || Extension == "lnk" || Extension == "ico");
            const auto Key = Specific ? Request.path_ : Extension;
            if (Cache.contains(Key)) { Images[Row] = Cache[Key]; continue; }
            const auto Name = (Specific ? Request.path_ : "hc-icon." + Extension).toStdWString();
            Images[Row] = ShellIconImage(Name, !Specific, Pixels);
            Cache.insert(Key, Images[Row]);
        }
        return Images;
    }));
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
    for (const auto& Item : Items)
    {
        Item->VecContent.clear();
        Item->columnValues_.clear();
        for (const auto& Column : columnView_.VecColumns)
        {
            const auto Value = evaluateColumn(Item, Column, false);
            Item->columnValues_.append(Value);
            Item->VecContent.append(CColumnMgr::FormatValue(Column.definition_, Value, Item.get()));
        }
    }
}

void CFSModel::createBuiltFsValues(const nsHC::TySpFileSource& Item, int Column)
{
    if (!Item) return;
    if (Column < 0) {
        for (int Index = 0; Index < columnView_.VecColumns.size(); ++Index) createBuiltFsValues(Item, Index);
        return;
    }
    if (Column >= columnView_.VecColumns.size()) return;
    const auto& Definition = columnView_.VecColumns[Column].definition_;
    const auto Value = evaluateColumn(Item, columnView_.VecColumns[Column], false);
    Item->columnValues_.resize(columnView_.VecColumns.size());
    Item->VecContent.resize(columnView_.VecColumns.size());
    Item->columnValues_[Column] = Value;
    Item->VecContent[Column] = CColumnMgr::FormatValue(Definition, Value, Item.get());
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
    if (auto* Model = qobject_cast<CFSModel*>(sourceModel())) Model->SetPageFilter(search_, IsShowHiddenSystem_, searchMatch_);
    
    invalidateFilter();
}

void CFSProxyModel::SetSearch(const QString& Query, const QString& Match)
{
    search_ = Query.left(4096); searchMatch_ = Match;
    if (auto* Model = qobject_cast<CFSModel*>(sourceModel())) Model->SetPageFilter(search_, IsShowHiddenSystem_, searchMatch_);
    invalidateFilter();
}

void CFSProxyModel::setSourceModel(QAbstractItemModel* Source)
{
    QSortFilterProxyModel::setSourceModel(Source);
    if (auto* Model = qobject_cast<CFSModel*>(Source)) Model->SetPageFilter(search_, IsShowHiddenSystem_, searchMatch_);
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
        if (const auto Model = qobject_cast<const CFSModel*>(sourceModel()); Model && Model->UsesIndexedOrder()) return true;
        const auto Attr = Index.data( CFSModel::USR_ROLE_ATTRIBUTE ).toUInt();
        if (!IsShowHiddenSystem_ && (FlagOn(Attr, FILE_ATTRIBUTE_HIDDEN) || FlagOn(Attr, FILE_ATTRIBUTE_SYSTEM))) return false;
        const auto Model = qobject_cast<const CFSModel*>(sourceModel());
        const auto Item = Model ? Model->GetFileInfo(Index) : nsHC::TySpFileSource{};
        if (Item && Item->storageEntry_ && !(searchMatch_.isEmpty() ? Model->MatchesSearch(*Item->storageEntry_, search_) : hc::quickfind::Matches(QString::fromStdString(Item->storageEntry_->name_), search_, searchMatch_))) return false;
    }

    return QSortFilterProxyModel::filterAcceptsRow( source_row, source_parent );
}

QVector<hc::columns::FieldDefinition> CFSModel::AvailableColumns() const
{
    using namespace hc::columns;
    auto Fields = PluginFields::Instance().Fields(); Fields += ScriptFields(columnView_.profile_);
    if (SnapshotProvider()) for (const auto& Column : SnapshotProvider()->Columns())
        Fields.append({QString::fromStdString(Column.key_), tr("VFS / %1").arg(QString::fromStdString(Column.title_)), Column.flags_ & 112 ? FieldType::Real : FieldType::Text, ProviderKind::DOpus, EvaluationCost::Snapshot, true, Column.flags_ & 4 ? Qt::AlignCenter : Column.flags_ & 1 ? Qt::AlignRight : Qt::AlignLeft});
    return Fields;
}

QVector<hc::columns::PreviewSample> CFSModel::ColumnSamples(const QStringList& Selection) const
{
    using namespace hc::columns; QVector<PreviewSample> Samples;
    const auto Fields = AvailableColumns();
    for (const auto& Item : vecNode_) {
        if (Item->Name_ == "..") continue;
        PreviewSample Sample; Sample.identity_ = Item->storageEntry_ ? hc::workspace::SelectionKey(*Item->storageEntry_) : Item->Name_;
        if (!Selection.isEmpty() && !Selection.contains(Sample.identity_)) continue;
        Sample.path_ = queryPath(Item); Sample.name_ = Item->Name_; Sample.directory_ = Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY;
        for (const auto& Field : Fields) if (Field.cost_ == EvaluationCost::Snapshot) Sample.values_[Field.id_] = readField(Item, Field.id_, false, false);
        if (Item->storageEntry_ && SnapshotProvider()) Sample.source_ = std::make_shared<hc::storage::ProviderContentSource>(SnapshotProvider(), *Item->storageEntry_);
        Samples.append(std::move(Sample)); if (Samples.size() == 50) break;
    }
    return Samples;
}

QString CFSModel::queryPath(const nsHC::TySpFileSource& Item) const
{
    if (!Item->storageEntry_ || Item->Name_ == "..") return {};
    const auto& Location = Item->storageEntry_->locator_;
    if (Location.kind_ != hc::storage::LocatorKind::Local && Location.kind_ != hc::storage::LocatorKind::Smb) return {};
    return QDir::toNativeSeparators(QDir(GetCurrentPathWithRoot()).filePath(Item->Name_));
}

hc::columns::CellValue CFSModel::readField(const nsHC::TySpFileSource& Item, const QString& Field, bool Request, bool Urgent) const
{
    using namespace hc::columns;
    if (Field.startsWith("lua.")) {
        if (Item->Name_ == "..") return CellValue::Missing(ValueState::Empty);
        const auto* Script = FindScript(columnView_.profile_, Field);
        if (!Script || !Script->enabled_) return CellValue::Missing(ValueState::Unsupported, "script-disabled-or-missing");
        QJsonObject Values; std::optional<CellValue> Missing;
        for (const auto& Dependency : Script->dependencies_) {
            const auto Value = readField(Item, Dependency, Request, Urgent);
            if (Value.State() == ValueState::Ready) Values[Dependency] = EncodeScriptInput(Value);
            else if (Value.State() != ValueState::Empty && (!Missing || Value.State() == ValueState::Pending)) Missing = Value;
        }
        if (Missing) return *Missing;
        const auto Identity = Item->storageEntry_ ? hc::workspace::SelectionKey(*Item->storageEntry_) : Item->Name_;
        const auto Envelope = QJsonDocument(QJsonObject{{"identity", Identity}, {"values", Values},
            {"source", scriptRevisions_.value(Script->id_)}}).toJson(QJsonDocument::Compact);
        if (Envelope.size() > 65536) return CellValue::Missing(ValueState::Error, "script-input-budget");
        return columnQueries_->Read(QString::fromUtf8(Envelope), Field, Request, Urgent);
    }
    if (IsVfsColumn(Field)) {
        if (!Item->storageEntry_ || !SnapshotProvider()) return CellValue::Missing(ValueState::Unsupported);
        for (const auto& Definition : SnapshotProvider()->Columns()) if (QString::fromStdString(Definition.key_) == Field) {
            const auto It = Item->storageEntry_->hints_.find(Definition.valueKey_);
            return It == Item->storageEntry_->hints_.end() ? CellValue::Missing(ValueState::Empty) : VfsColumnCell(QString::fromStdString(It->second), Definition.flags_);
        }
        return CellValue::Missing(ValueState::Unsupported);
    }
    if (!QueryFamily(Field).isEmpty()) {
        const auto Path = queryPath(Item);
        if (Path.isEmpty()) return CellValue::Missing(Item->Name_ == ".." ? ValueState::Empty : ValueState::Unsupported);
        if ((Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) && Field != "file.linkTarget" && !IsPluginField(Field)) return CellValue::Missing(ValueState::Empty);
        auto Value = columnQueries_->Read(Path, Field, Request, Urgent);
        if (IsPluginField(Field) && Value.State() == ValueState::OnDemand) requestOnlyValues_ = true;
        return Value;
    }
    return CColumnMgr::GetFieldValue(Field, Item.get(), GetCurrentPathWithRoot());
}

hc::columns::CellValue CFSModel::evaluateColumn(const nsHC::TySpFileSource& Item, const Column& Binding, bool Request, bool Sort) const
{
    const auto& Definition = Binding.definition_;
    if (Sort && !Definition.sortField_.isEmpty()) return readField(Item, Definition.sortField_, Request, !Sort);
    if (Definition.template_.isEmpty()) return readField(Item, Definition.fieldId_, Request, !Sort);
    return hc::columns::RenderTemplate(Binding.compiled_, Definition.format_, [&](const QString& Field) { return readField(Item, Field, Request, !Sort); });
}

void CFSModel::columnResultsReady(const QSet<QString>& Paths)
{
    if (loading_) return;
    emit ColumnValuesChanging();
    QSet<QString> ScriptItems;
    for (const auto& Path : Paths) if (Path.startsWith('{')) ScriptItems.insert(QJsonDocument::fromJson(Path.toUtf8()).object()["identity"].toString());
    int First = rowCount(), Last = -1;
    for (int Row = 0; Row < vecNode_.size(); ++Row) if (Paths.contains(queryPath(vecNode_[Row])) ||
        ScriptItems.contains(vecNode_[Row]->storageEntry_ ? hc::workspace::SelectionKey(*vecNode_[Row]->storageEntry_) : vecNode_[Row]->Name_)) {
        createBuiltFsValues(vecNode_[Row], -1); First = std::min(First, Row); Last = Row;
    }
    if (Last >= First && columnCount()) emit dataChanged(index(First, 0), index(Last, columnCount() - 1), {Qt::DisplayRole, Qt::ToolTipRole, ColumnStateRole});
    refreshColumnSortRanks();
    emit ColumnValuesChanged();
}

void CFSModel::CancelColumnQueries()
{
    pageStop_.request_stop(); ++pageGeneration_; pageBusy_ = false;
    const bool HadPending = columnQueries_->Pending() != 0;
    const bool SortChanged = columnSorts_ != completedSorts_;
    columnQueries_->Cancel(); columnSorts_ = completedSorts_;
    if (SortChanged) emit ColumnSortCancelled();
    emit ColumnQueryStatus(tr("컬럼 조회가 중지되었습니다."));
    // Ready cached values did not change. Invalidating all rows would force a
    // synchronous grid sort even when there was no query to cancel.
    if (HadPending && rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole, ColumnStateRole});
}
void CFSModel::ResumeColumnQueries()
{
    requestOnlyValues_ = false;
    columnQueries_->SetLoader(hc::columns::WithLuaColumns(hc::columns::PluginFields::Instance().Loader(true), columnView_.profile_, QUuid::createUuid().toString()));
    refreshColumnSortRanks();
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::DisplayRole, ColumnStateRole});
}

void CFSModel::SetColumnSorts(const QVector<hc::columns::SortDefinition>& Sorts)
{
    if (Sorts == columnSorts_ && columnSortRanks_.size() == vecNode_.size()) return;
    if (Sorts != columnSorts_) appendOrder_ = false;
    if (UsesIndexedOrder()) { if (Sorts != columnSorts_) { columnSorts_ = Sorts; if (!loading_) requestPage(0, true); } return; }
    columnQueries_->Resume(); columnSorts_ = Sorts; refreshColumnSortRanks();
}

void CFSModel::refreshColumnSortRanks()
{
    using namespace hc::columns;
    if (loading_) return;
    if (UsesIndexedOrder() || appendOrder_) {
        columnSortRanks_.resize(vecNode_.size()); std::iota(columnSortRanks_.begin(), columnSortRanks_.end(), 0);
        return;
    }
    if (columnSorts_.isEmpty()) {
        columnQueries_->EndSnapshot(); columnSortRanks_.clear(); completedSorts_.clear();
        emit ColumnQueryStatus(columnQueries_->Pending() ? tr("컬럼 조회 중… %1건").arg(columnQueries_->Pending()) : requestOnlyValues_ ? tr("요청형 값이 있습니다. 다시 조회를 누르면 계산합니다.") : QString{}); return;
    }
    QVector<SortRow> Rows; QVector<Qt::SortOrder> Orders; QVector<const Column*> Bindings;
    for (const auto& Sort : columnSorts_) {
        const auto It = std::find_if(columnView_.VecColumns.begin(), columnView_.VecColumns.end(), [&](const auto& Col) { return Col.definition_.id_ == Sort.columnId_; });
        if (It == columnView_.VecColumns.end()) continue;
        Orders.append(Sort.order_); Bindings.append(&*It);
    }
    if (Bindings.isEmpty()) return;
    QSet<QString> Families;
    for (const auto* Binding : Bindings) {
        auto Fields = Binding->compiled_.dependencies_;
        if (Binding->definition_.template_.isEmpty()) Fields = {Binding->definition_.fieldId_};
        if (!Binding->definition_.sortField_.isEmpty()) Fields = {Binding->definition_.sortField_};
        for (const auto& Field : Fields) if (!QueryFamily(Field).isEmpty()) Families.insert(QueryFamily(Field));
    }
    if (qint64(vecNode_.size()) * Families.size() > 4096) { emit ColumnQueryStatus(tr("정렬 조회 한도(4096건)를 초과했습니다. 범위를 줄이거나 중지하세요.")); return; }
    columnQueries_->BeginSnapshot();
    int Pending = 0; QSet<QString> Jobs;
    for (const auto& Item : vecNode_) {
        SortRow Row; Row.parent_ = Item->Name_ == ".."; Row.directory_ = Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY;
        Row.id_ = Item->storageEntry_ ? hc::workspace::SelectionKey(*Item->storageEntry_) : Item->Name_;
        for (const auto* Binding : Bindings) {
            auto Value = evaluateColumn(Item, *Binding, false, true);
            if (Value.State() == ValueState::Pending) {
                ++Pending;
                auto Fields = Binding->compiled_.dependencies_;
                if (Binding->definition_.template_.isEmpty()) Fields = {Binding->definition_.fieldId_};
                if (!Binding->definition_.sortField_.isEmpty()) Fields = {Binding->definition_.sortField_};
                for (const auto& Field : Fields) if (!QueryFamily(Field).isEmpty()) Jobs.insert(queryPath(Item) + QChar(0) + QueryFamily(Field));
            }
            Row.values_.append(Value);
        }
        Rows.append(Row);
    }
    if (Pending) {
        if (Jobs.size() > 4096) { emit ColumnQueryStatus(tr("정렬 조회 한도(4096건)를 초과했습니다. 범위를 줄이거나 중지하세요.")); return; }
        for (const auto& Item : vecNode_) for (const auto* Binding : Bindings) evaluateColumn(Item, *Binding, true, true);
        emit ColumnQueryStatus(tr("정렬 값을 조회 중… 남은 값 %1개").arg(Pending)); return;
    }
    columnSortRanks_ = SortRanks(Rows, Orders, displaySettings_["sortLocaleAware"].toBool(true), displaySettings_["sortNumericAware"].toBool(true),
        displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    columnQueries_->EndSnapshot();
    completedSorts_ = columnSorts_; emit ColumnQueryStatus(columnQueries_->Pending() ? tr("컬럼 조회 중… %1건").arg(columnQueries_->Pending()) : requestOnlyValues_ ? tr("요청형 값이 있습니다. 다시 조회를 누르면 계산합니다.") : QString{});
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {GridSortRole});
}
