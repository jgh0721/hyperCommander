#include "stdafx.h"
#include "runtime/session.hpp"
#include "runtime/batch.hpp"
#include "runtime/guiIpc.hpp"
#include "runtime/gitTool.hpp"
#include "runtime/semantic.hpp"
#include "runtime/workerHost.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <Windows.h>

namespace
{
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    BOOL WINAPI control(DWORD Type)
    { if (Type != CTRL_C_EVENT && Type != CTRL_BREAK_EVENT) return FALSE; canceled->store(true); return TRUE; }
    void print(const QByteArray& Text, FILE* Stream)
    { QFile file; if (file.open(Stream, QIODevice::WriteOnly)) { file.write(Text); file.flush(); } }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); app.setApplicationName("HyperCompare"); app.setOrganizationName("HyperTools"); app.setApplicationVersion("0.1.0");
    if (app.arguments() == QStringList{app.arguments().front(), "-internal-worker"}) return hc::compare::WorkerMain();
    if (app.arguments() == QStringList{app.arguments().front(), "-internal-worker-resident"}) return hc::compare::ResidentWorkerMain();
    SetConsoleCtrlHandler(control, TRUE);
    if (hc::compare::IsGitToolInvocation(app.arguments()))
    {
        QByteArray output; QString error; const auto code = hc::compare::RunGitTool(app.arguments(), canceled, output, error);
        if (!output.isEmpty()) print(output, stdout); if (!error.isEmpty()) print(error.toUtf8() + '\n', stderr); return code;
    }
    const auto parsed = hc::compare::Parse(app.arguments(), true); const auto& request = parsed.request_;
    if (!parsed.error_.isEmpty()) { print(parsed.error_.toUtf8() + '\n', stderr); return 2; }
    if (request.help_) { print(parsed.help_.toUtf8() + "\nConsole Git adapter: -git-tool config | diff -file1 LOCAL -file2 REMOTE | merge -base BASE -file1 LOCAL -file2 REMOTE -output MERGED\n", stdout); return 0; }
    if (request.version_) { print("HyperCompare 0.1.0; CLI schema 1; foundation\n", stdout); return 0; }
    if (request.mode_ == "capabilities") { print(QJsonDocument(hc::compare::Capabilities()).toJson(), stdout); return 0; }
    if (request.mode_ == "batch") return hc::compare::RunBatchAndReport(request, canceled);
    if (request.gui_)
    {
        QString error; const int code = hc::compare::SubmitGuiRequest(app.arguments(), request, canceled, error);
        if (!error.isEmpty()) print(error.toUtf8() + '\n', stderr); return code;
    }
    const auto result = hc::compare::Execute(request, canceled, true);
    return hc::compare::WriteReport(request, result);
}
