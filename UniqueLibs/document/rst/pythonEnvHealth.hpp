#pragma once

#include <QString>

namespace hc::rst {

/// venv launcher 또는 그 기반 Python 이 사라진 상태를 설명한다.
/// 손상이 없거나 정적 파일 검사만으로 판정할 수 없으면 빈 문자열을 돌려준다.
[[nodiscard]] QString                   pythonVenvDamageReason( const QString& venvDir );

/// Python 프로세스 결과가 인터프리터 자체의 손상을 뜻하는가.
/// 일반 Sphinx 빌드 오류와 구분해 내장 환경 재시도 여부를 결정한다.
[[nodiscard]] bool                      pythonFailureIndicatesBrokenEnvironment(
                                            int exitCode, bool crashed, const QString& output );

}  // namespace hc::rst
