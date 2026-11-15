#pragma once
/// 공통 라이브러리
#include "registryPolicyFile.hpp"

namespace hc::registry {
// Accesses only the local machine/user GPO. No caller-supplied filesystem path,
// remote GPO, arbitrary SID or command line is accepted by this interface.
class LocalPolicyStore
{
public:
    static RegistryResult<QByteArray> Read(bool Machine);
    static RegistryResult<QByteArray> Canonical(const QByteArray& Bytes);
    static RegistryResult<QByteArray> Apply(bool Machine, const QByteArray& Expected, const QByteArray& Desired);
    // Run in the original caller process so user policy refresh targets its logon.
    // Completion is the exit of gpupdate /wait:-1; timeout/cancel never imply success.
    static quint32 Refresh(bool Machine, const std::atomic_bool* Cancel = nullptr, int TimeoutMs = 120000);
};
} // namespace hc::registry
