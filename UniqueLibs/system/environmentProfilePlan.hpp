#pragma once
#include "environmentStorage.hpp"

namespace hc::environment {
using ProfileBaseline = std::map<std::wstring, std::optional<Value>, NameLess>;
struct ProfileUpdatePlan
{
    QList<EnvironmentChange> changes_;
    ProfileBaseline baseline_;
    quint32 error_ = 0;
    explicit operator bool() const { return !error_; }
};
// Definitions and original values are synthetic/in-memory inputs here. The
// storage preview subsequently compares every expected value with the backend.
inline ProfileUpdatePlan BuildProfileUpdate(const std::vector<Value>& Old, const std::vector<Value>& Next,
    const ProfileBaseline& Baseline, const std::vector<Value>& Current)
{
    ProfileUpdatePlan Result; Result.baseline_ = Baseline;
    std::map<std::wstring, Value, NameLess> Before, After, Registry;
    const auto Insert = [](const std::vector<Value>& Values, auto& Map) {
        for (const auto& Item : Values) if (Item.scope_ != Scope::User || ValidateValue(Item) || !Map.emplace(Item.name_, Item).second) return false;
        return true;
    };
    if (!Insert(Old, Before) || !Insert(Next, After) || !Insert(Current, Registry)) { Result.error_ = ERROR_INVALID_DATA; return Result; }
    for (const auto& [Name, Item] : Before) {
        const auto Original = Baseline.find(Name);
        if (Original == Baseline.end()) { Result.error_ = ERROR_INVALID_DATA; return Result; }
        if (const auto Found = After.find(Name); Found != After.end()) Result.changes_.append({Scope::User, QString::fromStdWString(Name), Item, Found->second});
        else {
            Result.changes_.append({Scope::User, QString::fromStdWString(Name), Item, Original->second});
            Result.baseline_.erase(Name);
        }
    }
    for (const auto& [Name, Item] : After) if (!Before.contains(Name)) {
        const auto Found = Registry.find(Name);
        const auto Original = Found == Registry.end() ? std::nullopt : std::optional(Found->second);
        Result.changes_.append({Scope::User, QString::fromStdWString(Name), Original, Item}); Result.baseline_[Name] = Original;
    }
    return Result;
}
}
