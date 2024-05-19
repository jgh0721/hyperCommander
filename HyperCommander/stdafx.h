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

#include "cmnIncludeLibrary.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>
