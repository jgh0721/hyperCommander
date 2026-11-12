#pragma once

#include <QtWidgets>
#include <QCache>
#include <functional>
#include <stop_token>
#include <fmfilelist/RecordPainting.h>
#include "filePanelSettings.hpp"
#include "commonLibs/columns/columnGrouping.hpp"
#include "commonLibs/columns/flatProfile.hpp"

#include "cmnTypeDefs.hpp"
#include "Modules/mdlFileSystem.hpp"

// Panel 에 표시되는 뷰 형태의 기초 클래스
// 또는 Lister 에 표시되는 WLX 등을 호스트한다.

class CFsModelT;
struct ColumnView;
class QtitanColumnAdapter;
class QtViewerHost;
class QtViewerSurface;
class ThumbnailModel;
namespace fm::filelist { class ThumbnailView; }
namespace hc::storage { class IContentSource; class IWritableStorage; }
namespace hc::document { struct DocumentHandlerCatalog; }

class CViewT : public QWidget
{
    Q_OBJECT
public:
    enum TyEnViewMode { VM_NONE, VM_GRID, VM_QUICK, VM_LISTER, VM_SYSTEM, VM_DETAIL };
    CViewT( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    TyEnViewMode                    GetViewMode() const { return viewMode_; }
    virtual void                    ApplyColorScheme( const TyColorScheme& ColorScheme );
    virtual bool                    IsEditing() const { return false; };
    virtual void                    AdjustSizeToContents() const {};
    virtual void                    EnsureKeyboardFocusOnView() const {};
    virtual void                    CloseView() {};

signals:
    void                            sig_NotifyViewActivated();

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;

    TyEnViewMode                    viewMode_ = VM_NONE;
};

namespace Qtitan
{
    class Grid;
    class GridViewBase;
    class GridSelection;
    class GridRecordPainter;
    class GridBandedTableView;
    class ContextMenuEventArgs;
    class CellClickEventArgs;
    class RowClickEventArgs;
    class EditorEventArgs;
} // Qtitan

class CGridView : public CViewT
{
    Q_OBJECT
public:
    CGridView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );
    ~CGridView() override;
    bool SetListMode(const QString& Mode);
    void SetEverythingPresentation(bool Enabled);
    bool EverythingPresentation() const { return everythingPresentation_; }
    // 평면 보기: 이름 앞에 상대 경로를 그린다. 목록 모드 · 썸네일은 사용자 설정을 유지한다.
    void SetFlatPresentation(bool Enabled);
    bool FlatPresentation() const { return flatPresentation_; }
    // 이름 앞 상대 경로(시안 D). 다른 평면 세트(경로 열 · 둘째 줄 · 그룹)에서는 그리지 않는다.
    bool FlatPrefixVisible() const { return flatPresentation_ && flatPrefix_; }
    bool FlatPrefixDim() const { return flatPrefixDim_; }
    // 평면 트리(시안 E): 이름 칸 들여쓰기 한 단계의 폭.
    static constexpr int FlatTreeStep = 16;
    bool FlatTreeVisible() const { return flatPresentation_ && flatTree_; }
    bool FlatTreeLines() const { return flatTreeLines_; }
    QString ListMode() const;
    void MoveQuickFindCursor(const QString& Query, const QString& Match, int Direction = 0);
    void CancelQuickFind();
    QString ListModeOverride() const { return listModeOverride_.isEmpty() ? "inherit" : listModeOverride_; }
    const fm::filelist::ListAppearance& ListAppearance() const { return listAppearance_; }
    const fm::filelist::detail::RecordGeometry& RecordGeometry() const { return recordGeometry_; }
    const fm::style::ThemeColors& RecordColors() const { return recordColors_; }
    // 레코드 블록 높이에 맞춘 아이콘 크기(논리 px)와 2줄 보기의 글자 들여쓰기.
    int ListIconSize() const;
    int TwoLineTextIndent() const;
    bool IsPanelActive() const { return panelActive_; }
    QModelIndexList VisibleSourceRows(int Overscan = 0) const;
    // Source rows below the viewport, up to Screens screen heights, in view order.
    QModelIndexList AheadSourceRows(int Screens) const;
    void RecordStartupPaint(const QModelIndex& Index,int Part);
    void StartupPaintCompleted();
    bool IsTwoLine() const;
    bool IsThumbnailMode() const;
    // 제목표시줄 미리 듣기가 재생 중일 때 재생 행의 막대를 움직인다.
    void SetNowPlayingAnimated(bool On);
    bool NowPlayingAnimating() const;
    QPoint ListContentTopLeft() const;
    using RenameCompletion = std::function<void(bool)>;
    using RenameHandler = std::function<void(const QModelIndex&, const QString&, RenameCompletion)>;
    bool BeginRename(const QModelIndex& Index, bool SelectExtension, RenameHandler Handler);
    void CancelRename();
    void SyncThumbnailCursor();
    void ClearThumbnailCache();
    ThumbnailModel* Thumbnails() const;


