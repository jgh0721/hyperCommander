#pragma once

#include <QString>

namespace hc::rst {
/// 입력 파일과 템플릿 목록을 비교한다. 구 버전·알 수 없는 지문은 다시 빌드한다.
bool previewInputsChanged(const QString& inputsFile);

/// 입력 지문을 이만큼 쓰지 않으면 지운다. 지문은 "바뀐 것이 없으면 빌드를 건너뛴다" 는 최적화일 뿐이라,
/// 없어도 다음 미리보기가 빌드를 한 번 하고 다시 만든다.
inline constexpr int kPreviewInputsMaxAgeDays = 90;

/// 상태 폴더에서 쓸모없어진 입력 지문을 지운다: 그 프로젝트의 소스 폴더가 없어졌거나, maxAgeDays 넘게
/// 고쳐지지 않았거나, 읽을 수 없는 것. 지문은 프로젝트마다 하나씩 생기고 프로젝트 폴더가 아니라 환경
/// 폴더에 있어서, 프로젝트를 지우거나 옮겨도 따라 없어지지 않는다 — 그래서 여기서 걷어 낸다.
/// 이름이 `preview-inputs-*.json` 인 파일만 건드린다. 지운 개수를 돌려준다.
int prunePreviewInputs(const QString& stateDirectory, int maxAgeDays = kPreviewInputsMaxAgeDays);
} // namespace hc::rst
