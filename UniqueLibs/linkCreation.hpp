#pragma once
#include "nativeLinks.hpp"
#include "commonLibs/operations/fileElevation.hpp"

namespace hc::links {
struct Request {
    QStringList sources_;
    QString destination_;
    Kind kind_ = Kind::Symbolic;
    bool targetIsName_ = false, relative_ = false, createParents_ = true;
    QString conflict_ = "ask";
    std::shared_ptr<operations::IFileElevationService> elevation_;
};
operations::PreparedJob Prepare(Request Value);
}