    void                            SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns );
    void                            ApplyColorScheme( const TyColorScheme& ColorScheme ) override;
    void                            ApplyDisplaySettings();
    void                            SetPanelActive(bool Active);
    bool                            IsEditing() const override;
    void                            AdjustSizeToContents() const override;
    void                            EnsureKeyboardFocusOnView() const override;

    nsHC::TySpFileSource            SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown, bool& IsSelected );
    // 목록 문맥 메뉴(설계 261110 6절 · 9절). Shell이면 탐색기 메뉴, 아니면 HC 문맥 메뉴를 연다.
    // 마우스로 선택 밖의 행을 열면 선택을 그 행으로 바꾼다(이전 선택은 hc.selection.restore).
    enum class MenuOrigin { Mouse, Keyboard };
    bool ShowContextMenu(const QModelIndex& Index, MenuOrigin Origin, bool Shell = false);
    // general.filePanel.rightClickMenu 가 "shell"이면 오른쪽 클릭이 탐색기 메뉴를 연다.
    static bool RightClickOpensShellMenu();
    // 커서 행(없으면 목록 왼쪽 위)의 아래 모서리. 키보드로 연 메뉴의 기준점이며 전역 논리 좌표다.
    QPoint MenuAnchor(const QModelIndex& Index);
    // Global(전역 논리 좌표)에 Paths의 탐색기 메뉴를 연다. Source가 오디오 파일 하나면 "오디오 재생"을 더한다.
    bool OpenShellMenuAt(const QStringList& Paths, const QPoint& Global, const QModelIndex& Source = {});
    // 원본 모델 색인을 목록(프록시) 색인으로. 명령 경로의 CursorIndex 와 같은 종류다.
    QModelIndex ViewIndex(const QModelIndex& Source) const;
    bool ShowsHiddenItems() const;
    QStringList DragPaths(const QModelIndex& Index) const;
    QString FileDropDirectory(const QModelIndex& Index, bool SameView) const;
    Qt::DropAction ChooseFileDropAction(int Count, const QString& Destination, Qt::DropActions Allowed, const QPoint& Position);
    QStringList ShellMenuPaths(const QModelIndex& Index = {}, bool UseSelection = true) const;
    bool ShowShellMenu(const QModelIndex& Index = {}, bool AtCursor = false);

    Qtitan::GridViewBase*           BaseView() const;
    CFsModelT*                      BaseModel() const;
    QtitanColumnAdapter*            ColumnWidths() const { return columnAdapter_; }
    void                           ConfigureColumns();
    // Flat: 평면 폴더 세트 적용. 수동 고정(manualColumns_)을 켜지 않고, 종료 시 원래 상태 복원으로 되돌린다.
    enum class ApplyReason { Manual, Sync, Flat };
    bool                           ApplyColumnProfile(const hc::columns::ColumnProfile& Profile, ApplyReason Reason = ApplyReason::Manual);
    hc::columns::ColumnProfile      ColumnProfile() const;
    bool AutomaticColumns() const { return !manualColumns_; }
    void SetAutomaticColumns(bool Automatic);
    QJsonObject                    CaptureWorkspaceView() const;
    void                           RestoreWorkspaceView(const QJsonObject& View);
    QStringList                    CaptureSelection() const;
    QString                        CaptureFocus() const;
    QJsonObject CaptureScrollState() const;
    void RestoreScrollState(const QJsonObject& State);
    void                           RestoreSelection(const QStringList& Selection, const QString& Focus);
    void                           RemoveSelection(const QStringList& Selection);
    bool SetGrouping(const hc::columns::GroupingState& State);
    bool SetGroupingLevels(const QVector<hc::columns::GroupingState>& Levels);
    const QVector<hc::columns::GroupingState>& GroupingLevels() const { return groupingLevels_; }
    QString ColumnSourceText() const;
    // 평면 폴더 세트를 이 탭에 적용한다. Origin은 평면 보기를 시작한 탭의 일반 세트다. Source는 상태 표시줄의 출처 문구다.
    bool ApplyFlatProfile(const hc::columns::FlatProfile& Profile, const hc::columns::ColumnProfile& Origin, const QString& Source);
    void ClearFlatProfile();
    const std::optional<hc::columns::FlatProfile>& ActiveFlatProfile() const { return flatProfile_; }
    // 평면 세트를 적용하기 전의 일반 세트(열 출처 "시작한 탭"의 기준).
    const hc::columns::ColumnProfile& FlatOriginProfile() const { return flatOrigin_; }
    hc::columns::GroupingState Grouping() const { return grouping_; }
    bool GroupingActive() const;
    bool IsGroupFocused() const;
    void SetGroupsExpanded(bool Expanded);
    QJsonObject CaptureGroupExpansion() const { return {{"defaultExpanded", groupsExpandedByDefault_}, {"groups", groupExpansion_}}; }
    void RestoreGroupExpansion(const QJsonObject& State);
    QModelIndexList SelectionCandidates() const;
    QModelIndexList SelectedSourceIndexes() const;
    bool IsSourceSelected(const QModelIndex& Index) const;

