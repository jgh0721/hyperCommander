#pragma once

/// 공통 라이브러리
#include "commonLibs/system/registryAccess.hpp"
#include <QStringList>
class QWidget;
namespace hc::ui {
std::optional<QString> AskRegistryName(QWidget* Parent, const QString& Title, const QString& Initial, bool Key);
std::optional<hc::registry::RegistryValue> AskRegistryNewValue(QWidget* Parent);
bool ConfirmRegistryDelete(QWidget* Parent, const QStringList& Names);
}
