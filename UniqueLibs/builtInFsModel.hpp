#pragma once
#include "columnPreview.hpp"
#include <functional>
#include <QSet>

#include "columnMgr.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/columns/columnSort.hpp"
#include "commonLibs/columns/columnGrouping.hpp"
#include "qtWorkspaceState.hpp"
#include "qtDirectoryTracker.hpp"
#include "qtPagedStorage.hpp"
#include "cmnTypeDefs.hpp"

#include "Modules/mdlFileModel.hpp"
namespace hc::permissions { class DirectoryElevationSession; }
namespace hc::search { class EverythingSession; }

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

class CFSModel : public CFsModelT
{
    Q_OBJECT
public:

    ////////////////////////////////////////////////////////////////////////////
    ///

    CFSModel();
    ~CFSModel() override;
    void SetColumnView(const ColumnView& View) override;
    void SetColumnLayoutMode(hc::columns::LayoutMode Mode);
    // 목록이 그리는 아이콘의 물리 px. 바뀌면 아이콘을 그 크기로 다시 읽는다.
    void SetIconPixels(int Pixels);
    int IconPixels() const { return iconPixels_; }
    void SetIconViewport(const QModelIndexList& Entries, bool Enabled);
    void ForceRefresh();
    void SetClipboardCut(const QSet<QString>& Keys, const QString& Root);
    static constexpr int ClipboardCutRole = Qt::UserRole + 73;
    static constexpr int FileEffectsRole = Qt::UserRole + 74;
    void SetColumnSorts(const QVector<hc::columns::SortDefinition>& Sorts);
    const QVector<hc::columns::SortDefinition>& ColumnSorts() const { return columnSorts_; }
    void SetGrouping(const hc::columns::GroupingState& State);
    void RefreshGroupingDate();
    static constexpr int GroupKeyRole = Qt::UserRole + 70;
    static constexpr int GroupOrderRole = Qt::UserRole + 71;
    static constexpr int GroupTextRole = Qt::UserRole + 72;
    void CancelColumnQueries();
    void ResumeColumnQueries();
    QVector<hc::columns::FieldDefinition> AvailableColumns() const;
    QVector<hc::columns::PreviewSample> ColumnSamples(const QStringList& Selection = {}) const;
    int PendingColumnQueries() const { return columnQueries_->Pending(); }
    void StartEverythingSearch(const QString& Query, const QString& Match, int Interval);
    void StopEverythingSearch(bool Leave = false);
    void SetEverythingDisplayInterval(int Interval);
    bool IsSearchResults() const { return searchResults_; }
    QString EverythingQuery() const { return everythingQuery_; }
    QString EverythingStatus() const { return everythingStatus_; }
    bool IsEverythingRunning() const;
    std::shared_ptr<hc::storage::IStorageProvider> ProviderFor(const hc::storage::EntrySnapshot& Entry) const;
    static constexpr int FileColorOverrideRole = Qt::UserRole + 66;
    static constexpr int GridSortRole = Qt::UserRole + 65;
    static constexpr int ColumnStateRole = Qt::UserRole + 64;
    void SetWatchEnabled(bool Enabled);
    void SetSelectionInputActive(bool Active);
    void ApplyRefreshSettings();
    bool IsAutomaticRefresh() const { return automaticRefresh_; }
    void NotifyNetworkChange();
    void SetPlaceholder(const hc::storage::Locator& Location, const QString& Reason, bool Pending = false);
    hc::storage::Locator WorkspaceLocation() const;
    void ClearPlaceholder();
    bool HasPlaceholder() const { return placeholder_.has_value(); }
    quint64 DirectoryGeneration() const { return generation_; }
    std::shared_ptr<hc::storage::IEntryIndex> DirectoryEntries() const;
    quint64 MetadataGeneration() const { return index_.Generation(); }
    auto SearchMetadata() const { return index_.SearchMetadata(); }
    bool PutMetadata(const hc::storage::EntrySnapshot& Entry, const QMap<QString, QString>& Values, quint64 Generation);
    bool MatchesSearch(const hc::storage::EntrySnapshot& Entry, const QString& Query) const { return index_.Matches(Entry, Query); }
    void                            SetProviderEnabled(bool Enabled);
    bool                            ProviderEnabled() const { return providerEnabled_; }
    bool                            IsLoading() const override { return loading_; }
    // Large directories use a disk-backed sort snapshot, but expose every row.
    static constexpr std::uint64_t IndexedOrderThreshold = 4096;
    bool UsesIndexedOrder() const { return pageSource_ && pageSource_->Count() > IndexedOrderThreshold; }
    std::uint64_t EntryCount() const { return searchResults_ ? vecNode_.size() : pageView_ ? pageView_->Count() : 0; }
    bool IsPageLoading() const { return pageBusy_; }
    void SetPageFilter(const QString& Query, bool ShowHidden, const QString& Match = {});
    hc::storage::Result<std::shared_ptr<hc::storage::ISelectionManifest>> SelectAllEntries() const;
    bool                            HandlesRefreshSignals() const override { return providerEnabled_; }
    // Superseding: the caller starts another navigation at once, so the
    // cancelled state is not shown unless that navigation never arrives.
    void                            CancelNavigation(bool Superseding = false);
    std::optional<hc::storage::Error> LastNavigationError() const { return navigationError_; }
    bool RetryFailedNavigation(bool Elevated = false);
    void DismissNavigationError();

