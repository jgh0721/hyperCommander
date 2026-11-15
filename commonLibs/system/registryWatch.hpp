#pragma once
#include "registryAccess.hpp"
#include <functional>
#include <thread>

namespace hc::registry {
// Callback runs on the watcher thread. Owners queue immutable notifications to
// their service and stop the watcher from a different thread before destruction.
class RegistryWatcher final
{
public:
    using Callback = std::function<void(quint32 Error)>;
    RegistryWatcher();
    ~RegistryWatcher();
    RegistryWatcher(const RegistryWatcher&) = delete;
    RegistryWatcher& operator=(const RegistryWatcher&) = delete;
    bool Start(const RegistryLocation& Location, bool Subtree, Callback Notify);
    void Stop();
private:
    HANDLE stop_ = nullptr;
    std::thread worker_;
};
} // namespace hc::registry
