#include "builtinUnEgg.hpp"
#include "hcUnEggManifest.hpp"
#include <QCoreApplication>
namespace hc::plugins {
ApprovedWcxOptions BuiltinUnEggOptions()
{
    ApprovedWcxOptions Options; const auto Root = QCoreApplication::applicationDirPath();
    Options.executable_ = Root + "/hc-plugin-host-x64.exe";
    Options.binaryPath_ = Root + "/BuiltInPlugin/AlzEgg/UnEGG64.dll";
    Options.manifestPath_ = Root + "/BuiltInPlugin/AlzEgg/alzegg.json";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinUnEggManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    return Options;
}
}
