#pragma once
#include "commonLibs/plugins/pluginManifest.hpp"
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace hc::plugins
{
    storage::Result<PluginManifest> ParseManifest(const QByteArray& Json);
    QByteArray EncodeManifest(const PluginManifest& Manifest);
    struct InspectedPlugin
    {
        PluginManifest manifest_;
        QString binaryPath_;
        QStringList dependencyPaths_;
    };
    // Worker/CLI operation. This is a point-in-time inspection, not a load
    // authorization or a file lease. The future host must revalidate at load.
    storage::Result<InspectedPlugin> InspectManifestFile(const QString& Path, storage::OperationContext Context = {});
}
