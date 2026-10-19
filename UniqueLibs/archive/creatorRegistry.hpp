#pragma once
#include "commonLibs/plugins/archiveCreation.hpp"
#include "pluginHost/approvedWcxBackend.hpp"
#include "creationPlan.hpp"
#include <QJsonObject>

namespace hc::settings { class Repository; }
namespace hc::archive {
struct CreatorRuntime {
    plugins::ArchiveCreatorDescriptor descriptor_;
    plugins::ApprovedWcxOptions options_;
    bool available_ = true;
    QString status_;
};
std::vector<CreatorRuntime> CreatorCatalog(settings::Repository& Settings);
CreatorRuntime ExternalCreator(const QJsonObject& Record, plugins::PluginFormat Format, const QString& SettingsDirectory);
storage::Result<storage::Unit> RevalidateCreator(const CreatorRuntime& Creator, storage::OperationContext Context);
storage::Result<std::shared_ptr<void>> AcquireCreatorSettings(const CreatorRuntime& Creator, storage::OperationContext Context);
struct PreparedArchiveOutput { QString path_; std::shared_ptr<void> lifetime_; };
storage::Result<PreparedArchiveOutput> CreatePreparedArchive(const CreatorRuntime& Creator, const plugins::ArchiveCreateSettings& Settings,
    std::string Format, const OutputPlan& Plan, const QString& Input, const QString& Temporary, storage::OperationContext Context);
// Explicit worker-side probe, never invoked while filling the format combo.
storage::Result<QJsonObject> VerifyCreationFormats(CreatorRuntime Creator, const QStringList& Formats, storage::OperationContext Context);
QByteArray CreatorManifestIdentity(const plugins::PluginManifest& Manifest);
}
