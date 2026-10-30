#pragma once
#include "errorPresentation.hpp"

namespace hc::ui
{
    // Compatibility entry point; all classification and formatting is shared.
    inline QString FileOperationErrorText(const std::string& Code) { return ErrorText(Code); }
}
