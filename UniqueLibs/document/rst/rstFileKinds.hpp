#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include <set>
#include <string>

/// 파일 종류에 대한 앱 전체의 단일 출처.
///
/// 확장자 목록과 "훑을 때 건너뛸 디렉터리" 목록이 네 곳에 따로 있었고 이미 서로
/// 어긋나 있었다 (`.tox` 는 검색에만, `.mypy_cache` 는 스캐너에만, `env` 는 검색에만
/// 있었다). 경로 자동완성이 같은 판단을 다섯 번째로 하게 되므로 여기로 모은다.
///
/// 목록이 어긋나면 증상이 고약하다: `_build/` 를 한 곳에서만 빼면 그 경로가 후보로
/// 올라오고, 사용자가 그것을 고르면 다음 빌드에서 산출물이 지워져 링크가 깨진다.
namespace hc::rst::filekinds {

/// 아래 목록은 전부 **소문자, 점 없음** 이다 ("png", "rst").
/// `QFileInfo::suffix()` 와 바로 비교할 수 있게 맞춘 것이다.

/// 브라우저와 Sphinx 가 함께 다룰 수 있는 이미지.
[[nodiscard]] const QStringList&             imageExtensions();
[[nodiscard]] const QStringList&             markdownExtensions();
/// reStructuredText 본문. `.txt` 는 넣지 않는다 — Sphinx 는 source_suffix 에
/// 따라 문서로 읽을 수 있지만, 대부분의 `.txt` 는 문서가 아니다.
[[nodiscard]] const QStringList&             restructuredTextExtensions();
/// Sphinx 가 문서로 읽는 것 (toctree / :doc: 후보).
[[nodiscard]] const QStringList&             documentExtensions();
/// literalinclude 처럼 "글자로 읽는" 파일. 후보를 **거르는** 데 쓰지 말고
/// 순위를 매기는 데만 쓸 것 — literalinclude 자체는 무엇이든 받는다.
[[nodiscard]] const QStringList&             textLikeExtensions();

/// 트리를 훑을 때 통째로 건너뛸 디렉터리 이름 (소문자).
[[nodiscard]] const QSet< QString >&         excludedScanDirectories();
/// 같은 목록의 std 판. Qt 를 쓰지 않는 ProjectScanner 용.
[[nodiscard]] const std::set< std::string >& excludedScanDirectoriesNarrow();

/// 대소문자를 구분하지 않고 확장자를 비교한다.
[[nodiscard]] bool                           hasExtension( const QString& path,
                                                           const QStringList& extensions );
[[nodiscard]] bool                           isImageFile( const QString& path );
[[nodiscard]] bool                           isExcludedDirectoryName( const QString& name );

/// `/` 로 구분된 **상대** 경로의 디렉터리 성분 중 제외 대상이 있는가.
/// 마지막 성분(파일 이름)은 보지 않는다. 임의 깊이에서 걸러야 한다 —
/// 실제 프로젝트는 `본편/1권/source/_build/…/_images/` 처럼 깊은 곳에
/// 원본 이미지의 사본을 만들어 둔다.
[[nodiscard]] bool                           isUnderExcludedDirectory( const QString& relativePath );

}   // namespace hc::rst::filekinds
