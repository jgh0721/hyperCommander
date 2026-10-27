#include "stdafx.h"
#include "updateMessages.hpp"
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
    Path Root, Tx, Self; std::string Error; std::wstring Handoff; unsigned ParentPid = 0; bool HandoffReady = false;
    const bool Quiet = Arguments && Count > 1 && std::wstring_view(Arguments[1]) == L"--ack";
    const bool Applying = Arguments && Count > 1 && std::wstring_view(Arguments[1]) == L"--apply";
    try {
        if (!Arguments || Count < 3) throw std::runtime_error("Usage: HyperCommanderUpdater.exe --recover|--ack ROOT; --apply ROOT TRANSACTION PID CREATED");
        // Keep startup failures diagnosable even when self-verification fails.
        Root = Path(Arguments[2]);
        if (Applying && (Count == 6 || Count == 7)) {
            Tx = Path(Arguments[3]); ParentPid = std::stoul(Arguments[4]);
            if (Count == 7) Handoff = Arguments[6];
        }
        wchar_t Image[32768]{}; const auto Length = GetModuleFileNameW(nullptr, Image, DWORD(std::size(Image)));
        if (!Length || Length >= std::size(Image)) throw std::runtime_error("update-helper-image"); Self = Image; VerifyProductSignature(Self);
        const std::wstring Mode = Arguments[1];
        if (Mode == L"--apply" && (Count == 6 || Count == 7)) {
            ApplyTransaction(Root, Tx, ParentPid, std::stoull(Arguments[5]), Handoff, &HandoffReady); Launch(Root, Tx);
        } else if (Mode == L"--recover" && Count == 3) { RecoverTransaction(Root); Launch(Root, {}); }
        else if (Mode == L"--ack" && Count == 3) AcknowledgeTransaction(Root);
        else throw std::runtime_error("update-helper-arguments");
    } catch (const std::exception& Failure) { Error = Failure.what(); }
    if (Arguments) LocalFree(Arguments);
    if (Error.empty()) return 0;
    if (!Root.empty()) {
        try { DurableWrite(Root / L".update/last-error.json", "{\"error\":" + QuoteJson(Error) + ",\"handoff\":" + QuoteJson(Utf8(Handoff)) + "}"); } catch (...) {}
    }
    // Before the parent accepts the handoff it owns error reporting and must
    // remain open. Do not launch a second product or an orphan error dialog.
    bool ParentRunning = false;
    if (!Handoff.empty()) {
        const auto Parent = OpenProcess(SYNCHRONIZE, FALSE, ParentPid);
        ParentRunning = Parent && WaitForSingleObject(Parent, 0) == WAIT_TIMEOUT;
        if (Parent) CloseHandle(Parent);
        if (!HandoffReady || Error == "update-handoff-cancelled") return 2;
    }
    // Health acknowledgement runs quietly. A failed apply/recovery remains
    // reviewable and retains its journal and original files.
    if (!Quiet) {
        std::wstring Details; try { Details = Wide(Error); } catch (...) { Details = L"update-failed"; }
        const bool CanStart = !ParentRunning && !Root.empty() && StartupAllowed(Root); bool Relaunched = false;
        if (Applying && CanStart) { try { Launch(Root, Tx); Relaunched = true; } catch (...) {} }
        std::wstring Guide; try { Guide = Explain(Error, Root); } catch (...) {}
        auto Message = L"업데이트를 완료하지 못했습니다. 파일과 복구 기록을 보존했습니다.\n\n" + (Guide.empty() ? Details : Guide + L"\n\n" + Details);
        if (ParentRunning) Message += L"\n\n기존 HyperCommander 프로세스가 아직 종료되지 않았습니다. 파일은 교체하지 않았습니다. 종료를 확인한 뒤 다시 실행하십시오.";
        else if (CanStart) Message += Relaunched ? L"\n\n기존 제품을 다시 실행했습니다." : L"\n\n기존 제품을 다시 실행할 수 있습니다.";
        else Message += L"\n\n제품 창을 모두 닫은 뒤 다음 명령으로 복구하십시오.\n"
            + Argument((Self.empty() ? Root / L"HyperCommanderUpdater.exe" : Self).wstring()) + L" --recover " + Argument(Root.wstring());
        MessageBoxW(nullptr, Message.c_str(), L"HyperCommander 업데이트", MB_OK | MB_ICONERROR);
    }
    return 2;
}
