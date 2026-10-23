#include "stdafx.h"
#include "core/compare.hpp"
#include "core/linearDiff.hpp"
#include "core/overviewMap.hpp"
#include "runtime/session.hpp"
#include "runtime/inputEditing.hpp"
#include "runtime/saveRecovery.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include <QStandardPaths>
#include <QScopeGuard>
#include <QProcessEnvironment>
#include "runtime/directoryPlan.hpp"
#include "runtime/directoryApply.hpp"
#include "runtime/decisionPlan.hpp"
#include "runtime/mergeWork.hpp"
#include "runtime/semantic.hpp"
#include "runtime/engineRegistry.hpp"
#include "runtime/workerHost.hpp"
#include "runtime/controllerMemory.hpp"
#include "runtime/requestFile.hpp"
#include "runtime/batch.hpp"
#include "runtime/gitTool.hpp"
#include "zipFixture.hpp"
#include "docxFixture.hpp"
#include <QCryptographicHash>
#include <QBuffer>
#include <QImage>
#include <QtEndian>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <random>
#include <numeric>
#include <future>
#include <thread>
#include <Windows.h>
#include <sddl.h>

using namespace hc::compare;
class CompareTests final : public QObject
{
    Q_OBJECT
private:
    static QByteArray png(const QImage& Image)
    { QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); if (!Image.save(&buffer, "PNG")) qFatal("PNG fixture failed"); return bytes; }
    static void write(const QString& Path, const QByteArray& Data)
    { QFile file(Path); if (!file.open(QIODevice::WriteOnly) || file.write(Data) != Data.size()) qFatal("Fixture write failed"); }
    static QByteArray read(const QString& Path)
    { QFile file(Path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
    static void requestFile(const QString& Path, const QString& Left, const QString& Right, const QString& Output = {})
    {
        QJsonObject json{{"schemaVersion", 1}, {"mode", Output.isEmpty() ? "compare" : "merge"},
            {"inputs", QJsonObject{{"left", QJsonObject{{"path", Left}}}, {"right", QJsonObject{{"path", Right}}}}}};
        if (!Output.isEmpty()) { json["output"] = QJsonObject{{"path", Output}}; json["options"] = QJsonObject{{"takeall", "left"}}; }
        write(Path, QJsonDocument(json).toJson());
    }
    static void manifest(const QString& Path, const QStringList& Requests)
    { QJsonArray jobs; for (int i = 0; i < Requests.size(); ++i) jobs.append(QJsonObject{{"jobId", "job" + QString::number(i)}, {"request", Requests[i]}}); write(Path, QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"jobs", jobs}}).toJson()); }
    static std::size_t lcs(const Lines& A, const Lines& B)
    {
        std::vector<std::size_t> row(B.size() + 1);
        for (const auto line : A)
        {
            auto previous = row;
            for (std::size_t j = 1; j <= B.size(); ++j) row[j] = line == B[j - 1] ? previous[j - 1] + 1 : std::max(row[j - 1], previous[j]);
        }
        return row.back();
    }
