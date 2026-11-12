#include "stdafx.h"
#include "sphinxScanner.hpp"

/// 모듈
#include "rstFileKinds.hpp"
#include "rstBackgroundWork.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace fs = std::filesystem;
namespace {

// Sphinx 는 conf.py 를 utf-8-sig 로 읽는다. BOM 이 첫 줄 대입을 가리지 않게 벗긴다.
std::string withoutBom(std::string text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string normalizeDocName(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = trim(value);
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    static const std::regex suffix(R"(\.(rst|txt|md)$)", std::regex::icase);
    value = std::regex_replace(value, suffix, "");
    return value.empty() ? "index" : value;
}

bool pathIsBelow(const fs::path& path, const fs::path& base) {
    const fs::path candidate = fs::weakly_canonical(path);
    const fs::path root = fs::weakly_canonical(base);
    auto candidateIt = candidate.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *candidateIt != *rootIt) {
            return false;
        }
    }
    return true;
}

int relativeDepth(const fs::path& path, const fs::path& base) {
    if (!pathIsBelow(path, base)) {
        return 1'000'000;
    }
    const fs::path rel = fs::relative(fs::weakly_canonical(path), fs::weakly_canonical(base));
    return static_cast<int>(std::distance(rel.begin(), rel.end()));
}

}  // namespace

