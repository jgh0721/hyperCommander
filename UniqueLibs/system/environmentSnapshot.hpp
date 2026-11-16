#pragma once
#include "commonLibs/system/environmentMerge.hpp"
#include "commonLibs/system/registryAccess.hpp"

namespace hc::environment {
struct EnvironmentSnapshot
{
    MergeInput input_;
    MergeResult merged_;
    quint32 userError_ = 0, systemError_ = 0;
    QStringList unsupported_;
};
// Only product code constructs a native backend. Tests pass a fake backend and
// synthetic defaults; constructing the pure model never touches process state.
EnvironmentSnapshot LoadEnvironmentSnapshot(registry::IRegistryBackend& Backend, MergeInput Defaults, const QString& CallerSid);
MergeInput ReadWindowsEnvironmentDefaults();
ValueMap ReadOwnEnvironment();
ValueMap ReconcileEnvironment(const ValueMap& Inherited, const ValueMap& LastModel,
    const ValueMap& NextModel, const ValueMap& Current, const ValueMap& Overlay);
}
