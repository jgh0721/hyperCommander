#pragma once

#include <QtWidgets/QDialog>

#include "cmnConcurrent.hpp"


#include "cmnTypeDefs_Name.hpp"

class QMainUI;
class QSHChangeNotify;

class HyperCommanderApp : public QApplication
{
    Q_OBJECT
public:
    HyperCommanderApp( int& argc, char** argv );

    Q_INVOKABLE void                    ShowMultiRename( const QVector< QString >& VecFiles );

private:

    Q_INVOKABLE void                    initialize();

    QMainUI*                            mainUI = nullptr;
};
