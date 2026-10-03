#include "builtinSevenZip.hpp"
#include "hcSevenZipManifest.hpp"
#include <QCoreApplication>
namespace hc::plugins {
ApprovedWcxOptions BuiltinSevenZipOptions()
{
    ApprovedWcxOptions Options; const auto Root = QCoreApplication::applicationDirPath();
    Options.executable_ = Root + "/hc-plugin-host-x64.exe";
    Options.binaryPath_ = Root + "/BuiltInPlugin/SevenZip/7z.dll";
    Options.manifestPath_ = Root + "/BuiltInPlugin/SevenZip/sevenzip.json";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinSevenZipManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    return Options;
}
}
