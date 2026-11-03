#pragma once
#include "nativeStorageRevision.hpp"

namespace hc::storage::native
{
    inline IStorageProvider::CopyExtras CopyExtrasAt(const std::wstring& Path, const OperationContext& Context = {})
    {
        // RDP transports file contents and basic attributes, not Windows named
        // streams or persistent ACLs, regardless of the client's filesystem.
        if (IsRedirectedPath(Path)) return {};
        wchar_t Root[32768]{}; DWORD Flags = 0;
        if (!GetVolumePathNameW(Path.c_str(), Root, DWORD(std::size(Root)))) return {};
        // Source planning, destination staging and extra-stream setup query the
        // same volume repeatedly. Cache fixed-local capabilities only for this
        // execution scope; weak ownership never extends path-lock lifetimes.
        // Network/redirected paths keep querying their endpoint. Capability reuse
        // does not replace the later handle/identity or actual I/O checks.
        static thread_local std::weak_ptr<MutationScope> Scope;
        static thread_local std::map<std::wstring, IStorageProvider::CopyExtras> Volumes;
        const auto Owner = Scope.lock();
        if (!Context.mutationScope_ || Owner != Context.mutationScope_) {
            Volumes.clear(); Scope = Context.mutationScope_;
        }
        if (Context.mutationScope_) {
            if (const auto Found = Volumes.find(Root); Found != Volumes.end()) return Found->second;
        }
        if (!GetVolumeInformationW(Root, nullptr, 0, nullptr, nullptr, &Flags, nullptr, 0)) return {};
        const IStorageProvider::CopyExtras Extras{(Flags & FILE_PERSISTENT_ACLS) != 0, (Flags & FILE_NAMED_STREAMS) != 0};
        if (Context.mutationScope_ && GetDriveTypeW(Root) == DRIVE_FIXED) Volumes.emplace(Root, Extras);
        // Unknown optional capabilities are conservative. An access/read/write
        // failure in a supported operation still propagates normally.
        return {(Flags & FILE_PERSISTENT_ACLS) != 0, (Flags & FILE_NAMED_STREAMS) != 0};
    }
}
