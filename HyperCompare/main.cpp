#include "stdafx.h"
#include "runtime/session.hpp"
#include "widgets/compareWindow.hpp"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QMessageBox>
#include <fmstyle/ThemeManager.h>
#include <Windows.h>
#include <shellapi.h>

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
    QCoreApplication::setApplicationName("HyperCompare"); QCoreApplication::setOrganizationName("HyperTools");
    QCoreApplication::setApplicationVersion("0.1.0");
    int count = 0; auto** wide = CommandLineToArgvW(GetCommandLineW(), &count); QStringList arguments;
    if (!wide) return 2;
    for (int i = 0; i < count; ++i) arguments.push_back(QString::fromWCharArray(wide[i])); LocalFree(wide);
    const auto parsed = hc::compare::Parse(arguments, false); const auto& request = parsed.request_;
    if (request.headless_ || request.help_ || request.version_ || request.mode_ == "capabilities")
    {
        QCoreApplication app(argc, argv); SetConsoleCtrlHandler(control, TRUE);
        if (!parsed.error_.isEmpty()) { print(parsed.error_.toUtf8() + '\n', stderr); return 2; }
        if (request.help_) { print(parsed.help_.toUtf8(), stdout); return 0; }
        if (request.version_) { print("HyperCompare 0.1.0; CLI schema 1; foundation\n", stdout); return 0; }
        if (request.mode_ == "capabilities") { print(QJsonDocument(hc::compare::Capabilities()).toJson(), stdout); return 0; }
        const auto result = hc::compare::Execute(request, canceled, true);
        return hc::compare::WriteReport(request, result);
    }
    QApplication app(argc, argv);
    auto& theme = fm::style::ThemeManager::instance(); theme.setDesign(fm::style::Design::Watercolor); theme.install(app);
    if (!parsed.error_.isEmpty()) { QMessageBox::warning(nullptr, "HyperCompare", parsed.error_); return 2; }
    hc::compare::CompareWindow window(request); window.show(); app.exec();
    return request.wait_ ? window.ExitCode() : 0;
}
