#pragma once

#include "columnMgr.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "qtWorkspaceState.hpp"
#include "qtDirectoryTracker.hpp"
#include "cmnTypeDefs.hpp"

#include "Modules/mdlFileModel.hpp"

// https://github.com/KubaO/stackoverflown/blob/master/questions/icon-proxy-39144638/main.cpp

class CFSIconGatherer : public QRunnable
{
    CFsModelT*                          model_ = nullptr;
    nsCmn::nsConcurrent::CCmnMutex*     lock_ = nullptr;
    QString                             currentPathWithRoot_;
    QVector< nsHC::TySpFileSource >&    vecNode_;

public:
    CFSIconGatherer( CFsModelT* Model, QVector< nsHC::TySpFileSource >& Nodes, 
                     const QString& CurrentPathWithRoot,
                     nsCmn::nsConcurrent::CCmnMutex* lock = nullptr )
        : model_( Model ), vecNode_( Nodes ), currentPathWithRoot_( CurrentPathWithRoot ), lock_( lock ) {}

    void                            run() override;

    void                            requestInterruption() { interrupt_.testAndSetOrdered( 0, 1 ); }
    bool                            isInterruptionRequested() const { return static_cast< int >( interrupt_ ); }
    void                            clearInterruption() { interrupt_.testAndSetOrdered( 1, 0 ); }

private:
    QAtomicInt                      interrupt_;
};

class CFSDirSizeGatherer : public QRunnable
{
    CFsModelT* model_ = nullptr;
    nsCmn::nsConcurrent::CCmnMutex*  lock_ = nullptr;
    QString                          currentPathWithRoot_;
    QVector< nsHC::TySpFileSource >& vecNode_;
    int                              sizeCol_ = -1;

public:
    CFSDirSizeGatherer( CFsModelT* Model, QVector< nsHC::TySpFileSource >& Nodes,
                     const QString& CurrentPathWithRoot, int SizeCol,
                     nsCmn::nsConcurrent::CCmnMutex* lock = nullptr )
        : model_( Model ), vecNode_( Nodes ), currentPathWithRoot_( CurrentPathWithRoot ), sizeCol_( SizeCol ), lock_( lock ) {}

    void                            run() override;
    void                            setCurrentPath( const QString& CurrentPath ) { currentPathWithRoot_ = CurrentPath; }

    void                            requestInterruption() { interrupt_.testAndSetOrdered( 0, 1 ); }
    bool                            isInterruptionRequested() const { return static_cast< int >( interrupt_ ); }
    void                            clearInterruption() { interrupt_.testAndSetOrdered( 1, 0 ); }

private:
    QAtomicInt                      interrupt_;
};

class CFSModel : public CFsModelT
{
    Q_OBJECT
public:

    ////////////////////////////////////////////////////////////////////////////
    ///

    CFSModel();
    ~CFSModel() override;
    void SetColumnView(const ColumnView& View) override;
    void SetColumnSorts(const QVector<hc::columns::SortDefinition>& Sorts);
    const QVector<hc::columns::SortDefinition>& ColumnSorts() const { return columnSorts_; }
    void CancelColumnQueries();
    void ResumeColumnQueries();
    QVector<hc::columns::FieldDefinition> AvailableColumns() const;
    int PendingColumnQueries() const { return columnQueries_->Pending(); }
    static constexpr int GridSortRole = Qt::UserRole + 65;
    static constexpr int ColumnStateRole = Qt::UserRole + 64;
    void SetWatchEnabled(bool Enabled);
    void NotifyNetworkChange();
    void SetPlaceholder(const hc::storage::Locator& Location, const QString& Reason);
    hc::storage::Locator WorkspaceLocation() const;
    void ClearPlaceholder();
    bool HasPlaceholder() const { return placeholder_.has_value(); }
    quint64 MetadataGeneration() const { return index_.Generation(); }
    auto SearchMetadata() const { return index_.SearchMetadata(); }
    bool PutMetadata(const hc::storage::EntrySnapshot& Entry, const QMap<QString, QString>& Values, quint64 Generation);
    bool MatchesSearch(const hc::storage::EntrySnapshot& Entry, const QString& Query) const { return index_.Matches(Entry, Query); }
    void                            SetProviderEnabled(bool Enabled);
    bool                            ProviderEnabled() const { return providerEnabled_; }
    bool                            IsLoading() const override { return loading_; }
    bool                            HandlesRefreshSignals() const override { return providerEnabled_; }
    void                            CancelNavigation();
    std::optional<hc::storage::Error> LastNavigationError() const { return navigationError_; }

