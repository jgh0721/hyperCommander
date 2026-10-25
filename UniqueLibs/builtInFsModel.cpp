#include "stdafx.h"
#include "builtInFsModel.hpp"
#include "shellDriveSort.hpp"
#include "fileIconService.hpp"
#include "quickFindSettings.hpp"
#include "everythingSearchSession.hpp"
#include "directoryRefreshOrder.hpp"
#include "columnNativeFields.hpp"
#include "folderSize.hpp"
#include "nativeFileElevation.hpp"
#include <QPromise>
#include <QPointer>
#include <QThreadPool>
#include <QScopeGuard>
#include <QPainter>
#include <QStyle>
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

    refreshDisplayIcons(int(Row));

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

void CFSModel::SetClipboardCut(const QSet<QString>& Keys, const QString& Root)
{
    if (clipboardCutKeys_ == Keys && clipboardCutRoot_ == Root) return;
    clipboardCutKeys_ = Keys; clipboardCutRoot_ = Root;
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {ClipboardCutRole});
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

    // Group roles are snapshot values and do not need file-color evaluation.
    const bool GroupOnly = std::all_of(roleDataSpan.begin(), roleDataSpan.end(), [](const auto& Role) {
        return Role.role() == GroupKeyRole || Role.role() == GroupOrderRole || Role.role() == GroupTextRole;
    });
    if (GroupOnly) {
        const auto& Definition = columnView_.VecColumns[Col].definition_;
        const auto Value = hc::columns::MakeGroupValue(Definition.fieldId_, grouping_.mode_, readField(vecNode_[Row], Definition.fieldId_, false), groupingDate_, groupingZone_);
        for (auto& Role : roleDataSpan) Role.setData(Role.role() == GroupKeyRole ? QVariant(Value.key_)
            : Role.role() == GroupOrderRole ? Value.order_ : QVariant(Value.text_));
        return;
    }

    // Sorting, measuring columns and reading names must never evaluate colors.
    const bool NeedsStyle = std::any_of(roleDataSpan.begin(), roleDataSpan.end(), [](const auto& Role) {
        return Role.role() == Qt::ForegroundRole || Role.role() == Qt::BackgroundRole
            || Role.role() == Qt::FontRole || Role.role() == FileColorOverrideRole || Role.role() == FileEffectsRole;
    });
    const auto RuleStyle = NeedsStyle ? fileRuleStyle(Row) : hc::settings::FileRuleResult{};
    for( QModelRoleData& RoleWith : roleDataSpan )
    {
        const int Role = RoleWith.role();
        const auto& Item = vecNode_[ Row ];

        switch( Role )
        {
            case GroupKeyRole: case GroupOrderRole: case GroupTextRole: {
                const auto& Definition = columnView_.VecColumns[Col].definition_;
                const auto Value = hc::columns::MakeGroupValue(Definition.fieldId_, grouping_.mode_, readField(Item, Definition.fieldId_, false), groupingDate_, groupingZone_);
                RoleWith.setData(Role == GroupKeyRole ? QVariant(Value.key_) : Role == GroupOrderRole ? Value.order_ : QVariant(Value.text_));
            } break;
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

            case FileEffectsRole: { RoleWith.setData(RuleStyle.font_); } break;
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
            case ClipboardCutRole: {
                RoleWith.setData(Item->Name_ != ".." && Item->storageEntry_
                    && ((!clipboardCutKeys_.isEmpty() && clipboardCutKeys_.contains(hc::workspace::SelectionKey(*Item->storageEntry_)))
                        || (!clipboardCutRoot_.isEmpty() && clipboardCutRoot_ == hc::workspace::LocationKey(WorkspaceLocation()))));
            } break;
        }
    }
}

void CFSModel::doRefresh()
{
    if (searchResults_) { emit SearchRefreshRequested(); return; }
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

    refreshDisplayIcons();


}

