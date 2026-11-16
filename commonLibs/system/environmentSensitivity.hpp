#pragma once
#include "environmentMerge.hpp"

namespace hc::environment {
// Preserve dependency-based masking even when a displayed/history value is an
// expanded string whose own name contains no credential-related word.
inline std::set<std::wstring, NameLess> SensitiveVariables(const std::vector<Value>& Definitions)
{
    std::set<std::wstring, NameLess> Result;
    for (const auto& Item : Definitions) if (SensitiveName(Item.name_)) Result.insert(Item.name_);
    for (size_t Pass = 0; Pass <= Definitions.size(); ++Pass) {
        const auto Count = Result.size();
        for (const auto& Item : Definitions) for (const auto& Reference : References(Item.raw_))
            if (SensitiveName(Reference) || Result.contains(Reference)) Result.insert(Item.name_);
        if (Result.size() == Count) break;
    }
    return Result;
}
}
