#pragma once

#include <QString>

namespace hc::rst {

/// 프리뷰 빌더가 진행률을 실어 보낼 때 줄 앞에 붙이는 표식.
///
/// stdout 은 로그 패널로 그대로 흘러가는 통로다. 표식을 붙여 두면 컨트롤러가
/// 진행률 줄만 걷어내 로그를 더럽히지 않고 쓸 수 있다.
inline constexpr auto kPreviewProgressTag = "@@MRST-PROGRESS ";

/// 빌더가 흘려보낸 진행률 한 줄.
struct PreviewBuildProgress
{
    /// 표식이 붙은 줄이고 내용도 읽혔는가.
    bool    valid = false;
    /// `read`(원본 읽기) 또는 `write`(HTML 쓰기). 그 밖의 값은 무시해도 된다.
    QString phase;
    int     done  = 0;
    int     total = 0;
};

/// 빌더 출력 한 줄을 진행률로 읽는다. 표식이 없으면 `valid == false` 다.
///
/// 깨진 JSON 도 `valid == false` 로 돌려준다. 빌더는 사용자 프로젝트의 venv 에서
/// 도는 남의 Sphinx 와 섞여 돌아가므로, 출력이 늘 우리가 기대하는 모양이라고
/// 가정하지 않는다.
[[nodiscard]] PreviewBuildProgress parsePreviewProgressLine( const QString& line );

/// 프리뷰 한 판이 지나는 단계.
///
/// 단계마다 전체 진행도의 구간을 따로 가진다. 그래야 단계가 넘어갈 때 막대가
/// 뒤로 가지 않는다 — 각 단계가 0~100% 를 자기 몫으로 쓰면 "읽기 90%" 다음에
/// "쓰기 5%" 가 와서 막대가 되돌아간다.
enum class PreviewPhase
{
    Prepare,      ///< 프리뷰 준비 중. 빌드가 시작되기 전.
    BuildRead,    ///< Sphinx 가 원본을 읽는 중.
    BuildWrite,   ///< Sphinx 가 HTML 을 쓰는 중.
    Load,         ///< 만들어진 HTML 을 WebEngine 이 읽는 중.
};

/// 그 단계의 `done/total` 을 프리뷰 한 판 전체에 대한 진행도로 옮긴다.
///
/// 0..1000(천분율)을 돌려준다. 백분율로는 구간이 좁아 막대가 뚝뚝 끊긴다.
/// `total <= 0` 이면 그 단계의 시작값을 돌려준다 — 아직 분모를 모르는 것이지
/// 진행도를 모르는 것은 아니다.
[[nodiscard]] int previewOverallPermille( PreviewPhase phase, int done, int total );

/// 빌더가 보내는 `phase` 문자열을 단계로 옮긴다. 모르는 값은 `BuildRead` 다 —
/// 빌드 중에 온 것이므로 그 구간에 두는 것이 진행도를 되돌리지 않는 쪽이다.
[[nodiscard]] PreviewPhase previewPhaseFromTag( const QString& tag );

}  // namespace hc::rst
