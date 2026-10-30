#pragma once

#include "commonLibs/storage/nativePath.hpp"
#include <QDir>
#include <QString>

namespace hc::storage::native
{
    inline QString FilePath(const QString& Path)
    {
        return QString::fromStdWString(ExtendedPath(QDir::cleanPath(Path).toStdWString()));
    }
}
