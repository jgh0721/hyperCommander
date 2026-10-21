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
        else if (Args.size() == 2 && Args[1] == "contract") {
            unsigned Passed = 0;
            const auto Reject = [&](const auto& Action) { try { Action(); } catch (const std::exception&) { ++Passed; return; } throw std::runtime_error("negative-case-accepted"); };
            const auto Value = ParseJson("{\"n\":18446744073709551615,\"s\":\"\\ud83d\\ude00\",\"ok\":true,\"v\":[null,2]}");
            if (Value.At("n", Json::Kind::Number).number_ != UINT64_MAX || Value.At("s", Json::Kind::String).text_ != "\xf0\x9f\x98\x80") throw std::runtime_error("valid-json"); ++Passed;
            for (const auto Text : {"{\"a\":1,\"a\":2}", "{\"a\":01}", "{\"a\":-1}", "{\"a\":1.5}", "{\"a\":18446744073709551616}", "\"\\ud800\"", "{}x", "[1,]"}) Reject([&] { ParseJson(Text); });
            Reject([&] { ParseJson(std::string(25, '[') + "0" + std::string(25, ']')); });
            Reject([&] { Value.At("missing", Json::Kind::String); });
            for (const auto Text : {"../a", "C:/a", "a\\b", "a/file:stream", "a/NUL.txt", "a/CONIN$", "a/COM1", "a/file.", "a//b", "a/"}) Reject([&] { CheckRelative(Text); });
            for (const auto Text : {"HyperCommander.json", "config/settings.json", "Addons/WCX/User/a.dll", ".update/active.json", "Plugins/User/a.dll"}) Reject([&] { CheckProductRelative(Text); });
            CheckProductRelative("Addons/WCX/IsoImage/hc_isoimage_wcx.wcx64"); ++Passed;
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
