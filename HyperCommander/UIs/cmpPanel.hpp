#pragma once

#include "ui_cmpPanel.h"

namespace Qtitan
{
    class GridSelection;
}

class QMainUI;
class FSProxyModel;
class FSModel;

namespace Qtitan
{
    class GridViewBase;
    class GridBandedTableView;
    class ContextMenuEventArgs;
    class CellClickEventArgs;
    class RowClickEventArgs;
    class EditorEventArgs;
}

class CmpPanel : public QWidget
{
    Q_OBJECT

    struct TyTabState
    {
        int                             ViewMode = 0;   // Normal FS, Packer, VFS
        Qtitan::GridBandedTableView*    View = nullptr;
        FSModel*                        Model = nullptr;
        FSProxyModel*                   ProxyModel = nullptr;

        int                             SelectedFileCount = 0;
        int                             SelectedDirectoryCount = 0;
        QVector< QModelIndex >          SelectedIndex;      // 데이터를 획득하려면 ProxyModel 을 통해 SourceIndex 로 변환해야한다.
        int                             LastFocusedRowIndex = -1;
    };

    using TySpTabState = std::shared_ptr<TyTabState>;

public:
    CmpPanel( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    Q_INVOKABLE void                    Initialize();

    Q_INVOKABLE void                    AddTab();
    Q_INVOKABLE void                    PrevTab();
    Q_INVOKABLE void                    NextTab();
    Q_INVOKABLE void                    CloseTab();
    Q_INVOKABLE void                    RefreshVolumeList();
    int                                 CurrentTabIndex() const;
    Q_INVOKABLE void                    SetFocusView( int TabIndex );
    Qtitan::GridBandedTableView*        GetFocusView() const;
    void                                EnsureKeyboardFocusOnView( Qtitan::GridBandedTableView* View );

    Q_INVOKABLE void                    SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown );
    void                                ReturnOnCurrentTab( const QModelIndex& SrcIndex );
    void                                NewFolderOnCurrentTab( const QModelIndex& SrcIndex );

    void                                RefreshSource( int TabIndex );
    void                                FileCopyToOtherPanel( CmpPanel* Dst );
    void                                FileDeleteOnCurrentTab( const QModelIndex& SrcIndex );
    void                                FileNormalization( const QModelIndex& SrcIndex );
    Q_INVOKABLE void                    RenameFileName( const QModelIndex& SrcIndex );
    void                                ContextMenuOnCurrentTab( const QModelIndex& SrcIndex );
    void                                ExternalEditorMenu( const QModelIndex& SrcIndex );

    void                                ViewOnLister( const QModelIndex& SrcIndex );

    int                                 InitializeGrid();

public slots:

    void                                oo_ChangedDirectory( const QString& CurrentPath );

    void                                on_cbxVolume_currentIndexChanged( int index );
    void                                on_btnGridStyle_clicked( bool checked = false );
    void                                on_tabWidget_currentChanged( int Index );
    void                                oo_grdLocal_contextMenu( Qtitan::ContextMenuEventArgs* Args );
    void                                oo_grdLocal_cellClicked( Qtitan::CellClickEventArgs* Args );
    void                                oo_grdLocal_rowDblClicked( Qtitan::RowClickEventArgs* Args );
    void                                oo_grdLocal_rowFocusChanged( int OldRowIndex, int NewRowIndex );
    void                                oo_grdLocal_editorStarted( Qtitan::EditorEventArgs* Args );
    void                                oo_grdLocal_editorPosting( Qtitan::EditorEventArgs* Args );
    void                                oo_grdLocal_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection );

signals:

    void                                sig_NotifyCurrentDirectory( const QString& CurrentPath );
    void                                sig_NotifyPanelActivated();

private:
    friend class QMainUI;

    bool                                eventFilter( QObject* Object, QEvent* Event ) override;
    void                                resizeEvent( QResizeEvent* event ) override;

    int                                 retrieveCurrentIndex() const;
    TySpTabState                        retrieveFocusState();
    Qtitan::GridBandedTableView*        retrieveFocusView() const;
    QModelIndex                         retrieveFocusViewCursorIndex() const;
    // 현재 마우스 커서의 전역 위치( GetCursorPos 를 통해 획득 ) 와 모델 색인을 통해
    // 실제 메뉴를 표시할 위치를 계산하여 전역 위치값으로 반환한다. 
    QPoint                              retrieveMenuPoint( const QPoint& GlobalCursor, QModelIndex SrcIndex );

    void                                processVolumeStatusText( QChar Drive );
    void                                processPanelStatusText();
    
    // 벡터의 색인은 tabWidget 의 색인과 동기화 되어야 한다. 
    QVector< TySpTabState >             vecTabStates;
    QTimer                              viewClickTimer;

    Ui::cmpPanel                        ui;
};
