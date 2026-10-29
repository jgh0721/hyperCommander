#include "core/ThemeManager.h"

#include "core/AppSettings.h"
#include <fmstyle/ThemeManager.h>

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
    // Accent or token edits keep the light/dark choice; views still need to repaint with the new colors.
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { emit themeChanged(m_theme); });
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
    if (key.startsWith(QStringLiteral("text.")) && overrides.contains(key)) return overrides.value(key);
    return defaults.value(key);
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
    // Screen and document colors come from the product's Paper Fluent tokens, so they follow the product's
    // accent and token settings; only the syntax palette below is the editor's own. The requested variant may
    // differ from the active one (a view still switching), so derive it rather than reading colors().
    using fm::style::Token;
    const auto& Manager = fm::style::ThemeManager::instance();
    const bool WantDark = theme == Dark;
    const auto& T = fm::style::isDarkVariant(Manager.effectiveVariant()) == WantDark
        ? Manager.colors() : Manager.colors(WantDark ? fm::style::Variant::Dark : fm::style::Variant::Light);
    const auto Alpha = [](QColor Color, int Value) { Color.setAlpha(Value); return Color; };
    QHash<QString, QColor> c;
    c.insert(QStringLiteral("common.background"), T[Token::Win]);
    c.insert(QStringLiteral("common.surface"), T[Token::Surface]);
    c.insert(QStringLiteral("common.surfaceAlt"), T[Token::Alt]);
    c.insert(QStringLiteral("common.foreground"), T[Token::Fg]);
    c.insert(QStringLiteral("common.foregroundMuted"), T[Token::Fg2]);
    c.insert(QStringLiteral("common.accent"), T[Token::Accent]);
    c.insert(QStringLiteral("common.border"), T[Token::Line]);
    c.insert(QStringLiteral("common.toolbar"), T[Token::Win]);
    c.insert(QStringLiteral("common.tabActive"), T[Token::Surface]);
    c.insert(QStringLiteral("common.tabInactive"), T[Token::Alt]);
    c.insert(QStringLiteral("common.canvas"), T[Token::Alt]);

    c.insert(QStringLiteral("pdf.canvas"), T[Token::Alt]);
    c.insert(QStringLiteral("pdf.pageGap"), T[Token::Grid]);
    c.insert(QStringLiteral("pdf.searchHighlight"), Alpha(T[Token::Warn], 0x5A));
    c.insert(QStringLiteral("pdf.searchCurrent"), Alpha(T[Token::Accent], 0x6E));
    c.insert(QStringLiteral("pdf.textSelection"), Alpha(T[Token::Accent], 0x46));
    c.insert(QStringLiteral("pdf.imageSelection"), Alpha(T[Token::Ok], 0xE6));
    c.insert(QStringLiteral("pdf.annotation"), Alpha(T[Token::Warn], 0x80));

    c.insert(QStringLiteral("image.canvas"), T[Token::Alt]);
    c.insert(QStringLiteral("image.draw"), T[Token::Danger]);
    c.insert(QStringLiteral("image.selection"), T[Token::Accent]);
    c.insert(QStringLiteral("image.compositeOutline"), T[Token::Surface]);
    c.insert(QStringLiteral("image.handleFill"), T[Token::Surface]);
    c.insert(QStringLiteral("image.handleBorder"), T[Token::Fg]);
    c.insert(QStringLiteral("image.accept"), T[Token::Ok]);
    c.insert(QStringLiteral("image.cancel"), T[Token::Danger]);

    c.insert(QStringLiteral("markdown.background"), T[Token::Surface]);
    c.insert(QStringLiteral("markdown.foreground"), T[Token::Fg]);
    c.insert(QStringLiteral("markdown.link"), T[Token::AccentFg]);
    c.insert(QStringLiteral("markdown.heading"), T[Token::Fg]);
    c.insert(QStringLiteral("markdown.border"), T[Token::Line]);
    c.insert(QStringLiteral("markdown.codeBackground"), T[Token::Alt]);
    c.insert(QStringLiteral("markdown.inlineCode"), T[Token::Danger]);
    c.insert(QStringLiteral("markdown.blockquote"), T[Token::Fg2]);
    c.insert(QStringLiteral("markdown.taskChecked"), T[Token::Ok]);

    c.insert(QStringLiteral("text.background"), T[Token::Surface]);
    c.insert(QStringLiteral("text.foreground"), T[Token::Fg]);
    c.insert(QStringLiteral("text.currentLine"), T[Token::Alt]);
    c.insert(QStringLiteral("text.selection"), T[Token::AccentSoft]);
    c.insert(QStringLiteral("text.selectionForeground"), T[Token::Fg]);
    c.insert(QStringLiteral("text.caret"), T[Token::Fg]);
    c.insert(QStringLiteral("text.marginBackground"), T[Token::Win]);
    c.insert(QStringLiteral("text.marginForeground"), T[Token::Fg3]);
    c.insert(QStringLiteral("text.indentGuide"), T[Token::Line]);
    c.insert(QStringLiteral("text.foldMarker"), T[Token::Fg3]);
    c.insert(QStringLiteral("text.braceMatch"), T[Token::Accent]);
    c.insert(QStringLiteral("text.braceMismatch"), T[Token::Danger]);

    // Keep syntax text at 4.5:1 against both Paper Fluent body/current-line
    // surfaces. Explicit user syntax overrides are applied separately.
    if (WantDark) {
        c.insert(QStringLiteral("text.lexer.comment"), QColor(QStringLiteral("#93907D")));
        c.insert(QStringLiteral("text.lexer.number"), QColor(QStringLiteral("#AE81FF")));
        c.insert(QStringLiteral("text.lexer.keyword"), QColor(QStringLiteral("#FF4388")));
        c.insert(QStringLiteral("text.lexer.type"), QColor(QStringLiteral("#66D9EF")));
        c.insert(QStringLiteral("text.lexer.string"), QColor(QStringLiteral("#E6DB74")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), QColor(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.operator"), QColor(QStringLiteral("#FF4388")));
        c.insert(QStringLiteral("text.lexer.identifier"), QColor(QStringLiteral("#F8F8F2")));
        c.insert(QStringLiteral("text.lexer.function"), QColor(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.variable"), QColor(QStringLiteral("#FD971F")));
    } else {
        c.insert(QStringLiteral("text.lexer.comment"), QColor(QStringLiteral("#716D5A")));
        c.insert(QStringLiteral("text.lexer.number"), QColor(QStringLiteral("#7444F5")));
        c.insert(QStringLiteral("text.lexer.keyword"), QColor(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.type"), QColor(QStringLiteral("#00749F")));
        c.insert(QStringLiteral("text.lexer.string"), QColor(QStringLiteral("#856800")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), QColor(QStringLiteral("#437900")));
        c.insert(QStringLiteral("text.lexer.operator"), QColor(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.identifier"), QColor(QStringLiteral("#272822")));
        c.insert(QStringLiteral("text.lexer.function"), QColor(QStringLiteral("#3B7D00")));
        c.insert(QStringLiteral("text.lexer.variable"), QColor(QStringLiteral("#AB5500")));
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

    const QJsonObject root = document.object();
    m_lightOverrides = jsonToColorMap(root.value(QStringLiteral("light")).toObject());
    m_darkOverrides = jsonToColorMap(root.value(QStringLiteral("dark")).toObject());

    if (document.isObject() && schemaVersion < kThemeOverridesSchemaVersion) {
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
    // Older screen overrides were ignored; only explicit choices in the text
    // color page replace product tokens. Null means inherit, including when
    // suppressing a legacy syntax override.
    for (const auto Theme : {Light, Dark}) {
        auto& Overrides = Theme == Dark ? m_darkOverrides : m_lightOverrides;
        for (const auto& Key : Overrides.keys()) if (!Key.startsWith("text.lexer.")) Overrides.remove(Key);
        const auto Colors = settings.value(QStringLiteral("textView/colors/%1").arg(Theme == Dark ? "dark" : "light")).toMap();
        for (auto It = Colors.begin(); It != Colors.end(); ++It) {
            if (!It.key().startsWith("text.")) continue;
            const QColor Color(It.value().toString());
            if (Color.isValid()) Overrides.insert(It.key(), Color);
            else Overrides.remove(It.key());
        }
    }
}

void ThemeManager::ReloadColorOverrides()
{
    const auto LightBefore = m_lightOverrides, DarkBefore = m_darkOverrides;
    loadOverrides();
    if (LightBefore != m_lightOverrides || DarkBefore != m_darkOverrides) emit themeChanged(m_theme);
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

