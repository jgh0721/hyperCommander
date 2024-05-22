#pragma once

#include <QtWidgets/QDialog>

#include "cmnConcurrent.hpp"


#include "cmnTypeDefs_Name.hpp"

class QSHChangeNotify;

class HyperCommanderApp : public QApplication
{
    Q_OBJECT
public:
    HyperCommanderApp( int& argc, char** argv );

    Q_INVOKABLE void                    ShowMultiRename( const QVector< QString >& VecFiles );

protected slots:

    void                                OnDriveAdd( const QString& Root );
    void                                OnDriveRemoved( const QString& Root );
    void                                OnMediaInserted( const QString& ItemIDDisplayName );
    void                                OnMediaRemoved( const QString& Root );

private:

    Q_INVOKABLE void                    initialize();

    QSHChangeNotify*                    shlChangeNotify = nullptr;
};
