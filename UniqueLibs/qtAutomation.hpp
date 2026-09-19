#pragma once
#include <QString>
#include "commonLibs/commands/automation.hpp"
class QWidget;
namespace hc::automation {
struct Diagnostic { qsizetype line_ = 0, column_ = 0; QString message_; };
storage::Result<Program> Compile(const QString& Source, Diagnostic* Diagnostic = nullptr);
void Show(QWidget* Parent, std::function<Host()> MakeHost);
}
