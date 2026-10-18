#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hc::update {
struct Json {
    enum class Kind { Null, Boolean, Number, String, Object, Array };
    Kind kind_ = Kind::Null;
    bool boolean_ = false;
    std::uint64_t number_ = 0;
    std::string text_;
    std::map<std::string, Json, std::less<>> object_;
    std::vector<Json> array_;
    const Json& At(std::string_view Key, Kind Expected) const;
};
// Bounded, duplicate-key rejecting JSON for both the GUI and the Qt-free updater.
// The update wire contract uses unsigned integers, not floating point numbers.
Json ParseJson(std::string_view Bytes);
std::string QuoteJson(std::string_view Text);
}
