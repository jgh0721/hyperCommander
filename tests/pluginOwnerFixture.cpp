#include <QCoreApplication>
#include <iostream>
#include "pluginProcess.hpp"
#include "pipeChannel.hpp"

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv); if (App.arguments().size() != 3) return 2;
    auto Manifest = hc::plugins::InspectManifestFile(App.arguments()[2]); if (!Manifest.HasValue()) return 3;
    // Test-only owner: approval is created explicitly for this local fixture.
    hc::plugins::PluginApproval Approval; Approval.reviewed_ = Manifest.Value().manifest_;
    Approval.approved_ = true; Approval.granted_ = Approval.reviewed_.permissions_;
    auto Started = hc::plugins::PluginProcess::Start(App.arguments()[1], App.arguments()[2], Approval);
    if (!Started.HasValue()) { std::cerr << Started.Failure().code_ << std::endl; return 4; }
    std::cout << Started.Value()->Snapshot().processId_ << std::endl;
    Sleep(INFINITE); return 0;
}
