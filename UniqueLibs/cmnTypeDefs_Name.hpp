#pragma once

#include <QString>

constexpr auto MAIN_CONFIG_FILENAME = QLatin1String( "HyperCommander.ini" );

constexpr auto TC_COMPAT_ENV_CMDPATH = L"COMMANDER_PATH";

constexpr auto DEFAULT_FS_DIR       = QLatin1String( "%COMMANDER_PATH%\\Addons\\WFX" );
constexpr auto DEFAULT_PACKER_DIR   = QLatin1String( "%COMMANDER_PATH%\\Addons\\WCX" );
constexpr auto DEFAULT_LISTER_DIR   = QLatin1String( "%COMMANDER_PATH%\\Addons\\WLX" );

constexpr auto NTPATH               = QLatin1String( R"(\\?\)" );
constexpr auto NTPATH_UNC           = QLatin1String( R"(\\?\UNC\)" );
constexpr auto VFSPATH              = QLatin1String( R"(\\\)" );
