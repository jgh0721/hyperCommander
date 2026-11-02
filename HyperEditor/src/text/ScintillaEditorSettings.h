#pragma once

#include <QFont>
#include "UniqueLibs/ProductDefaults_Edit.hpp"

struct ScintillaEditorSettings
{
    enum FontRenderingMode {
        Default = 0,
        NonAntialiased = 1,
        Antialiased = 2,       // GrayScale
        LcdOptimized = 3       // ClearType
    };

    enum ChangeHistoryMode {
        ChangeHistoryOff = 0,
        ChangeHistoryMarkers = 1,
        ChangeHistoryIndicators = 2,
        ChangeHistoryBoth = 3
    };

    enum IndentGuideStyle {
        IndentGuideReal = 1,
        IndentGuideLookForward = 2,
        IndentGuideLookBoth = 3
    };

    // 값은 Scintilla 의 SC_WRAP_* 와 같다.
    enum WrapMode {
        WrapNone = 0,
        WrapWord = 1,
        WrapChar = 2,
        WrapWhitespace = 3
    };

    // SC_WRAPVISUALFLAG_* 비트 조합. 필드는 enum 이 아니라 int 로 둔다.
    enum WrapVisualFlag {
        WrapFlagNone = 0x0000,
        WrapFlagEnd = 0x0001,
        WrapFlagStart = 0x0002,
        WrapFlagMargin = 0x0004
    };
    static constexpr int WrapFlagMask = 0x0007;

    // SC_WRAPINDENT_*
    enum WrapIndentMode {
        WrapIndentFixed = 0,
        WrapIndentSame = 1,
        WrapIndentIndent = 2,
        WrapIndentDeepIndent = 3
    };

    bool  useUtf8 = hc::defaults::edit::TextRuntime()["useUtf8"].toBool();
    bool  showLineNumbers = hc::defaults::edit::ViewValue("textView/showLineNumbers").toBool();
    bool  showIndentationGuides = hc::defaults::edit::ViewValue("textView/showIndentationGuides").toBool();
    bool  highlightCurrentLine = hc::defaults::edit::ViewValue("textView/highlightCurrentLine").toBool();
    bool  autoIndent = hc::defaults::edit::ViewValue("textView/autoIndent").toBool();
    bool  useTabs = hc::defaults::edit::ViewValue("textView/useTabs").toBool();
    bool  braceMatching = hc::defaults::edit::ViewValue("textView/braceHighlight").toBool();
    bool  readOnly = hc::defaults::edit::TextRuntime()["readOnly"].toBool();
    WrapMode wrapMode = static_cast<WrapMode>(hc::defaults::edit::ViewValue("textView/wordWrapMode").toInt());
    int wrapVisualFlags = hc::defaults::edit::ViewValue("textView/wrapVisualFlags").toInt();
    WrapIndentMode wrapIndentMode = static_cast<WrapIndentMode>(hc::defaults::edit::ViewValue("textView/wrapIndentMode").toInt());
    bool  showWhitespace = hc::defaults::edit::ViewValue("textView/showWhitespace").toBool();
    bool  showCodeFolding = hc::defaults::edit::ViewValue("textView/showCodeFolding").toBool();
    int   tabWidth = hc::defaults::edit::ViewValue("textView/tabWidth").toInt();
    double lineSpacingScale = hc::defaults::edit::ViewValue("textView/lineSpacing").toDouble();
    int   lineNumberMarginDigits = hc::defaults::edit::TextRuntime()["lineNumberMarginDigits"].toInt();
    FontRenderingMode fontRendering = static_cast<FontRenderingMode>(hc::defaults::edit::ViewValue("textView/fontRendering").toInt());
    ChangeHistoryMode changeHistoryMode = static_cast<ChangeHistoryMode>(hc::defaults::edit::ViewValue("textView/changeHistoryMode").toInt());
    IndentGuideStyle indentGuideStyle = static_cast<IndentGuideStyle>(hc::defaults::edit::ViewValue("textView/indentGuideStyle").toInt());

    static ScintillaEditorSettings standard();
    static ScintillaEditorSettings limitedPreview();

};

