#include "HyperCommanderUpdater/stdafx.h"
#include "HyperCommanderUpdater/updateArchive.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"

#include <QCoreApplication>
#include <iostream>

int main(int Count, char** Arguments) {
    QCoreApplication App(Count, Arguments); const auto Args = App.arguments(); using namespace hc::update;
    const auto Native = [](const QString& Value) { return Path(Value.toStdWString()); };
    const auto Release = [&](const QString& Directory) { const auto Root = Native(Directory); return ParseManifest(AuthenticateManifest(Root / L"update-manifest.cat", Root / L"update-manifest.json")); };
    try {
        if (Args.size() == 2 && Args[1] == "contract") {
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
