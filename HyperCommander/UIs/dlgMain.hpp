#pragma once

#include "ui_dlgMain.h"

class QMainUI : public QMainWindow
{
    Q_OBJECT
public:
    QMainUI( QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );

    Q_INVOKABLE void                    Tab_SwitchToAnother();

    Q_INVOKABLE void                    SelectRowOnCurrentPane( const QModelIndex& SrcIndex, bool IsMoveDown );
    
protected slots:

    void                                on_acShowMainOpts_triggered( bool checked = false );

    void                                oo_notifyCurrentDirectory( const QString& CurrentPath );

private:
    Q_INVOKABLE void                    initialize();

    CmpPanel*                           currentFocusPanel() const;
    int                                 currentPanelIndex = 0; // 0 = Left, 1 = Right
    Ui::dlgMain                         ui;
}; 