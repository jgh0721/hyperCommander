#pragma once

#include <QtCore>

#include "builtInFsModel.hpp"
#include "cmnTypeDefs.hpp"

#include "ui_cmpPanel.h"
#include "Components/cmpView.hpp"
#include "Modules/mdlFileSystem.hpp"

class CFsModelT;
class QMainUI;
class CFSProxyModel;
class CFSModel;
class CViewT;

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
        CFsModelT*                  Model = nullptr;
        CFSProxyModel*              ProxyModel = nullptr;

        quint64                     SelectedSize = 0;
        int                         SelectedFileCount = 0;
        int                         SelectedDirectoryCount = 0;

        CViewT::TyEnViewMode        GetViewMode() const { return QuickView != nullptr ? QuickView->GetViewMode() : View->GetViewMode(); }
        QModelIndex                 ConvertToSrcIndex( const QModelIndex& Index ) const { if( ProxyModel != nullptr ) return ProxyModel->mapToSource( Index ); return Index; }

// QVector< QModelIndex >      SelectedRowIndex;      // 데이터를 획득하려면 ProxyModel 을 통해 SourceIndex 로 변환해야한다.
//        //int                             LastFocusedRowIndex = -1;
    };

    using TySpTabState = std::shared_ptr<TyTabState>;

public:
    CmpPanel( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    Q_INVOKABLE void                RefreshVolumeList() const;

    void                            AddTab();
    void                            PrevTab();
    void                            NextTab();
    void                            CloseTab();
    int                             CurrentTabIndex() const;
    void                            SetFocusView( int TabIndex );
    CViewT*                         GetFocusView( int TabIndex ) const;

    void                            OpenQuickView( int TabIndex, const QString& FilePath );
    void                            CloseQuickView( int TabIndex );

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

    void                            on_cbxVolume_currentIndexChanged( int index, const QString& Path = "" );
    void                            on_tabWidget_currentChanged( int Index );

signals:
    void                            sig_NotifyPanelActivated();
    void                            sig_NotifyCurrentDirectory( const QString& CurrentPath );

private:
    friend class QMainUI;

    bool                            eventFilter( QObject* Object, QEvent* Event ) override;
    void                            resizeEvent( QResizeEvent* event ) override;

    void                            initializeUIs();

    CViewT*                         createGridView();
    int                             retrieveCurrentIndex() const;
    TySpTabState                    retrieveFocusState();
    CViewT*                         retrieveFocusView() const;

    // 현재 마우스 커서의 전역 위치( GetCursorPos 를 통해 획득 ) 와 모델 색인을 통해
    // 실제 메뉴를 표시할 위치를 계산하여 전역 위치값으로 반환한다. 
    QPoint                          retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex );
    QVector< QModelIndex >          makeSrcModel( const TySpTabState& SrcState );

    Q_INVOKABLE void                processVolumeStatusText( const nsHC::TySpFileSystem& SpFileSystem ) const;
    Q_INVOKABLE void                processPanelStatusText();

    Ui::cmpPanel                    ui;

    // 벡터의 색인은 tabWidget 의 색인과 동기화 되어야 한다. 
    QVector< TySpTabState >         vecTabStates;
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
//    //void                                NewFolderOnCurrentTab( const QModelIndex& SrcIndex );
//
//    //void                                FileCopyToOtherPanel( CmpPanel* Dst );
//    //void                                FileDeleteOnCurrentTab( const QModelIndex& SrcIndex );
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
