#include "documentHandlerRegistry.hpp"
#include <algorithm>
namespace hc::document {
bool DocumentHandlerRegistry::Register(HandlerDescriptor Handler) {
    if (Handler.stableId_.empty() || Find(Handler.stableId_)) return false;
    handlers_.push_back(std::move(Handler)); return true;
}
const HandlerDescriptor* DocumentHandlerRegistry::Find(const std::string& Id) const {
    const auto At = std::find_if(handlers_.begin(), handlers_.end(), [&](const auto& Item) { return Item.stableId_ == Id; });
    return At == handlers_.end() ? nullptr : &*At;
}
}
