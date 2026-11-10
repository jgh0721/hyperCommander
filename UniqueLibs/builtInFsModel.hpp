#pragma once
#include "columnPreview.hpp"
#include <functional>
#include <QSet>
#include <QWinEventNotifier>
#include <QElapsedTimer>

#include "columnMgr.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/columns/columnSort.hpp"
#include "commonLibs/columns/columnGrouping.hpp"
#include "qtWorkspaceState.hpp"
#include "qtDirectoryTracker.hpp"
#include "qtPagedStorage.hpp"
#include "cmnTypeDefs.hpp"

#include "Modules/mdlFileModel.hpp"
#include "flatListingSession.hpp"
#include "commonLibs/columns/sortRankWorker.hpp"
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
    // 제목표시줄 미리 듣기의 현재 곡: 0 없음, 1 재생, 2 일시정지.
    void SetNowPlaying(const QString& Key, int State);
    static constexpr int NowPlayingRole = Qt::UserRole + 75;
    void SetColumnSorts(const QVector<hc::columns::SortDefinition>& Sorts);
    const QVector<hc::columns::SortDefinition>& ColumnSorts() const { return columnSorts_; }
    void SetGrouping(const hc::columns::GroupingState& State);
    void SetGroupingLevels(const QVector<hc::columns::GroupingState>& Levels);
    void RefreshGroupingDate();
    static constexpr int GroupKeyRole = hc::columns::GroupKeyRole;
    static constexpr int GroupOrderRole = hc::columns::GroupOrderRole;
    static constexpr int GroupTextRole = hc::columns::GroupTextRole;
    void CancelColumnQueries();
    void ResumeColumnQueries();
    QVector<hc::columns::FieldDefinition> AvailableColumns() const;
    QVector<hc::columns::PreviewSample> ColumnSamples(const QStringList& Selection = {}) const;
    // Rows for the colour rules preview (same 50-row bound and selection keys as ColumnSamples).
    QVector<hc::settings::FileRuleSample> FileRuleSamples(const QStringList& Selection = {}) const;
    int PendingColumnQueries() const { return columnQueries_->Pending(); }
    void StartEverythingSearch(const QString& Query, const QString& Match, int Interval);
    void StopEverythingSearch(bool Leave = false);
    void SetEverythingDisplayInterval(int Interval);
    bool IsSearchResults() const { return searchResults_; }
    QString EverythingQuery() const { return everythingQuery_; }
    QString EverythingStatus() const { return everythingStatus_; }
    bool IsEverythingRunning() const;
    // 여러 위치 결과의 공급원. Everything 검색과 평면 보기는 같은 결과 모드(IsSearchResults)를 쓴다.
    enum class ResultSource { None, Everything, Flat };
    ResultSource ResultSourceKind() const { return searchResults_ ? resultSource_ : ResultSource::None; }
    // 평면 보기: 기준 폴더 아래 항목을 한 목록에 모은다. 열거가 끝나거나 중지되어도 평면 상태는 유지된다.
    void StartFlatListing(hc::flat::ListingOptions Options);
    // Leave가 false면 열거만 멈추고 그때까지의 결과를 남긴다.
    void StopFlatListing(bool Leave = false);
    bool IsFlatListing() const { return searchResults_ && resultSource_ == ResultSource::Flat; }
    bool IsFlatRunning() const { return flatSession_ && flatSession_->Running(); }
    const QString& FlatBase() const { return flatBase_; }
    const hc::flat::ListingOptions& FlatOptions() const { return flatOptions_; }
    const hc::flat::Status& FlatStatus() const { return flatStatus_; }
    // 평면 보기 중 기준 폴더 아래가 바뀌었는지(로컬만 감시). 자동으로 합치지 않고 상태 표시줄에 F5를 안내한다.
    bool FlatChanged() const { return flatChanged_; }
    // 평면 보기 트리(시안 E): 같은 그리드에서 트리 순서로 정렬하고, 폴더 행을 접으면 그 아래 행을 숨긴다.
    // ExpandLevel: -1 모두 펼침 · 1 첫 단계만 · 0 모두 접기. FolderTotal: 폴더 행 크기에 하위 파일 합계.
    void SetFlatTree(bool Tree, int ExpandLevel = -1, bool FolderTotal = true);
    bool FlatTree() const { return IsFlatListing() && flatTree_; }
    int FlatTreeDepth(int Row) const;
    bool FlatTreeHidden(int Row) const;
    bool FlatFolderCollapsed(int Row) const;
    void SetFlatFolderExpanded(int Row, bool Expanded);
    // 행의 트리 부모(상대 부모 폴더 행). 없으면 -1.
    int FlatTreeParent(int Row) const;
    // 평면 폴더 세트의 정렬 · 그룹 표시: 이름 정렬에 경로 순 비교, 폴더 그룹 제목을 마지막 폴더 이름으로, 그룹 순서를 최근 수정 먼저로.
    void SetFlatPresentation(bool PathSort, bool GroupLeaf, bool GroupRecent);
    // 파일 작업이 끝난 폴더만 다시 확인한다: 사라진 행은 빼고, 새로 생긴 항목은 그 폴더 아래만 다시 열거해 더한다.
    void RefreshFlatLocations(const QStringList& Folders);
    // 여러 위치 결과에서 행을 뺀다(정렬 순위는 작업 스레드에서 다시 계산).
    void RemoveSearchRows(QVector<int> Rows);
    // 이름 앞에 그리는 상대 부모 경로(열 0, 평면 보기에서만 값이 있다).
    static constexpr int FlatPrefixRole = Qt::UserRole + 76;
    // 평면 트리: 들여쓰기 깊이와 접기 표시(없음 · 0 펼침 · 1 접힘).
    static constexpr int FlatTreeDepthRole = Qt::UserRole + 77;
    static constexpr int FlatTreeExpanderRole = Qt::UserRole + 78;
    // 시험용: 정렬 작업 스레드가 아직 순위를 계산 중인지.
    bool SortRanksPending() const { return searchResults_ && sortRanked_ < sortSubmitted_; }
    std::shared_ptr<hc::storage::IStorageProvider> ProviderFor(const hc::storage::EntrySnapshot& Entry) const;
    static constexpr int FileColorOverrideRole = Qt::UserRole + 66;
    static constexpr int GridSortRole = Qt::UserRole + 65;
    static constexpr int ColumnStateRole = Qt::UserRole + 64;
    void SetWatchEnabled(bool Enabled);
    void SetSelectionInputActive(bool Active);
    void ApplyRefreshSettings();
    bool IsAutomaticRefresh() const { return automaticRefresh_; }
    // An automatic rescan retains the published location and entry snapshot.
    bool CanUseSnapshot() const { return !loading_ || automaticRefresh_; }
    void NotifyNetworkChange();
    void NotifyComputerChange();
    void SetPlaceholder(const hc::storage::Locator& Location, const QString& Reason, bool Pending = false);
    hc::storage::Locator WorkspaceLocation() const;
    void ClearPlaceholder();
    bool HasPlaceholder() const { return placeholder_.has_value(); }
    bool HasNavigationFallback() const { return recoveryLocation_.has_value(); }
    using CFsModelT::ChangeDirectory;
    void ChangeDirectory(const QString& Child) override;
    hc::storage::Result<hc::storage::Unit> EnterFile(const QModelIndex& Index, std::function<void()> BeforeMount = {});
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

    QString                         GetCurrentPathWithRoot() const override { return recoveryLocation_ ? QString::fromStdString(recoveryLocation_->root_) : retrieveRootWithPath(); }
    QString                         GetName( const QModelIndex& Index ) const;
    void                            SetColorScheme(const TyColorScheme& Scheme) override;
    void                            ApplyDisplaySettings();
    void RequestDirectorySizes(const QModelIndexList& Entries = {}, bool Automatic = false);
    void ApplyDirectorySizeSettings();
    int PendingDirectorySizes() const { return int(directorySizePending_.size()); }
    // Read-only observation: never schedules evaluation or expires cache entries.
    // Ahead: rows below the viewport (design 6.3: up to two screens) whose slow rule values are asked for
    // at normal priority before they scroll in.
    void SetFileColorViewport(const QModelIndexList& Rows, bool Active, const QModelIndexList& Ahead = {});
    // Design 6.4: rows still waiting for rule values keep the default look for the first 120 ms of a listing.
    bool InFileRuleGrace() const { return fileRuleGrace_; }
    // Whether colour rules use slow (queried) fields, so the view has rows ahead worth passing.
    bool HasSlowFileRuleFields() const { return fileColorRules_.HasRules() && hasQueryRuleFields(); }
    bool AreFileColorsReady(const QModelIndexList& Rows) const;
    quint64 FileRuleGeneration() const { return fileRuleLoadGeneration_; }
    bool FileRulesLoading() const { return fileRulesLoading_; }
    bool FileRuleCompletionIdle() const { return !fileRuleCompleting_ && !fileRuleCompleteTimer_.isActive(); }
    // Item selection reads the same field values as file colors (design 7.2): list values and
    // values already known; Pending when a query is still needed. Never queues anything.
    hc::columns::CellValue RuleFieldValue(const nsHC::TySpFileSource& Item, const QString& Field) const { return ruleFieldValue(Item, Field); }
    QString FieldQueryPath(const nsHC::TySpFileSource& Item) const { return queryPath(Item); }
    // Column and rule queries submitted by this listing's engine (diagnostics and tests).
    quint64 ColumnQueryJobs() const { return columnQueries_->Started(); }
    // Values a selection scan read are kept for colors too; repaints pick them up.
    void ShareRuleFieldValues(const QString& Path, const hc::columns::FieldValues& Values);

    ////////////////////////////////////////////////////////////////////////////
    /// 파일 / 디렉토리 조작 
    
    QVector< nsHC::TySpFileSource > GetChildItems( const QString& RootWithPath, bool IncludeDotDot = false, quint32* FailureCode = nullptr );
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
    std::shared_ptr<hc::storage::IStorageProvider> SnapshotProvider() const
    { return !searchResults_ ? acceptedProvider_ : IsFlatListing() && flatKind_ == hc::storage::LocatorKind::Smb ? searchSmb_ : searchLocal_; }

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
    void FlatTreeChanged();
