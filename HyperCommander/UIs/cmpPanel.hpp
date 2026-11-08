#pragma once

#include <QtCore>

#include "builtInFsModel.hpp"
#include "cmnTypeDefs.hpp"

#include "ui_cmpPanel.h"
#include "Components/cmpView.hpp"
#include "Modules/mdlFileSystem.hpp"

namespace fm::ui { class PanelTabStrip; class BreadcrumbBar; class DriveButton; class SegmentedControl; class PanelStatusBar; class ProgressBar; }
class CFsModelT;
class QMainUI;
class CFSProxyModel;
class CFSModel;
class CViewT;
class CSystemView;
namespace hc::ui { class PanelPathBar; }

namespace Qtitan
{
    class Grid;
    class GridViewBase;
    class GridSelection;
    class GridBandedTableView;
    class ContextMenuEventArgs;
    class CellClickEventArgs;
    class RowClickEventArgs;
    class EditorEventArgs;
}

/*!
 * 
 *
 * 초기화 순서
 * 1. UI 초기화, 컨트롤 설정
 * 2. UI 에 색상 구성표 적용
 * 3. 패널에 기본 탭 추가( GridView )
 * 4. 비동기로 각 탭마다 새로고침 시도
 *      Set CurrentVolume On cbxVolume
 *          -> 헤더의 VolumeInfo 갱신
 *          -> View 에서 Model 을 획득 후 경로 등 설정 후 새로고침
 *              -> Model 에서 새로고침 후 디렉토리 변경 통지를 받아 DirectoryInfo 갱신
 *
 */

class CmpPanel : public QWidget
{
    Q_OBJECT

    struct TyTabState
    {
        int                         Index = -1;
        CViewT*                     View = nullptr;
        CViewT*                     QuickView = nullptr;
        // 프로세스·서비스 보기. 파일 보기(View)와 모델은 숨긴 채 유지하고 탭 페이지만 덮는다.
        CViewT*                     SystemView = nullptr;
        QString                     revealName_;
        CFsModelT*                  Model = nullptr;
        CFSProxyModel*              ProxyModel = nullptr;
        QString                    navigationStatus_, navigationTarget_;

        quint64                     SelectedSize = 0;
        int                         SelectedFileCount = 0;
        int                         SelectedDirectoryCount = 0;
        quint64                     footerTotalSize_ = 0;
        int                         footerFileCount_ = 0, footerDirectoryCount_ = 0;
        bool                        footerValid_ = false, automaticSnapshot_ = false;
        hc::workspace::TabState     workspace_;
        std::optional<hc::workspace::TabState> searchOrigin_;
        QJsonObject searchScroll_, searchGroups_;
        bool restoreSearchGroups_ = false;
        bool                       pending_ = false, restoreSelection_ = false;

        CViewT::TyEnViewMode        GetViewMode() const { return QuickView != nullptr ? QuickView->GetViewMode() : SystemView ? SystemView->GetViewMode() : View ? View->GetViewMode() : CViewT::VM_GRID; }
        QModelIndex                 ConvertToSrcIndex( const QModelIndex& Index ) const { if( ProxyModel != nullptr ) return ProxyModel->mapToSource( Index ); return Index; }

// QVector< QModelIndex >      SelectedRowIndex;      // 데이터를 획득하려면 ProxyModel 을 통해 SourceIndex 로 변환해야한다.
//        //int                             LastFocusedRowIndex = -1;
    };

    using TySpTabState = std::shared_ptr<TyTabState>;

public:
    CmpPanel( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    Q_INVOKABLE void                RefreshVolumeList() const;
    void                            NotifyDriveRemoved(const QString& Root);

    void                            AddTab(bool Navigate = true);
    hc::workspace::PaneState        CaptureWorkspace(int OnlyTab = -1) const;
    void                            RestoreWorkspace(const hc::workspace::PaneState& Pane, const QJsonObject& Defaults = {});
    QString                         CurrentPath() const;
    QString                         Search() const;
    QString                         SearchMatch() const;
    void                            SetSearch(const QString& Query, const QString& Match = {});
    void StartEverythingSearch(const QString& Query, const QString& Match, int Interval);
    void StopEverythingSearch(bool Restore = true);
    void StopEverythingSearchForModel(CFsModelT* Model);
private:
    void stopEverythingSearch(int Index, bool Restore);
public:
    void OpenSearchLocation();
    // Folder로 이동해 선택 키 Key 항목에 커서를 둔다. 이미 그 폴더면 커서만 옮긴다.
    void RevealItem( const QString& Folder, const QString& Key );
    void                            NavigateHistory(int Offset);
    bool                            CanNavigateHistory(int Offset) const;
    void                            PrevTab();
    void                            NextTab();
    void                            CloseTab();
    int                             CurrentTabIndex() const;
    void                            SetFocusView( int TabIndex );
    CViewT*                         GetFocusView( int TabIndex ) const;

    void                            OpenQuickView( int TabIndex, const QString& FilePath );
    bool                            CloseQuickView( int TabIndex );
    // 반대 패널의 시스템 보기를 따라가는 상세 보기를 빠른 보기 자리에 연다.
    void                            OpenDetailView( int TabIndex, CSystemView* Source );

    // 현재 탭을 프로세스(0)·서비스(1) 보기로 덮는다. 이미 같은 보기면 그대로 돌려준다.
    CSystemView*                    ShowSystemView( int Kind );
    void                            HideSystemView( int TabIndex = -1 );
    CSystemView*                    SystemView( int TabIndex = -1 ) const;
    int                             FindSystemTab( int Kind ) const;
    // 위치 메뉴(드라이브·시스템 보기)를 드라이브 단추 아래에 연다.
    void                            ShowLocationMenu();
    // 파일이 있는 폴더로 이동해 그 파일에 커서를 둔다. 시스템 보기는 걷는다.
    void                            RevealFile( const QString& Path );

    void                            RefreshSource( int TabIndex );
    void                            SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown );
    void                            ReturnOnCurrentTab( const QModelIndex& SrcIndex );
    void                            ContextMenuOnCurrentTab( const QModelIndex& SrcIndex );
    void                            ExternalEditorMenu( const QModelIndex& SrcIndex );
    void                            ChangeVolume( const QString& Drive );
    void                            ChangeCurrentPath( const QString& Path, const QString& Parameter );
public slots:

