#pragma once

#include <atomic>

/// PlatQt 의 폭 측정 호출 횟수.
///
/// Scintilla 는 등폭 ASCII 지름길이 **실제로 걸렸는지** 알려주는 API 를 주지
/// 않는다. `SCI_STYLEGETCHECKMONOSPACED` 는 우리가 넣은 요청 값을 되돌려줄 뿐이고,
/// 판정 결과인 `Style::monospaceASCII` 에는 getter 가 없다.
///
/// 블랙박스로 알아내는 것도 원리적으로 불가능하다 — 판정이 "등폭" 으로 나온
/// 폰트에서는 지름길과 플랫폼 측정이 **정의상 같은 값**을 낸다. 구별할 관측이
/// 없다. 그래서 플랫폼 쪽에 호출 횟수를 세어 둔다. 지름길이 걸리면 이 값이
/// 늘지 않는다.
///
/// `#ifdef` 로 감싸지 않는다. 테스트가 배포본과 다른 바이너리를 검증하게 되는
/// 위험이, 세그먼트당 relaxed 증가 한 번(≈1ns, 호출 자체는 ≈20µs)보다 나쁘다.
namespace mrst::scintilla {

std::atomic< unsigned long long >& measureWidthsCallCount() noexcept;

/// 등폭(ASCII + 전각 CJK) 지름길을 켜고 끈다. 기본은 켜짐.
///
/// 테스트가 **한 프로세스 안에서** 두 경로의 문자 위치를 견주려고 쓴다. 그
/// 비교가 이 최적화의 유일한 안전장치다 — 폭이 틀리면 캐럿과 선택 범위가
/// 어긋나는데, 그건 눈으로 보기 전에는 알 수 없고 눈으로 봐도 원인을 짚기
/// 어렵다. 운영 중에도 `MRST_PLATQT_LEGACY_MEASURE` 로 끌 수 있다.
void setMonospaceFastPathEnabled( bool enabled ) noexcept;
bool monospaceFastPathEnabled() noexcept;

}  // namespace mrst::scintilla
