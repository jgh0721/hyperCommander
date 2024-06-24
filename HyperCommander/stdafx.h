#pragma once

#define QT_SHAREDPOINTER_TRACK_POINTERS

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <tchar.h>
#include <Windows.h>
#include <shellapi.h>
#include <ShlObj.h>

#include <QtWidgets>

#define USE_OS_NATIVE_IMPLEMENTATION
#define USE_QT_SUPPORT

#include "cmnIncludeLibrary.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include "UniqueLibs/solMultiLang_Defs.hpp"
