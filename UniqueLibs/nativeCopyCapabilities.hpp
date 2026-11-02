#pragma once
#include "nativeStorageRevision.hpp"

namespace hc::storage::native
{
    inline IStorageProvider::CopyExtras CopyExtrasAt(const std::wstring& Path)
    {
        // RDP transports file contents and basic attributes, not Windows named
        // streams or persistent ACLs, regardless of the client's filesystem.
        if (IsRedirectedPath(Path)) return {};
        wchar_t Root[32768]{}; DWORD Flags = 0;
        if (!GetVolumePathNameW(Path.c_str(), Root, DWORD(std::size(Root)))
            || !GetVolumeInformationW(Root, nullptr, 0, nullptr, nullptr, &Flags, nullptr, 0)) return {};
        // Unknown optional capabilities are conservative. An access/read/write
        // failure in a supported operation still propagates normally.
        return {(Flags & FILE_PERSISTENT_ACLS) != 0, (Flags & FILE_NAMED_STREAMS) != 0};
    }
}
