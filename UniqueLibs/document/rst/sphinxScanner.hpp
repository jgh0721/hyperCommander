#pragma once

#include <QSet>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

namespace hc::rst {

/// std::filesystem::path <-> QString 변환. Qt 경계마다 파일 로컬 static 으로
/// 중복 정의되던 것을 한 곳으로 모았다.
[[nodiscard]] QString toQString(const std::filesystem::path& path);
[[nodiscard]] QString toCanonicalQString(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path toPath(const QString& text);

struct SphinxProject {
    std::wstring projectId;
    std::filesystem::path rootPath;
    std::filesystem::path confPath;
    std::filesystem::path sourcePath;
    std::string rootDoc = "index";
    std::filesystem::path buildPath;
    /// conf.py 가 `.md` 를 Sphinx 원본으로 읽는가.
    ///
    /// `.md` 를 기존 Sphinx 빌드로 보낼지 내장 Markdown 렌더러로 보낼지 가르는
    /// 유일한 기준이다. 스캐너가 채운다.
    bool mystMarkdown = false;

    [[nodiscard]] bool contains(const std::filesystem::path& filePath) const;
};

struct ScannerSettings {
    std::string buildDirName = "_build/hypereditor";
    std::vector<std::string> excludedDirs;
};

class ProjectScanner final {
public:
    explicit ProjectScanner(std::filesystem::path workspaceRoot, ScannerSettings settings = {});

    [[nodiscard]] std::vector<SphinxProject> scan() const;
    [[nodiscard]] SphinxProject projectFromConf(const std::filesystem::path& confPath) const;

private:
    std::filesystem::path workspaceRoot_;
    ScannerSettings settings_;
    QSet<QString> excludedDirs_;   // 생성자에서 1회 구성 (유니코드 case folding)

    [[nodiscard]] bool isExcludedDirectory(const std::filesystem::path& path) const;
};

[[nodiscard]] std::string readRootDoc(const std::filesystem::path& confPath);

/// conf.py 에 `html_style = ''` 이 있는지. Sphinx 8 에서 _static checksum
/// 오류를 내므로 configOverrides 로 None 을 강제해야 한다.
///
/// 정규식이라 한계가 있다: 변수 대입(html_style = STYLE), 조건부 대입,
/// 삼중따옴표 문자열 안의 텍스트는 잡지 못한다. 정확한 판정은 Python
/// 빌더의 --auto-fix-legacy-conf(ast 파싱)가 하고, 이 함수는 런타임이 아직
/// 준비되지 않은 시점의 폴백이다.
[[nodiscard]] bool confDeclaresEmptyHtmlStyle(const std::filesystem::path& confPath);

/// conf.py 가 `.md` 를 Sphinx 원본으로 다루는지 정규식으로 본다.
///
/// confDeclaresEmptyHtmlStyle 과 같은 한계를 갖는다 — 변수 대입, 조건부 append,
/// 삼중따옴표 안의 텍스트는 잡지 못한다. 정확한 판정은 빌더 리포트가 하고
/// (요청한 문서를 못 찾으면 primaryDocname 이 빈다) 이 함수는 **첫 프리뷰를
/// 2.5초 기다리지 않기 위한 1차 판정**이다.
///
/// 술어를 일부러 Sphinx 쪽으로 기울인다. 거짓양성(myst 가 아닌데 그렇다고 본
/// 경우)은 빌더 리포트가 자동으로 정정하지만, 거짓음성은 정정할 수단이 없다 —
/// Sphinx 가 그 문서를 정상 렌더하므로 거부 신호가 나오지 않고, 조용히 품질만
/// 떨어진다. 그래서 extensions 뿐 아니라 source_suffix 에 실린 `.md` 도 본다
/// (recommonmark 나 커스텀 파서를 쓰는 프로젝트는 extensions 에 myst 가 없다).
[[nodiscard]] bool confDeclaresMystMarkdown( const std::filesystem::path& confPath );

/// conf.py 가 선언한 `html_theme`. 없거나 읽을 수 없으면 빈 문자열.
///
/// 가상 프로젝트가 "다른 프로젝트와 동일" 테마를 쓸 때, 그 "동일" 이 무엇인지
/// 알아내는 유일한 자리다. confDeclaresMystMarkdown 과 같은 한계를 갖는다 —
/// 변수 대입(`html_theme = THEME`)이나 조건부 대입은 잡지 못한다. 그때는 빈
/// 문자열이 나오고 호출자가 alabaster 로 물러선다.
[[nodiscard]] std::string readHtmlTheme( const std::filesystem::path& confPath );

[[nodiscard]] std::filesystem::path inferSourcePath(const std::filesystem::path& rootPath, const std::string& rootDoc);
[[nodiscard]] std::wstring projectIdFor(const std::filesystem::path& workspaceRoot, const std::filesystem::path& rootPath);
[[nodiscard]] const SphinxProject* resolveProjectForFile(const std::filesystem::path& filePath, const std::vector<SphinxProject>& projects);

}  // namespace hc::rst

