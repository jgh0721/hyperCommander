#pragma once
#include "nativeFilePath.hpp"
#include <Windows.h>

namespace hc::files
{
    // Remove only the origin marker, never the file body or unrelated streams.
    // No recursion: selecting a folder does not authorize changing its children.
    inline DWORD RemoveInternetBlock(const QString& Path, bool& Changed)
    {
        Changed = false;
        const auto Native = hc::storage::native::FilePath(Path).toStdWString();
        const auto Attributes = GetFileAttributesW(Native.c_str());
        if (Attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
        if (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
        const auto Stream = Native + L":Zone.Identifier";
        if (DeleteFileW(Stream.c_str())) { Changed = true; return ERROR_SUCCESS; }
        const auto Error = GetLastError();
        return Error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : Error;
    }
}
