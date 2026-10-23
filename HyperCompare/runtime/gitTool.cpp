#include "stdafx.h"
#include "gitTool.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>

namespace hc::compare
{
    namespace
    {
        QString shellQuote(QString Text)
        { Text.replace('\\', '/'); Text.replace('\'', "'\\''"); return '\'' + Text + '\''; }
        QString configQuote(QString Text)
        { Text.replace('\\', "\\\\"); Text.replace('"', "\\\""); return '"' + Text + '"'; }
        QByteArray unmergedIndex(const QString& Output, const std::shared_ptr<std::atomic_bool>& Canceled)
        {
            const auto git = QStandardPaths::findExecutable("git"); if (git.isEmpty()) throw Failure("GitExecutableUnavailable");
            QProcess process; process.setWorkingDirectory(QFileInfo(Output).absolutePath());
            process.start(git, {"--literal-pathspecs", "-c", "core.fsmonitor=false", "ls-files", "--unmerged", "-z", "--", Output});
            if (!process.waitForStarted(5000)) throw Failure("GitIndexReadFailed");
            QElapsedTimer timer; timer.start(); QByteArray data, error;
            for (;;)
            {
                process.waitForFinished(25); data += process.readAllStandardOutput(); error += process.readAllStandardError();
                if (Canceled->load() || timer.elapsed() > 10000 || data.size() > 65536 || error.size() > 65536)
                {
                    process.kill(); process.waitForFinished(1000);
                    throw Failure(Canceled->load() ? "Canceled" : "GitIndexReadLimit");
                }
                if (process.state() == QProcess::NotRunning) break;
            }
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) throw Failure("GitIndexReadFailed");
            return data;
        }
        bool validateStages(const QByteArray& Data)
        {
            if (Data.isEmpty() || !Data.endsWith('\0')) throw Failure("UnsupportedVcsState");
            static const QRegularExpression header("^(100644|100755) ([0-9a-f]{40}|[0-9a-f]{64}) ([123])$");
            int stages = 0; QByteArray path, mode;
            for (const auto& record : Data.split('\0'))
            {
                if (record.isEmpty()) continue;
                const auto tab = record.indexOf('\t'); if (tab < 0) throw Failure("UnsupportedVcsState");
                const auto match = header.match(QString::fromLatin1(record.left(tab)));
                if (!match.hasMatch() || record.mid(tab + 1).isEmpty()) throw Failure("UnsupportedVcsState");
                const auto nextPath = record.mid(tab + 1), nextMode = match.captured(1).toLatin1();
                if ((!path.isEmpty() && path != nextPath) || (!mode.isEmpty() && mode != nextMode)) throw Failure("UnsupportedVcsState");
                path = nextPath; mode = nextMode; const auto bit = 1 << match.captured(3).toInt();
                if (stages & bit) throw Failure("UnsupportedVcsState"); stages |= bit;
            }
            if (stages != 14 && stages != 12) throw Failure("UnsupportedVcsState");
            return stages == 12; // Explicit index absence of stage 1: add/add.
        }
        void requireFile(const QString& Path)
        { if (!QFileInfo(Path).isFile() || QFileInfo(Path).isSymLink()) throw Failure("UnsupportedVcsState"); }
    }
    bool IsGitToolInvocation(const QStringList& Arguments)
    {
        for (const auto& arg : Arguments.mid(1))
            if (arg == "-git-tool" || arg == "--git-tool" || arg.startsWith("-git-tool=") || arg.startsWith("--git-tool=")) return true;
        return false;
    }
    QString GitToolConfig(const QString& Executable)
    {
        if (Executable.isEmpty() || Executable.contains('\n') || Executable.contains('\r') || Executable.contains(QChar(0))) throw Failure("InvalidGitExecutablePath");
        const auto exe = shellQuote(QFileInfo(Executable).absoluteFilePath());
        const auto diff = exe + " -git-tool diff -file1 \"$LOCAL\" -file2 \"$REMOTE\"";
        const auto merge = exe + " -git-tool merge -base \"$BASE\" -file1 \"$LOCAL\" -file2 \"$REMOTE\" -output \"$MERGED\"";
        return "# HyperCompare: merge into the repository config after reviewing existing keys.\n"
            "# Invoke: git difftool --tool=hypercompare --trust-exit-code\n"
            "# Invoke: git mergetool --tool=hypercompare\n"
            "[difftool \"hypercompare\"]\n\tcmd = " + configQuote(diff) +
            "\n[mergetool \"hypercompare\"]\n\tcmd = " + configQuote(merge) +
            "\n\ttrustExitCode = true\n\thideResolved = false\n";
    }
    int GitToolExitCode(bool Merge, int Code)
    { return !Merge && Code == 1 ? 0 : Code >= 0 && Code <= 3 ? Code : 2; }
    int RunGitTool(const QStringList& Arguments, const std::shared_ptr<std::atomic_bool>& Canceled,
        QByteArray& Output, QString& Error, const GitGuiSubmit& Submit)
    {
        Output.clear(); Error.clear();
        try
        {
            QCommandLineParser parser; parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
            for (const auto& name : {"git-tool", "file1", "file2", "base", "output"}) parser.addOption({name, "Git adapter role", "value"});
            if (!parser.parse(Arguments) || !parser.positionalArguments().isEmpty()) throw Failure("InvalidGitArguments");
            QSet<QString> seen;
            for (const auto& name : parser.optionNames())
            { if (seen.contains(name) || parser.value(name).isEmpty()) throw Failure("InvalidGitArguments"); seen.insert(name); }
            const auto mode = parser.value("git-tool");
            if (mode == "config")
            {
                if (seen.size() != 1) throw Failure("InvalidGitArguments");
                Output = GitToolConfig(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe").toUtf8(); return 0;
            }
            const bool merge = mode == "merge";
            if ((!merge && mode != "diff") || seen.size() != (merge ? 5 : 3) || !seen.contains("file1") || !seen.contains("file2") ||
                (merge && (!seen.contains("base") || !seen.contains("output")))) throw Failure("InvalidGitArguments");
            if (Canceled->load()) throw Failure("Canceled");
            QTemporaryDir empty; if (!empty.isValid()) throw Failure("GitTemporaryDirectoryFailed");
            const auto role = [&](const QString& Name) {
                const auto value = parser.value(Name);
                if (!merge && (value == "/dev/null" || value.compare("nul", Qt::CaseInsensitive) == 0 ||
                    QDir::fromNativeSeparators(value).compare("//./nul", Qt::CaseInsensitive) == 0))
                {
                    const auto path = empty.filePath(Name); QFile file(path);
                    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Failure("GitEmptyInputFailed");
                    return path;
                }
                return QFileInfo(value).absoluteFilePath();
            };
            const auto left = role("file1"), right = role("file2"); requireFile(left); requireFile(right);
            QStringList args{"HyperCompare", "-gui", "-wait", "-mode", merge ? "merge" : "compare", "-file1", left, "-file2", right};
            QByteArray index; QString target;
            if (merge)
            {
                target = QFileInfo(parser.value("output")).absoluteFilePath(); requireFile(target);
                index = unmergedIndex(target, Canceled); const bool emptyBase = validateStages(index);
                const auto base = QFileInfo(parser.value("base")).absoluteFilePath(); requireFile(base);
                if (emptyBase && QFileInfo(base).size() != 0) throw Failure("InvalidGitEmptyBase");
                args << "-base" << base << "-output" << target << "-overwrite";
            }
            const auto parsed = Parse(args, true); if (!parsed.error_.isEmpty()) throw Failure(parsed.error_.toStdString());
            const auto code = Submit(args, parsed.request_, Canceled, Error);
            if (Canceled->load()) return 3;
            if (merge && code == 0 && unmergedIndex(target, Canceled) != index) throw Failure("GitIndexChangedDuringMerge");
            return Error.isEmpty() ? GitToolExitCode(merge, code) : 2;
        }
        catch (const std::exception& error)
        { Error = QString::fromUtf8(error.what()); return Error == "Canceled" ? 3 : 2; }
    }
}
