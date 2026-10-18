#include "builtinSevenZip.hpp"
#include "hcSevenZipManifest.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QCoreApplication>
namespace hc::plugins {
ApprovedWcxOptions BuiltinSevenZipOptions()
{
    ApprovedWcxOptions Options; const auto Root = QCoreApplication::applicationDirPath();
    const auto Package = Root + '/' + hc::layout::SevenZipPackage;
    Options.executable_ = Root + '/' + hc::layout::PluginHost;
    Options.binaryPath_ = Package + "/7z.dll";
    Options.manifestPath_ = Package + "/sevenzip.json";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinSevenZipManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    return Options;
}
}
