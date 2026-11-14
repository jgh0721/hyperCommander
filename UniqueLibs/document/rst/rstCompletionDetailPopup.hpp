#pragma once

#include "UniqueLibs/document/completion/completionDetailPopup.hpp"

namespace hc::rst {

/// 상세 패널도 자동완성 목록과 함께 공용(UniqueLibs/document/completion)으로 옮겼다. reST 코드는 이 이름을 그대로 쓴다.
using CompletionFileDetail = hc::document::CompletionFileDetail;
using CompletionDetailPopup = hc::document::CompletionDetailPopup;

}  // namespace hc::rst
