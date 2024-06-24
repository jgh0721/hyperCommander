#pragma once

#include "UniqueLibs/solTCPluginMgr.hpp"

#include "ui_cmpViewer.h"

class QDlgViewer;

class CmpViewer : public QWidget
{
    Q_OBJECT
public:
    CmpViewer( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );
    ~CmpViewer();

    void                                SetWLX( TySpWLX view );
    void                                SetLoadToFile( const QString& FilePath );

protected: 
    void                                closeEvent( QCloseEvent* Ev ) override;

private:
    friend class QDlgViewer;

    Q_INVOKABLE void                    initialize();
    void                                cleanup();

    TySpWLX                             viewer_;
    QString                             currentFilePath;

    QVBoxLayout*                        ly_ = nullptr;
    QWidget*                            wlx_ = nullptr;
    Ui::cmpViewer                       ui;
};