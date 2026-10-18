#pragma once

#include <QFont>

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

    bool  useUtf8 = true;
    bool  showLineNumbers = true;
    bool  showIndentationGuides = true;
    bool  highlightCurrentLine = true;
    bool  autoIndent = true;
    bool  useTabs = true;
    bool  braceMatching = true;
    bool  readOnly = false;
    WrapMode wrapMode = WrapChar;
    int wrapVisualFlags = WrapFlagEnd;
    WrapIndentMode wrapIndentMode = WrapIndentSame;
    bool  showWhitespace = false;
    bool  showCodeFolding = true;
    int   tabWidth = 4;
    double lineSpacingScale = 1.1;
    int   lineNumberMarginDigits = 5;
    FontRenderingMode fontRendering = Antialiased;
    ChangeHistoryMode changeHistoryMode = ChangeHistoryBoth;
    IndentGuideStyle indentGuideStyle = IndentGuideReal;

    static ScintillaEditorSettings standard();
    static ScintillaEditorSettings limitedPreview();

};

