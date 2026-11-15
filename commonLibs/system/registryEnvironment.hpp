#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>
#include <QStringList>
#include <QProcessEnvironment>

namespace hc::registry {
struct EnvironmentExpansion {
    QString text;
    QStringList missing;
    bool tooLarge = false;
};
// Pure, single-pass expansion. A caller supplies the target environment snapshot
// explicitly; the registry/environment editors can preview different users or
// pending environment drafts without modifying this process's environment.
EnvironmentExpansion ExpandEnvironmentText(const QString& Text, const QProcessEnvironment& Environment);
}
