#pragma once

#include <QString>
#include <QStringList>

namespace hc::rst {

/// 빌드 대화상자의 콤보에 채우는 빌더 목록.
///
/// Sphinx 가 기본 제공하는 것은 이보다 훨씬 많지만(htmlhelp · qthelp · devhelp ·
/// texinfo · gettext · linkcheck …) 전부 늘어놓으면 정작 쓰는 넷이 묻힌다.
/// 목록에 없는 빌더는 콤보에 직접 입력한다 — 그래서 콤보가 편집 가능하다.
[[nodiscard]] QStringList sphinxBuilderPresets();

/// 빌더 이름이 명령행 인자 하나로 안전한가.
///
/// 편집 가능한 콤보의 내용이 그대로 `-b` 뒤에 붙으므로 여기서 막는다.
/// 영숫자 · 하이픈 · 밑줄만 받는다 (실제 Sphinx 빌더 이름의 문법이다).
[[nodiscard]] bool isValidSphinxBuilderName( const QString& builder );

/// `-b` 로는 없는 이름이라 make 모드(`-M`)로 돌려야 하는 목표인가.
///
/// `latexpdf` 는 빌더가 아니라 Makefile 목표다 — `sphinx-build -b latexpdf` 는
/// "Builder name latexpdf not registered" 로 죽는다. 그런데 사람들이 가장 먼저
/// 치는 이름이 그것이라 목록에서 뺄 수는 없어서, 이 이름들만 `-M` 으로 돌린다.
[[nodiscard]] bool isSphinxMakeModeTarget( const QString& builder );

/// make 모드가 산출물을 놓는 하위 폴더 이름.
///
/// `-M` 은 언제나 `<출력 위치>/<무언가>` 아래에 쓰는데 그 이름이 목표 이름과
/// 같지 않다 — `latexpdf` 는 `latex/`, `info` 는 `texinfo/` 다. 빌드가 끝난 뒤
/// 탐색기로 열어 줄 자리가 이 값에 달려 있다.
/// make 모드 목표가 아니면 빈 문자열.
[[nodiscard]] QString sphinxMakeModeSubdirectory( const QString& builder );

/// `<프로젝트 루트>/_build/<빌더>`. Sphinx quickstart 가 만드는 배치다.
///
/// 프로젝트 루트는 conf.py 가 있는 디렉터리다(ProjectScanner 가 그렇게 채운다).
[[nodiscard]] QString defaultSphinxOutputDirectory( const QString& projectRoot,
                                                    const QString& builder );

}  // namespace hc::rst