QString toQString(const fs::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString toCanonicalQString(const fs::path& path) {
    return QString::fromStdWString(fs::weakly_canonical(path).wstring());
}

fs::path toPath(const QString& text) {
    return fs::path(text.toStdWString());
}

bool SphinxProject::contains(const fs::path& filePath) const {
    return pathIsBelow(filePath, sourcePath) || pathIsBelow(filePath, rootPath);
}

ProjectScanner::ProjectScanner(fs::path workspaceRoot, ScannerSettings settings)
    : workspaceRoot_(fs::weakly_canonical(std::move(workspaceRoot))), settings_(std::move(settings)) {
    excludedDirs_ = filekinds::excludedScanDirectories();
    for (const std::string& extra : settings_.excludedDirs) {
        excludedDirs_.insert(QString::fromStdString(extra).toCaseFolded());
    }
}

std::vector<SphinxProject> ProjectScanner::scan() const {
    std::vector<SphinxProject> projects;
    if (!fs::exists(workspaceRoot_)) {
        return projects;
    }

    std::vector<fs::path> stack{workspaceRoot_};
    while (!stack.empty()) {
        // 워크스페이스 전체를 재귀 순회한다. 종료 중이면 그만둔다 — 결과는
        // 어차피 버려지고, 끝까지 도는 동안 프로세스가 살아 있게 된다.
        if (isShuttingDown()) {
            return {};
        }

        fs::path current = stack.back();
        stack.pop_back();
        std::error_code ec;
        if (fs::is_regular_file(current / "conf.py", ec)) {
            projects.push_back(projectFromConf(current / "conf.py"));
        }
        for (const fs::directory_entry& child : fs::directory_iterator(current, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            if (child.is_directory(ec) && !isExcludedDirectory(child.path())) {
                stack.push_back(child.path());
            }
        }
    }

    std::sort(projects.begin(), projects.end(), [](const SphinxProject& left, const SphinxProject& right) {
        // path::string() 은 Windows 로컬 코드 페이지로 변환하며, 표현할 수 없는
        // 문자가 있으면 예외를 던진다. 정렬 때문에 스캔 전체가 버려지지 않도록
        // 경로는 유니코드로 비교한다.
        return QString::compare(toQString(left.rootPath), toQString(right.rootPath), Qt::CaseInsensitive) < 0;
    });
    return projects;
}

SphinxProject ProjectScanner::projectFromConf(const fs::path& confPath) const {
    SphinxProject project;
    project.rootPath = fs::weakly_canonical(confPath.parent_path());
    project.confPath = fs::weakly_canonical(confPath);
    project.rootDoc = readRootDoc(confPath);
    project.sourcePath = inferSourcePath(project.rootPath, project.rootDoc);
    project.buildPath = project.rootPath / fs::path(settings_.buildDirName);
    project.projectId = projectIdFor(workspaceRoot_, project.rootPath);
    project.mystMarkdown = confDeclaresMystMarkdown(confPath);
    return project;
}

bool ProjectScanner::isExcludedDirectory(const fs::path& path) const {
    return excludedDirs_.contains(toQString(path.filename()).toCaseFolded());
}

std::string readRootDoc(const fs::path& confPath) {
    std::ifstream file(confPath, std::ios::binary);
    if (!file) {
        return "index";
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = withoutBom(stream.str());
    static const std::regex assignment(R"((?:^|\n)\s*(root_doc|master_doc)\s*=\s*['\"]([^'\"]+)['\"])");
    std::string rootDoc = "index";
    for (std::sregex_iterator it(text.begin(), text.end(), assignment), end; it != end; ++it) {
        rootDoc = (*it)[2].str();
    }
    return normalizeDocName(rootDoc);
}

bool confDeclaresEmptyHtmlStyle(const fs::path& confPath) {
    std::ifstream file(confPath, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = withoutBom(stream.str());

    // 마지막 대입이 이긴다 (readRootDoc 과 같은 방식).
    static const std::regex assignment(R"((?:^|\n)[ \t]*html_style[ \t]*(?::[^=\n]*)?=[ \t]*([^\n]*))");
    bool empty = false;
    for (std::sregex_iterator it(text.begin(), text.end(), assignment), end; it != end; ++it) {
        std::string value = trim((*it)[1].str());
        // 줄 끝 주석 제거 (따옴표 안의 #은 신경 쓰지 않는다 — 폴백이므로).
        const std::size_t hash = value.find('#');
        if (hash != std::string::npos) {
            value = trim(value.substr(0, hash));
        }
        empty = (value == "''" || value == "\"\"" || value == "''''''" || value == "\"\"\"\"\"\"");
    }
    return empty;
}

bool confDeclaresMystMarkdown(const fs::path& confPath) {
    std::ifstream file(confPath, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = withoutBom(stream.str());

    // 따옴표에 감싸인 확장 이름. extensions 리스트가 여러 줄에 걸쳐 있어도,
    // myst_nb 처럼 myst 를 품은 확장이어도 잡힌다.
    static const std::regex mystExtension(R"(['\"](myst_parser|myst_nb)['\"])");
    // 따옴표로 **정확히** 감싼 .md / .markdown. source_suffix 가 dict 든 list 든
    // 걸리고, exclude_patterns 의 글롭('**/*.md')은 걸리지 않는다.
    static const std::regex markdownSuffix(R"(['\"]\.(md|markdown)['\"])");

    // 줄 단위로 보면서 주석을 걷어낸다. `# import myst_parser` 처럼 껐다 켜는
    // 흔적이 conf.py 에 자주 남아 있어서, 파일 전체를 한 번에 훑으면 그것까지
    // 켜진 것으로 읽는다. (따옴표 안의 # 은 신경 쓰지 않는다 — 폴백이므로.)
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(begin, end - begin);
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        if (std::regex_search(line, mystExtension) || std::regex_search(line, markdownSuffix)) {
            return true;
        }
        begin = end + 1;
    }
    return false;
}

std::string readHtmlTheme(const fs::path& confPath) {
    std::ifstream file(confPath, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = withoutBom(stream.str());

    // 마지막 대입이 이긴다 (readRootDoc 과 같은 방식). 줄머리에 못 박았으므로
    // `# html_theme = "furo"` 처럼 주석으로 남긴 흔적은 걸리지 않는다 —
    // conf.py 에는 테마를 바꿔 보다 주석 처리한 줄이 흔하다.
    static const std::regex assignment(R"((?:^|\n)[ \t]*html_theme[ \t]*(?::[^=\n]*)?=[ \t]*['\"]([^'\"]+)['\"])");
    std::string theme;
    for (std::sregex_iterator it(text.begin(), text.end(), assignment), end; it != end; ++it) {
        theme = trim((*it)[1].str());
    }
    return theme;
}

fs::path inferSourcePath(const fs::path& rootPath, const std::string& rootDoc) {
    const fs::path rootDocPath(rootDoc);
    if (!rootDocPath.has_parent_path()) {
        return fs::weakly_canonical(rootPath);
    }
    const fs::path possible = rootPath / rootDocPath.parent_path();
    std::error_code ec;
    return fs::exists(possible, ec) ? fs::weakly_canonical(possible) : fs::weakly_canonical(rootPath);
}

std::wstring projectIdFor(const fs::path& workspaceRoot, const fs::path& rootPath) {
    std::error_code ec;
    fs::path rel = fs::relative(fs::weakly_canonical(rootPath), fs::weakly_canonical(workspaceRoot), ec);
    if (ec || rel.empty() || rel == ".") {
        return fs::weakly_canonical(workspaceRoot).filename().wstring();
    }
    std::wstring id;
    for (const fs::path& part : rel) {
        if (!id.empty()) {
            id += L'.';
        }
        id += part.wstring();
    }
    return id.empty() ? fs::weakly_canonical(workspaceRoot).filename().wstring() : id;
}

const SphinxProject* resolveProjectForFile(const fs::path& filePath, const std::vector<SphinxProject>& projects) {
    const SphinxProject* best = nullptr;
    std::pair<int, int> bestScore{1'000'000, 1'000'000};
    for (const SphinxProject& project : projects) {
        if (!project.contains(filePath)) {
            continue;
        }
        const std::pair<int, int> score{relativeDepth(filePath, project.sourcePath), relativeDepth(filePath, project.rootPath)};
        if (score < bestScore) {
            bestScore = score;
            best = &project;
        }
    }
    return best;
}

}  // namespace hc::rst

