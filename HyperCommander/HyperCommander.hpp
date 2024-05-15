#pragma once

#include <QtWidgets/QDialog>

#include "cmnConcurrent.hpp"

class QSHChangeNotify;

class HyperCommanderApp : public QApplication
{
public:
    HyperCommanderApp( int& argc, char** argv );


private:


    QSHChangeNotify*                    shlChangeNotify = nullptr;
};

using TyStSettings = nsCmn::nsConcurrent::TSingleton< QSettings >;
