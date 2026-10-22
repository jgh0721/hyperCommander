#include "stdafx.h"
#include "core/compare.hpp"
#include "runtime/session.hpp"
#include "runtime/decisionPlan.hpp"
#include "runtime/mergeWork.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <random>
#include <Windows.h>

using namespace hc::compare;
class CompareTests final : public QObject
{
    Q_OBJECT
private:
    static void write(const QString& Path, const QByteArray& Data)
    { QFile file(Path); if (!file.open(QIODevice::WriteOnly) || file.write(Data) != Data.size()) qFatal("Fixture write failed"); }
    static QByteArray read(const QString& Path)
    { QFile file(Path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
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
        QVERIFY(three.plan_->blocks_.empty()); MergeWork automatic{three.output_, {}, false};
        QCOMPARE(DecodeMergeWork(r, three, EncodeMergeWork(r, three, automatic, {}), {}).text_, three.output_);
        write(r.base_, "head\nbase\r\nkeep\r\nbase"); three = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        QVERIFY(!three.plan_->blocks_.empty()); MergeWork conflicts{three.output_, three.plan_->initial_, false};
        QCOMPARE(DecodeMergeWork(r, three, EncodeMergeWork(r, three, conflicts, {}), {}).state_, conflicts.state_);
    }
    void mergeWorkStrictValidation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, QString::fromUtf8("가\r\nkeep\r\nz").toUtf8()); write(r.file2_, "A\r\nkeep\r\nZ");
        const auto session = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        MergeWork work{session.inputs_[0].utf8_, session.plan_->initial_, false};
        const auto json = EncodeMergeWork(r, session, work, {}); const auto original = QJsonDocument::fromJson(json).object();
        QVector<QByteArray> invalid; auto duplicate = json; duplicate.insert(1, "\"reviewRequired\":false,"); invalid.push_back(duplicate);
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
        QCOMPARE(initial.completion_, QString("Complete")); QCOMPARE(initial.plan_->blocks_.size(), std::size_t(1));
        const auto templatePlan = MakeDecisionPlan(r, initial, {});
        const std::vector<std::pair<QString, QByteArray>> cases{{"left", "L\nb\nlc\n"}, {"right", "R\nrb\nlc\n"},
            {"base", "a\nb\nlc\n"}, {"leftThenRight", "L\nb\nR\nrb\nlc\n"}, {"rightThenLeft", "R\nrb\nL\nb\nlc\n"}, {"manual", "lc\n"}};
        for (const auto& [name, expected] : cases)
        {
            auto plan = templatePlan; auto blocks = plan["blocks"].toArray(); auto block = blocks[0].toObject(); block["choice"] = name;
            blocks[0] = block; plan["blocks"] = blocks; auto result = initial;
            ApplyDecisionPlan(r, result, QJsonDocument(plan).toJson(), {});
            QCOMPARE(result.exitCode_, 0); QCOMPARE(result.output_, expected);
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
        }
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
        auto result = Execute(r, cancel, false); QCOMPARE(result.exitCode_, 1); QVERIFY(result.edits_.empty());
        write(r.file1_, QByteArray::fromHex("ff00")); QCOMPARE(Execute(r, cancel, false).exitCode_, 2);
        write(r.file1_, QByteArray("a\0b", 3)); QCOMPARE(Execute(r, cancel, false).error_, QString("document-text-nul-or-surrogate"));
        r.file1_ = dir.filePath("missing"); QCOMPARE(Execute(r, cancel, false).exitCode_, 2);
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
