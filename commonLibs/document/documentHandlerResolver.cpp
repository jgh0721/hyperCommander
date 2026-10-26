#include "documentHandlerResolver.hpp"
#include <algorithm>
#include <cctype>
namespace hc::document {
namespace {
std::string Lower(std::string Value) {
    std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Ch) { return char(std::tolower(Ch)); }); return Value;
}
bool Match(const std::string& Pattern, const std::string& Value) {
    std::size_t P = 0, V = 0, Star = std::string::npos, Retry = 0;
    while (V < Value.size()) {
        if (P < Pattern.size() && (Pattern[P] == '?' || Pattern[P] == Value[V])) { ++P; ++V; }
        else if (P < Pattern.size() && Pattern[P] == '*') { Star = P++; Retry = V; }
        else if (Star != std::string::npos) { P = Star + 1; V = ++Retry; }
        else return false;
    }
    while (P < Pattern.size() && Pattern[P] == '*') ++P;
    return P == Pattern.size();
}
std::string Extension(const std::string& Name) {
    const auto Dot = Name.find_last_of('.'); return Dot == std::string::npos ? "" : Lower(Name.substr(Dot + 1));
}
}
bool DocumentHandlerResolver::Supports(const HandlerDescriptor& Handler, DocumentIntent Intent, const std::vector<std::string>& Names,
    const std::vector<bool>& TextContent) {
    if (!Handler.available_ || Names.empty() || Names.size() > Handler.maxFiles_
        || (Intent == DocumentIntent::View ? !Handler.view_ : !Handler.edit_ || !Handler.editable_)) return false;
    return std::all_of(Names.begin(), Names.end(), [&](const auto& Name) {
        const auto Index = std::size_t(&Name - Names.data());
        const auto Ext = Extension(Name);
        if (std::any_of(Handler.excludedExtensions_.begin(), Handler.excludedExtensions_.end(), [&](const auto& Value) { return Lower(Name).ends_with('.' + Lower(Value)); })) return false;
        const auto Matches = [&](const std::vector<std::string>& Patterns) {
            return std::any_of(Patterns.begin(), Patterns.end(), [&](auto Pattern) {
                Pattern = Lower(Pattern); if (Pattern.starts_with("*.")) Pattern.erase(0, 2); else if (Pattern.starts_with('.')) Pattern.erase(0, 1);
                return Match(Pattern, Ext);
            });
        };
        const bool Registered = Handler.registeredExtensions_.empty() || Matches(Handler.registeredExtensions_);
        return Registered && (Matches(Handler.extensions_)
            || (Handler.stableId_ == "builtin.hyperviewer" && Index < TextContent.size() && TextContent[Index]));
    });
}
HandlerResolution DocumentHandlerResolver::Resolve(const DocumentHandlerRegistry& Registry, DocumentIntent Intent,
    const HandlerSelection& Selection, const std::string& DefaultId) {
    HandlerResolution Result;
    if (Selection.directory_) { Result.reason_ = "document-directory"; return Result; }
    if (!Selection.providerSupported_) { Result.reason_ = "document-provider-unsupported"; return Result; }
    if (Selection.names_.empty()) { Result.reason_ = "document-selection-empty"; return Result; }
    for (const auto& Handler : Registry.Handlers())
        if (Supports(Handler, Intent, Selection.names_, Selection.textContent_)) Result.candidates_.push_back(Handler.stableId_);
    const auto Default = std::find(Result.candidates_.begin(), Result.candidates_.end(), DefaultId);
    if (Default != Result.candidates_.end()) std::rotate(Result.candidates_.begin(), Default, Default + 1);
    if (!Result.candidates_.empty()) return Result;
    for (std::size_t Index = 0; Index < Selection.names_.size(); ++Index) Result.groups_[Extension(Selection.names_[Index])].push_back(Index);
    // Every input remains visible, including a format for which no handler exists.
    Result.reason_ = "document-no-common-handler"; return Result;
}
}
