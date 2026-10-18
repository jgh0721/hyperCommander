#include "core/ThemeManager.h"

#include "core/AppSettings.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr auto kThemeOverridesKey = "theme/colorOverridesJson";
constexpr auto kThemeOverridesSchemaVersionKey = "theme/colorOverridesSchemaVersion";
constexpr int kThemeOverridesSchemaVersion = 4;

QJsonObject colorMapToJson(const QHash<QString, QColor>& colors)
{
    QJsonObject object;
    const auto keys = colors.keys();
    for (const QString& key : keys) {
        const QColor color = colors.value(key);
        if (color.isValid())
            object.insert(key, color.name(QColor::HexArgb));
    }
    return object;
}

QHash<QString, QColor> jsonToColorMap(const QJsonObject& object)
{
    QHash<QString, QColor> colors;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QColor color(it.value().toString());
        if (color.isValid())
            colors.insert(it.key(), color);
    }
    return colors;
}

} // namespace

ThemeManager& ThemeManager::instance()
{
    static ThemeManager mgr;
    return mgr;
}

ThemeManager::ThemeManager()
{
    loadOverrides();
}

ThemeManager::Theme ThemeManager::currentTheme() const { return m_theme; }

void ThemeManager::setTheme(Theme theme)
{
    if (m_theme == theme) return;
    m_theme = theme;
    // Application chrome is owned by waterColorStyle.
    emit themeChanged(theme);
}

QColor ThemeManager::color(const QString& key) const
{
    const auto defaults = defaultColors(m_theme);
    const auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    return overrides.value(key, defaults.value(key, QColor(Qt::magenta)));
}

bool ThemeManager::hasColor(const QString& key) const
{
    const auto defaults = defaultColors(m_theme);
    const auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    return overrides.contains(key) || defaults.contains(key);
}

bool ThemeManager::hasColorOverride(const QString& key) const
{
    const auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    return overrides.contains(key);
}

void ThemeManager::setColorOverride(const QString& key, const QColor& color)
{
    if (key.isEmpty() || !color.isValid())
        return;

    auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    overrides.insert(key, color);
    saveOverrides();
    applyToApplication();
    emit themeChanged(m_theme);
}

void ThemeManager::setColorOverrides(const QHash<QString, QColor>& colors)
{
    auto& overrides = (m_theme == Dark) ? m_darkOverrides : m_lightOverrides;
    overrides = colors;
    saveOverrides();
    applyToApplication();
    emit themeChanged(m_theme);
}

void ThemeManager::resetColorOverrides(Theme theme)
{
    if (theme == Dark)
        m_darkOverrides.clear();
    else
        m_lightOverrides.clear();
    saveOverrides();
    if (m_theme == theme) {
        applyToApplication();
        emit themeChanged(m_theme);
    }
}

QHash<QString, QColor> ThemeManager::colorOverrides(Theme theme) const
{
    return theme == Dark ? m_darkOverrides : m_lightOverrides;
}

QHash<QString, QColor> ThemeManager::effectiveColors(Theme theme) const
{
    auto colors = defaultColors(theme);
    const auto overrides = colorOverrides(theme);
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        colors.insert(it.key(), it.value());
    return colors;
}

