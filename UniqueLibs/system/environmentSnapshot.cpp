#include "stdafx.h"
#include "environmentSnapshot.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "environmentStorage.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <ShlObj.h>
#include <Lmcons.h>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentSnapshot, "hc.environment.snapshot")
namespace hc::environment {
EnvironmentSnapshot LoadEnvironmentSnapshot(registry::IRegistryBackend& Backend, MergeInput Defaults, const QString& CallerSid)
{
    EnvironmentSnapshot Result; Result.input_ = std::move(Defaults);
    for (auto Source : {Scope::System, Scope::User}) {
        auto& Error = Source == Scope::System ? Result.systemError_ : Result.userError_;
        auto& Values = Source == Scope::System ? Result.input_.system_ : Result.input_.user_;
        Values.clear(); const auto Read = Backend.Enumerate(EnvironmentLocation(Source, CallerSid));
        if (!Read) { if (Read.error != ERROR_FILE_NOT_FOUND && Read.error != ERROR_PATH_NOT_FOUND) Error = Read.error; continue; }
        if (Read.value.info.link) { Error = ERROR_CANT_ACCESS_FILE; continue; }
        for (const auto& Raw : Read.value.values) {
            const auto Item = DecodeEnvironmentValue(Raw, Source);
            if (Item) Values.push_back(Item.value);
            else Result.unsupported_.append(Raw.name); // names only; never log value bytes
        }
    }
    Result.merged_ = BuildEnvironment(Result.input_); return Result;
}
MergeInput ReadWindowsEnvironmentDefaults()
{
    MergeInput Result;
    const auto Known = [&](REFKNOWNFOLDERID Id, const wchar_t* Name, bool System) {
        PWSTR Path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(Id, KF_FLAG_DONT_VERIFY, nullptr, &Path))) {
            (System ? Result.coreSystem_ : Result.coreUser_).push_back({Name, Path, ValueType::String, System ? Scope::CoreSystem : Scope::CoreUser});
            CoTaskMemFree(Path);
        }
    };
    Known(FOLDERID_ProgramData, L"ALLUSERSPROFILE", true); Known(FOLDERID_ProgramData, L"ProgramData", true);
    Known(FOLDERID_Public, L"PUBLIC", true); Known(FOLDERID_Profile, L"USERPROFILE", false);
    Known(FOLDERID_RoamingAppData, L"APPDATA", false); Known(FOLDERID_LocalAppData, L"LOCALAPPDATA", false);
    wchar_t Windows[MAX_PATH + 1]{}; const auto Count = GetWindowsDirectoryW(Windows, MAX_PATH);
    if (Count && Count < MAX_PATH) {
        Result.coreSystem_.push_back({L"SystemRoot", Windows});
        if (Windows[1] == L':') Result.coreSystem_.push_back({L"SystemDrive", std::wstring(Windows, 2)});
    }
    wchar_t Computer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD Size = DWORD(std::size(Computer));
    if (GetComputerNameW(Computer, &Size)) Result.coreUser_.push_back({L"COMPUTERNAME", Computer});
    wchar_t User[UNLEN + 1]{}; Size = DWORD(std::size(User));
    if (GetUserNameW(User, &Size)) Result.account_.push_back({L"USERNAME", User});
    Result.coreSystem_.push_back({L"USERNAME", L"SYSTEM"});
    HANDLE Token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) {
        const auto Close = qScopeGuard([&] { CloseHandle(Token); });
        DWORD Bytes = 0; GetTokenInformation(Token, TokenUser, nullptr, 0, &Bytes);
        std::vector<BYTE> Buffer(Bytes);
        if (Bytes && GetTokenInformation(Token, TokenUser, Buffer.data(), Bytes, &Bytes)) {
            DWORD NameSize = 0, DomainSize = 0; SID_NAME_USE Use{};
            auto* Sid = reinterpret_cast<TOKEN_USER*>(Buffer.data())->User.Sid;
            LookupAccountSidW(nullptr, Sid, nullptr, &NameSize, nullptr, &DomainSize, &Use);
            std::wstring Name(NameSize, L'\0'), Domain(DomainSize, L'\0');
            if (NameSize && DomainSize && LookupAccountSidW(nullptr, Sid, Name.data(), &NameSize, Domain.data(), &DomainSize, &Use)) {
                Domain.resize(DomainSize); Result.account_.push_back({L"USERDOMAIN", Domain});
            }
        }
    }
    registry::WinRegistryBackend Backend;
    const registry::RegistryLocation Version{registry::Hive::LocalMachine, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion"};
    for (const auto& Pair : {std::pair{L"ProgramFiles", "ProgramFilesDir"}, std::pair{L"ProgramFiles(x86)", "ProgramFilesDir (x86)"},
        std::pair{L"ProgramW6432", "ProgramFilesDir"}, std::pair{L"CommonProgramFiles", "CommonFilesDir"},
        std::pair{L"CommonProgramFiles(x86)", "CommonFilesDir (x86)"}, std::pair{L"CommonProgramW6432", "CommonFilesDir"}}) {
        const auto Raw = Backend.Read(Version, QString::fromLatin1(Pair.second));
        if (Raw) { const auto Text = registry::RegistryValueCodec::DecodeText(Raw.value); if (Text.lossless) Result.coreUser_.push_back({Pair.first, Text.text.toStdWString()}); }
    }
    // VM Win11 26200 x64, CreateEnvironmentBlock(inherit=false): these six
    // references remain literal in machine REG_EXPAND_SZ. Do not seed stage 3.
    return Result;
}
ValueMap ReadOwnEnvironment()
{
    ValueMap Result; auto* Block = GetEnvironmentStringsW(); if (!Block) return Result;
    const auto Free = qScopeGuard([&] { FreeEnvironmentStringsW(Block); });
    for (const auto* Item = Block; *Item; Item += wcslen(Item) + 1) {
        const std::wstring Text(Item); const auto Equal = Text.find(L'=', Text.front() == L'=' ? 1 : 0);
        if (Equal != Text.npos) Result[Text.substr(0, Equal)] = Text.substr(Equal + 1);
    }
    return Result;
}
ValueMap ReconcileEnvironment(const ValueMap& Inherited, const ValueMap& LastModel,
    const ValueMap& NextModel, const ValueMap& Current, const ValueMap& Overlay)
{
    ValueMap Result = NextModel;
    // Inherited-only values survive; deleted persistent values must not revive.
    for (const auto& [Name, Text] : Inherited) if (!LastModel.contains(Name) && !NextModel.contains(Name)) Result[Name] = Text;
    for (const auto& [Name, Text] : Current) {
        const auto Previous = LastModel.find(Name), Start = Inherited.find(Name);
        const bool RuntimeChange = Previous != LastModel.end() ? Previous->second != Text : Start == Inherited.end() || Start->second != Text;
        if (RuntimeChange) Result[Name] = Text;
    }
    for (const auto& [Name, Text] : LastModel) if (!Current.contains(Name)) Result.erase(Name);
    for (const auto& [Name, Text] : Overlay) Result[Name] = Text;
    return Result;
}
}