CFSModel::CFSModel()
{
    fileRulePump_.setSingleShot(true);
    connect(&fileRulePump_, &QTimer::timeout, this, &CFSModel::pumpFileRules);
    fileRuleCompleteTimer_.setSingleShot(true);
    connect(&fileRuleCompleteTimer_, &QTimer::timeout, this, &CFSModel::completeFileRuleStyles);
    connect(this, &QAbstractItemModel::modelAboutToBeReset, this, &CFSModel::resetFileRules);
    iconApplyTimer_.setSingleShot(true); iconApplyTimer_.setInterval(0);
    connect(&iconApplyTimer_, &QTimer::timeout, this, [this] { applyRequestedIcons(); });
    hc::icons::FileIconService::Instance().Subscribe(this, [this] { if (!iconApplyTimer_.isActive()) iconApplyTimer_.start(); });
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
            invalidateDisplayIcons(false, 500);
            carryDirectorySizes_ = true; autoRefreshRequested_ = true; Refresh();
        }
    });
    connect(this, &CFSModel::sigRefreshCompleted, this, [this] { tracker_.SetBusy(loading_ || selectionInputActive_); });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState State) {
        const auto Settings = hc::refresh::Settings::FromJson(hc::settings::AppSettings().CommittedObject()["general"].toObject()["refresh"].toObject());
        tracker_.SetSuspended(Settings.pauseInBackground && State != Qt::ApplicationActive);
        hc::icons::FileIconService::Instance().SetAutomaticRefresh(this, !Settings.Excludes(GetCurrentPathWithRoot())
            && (!Settings.pauseInBackground || State == Qt::ApplicationActive));
        if (State == Qt::ApplicationActive && !iconApplyTimer_.isActive()) iconApplyTimer_.start();
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

void CFSModel::SetSelectionInputActive(bool Active)
{
    if (selectionInputActive_ == Active) return;
    selectionInputActive_ = Active;
    // Keep native notifications pending until a held selection key is released.
    tracker_.SetBusy(loading_ || selectionInputActive_);
}

void CFSModel::SetWatchEnabled(bool Enabled)
{
    watchEnabled_ = Enabled;
    const auto Location = WorkspaceLocation();
    const auto Root = GetRoot();
    // Registering a native watcher on an unreachable server can block the GUI.
    const auto Native = Root && !Root->IsRemote() && Location.kind_ == hc::storage::LocatorKind::Local
        && Location.nesting_.empty() ? QString::fromStdString(Location.root_) : QString{};
    tracker_.SetLocation(Enabled && Root && providerEnabled_ && !searchResults_ ? hc::workspace::LocationKey(Location) : QString{}, Native,
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
    fileRuleStop_.request_stop();
    hc::icons::FileIconService::Instance().Unsubscribe(this);
    everythingSession_.reset();
    navigationStop_.request_stop(); iconStop_.request_stop(); pageStop_.request_stop(); directorySizeStop_.request_stop();
    if (FsIconGatherer_)
    {
        if (FsIconGatherer_) FsIconGatherer_->requestInterruption();
        QThreadPool::globalInstance()->waitForDone();
        delete FsIconGatherer_;
    }
}

void CFSModel::CancelNavigation(bool Superseding)
{
    automaticRefresh_ = autoRefreshRequested_ = false;
    failedNavigationRetry_ = {};
    directorySizeStop_.request_stop(); directorySizeStop_ = std::stop_source{}; directorySizePending_.clear();
    setProperty("hc.restoreTicket", QString{}); setProperty("hc.restorePending", false);
    emit NavigationCancelled();
    navigationStop_.request_stop();
    const auto Generation = ++generation_;
    pageStop_.request_stop(); ++pageGeneration_; pageBusy_ = false;
    if (!loading_) return;
    if (!Superseding)
    {
        loading_ = false;
        navigationError_ = hc::storage::Error{hc::storage::ErrorCategory::Cancelled, "cancelled"};
        emit sigNavigationFailed("cancelled");
        emit sigRefreshCompleted();
        return;
    }
    // A superseding request starts in this same call. Publishing a cancelled
    // listing in between would flash a failure notice and consume a pending
    // selection restore, so only the request's outcome is reported now; the
    // panel settles as cancelled only if no request has arrived by the next turn.
    emit sigNavigationFailed("cancelled");
    QTimer::singleShot(0, this, [this, Generation] {
        if (generation_ != Generation || !loading_) return;
        loading_ = false;
        navigationError_ = hc::storage::Error{hc::storage::ErrorCategory::Cancelled, "cancelled"};
        emit sigRefreshCompleted();
    });
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
    emit DirectoryReadStarted(TargetFullPath, Location.kind_ == LocatorKind::Archive);
    OperationContext Context;
    Context.requestId_ = std::to_string(Generation);
    Context.stopToken_ = navigationStop_.get_token();
    Context.freshEnumerationMetadata_ = true;
    Context.allowUnavailableChildMetadata_ = true;
    struct Listing { std::shared_ptr<PagedEntryIndex> source_, view_; EntryPage page_; std::shared_ptr<hc::permissions::DirectoryElevationSession> session_; std::vector<hc::settings::FileRuleResult> styles_; };
    using ReadResult = Result<Listing>;
    const auto Rules = fileColorRules_; const auto RuleGeneration = fileRuleLoadGeneration_; const bool Dark = colorScheme_.IsDarkMode;
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
    connect(Watcher, &QFutureWatcher<ReadResult>::finished, this, [this, Watcher, Generation, TargetRoots, TargetPath, Provider, Location, Query, Match, ShowHidden, Sorts, Fallback, Display, Append, RuleGeneration, Dark] {
        auto Result = Watcher->result();
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
        pageSorts_ = Sorts; completedSorts_ = Sorts;
        appendOrder_ = Append;
        if (UsesIndexedOrder() && Fallback) { columnSorts_.clear(); completedSorts_.clear(); pageSorts_.clear(); emit ColumnSortCancelled(); }
        publishProvider(Result.Value().page_.entries_, fileRuleSeedsUsable(RuleGeneration, Dark) ? std::move(Result.Value().styles_) : std::vector<hc::settings::FileRuleResult>{});
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
    Watcher->setFuture(QtConcurrent::run([Provider, Location, Context, Compare, Query, Match, ShowHidden, Elevated, Session, Append, Previous, SortPrevious, Rules, Dark, PreviousRows = std::move(PreviousRows)]() mutable -> ReadResult {
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
            auto Styles = seedFileRuleStyles(Rules, Page.Value().entries_, Dark, Context.stopToken_);
            return Listing{Source.Value(), View.Value(), std::move(Page.Value()), std::move(ActiveSession), std::move(Styles)};
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
        columnSorts_ = pageSorts_; completedSorts_ = pageSorts_; emit ColumnSortCancelled();
        emit ColumnQueryStatus(tr("이 컬럼은 전체 목록의 정렬 값 수집을 아직 지원하지 않습니다.")); return;
    }
    pageStop_.request_stop(); pageStop_ = std::stop_source{};
    pageBusy_ = true;
    const auto Generation = ++pageGeneration_; const auto Navigation = generation_;
    const auto Source = pageSource_; const auto Current = pageView_;
    const auto Query = pageQuery_; const auto Match = pageMatch_; const auto ShowHidden = pageShowHidden_; const auto Metadata = index_;
    const auto Sorts = Rebuild ? columnSorts_ : pageSorts_;
    OperationContext Context; Context.stopToken_ = pageStop_.get_token();
    struct Listing { std::shared_ptr<PagedEntryIndex> view_; EntryPage page_; std::vector<hc::settings::FileRuleResult> styles_; };
    using PageResult = Result<Listing>;
    const auto Rules = fileColorRules_; const auto RuleGeneration = fileRuleLoadGeneration_; const bool Dark = colorScheme_.IsDarkMode;
    auto* Watcher = new QFutureWatcher<PageResult>(this);
    emit ColumnQueryStatus(tr("목록을 준비하고 있습니다…"));
    connect(Watcher, &QFutureWatcher<PageResult>::finished, this, [this, Watcher, Generation, Navigation, Offset, Sorts, RuleGeneration, Dark] {
        auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation != pageGeneration_ || Navigation != generation_) return;
        pageBusy_ = false;
        if (!Result.HasValue()) {
            columnSorts_ = pageSorts_; completedSorts_ = pageSorts_; emit ColumnSortCancelled();
            emit ColumnQueryStatus(QString::fromStdString(Result.Failure().code_)); return;
        }
        pageView_ = Result.Value().view_; pageSorts_ = Sorts; completedSorts_ = Sorts;
        publishProvider(Result.Value().page_.entries_, fileRuleSeedsUsable(RuleGeneration, Dark) ? std::move(Result.Value().styles_) : std::vector<hc::settings::FileRuleResult>{});
        emit PageChanged(); emit sigRefreshCompleted(); emit ColumnQueryStatus({});
    });
    Watcher->setFuture(QtConcurrent::run([Source, Current, Offset, Rebuild, Compare, Query, Match, ShowHidden, Metadata, Context, Rules, Dark]() -> PageResult {
        try {
            auto View = Rebuild ? Source->Project(Compare.Value(), [Query, Match, ShowHidden, Metadata](const EntrySnapshot& Entry) {
                return (ShowHidden || !(Entry.attributes_ & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) && (Match.isEmpty() ? Metadata.Matches(Entry, Query) : hc::quickfind::Matches(QString::fromStdString(Entry.name_), Query, Match));
            }, Context, {}) : Result<std::shared_ptr<PagedEntryIndex>>(Current);
            if (!View.HasValue()) return View.Failure();
            auto Page = View.Value()->Read(0, (std::numeric_limits<std::size_t>::max)(), Context); if (!Page.HasValue()) return Page.Failure();
            auto Styles = seedFileRuleStyles(Rules, Page.Value().entries_, Dark, Context.stopToken_);
            return Listing{View.Value(), std::move(Page.Value()), std::move(Styles)};
        } catch (const std::exception&) { return Error{ErrorCategory::Internal, "page-worker-exception"}; }
    }));
}

std::vector<hc::settings::FileRuleResult> CFSModel::seedFileRuleStyles(const hc::settings::FileColorRules& Rules, const std::vector<hc::storage::EntrySnapshot>& Entries, bool Dark, std::stop_token Token)
{
    // Runs inside the listing worker while the rows are still private to it, so
    // the first paint already carries group colors. Pages above the threshold
    // are sorted, so the leading entries are the top of the view; rows past the
    // budget fall back to the pump.
    std::vector<hc::settings::FileRuleResult> Styles;
    if (!Rules.HasRules()) return Styles;
    hc::settings::FileColorRules::EvaluationContext Context;
    QElapsedTimer Clock; Clock.start();
    const auto Limit = std::min<std::size_t>(Entries.size(), FileRuleSeedRows);
    Styles.reserve(Limit);
    const auto Reference = QDateTime::currentMSecsSinceEpoch();
    for (std::size_t At = 0; At < Limit; ++At) {
        if (Token.stop_requested() || Clock.elapsed() > FileRuleSeedBudgetMs) break;
        const auto& Entry = Entries[At];
        const bool Folder = (Entry.attributes_ & FILE_ATTRIBUTE_DIRECTORY) != 0;
        hc::settings::FileRuleInput Input{QString::fromStdString(Entry.name_), Entry.attributes_, Folder ? 0 : Entry.size_, Dark};
        if (Entry.modifiedUtcMs_) Input.modifiedUtcMs_ = hc::settings::FileColorRules::MetadataTime(Entry.modifiedUtcMs_);
        if (const auto Tag = Entry.hints_.find("reparseTag"); Tag != Entry.hints_.end()) {
            if (const auto Value = QString::fromStdString(Tag->second).toUInt()) Input.reparseTag_ = Value;
        }
        Input.referenceUtcMs_ = Reference;
        Styles.push_back(Rules.EvaluateStyle(Input, &Context));
    }
    return Styles;
}

bool CFSModel::fileRuleSeedsUsable(quint64 RuleGeneration, bool Dark) const
{
    return RuleGeneration == fileRuleLoadGeneration_ && Dark == colorScheme_.IsDarkMode && fileColorRules_.HasRules();
}

namespace {
QThreadPool* DirectorySizePool()
{
    // Slow traversal/index I/O must not occupy the workers that read a new folder.
    static QPointer<QThreadPool> Pool;
    if (!Pool) { Pool = new QThreadPool(qApp); Pool->setMaxThreadCount(2); Pool->setObjectName("hc.directorySizePool"); }
    return Pool;
}

QThreadPool* FileRulePool()
{
    // Color batches are short; a lane of their own keeps the first batch from
    // queueing behind listing, column and search workers in the global pool.
    static QPointer<QThreadPool> Pool;
    if (!Pool) { Pool = new QThreadPool(qApp); Pool->setMaxThreadCount(1); Pool->setObjectName("hc.fileRulePool"); }
    return Pool;
}

QPixmap GenericIcon(bool Directory, int Pixels, bool Explorer = true)
{
    return hc::icons::FileIconService::Instance().DefaultIcon(Directory, Pixels, Explorer);
}
struct DisplayIconQueries {
    hc::icons::Request base_, overlay_;
    bool overlayEnabled_ = false;
};
DisplayIconQueries IconQueries(const nsHC::TySpFileSource& Item, const QJsonObject& Settings, int Pixels, const QString& LegacyBase = {})
{
    using namespace hc::icons;
    const bool Directory = Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY;
    const auto Mode = Settings["iconMode"].toString("standard");
    Request Base; Base.pixels_ = Pixels; Base.directory_ = Directory;
    Base.kind_ = Directory ? Kind::Folder : Mode == "standard" ? Kind::File : Kind::Associated;
    Base.extension_ = Base.kind_ == Kind::Associated ? QFileInfo(Item->Name_).suffix().toLower() : QString{};
    Request Specific; Specific.kind_ = Kind::Item; Specific.pixels_ = Pixels; Specific.directory_ = Directory;
    if (Item->storageEntry_ && Item->storageEntry_->locator_.nesting_.empty() && Item->Name_ != "..") {
        const auto& Entry = *Item->storageEntry_;
        Specific.remote_ = Entry.locator_.kind_ == hc::storage::LocatorKind::Smb;
        if (Entry.locator_.kind_ == hc::storage::LocatorKind::Local || Entry.locator_.kind_ == hc::storage::LocatorKind::Smb)
            Specific.path_ = QDir::toNativeSeparators(QString::fromStdString(Entry.locator_.root_));
        else if (Entry.locator_.kind_ == hc::storage::LocatorKind::Shell) {
            const auto File = Entry.hints_.find("filesystemPath"); Specific.virtual_ = File == Entry.hints_.end();
            Specific.path_ = QString::fromStdString(Specific.virtual_ ? Entry.locator_.root_ : File->second);
        }
        Specific.revision_ = QString::number(Entry.modifiedUtcMs_) + ':' + QString::number(Entry.size_) + ':' + QString::number(Entry.attributes_);
    }
    if (!Item->storageEntry_ && !LegacyBase.isEmpty() && Item->Name_ != "..") {
        Specific.path_ = QDir::toNativeSeparators(QDir(LegacyBase).filePath(Item->Name_));
        Specific.revision_ = QString::number(Item->Modified_.toMSecsSinceEpoch()) + ':' + QString::number(Item->Size_) + ':' + QString::number(Item->Attributes_);
    }
    const auto Extension = QFileInfo(Item->Name_).suffix().toLower();
    const bool Individual = !Specific.path_.isEmpty() && (Specific.virtual_ ? Settings["virtualFolderIcons"].toBool(true)
        : Directory ? Settings["specialFolderIcons"].toBool(true)
        : Mode == "all" && (Extension == "exe" || Extension == "lnk" || Extension == "ico"));
    DisplayIconQueries Queries{Individual ? Specific : Base, Specific, Settings["iconOverlays"].toBool(true) && !Specific.path_.isEmpty()};
    Queries.overlay_.kind_ = Kind::Overlay;
    return Queries;
}
QString LegacyIconBase(const CFSModel* Model)
{
    if (Model->ProviderEnabled() || !Model->GetRoot()) return {};
    const auto Location = Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot());
    return Location.nesting_.empty() && (Location.kind_ == hc::storage::LocatorKind::Local || Location.kind_ == hc::storage::LocatorKind::Smb)
        ? QString::fromStdString(Location.root_) : QString{};
}
}

void CFSModel::StartEverythingSearch(const QString& Query, const QString& Match, int Interval)
{
    StopEverythingSearch(); CancelNavigation(); navigationError_.reset();
    searchResults_ = true; everythingQuery_ = Query;
    pageSource_.reset(); pageView_.reset(); index_.Publish({});
    columnQueries_->Reset(); iconStop_.request_stop(); ++iconGeneration_;
    SetWatchEnabled(watchEnabled_);
    if (!searchLocal_) searchLocal_ = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
    if (!searchSmb_) searchSmb_ = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Smb);
    beginResetModel(); vecNode_.clear(); vecRowColors_.clear(); nameSortRanks_.clear(); columnSortRanks_.clear();
    fileCount_ = directoryCount_ = 0; totalSize_ = 0; endResetModel();
    everythingSession_ = std::make_unique<hc::search::EverythingSession>(this, searchLocal_, searchSmb_);
    everythingSession_->append_ = [this](auto Entries) { appendSearchResults(std::move(Entries)); };
    everythingSession_->status_ = [this](const QString& Text) { everythingStatus_ = Text; emit SearchResultsChanged(); };
    everythingSession_->Start(Query, Match, Interval, property("hc.search.helper").toString());
}

void CFSModel::StopEverythingSearch(bool Leave)
{
    if (everythingSession_) { everythingSession_->Cancel(); everythingSession_.reset(); }
    if (Leave) { searchResults_ = false; everythingQuery_.clear(); everythingStatus_.clear(); }
    else if (searchResults_) { everythingStatus_ = tr("Everything 검색 중단 · %1개 표시").arg(vecNode_.size()); emit SearchResultsChanged(); }
}

bool CFSModel::IsEverythingRunning() const
{
    return everythingSession_ && everythingSession_->Running();
}

void CFSModel::SetEverythingDisplayInterval(int Interval)
{
    if (everythingSession_) everythingSession_->SetDisplayInterval(Interval);
}

std::shared_ptr<hc::storage::IStorageProvider> CFSModel::ProviderFor(const hc::storage::EntrySnapshot& Entry) const
{
    if (!searchResults_) return acceptedProvider_;
    return Entry.locator_.kind_ == hc::storage::LocatorKind::Smb ? searchSmb_ : searchLocal_;
}

std::shared_ptr<hc::storage::IEntryIndex> CFSModel::DirectoryEntries() const
{
    if (!searchResults_) return pageSource_;
    class Snapshot final : public hc::storage::IEntryIndex {
    public:
        std::vector<hc::storage::EntrySnapshot> entries_;
        std::uint64_t Count() const override { return entries_.size(); }
        hc::storage::Result<hc::storage::EntryPage> Read(std::uint64_t Offset, std::size_t MaxItems, const hc::storage::OperationContext& Context) const override {
            if (const auto Failure = Context.Check({"search-results"})) return *Failure;
            hc::storage::EntryPage Page;
            for (auto Row = Offset; Row < entries_.size() && Page.entries_.size() < MaxItems; ++Row) Page.entries_.push_back(entries_[Row]);
            Page.end_ = Offset + Page.entries_.size() >= entries_.size(); return Page;
        }
    };
    auto Value = std::make_shared<Snapshot>(); Value->entries_.reserve(vecNode_.size());
    for (const auto& Item : vecNode_) if (Item->storageEntry_) Value->entries_.push_back(*Item->storageEntry_);
    return Value;
}

nsHC::TySpFileSource CFSModel::makeDisplayItem(hc::storage::EntrySnapshot Entry, const QPixmap& FolderIcon, const QPixmap& FileIcon) const
{
    nsHC::TySpFileSource Item(new nsHC::CFileSourceT, [](nsHC::CFileSourceT* Value) { if (Value->PIDL) CoTaskMemFree(Value->PIDL); delete Value; });
    Item->Name_ = QString::fromStdString(Entry.name_); Item->Ext_ = nsCmn::nsCmnPath::GetFileExtension(Item->Name_);
    Item->Attributes_ = Entry.attributes_; Item->Size_ = qint64(Entry.size_);
    if (Entry.createdUtcMs_) Item->Created_ = QDateTime::fromMSecsSinceEpoch(Entry.createdUtcMs_);
    if (Entry.modifiedUtcMs_) Item->Modified_ = QDateTime::fromMSecsSinceEpoch(Entry.modifiedUtcMs_);
    if (hc::settings::FileColorRules::HasDecomposedName(Item->Name_)) Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_UNICODE_NFD;
    if (Entry.hints_.contains("reparseTag")) Item->Reserved0_ = QString::fromStdString(Entry.hints_.at("reparseTag")).toUInt();
    if (Entry.hints_.contains("shellPidl")) {
        const auto Bytes = QByteArray::fromBase64(QByteArray::fromStdString(Entry.hints_.at("shellPidl")));
        Item->PIDL = static_cast<LPITEMIDLIST>(CoTaskMemAlloc(Bytes.size()));
        if (Item->PIDL) memcpy(Item->PIDL, Bytes.data(), Bytes.size());
        Item->Flags_ |= nsHC::CFileSourceT::FS_FLAG_PIDL;
    }
    Item->Icon = Entry.directory_ ? FolderIcon : FileIcon; Item->storageEntry_ = std::move(Entry);
    return Item;
}

void CFSModel::appendSearchResults(std::vector<hc::storage::EntrySnapshot> Entries)
{
    if (!searchResults_ || Entries.empty()) return;
    emit ColumnValuesChanging();
    const bool Explorer = displaySettings_["explorerDefaultIcons"].toBool(true);
    const auto FolderIcon = GenericIcon(true, iconPixels_, Explorer), FileIcon = GenericIcon(false, iconPixels_, Explorer);
    QVector<nsHC::TySpFileSource> Items; Items.reserve(Entries.size());
    for (const auto& Entry : Entries) Items.push_back(makeDisplayItem(Entry, FolderIcon, FileIcon));
    createBuiltFsValues(Items); index_.Append(Entries);
    const auto First = vecNode_.size();
    beginInsertRows({}, int(First), int(First + Items.size() - 1));
    for (auto& Item : Items) {
        auto Color = qMakePair(colorScheme_.FileList_FGColor, colorScheme_.FileList_BGColor);
        for (const auto& Set : vecFileSetColors_) if (TyStColorSchemeMgr::GetInstance()->JudgeFileSet(Set.first, Item.get(), nullptr, nullptr)) { Color = Set.second; break; }
        vecRowColors_.push_back(Color);
        if (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ++directoryCount_;
        else { ++fileCount_; totalSize_ += quint64(Item->Size_); }
        vecNode_.push_back(std::move(Item));
    }
    refreshNameSortRanks(int(First)); endInsertRows(); refreshColumnSortRanks(int(First)); refreshDisplayIcons(int(First));
    emit ColumnValuesChanged(); emit SearchResultsChanged();
}

void CFSModel::publishProvider(std::vector<hc::storage::EntrySnapshot> Entries, std::vector<hc::settings::FileRuleResult> Styles)
{
    const auto IconIdentity = [](const hc::storage::EntrySnapshot& Entry) {
        return hc::workspace::SelectionKey(Entry) + ':' + QString::number(Entry.modifiedUtcMs_) + ':' + QString::number(Entry.size_) + ':' + QString::number(Entry.attributes_);
    };
    QHash<QString, QPixmap> PreviousIcons;
    for (const int Row : iconVisibleRows_) if (Row >= 0 && Row < vecNode_.size() && vecNode_[Row]->storageEntry_ && !vecNode_[Row]->Icon.isNull())
        PreviousIcons.insert(IconIdentity(*vecNode_[Row]->storageEntry_), vecNode_[Row]->Icon);
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
    // The whole listing fits beside the previous folder, up to the budget the
    // background pass may style.
    fileColorCache_.setMaxCost(int(qBound<qsizetype>(8192, qsizetype(Entries.size()) + 8192, qsizetype(FileRuleCompleteRows) + 8192)));
    QVector<nsHC::TySpFileSource> Items;
    if (GetRoot()->ReadLocation(GetCurrentPathWithRoot()).kind_ != hc::storage::LocatorKind::Shell || GetRootCount() > 1)
    {
        auto Parent = std::make_shared<nsHC::CFileSourceT>();
        Parent->Name_ = "..";
        Parent->Attributes_ = FILE_ATTRIBUTE_DIRECTORY;
        Items.push_back(Parent);
    }
    const bool Explorer = displaySettings_["explorerDefaultIcons"].toBool(true);
    const auto FolderIcon = GenericIcon(true, iconPixels_, Explorer);
    const auto FileIcon = GenericIcon(false, iconPixels_, Explorer);
    const int ParentRows = int(Items.size());
    std::size_t Seeded = 0;
    for (auto& Entry : Entries)
    {
        const auto Seed = Seeded++;
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
        Item->Icon = PreviousIcons.value(IconIdentity(Entry), Entry.directory_ ? FolderIcon : FileIcon);
        Item->storageEntry_ = std::move(Entry);
        if (Item->storageEntry_->directory_) {
            const auto Key = QString::fromStdString(hc::storage::Serialize(Item->storageEntry_->id_));
            if (const auto Known = KnownSizes.constFind(Key); Known != KnownSizes.cend()) {
                Item->directorySize_ = *Known; Item->Size_ = qint64(qMin<quint64>(*Known, (std::numeric_limits<qint64>::max)()));
                directorySizeStale_.insert(Key);
            }
        }
        // The worker evaluated this row from the same snapshot; keying it from the
        // finished item guarantees the first paint finds it.
        if (Seed < Styles.size()) {
            auto& Style = Styles[Seed];
            if (Style.validUntilUtcMs_) scheduleFileRuleExpiry(*Style.validUntilUtcMs_);
            fileColorCache_.insert(hc::settings::FileRuleKey::From(fileRuleInput(Item)), new CachedFileRule{std::move(Style)});
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
    fileRuleCompleteLow_ = fileRuleCompleteNext_ = ParentRows + int(std::min(Styles.size(), std::size_t(Entries.size())));
    fileRuleCompleteBudget_ = FileRuleCompleteRows;
    completeFileRuleStyles();
}

void CFSModel::ApplyDisplaySettings()
{
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    SetEverythingDisplayInterval(hc::quickfind::Settings::FromJson(General["quickFind"].toObject()).everythingDisplayIntervalMs_);
    bool IconsChanged = false;
    for (const auto* Key : {"iconMode", "specialFolderIcons", "virtualFolderIcons", "explorerDefaultIcons", "iconOverlays"})
        IconsChanged = IconsChanged || displaySettings_[Key] != General[Key];
    displaySettings_ = General;
    if (UsesIndexedOrder() && !loading_) requestPage(0, true);
    refreshNameSortRanks();
    if (IconsChanged) { setProperty("hc.resetIconAppearance", true); refreshDisplayIcons(); }
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
    if (!searchResults_ && hc::folders::IsNetwork(Location)) return;
    if (Automatic && !hc::folders::Automatic(Mode, Location)) return;
    const bool UseIndex = !searchResults_ && Options["folderSizeUseEverything"].toBool(true)
        && (Location.kind_ == LocatorKind::Local || Location.kind_ == LocatorKind::Smb);
    const bool IndexOnly = Automatic && Mode == "everything";
    if (IndexOnly && !UseIndex) return;
    std::vector<EntrySnapshot> Targets;
    QHash<QString, int> Rows;
    const QSet<QModelIndex> Requested(Entries.begin(), Entries.end());
    QHash<QString, bool> NetworkRoots;
    const auto IsNetwork = [&](const Locator& At) {
        if (At.kind_ != LocatorKind::Local) return hc::folders::IsNetwork(At);
        auto Root = QDir::toNativeSeparators(QString::fromStdString(At.root_));
        if (Root.startsWith("\\\\?\\")) Root = Root.mid(4);
        if (Root.startsWith("\\\\") || Root.startsWith("UNC\\", Qt::CaseInsensitive)) return true;
        Root = Root.left(3).toUpper();
        if (const auto Found = NetworkRoots.constFind(Root); Found != NetworkRoots.cend()) return *Found;
        // Drive type is shared by its children; do not call Windows for every row.
        const bool Network = hc::folders::IsNetwork(At); NetworkRoots.insert(Root, Network); return Network;
    };
    bool HadSizes = false;
    for (int Row = 0; Row < rowCount(); ++Row) {
        if (!Entries.isEmpty() && !Requested.contains(index(Row, 0))) continue;
        const auto& Item = vecNode_[Row];
        if (!Item->storageEntry_ || Item->Name_ == ".." || !Item->storageEntry_->directory_
            || (Item->Attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (IsNetwork(Item->storageEntry_->locator_)) continue;
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
    if (Targets.empty()) return;
    // Poll ready results in bounded batches. A fast index can finish thousands
    // of folders at once; per-result queued signals otherwise monopolize input.
    const auto Generation = generation_;
    const auto Token = directorySizeStop_.get_token();
    const auto SizePresentation = [this] {
        QVector<int> Columns;
        QHash<QString, bool> Dependencies;
        const std::function<bool(const QString&)> UsesField = [&](const QString& Field) {
            if (Field == "file.size") return true;
            if (const auto Found = Dependencies.constFind(Field); Found != Dependencies.cend()) return *Found;
            Dependencies.insert(Field, false);
            bool Uses = false;
            if (const auto* Script = hc::columns::FindScript(columnView_.profile_, Field))
                Uses = std::any_of(Script->dependencies_.begin(), Script->dependencies_.end(), UsesField);
            Dependencies.insert(Field, Uses); return Uses;
        };
        const auto UsesSize = [&](const Column& Binding) {
            const auto Fields = Binding.definition_.template_.isEmpty() ? QStringList{Binding.definition_.fieldId_} : Binding.compiled_.dependencies_;
            return std::any_of(Fields.begin(), Fields.end(), UsesField);
        };
        for (int Col = 0; Col < columnView_.VecColumns.size(); ++Col) if (UsesSize(columnView_.VecColumns[Col])) Columns.append(Col);
        bool Sorts = false;
        for (const auto& Sort : columnSorts_) for (const auto& Binding : columnView_.VecColumns)
            if (Binding.definition_.id_ == Sort.columnId_ && (UsesSize(Binding) || UsesField(Binding.definition_.sortField_))) Sorts = true;
        return std::pair{Columns, Sorts};
    };
    if (rowCount()) for (const int Col : SizePresentation().first) emit dataChanged(index(0, Col), index(rowCount() - 1, Col), {Qt::DisplayRole});
    if (HadSizes) emit DirectorySizesChanged();
    struct SizeResult { ResourceId id_; Result<std::uint64_t> bytes_; };
    const auto Parent = QString::fromStdString(Location.root_);
    auto Future = QtConcurrent::run(DirectorySizePool(), [Targets = std::move(Targets), Provider, Parent, Token, UseIndex, IndexOnly, Automatic](QPromise<SizeResult>& Promise) {
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
    });
    auto* Apply = new QTimer(this); Apply->setInterval(16);
    connect(Apply, &QTimer::timeout, this, [this, Apply, Future, Next = 0, Generation, Token, Rows, SizePresentation, Automatic]() mutable {
        if (Generation != generation_ || Token.stop_requested()) { Apply->stop(); Apply->deleteLater(); return; }
        const int Ready = Future.resultCount();
        const int End = std::min(Ready, Next + 32);
        if (Next < End) {
            // Profiles and sort rules can change while the worker is running.
            const auto [SizeColumns, SortSizes] = SizePresentation();
            QElapsedTimer Budget; Budget.start();
            QVector<int> Changed;
            if (SortSizes) emit ColumnValuesChanging();
            for (; Next < End; ++Next) {
                const auto Result = Future.resultAt(Next);
                const auto Key = QString::fromStdString(Serialize(Result.id_));
                directorySizePending_.remove(Key); directorySizeStale_.remove(Key);
                const auto Row = Rows.value(Key, -1);
                if (Row < 0 || Row >= vecNode_.size()) continue;
                const auto& Item = vecNode_[Row];
                if (!Item->storageEntry_ || Item->storageEntry_->id_ != Result.id_) continue;
                Changed.append(Row);
                if (Result.bytes_.HasValue()) {
                    totalSize_ -= Item->directorySize_.value_or(0);
                    Item->directorySize_ = Result.bytes_.Value();
                    totalSize_ += qMin<quint64>(Result.bytes_.Value(), (std::numeric_limits<quint64>::max)() - totalSize_);
                    Item->Size_ = qint64(qMin<quint64>(Result.bytes_.Value(), (std::numeric_limits<qint64>::max)()));
                    for (const int Col : SizeColumns) createBuiltFsValues(Item, Col);
                }
                if (Budget.elapsed() >= 4) { ++Next; break; }
            }
            if (!Changed.isEmpty()) {
                for (int At = 0; At < Changed.size();) {
                    const int First = Changed[At]; int Last = First; ++At;
                    while (At < Changed.size() && Changed[At] == Last + 1) Last = Changed[At++];
                    for (const int Col : SizeColumns) emit dataChanged(index(First, Col), index(Last, Col), {Qt::DisplayRole, Qt::ToolTipRole, ColumnStateRole});
                }
                if (SortSizes) refreshColumnSortRanks();
                emit DirectorySizesChanged();
            }
            if (SortSizes) emit ColumnValuesChanged();
        }
        if (Future.isFinished() && Next == Future.resultCount()) {
            Apply->stop(); Apply->deleteLater();
            if (!Automatic && Generation == generation_ && !Token.stop_requested()) RequestDirectorySizes({}, true);
        }
    });
    Apply->start();
}

void CFSModel::refreshNameSortRanks(int First)
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
    const bool Merge = searchResults_ && First > 0 && nameSortRanks_.size() == First;
    QVector<int> Order(vecNode_.size());
    if (Merge) for (int Row = 0; Row < First; ++Row) Order[nameSortRanks_[Row]] = Row;
    else First = 0;
    std::iota(Order.begin() + First, Order.end(), First);
    const bool DriveList = hc::ui::IsDriveList(WorkspaceLocation());
    const auto Less = [&](int Left, int Right) {
        if (DriveList) {
            const bool LeftParent = vecNode_[Left]->Name_ == "..", RightParent = vecNode_[Right]->Name_ == "..";
            if (LeftParent != RightParent) return LeftParent;
            const auto DriveKey = [&](int Row) {
                const auto& Entry = vecNode_[Row]->storageEntry_;
                return Entry ? hc::ui::DriveLetterSortKey(*Entry) : 26;
            };
            const int LeftKey = DriveKey(Left), RightKey = DriveKey(Right);
            if (LeftKey != RightKey) return LeftKey < RightKey;
        }
        const auto Group = [&](int Row) { return vecNode_[Row]->Name_ == ".." ? 0 : (vecNode_[Row]->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2; };
        if (Group(Left) != Group(Right)) return Group(Left) < Group(Right);
        const auto Name = [&](int Row) { const auto& Item = vecNode_[Row];
            return Item->columnValues_.isEmpty() ? Item->Name_ : Item->columnValues_.front().Raw().toString(); };
        const auto Compared = Compare(Name(Left), Name(Right));
        if (Compared || !searchResults_) return Compared < 0;
        return hc::workspace::SelectionKey(*vecNode_[Left]->storageEntry_) < hc::workspace::SelectionKey(*vecNode_[Right]->storageEntry_);
    };
    std::stable_sort(Order.begin() + First, Order.end(), Less);
    if (Merge) std::inplace_merge(Order.begin(), Order.begin() + First, Order.end(), Less);
    nameSortRanks_.resize(Order.size());
    for (int Rank = 0; Rank < Order.size(); ++Rank) nameSortRanks_[Order[Rank]] = Rank;
}

void CFSModel::SetIconPixels(int Pixels)
{
    Pixels = std::clamp(Pixels, 16, 512);
    if (iconPixels_ == Pixels) return;
    iconPixels_ = Pixels;
    setProperty("hc.resetIconAppearance", true);
    if (!vecNode_.isEmpty()) refreshDisplayIcons();
}

void CFSModel::refreshDisplayIcons(int First)
{
    if (property("hc.explicitIconRefresh").toBool()) {
        setProperty("hc.explicitIconRefresh", false); setProperty("hc.revalidateIconsOnce", true);
    }
    auto& Icons = hc::icons::FileIconService::Instance();
    const auto Mode = displaySettings_["iconMode"].toString("standard");
    const bool ResetAppearance = property("hc.resetIconAppearance").toBool(); setProperty("hc.resetIconAppearance", false);
    if (!First) { ++iconGeneration_; iconVisibleRows_.clear(); }
    if (Mode == "none") {
        forceIconRefresh_ = false;
        Icons.BeginRequests(this);
        for (int Row = First; Row < vecNode_.size(); ++Row) vecNode_[Row]->Icon = {};
        return;
    }
    const bool Explorer = displaySettings_["explorerDefaultIcons"].toBool(true);
    const auto Folder = Icons.DefaultIcon(true, iconPixels_, Explorer, this);
    const auto File = Icons.DefaultIcon(false, iconPixels_, Explorer, this);
    for (int Row = First; Row < vecNode_.size(); ++Row) {
        auto& Item = vecNode_[Row];
        if (ResetAppearance || Item->Icon.isNull()) Item->Icon = Item->Name_ == ".." ? QPixmap{} : Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY ? Folder : File;
    }
    if (std::exchange(forceIconRefresh_, false) && forceIconLocation_ == GetCurrentPathWithRoot()) invalidateDisplayIcons(true);
    // A view supplies the sorted/filtered range on the next event loop. A model
    // used without a view retains a small initial range for diagnostics/tests.
    if (!iconViewportBound_) for (int Row = First; Row < std::min<int>(vecNode_.size(), First + 256); ++Row) iconVisibleRows_.append(Row);
    if (!iconApplyTimer_.isActive()) iconApplyTimer_.start();
}

void CFSModel::SetIconViewport(const QModelIndexList& Entries, bool Enabled)
{
    iconViewportBound_ = true; iconViewportEnabled_ = Enabled;
    QSet<int> Wanted; QVector<int> Rows;
    for (const auto& Index : Entries) if (Enabled && Index.model() == this && Index.row() >= 0 && Index.row() < vecNode_.size() && !Wanted.contains(Index.row())) {
        Wanted.insert(Index.row()); Rows.append(Index.row());
    }
    auto& Icons = hc::icons::FileIconService::Instance();
    const bool Show = displaySettings_["iconMode"].toString("standard") != "none";
    for (const int Row : iconVisibleRows_) if (Row >= 0 && Row < vecNode_.size() && !Wanted.contains(Row)) {
        auto& Item = vecNode_[Row];
        Item->Icon = Show && Item->Name_ != ".." ? Icons.DefaultIcon(Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY, iconPixels_, displaySettings_["explorerDefaultIcons"].toBool(true)) : QPixmap{};
    }
    iconVisibleRows_ = std::move(Rows);
    if (!Enabled) { Icons.BeginRequests(this); return; }
    if (!iconApplyTimer_.isActive()) iconApplyTimer_.start();
}

void CFSModel::applyRequestedIcons()
{
    using namespace hc::icons;
    auto& Icons = FileIconService::Instance(); Icons.BeginRequests(this);
    const auto RefreshSettings = hc::refresh::Settings::FromJson(displaySettings_["refresh"].toObject());
    Icons.SetAutomaticRefresh(this, !RefreshSettings.Excludes(GetCurrentPathWithRoot())
        && (!RefreshSettings.pauseInBackground || qGuiApp->applicationState() == Qt::ApplicationActive));
    if (!iconViewportEnabled_ || displaySettings_["iconMode"].toString("standard") == "none") return;
    const bool Explorer = displaySettings_["explorerDefaultIcons"].toBool(true);
    const auto Folder = Icons.DefaultIcon(true, iconPixels_, Explorer, this), File = Icons.DefaultIcon(false, iconPixels_, Explorer, this);
    const auto LegacyBase = LegacyIconBase(this);
    const bool Explicit = property("hc.revalidateIconsOnce").toBool();
    QVector<int> Changed;
    for (const int Row : iconVisibleRows_) {
        if (Row < 0 || Row >= vecNode_.size()) continue;
        auto& Item = vecNode_[Row]; if (Item->Name_ == "..") continue;
        const auto Queries = IconQueries(Item, displaySettings_, iconPixels_, LegacyBase);
        auto Pixmap = Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY ? Folder : File;
        if (Queries.base_.kind_ == Kind::Associated || Queries.base_.kind_ == Kind::Item) {
            const auto Value = Icons.Read(Queries.base_, this, Explicit);
            if (!Value.pixmap_.isNull()) Pixmap = Value.pixmap_;
        }
        if (Queries.overlayEnabled_) Pixmap = Icons.Compose(Pixmap, Icons.Read(Queries.overlay_, this, Explicit));
        if (Item->Icon.cacheKey() != Pixmap.cacheKey()) { Item->Icon = Pixmap; Changed.append(Row); }
    }
    std::sort(Changed.begin(), Changed.end());
    if (!iconVisibleRows_.isEmpty()) setProperty("hc.revalidateIconsOnce", false);
    for (int At = 0; At < Changed.size();) {
        const int First = Changed[At]; int Last = First; ++At;
        while (At < Changed.size() && Changed[At] == Last + 1) Last = Changed[At++];
        emit dataChanged(index(First, 0), index(Last, 0), {Qt::DecorationRole});
    }
}

void CFSModel::invalidateDisplayIcons(bool All, int SettleMs)
{
    if (displaySettings_["iconMode"].toString("standard") == "none") return;
    QSet<QString> Keys, Paths;
    const auto LegacyBase = LegacyIconBase(this);
    const auto Add = [&](int Row) {
        if (Row < 0 || Row >= vecNode_.size() || vecNode_[Row]->Name_ == "..") return;
        const auto Queries = IconQueries(vecNode_[Row], displaySettings_, iconPixels_, LegacyBase);
        if (All || Queries.base_.kind_ == hc::icons::Kind::Item) Keys.insert(Queries.base_.Key());
        if (Queries.overlayEnabled_) Keys.insert(Queries.overlay_.Key());
        if (!Queries.overlay_.path_.isEmpty()) Paths.insert(Queries.overlay_.path_);
    };
    if (All) for (int Row = 0; Row < vecNode_.size(); ++Row) Add(Row);
    else for (int Row : iconVisibleRows_) Add(Row);
    auto& Icons = hc::icons::FileIconService::Instance(); Icons.Invalidate(Keys, SettleMs); Icons.InvalidatePaths(Paths, SettleMs);
}

void CFSModel::ForceRefresh()
{
    forceIconRefresh_ = true; forceIconLocation_ = GetCurrentPathWithRoot(); Refresh();
}

hc::settings::FileRuleInput CFSModel::fileRuleInput(const nsHC::TySpFileSource& Item) const
{
    // Every cache key is derived here so the seeded, queued and re-validated
    // forms of one row never diverge. Size conditions skip folders, so a folder
    // keeps its key when its measured size arrives instead of turning plain.
    const bool Folder = (Item->Attributes_ & FILE_ATTRIBUTE_DIRECTORY) != 0;
    hc::settings::FileRuleInput Input{Item->Name_, Item->Attributes_, Folder ? 0 : quint64(qMax<qint64>(0, Item->Size_)), colorScheme_.IsDarkMode};
    if (Item->Modified_.isValid()) Input.modifiedUtcMs_ = hc::settings::FileColorRules::MetadataTime(Item->Modified_.toMSecsSinceEpoch());
    if (Item->Reserved0_) Input.reparseTag_ = Item->Reserved0_;
    return Input;
}

hc::settings::FileRuleResult CFSModel::fileRuleStyle(int Row) const
{
    const auto& Item = vecNode_[Row];
    if (Item->Name_ == ".." || !fileColorRules_.HasRules()) return {};
    auto Input = fileRuleInput(Item);
    const auto Key = hc::settings::FileRuleKey::From(Input);
    if (auto* Cached = fileColorCache_.object(Key)) {
        if (!Cached->style_.validUntilUtcMs_ || Input.referenceUtcMs_ < *Cached->style_.validUntilUtcMs_) {
            if (Cached->style_.validUntilUtcMs_) scheduleFileRuleExpiry(*Cached->style_.validUntilUtcMs_);
            return Cached->style_;
        }
        fileColorCache_.remove(Key);
    }
    requestFileRuleCompletion(Row);
    if (fileRulePending_.size() < 256 && !fileRulePending_.contains(Key)) {
        fileRulePending_.insert(Key); fileRuleQueue_.append({Key, std::move(Input), Row, Item});
        if (!fileRulePump_.isActive()) fileRulePump_.start(0);
    } else if (!fileRulePending_.contains(Key)) {
        fileRuleDeferredRows_.insert(Row);
    }
    return {};
}

void CFSModel::resetFileRules()
{
    // Listing changes drop queued work only. Cached styles are keyed by file
    // metadata, so a refresh or a revisited folder paints from them at once;
    // rule and palette changes clear the cache in SetColorScheme.
    fileRuleStop_.request_stop(); fileRuleStop_ = std::stop_source{}; ++fileRuleEpoch_;
    fileRuleQueue_.clear(); fileRulePending_.clear(); fileRuleDeferredRows_.clear(); fileRulePump_.stop();
    fileRuleFirstBatch_ = true;
    // An in-flight completion chunk reports to the old epoch and is dropped.
    fileRuleCompleteTimer_.stop(); fileRuleCompleteLow_ = fileRuleCompleteNext_ = 0; fileRuleCompleteBudget_ = 0;
}

void CFSModel::requestFileRuleCompletion(int Row) const
{
    // The view outran the background pass or returned to rows the cache let
    // go: restart the pass at the row being painted so the following pages
    // are styled before they are reached.
    const bool Covered = Row >= fileRuleCompleteLow_ && Row < fileRuleCompleteNext_;
    const bool Upcoming = Row >= fileRuleCompleteNext_ && Row < fileRuleCompleteNext_ + int(FileRuleSeedRows) && (fileRuleCompleting_ || fileRuleCompleteTimer_.isActive());
    if (Covered || Upcoming) return;
    fileRuleCompleteLow_ = fileRuleCompleteNext_ = Row; fileRuleCompleteBudget_ = qMax(fileRuleCompleteBudget_, int(FileRuleSeedRows));
    if (!fileRuleCompleting_ && !fileRuleCompleteTimer_.isActive()) fileRuleCompleteTimer_.start(0);
}

void CFSModel::completeFileRuleStyles()
{
    if (fileRuleCompleting_ || !fileColorRules_.HasRules()) return;
    const int Count = rowCount();
    if (fileRuleCompleteBudget_ <= 0 || fileRuleCompleteNext_ < 0 || fileRuleCompleteNext_ >= Count) return;
    // Inputs are gathered here because row metadata belongs to the GUI thread;
    // the worker sees immutable copies only.
    const int First = fileRuleCompleteNext_, Last = std::min<int>(Count, First + int(FileRuleSeedRows));
    QList<hc::settings::FileRuleInput> Inputs; Inputs.reserve(Last - First);
    for (int Row = First; Row < Last; ++Row) Inputs.append(fileRuleInput(vecNode_[Row]));
    fileRuleCompleteNext_ = Last; fileRuleCompleteBudget_ -= Last - First; fileRuleCompleting_ = true;
    const auto Epoch = fileRuleEpoch_; const auto Token = fileRuleStop_.get_token(); const auto Rules = fileColorRules_;
    using Chunk = QList<hc::settings::FileRuleResult>;
    auto* Watcher = new QFutureWatcher<Chunk>(this);
    connect(Watcher, &QFutureWatcher<Chunk>::finished, this, [this, Watcher, Epoch, Inputs] {
        auto Results = Watcher->future().takeResult(); Watcher->deleteLater(); fileRuleCompleting_ = false;
        if (Epoch == fileRuleEpoch_) {
            // Rows already on screen were queued by the pump and repaint from
            // there; rows not yet painted need no signal.
            for (qsizetype I = 0; I < Results.size(); ++I) {
                if (Results[I].validUntilUtcMs_) scheduleFileRuleExpiry(*Results[I].validUntilUtcMs_);
                fileColorCache_.insert(hc::settings::FileRuleKey::From(Inputs[I]), new CachedFileRule{std::move(Results[I])});
            }
        }
        completeFileRuleStyles();
    });
    Watcher->setFuture(QtConcurrent::run([Rules, Inputs, Token, Context = fileRuleCompleteContext_] {
        Chunk Results; Results.reserve(Inputs.size());
        const auto Reference = QDateTime::currentMSecsSinceEpoch();
        for (auto Input : Inputs) {
            if (Token.stop_requested()) break;
            Input.referenceUtcMs_ = Reference; Results.append(Rules.EvaluateStyle(Input, Context.get()));
        }
        return Results;
    }));
}

void CFSModel::pumpFileRules()
{
    if (fileRuleRunning_ || fileRuleQueue_.isEmpty()) return;
    QList<FileRuleJob> Jobs;
    // The first batch after a listing covers every row the view asked for, so
    // unseeded rows change color in one repaint instead of a top-down wave.
    const int Limit = std::exchange(fileRuleFirstBatch_, false) ? 256 : 64;
    while (!fileRuleQueue_.isEmpty() && Jobs.size() < Limit) Jobs.append(fileRuleQueue_.takeFirst());
    fileRuleRunning_ = true;
    const auto Epoch = fileRuleEpoch_; const auto Token = fileRuleStop_.get_token();
    using Batch = QList<QPair<FileRuleJob, hc::settings::FileRuleResult>>;
    auto* Watcher = new QFutureWatcher<Batch>(this);
    connect(Watcher, &QFutureWatcher<Batch>::finished, this, [this, Watcher, Epoch] {
        auto Results = Watcher->future().takeResult(); Watcher->deleteLater(); fileRuleRunning_ = false;
        if (Epoch == fileRuleEpoch_) {
            QList<int> Rows;
            for (auto& Result : Results) {
                auto& Job = Result.first; fileRulePending_.remove(Job.key_);
                const int Row = Job.row_ >= 0 && Job.row_ < vecNode_.size() && vecNode_[Job.row_] == Job.item_ ? Job.row_ : vecNode_.indexOf(Job.item_);
                if (Row < 0) continue;
                if (hc::settings::FileRuleKey::From(fileRuleInput(vecNode_[Row])) != Job.key_) { fileRuleDeferredRows_.insert(Row); continue; }
                if (Result.second.validUntilUtcMs_) scheduleFileRuleExpiry(*Result.second.validUntilUtcMs_);
                fileColorCache_.insert(Job.key_, new CachedFileRule{std::move(Result.second)}); Rows.append(Row);
            }
            std::sort(Rows.begin(), Rows.end());
            for (int At = 0; At < Rows.size();) {
                const int First = Rows[At]; int Last = First; ++At;
                while (At < Rows.size() && Rows[At] <= Last + 1) Last = std::max(Last, Rows[At++]);
                if (First >= 0 && Last < rowCount() && columnCount()) emit dataChanged(index(First, 0), index(Last, columnCount() - 1),
                    {Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole, FileColorOverrideRole, FileEffectsRole});
            }
        }
        while (!fileRuleDeferredRows_.isEmpty() && fileRulePending_.size() < 256) {
            const int Row = *fileRuleDeferredRows_.begin(); fileRuleDeferredRows_.remove(Row);
            if (Row >= 0 && Row < rowCount()) fileRuleStyle(Row);
        }
        if (!fileRuleQueue_.isEmpty()) fileRulePump_.start(16);
    });
    const auto Rules = fileColorRules_;
    Watcher->setFuture(QtConcurrent::run(FileRulePool(), [Rules, Jobs = std::move(Jobs), Token, Context = fileRuleContext_] {
        Batch Results;
        for (const auto& Job : Jobs) {
            if (Token.stop_requested()) break;
            auto Input = Job.input_; Input.referenceUtcMs_ = QDateTime::currentMSecsSinceEpoch();
            Results.append({Job, Rules.EvaluateStyle(Input, Context.get())});
        }
        return Results;
    }));
}

void CFSModel::SetColorScheme(const TyColorScheme& Scheme)
{
    resetFileRules();
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
        {Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole, FileEffectsRole});
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
        if (Item->storageEntry_ && ProviderFor(*Item->storageEntry_)) Sample.source_ = std::make_shared<hc::storage::ProviderContentSource>(ProviderFor(*Item->storageEntry_), *Item->storageEntry_);
        Samples.append(std::move(Sample)); if (Samples.size() == 50) break;
    }
    return Samples;
}

QString CFSModel::queryPath(const nsHC::TySpFileSource& Item) const
{
    if (!Item->storageEntry_ || Item->Name_ == "..") return {};
    const auto& Location = Item->storageEntry_->locator_;
    if (Location.kind_ != hc::storage::LocatorKind::Local && Location.kind_ != hc::storage::LocatorKind::Smb) return {};
    return QDir::toNativeSeparators(QString::fromStdString(Location.root_));
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
    const auto Native = queryPath(Item);
    return CColumnMgr::GetFieldValue(Field, Item.get(), Native.isEmpty() ? GetCurrentPathWithRoot() : QFileInfo(Native).absolutePath());
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

void CFSModel::SetGrouping(const hc::columns::GroupingState& State)
{
    if (grouping_ == State) return;
    emit ColumnValuesChanging();
    grouping_ = State; groupingDate_ = QDate::currentDate(); groupingZone_ = QTimeZone::systemTimeZone();
    refreshColumnSortRanks();
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {GroupKeyRole, GroupOrderRole, GroupTextRole, GridSortRole});
    emit ColumnValuesChanged();
    // Grouped sorting uses snapshot ranks. Rebuild a paged listing when its
    // accepted file sort changed while grouping was active.
    if (!State.enabled_ && UsesIndexedOrder() && !loading_ && columnSorts_ != pageSorts_) requestPage(0, true);
}

void CFSModel::RefreshGroupingDate()
{
    if (!grouping_.enabled_) return;
    const auto Today = QDate::currentDate(); const auto Zone = QTimeZone::systemTimeZone();
    if (groupingDate_ == Today && groupingZone_ == Zone) return;
    emit ColumnValuesChanging(); groupingDate_ = Today; groupingZone_ = Zone;
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {GroupKeyRole, GroupOrderRole, GroupTextRole});
    emit ColumnValuesChanged();
}

void CFSModel::SetColumnSorts(const QVector<hc::columns::SortDefinition>& Sorts)
{
    if (Sorts == columnSorts_ && columnSortRanks_.size() == vecNode_.size()) return;
    if (Sorts != columnSorts_) appendOrder_ = false;
    if (UsesIndexedOrder() && !grouping_.enabled_) { if (Sorts != columnSorts_) { columnSorts_ = Sorts; if (!loading_) requestPage(0, true); } return; }
    columnQueries_->Resume(); columnSorts_ = Sorts; refreshColumnSortRanks();
}

void CFSModel::refreshColumnSortRanks(int First)
{
    using namespace hc::columns;
    if (loading_) return;
    if (hc::ui::IsDriveList(WorkspaceLocation())) {
        // Drive letters stay A-Z even when a saved column sort is descending.
        // GridSortRole compensates for the grid's direction using these ranks.
        columnQueries_->EndSnapshot(); refreshNameSortRanks();
        columnSortRanks_ = nameSortRanks_; completedSorts_ = columnSorts_;
        if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {GridSortRole});
        return;
    }
    if ((UsesIndexedOrder() || appendOrder_) && !grouping_.enabled_) {
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
    const bool Merge = searchResults_ && First > 0 && searchSortRows_.size() == First && searchSortOrder_.size() == First && searchSorts_ == columnSorts_;
    if (Merge) Rows = searchSortRows_;
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
    for (int ItemRow = Merge ? First : 0; ItemRow < vecNode_.size(); ++ItemRow) {
        const auto& Item = vecNode_[ItemRow];
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
    if (searchResults_) {
        const auto Compare = SortLess(Orders, displaySettings_["sortLocaleAware"].toBool(true), displaySettings_["sortNumericAware"].toBool(true),
            displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive);
        auto Order = Merge ? searchSortOrder_ : QVector<int>{};
        const auto Start = Order.size();
        for (int Row = Start; Row < Rows.size(); ++Row) Order.append(Row);
        const auto Less = [&](int A, int B) { return Compare(Rows[A], Rows[B]); };
        std::stable_sort(Order.begin() + Start, Order.end(), Less);
        if (Merge) std::inplace_merge(Order.begin(), Order.begin() + Start, Order.end(), Less);
        columnSortRanks_.resize(Order.size());
        for (int Rank = 0; Rank < Order.size(); ++Rank) columnSortRanks_[Order[Rank]] = Rank;
        searchSortRows_ = std::move(Rows); searchSortOrder_ = std::move(Order); searchSorts_ = columnSorts_;
    } else columnSortRanks_ = SortRanks(Rows, Orders, displaySettings_["sortLocaleAware"].toBool(true), displaySettings_["sortNumericAware"].toBool(true),
        displaySettings_["sortCaseSensitive"].toBool() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    columnQueries_->EndSnapshot();
    completedSorts_ = columnSorts_; emit ColumnQueryStatus(columnQueries_->Pending() ? tr("컬럼 조회 중… %1건").arg(columnQueries_->Pending()) : requestOnlyValues_ ? tr("요청형 값이 있습니다. 다시 조회를 누르면 계산합니다.") : QString{});
    if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {GridSortRole});
}
