#pragma once
/// 고유 라이브러리
#include "UniqueLibs/registry/registryBatch.hpp"
class QWidget;
namespace hc::ui {
std::optional<QString> AskRegistryReplacement(QWidget* Parent);
bool ConfirmRegistryReplacement(QWidget* Parent, hc::registry::RegistryBatchPreview& Preview);
}