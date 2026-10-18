#pragma once
#include "documentOpenController.hpp"
namespace hc::document {
struct DocumentHandlerChoice { std::string handler_; std::vector<std::size_t> indexes_; bool configure_ = false; };
class DocumentHandlerMenu {
public:
    static DocumentHandlerChoice Choose(QWidget* Parent, const DocumentHandlerCatalog& Catalog,
        DocumentIntent Intent, const HandlerSelection& Selection, const HandlerResolution& Resolution);
};
}