    QString                         GetCurrentPathWithRoot() const override { return retrieveRootWithPath(); }
    QString                         GetName( const QModelIndex& Index ) const;
    void                            SetColorScheme(const TyColorScheme& Scheme) override;
    void                            ApplyDisplaySettings();
    void RequestDirectorySizes(const QModelIndexList& Entries = {}, bool Automatic = false);
    void ApplyDirectorySizeSettings();
    int PendingDirectorySizes() const { return int(directorySizePending_.size()); }
    // Read-only observation: never schedules evaluation or expires cache entries.
    void SetFileColorViewport(const QModelIndexList& Rows, bool Active);
    bool AreFileColorsReady(const QModelIndexList& Rows) const;
    quint64 FileRuleGeneration() const { return fileRuleLoadGeneration_; }
    bool FileRulesLoading() const { return fileRulesLoading_; }
    bool FileRuleCompletionIdle() const { return !fileRuleCompleting_ && !fileRuleCompleteTimer_.isActive(); }

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
    std::shared_ptr<hc::storage::IStorageProvider> SnapshotProvider() const { return searchResults_ ? searchLocal_ : acceptedProvider_; }

signals:
    void DirectoryReadStarted(const QString& Path, bool Archive);
    void NavigationCancelled();
    void DirectoryReadFailed(const QString& Path, bool Local);
    void RetryRequested();
    void MetadataChanged();
    void DirectorySizesChanged();
    void ColumnValuesChanging();
    void ColumnValuesChanged();
    void ColumnQueryStatus(const QString& Text);
    void ColumnSortCancelled();
    void PageChanged();
    void SearchResultsChanged();
    void SearchRefreshRequested();
protected:
    void                            doRefresh() override;

private:
    friend class CFSIconGatherer;

