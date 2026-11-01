#pragma once
#include "updateProtocol.hpp"

#include <chrono>
#include <functional>

namespace hc::update {
/// A ROOT file copied from its carried copy (layout::RootCarrier), by name and SHA-256.
struct RootCopy { std::string name_, hash_; };
/// Runs Attempt until it succeeds, retrying sharing, lock and access errors for up to 5 seconds.
/// The last Win32 error stays set when it fails.
bool Retried(const std::function<bool()>& Attempt);
void CheckInstallRoot(const Path& Root);
/// The stored spelling of Launched when it differs only in letter case or 8.3 names.
Path InstallRoot(const Path& Launched);
Path ExecutableRoot();
void CheckProductRelative(std::string_view Relative);
bool StartupAllowed(const Path& Root);
enum class ProductProcessCheck { Strict, BeforeRestart };
bool OtherProductProcesses(const Path& Root, unsigned IgnorePid = 0, ProductProcessCheck Check = ProductProcessCheck::Strict);
/// True when any process runs the executable File.
bool ImageRunning(const Path& File);
/// While an applied update awaits its health check, records Copies in its journal so that
/// any recovery restores the pre-update ROOT, then runs Place under the root lock.
/// Returns false without running Place when no update is active. Throws
/// "update-promotion-pending" when the transaction cannot take the copies yet.
bool PlaceWithinTransaction(const Path& Root, const std::vector<RootCopy>& Copies, const std::function<void()>& Place);
void DurableWrite(const Path& File, std::string_view Bytes);
void ApplyTransaction(const Path& Root, const Path& Transaction, unsigned ParentPid, std::uint64_t ParentCreated,
    std::wstring_view Handoff = {}, bool* HandoffReady = nullptr);
void RecoverTransaction(const Path& Root);
void AcknowledgeTransaction(const Path& Root);
/// Removes transactions that were never applied: Own at any age, others older than MinimumAge.
void PruneTransactions(const Path& Root, const Path& Own, std::chrono::seconds MinimumAge);
}
