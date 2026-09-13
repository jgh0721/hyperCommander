#include "builtinIso.hpp"
#include "hcIsoManifest.hpp"
#include <QCoreApplication>

namespace hc::plugins {
ApprovedWcxOptions BuiltinIsoOptions()
{
    ApprovedWcxOptions Options;
    const auto Root = QCoreApplication::applicationDirPath();
    Options.executable_ = Root + "/hc-plugin-host-x64.exe";
    Options.manifestPath_ = Root + "/BuiltInPlugin/IsoImage/isoimage.json";
    Options.binaryPath_ = Root + "/BuiltInPlugin/IsoImage/hc_isoimage_wcx.wcx64";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinIsoManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    Options.limits_.archiveWriteMs_ = 3600000;
    return Options;
}
}