signals:
    void                           sigViewSettingsChanged();
    void                           sigListPresentationChanged();
    void sigThumbnailSelectionChanged();
    void sigSelectionIdentityChanged();
    void sigPreviewSelectionChanged();
    // 평면 보기에서 Backspace · 썸네일의 상위 이동: 패널이 평면을 끝내고 기준 폴더로 돌아간다.
    void sigFlatExitRequested();

protected slots:
    void                            oo_grid_contextMenu( Qtitan::ContextMenuEventArgs* Args );
    void                            oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args );
    void                            oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args );
    void                            oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex );
    void                            oo_grid_editorStarting( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_editorStarted( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_editorPosting( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection );

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;
    void hideEvent(QHideEvent* Event) override;
    void showEvent(QShowEvent* Event) override;
    void                            resizeEvent( QResizeEvent* event ) override;
    bool                            nativeEvent( const QByteArray& eventType, void* message, qintptr* result ) override;

private:
    struct StartupPaint { int parts_=0; QString focus_; quint64 directory_=0, rules_=0; };
    QHash<int,StartupPaint> startupPaints_;
    bool startupInput_=false; QString startupInputFocus_; QTimer startupTraceTimer_;
    void initializeGrouping();
    void applyGrouping();
    void focusFirstEntry();
    void applySetGrouping(const hc::columns::ColumnProfile& Profile);
    void rememberGrouping();
    QHash<QUuid, QVector<hc::columns::GroupingState>> groupingBySet_;
    QList<QUuid> groupingBySetOrder_;
    hc::columns::ProfileGrouping::Expand expandPolicy_ = hc::columns::ProfileGrouping::Expand::All;
    bool hideGroupedColumn_ = false;
    void applyGroupExpansion();
    void projectGroupedSelection();
    QString sourceSelectionKey(QModelIndex Index) const;
    void showGroupingMenu(Qtitan::ContextMenuEventArgs* Args);
    hc::columns::GroupingState grouping_;
    QVector<hc::columns::GroupingState> groupingLevels_;
    int automaticColumnRule_ = -1;
    QJsonObject unsupportedGrouping_, groupExpansion_;
    QSet<QString> groupedSelection_;
    QHash<QString, QString> groupLastFocus_;
    QString groupingContext_;
    bool groupingProjection_ = false, groupingReset_ = false, userGroupCollapse_ = false;
    bool groupsExpandedByDefault_ = true, groupInteractionPending_ = false;
    QString groupCollapseFocus_, groupInteractionFocus_;
    int groupingUpdateDepth_ = 0;
    QTimer groupingTimer_, groupingDateTimer_, groupInteractionTimer_;
    void queueSelectionKey(QKeyCombination Key);
    void drainSelectionKeys();
    bool invertSelectionAlongNavigation(QKeyCombination Key);
    bool forwardingSelectionNavigation_ = false;
    void finishSelectionInput();
    void syncSelectionInputActivity();
    QList<QKeyCombination> pendingSelectionKeys_;
    CFsModelT* selectionKeyModel_ = nullptr;
    QString selectionKeyLocation_;
    bool processingSelectionKey_ = false;
    int selectionHeldKey_ = 0;
    QTimer selectionKeyTimer_;
    void resetRenameClicks();
    void selectRenameText(bool IncludeExtension);
    void positionRenameEditor();
    void commitRename(int Direction);
    void finishRename(bool Success, quint64 Generation);
    void continueRename();
    QPointer<QLineEdit> renameEditor_;
    QPersistentModelIndex renameIndex_;
    RenameHandler renameHandler_;
    QTimer renameTimer_;
    QString renameLocation_, renameOriginal_, renameNext_;
    bool renameSelectExtension_ = false, renameSubmitting_ = false, renameAwaitingRefresh_ = false, renameRetry_ = false;
    int renameDirection_ = 0;
    quint64 renameGeneration_ = 0;
    QModelIndex                     retrieveCursorIndex() const;
    void syncViewportBackground();
    void requestPresentation();
    void requestIconViewport();
    void updateIconViewport();
    QTimer iconViewportTimer_;
    void requestAutomaticColumns();
    bool manualColumns_ = false, applyingAutoColumns_ = false;
    QByteArray automaticColumnsSignature_;
    quint64 automaticColumnsGeneration_ = 0;
    std::stop_source automaticColumnsStop_;
    std::optional<hc::columns::ColumnProfile> automaticColumnsBase_;
    void applyThumbnailPresentation();
    void ensureThumbnails();
    QFont thumbnailFont_;
    fm::filelist::ThumbnailView* thumbnails_ = nullptr;
    ThumbnailModel* thumbnailModel_ = nullptr;
    QStackedWidget* presentationStack_ = nullptr;
    std::optional<int> thumbnailSizeOverride_;
    bool syncingThumbnailCursor_ = false;
    void applyPresentation();
    QJsonObject capturePresentation() const;
    void restorePresentation(const QJsonObject& View);
    QString listModeOverride_;
    bool everythingPresentation_ = false;
    bool flatPresentation_ = false;
    bool flatPrefix_ = true, flatPrefixDim_ = true, flatTree_ = false, flatTreeLines_ = true;
    // 평면 썸네일(시안 F): 폴더 구획 · 타일 아래 폴더 칩 · 구획 제목을 마지막 폴더 이름으로.
    bool flatSections_ = false, flatChips_ = false, flatLeafTitles_ = false;
    // 트리 접기 표시를 누른 위치면 그 행을 펼치거나 접는다.
    bool toggleFlatTreeAt(const QPoint& Position);
    bool flatTreeKey(int Key);
    std::optional<hc::columns::FlatProfile> flatProfile_;
    hc::columns::ColumnProfile flatOrigin_;
    QString flatSource_;
    std::optional<bool> nameBelowOverride_;
    QJsonObject unsupportedPresentation_;
    fm::filelist::ListAppearance listAppearance_;
    fm::filelist::detail::RecordGeometry recordGeometry_;
    fm::style::ThemeColors recordColors_;
    QTimer presentationTimer_;
    QTimer selectionSummaryTimer_;
    bool panelActive_ = false, applyingPresentation_ = false, restoringPresentation_ = false;
    bool autoInitialized_ = false, autoTwoLine_ = false;
    QString autoLocation_;
    Qtitan::GridRecordPainter* recordPainter_ = nullptr;
    quint64 quickFindGeneration_ = 0;
    QMetaObject::Connection quickFindPageConnection_;


    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;
    Qtitan::GridBandedTableView*    view_ = nullptr;

    QJsonObject                     appliedDisplaySettings_;
    CFsModelT*                      model_ = nullptr;
    QAbstractProxyModel*            proxy_ = nullptr;
    QtitanColumnAdapter*            columnAdapter_ = nullptr;
    QMetaObject::Connection         selectionReset_;
    QMetaObject::Connection         selectionRestore_;
    QStringList                    resetSelection_, resetFocusFallback_;
    QString                        resetFocus_, resetPath_;
    // Keep only recent positions; a long-lived tab may visit many paths.
    QCache<QString, QJsonObject>    navigationScroll_{128};
    
    // DataGrid 의 클릭 상태 추적을 위한 타이머
    QTimer                          viewClickTimer_;
    QTimer                          nowPlayingTimer_;
    QPersistentModelIndex           mouseRenameIndex_;
    QPersistentModelIndex           lastNameClickIndex_;
    QElapsedTimer                   lastNameClickTime_;
    QPoint                          dragPressPosition_;
    QStringList                     dragPaths_;
    Qt::MouseButton                 dragButton_ = Qt::NoButton;
    QPointF                         rightPressGlobalPosition_;
    bool                            rightPressActive_ = false;
    bool                            suppressMouseContextMenu_ = false;
    // 메뉴 키 · Shift+F10이 명령으로 연 메뉴가 닫힌 시각. 같은 키에서 뒤따라 온 키보드 문맥 메뉴 이벤트를 삼킨다.
    QElapsedTimer                   keyboardMenuClosed_;
    QPoint                          contextGlobal_;    // 마지막 마우스 문맥 메뉴 이벤트의 전역 위치와 수정 키
    Qt::KeyboardModifiers           contextModifiers_;
    QPoint anchorInViewport(const QModelIndex& Source, QWidget*& Viewport);
    QList<QPair<QString, std::function<void()>>> shellExtras(const QStringList& Paths, const QModelIndex& Source) const;   // ShellMenuExtras
    bool                            rightDrop_ = false;
    QPoint                          renamePressPosition_;
    bool                            plainLeftClick_ = false;
    QTimer                          columnSortTimer_;
    void                            positionColumnStatus();
    void                            showColumnStatus();
    QLabel*                         columnStatus_ = nullptr;
    // The query overlay appears only for checks still running after this delay.
    static constexpr int            ColumnStatusDelayMs = 400;
    QTimer                          columnStatusDelay_;
    QString                         columnStatusText_;
    QStringList                     querySelection_;
    QString                         queryFocus_;
    QJsonObject queryScroll_;
    int                             fileNameColumnIndex = -1;       // 추후 사용.
};

class CCardView : public CViewT
{
    Q_OBJECT
public:

private:
    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;

};

class CViewBase : public CViewT
{
    Q_OBJECT
public:
    CViewBase( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() )
        : CViewT( Parent, f )
    {}

    void                            SetFilePath( const QString& FilePath ) { filePath_ = FilePath; }
    QString                         GetFilePath() const { return filePath_; }

protected:

    QString                         filePath_;

private:
};

class CInternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CInternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

};

class CExternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CExternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::FramelessWindowHint  );
    ~CExternalViewer();

    void                            SetWLX( TySpWLX Viewer );
    bool                            Initialize();

    ///////////////////////////////////////////////////////////////////////////
    ///

    HWND                            ListLoad( HWND ParentWin, const QString& FilePath, int ShowFlags );

private:
    void                            setDefaultParams();

    static LRESULT CALLBACK         cbWndProc( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );
    static LRESULT CALLBACK         cbWndProcChild( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );

protected:

    TySpWLX                         viewer_;
    ATOM                            atom;
    QWidget*                        wParent = nullptr;
    HWND                            hParent = nullptr;

    QWidget*                        wChild = nullptr;
    HWND                            hChild = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};

class CQuickView : public CViewT
{
    Q_OBJECT
public:
    CQuickView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );
    ~CQuickView() override;

    void                            SetFilePath( const QString& FilePath );
    void                            SetContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source,
                                        std::shared_ptr<hc::storage::IWritableStorage> Writer = {});
    void                            SetInformation(const QString& Information);
    bool                            RequestClose();
    bool                            IsPinned() const;
    void                            CloseView() override;
    void                            EnsureKeyboardFocusOnView() const override;

signals:
    void                            CloseRequested();
    void                            ReturnFocusRequested();
    void                            FollowSelectionRequested();

protected:
    void                            showEvent( QShowEvent* event ) override;

private:

    bool                            isInitOnce_ = false;
    QtViewerSurface*                surface_ = nullptr;
    QString                         filePath_;
    QWidget*                        information_ = nullptr;
    QString                         informationText_;
    QAction*                        returnAction_ = nullptr;
    std::stop_source                probeStop_;
    std::uint64_t                   probeGeneration_ = 0;
    std::shared_ptr<hc::document::DocumentHandlerCatalog> handlerCatalog_;
    QByteArray handlerSettings_;
    QMap<QString, QPair<qint64, qint64>> handlerFiles_;
    void                            cancelProbe();
    void                            openContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source,
                                        std::shared_ptr<hc::storage::IWritableStorage> Writer, bool TextContent);
    void                            refreshReturnKey();
    CInternalViewer*                internalViewer_ = nullptr;
    CExternalViewer*                externalViewer_ = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};
