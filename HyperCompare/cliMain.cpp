#include "stdafx.h"
#include "runtime/session.hpp"
#include "runtime/batch.hpp"
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
    SetConsoleCtrlHandler(control, TRUE);
    const auto parsed = hc::compare::Parse(app.arguments(), true); const auto& request = parsed.request_;
    if (!parsed.error_.isEmpty()) { print(parsed.error_.toUtf8() + '\n', stderr); return 2; }
    if (request.help_) { print(parsed.help_.toUtf8(), stdout); return 0; }
    if (request.version_) { print("HyperCompare 0.1.0; CLI schema 1; foundation\n", stdout); return 0; }
    if (request.mode_ == "capabilities") { print(QJsonDocument(hc::compare::Capabilities()).toJson(), stdout); return 0; }
    if (request.mode_ == "batch") return hc::compare::RunBatchAndReport(request, canceled);
    if (request.gui_)
    {
        auto args = app.arguments().mid(1); args.removeAll("-gui"); args.removeAll("--gui");
        if (!args.contains("-wait") && !args.contains("--wait")) args.push_back("-wait");
        QProcess process; process.setProcessChannelMode(QProcess::ForwardedChannels);
        process.start(app.applicationDirPath() + "/HyperCompare.exe", args);
        if (!process.waitForStarted()) { print(process.errorString().toUtf8() + '\n', stderr); return 2; }
        if (!process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit) return 2;
        return process.exitCode();
    }
    const auto result = hc::compare::Execute(request, canceled, true);
    return hc::compare::WriteReport(request, result);
}
