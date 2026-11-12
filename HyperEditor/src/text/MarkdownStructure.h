#pragma once

// Qt 비의존 순수 모듈이다. RstContainerLexer 와 같은 성격이고, 같은 이유로
// stdafx.h 를 포함하지 않는다 — 테스트 타깃에서 Qt Widgets/WebEngine 없이 그대로
// 컴파일된다.
//
// FoldLine 을 hc::rst::rst 에서 가져온다. 접기 깊이를 Scintilla 에 주입하는 루프를
// reST 와 공유하려면 타입이 하나여야 한다. 이름이 rst 로 남은 것은 냄새이지만,
// 타입을 둘로 나누면 주입 루프도 둘이 되고 그 둘이 갈라지는 것이 더 나쁘다.
#include "text/RstContainerLexer.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hc::rst::md {

/// 문서에서 찾은 제목 하나.
struct MdHeading
{
    std::string text;            ///< 마커(`#`, 닫는 `#`)를 벗긴 제목 글자
    int         level = 1;       ///< 1~6
    std::size_t line = 1;        ///< 1-based. **제목 글자가 있는 줄**
    bool        setext = false;  ///< `===` / `---` 밑줄로 만든 제목
};

/// 한 번 훑어 제목과 접기 깊이를 함께 낸다.
///
/// 둘을 한 함수에서 내는 이유: 코드펜스와 front matter 판정을 두 번 구현하면
/// 반드시 갈라진다. 그러면 개요에는 없는 제목이 접기 마커를 만들거나 그 반대가
/// 되어, 증상이 "접기가 이상하다" 로 나타나면서 원인은 개요 쪽에 있게 된다.
struct MdScan
{
    std::vector< MdHeading >           headings;
    std::vector< hc::rst::syntax::FoldLine > folds;
};

[[nodiscard]] MdScan scanMarkdown( const std::string& utf8Text );

/// 접기 깊이만 필요한 호출자용. 내부는 scanMarkdown() 과 같다.
[[nodiscard]] std::vector< hc::rst::syntax::FoldLine > computeMarkdownFoldLevels( const std::string& utf8Text );

}   // namespace hc::rst::md