    void                            OnColorSchemeChanged( const TyColorScheme& ColorScheme );
    void                            OnChangedDirectory( const QString& CurrentPath );

    void                            changeVolumeIndex( int index, const QString& Path = "" );
    void                            on_tabWidget_currentChanged( int Index );

signals:
    void                            sig_NotifyPanelActivated();
    void                            sigViewSettingsChanged();
    void                            sigNavigationStateChanged();
    void                            sig_NotifyCurrentDirectory( const QString& CurrentPath );

private:
    friend class QMainUI;
    void                            setActivePanel(bool Active);
    void                            updateActiveAppearance();

    bool                            eventFilter( QObject* Object, QEvent* Event ) override;
    void                            resizeEvent( QResizeEvent* event ) override;

    void                            initializeUIs();
    void                            syncPanelChrome();
    void                            closeTabAt(int Index);
    void                            navigateHistoryIndex(int Index);
    void                            restoreTab(int Index);
    void                            materializeTab(int Index);
    QJsonObject                     workspaceDefaults_;
    void                            updateWorkspaceStatus();
    void                            updateNavigationProgress();

    CViewT*                         createGridView();
    // 탭 페이지 위젯을 바꾼다(제목·도움말 유지, 현재 탭 유지).
    void                            replacePage( int Index, QWidget* Page );
    void                            applySystemTitle( int Index );
    void                            restoreFileTitle( int Index );
    void                            restoreSystemView( int Index );
    void                            finishReveal( const TySpTabState& State );
    int                             retrieveCurrentIndex() const;
    TySpTabState                    retrieveFocusState();
    CViewT*                         retrieveFocusView() const;

    // 현재 마우스 커서의 전역 위치( GetCursorPos 를 통해 획득 ) 와 모델 색인을 통해
    // 실제 메뉴를 표시할 위치를 계산하여 전역 위치값으로 반환한다. 
    QPoint                          retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex );
    QVector< QModelIndex >          makeSrcModel( const TySpTabState& SrcState );

    Q_INVOKABLE void                processVolumeStatusText( const nsHC::TySpFileSystem& SpFileSystem );
    Q_INVOKABLE void                processPanelStatusText();

    Ui::cmpPanel                    ui;

    // 벡터의 색인은 tabWidget 의 색인과 동기화 되어야 한다. 
    QVector< TySpTabState >         vecTabStates;
    bool                            workspaceRestored_ = false;
    fm::ui::PanelTabStrip*           tabs_ = nullptr;
    hc::ui::PanelPathBar*            path_ = nullptr;
    fm::ui::ProgressBar*             navigationProgress_ = nullptr;
    fm::ui::DriveButton*             drive_ = nullptr;
    fm::ui::SegmentedControl*        viewMode_ = nullptr;
    fm::ui::PanelStatusBar*          status_ = nullptr;
    QToolButton*                    historyButton_ = nullptr;
    quint64                         capacityGeneration_ = 0;
    QLineEdit*                      search_ = nullptr;
    QLabel*                         workspaceStatus_ = nullptr;
    QPushButton*                    retry_ = nullptr;
    QComboBox*                      history_ = nullptr;
    QLabel*                        activeIndicator_ = nullptr;
    bool                           activePanel_ = false;
    bool                           darkTheme_ = false;
    // 위치 메뉴의 드라이브 여유 / 전체 용량(루트별 마지막 값). 메뉴를 열 때 먼저 보이고 백그라운드로 다시 잰다.
    QHash<QString, QString>        capacityText_;
    bool                           restoringSystem_ = false;   // 작업공간 복원 중에는 패널 활성화를 바꾸지 않는다
};

//class CmpPanel : public QWidget
//{
//    Q_OBJECT
//
//public:
//    CmpPanel( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );
//
//    Q_INVOKABLE void                    Initialize();
//
//    //Qtitan::GridBandedTableView*        GetFocusView() const;
//
//
//    //void                                FileCopyToOtherPanel( CmpPanel* Dst );
//    //void                                FileNormalization( const QModelIndex& SrcIndex );
//    //void                                FileSetAttrib( const QModelIndex& SrcIndex );
//    //Q_INVOKABLE void                    RenameFileName( const QModelIndex& SrcIndex );
//
//    //void                                ViewOnLister( const QModelIndex& SrcIndex );
//
//    //int                                 InitializeGrid();
//
//public slots:
////
////    void                                on_btnGridStyle_clicked( bool checked = false );
//private:
//
//    void                                initializeVolumeList();
//    Qtitan::GridBandedTableView*        initializeGrid( Qtitan::Grid* Grid );
//
////
//
//    Ui::cmpPanel                        ui;
//};
