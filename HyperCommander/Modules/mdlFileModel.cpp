#include "stdafx.h"
#include "mdlFileModel.hpp"

#include "fileSetMgr.hpp"
#include "solTCPluginMgr.hpp"
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

DECLARE_CMNLIBSV2_NAMESPACE

////////////////////////////////////////////////////////////////////////////////
///

CFsModelT::CFsModelT( QObject* Parent )
    : QAbstractItemModel( Parent )
{
    fileRuleClock_ = new QTimer(this); fileRuleClock_->setSingleShot(true);
    connect(fileRuleClock_, &QTimer::timeout, this, [this] {
        bool Expired = false; std::optional<qint64> Next;
        {
            CCmnMutexLocker Locker(&lock); fileRuleExpiry_.reset(); const auto Now = QDateTime::currentMSecsSinceEpoch();
            for (const auto& Key : fileColorCache_.keys()) {
                auto* Cached = fileColorCache_.object(Key); const auto Deadline = Cached->style_.validUntilUtcMs_; if (!Deadline) continue;
                if (*Deadline <= Now) { Expired = true; fileColorCache_.remove(Key); }
                else if (!Next || *Deadline < *Next) Next = Deadline;
            }
            if (Next) scheduleFileRuleExpiry(*Next);
        }
        // Cached styles outlive listings, so an entry no longer knows its row.
        // Style roles repaint without relayout; one range covers every row.
        if (Expired && rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole});
    });
}

void CFsModelT::scheduleFileRuleExpiry(qint64 Deadline) const
{
    if (QThread::currentThread() != thread()) return;
    if (fileRuleExpiry_ && Deadline >= *fileRuleExpiry_ && fileRuleClock_->isActive()) return;
    fileRuleExpiry_ = Deadline; const auto Now = QDateTime::currentMSecsSinceEpoch();
    const auto Delay = Deadline > Now ? quint64(Deadline) - quint64(Now) : 0;
    fileRuleClock_->start(int(qBound<quint64>(1000, Delay, std::numeric_limits<int>::max())));
}

void CFsModelT::Refresh()
{
    doRefresh();

    if (!HandlesRefreshSignals()) emit sigRefreshCompleted();
}

void CFsModelT::SetRoot( nsHC::TySpFileSystem Fs )
{
    vecFs_.clear();
    vecFs_.push_back( Fs );
}

void CFsModelT::AppendRoot( nsHC::TySpFileSystem Fs )
{
    vecFs_.push_back( Fs );
}

nsHC::TySpFileSystem CFsModelT::GetRoot() const
{
    if( vecFs_.isEmpty() == true )
        return {};

    return vecFs_[ vecFs_.size() - 1 ];
}

void CFsModelT::SetCurrentPath( const QString& Path )
{
    if( Path.isEmpty() == true )
        path_ = "\\";
    else
        path_ = Path;
}

QString CFsModelT::GetCurrentPath() const
{
    return path_;
}

