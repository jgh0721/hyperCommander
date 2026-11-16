#pragma once
#include "commonLibs/system/childEnvironment.hpp"
#include <QProcess>
#include <QProcessEnvironment>
namespace hc::ui {
inline bool StartChildProcess(const QString& Program, const QStringList& Arguments, const QString& Directory = {})
{
    QProcess Process; Process.setProgram(Program); Process.setArguments(Arguments); Process.setWorkingDirectory(Directory);
    if (const auto Snapshot = hc::environment::ChildEnvironmentSnapshot()) {
        QProcessEnvironment Environment;
        for (const auto& [Name, Text] : *Snapshot) Environment.insert(QString::fromStdWString(Name), QString::fromStdWString(Text));
        Process.setProcessEnvironment(Environment);
    }
    return Process.startDetached();
}
}
