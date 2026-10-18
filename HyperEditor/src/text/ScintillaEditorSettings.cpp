#include "text/ScintillaEditorSettings.h"

ScintillaEditorSettings ScintillaEditorSettings::standard()
{
    return {};
}

ScintillaEditorSettings ScintillaEditorSettings::limitedPreview()
{
    ScintillaEditorSettings settings;
    settings.showLineNumbers = false;
    settings.showIndentationGuides = false;
    settings.highlightCurrentLine = false;
    settings.braceMatching = false;
    settings.readOnly = true;
    return settings;
}



