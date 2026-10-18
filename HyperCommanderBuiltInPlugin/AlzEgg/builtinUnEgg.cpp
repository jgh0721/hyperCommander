#include "builtinUnEgg.hpp"
#include "hcUnEggManifest.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QCoreApplication>
namespace hc::plugins {
ApprovedWcxOptions BuiltinUnEggOptions()
{
    ApprovedWcxOptions Options; const auto Root = QCoreApplication::applicationDirPath();
    const auto Package = Root + '/' + hc::layout::AlzEggPackage;
    Options.executable_ = Root + '/' + hc::layout::PluginHost;
    Options.binaryPath_ = Package + "/UnEGG64.dll";
    Options.manifestPath_ = Package + "/alzegg.json";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinUnEggManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    return Options;
}
}
