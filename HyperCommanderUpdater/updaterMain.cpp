#include "stdafx.h"
#include "updateTransaction.hpp"

#include <shellapi.h>

namespace {
std::wstring Argument(std::wstring_view Value) {
    std::wstring Result = L"\""; unsigned Slashes = 0;
    for (const auto Ch : Value) {
        if (Ch == L'\\') { ++Slashes; continue; }
        if (Ch == L'\"') Result.append(Slashes * 2 + 1, L'\\'); else Result.append(Slashes, L'\\');
        Slashes = 0; Result += Ch;
    }
    Result.append(Slashes * 2, L'\\'); return Result + L'\"';
}
void Launch(const hc::update::Path& Root, const hc::update::Path& Tx) {
    using namespace hc::update;
    if (!StartupAllowed(Root)) return; const auto Image = Root / L"HyperCommander.exe"; VerifyProductSignature(Image);
    auto Command = Argument(Image.wstring());
    if (!Tx.empty() && std::filesystem::exists(Tx / L"restart.json")) {
        const auto Config = ParseJson(ReadBounded(Tx / L"restart.json", 32768)).At("configDirectory", Json::Kind::String).text_;
        const Path Directory(Wide(Config)); if (!Directory.is_absolute()) throw std::runtime_error("update-config-directory");
        Command += L" --config-dir " + Argument(Directory.wstring());
    }
    STARTUPINFOW Startup{sizeof(Startup)}; PROCESS_INFORMATION Process{};
    if (!CreateProcessW(Image.c_str(), Command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, Root.c_str(), &Startup, &Process))
        throw std::runtime_error("update-relaunch-failed");
    CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace hc::update; int Count = 0; auto* Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    Path Root, Tx, Self; std::string Error;
    const bool Quiet = Arguments && Count > 1 && std::wstring_view(Arguments[1]) == L"--ack";
    const bool Applying = Arguments && Count > 1 && std::wstring_view(Arguments[1]) == L"--apply";
    try {
        if (!Arguments || Count < 3) throw std::runtime_error("Usage: HyperCommanderUpdater.exe --recover|--ack ROOT; --apply ROOT TRANSACTION PID CREATED");
        wchar_t Image[32768]{}; const auto Length = GetModuleFileNameW(nullptr, Image, DWORD(std::size(Image)));
        if (!Length || Length >= std::size(Image)) throw std::runtime_error("update-helper-image"); Self = Image; VerifyProductSignature(Self);
        Root = Path(Arguments[2]); const std::wstring Mode = Arguments[1];
        if (Mode == L"--apply" && Count == 6) {
            Tx = Path(Arguments[3]); ApplyTransaction(Root, Tx, std::stoul(Arguments[4]), std::stoull(Arguments[5])); Launch(Root, Tx);
        } else if (Mode == L"--recover" && Count == 3) { RecoverTransaction(Root); Launch(Root, {}); }
        else if (Mode == L"--ack" && Count == 3) AcknowledgeTransaction(Root);
        else throw std::runtime_error("update-helper-arguments");
    } catch (const std::exception& Failure) { Error = Failure.what(); }
    if (Arguments) LocalFree(Arguments);
    if (Error.empty()) return 0;
    if (!Root.empty()) {
        try { DurableWrite(Root / L".update/last-error.json", "{\"error\":" + QuoteJson(Error) + "}"); } catch (...) {}
    }
    // Health acknowledgement runs quietly. A failed apply/recovery remains
    // reviewable and retains its journal and original files.
    if (!Quiet) {
        std::wstring Details; try { Details = Wide(Error); } catch (...) { Details = L"update-failed"; }
        const bool CanStart = !Root.empty() && StartupAllowed(Root); bool Relaunched = false;
        if (Applying && CanStart) { try { Launch(Root, Tx); Relaunched = true; } catch (...) {} }
        auto Message = L"업데이트를 완료하지 못했습니다. 파일과 복구 기록을 보존했습니다.\n\n" + Details;
        if (CanStart) Message += Relaunched ? L"\n\n기존 제품을 다시 실행했습니다." : L"\n\n기존 제품을 다시 실행할 수 있습니다.";
        else Message += L"\n\n제품 창을 모두 닫은 뒤 다음 명령으로 복구하십시오.\n"
            + Argument((Self.empty() ? Root / L"HyperCommanderUpdater.exe" : Self).wstring()) + L" --recover " + Argument(Root.wstring());
        MessageBoxW(nullptr, Message.c_str(), L"HyperCommander 업데이트", MB_OK | MB_ICONERROR);
    }
    return 2;
}
