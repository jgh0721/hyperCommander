#pragma once

#include <QObject>
#include <QString>

// VERSION = X.Y.Z.Build

#include "UniqueLibs/productDefaults.hpp"

constexpr auto OPT_SEC_WINDOW = QLatin1String( "Window" );
constexpr auto OPT_SEC_EDITOR = QLatin1String( "Editor" );

QString                                 GetOPTValue( const TyOpts& Option );
void                                    SetOPTValue( const TyOpts& Option, const QVariant& Variant );