void CFsModelT::ChangeDirectory( const QString& Child )
{
    QString Notify; bool IsEscape = false;
    const auto Root = GetRoot();
    QString PreviousDirectory;
    if (Child == "..") {
        auto PreviousPath = QDir::fromNativeSeparators(GetCurrentPath());
        if (IsRoot() && Root && (Root->GetFeatures() & nsHC::FS_FEA_PACK)) PreviousPath = QDir::fromNativeSeparators(Root->GetRoot());
        while (PreviousPath.endsWith('/') && PreviousPath.size() > 1) PreviousPath.chop(1);
        PreviousDirectory = PreviousPath.section('/', -1);
    }
    setProperty("hc.parentFocusName", PreviousDirectory);
    setProperty("hc.parentNavigation", Child == "..");

    if( Child.startsWith( '/' ) == true || Child.startsWith( '\\' ) == true )
    {
        SetCurrentPath( Child );
    }
    else
    {
        if( IsRoot() == true )
        {
            if( Child == ".." )
            {
                if (Root->GetCate() == nsHC::FS_CATE_VIRTUAL && vecFs_.size() > 1)
                {
                    vecFs_.removeLast();
                    SetCurrentPath("\\");
                    Refresh();
                    if (!HandlesRefreshSignals()) emit sigChangedDirectory(GetCurrentPathWithRoot());
                    return;
                }
                // LOCAL FS 의 Root 에서 .. 일 때와 PACK FS 의 Root 에서 .. 일 때 구분,
                if( !FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) )
                {
                    SetCurrentPath( "" );
                    AppendRoot( nsHC::TySpFileSystem( new nsHC::CFSShell( FOLDERID_ComputerFolder ) ) );
                }
                else
                {
                    // 압축파일 보기 모드에서 빠져나가야 한다.
                    IsEscape = true;
                    SetCurrentPath( ( ( nsHC::CFSPack* )Root.get() )->GetParent() );
                    vecFs_.removeLast();
                }
            }
            else
                SetCurrentPath( QDir::toNativeSeparators( QDir::cleanPath( GetCurrentPath() + Child ) ) );
        }
        else
            SetCurrentPath( QDir::toNativeSeparators( QDir::cleanPath( GetCurrentPath() + "\\" + Child ) ) );
    }

    if( !FlagOn( Root->GetFeatures(), nsHC::FS_FEA_PACK ) || IsEscape == true )
        Notify = QDir::toNativeSeparators( QDir::cleanPath( GetCurrentPathWithRoot() ) );
    else
    {
        int a = 0;
        if( IsRoot() == true )
        {
            Notify = QDir::toNativeSeparators( QDir::cleanPath( Root->GetRoot() ) );
        }
        else
        {
            Notify = GetCurrentPath();
        }
    }

    qDebug() << __FUNCTION__ << " : " << Notify;
    Refresh();
    if (!HandlesRefreshSignals()) emit sigChangedDirectory( Notify );
}

