#pragma once
#include "updateProtocol.hpp"

namespace hc::update {
void CheckInstallRoot(const Path& Root);
Path ExecutableRoot();
void CheckProductRelative(std::string_view Relative);
bool StartupAllowed(const Path& Root);
enum class ProductProcessCheck { Strict, BeforeRestart };
bool OtherProductProcesses(const Path& Root, unsigned IgnorePid = 0, ProductProcessCheck Check = ProductProcessCheck::Strict);
void DurableWrite(const Path& File, std::string_view Bytes);
void ApplyTransaction(const Path& Root, const Path& Transaction, unsigned ParentPid, std::uint64_t ParentCreated,
    std::wstring_view Handoff = {}, bool* HandoffReady = nullptr);
void RecoverTransaction(const Path& Root);
void AcknowledgeTransaction(const Path& Root);
}
