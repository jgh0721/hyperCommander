#pragma once
#include "commonLibs/storage/storage.hpp"
#include <Windows.h>

namespace hc::storage::native
{
    inline std::uint64_t FileTimeTicks(FILETIME Time)
    {
        return (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
    }
    inline ResourceRevision ListingRevision(const WIN32_FILE_ATTRIBUTE_DATA& Data)
    {
        // SMB may synthesize different creation times for attribute-only and
        // read handles. Creation time is display metadata, not a content version.
        return {"file:" + std::to_string(FileTimeTicks(Data.ftLastWriteTime))
            + ":" + std::to_string(Data.nFileSizeHigh) + ":" + std::to_string(Data.nFileSizeLow)};
    }
    inline ResourceRevision HandleRevision(const BY_HANDLE_FILE_INFORMATION& Info)
    {
        const auto Identity = std::to_string(Info.dwVolumeSerialNumber) + ":"
            + std::to_string(Info.nFileIndexHigh) + ":" + std::to_string(Info.nFileIndexLow);
        // SMB servers may synthesize creation time differently between handles.
        // A nonzero file ID detects replacement without rejecting child deletion.
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (Info.nFileIndexHigh || Info.nFileIndexLow) return {"directory:identity:" + Identity};
            // Retain the conservative fallback when the server supplies no ID.
            return {"directory:" + std::to_string(FileTimeTicks(Info.ftCreationTime)) + ":" + Identity};
        }
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        // Fresh snapshots additionally detect replacement by a different file
        // even when its size and modification time were deliberately preserved.
        return {ListingRevision(Data).token_ + ":identity:" + Identity};
    }
    inline ResourceRevision MetadataRevision(ResourceRevision Revision)
    {
        // RDP redirected files can report a synthetic file ID per opened handle.
        // Callers which bind document bytes by hash may retain the exact mtime
        // and size token without treating that synthetic ID as persistent.
        if(Revision.token_.starts_with("file:")){
            const auto At=Revision.token_.find(":identity:");
            if(At!=std::string::npos)Revision.token_.resize(At);
        }
        return Revision;
    }
    inline bool MatchesRevision(const ResourceRevision& Expected, const BY_HANDLE_FILE_INFORMATION& Info)
    {
        if (Expected == HandleRevision(Info)) return true;
        if (Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        const WIN32_FILE_ATTRIBUTE_DATA Data{Info.dwFileAttributes, Info.ftCreationTime,
            Info.ftLastAccessTime, Info.ftLastWriteTime, Info.nFileSizeHigh, Info.nFileSizeLow};
        // Cheap directory listings have no file ID. Accept only the exact
        // metadata-only token; Stat then upgrades it to a handle revision.
        return Expected == ListingRevision(Data);
    }
}