void CFsModelT::ChangeDirectory( const QModelIndex& Child )
{
    const auto& Node = GetFileInfo( Child );
    if( Node == nullptr || Node->Name_.isEmpty() == true )
    {
        ChangeDirectory( "" );
        return;
    }

    if (Node->storageEntry_ && Node->storageEntry_->hints_.contains("filesystemPath") && (Node->Attributes_ & FILE_ATTRIBUTE_DIRECTORY))
    {
        const auto Path = QString::fromStdString(Node->storageEntry_->hints_.at("filesystemPath"));
        const auto Fs = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(Path));
        if (Fs)
        {
            SetRoot(Fs);
            SetCurrentPath(Path.mid(Fs->GetRoot().size()));
            ChangeDirectory("");
        }
        return;
    }
    if (Node->storageEntry_ && Node->PIDL && (Node->Attributes_ & FILE_ATTRIBUTE_DIRECTORY))
    {
        AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSShell(Node->PIDL)));
        SetCurrentPath("\\");
        ChangeDirectory("");
        return;
    }

    if( FlagOn( Node->Flags_, nsHC::CFileSourceT::FS_FLAG_DRIVE ) )
    {
        TyOsError Err;
        const auto Sp = nsHC::TySpFileSystem( nsHC::CFileSystemT::MakeFileSystem( Node->Name_, &Err ) );
        if( Err.ErrorCode == ERROR_SUCCESS )
        {
            SetRoot( Sp );
            ChangeDirectory( "" );
        }
    }
    else
    {
        if( !FlagOn( Node->Attributes_, FILE_ATTRIBUTE_DIRECTORY ) )
        {
            // 압축파일
            const auto StPlugInMgr = TyStPlugInMgr::GetInstance();
            const auto VecWCXs = StPlugInMgr->RetrieveWCXByExts( Node->Ext_, Node->Name_ );

            bool Mounted = false;
            for( const auto& WCX : VecWCXs )
            {
                // 플러그인 로딩 후, OpenArchive 가 성공하면 해당 플러그인을 사용하여 파일 목록을 구성한다.
                const auto SpWCX = TySpWCX( new CTCPlugInWCX );
                if( SpWCX->LoadWCX( WCX ).ErrorCode != ERROR_SUCCESS )
                    continue;

                const auto ParentPack = dynamic_cast<nsHC::CFSPack*>(GetRoot().get());
                if (ParentPack) {
                    // Nested bytes are materialized by the read provider on its
                    // worker. Never pass an archive-internal path to Win32 I/O.
                    if (!Node->storageEntry_ || Node->storageEntry_->locator_.nesting_.size() >= 8) continue;
                } else {
                    const auto Ret = SpWCX->OpenArchive(GetFileFullPath(Child), PK_OM_LIST);
                    if (!Ret.Value.has_value() || !Ret.Value.value()) continue;
                }

                const auto Packer = new nsHC::CFSPack( GetFileFullPath( Child ), SpWCX );
                Packer->SetParent( GetCurrentPath() );
                if (ParentPack) {
                    auto Location = Node->storageEntry_->locator_; Location.nesting_.push_back({"wcx", "/"});
                    Packer->SetArchiveChain(std::move(Location), ParentPack->ArchiveBackends());
                }
                
                // CFSPack 에 TySpWCX 와 압축파일 이름을 넣고, FS 스택 위에 넣는다.
                const auto Fs = nsHC::TySpFileSystem( Packer );
                vecFs_.push_back( Fs ); Mounted = true;
                break;
            }

            if (Mounted) ChangeDirectory( "/" );
            return;
        }

        // 디렉토리, 일반적인 경우

        if( FlagOn( Node->Attributes_, FILE_ATTRIBUTE_REPARSE_POINT ) )
        {
            if( FlagOn( Node->Reserved0_, IO_REPARSE_TAG_SYMLINK ) ||            // SYMLINKD
                FlagOn( Node->Reserved0_, IO_REPARSE_TAG_MOUNT_POINT ) )         // JUNTION
            {
                const auto s = nsCmn::nsCmnPath::GetReparsePointTo( GetFileFullPath( Child ) );

                //if( s.length() > 2 && s.contains( ':' ) == true )
                //{
                //    SetRoot( s.left( 2 ) );
                //    SetCurrentPath( s.mid( 2 ) );

                //    Refresh();

                //    emit sigChangedDirectory( QDir::toNativeSeparators( Root + GetCurrentPath() ) );
                //    return;
                //}
                //else
                //{
                //    // TODO: 오류 처리
                //}
            }
        }

        ChangeDirectory( Node->Name_ );
    }
}


void CFsModelT::SetColumnView( const ColumnView& CV )
{
    columnView_ = CV;
}

