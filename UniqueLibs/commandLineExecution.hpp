#pragma once

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QObject>
#include <QRegularExpression>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace hc::ui::commandLine {
struct LaunchPlan { QString program_; QString arguments_; };

inline bool HasShellSyntax(const QString& Text)
{
    bool Quoted = false;
    for (const auto Ch : Text) {
        if (Ch == u'"') Quoted = !Quoted;
        if (Ch == u'%' || Ch == u'!' || Ch == u'\r' || Ch == u'\n') return true;
        if (!Quoted && QStringLiteral("&|<>^()").contains(Ch)) return true;
    }
    return Quoted;
}

inline QString Executable(const QString& Name, const QString& Directory)
{
    QStringList Paths{Directory};
    Paths.append(qEnvironmentVariable("PATH").split(u';', Qt::SkipEmptyParts));
    for (auto& Path : Paths)
        if (QDir::isRelativePath(Path)) Path = QDir(Directory).absoluteFilePath(Path);
    if (Name.contains(u'/') || Name.contains(u'\\')) {
        auto Path = QDir::isAbsolutePath(Name) ? Name : QDir(Directory).absoluteFilePath(Name);
        if (QFileInfo(Path).suffix().isEmpty()) Path += ".exe";
        return QFileInfo(Path).isFile() ? QDir::toNativeSeparators(Path) : QString{};
    }
    return QDir::toNativeSeparators(QStandardPaths::findExecutable(Name, Paths));
}

inline LaunchPlan Plan(const QString& Text, const QString& Directory, bool KeepOpen)
{
    const auto Command = Text.trimmed();
    QString Name, Tail;
    if (Command.startsWith(u'"')) {
        const auto End = Command.indexOf(u'"', 1);
        if (End > 0) { Name = Command.mid(1, End - 1); Tail = Command.mid(End + 1).trimmed(); }
    } else {
        qsizetype End = 0;
        while (End < Command.size() && !Command[End].isSpace()) ++End;
        Name = Command.left(End); Tail = Command.mid(End).trimmed();
    }
    const auto Shell = qEnvironmentVariable("COMSPEC", qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/cmd.exe");
    const bool Plain = !HasShellSyntax(Command);
    const auto BaseName = QFileInfo(Name).fileName();
    // Explicit cmd owns its switches. A bare cmd opens exactly one shell.
    static const QRegularExpression ExplicitCommandSwitch(QStringLiteral("(?:^|\\s)/[ck](?:\\s|$)"), QRegularExpression::CaseInsensitiveOption);
    if ((Plain || ExplicitCommandSwitch.match(Tail).hasMatch())
        && (BaseName.compare("cmd", Qt::CaseInsensitive) == 0 || BaseName.compare("cmd.exe", Qt::CaseInsensitive) == 0)) {
        const auto Program = (Name.compare("cmd", Qt::CaseInsensitive) == 0 || Name.compare("cmd.exe", Qt::CaseInsensitive) == 0)
            ? Shell : Executable(Name, Directory);
        if (!Program.isEmpty()) return {Program, Tail};
    }
    if (Plain && !KeepOpen) {
        const auto Program = Executable(Name, Directory);
        const auto Extension = QFileInfo(Program).suffix();
        if (Extension.compare("exe", Qt::CaseInsensitive) == 0 || Extension.compare("com", Qt::CaseInsensitive) == 0)
            return {Program, Tail};
    }
    // /S strips this outer pair; quotes around paths inside it remain intact.
    return {Shell, QString(KeepOpen ? "/D /S /K \"%1\"" : "/D /S /C \"%1\"").arg(Command)};
}

inline QString Launch(const LaunchPlan& Plan, const QString& Directory)
{
    // cmd silently substitutes a different directory for UNC paths. Do not
    // use pushd, which creates drive mappings as a side effect.
    if (QDir::toNativeSeparators(Directory).startsWith("\\\\"))
        return QObject::tr("UNC 위치에서는 명령행 실행을 지원하지 않습니다. 로컬 폴더 또는 기존 네트워크 드라이브에서 실행하십시오.");
    if (!QFileInfo(Directory).isDir()) return QObject::tr("실행 폴더에 접근할 수 없습니다: %1").arg(Directory);
    auto Command = (QStringLiteral("\"%1\" ").arg(Plan.program_) + Plan.arguments_).toStdWString();
    const auto Program = Plan.program_.toStdWString();
    const auto Cwd = QDir::toNativeSeparators(Directory).toStdWString();
    STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup);
    PROCESS_INFORMATION Process{};
    // Real console input/output; no application pipes or retained child handles.
    if (!CreateProcessW(Program.c_str(), Command.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr, Cwd.c_str(), &Startup, &Process)) {
        const auto Error = GetLastError();
        wchar_t Message[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, Error, 0, Message, 1024, nullptr);
        return QObject::tr("명령 시작 실패 (%1): %2").arg(Error).arg(QString::fromWCharArray(Message).trimmed());
    }
    CloseHandle(Process.hThread); CloseHandle(Process.hProcess);
    return {};
}
}
