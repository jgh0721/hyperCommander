#pragma once

#include "commonLibs/storage/storage.hpp"
#include <qt_windows.h>
#include <winnetwk.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QStringList>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "mpr")
#pragma comment(lib, "shell32")
#pragma comment(lib, "uuid")

namespace hc::storage::native
{
    template<class Probe>
    HANDLE FindFirstCompatible(Probe Work, const OperationContext& Context)
    {
        const auto Value = Work(DWORD(FIND_FIRST_EX_LARGE_FETCH));
        if (Value != INVALID_HANDLE_VALUE) return Value;
        const auto Code = GetLastError();
        // Some redirectors reject the optional large-buffer request. Retry the
        // same read without it before treating the directory as unavailable.
        if ((Code == ERROR_UNEXP_NET_ERR || Code == ERROR_INVALID_PARAMETER || Code == ERROR_NOT_SUPPORTED)
            && !Context.Check({})) return Work(DWORD(0));
        SetLastError(Code); return Value;
    }

    // One recovery budget for the directory probe and the first enumeration.
    // Never disconnect a mapping: it may be in use by another tab or process.
    class MappedDriveRecovery
    {
    public:
        using MappingQuery = std::function<bool(const std::wstring&)>;
        using Restore = std::function<void(const std::wstring&)>;
        using ShellProbe = std::function<HRESULT(const std::wstring&)>;

        explicit MappedDriveRecovery(std::wstring Path,
            MappingQuery Query = {}, Restore Prepare = {}, ShellProbe Shell = {})
            : query_(std::move(Query)), prepare_(std::move(Prepare)), shell_(std::move(Shell))
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
            initialError_ = lastError_ = Code;
            const bool Mapped = query_ ? query_(root_.substr(0, 2)) : isMapped(root_.substr(0, 2));
            if (!Mapped || Context.Check({})) {
                trace();
                SetLastError(Code); return Value;
            }
            // Null owner and no DIRCREATE flags: no dialogs or directory writes.
            // The shell restores the existing mapping using Windows credentials.
            if (prepare_) prepare_(root_);
            else prepareStatus_ = restore(root_);
            if (Context.Check({})) { SetLastError(Code); return Value; }
            Value = Work();
            lastError_ = IsFailure(Value) ? GetLastError() : ERROR_SUCCESS;
            if (lastError_ == ERROR_UNEXP_NET_ERR && !Context.Check({}) && (shell_ || !prepare_)) {
                // A path-existence check can succeed while the redirector's
                // first directory read still fails. Exercise the read-only Shell
                // enumeration path used by Explorer, without opening Explorer.
                shellStatus_ = shell_ ? shell_(root_) : probeShell(root_);
                if (SUCCEEDED(*shellStatus_) && !Context.Check({})) {
                    Value = Work(); lastError_ = IsFailure(Value) ? GetLastError() : ERROR_SUCCESS;
                }
            }
            trace();
            if (lastError_) SetLastError(lastError_);
            return Value;
        }

        QString Diagnostics() const
        {
            if (!attempted_) return QStringLiteral("recovery=not-attempted");
            QStringList Fields{QStringLiteral("drive=%1; initial=win32:%2; retry=win32:%3")
                .arg(QString::fromStdWString(root_)).arg(initialError_).arg(lastError_)};
            if (mappingStatus_) Fields.append(QStringLiteral("mapping=win32:%1").arg(*mappingStatus_));
            if (!remote_.empty()) Fields.append(QStringLiteral("remote=%1").arg(QString::fromStdWString(remote_)));
            if (prepareStatus_) Fields.append(QStringLiteral("prepare=0x%1").arg(qulonglong(DWORD(*prepareStatus_)), 8, 16, QLatin1Char('0')));
            if (shellStatus_) Fields.append(QStringLiteral("shell=0x%1").arg(qulonglong(DWORD(*shellStatus_)), 8, 16, QLatin1Char('0')));
            return Fields.join(QStringLiteral("; "));
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
        bool isMapped(const std::wstring& Device)
        {
            DWORD Length = 512;
            std::vector<wchar_t> Remote(Length);
            auto Code = WNetGetConnectionW(Device.c_str(), Remote.data(), &Length);
            if (Code == ERROR_MORE_DATA && Length <= 32768) {
                Remote.resize(Length);
                Code = WNetGetConnectionW(Device.c_str(), Remote.data(), &Length);
            }
            mappingStatus_ = Code;
            if (Code == NO_ERROR || Code == ERROR_CONNECTION_UNAVAIL) {
                remote_ = Remote.data(); return true;
            }
            return false;
        }
        static HRESULT restore(const std::wstring& Root)
        {
            const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const auto Result = SHPathPrepareForWriteW(nullptr, nullptr, Root.c_str(), SHPPFW_NONE);
            if (SUCCEEDED(Initialized)) CoUninitialize();
            return Result;
        }

        static HRESULT probeShell(const std::wstring& Root)
        {
            const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(Initialized) && Initialized != RPC_E_CHANGED_MODE) return Initialized;
            const auto Uninitialize = qScopeGuard([Initialized] { if (SUCCEEDED(Initialized)) CoUninitialize(); });
            Microsoft::WRL::ComPtr<IShellItem> Item;
            auto Result = SHCreateItemFromParsingName(Root.c_str(), nullptr, IID_PPV_ARGS(Item.GetAddressOf()));
            if (FAILED(Result)) return Result;
            Microsoft::WRL::ComPtr<IShellFolder> Folder;
            Result = Item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(Folder.GetAddressOf()));
            if (FAILED(Result)) return Result;
            Microsoft::WRL::ComPtr<IEnumIDList> Entries;
            Result = Folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, Entries.GetAddressOf());
            if (FAILED(Result) || !Entries) return Result;
            PITEMID_CHILD Child = nullptr; ULONG Fetched = 0;
            Result = Entries->Next(1, &Child, &Fetched);
            CoTaskMemFree(Child);
            return Result;
        }

        static const QLoggingCategory& log()
        { static const QLoggingCategory Category("hc.storage.mappedDrive"); return Category; }
        void trace() const { if (!prepare_) qCWarning(log).noquote() << Diagnostics(); }

        std::wstring root_;
        MappingQuery query_;
        Restore prepare_;
        ShellProbe shell_;
        std::wstring remote_;
        std::optional<DWORD> mappingStatus_;
        std::optional<HRESULT> prepareStatus_, shellStatus_;
        DWORD initialError_ = ERROR_SUCCESS, lastError_ = ERROR_SUCCESS;
        bool attempted_ = false;
    };
}
