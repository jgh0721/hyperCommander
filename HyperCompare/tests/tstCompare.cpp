#include "stdafx.h"
#include "core/compare.hpp"
#include "runtime/session.hpp"
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
