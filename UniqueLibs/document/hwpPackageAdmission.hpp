#pragma once
#include "hwpBundleHost.hpp"
namespace hc::document
{
    // ExpectedManifestSha256 is a native release-policy pin, not supplied by JS
    // or read back from the package being approved. Binary/DLL admission remains
    // the trusted owner's separate responsibility before launching this helper.
    std::shared_ptr<const HwpBundle> LoadApprovedHwpPackage(const QString& Root,
        const QByteArray& ExpectedManifestSha256, QString& Error);
}
