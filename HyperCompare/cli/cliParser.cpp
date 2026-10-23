#include "stdafx.h"
#include "runtime/session.hpp"
#include "runtime/requestFile.hpp"
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
            "Three-way merge: -mode merge -base BASE -file1 LEFT -file2 RIGHT -output RESULT\n"
            "Paths accept existing mapped drives and UNC shares using the current Windows session.\n"
            "Example: -file1 \\\\server\\share\\left.txt -file2 Z:\\right.txt");
        parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        const auto flag = [&](const QString& Name, const QString& Description) { parser.addOption({Name, Description}); };
        const auto value = [&](const QString& Name, const QString& Description) { parser.addOption({Name, Description, "value"}); };
        flag("help", "Show supported options."); flag("version", "Show product and CLI schema versions.");
        value("request", "Schema 1 request JSON; relative paths resolve from its directory (maximum 1 MiB).");
        for (const auto& name : {"file1", "file2", "file3", "base", "output"}) value(name, "Path for this input/output role.");
        value("mode", "auto, compare, merge, directory, directory-merge, apply, batch, capabilities, probe.");
        value("manifest", "Batch schema 1 JSON manifest; maximum 4 MiB and 1000 jobs.");
        value("jobs", "Batch concurrent jobs: 1 (default) or 2."); flag("failfast", "Stop starting batch jobs after exit code 2; differences do not stop the batch.");
        value("format", "text or json. stdout contains only this report."); value("report", "Report file; '-' means stdout.");
        flag("include-content", "Include bounded DOCX paragraph text in JSON reports; default reports contain locations only.");
        value("encoding", "Explicit UTF-8, UTF-16LE, UTF-16BE or CP949 for all inputs.");
        value("takeall", "Explicit whole-side selection for two-way merge: left or right.");
        value("planout", "Export a text merge decision template or directory merge plan to a new JSON file (headless only).");
        value("decisions", "Apply a JSON decision plan bound to the current inputs (headless only).");
        value("timeout-ms", "Cooperative request deadline in milliseconds (100..3600000); pending native/network I/O may exceed it.");
        value("include", "Directory include globs separated by ';'; filename or root-relative path with '/'.");
        value("exclude", "Directory exclusion globs separated by ';'; matching directories are pruned.");
        value("directory-level", "bytes (default) or metadata; metadata does not verify file contents.");
        value("theme", "GUI theme: light or dark.");
        value("semantic", "auto (default C/C++ function relations) or off. Syntactic annotations never resolve merge conflicts.");
        flag("headless", "Run without a GUI."); flag("gui", "Open the GUI (console launcher waits).");
        flag("wait", "Return the completed request status."); flag("exitcode", "Return comparison/merge status; GUI requires -wait.");
        flag("overwrite", "Allow replacing an existing output, subject to revision validation.");
        value("plan", "Reviewed directory plan to apply with -mode apply.");
        value("journal", "Directory apply journal; default PLAN.journal, outside all roots.");
        flag("resume", "Verify and resume an interrupted directory plan.");
        flag("allowdelete", "Authorize the reviewed plan removals; no implicit recursive deletion.");
        flag("mirror", "Directory plan: include removals for output-only paths; apply requires explicit deletion authorization.");
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
        if (parser.isSet("request"))
        {
            if (parser.value("request").isEmpty()) return fail("Empty request path.");
            QStringList overrides;
            for (const auto& name : parser.optionNames())
            {
                if (name == "request") continue;
                if (!QStringList{"headless", "gui", "wait", "exitcode", "format", "report", "timeout-ms", "include-content"}.contains(name)) return fail("Option cannot be combined with -request: " + name);
                overrides << '-' + name; if (QStringList{"format", "report", "timeout-ms"}.contains(name)) overrides << parser.value(name);
            }
            return ParseRequestFile(parser.value("request"), overrides, Console);
        }
        request.file1_ = parser.value("file1"); request.file2_ = parser.value("file2");
        request.file3_ = parser.value("file3"); request.base_ = parser.value("base"); request.output_ = parser.value("output");
        request.encoding_ = parser.value("encoding"); request.report_ = parser.value("report");
        request.takeAll_ = parser.value("takeall"); request.theme_ = parser.value("theme");
        if (parser.isSet("semantic")) request.semantic_ = parser.value("semantic");
        if (request.semantic_ != "auto" && request.semantic_ != "off") return fail("Invalid semantic mode.");
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
            !QStringList{"auto", "directory", "directory-merge"}.contains(parser.isSet("mode") ? parser.value("mode") : "auto")) return fail("Directory options require auto or directory mode.");
        request.planOut_ = parser.value("planout"); request.decisions_ = parser.value("decisions");
        for (const auto& name : {"planout", "decisions"})
            if (parser.isSet(name) && parser.value(name).isEmpty()) return fail("Empty path: " + QString(name));
        if (parser.isSet("mode")) request.mode_ = parser.value("mode");
        if (parser.isSet("format")) request.format_ = parser.value("format");
        if (parser.isSet("headless") && parser.isSet("gui")) return fail("-gui and -headless are mutually exclusive.");
        request.gui_ = parser.isSet("gui"); request.headless_ = parser.isSet("headless") || (Console && !request.gui_);
        request.wait_ = parser.isSet("wait") || Console;
        request.overwrite_ = parser.isSet("overwrite"); request.dryRun_ = parser.isSet("dryrun");
        request.includeContent_ = parser.isSet("include-content");
        if (request.includeContent_ && (!request.headless_ || (request.format_ != "json" && request.mode_ != "batch"))) return fail("-include-content requires headless JSON reporting.");
        request.applyPlan_ = parser.value("plan"); request.journal_ = parser.value("journal");
        request.resume_ = parser.isSet("resume"); request.allowDelete_ = parser.isSet("allowdelete");
        if (request.mode_ != "apply" && (parser.isSet("plan") || parser.isSet("journal") || request.resume_ || request.allowDelete_)) return fail("Apply options require -mode apply.");
        request.mirror_ = parser.isSet("mirror");
        if (request.mirror_ && request.mode_ != "directory-merge") return fail("-mirror requires directory-merge.");
        const bool decisionOptions = !request.planOut_.isEmpty() || !request.decisions_.isEmpty();
        if (decisionOptions && (!request.headless_ || (request.mode_ != "merge" && request.mode_ != "directory-merge"))) return fail("-planout/-decisions require headless -mode merge.");
        if (decisionOptions && request.mode_ != "directory-merge" && !request.takeAll_.isEmpty()) return fail("-takeall cannot be combined with decision plan options.");
        if (!request.planOut_.isEmpty() && !request.decisions_.isEmpty()) return fail("Use -planout and -decisions in separate requests.");
        if (!request.headless_ && (request.dryRun_ || parser.isSet("report") || parser.isSet("format")))
            return fail("-dryrun/-report/-format require headless mode in this version.");
        if (parser.isSet("exitcode") && !request.wait_ && !request.headless_) return fail("-exitcode requires -wait in GUI mode.");
        if (parser.isSet("timeout-ms"))
        {
            bool valid = false; request.timeoutMs_ = parser.value("timeout-ms").toInt(&valid);
            if (!valid || request.timeoutMs_ < 100 || request.timeoutMs_ > 3600000) return fail("Invalid timeout-ms.");
        }
        if (!QStringList{"auto", "compare", "merge", "directory", "directory-merge", "apply", "batch", "capabilities", "probe"}.contains(request.mode_)) return fail("Unsupported mode: " + request.mode_);
        if (request.mode_ == "probe" && !request.headless_) return fail("Probe requires headless mode.");
        if (request.mode_ == "batch")
        {
            for (const auto& name : parser.optionNames())
                if (!QStringList{"mode", "manifest", "jobs", "failfast", "headless", "wait", "exitcode", "format", "report", "timeout-ms", "include-content"}.contains(name)) return fail("Option not valid for batch: " + name);
            if (!request.headless_ || parser.value("manifest").isEmpty()) return fail("Batch requires headless -manifest.");
            request.format_ = parser.isSet("format") ? parser.value("format") : "ndjson"; if (request.format_ != "ndjson") return fail("Batch requires ndjson format.");
            if (parser.isSet("jobs")) { bool valid = false; request.jobs_ = parser.value("jobs").toInt(&valid); if (!valid || request.jobs_ < 1 || request.jobs_ > 2) return fail("Batch jobs must be 1 or 2."); }
            request.manifest_ = QDir::cleanPath(QFileInfo(parser.value("manifest")).absoluteFilePath()); request.failFast_ = parser.isSet("failfast");
            if (!request.report_.isEmpty() && request.report_ != "-") request.report_ = QDir::cleanPath(QFileInfo(request.report_).absoluteFilePath()); return result;
        }
        if (parser.isSet("manifest") || parser.isSet("jobs") || parser.isSet("failfast")) return fail("Batch options require -mode batch.");
        if (!QStringList{"text", "json"}.contains(request.format_)) return fail("Unsupported report format.");
        if (!request.encoding_.isEmpty() && !QStringList{"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}.contains(request.encoding_)) return fail("Unsupported encoding.");
        if (!request.theme_.isEmpty() && request.theme_ != "light" && request.theme_ != "dark") return fail("Invalid theme.");
        if (request.mode_ == "apply")
        {
            if (!request.headless_ || request.applyPlan_.isEmpty()) return fail("Apply requires headless -plan.");
            for (const auto& name : parser.optionNames())
                if (!QStringList{"mode", "plan", "journal", "resume", "allowdelete", "headless", "wait", "exitcode", "dryrun", "format", "report", "timeout-ms"}.contains(name)) return fail("Option not valid for apply: " + name);
            for (auto* path : {&request.applyPlan_, &request.journal_, &request.report_})
                if (!path->isEmpty() && *path != "-") *path = QDir::cleanPath(QFileInfo(*path).absoluteFilePath());
            if (request.applyPlan_ == "-" || (parser.isSet("journal") && request.journal_.isEmpty())) return fail("Apply requires file paths.");
            return result;
        }
        if (request.mode_ == "capabilities") return result;
        if (request.file1_.isEmpty() && request.file2_.isEmpty() && !request.headless_ && Arguments.size() == 1) return result;
        if (request.file1_.isEmpty() || request.file2_.isEmpty()) return fail("Both -file1 and -file2 are required.");
        if (!request.base_.isEmpty() && !request.file3_.isEmpty()) return fail("-base and -file3 are mutually exclusive.");
        if (request.mode_ == "directory-merge")
        {
            if ((request.headless_ && request.planOut_.isEmpty()) || request.output_.isEmpty() || !request.decisions_.isEmpty() || !request.file3_.isEmpty() || request.dryRun_)
                return fail("directory-merge requires -output and headless requests also require -planout; use -base for three-way.");
            if (!request.takeAll_.isEmpty() && (!request.base_.isEmpty() || (request.takeAll_ != "left" && request.takeAll_ != "right"))) return fail("Invalid directory selection.");
        }
        else if (request.mode_ == "merge")
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
