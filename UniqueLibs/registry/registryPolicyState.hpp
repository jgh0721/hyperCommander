#pragma once
/// 고유 라이브러리
#include "registryPolicyDefinition.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QHash>

namespace hc::registry {
struct PolicyObservation
{
    PolicyState state = PolicyState::Unknown;
    QVariantMap options;
    QMap<QString, RegistryResult<KeySnapshot>> keys;
    bool present = false, local = false;
    QString diagnostic;
};
// Worker-only, shared enumeration cache. No registry writes and no UI access.
class RegistryPolicyInspector
{
public:
    explicit RegistryPolicyInspector(IRegistryBackend& Backend, const std::atomic_bool* Cancel = nullptr)
        : backend_(Backend), cancel_(Cancel) {}
    PolicyObservation Inspect(const AdmxPolicy& Policy, bool Machine, const QList<RegistryPolRecord>& LocalRecords);
private:
    IRegistryBackend& backend_;
    const std::atomic_bool* cancel_;
    QHash<QString, RegistryResult<KeySnapshot>> cache_;
};
} // namespace hc::registry