QList<ThemeManager::ColorEntry> ThemeManager::editableColorEntries()
{
    return {
        {QStringLiteral("common.background"), tr("앱 배경"), tr("공통")},
        {QStringLiteral("common.surface"), tr("표면/패널"), tr("공통")},
        {QStringLiteral("common.surfaceAlt"), tr("보조 표면"), tr("공통")},
        {QStringLiteral("common.foreground"), tr("기본 글자"), tr("공통")},
        {QStringLiteral("common.foregroundMuted"), tr("보조 글자"), tr("공통")},
        {QStringLiteral("common.accent"), tr("강조색"), tr("공통")},
        {QStringLiteral("common.border"), tr("테두리"), tr("공통")},
        {QStringLiteral("common.toolbar"), tr("툴바"), tr("공통")},
        {QStringLiteral("common.tabActive"), tr("활성 탭"), tr("공통")},
        {QStringLiteral("common.tabInactive"), tr("비활성 탭"), tr("공통")},
        {QStringLiteral("common.canvas"), tr("캔버스 영역"), tr("공통")},

        {QStringLiteral("pdf.canvas"), tr("PDF 캔버스"), tr("PDF")},
        {QStringLiteral("pdf.pageGap"), tr("PDF 페이지 간격"), tr("PDF")},
        {QStringLiteral("pdf.searchHighlight"), tr("PDF 검색 하이라이트"), tr("PDF")},
        {QStringLiteral("pdf.searchCurrent"), tr("PDF 현재 검색 결과"), tr("PDF")},
        {QStringLiteral("pdf.textSelection"), tr("PDF 텍스트 선택"), tr("PDF")},
        {QStringLiteral("pdf.imageSelection"), tr("PDF 이미지 선택"), tr("PDF")},
        {QStringLiteral("pdf.annotation"), tr("PDF 기본 주석"), tr("PDF")},

        {QStringLiteral("image.canvas"), tr("이미지 캔버스"), tr("IMAGE")},
        {QStringLiteral("image.draw"), tr("이미지 기본 그리기"), tr("IMAGE")},
        {QStringLiteral("image.selection"), tr("이미지 선택/크롭"), tr("IMAGE")},
        {QStringLiteral("image.compositeOutline"), tr("이미지 합성 경계"), tr("IMAGE")},
        {QStringLiteral("image.handleFill"), tr("이미지 핸들 배경"), tr("IMAGE")},
        {QStringLiteral("image.handleBorder"), tr("이미지 핸들 테두리"), tr("IMAGE")},
        {QStringLiteral("image.accept"), tr("이미지 승인 버튼"), tr("IMAGE")},
        {QStringLiteral("image.cancel"), tr("이미지 취소 버튼"), tr("IMAGE")},

        {QStringLiteral("markdown.background"), tr("Markdown 문서 배경"), tr("Markdown")},
        {QStringLiteral("markdown.foreground"), tr("Markdown 본문"), tr("Markdown")},
        {QStringLiteral("markdown.link"), tr("Markdown 링크"), tr("Markdown")},
        {QStringLiteral("markdown.heading"), tr("Markdown 제목"), tr("Markdown")},
        {QStringLiteral("markdown.border"), tr("Markdown 테두리"), tr("Markdown")},
        {QStringLiteral("markdown.codeBackground"), tr("Markdown 코드 배경"), tr("Markdown")},
        {QStringLiteral("markdown.inlineCode"), tr("Markdown 인라인 코드"), tr("Markdown")},
        {QStringLiteral("markdown.blockquote"), tr("Markdown 인용문"), tr("Markdown")},
        {QStringLiteral("markdown.taskChecked"), tr("Markdown 체크 항목"), tr("Markdown")},

        {QStringLiteral("text.background"), tr("Text 편집기 배경"), tr("TEXT")},
        {QStringLiteral("text.foreground"), tr("Text 기본 글자"), tr("TEXT")},
        {QStringLiteral("text.currentLine"), tr("Text 현재 줄"), tr("TEXT")},
        {QStringLiteral("text.selection"), tr("Text 선택 배경"), tr("TEXT")},
        {QStringLiteral("text.selectionForeground"), tr("Text 선택 글자"), tr("TEXT")},
        {QStringLiteral("text.caret"), tr("Text 캐럿"), tr("TEXT")},
        {QStringLiteral("text.marginBackground"), tr("Text 여백 배경"), tr("TEXT")},
        {QStringLiteral("text.marginForeground"), tr("Text 줄 번호"), tr("TEXT")},
        {QStringLiteral("text.indentGuide"), tr("Text 들여쓰기 가이드"), tr("TEXT")},
        {QStringLiteral("text.foldMarker"), tr("Text 폴딩 마커"), tr("TEXT")},
        {QStringLiteral("text.braceMatch"), tr("Text 괄호 일치"), tr("TEXT")},
        {QStringLiteral("text.braceMismatch"), tr("Text 괄호 오류"), tr("TEXT")},
        {QStringLiteral("text.lexer.comment"), tr("Lexer 주석"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.number"), tr("Lexer 숫자"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.keyword"), tr("Lexer 키워드"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.type"), tr("Lexer 타입/보조 키워드"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.string"), tr("Lexer 문자열"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.preprocessor"), tr("Lexer 전처리/속성"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.operator"), tr("Lexer 연산자"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.identifier"), tr("Lexer 식별자"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.function"), tr("Lexer 함수/클래스"), tr("TEXT Lexer")},
        {QStringLiteral("text.lexer.variable"), tr("Lexer 변수"), tr("TEXT Lexer")},

        {QStringLiteral("text.lexer.cpp.comment"), tr("C++ 주석"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.cpp.keyword"), tr("C++ 키워드"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.cpp.string"), tr("C++ 문자열"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.cpp.function"), tr("C++ 함수/클래스"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.python.comment"), tr("Python 주석"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.python.keyword"), tr("Python 키워드"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.python.string"), tr("Python 문자열"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.python.function"), tr("Python 함수/클래스"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.json.keyword"), tr("JSON true/false/null"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.json.string"), tr("JSON 문자열"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.json.function"), tr("JSON 속성명"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.xml.keyword"), tr("XML/HTML 태그"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.xml.preprocessor"), tr("XML/HTML 속성"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.css.keyword"), tr("CSS 선택자"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.css.preprocessor"), tr("CSS 속성"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.bash.variable"), tr("Bash 변수"), tr("TEXT Lexer 상세")},
        {QStringLiteral("text.lexer.sql.keyword"), tr("SQL 키워드"), tr("TEXT Lexer 상세")},
    };
}

QHash<QString, QColor> ThemeManager::defaultColors(Theme theme)
{
    QHash<QString, QColor> c;
    if (theme == Dark) {
        // Modern Windows 11 Dark 기반 공통 팔레트
        c.insert(QStringLiteral("common.background"), QColor(QStringLiteral("#202020")));
        c.insert(QStringLiteral("common.surface"), QColor(QStringLiteral("#2B2B2B")));
        c.insert(QStringLiteral("common.surfaceAlt"), QColor(QStringLiteral("#323232")));
        c.insert(QStringLiteral("common.foreground"), QColor(QStringLiteral("#F3F3F3")));
        c.insert(QStringLiteral("common.foregroundMuted"), QColor(QStringLiteral("#CFCFCF")));
        c.insert(QStringLiteral("common.accent"), QColor(QStringLiteral("#60CDFF")));
        c.insert(QStringLiteral("common.border"), QColor(QStringLiteral("#3A3A3A")));
        c.insert(QStringLiteral("common.toolbar"), QColor(QStringLiteral("#2B2B2B")));
        c.insert(QStringLiteral("common.tabActive"), QColor(QStringLiteral("#202020")));
        c.insert(QStringLiteral("common.tabInactive"), QColor(QStringLiteral("#323232")));
        c.insert(QStringLiteral("common.canvas"), QColor(QStringLiteral("#1C1C1C")));
    } else {
        // Modern Windows 11 Light 기반 공통 팔레트
        c.insert(QStringLiteral("common.background"), QColor(QStringLiteral("#F3F3F3")));
        c.insert(QStringLiteral("common.surface"), QColor(QStringLiteral("#FFFFFF")));
        c.insert(QStringLiteral("common.surfaceAlt"), QColor(QStringLiteral("#F9F9F9")));
        c.insert(QStringLiteral("common.foreground"), QColor(QStringLiteral("#1A1A1A")));
        c.insert(QStringLiteral("common.foregroundMuted"), QColor(QStringLiteral("#5C5C5C")));
        c.insert(QStringLiteral("common.accent"), QColor(QStringLiteral("#0078D4")));
        c.insert(QStringLiteral("common.border"), QColor(QStringLiteral("#E5E5E5")));
        c.insert(QStringLiteral("common.toolbar"), QColor(QStringLiteral("#F9F9F9")));
        c.insert(QStringLiteral("common.tabActive"), QColor(QStringLiteral("#FFFFFF")));
        c.insert(QStringLiteral("common.tabInactive"), QColor(QStringLiteral("#F3F3F3")));
        c.insert(QStringLiteral("common.canvas"), QColor(QStringLiteral("#F3F3F3")));
    }

    c.insert(QStringLiteral("pdf.canvas"), c.value(QStringLiteral("common.canvas")));
    c.insert(QStringLiteral("pdf.pageGap"), theme == Dark ? QColor(QStringLiteral("#2B2B2B")) : QColor(QStringLiteral("#E5E5E5")));
    c.insert(QStringLiteral("pdf.searchHighlight"), QColor(QStringLiteral("#5AFFEB3B")));
    c.insert(QStringLiteral("pdf.searchCurrent"), QColor(QStringLiteral("#6EFF8C00")));
    c.insert(QStringLiteral("pdf.textSelection"), QColor(QStringLiteral("#462196F3")));
    c.insert(QStringLiteral("pdf.imageSelection"), QColor(QStringLiteral("#E62ECC71")));
    c.insert(QStringLiteral("pdf.annotation"), QColor(QStringLiteral("#80FFEB3B")));

    c.insert(QStringLiteral("image.canvas"), c.value(QStringLiteral("common.canvas")));
    c.insert(QStringLiteral("image.draw"), QColor(QStringLiteral("#D13438")));
    c.insert(QStringLiteral("image.selection"), c.value(QStringLiteral("common.accent")));
    c.insert(QStringLiteral("image.compositeOutline"), theme == Dark ? QColor(QStringLiteral("#F3F3F3")) : QColor(QStringLiteral("#FFFFFF")));
    c.insert(QStringLiteral("image.handleFill"), QColor(QStringLiteral("#FFFFFF")));
    c.insert(QStringLiteral("image.handleBorder"), QColor(QStringLiteral("#1A1A1A")));
    c.insert(QStringLiteral("image.accept"), QColor(QStringLiteral("#107C10")));
    c.insert(QStringLiteral("image.cancel"), QColor(QStringLiteral("#D13438")));

    c.insert(QStringLiteral("markdown.background"), theme == Dark ? QColor(QStringLiteral("#0D1117")) : QColor(QStringLiteral("#FFFFFF")));
    c.insert(QStringLiteral("markdown.foreground"), theme == Dark ? QColor(QStringLiteral("#C9D1D9")) : QColor(QStringLiteral("#24292F")));
    c.insert(QStringLiteral("markdown.link"), theme == Dark ? QColor(QStringLiteral("#58A6FF")) : QColor(QStringLiteral("#0969DA")));
    c.insert(QStringLiteral("markdown.heading"), theme == Dark ? QColor(QStringLiteral("#E6EDF3")) : QColor(QStringLiteral("#24292F")));
    c.insert(QStringLiteral("markdown.border"), theme == Dark ? QColor(QStringLiteral("#30363D")) : QColor(QStringLiteral("#D0D7DE")));
    c.insert(QStringLiteral("markdown.codeBackground"), theme == Dark ? QColor(QStringLiteral("#161B22")) : QColor(QStringLiteral("#F6F8FA")));
    c.insert(QStringLiteral("markdown.inlineCode"), theme == Dark ? QColor(QStringLiteral("#FF7B72")) : QColor(QStringLiteral("#CF222E")));
    c.insert(QStringLiteral("markdown.blockquote"), theme == Dark ? QColor(QStringLiteral("#8B949E")) : QColor(QStringLiteral("#57606A")));
    c.insert(QStringLiteral("markdown.taskChecked"), theme == Dark ? QColor(QStringLiteral("#238636")) : QColor(QStringLiteral("#2DA44E")));

    if (theme == Dark) {
        // Monokai 원형에 가까운 Dark 코드 팔레트
        c.insert(QStringLiteral("text.background"), QColor(QStringLiteral("#272822")));
        c.insert(QStringLiteral("text.foreground"), QColor(QStringLiteral("#F8F8F2")));
        c.insert(QStringLiteral("text.currentLine"), QColor(QStringLiteral("#3E3D32")));
        c.insert(QStringLiteral("text.selection"), QColor(QStringLiteral("#804A90E2")));
        c.insert(QStringLiteral("text.selectionForeground"), QColor(QStringLiteral("#FFFFFFFF")));
        c.insert(QStringLiteral("text.caret"), QColor(QStringLiteral("#F8F8F0")));
        c.insert(QStringLiteral("text.marginBackground"), QColor(QStringLiteral("#1E1F1C")));
        c.insert(QStringLiteral("text.marginForeground"), QColor(QStringLiteral("#90908A")));
        c.insert(QStringLiteral("text.indentGuide"), QColor(QStringLiteral("#5A5A52")));
        c.insert(QStringLiteral("text.foldMarker"), QColor(QStringLiteral("#75715E")));
        c.insert(QStringLiteral("text.braceMatch"), QColor(QStringLiteral("#66D9EF")));
        c.insert(QStringLiteral("text.braceMismatch"), QColor(QStringLiteral("#F92672")));
        c.insert(QStringLiteral("text.lexer.comment"), QColor(QStringLiteral("#75715E")));
        c.insert(QStringLiteral("text.lexer.number"), QColor(QStringLiteral("#AE81FF")));
        c.insert(QStringLiteral("text.lexer.keyword"), QColor(QStringLiteral("#F92672")));
        c.insert(QStringLiteral("text.lexer.type"), QColor(QStringLiteral("#66D9EF")));
        c.insert(QStringLiteral("text.lexer.string"), QColor(QStringLiteral("#E6DB74")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), QColor(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.operator"), QColor(QStringLiteral("#F92672")));
        c.insert(QStringLiteral("text.lexer.identifier"), QColor(QStringLiteral("#F8F8F2")));
        c.insert(QStringLiteral("text.lexer.function"), QColor(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.variable"), QColor(QStringLiteral("#FD971F")));
    } else {
        // Windows 11 Light 표면 위에서 Monokai 계열 강조색을 유지한 Light 코드 팔레트
        c.insert(QStringLiteral("text.background"), QColor(QStringLiteral("#FFFFFF")));
        c.insert(QStringLiteral("text.foreground"), QColor(QStringLiteral("#272822")));
        c.insert(QStringLiteral("text.currentLine"), QColor(QStringLiteral("#FFF8D6")));
        c.insert(QStringLiteral("text.selection"), QColor(QStringLiteral("#660078D4")));
        c.insert(QStringLiteral("text.selectionForeground"), QColor(QStringLiteral("#FFFFFFFF")));
        c.insert(QStringLiteral("text.caret"), QColor(QStringLiteral("#1A1A1A")));
        c.insert(QStringLiteral("text.marginBackground"), QColor(QStringLiteral("#F3F3F3")));
        c.insert(QStringLiteral("text.marginForeground"), QColor(QStringLiteral("#6E6E6E")));
        c.insert(QStringLiteral("text.indentGuide"), QColor(QStringLiteral("#D0D0D0")));
        c.insert(QStringLiteral("text.foldMarker"), QColor(QStringLiteral("#75715E")));
        c.insert(QStringLiteral("text.braceMatch"), QColor(QStringLiteral("#0078D4")));
        c.insert(QStringLiteral("text.braceMismatch"), QColor(QStringLiteral("#D13438")));
        c.insert(QStringLiteral("text.lexer.comment"), QColor(QStringLiteral("#75715E")));
        c.insert(QStringLiteral("text.lexer.number"), QColor(QStringLiteral("#7C4DFF")));
        c.insert(QStringLiteral("text.lexer.keyword"), QColor(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.type"), QColor(QStringLiteral("#0078A8")));
        c.insert(QStringLiteral("text.lexer.string"), QColor(QStringLiteral("#8A6D00")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), QColor(QStringLiteral("#4E8A00")));
        c.insert(QStringLiteral("text.lexer.operator"), QColor(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.identifier"), QColor(QStringLiteral("#272822")));
        c.insert(QStringLiteral("text.lexer.function"), QColor(QStringLiteral("#3B7D00")));
        c.insert(QStringLiteral("text.lexer.variable"), QColor(QStringLiteral("#B85C00")));
    }

    const QStringList lexerKeys = {
        QStringLiteral("cpp"), QStringLiteral("python"), QStringLiteral("json"),
        QStringLiteral("xml"), QStringLiteral("hypertext"), QStringLiteral("css"),
        QStringLiteral("bash"), QStringLiteral("sql")
    };
    const QStringList tokenKeys = {
        QStringLiteral("comment"), QStringLiteral("number"), QStringLiteral("keyword"),
        QStringLiteral("type"), QStringLiteral("string"), QStringLiteral("preprocessor"),
        QStringLiteral("operator"), QStringLiteral("identifier"), QStringLiteral("function"),
        QStringLiteral("variable")
    };
    for (const QString& lexerKey : lexerKeys) {
        for (const QString& tokenKey : tokenKeys) {
            c.insert(QStringLiteral("text.lexer.%1.%2").arg(lexerKey, tokenKey),
                     c.value(QStringLiteral("text.lexer.%1").arg(tokenKey)));
        }
    }
    return c;
}

QString ThemeManager::themeName(Theme theme)
{
    return theme == Dark ? QStringLiteral("Windows 11 Dark") : QStringLiteral("Windows 11 Light");
}

bool ThemeManager::importThemeFile(const QString& filePath, QString* errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = tr("테마 파일을 열 수 없습니다: %1").arg(filePath);
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (errorMessage) *errorMessage = tr("올바른 JSON 테마 파일이 아닙니다.");
        return false;
    }

    const QJsonObject root = document.object();
    const QString mode = root.value(QStringLiteral("mode")).toString(QStringLiteral("light")).toLower();
    const Theme importedTheme = mode == QStringLiteral("dark") ? Dark : Light;
    const QJsonObject colors = root.value(QStringLiteral("colors")).toObject();
    if (colors.isEmpty()) {
        if (errorMessage) *errorMessage = tr("테마 파일에 colors 객체가 없습니다.");
        return false;
    }

    if (importedTheme == Dark)
        m_darkOverrides = jsonToColorMap(colors);
    else
        m_lightOverrides = jsonToColorMap(colors);
    m_theme = importedTheme;
    saveOverrides();
    applyToApplication();
    emit themeChanged(m_theme);
    return true;
}

bool ThemeManager::exportThemeFile(const QString& filePath, QString* errorMessage) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = tr("테마 파일을 저장할 수 없습니다: %1").arg(filePath);
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("multiviewer.theme.v1"));
    root.insert(QStringLiteral("name"), themeName(m_theme));
    root.insert(QStringLiteral("mode"), m_theme == Dark ? QStringLiteral("dark") : QStringLiteral("light"));
    root.insert(QStringLiteral("colors"), colorMapToJson(effectiveColors(m_theme)));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QColor ThemeManager::backgroundColor() const  { return color(QStringLiteral("common.background")); }
QColor ThemeManager::foregroundColor() const   { return color(QStringLiteral("common.foreground")); }
QColor ThemeManager::toolBarColor() const      { return color(QStringLiteral("common.toolbar")); }
QColor ThemeManager::tabActiveColor() const    { return color(QStringLiteral("common.tabActive")); }
QColor ThemeManager::tabInactiveColor() const  { return color(QStringLiteral("common.tabInactive")); }
QColor ThemeManager::canvasAreaColor() const   { return color(QStringLiteral("common.canvas")); }

void ThemeManager::applyToApplication()
{
    const auto bg  = backgroundColor().name();
    const auto fg  = foregroundColor().name();
    const auto tb  = toolBarColor().name();
    const auto ta  = tabActiveColor().name();
    const auto ti  = tabInactiveColor().name();
    const auto cv  = canvasAreaColor().name();
    const auto border = color(QStringLiteral("common.border")).name();
    const auto surface = color(QStringLiteral("common.surface")).name();
    const auto accent = color(QStringLiteral("common.accent")).name();

    const QString ss = QStringLiteral(
        "QWidget { background-color: %1; color: %2; }"
        "QFrame, QGroupBox { border-color: %7; }"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QTableWidget, QListWidget, QTreeWidget { background-color: %8; color: %2; border: 1px solid %7; }"
        "QPushButton { background-color: %8; color: %2; border: 1px solid %7; padding: 4px 10px; border-radius: 4px; }"
        "QPushButton:hover { border-color: %9; }"
        "QScrollArea#canvasScrollArea, "
        "QScrollArea#canvasScrollArea > QWidget#qt_scrollarea_viewport, "
        "QScrollArea#canvasScrollArea > QWidget#qt_scrollarea_viewport > QWidget { background-color: %6; }"
        "QToolBar { background-color: %3; border: none; spacing: 4px; }"
        "QTabBar::tab { background: %5; color: %2; padding: 6px 14px; margin-right: 2px; }"
        "QTabBar::tab:selected { background: %4; }"
        "QMenuBar { background-color: %3; color: %2; }"
        "QMenuBar::item:selected { background-color: %4; }"
        "QStatusBar { background-color: %3; color: %2; }"
        "QScrollBar { background-color: %3; }"
    ).arg(bg, fg, tb, ta, ti, cv, border, surface, accent);

    if (qApp)
        qApp->setStyleSheet(ss);
}

void ThemeManager::loadOverrides()
{
    AppSettings settings;
    const int schemaVersion = settings.value(QString::fromLatin1(kThemeOverridesSchemaVersionKey), 1).toInt();
    const QJsonDocument document = QJsonDocument::fromJson(settings.value(QString::fromLatin1(kThemeOverridesKey)).toByteArray());
    if (!document.isObject())
        return;

    const QJsonObject root = document.object();
    m_lightOverrides = jsonToColorMap(root.value(QStringLiteral("light")).toObject());
    m_darkOverrides = jsonToColorMap(root.value(QStringLiteral("dark")).toObject());

    if (schemaVersion < kThemeOverridesSchemaVersion) {
        const auto legacyMonokaiDefaults = defaultColors(Dark);
        const auto keys = m_lightOverrides.keys();
        for (const QString& key : keys) {
            if (!key.startsWith(QStringLiteral("text.")))
                continue;
            const QColor overrideColor = m_lightOverrides.value(key);
            const QColor legacyColor = legacyMonokaiDefaults.value(key);
            if (overrideColor.isValid() && legacyColor.isValid()
                && overrideColor.name(QColor::HexArgb).compare(legacyColor.name(QColor::HexArgb), Qt::CaseInsensitive) == 0) {
                m_lightOverrides.remove(key);
            }
        }
        if (schemaVersion < 3) {
            const auto removeLegacySelectionOverride = [](QHash<QString, QColor>& overrides) {
                const QColor selection = overrides.value(QStringLiteral("text.selection"));
                const QString selectionName = selection.name(QColor::HexArgb).toLower();
                if (selectionName == QStringLiteral("#ff49483e")
                    || selectionName == QStringLiteral("#ffd7e8ff")
                    || selectionName == QStringLiteral("#ff0078d4")
                    || selectionName == QStringLiteral("#ff264f78")
                    || selectionName == QStringLiteral("#49483e")
                    || selectionName == QStringLiteral("#d7e8ff")) {
                    overrides.remove(QStringLiteral("text.selection"));
                }
                if (!overrides.contains(QStringLiteral("text.selectionForeground")))
                    overrides.remove(QStringLiteral("text.selectionForeground"));
            };
            removeLegacySelectionOverride(m_lightOverrides);
            removeLegacySelectionOverride(m_darkOverrides);
        }
        saveOverrides();
    }
}

void ThemeManager::saveOverrides() const
{
    QJsonObject root;
    root.insert(QStringLiteral("light"), colorMapToJson(m_lightOverrides));
    root.insert(QStringLiteral("dark"), colorMapToJson(m_darkOverrides));
    AppSettings settings;
    settings.setValue(QString::fromLatin1(kThemeOverridesKey), QJsonDocument(root).toJson(QJsonDocument::Compact));
    settings.setValue(QString::fromLatin1(kThemeOverridesSchemaVersionKey), kThemeOverridesSchemaVersion);
}

