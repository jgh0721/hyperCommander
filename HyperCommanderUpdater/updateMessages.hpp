#pragma once
#include "updateTrust.hpp"

namespace hc::update {
/// What the user can do about an update error code, or empty when the code needs no advice.
std::wstring Explain(std::string_view Error, const Path& Root);
}
