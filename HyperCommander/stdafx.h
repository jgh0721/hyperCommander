#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define NTDDI_VERSION 0x06010000
#include <tchar.h>
#include <Windows.h>
#include <shellapi.h>

#include <QtWidgets>

#define USE_OS_NATIVE_IMPLEMENTATION
#define USE_QT_SUPPORT

#ifndef FlagOn
#define FlagOn(_F,_SF)        ((_F) & (_SF))
#endif

#ifndef BooleanFlagOn
#define BooleanFlagOn(F,SF)   ((BOOLEAN)(((F) & (SF)) != 0))
#endif

#ifndef SetFlag
#define SetFlag(_F,_SF)       ((_F) |= (_SF))
#endif

#ifndef ClearFlag
#define ClearFlag(_F,_SF)     ((_F) &= ~(_SF))
#endif

#include "commonLibs/cmnSystemDetection.hpp"
#include "commonLibs/cmnCompilerDetection.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>