protected:
    void                            doRefresh() override;

private:
    friend class CFSIconGatherer;

    QString                         retrieveRootWithPath() const;
    void                            createBuiltFsValues( QVector< nsHC::TySpFileSource >& Items );
    void                            createBuiltFsValues( const nsHC::TySpFileSource& Item, int Column );
    void                            refreshProvider();
    void publishNavigationFallback(const hc::storage::Locator& Location);
    void requestPage(std::uint64_t Offset, bool Rebuild);
    hc::storage::Result<hc::storage::PagedEntryIndex::Less> pageComparison(const QString& ParentPath = {}, bool NameOnly = false) const;
    struct PreparedFileRow {
        QString name_, extension_;
        QDateTime created_, modified_;
        QByteArray pidl_;
        quint32 flags_ = 0, reparseTag_ = 0;
    };
    static PreparedFileRow prepareFileRow(const hc::storage::EntrySnapshot& Entry);
    static std::vector<PreparedFileRow> prepareFileRows(const std::vector<hc::storage::EntrySnapshot>& Entries, std::stop_token Token);
    void publishProvider(std::vector<hc::storage::EntrySnapshot> Entries, std::vector<hc::settings::FileRuleResult> Styles = {}, std::vector<PreparedFileRow> Prepared = {});
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
    ResultSource resultSource_ = ResultSource::None;
    std::unique_ptr<hc::flat::ListingSession> flatSession_;
    hc::flat::ListingOptions flatOptions_;
    hc::flat::Status flatStatus_;
    QString flatBase_;
    std::string flatBaseUtf8_;
    hc::storage::LocatorKind flatKind_ = hc::storage::LocatorKind::Local;
    bool flatPathSort_ = true, flatGroupLeaf_ = false, flatGroupRecent_ = false;
    // 상대 부모 경로(소문자) → 그 폴더 파일의 가장 최근 수정 시각. 폴더 그룹의 "최근 수정 먼저" 순서에 쓴다.
    QHash<QString, qint64> flatFolderLatest_;
    bool flatTree_ = false, flatTreeTotal_ = true;
    int flatTreeExpand_ = -1;
    QSet<QString> flatCollapsed_;
    QString flatRelative(const nsHC::TySpFileSource& Item, bool Self) const;
    void refreshFlatTreeTotals();
    // 파일 작업 뒤 영향받은 폴더만 다시 열거하는 보조 세션.
    std::unique_ptr<hc::flat::ListingSession> flatRescan_;
    // 기준 폴더 하위 포함 변경 알림(FindFirstChangeNotificationW, bWatchSubtree=TRUE).
    void* flatWatchHandle_ = nullptr;
    std::unique_ptr<QWinEventNotifier> flatWatch_;
    bool flatChanged_ = false;
    QElapsedTimer flatOwnChange_;
    void startFlatWatch();
    void stopFlatWatch();
    int flatDepth(const nsHC::TySpFileSource& Item) const;
    void noteFlatFolder(const nsHC::TySpFileSource& Item);
    // 여러 위치 결과의 열 정렬은 작업 스레드에서 병합한다(정렬 키 사전 계산).
    std::unique_ptr<hc::columns::SortRankWorker> sortWorker_;
    quint64 sortGeneration_ = 0;
    int sortSubmitted_ = 0, sortRanked_ = 0;
    QVector<hc::columns::SortDefinition> sortSubmittedSorts_;
    hc::columns::SortTextOptions sortSubmittedOptions_;
    hc::columns::SortTextOptions sortTextOptions() const;
    void resetSortWorker();
    void applySortRanks(quint64 Generation, int Rows, QVector<int> Ranks);
    void notifySortRanks(const QVector<int>& Previous);
    hc::columns::CellValue readField(const nsHC::TySpFileSource& Item, const QString& Field, bool Request, bool Urgent = true) const;
    hc::columns::CellValue evaluateColumn(const nsHC::TySpFileSource& Item, const Column& Binding, bool Request, bool Sort = false) const;
    QString queryPath(const nsHC::TySpFileSource& Item) const;
    void columnResultsReady(const QSet<QString>& Paths);
    // Query families read by a column's value, template or sort field.
    static QSet<QString> bindingFamilies(const Column& Binding);
    // True when a value of these families can move rows: sort, grouping or their templates use it.
    bool orderUsesFamilies(const QSet<QString>& Families) const;
    // Rows by query path, rebuilt after structural changes; a stale or missing hit rebuilds once.
    void rebuildQueryRows();
    QHash<QString, QVector<int>> queryRows_;
    bool queryRowsValid_ = false;
    hc::settings::FileRuleInput fileRuleInput(const nsHC::TySpFileSource& Item) const;
    // Drawn: the row is being painted or is in the viewport; other rows only ask for slow values ahead.
    hc::settings::FileRuleResult fileRuleStyle(int Row, bool Drawn = true) const;
    // Keep a small display-only fallback while a field arrival re-keys a visible row.
    struct DisplayedFileRule { nsHC::TySpFileSource item_; hc::settings::FileRuleResult style_; };
    mutable QCache<int, DisplayedFileRule> displayedFileRules_{256};
    // Design 6.2: column field values used by file color rules, kept for the listing's lifetime
    // (the column cache expires after 30 s and a repaint must not re-query a decided row).
    mutable QHash<QString, hc::columns::FieldValues> ruleFields_;
    // Path \0 family of rule queries in flight, for the status line.
    mutable QSet<QString> ruleFieldRequests_;
    mutable QHash<QString, bool> networkRoots_;
    // Paths whose values were carried over a background re-list; a drawn row re-checks them once,
    // because a stream write need not change the metadata the carry compared (design 6.2).
    mutable QSet<QString> ruleFieldsStale_;
    void revalidateRuleFields(const nsHC::TySpFileSource& Item) const;
    mutable bool ruleStatusQueued_ = false;
    bool ruleRecolorQueued_ = false;
    QSet<QString> ruleRecolorPaths_;
    hc::columns::CellValue ruleFieldValue(const nsHC::TySpFileSource& Item, const QString& Field) const;
    bool ruleFieldAllowed(const nsHC::TySpFileSource& Item, const QString& Field) const;
    void requestRuleFields(const nsHC::TySpFileSource& Item, quint64 Slots, bool Urgent = true) const;
    bool hasQueryRuleFields() const;
    void prefetchRuleFields(int Row, const hc::settings::FileRuleResult& Style) const;
    void endFileRuleGrace();
    // Design 6.3: rows up to two screens below the viewport; their rule values are asked for at normal
    // priority, at most FileRuleAheadRequests in flight so a long listing never floods the query queue.
    QSet<int> fileRuleAheadRows_;
    static constexpr int FileRuleAheadRequests = 256;
    // Design 6.4: a listings first batch waits up to FileRuleGraceMs for rule values before it shows the
    // pending colour, so a value that arrives in time changes the row once.
    static constexpr int FileRuleGraceMs = 120;
    QTimer fileRuleGraceTimer_;
    bool fileRuleGrace_ = false;
    bool isNetworkLocation(const hc::storage::Locator& At) const;
    void refreshColumnStatus();
    void clearRuleFields();
    // Leading rows are styled inside the listing worker so the first paint
    // carries group colors; the budget bounds a pathological rule set.
    static constexpr std::size_t FileRuleSeedRows = 4096;
    static constexpr int FileRuleSeedBudgetMs = 25;
    static std::vector<hc::settings::FileRuleResult> seedFileRuleStyles(const hc::settings::FileColorRules& Rules, const std::vector<hc::storage::EntrySnapshot>& Entries, bool Dark, std::stop_token Token);
    // A queried field the listing attributes already decide as Empty; never queried or left pending.
    static bool KnownEmptyField(quint32 Attributes, const QString& Field, bool Search);
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
    QHash<QUuid, QString> groupingModes_;
    QDate groupingDate_ = QDate::currentDate();
    QTimeZone groupingZone_ = QTimeZone::systemTimeZone();
    QVector<hc::columns::SortDefinition> columnSorts_, completedSorts_, pageSorts_;
    // Sort rules of the last GridSortRole notification; with them unchanged only moved ranks are sent.
    QVector<hc::columns::SortDefinition> notifiedRankSorts_;
    void refreshColumnSortRanks(int First = 0);
    QJsonObject                     displaySettings_;
    quint64                         iconGeneration_ = 0;
    int                             iconPixels_ = 32;
    std::stop_source                iconStop_;
    QVector<int>                    iconVisibleRows_;
    QTimer                          iconApplyTimer_;
    bool                            iconViewportBound_ = false, iconViewportEnabled_ = true;
    QSet<QString> clipboardCutKeys_;
    QString clipboardCutRoot_;
    QString nowPlayingKey_;
    int nowPlayingState_ = 0;
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
    std::optional<hc::storage::Locator> recoveryLocation_;
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
