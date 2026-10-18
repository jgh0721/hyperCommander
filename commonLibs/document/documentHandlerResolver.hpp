#pragma once
#include "documentHandlerRegistry.hpp"
#include <map>
namespace hc::document {
struct HandlerSelection {
    std::vector<std::string> names_;
    bool directory_ = false, providerSupported_ = true;
    std::vector<bool> textContent_;
};
struct HandlerResolution {
    std::vector<std::string> candidates_;
    std::map<std::string, std::vector<std::size_t>> groups_;
    std::string reason_;
};
class DocumentHandlerResolver {
public:
    static HandlerResolution Resolve(const DocumentHandlerRegistry& Registry, DocumentIntent Intent,
        const HandlerSelection& Selection, const std::string& DefaultId = {});
    static bool Supports(const HandlerDescriptor& Handler, DocumentIntent Intent, const std::vector<std::string>& Names,
        const std::vector<bool>& TextContent = {});
};
}
