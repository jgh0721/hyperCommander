#pragma once

#include "commonLibs/storage/storage.hpp"
#include <qt_windows.h>
#include <winnetwk.h>
#include <shlobj.h>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "mpr")
#pragma comment(lib, "shell32")

namespace hc::storage::native
{
    // One recovery budget for the directory probe and the first enumeration.
    // Never disconnect a mapping: it may be in use by another tab or process.
    class MappedDriveRecovery
    {
    public:
        using MappingQuery = std::function<bool(const std::wstring&)>;
        using Restore = std::function<void(const std::wstring&)>;

        explicit MappedDriveRecovery(std::wstring Path,
            MappingQuery Query = isMapped, Restore Prepare = restore)
            : query_(std::move(Query)), prepare_(std::move(Prepare))
        {
            if (Path.starts_with(L"\\\\?\\")) Path.erase(0, 4);
            if (Path.size() >= 3 && ((Path[0] >= L'A' && Path[0] <= L'Z')
                || (Path[0] >= L'a' && Path[0] <= L'z')) && Path[1] == L':'
                && (Path[2] == L'\\' || Path[2] == L'/'))
                root_ = Path.substr(0, 2) + L"\\";
        }

        template<class Probe, class Failed>
        auto Invoke(Probe Work, Failed IsFailure, const OperationContext& Context)
        {
            auto Value = Work();
            if (!IsFailure(Value)) return Value;
            const auto Code = GetLastError();
            if (attempted_ || root_.empty() || !connectionFailure(Code) || Context.Check({})) {
                SetLastError(Code); return Value;
            }
            attempted_ = true;
            if (!query_(root_.substr(0, 2)) || Context.Check({})) {
                SetLastError(Code); return Value;
            }
            // Null owner and no DIRCREATE flags: no dialogs or directory writes.
            // The shell restores the existing mapping using Windows credentials.
            prepare_(root_);
            if (Context.Check({})) { SetLastError(Code); return Value; }
            return Work();
        }

    private:
        static bool connectionFailure(DWORD Code)
        {
            switch (Code) {
            case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
            case ERROR_BAD_NETPATH: case ERROR_NETWORK_BUSY: case ERROR_DEV_NOT_EXIST:
            case ERROR_UNEXP_NET_ERR: case ERROR_NETNAME_DELETED: case ERROR_BAD_NET_NAME:
            case ERROR_SEM_TIMEOUT: case ERROR_CONNECTION_UNAVAIL: case ERROR_NO_NETWORK:
            case ERROR_NETWORK_UNREACHABLE: case ERROR_HOST_UNREACHABLE:
            case ERROR_CONNECTION_ABORTED: case ERROR_NOT_CONNECTED:
                return true;
            default: return false;
            }
        }
        static bool isMapped(const std::wstring& Device)
        {
            DWORD Length = 512;
            std::vector<wchar_t> Remote(Length);
            auto Code = WNetGetConnectionW(Device.c_str(), Remote.data(), &Length);
            if (Code == ERROR_MORE_DATA && Length <= 32768) {
                Remote.resize(Length);
                Code = WNetGetConnectionW(Device.c_str(), Remote.data(), &Length);
            }
            return Code == NO_ERROR || Code == ERROR_CONNECTION_UNAVAIL;
        }
        static void restore(const std::wstring& Root)
        {
            const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            SHPathPrepareForWriteW(nullptr, nullptr, Root.c_str(), 0);
            if (SUCCEEDED(Initialized)) CoUninitialize();
        }

        std::wstring root_;
        MappingQuery query_;
        Restore prepare_;
        bool attempted_ = false;
    };
}
