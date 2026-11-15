#pragma once
/// 고유 라이브러리
#include "registryPolicyState.hpp"
#include "registryHistory.hpp"

namespace hc::registry {
struct RegistryPolicyEditPlan
{
    QByteArray beforePolicy, afterPolicy;
    QList<RegistryOperation> operations;
    QList<RegistryStep> expectedValues;
    QString error;
    explicit operator bool() const { return error.isEmpty(); }
};
class RegistryPolicyEdit
{
public:
    // Builds a preview from the observation shown to the user, with no writes.
    static RegistryPolicyEditPlan Prepare(const AdmxPolicy& Policy, bool Machine, PolicyState State,
        const QVariantMap& Options, const PolicyObservation& Observation, const QByteArray& LocalPolicy);
};
} // namespace hc::registry
