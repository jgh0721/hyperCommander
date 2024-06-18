#pragma once

#include "cmnConcurrent.hpp"


class CIconCache
{
public:
    static constexpr auto NORMAL_DRIVE      = ":NormalDrive";
    static constexpr auto NORMAL_DIRECTORY  = ":NormalDirectory";
    static constexpr auto NORMAL_FILE       = ":NormalFile";
    static constexpr auto PROGRESS_MASK     = ":ProgressMask";

private:

    QReadWriteLock              Lock;
    QCache< QString, QPixmap >  Values;
};

using TyStIconCache = nsCmn::nsConcurrent::TSingleton< CIconCache >;
