#pragma once

#include "ui_cmpPanel.h"

class FSProxyModel;
class FSModel;

namespace Qtitan
{
    class GridBandedTableView;
}

namespace Qtitan
{
    class GridViewBase;
}

namespace Qtitan
{
    class ContextMenuEventArgs;
}

class CmpPanel : public QWidget
{
    Q_OBJECT
public:
    CmpPanel( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    Q_INVOKABLE void                    AddTab();
    Q_INVOKABLE void                    CloseTab();
    Q_INVOKABLE void                    RefreshVolumeList();
    int                                 CurrentTabIndex() const;
    Q_INVOKABLE void                    SetFocusView( int TabIndex );

    int                                 InitializeGrid();

public slots:

    void                                oo_ChangedDirectory( const QString& CurrentPath );

    void                                on_cbxVolume_currentIndexChanged( int index );
    void                                on_tabWidget_currentChanged( int Index );
    void                                oo_grdLocal_contextMenu( Qtitan::ContextMenuEventArgs* Args );

signals:

    void                                sig_NotifyCurrentDirectory( const QString& CurrentPath );

private:

    bool                                eventFilter( QObject* Object, QEvent* Event ) override;
    void                                focusInEvent(QFocusEvent* event) override;

    struct TyTabState
    {
        int                             ViewMode = 0;   // Normal FS, Packer, VFS
        Qtitan::GridBandedTableView*    View = nullptr;
        FSModel*                        Model = nullptr;
        FSProxyModel*                   ProxyModel = nullptr;
    };

    // 현재 작업 중인 탭 색인 번호
    int                                 currentIndex = -1;
    QMap< int, TyTabState >             mapTabToStats;

    Ui::cmpPanel                        ui;

};
