#pragma once
#include <QDir>
#include <QString>

namespace hc::search {
inline bool IndexParentMatches(const QString& ResultPath, const QString& Parent)
{
    auto Path = QDir::fromNativeSeparators(ResultPath);
    // Everything reports drive-root parents as "D:"; this SDK value denotes
    // the root, not the drive-relative path that Qt normally assigns to it.
    if (Path.size() == 2 && Path[0].isLetter() && Path[1] == ':') Path += '/';
    return QDir::cleanPath(Path).compare(QDir::cleanPath(QDir::fromNativeSeparators(Parent)), Qt::CaseInsensitive) == 0;
}
}