void CFsModelT::SetColorScheme( const TyColorScheme& ColorScheme )
{
    colorScheme_ = ColorScheme;
    fileColorCache_.clear();
    const auto Document = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["fileColorRules"].toObject();
    if (fileRuleDocument_ != Document) {
        fileRuleDocument_ = Document; fileRulesLoading_ = false; fileRulesError_.clear(); const auto Generation = ++fileRuleLoadGeneration_;
        if (hc::settings::FileColorRules::HasRegex(Document)) {
            fileRulesLoading_ = true;
            auto Previous = fileColorRules_;
            auto Failure=std::make_shared<QString>();
            fileColorRules_.Load(hc::settings::FileColorRules::Empty());
            auto* Watcher = new QFutureWatcher<hc::settings::FileColorRules>(this);
            connect(Watcher, &QFutureWatcher<hc::settings::FileColorRules>::finished, this, [this, Watcher, Generation, Failure] {
                auto Rules = Watcher->future().takeResult(); Watcher->deleteLater();
                if (Generation != fileRuleLoadGeneration_) return;
                fileRulesError_=*Failure; fileRulesLoading_ = false; fileColorRules_ = std::move(Rules); fileColorCache_.clear();
                if (rowCount() && columnCount()) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1), {Qt::ForegroundRole, Qt::BackgroundRole, Qt::FontRole});
            });
            Watcher->setFuture(QtConcurrent::run([Document, Previous, Failure]() mutable { *Failure=Previous.Load(Document); return Previous; }));
        } else fileRulesError_=fileColorRules_.Load(Document);
    }
    fileRuleClock_->stop(); fileRuleExpiry_.reset();

    decltype( vecFileSetColors_ ) VecFileSetColors;
    if( colorScheme_.Name.isEmpty() == true )
    {
        qSwap( VecFileSetColors, vecFileSetColors_ );
        return;
    }

    const auto StFileSetMgr = TyStFileSetMgr::GetInstance();
    for( const auto& Name : colorScheme_.MapNameToColors.keys() )
    {
        const auto& Set = StFileSetMgr->GetFileSet( Name );
        if( Set.Name.isEmpty() == true )
            continue;

        VecFileSetColors.push_back( qMakePair( Set, colorScheme_.MapNameToColors[ Name ] ) );
    }

    qSwap( VecFileSetColors, vecFileSetColors_ );
}

//////////////////////////////////////////////////////////////////////////
///

bool CFsModelT::IsRoot() const
{
    return path_.length() == 1 && ( path_ == '/' || path_ == '\\' );
}

int CFsModelT::GetFileCount() const
{
    return fileCount_;
}

int CFsModelT::GetDirectoryCount() const
{
    return directoryCount_;
}

uint64_t CFsModelT::GetTotalSize() const
{
    return totalSize_;
}

nsHC::TySpFileSource CFsModelT::GetFileInfo( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid | CheckIndexOption::DoNotUseParent ) == false )
        return {};

    return vecNode_.at( Index.row() );
}

QString CFsModelT::GetFileFullPath( const QModelIndex& Index ) const
{
    if( checkIndex( Index, CheckIndexOption::IndexIsValid ) == false )
        return "";

    const auto& Item = vecNode_.at( Index.row() );
    if (Item->storageEntry_ && Item->storageEntry_->hints_.contains("filesystemPath"))
        return QString::fromStdString(Item->storageEntry_->hints_.at("filesystemPath"));
    return QString( "%1%2%3" ).arg( GetCurrentPathWithRoot() ).arg( IsRoot() ? "" : "\\" ).arg( Item->Name_ );
}

QModelIndex CFsModelT::index( int row, int column, const QModelIndex& parent ) const
{
    if( row < 0 || column < 0 || row >= rowCount( parent ) || column >= columnCount( parent ) )
        return QModelIndex();

    return createIndex( row, column );
}

QModelIndex CFsModelT::parent( const QModelIndex& child ) const
{
    if( child.isValid() == true )
        return QModelIndex();

    return QModelIndex();
}

int CFsModelT::rowCount( const QModelIndex& parent ) const
{
    if( parent.isValid() == true )
        return 0;

    return vecNode_.size();
}

int CFsModelT::columnCount( const QModelIndex& parent ) const
{
    if( parent.isValid() == true )
        return 0;

    return columnView_.VecColumns.size();
}

QVariant CFsModelT::data( const QModelIndex& index, int role ) const
{
    QModelRoleData roleData( role );
    multiData( index, roleData );
    return roleData.data();
}

QVariant CFsModelT::headerData( int section, Qt::Orientation orientation, int role ) const
{
    if( orientation != Qt::Horizontal )
        return {};

    if( role != Qt::DisplayRole )
        return {};
    if( section < 0 || section >= columnView_.VecColumns.size() )
        return {};

    return columnView_.VecColumns[ section ].definition_.title_;
}

Qt::ItemFlags CFsModelT::flags( const QModelIndex& index ) const
{
    const auto Def = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    return index.column() == 0 ? Def | Qt::ItemIsEditable : Def;
}

