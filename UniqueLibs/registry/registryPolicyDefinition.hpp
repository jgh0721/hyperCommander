#pragma once
/// 고유 라이브러리
#include "registryAdmx.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryPolicyFile.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVariantMap>

namespace hc::registry {
enum class PolicyState { NotConfigured, Enabled, Disabled, Partial, Unknown };
struct PolicyAssignment
{
    QString key, name;
    std::optional<RegistryValue> value;
    bool soft = false;
    bool explicitDeletion = false;
};
struct PolicyProgram
{
    QList<PolicyAssignment> assignments;
    QStringList clearKeys, additiveKeys;
    QString error;
    explicit operator bool() const { return error.isEmpty(); }
};
class RegistryPolicyDefinition
{
public:
    static PolicyProgram Compile(const AdmxPolicy& Policy, PolicyState State, const QVariantMap& Options);
    static QVariantMap Defaults(const AdmxPolicy& Policy);
    static QString OptionLabel(const AdmxPolicy& Policy, const AdmxNode& Element);
    static QString StateText(PolicyState State);
};
} // namespace hc::registry
