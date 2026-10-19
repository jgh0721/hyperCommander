#pragma once
#include "creatorRegistry.hpp"
#include <QJsonObject>
#include <QWidget>

namespace hc::archive {
QJsonObject CreationPreferences(settings::Repository& Settings);
storage::Result<storage::Unit> SaveCreationPreferences(settings::Repository& Settings, QJsonObject Preferences);
plugins::ArchiveCompositionOptions ReadComposition(const QJsonObject& Preferences);
void WriteComposition(QJsonObject& Preferences, plugins::ArchiveCompositionOptions Options);
storage::Result<plugins::ArchiveCreateSettings> ReadCreatorSettings(const QJsonObject& Preferences, const CreatorRuntime& Creator, const std::string& Format);
void WriteCreatorSettings(QJsonObject& Preferences, const CreatorRuntime& Creator, const std::string& Format, const plugins::ArchiveCreateSettings& Settings);
QString CreationSettingsSummary(const plugins::ArchiveCreateSettings& Settings, const std::string& Format);
bool EditBuiltinCreationSettings(QWidget* Parent, const std::string& Format, plugins::ArchiveCreateSettings& Settings);
storage::Result<QJsonObject> RunArchiveWorker(QWidget* Parent, const QString& Title, std::function<storage::Result<QJsonObject>(storage::OperationContext)> Work);
storage::Result<QJsonObject> ConfigureCreator(QWidget* Parent, CreatorRuntime Creator);
storage::Result<storage::Unit> ValidateIsoSettings(const plugins::IsoCreateSettings& Settings);
}
