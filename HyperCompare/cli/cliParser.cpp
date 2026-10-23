#include "stdafx.h"
#include "runtime/session.hpp"
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>

namespace hc::compare
{
    Parsed Parse(const QStringList& Arguments, bool Console)
    {
        Parsed result; auto& request = result.request_;
        // Preserve headless intent even when option validation fails before role mapping.
        request.headless_ = Console || Arguments.contains("-headless") || Arguments.contains("--headless");
        QCommandLineParser parser;
        parser.setApplicationDescription("HyperCompare: independent file and directory comparison\n"
            "HyperCompare.exe -file1 \"D:\\left.cpp\" -file2 \"C:\\right.cpp\"\n"
            "Three independent files: -file1 LEFT -file2 CENTER -file3 RIGHT\n"
            "Three-way merge: -mode merge -base BASE -file1 LEFT -file2 RIGHT -output RESULT");
        parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        const auto flag = [&](const QString& Name, const QString& Description) { parser.addOption({Name, Description}); };
        const auto value = [&](const QString& Name, const QString& Description) { parser.addOption({Name, Description, "value"}); };
        flag("help", "Show supported options."); flag("version", "Show product and CLI schema versions.");
        for (const auto& name : {"file1", "file2", "file3", "base", "output"}) value(name, "Path for this input/output role.");
        value("mode", "auto, compare, merge, directory, capabilities.");
        value("format", "text or json. stdout contains only this report."); value("report", "Report file; '-' means stdout.");
        value("encoding", "Explicit UTF-8, UTF-16LE, UTF-16BE or CP949 for all inputs.");
        value("takeall", "Explicit whole-side selection for two-way merge: left or right.");
        value("planout", "Export a text merge decision template to a new JSON file (headless only).");
        value("decisions", "Apply a JSON decision plan bound to the current inputs (headless only).");
        value("timeout-ms", "Request deadline in milliseconds (100..3600000).");
        value("include", "Directory include globs separated by ';'; filename or root-relative path with '/'.");
        value("exclude", "Directory exclusion globs separated by ';'; matching directories are pruned.");
        value("directory-level", "bytes (default) or metadata; metadata does not verify file contents.");
        value("theme", "GUI theme: light or dark.");
        flag("headless", "Run without a GUI."); flag("gui", "Open the GUI (console launcher waits).");
        flag("wait", "Return the completed request status."); flag("exitcode", "Return comparison/merge status; GUI requires -wait.");
        flag("overwrite", "Allow replacing an existing output, subject to revision validation.");
        flag("dryrun", "Compute without writing the merge output.");
        result.help_ = parser.helpText();
        const auto fail = [&](QString Text) { result.error_ = std::move(Text); return result; };
        if (!parser.parse(Arguments)) return fail(parser.errorText());
        if (!parser.positionalArguments().isEmpty()) return fail("Use -file1/-file2; positional arguments are not accepted.");
        QSet<QString> seen;
        for (const auto& name : parser.optionNames())
        { if (seen.contains(name)) return fail("Duplicate option: " + name); seen.insert(name); }
        request.help_ = parser.isSet("help"); request.version_ = parser.isSet("version");
        if (request.help_ || request.version_) return result;
        request.file1_ = parser.value("file1"); request.file2_ = parser.value("file2");
        request.file3_ = parser.value("file3"); request.base_ = parser.value("base"); request.output_ = parser.value("output");
        request.encoding_ = parser.value("encoding"); request.report_ = parser.value("report");
        request.takeAll_ = parser.value("takeall"); request.theme_ = parser.value("theme");
        for (const auto& name : {"include", "exclude"})
        {
            if (!parser.isSet(name)) continue;
            const auto patterns = parser.value(name).split(';');
            if (patterns.size() > 32) return fail("Too many directory filters.");
            for (const auto& pattern : patterns)
                if (pattern.isEmpty() || pattern.size() > 256 || pattern.contains('\\')) return fail("Invalid directory filter.");
            (QString(name) == "include" ? request.directory_.include_ : request.directory_.exclude_) = patterns;
        }
        if (parser.isSet("directory-level")) request.directory_.level_ = parser.value("directory-level");
        if (request.directory_.level_ != "bytes" && request.directory_.level_ != "metadata") return fail("Invalid directory-level.");
        if ((parser.isSet("include") || parser.isSet("exclude") || parser.isSet("directory-level")) &&
            !QStringList{"auto", "directory"}.contains(parser.isSet("mode") ? parser.value("mode") : "auto")) return fail("Directory options require auto or directory mode.");
        request.planOut_ = parser.value("planout"); request.decisions_ = parser.value("decisions");
        for (const auto& name : {"planout", "decisions"})
            if (parser.isSet(name) && parser.value(name).isEmpty()) return fail("Empty path: " + QString(name));
        if (parser.isSet("mode")) request.mode_ = parser.value("mode");
        if (parser.isSet("format")) request.format_ = parser.value("format");
        if (parser.isSet("headless") && parser.isSet("gui")) return fail("-gui and -headless are mutually exclusive.");
        request.gui_ = parser.isSet("gui"); request.headless_ = parser.isSet("headless") || (Console && !request.gui_);
        request.wait_ = parser.isSet("wait") || Console;
        request.overwrite_ = parser.isSet("overwrite"); request.dryRun_ = parser.isSet("dryrun");
        const bool decisionOptions = !request.planOut_.isEmpty() || !request.decisions_.isEmpty();
        if (decisionOptions && (!request.headless_ || request.mode_ != "merge")) return fail("-planout/-decisions require headless -mode merge.");
        if (decisionOptions && !request.takeAll_.isEmpty()) return fail("-takeall cannot be combined with decision plan options.");
        if (!request.planOut_.isEmpty() && !request.decisions_.isEmpty()) return fail("Use -planout and -decisions in separate requests.");
        if (!request.headless_ && (request.dryRun_ || parser.isSet("report") || parser.isSet("format")))
            return fail("-dryrun/-report/-format require headless mode in this version.");
        if (parser.isSet("exitcode") && !request.wait_ && !request.headless_) return fail("-exitcode requires -wait in GUI mode.");
        if (parser.isSet("timeout-ms"))
        {
            bool valid = false; request.timeoutMs_ = parser.value("timeout-ms").toInt(&valid);
            if (!valid || request.timeoutMs_ < 100 || request.timeoutMs_ > 3600000) return fail("Invalid timeout-ms.");
        }
        if (!QStringList{"auto", "compare", "merge", "directory", "capabilities"}.contains(request.mode_)) return fail("Unsupported mode: " + request.mode_);
        if (!QStringList{"text", "json"}.contains(request.format_)) return fail("Unsupported report format.");
        if (!request.encoding_.isEmpty() && !QStringList{"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}.contains(request.encoding_)) return fail("Unsupported encoding.");
        if (!request.theme_.isEmpty() && request.theme_ != "light" && request.theme_ != "dark") return fail("Invalid theme.");
        if (request.mode_ == "capabilities") return result;
        if (request.file1_.isEmpty() && request.file2_.isEmpty() && !request.headless_ && Arguments.size() == 1) return result;
        if (request.file1_.isEmpty() || request.file2_.isEmpty()) return fail("Both -file1 and -file2 are required.");
        if (!request.base_.isEmpty() && !request.file3_.isEmpty()) return fail("-base and -file3 are mutually exclusive.");
        if (request.mode_ == "merge")
        {
            if (request.output_.isEmpty() || !request.file3_.isEmpty()) return fail("Merge requires -output and does not accept -file3.");
            if (!request.takeAll_.isEmpty() && (!request.base_.isEmpty() || (request.takeAll_ != "left" && request.takeAll_ != "right"))) return fail("-takeall left|right is only valid for two-way merge.");
        }
        else if (!request.output_.isEmpty() || !request.takeAll_.isEmpty() || request.overwrite_ || request.dryRun_) return fail("Output/write options require -mode merge.");
        for (auto* path : {&request.file1_, &request.file2_, &request.file3_, &request.base_, &request.output_, &request.planOut_, &request.decisions_})
        { if (!path->isEmpty()) *path = QDir::cleanPath(QFileInfo(*path).absoluteFilePath()); }
        if (!request.report_.isEmpty() && request.report_ != "-") request.report_ = QFileInfo(request.report_).absoluteFilePath();
        if (request.output_ == "-" || QFileInfo(request.output_).fileName() == "-") return fail("Merge output must be a file path.");
        if (!request.report_.isEmpty() && request.report_ != "-")
            for (const auto& path : {request.file1_, request.file2_, request.file3_, request.base_, request.output_, request.planOut_, request.decisions_})
                if (!path.isEmpty() && request.report_.compare(path, Qt::CaseInsensitive) == 0) return fail("Report path aliases an input/output.");
        for (const auto& artifact : {request.planOut_, request.decisions_})
        {
            if (artifact.isEmpty()) continue;
            if (QFileInfo(artifact).fileName() == "-") return fail("Decision plan requires a file path.");
            for (const auto& path : {request.file1_, request.file2_, request.file3_, request.base_, request.output_})
                if (!path.isEmpty() && artifact.compare(path, Qt::CaseInsensitive) == 0) return fail("Decision plan aliases an input/output.");
        }
        return result;
    }
}
