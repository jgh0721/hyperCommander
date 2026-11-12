#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QHash>
#include <QString>
#include <QStringList>

namespace hc::rst
{

/// 프로젝트가 스스로 밝힌 Python 패키지( Sphinx 테마 · 확장 ) 요구.
///
/// Sphinx 는 언제나 HyperEditor 가 구성한 환경의 Python 으로 돈다. 프로젝트 쪽의 가상환경이나
/// pyproject.toml · requirements 파일은 **실행하지 않고 읽기만** 한다 — 그 프로젝트가 어떤 테마와 확장을
/// 어느 버전으로 쓰는지 알아내어, 없는 것을 HyperEditor 환경에 설치할 때 쓴다.
struct ProjectRequirements
{
    /// 정규화한 배포명 -> 설치에 쓸 요구 문자열( 예: "sphinx-tabs==3.4.5", "furo>=2024.1" ).
    QHash< QString, QString >           specs;
    /// import 이름( 모듈 ) -> 정규화한 배포명. 프로젝트 가상환경의 top_level.txt 에서 읽는다.
    QHash< QString, QString >           modules;
    /// 읽은 파일 · 폴더( 로그용 ).
    QStringList                         sources;

    /// 배포명이나 모듈 이름으로 요구 문자열을 찾는다. 프로젝트가 밝힌 것이 없으면 빈 문자열.
    [[nodiscard]] QString               specFor( const QString& distributionOrModule ) const;
    [[nodiscard]] bool                  isEmpty() const { return specs.isEmpty() && modules.isEmpty(); }
};

/// PEP 503 정규화: 소문자, `-` `_` `.` 의 연속은 `-` 하나.
[[nodiscard]] QString normalizedDistributionName( const QString& name );

/// 요구 한 줄( PEP 508 )을 설치 명령에 넣어도 되는 모양으로 다듬는다: `이름[extras] 버전조건`.
/// 환경 표식( `; python_version < "3.9"` )과 주석은 떼고, URL · 경로 · 옵션( `-r` · `--index-url` ) ·
/// `이름 @ 주소` 처럼 PyPI 이름과 버전 조건이 아닌 것은 빈 문자열을 낸다 — 프로젝트 파일의 내용이
/// 설치 명령의 옵션이나 다른 출처로 흘러가지 않게 한다.
[[nodiscard]] QString sanitizedRequirement( const QString& line );
/// 다듬은 요구 문자열의 배포명( 정규화 전 ).
[[nodiscard]] QString requirementName( const QString& requirement );

/// pyproject.toml 의 `[project] dependencies`, `[project.optional-dependencies]`, `[dependency-groups]`
/// 배열에서 요구 문자열을 모은다. TOML 을 다 해석하지 않고 그 배열들의 문자열만 읽는다.
[[nodiscard]] QStringList requirementsFromPyproject( const QString& text );
/// requirements.txt 형식.
[[nodiscard]] QStringList requirementsFromRequirementsText( const QString& text );

/// 프로젝트 폴더와 그 조상( 저장소 뿌리까지 )의 선언 파일, 그리고 프로젝트 가상환경에 설치된 배포판을 읽는다.
/// 같은 배포판이 여러 곳에 있으면 선언 파일이 설치된 버전보다, 프로젝트에 가까운 파일이 먼 파일보다 앞선다.
/// venvDir 는 비어 있어도 된다.
[[nodiscard]] ProjectRequirements readProjectRequirements( const QString& projectRoot, const QString& venvDir );

/// 빌드가 없다고 알린 배포판 · 테마 이름을 프로젝트가 밝힌 요구로 바꾼다( 없으면 정규화한 이름 그대로 ).
/// 같은 배포판은 한 번만 낸다.
[[nodiscard]] QStringList resolveMissingPackages( const ProjectRequirements& requirements,
                                                  const QStringList& distributions,
                                                  const QStringList& themes );

}  // namespace hc::rst
