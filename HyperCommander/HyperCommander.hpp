#pragma once

#include <QtWidgets/QDialog>

#include "cmnConcurrent.hpp"


#include "cmnTypeDefs_Name.hpp"

class QMainUI;
class QSHChangeNotify;
namespace hc::settings { class SingleInstance; }

class HyperCommanderApp : public QApplication
{
    Q_OBJECT
public:
    HyperCommanderApp( int& argc, char** argv );

    bool ShouldRun() const { return shouldRun_; }
    int StartupExitCode() const { return startupExitCode_; }

    Q_INVOKABLE void                    ShowMultiRename( const QVector< QString >& VecFiles );

private:

    Q_INVOKABLE void                    initialize();

    QMainUI*                            mainUI = nullptr;
    hc::settings::SingleInstance* singleInstance_ = nullptr;
    bool shouldRun_ = true;
    int startupExitCode_ = 0;
};
