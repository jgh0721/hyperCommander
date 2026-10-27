#include "HyperCommanderUpdater/stdafx.h"
#include "HyperCommanderUpdater/updateArchive.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <iostream>
#include <thread>

namespace {
void ProcessPolicy(const QString& Directory) {
    using namespace hc::update;
    QTemporaryDir Fixture(QDir(Directory).absoluteFilePath("process-policy-XXXXXX"));
    if (!Fixture.isValid()) throw std::runtime_error("process-fixture-directory");
    const Path Root(Fixture.path().toStdWString());
    const auto Check = [&](bool Expected, ProductProcessCheck Policy) {
        if (OtherProductProcesses(Root, 0, Policy) != Expected) throw std::runtime_error("process-policy-mismatch");
    };
    Check(false, ProductProcessCheck::Strict);
    unsigned Passed = 1;
    for (const auto& Mode : {QString("hidden"), QString("visible"), QString("embedded"), QString("editor")}) {
        const auto Image = QDir(Fixture.path()).filePath(Mode == "editor" ? "HyperEditor.exe" : "hc-plugin-host-x64.exe");
        if (QFile::exists(Image) && !QFile::remove(Image)) throw std::runtime_error("process-fixture-remove");
        if (!QFile::copy(QCoreApplication::applicationFilePath(), Image)) throw std::runtime_error("process-fixture-copy");
        HWND Parent = nullptr;
        if (Mode == "embedded") {
            Parent = CreateWindowExW(0, L"STATIC", L"Update embedded viewer test", WS_OVERLAPPEDWINDOW,
                0, 0, 120, 80, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!Parent) throw std::runtime_error("process-fixture-parent");
            ShowWindow(Parent, SW_SHOWNOACTIVATE);
        }
        const auto CloseParent = qScopeGuard([&] { if (Parent) DestroyWindow(Parent); });
        QProcess Child;
        const auto Reap = qScopeGuard([&] { if (Child.state() != QProcess::NotRunning) { Child.kill(); Child.waitForFinished(5000); } });
        auto Environment = QProcessEnvironment::systemEnvironment();
        Environment.insert("PATH", QCoreApplication::applicationDirPath() + ";" + Environment.value("PATH"));
        Child.setProcessEnvironment(Environment);
        Child.start(Image, {"process-fixture", Mode, QString::number(reinterpret_cast<quintptr>(Parent))});
        if (!Child.waitForStarted(5000)) throw std::runtime_error("process-fixture-start");
        // Creating an embedded native child sends messages to its owner thread.
        // Keep that thread pumping while waiting for the fixture's ready signal.
        QByteArray Ready; const auto Until = GetTickCount64() + 5000;
        while (!Ready.contains("ready") && Child.state() != QProcess::NotRunning && GetTickCount64() < Until) {
            Child.waitForReadyRead(10); Ready += Child.readAllStandardOutput(); QCoreApplication::processEvents();
        }
        if (!Ready.contains("ready")) throw std::runtime_error("process-fixture-ready: " + Mode.toStdString() + ": " + Child.readAllStandardError().toStdString());
        Check(true, ProductProcessCheck::Strict);
        Check(Mode != "hidden", ProductProcessCheck::BeforeRestart);
        if (OtherProductProcesses(Root, unsigned(Child.processId()))) throw std::runtime_error("process-ignore-pid");
        Child.kill(); if (!Child.waitForFinished(5000)) throw std::runtime_error("process-fixture-exit");
        Check(false, ProductProcessCheck::Strict); ++Passed;
    }
    std::cout << "Process policy: " << Passed << " passed (strict, owned background, visible, embedded, other product)\n";
}
// The fixture must live on a local NTFS/ReFS volume and run without elevation.
void Transactions(const QString& Directory) {
    using namespace hc::update;
    QTemporaryDir Fixture(QDir(Directory).absoluteFilePath("update-transactions-XXXXXX"));
    if (!Fixture.isValid()) throw std::runtime_error("transactions-fixture-directory");
    const auto Root = std::filesystem::weakly_canonical(Path(Fixture.path().toStdWString()));
    unsigned Passed = 0;
    const auto Reject = [&](const auto& Action, std::string_view Code) {
        try { Action(); } catch (const std::runtime_error& Error) { if (std::string_view(Error.what()).starts_with(Code)) { ++Passed; return; } throw; }
        throw std::runtime_error("negative-case-accepted: " + std::string(Code));
    };
    // Launch spellings that differ only in letter case or separators name the stored root.
    auto Lower = Root.wstring(); CharLowerBuffW(Lower.data(), DWORD(Lower.size()));
    if (Lower == Root.wstring()) throw std::runtime_error("transactions-fixture-case");
    Reject([&] { CheckInstallRoot(Path(Lower)); }, "update-install-root");
    if (InstallRoot(Path(Lower)) != Root || InstallRoot(Path(Root.generic_wstring())) != Root) throw std::runtime_error("install-root-spelling"); ++Passed;
    CheckInstallRoot(InstallRoot(Path(Lower))); ++Passed;
    std::wstring Short(32768, L'\0'); Short.resize(GetShortPathNameW(Root.c_str(), Short.data(), DWORD(Short.size())));
    if (!Short.empty() && !Short.starts_with(Root.wstring())) { if (InstallRoot(Path(Short)) != Root) throw std::runtime_error("install-root-short-name"); ++Passed; }
    // A junction keeps its own spelling and stays rejected.
    const auto Link = Root.parent_path() / (Root.filename().wstring() + L"-link");
    if (QProcess::execute("cmd.exe", {"/d", "/c", "mklink", "/J", QString::fromStdWString(Link.wstring()), QString::fromStdWString(Root.wstring())}) != 0)
        throw std::runtime_error("transactions-fixture-junction");
    { const auto Unlink = qScopeGuard([&] { RemoveDirectoryW(Link.c_str()); });
      if (InstallRoot(Link) != Link) throw std::runtime_error("install-root-junction"); ++Passed;
      Reject([&] { CheckInstallRoot(InstallRoot(Link)); }, "update-path-linked: " + Utf8(Link.wstring())); }
    // Failures name the file they concern.
    Reject([&] { ReadBounded(Root / L"missing.json", 16); }, "update-file-open: " + Utf8((Root / L"missing.json").wstring()));
    std::filesystem::create_directories(Root / L"Licenses"); std::ofstream(Root / L"Licenses/a.txt", std::ios::binary) << "notice";
    Reject([&] { VerifyProductFile(Root / L"Licenses/a.txt", "Licenses/a.txt", std::string(64, '0'), 6); }, "update-file-hash: Licenses/a.txt");
    Reject([&] { VerifyProductFile(Root / L"Licenses/a.txt", "Licenses/a.txt", std::string(64, '0'), 7); }, "update-file-size: Licenses/a.txt");
    // Unapplied transactions: the caller's own at any age, others after an hour.
    const auto Update = Root / L".update";
    const auto Make = [&](std::wstring_view Name, std::initializer_list<std::wstring_view> Files, bool Old) {
        const auto Tx = Update / Name; std::filesystem::create_directories(Tx);
        for (const auto File : Files) { std::filesystem::create_directories((Tx / File).parent_path()); std::ofstream(Tx / File, std::ios::binary) << "x"; }
        if (Old) {
            const auto Time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(2);
            for (const auto& Entry : std::filesystem::recursive_directory_iterator(Tx)) std::filesystem::last_write_time(Entry.path(), Time);
            std::filesystem::last_write_time(Tx, Time);
        }
        return Tx;
    };
    const auto Own = Make(L"00000000-0000-4000-8000-000000000001", {L"update-manifest.json", L"update-manifest.cat", L"portable.zip", L"portable.zip.AbC123", L"staging/Libs/a.dll"}, false);
    const auto Orphan = Make(L"00000000-0000-4000-8000-000000000002", {L"update-manifest.json", L"update-manifest.cat"}, true);
    const auto Empty = Make(L"00000000-0000-4000-8000-000000000003", {}, true);
    const auto Recent = Make(L"00000000-0000-4000-8000-000000000004", {L"update-manifest.json", L"portable.zip"}, false);
    const auto Ready = Make(L"00000000-0000-4000-8000-000000000005", {L"update-manifest.json", L"staging/HyperCommander.exe"}, true);
    const auto Applied = Make(L"00000000-0000-4000-8000-000000000006", {L"journal.json", L"update-manifest.json"}, true);
    const auto Unknown = Make(L"00000000-0000-4000-8000-000000000007", {L"update-manifest.json", L"notes.txt"}, true);
    const auto Other = Make(L"keep-me", {L"update-manifest.json"}, true);
    std::ofstream(Update / L"ready.json", std::ios::binary) << "{\"transaction\":\"00000000-0000-4000-8000-000000000005\"}";
    PruneTransactions(Root, Own, std::chrono::hours(1));
    for (const auto& Gone : {Own, Orphan, Empty}) { if (std::filesystem::exists(Gone)) throw std::runtime_error("prune-kept: " + Utf8(Gone.filename().wstring())); ++Passed; }
    for (const auto& Kept : {Recent / L"portable.zip", Ready / L"staging/HyperCommander.exe", Applied / L"journal.json", Unknown / L"notes.txt", Unknown / L"update-manifest.json", Other / L"update-manifest.json"}) {
        if (!std::filesystem::exists(Kept)) throw std::runtime_error("prune-removed: " + Utf8(Kept.lexically_relative(Update).wstring())); ++Passed;
    }
    // An unreadable reference may hide a prepared transaction: prune nothing.
    std::ofstream(Update / L"ready.json", std::ios::binary | std::ios::trunc) << "not json";
    const auto Hidden = Make(L"00000000-0000-4000-8000-000000000008", {L"update-manifest.json"}, true);
    Reject([&] { PruneTransactions(Root, {}, std::chrono::hours(1)); }, "update-json-invalid");
    if (!std::filesystem::exists(Hidden)) throw std::runtime_error("prune-unreadable-reference"); ++Passed;
    // A held root lock skips the optional cleanup at once but only delays a health check.
    const auto Held = CreateFileW((Update / L"lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (Held == INVALID_HANDLE_VALUE) throw std::runtime_error("transactions-fixture-lock");
    {   std::jthread Release([Held] { Sleep(300); CloseHandle(Held); });
        Reject([&] { PruneTransactions(Root, {}, std::chrono::hours(1)); }, "update-root-locked");
        const auto Started = GetTickCount64(); AcknowledgeTransaction(Root);
        if (GetTickCount64() - Started < 200) throw std::runtime_error("lock-wait-not-exercised"); ++Passed; }
    std::cout << "Transactions: " << Passed << " passed (install root spelling, error paths, unapplied cleanup, lock wait)\n";
}
}

int main(int Count, char** Arguments) {
    QCoreApplication App(Count, Arguments); const auto Args = App.arguments(); using namespace hc::update;
    const auto Native = [](const QString& Value) { return Path(Value.toStdWString()); };
    const auto Release = [&](const QString& Directory) { const auto Root = Native(Directory); return ParseManifest(AuthenticateManifest(Root / L"update-manifest.cat", Root / L"update-manifest.json")); };
    try {
        if (Args.size() == 4 && Args[1] == "process-fixture") {
            HWND Window = nullptr;
            if (Args[2] == "visible" || Args[2] == "embedded") {
                const auto Parent = reinterpret_cast<HWND>(Args[3].toULongLong());
                Window = CreateWindowExW(0, L"STATIC", L"Update plugin window test",
                    Parent ? WS_CHILD | WS_VISIBLE : WS_OVERLAPPEDWINDOW, 0, 0, 100, 60,
                    Parent, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (!Window) throw std::runtime_error("process-fixture-window");
                ShowWindow(Window, SW_SHOWNOACTIVATE);
            }
            std::cout << "ready\n" << std::flush;
            return App.exec();
        } else if (Args.size() == 3 && Args[1] == "process-policy") ProcessPolicy(Args[2]);
        else if (Args.size() == 3 && Args[1] == "transactions") Transactions(Args[2]);
        else if (Args.size() == 2 && Args[1] == "contract") {
            unsigned Passed = 0;
            const auto Reject = [&](const auto& Action) { try { Action(); } catch (const std::exception&) { ++Passed; return; } throw std::runtime_error("negative-case-accepted"); };
            const auto Value = ParseJson("{\"n\":18446744073709551615,\"s\":\"\\ud83d\\ude00\",\"ok\":true,\"v\":[null,2]}");
            if (Value.At("n", Json::Kind::Number).number_ != UINT64_MAX || Value.At("s", Json::Kind::String).text_ != "\xf0\x9f\x98\x80") throw std::runtime_error("valid-json"); ++Passed;
            for (const auto Text : {"{\"a\":1,\"a\":2}", "{\"a\":01}", "{\"a\":-1}", "{\"a\":1.5}", "{\"a\":18446744073709551616}", "\"\\ud800\"", "{}x", "[1,]"}) Reject([&] { ParseJson(Text); });
            Reject([&] { ParseJson(std::string(25, '[') + "0" + std::string(25, ']')); });
            Reject([&] { Value.At("missing", Json::Kind::String); });
            for (const auto Text : {"../a", "C:/a", "a\\b", "a/file:stream", "a/NUL.txt", "a/CONIN$", "a/COM1", "a/file.", "a//b", "a/"}) Reject([&] { CheckRelative(Text); });
            for (const auto Text : {"HyperCommander.json", "config/settings.json", "Addons/WCX/User/a.dll", ".update/active.json", "Plugins/User/a.dll", "HyperCompareOther.exe", "HyperCompareCli.exe.bak"}) Reject([&] { CheckProductRelative(Text); });
            for (const auto Text : {"Addons/WCX/IsoImage/hc_isoimage_wcx.wcx64", "HyperCompare.exe", "HyperCompareCli.exe", "opengl32sw.dll", "Qt6Core.dll", "vcruntime140_1.dll", "d3dcompiler_47.dll", "Libs/platforms/qwindows.dll", "product-inventory.json"}) { CheckProductRelative(Text); ++Passed; }
            for (const auto Text : {"Qt6Core.exe", "opengl32sw.dll.bak", "Libs", "Qt6/Core.dll", "libs/platforms/qwindows.dll"}) Reject([&] { CheckProductRelative(Text); });
            for (const auto Text : {"HyperCompareOther.exe", "Addons/WCX/User/custom.dll"}) {
                try { CheckProductRelative(Text); throw std::logic_error("unowned-path-accepted"); }
                catch (const std::runtime_error& Error) {
                    if (std::string(Error.what()) != "update-unowned-path: " + std::string(Text)) throw;
                    ++Passed;
                }
            }
            if (ParseVersion("2026.10.18.1") != std::array<unsigned, 4>{2026, 10, 18, 1}) throw std::runtime_error("valid-version"); ++Passed;
            for (const auto Text : {"2026.10.18", "2026.10.18.1a", "2026.10.18.65536", "2026.10.18.1.2"}) Reject([&] { ParseVersion(Text); });
            std::cout << "Contract: " << Passed << " passed, 0 failed\n";
        } else if (Args.size() == 4 && Args[1] == "verify") VerifyTree(Native(Args[3]), Release(Args[2]), false);
        else if (Args.size() == 6 && Args[1] == "stage") StageArchive(Args[3], Args[4], Args[5], Release(Args[2]));
        else if (Args.size() == 6 && Args[1] == "apply") ApplyTransaction(Native(Args[2]), Native(Args[3]), Args[4].toUInt(), Args[5].toULongLong());
        else if (Args.size() == 3 && Args[1] == "recover") RecoverTransaction(Native(Args[2]));
        else if (Args.size() == 3 && Args[1] == "ack") AcknowledgeTransaction(Native(Args[2]));
        else if (Args.size() == 3 && Args[1] == "gate") { if (!StartupAllowed(Native(Args[2]))) throw std::runtime_error("startup-blocked"); }
        else throw std::runtime_error("test-arguments");
        std::cout << "PASS\n"; return 0;
    } catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