private slots:
    void gitToolConfigurationAndShellQuoting()
    {
        QTemporaryDir dir; const auto script = dir.filePath(QString::fromUtf8("도구 ' $ & [x].sh"));
        write(script, "#!/bin/sh\nprintf '%s\\0' \"$@\"\nexit 3\n");
        const auto config = dir.filePath("gitconfig"); write(config, GitToolConfig(script).toUtf8());
        const auto git = QStandardPaths::findExecutable("git"); QVERIFY(!git.isEmpty());
        QProcess process; process.start(git, {"config", "--file", config, "--get", "difftool.hypercompare.cmd"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto command = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        const auto shell = QDir(QFileInfo(git).absolutePath()).absoluteFilePath("../bin/sh.exe"); QVERIFY(QFileInfo::exists(shell));
        auto env = QProcessEnvironment::systemEnvironment(); const auto local = QString::fromUtf8("공백 ' $() & [a].txt");
        env.insert("LOCAL", local); env.insert("REMOTE", "remote `literal`.txt"); process.setProcessEnvironment(env);
        process.start(shell, {"-c", command}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 3);
        const auto fields = process.readAllStandardOutput().split('\0'); QCOMPARE(fields.size(), 7);
        QCOMPARE(fields[0], QByteArray("-git-tool")); QCOMPARE(fields[1], QByteArray("diff")); QCOMPARE(fields[3], local.toUtf8()); QCOMPARE(fields[5], QByteArray("remote `literal`.txt"));
        QVERIFY_EXCEPTION_THROWN(GitToolConfig("bad\npath"), Failure);
        process.setProcessEnvironment(QProcessEnvironment::systemEnvironment()); process.setWorkingDirectory(dir.path());
        process.start(git, {"init", "-q"}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto fileName = QString::fromUtf8("추가 ' $ [a].txt"); write(dir.filePath(fileName), "new\n");
        process.start(git, {"add", "--", fileName}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        process.start(git, {"-c", "include.path=" + config, "difftool", "--no-prompt", "--trust-exit-code", "--tool=hypercompare", "--cached", "--", fileName});
        QVERIFY(process.waitForFinished(10000)); QVERIFY(process.exitCode() != 0); // Git for Windows may wrap the tool code as 128.
        const auto gitArgs = process.readAllStandardOutput().split('\0'); QCOMPARE(gitArgs.size(), 7);
        QCOMPARE(gitArgs[1], QByteArray("diff")); QCOMPARE(gitArgs[3], QByteArray("/dev/null")); QVERIFY(gitArgs[5].endsWith(fileName.toUtf8()));
        process.start(git, {"hash-object", "-w", "--", fileName}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto hash = process.readAllStandardOutput().trimmed(); QByteArray index = "0 " + QByteArray(40, '0') + '\t' + fileName.toUtf8() + '\0';
        for (int stage : {1, 2, 3}) index += "100644 " + hash + ' ' + QByteArray::number(stage) + '\t' + fileName.toUtf8() + '\0';
        process.start(git, {"update-index", "-z", "--index-info"}); QVERIFY(process.waitForStarted()); process.write(index); process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        process.start(git, {"ls-files", "-u", "-z"}); QVERIFY(process.waitForFinished(10000)); const auto before = process.readAllStandardOutput(); QVERIFY(!before.isEmpty());
        process.start(git, {"-c", "include.path=" + config, "mergetool", "--no-prompt", "--tool=hypercompare", "--", fileName});
        QVERIFY(process.waitForFinished(10000)); QVERIFY(process.exitCode() != 0);
        QVERIFY(process.readAllStandardOutput().contains(QByteArray("-git-tool\0merge\0", 16)));
        process.start(git, {"ls-files", "-u", "-z"}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.readAllStandardOutput(), before);
        process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-git-tool", "config"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0); QVERIFY(process.readAllStandardOutput().contains("hideResolved = false"));
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-git-tool", "merge", "-takeall", "left"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 2);
    }
    void gitToolRolesStatesAndExitCodes()
    {
        QTemporaryDir dir; const auto git = QStandardPaths::findExecutable("git"); QVERIFY(!git.isEmpty());
        const auto invoke = [&](const QStringList& Args, const QByteArray& Input = QByteArray{}) {
            QProcess process; process.setWorkingDirectory(dir.path()); process.start(git, Args);
            if (!process.waitForStarted(5000)) return QPair<int, QByteArray>{-1, {}};
            process.write(Input); process.closeWriteChannel();
            if (!process.waitForFinished(10000)) return QPair<int, QByteArray>{-1, {}};
            return QPair<int, QByteArray>{process.exitCode(), process.readAllStandardOutput()};
        };
        QCOMPARE(invoke({"init", "-q"}).first, 0);
        const auto targetName = QString::fromUtf8("충돌 ' $ [1].txt"), target = dir.filePath(targetName);
        const auto left = dir.filePath("local"), right = dir.filePath("remote"), base = dir.filePath("base");
        write(left, "left\n"); write(right, "right\n"); write(base, "base\n"); write(target, "unresolved\n");
        const auto blob = invoke({"hash-object", "-w", "--stdin"}, "base\n"); QCOMPARE(blob.first, 0); const auto hash = blob.second.trimmed();
        const auto setStages = [&](const QList<int>& Stages, const QByteArray& Mode = QByteArray("100644")) {
            QByteArray data = "0 " + QByteArray(40, '0') + '\t' + targetName.toUtf8() + '\0';
            for (int stage : Stages) data += Mode + ' ' + hash + ' ' + QByteArray::number(stage) + '\t' + targetName.toUtf8() + '\0';
            return invoke({"update-index", "-z", "--index-info"}, data).first;
        };
        const auto cancel = std::make_shared<std::atomic_bool>(false); QByteArray output; QString error; Request captured; QStringList received; int calls = 0, status = 0;
        const GitGuiSubmit submit = [&](const QStringList& Args, const Request& Input, const auto&, QString&) { received = Args; captured = Input; ++calls; return status; };
        const QStringList args{"app", "-git-tool", "merge", "-file1", left, "-file2", right, "-base", base, "-output", target};
        QCOMPARE(setStages({1, 2, 3}), 0);
        const auto originalIndex = read(dir.filePath(".git/index"));
        for (int code : {0, 1, 2, 3})
        {
            status = code; QCOMPARE(RunGitTool(args, cancel, output, error, submit), code); QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(captured.gui_); QVERIFY(captured.wait_); QVERIFY(captured.overwrite_); QCOMPARE(captured.mode_, QString("merge"));
            QCOMPARE(captured.base_, base); QCOMPARE(read(target), QByteArray("unresolved\n"));
        }
        QCOMPARE(read(dir.filePath(".git/index")), originalIndex); QCOMPARE(calls, 4);
        QCOMPARE(setStages({2, 3}), 0); status = 0;
        QCOMPARE(RunGitTool(args, cancel, output, error, submit), 2); QCOMPARE(error, QString("InvalidGitEmptyBase"));
        write(base, {}); QCOMPARE(RunGitTool(args, cancel, output, error, submit), 0); QCOMPARE(calls, 5);
        for (const auto& stages : {QList<int>{1, 2}, QList<int>{1, 3}, QList<int>{2}, QList<int>{1}, QList<int>{}})
        { QCOMPARE(setStages(stages), 0); QCOMPARE(RunGitTool(args, cancel, output, error, submit), 2); QCOMPARE(error, QString("UnsupportedVcsState")); }
        QCOMPARE(setStages({1, 2, 3}, "120000"), 0); QCOMPARE(RunGitTool(args, cancel, output, error, submit), 2); QCOMPARE(calls, 5);
        QCOMPARE(setStages({1, 2, 3}), 0);
        const GitGuiSubmit changeIndex = [&](const auto&, const auto&, const auto&, QString&) { setStages({2, 3}); return 0; };
        QCOMPARE(RunGitTool(args, cancel, output, error, changeIndex), 2); QCOMPARE(error, QString("GitIndexChangedDuringMerge"));
        QStringList diff{"app", "-git-tool", "diff", "-file1", left, "-file2", right};
        for (int code : {0, 1, 2, 3}) { status = code; QCOMPARE(RunGitTool(diff, cancel, output, error, submit), code == 1 ? 0 : code); }
        diff[4] = "/dev/null"; bool emptySeen = false;
        const GitGuiSubmit emptySubmit = [&](const auto&, const Request& Input, const auto&, QString&) { emptySeen = QFileInfo(Input.file1_).isFile() && read(Input.file1_).isEmpty(); return 1; };
        for (const auto& nullPath : {QString("/dev/null"), QString("nul"), QString("\\\\.\\NUL")})
        { diff[4] = nullPath; emptySeen = false; QCOMPARE(RunGitTool(diff, cancel, output, error, emptySubmit), 0); QVERIFY(emptySeen); }
        cancel->store(true); QCOMPARE(RunGitTool(args, cancel, output, error, submit), 3);
        QCOMPARE(GitToolExitCode(true, -1), 2); QCOMPARE(GitToolExitCode(false, 4), 2);
    }
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void boundedLineScanning()
    {
        const auto failure = [](auto Operation) {
            try { Operation(); return std::string{}; } catch (const Failure& error) { return error.code_; }
        };
        QCOMPARE(CountLines(""), std::size_t(0));
        const std::string mixed = "a\r\nb\rc\nend";
        const auto lines = SplitLines(mixed, {}, 4, 3); QCOMPARE(lines.size(), std::size_t(4));
        QCOMPARE(lines[0], std::string_view("a\r\n")); QCOMPARE(lines[1], std::string_view("b\r"));
        QCOMPARE(lines[2], std::string_view("c\n")); QCOMPARE(lines[3], std::string_view("end"));
        QCOMPARE(failure([&] { CountLines(mixed, {}, 3, 3); }), std::string("LineLimit"));
        QCOMPARE(failure([&] { SplitLines("a\r\n", {}, 1, 2); }), std::string("LongLineLimit"));
        QCOMPARE(failure([&] { CountLines("x", {}, 0); }), std::string("LineLimit"));
        QCOMPARE(CountLines("", {}, 0, 0), std::size_t(0));
        const std::string many(16 * 1024 * 1024, '\n'); QElapsedTimer timer; timer.start();
        QCOMPARE(failure([&] { CountLines(many, {}, 300000, 256 * 1024); }), std::string("LineLimit"));
        qInfo("16 MiB newline input rejected without line-index allocation in %lld ms", timer.elapsed());
        QCOMPARE(failure([&] { Diff(many, ""); }), std::string("LineLimit"));
        const std::string hugeLine(16 * 1024 * 1024, 'x');
        QCOMPARE(failure([&] { SplitLines(hugeLine, {}, 300000, 256 * 1024); }), std::string("LongLineLimit"));
        std::atomic_bool canceled{true}; Budget budget; budget.canceled_ = &canceled;
        QCOMPARE(failure([&] { CountLines(hugeLine, budget); }), std::string("Canceled"));
        QCOMPARE(failure([&] { SplitLines(many, budget); }), std::string("Canceled"));
        budget.canceled_ = nullptr; budget.deadline_ = std::chrono::steady_clock::now();
        QCOMPARE(failure([&] { CountLines(many, budget); }), std::string("Timeout"));
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right");
        write(request.file1_, QByteArray(many.data(), qsizetype(many.size()))); write(request.file2_, "right");
        auto cancel = std::make_shared<std::atomic_bool>(false);
        QCOMPARE(Execute(request, cancel, false).error_, QString("LineLimit"));
        write(request.file1_, "left"); const auto session = Execute(request, cancel, false);
        QCOMPARE(CompareOpenBuffers(request, session, {QByteArray(many.data(), qsizetype(many.size())), "right"}, {}).error_, QString("LineLimit"));
    }
    void recoveryCrashChild()
    {
        const auto root = qEnvironmentVariable("HC_SAVE_CRASH_ROOT"); if (root.isEmpty()) return;
        const auto phase = qEnvironmentVariableIntValue("HC_SAVE_CRASH_PHASE");
        const auto path = QDir(root).filePath("destination.txt");
        const hc::storage::Locator destination{path.startsWith("//") || path.startsWith("\\\\") ? hc::storage::LocatorKind::Smb : hc::storage::LocatorKind::Local, path.toStdString()};
        auto writer = hc::storage::MakeNativeWritableStorage(destination.kind_, hc::storage::MakeNativeReadProvider(destination.kind_));
        const auto inspected = writer->Inspect(destination, {}); QVERIFY(inspected.HasValue()); QVERIFY(inspected.Value());
        SaveRecoveryJournal journal(qEnvironmentVariable("HC_SAVE_CRASH_JOURNALS")); auto checkpoint = journal.Checkpoint();
        hc::storage::OperationContext context; context.writeCheckpoint_ = [checkpoint, phase](const auto& Record, auto Previous) {
            auto saved = checkpoint(Record, Previous);
            if (saved.HasValue() && int(Record.phase_) == phase) TerminateProcess(GetCurrentProcess(), 73);
            return saved;
        };
        const QByteArray payload("new result\n");
        const auto saved = hc::document::SaveDocumentBytes(*writer, destination, {true, inspected.Value()->id_.revision_},
            {reinterpret_cast<const std::byte*>(payload.constData()), std::size_t(payload.size())}, context);
        QFAIL(saved.HasValue() ? "Crash checkpoint did not fire" : saved.Failure().code_.c_str());
    }
    void saveRecoveryAtCommitBoundaries()
    {
        for (int phase = 0; phase <= 3; ++phase)
        {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("destination.txt"); write(path, "original\n");
            auto env = QProcessEnvironment::systemEnvironment(); env.insert("HC_SAVE_CRASH_ROOT", dir.path()); env.insert("HC_SAVE_CRASH_PHASE", QString::number(phase)); env.insert("HC_SAVE_CRASH_JOURNALS", dir.filePath("journals"));
            QProcess child; child.setProcessEnvironment(env); child.start(QCoreApplication::applicationFilePath(), {"recoveryCrashChild", "-o", dir.filePath("child.txt") + ",txt"});
            QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(15000)); QCOMPARE(child.exitCode(), 73);
            const auto files = QDir(dir.filePath("journals")).entryList({"*.hcsave"}, QDir::Files); QCOMPARE(files.size(), 1);
            Request request; request.mode_ = "recover"; request.journal_ = QDir(dir.filePath("journals")).filePath(files[0]); request.dryRun_ = true;
            auto result = Execute(request, std::make_shared<std::atomic_bool>(false), true); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.outcome_, QString("RecoveryPreview")); QVERIFY(!result.saveRecovery_["identitiesVerified"].toBool());
            QCOMPARE(QFileInfo::exists(path), phase != 2);
            const auto reviewedHash = result.saveRecovery_["journalSha256"].toString().toLatin1(); QCOMPARE(reviewedHash.size(), 64);
            if (phase == 2)
            {
                { QFile journal(request.journal_); QVERIFY(journal.open(QIODevice::Append)); journal.write("{torn"); }
                auto apply = request; apply.dryRun_ = false; Result bound; bool refused = false;
                try { RecoverSavedFile(apply, bound, {}, reviewedHash); }
                catch (const Failure& error) { refused = error.code_ == "SaveRecoveryPreviewChanged"; }
                QVERIFY(refused); QVERIFY(!QFileInfo::exists(path));
            }
            request.dryRun_ = false; result = Execute(request, std::make_shared<std::atomic_bool>(false), true);
            QVERIFY2(result.exitCode_ == 0, qPrintable(result.error_)); QVERIFY(result.saveRecovery_["identitiesVerified"].toBool());
            QCOMPARE(read(path), phase == 3 ? QByteArray("new result\n") : QByteArray("original\n"));
            if (phase == 2) QVERIFY(result.warnings_.contains("SaveRecoveryIgnoredTornTail"));
            QVERIFY(QFileInfo::exists(request.journal_)); QVERIFY(result.recovery_.size() >= 2);
            const auto again = Execute(request, std::make_shared<std::atomic_bool>(false), true); QCOMPARE(again.exitCode_, 0);
            QCOMPARE(read(path), phase == 3 ? QByteArray("new result\n") : QByteArray("original\n"));
        }
    }
    void saveRecoveryRejectsForeignDestinationAndCorruptJournal()
    {
        QTemporaryDir dir; const auto path = dir.filePath("destination.txt"); write(path, "original\n");
        auto env = QProcessEnvironment::systemEnvironment(); env.insert("HC_SAVE_CRASH_ROOT", dir.path()); env.insert("HC_SAVE_CRASH_PHASE", "2"); env.insert("HC_SAVE_CRASH_JOURNALS", dir.filePath("journals"));
        QProcess child; child.setProcessEnvironment(env); child.start(QCoreApplication::applicationFilePath(), {"recoveryCrashChild", "-o", dir.filePath("child.txt") + ",txt"}); QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(15000)); QCOMPARE(child.exitCode(), 73);
        const auto files = QDir(dir.filePath("journals")).entryList({"*.hcsave"}, QDir::Files); QCOMPARE(files.size(), 1);
        Request r; r.mode_ = "recover"; r.journal_ = QDir(dir.filePath("journals")).filePath(files[0]);
        write(path, "external change\n"); auto result = Execute(r, std::make_shared<std::atomic_bool>(false), true);
        QCOMPARE(result.exitCode_, 2); QCOMPARE(result.error_, QString("write-recovery-destination-changed")); QCOMPARE(read(path), QByteArray("external change\n"));
        const auto journal = read(r.journal_); write(r.journal_, journal + "bad complete record\n"); result = Execute(r, std::make_shared<std::atomic_bool>(false), true);
        QCOMPARE(result.error_, QString("SaveRecoveryCorrupt")); QCOMPARE(read(path), QByteArray("external change\n"));
        write(r.journal_, journal); auto mutated = journal; mutated.replace("backedUp", "finished"); write(r.journal_, mutated);
        result = Execute(r, std::make_shared<std::atomic_bool>(false), true); QCOMPARE(result.error_, QString("SaveRecoveryInconsistent"));
        QCOMPARE(read(path), QByteArray("external change\n"));
    }
    void saveRecoveryCleanupAndJournalFailure()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out"); r.takeAll_ = "right";
        write(r.file1_, "left\n"); write(r.file2_, "right\n");
        const QDir journals(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery");
        const auto before = journals.entryList({"*.hcsave"}, QDir::Files);
        auto saved = Execute(r, std::make_shared<std::atomic_bool>(false), true); QCOMPARE(saved.exitCode_, 0); QVERIFY(saved.recovery_.isEmpty()); QCOMPARE(read(r.output_), QByteArray("right\n"));
        QCOMPARE(journals.entryList({"*.hcsave"}, QDir::Files), before);
        r.overwrite_ = true; r.takeAll_ = "left"; saved = Execute(r, std::make_shared<std::atomic_bool>(false), true); QCOMPARE(saved.exitCode_, 0); QVERIFY(saved.recovery_.isEmpty()); QCOMPARE(read(r.output_), QByteArray("left\n"));
        QCOMPARE(journals.entryList({"*.hcsave"}, QDir::Files), before);
        // Losing access to the journal must stop the write before moving the original.
        const auto unavailable = dir.filePath("journal-folder"); SaveRecoveryJournal journal(unavailable); QVERIFY(QDir{}.rmdir(unavailable));
        hc::storage::Locator destination{hc::storage::LocatorKind::Local, r.output_.toStdString()};
        auto writer = hc::storage::MakeNativeWritableStorage(destination.kind_, hc::storage::MakeNativeReadProvider(destination.kind_));
        const auto expected = writer->Inspect(destination, {}); QVERIFY(expected.HasValue()); QVERIFY(expected.Value());
        hc::storage::OperationContext ctx; ctx.writeCheckpoint_ = journal.Checkpoint(); const QByteArray payload("should not publish");
        const auto failed = hc::document::SaveDocumentBytes(*writer, destination, {true, expected.Value()->id_.revision_},
            {reinterpret_cast<const std::byte*>(payload.constData()), std::size_t(payload.size())}, ctx);
        QVERIFY(!failed.HasValue()); QCOMPARE(failed.Failure().code_, std::string("save-journal-open")); QCOMPARE(read(r.output_), QByteArray("left\n"));
        QVERIFY(QDir(dir.path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void saveRecoveryCannotRunInBatch()
    {
        QTemporaryDir dir; write(dir.filePath("recover.json"), QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"mode", "recover"}, {"options", QJsonObject{{"journal", "unused.hcsave"}}}}).toJson());
        manifest(dir.filePath("batch.json"), {"recover.json"}); Request request; request.mode_ = "batch"; request.manifest_ = dir.filePath("batch.json");
        bool rejected = false;
        try { RunBatch(request, std::make_shared<std::atomic_bool>(false), [](const auto&) { return true; }); }
        catch (const Failure& failure) { rejected = failure.code_ == "BatchModeUnsupported"; }
        QVERIFY(rejected);
    }
    void recoveryJournalFailureProtectsWorkAndDirectoryOutput()
    {
        QTemporaryDir dir; Request merge; merge.mode_ = "merge"; merge.file1_ = dir.filePath("left"); merge.file2_ = dir.filePath("right"); merge.output_ = dir.filePath("result");
        write(merge.file1_, "left\n"); write(merge.file2_, "right\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto session = Execute(merge, cancel, false); const MergeWork work{session.inputs_[0].utf8_, session.plan_->initial_, false};
        const auto workPath = dir.filePath("work.hcmerge"); const auto saved = SaveMergeWorkFile(workPath, merge, session, work, {}, false, {}); const auto original = read(workPath);
        Request plan; plan.mode_ = "directory-merge"; plan.headless_ = true; plan.takeAll_ = "left"; plan.overwrite_ = true;
        plan.file1_ = dir.filePath("l"); plan.file2_ = dir.filePath("r"); plan.output_ = dir.filePath("out"); plan.planOut_ = dir.filePath("plan.json");
        for (const auto& path : {plan.file1_, plan.file2_, plan.output_}) QVERIFY(QDir{}.mkpath(path));
        write(plan.file1_ + "/a", "new"); write(plan.output_ + "/a", "old"); QCOMPARE(Execute(plan, cancel, true).exitCode_, 0);
        const auto oldName = QCoreApplication::applicationName(); QCoreApplication::setApplicationName("RecoveryBlocked-" + QUuid::createUuid().toString(QUuid::Id128));
        const auto blocked = SaveRecoveryDirectory(); const auto cleanup = qScopeGuard([oldName, blocked] { QFile::remove(blocked); QCoreApplication::setApplicationName(oldName); });
        QVERIFY(QDir{}.mkpath(QFileInfo(blocked).absolutePath())); write(blocked, "not a directory");
        QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(workPath, merge, session, work, saved.expected_, true, {}), Failure); QCOMPARE(read(workPath), original);
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = plan.planOut_;
        auto result = Execute(apply, cancel, true); QCOMPARE(result.error_, QString("SaveRecoveryDirectory")); QCOMPARE(read(plan.output_ + "/a"), QByteArray("old"));
        QVERIFY(QFile::remove(blocked)); apply.resume_ = true; result = Execute(apply, cancel, true);
        QVERIFY2(result.exitCode_ == 0, qPrintable(result.error_)); QCOMPARE(read(plan.output_ + "/a"), QByteArray("new"));
        const auto updated = SaveMergeWorkFile(workPath, merge, session, work, saved.expected_, true, {}); QVERIFY(updated.recovery_.isEmpty());
        QVERIFY(QDir(SaveRecoveryDirectory()).entryList({"*.hcsave"}, QDir::Files).isEmpty());
    }
    void inputSavePreservesEncodingAndRevision()
    {
        QTemporaryDir dir; const auto target = dir.filePath("target");
        write(target, QByteArray::fromHex("fffe61000a00")); auto snapshot = ReadTextSnapshot(target, "", {});
        auto saved = SaveInputBuffer(snapshot, QString::fromUtf8("가\n").toUtf8(), {}); QVERIFY2(saved.error_.isEmpty(), qPrintable(saved.error_));
        QCOMPARE(read(target), QByteArray::fromHex("fffe00ac0a00")); QVERIFY(saved.recovery_.isEmpty());
        write(target, QByteArray::fromHex("fffe62000a00")); saved = SaveInputBuffer(saved.snapshot_, "overwrite", {}); QVERIFY(!saved.error_.isEmpty()); QCOMPARE(read(target), QByteArray::fromHex("fffe62000a00"));
        write(target, QByteArray::fromHex("b0a10a")); snapshot = ReadTextSnapshot(target, "CP949", {});
        saved = SaveInputBuffer(snapshot, QString::fromUtf8("😀\n").toUtf8(), {}); QVERIFY(!saved.error_.isEmpty()); QCOMPARE(read(target), QByteArray::fromHex("b0a10a"));
        snapshot = ReadTextSnapshot(target, "CP949", {}); saved = SaveInputBuffer(snapshot, {}, {}); QVERIFY2(saved.error_.isEmpty(), qPrintable(saved.error_)); QCOMPARE(read(target), QByteArray{});
    }
    void saveRecoveryCommandLine()
    {
        auto parsed = Parse({"hc", "-mode", "recover", "-journal", "save.hcsave", "-dryrun", "-format", "json"}, true); QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.mode_, QString("recover"));
        QVERIFY(!Parse({"hc", "-mode", "recover", "-journal", "save.hcsave", "-report", "out.txt"}, true).error_.isEmpty());
        QVERIFY(!Parse({"hc", "-mode", "recover", "-journal", "save.hcsave", "-file1", "anything"}, true).error_.isEmpty());
        QVERIFY(Parse({"hc", "-mode", "recover"}, true).error_.isEmpty());
        Request list; list.mode_ = "recover"; const auto listed = Execute(list, std::make_shared<std::atomic_bool>(false), true);
        QCOMPARE(listed.exitCode_, 0); QCOMPARE(listed.outcome_, QString("RecoveryJournals")); QCOMPARE(listed.saveRecovery_["directory"].toString(), SaveRecoveryDirectory());
        QVERIFY(!Parse({"hc", "-mode", "recover", "-journal", "save.hcsave", "-gui"}, true).error_.isEmpty());
    }
    void guiTransportOptions()
    {
        auto parsed = Parse({"hc", "-gui", "-reuse", "-wait-timeout-ms", "100", "-file1", "left", "-file2", "right"}, true);
        QVERIFY(parsed.error_.isEmpty()); QVERIFY(parsed.request_.reuse_); QVERIFY(parsed.request_.wait_); QCOMPARE(parsed.request_.guiWaitMs_, 100);
        QVERIFY(!Parse({"hc", "-reuse", "-file1", "left", "-file2", "right"}, true).error_.isEmpty());
        QVERIFY(!Parse({"hc", "-wait-timeout-ms", "100", "-file1", "left", "-file2", "right"}, false).error_.isEmpty());
    }
    void largeBoundaryExactScripts()
    {
        std::string large, different; for (int i = 0; i < 20000; ++i) { large += "line-" + std::to_string(i) + "\r\n"; different += "other-" + std::to_string(i) + "\r\n"; }
        for (const auto& pair : {std::pair{std::string{}, large}, std::pair{large, std::string{}}, std::pair{large, different}, std::pair{std::string("head\ntail"), "head\n" + large + "tail"}})
        {
            const auto edits = Diff(pair.first, pair.second); QCOMPARE(edits.size(), std::size_t(1)); QCOMPARE(Apply(pair.first, edits), pair.second);
            QCOMPARE(edits[0].otherEnd_ - edits[0].otherBegin_, SplitLines(edits[0].replacement_).size());
            const auto map = AlignmentMap::Build(SplitLines(pair.first).size(), SplitLines(pair.second).size(), edits); QVERIFY(map.RowCount() >= 20000);
        }
        const auto merged = Merge("head\ntail", "head\n" + large + "tail", "head\nTAIL"); QVERIFY(merged.conflicts_.empty()); QCOMPARE(merged.text_, "head\n" + large + "TAIL");
        const auto shorter = large.substr(0, large.find("line-2500\r\n")), other = different.substr(0, different.find("other-2500\r\n"));
        const auto edits = Diff(shorter, other); QCOMPARE(edits.size(), std::size_t(1)); QCOMPARE(Apply(shorter, edits), other);
        Budget low; low.steps_ = 10; QVERIFY_EXCEPTION_THROWN(Diff("", large, low), Failure); QVERIFY_EXCEPTION_THROWN(Diff(large, different, low), Failure);
        std::atomic_bool stop{true}; low.canceled_ = &stop; QVERIFY_EXCEPTION_THROWN(Diff("", large, low), Failure);
    }
    void disjointShortcutRetainsSharedInterior()
    {
        std::string left, right;
        for (int i = 0; i < 500; ++i) { left += "left\n"; right += "right\n"; }
        for (int i = 0; i < 4000; ++i) { left += "shared\n"; right += "shared\n"; }
        for (int i = 0; i < 500; ++i) { left += "left\n"; right += "right\n"; }
        const auto edits = Diff(left, right); QCOMPARE(edits.size(), std::size_t(2)); QCOMPARE(Apply(left, edits), right);
        QCOMPARE(edits[0].end_, std::size_t(500)); QCOMPARE(edits[1].begin_, std::size_t(4500));
    }
    void batchResultsConcurrencyAndConsole()
    {
        QTemporaryDir dir; write(dir.filePath("a"), "a"); write(dir.filePath("b"), "b");
        requestFile(dir.filePath("same.json"), "a", "a"); requestFile(dir.filePath("diff.json"), "a", "b"); requestFile(dir.filePath("merge.json"), "a", "b", "out");
        const auto path = dir.filePath("batch.json"); manifest(path, {"same.json", "diff.json", "merge.json"});
        auto parsed = Parse({"hc", "-mode", "batch", "-manifest", path, "-jobs", "2"}, true); QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_));
        QVector<QJsonObject> events; QCOMPARE(RunBatch(parsed.request_, std::make_shared<std::atomic_bool>(false), [&](const auto& Event) { events.append(Event); return true; }), 1);
        QCOMPARE(events.back()["event"].toString(), "batchSummary"); QCOMPARE(events.back()["success"].toInt(), 2); QCOMPARE(events.back()["different"].toInt(), 1); QCOMPARE(read(dir.filePath("out")), "a");
        QSet<QString> ids; for (const auto& event : events) if (event["event"] == "result") { QVERIFY(!ids.contains(event["jobId"].toString())); ids.insert(event["jobId"].toString()); } QCOMPARE(ids.size(), 3);
        manifest(path, {"same.json", "diff.json"}); QProcess cli; cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-mode", "batch", "-manifest", path, "-jobs", "2"});
        QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 1); QVERIFY(cli.readAllStandardError().isEmpty());
        auto lines = cli.readAllStandardOutput().trimmed().split('\n'); QCOMPARE(lines.size(), 7); QCOMPARE(QJsonDocument::fromJson(lines.back()).object()["event"].toString(), "batchSummary");
        const auto report = dir.filePath("report.ndjson"); cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-mode", "batch", "-manifest", path, "-report", report});
        QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 1); QVERIFY(cli.readAllStandardOutput().isEmpty()); QCOMPARE(read(report).trimmed().split('\n').size(), 7);
    }
    void batchFailFastAndDifferencePriority()
    {
        QTemporaryDir dir; write(dir.filePath("a"), "a"); write(dir.filePath("b"), "b");
        requestFile(dir.filePath("first.json"), "missing", "b"); requestFile(dir.filePath("second.json"), "a", "b", "out"); manifest(dir.filePath("batch.json"), {"first.json", "second.json"});
        const auto parsed = Parse({"hc", "-mode", "batch", "-manifest", dir.filePath("batch.json"), "-failfast"}, true); QVERIFY(parsed.error_.isEmpty()); QVector<QJsonObject> events;
        auto sink = [&](const QJsonObject& Event) { events.append(Event); return true; };
        QCOMPARE(RunBatch(parsed.request_, std::make_shared<std::atomic_bool>(false), sink), 2); QVERIFY(!QFileInfo::exists(dir.filePath("out"))); QCOMPARE(events.back()["skipped"].toInt(), 1);
        requestFile(dir.filePath("first.json"), "a", "b"); events.clear(); QCOMPARE(RunBatch(parsed.request_, std::make_shared<std::atomic_bool>(false), sink), 1); QCOMPARE(read(dir.filePath("out")), "a"); QCOMPARE(events.back()["skipped"].toInt(), 0);
    }
    void batchPreflightConflictsAndManifestValidation()
    {
        QTemporaryDir dir; write(dir.filePath("a"), "a"); write(dir.filePath("b"), "b"); const auto path = dir.filePath("batch.json");
        requestFile(dir.filePath("one.json"), "a", "b", "out"); requestFile(dir.filePath("two.json"), "a", "b", "out"); manifest(path, {"one.json", "two.json"});
        auto parsed = Parse({"hc", "-mode", "batch", "-manifest", path}, true); QVERIFY(parsed.error_.isEmpty()); int delivered = 0;
        auto reject = [&] { bool rejected = false; try { RunBatch(parsed.request_, std::make_shared<std::atomic_bool>(false), [&](const auto&) { ++delivered; return true; }); } catch (const Failure&) { rejected = true; } QVERIFY(rejected); QCOMPARE(delivered, 0); QVERIFY(!QFileInfo::exists(dir.filePath("out"))); };
        reject(); requestFile(dir.filePath("two.json"), "out", "b"); reject();
        requestFile(dir.filePath("two.json"), "a", "b"); parsed.request_.report_ = dir.filePath("a"); reject(); parsed.request_.report_.clear();
        const auto alias = dir.filePath("alias"); QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(dir.filePath("a").utf16()), nullptr));
        requestFile(dir.filePath("two.json"), "a", "b", "alias"); reject();
        write(path, "{\"schemaVersion\":1,\"jobs\":[],\"jobs\":[]}"); reject();
        manifest(path, {"one.json"}); auto json = QJsonDocument::fromJson(read(path)).object(); auto jobs = json["jobs"].toArray(); jobs.append(jobs[0]); json["jobs"] = jobs; write(path, QJsonDocument(json).toJson()); reject();
        QVERIFY(!Parse({"hc", "-mode", "batch", "-manifest", path, "-jobs", "3"}, true).error_.isEmpty());
        QVERIFY(!Parse({"hc", "-mode", "batch", "-manifest", path, "-gui"}, true).error_.isEmpty());
    }
    void batchCancellationAndDeliveryFailure()
    {
        QTemporaryDir dir; write(dir.filePath("a"), "a"); write(dir.filePath("b"), "b"); requestFile(dir.filePath("one.json"), "a", "b", "out"); requestFile(dir.filePath("two.json"), "a", "b"); manifest(dir.filePath("batch.json"), {"one.json", "two.json"});
        const auto parsed = Parse({"hc", "-mode", "batch", "-manifest", dir.filePath("batch.json")}, true); QVERIFY(parsed.error_.isEmpty()); auto canceled = std::make_shared<std::atomic_bool>(false); QVector<QJsonObject> events;
        QCOMPARE(RunBatch(parsed.request_, canceled, [&](const QJsonObject& Event) { events.append(Event); if (Event["event"] == "started") canceled->store(true); return true; }), 3);
        QCOMPARE(events.back()["canceled"].toInt(), 2); QVERIFY(!QFileInfo::exists(dir.filePath("out")));
        canceled->store(false); QCOMPARE(RunBatch(parsed.request_, canceled, [](const auto&) { return false; }), 2); QVERIFY(canceled->load()); QVERIFY(!QFileInfo::exists(dir.filePath("out")));
        auto timed = parsed.request_; timed.timeoutMs_ = 100; canceled->store(false);
        QCOMPARE(RunBatch(timed, canceled, [](const QJsonObject& Event) { if (Event["event"] == "started") std::this_thread::sleep_for(std::chrono::milliseconds(150)); return true; }), 2); QVERIFY(!QFileInfo::exists(dir.filePath("out")));
        canceled->store(false); QCOMPARE(RunBatch(parsed.request_, canceled, [](const QJsonObject& Event) { return Event["event"] != "result"; }), 2); QCOMPARE(read(dir.filePath("out")), "a");
    }
    void batchAppliesReviewedDirectoryPlan()
    {
        QTemporaryDir dir; QVERIFY(QDir().mkdir(dir.filePath("left"))); QVERIFY(QDir().mkdir(dir.filePath("right"))); write(dir.filePath("left/a"), "copy");
        Request plan; plan.mode_ = "directory-merge"; plan.headless_ = true; plan.file1_ = dir.filePath("left"); plan.file2_ = dir.filePath("right"); plan.output_ = dir.filePath("output"); plan.planOut_ = dir.filePath("plan.json"); plan.takeAll_ = "left";
        auto planned = Execute(plan, std::make_shared<std::atomic_bool>(false), true); QVERIFY2(planned.error_.isEmpty(), qPrintable(planned.error_));
        write(dir.filePath("apply.json"), QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"mode", "apply"}, {"options", QJsonObject{{"plan", "plan.json"}}}}).toJson());
        manifest(dir.filePath("batch.json"), {"apply.json"}); const auto parsed = Parse({"hc", "-mode", "batch", "-manifest", dir.filePath("batch.json")}, true); QVERIFY(parsed.error_.isEmpty());
        QVector<QJsonObject> events; QCOMPARE(RunBatch(parsed.request_, std::make_shared<std::atomic_bool>(false), [&](const auto& Event) { events.append(Event); return true; }), 0);
        QCOMPARE(read(dir.filePath("output/a")), "copy"); QCOMPARE(events[1]["result"].toObject()["commit"].toString(), "Committed");
        // The apply output must not contain its batch controller file, even for a resumed plan.
        manifest(dir.filePath("output/batch.json"), {"../apply.json"}); auto collision = parsed.request_; collision.manifest_ = dir.filePath("output/batch.json");
        bool rejected = false; try { RunBatch(collision, std::make_shared<std::atomic_bool>(false), [](const auto&) { return true; }); } catch (const Failure& error) { rejected = error.code_ == "BatchPathConflict"; } QVERIFY(rejected);
    }
    void requestFilesRelativeRolesAndEncodings()
    {
        QTemporaryDir dir; const auto requestPath = dir.filePath(QString::fromUtf8("요청 😀.json"));
        write(dir.filePath("left.txt"), QByteArray::fromHex("b0a10a")); write(dir.filePath("right.txt"), QString::fromUtf8("가\n").toUtf8());
        const QJsonObject json{{"schemaVersion", 1}, {"requestId", "encoding-test"}, {"mode", "compare"},
            {"inputs", QJsonObject{{"left", QJsonObject{{"path", "left.txt"}}}, {"right", QJsonObject{{"path", "right.txt"}}}}},
            {"policy", QJsonObject{{"encodingByRole", QJsonObject{{"left", "CP949"}, {"right", "UTF-8"}}}}}};
        write(requestPath, QJsonDocument(json).toJson()); auto parsed = Parse({"hc", "-request", requestPath, "-format", "json"}, true);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_)); QCOMPARE(parsed.request_.file1_, dir.filePath("left.txt")); QCOMPARE(InputEncoding(parsed.request_, 0), "CP949");
        auto result = Execute(parsed.request_, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QVERIFY(result.edits_.empty()); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.outcome_, "Equivalent"); QCOMPARE(result.Json()["requestId"].toString(), "encoding-test"); QCOMPARE(result.inputs_[0].encoding_, "CP949");
        auto three = json; auto inputs = three["inputs"].toObject(); inputs["center"] = QJsonObject{{"path", "right.txt"}}; three["inputs"] = inputs; write(requestPath, QJsonDocument(three).toJson());
        parsed = Parse({"hc", "-request", requestPath, "-timeout-ms", "1234"}, true); QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.file2_, dir.filePath("right.txt")); QCOMPARE(parsed.request_.file3_, dir.filePath("right.txt")); QCOMPARE(parsed.request_.timeoutMs_, 1234);
        QProcess cli; cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-request", requestPath, "-format", "json"}); QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 0);
        QCOMPARE(QJsonDocument::fromJson(cli.readAllStandardOutput()).object()["operation"].toString(), "compare3");
    }
    void requestValidationAndProtectedFiles()
    {
        QTemporaryDir dir; const auto path = dir.filePath("request.json"); const QJsonObject json{{"schemaVersion", 1}, {"mode", "merge"},
            {"inputs", QJsonObject{{"left", QJsonObject{{"path", "a"}}}, {"right", QJsonObject{{"path", "b"}}}}},
            {"output", QJsonObject{{"path", "out"}}}, {"options", QJsonObject{{"takeall", "left"}}}};
        write(dir.filePath("a"), "a"); write(dir.filePath("b"), "b");
        const auto bytes = QJsonDocument(json).toJson();
        QVector<QByteArray> invalid{QByteArray(1024 * 1024 + 1, ' '), "{}", "{\"schemaVersion\":1,\"schem\\u0061Version\":1}"};
        auto value = json; value["unknown"] = true; invalid << QJsonDocument(value).toJson(); value = json; value["schemaVersion"] = 2; invalid << QJsonDocument(value).toJson();
        value = json; value["limits"] = QJsonObject{{"timeoutMs", 500.5}}; invalid << QJsonDocument(value).toJson();
        value = json; value["output"] = QJsonObject{{"path", "request.json"}, {"overwrite", true}}; invalid << QJsonDocument(value).toJson();
        value = json; value["policy"] = QJsonObject{{"encodingByRole", QJsonObject{{"left", "unknown"}}}}; invalid << QJsonDocument(value).toJson();
        for (const auto& data : invalid) { write(path, data); QVERIFY2(!Parse({"hc", "-request", path}, true).error_.isEmpty(), data.constData()); QCOMPARE(read(path), data); }
        write(path, bytes); QVERIFY(!Parse({"hc", "-request", path, "-file1", "override"}, true).error_.isEmpty());
        const auto alias = dir.filePath("alias.json"); QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(path.utf16()), nullptr));
        QVERIFY(!Parse({"hc", "-request", path, "-report", alias}, true).error_.isEmpty());
        auto parsed = Parse({"hc", "-request", path}, true); QVERIFY(parsed.error_.isEmpty()); const auto result = Execute(parsed.request_, std::make_shared<std::atomic_bool>(false), true);
        QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.exitCode_, 0); QCOMPARE(read(dir.filePath("out")), "a"); QCOMPARE(read(path), bytes);
    }
    void docxParagraphProjectionAndLocations()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.docx"); request.file2_ = dir.filePath("right.docx"); request.headless_ = true;
        const auto common = test::Paragraph(QString::fromUtf8("한글 & <문단> 😀"));
        write(request.file1_, test::Docx(common + test::Paragraph("left") + "<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t><w:br/><w:t>c</w:t></w:r></w:p>"));
        write(request.file2_, test::Docx(common + test::Paragraph("right") + "<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t><w:br/><w:t>c</w:t></w:r></w:p>"));
        auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.exitCode_, 1);
        QCOMPARE(result.operation_, "compare-document"); QVERIFY(!result.Json()["coverage"].toObject()["scopeComplete"].toBool());
        QVERIFY(!result.Json()["extension"].toObject()["documents"].toArray()[0].toObject()["paragraphs"].toArray()[0].toObject().contains("text"));
        QVERIFY(result.Json(true)["extension"].toObject()["documents"].toArray()[0].toObject()["paragraphs"].toArray()[0].toObject().contains("text"));
        const auto changes = result.extension_["changes"].toArray(); QCOMPARE(changes.size(), 1); QCOMPARE(changes[0].toObject()["begin"].toInt(), 1); QCOMPARE(changes[0].toObject()["end"].toInt(), 2);
        const auto paragraphs = result.extension_["documents"].toArray()[0].toObject()["paragraphs"].toArray(); QCOMPARE(paragraphs.size(), 3);
        QCOMPARE(paragraphs[2].toObject()["text"].toString(), "a\tb\nc"); QCOMPARE(paragraphs[0].toObject()["text"].toString(), QString::fromUtf8("한글 & <문단> 😀"));
        const auto location = paragraphs[1].toObject()["location"].toObject(); QCOMPARE(location["schema"].toString(), "office.paragraph.v1");
        QCOMPARE(location["payload"].toObject()["paragraph"].toInt(), 1); QCOMPARE(location["payload"].toObject()["locator"].toObject()["entryPath"].toString(), "word/document.xml");
        request.mode_ = "probe"; result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QCOMPARE(result.exitCode_, 0);
        request.mode_ = "merge"; result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY(!result.error_.isEmpty());
    }
    void docxScopeStrictAndPackageChanges()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.docx"); request.file2_ = dir.filePath("right.docx"); request.headless_ = true;
        const auto body = test::Paragraph("same") + "<w:tbl><w:tr><w:tc>" + test::Paragraph("table") + "</w:tc></w:tr></w:tbl>" +
            "<w:del>" + test::Paragraph("deleted") + "</w:del><w:p><w:r><w:drawing><w:txbxContent>" + test::Paragraph("textbox") + "</w:txbxContent></w:drawing></w:r></w:p>";
        write(request.file1_, test::Docx(body, "left-style")); write(request.file2_, test::Docx(body, "right-style", true));
        auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.exitCode_, 1);
        QVERIFY(result.extension_["textEqual"].toBool()); QVERIFY(!result.extension_["contentsEqual"].toBool());
        const auto paragraphs = result.extension_["documents"].toArray()[1].toObject()["paragraphs"].toArray(); QCOMPARE(paragraphs.size(), 3); QCOMPARE(paragraphs[1].toObject()["text"].toString(), "table");
        write(request.file2_, read(request.file1_)); result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QCOMPARE(result.exitCode_, 0);
        QProcess cli; cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", request.file1_, "-file2", request.file2_, "-format", "json"}); QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 0);
        QCOMPARE(QJsonDocument::fromJson(cli.readAllStandardOutput()).object()["extension"].toObject()["engine"].toString(), "office.docx");
        cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", request.file1_, "-file2", request.file2_, "-format", "json", "-include-content"}); QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 0);
        QCOMPARE(QJsonDocument::fromJson(cli.readAllStandardOutput()).object()["extension"].toObject()["documents"].toArray()[0].toObject()["paragraphs"].toArray()[0].toObject()["text"].toString(), "same");
    }
    void docxRejectsInvalidXmlRelationshipsAndLimits()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.docx"); request.file2_ = dir.filePath("right.docx"); request.headless_ = true;
        write(request.file2_, test::Docx(test::Paragraph("valid")));
        auto reject = [&](const QByteArray& Bytes, const QString& Reason) {
            write(request.file1_, Bytes); const auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false);
            QCOMPARE(result.exitCode_, 2); QVERIFY2(result.error_.contains(Reason), qPrintable(result.error_));
        };
        reject(test::StoredZip({{"other", "not a document"}}), "DocxMissingPart");
        reject(test::Docx("<w:p>"), "DocxInvalidXml");
        reject(test::Docx(test::Paragraph("ok"), {}, false, "../outside.xml"), "DocxPartPathUnsupported");
        reject(test::Docx(test::Paragraph("ok"), {}, false, "https://invalid.example/document.xml"), "DocxPartPathUnsupported");
        reject(test::Docx(test::Paragraph("ok"), {}, false, "word/document.xml\" TargetMode=\"External"), "DocxExternalMainPart");
        QByteArray deep; for (int i = 0; i < 257; ++i) deep += "<w:sdt>"; for (int i = 0; i < 257; ++i) deep += "</w:sdt>"; reject(test::Docx(deep), "DocxXmlDepthLimit");
        QByteArray many; for (int i = 0; i < 50001; ++i) many += "<w:p/>"; reject(test::Docx(many), "DocxParagraphLimit");
        reject(test::Docx(test::Paragraph(QString(256 * 1024 + 1, 'x'))), "DocxTextLimit");
        const auto dtd = test::StoredZip({{"_rels/.rels", "<!DOCTYPE Relationships [<!ENTITY x 'x'>]><Relationships xmlns='http://schemas.openxmlformats.org/package/2006/relationships'/>"}, {"[Content_Types].xml", ""}});
        reject(dtd, "DocxDtdUnsupported");
    }
    void zipEntriesContentsAndNestedLocations()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.zip"); request.file2_ = dir.filePath("right.zip"); request.headless_ = true;
        write(request.file1_, test::StoredZip({{"same", "same"}, {"changed.txt", "left"}, {"left-only", ""}, {"folder/", ""}, {QString::fromUtf8("한글.txt").toUtf8(), "unicode"}}));
        write(request.file2_, test::StoredZip({{"same", "same"}, {"changed.txt", "right"}, {"right-only", "new"}, {"folder", "file"}, {QString::fromUtf8("한글.txt").toUtf8(), "unicode"}}));
        auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QCOMPARE(result.operation_, "compare-archive"); QCOMPARE(result.extension_["engine"].toString(), "container.zip");
        const auto entries = result.extension_["entries"].toArray(); QCOMPARE(entries.size(), 6);
        QMap<QString, QJsonObject> paths; for (const auto& entry : entries) paths.insert(entry.toObject()["path"].toString(), entry.toObject());
        QCOMPARE(paths["changed.txt"]["state"].toString(), "Changed"); QCOMPARE(paths["folder"]["state"].toString(), "TypeChanged");
        QCOMPARE(paths["left-only"]["state"].toString(), "LeftOnly"); QCOMPARE(paths["right-only"]["state"].toString(), "RightOnly");
        const auto location = paths["changed.txt"]["left"].toObject()["location"].toObject();
        QCOMPARE(location["schema"].toString(), "archive.entry.v1"); QCOMPARE(location["payload"].toObject()["ordinal"].toInt(), 1);
        QCOMPARE(location["payload"].toObject()["locator"].toObject()["entryPath"].toString(), "changed.txt");
        QCOMPARE(location["resourceRevision"].toString(), "sha256:" + QString::fromLatin1(QCryptographicHash::hash(read(request.file1_), QCryptographicHash::Sha256).toHex()));
        QVERIFY(!QFileInfo::exists(dir.filePath("changed.txt"))); request.mode_ = "probe";
        result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QCOMPARE(result.exitCode_, 0); QVERIFY(!result.engineProbe_["mergeTwoAvailable"].toBool());
        QCOMPARE(result.engineProbe_["inputs"].toArray()[0].toObject()["validation"].toString(), "all-zip-entries-verified");
    }
    void zipRejectionsAndContentIntegrity()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.zip"); request.file2_ = dir.filePath("right.zip"); request.headless_ = true;
        const auto good = test::StoredZip({{"a", "hello"}}); write(request.file1_, good); auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto rejected = [&](const QByteArray& Bytes) { write(request.file2_, Bytes); return Execute(request, cancel, true).error_; };
        QCOMPARE(rejected(test::StoredZip({{"../escape.txt", "x"}})), "ZipUnsafePath");
        QCOMPARE(rejected(test::StoredZip({{"a", "x"}, {"a", "y"}})), "ZipDuplicateNameUnsupported");
        QCOMPARE(rejected(test::StoredZip({{"a", "x", 0x801}})), "EncryptedZipUnsupported");
        QCOMPARE(rejected(test::StoredZip({{"link", "target", 0x800, 0120000u << 16}})), "ZipSpecialEntryUnsupported");
        QCOMPARE(rejected(test::StoredZip({{QString::fromUtf8("한글").toUtf8(), "x", 0}})), "ZipNameEncodingUnsupported");
        auto corrupt = good; corrupt[31] = 'X'; QCOMPARE(rejected(corrupt), "ZipContentInvalid");
        corrupt = good; corrupt.chop(10); QCOMPARE(rejected(corrupt), "InvalidZip");
        auto large = good; const auto central = large.indexOf(QByteArray::fromHex("504b0102"));
        qToLittleEndian<quint16>(8, large.data() + 8); qToLittleEndian<quint16>(8, large.data() + central + 10);
        qToLittleEndian<quint32>(16 * 1024 * 1024 + 1, large.data() + 22); qToLittleEndian<quint32>(16 * 1024 * 1024 + 1, large.data() + central + 24);
        QCOMPARE(rejected(large), "ArchiveExpandedLimit");
        QVERIFY(!QFileInfo::exists(dir.filePath("escape.txt")));
    }
    void zipPackagingDifferencesAndConsole()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.zip"); request.file2_ = dir.filePath("right.zip"); request.headless_ = true;
        write(request.file1_, test::StoredZip({{"a", "a"}, {"B", "b"}})); write(request.file2_, test::StoredZip({{"B", "b"}, {"a", "a"}}, "comment"));
        const auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QVERIFY(result.extension_["contentsEqual"].toBool()); QVERIFY(!result.extension_["rawEqual"].toBool()); QCOMPARE(result.exitCode_, 1);
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", request.file1_, "-file2", request.file2_, "-format", "json"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 1); QCOMPARE(QJsonDocument::fromJson(process.readAllStandardOutput()).object()["extension"].toObject()["payloadSchema"].toString(), "archive.entries.v1");
        write(request.file1_, test::StoredZip({})); write(request.file2_, test::StoredZip({})); QCOMPARE(Execute(request, std::make_shared<std::atomic_bool>(false), false).exitCode_, 0);
    }
    void zipDeflateAndEntryBudget()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("compressed.zip"); request.file2_ = dir.filePath("stored.zip"); request.headless_ = true;
        const auto compressed = QByteArray::fromBase64("UEsDBBQAAAAIAENuV11aGkI8IAAAAEAGAAAOAAAAY29tcHJlc3NlZC50eHRLzs8tKEotLk5N0U1JLEnkSh7lj/JH+aP8Uf4onwg+AFBLAQIUABQAAAAIAENuV11aGkI8IAAAAEAGAAAOAAAAAAAAAAAAAACAAQAAAABjb21wcmVzc2VkLnR4dFBLBQYAAAAAAQABADwAAABMAAAAAAA=");
        write(request.file1_, compressed); write(request.file2_, test::StoredZip({{"compressed.txt", QByteArray("compressed-data\n").repeated(100)}}));
        auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QVERIFY(result.extension_["contentsEqual"].toBool()); QCOMPARE(result.extension_["entries"].toArray()[0].toObject()["state"].toString(), "Equal");
        QVector<test::ZipEntry> many; for (int i = 0; i < 10001; ++i) many.append({QByteArray::number(i), ""});
        write(request.file2_, test::StoredZip(many)); result = Execute(request, std::make_shared<std::atomic_bool>(false), false); QCOMPARE(result.error_, "ArchiveEntryLimit");
    }
    void pngPixelsMetadataAndLocations()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.png"); request.file2_ = dir.filePath("right.png"); request.headless_ = true;
        QImage a(20, 12, QImage::Format_RGBA8888); a.fill(QColor(10, 20, 30, 85)); auto b = a.copy(); b.setPixelColor(5, 3, QColor(44, 55, 66, 85));
        write(request.file1_, png(a)); write(request.file2_, png(b)); const auto canceled = std::make_shared<std::atomic_bool>(false);
        auto result = Execute(request, canceled, false); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.exitCode_, 1);
        QCOMPARE(result.operation_, "compare-image"); const auto pair = result.extension_["pairs"].toArray()[0].toObject();
        QCOMPARE(pair["differentPixels"].toInteger(), 1); QCOMPARE(pair["bounds"].toObject(), (QJsonObject{{"x", 5}, {"y", 3}, {"width", 1}, {"height", 1}}));
        QCOMPARE(pair["locations"].toArray()[0].toObject()["schema"].toString(), "image.rect.v1");
        QVERIFY(!result.extension_["documents"].toArray()[0].toObject().contains("rgba"));
        b = a; b.setText("Comment", "metadata changed"); write(request.file2_, png(b)); result = Execute(request, canceled, false);
        QCOMPARE(result.exitCode_, 1); QVERIFY(result.extension_["pairs"].toArray()[0].toObject()["pixelEqual"].toBool());
        write(request.file2_, read(request.file1_)); result = Execute(request, canceled, false); QCOMPARE(result.exitCode_, 0);
        request.mode_ = "probe"; result = Execute(request, canceled, false); QCOMPARE(result.exitCode_, 0);
        QCOMPARE(result.engineProbe_["inputs"].toArray()[0].toObject()["validation"].toString(), "full-png-decode"); QVERIFY(!result.engineProbe_["mergeTwoAvailable"].toBool());
        request.mode_ = "merge"; request.output_ = dir.filePath("output.png"); result = Execute(request, canceled, true);
        QCOMPARE(result.error_, "ExtensionMergeUnsupported"); QVERIFY(!QFileInfo::exists(request.output_));
    }
    void pngAlphaDimensionsAndRejections()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("a.png"); request.file2_ = dir.filePath("b.png"); request.headless_ = true;
        QImage a(10, 20, QImage::Format_RGBA8888); a.fill(QColor(10, 20, 30, 0)); QImage b(20, 12, QImage::Format_RGBA8888); b.fill(QColor(10, 20, 30, 0));
        write(request.file1_, png(a)); write(request.file2_, png(b)); auto canceled = std::make_shared<std::atomic_bool>(false);
        auto result = Execute(request, canceled, false); QCOMPARE(result.extension_["pairs"].toArray()[0].toObject()["differentPixels"].toInteger(), 200);
        const auto locations = result.extension_["pairs"].toArray()[0].toObject()["locations"].toArray();
        QCOMPARE(locations[0].toObject()["payload"].toObject()["width"].toInt(), 10);
        QCOMPARE(locations[1].toObject()["payload"].toObject()["height"].toInt(), 12);
        b = a.copy(); b.setPixelColor(0, 0, QColor(99, 20, 30, 0)); write(request.file2_, png(b)); result = Execute(request, canceled, false);
        QCOMPARE(result.extension_["pairs"].toArray()[0].toObject()["differentPixels"].toInteger(), 1);
        auto invalid = png(a); qToBigEndian<quint32>(8193, invalid.data() + 16); write(request.file2_, invalid); result = Execute(request, canceled, false); QCOMPARE(result.error_, "ImagePixelLimit");
        invalid = png(a); invalid[24] = char(16); write(request.file2_, invalid); result = Execute(request, canceled, false); QCOMPARE(result.error_, "Png16BitUnsupported");
        invalid = png(a); invalid.insert(invalid.size() - 12, QByteArray::fromHex("000000086163544c000000010000000000000000"));
        write(request.file2_, invalid); result = Execute(request, canceled, false); QCOMPARE(result.error_, "AnimatedPngUnsupported");
        invalid = png(a); invalid.chop(5); write(request.file2_, invalid); result = Execute(request, canceled, false); QCOMPARE(result.error_, "TruncatedOrTrailingPng");
        request.file3_ = request.file1_; result = Execute(request, canceled, false); QCOMPARE(result.error_, "ExtensionThreeUnsupported");
    }
    void pngMaximumRasterAndConsole()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("a.png"); request.file2_ = dir.filePath("b.png");
        QImage image(2048, 2048, QImage::Format_RGBA8888); image.fill(QColor(20, 40, 60, 128)); write(request.file1_, png(image)); write(request.file2_, read(request.file1_));
        QElapsedTimer timer; timer.start(); const auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false);
        QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.exitCode_, 0);
        const auto document = result.extension_["documents"].toArray()[0].toObject();
        QCOMPARE(QByteArray::fromBase64(document["rgba"].toString().toLatin1()).size(), 2048 * 2048 * 4);
        qInfo("PNG two 2048x2048 full raster response: %lld ms", timer.elapsed());
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", request.file1_, "-file2", request.file2_, "-format", "json"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object(); QCOMPARE(report["extension"].toObject()["payloadSchema"].toString(), "image.rgba.v1");
        QVERIFY(!report["extension"].toObject()["documents"].toArray()[0].toObject().contains("rgba"));
    }
    void comparisonReportStorageAndCoverage()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt");
        write(r.file1_, "left secret"); write(r.file2_, "right secret"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        auto session = Execute(r, cancel, false); QCOMPARE(session.completion_, QString("Complete")); const auto path = dir.filePath("report.json");
        const auto receipt = SaveComparisonReport(path, r, session, {{"scope", "loaded-snapshots"}}, false, {});
        QVERIFY(receipt.warnings_.isEmpty()); QVERIFY(receipt.recovery_.isEmpty()); const auto original = read(path);
        QVERIFY(!original.contains("left secret")); QVERIFY(!original.contains("right secret"));
        QCOMPARE(QJsonDocument::fromJson(original).object()["reportKind"].toString(), QString("gui-comparison-snapshot"));
        QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(path, r, session, {}, false, {}), Failure); QCOMPARE(read(path), original);
        SaveComparisonReport(path, r, session, {{"updated", true}}, true, {});
        QVERIFY(QJsonDocument::fromJson(read(path)).object()["viewState"].toObject()["updated"].toBool());
        QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(r.file1_, r, session, {}, true, {}), Failure); QCOMPARE(read(r.file1_), QByteArray("left secret"));
        const auto alias = dir.filePath("alias.json"); QVERIFY(CreateHardLinkW(reinterpret_cast<const wchar_t*>(alias.utf16()), reinterpret_cast<const wchar_t*>(r.file1_.utf16()), nullptr));
        QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(alias, r, session, {}, true, {}), Failure); QCOMPARE(read(alias), QByteArray("left secret"));
        auto protectedRequest = r; protectedRequest.protectedInputs_.append(path);
        QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(path, protectedRequest, session, {}, true, {}), Failure);
        Budget stopped; std::atomic_bool canceled{true}; stopped.canceled_ = &canceled;
        QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(dir.filePath("canceled.json"), r, session, {}, false, stopped), Failure);
        QVERIFY(!QFileInfo::exists(dir.filePath("canceled.json")));
        session.extension_ = {{"payloadSchema", "image.rgba.v1"}, {"documents", QJsonArray{QJsonObject{{"rgba", "secret-raster"}, {"width", 1}}}}};
        SaveComparisonReport(path, r, session, {}, true, {}); QVERIFY(!read(path).contains("secret-raster"));
        r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_));
        session = Execute(r, cancel, false); QVERIFY_EXCEPTION_THROWN(SaveComparisonReport(r.file1_ + "/report.json", r, session, {}, false, {}), Failure);
        QVERIFY(!QFileInfo::exists(r.file1_ + "/report.json"));
    }
    void boundedSearchWorker()
    {
        const auto raw = QString("가 123\n나 456\n").toUtf8();
        QJsonObject input{{"text", QString::fromLatin1(raw.toBase64())}, {"query", "\\d+"}, {"regex", true}};
        auto result = RunWorker("search.text", input, {}); QCOMPARE(result["status"].toString(), QString("Complete"));
        QCOMPARE(result["matches"].toArray().size(), 2); QCOMPARE(result["matches"].toArray()[0].toObject()["begin"].toInt(), 4);
        input["query"] = "["; QCOMPARE(RunWorker("search.text", input, {})["reason"].toString(), QString("InvalidSearchExpression"));
        input["query"] = "^"; result = RunWorker("search.text", input, {}); QVERIFY(result["matches"].toArray().size() >= 2);
        input["text"] = QString::fromLatin1(QByteArray(10000, 'x').toBase64()); input["query"] = "x"; input["all"] = false; input["cursor"] = 9000;
        result = RunWorker("search.text", input, {}); QVERIFY(result["matches"].toArray().contains(QJsonObject{{"begin", 9000}, {"end", 9001}}));
        WorkerPolicy shortPolicy; shortPolicy.timeoutMs_ = 1;
        QCOMPARE(RunWorker("search.text", input, {}, shortPolicy)["status"].toString(), QString("Unavailable"));
    }
    void controllerMemoryAdmission()
    {
        constexpr std::uint64_t mib = 1024 * 1024;
        std::atomic<std::uint64_t> measured{100 * mib};
        auto pool = std::make_shared<ControllerMemory>([&] { return measured.load(); });
        auto scope = pool->Enter(1024 * mib);
        auto first = pool->TryWorker(512 * mib, 64 * mib, 64 * mib); QCOMPARE(first.bytes_, 512 * mib);
        auto second = pool->TryWorker(512 * mib, 64 * mib, 64 * mib); QCOMPARE(second.bytes_, 284 * mib);
        QVERIFY(!pool->TryWorker(512 * mib, 64 * mib, 64 * mib).lease_);
        QCOMPARE(pool->Reserved(), 924 * mib);
        measured = 120 * mib; QVERIFY(!pool->TryWorker(128 * mib, 64 * mib, 32 * mib).lease_); pool->Check();
        measured = 100 * mib;
        QVERIFY_EXCEPTION_THROWN(pool->Enter(256 * mib), Failure); pool->Check();
        second = {}; first = {}; QCOMPARE(pool->Reserved(), std::uint64_t(0));
        auto smaller = pool->Enter(256 * mib);
        auto adjusted = pool->TryWorker(512 * mib, 64 * mib, 64 * mib); QCOMPARE(adjusted.bytes_, 92 * mib);
        adjusted = {}; smaller.reset();
        measured = 1025 * mib; QVERIFY_EXCEPTION_THROWN(pool->TryWorker(256 * mib, 64 * mib, 64 * mib), Failure);
        measured = 100 * mib;
        // Concurrent reservations cannot each spend the same available bytes.
        std::vector<std::future<bool>> futures; std::atomic_bool release{false}; std::atomic_int entered{0};
        for (int i = 0; i < 16; ++i) futures.push_back(std::async(std::launch::async, [&] {
            auto reservation = pool->TryWorker(128 * mib, 128 * mib, 32 * mib); ++entered;
            while (!release.load()) std::this_thread::yield(); return bool(reservation.lease_);
        }));
        while (entered.load() != 16) std::this_thread::yield();
        const auto reserved = pool->Reserved(); release = true; int admitted = 0; for (auto& future : futures) admitted += future.get();
        QCOMPARE(admitted, 5); QCOMPARE(reserved, 800 * mib); QCOMPARE(pool->Reserved(), std::uint64_t(0));
        scope.reset(); QVERIFY_EXCEPTION_THROWN(pool->Enter(255 * mib), Failure);
    }
    void controllerReservationsAndStreamingReport()
    {
        constexpr std::uint64_t mib = 1024 * 1024; std::uint64_t measured = 100 * mib;
        auto pool = std::make_shared<ControllerMemory>([&] { return measured; }); auto scope = pool->Enter(256 * mib);
        auto first = pool->ReserveController(100 * mib); QCOMPARE(pool->Reserved(), 100 * mib);
        QVERIFY_EXCEPTION_THROWN(pool->ReserveController(57 * mib), Failure);
        QVERIFY(!pool->TryWorker(64 * mib, 64 * mib, 0).lease_);
        measured = 200 * mib; pool->Check(); first.reset(); QCOMPARE(pool->Reserved(), std::uint64_t(0));
        { auto last = pool->ReserveController(56 * mib); QVERIFY_EXCEPTION_THROWN(pool->ReserveController(1), Failure); }
        QCOMPARE(pool->Reserved(), std::uint64_t(0));
        QTemporaryDir dir; Request request; request.format_ = "json"; request.report_ = dir.filePath("report.json");
        Result result; result.operation_ = "directory2"; result.completion_ = "Complete"; result.exitCode_ = 0;
        for (int i = 0; i < 20000; ++i) result.entries_.append(QJsonObject{{"path", QString("한글/quote\"%1.txt").arg(i)}, {"status", "Equivalent"}});
        QCOMPARE(WriteReport(request, result), 0);
        QCOMPARE(QJsonDocument::fromJson(read(request.report_)).object(), result.Json());
        QCOMPARE(result.Json(false, false)["entries"].toArray().size(), 0);
        SaveComparisonReport(dir.filePath("gui.json"), request, result, {{"scope", "test"}}, false, {});
        const auto gui = QJsonDocument::fromJson(read(dir.filePath("gui.json"))).object();
        QCOMPARE(gui["entries"], result.Json()["entries"]); QCOMPARE(gui["viewState"].toObject()["scope"].toString(), QString("test"));
    }
    void controllerMemoryCliAndRequest()
    {
        for (const auto& value : {"255", "2049", "0", "-1", "256.5", "bad"})
            QVERIFY(!Parse({"hc", "-file1", "a", "-file2", "b", "-memory-mib", value}, true).error_.isEmpty());
        auto parsed = Parse({"hc", "-file1", "a", "-file2", "b", "-memory-mib", "256"}, true);
        QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.memoryMiB_, 256);
        QTemporaryDir dir; const auto path = dir.filePath("request.json");
        write(dir.filePath("a.cpp"), "int f(){return 1;}\n"); write(dir.filePath("b.cpp"), "int f(){return 2;}\n");
        QJsonObject json{{"schemaVersion", 1}, {"mode", "compare"},
            {"inputs", QJsonObject{{"left", QJsonObject{{"path", "a.cpp"}}}, {"right", QJsonObject{{"path", "b.cpp"}}}}},
            {"limits", QJsonObject{{"memoryMiB", 512}}}};
        write(path, QJsonDocument(json).toJson()); parsed = Parse({"hc", "-request", path}, true);
        QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.memoryMiB_, 512);
        parsed = Parse({"hc", "-request", path, "-memory-mib", "256"}, true); QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.memoryMiB_, 256);
        QProcess cli; cli.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-request", path, "-memory-mib", "256", "-format", "json"});
        QVERIFY(cli.waitForFinished(10000)); QCOMPARE(cli.exitCode(), 1);
        const auto report = QJsonDocument::fromJson(cli.readAllStandardOutput()).object(); QCOMPARE(report["completion"].toString(), "Complete");
        for (const auto& value : {QJsonValue(255), QJsonValue(2049), QJsonValue(256.5), QJsonValue("512")})
        { json["limits"] = QJsonObject{{"memoryMiB", value}}; write(path, QJsonDocument(json).toJson()); QVERIFY(!Parse({"hc", "-request", path}, true).error_.isEmpty()); }
        Budget limits; ConfigureMemoryBudget(limits, 1024);
        const auto before = ProcessControllerMemory()->Reserved();
        const auto response = RunWorker("source.tree-sitter", {{"language", "cpp"}, {"source", QString::fromLatin1(QByteArray("int f(){}").toBase64())}}, limits);
        QCOMPARE(response["status"].toString(), "Complete"); QVERIFY(response["workerMemoryLimitBytes"].toDouble() >= 64 * 1024 * 1024);
        QCOMPARE(ProcessControllerMemory()->Reserved(), before);
        Budget checked; checked.memoryCheck_ = [] { throw Failure("ControllerMemoryLimit"); };
        QVERIFY_EXCEPTION_THROWN(Diff("left", "right", checked), Failure);
        QCOMPARE(RunWorker("source.tree-sitter", {}, checked)["reason"].toString(), "ControllerMemoryLimit");
    }
    void workerSharedCapacityAndAbandonedLease()
    {
        struct Handles { HANDLE values_[2]{}; ~Handles() { for (auto value : values_) if (value) CloseHandle(value); } } handles;
        const auto owner = QString::fromLatin1(QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256).toHex());
        for (int i = 0; i < 2; ++i)
        { const auto name = QString("Local\\HyperCompare.Worker.%1.%2").arg(owner).arg(i); handles.values_[i] = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16())); QVERIFY(handles.values_[i]); }
        std::promise<DWORD> acquired; auto ready = acquired.get_future(); std::promise<void> release; auto released = release.get_future();
        std::thread holder([&] { const auto wait = WaitForMultipleObjects(2, handles.values_, TRUE, 3000); acquired.set_value(wait); released.wait(); });
        struct Join { std::promise<void>& release_; std::thread& thread_; ~Join() { if (thread_.joinable()) { release_.set_value(); thread_.join(); } } } join{release, holder};
        const auto held = ready.get(); QVERIFY(held == WAIT_OBJECT_0 || (held >= WAIT_ABANDONED_0 && held < WAIT_ABANDONED_0 + 2));
        Budget budget; budget.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
        QCOMPARE(RunWorker("source.tree-sitter", {{"language", "cpp"}, {"source", ""}}, budget)["reason"].toString(), "TimeLimit");
        release.set_value(); holder.join(); // Deliberately exit without ReleaseMutex: both slots become abandoned.
        QCOMPARE(RunWorker("source.tree-sitter", {{"language", "cpp"}, {"source", ""}}, Budget{})["status"].toString(), "Complete");
    }
    void workerProtocolAndPolicyBounds()
    {
        const QJsonObject payload{{"language", "cpp"}, {"source", QString::fromLatin1(QByteArray("int f(){}").toBase64())}};
        const auto response = RunWorker("source.tree-sitter", payload, Budget{});
        QCOMPARE(response["status"].toString(), "Complete"); QCOMPARE(response["engine"].toString(), "source.tree-sitter");
        QCOMPARE(response["digest"].toString(), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(RunWorker("unknown", payload, Budget{})["reason"].toString(), "WorkerEngineUnsupported");
        QCOMPARE(RunWorker("source.tree-sitter", {{"language", "cpp"}, {"source", "%%%"}}, Budget{})["reason"].toString(), "InvalidInputEncoding");
        WorkerPolicy policy; policy.requestBytes_ = 1;
        QCOMPARE(RunWorker("source.tree-sitter", payload, Budget{}, policy)["reason"].toString(), "WorkerInputLimit");
        policy = {}; policy.responseBytes_ = 1;
        QCOMPARE(RunWorker("source.tree-sitter", payload, Budget{}, policy)["reason"].toString(), "WorkerOutputLimit");
        policy = {}; policy.memoryBytes_ = 1024ull * 1024 * 1024;
        QCOMPARE(RunWorker("source.tree-sitter", payload, Budget{}, policy)["reason"].toString(), "WorkerPolicy");
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-internal-worker"}); QVERIFY(process.waitForStarted());
        const QJsonObject altered{{"schema", 1}, {"nonce", QString(36, 'a')}, {"engine", "source.tree-sitter"}, {"digest", QString(64, '0')}, {"request", payload}};
        process.write(QJsonDocument(altered).toJson(QJsonDocument::Compact)); process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 2);
    }
    void engineRegistryContractsAndSignatures()
    {
        QSet<QString> ids;
        for (const auto& engine : RegisteredEngines())
        {
            QVERIFY(!ids.contains(engine.id_)); ids.insert(engine.id_); QVERIFY(!engine.version_.isEmpty()); QVERIFY(!engine.locationSchema_.isEmpty());
            if (engine.mergeTwo_ || engine.mergeBase_) QVERIFY(!engine.writer_.isEmpty());
        }
        QCOMPARE(FindComparisonEngine("text")->id_, "text.myers"); QCOMPARE(FindComparisonEngine("png")->id_, "image.png");
        QCOMPARE(DetectResourceFormat("fake.png", "plain text"), "text");
        QCOMPARE(DetectResourceFormat("fake.txt", QByteArray::fromHex("89504e470d0a1a0a")), "png");
        QCOMPARE(DetectResourceFormat("a.DOCX", QByteArray::fromHex("504b0304")), "docx");
        QCOMPARE(DetectResourceFormat("a.zip", QByteArray::fromHex("504b0506")), "zip");
        QCOMPARE(DetectResourceFormat("a.txt", QByteArray::fromHex("610062")), "binary");
        QCOMPARE(DetectResourceFormat("a.txt", QByteArray::fromHex("fffe6100")), "text");
        QCOMPARE(Capabilities()["engineRegistry"].toObject()["schemaVersion"].toInt(), 1);
    }
    void engineProbeInputsAndLimits()
    {
        QTemporaryDir dir; Request request; request.mode_ = "probe"; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right");
        write(request.file1_, "hello\n"); write(request.file2_, "world\n"); auto cancel = std::make_shared<std::atomic_bool>(false);
        auto result = Execute(request, cancel, true); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.operation_, "probe");
        QVERIFY(result.inputs_.isEmpty()); QVERIFY(!result.engineProbe_["outputValidated"].toBool());
        QCOMPARE(result.engineProbe_["inputs"].toArray()[0].toObject()["validation"].toString(), "full-text-decode");
        QVERIFY(result.engineProbe_["mergeTwoAvailable"].toBool()); QCOMPARE(result.commit_, "NotRequested");
        write(request.file2_, QByteArray::fromHex("89504e470d0a1a0a")); result = Execute(request, cancel, false);
        QCOMPARE(result.exitCode_, 2); QCOMPARE(result.completion_, "Complete"); QCOMPARE(result.outcome_, "Unsupported");
        QCOMPARE(result.engineProbe_["inputs"].toArray()[1].toObject()["reason"].toString(), "InvalidPng");
        write(request.file2_, QByteArray::fromHex("ff")); result = Execute(request, cancel, false); QCOMPARE(result.exitCode_, 2);
        write(request.file1_, QByteArray::fromHex("6100")); write(request.file2_, QByteArray::fromHex("6200")); request.encoding_ = "UTF-16LE";
        result = Execute(request, cancel, false); QCOMPARE(result.exitCode_, 0);
        request.encoding_.clear(); write(request.file2_, QByteArray(16 * 1024 * 1024 + 1, 'a')); result = Execute(request, cancel, false);
        QCOMPARE(result.engineProbe_["inputs"].toArray()[1].toObject()["reason"].toString(), "InputSizeLimit");
    }
    void engineProbeDirectoryModesAndConsole()
    {
        QTemporaryDir dir; Request request; request.mode_ = "probe"; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right"); request.file3_ = dir.filePath("third");
        QVERIFY(QDir().mkpath(request.file1_)); QVERIFY(QDir().mkpath(request.file2_)); QVERIFY(QDir().mkpath(request.file3_));
        auto result = Execute(request, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(result.engineProbe_["reason"].toString(), "IndependentThreeUnsupported");
        request.base_ = request.file3_; request.file3_.clear(); result = Execute(request, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(result.exitCode_, 0); QVERIFY(result.engineProbe_["mergeBaseAvailable"].toBool());
        QCOMPARE(result.engineProbe_["inputs"].toArray()[0].toObject()["validation"].toString(), "root-only");
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-mode", "probe", "-file1", request.file1_, "-file2", request.file2_, "-format", "json"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object(); QCOMPARE(report["engineProbe"].toObject()["engine"].toObject()["id"].toString(), "directory.native");
        QVERIFY(!Parse({"hc", "-mode", "probe", "-file1", request.file1_, "-file2", request.file2_}, false).error_.isEmpty());
    }
    void functionRelationsMovementAndModification()
    {
        const QByteArray a = "int a(){return 1;}\n", b = "int b(){return 2;}\n", c = "int c(){return 3;}\n";
        const auto left = AnalyzeFunctions("l.cpp", a + b + c);
        const auto right = AnalyzeFunctions("r.cpp", b + c + "int a(){return 10;}\n");
        const auto match = MatchFunctions(left, right); QCOMPARE(match["status"].toString(), "Complete");
        QCOMPARE(match["moved"].toInt(), 1); QCOMPARE(match["modified"].toInt(), 1);
        QCOMPARE(match["relations"].toArray()[0].toObject()["status"].toString(), "MovedModified");
        const auto shifted = AnalyzeFunctions("r.cpp", "// prefix\n\nint extra(){return 0;}\n" + a + b + c);
        const auto shift = MatchFunctions(left, shifted); QCOMPARE(shift["moved"].toInt(), 0); QCOMPARE(shift["modified"].toInt(), 0);
        QCOMPARE(shift["relations"].toArray().last().toObject()["status"].toString(), "Added");
        const auto removed = MatchFunctions(left, AnalyzeFunctions("r.cpp", a + c));
        QCOMPARE(removed["relations"].toArray()[1].toObject()["status"].toString(), "Removed");
    }
    void functionRelationsOverloadsAmbiguityAndPartial()
    {
        const auto left = AnalyzeFunctions("l.cpp", "namespace N { int f(int x){return x;} double f(double x){return x;} }\nnamespace Other { int f(){return 1;} }");
        const auto right = AnalyzeFunctions("r.cpp", "namespace Other { int f(){return 1;} }\nnamespace N { double f(double x){return x;} int f(int x){return x;} }");
        const auto match = MatchFunctions(left, right); QCOMPARE(match["moved"].toInt(), 1); QCOMPARE(match["ambiguous"].toInt(), 0);
        const auto duplicates = AnalyzeFunctions("a.cpp", "int f(){return 1;} int f(){return 1;}");
        const auto ambiguous = MatchFunctions(duplicates, duplicates); QCOMPARE(ambiguous["status"].toString(), "Partial"); QCOMPARE(ambiguous["ambiguous"].toInt(), 4);
        auto partial = left; partial["status"] = "Partial"; partial["functions"] = QJsonArray{};
        const auto unknown = MatchFunctions(partial, right);
        for (const auto& value : unknown["relations"].toArray()) QCOMPARE(value.toObject()["status"].toString(), "Unmatched");
    }
    void functionRelationsRandomOrderOracle()
    {
        std::mt19937 rng(0x51a7); std::vector<int> order(40); std::iota(order.begin(), order.end(), 0);
        const auto document = [](const std::vector<int>& Values) {
            QJsonArray functions; for (int value : Values) functions.append(QJsonObject{{"name", QString::number(value)}, {"scope", ""}, {"signature", "signature"}, {"body", "body"}});
            return QJsonObject{{"status", "Complete"}, {"functions", functions}};
        };
        const auto left = document(order);
        for (int trial = 0; trial < 100; ++trial)
        {
            std::shuffle(order.begin(), order.end(), rng); std::vector<int> lengths(order.size(), 1); int longest = 1;
            for (std::size_t i = 0; i < order.size(); ++i) for (std::size_t j = 0; j < i; ++j)
                if (order[j] < order[i]) { lengths[i] = std::max(lengths[i], lengths[j] + 1); longest = std::max(longest, lengths[i]); }
            const auto result = MatchFunctions(left, document(order)); QCOMPARE(result["moved"].toInt(), int(order.size()) - longest);
        }
    }
    void sourceAnalysisSessionAndConsole()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("l.cpp"); request.file2_ = dir.filePath("r.cpp"); request.base_ = dir.filePath("base.cpp");
        write(request.file1_, "int a(){return 1;} int b(){return 2;}"); write(request.file2_, "int b(){return 2;} int a(){return 1;}"); write(request.base_, "int a(){return 0;} int b(){return 2;}");
        request.headless_ = true; const auto cancel = std::make_shared<std::atomic_bool>(false); const auto value = Execute(request, cancel, false);
        QCOMPARE(value.completion_, "Complete"); QCOMPARE(value.semantic_["pairs"].toArray().size(), 3); QCOMPARE(value.semantic_["status"].toString(), "Complete");
        QCOMPARE(value.Json()["sourceAnalysis"], QJsonValue(value.semantic_));
        request.semantic_ = "off"; const auto off = Execute(request, cancel, false);
        QCOMPARE(off.semantic_["status"].toString(), "Disabled"); QCOMPARE(off.edits_, value.edits_); QCOMPARE(off.merged_.text_, value.merged_.text_);
        QVERIFY(!Parse({"hc", "-semantic", "invalid", "-file1", request.file1_, "-file2", request.file2_}, true).error_.isEmpty());
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", request.file1_, "-file2", request.file2_, "-format", "json"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 1);
        const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(report["sourceAnalysis"].toObject()["pairs"].toArray()[0].toObject()["moved"].toInt(), 1);
    }
    void functionTemplatesAndResourceLimits()
    {
        const auto a = AnalyzeFunctions("a.cpp", "template<class T=int> T f(T x){return x;}\n");
        const auto b = AnalyzeFunctions("b.cpp", "template<class T=long> T f(T x){return x;}\n");
        QCOMPARE(a["status"].toString(), "Complete"); QCOMPARE(a["functions"].toArray()[0].toObject()["begin"].toInt(), 0);
        const auto relation = MatchFunctions(a, b)["relations"].toArray()[0].toObject();
        QCOMPARE(relation["status"].toString(), "Modified"); QCOMPARE(relation["match"].toString(), "UniqueName");
        QByteArray source; for (int i = 0; i < 10001; ++i) source += "int f" + QByteArray::number(i) + "(){return 0;}\n";
        QElapsedTimer timer; timer.start(); const auto limited = AnalyzeFunctions("large.cpp", source);
        QCOMPARE(limited["status"].toString(), "Unavailable"); QVERIFY((QStringList{"FunctionLimit", "TimeLimit"}.contains(limited["reason"].toString())));
        QVERIFY(timer.elapsed() < 5000); qInfo("Semantic 10001-function limit: %lld ms", timer.elapsed());
    }
    void semanticWorkerScopesAndTokens()
    {
        const QByteArray source = "namespace space {\nint first(int x) { return x; }\nstruct Type { int second() { return 2; } };\n}\nint space::Type::outside() { return 3; }\n";
        const auto result = AnalyzeFunctions("sample.cpp", source);
        QCOMPARE(result["status"].toString(), "Complete");
        const auto functions = result["functions"].toArray(); QCOMPARE(functions.size(), 3);
        QCOMPARE(functions[0].toObject()["name"].toString(), "space::first");
        QCOMPARE(functions[1].toObject()["name"].toString(), "space::Type::second");
        QCOMPARE(functions[2].toObject()["name"].toString(), "space::Type::outside");
        QCOMPARE(functions[0].toObject()["line"].toInt(), 1);
        const auto normal = AnalyzeFunctions("a.c", "int f(int x){ return x + 1; }")["functions"].toArray()[0].toObject();
        const auto comment = AnalyzeFunctions("b.c", "int /* header */ f(int x) { /*body*/ return x+1; }")["functions"].toArray()[0].toObject();
        QCOMPARE(normal["signature"], comment["signature"]); QCOMPARE(normal["body"], comment["body"]);
        const auto changed = AnalyzeFunctions("b.c", "int f(int x) { return x + 2; }")["functions"].toArray()[0].toObject();
        QCOMPARE(normal["signature"], changed["signature"]); QVERIFY(normal["body"] != changed["body"]);
    }
    void semanticWorkerUnicodeErrorsAndLimits()
    {
        const auto unicode = AnalyzeFunctions("a.cpp", QString::fromUtf8("// 한글😀\nint 함수() { return 1; }\n").toUtf8());
        QCOMPARE(unicode["status"].toString(), "Complete");
        const auto function = unicode["functions"].toArray()[0].toObject();
        QCOMPARE(function["name"].toString(), QString::fromUtf8("함수")); QCOMPARE(function["begin"].toInt(), 14);
        QCOMPARE(AnalyzeFunctions("a.txt", "int f(){}")["status"].toString(), "Unsupported");
        const auto broken = AnalyzeFunctions("a.cpp", "int good() {return 1;}\nint bad( {\n");
        QCOMPARE(broken["status"].toString(), "Partial"); QVERIFY(broken["syntaxErrors"].toBool());
        QCOMPARE(broken["functions"].toArray().size(), 1);
        std::atomic_bool cancel{true}; Budget canceled; canceled.canceled_ = &cancel;
        QCOMPARE(AnalyzeFunctions("a.cpp", "", canceled)["reason"].toString(), "Canceled");
        Budget expired; expired.deadline_ = std::chrono::steady_clock::now();
        QCOMPARE(AnalyzeFunctions("a.cpp", "", expired)["reason"].toString(), "TimeLimit");
        QCOMPARE(AnalyzeFunctions("a.cpp", QByteArray(16 * 1024 * 1024 + 1, ' '))["reason"].toString(), "InputLimit");
        const auto anonymous = AnalyzeFunctions("a.cpp", "namespace { int f() {} } struct { int g() {} } value;");
        QCOMPARE(anonymous["functions"].toArray().size(), 2);
        Budget shortBudget; shortBudget.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        QCOMPARE(AnalyzeFunctions("a.cpp", QByteArray(1024 * 1024, ' '), shortBudget)["reason"].toString(), "TimeLimit");
        QProcess malformed; malformed.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-internal-worker"});
        QVERIFY(malformed.waitForStarted()); malformed.write("{invalid}"); malformed.closeWriteChannel(); QVERIFY(malformed.waitForFinished());
        QCOMPARE(malformed.exitCode(), 2); QVERIFY(malformed.readAllStandardOutput().isEmpty());
    }
    void overviewTypesAndBoundaries()
    {
        const auto insertion = AlignmentMap::Build(0, 3, {{0, 0, 0, 3, {}}});
        const auto aligned = BuildOverviewRaster(insertion, 4, true);
        QCOMPARE(aligned.rows_, (std::array<std::size_t, 2>{4, 4}));
        QCOMPARE(aligned.bands_[0], (std::vector<std::uint8_t>{Gap, Gap, Gap, 0}));
        QCOMPARE(aligned.bands_[1], (std::vector<std::uint8_t>{Added, Added, Added, 0}));
        const auto plain = BuildOverviewRaster(insertion, 4, false);
        QCOMPARE(plain.rows_, (std::array<std::size_t, 2>{1, 4}));
        QCOMPARE(plain.bands_[0], (std::vector<std::uint8_t>{Gap, 0, 0, 0}));
        const auto deletion = BuildOverviewRaster(AlignmentMap::Build(3, 0, {{0, 3, 0, 0, {}}}), 4, true);
        QCOMPARE(deletion.bands_[0], (std::vector<std::uint8_t>{Removed, Removed, Removed, 0}));
        QCOMPARE(deletion.bands_[1], aligned.bands_[0]);
        const auto unequal = BuildOverviewRaster(AlignmentMap::Build(2, 3, {{0, 2, 0, 3, {}}}), 4, true);
        QCOMPARE(unequal.bands_[0], (std::vector<std::uint8_t>{Modified, Modified, Gap, 0}));
        QCOMPARE(unequal.bands_[1], (std::vector<std::uint8_t>{Modified, Modified, Modified, 0}));
        const auto mixed = BuildOverviewRaster(AlignmentMap::Build(2, 2, {{0, 1, 0, 0, {}}, {2, 2, 1, 2, {}}}), 1, false);
        QCOMPARE(mixed.bands_[0], (std::vector<std::uint8_t>{Removed | Gap}));
        QCOMPARE(mixed.bands_[1], (std::vector<std::uint8_t>{Added | Gap}));
        const auto empty = BuildOverviewRaster(AlignmentMap::Build(0, 0, {}), 1, true);
        QCOMPARE(empty.rows_[0], std::size_t(1)); QCOMPARE(empty.bands_[0], (std::vector<std::uint8_t>{0}));
        QVERIFY(BuildOverviewRaster(insertion, 0, true).bands_[0].empty());
    }
    void overviewProjectionCoverage()
    {
        std::mt19937 rng(0x0be71e);
        for (int trial = 0; trial < 1000; ++trial)
        {
            std::string a, b;
            for (auto n = rng() % 15; n > 0; --n) a += std::string(1, char('a' + rng() % 5)) + '\n';
            for (auto n = rng() % 15; n > 0; --n) b += std::string(1, char('a' + rng() % 5)) + '\n';
            const auto map = AlignmentMap::Build(SplitLines(a).size(), SplitLines(b).size(), Diff(a, b));
            const std::size_t pixels = 1 + rng() % 30;
            for (bool align : {false, true})
            {
                const auto raster = BuildOverviewRaster(map, pixels, align);
                for (unsigned side = 0; side < 2; ++side)
                {
                    struct Span { std::size_t first, last; std::uint8_t kind; };
                    std::vector<Span> spans;
                    for (const auto& run : map.Runs())
                    {
                        if (!run.changed_) continue;
                        const auto own = run.end_[side] - run.begin_[side], other = run.end_[1 - side] - run.begin_[1 - side];
                        const auto begin = align ? run.rowBegin_ : run.begin_[side];
                        if (own) spans.push_back({begin, begin + own, std::uint8_t(other ? Modified : side ? Added : Removed)});
                        if (own < other) spans.push_back({begin + own, align ? run.rowEnd_ : begin + own, Gap});
                    }
                    // Independent interval-intersection oracle for every bin.
                    for (std::size_t p = 0; p < pixels; ++p)
                    {
                        std::uint8_t expected = 0; const auto rows = raster.rows_[side];
                        for (const auto& span : spans)
                        {
                            const bool overlaps = span.first == span.last ? span.first * pixels / rows == p :
                                span.first * pixels < (p + 1) * rows && p * rows < span.last * pixels;
                            if (overlaps) expected |= span.kind;
                        }
                        QCOMPARE(raster.bands_[side][p], expected);
                    }
                }
            }
        }
    }
    void overviewLargeMapBoundedRaster()
    {
        std::vector<Edit> edits;
        for (std::size_t line = 0; line < 200000; line += 2) edits.push_back({line, line + 1, line, line + 1, {}});
        const auto map = AlignmentMap::Build(200000, 200000, edits);
        QElapsedTimer timer; timer.start(); const auto raster = BuildOverviewRaster(map, 1000000, true);
        QCOMPARE(raster.bands_[0].size(), std::size_t(4096)); QCOMPARE(raster.bands_[1].size(), std::size_t(4096));
        QCOMPARE(raster.rows_[0], std::size_t(200001));
        for (auto flags : raster.bands_[0]) QCOMPARE(flags, std::uint8_t(Modified));
        qInfo("Overview 100000 changes: %lld us, %zu output bins", timer.nsecsElapsed() / 1000, raster.bands_[0].size() + raster.bands_[1].size());
    }
    void inlineRangesUnicodeAndLineEndings()
    {
        const std::string a = "head\r\naXbcYd\r\n한글😀 끝\r\ntail\n", b = "head\r\naQbcRd\r\n한굴😃 끝\r\ntail\n";
        const auto diff = BuildInlineDiff(a, b, Diff(a, b));
        const std::vector<ByteRange> expected{{7, 8}, {10, 11}, {17, 24}};
        QCOMPARE(diff.ranges_[0], expected); QCOMPARE(diff.ranges_[1], expected);
        QCOMPARE(diff.pairedLines_, std::size_t(2)); QCOMPARE(diff.skippedLines_, std::size_t(0));
        const auto insert = BuildInlineDiff("ab", "a한😀b", Diff("ab", "a한😀b"));
        QVERIFY(insert.ranges_[0].empty()); QCOMPARE(insert.ranges_[1], (std::vector<ByteRange>{{1, 8}}));
        const auto remove = BuildInlineDiff("a한😀b", "ab", Diff("a한😀b", "ab"));
        QCOMPARE(remove.ranges_[0], insert.ranges_[1]); QVERIFY(remove.ranges_[1].empty());
        for (const auto& [left, right] : std::vector<std::pair<std::string, std::string>>{
            {"x\r\ny\r", "x\ny\n"}, {"\r\n", "\n"}, {"x", "x\n"}, {"", "new\n"}, {"old", ""}, {"", ""}})
        {
            const auto endings = BuildInlineDiff(left, right, Diff(left, right));
            QVERIFY(endings.ranges_[0].empty()); QVERIFY(endings.ranges_[1].empty());
            QCOMPARE(endings.skippedLines_, std::size_t(0));
        }
        // Only ordinal pairs in a replacement block receive character ranges.
        const auto unequal = BuildInlineDiff("aX\nbY\n", "aQ\nbR\ncS\n", {{0, 2, 0, 3, {}}});
        QCOMPARE(unequal.pairedLines_, std::size_t(2));
        QCOMPARE(unequal.ranges_[1], (std::vector<ByteRange>{{1, 2}, {4, 5}}));
    }
    void inlineRandomizedCommonSubsequence()
    {
        std::mt19937 rng(0x1a11ce);
        const std::vector<std::string> alphabet{"a", "b", "c", "한", "😀", "\xcc\x81"};
        for (int iteration = 0; iteration < 1500; ++iteration)
        {
            std::array<std::string, 2> texts;
            std::array<std::vector<std::string_view>, 2> scalars;
            for (auto& text : texts) for (auto n = rng() % 24; n > 0; --n) text += alphabet[rng() % alphabet.size()];
            for (std::size_t side = 0; side < 2; ++side)
                for (std::size_t i = 0; i < texts[side].size();)
                {
                    const auto begin = i++;
                    while (i < texts[side].size() && (static_cast<unsigned char>(texts[side][i]) & 0xc0) == 0x80) ++i;
                    scalars[side].push_back(std::string_view(texts[side]).substr(begin, i - begin));
                }
            // Explicit one-line pairing includes empty sides, unlike a whole-line insertion.
            const std::array<std::string, 2> lines{texts[0] + '\n', texts[1] + '\n'};
            const auto diff = BuildInlineDiff(lines[0], lines[1], {{0, 1, 0, 1, {}}});
            QCOMPARE(diff.skippedLines_, std::size_t(0));
            std::array<std::string, 2> common;
            std::size_t commonScalars = 0;
            for (std::size_t side = 0; side < 2; ++side)
            {
                std::size_t previous = 0;
                for (const auto& range : diff.ranges_[side])
                {
                    QVERIFY(range.begin_ >= previous && range.begin_ < range.end_ && range.end_ <= texts[side].size());
                    for (auto offset : {range.begin_, range.end_})
                        QVERIFY(offset == texts[side].size() || (static_cast<unsigned char>(texts[side][offset]) & 0xc0) != 0x80);
                    common[side] += texts[side].substr(previous, range.begin_ - previous); previous = range.end_;
                }
                common[side] += texts[side].substr(previous);
            }
            QCOMPARE(common[0], common[1]);
            for (auto byte : common[0]) if ((static_cast<unsigned char>(byte) & 0xc0) != 0x80) ++commonScalars;
            QCOMPARE(commonScalars, lcs(scalars[0], scalars[1]));
        }
    }
    void inlineBudgetsAndCancellation()
    {
        const auto run = [](const std::string& A, const std::string& B, const Budget& Limits = Budget{}) {
            return BuildInlineDiff(A, B, {{0, SplitLines(A).size(), 0, SplitLines(B).size(), {}}}, Limits);
        };
        const auto huge = run(std::string(16385, 'a'), std::string(16385, 'b'));
        QCOMPARE(huge.skippedLines_, std::size_t(1)); QCOMPARE(huge.cells_, std::uint64_t(0));
        const auto dense = run(std::string(1000, 'a'), std::string(1000, 'b'));
        QCOMPARE(dense.skippedLines_, std::size_t(1)); QVERIFY(dense.ranges_[0].empty());
        Budget low; low.steps_ = 0;
        QCOMPARE(run("abc", "xyz", low).skippedLines_, std::size_t(1));
        low = {}; low.traceBytes_ = 1;
        QCOMPARE(run("abc", "xyz", low).skippedLines_, std::size_t(1));
        low = {}; low.deadline_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        QCOMPARE(run("abc", "xyz", low).skippedLines_, std::size_t(1));
        std::atomic_bool cancel{true}; low = {}; low.canceled_ = &cancel;
        QVERIFY_EXCEPTION_THROWN(run("a", "b", low), Failure);
        QVERIFY_EXCEPTION_THROWN(BuildInlineDiff("a", "b", {{0, 2, 0, 1, {}}}), Failure);
        std::string a, b;
        for (int i = 0; i < 5000; ++i) { a += "a\n"; b += "b\n"; }
        auto many = run(a, b); QCOMPARE(many.pairedLines_, std::size_t(5000)); QCOMPARE(many.skippedLines_, std::size_t(904));
        QCOMPARE(many.ranges_[0].size(), std::size_t(4096));
        a.clear(); b.clear();
        for (int i = 0; i < 40; ++i) { a += std::string(400, 'a') + '\n'; b += std::string(400, 'b') + '\n'; }
        QElapsedTimer timer; timer.start(); const auto bounded = run(a, b);
        QVERIFY(bounded.cells_ <= 4'000'000); QVERIFY(bounded.skippedLines_ > 0);
        qInfo("Inline dense corpus: %lld ms, %llu cells, %zu skipped pairs", timer.elapsed(), bounded.cells_, bounded.skippedLines_);
        // A pair is either complete or omitted when the output-range cap is hit.
        a.clear(); b.clear();
        for (int i = 0; i < 900; ++i) { for (int j = 0; j < 20; ++j) { a += "Xa"; b += "Ya"; } a += '\n'; b += '\n'; }
        const auto fragmented = run(a, b);
        QCOMPARE(fragmented.ranges_[0].size() + fragmented.ranges_[1].size(), std::size_t(32760));
        QCOMPARE(fragmented.ranges_[0].size(), fragmented.ranges_[1].size()); QVERIFY(fragmented.skippedLines_ > 0);
    }
    void inlineRuntimePresentationOnly()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt");
        write(r.file1_, "same\naXb\n"); write(r.file2_, "same\naYb\n");
        const auto canceled = std::make_shared<std::atomic_bool>(false);
        const auto gui = Execute(r, canceled, false); QCOMPARE(gui.completion_, QString("Complete"));
        QVERIFY(gui.pairViews_[0]); QCOMPARE(gui.pairViews_[0]->inline_.ranges_[0], (std::vector<ByteRange>{{6, 7}}));
        r.headless_ = true; const auto cli = Execute(r, canceled, false);
        QVERIFY(!cli.pairViews_[0]); QCOMPARE(cli.Json(), gui.Json());
        r.headless_ = false; write(r.file1_, QByteArray(20000, 'a')); write(r.file2_, QByteArray(20000, 'b'));
        const auto limited = Execute(r, canceled, false);
        QCOMPARE(limited.completion_, QString("Complete")); QCOMPARE(limited.exitCode_, 1);
        QCOMPARE(limited.pairViews_[0]->inline_.skippedLines_, std::size_t(1)); QVERIFY(!limited.edits_.empty());
    }
    void alignmentBoundaries()
    {
        const std::string left = "a\r\nkeep\nz", right = "prefix\na\r\ninsert\nkeep\nZ\nend";
        const auto map = AlignmentMap::Build(3, 6, Diff(left, right));
        QCOMPARE(map.MapLine(0, 0), std::size_t(1)); QCOMPARE(map.MapLine(0, 1), std::size_t(3));
        QCOMPARE(map.MapLine(0, 2), std::size_t(4)); QCOMPARE(map.MapLine(0, 3), std::size_t(6));
        QCOMPARE(map.MapLine(1, 0), std::size_t(0)); QCOMPARE(map.MapLine(1, 2), std::size_t(1));
        QCOMPARE(map.MapLine(1, 5), std::size_t(2)); QCOMPARE(map.MapLine(1, 6), std::size_t(3));
        const auto empty = AlignmentMap::Build(0, 0, {}); QVERIFY(empty.Runs().empty()); QCOMPARE(empty.MapLine(0, 0), std::size_t(0));
        const auto inserted = AlignmentMap::Build(0, 3, Diff("", left));
        QCOMPARE(inserted.MapLine(0, 0), std::size_t(3)); QCOMPARE(inserted.MapLine(1, 1), std::size_t(0));
        const auto deleted = AlignmentMap::Build(3, 0, Diff(left, ""));
        QCOMPARE(deleted.MapLine(0, 2), std::size_t(0)); QCOMPARE(deleted.MapLine(1, 0), std::size_t(3));
        const auto eof = AlignmentMap::Build(1, 2, Diff("keep\n", "keep\nextra\n"));
        QCOMPARE(eof.MapLine(0, 0), std::size_t(0)); QCOMPARE(eof.MapLine(0, 1), std::size_t(2));
        QVERIFY_EXCEPTION_THROWN(map.MapLine(2, 0), Failure);
    }
    void alignmentDisplayRows()
    {
        const auto map = AlignmentMap::Build(4, 7, Diff("a\nb\nc\nz\n", "prefix\na\nB\nC\nD\nz\ntail"));
        QCOMPARE(map.RowCount(), std::size_t(7));
        const std::array<std::size_t, 5> rows{1, 2, 3, 5, 7};
        for (std::size_t line = 0; line < rows.size(); ++line) QCOMPARE(map.DisplayRow(0, line), rows[line]);
        for (const auto row : {0u, 4u, 6u, 7u}) QVERIFY(!map.LineAtRow(0, row));
        QCOMPARE(*map.LineAtRow(0, 3), std::size_t(2));
        QVERIFY_EXCEPTION_THROWN(map.DisplayRow(0, 5), Failure);
        QVERIFY_EXCEPTION_THROWN(map.DisplayRow(2, 0), Failure);
        QVERIFY_EXCEPTION_THROWN(map.LineAtRow(2, 0), Failure);
        const auto empty = AlignmentMap::Build(0, 0, {});
        QCOMPARE(empty.RowCount(), std::size_t(0)); QCOMPARE(empty.DisplayRow(1, 0), std::size_t(0)); QVERIFY(!empty.LineAtRow(0, 0));
        const auto large = AlignmentMap::Build(1, 300000, {{1, 1, 1, 300000, {}}});
        QCOMPARE(large.Runs().size(), std::size_t(2)); QCOMPARE(large.RowCount(), std::size_t(300000));
        QVERIFY(!large.LineAtRow(0, 290000)); QCOMPARE(*large.LineAtRow(1, 290000), std::size_t(290000));
    }
    void alignmentRandomCorrespondence()
    {
        std::mt19937 random(10959);
        for (int trial = 0; trial < 4000; ++trial)
        {
            std::string left, right;
            for (auto* text : {&left, &right})
            {
                const auto count = random() % 24;
                for (unsigned i = 0; i < count; ++i) { *text += char('a' + random() % 8); *text += random() % 3 ? "\n" : "\r\n"; }
                if (!text->empty() && random() % 2) text->pop_back();
            }
            const auto a = SplitLines(left), b = SplitLines(right); const auto edits = Diff(left, right);
            const auto map = AlignmentMap::Build(a.size(), b.size(), edits);
            QVERIFY(map.Runs().size() <= edits.size() * 2 + 1);
            for (unsigned side = 0; side < 2; ++side)
            {
                std::size_t previous = 0;
                for (std::size_t line = 0; line <= map.LineCount(side); ++line)
                {
                    const auto target = map.MapLine(side, line); QVERIFY(target >= previous); QVERIFY(target <= map.LineCount(1 - side)); previous = target;
                    if (line < map.LineCount(side)) QCOMPARE(*map.LineAtRow(side, map.DisplayRow(side, line)), line);
                }
                std::size_t nextLine = 0;
                for (std::size_t row = 0; row < map.RowCount(); ++row)
                    if (const auto line = map.LineAtRow(side, row)) QCOMPARE(*line, nextLine++);
                QCOMPARE(nextLine, map.LineCount(side));
            }
            std::array<std::size_t, 2> end{};
            for (const auto& run : map.Runs())
            {
                QCOMPARE(run.begin_, end); end = run.end_;
                if (run.changed_) continue;
                for (auto line = run.begin_[0]; line < run.end_[0]; ++line)
                {
                    const auto other = map.MapLine(0, line); QCOMPARE(a[line], b[other]); QCOMPARE(map.MapLine(1, other), line);
                    QCOMPARE(map.DisplayRow(0, line), map.DisplayRow(1, other));
                }
            }
            QCOMPARE(end[0], a.size()); QCOMPARE(end[1], b.size());
        }
    }
    void alignmentLimitsAndCompactRuns()
    {
        const auto map = AlignmentMap::Build(290000, 300000, {{150000, 150000, 150000, 160000, {}}});
        QCOMPARE(map.Runs().size(), std::size_t(3));
        for (std::size_t line = 0; line < 290000; ++line) QCOMPARE(map.MapLine(0, line), line < 150000 ? line : line + 10000);
        const std::vector<std::vector<Edit>> invalid{
            {{1, 0, 1, 1, {}}}, {{0, 1, 0, 9, {}}}, {{1, 1, 1, 1, {}}}, {{1, 2, 0, 1, {}}},
            {{0, 1, 0, 1, {}}, {0, 1, 0, 1, {}}}, {{0, 1, 0, 2, {}}}};
        for (const auto& edits : invalid) QVERIFY_EXCEPTION_THROWN(AlignmentMap::Build(4, 4, edits), Failure);
        QVERIFY_EXCEPTION_THROWN(AlignmentMap::Build(2'000'001, 0, {}), Failure);
        std::atomic_bool stop{true}; Budget limits; limits.canceled_ = &stop;
        QVERIFY_EXCEPTION_THROWN(AlignmentMap::Build(1, 1, {}, limits), Failure);
        limits.canceled_ = nullptr; limits.deadline_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        QVERIFY_EXCEPTION_THROWN(AlignmentMap::Build(1, 1, {}, limits), Failure);
    }
    void mergeWorkRoundTrip()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "head\na\r\nkeep\r\nz"); write(r.file2_, "head\nA\r\nkeep\r\nZ");
        const auto session = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        MergeModel model(session.plan_); MergeWork work{session.inputs_[0].utf8_, model.State(), false};
        const auto edit = model.Prepare(1, model.Revision(), Resolution::Right); model.Apply(edit);
        work.text_.replace(qsizetype(edit.begin_), qsizetype(edit.end_ - edit.begin_), QByteArray::fromStdString(edit.replacement_));
        const auto prefix = QString::fromUtf8("수동😀\r\n").toUtf8(); model.TextChanged(0, 0, prefix.size()); work.text_.prepend(prefix); work.state_ = model.State();
        const auto json = EncodeMergeWork(r, session, work, {}); auto restored = DecodeMergeWork(r, session, json, {});
        QCOMPARE(restored.text_, work.text_); QCOMPARE(restored.state_, work.state_); QVERIFY(!restored.reviewRequired_);
        // An unmapped block has no usable offset, even after later text deletion.
        work.state_[1] = {9999, 99999, Resolution::Manual, false}; work.reviewRequired_ = true;
        restored = DecodeMergeWork(r, session, EncodeMergeWork(r, session, work, {}), {});
        QCOMPARE(restored.state_[1].begin_, std::size_t(0)); QVERIFY(!restored.state_[1].mapped_); QVERIFY(restored.reviewRequired_);
        // Binary chunk boundaries may split a UTF-8 sequence; the joined RESULT is validated.
        work.text_ = QByteArray(512 * 1024 - 1, 'x') + prefix + QByteArray(1024 * 1024, 'y');
        for (auto& state : work.state_) state = {0, 0, Resolution::Manual, false};
        restored = DecodeMergeWork(r, session, EncodeMergeWork(r, session, work, {}), {}); QCOMPARE(restored.text_, work.text_);
        r.base_ = dir.filePath("base"); write(r.base_, "head\na\r\nkeep\r\nz");
        auto three = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        QVERIFY(!three.plan_->blocks_.empty()); MergeWork automatic{three.output_, three.plan_->initial_, false};
        QCOMPARE(DecodeMergeWork(r, three, EncodeMergeWork(r, three, automatic, {}), {}).text_, three.output_);
        write(r.base_, "head\nbase\r\nkeep\r\nbase"); three = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        QVERIFY(!three.plan_->blocks_.empty()); MergeWork conflicts{three.output_, three.plan_->initial_, false};
        QCOMPARE(DecodeMergeWork(r, three, EncodeMergeWork(r, three, conflicts, {}), {}).state_, conflicts.state_);
    }
    void legacyMergeWorkPreservesTextForReview()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "a\nkeep\nz\n"); write(r.file2_, "A\nkeep\nZ\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto session = Execute(r, cancel, false);
        MergeWork work{QByteArray("manually edited result\n"), std::vector<BlockState>(session.plan_->blocks_.size(), {0, 0, Resolution::Manual, false}), false};
        for (const auto& engine : {"myers-bounded-1", "myers-bounded-2"})
        {
            auto json = QJsonDocument::fromJson(EncodeMergeWork(r, session, work, {})).object(); auto identity = json["identity"].toObject();
            identity.remove("generation"); identity["engineVersion"] = engine;
            identity["generation"] = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            json["identity"] = identity; json["blocks"] = QJsonArray{}; // Legacy block count may differ.
            const auto restored = DecodeMergeWork(r, session, QJsonDocument(json).toJson(), {});
            QCOMPARE(restored.text_, work.text_); QVERIFY(restored.reviewRequired_); QCOMPARE(restored.state_.size(), session.plan_->blocks_.size());
            for (const auto& state : restored.state_) { QVERIFY(!state.mapped_); QCOMPARE(state.resolution_, Resolution::Unresolved); }
            auto bad = json; auto badIdentity = identity; badIdentity["generation"] = "wrong"; bad["identity"] = badIdentity;
            QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, session, QJsonDocument(bad).toJson(), {}), Failure);
            bad = json; bad["resultSha256"] = "wrong"; QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, session, QJsonDocument(bad).toJson(), {}), Failure);
            write(r.file2_, "changed\n"); const auto changed = Execute(r, cancel, false);
            QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, changed, QJsonDocument(json).toJson(), {}), Failure); write(r.file2_, "A\nkeep\nZ\n");
        }
    }
    void mergeWorkStrictValidation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, QString::fromUtf8("가\r\nkeep\r\nz").toUtf8()); write(r.file2_, "A\r\nkeep\r\nZ");
        const auto session = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        MergeWork work{session.inputs_[0].utf8_, session.plan_->initial_, false};
        const auto json = EncodeMergeWork(r, session, work, {}); const auto original = QJsonDocument::fromJson(json).object();
        QVector<QByteArray> invalid; auto duplicate = json; duplicate.insert(1, "\"reviewRequired\":false,"); invalid.push_back(duplicate);
        auto legacy = original; auto identity = legacy["identity"].toObject(); identity["engineVersion"] = "myers-bounded-2"; legacy["identity"] = identity; invalid.push_back(QJsonDocument(legacy).toJson());
        for (const auto& key : {QString("schemaVersion"), QString("kind"), QString("identity"), QString("takeAll"), QString("resultBytes"),
            QString("resultSha256"), QString("resultChunks"), QString("blocks"), QString("reviewRequired"), QString("unknown")})
        { auto changed = original; changed[key] = "invalid"; invalid.push_back(QJsonDocument(changed).toJson()); }
        for (const auto& pair : {std::pair{QString("begin"), QJsonValue(1)}, {QString("end"), QJsonValue(9999)},
            {QString("begin"), QJsonValue(0.5)}, {QString("id"), QJsonValue(99)}, {QString("resolution"), QJsonValue(2)},
            {QString("resolution"), QJsonValue(3)}, {QString("resolution"), QJsonValue(7)}, {QString("mapped"), QJsonValue("true")}})
        {
            auto changed = original; auto blocks = changed["blocks"].toArray(); auto block = blocks[0].toObject(); block[pair.first] = pair.second;
            blocks[0] = block; changed["blocks"] = blocks; invalid.push_back(QJsonDocument(changed).toJson());
        }
        auto changed = original; auto chunks = changed["resultChunks"].toArray(); chunks[0] = chunks[0].toString() + "!";
        changed["resultChunks"] = chunks; invalid.push_back(QJsonDocument(changed).toJson());
        for (const auto& bytes : invalid) QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, session, bytes, {}), Failure);
        r.takeAll_ = "right"; QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, session, json, {}), Failure); r.takeAll_.clear();
        std::atomic_bool canceled{true}; Budget limits; limits.canceled_ = &canceled;
        QVERIFY_EXCEPTION_THROWN(DecodeMergeWork(r, session, json, limits), Failure);
        work.text_ = QByteArray(1, char(0xff)); QVERIFY_EXCEPTION_THROWN(EncodeMergeWork(r, session, work, {}), Failure);
        work.text_ = QByteArray("a\0b", 3); QVERIFY_EXCEPTION_THROWN(EncodeMergeWork(r, session, work, {}), Failure);
        work.text_ = QByteArray(32 * 1024 * 1024 + 1, 'a'); QVERIFY_EXCEPTION_THROWN(EncodeMergeWork(r, session, work, {}), Failure);
    }
    void mergeWorkStorageGuards()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto session = Execute(r, cancel, false); const MergeWork work{session.inputs_[0].utf8_, session.plan_->initial_, false};
        const auto path = dir.filePath("작업.hcmerge"); auto saved = SaveMergeWorkFile(path, r, session, work, {}, false, {});
        QVERIFY(saved.expected_.exists_); QVERIFY(saved.warnings_.isEmpty());
        auto loaded = LoadMergeWorkFile(path, r, session, {}); QCOMPARE(loaded.work_.text_, work.text_); QVERIFY(!QFile::exists(r.output_));
        QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(path, r, session, work, {}, false, {}), Failure);
        saved = SaveMergeWorkFile(path, r, session, work, saved.expected_, false, {}); QVERIFY(saved.expected_.exists_);
        const auto original = read(path); write(path, "external change");
        QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(path, r, session, work, saved.expected_, true, {}), Failure); QCOMPARE(read(path), QByteArray("external change"));
        for (const auto& alias : {r.file1_, r.file2_, r.output_})
            QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(alias, r, session, work, {}, true, {}), Failure);
        const auto hardlink = dir.filePath("alias.hcmerge");
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(hardlink.utf16()), reinterpret_cast<LPCWSTR>(r.file1_.utf16()), nullptr));
        QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(hardlink, r, session, work, {}, true, {}), Failure); QCOMPARE(read(r.file1_), QByteArray("left\n"));
        write(path, original); write(r.file2_, "changed right\n");
        QVERIFY_EXCEPTION_THROWN(LoadMergeWorkFile(path, r, session, {}), Failure);
        const auto fresh = Execute(r, cancel, false); QVERIFY_EXCEPTION_THROWN(LoadMergeWorkFile(path, r, fresh, {}), Failure);
        // Preserve old unsaved work even when original files have changed.
        saved = SaveMergeWorkFile(dir.filePath("rescue.hcmerge"), r, session, work, {}, false, {}); QVERIFY(saved.expected_.exists_);
        std::atomic_bool stop{true}; Budget limits; limits.canceled_ = &stop;
        QVERIFY_EXCEPTION_THROWN(SaveMergeWorkFile(dir.filePath("canceled.hcmerge"), r, session, work, {}, false, limits), Failure);
        QVERIFY(!QFile::exists(dir.filePath("canceled.hcmerge")));
    }
    void decisionWorkConversionAndGuards()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "head\na\nkeep\nz\n"); write(r.file2_, "head\nA\nkeep\nZ\n");
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        for (bool three : {false, true})
        {
            if (three) { r.base_ = dir.filePath("base"); write(r.base_, "head\nb\nkeep\nx\n"); }
            const auto session = Execute(r, cancel, false); QVERIFY(session.plan_); QCOMPARE(session.plan_->blocks_.size(), std::size_t(2));
            for (const auto resolution : {Resolution::Unresolved, Resolution::Left, Resolution::Right, Resolution::Base,
                Resolution::LeftThenRight, Resolution::RightThenLeft, Resolution::Manual, Resolution::Omit})
            {
                if (!three && resolution == Resolution::Base) continue;
                MergeModel model(session.plan_); MergeWork work{three ? session.output_ : session.inputs_[0].utf8_, model.State(), false};
                if (resolution != Resolution::Unresolved)
                {
                    const auto edit = model.Prepare(1, model.Revision(), resolution); model.Apply(edit);
                    if (resolution != Resolution::Manual) work.text_.replace(qsizetype(edit.begin_), qsizetype(edit.end_ - edit.begin_), QByteArray::fromStdString(edit.replacement_));
                    work.state_ = model.State();
                    if (resolution == Resolution::Manual)
                    {
                        const auto manual = QString::fromUtf8("수동😀\r\n").toUtf8(); const auto old = work.state_[0];
                        work.text_.replace(qsizetype(old.begin_), qsizetype(old.end_ - old.begin_), manual);
                        const auto delta = manual.size() - qsizetype(old.end_ - old.begin_); work.state_[0].end_ = old.begin_ + manual.size();
                        work.state_[1].begin_ += delta; work.state_[1].end_ += delta;
                    }
                }
                const auto bytes = EncodeDecisionWork(r, session, work, {}); const auto decoded = DecodeDecisionWork(r, session, bytes, {});
                QCOMPARE(decoded.text_, work.text_); QCOMPARE(decoded.state_, work.state_);
                auto cli = session; ApplyDecisionPlan(r, cli, bytes, {}); QCOMPARE(cli.output_, work.text_);
            }
            MergeWork work{three ? session.output_ : session.inputs_[0].utf8_, session.plan_->initial_, false};
            work.reviewRequired_ = true; QVERIFY_EXCEPTION_THROWN(EncodeDecisionWork(r, session, work, {}), Failure); work.reviewRequired_ = false;
            work.text_.prepend("outside\n"); for (auto& state : work.state_) { state.begin_ += 8; state.end_ += 8; }
            QVERIFY_EXCEPTION_THROWN(EncodeDecisionWork(r, session, work, {}), Failure);
            work.state_[0] = {0, 0, Resolution::Manual, false}; QVERIFY_EXCEPTION_THROWN(EncodeDecisionWork(r, session, work, {}), Failure);
        }
    }
    void decisionWorkStorageAndCli()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto session = Execute(r, cancel, false); MergeModel model(session.plan_); model.Apply(model.Prepare(1, model.Revision(), Resolution::Right));
        const MergeWork work{QByteArray("right\n"), model.State(), false}; const auto path = dir.filePath("decisions.json");
        SaveDecisionWorkFile(path, r, session, work, false, {}); QCOMPARE(LoadDecisionWorkFile(path, r, session, {}).work_.text_, work.text_);
        QVERIFY_EXCEPTION_THROWN(SaveDecisionWorkFile(path, r, session, work, false, {}), Failure);
        SaveDecisionWorkFile(path, r, session, work, true, {}); const auto original = read(path);
        for (const auto& alias : {r.file1_, r.file2_, r.output_})
            QVERIFY_EXCEPTION_THROWN(SaveDecisionWorkFile(alias, r, session, work, true, {}), Failure);
        const auto hardlink = dir.filePath("alias.json");
        QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(hardlink.utf16()), reinterpret_cast<LPCWSTR>(r.file1_.utf16()), nullptr));
        QVERIFY_EXCEPTION_THROWN(SaveDecisionWorkFile(hardlink, r, session, work, true, {}), Failure);
        auto cli = r; cli.headless_ = true; cli.decisions_ = path; const auto merged = Execute(cli, cancel, true);
        QCOMPARE(merged.exitCode_, 0); QCOMPARE(read(r.output_), work.text_);
        auto invalid = work; invalid.text_.prepend("outside"); invalid.state_[0].begin_ += 7; invalid.state_[0].end_ += 7;
        QVERIFY_EXCEPTION_THROWN(SaveDecisionWorkFile(path, r, session, invalid, true, {}), Failure); QCOMPARE(read(path), original);
        std::atomic_bool stop{true}; Budget limits; limits.canceled_ = &stop;
        QVERIFY_EXCEPTION_THROWN(LoadDecisionWorkFile(path, r, session, limits), Failure);
        QVERIFY_EXCEPTION_THROWN(SaveDecisionWorkFile(dir.filePath("canceled.json"), r, session, work, false, limits), Failure);
        QVERIFY(!QFile::exists(dir.filePath("canceled.json")));
        write(r.file2_, "changed\n"); QVERIFY_EXCEPTION_THROWN(LoadDecisionWorkFile(path, r, session, {}), Failure);
        const auto fresh = Execute(r, cancel, false); QVERIFY_EXCEPTION_THROWN(LoadDecisionWorkFile(path, r, fresh, {}), Failure);
    }
    void decisionPlanRoundTrip()
    {
        QTemporaryDir dir; Request r; r.headless_ = true; r.mode_ = "merge"; r.file1_ = dir.filePath("왼쪽.cpp");
        r.file2_ = dir.filePath("오른쪽.cpp"); r.output_ = dir.filePath("결과.cpp"); r.planOut_ = dir.filePath("선택.json");
        const auto left = QString::fromUtf8("가\r\n유지\r\n끝").toUtf8(), right = QString::fromUtf8("나\r\n추가\r\n유지\r\n마침").toUtf8();
        write(r.file1_, QByteArray::fromHex("efbbbf") + left); write(r.file2_, right);
        const auto cancel = std::make_shared<std::atomic_bool>(false); r.dryRun_ = true;
        auto result = Execute(r, cancel, true); QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.exitCode_, 1);
        QVERIFY(!QFile::exists(r.output_)); QCOMPARE(result.planWritten_, r.planOut_);
        auto plan = QJsonDocument::fromJson(read(r.planOut_)).object(); auto blocks = plan["blocks"].toArray(); QCOMPARE(blocks.size(), 2);
        auto first = blocks[0].toObject(); first["choice"] = "right"; blocks[0] = first;
        auto second = blocks[1].toObject(); second["choice"] = "manual"; second["replacement"] = QString::fromUtf8("수동😀"); blocks[1] = second;
        plan["blocks"] = blocks; r.decisions_ = r.planOut_; r.planOut_.clear(); write(r.decisions_, QJsonDocument(plan).toJson());
        const auto expected = QString::fromUtf8("나\r\n추가\r\n유지\r\n수동😀").toUtf8();
        result = Execute(r, cancel, true); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.output_, expected); QVERIFY(!QFile::exists(r.output_));
        r.dryRun_ = false; result = Execute(r, cancel, true); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.commit_, QString("Committed"));
        QCOMPARE(read(r.output_), QByteArray::fromHex("efbbbf") + expected); QCOMPARE(result.Json()["unresolvedCount"].toInt(), 0);
        r.overwrite_ = true; write(r.file1_, "changed input");
        result = Execute(r, cancel, true); QCOMPARE(result.error_, QString("StalePlan"));
        QCOMPARE(read(r.output_), QByteArray::fromHex("efbbbf") + expected);
    }
    void decisionPlanThreeWayChoices()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left");
        r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        write(r.base_, "a\nb\nc\n"); write(r.file1_, "L\nb\nlc\n"); write(r.file2_, "R\nrb\nc\n");
        const auto initial = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(initial.completion_, QString("Complete")); QCOMPARE(initial.plan_->blocks_.size(), std::size_t(2));
        const auto templatePlan = MakeDecisionPlan(r, initial, {});
        const std::vector<std::pair<QString, QByteArray>> cases{{"left", "L\nb\nlc\n"}, {"right", "R\nrb\nlc\n"},
            {"base", "a\nb\nlc\n"}, {"leftThenRight", "L\nb\nR\nrb\nlc\n"}, {"rightThenLeft", "R\nrb\nL\nb\nlc\n"}, {"manual", "lc\n"}, {"omit", "lc\n"}};
        for (const auto& [name, expected] : cases)
        {
            auto plan = templatePlan; auto blocks = plan["blocks"].toArray(); auto block = blocks[0].toObject(); block["choice"] = name;
            blocks[0] = block; plan["blocks"] = blocks; auto result = initial;
            ApplyDecisionPlan(r, result, QJsonDocument(plan).toJson(), {});
            QCOMPARE(result.exitCode_, 0); QCOMPARE(result.output_, expected);
            const auto report = result.Json(); QCOMPARE(report["resolutions"].toArray()[0].toObject()["choice"].toString(), name);
            QCOMPARE(report["engineVersion"].toString(), QString(textEngineVersion)); QVERIFY(report["extension"].toObject().isEmpty());
        }
        auto partial = initial; ApplyDecisionPlan(r, partial, QJsonDocument(templatePlan).toJson(), {});
        QCOMPARE(partial.exitCode_, 1); QVERIFY(!QFile::exists(r.output_));
    }
    void decisionPlanStrictValidation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "a\nkeep\nz\n"); write(r.file2_, "A\nkeep\nZ\n");
        const auto initial = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        const auto plan = MakeDecisionPlan(r, initial, {}); const auto json = QJsonDocument(plan).toJson(QJsonDocument::Compact);
        QVector<QByteArray> invalid;
        auto duplicate = json; duplicate.insert(1, "\"schemaVersion\":1,"); invalid.push_back(duplicate);
        duplicate = json; duplicate.insert(1, "\"schem\\u0061Version\":1,"); invalid.push_back(duplicate);
        auto mutated = plan; mutated["schemaVersion"] = 2; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["engineVersion"] = "myers-bounded-1"; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["engineVersion"] = "myers-bounded-2"; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["policyDigest"] = "wrong"; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["generation"] = "old"; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["unknown"] = true; invalid.push_back(QJsonDocument(mutated).toJson());
        mutated = plan; mutated["blocks"] = QJsonArray{}; invalid.push_back(QJsonDocument(mutated).toJson());
        for (const auto& field : {QString("choice"), QString("expectedSpanDigest"), QString("changeId"), QString("unknown")})
        {
            mutated = plan; auto blocks = mutated["blocks"].toArray(); auto block = blocks[1].toObject(); block[field] = "invalid";
            blocks[1] = block; mutated["blocks"] = blocks; invalid.push_back(QJsonDocument(mutated).toJson());
        }
        for (const auto& value : {QByteArray("\\ud800"), QByteArray("\\udc00"), QByteArray("\\u0000"), QByteArray(1, char(0xff))})
        {
            auto bytes = json; bytes.replace("\"choice\":\"unresolved\"", "\"choice\":\"manual\"");
            bytes.replace("\"replacement\":\"\"", "\"replacement\":\"" + value + '"'); invalid.push_back(bytes);
        }
        mutated = plan; auto blocks = mutated["blocks"].toArray(); auto block = blocks[0].toObject(); block["choice"] = "base";
        blocks[0] = block; mutated["blocks"] = blocks; invalid.push_back(QJsonDocument(mutated).toJson());
        block["choice"] = "manual"; block["replacement"] = QString(1048577, 'x'); blocks[0] = block; mutated["blocks"] = blocks;
        invalid.push_back(QJsonDocument(mutated).toJson());
        invalid.push_back("{\"x\":" + QByteArray(12, '[') + '0' + QByteArray(12, ']') + '}');
        for (const auto& bytes : invalid)
        {
            auto result = initial; QVERIFY_EXCEPTION_THROWN(ApplyDecisionPlan(r, result, bytes, {}), Failure);
            QCOMPARE(result.output_, initial.output_); QCOMPARE(result.plan_, initial.plan_);
        }
        auto result = initial; Budget canceled; std::atomic_bool stop{true}; canceled.canceled_ = &stop;
        QVERIFY_EXCEPTION_THROWN(ApplyDecisionPlan(r, result, json, canceled), Failure);
        QVERIFY_EXCEPTION_THROWN(ApplyDecisionPlan(r, result, QByteArray(decisionPlanBytes + 1, ' '), {}), Failure);
    }
    void decisionPlanManyBlocks()
    {
        Request r; r.mode_ = "merge"; Result initial; std::string left, right; std::vector<Edit> edits;
        for (std::size_t i = 0; i < 5000; ++i)
        {
            left += "a\nkeep\n"; right += "B\nkeep\n";
            edits.push_back({i * 2, i * 2 + 1, i * 2, i * 2 + 1, "B\n"});
        }
        Snapshot l, rr; l.path_ = "left"; rr.path_ = "right"; l.encoding_ = rr.encoding_ = "UTF-8";
        l.raw_ = l.utf8_ = QByteArray::fromStdString(left); rr.raw_ = rr.utf8_ = QByteArray::fromStdString(right); initial.inputs_ = {l, rr};
        initial.plan_ = std::make_shared<MergePlan>(TwoWayPlan(left, right, edits));
        auto plan = MakeDecisionPlan(r, initial, {}); auto blocks = plan["blocks"].toArray();
        for (qsizetype i = 0; i < blocks.size(); ++i) { auto block = blocks[i].toObject(); block["choice"] = "right"; blocks[i] = block; }
        plan["blocks"] = blocks; const auto json = QJsonDocument(plan).toJson(QJsonDocument::Compact);
        QElapsedTimer timer; timer.start(); ApplyDecisionPlan(r, initial, json, {});
        qInfo("5000 decisions: %lld ms, JSON %lld bytes", qlonglong(timer.elapsed()), qlonglong(json.size()));
        QCOMPARE(initial.output_, QByteArray::fromStdString(right)); QCOMPARE(initial.exitCode_, 0);
        for (const auto& state : initial.plan_->initial_) QCOMPARE(state.resolution_, Resolution::Right);
    }
    void decisionPlanArtifactProtection()
    {
        QTemporaryDir dir; Request r; r.headless_ = true; r.mode_ = "merge"; r.file1_ = dir.filePath("left");
        r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json"); r.dryRun_ = true;
        write(r.file1_, "a\n"); write(r.file2_, "b\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        QCOMPARE(Execute(r, cancel, true).exitCode_, 1); const auto original = read(r.planOut_);
        r.overwrite_ = true; QCOMPARE(Execute(r, cancel, true).error_, QString("PlanAlreadyExists")); QCOMPARE(read(r.planOut_), original);
        const auto alias = dir.filePath("input-link"); QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(r.file1_.utf16()), nullptr));
        const auto planPath = r.planOut_; r.planOut_ = alias;
        QCOMPARE(Execute(r, cancel, true).error_, QString("ArtifactPathAlias")); QCOMPARE(read(r.file1_), QByteArray("a\n"));
        r.planOut_.clear(); r.decisions_ = planPath; r.dryRun_ = false;
        // Unresolved decisions must not replace an existing RESULT.
        write(r.output_, "keep output"); QCOMPARE(Execute(r, cancel, true).exitCode_, 1); QCOMPARE(read(r.output_), QByteArray("keep output"));
        auto plan = QJsonDocument::fromJson(original).object(); auto blocks = plan["blocks"].toArray(); auto block = blocks[0].toObject();
        block["choice"] = "right"; blocks[0] = block; plan["blocks"] = blocks; write(planPath, QJsonDocument(plan).toJson());
        auto result = Execute(r, cancel, false); QCOMPARE(result.exitCode_, 0);
        write(planPath, "changed decision file"); QVERIFY_EXCEPTION_THROWN(SaveResult(r, result, result.output_), Failure);
        QCOMPARE(read(r.output_), QByteArray("keep output"));
        const auto report = dir.filePath("report-link"); QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(report.utf16()), reinterpret_cast<LPCWSTR>(planPath.utf16()), nullptr));
        r.report_ = report; result = Execute(r, cancel, true); QCOMPARE(result.error_, QString("ArtifactPathAlias"));
        QCOMPARE(WriteReport(r, result), 2); QCOMPARE(read(planPath), QByteArray("changed decision file"));
    }
    void decisionPlanProcesses()
    {
        QTemporaryDir dir; const auto a = dir.filePath("왼쪽 a"), b = dir.filePath("오른쪽 b"), out = dir.filePath("결과"), plan = dir.filePath("결정.json");
        write(a, "left\n"); write(b, "right\n");
        const QStringList args{"-mode", "merge", "-file1", a, "-file2", b, "-output", out, "-format", "json"};
        QProcess process; const auto cli = QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe";
        process.start(cli, args + QStringList{"-planout", plan, "-dryrun"}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 1);
        QCOMPARE(QJsonDocument::fromJson(process.readAllStandardOutput()).object()["planWritten"].toString(), plan); QVERIFY(!QFile::exists(out));
        auto doc = QJsonDocument::fromJson(read(plan)).object(); auto blocks = doc["blocks"].toArray(); auto block = blocks[0].toObject();
        block["choice"] = "right"; blocks[0] = block; doc["blocks"] = blocks; write(plan, QJsonDocument(doc).toJson());
        process.start(cli, args + QStringList{"-decisions", plan}); QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0);
        QCOMPARE(read(out), QByteArray("right\n")); QCOMPARE(QJsonDocument::fromJson(process.readAllStandardOutput()).object()["unresolvedCount"].toInt(), 0);
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompare.exe", args + QStringList{"-headless", "-decisions", plan, "-overwrite"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 0); QCOMPARE(read(out), QByteArray("right\n"));
        QVERIFY(process.readAllStandardError().isEmpty());
    }
    void recompareDecisionsAndDestination()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp");
        const QByteArray middle = QByteArray("unchanged line\n").repeated(20);
        const auto left = "a\n" + middle + "z\n", right = "A\n" + middle + "Z\n";
        write(r.file1_, left); write(r.file2_, right); const auto cancel = std::make_shared<std::atomic_bool>(false);
        auto previous = Execute(r, cancel, false); QCOMPARE(previous.completion_, QString("Complete"));
        MergeModel model(previous.plan_); model.SelectAll(Resolution::Right);
        SaveResult(r, previous, right); QVERIFY(previous.expected_.exists_);
        const auto same = Recompare(r, previous, model.State(), std::size_t(right.size()), cancel);
        QCOMPARE(same.completion_, QString("Complete")); QVERIFY(!same.inputsChanged_); QCOMPARE(same.plan_, previous.plan_);
        write(r.file1_, "a\n" + middle + "new tail\n");
        auto changed = Recompare(r, previous, model.State(), std::size_t(right.size()), cancel);
        QCOMPARE(changed.completion_, QString("Complete")); QVERIFY(changed.inputsChanged_);
        QCOMPARE(changed.plan_->initial_.size(), std::size_t(2));
        QCOMPARE(changed.plan_->initial_[0], model.State()[0]); QVERIFY(!changed.plan_->initial_[1].mapped_);
        QCOMPARE(changed.expected_.revision_, previous.expected_.revision_);
        // An external output must never become an implicitly approved overwrite on refresh.
        write(r.output_, "external output\n");
        changed = Recompare(r, previous, model.State(), std::size_t(right.size()), cancel);
        QCOMPARE(changed.completion_, QString("Complete")); QCOMPARE(changed.expected_.revision_, previous.expected_.revision_);
        QVERIFY_EXCEPTION_THROWN(SaveResult(r, changed, right), Failure); QCOMPARE(read(r.output_), QByteArray("external output\n"));
        cancel->store(true); QCOMPARE(Recompare(r, previous, model.State(), std::size_t(right.size()), cancel).completion_, QString("Canceled"));
    }
    void recompareDuplicateContexts()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left");
        r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        const auto padding = QByteArray("padding\n").repeated(20);
        const auto left = padding + "a\n" + padding + "a\n" + padding;
        const auto right = padding + "A\n" + padding + "A\n" + padding;
        write(r.file1_, left); write(r.file2_, right); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto previous = Execute(r, cancel, false); MergeModel model(previous.plan_); model.SelectAll(Resolution::Right);
        write(r.file1_, left + "new common tail\n"); write(r.file2_, right + "new common tail\n");
        const auto result = Recompare(r, previous, model.State(), std::size_t(right.size()), cancel);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.plan_->initial_.size(), std::size_t(2));
        for (const auto& state : result.plan_->initial_) { QVERIFY(!state.mapped_); QCOMPARE(state.resolution_, Resolution::Unresolved); }
    }
    void recompareReorderedBlocks()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left");
        r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        const auto padding = QByteArray("padding\n").repeated(20);
        const auto leftA = padding + "a\n" + padding, leftB = padding + "b\n" + padding;
        const auto rightA = padding + "A\n" + padding, rightB = padding + "B\n" + padding;
        write(r.file1_, leftA + leftB); write(r.file2_, rightA + rightB);
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto previous = Execute(r, cancel, false); MergeModel model(previous.plan_); model.SelectAll(Resolution::Right);
        write(r.file1_, leftB + leftA); write(r.file2_, rightB + rightA);
        const auto result = Recompare(r, previous, model.State(), std::size_t(rightA.size() + rightB.size()), cancel);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.plan_->initial_.size(), std::size_t(2));
        for (const auto& state : result.plan_->initial_) QVERIFY(!state.mapped_);
    }
    void randomizedReconstruction()
    {
        std::mt19937 rng(17021);
        for (int trial = 0; trial < 4000; ++trial)
        {
            std::string a, b;
            for (int i = 0, n = int(rng() % 45); i < n; ++i) { a += char('a' + rng() % 5); a += rng() % 2 ? "\n" : "\r\n"; }
            for (int i = 0, n = int(rng() % 45); i < n; ++i) { b += char('a' + rng() % 5); b += rng() % 2 ? "\n" : "\r\n"; }
            if (rng() % 2 && !a.empty()) a.pop_back(); if (rng() % 2 && !b.empty()) b.pop_back();
            const auto edits = Diff(a, b); QCOMPARE(Apply(a, edits), b);
            const auto al = SplitLines(a), bl = SplitLines(b); std::size_t length = 0;
            for (const auto& edit : edits) length += edit.end_ - edit.begin_ + edit.otherEnd_ - edit.otherBegin_;
            QCOMPARE(length, al.size() + bl.size() - 2 * lcs(al, bl));
        }
    }
    void linearDiffMatchesOptimalEdits()
    {
        std::mt19937 random(74532);
        for (int sample = 0; sample < 3000; ++sample)
        {
            std::array<std::string, 2> texts;
            for (auto& text : texts)
            {
                for (int i = 0, n = int(random() % 70); i < n; ++i) { text += char('a' + random() % 6); text += random() % 2 ? "\n" : "\r\n"; }
                if (random() % 2 && !text.empty()) text.pop_back();
            }
            const auto a = SplitLines(texts[0]), b = SplitLines(texts[1]); std::uint64_t steps = 0;
            const auto edits = LinearDiff(a, b, 0, 0, {}, steps); QCOMPARE(Apply(texts[0], edits), texts[1]);
            std::size_t cost = 0; for (const auto& edit : edits) cost += edit.end_ - edit.begin_ + edit.otherEnd_ - edit.otherBegin_;
            QCOMPARE(cost, a.size() + b.size() - 2 * lcs(a, b));
            steps = 0; QCOMPARE(LinearDiff(a, b, 0, 0, {}, steps), edits);
        }
    }
    void linearDiffFallbackAndLimits()
    {
        std::string a, b;
        for (int i = 0; i < 3000; ++i) { a += std::to_string(i) + "\n"; b += std::to_string(2999 - i) + "\n"; }
        QElapsedTimer timer; timer.start(); const auto edits = Diff(a, b); QCOMPARE(Apply(a, edits), b);
        std::size_t cost = 0; for (const auto& edit : edits) cost += edit.end_ - edit.begin_ + edit.otherEnd_ - edit.otherBegin_;
        QCOMPARE(cost, std::size_t(5998)); qInfo("Linear diff fallback: 3000 reversed lines in %lld ms", timer.elapsed());
        const auto al = SplitLines(a), bl = SplitLines(b); std::uint64_t steps = 0; Budget limits; limits.traceBytes_ = 8;
        QVERIFY_EXCEPTION_THROWN(LinearDiff(al, bl, 0, 0, limits, steps), Failure);
        limits = {}; limits.steps_ = 10; steps = 9; QVERIFY_EXCEPTION_THROWN(LinearDiff(al, bl, 0, 0, limits, steps), Failure);
        std::atomic_bool canceled{true}; limits = {}; limits.canceled_ = &canceled; steps = 0;
        QVERIFY_EXCEPTION_THROWN(LinearDiff(al, bl, 0, 0, limits, steps), Failure);
        limits = {}; limits.deadline_ = std::chrono::steady_clock::now(); QVERIFY_EXCEPTION_THROWN(LinearDiff(al, bl, 0, 0, limits, steps), Failure);
        const auto merged = Merge(a, a, b); QCOMPARE(merged.text_, b); QVERIFY(merged.conflicts_.empty());
        const auto paddedA = std::string("head\n") + a + "tail\n", paddedB = std::string("head\n") + b + "tail\n";
        QCOMPARE(Apply(paddedA, Diff(paddedA, paddedB)), paddedB);
        steps = 0; limits = {}; limits.traceBytes_ = LinearDiffWorkspaceBytes(al.size() + bl.size());
        QCOMPARE(Apply(a, LinearDiff(al, bl, 0, 0, limits, steps)), b);
        --limits.traceBytes_; steps = 0; QVERIFY_EXCEPTION_THROWN(LinearDiff(al, bl, 0, 0, limits, steps), Failure);
        // A long mostly equal input needs little history even if two full vectors cannot fit.
        std::string sparseA = "a\n", sparseB = "b\n";
        for (int i = 0; i < 20000; ++i) { sparseA += "same\n"; sparseB += "same\n"; }
        sparseA += "c\n"; sparseB += "d\n"; limits = {}; limits.traceBytes_ = 1024;
        QCOMPARE(Apply(sparseA, Diff(sparseA, sparseB, limits)), sparseB);
    }
    void mergeIdentities()
    {
        const std::vector<std::string> samples{"", "x", "x\n", "x\r\n", "a\nb\nc\n", "c\na\nb\n", "a\na\na\n"};
        for (const auto& base : samples) for (const auto& changed : samples)
        {
            const auto a = Merge(base, base, changed), b = Merge(base, changed, base), c = Merge(base, changed, changed);
            QCOMPARE(a.text_, changed); QVERIFY(a.conflicts_.empty());
            QCOMPARE(b.text_, changed); QVERIFY(b.conflicts_.empty());
            QCOMPARE(c.text_, changed); QVERIFY(c.conflicts_.empty());
        }
    }
    void mergeConflicts()
    {
        const auto disjoint = Merge("a\nb\nc\n", "A\nb\nc\n", "a\nb\nC\n");
        QCOMPARE(disjoint.text_, std::string("A\nb\nC\n")); QVERIFY(disjoint.conflicts_.empty());
        const auto sameGap = Merge("a\n", "x\na\n", "y\na\n"); QCOMPARE(sameGap.conflicts_.size(), std::size_t(1));
        QCOMPARE(sameGap.conflicts_[0].left_, std::string("x\n")); QCOMPARE(sameGap.conflicts_[0].right_, std::string("y\n"));
        const auto boundary = Merge("a\nb\n", "x\na\nb\n", "b\n");
        QCOMPARE(boundary.text_, std::string("x\nb\n")); QVERIFY(boundary.conflicts_.empty());
        const auto deleteModify = Merge("a\nb\nc\n", "a\n", "a\nB\nc\n"); QCOMPARE(deleteModify.conflicts_.size(), std::size_t(1));
        const auto swapped = Merge("a\nb\nc\n", "a\nB\nc\n", "a\n"); QCOMPARE(swapped.conflicts_.size(), deleteModify.conflicts_.size());
    }
    void budgets()
    {
        Budget budget; budget.traceBytes_ = 8; QVERIFY_EXCEPTION_THROWN(Diff("a\nb\n", "x\ny\n", budget), Failure);
        std::atomic_bool cancel(true); budget.canceled_ = &cancel; QVERIFY_EXCEPTION_THROWN(Diff("a", "b", budget), Failure);
        budget.canceled_ = nullptr; budget.deadline_ = std::chrono::steady_clock::now(); QVERIFY_EXCEPTION_THROWN(Diff("a", "a", budget), Failure);
    }
    void blockDecisions()
    {
        const std::string left = "a\nkeep\nlast\n", right = "A\nextra\nkeep\nLAST\n";
        auto plan = std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right)));
        MergeModel model(plan); QCOMPARE(model.UnresolvedCount(), std::size_t(2));
        std::string text = left;
        const auto old = model.Prepare(2, model.Revision(), Resolution::Right);
        MergeModel anotherSession(plan); QVERIFY_EXCEPTION_THROWN(anotherSession.Apply(old), Failure);
        auto change = model.Prepare(1, model.Revision(), Resolution::Right);
        text.replace(change.begin_, change.end_ - change.begin_, change.replacement_); model.Apply(change);
        QVERIFY_EXCEPTION_THROWN(model.Apply(old), Failure);
        change = model.Prepare(2, model.Revision(), Resolution::Right);
        text.replace(change.begin_, change.end_ - change.begin_, change.replacement_); model.Apply(change);
        QCOMPARE(text, right); QCOMPARE(model.UnresolvedCount(), std::size_t(0));
        const auto state = model.State(); const auto revision = model.Revision();
        model.TextChanged(0, 0, 3); QCOMPARE(model.UnresolvedCount(), std::size_t(1));
        model.Restore(state); QCOMPARE(model.State(), state); QVERIFY(model.Revision() > revision);
        QVERIFY_EXCEPTION_THROWN(model.Prepare(1, revision, Resolution::Left), Failure);
        QVERIFY_EXCEPTION_THROWN(model.Prepare(1, model.Revision(), Resolution::Base), Failure);
        QVERIFY_EXCEPTION_THROWN(model.Prepare(999, model.Revision(), Resolution::Right), Failure);
    }
    void threeWayBlockLocations()
    {
        std::mt19937 random(88117);
        for (int trial = 0; trial < 2000; ++trial)
        {
            const auto sample = [&] {
                std::string value;
                for (int i = 0, count = int(random() % 18); i < count; ++i)
                { value += char('a' + random() % 6); value += random() % 2 ? "\r\n" : "\n"; }
                if (!value.empty() && random() % 3 == 0) value.pop_back();
                return value;
            };
            const auto base = sample(), left = sample(), right = sample();
            const auto merged = Merge(base, left, right);
            auto plan = std::make_shared<MergePlan>(ThreeWayPlan(base, left, right, merged));
            MergeModel model(plan); auto text = merged.text_;
            for (const auto& block : plan->blocks_)
            {
                const auto change = model.Prepare(block.id_, model.Revision(), Resolution::Right);
                text.replace(change.begin_, change.end_ - change.begin_, change.replacement_); model.Apply(change);
            }
            for (std::size_t i = 0; i < plan->blocks_.size(); ++i)
            {
                const auto& at = model.State()[i];
                QCOMPARE(text.substr(at.begin_, at.end_ - at.begin_), plan->blocks_[i].right_);
            }
            QCOMPARE(model.UnresolvedCount(), std::size_t(0));
            QCOMPARE(text, right);
            for (const auto choice : {Resolution::Left, Resolution::Base})
            {
                for (const auto& block : plan->blocks_)
                {
                    const auto change = model.Prepare(block.id_, model.Revision(), choice);
                    text.replace(change.begin_, change.end_ - change.begin_, change.replacement_); model.Apply(change);
                }
                QCOMPARE(text, choice == Resolution::Left ? left : base);
            }
        }
    }
    void automaticThreeWayDecisionRoundTrip()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        write(r.base_, "a\nkeep\nz\n"); write(r.file1_, "L\nkeep\nz\n"); write(r.file2_, "a\nkeep\nR\n");
        auto result = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(result.exitCode_, 0); QCOMPARE(result.plan_->blocks_.size(), std::size_t(2));
        const auto original = result.output_; auto plan = MakeDecisionPlan(r, result, {}); auto blocks = plan["blocks"].toArray();
        QCOMPARE(blocks[0].toObject()["choice"].toString(), "left"); QCOMPARE(blocks[1].toObject()["choice"].toString(), "right");
        ApplyDecisionPlan(r, result, QJsonDocument(plan).toJson(), {}); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.output_, original);
        auto first = blocks[0].toObject(); first["choice"] = "base"; blocks[0] = first; plan["blocks"] = blocks;
        ApplyDecisionPlan(r, result, QJsonDocument(plan).toJson(), {}); QCOMPARE(result.output_, QByteArray("a\nkeep\nR\n"));
    }
    void multiPaneAlignmentSharedRows()
    {
        const std::vector<std::string_view> texts{"a\nb\n", "x\na\nb\n", "a\ny\nb\n", "x\na\ny\nb\n"};
        auto map = MultiAlignmentMap::Build(texts); QCOMPARE(map.RowCount(), std::size_t(4));
        const auto pair = map.Pair(0, 3); QCOMPARE(pair.DisplayRow(0, 0), std::size_t(1)); QCOMPARE(pair.DisplayRow(1, 1), std::size_t(1));
        QCOMPARE(pair.DisplayRow(0, 1), std::size_t(3)); QCOMPARE(pair.DisplayRow(1, 3), std::size_t(3));
        const auto identicalPair = MultiAlignmentMap::Build({"a\n", "a\n", "only third\na\n"}).Pair(0, 1);
        for (const auto& run : identicalPair.Runs()) QVERIFY(!run.changed_);
        std::mt19937 random(99172);
        for (int trial = 0; trial < 500; ++trial)
        {
            std::vector<std::string> data(4); std::vector<std::string_view> inputs;
            for (auto& text : data)
            {
                for (unsigned i = 0, n = random() % 20; i < n; ++i) { text += char('a' + random() % 5); text += random() % 2 ? "\r\n" : "\n"; }
                if (!text.empty() && random() % 3 == 0) text.pop_back(); inputs.push_back(text);
            }
            map = MultiAlignmentMap::Build(inputs);
            for (std::size_t side = 0; side < inputs.size(); ++side)
            {
                const auto projection = map.Pair(side, (side + 1) % inputs.size());
                for (std::size_t line = 0; line < map.LineCount(side); ++line)
                    QCOMPARE(projection.LineAtRow(0, projection.DisplayRow(0, line)), std::optional<std::size_t>(line));
                std::string rebuilt; const auto lines = SplitLines(inputs[side]);
                for (const auto& run : map.Runs()) for (auto i = run.begin_[side]; i < run.end_[side]; ++i) rebuilt += lines[i];
                QCOMPARE(rebuilt, data[side]);
            }
        }
        std::atomic_bool canceled{true}; Budget budget; budget.canceled_ = &canceled;
        QVERIFY_EXCEPTION_THROWN(MultiAlignmentMap::Build(texts, budget), Failure);
    }
    void commandLine()
    {
        const auto parse = [](QStringList Args) { Args.prepend("HyperCompare.exe"); return Parse(Args, true); };
        const auto valid = parse({"-file1", "D:\\한글 경로\\a.cpp", "-file2", "C:\\b.cpp"}); QVERIFY(valid.error_.isEmpty());
        QVERIFY(!parse({"-file1", "a", "-file1", "b", "-file2", "c"}).error_.isEmpty());
        QVERIFY(!parse({"-file1", "a", "-file2", "b", "-file3", "c", "-base", "d"}).error_.isEmpty());
        QVERIFY(!parse({"-file1", "a", "-file2", "b", "-output", "c"}).error_.isEmpty());
        QVERIFY(!parse({"-file1", "a", "-file2"}).error_.isEmpty());
        QVERIFY(!parse({"-file1", "a", "-file2", "b", "-mode", "directory-merge"}).error_.isEmpty());
        QVERIFY(parse({"-file1", "a", "-file2", "b", "-file3", "c"}).error_.isEmpty());
        const auto invalidHeadless = Parse({"HyperCompare.exe", "-headless", "-unknown"}, false);
        QVERIFY(invalidHeadless.request_.headless_); QVERIFY(!invalidHeadless.error_.isEmpty());
        const QStringList merge{"-file1", "a", "-file2", "b", "-mode", "merge", "-output", "out"};
        QVERIFY(parse(merge + QStringList{"-planout", "plan", "-dryrun"}).error_.isEmpty());
        QVERIFY(parse(merge + QStringList{"-decisions", "plan"}).error_.isEmpty());
        for (const auto& extra : {QStringList{"-decisions", "plan", "-takeall", "right"}, QStringList{"-planout", "plan", "-decisions", "plan2"},
            QStringList{"-planout", "out"}, QStringList{"-planout", "a"}, QStringList{"-decisions", "plan", "-report", "plan"},
            QStringList{"-planout", ""}, QStringList{"-planout", "-"}, QStringList{"-planout", "plan", "-gui"}})
            QVERIFY(!parse(merge + extra).error_.isEmpty());
    }
    void sessionAndSave()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid()); auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto a = dir.filePath("a.cpp"), b = dir.filePath("b.cpp"), base = dir.filePath("base.cpp"), out = dir.filePath("out.cpp");
        write(a, "A\nb\nc\n"); write(b, "a\nb\nC\n"); write(base, "a\nb\nc\n");
        Request request; request.file1_ = a; request.file2_ = b;
        auto result = Execute(request, cancel, true); QCOMPARE(result.exitCode_, 1); QCOMPARE(result.completion_, QString("Complete"));
        request.mode_ = "merge"; request.base_ = base; request.output_ = out;
        result = Execute(request, cancel, true); QCOMPARE(result.error_, QString{}); QCOMPARE(result.exitCode_, 0); QCOMPARE(read(out), QByteArray("A\nb\nC\n"));
        QCOMPARE(Execute(request, cancel, true).exitCode_, 2); QCOMPARE(read(out), QByteArray("A\nb\nC\n"));
        request.overwrite_ = true; result = Execute(request, cancel, false); QCOMPARE(result.exitCode_, 0);
        write(out, "external change"); QVERIFY_EXCEPTION_THROWN(SaveResult(request, result, result.output_), Failure); QCOMPARE(read(out), QByteArray("external change"));
        write(a, "a\nL\n"); write(b, "a\nR\n"); write(base, "a\n");
        result = Execute(request, cancel, true); QCOMPARE(result.exitCode_, 1); QCOMPARE(read(out), QByteArray("external change"));
        request.base_.clear(); request.takeAll_ = "right";
        result = Execute(request, cancel, true); QCOMPARE(result.exitCode_, 0); QCOMPARE(read(out), QByteArray("a\nR\n"));
        request.output_ = a; result = Execute(request, cancel, true); QCOMPARE(result.exitCode_, 2); QCOMPARE(read(a), QByteArray("a\nL\n"));
        request.output_ = out; result = Execute(request, cancel, false); QCOMPARE(result.exitCode_, 0);
        write(b, "changed after comparison");
        QVERIFY_EXCEPTION_THROWN(SaveResult(request, result, result.output_), Failure); QCOMPARE(read(out), QByteArray("a\nR\n"));
        request.dryRun_ = true; result = Execute(request, cancel, true); QCOMPARE(result.commit_, QString("NotRequested"));
        QVERIFY_EXCEPTION_THROWN(SaveResult(request, result, result.output_), Failure); QCOMPARE(read(out), QByteArray("a\nR\n"));
    }
    void threePairViewsAndHeadlessContract()
    {
        QTemporaryDir dir; auto cancel = std::make_shared<std::atomic_bool>(false); Request r;
        r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b"); r.file3_ = dir.filePath("c");
        const std::array<std::string, 3> text{"head\none A\r\n한글😀\nend", "head\none B\r\n한굴😃\nend", "prefix\nhead\none A\r\n한글😀\nend\ntail"};
        write(r.file1_, QByteArray::fromStdString(text[0])); write(r.file2_, QByteArray::fromStdString(text[1])); write(r.file3_, QByteArray::fromStdString(text[2]));
        const auto gui = Execute(r, cancel, false); QCOMPARE(gui.completion_, QString("Complete"));
        const std::array<std::array<int, 2>, 3> pairs{{{0, 1}, {0, 2}, {1, 2}}};
        for (std::size_t slot = 0; slot < pairs.size(); ++slot)
        {
            const auto& item = gui.pairViews_[slot]; QVERIFY(item); QCOMPARE(item->inputs_, pairs[slot]);
            const auto first = pairs[slot][0], second = pairs[slot][1]; const auto changes = Diff(text[first], text[second]);
            QCOMPARE(item->changes_.size(), changes.size());
            for (std::size_t i = 0; i < changes.size(); ++i)
            {
                QCOMPARE(item->changes_[i].begin_, changes[i].begin_); QCOMPARE(item->changes_[i].end_, changes[i].end_);
                QCOMPARE(item->changes_[i].otherBegin_, changes[i].otherBegin_); QCOMPARE(item->changes_[i].otherEnd_, changes[i].otherEnd_);
            }
            QCOMPARE(item->alignment_->LineCount(0), SplitLines(text[first]).size());
            QCOMPARE(item->alignment_->LineCount(1), SplitLines(text[second]).size());
            QCOMPARE(item->inline_.ranges_, BuildInlineDiff(text[first], text[second], changes).ranges_);
        }
        r.headless_ = true; const auto cli = Execute(r, cancel, false); QCOMPARE(cli.Json(), gui.Json());
        for (const auto& item : cli.pairViews_) QVERIFY(!item);
        // Captured metadata and decoded inputs remain tied to their original snapshot.
        write(r.file3_, "external change"); QCOMPARE(gui.inputs_[2].utf8_, QByteArray::fromStdString(text[2]));
        r.headless_ = false; r.base_ = r.file3_; r.file3_.clear(); r.mode_ = "merge"; r.output_ = dir.filePath("out");
        const auto merge = Execute(r, cancel, false); QCOMPARE(merge.completion_, QString("Complete"));
        QVERIFY(merge.pairViews_[0]); QVERIFY(!merge.pairViews_[1]); QVERIFY(!merge.pairViews_[2]);
    }
    void threePairBudgetsAndEmptyInputs()
    {
        QTemporaryDir dir; auto cancel = std::make_shared<std::atomic_bool>(false); Request r;
        r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b"); r.file3_ = dir.filePath("c");
        write(r.file1_, ""); write(r.file2_, "x"); write(r.file3_, "");
        auto result = Execute(r, cancel, false); QCOMPARE(result.completion_, QString("Complete"));
        QCOMPARE(result.exitCode_, 1); QVERIFY(result.pairViews_[1]->changes_.empty());
        QCOMPARE(result.pairViews_[1]->alignment_->RowCount(), std::size_t(0));
        QByteArray a, b, c;
        for (int i = 0; i < 20; ++i)
        { a += QByteArray(400, 'a') + '\n'; b += QByteArray(400, 'b') + '\n'; c += QByteArray(400, 'c') + '\n'; }
        write(r.file1_, a); write(r.file2_, b); write(r.file3_, c);
        result = Execute(r, cancel, false); QCOMPARE(result.completion_, QString("Complete"));
        std::uint64_t total = 0; std::size_t skipped = 0;
        for (const auto& item : result.pairViews_) { QVERIFY(item); total += item->inline_.cells_; skipped += item->inline_.skippedLines_; }
        QVERIFY(total <= 4'000'000); QVERIFY(skipped > 0);
        cancel->store(true); QCOMPARE(Execute(r, cancel, false).completion_, QString("Canceled"));
    }
    void threeFilesAreNotAncestors()
    {
        QTemporaryDir dir; auto cancel = std::make_shared<std::atomic_bool>(false); Request r;
        r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b"); r.file3_ = dir.filePath("c");
        write(r.file1_, "a\n"); write(r.file2_, "b\n"); write(r.file3_, "c\n");
        const auto result = Execute(r, cancel, false); QCOMPARE(result.operation_, QString("compare3"));
        QCOMPARE(result.comparisons_.size(), qsizetype(3)); QVERIFY(result.merged_.conflicts_.empty());
    }
    void encodingAndMissingInput()
    {
        QTemporaryDir dir; auto cancel = std::make_shared<std::atomic_bool>(false); Request r;
        r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b");
        write(r.file1_, QByteArray::fromHex("efbbbf61")); write(r.file2_, "a");
        auto result = Execute(r, cancel, false); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.outcome_, "Equivalent"); QVERIFY(result.edits_.empty());
        QVERIFY(result.Json()["inputs"].toArray()[0].toObject()["bom"].toBool());
        QVERIFY(result.textComparison_["encodingOnly"].toBool());
        r.textEquality_ = "bytes"; QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        write(r.file1_, QByteArray::fromHex("ff00")); QCOMPARE(Execute(r, cancel, false).exitCode_, 2);
        write(r.file1_, QByteArray("a\0b", 3)); QCOMPARE(Execute(r, cancel, false).error_, QString("document-text-nul-or-surrogate"));
        r.file1_ = dir.filePath("missing"); QCOMPARE(Execute(r, cancel, false).exitCode_, 2);
    }
    void mixedEncodingEquality()
    {
        QTemporaryDir dir; auto cancel = std::make_shared<std::atomic_bool>(false);
        Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.file3_ = dir.filePath("third");
        write(r.file1_, QByteArray::fromHex("fffe00ac0a00")); // UTF-16LE: 가 + LF
        write(r.file2_, QByteArray::fromHex("feffac00000a"));
        write(r.file3_, QString::fromUtf8("가\n").toUtf8());
        auto value = Execute(r, cancel, false); QCOMPARE(value.exitCode_, 0); QCOMPARE(value.outcome_, "Equivalent");
        QCOMPARE(value.textComparison_["pairs"].toArray().size(), 3);
        r.inputEncodings_[0] = "UTF-8"; QCOMPARE(Execute(r, cancel, false).error_, "EncodingBomMismatch");
        r.inputEncodings_[0] = "auto"; r.textEquality_ = "bytes"; QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        r.textEquality_ = "content"; write(r.file3_, QString::fromUtf8("가\r\n").toUtf8()); QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        r.file3_.clear(); write(r.file1_, QByteArray::fromHex("b0a10a")); r.inputEncodings_[0] = "CP949";
        value = Execute(r, cancel, false); QCOMPARE(value.exitCode_, 0); QCOMPARE(value.outcome_, "Equivalent");
        r.mode_ = "merge"; r.output_ = dir.filePath("out");
        value = Execute(r, cancel, false); SaveResult(r, value, QString::fromUtf8("가\n").toUtf8());
        QCOMPARE(value.exitCode_, 0); QCOMPARE(read(r.output_), QByteArray::fromHex("b0a10a"));
        auto parsed = Parse({"app", "-file1", r.file1_, "-file2", r.file2_, "-encoding1", "CP949", "-encoding2", "auto", "-text-equality", "bytes"}, true);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_)); QCOMPARE(InputEncoding(parsed.request_, 0), "CP949"); QVERIFY(InputEncoding(parsed.request_, 1).isEmpty());
    }
    void directoryTextAutoMergeAndEncoding()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true;
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.base_ = dir.filePath("b"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        for (const auto& root : {r.file1_, r.file2_, r.base_}) QVERIFY(QDir().mkpath(root));
        write(r.file1_ + "/a", "LEFT\r\nkeep\r\nend"); write(r.file2_ + "/a", "start\r\nkeep\r\nRIGHT"); write(r.base_ + "/a", "start\r\nkeep\r\nend");
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, true);
        QCOMPARE(result.exitCode_, 0); QCOMPARE(result.directoryPlan_["actions"].toArray()[0].toObject()["choice"].toString(), QString("merge"));
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_;
        const auto originalPlan = read(r.planOut_); auto oldPlan = result.directoryPlan_; auto oldPolicy = oldPlan["textMerge"].toObject(); oldPolicy["engine"] = "myers-bounded-1"; oldPlan["textMerge"] = oldPolicy;
        write(r.planOut_, QJsonDocument(oldPlan).toJson()); QCOMPARE(Execute(apply, cancel, true).error_, "DirectoryTextPlanEngineChanged"); QVERIFY(!QFileInfo::exists(r.output_)); write(r.planOut_, originalPlan);
        oldPolicy["engine"] = "myers-bounded-2"; oldPlan["textMerge"] = oldPolicy;
        write(r.planOut_, QJsonDocument(oldPlan).toJson()); QCOMPARE(Execute(apply, cancel, true).error_, "DirectoryTextPlanEngineChanged"); QVERIFY(!QFileInfo::exists(r.output_)); write(r.planOut_, originalPlan);
        result = Execute(apply, cancel, true); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QCOMPARE(read(r.output_ + "/a"), QByteArray("LEFT\r\nkeep\r\nRIGHT")); apply.resume_ = true; QCOMPARE(Execute(apply, cancel, true).exitCode_, 0);
        // Mixed encodings preserve LEFT's UTF-16LE BOM and byte-level newline policy.
        write(r.file1_ + "/a", QByteArray::fromHex("fffe4c000a0062000a006300")); write(r.file2_ + "/a", "a\nb\nR"); write(r.base_ + "/a", "a\nb\nc");
        r.output_ = dir.filePath("utf16"); r.planOut_ = dir.filePath("utf16.json"); result = Execute(r, cancel, true); QCOMPARE(result.exitCode_, 0);
        apply.applyPlan_ = r.planOut_; apply.resume_ = false; QCOMPARE(Execute(apply, cancel, true).exitCode_, 0);
        QCOMPARE(read(r.output_ + "/a"), QByteArray::fromHex("fffe4c000a0062000a005200"));
    }
    void directoryTextConflictsFallbackAndLegacyPlan()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true;
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.base_ = dir.filePath("b"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        for (const auto& root : {r.file1_, r.file2_, r.base_}) QVERIFY(QDir().mkpath(root));
        write(r.file1_ + "/a", "left"); write(r.file2_ + "/a", "right"); write(r.base_ + "/a", "base");
        write(r.file1_ + "/binary", QByteArray::fromHex("004c")); write(r.file2_ + "/binary", QByteArray::fromHex("0052")); write(r.base_ + "/binary", QByteArray::fromHex("0042"));
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, true); QCOMPARE(result.Json()["unresolvedCount"].toInt(), 2);
        auto plan = result.directoryPlan_; auto actions = plan["actions"].toArray(); auto item = actions[0].toObject(); item["choice"] = "merge"; actions[0] = item; plan["actions"] = actions; write(r.planOut_, QJsonDocument(plan).toJson());
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_;
        QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectoryTextMergeUnavailable")); QVERIFY(!QFileInfo::exists(r.output_));
        // Schema 1 plans without the newer text policy remain executable.
        plan.remove("textMerge"); plan["schemaVersion"] = 1;
        for (qsizetype i = 0; i < actions.size(); ++i) { auto action = actions[i].toObject(); action["choice"] = "left"; actions[i] = action; }
        plan["actions"] = actions; write(r.planOut_, QJsonDocument(plan).toJson()); QCOMPARE(Execute(apply, cancel, true).exitCode_, 0);
        QCOMPARE(read(r.output_ + "/a"), QByteArray("left")); QCOMPARE(read(r.output_ + "/binary"), QByteArray::fromHex("004c"));
    }
    void directoryApplyCopyAndResume()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true; r.takeAll_ = "left";
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        QVERIFY(QDir().mkpath(r.file1_ + "/sub")); QVERIFY(QDir().mkpath(r.file2_));
        write(r.file1_ + "/sub/a", QByteArray(2000000, 'a')); write(r.file1_ + "/z", "last");
        auto cancel = std::make_shared<std::atomic_bool>(false); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_; apply.dryRun_ = true;
        auto result = Execute(apply, cancel, true); QCOMPARE(result.exitCode_, 0); QVERIFY(!QFileInfo::exists(r.output_));
        QVERIFY(!QFileInfo::exists(r.planOut_ + ".journal"));
        apply.dryRun_ = false; Budget limits; limits.canceled_ = cancel.get();
        QVERIFY_EXCEPTION_THROWN(ApplyDirectoryPlan(apply, result, limits, [&](std::size_t Count) { if (Count == 3) cancel->store(true); }), Failure);
        QCOMPARE(result.commit_, QString("Partial")); QCOMPARE(read(r.output_ + "/sub/a"), QByteArray(2000000, 'a')); QVERIFY(!QFileInfo::exists(r.output_ + "/z"));
        cancel->store(false); apply.resume_ = true; result = Execute(apply, cancel, true);
        QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.commit_, QString("Committed")); QCOMPARE(read(r.output_ + "/z"), QByteArray("last"));
        const auto before = CaptureDirectory(r.output_, {}, {}); result = Execute(apply, cancel, true); QCOMPARE(result.exitCode_, 0);
        ValidateDirectory(before, {}, {}); // completed work is verified without copying again
        write(r.output_ + "/z", "external"); QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectorySnapshotMismatch"));
    }
    void directoryApplyPreconditionsAndDelete()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true; r.takeAll_ = "left"; r.overwrite_ = true; r.mirror_ = true;
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        for (const auto& root : {r.file1_, r.file2_, r.output_}) QVERIFY(QDir().mkpath(root));
        write(r.file1_ + "/a", "new"); write(r.output_ + "/a", "old"); QVERIFY(QDir().mkpath(r.output_ + "/extra/sub")); write(r.output_ + "/extra/sub/remove", "extra");
        auto cancel = std::make_shared<std::atomic_bool>(false); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_;
        QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectoryDeletionAuthorizationRequired")); QCOMPARE(read(r.output_ + "/a"), QByteArray("old"));
        apply.allowDelete_ = true; auto result = Execute(apply, cancel, true); QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_));
        QCOMPARE(result.exitCode_, 0); QCOMPARE(read(r.output_ + "/a"), QByteArray("new")); QVERIFY(!QFileInfo::exists(r.output_ + "/extra"));
        QCOMPARE(read(r.file1_ + "/a"), QByteArray("new"));
        apply.resume_ = true; write(r.file1_ + "/a", "changed source"); QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectorySnapshotMismatch"));
    }
    void directoryApplyRejectsPlanTampering()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true; r.takeAll_ = "left";
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        QVERIFY(QDir().mkpath(r.file1_ + "/sub")); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/sub/a", "data");
        auto cancel = std::make_shared<std::atomic_bool>(false); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        const auto original = QJsonDocument::fromJson(read(r.planOut_)).object(); Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_;
        auto changed = original; changed["schemaVersion"] = 999; write(r.planOut_, QJsonDocument(changed).toJson());
        QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectorySchemaInvalid"));
        changed = original; auto actions = changed["actions"].toArray(); auto action = actions[0].toObject(); action["path"] = "../escape"; actions[0] = action; changed["actions"] = actions;
        write(r.planOut_, QJsonDocument(changed).toJson()); QCOMPARE(Execute(apply, cancel, true).exitCode_, 2); QVERIFY(!QFileInfo::exists(r.output_));
        changed = original; actions = changed["actions"].toArray(); action = actions[0].toObject(); action["choice"] = "omit"; actions[0] = action; changed["actions"] = actions;
        write(r.planOut_, QJsonDocument(changed).toJson()); QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectoryParentChoiceConflict"));
        write(r.planOut_, QJsonDocument(original).toJson()); write(r.file1_ + "/added", "not captured");
        QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectorySnapshotMismatch")); QVERIFY(!QFileInfo::exists(r.output_));
        const auto parsed = Parse({"app", "-mode", "apply", "-plan", r.planOut_, "-resume", "-allowdelete", "-dryrun"}, true);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_)); QVERIFY(parsed.request_.resume_); QVERIFY(parsed.request_.allowDelete_);
    }
    void directoryApplyLostCompletionAndConsole()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true; r.takeAll_ = "left";
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a", "complete bytes");
        auto cancel = std::make_shared<std::atomic_bool>(false); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_;
        QCOMPARE(Execute(apply, cancel, false).outcome_, QString("Validated")); QVERIFY(!QFileInfo::exists(r.output_));
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-mode", "apply", "-plan", r.planOut_, "-format", "json"});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        QCOMPARE(QJsonDocument::fromJson(process.readAllStandardOutput()).object()["commit"].toString(), QString("Committed"));
        const auto snapshot = CaptureDirectory(r.output_, {}, {});
        // Simulate power loss after commit: durable pending record, torn completion suffix.
        const auto journalPath = r.planOut_ + ".journal"; auto journal = read(journalPath);
        const auto finalLine = journal.lastIndexOf('\n', journal.size() - 2) + 1; QVERIFY(finalLine > 0);
        journal.truncate(finalLine); journal += "{\"index\":"; write(journalPath, journal);
        apply.resume_ = true; const auto result = Execute(apply, cancel, true);
        QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(result.commit_, QString("Committed"));
        ValidateDirectory(snapshot, {}, {}); QCOMPARE(read(r.output_ + "/a"), QByteArray("complete bytes"));
        QCOMPARE(Execute(apply, cancel, true).exitCode_, 0);
    }
    void directoryApplyTypeReplacementAndEmptyFile()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true; r.takeAll_ = "left";
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); QVERIFY(QDir().mkpath(r.output_ + "/item/sub"));
        write(r.file1_ + "/empty", ""); write(r.file1_ + "/item", "replacement"); write(r.output_ + "/item/sub/keep", "existing");
        r.overwrite_ = true; auto cancel = std::make_shared<std::atomic_bool>(false); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        Request apply; apply.mode_ = "apply"; apply.headless_ = true; apply.applyPlan_ = r.planOut_; apply.allowDelete_ = true;
        QCOMPARE(Execute(apply, cancel, true).error_, QString("DirectoryPreservedChildConflict"));
        QCOMPARE(read(r.output_ + "/item/sub/keep"), QByteArray("existing"));
        r.mirror_ = true; r.planOut_ = dir.filePath("mirror.json"); QCOMPARE(Execute(r, cancel, true).exitCode_, 0);
        apply.applyPlan_ = r.planOut_; const auto result = Execute(apply, cancel, true);
        QVERIFY2(result.error_.isEmpty(), qPrintable(result.error_)); QCOMPARE(read(r.output_ + "/item"), QByteArray("replacement"));
        QVERIFY(QFileInfo(r.output_ + "/empty").isFile()); QCOMPARE(QFileInfo(r.output_ + "/empty").size(), qint64(0));
        apply.resume_ = true; QCOMPARE(Execute(apply, cancel, true).exitCode_, 0);
    }
    void directoryPlanExportAndChoices()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true;
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_));
        write(r.file1_ + "/equal", "same"); write(r.file2_ + "/equal", "same"); write(r.file1_ + "/one", "left"); write(r.file2_ + "/one", "right");
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, true);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.outcome_, QString("Planned"));
        QCOMPARE(result.exitCode_, 1); QCOMPARE(result.Json()["unresolvedCount"].toInt(), 1); QVERIFY(!QFileInfo::exists(r.output_));
        auto plan = QJsonDocument::fromJson(read(r.planOut_)).object(); QCOMPARE(plan["kind"].toString(), QString("HyperCompareDirectoryPlan"));
        const auto roots = plan["roots"].toArray(); QCOMPARE(roots.size(), qsizetype(2));
        QCOMPARE(roots[0].toObject()["entries"].toArray()[0].toObject()["sha256"].toString().size(), qsizetype(64));
        QCOMPARE(plan["actions"].toArray()[0].toObject()["choice"].toString(), QString("left"));
        QCOMPARE(Execute(r, cancel, true).error_, QString("PlanAlreadyExists"));
        r.planOut_ = dir.filePath("selected.json"); r.takeAll_ = "right";
        result = Execute(r, cancel, true); QCOMPARE(result.exitCode_, 0);
        for (const auto& action : result.directoryPlan_["actions"].toArray()) QCOMPARE(action.toObject()["choice"].toString(), QString("right"));
        const auto parsed = Parse({"app", "-mode", "directory-merge", "-file1", r.file1_, "-file2", r.file2_, "-output", r.output_, "-planout", dir.filePath("cli.json"), "-takeall", "left"}, true);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_));
    }
    void directoryMergeGuiRequestDoesNotWrite()
    {
        QTemporaryDir dir; const auto left = dir.filePath("left"), right = dir.filePath("right"), output = dir.filePath("output");
        QVERIFY(QDir().mkpath(left)); QVERIFY(QDir().mkpath(right)); write(left + "/a", "left");
        const auto parsed = Parse({"app", "-mode", "directory-merge", "-file1", left, "-file2", right, "-output", output}, false);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_));
        const auto result = Execute(parsed.request_, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.operation_, QString("directory2")); QVERIFY(!QFileInfo::exists(output));
    }
    void directoryPlanBaseMirrorAndBoundaries()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory-merge"; r.headless_ = true;
        r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.base_ = dir.filePath("b"); r.output_ = dir.filePath("out"); r.planOut_ = dir.filePath("plan.json");
        for (const auto& root : {r.file1_, r.file2_, r.base_, r.output_}) QVERIFY(QDir().mkpath(root));
        write(r.file1_ + "/a", "new"); write(r.file2_ + "/a", "old"); write(r.base_ + "/a", "old");
        write(r.output_ + "/extra", "preserve");
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, true);
        QCOMPARE(result.exitCode_, 0); QCOMPARE(result.directoryPlan_["actions"].toArray().size(), qsizetype(1));
        QCOMPARE(result.directoryPlan_["actions"].toArray()[0].toObject()["choice"].toString(), QString("left"));
        r.planOut_ = dir.filePath("mirror.json"); r.mirror_ = true; result = Execute(r, cancel, true);
        QCOMPARE(result.directoryPlan_["actions"].toArray().size(), qsizetype(2)); QCOMPARE(read(r.output_ + "/extra"), QByteArray("preserve"));
        r.output_ = r.file1_ + "/inside"; QCOMPARE(Execute(r, cancel, true).error_, QString("DirectoryOutputOverlapsInput"));
        r.output_ = dir.path(); QCOMPARE(Execute(r, cancel, true).error_, QString("DirectoryArtifactOverlapsRoot"));
        r.output_ = dir.filePath("out"); r.planOut_ = r.file1_ + "/plan.json";
        QCOMPARE(Execute(r, cancel, true).error_, QString("DirectoryArtifactOverlapsRoot"));
        for (const auto& suffix : {"../escape", "NUL", "name.", "name ", "file:stream"})
            QVERIFY_EXCEPTION_THROWN(CheckedDirectoryPath(dir.path() + '/' + suffix), Failure);
        QVERIFY(!DirectoryPathsOverlap(dir.path() + "/a", dir.path() + "/abc"));
    }
    void directoryRejectsUnqualifiedUncRoots()
    {
        for (const auto& path : {"//", "//server", "//server/", "//?/UNC/server/share", "//./server/share"})
        {
            try { CheckedDirectoryPath(QString::fromLatin1(path)); QFAIL("Unqualified/device path accepted"); }
            catch (const Failure& error) { QVERIFY2(error.code_ == "InvalidDirectoryPath", qPrintable(QString::fromLatin1(path) + ": " + QString::fromUtf8(error.what()))); }
        }
    }
    void directoryThreeWayStates()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); r.base_ = dir.filePath("b");
        for (const auto& root : {r.file1_, r.file2_, r.base_}) QVERIFY(QDir().mkpath(root));
        struct Case { const char* name; const char* left; const char* right; const char* base; const char* status; };
        const std::vector<Case> cases{
            {"same", "a", "a", "a", "Equal"}, {"left", "b", "a", "a", "LeftChanged"},
            {"right", "a", "b", "a", "RightChanged"}, {"both", "b", "b", "a", "BothSame"},
            {"conflict", "b", "c", "a", "Conflict"}, {"left-add", "a", nullptr, nullptr, "LeftAdded"},
            {"right-add", nullptr, "a", nullptr, "RightAdded"}, {"both-add", "a", "a", nullptr, "BothSame"},
            {"add-conflict", "a", "b", nullptr, "Conflict"}, {"delete-left", nullptr, "a", "a", "LeftDeleted"},
            {"delete-right", "a", nullptr, "a", "RightDeleted"}, {"delete-both", nullptr, nullptr, "a", "BothDeleted"},
            {"delete-modify", nullptr, "b", "a", "Conflict"}, {"modify-delete", "b", nullptr, "a", "Conflict"}};
        for (const auto& item : cases)
        {
            if (item.left) write(r.file1_ + '/' + item.name, item.left);
            if (item.right) write(r.file2_ + '/' + item.name, item.right);
            if (item.base) write(r.base_ + '/' + item.name, item.base);
        }
        auto cancel = std::make_shared<std::atomic_bool>(false); const auto result = Execute(r, cancel, false);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.operation_, QString("directory3"));
        QCOMPARE(result.outcome_, QString("Conflict")); QCOMPARE(result.entries_.size(), qsizetype(cases.size()));
        for (const auto& item : cases)
        {
            bool found = false;
            for (const auto& row : result.entries_) if (row.toObject()["path"] == item.name)
            { QCOMPARE(row.toObject()["status"].toString(), QString(item.status)); found = true; }
            QVERIFY(found);
        }
        r.file3_ = r.base_; r.base_.clear(); QCOMPARE(Execute(r, cancel, false).error_, QString("DirectoryIndependentThreeUnsupported"));
    }
    void directoryProgressQueueCancellation()
    {
        DirectoryProgressChannel channel; std::atomic_bool canceled{false}; Budget limits; limits.canceled_ = &canceled;
        for (int i = 0; i < 4; ++i) channel.Push({}, limits);
        auto waiting = std::async(std::launch::async, [&] { try { channel.Push({}, limits); return std::string{}; } catch (const Failure& error) { return error.code_; } });
        QVERIFY(waiting.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
        canceled.store(true); QVERIFY(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready); QCOMPARE(waiting.get(), std::string("Canceled"));
        for (int i = 0; i < 4; ++i) QVERIFY(channel.Take()); QVERIFY(!channel.Take());
    }
    void directoryTextEquivalenceAndPlan()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.directory_.level_ = "text";
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto utf8 = QString("가\r\n").toUtf8(); write(r.file1_ + "/a.txt", utf8);
        for (const auto& encoded : {QByteArray::fromHex("efbbbf") + utf8, QByteArray::fromHex("fffe00ac0d000a00"), QByteArray::fromHex("feffac00000d000a")})
        {
            write(r.file2_ + "/a.txt", encoded); const auto result = Execute(r, cancel, false);
            QVERIFY2(result.completion_ == "Complete", qPrintable(result.error_)); QCOMPARE(result.exitCode_, 0);
            QCOMPARE(result.entries_.first().Status(), QString("Equivalent")); QCOMPARE(result.Json()["entries"].toArray()[0].toObject()["status"].toString(), QString("Equivalent"));
            auto bytes = r; bytes.directory_.level_ = "bytes"; QCOMPARE(Execute(bytes, cancel, false).exitCode_, 1);
        }
        write(r.file2_ + "/a.txt", QByteArray::fromHex("b0a10d0a")); QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        r.inputEncodings_[1] = "CP949"; QCOMPARE(Execute(r, cancel, false).entries_.first().Status(), QString("Equivalent"));
        r.inputEncodings_[1].clear(); write(r.file2_ + "/a.txt", QString("가\n").toUtf8()); QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        write(r.file1_ + "/a.txt", QByteArray("a\0b", 3)); write(r.file2_ + "/a.txt", QByteArray("a\0c", 3)); QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        write(r.file1_ + "/a.txt", "AB"); write(r.file2_ + "/a.txt", "AB"); r.inputEncodings_[1] = "UTF-16LE";
        QCOMPARE(Execute(r, cancel, false).exitCode_, 1); r.inputEncodings_[1].clear();
        r.base_ = dir.filePath("base"); QVERIFY(QDir().mkpath(r.base_)); write(r.base_ + "/a.txt", utf8);
        write(r.file1_ + "/a.txt", QByteArray::fromHex("fffe00ac0d000a00")); write(r.file2_ + "/a.txt", "changed\r\n");
        auto result = Execute(r, cancel, false); QCOMPARE(result.entries_.first().Status(), QString("RightChanged"));
        r.output_ = dir.filePath("out"); const auto plan = MakeDirectoryPlan(r, result, {});
        QCOMPARE(plan["actions"].toArray()[0].toObject()["choice"].toString(), QString("right"));
        r.base_.clear(); write(r.file2_ + "/a.txt", utf8); result = Execute(r, cancel, false);
        const auto equivalentPlan = MakeDirectoryPlan(r, result, {}); QCOMPARE(equivalentPlan["actions"].toArray()[0].toObject()["choice"].toString(), QString("left"));
        r.planOut_ = dir.filePath("plan.json"); SaveDirectoryPlan(equivalentPlan, r.planOut_, result, {});
        Request apply; apply.mode_ = "apply"; apply.applyPlan_ = r.planOut_;
        const auto applied = Execute(apply, cancel, true); QVERIFY2(applied.exitCode_ == 0, qPrintable(applied.error_));
        QCOMPARE(read(r.output_ + "/a.txt"), read(r.file1_ + "/a.txt"));
        const auto parsed = Parse({"app", "-mode", "directory", "-file1", r.file1_, "-file2", r.file2_, "-directory-level", "text", "-encoding2", "CP949"}, true);
        QVERIFY2(parsed.error_.isEmpty(), qPrintable(parsed.error_)); QCOMPARE(parsed.request_.directory_.level_, QString("text"));
    }
    void compactDirectoryRowsPreserveEvidence()
    {
        const QJsonObject side{{"present", true}, {"path", "소스/File.cpp"}, {"directory", false}, {"size", "9007199254740993"}, {"modifiedUtcMs", "1792751958687"}};
        QJsonObject other = side; other["path"] = "소스/file.cpp";
        const QJsonObject row{{"path", "소스/File.cpp"}, {"status", "NameDifferent"}, {"leftRight", "NameDifferent"}, {"leftBase", "LeftOnly"}, {"rightBase", "LeftOnly"},
            {"left", side}, {"right", other}, {"base", QJsonObject{{"present", false}}}, {"errors", QJsonArray{}}};
        DirectoryEntries entries(QJsonArray{row}); QCOMPARE(entries.Json(), QJsonArray{row});
        auto copy = entries; copy.append(row); QCOMPARE(entries.size(), 1); QCOMPARE(copy.size(), 2);
        QCOMPARE(entries.first().sides_[0].size_, std::uint64_t(9007199254740993));
    }
    void missingDirectoryFileCreatesOnSaveOnly()
    {
        QTemporaryDir dir; Request r; r.mode_ = "compare"; r.file1_ = dir.filePath("absent/nested/new.txt"); r.file2_ = dir.filePath("existing.txt");
        write(r.file2_, "whole file\n"); const auto cancel = std::make_shared<std::atomic_bool>(false);
        QCOMPARE(Execute(r, cancel, false).completion_, QString("Failed"));
        r.allowMissingInputs_[0] = true; auto result = Execute(r, cancel, false);
        QVERIFY2(result.completion_ == "Complete", qPrintable(result.error_)); QCOMPARE(result.edits_.size(), 1);
        QVERIFY(!result.inputs_[0].exists_); QVERIFY(result.inputs_[0].utf8_.isEmpty()); QVERIFY(!QFileInfo::exists(dir.filePath("absent")));
        auto saved = SaveInputBuffer(result.inputs_[0], result.inputs_[1].utf8_, {});
        QVERIFY2(saved.error_.isEmpty(), qPrintable(saved.error_)); QVERIFY(saved.snapshot_.exists_); QCOMPARE(read(r.file1_), read(r.file2_));
        saved = SaveInputBuffer(result.inputs_[0], "racing overwrite", {});
        QVERIFY(!saved.error_.isEmpty()); QCOMPARE(read(r.file1_), QByteArray("whole file\n"));
        r.file1_ = dir.filePath("missing.txt"); r.allowMissingInputs_ = {true, false};
        result = Execute(r, cancel, false); QVERIFY(!result.inputs_[0].exists_);
        std::swap(r.file1_, r.file2_); r.allowMissingInputs_ = {false, true};
        result = Execute(r, cancel, false); QCOMPARE(result.completion_, QString("Complete")); QVERIFY(!result.inputs_[1].exists_);
        saved = SaveInputBuffer(result.inputs_[1], {}, {}); QVERIFY2(saved.error_.isEmpty(), qPrintable(saved.error_)); QVERIFY(QFileInfo::exists(r.file2_));
    }
    void directoryAccessFailuresPreserveKnownResults()
    {
        QTemporaryDir dir; const auto left = dir.filePath("left"), right = dir.filePath("right");
        QVERIFY(QDir().mkpath(left + "/blocked")); QVERIFY(QDir().mkpath(right + "/blocked"));
        write(left + "/known.txt", "left\n"); write(right + "/known.txt", "right\n");
        write(left + "/blocked/child", "same"); write(right + "/blocked/child", "same");
        write(left + "/locked.txt", "locked"); write(right + "/locked.txt", "locked");
        struct Acl { QString path_; QByteArray original_; }; QVector<Acl> changed;
        const auto restore = qScopeGuard([&] {
            for (const auto& acl : changed) SetFileSecurityW(reinterpret_cast<LPCWSTR>(acl.path_.utf16()), DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<char*>(acl.original_.constData())));
        });
        for (const auto& item : {qMakePair(left + "/blocked", QString("0x1")), qMakePair(left + "/locked.txt", QString("0x1"))})
        {
            DWORD size = 0; GetFileSecurityW(reinterpret_cast<LPCWSTR>(item.first.utf16()), DACL_SECURITY_INFORMATION, nullptr, 0, &size); QVERIFY(size > 0);
            QByteArray original(qsizetype(size), '\0'); QVERIFY(GetFileSecurityW(reinterpret_cast<LPCWSTR>(item.first.utf16()), DACL_SECURITY_INFORMATION, original.data(), size, &size));
            changed.push_back({item.first, original}); PSECURITY_DESCRIPTOR restricted = nullptr;
            const auto sddl = ("D:(D;;" + item.second + ";;;WD)(A;;FA;;;WD)").toStdWString();
            QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &restricted, nullptr));
            const auto free = qScopeGuard([&] { LocalFree(restricted); });
            QVERIFY(SetFileSecurityW(reinterpret_cast<LPCWSTR>(item.first.utf16()), DACL_SECURITY_INFORMATION, restricted));
        }
        Request r; r.mode_ = "directory"; r.file1_ = left; r.file2_ = right; r.output_ = dir.filePath("out");
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        for (const auto& base : {QString{}, right})
        {
            r.base_ = base; const auto result = Execute(r, cancel, false);
            QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.outcome_, QString("Partial")); QCOMPARE(result.exitCode_, 2);
            QVERIFY(!result.directoryComplete_); QVERIFY(!result.Json()["coverage"].toObject()["scopeComplete"].toBool());
            QMap<QString, QJsonObject> rows; for (const auto& entry : result.entries_) rows.insert(entry.toObject()["path"].toString(), entry.toObject());
            QVERIFY(rows.contains("known.txt")); QVERIFY(rows["known.txt"]["status"] != "Error");
            for (const auto& path : {"blocked", "blocked/child", "locked.txt"})
            { QVERIFY2(rows[path]["status"] == "Error", qPrintable(QString(path) + ": " + QString::fromUtf8(QJsonDocument(rows[path]).toJson()))); QVERIFY(!rows[path]["errors"].toArray().isEmpty()); }
            QVERIFY_EXCEPTION_THROWN(MakeDirectoryPlan(r, result, {}), Failure);
            QVERIFY_EXCEPTION_THROWN(ValidateDirectory(result.directories_->front(), r.directory_, {}), Failure);
        }
        QVERIFY_EXCEPTION_THROWN(CaptureDirectory(left, {}, {}), Failure); // Mutation capture remains strict.
        r.base_.clear(); r.directory_.exclude_ = {"blocked", "locked.txt"};
        const auto filtered = Execute(r, cancel, false); QCOMPARE(filtered.completion_, QString("Complete")); QVERIFY(filtered.directoryComplete_); QCOMPARE(filtered.entries_.size(), 1);
    }
    void directoryFiltersAndMetadata()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r");
        for (const auto& root : {r.file1_, r.file2_}) { QVERIFY(QDir().mkpath(root + "/nested")); QVERIFY(QDir().mkpath(root + "/build")); }
        write(r.file1_ + "/nested/a.cpp", "left"); write(r.file2_ + "/nested/a.cpp", "same");
        write(r.file1_ + "/skip.txt", "excluded"); write(r.file1_ + "/build/only.cpp", "excluded subtree");
        r.directory_.include_ = {"*.cpp"}; r.directory_.exclude_ = {"build"};
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, false);
        QCOMPARE(result.completion_, QString("Complete")); QCOMPARE(result.entries_.size(), qsizetype(2));
        QCOMPARE(result.exitCode_, 1); QVERIFY(result.Json()["coverage"].toObject()["filtered"].toBool());
        const auto stamp = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
        for (const auto& root : {r.file1_, r.file2_})
        { QFile f(root + "/nested/a.cpp"); QVERIFY(f.open(QIODevice::ReadWrite)); QVERIFY(f.setFileTime(stamp, QFileDevice::FileModificationTime)); }
        r.directory_.level_ = "metadata"; result = Execute(r, cancel, false);
        QCOMPARE(result.exitCode_, 0); QCOMPARE(result.Json()["coverage"].toObject()["comparisonLevel"].toString(), QString("metadata"));
        r.directory_.level_ = "bytes"; QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        r.directory_.include_ = {"nested/*.cpp"}; QCOMPARE(Execute(r, cancel, false).entries_.size(), qsizetype(2));
        r.directory_.include_ = {QString(257, 'a')}; QCOMPARE(Execute(r, cancel, false).error_, QString("InvalidDirectoryFilter"));
        const auto parsed = Parse({"app", "-file1", r.file1_, "-file2", r.file2_, "-exclude", "build;*.txt", "-directory-level", "metadata"}, true);
        QVERIFY(parsed.error_.isEmpty()); QCOMPARE(parsed.request_.directory_.exclude_.size(), qsizetype(2));
        QVERIFY(!Parse({"app", "-file1", "a", "-file2", "b", "-include", ""}, true).error_.isEmpty());
    }
    void directorySnapshotMutationAndKinds()
    {
        QTemporaryDir dir; const auto root = dir.filePath("root"); QVERIFY(QDir().mkpath(root)); write(root + "/a", "one");
        const auto first = CaptureDirectory(root, {}, {}); ValidateDirectory(first, {}, {});
        write(root + "/a", "changed"); QVERIFY_EXCEPTION_THROWN(ValidateDirectory(first, {}, {}), Failure);
        const auto second = CaptureDirectory(root, {}, {}); write(root + "/new", "one");
        QVERIFY_EXCEPTION_THROWN(ValidateDirectory(second, {}, {}), Failure);
        const auto third = CaptureDirectory(root, {}, {}); QVERIFY(QFile::remove(root + "/new"));
        QVERIFY_EXCEPTION_THROWN(ValidateDirectory(third, {}, {}), Failure);
        Request r; r.file1_ = root; r.file2_ = dir.filePath("right"); QVERIFY(QDir().mkpath(r.file2_ + "/a"));
        auto cancel = std::make_shared<std::atomic_bool>(false); auto result = Execute(r, cancel, false);
        QCOMPARE(result.entries_[0].toObject()["status"].toString(), QString("TypeConflict"));
        r.base_ = root + "/a"; QCOMPARE(Execute(r, cancel, false).error_, QString("ExpectedDirectory"));
        cancel->store(true); QCOMPARE(Execute(r, cancel, false).completion_, QString("Canceled"));
    }
    void directoryComparison()
    {
        QTemporaryDir dir; QDir root(dir.path()); QVERIFY(root.mkdir("l")); QVERIFY(root.mkdir("r"));
        write(dir.filePath("l/a"), "same"); write(dir.filePath("r/a"), "same");
        Request r; r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r"); auto cancel = std::make_shared<std::atomic_bool>(false);
        auto result = Execute(r, cancel, false); QCOMPARE(result.exitCode_, 0); QCOMPARE(result.operation_, QString("directory2"));
        write(dir.filePath("r/a"), "change"); QCOMPARE(Execute(r, cancel, false).exitCode_, 1);
        write(dir.filePath("l/b"), "new"); result = Execute(r, cancel, false); QCOMPARE(result.entries_.size(), qsizetype(2));
    }
    void consoleProcess()
    {
        QTemporaryDir dir; const auto a = dir.filePath("한글 a.cpp"), b = dir.filePath("공백 b.cpp"); write(a, "a\n"); write(b, "b\n");
        QProcess process; process.start(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe", {"-file1", a, "-file2", b, "-format", "json"});
        QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 1);
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError); QCOMPARE(doc.object()["exitCode"].toInt(), 1); QVERIFY(process.readAllStandardError().isEmpty());
        process.start(QCoreApplication::applicationDirPath() + "/HyperCompare.exe", {"-headless", "-unknown"});
        QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 2);
        QVERIFY(!process.readAllStandardError().isEmpty());
    }
};
QTEST_GUILESS_MAIN(CompareTests)
#include "tstCompare.moc"
