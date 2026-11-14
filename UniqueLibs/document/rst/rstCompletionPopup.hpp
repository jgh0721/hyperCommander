#pragma once

#include "UniqueLibs/document/completion/completionPopup.hpp"

namespace hc::rst {

/// 자동완성 목록은 SQLite 쿼리 편집기와 함께 쓰도록 공용(UniqueLibs/document/completion)으로 옮겼다(SQLite 설계 261113 5.2).
/// reST 코드는 이 이름을 그대로 쓴다.
using CompletionDisplayItem = hc::document::CompletionDisplayItem;
using CompletionPopupWidget = hc::document::CompletionPopupWidget;

}  // namespace hc::rst
