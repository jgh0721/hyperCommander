#include "text/TextEditorBackendFactory.h"

#include "text/ScintillaQtDirectBackend.h"

#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QWidget>

namespace TextEditorBackendFactory {

namespace {

constexpr char kDirectBackendName[] = "ScintillaQtDirectBackend";

void writeBackendTrace(const char* backendName)
{
    Q_UNUSED(backendName);
}

} // namespace

bool hasDirectBackendSupport()
{
	return true;
}

const char* selectedBackendName()
{
	return kDirectBackendName;
}

ITextEditorBackend* createDefault(QWidget* editorParent, QObject* owner)
{
	QObject* backendOwner = owner ? owner : static_cast<QObject*>(editorParent);
	if (!hasDirectBackendSupport())
		qFatal("Direct text backend support is required but was not compiled in.");
	writeBackendTrace(kDirectBackendName);
	return new ScintillaQtDirectBackend(editorParent, backendOwner);
}


} // namespace TextEditorBackendFactory
