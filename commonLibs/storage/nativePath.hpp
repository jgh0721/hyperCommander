#pragma once

#include <algorithm>
#include <string>

namespace hc::storage::native
{
    // For validated absolute file paths at Win32 file-API boundaries only.
    // Keep logical/display paths and Shell/plugin ABI inputs separate.
    inline std::wstring ExtendedPath(std::wstring Path)
    {
        std::replace(Path.begin(), Path.end(), L'/', L'\\');
        if (Path.starts_with(L"\\\\?\\") || Path.starts_with(L"\\\\.\\")) return Path;
        if (Path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + Path.substr(2);
        if (Path.size() >= 3 && Path[1] == L':' && Path[2] == L'\\') return L"\\\\?\\" + Path;
        return Path; // Relative paths retain the caller's normal Win32 semantics.
    }
}
