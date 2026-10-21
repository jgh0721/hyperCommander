#pragma once

#include <QString>

class QLineEdit;

namespace hc::ui {
// BaseDirectory is captured from the operation's panel, never the process cwd.
void InstallNativePathCompletion(QLineEdit* Editor, const QString& BaseDirectory, bool IncludeFiles = true);
}
