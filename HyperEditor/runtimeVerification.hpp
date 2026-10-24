#pragma once
// Explicit installation diagnostic; reads the runtime and edits only its own
// temporary fixtures. The JSON result is written to inherited standard output.
class QString;
int VerifyDocumentRuntime();
int VerifyEpubRuntime(const QString& Path);
