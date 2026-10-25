#pragma once

#include "commonLibs/storage/storage.hpp"
#include <QString>
#include <string_view>

namespace hc::ui
{
    inline bool IsDriveList(const storage::Locator& Location)
    {
        if (Location.kind_ != storage::LocatorKind::Shell) return false;
        const auto Root = QString::fromStdString(Location.root_);
        return Root.compare("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", Qt::CaseInsensitive) == 0
            || Root.compare("shell:MyComputerFolder", Qt::CaseInsensitive) == 0
            || Root.compare("shell:ComputerFolder", Qt::CaseInsensitive) == 0;
    }

    // Use the Shell filesystem path, never a localized label containing "(X:)".
    // No filesystem or network queries are needed to identify a drive root.
    inline int DriveLetterSortKey(const storage::EntrySnapshot& Entry)
    {
        const auto It = Entry.hints_.find("filesystemPath");
        auto Path = std::string_view(It != Entry.hints_.end() ? It->second : Entry.locator_.root_);
        if (Path.starts_with("\\\\?\\")) Path.remove_prefix(4);
        if ((Path.size() != 2 && Path.size() != 3) || Path[1] != ':'
            || (Path.size() == 3 && Path[2] != '\\' && Path[2] != '/')) return 26;
        const auto Letter = Path[0];
        if (Letter >= 'A' && Letter <= 'Z') return Letter - 'A';
        if (Letter >= 'a' && Letter <= 'z') return Letter - 'a';
        return 26;
    }
}
