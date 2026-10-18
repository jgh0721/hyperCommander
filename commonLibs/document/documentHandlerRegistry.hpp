#pragma once
#include <string>
#include <vector>
namespace hc::document {
enum class DocumentIntent { View, Edit };
enum class HandlerKind { Plugin, Executable };
enum class FileDelivery { OnePerProcess, ArgumentList, Workspace };
struct HandlerDescriptor {
    std::string stableId_, name_;
    HandlerKind kind_ = HandlerKind::Executable;
    FileDelivery delivery_ = FileDelivery::OnePerProcess;
    std::vector<std::string> extensions_;
    bool view_ = false, edit_ = false, editable_ = false, available_ = false;
    std::size_t maxFiles_ = 1;
    std::string unavailableReason_;
    std::vector<std::string> excludedExtensions_;
    std::vector<std::string> registeredExtensions_;
};
class DocumentHandlerRegistry {
public:
    bool Register(HandlerDescriptor Handler);
    const HandlerDescriptor* Find(const std::string& Id) const;
    const std::vector<HandlerDescriptor>& Handlers() const { return handlers_; }
private:
    std::vector<HandlerDescriptor> handlers_;
};
}
