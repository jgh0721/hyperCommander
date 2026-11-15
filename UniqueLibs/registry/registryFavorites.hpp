#pragma once

/// 공통 라이브러리
#include "commonLibs/system/registryFile.hpp"

namespace hc::registry {
struct RegistryFavorite { QString name; RegistryLocation location; };
// Pure snapshot transformation. The caller reads asynchronously and submits the
// returned operations through the same durable transaction path as other edits.
// Tests inject a dedicated location; they never write the user's Regedit data.
class RegistryFavorites final
{
public:
    static RegistryLocation SharedLocation();
    static QList<RegistryFavorite> Decode(const KeySnapshot& Snapshot, View SelectedView);
    static QList<RegistryOperation> Toggle(const RegistryLocation& Store, const KeySnapshot& Snapshot,
        const RegistryLocation& Target);
};
} // namespace hc::registry
