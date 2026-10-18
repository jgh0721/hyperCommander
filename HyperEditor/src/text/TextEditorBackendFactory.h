#pragma once

class ITextEditorBackend;
class QObject;
class QWidget;

namespace TextEditorBackendFactory {

bool hasDirectBackendSupport();
const char* selectedBackendName();
ITextEditorBackend* createDefault(QWidget* editorParent, QObject* owner = nullptr);

} // namespace TextEditorBackendFactory


