#pragma once
#include "environmentTypes.hpp"
#include <atomic>
#include <memory>

namespace hc::environment {
// Only product startup publishes this snapshot. Other executables and test
// runtimes keep their normal inherited environment until explicitly configured.
inline std::atomic<std::shared_ptr<const ValueMap>> childEnvironmentSnapshot;
inline void PublishChildEnvironment(ValueMap Values)
{
    childEnvironmentSnapshot.store(std::make_shared<const ValueMap>(std::move(Values)));
}
inline std::shared_ptr<const ValueMap> ChildEnvironmentSnapshot()
{
    return childEnvironmentSnapshot.load();
}
inline std::vector<wchar_t> NativeChildEnvironment()
{
    const auto Snapshot = ChildEnvironmentSnapshot(); if (!Snapshot) return {};
    std::vector<wchar_t> Result;
    for (const auto& [Name, Text] : *Snapshot) {
        Result.insert(Result.end(), Name.begin(), Name.end()); Result.push_back(L'=');
        Result.insert(Result.end(), Text.begin(), Text.end()); Result.push_back(L'\0');
    }
    Result.push_back(L'\0'); if (Snapshot->empty()) Result.push_back(L'\0');
    return Result;
}
}