    QString                         GetCurrentPathWithRoot() const override { return retrieveRootWithPath(); }
    QString                         GetName( const QModelIndex& Index ) const;
    void                            SetColorScheme(const TyColorScheme& Scheme) override;
    void                            ApplyDisplaySettings();

    ////////////////////////////////////////////////////////////////////////////
    /// 파일 / 디렉토리 조작 
    
    QVector< nsHC::TySpFileSource > GetChildItems( const QString& RootWithPath, bool IncludeDotDot = false );
    TyOsError                       InsertChildItem( const QString& NewName );

    // QString                         GetFileFullPath( const QString& Name ) const;


//    QString                             GetCurrentPath() const;
//        
//
//    DWORD                               Rename( const QModelIndex& Index, const QString& NewName );
//    QVector< Node >                     GetChildItems( const QString& Path );
//
    ////////////////////////////////////////////////////////////////////////////
    /// 모델
    
    void                            multiData( const QModelIndex& index, QModelRoleDataSpan roleDataSpan ) const override;

    // Resource IDs belong to the provider that published the displayed rows.
    // Another tab may refresh the provider cached by a shared filesystem root.
    std::shared_ptr<hc::storage::IStorageProvider> SnapshotProvider() const { return acceptedProvider_; }

signals:
    void NavigationCancelled();
    void RetryRequested();
    void MetadataChanged();
    void ColumnValuesChanging();
    void ColumnValuesChanged();
    void ColumnQueryStatus(const QString& Text);
    void ColumnSortCancelled();
protected:
    void                            doRefresh() override;

private:
    friend class CFSIconGatherer;
    friend class CFSDirSizeGatherer;

    QString                         retrieveRootWithPath() const;
    void                            createBuiltFsValues( QVector< nsHC::TySpFileSource >& Items );
    void                            createBuiltFsValues( const nsHC::TySpFileSource& Item, int Column );
    void                            refreshProvider();
    void                            publishProvider(std::vector<hc::storage::EntrySnapshot> Entries);
    void                            refreshDisplayIcons();
    void                            refreshNameSortRanks();
    hc::columns::CellValue readField(const nsHC::TySpFileSource& Item, const QString& Field, bool Request, bool Urgent = true) const;
    hc::columns::CellValue evaluateColumn(const nsHC::TySpFileSource& Item, const Column& Binding, bool Request, bool Sort = false) const;
    QString queryPath(const nsHC::TySpFileSource& Item) const;
    void columnResultsReady(const QSet<QString>& Paths);
    std::unique_ptr<hc::columns::QueryEngine> columnQueries_;
    QMap<QUuid, QString> scriptRevisions_;
    mutable bool requestOnlyValues_ = false;
    QVector<int>                    nameSortRanks_, columnSortRanks_;
    QVector<hc::columns::SortDefinition> columnSorts_, completedSorts_;
    void refreshColumnSortRanks();
    QJsonObject                     displaySettings_;
    quint64                         iconGeneration_ = 0;
    std::stop_source                iconStop_;
    bool                            providerEnabled_ = true;
    QtDirectoryTracker              tracker_;
    bool                            watchEnabled_ = false;
    std::optional<hc::storage::Locator> placeholder_;
    hc::workspace::DirectoryIndex   index_;
    bool                            loading_ = false;
    std::uint64_t                   generation_ = 0;
    std::stop_source                navigationStop_;
    std::optional<hc::storage::Error> navigationError_;
    QVector<nsHC::TySpFileSystem>    acceptedRoots_;
    QString                         acceptedPath_;
    std::shared_ptr<hc::storage::IStorageProvider> acceptedProvider_;

    CFSIconGatherer*                FsIconGatherer_ = nullptr;
    CFSDirSizeGatherer*             DirSizeGatherer_ = nullptr;

//    // 경로 구분자로 끝나지 않는 형태( 예). C:, \\DEVTESTER-QST 등 )
//    // Root 가 빈 문자열이라면 드라이브 목록을 표시한다. 
//    QString                             Root;
//    // 경로 구분자로 시작하는 경로, Root + CurrentPath 를 더하면 완전 경로가 나온다. 경로 구분자는 \\ 사용
//    QString                             CurrentPath;
//    ColumnView                          CurrentView;
//    QPixmap                             Icon_Directory;
//    // TODO: Mutex 등을 통해 추가로 보호해야 한다.
};

class CFSProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:

    Q_INVOKABLE void                    SetHiddenSystem( bool IsShow );
    Q_INVOKABLE bool                    GetHiddenSystem() const;
    void SetSearch(const QString& Query) { search_ = Query.left(4096); invalidateFilter(); }
    QString Search() const { return search_; }

    bool                                filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const override;

private:

    bool                                IsShowHiddenSystem_ = false;
    QString                             search_;
};
