#pragma once
#include "hostedViewerSession.hpp"
#include <QFileInfo>
#include <qt_windows.h>

namespace hc::plugins {
// Worker-only: read the SDK declaration through the already approved host.
inline storage::Result<QString> ReadWlxDetection(const PluginViewerOptions& Options, storage::OperationContext Context = {})
{
    auto Inspected = InspectManifestFile(Options.manifestPath_, Context);
    if (!Inspected.HasValue()) return Inspected.Failure();
    if (Inspected.Value().manifest_.format_ != PluginFormat::Wlx || Options.approval_.reviewed_ != Inspected.Value().manifest_
        || QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
        return storage::Error{storage::ErrorCategory::AccessDenied, "plugin-review-required"};
    auto Host = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_, Context, Options.defaultIniPath_);
    if (!Host.HasValue()) return Host.Failure();
    auto Probe = Host.Value()->Probe(Context);
    Host.Value()->Close();
    if (!Probe.HasValue()) return Probe.Failure();
    if (Probe.Value()["ansiCodePage"].toInt(-1) != int(GetACP()))
        return storage::Error{storage::ErrorCategory::Unsupported, "wlx-detect-code-page"};
    const auto Decoded = QByteArray::fromBase64Encoding(Probe.Value()["detectStringBytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!Decoded) return storage::Error{storage::ErrorCategory::InvalidRequest, "wlx-detect-base64"};
    const auto Text = QString::fromLocal8Bit(Decoded.decoded);
    if (Text.toLocal8Bit() != Decoded.decoded) return storage::Error{storage::ErrorCategory::Unsupported, "wlx-detect-encoding"};
    return Text;
}
}