    QString                         retrieveRootWithPath() const;
    void                            createBuiltFsValues( QVector< nsHC::TySpFileSource >& Items );
    void                            createBuiltFsValues( const nsHC::TySpFileSource& Item, int Column );
    void                            refreshProvider();
    void requestPage(std::uint64_t Offset, bool Rebuild);
    hc::storage::Result<hc::storage::PagedEntryIndex::Less> pageComparison(const QString& ParentPath = {}, bool NameOnly = false) const;
    void                            publishProvider(std::vector<hc::storage::EntrySnapshot> Entries, std::vector<hc::settings::FileRuleResult> Styles = {});
    void                            refreshDisplayIcons(int First = 0);
    void applyRequestedIcons();
    void invalidateDisplayIcons(bool All, int SettleMs = 0);
    void                            refreshNameSortRanks(int First = 0);
    void appendSearchResults(std::vector<hc::storage::EntrySnapshot> Entries);
    nsHC::TySpFileSource makeDisplayItem(hc::storage::EntrySnapshot Entry, const QPixmap& FolderIcon, const QPixmap& FileIcon) const;
    bool searchResults_ = false;
    QString everythingQuery_, everythingStatus_;
    std::unique_ptr<hc::search::EverythingSession> everythingSession_;
    std::shared_ptr<hc::storage::IStorageProvider> searchLocal_, searchSmb_;
    hc::columns::CellValue readField(const nsHC::TySpFileSource& Item, const QString& Field, bool Request, bool Urgent = true) const;
    hc::columns::CellValue evaluateColumn(const nsHC::TySpFileSource& Item, const Column& Binding, bool Request, bool Sort = false) const;
    QString queryPath(const nsHC::TySpFileSource& Item) const;
    void columnResultsReady(const QSet<QString>& Paths);
    hc::settings::FileRuleInput fileRuleInput(const nsHC::TySpFileSource& Item) const;
    hc::settings::FileRuleResult fileRuleStyle(int Row) const;
    // Leading rows are styled inside the listing worker so the first paint
    // carries group colors; the budget bounds a pathological rule set.
    static constexpr std::size_t FileRuleSeedRows = 4096;
    static constexpr int FileRuleSeedBudgetMs = 25;
    static std::vector<hc::settings::FileRuleResult> seedFileRuleStyles(const hc::settings::FileColorRules& Rules, const std::vector<hc::storage::EntrySnapshot>& Entries, bool Dark, std::stop_token Token);
    bool fileRuleSeedsUsable(quint64 RuleGeneration, bool Dark) const;
    void pumpFileRules();
    void resetFileRules();
    bool fileRuleFirstBatch_ = true;
    // Rows past the seeds are styled in the background so paging never reaches
    // an unstyled row; past the budget, a miss restarts the pass at that row.
    static constexpr int FileRuleCompleteRows = 65536;
    void completeFileRuleStyles();
    void requestFileRuleCompletion(int Row) const;
    mutable QTimer fileRuleCompleteTimer_;
    mutable int fileRuleCompleteLow_ = 0, fileRuleCompleteNext_ = 0, fileRuleCompleteBudget_ = 0;
    mutable bool fileRuleCompleting_ = false;
    std::shared_ptr<hc::settings::FileColorRules::EvaluationContext> fileRuleCompleteContext_ = std::make_shared<hc::settings::FileColorRules::EvaluationContext>();
    struct FileRuleJob { hc::settings::FileRuleKey key_; hc::settings::FileRuleInput input_; int row_; nsHC::TySpFileSource item_; };
    mutable QList<FileRuleJob> fileRuleQueue_;
    QSet<int> fileRuleVisibleRows_;
    bool fileRuleActiveViewport_ = false;
    mutable QSet<hc::settings::FileRuleKey> fileRulePending_;
    mutable QSet<int> fileRuleDeferredRows_;
    std::shared_ptr<hc::settings::FileColorRules::EvaluationContext> fileRuleContext_ = std::make_shared<hc::settings::FileColorRules::EvaluationContext>();
    mutable QTimer fileRulePump_;
    std::stop_source fileRuleStop_;
    quint64 fileRuleEpoch_ = 0;
    bool fileRuleRunning_ = false;
    std::unique_ptr<hc::columns::QueryEngine> columnQueries_;
    QMap<QUuid, QString> scriptRevisions_;
    mutable bool requestOnlyValues_ = false;
    QVector<int>                    nameSortRanks_, columnSortRanks_;
    hc::columns::GroupingState grouping_;
    QDate groupingDate_ = QDate::currentDate();
    QTimeZone groupingZone_ = QTimeZone::systemTimeZone();
    QVector<hc::columns::SortDefinition> columnSorts_, completedSorts_, pageSorts_;
    void refreshColumnSortRanks(int First = 0);
    QVector<hc::columns::SortRow> searchSortRows_;
    QVector<int> searchSortOrder_;
    QVector<hc::columns::SortDefinition> searchSorts_;
    QJsonObject                     displaySettings_;
    quint64                         iconGeneration_ = 0;
    int                             iconPixels_ = 32;
    std::stop_source                iconStop_;
    QVector<int>                    iconVisibleRows_;
    QTimer                          iconApplyTimer_;
    bool                            iconViewportBound_ = false, iconViewportEnabled_ = true;
    QSet<QString> clipboardCutKeys_;
    QString clipboardCutRoot_;
    bool                            forceIconRefresh_ = false;
    QString                         forceIconLocation_;
    std::stop_source                directorySizeStop_;
    QSet<QString>                   directorySizePending_;
    // Sizes carried over a refresh of the same folder: shown until re-measured, not "calculating".
    QSet<QString>                   directorySizeStale_;
    bool                            carryDirectorySizes_ = false;
    bool                            providerEnabled_ = true;
    QtDirectoryTracker              tracker_;
    bool                            watchEnabled_ = false;
    bool                            selectionInputActive_ = false;
    bool                            autoRefreshRequested_ = false, automaticRefresh_ = false;
    bool                            appendOrder_ = false;
    std::optional<hc::storage::Locator> placeholder_;
    hc::workspace::DirectoryIndex   index_;
    bool                            loading_ = false;
    std::uint64_t                   generation_ = 0;
    std::stop_source                navigationStop_;
    std::optional<hc::storage::Error> navigationError_;
    std::function<void()> failedNavigationRetry_;
    bool elevatedDirectoryRequest_ = false;
    std::shared_ptr<hc::permissions::DirectoryElevationSession> elevatedDirectorySession_;
    QVector<nsHC::TySpFileSystem>    acceptedRoots_;
    QString                         acceptedPath_;
    std::shared_ptr<hc::storage::IStorageProvider> acceptedProvider_;
    std::shared_ptr<hc::storage::PagedEntryIndex> pageSource_, pageView_;
    QString pageQuery_, pageMatch_;
    bool pageShowHidden_ = false;
    bool pageBusy_ = false;
    std::stop_source pageStop_;
    std::uint64_t pageGeneration_ = 0;

    CFSIconGatherer*                FsIconGatherer_ = nullptr;

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
    void SetSearch(const QString& Query, const QString& Match = {});
    void setSourceModel(QAbstractItemModel* Source) override;
    QString Search() const { return search_; }
    QString SearchMatch() const { return searchMatch_; }

    bool                                filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const override;

private:

    bool                                IsShowHiddenSystem_ = false;
    QString                             search_, searchMatch_;
};
