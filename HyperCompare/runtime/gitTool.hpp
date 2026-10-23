#pragma once
#include "guiIpc.hpp"
#include <functional>

namespace hc::compare
{
    using GitGuiSubmit = std::function<int(const QStringList&, const Request&, const std::shared_ptr<std::atomic_bool>&, QString&)>;
    bool IsGitToolInvocation(const QStringList& Arguments);
    QString GitToolConfig(const QString& Executable);
    int GitToolExitCode(bool Merge, int Code);
    // No configuration or index writes. Git owns staging after a successful merge.
    int RunGitTool(const QStringList& Arguments, const std::shared_ptr<std::atomic_bool>& Canceled,
        QByteArray& Output, QString& Error, const GitGuiSubmit& Submit = SubmitGuiRequest);
}
