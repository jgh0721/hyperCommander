#include "stdafx.h"
#include "widgets/compareWindow.hpp"
#include "widgets/largeTextView.hpp"
#include <QSpinBox>
#include "widgets/guiRequestServer.hpp"
#include "runtime/guiIpc.hpp"
#include "runtime/controllerMemory.hpp"
#include "widgets/mergeController.hpp"
#include "widgets/boundedEditor.hpp"
#include "widgets/chunkGutter.hpp"
#include "widgets/multiPaneAlignment.hpp"
#include <QToolButton>
#include "widgets/scrollSynchronizer.hpp"
#include "widgets/scintillaPadding.hpp"
#include "widgets/findBar.hpp"
#include <QDockWidget>
#include <QCryptographicHash>
#include "widgets/overviewBar.hpp"
#include "widgets/directoryView.hpp"
#include "widgets/saveRecoveryDialog.hpp"
#include <QProcess>
#include <QProcessEnvironment>
#include "widgets/directoryTreeModel.hpp"
#include "widgets/directoryMergeDialog.hpp"
#include "widgets/semanticView.hpp"
#include "widgets/imageCompareView.hpp"
#include "widgets/archiveCompareView.hpp"
#include "zipFixture.hpp"
#include "docxFixture.hpp"
#include <QPlainTextEdit>
#include <QBuffer>
#include <QPainter>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableView>
#include <QPushButton>
#include <QAbstractItemModelTester>
#include <QtTest>
#include <QtMath>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QLabel>
#include <QTreeWidget>
#include <QAction>
#include <QFileDialog>
#include <QTimer>
#include <QMenuBar>
#include <QMenu>
#include <QScrollBar>
#include <QWheelEvent>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QInputDialog>
#include <QUuid>
#include <fmstyle/ThemeManager.h>
#include <ScintillaEditBase.h>
#include <Windows.h>
#include <psapi.h>

using namespace hc::compare;
class CompareUiTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir settings_;
    static void write(const QString& Path, const QByteArray& Text)
    { QFile file(Path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(Text), Text.size()); }
    static void recoveryFixture(const QString& Root, QString& Journal)
    {
        write(Root + "/destination.txt", "original\n");
        auto env = QProcessEnvironment::systemEnvironment(); env.insert("HC_SAVE_CRASH_ROOT", Root); env.insert("HC_SAVE_CRASH_PHASE", "2"); env.insert("HC_SAVE_CRASH_JOURNALS", Root + "/journals");
        QProcess child; child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationDirPath() + "/hc_compare_tests.exe", {"recoveryCrashChild", "-o", Root + "/child.txt,txt"});
        QVERIFY(child.waitForStarted()); QVERIFY(child.waitForFinished(15000)); QCOMPARE(child.exitCode(), 73);
        const auto files = QDir(Root + "/journals").entryList({"*.hcsave"}, QDir::Files); QCOMPARE(files.size(), 1);
        Journal = Root + "/journals/" + files[0]; QVERIFY(!QFileInfo::exists(Root + "/destination.txt"));
    }
    static QByteArray readBytes(const QString& Path)
    { QFile file(Path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
    static QAction* action(CompareWindow& Window, const QString& Text)
    { for (auto* item : Window.findChildren<QAction*>()) if (item->text() == Text) return item; return nullptr; }
    static QByteArray contents(ScintillaEditBase* Editor)
    {
        QByteArray text(qsizetype(Editor->send(SCI_GETLENGTH)) + 1, '\0');
        Editor->send(SCI_GETTEXT, uptr_t(text.size()), reinterpret_cast<sptr_t>(text.data())); text.chop(1); return text;
    }
    static void setText(ScintillaEditBase& Editor, const std::string& Text)
    {
        Editor.send(SCI_SETCODEPAGE, SC_CP_UTF8); Editor.send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(Text.c_str()));
        Editor.send(SCI_EMPTYUNDOBUFFER); Editor.send(SCI_SETSAVEPOINT);
    }
    static QByteArray lines(int Begin, int End, const QByteArray& Prefix = "common")
    {
        QByteArray text;
        for (int i = Begin; i < End; ++i) text += Prefix + QByteArray::number(i) + "\t" + QByteArray(120, 'x') + "\r\n";
        return text;
    }
    static void chooseFile(QAction* Action, const QString& Path)
    {
        // QFileSystemModel fills directories asynchronously. One early accept
        // can be ignored; retry the active dialog and bound the modal wait.
        QTimer timer; int tries = 0;
        QObject::connect(&timer, &QTimer::timeout, &timer, [&] {
            auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (++tries > 100) { if (active) active->reject(); return; }
            auto* dialog = qobject_cast<QFileDialog*>(active);
            if (!dialog) return;
            dialog->selectFile(Path);
            // selectFile can preserve a focused filename field; enter the path
            // through that field as the user would before accepting the dialog.
            if (auto* name = dialog->findChild<QLineEdit*>("fileNameEdit")) name->setText(QDir::toNativeSeparators(Path));
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        timer.start(50); Action->trigger(); timer.stop();
    }
private slots:
    void largeTextPagesEditMergeUndoAndSave()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.txt"); request.file2_ = dir.filePath("right.txt");
        const auto original = lines(0, 2500); auto changed = original; changed.replace("common200\t", "CHANGED200\t");
        write(request.file1_, original); write(request.file2_, changed);
        auto comparison = std::make_shared<LargeTextComparison>(CompareLargeText({ReadLargeText(request.file1_, {}, {}), ReadLargeText(request.file2_, {}, {})}, {}));
        LargeTextView view(request, comparison, nullptr); view.resize(1200, 700); view.show();
        auto* left = view.findChild<ScintillaEditBase*>("largeTextLeft"); auto* right = view.findChild<ScintillaEditBase*>("largeTextRight"); QVERIFY(left); QVERIFY(right);
        QTRY_VERIFY_WITH_TIMEOUT(!view.Busy() && !contents(left).isEmpty(), 10000); QCOMPARE(contents(left), lines(0, 400));
        view.findChild<QPushButton*>("largeMergeRight")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000);
        QVERIFY2(contents(right) == contents(left), qPrintable(view.findChild<QLabel*>("largeStatus")->text())); QCOMPARE(readBytes(request.file2_), changed); QVERIFY(view.Dirty());
        view.findChild<QPushButton*>("largeUndo")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QVERIFY(contents(right) != contents(left));
        view.findChild<QPushButton*>("largeRedo")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QCOMPARE(contents(right), contents(left));
        const QByteArray prefix("header\r\n"); left->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>(prefix.constData()));
        view.findChild<QPushButton*>("largeApplyPages")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QVERIFY(contents(left).startsWith(prefix));
        view.findChild<QPushButton*>("largeSaveLeft")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QCOMPARE(readBytes(request.file1_), prefix + original);
        view.findChild<QPushButton*>("largeSaveRight")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QCOMPARE(readBytes(request.file2_), original); QVERIFY(!view.Dirty());
        // An external writer after the last save must never be overwritten.
        write(request.file2_, "external"); view.findChild<QPushButton*>("largeSaveRight")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QCOMPARE(readBytes(request.file2_), QByteArray("external"));
    }
    void largeWindowAndLongLinePaging()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.txt"); request.file2_ = dir.filePath("right.txt");
        const QByteArray text(17 * 1024 * 1024, 'a'); write(request.file1_, text); write(request.file2_, text);
        CompareWindow window(request); window.resize(1200, 700); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QWidget*>("largeTextView"), 30000);
        auto* view = dynamic_cast<LargeTextView*>(window.findChild<QWidget*>("largeTextView")); QVERIFY(view);
        auto* left = view->findChild<ScintillaEditBase*>("largeTextLeft"); QTRY_VERIFY_WITH_TIMEOUT(!view->Busy() && !contents(left).isEmpty(), 10000);
        QCOMPARE(contents(left).size(), qsizetype(largeTextPageMaximum));
        view->findChild<QPushButton*>("largeNextLeft")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view->Busy(), 10000);
        left->send(SCI_SETTARGETSTART, 0); left->send(SCI_SETTARGETEND, 1); left->send(SCI_REPLACETARGET, 1, reinterpret_cast<sptr_t>("Z"));
        view->findChild<QPushButton*>("largeApplyPages")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view->Busy(), 30000);
        QVERIFY(contents(left).startsWith("Z"));
        view->findChild<QPushButton*>("largeSaveLeft")->click(); QTRY_VERIFY_WITH_TIMEOUT(!view->Busy(), 30000);
        auto expected = text; expected[qsizetype(largeTextPageMaximum)] = 'Z'; QCOMPARE(readBytes(request.file1_), expected); QCOMPARE(readBytes(request.file2_), text);
        action(window, "Dark")->setChecked(false); QTest::qWait(50);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/large-text-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/large-text-dark.png"));
        window.close();
    }
    void directoryPartialResultsAndPlanGuard()
    {
        Request request; request.file1_ = "D:/left"; request.file2_ = "D:/right"; Result value; value.directoryComplete_ = false;
        const QJsonObject missing{{"present", false}}, folder{{"present", true}, {"directory", true}, {"path", "blocked"}};
        value.entries_ = QJsonArray{
            QJsonObject{{"path", "blocked"}, {"status", "Error"}, {"left", folder}, {"right", folder}, {"errors", QJsonArray{QJsonObject{{"path", "blocked"}, {"error", "win32:5"}}}}},
            QJsonObject{{"path", "blocked/child"}, {"status", "Error"}, {"left", missing}, {"right", QJsonObject{{"present", true}, {"path", "blocked/child"}}}},
            QJsonObject{{"path", "normal"}, {"status", "Different"}, {"left", missing}, {"right", QJsonObject{{"present", true}, {"path", "normal"}}}}};
        DirectoryView view; view.resize(1000, 600); view.SetComparison(request, value); view.show();
        QVERIFY(!view.findChild<QPushButton*>("directoryMergePlan")->isEnabled());
        auto* filter = view.findChild<QComboBox*>("directoryStatusFilter"); QCOMPARE(filter->count(), 4); filter->setCurrentIndex(3);
        auto* tree = view.findChild<QTreeView*>("directoryLeftTree"); QVERIFY(tree); QCOMPARE(tree->model()->rowCount(), 1);
        const auto blocked = tree->model()->index(0, 0); QVERIFY(blocked.data(Qt::ToolTipRole).toString().contains("win32:5"));
        QCOMPARE(tree->model()->index(0, 1, blocked).data().toString(), QString::fromUtf8("비교 불가"));
        value.directoryComplete_ = true; view.SetComparison(request, value); QVERIFY(view.findChild<QPushButton*>("directoryMergePlan")->isEnabled()); view.close();
    }
    void gitSetupPreview()
    {
        CompareWindow window({}); window.resize(1100, 720); window.show();
        auto* setup = window.findChild<QAction*>("openGitSetup"); QVERIFY(setup); setup->trigger();
        auto* dialog = window.findChild<QDialog*>("gitSetupDialog"); QVERIFY(dialog);
        auto* text = dialog->findChild<QPlainTextEdit*>("gitSetupConfig"); QVERIFY(text); QVERIFY(text->isReadOnly());
        QVERIFY(text->toPlainText().contains("-git-tool merge")); QVERIFY(text->toPlainText().contains("trustExitCode = true"));
        QVERIFY(dialog->findChild<QPushButton*>("copyGitSetup"));
        action(window, "Dark")->setChecked(false); QTest::qWait(40); QVERIFY(dialog->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/git-setup-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(dialog->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/git-setup-dark.png"));
        dialog->close(); window.close();
    }
    void allPaneAlignmentPreservesEditingAndUndo()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        write(r.base_, "a\ncommon\nz\n"); write(r.file1_, "left\na\ncommon\nz\n"); write(r.file2_, "a\ncommon\nright\nz\n");
        CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<MultiPaneAlignment*>());
        auto* alignment = window.findChild<MultiPaneAlignment*>(); QTRY_VERIFY2(alignment->Ready(), qPrintable(alignment->Error()));
        const auto editors = window.findChildren<ScintillaEditBase*>(); QCOMPARE(editors.size(), 4);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); const auto initial = contents(output);
        const auto commonRow = [](ScintillaEditBase* Editor) { return Editor->send(SCI_VISIBLEFROMDOCLINE, Editor->send(SCI_LINEFROMPOSITION, uptr_t(contents(Editor).indexOf("common")))); };
        for (auto* editor : editors) QCOMPARE(commonRow(editor), commonRow(output));
        auto* changes = window.findChild<QTreeWidget*>("compareChanges"); changes->setCurrentItem(changes->topLevelItem(0));
        auto* chooseBase = action(window, QString::fromUtf8("블록 BASE")); QVERIFY(chooseBase); chooseBase->trigger(); QTRY_VERIFY(alignment->Ready());
        QCOMPARE(contents(output), QByteArray("a\ncommon\nright\nz\n"));
        for (auto* editor : editors) QCOMPARE(commonRow(editor), commonRow(output));
        output->send(SCI_UNDO); QTRY_VERIFY(alignment->Ready()); QCOMPARE(contents(output), initial);
        output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n"));
        QTRY_VERIFY2(alignment->Ready(), qPrintable(alignment->Error()));
        for (auto* editor : editors) QCOMPARE(commonRow(editor), commonRow(output));
        QCOMPARE(contents(output), QByteArray("manual\n") + initial);
        output->send(SCI_UNDO); QTRY_VERIFY(alignment->Ready()); QCOMPARE(contents(output), initial);
        output->send(SCI_REDO); QTRY_VERIFY(alignment->Ready()); QCOMPARE(contents(output), QByteArray("manual\n") + initial);
        output->send(SCI_SETFIRSTVISIBLELINE, 2); QTest::qWait(40);
        for (auto* editor : editors) QCOMPARE(editor->send(SCI_GETFIRSTVISIBLELINE), output->send(SCI_GETFIRSTVISIBLELINE));
        action(window, QString::fromUtf8("전체 패널 표시 줄 정렬"))->setChecked(false);
        QVERIFY(!window.findChild<MultiPaneAlignment*>()); QCOMPARE(contents(output), QByteArray("manual\n") + initial);
        output->send(SCI_UNDO); QCOMPARE(contents(output), initial);
        action(window, QString::fromUtf8("전체 패널 표시 줄 정렬"))->setChecked(true);
        QTRY_VERIFY(window.findChild<MultiPaneAlignment*>()->Ready());
        for (auto* editor : editors) QCOMPARE(commonRow(editor), commonRow(output));
        action(window, "Dark")->setChecked(false); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/all-pane-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/all-pane-dark.png"));
    }
    void allPaneAlignmentSwitchesComparePair()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("first"); r.file2_ = dir.filePath("second"); r.file3_ = dir.filePath("third");
        write(r.file1_, "common"); write(r.file2_, "prefix\ncommon"); write(r.file3_, "common\nsuffix\n");
        CompareWindow window(r); window.show();
        QTRY_VERIFY(window.findChild<MultiPaneAlignment*>() && window.findChild<MultiPaneAlignment*>()->Ready());
        auto* selector = window.findChild<QComboBox*>("comparePairSelector"); QVERIFY(selector);
        for (int pair : {1, 2, 0})
        {
            selector->setCurrentIndex(pair);
            QTRY_VERIFY(window.findChild<MultiPaneAlignment*>()->Ready());
            const auto editors = window.findChildren<ScintillaEditBase*>(); QCOMPARE(editors.size(), 3);
            sptr_t row = -1;
            for (auto* editor : editors)
            {
                const auto current = editor->send(SCI_VISIBLEFROMDOCLINE, editor->send(SCI_LINEFROMPOSITION, uptr_t(contents(editor).indexOf("common"))));
                if (row < 0) row = current; else QCOMPARE(current, row);
            }
        }
    }
    void allPaneSimilarLinesAfterEditing()
    {
        QWidget host; ScintillaEditBase base(&host), left(&host), right(&host), output(&host);
        const QByteArray a = "head\nalpha_value = 1;\nomega_counter = 2;\ntail\n";
        const QByteArray b = "head\n// left\nalpha_value = 9;\nomega_counter = 8;\ntail\n";
        const QByteArray c = "head\nalpha_value = 3;\n// right\nomega_counter = 4;\ntail\n";
        setText(base, a.toStdString()); setText(left, b.toStdString()); setText(right, c.toStdString()); setText(output, a.toStdString());
        host.resize(800, 400); host.show();
        MultiPaneAlignment alignment({&base, &left, &right, &output}, &host);
        const auto check = [&] {
            for (const auto word : {"alpha", "omega"})
            {
                const auto baseLine = base.send(SCI_LINEFROMPOSITION, a.indexOf(word));
                const auto row = base.send(SCI_VISIBLEFROMDOCLINE, baseLine);
                for (auto* editor : {&left, &right, &output})
                {
                    const auto line = editor->send(SCI_LINEFROMPOSITION, contents(editor).indexOf(word));
                    QCOMPARE(editor->send(SCI_VISIBLEFROMDOCLINE, line), row);
                }
            }
        };
        QTRY_VERIFY(alignment.Ready()); check();
        QCOMPARE(base.send(SCI_VISIBLEFROMDOCLINE, 1), sptr_t(2));
        QCOMPARE(base.send(SCI_VISIBLEFROMDOCLINE, 2), sptr_t(4));
        const QByteArray inserted = "// manual\n";
        output.send(SCI_INSERTTEXT, 5, reinterpret_cast<sptr_t>(inserted.constData()));
        QTRY_VERIFY(alignment.Ready()); check(); QCOMPARE(contents(&output), a.left(5) + inserted + a.mid(5));
        output.send(SCI_UNDO); QTRY_VERIFY(alignment.Ready()); check(); QCOMPARE(contents(&output), a);
        QCOMPARE(contents(&base), a); QCOMPARE(contents(&left), b); QCOMPARE(contents(&right), c);
    }
    void concurrentWindowsMemoryAndCancellation()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        QTemporaryDir dir; const auto left = dir.filePath("left.txt"), right = dir.filePath("right.txt"), third = dir.filePath("third.txt");
        const auto original = lines(0, 8000); auto modified = original; modified.replace("common4000", "edited4000");
        write(left, original); write(right, modified); write(third, original);
        std::uint64_t peak = 0; QTimer sampler;
        connect(&sampler, &QTimer::timeout, &sampler, [&] {
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
                peak = std::max<std::uint64_t>(peak, counters.PrivateUsage);
        }); sampler.start(10);
        for (int cycle = 0; cycle < 3; ++cycle) {
            std::vector<std::unique_ptr<CompareWindow>> windows;
            for (int i = 0; i < 3; ++i) {
                Request request; request.file1_ = left; request.file2_ = right; request.file3_ = third; request.memoryMiB_ = 512;
                windows.push_back(std::make_unique<CompareWindow>(request)); windows.back()->show();
            }
            if (!cycle) windows.back()->CancelRemoteRequest();
            for (std::size_t i = 0; i < windows.size(); ++i) {
                auto& window = *windows[i];
                if (!cycle && i == 2) { QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 3, 15000); continue; }
                QTRY_VERIFY_WITH_TIMEOUT(window.findChild<MultiPaneAlignment*>() && window.findChild<MultiPaneAlignment*>()->Ready(), 15000);
                QCOMPARE(contents(window.findChild<ScintillaEditBase*>("FILE 1")), original);
                QCOMPARE(contents(window.findChild<ScintillaEditBase*>("FILE 2")), modified);
                QCOMPARE(contents(window.findChild<ScintillaEditBase*>("FILE 3")), original);
            }
            for (auto& window : windows) window->close(); windows.clear(); QCoreApplication::processEvents();
            QCOMPARE(ProcessControllerMemory()->Reserved(), std::uint64_t(0));
            qInfo("Concurrent comparison cycle %d: 3 windows, sampled peak private %llu bytes, reservations returned", cycle + 1, peak);
        }
    }
    void largeConcurrentWindowsAdmission_data()
    {
        QTest::addColumn<int>("memoryMiB");
        QTest::newRow("bounded-admission") << 512;
        QTest::newRow("default-budget") << 2048;
    }
    void largeConcurrentWindowsAdmission()
    {
        QFETCH(int, memoryMiB);
        QSettings{}.setValue("view/alignAllPanes", true);
        QTemporaryDir dir; const auto left = dir.filePath("left.txt"), right = dir.filePath("right.txt");
        const auto original = lines(0, 80000); auto modified = original; modified.replace("common40000", "edited40000");
        write(left, original); write(right, modified);
        std::uint64_t peak = 0; QTimer sampler;
        connect(&sampler, &QTimer::timeout, &sampler, [&] {
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
                peak = std::max<std::uint64_t>(peak, counters.PrivateUsage);
        }); sampler.start(10);
        std::vector<std::unique_ptr<CompareWindow>> windows;
        for (int i = 0; i < 3; ++i) {
            Request r; r.file1_ = left; r.file2_ = right; r.file3_ = left; r.memoryMiB_ = memoryMiB;
            windows.push_back(std::make_unique<CompareWindow>(r)); windows.back()->show();
        }
        int completed = 0, rejected = 0;
        for (auto& window : windows) {
            QTRY_VERIFY_WITH_TIMEOUT((window->findChild<MultiPaneAlignment*>() && (window->findChild<MultiPaneAlignment*>()->Ready() || window->findChild<MultiPaneAlignment*>()->Error() == "ControllerMemoryLimit")) || window->ExitCode() == 2, 30000);
            if (window->ExitCode() == 2) {
                QVERIFY2(window->findChild<QLabel*>("compareStatus")->text().contains("ControllerMemoryLimit"), qPrintable(window->findChild<QLabel*>("compareStatus")->text())); ++rejected;
            } else {
                QCOMPARE(contents(window->findChild<ScintillaEditBase*>("FILE 1")), original);
                QCOMPARE(contents(window->findChild<ScintillaEditBase*>("FILE 2")), modified);
                if (window->findChild<MultiPaneAlignment*>()->Ready()) ++completed; else ++rejected;
            }
        }
        for (auto& window : windows) window->close(); windows.clear(); QCoreApplication::processEvents();
        QCOMPARE(completed + rejected, 3); QCOMPARE(ProcessControllerMemory()->Reserved(), std::uint64_t(0));
        qInfo("Three large windows under %d MiB: %d completed, %d safely limited, sampled peak private %llu bytes", memoryMiB, completed, rejected, peak);
        if (memoryMiB == 2048) QCOMPARE(completed, 3);
        QCOMPARE(readBytes(left), original); QCOMPARE(readBytes(right), modified);
    }
    void boundedEditorMemoryAndUndo()
    {
        constexpr std::uint64_t mib = 1024 * 1024;
        auto pool = std::make_shared<ControllerMemory>([] { return 100 * mib; });
        QWidget host; BoundedEditor editor(&host, 256, 1024, pool); setText(editor, "seed"); editor.EnableProtection(1024);
        editor.send(SCI_SETSEL, 1, 3); const auto cursor = editor.send(SCI_GETUNDOCURRENT);
        auto occupied = pool->ReserveController(1948 * mib); // default ceiling is 2 GiB outside a request
        QCOMPARE(editor.send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("replacement")), sptr_t(-1));
        QCOMPARE(contents(&editor), QByteArray("seed")); QCOMPARE(editor.send(SCI_GETSELECTIONSTART), sptr_t(1));
        QCOMPARE(editor.send(SCI_GETUNDOCURRENT), cursor);
        QTest::keyClick(&editor, Qt::Key_X); QCOMPARE(contents(&editor), QByteArray("seed"));
        QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("한글")); QApplication::sendEvent(&editor, &ime);
        QCOMPARE(contents(&editor), QByteArray("seed"));
        QMimeData dragText; dragText.setText("move");
        QDragEnterEvent enter(QPoint(1, 1), Qt::MoveAction, &dragText, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(editor.viewport(), &enter);
        QDropEvent drop(QPointF(1, 1), Qt::MoveAction, &dragText, Qt::LeftButton, Qt::NoModifier);
        drop.acceptProposedAction(); QApplication::sendEvent(editor.viewport(), &drop);
        QVERIFY(!drop.isAccepted()); QCOMPARE(contents(&editor), QByteArray("seed"));
        editor.send(SCI_SETREADONLY, 1);
        QVERIFY_EXCEPTION_THROWN(editor.BeginEdit(4, 4, false, false, true), Failure);
        QCOMPARE(contents(&editor), QByteArray("seed"));
        occupied.reset();
        { const auto refresh = editor.BeginEdit(4, 4, false, false, true); QVERIFY(bool(refresh)); }
        editor.send(SCI_SETREADONLY, 0); editor.send(SCI_SETSEL, 4, 4);
        editor.send(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("abc")); QCOMPARE(contents(&editor), QByteArray("seedabc"));
        occupied = pool->ReserveController(1948 * mib);
        QCOMPARE(editor.send(SCI_UNDO), sptr_t(-1)); QCOMPARE(contents(&editor), QByteArray("seedabc"));
        occupied.reset(); editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray("seed"));
        editor.send(SCI_REDO); QCOMPARE(contents(&editor), QByteArray("seedabc"));
        QCOMPARE(pool->Reserved(), std::uint64_t(0));
    }
    void boundedEditorSizeAndHistoryLimits()
    {
        auto pool = std::make_shared<ControllerMemory>([] { return 100ULL * 1024 * 1024; });
        QWidget host; BoundedEditor editor(&host, 256, 256, pool); setText(editor, "seed"); editor.EnableProtection(512);
        const QByteArray tooLarge(300, 'x');
        QCOMPARE(editor.send(SCI_APPENDTEXT, tooLarge.size(), reinterpret_cast<sptr_t>(tooLarge.constData())), sptr_t(-1));
        QCOMPARE(contents(&editor), QByteArray("seed"));
        const QByteArray first(80, 'a'), next(200, 'b');
        editor.send(SCI_APPENDTEXT, first.size(), reinterpret_cast<sptr_t>(first.constData())); QCOMPARE(contents(&editor), QByteArray("seed") + first);
        // A whole-document replacement fits the document limit but exceeds the retained Undo allowance.
        editor.send(SCI_SETTARGETRANGE, 0, editor.send(SCI_GETLENGTH));
        QCOMPARE(editor.send(SCI_REPLACETARGET, next.size(), reinterpret_cast<sptr_t>(next.constData())), sptr_t(-1));
        QCOMPARE(contents(&editor), QByteArray("seed") + first);
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray("seed"));
        editor.send(SCI_REDO); QCOMPARE(contents(&editor), QByteArray("seed") + first);
        editor.send(SCI_EMPTYUNDOBUFFER); editor.EnableProtection(1024);
        editor.send(SCI_SETTARGETRANGE, 0, editor.send(SCI_GETLENGTH));
        editor.send(SCI_REPLACETARGET, next.size(), reinterpret_cast<sptr_t>(next.constData())); QCOMPARE(contents(&editor), next);
        QCOMPARE(pool->Reserved(), std::uint64_t(0));
    }
    void boundedMergeRejectsBeforeDecisionChange()
    {
        constexpr std::uint64_t mib = 1024 * 1024;
        auto pool = std::make_shared<ControllerMemory>([] { return 100 * mib; });
        QWidget host; BoundedEditor editor(&host, 256, 1024, pool); setText(editor, "left\n"); editor.EnableProtection();
        const std::string left = "left\n", right = "right\n";
        MergeController controller(&editor, std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right))), &host);
        const auto before = controller.Model().State(); const auto revision = controller.Model().Revision();
        auto occupied = pool->ReserveController(1948 * mib);
        QVERIFY_EXCEPTION_THROWN(controller.ChooseAll(Resolution::Right, "right\n"), Failure);
        QCOMPARE(contents(&editor), QByteArray("left\n")); QCOMPARE(controller.Model().Revision(), revision);
        QCOMPARE(controller.Model().State().size(), before.size());
        for (std::size_t i = 0; i < before.size(); ++i) QCOMPARE(controller.Model().State()[i].resolution_, before[i].resolution_);
        occupied.reset(); controller.ChooseAll(Resolution::Right, "right\n"); QCOMPARE(contents(&editor), QByteArray("right\n"));
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray("left\n")); QCOMPARE(pool->Reserved(), std::uint64_t(0));
    }
    void allPaneMemoryPressurePreservesEditors()
    {
        QWidget host; ScintillaEditBase first(&host), second(&host), third(&host);
        const QByteArray large(2 * 1024 * 1024, 'a');
        setText(first, large.toStdString()); setText(second, "second"); setText(third, "third");
        const auto pool = ProcessControllerMemory(); auto scope = pool->Enter(2048ULL * 1024 * 1024);
        PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
        QVERIFY(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)));
        QVERIFY(counters.PrivateUsage < 2047ULL * 1024 * 1024);
        auto occupied = pool->ReserveController(2047ULL * 1024 * 1024 - counters.PrivateUsage);
        MultiPaneAlignment alignment({&first, &second, &third}, &host);
        QTRY_COMPARE(alignment.Error(), QString("ControllerMemoryLimit"));
        QVERIFY(!alignment.Ready()); QCOMPARE(contents(&first), large); QCOMPARE(contents(&second), QByteArray("second"));
        occupied.reset(); setText(first, "first"); alignment.Refresh();
        QTRY_VERIFY2(alignment.Ready(), qPrintable(alignment.Error()));
        QCOMPARE(contents(&first), QByteArray("first")); QCOMPARE(contents(&third), QByteArray("third"));
    }
    void allPaneAlignmentEmptyDocuments()
    {
        QWidget host; ScintillaEditBase first(&host), second(&host), third(&host);
        setText(first, ""); setText(second, "insert"); setText(third, ""); host.resize(600, 300); host.show();
        MultiPaneAlignment alignment({&first, &second, &third}, &host);
        QTRY_VERIFY(alignment.Ready()); QCOMPARE(contents(&first), QByteArray{}); QCOMPARE(contents(&second), QByteArray("insert"));
        QTRY_COMPARE(first.send(SCI_VISIBLEFROMDOCLINE, 0), sptr_t(1));
        second.send(SCI_CLEARALL); QTRY_VERIFY(alignment.Ready());
        QCOMPARE(first.send(SCI_VISIBLEFROMDOCLINE, 0), sptr_t(0)); QCOMPARE(third.send(SCI_VISIBLEFROMDOCLINE, 0), sptr_t(0));
    }
    void automaticThreeWayChunksRemainEditable()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        write(r.base_, "a\nkeep\nz\n"); write(r.file1_, "L\nkeep\nz\n"); write(r.file2_, "a\nkeep\nR\n");
        CompareWindow window(r); window.show(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QTRY_COMPARE(tree->topLevelItemCount(), 2); auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output);
        QCOMPARE(contents(output), QByteArray("L\nkeep\nR\n")); QVERIFY(action(window, QString::fromUtf8("결과 저장"))->isEnabled());
        QVERIFY(tree->topLevelItem(0)->text(0).contains(QString::fromUtf8("자동 구간")));
        tree->setCurrentItem(tree->topLevelItem(0)); action(window, QString::fromUtf8("블록 BASE"))->trigger();
        QCOMPARE(contents(output), QByteArray("a\nkeep\nR\n"));
        output->send(SCI_UNDO); QCOMPARE(contents(output), QByteArray("L\nkeep\nR\n"));
        output->send(SCI_REDO); QCOMPARE(contents(output), QByteArray("a\nkeep\nR\n"));
        tree->setCurrentItem(tree->topLevelItem(1)); action(window, QString::fromUtf8("블록 LEFT"))->trigger();
        QCOMPARE(contents(output), QByteArray("a\nkeep\nz\n"));
        action(window, QString::fromUtf8("결과 저장"))->trigger(); QTRY_COMPARE(window.ExitCode(), 0); window.close();
    }
    void chunkGutterApplyRemoveUndoAndWork()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("result.cpp");
        write(r.file1_, "start\nleft\nkeep\nold\nend\n"); write(r.file2_, "start\nright\nkeep\nnew\nend\n");
        CompareWindow window(r); window.show();
        QTRY_COMPARE(window.findChildren<ScintillaEditBase*>().size(), 3);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output);
        auto* left = window.findChild<ScintillaEditBase*>("LEFT"); auto* right = window.findChild<ScintillaEditBase*>("RIGHT");
        QVERIFY(left->mapTo(&window, QPoint{}).x() < output->mapTo(&window, QPoint{}).x()); QVERIFY(output->mapTo(&window, QPoint{}).x() < right->mapTo(&window, QPoint{}).x());
        auto gutters = window.findChildren<ChunkGutter*>(); QCOMPARE(gutters.size(), 2);
        std::sort(gutters.begin(), gutters.end(), [](auto* A, auto* B) { return A->SourceSide() < B->SourceSide(); });
        QTRY_VERIFY(gutters[1]->findChild<QToolButton*>("chunk1_action0"));
        QTRY_VERIFY(gutters[1]->findChild<QToolButton*>("chunk1_action0")->isVisible());
        QTest::mouseClick(gutters[1]->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
        QCOMPARE(contents(output), QByteArray("start\nright\nkeep\nold\nend\n"));
        QTRY_VERIFY(gutters[0]->findChild<QToolButton*>("chunk2_action1")->isVisible());
        QTest::mouseClick(gutters[0]->findChild<QToolButton*>("chunk2_action1"), Qt::LeftButton);
        QCOMPARE(contents(output), QByteArray("start\nright\nkeep\nend\n"));
        output->send(SCI_UNDO); QCOMPARE(contents(output), QByteArray("start\nright\nkeep\nold\nend\n"));
        output->send(SCI_REDO); QCOMPARE(contents(output), QByteArray("start\nright\nkeep\nend\n"));
        window.SaveWork(dir.filePath("work.hcmerge")); QTRY_VERIFY(QFileInfo::exists(dir.filePath("work.hcmerge")));
        QTRY_VERIFY(action(window, QString::fromUtf8("결과 저장"))->isEnabled());
        output->send(SCI_UNDO); window.LoadWork(dir.filePath("work.hcmerge"));
        QTRY_COMPARE(contents(output), QByteArray("start\nright\nkeep\nend\n"));
        for (auto* editor : {left, output, right}) editor->send(SCI_SETFIRSTVISIBLELINE, 0);
        action(window, "Dark")->setChecked(false); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/chunk-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/chunk-dark.png"));
        action(window, QString::fromUtf8("결과 저장"))->trigger(); QTRY_COMPARE(window.ExitCode(), 0);
        QFile saved(r.output_); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QByteArray("start\nright\nkeep\nend\n")); window.close();
    }
    void chunkGutterVirtualizesAndRejectsStaleChoice()
    {
        QWidget host; host.resize(700, 180); ScintillaEditBase left(&host), right(&host);
        left.setGeometry(0, 0, 300, 180); right.setGeometry(400, 0, 300, 180);
        const auto bytes = lines(0, 10000); setText(left, bytes.toStdString()); setText(right, bytes.toStdString());
        ChunkGutter gutter(&left, &right, 0, &host); gutter.setGeometry(300, 0, 84, 180); host.show();
        std::vector<GutterChunk> chunks;
        for (int line = 0; line < 10000; ++line)
        {
            const auto begin = std::size_t(left.send(SCI_POSITIONFROMLINE, line)); const auto end = std::size_t(left.send(SCI_GETLINEENDPOSITION, line));
            chunks.push_back({std::uint64_t(line + 1), {begin, begin}, {end, end}});
        }
        gutter.SetChunks(chunks, 7); QTest::qWait(40); QVERIFY(gutter.findChildren<QToolButton*>().size() < 100);
        left.send(SCI_SETFIRSTVISIBLELINE, 9000); right.send(SCI_SETFIRSTVISIBLELINE, 9000); gutter.Refresh();
        QTRY_VERIFY(gutter.findChild<QToolButton*>("chunk9001_action0")); QVERIFY(gutter.findChildren<QToolButton*>().size() < 100);
        QFont larger = gutter.font(); larger.setPointSize(20); gutter.setFont(larger);
        QTRY_VERIFY(gutter.findChild<QToolButton*>("chunk9001_action0")->height() >= gutter.fontMetrics().height() + 2);
        const auto edits = Diff("a\n", "b\n", {});
        auto plan = std::make_shared<MergePlan>(TwoWayPlan("a\n", "b\n", edits)); MergeModel model(plan);
        const auto revision = model.Revision(); model.Apply(model.Prepare(1, revision, Resolution::Omit));
        QVERIFY_EXCEPTION_THROWN(model.Prepare(1, revision, Resolution::Right), Failure);
        QCOMPARE(model.State()[0].begin_, model.State()[0].end_);
    }
    void chunkGutterControlsFollowBothEdges()
    {
        QWidget host; host.resize(800, 400); ScintillaEditBase left(&host), right(&host);
        left.setGeometry(0, 0, 320, 400); right.setGeometry(480, 0, 320, 400);
        for (auto* editor : {&left, &right})
        {
            editor->send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<sptr_t>("Consolas"));
            editor->send(SCI_STYLESETSIZE, STYLE_DEFAULT, 12); editor->send(SCI_STYLECLEARALL); editor->send(SCI_SETENDATLASTLINE, 0);
        }
        const auto text = lines(0, 80); setText(left, text.toStdString()); setText(right, text.toStdString());
        ChunkGutter gutter(&left, &right, -1, &host); gutter.setGeometry(320, 0, 100, 400); host.show();
        const auto pos = [](ScintillaEditBase& editor, int line) { return std::size_t(editor.send(SCI_POSITIONFROMLINE, line)); };
        gutter.SetChunks({{1, {pos(left, 2), pos(right, 8)}, {pos(left, 3), pos(right, 9)}}}, 1);
        QTRY_VERIFY(gutter.findChild<QToolButton*>("chunk1_action1"));
        auto* forward = gutter.findChild<QToolButton*>("chunk1_action0"); auto* backward = gutter.findChild<QToolButton*>("chunk1_action1");
        QTRY_VERIFY(forward->isVisible() && backward->isVisible());
        QVERIFY(forward->geometry().right() + 20 < backward->x()); QVERIFY(forward->y() < backward->y());
        QCOMPARE(forward->y(), int(left.send(SCI_POINTYFROMPOSITION, 0, pos(left, 2))));
        QCOMPARE(backward->y(), int(right.send(SCI_POINTYFROMPOSITION, 0, pos(right, 8))));
        QVERIFY(forward->autoRaise()); QCOMPARE(forward->arrowType(), Qt::RightArrow); QCOMPARE(backward->arrowType(), Qt::LeftArrow);
        left.send(SCI_SETFIRSTVISIBLELINE, 30); gutter.Refresh();
        QTest::qWait(50);
        QVERIFY2(!forward->isVisible(), qPrintable(QString("first=%1 y=%2 widgetY=%3").arg(left.send(SCI_GETFIRSTVISIBLELINE)).arg(left.send(SCI_POINTYFROMPOSITION, 0, pos(left, 2))).arg(forward->y()))); QVERIFY(backward->isVisible());
        left.send(SCI_SETFIRSTVISIBLELINE, 0); gutter.Refresh(); QTRY_VERIFY(forward->isVisible());
        QVERIFY(host.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/chunk-edge-layout.png"));
        host.close();
    }
    void chunkGutterEmptyEofAndThreeWay()
    {
        for (int scenario = 0; scenario < 3; ++scenario)
        {
            QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
            const auto left = scenario == 0 ? QByteArray{} : scenario == 1 ? QByteArray("keep\n") : QByteArray("keep\nleft");
            const auto right = scenario == 0 ? QString::fromUtf8("가").toUtf8() : scenario == 1 ? QByteArray("keep\ninsert") : QByteArray("keep\nright");
            write(r.file1_, left); write(r.file2_, right);
            if (scenario == 2) { r.base_ = dir.filePath("base"); write(r.base_, "keep\nbase"); }
            CompareWindow window(r); window.show();
            QTRY_VERIFY(window.findChild<ScintillaEditBase*>("RESULT")); auto* output = window.findChild<ScintillaEditBase*>("RESULT");
            action(window, QString::fromUtf8("왼쪽·오른쪽 표시 줄 정렬"))->setChecked(true);
            auto gutters = window.findChildren<ChunkGutter*>(); QCOMPARE(gutters.size(), 2);
            std::sort(gutters.begin(), gutters.end(), [](auto* A, auto* B) { return A->SourceSide() < B->SourceSide(); });
            QTRY_VERIFY(gutters[1]->findChild<QToolButton*>("chunk1_action0"));
            auto* take = gutters[1]->findChild<QToolButton*>("chunk1_action0"); QTRY_VERIFY(take->isVisible());
            take->setFocus(); QTest::keyClick(take, Qt::Key_Space); QCOMPARE(contents(output), right);
            QTRY_VERIFY(gutters[1]->findChild<QToolButton*>("chunk1_action1")->isVisible());
            QTest::mouseClick(gutters[1]->findChild<QToolButton*>("chunk1_action1"), Qt::LeftButton);
            QCOMPARE(contents(output), scenario == 0 ? QByteArray{} : QByteArray("keep\n"));
            output->send(SCI_UNDO); QCOMPARE(contents(output), right);
            action(window, QString::fromUtf8("결과 저장"))->trigger(); QTRY_COMPARE(window.ExitCode(), 0); window.close();
        }
    }
    void compareGutterAppliesInPlaceAndSavesEachFile()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        write(r.file1_, "A\nkeep\nleft\n"); write(r.file2_, "B\nkeep\nright\n");
        CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ChunkGutter*>());
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        auto* tree = window.findChild<QTreeWidget*>("compareChanges"); QTRY_COMPARE(tree->topLevelItemCount(), 2);
        int dialogs = 0; QTimer guard; connect(&guard, &QTimer::timeout, &guard, [&] { if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) { ++dialogs; dialog->reject(); } }); guard.start(10);
        auto* gutter = window.findChild<ChunkGutter*>(); QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action0")->isVisible());
        QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
        QCOMPARE(contents(right), QByteArray("A\nkeep\nright\n")); QCOMPARE(dialogs, 0); QVERIFY(!window.findChild<ScintillaEditBase*>("RESULT"));
        QFile originalRight(r.file2_); QVERIFY(originalRight.open(QIODevice::ReadOnly)); QCOMPARE(originalRight.readAll(), QByteArray("B\nkeep\nright\n")); originalRight.close();
        QTRY_COMPARE(tree->topLevelItemCount(), 1); QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action1")->isVisible());
        action(window, "Dark")->setChecked(false); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/input-edit-pending-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/input-edit-pending-dark.png"));
        QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action1"), Qt::LeftButton);
        QCOMPARE(contents(left), QByteArray("A\nkeep\nright\n")); QTRY_COMPARE(tree->topLevelItemCount(), 0); QCOMPARE(dialogs, 0); guard.stop();
        action(window, QString::fromUtf8("실행 취소"))->trigger(); QCOMPARE(contents(left), QByteArray("A\nkeep\nleft\n"));
        action(window, QString::fromUtf8("다시 실행"))->trigger(); QCOMPARE(contents(left), QByteArray("A\nkeep\nright\n"));
        auto* saveRight = window.findChild<QAction*>("saveInput2"); auto* saveLeft = window.findChild<QAction*>("saveInput1");
        QVERIFY(saveRight->isEnabled()); saveRight->trigger(); QTRY_VERIFY(!right->send(SCI_GETMODIFY));
        QVERIFY(originalRight.open(QIODevice::ReadOnly)); QCOMPARE(originalRight.readAll(), QByteArray("A\nkeep\nright\n")); originalRight.close();
        QVERIFY(left->send(SCI_GETMODIFY)); QTRY_VERIFY(saveLeft->isEnabled()); saveLeft->trigger(); QTRY_VERIFY(!left->send(SCI_GETMODIFY));
        QTRY_COMPARE(window.ExitCode(), 0); QCOMPARE(window.findChildren<ScintillaEditBase*>().size(), 2);
        action(window, "Dark")->setChecked(false); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/input-edit-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/input-edit-dark.png")); window.close();
    }
    void selectedChunkRange_data()
    {
        QTest::addColumn<QByteArray>("source"); QTest::addColumn<QByteArray>("target");
        QTest::addColumn<int>("begin"); QTest::addColumn<int>("end"); QTest::addColumn<QByteArray>("expected");
        QTest::newRow("middle-line") << QByteArray("head\nA0\nA1\nA2\ntail\n") << QByteArray("head\nB0\nB1\nB2\nB3\ntail\n") << 8 << 11 << QByteArray("head\nB0\nA1\nB2\nB3\ntail\n");
        QTest::newRow("unicode-characters") << QString("prefix 가나 suffix\n").toUtf8() << QByteArray("prefix XY suffix\n") << 7 << 13 << QString("prefix 가나 suffix\n").toUtf8();
        QTest::newRow("partial-text") << QByteArray("AAA selected ZZZ\n") << QByteArray("BBB oldvalue YYY\n") << 4 << 12 << QByteArray("BBB selected YYY\n");
        QTest::newRow("empty-target") << QByteArray("first\nsecond\nthird\n") << QByteArray{} << 6 << 13 << QByteArray("second\n");
        QTest::newRow("unequal-eof") << QByteArray("A0\nA1\nA2") << QByteArray("B0\nB1\nB2\nB3") << 6 << 8 << QByteArray("B0\nB1\nA2\nB3");
        QTest::newRow("crlf") << QByteArray("A0\r\nA1\r\nA2\r\n") << QByteArray("B0\r\nB1\r\nB2\r\n") << 4 << 8 << QByteArray("B0\r\nA1\r\nB2\r\n");
    }
    void selectedChunkRange()
    {
        QFETCH(QByteArray, source); QFETCH(QByteArray, target); QFETCH(int, begin); QFETCH(int, end); QFETCH(QByteArray, expected);
        for (bool reverse : {false, true})
        {
            QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
            write(r.file1_, reverse ? target : source); write(r.file2_, reverse ? source : target);
            CompareWindow window(r); window.resize(1300, 800); window.show();
            QTRY_VERIFY(window.findChild<ScintillaEditBase*>("FILE 2"));
            auto* from = window.findChild<ScintillaEditBase*>(reverse ? "FILE 2" : "FILE 1");
            auto* to = window.findChild<ScintillaEditBase*>(reverse ? "FILE 1" : "FILE 2");
            auto* gutter = window.findChild<ChunkGutter*>(); QVERIFY(gutter);
            from->send(SCI_SETSEL, end, begin); from->setFocus();
            const auto buttonName = QString("chunk1_action%1").arg(reverse ? 1 : 0);
            QTRY_VERIFY(gutter->findChild<QToolButton*>(buttonName) && gutter->findChild<QToolButton*>(buttonName)->isVisible());
            QTRY_VERIFY(gutter->findChild<QToolButton*>(buttonName)->toolTip().contains("선택한 텍스트"));
            QTest::mouseClick(gutter->findChild<QToolButton*>(buttonName), Qt::LeftButton);
            QCOMPARE(contents(to), expected); QCOMPARE(contents(from), source);
            QCOMPARE(readBytes(reverse ? r.file1_ : r.file2_), target); QVERIFY(!QApplication::activeModalWidget());
            to->send(SCI_UNDO); QCOMPARE(contents(to), target); to->send(SCI_REDO); QCOMPARE(contents(to), expected);
            to->send(SCI_UNDO); QTRY_VERIFY(!to->send(SCI_GETMODIFY)); window.close();
        }
    }
    void similarLineSelectionPreservesTargetInsertions()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        const QByteArray a = "head\nalpha_value = 1;\nomega_counter = 2;\ntail\n";
        const QByteArray b = "head\n// new log\nalpha_value = 9;\n// keep inserted\nomega_counter = 8;\ntail\n";
        for (bool reverse : {false, true}) for (int selection = 0; selection < 3; ++selection)
        {
            QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
            write(r.file1_, reverse ? b : a); write(r.file2_, reverse ? a : b);
            CompareWindow window(r); window.resize(1300, 800); window.show();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 2"), 10000);
            auto* from = window.findChild<ScintillaEditBase*>(reverse ? "FILE 2" : "FILE 1");
            auto* to = window.findChild<ScintillaEditBase*>(reverse ? "FILE 1" : "FILE 2");
            QCOMPARE(from->send(SCI_VISIBLEFROMDOCLINE, 1), to->send(SCI_VISIBLEFROMDOCLINE, 2));
            QCOMPARE(from->send(SCI_VISIBLEFROMDOCLINE, 2), to->send(SCI_VISIBLEFROMDOCLINE, 4));
            QByteArray expected = b; const auto digit = a.indexOf('1');
            if (selection == 0) { from->send(SCI_SETSEL, digit, digit + 1); expected.replace("alpha_value = 9", "alpha_value = 1"); }
            else if (selection == 1) { from->send(SCI_SETSEL, 5, a.indexOf("tail")); expected.replace("alpha_value = 9", "alpha_value = 1"); expected.replace("omega_counter = 8", "omega_counter = 2"); }
            else { from->send(SCI_SETSEL, 5, 5); expected = a; } // No selection still replaces the whole chunk.
            auto* gutter = window.findChild<ChunkGutter*>(); const auto buttonName = QString("chunk1_action%1").arg(reverse ? 1 : 0);
            QTRY_VERIFY(gutter->findChild<QToolButton*>(buttonName) && gutter->findChild<QToolButton*>(buttonName)->isVisible());
            QTest::mouseClick(gutter->findChild<QToolButton*>(buttonName), Qt::LeftButton); QCOMPARE(contents(to), expected);
            QCOMPARE(contents(from), a); QCOMPARE(readBytes(reverse ? r.file1_ : r.file2_), b);
            to->send(SCI_UNDO); QCOMPARE(contents(to), b); QTRY_VERIFY(!action(window, "취소")->isEnabled());
            if (!reverse && selection == 0)
            {
                // Exercise synchronous painting before the deferred chunk refresh.
                QVERIFY(!window.grab().isNull());
                QTRY_COMPARE(from->send(SCI_VISIBLEFROMDOCLINE, 1), to->send(SCI_VISIBLEFROMDOCLINE, 2));
                QTRY_VERIFY(gutter->findChild<QToolButton*>(buttonName)->isVisible());
                QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/similar-lines.png"));
            }
            window.close();
        }
    }
    void threeFileSelectedRangeUsesSharedRows()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        const QByteArray a = "head\nalpha_value = 1;\nomega_counter = 2;\ntail\n";
        const QByteArray b = "head\n// new log\nalpha_value = 9;\n// keep inserted\nomega_counter = 8;\ntail\n";
        const std::array<std::array<int, 2>, 3> pairs{{{0, 1}, {0, 2}, {1, 2}}};
        for (int pairIndex = 0; pairIndex < 3; ++pairIndex) for (bool reverse : {false, true}) for (int selection = 0; selection < 3; ++selection)
        {
            QTemporaryDir dir; Request r; r.file1_ = dir.filePath("one"); r.file2_ = dir.filePath("two"); r.file3_ = dir.filePath("three");
            const auto fromIndex = pairs[pairIndex][reverse ? 1 : 0], toIndex = pairs[pairIndex][reverse ? 0 : 1];
            const std::array<QString, 3> paths{r.file1_, r.file2_, r.file3_};
            for (int i = 0; i < 3; ++i) write(paths[i], i == toIndex ? b : a);
            CompareWindow window(r); window.resize(1400, 800); window.show();
            QTRY_VERIFY(window.findChild<ScintillaEditBase*>("FILE 3"));
            window.findChild<QComboBox*>("comparePairSelector")->setCurrentIndex(pairIndex);
            QTRY_VERIFY(window.findChild<MultiPaneAlignment*>() && window.findChild<MultiPaneAlignment*>()->Ready());
            auto* from = window.findChild<ScintillaEditBase*>(QString("FILE %1").arg(fromIndex + 1));
            auto* to = window.findChild<ScintillaEditBase*>(QString("FILE %1").arg(toIndex + 1));
            QCOMPARE(from->send(SCI_VISIBLEFROMDOCLINE, 1), to->send(SCI_VISIBLEFROMDOCLINE, 2));
            QCOMPARE(from->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, a.indexOf("alpha")), sptr_t(0));
            QCOMPARE(from->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, a.indexOf('1')), sptr_t(1));
            QCOMPARE(to->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, b.indexOf('9')), sptr_t(1));
            QByteArray expected = b;
            if (selection == 0) { const auto digit = a.indexOf('1'); from->send(SCI_SETSEL, digit, digit + 1); expected.replace("alpha_value = 9", "alpha_value = 1"); }
            else if (selection == 1) { from->send(SCI_SETSEL, 5, a.indexOf("tail")); expected.replace("alpha_value = 9", "alpha_value = 1"); expected.replace("omega_counter = 8", "omega_counter = 2"); }
            else { from->send(SCI_SETSEL, 5, 5); expected = a; }
            auto* gutter = window.findChild<ChunkGutter*>(); const auto name = QString("chunk1_action%1").arg(reverse ? 1 : 0);
            QTRY_VERIFY(gutter->findChild<QToolButton*>(name) && gutter->findChild<QToolButton*>(name)->isVisible());
            if (selection == 0)
            {
                auto* alignment = window.findChild<MultiPaneAlignment*>(); alignment->Refresh(); QVERIFY(!alignment->Ready());
                QCOMPARE(from->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, a.indexOf('1')), sptr_t(0));
                gutter->findChild<QToolButton*>(name)->click(); QCOMPARE(contents(to), b);
                QTRY_VERIFY(alignment->Ready());
                QTRY_VERIFY(gutter->findChild<QToolButton*>(name)->isVisible());
            }
            QTest::mouseClick(gutter->findChild<QToolButton*>(name), Qt::LeftButton);
            QCOMPARE(contents(to), expected); QCOMPARE(contents(from), a); QCOMPARE(readBytes(paths[toIndex]), b);
            QCOMPARE(contents(window.findChild<ScintillaEditBase*>(QString("FILE %1").arg(4 - fromIndex - toIndex))), a);
            to->send(SCI_UNDO); QCOMPARE(contents(to), b);
            to->send(SCI_REDO); QCOMPARE(contents(to), expected);
            to->send(SCI_UNDO); QCOMPARE(contents(to), b);
            QTRY_VERIFY(window.findChild<MultiPaneAlignment*>() && window.findChild<MultiPaneAlignment*>()->Ready());
            window.close();
        }
    }
    void selectedChunkRejectsUnrelatedAndRectangle()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        write(r.file1_, "A\nkeep\nC\n"); write(r.file2_, "B\nkeep\nD\n");
        CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("FILE 2"));
        auto* source = window.findChild<ScintillaEditBase*>("FILE 1"); auto* target = window.findChild<ScintillaEditBase*>("FILE 2");
        auto* gutter = window.findChild<ChunkGutter*>(); source->send(SCI_SETSEL, 0, 2);
        QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk2_action0"));
        QTRY_VERIFY(!gutter->findChild<QToolButton*>("chunk2_action0")->isEnabled());
        source->send(SCI_SETSEL, 0, 1); source->send(SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
        QVERIFY(source->send(SCI_SELECTIONISRECTANGLE));
        QTRY_VERIFY(!gutter->findChild<QToolButton*>("chunk1_action0")->isEnabled());
        QCOMPARE(contents(target), QByteArray("B\nkeep\nD\n"));
        source->send(SCI_SETSELECTIONMODE, SC_SEL_STREAM); source->send(SCI_SETSEL, 0, 0);
        QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action0")->isEnabled());
        QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
        QCOMPARE(contents(target), QByteArray("A\nkeep\nD\n")); target->send(SCI_UNDO); window.close();
    }
    void selectedResultRangesPreserveInsertionsAndHistory()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        for (int side : {0, 1})
        {
            const QByteArray a = "alpha_value = 1;\nomega_counter = 2;\n";
            const QByteArray b = "// keep leading\nalpha_value = 9;\n// keep middle\nomega_counter = 8;\n";
            QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
            write(r.file1_, side ? b : a); write(r.file2_, side ? a : b); write(r.base_, "alpha_value = 3;\nomega_counter = 4;\n");
            CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("RESULT"));
            auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* source = window.findChild<ScintillaEditBase*>(side ? "RIGHT" : "LEFT");
            MergeController* controller = nullptr; for (auto* object : window.findChildren<QObject*>()) if (auto* value = dynamic_cast<MergeController*>(object)) controller = value;
            QVERIFY(controller); QCOMPARE(controller->Model().State().size(), std::size_t(1));
            const auto initial = contents(output); const auto state = controller->Model().State().front();
            controller->ReplaceRange(1, controller->Model().Revision(), state.begin_, state.end_, b);
            QTRY_VERIFY(window.findChild<MultiPaneAlignment*>()->Ready());
            auto* base = window.findChild<ScintillaEditBase*>("BASE"); QVERIFY(base);
            QCOMPARE(base->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, contents(base).indexOf('3')), sptr_t(1));
            QCOMPARE(output->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, contents(output).indexOf('9')), sptr_t(1));
            QCOMPARE(output->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, contents(output).indexOf("alpha")), sptr_t(0));
            const auto before = contents(output); const auto states = controller->Model().State();
            QVERIFY(before.contains("// keep middle"));
            const auto revision = controller->Model().Revision();
            QVERIFY_EXCEPTION_THROWN(controller->ReplaceRanges(1, revision, {{2, 4, "x"}, {3, 5, "y"}}), Failure);
            QCOMPARE(contents(output), before); QVERIFY(controller->Model().State() == states);
            source->send(SCI_SETSEL, 0, a.size()); ChunkGutter* gutter = nullptr;
            for (auto* value : window.findChildren<ChunkGutter*>()) if (value->SourceSide() == side) gutter = value;
            QVERIFY(gutter); QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action0") && gutter->findChild<QToolButton*>("chunk1_action0")->isVisible());
            QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
            auto expected = before; expected.replace("alpha_value = 9", "alpha_value = 1"); expected.replace("omega_counter = 8", "omega_counter = 2");
            QCOMPARE(contents(output), expected); QCOMPARE(controller->Model().UnresolvedCount(), std::size_t(1));
            output->send(SCI_UNDO); QCOMPARE(contents(output), before); QVERIFY(controller->Model().State() == states);
            output->send(SCI_REDO); QCOMPARE(contents(output), expected); QCOMPARE(controller->Model().UnresolvedCount(), std::size_t(1));
            QVERIFY(!QFileInfo::exists(r.output_));
            output->send(SCI_UNDO); output->send(SCI_UNDO); QCOMPARE(contents(output), initial);
            action(window, QString::fromUtf8("전체 수동 완료"))->trigger(); action(window, QString::fromUtf8("결과 저장"))->trigger();
            QTRY_COMPARE(window.ExitCode(), 0); window.close();
        }
    }
    void selectedThreeWayRangeKeepsConflictAndUndo()
    {
        for (int side : {0, 1})
        {
            QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
            write(r.file1_, "L0\nL1\nL2\n"); write(r.file2_, "R0\nR1\nR2\n"); write(r.base_, "B0\nB1\nB2\n");
            CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("RESULT"));
            auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* source = window.findChild<ScintillaEditBase*>(side ? "RIGHT" : "LEFT");
            const auto initial = contents(output); auto expected = initial; expected.replace(3, 3, side ? "R1\n" : "L1\n");
            MergeController* controller = nullptr; for (auto* object : window.findChildren<QObject*>()) if (auto* value = dynamic_cast<MergeController*>(object)) controller = value;
            QVERIFY(controller); const auto states = controller->Model().State();
            source->send(SCI_SETSEL, 3, 6); ChunkGutter* gutter = nullptr;
            for (auto* value : window.findChildren<ChunkGutter*>()) if (value->SourceSide() == side) gutter = value;
            QVERIFY(gutter); QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action0"));
            QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
            QCOMPARE(contents(output), expected); QCOMPARE(controller->Model().UnresolvedCount(), std::size_t(1));
            QVERIFY(!QFileInfo::exists(r.output_)); output->send(SCI_UNDO); QCOMPARE(contents(output), initial); QVERIFY(controller->Model().State() == states);
            output->send(SCI_REDO); QCOMPARE(contents(output), expected); QCOMPARE(controller->Model().UnresolvedCount(), std::size_t(1));
            output->send(SCI_UNDO); action(window, QString::fromUtf8("전체 수동 완료"))->trigger();
            action(window, QString::fromUtf8("결과 저장"))->trigger(); QTRY_COMPARE(window.ExitCode(), 0); window.close();
        }
    }
    void compareInputExternalChangeAndPendingUndo()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        write(r.file1_, "a\n"); write(r.file2_, "b\n"); CompareWindow window(r); window.show();
        QTRY_VERIFY(window.findChild<ScintillaEditBase*>("FILE 1")); auto* left = window.findChild<ScintillaEditBase*>("FILE 1");
        left->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n")); left->send(SCI_UNDO); left->send(SCI_REDO);
        QCOMPARE(contents(left), QByteArray("manual\na\n"));
        action(window, QString::fromUtf8("다시 비교"))->trigger(); QCOMPARE(contents(left), QByteArray("manual\na\n")); QVERIFY(left->send(SCI_CANUNDO));
        write(r.file1_, "external\n"); window.findChild<QAction*>("saveInput1")->trigger();
        QTRY_VERIFY(window.findChild<QLabel*>("compareStatus")->text().contains(QString::fromUtf8("저장 실패")));
        QCOMPARE(contents(left), QByteArray("manual\na\n")); QVERIFY(left->send(SCI_GETMODIFY));
        QFile disk(r.file1_); QVERIFY(disk.open(QIODevice::ReadOnly)); QCOMPARE(disk.readAll(), QByteArray("external\n")); disk.close();
        left->send(SCI_UNDO); QTRY_VERIFY(!left->send(SCI_GETMODIFY)); window.close();
    }
    void compareThreeFileEditingKeepsPairAndBuffers()
    {
        QSettings{}.setValue("view/alignAllPanes", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b"); r.file3_ = dir.filePath("c");
        write(r.file1_, "a\n"); write(r.file2_, "b\n"); write(r.file3_, "c\n"); CompareWindow window(r); window.show();
        QTRY_COMPARE(window.findChildren<ScintillaEditBase*>().size(), 3);
        auto* selector = window.findChild<QComboBox*>("comparePairSelector"); QTRY_VERIFY(selector->isEnabled()); selector->setCurrentIndex(2);
        auto* gutter = window.findChild<ChunkGutter*>(); QTRY_VERIFY(gutter->findChild<QToolButton*>("chunk1_action0"));
        QTest::mouseClick(gutter->findChild<QToolButton*>("chunk1_action0"), Qt::LeftButton);
        auto* third = window.findChild<ScintillaEditBase*>("FILE 3"); QCOMPARE(contents(third), QByteArray("b\n")); QTRY_VERIFY(selector->isEnabled());
        selector->setCurrentIndex(1); QCOMPARE(contents(third), QByteArray("b\n")); QVERIFY(third->send(SCI_CANUNDO));
        third->send(SCI_UNDO); QTRY_VERIFY(!third->send(SCI_GETMODIFY)); window.close();
    }
    void encodingRecoveryAndFormatOnly()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt");
        write(r.file1_, QByteArray::fromHex("b0a10a")); write(r.file2_, QString::fromUtf8("가\n").toUtf8());
        CompareWindow window(r); window.show();
        auto* cp949 = window.findChild<QAction*>("encoding1_CP949"); QVERIFY(cp949);
        QTRY_VERIFY(cp949->isEnabled()); QTRY_COMPARE(window.ExitCode(), 2);
        cp949->trigger(); QTRY_COMPARE(window.ExitCode(), 0);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); QVERIFY(left);
        QCOMPARE(contents(left), QString::fromUtf8("가\n").toUtf8());
        QVERIFY(window.findChild<QLabel*>("format_FILE 1")->text().contains("CP949"));
        QVERIFY(window.findChild<QLabel*>("compareStatus")->text().contains(QString::fromUtf8("내용 동일")));
        window.close();
    }
    void guiRequestIsolation()
    {
        QTemporaryDir dir; write(dir.filePath("a.txt"), "a"); write(dir.filePath("b.txt"), "b");
        const auto endpoint = "hc-test-" + QUuid::createUuid().toString(QUuid::Id128); GuiRequestServer server, duplicate;
        QVERIFY(server.Listen(endpoint)); QVERIFY(!duplicate.Listen(endpoint)); QLocalSocket first, second;
        const QString token(32, 'a');
        for (auto* socket : {&first, &second})
        {
            socket->connectToServer(endpoint); QTRY_COMPARE(socket->state(), QLocalSocket::ConnectedState);
            socket->write(GuiFrame({{"version", 1}, {"token", token}, {"wait", true}, {"arguments", QJsonArray{"app", "-gui", "-file1", dir.filePath("a.txt"), "-file2", dir.filePath(socket == &first ? "b.txt" : "a.txt")}}}));
            socket->flush(); QTRY_VERIFY(socket->canReadLine()); QCOMPARE(QJsonDocument::fromJson(socket->readLine()).object()["kind"].toString(), QString("accepted"));
        }
        CompareWindow* different = nullptr; CompareWindow* same = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* window = dynamic_cast<CompareWindow*>(widget))
            (window->windowTitle().endsWith("b.txt") ? different : same) = window;
        QVERIFY(different); QVERIFY(same); QTRY_COMPARE(different->ExitCode(), 1); QTRY_COMPARE(same->ExitCode(), 0);
        QVERIFY(!action(*different, QString::fromUtf8("파일 열기"))->isEnabled());
        different->close(); QTRY_VERIFY(first.canReadLine()); QCOMPARE(QJsonDocument::fromJson(first.readLine()).object()["exitCode"].toInt(), 1);
        QVERIFY(same->isVisible()); QVERIFY(!second.canReadLine()); same->close(); QTRY_VERIFY(second.canReadLine());
        QCOMPARE(QJsonDocument::fromJson(second.readLine()).object()["exitCode"].toInt(), 0);
    }
    void guiRequestDisconnectPreservesResult()
    {
        QTemporaryDir dir; write(dir.filePath("a.txt"), "a"); write(dir.filePath("b.txt"), "b");
        const auto endpoint = "hc-test-" + QUuid::createUuid().toString(QUuid::Id128); GuiRequestServer server; QVERIFY(server.Listen(endpoint));
        QLocalSocket socket; socket.connectToServer(endpoint); QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write(GuiFrame({{"version", 1}, {"token", QString(32, 'b')}, {"wait", true}, {"arguments", QJsonArray{"app", "-gui", "-mode", "merge", "-file1", dir.filePath("a.txt"), "-file2", dir.filePath("b.txt"), "-output", dir.filePath("out.txt")}}}));
        socket.flush(); QTRY_VERIFY(socket.canReadLine()); socket.readLine(); CompareWindow* window = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* candidate = dynamic_cast<CompareWindow*>(widget)) window = candidate;
        QVERIFY(window); QTRY_VERIFY(window->findChild<ScintillaEditBase*>("RESULT"));
        action(*window, QString::fromUtf8("오른쪽 전체 선택"))->trigger(); auto* output = window->findChild<ScintillaEditBase*>("RESULT"); QCOMPARE(contents(output), QByteArray("b"));
        socket.abort(); QTest::qWait(50); QVERIFY(window->isVisible()); QCOMPARE(contents(output), QByteArray("b")); QCOMPARE(window->ExitCode(), 3);
        action(*window, QString::fromUtf8("결과 저장"))->trigger(); QTRY_COMPARE(window->ExitCode(), 0); window->close();
    }
    void guiRequestProtocolBounds()
    {
        const auto endpoint = "hc-test-" + QUuid::createUuid().toString(QUuid::Id128); GuiRequestServer server; QVERIFY(server.Listen(endpoint));
        for (const auto& data : {QByteArray(guiMessageLimit + 1, 'a'), QByteArray("{\"version\":2}\n"), QByteArray("{}\n{}\n")})
        {
            QLocalSocket socket; socket.connectToServer(endpoint); QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
            socket.write(data); socket.flush(); QTRY_VERIFY(socket.state() == QLocalSocket::UnconnectedState || socket.canReadLine());
            if (socket.canReadLine()) QCOMPARE(QJsonDocument::fromJson(socket.readLine()).object()["kind"].toString(), QString("error"));
        }
    }
    void initTestCase()
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setOrganizationName("HyperCompareTests"); QCoreApplication::setApplicationName("UI");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        auto& theme = fm::style::ThemeManager::instance(); theme.setDesign(fm::style::Design::Watercolor); theme.install(*qApp);
    }
    void init()
    {
        QSettings settings; settings.setValue("view/alignSourceRows", false);
        settings.setValue("view/alignAllPanes", false);
        settings.setValue("view/inlineHighlights", true);
        settings.setValue("view/overview", true); settings.setValue("view/synchronizeSources", true);
        settings.setValue("find/caseSensitive", false); settings.setValue("find/wholeWord", false);
    }
    void largeEmptySourceAlignment()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("empty.txt"); request.file2_ = dir.filePath("large.txt");
        QByteArray text; for (int i = 0; i < 20000; ++i) text += "inserted-" + QByteArray::number(i) + '\n';
        write(request.file1_, {}); write(request.file2_, text); QElapsedTimer elapsed; elapsed.start(); CompareWindow window(request); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QVERIFY(left); QVERIFY(right);
        QCOMPARE(contents(left), QByteArray{}); QCOMPARE(contents(right), text); QCOMPARE(left->send(GetLeadingRows), sptr_t(20000));
        left->send(SCI_SETFIRSTVISIBLELINE, 10000); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(10000));
        QVERIFY(!left->send(SCI_CANUNDO)); QVERIFY(!right->send(SCI_CANUNDO)); QCOMPARE(contents(left), QByteArray{}); qInfo("20000 inserted lines GUI ready and aligned: %lld ms", elapsed.elapsed()); window.close();
    }
    void docxParagraphViewAndCoverage()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.docx"); request.file2_ = dir.filePath("right.docx");
        write(request.file1_, test::Docx(test::Paragraph(QString::fromUtf8("공통 제목")) + test::Paragraph(QString::fromUtf8("변경 전 본문"))));
        write(request.file2_, test::Docx(test::Paragraph(QString::fromUtf8("공통 제목")) + test::Paragraph(QString::fromUtf8("변경 후 본문")) + test::Paragraph(QString::fromUtf8("추가 문단"))));
        CompareWindow window(request); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QWidget*>("docxCompareView"), 10000);
        auto* table = window.findChild<QTableView*>("docxParagraphTable"); QCOMPARE(table->model()->rowCount(), 2); QAbstractItemModelTester tester(table->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto* left = window.findChild<QPlainTextEdit*>("docxLeftText"); auto* right = window.findChild<QPlainTextEdit*>("docxRightText");
        QCOMPARE(left->toPlainText(), QString::fromUtf8("변경 전 본문")); QCOMPARE(right->toPlainText(), QString::fromUtf8("변경 후 본문")); QVERIFY(left->isReadOnly());
        table->setCurrentIndex(table->model()->index(1, 0)); QVERIFY(left->toPlainText().isEmpty()); QCOMPARE(right->toPlainText(), QString::fromUtf8("추가 문단"));
        QVERIFY(window.findChild<QLabel*>("docxCoverage")->text().contains(QString::fromUtf8("제외"))); QVERIFY(!action(window, "결과 저장")->isEnabled());
        auto* tabs = window.findChild<QTabWidget*>("docxTabs"); tabs->setCurrentIndex(1); QCOMPARE(window.findChild<QTableView*>("archiveTable")->model()->rowCount(), 4); tabs->setCurrentIndex(0);
        table->setCurrentIndex(table->model()->index(0, 0));
        action(window, "Dark")->setChecked(false); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/docx-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/docx-dark.png")); window.close();
    }
    void zipViewContentsFiltersAndThemes()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.zip"); request.file2_ = dir.filePath("right.zip");
        write(request.file1_, test::StoredZip({{"same.txt", "same"}, {"src/main.cpp", "left"}, {"left-only.txt", "left"}}));
        write(request.file2_, test::StoredZip({{"same.txt", "same"}, {"src/main.cpp", "right"}, {"right-only.txt", "right"}}));
        CompareWindow window(request); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QWidget*>("archiveCompareView"), 10000);
        auto* table = window.findChild<QTableView*>("archiveTable"); QCOMPARE(table->model()->rowCount(), 4); QAbstractItemModelTester tester(table->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto* status = window.findChild<QComboBox*>("archiveStatus"); status->setCurrentIndex(1); QCOMPARE(table->model()->rowCount(), 3);
        auto* query = window.findChild<QLineEdit*>("archiveQuery"); query->setText("main"); QCOMPARE(table->model()->rowCount(), 1); QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("내용 다름")); query->clear();
        QVERIFY(!action(window, "결과 저장")->isEnabled()); QVERIFY(window.findChildren<ScintillaEditBase*>().isEmpty());
        action(window, "Dark")->setChecked(false); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/zip-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/zip-dark.png"));
        window.close();
    }
    void pngViewOriginalRasterZoomAndThemes()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.png"); request.file2_ = dir.filePath("right.png");
        QImage left(640, 480, QImage::Format_RGBA8888); left.fill(QColor(240, 245, 250));
        { QPainter painter(&left); painter.fillRect(50, 50, 240, 180, QColor("#437ead")); painter.fillRect(350, 210, 160, 180, QColor(100, 160, 110, 160)); }
        auto right = left.copy(); { QPainter painter(&right); painter.fillRect(80, 80, 160, 100, QColor("#bd765d")); }
        QVERIFY(left.save(request.file1_, "PNG")); QVERIFY(right.save(request.file2_, "PNG"));
        CompareWindow window(request); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QWidget*>("imageCompareView"), 10000);
        auto* view = static_cast<ImageCompareView*>(window.findChild<QWidget*>("imageCompareView"));
        QCOMPARE(view->Image(0).size(), left.size()); QCOMPARE(view->Image(1).pixelColor(100, 100), right.pixelColor(100, 100));
        QVERIFY(window.findChildren<ScintillaEditBase*>().isEmpty()); QVERIFY(!action(window, "결과 저장")->isEnabled());
        auto* zoom = view->findChild<QComboBox*>("imageZoom"); zoom->setCurrentIndex(4);
        auto* area = view->findChild<QScrollArea*>("imageArea0"); QCOMPARE(area->widget()->size(), QSize(1280, 960)); QVERIFY(!area->widgetResizable());
        zoom->setCurrentIndex(0); QVERIFY(area->widgetResizable());
        action(window, "Dark")->setChecked(false); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/png-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/png-dark.png"));
        const auto before = view->Image(0).copy(); action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        auto* refreshed = static_cast<ImageCompareView*>(window.findChild<QWidget*>("imageCompareView")); QVERIFY(refreshed); QCOMPARE(refreshed->Image(0), before); window.close();
    }
    void sourceFunctionsNavigationAndPairs()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.cpp"); request.file2_ = dir.filePath("right.cpp"); request.file3_ = dir.filePath("third.cpp");
        const QByteArray a = "int alpha() { return 1; }\n", b = "int beta() { return 2; }\n", c = "int gamma() { return 3; }\n";
        write(request.file1_, a + b + c); write(request.file2_, b + c + "int alpha() { return 10; }\n"); write(request.file3_, c + a + b);
        CompareWindow window(request); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        auto* pairs = window.findChild<QComboBox*>("semanticPairs"); auto* tabs = window.findChild<QTabWidget*>("comparisonDetails");
        QTRY_COMPARE_WITH_TIMEOUT(pairs->count(), 3, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        tabs->setCurrentIndex(1); QCOMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->model()->data(table->model()->index(0, 0)).toString(), "alpha");
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이동 + 수정"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* first = window.findChild<ScintillaEditBase*>("FILE 1"); auto* second = window.findChild<ScintillaEditBase*>("FILE 2");
        QVERIFY(first); QVERIFY(second); QCOMPARE(first->send(SCI_GETSELECTIONSTART), sptr_t(0)); QCOMPARE(second->send(SCI_GETSELECTIONSTART), sptr_t(b.size() + c.size()));
        QCOMPARE(contents(first), a + b + c); QVERIFY(!first->send(SCI_CANUNDO));
        action(window, "Dark")->setChecked(false); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/functions-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/functions-dark.png"));
        pairs->setCurrentIndex(2); QCOMPARE(window.findChild<QComboBox*>("comparePairSelector")->currentIndex(), 2);
        QAbstractItemModelTester tester(table->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        window.close();
    }
    void sourceFunctionRenameNavigation()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.cpp"); request.file2_ = dir.filePath("right.cpp");
        const QByteArray body = "(int x){if(x < 0) return -x; return x * 2 + 1;}";
        write(request.file1_, "int oldName" + body); write(request.file2_, "// prefix\nint newName" + body);
        CompareWindow window(request); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 0)).toString(), QString::fromUtf8("oldName → newName"));
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정"));
        QVERIFY(window.findChild<QLabel*>("semanticCoverage")->text().contains(QString::fromUtf8("이름 변경 추정 1")));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* second = window.findChild<ScintillaEditBase*>("FILE 2"); QVERIFY(second);
        QCOMPARE(second->send(SCI_GETSELECTIONSTART), sptr_t(10)); QVERIFY(!second->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/function-rename.png")); window.close();
    }
    void sourceFunctionRenameModifiedAnnotation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp");
        const QByteArray body = "(int x){ int y = x * 2; y += x; y -= x; if (y < x) { y += x; } return y + x; }";
        auto changed = body; changed.replace("* 2", "* 3"); write(r.file1_, "int oldName" + body); write(r.file2_, "int newName" + changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정 + 본문 수정"));
        QVERIFY(table->model()->data(table->model()->index(0, 1), Qt::ToolTipRole).toString().contains("숫자 리터럴"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), "int newName" + changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/function-rename-modified.png")); window.close();
    }
    void sourcePythonFunctionNavigation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.py"); r.file2_ = dir.filePath("right.py");
        write(r.file1_, "class Box:\n    @staticmethod\n    def value():\n        return 1\n");
        const QByteArray changed = "class Box:\n    @staticmethod\n    def value():\n        return 2\n"; write(r.file2_, changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 0)).toString(), QString("Box::value"));
        QVERIFY(window.findChild<QLabel*>("semanticCoverage")->toolTip().contains(QString::fromUtf8("중첩 함수")));
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("수정"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/python-function.png")); window.close();
    }
    void sourcePythonTokenRenameAnnotation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.py"); r.file2_ = dir.filePath("right.py");
        const QByteArray body = "(x):\n    y = x * 2\n    y += x\n    y -= x\n    y *= x\n    y /= x\n    y += y\n    if y > x:\n        y += x\n    return y + x\n";
        auto changed = body; changed.replace("* 2", "/ 2"); write(r.file1_, "def oldName" + body); write(r.file2_, "def newName" + changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정 + 본문 수정"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), "def newName" + changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/python-token-rename.png")); window.close();
    }
    void sourcePythonNumericRenameAnnotation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.py"); r.file2_ = dir.filePath("right.py");
        const QByteArray body = "(x):\n    y = x * 2\n    y += x\n    y -= x\n    y *= x\n    if y > x:\n        y += x\n    return y + x\n";
        auto changed = body; changed.replace("* 2", "* 3"); write(r.file1_, "def oldName" + body); write(r.file2_, "def newName" + changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정 + 본문 수정"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), "def newName" + changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/python-numeric-rename.png")); window.close();
    }
    void sourcePythonRenameAnnotation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.py"); r.file2_ = dir.filePath("right.py");
        const QByteArray body = "(x):\n    y = x * 2\n    if y > x:\n        y += x\n    return y + x\n";
        auto changed = body; write(r.file1_, "def oldName" + body); write(r.file2_, "def newName" + changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), "def newName" + changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/python-rename.png")); window.close();
    }
    void sourceFunctionRenameTokenEditAnnotation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp");
        const QByteArray body = "(int x){ int y = x * 2; y += x; y -= x; if (y < x) { y += x; } return y + x; }";
        auto changed = body; changed.replace("* 2", "/ 2"); write(r.file1_, "int oldName" + body); write(r.file2_, "int newName" + changed);
        CompareWindow window(r); window.resize(1300, 800); window.show(); auto* table = window.findChild<QTableView*>("semanticTable");
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 1, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1);
        QCOMPARE(table->model()->data(table->model()->index(0, 1)).toString(), QString::fromUtf8("이름 변경 추정 + 본문 수정"));
        QVERIFY(table->model()->data(table->model()->index(0, 1), Qt::ToolTipRole).toString().contains("90%"));
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* right = window.findChild<ScintillaEditBase*>("FILE 2"); QCOMPARE(contents(right), "int newName" + changed); QVERIFY(!right->send(SCI_CANUNDO));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/function-rename-token-edit.png")); window.close();
    }
    void sourceFunctionsBaseAndPartialCoverage()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left.cpp"); request.file2_ = dir.filePath("right.cpp"); request.base_ = dir.filePath("base.cpp");
        write(request.file1_, "int first(){return 1;}\nint second(){return 2;}\n");
        write(request.file2_, "int first(){return 2;}\nint second(){return 2;}\n");
        write(request.base_, "int second(){return 2;}\nint first(){return 0;}\nint invalid( {");
        CompareWindow window(request); window.show(); auto* pairs = window.findChild<QComboBox*>("semanticPairs");
        QTRY_COMPARE_WITH_TIMEOUT(pairs->count(), 3, 10000); QTRY_VERIFY(!action(window, "취소")->isEnabled());
        window.findChild<QTabWidget*>("comparisonDetails")->setCurrentIndex(1); pairs->setCurrentIndex(1);
        QCOMPARE(pairs->currentText(), QString::fromUtf8("LEFT ↔ BASE"));
        QVERIFY(window.findChild<QLabel*>("semanticCoverage")->text().contains(QString::fromUtf8("문법 오류")));
        auto* table = window.findChild<QTableView*>("semanticTable"); QVERIFY(table->model()->rowCount() > 0);
        table->setCurrentIndex(table->model()->index(0, 0)); window.findChild<QPushButton*>("semanticReveal")->click();
        auto* base = window.findChild<ScintillaEditBase*>("BASE"); QVERIFY(base); QVERIFY(base->send(SCI_GETSELECTIONEND) > base->send(SCI_GETSELECTIONSTART));
        QVERIFY(!base->send(SCI_CANUNDO)); window.close();
    }
    void directoryGuiCopyOptionsInvalidateAndApply()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a", "same"); write(r.file1_ + "/a:info", "ADS"); write(r.file2_ + "/a", "same");
        auto compared = Execute(r, std::make_shared<std::atomic_bool>(false), false); DirectoryMergeDialog dialog(r, compared); dialog.show();
        dialog.PrepareTransfer(0, "a"); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QCOMPARE(dialog.Model()->Plan()["schemaVersion"].toInt(), 3);
        for (const auto& name : {"directoryCopyMetadata", "directoryCopyAcl", "directoryCopyStreams"}) { auto* check = dialog.findChild<QCheckBox*>(name); QVERIFY(check); check->setChecked(true); }
        dialog.SavePlan(dir.filePath("stale.json")); QVERIFY(!QFileInfo::exists(dir.filePath("stale.json")));
        dialog.PrepareTransfer(0, "a"); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QVERIFY2(dialog.LastResult().error_.isEmpty(), qPrintable(dialog.LastResult().error_));
        QCOMPARE(dialog.Model()->Plan()["schemaVersion"].toInt(), 4); QVERIFY(dialog.Model()->Plan()["copyPolicy"].toObject()["streams"].toBool());
        dialog.SavePlan(dir.filePath("copy.json")); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); dialog.ApplyPlan(false); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QVERIFY2(dialog.LastResult().error_.isEmpty(), qPrintable(dialog.LastResult().error_)); QFile stream(r.file2_ + "/a:info"); QVERIFY(stream.open(QIODevice::ReadOnly)); QCOMPARE(stream.readAll(), QByteArray("ADS")); stream.close();
        QVERIFY(dialog.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-copy-options.png")); dialog.close();
    }
    void directoryRenameMenuAndKeyboard()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a.txt", "left"); write(r.file2_ + "/a.txt", "right");
        auto cancel = std::make_shared<std::atomic_bool>(false); DirectoryView view; view.resize(1400, 600);
        view.SetComparison(r, Execute(r, cancel, false)); view.show(); view.activateWindow(); view.NavigateDifference(1);
        int refreshes = 0; view.levelChanged_ = [&](const QString&) { ++refreshes; };
        QVERIFY(view.findChild<QAction*>("directoryRenameLeft")->isEnabled());
        view.RenameCurrent(0, "../invalid"); QVERIFY(view.Busy()); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000); QVERIFY(!view.LastMutation().error_.isEmpty()); QCOMPARE(refreshes, 0);
        bool prompted = false;
        QTimer::singleShot(0, &view, [&] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { prompted = true; dialog->setTextValue("left-renamed.txt"); dialog->accept(); } });
        view.findChild<QAction*>("directoryRenameLeft")->trigger(); QVERIFY(prompted); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000);
        QVERIFY2(view.LastMutation().error_.isEmpty(), qPrintable(view.LastMutation().error_)); QCOMPARE(refreshes, 1); QVERIFY(QFileInfo::exists(r.file1_ + "/left-renamed.txt"));
        view.SetComparison(r, Execute(r, cancel, false)); view.NavigateDifference(1);
        auto* right = view.findChild<QTreeView*>("directoryRightTree"); right->setFocus(); QTest::qWait(30); prompted = false;
        QTimer::singleShot(0, &view, [&] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { prompted = true; dialog->setTextValue("right-renamed.txt"); dialog->accept(); } });
        QTest::keyClick(right, Qt::Key_F2); QVERIFY(prompted); QTRY_VERIFY_WITH_TIMEOUT(!view.Busy(), 10000);
        QVERIFY2(view.LastMutation().error_.isEmpty(), qPrintable(view.LastMutation().error_)); QCOMPARE(refreshes, 2); QVERIFY(QFileInfo::exists(r.file2_ + "/right-renamed.txt"));
        view.SetComparison(r, Execute(r, cancel, false)); QVERIFY(view.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-rename.png")); view.close();
    }
    void directoryGuiTransferSelection()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a", "left"); write(r.file2_ + "/keep", "preserved");
        const auto compared = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        DirectoryView view; view.resize(1300, 600); view.SetComparison(r, compared); view.show(); view.NavigateDifference(1);
        auto* copy = view.findChild<QPushButton*>("directoryCopyRight"); QVERIFY(copy); QVERIFY(copy->isEnabled()); QTest::mouseClick(copy, Qt::LeftButton);
        auto* dialog = static_cast<DirectoryMergeDialog*>(view.findChild<QDialog*>("directoryMergeDialog")); QVERIFY(dialog); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000);
        QVERIFY2(dialog->LastResult().error_.isEmpty(), qPrintable(dialog->LastResult().error_)); QCOMPARE(dialog->Model()->Plan()["transfer"].toBool(), true);
        QVERIFY(!dialog->findChild<QLineEdit*>("directoryOutput")->isEnabled()); QCOMPARE(dialog->Model()->Output(), r.file2_);
        for (int row = 0; row < dialog->Model()->rowCount(); ++row) QVERIFY(!dialog->Model()->setData(dialog->Model()->index(row, 1), "omit"));
        dialog->SavePlan(dir.filePath("transfer.json")); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000);
        dialog->ApplyPlan(false); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000); QVERIFY2(dialog->LastResult().error_.isEmpty(), qPrintable(dialog->LastResult().error_));
        QFile file(r.file2_ + "/a"); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("left")); file.close();
        QVERIFY(QFileInfo::exists(r.file2_ + "/keep")); QVERIFY(dialog->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-transfer.png")); dialog->close(); view.close();
    }
    void directoryGuiPlanChoicesSaveApply()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a", "left"); write(r.file2_ + "/a", "right");
        const auto cancel = std::make_shared<std::atomic_bool>(false); const auto compared = Execute(r, cancel, false);
        DirectoryMergeDialog dialog(r, compared); dialog.show(); QAbstractItemModelTester tester(dialog.Model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        dialog.Prepare(dir.filePath("out")); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QVERIFY2(dialog.LastResult().error_.isEmpty(), qPrintable(dialog.LastResult().error_)); QCOMPARE(dialog.Model()->Unresolved(), 1);
        const auto choice = dialog.Model()->index(0, 1); QVERIFY(!dialog.Model()->setData(choice, "base"));
        auto* table = dialog.findChild<QTableView*>("directoryPlanTable"); table->setCurrentIndex(choice); table->edit(choice);
        QTRY_VERIFY(table->findChild<QComboBox*>()); auto* editor = table->findChild<QComboBox*>();
        editor->setCurrentIndex(editor->findData("right")); QTest::keyClick(editor, Qt::Key_Return); QTRY_COMPARE(dialog.Model()->Unresolved(), 0);
        fm::style::ThemeManager::instance().setScheme(fm::style::ThemeManager::Scheme::Light);
        QTest::qWait(40); QVERIFY(dialog.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-plan-light.png"));
        fm::style::ThemeManager::instance().setScheme(fm::style::ThemeManager::Scheme::Dark);
        QTest::qWait(40); QVERIFY(dialog.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-plan-dark.png"));
        const auto plan = dir.filePath("plan.json"); dialog.SavePlan(plan); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QVERIFY2(dialog.LastResult().error_.isEmpty(), qPrintable(dialog.LastResult().error_)); QVERIFY(!QFileInfo::exists(dir.filePath("out")));
        dialog.ApplyPlan(false); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QVERIFY2(dialog.LastResult().error_.isEmpty(), qPrintable(dialog.LastResult().error_)); QCOMPARE(dialog.LastResult().commit_, QString("Committed"));
        QFile file(dir.filePath("out/a")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("right")); file.close();
        dialog.ApplyPlan(false, true); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QCOMPARE(dialog.LastResult().exitCode_, 0);
        // External changes to a saved plan cannot bypass the choices reviewed in the GUI.
        QFile stored(plan); QVERIFY(stored.open(QIODevice::ReadOnly)); auto document = QJsonDocument::fromJson(stored.readAll()).object(); stored.close();
        auto actions = document["actions"].toArray(); auto item = actions[0].toObject(); item["choice"] = "left"; actions[0] = item; document["actions"] = actions;
        write(plan, QJsonDocument(document).toJson()); dialog.ApplyPlan(false, true); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QCOMPARE(dialog.LastResult().error_, QString("DirectoryPlanChanged")); dialog.close();
    }
    void directoryGuiPlanFailureAndChoiceInvalidation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("l"); r.file2_ = dir.filePath("r");
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a", "left");
        const auto compared = Execute(r, std::make_shared<std::atomic_bool>(false), false); DirectoryMergeDialog dialog(r, compared); dialog.show();
        dialog.Prepare(r.file1_ + "/bad"); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QVERIFY(!dialog.Model()->HasPlan());
        QCOMPARE(dialog.LastResult().error_, QString("DirectoryOutputOverlapsInput"));
        dialog.Prepare(dir.filePath("out")); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        const auto index = dialog.Model()->index(0, 1); QVERIFY(dialog.Model()->setData(index, "left"));
        dialog.SavePlan(dir.filePath("plan.json")); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        auto* output = dialog.findChild<QLineEdit*>("directoryOutput"); output->setText(dir.filePath("different-output"));
        dialog.ApplyPlan(true); QVERIFY(!dialog.Busy()); QVERIFY(!QFileInfo::exists(dir.filePath("out"))); output->setText(dir.filePath("out"));
        QVERIFY(dialog.Model()->setData(index, "omit")); dialog.ApplyPlan(true); QVERIFY(!dialog.Busy()); QVERIFY(!QFileInfo::exists(dir.filePath("out")));
        dialog.SavePlan(dir.filePath("changed.json")); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        write(r.file1_ + "/a", "changed input"); dialog.ApplyPlan(true); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QCOMPARE(dialog.LastResult().error_, QString("DirectorySnapshotMismatch")); QVERIFY(!QFileInfo::exists(dir.filePath("out"))); dialog.close();
    }
    void saveRecoveryDialogPreviewRestoreAndMenu()
    {
        QTemporaryDir dir; QString journal; recoveryFixture(dir.path(), journal); QVERIFY(!journal.isEmpty());
        Request request; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right"); request.theme_ = "light";
        write(request.file1_, "left\n"); write(request.file2_, "right\n"); CompareWindow window(request); window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<ScintillaEditBase*>().size(), 2, 10000);
        auto* editor = window.findChild<ScintillaEditBase*>("FILE 1"); QVERIFY(editor);
        editor->setFocus(); editor->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("draft-"));
        QTRY_COMPARE(contents(editor), QByteArray("draft-left\n"));
        auto* menuAction = window.findChild<QAction*>("openSaveRecovery"); QVERIFY(menuAction); QTRY_VERIFY(menuAction->isEnabled()); menuAction->trigger();
        auto* dialog = static_cast<SaveRecoveryDialog*>(window.findChild<QDialog*>("saveRecoveryDialog")); QVERIFY(dialog); QPointer<QDialog> lifetime(dialog);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000); auto* apply = dialog->findChild<QPushButton*>("recoveryApply"); QVERIFY(!apply->isEnabled());
        dialog->OpenJournal(journal); QVERIFY(dialog->Busy()); QVERIFY(!apply->isEnabled()); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000);
        QCOMPARE(dialog->LastResult().outcome_, QString("RecoveryPreview")); QVERIFY(apply->isEnabled()); QVERIFY(!QFileInfo::exists(dir.filePath("destination.txt")));
        QCOMPARE(dialog->findChild<QLineEdit*>("recoveryDestination")->text(), dir.filePath("destination.txt"));
        const auto stage = dialog->LastResult().saveRecovery_["stage"].toString(); QVERIFY(QFileInfo::exists(stage));
        QTest::qWait(40); QVERIFY(dialog->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/recovery-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(dialog->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/recovery-dark.png"));
        QTest::mouseClick(apply, Qt::LeftButton); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000);
        QVERIFY2(dialog->LastResult().error_.isEmpty(), qPrintable(dialog->LastResult().error_)); QCOMPARE(readBytes(dir.filePath("destination.txt")), QByteArray("original\n"));
        QVERIFY(dialog->LastResult().saveRecovery_["identitiesVerified"].toBool()); QVERIFY(QFileInfo::exists(journal)); QVERIFY(QFileInfo::exists(stage)); QVERIFY(!apply->isEnabled());
        dialog->OpenJournal(journal); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000); dialog->Recover(); QTRY_VERIFY_WITH_TIMEOUT(!dialog->Busy(), 10000);
        QCOMPARE(readBytes(dir.filePath("destination.txt")), QByteArray("original\n")); QVERIFY(dialog->LastResult().error_.isEmpty());
        dialog->close(); QTRY_VERIFY(lifetime.isNull()); QCOMPARE(contents(editor), QByteArray("draft-left\n")); QCOMPARE(readBytes(request.file1_), QByteArray("left\n"));
        editor->setFocus(); action(window, "실행 취소")->trigger(); QTRY_COMPARE(contents(editor), QByteArray("left\n")); window.close();
    }
    void saveRecoveryDialogRejectsChangedRecordAndDestination()
    {
        QTemporaryDir dir; QString journal; recoveryFixture(dir.path(), journal); QVERIFY(!journal.isEmpty());
        SaveRecoveryDialog dialog({}); dialog.show(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        dialog.OpenJournal(journal); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QCOMPARE(dialog.LastResult().outcome_, QString("RecoveryPreview"));
        const auto bytes = readBytes(journal); write(journal, bytes + "{torn");
        dialog.Recover(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QCOMPARE(dialog.LastResult().error_, QString("SaveRecoveryPreviewChanged"));
        QVERIFY(!QFileInfo::exists(dir.filePath("destination.txt"))); QVERIFY(!dialog.findChild<QPushButton*>("recoveryApply")->isEnabled());
        dialog.OpenJournal(journal); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QVERIFY(dialog.LastResult().warnings_.contains("SaveRecoveryIgnoredTornTail"));
        write(dir.filePath("destination.txt"), "foreign\n"); dialog.Recover(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QCOMPARE(dialog.LastResult().error_, QString("write-recovery-destination-changed")); QCOMPARE(readBytes(dir.filePath("destination.txt")), QByteArray("foreign\n"));
        write(journal, bytes + "bad complete record\n"); dialog.OpenJournal(journal); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        QCOMPARE(dialog.LastResult().error_, QString("SaveRecoveryCorrupt")); QVERIFY(!dialog.findChild<QPushButton*>("recoveryApply")->isEnabled()); dialog.close();
        write(journal, bytes); Request protectedContext; protectedContext.protectedInputs_ = {dir.filePath("destination.txt")}; SaveRecoveryDialog protectedDialog(protectedContext);
        QTRY_VERIFY_WITH_TIMEOUT(!protectedDialog.Busy(), 10000); protectedDialog.OpenJournal(journal); QTRY_VERIFY_WITH_TIMEOUT(!protectedDialog.Busy(), 10000);
        QCOMPARE(protectedDialog.LastResult().error_, QString("RequestArtifactAlias")); QVERIFY(!protectedDialog.findChild<QPushButton*>("recoveryApply")->isEnabled());
        QCOMPARE(readBytes(dir.filePath("destination.txt")), QByteArray("foreign\n"));
    }
    void saveRecoveryDialogCloseDuringPreview()
    {
        QTemporaryDir dir; QString journal; recoveryFixture(dir.path(), journal); QVERIFY(!journal.isEmpty());
        SaveRecoveryDialog dialog({}); dialog.show(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000);
        dialog.OpenJournal(journal); QVERIFY(dialog.Busy()); dialog.close();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.Busy(), 10000); QTRY_VERIFY(!dialog.isVisible());
        QVERIFY(!QFileInfo::exists(dir.filePath("destination.txt"))); QVERIFY(QFileInfo::exists(journal));
    }
    void directoryTreeModelContractsAndFiltering()
    {
        DirectoryTreeModel model; QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        DirectoryTreeFilter filter(nullptr); filter.setSourceModel(&model);
        QAbstractItemModelTester proxyTester(&filter, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto side = [](QString Path, bool Directory = false) { return QJsonObject{{"path", Path}, {"present", true}, {"directory", Directory}, {"size", "12"}}; };
        QJsonArray entries{
            QJsonObject{{"path", "Src/deep/한글.cpp"}, {"status", "Conflict"}, {"left", side("Src/deep/한글.cpp")}, {"right", side("src/deep/한글.cpp")}},
            QJsonObject{{"path", "src"}, {"status", "NameDifferent"}, {"left", side("Src", true)}, {"right", side("src", true)}},
            QJsonObject{{"path", "src/deep"}, {"status", "TypeConflict"}, {"left", side("Src/deep", true)}, {"right", side("src/deep")}},
            QJsonObject{{"path", "only.txt"}, {"status", "LeftOnly"}, {"left", side("only.txt")}},
            QJsonObject{{"path", "same.txt"}, {"status", "Equal"}, {"left", side("same.txt")}, {"right", side("same.txt")}}
        };
        model.SetEntries(entries); QCOMPARE(model.rowCount(), 3);
        const auto src = model.ForEntry(1), deep = model.ForEntry(2), child = model.ForEntry(0);
        QCOMPARE(model.parent(child), deep); QCOMPARE(model.parent(deep), src); QCOMPARE(model.rowCount(src), 1);
        QCOMPARE(src.data().toString(), QString("Src")); QCOMPARE(src.siblingAtColumn(3).data().toString(), QString("src"));
        QCOMPARE(model.ForEntry(3).siblingAtColumn(4).data().toString(), QString("없음"));
        QCOMPARE(deep.siblingAtColumn(2).data().toString(), QString("폴더")); QCOMPARE(deep.siblingAtColumn(5).data().toString(), QString("12 B"));
        filter.SetQuery("한글", 2); QCOMPARE(filter.rowCount(), 1); QCOMPARE(filter.rowCount(filter.index(0, 0)), 1);
        QVERIFY(filter.mapFromSource(child).isValid()); QCOMPARE(model.VisibleEntries(), 1);
        QCOMPARE(model.NextDifference({}, 1), child); QCOMPARE(model.NextDifference(child, -1), child);
        filter.SetQuery("missing", 0); QCOMPARE(filter.rowCount(), 0); QVERIFY(!model.NextDifference({}, 1).isValid());
        filter.SetQuery({}, 0); model.SetEntries({}); QCOMPARE(filter.rowCount(), 0);
    }
    void directoryTreesSynchronizeAndOpenFiles()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.theme_ = "light";
        for (const auto& root : {r.file1_, r.file2_, r.base_}) { QVERIFY(QDir{}.mkpath(root + "/src/nested")); write(root + "/same.txt", "same"); }
        for (int i = 0; i < 100; ++i)
        {
            const auto name = QString("/src/nested/file%1.cpp").arg(i, 3, 10, QChar('0'));
            write(r.file1_ + name, "left"); write(r.file2_ + name, "right"); write(r.base_ + name, "base");
        }
        write(r.file1_ + "/only-left.txt", "left"); write(r.file2_ + "/only-right.txt", "right");
        CompareWindow window(r); window.resize(1350, 800); window.show();
        auto* left = window.findChild<QTreeView*>("directoryLeftTree"); auto* right = window.findChild<QTreeView*>("directoryRightTree");
        QTRY_VERIFY_WITH_TIMEOUT(left->isVisible() && left->model()->rowCount() == 4 && window.findChild<QLabel*>("compareStatus")->text().startsWith("디렉터리 비교"), 10000);
        QCOMPARE(left->model(), right->model()); auto* model = left->model();
        const auto src = model->index(0, 0), nested = model->index(0, 0, src);
        QCOMPARE(src.data().toString(), QString("src"));
        left->collapse(src); QTRY_VERIFY(!right->isExpanded(src)); right->expand(src); QTRY_VERIFY(left->isExpanded(src));
        right->expand(nested); QTRY_VERIFY(left->isExpanded(nested));
        const auto target = model->index(50, 3, nested); right->setCurrentIndex(target); right->scrollTo(target);
        QTRY_COMPARE(left->currentIndex().data(DirectoryTreeModel::PathRole), target.data(DirectoryTreeModel::PathRole));
        QTRY_COMPARE(left->verticalScrollBar()->value(), right->verticalScrollBar()->value());
        QCOMPARE(left->visualRect(target.siblingAtColumn(0)).y(), right->visualRect(target).y());
        right->setFocus(); QTest::keyClick(right, Qt::Key_Down);
        QTRY_COMPARE(left->currentIndex().data(DirectoryTreeModel::PathRole), right->currentIndex().data(DirectoryTreeModel::PathRole));
        auto* view = static_cast<DirectoryView*>(window.findChild<QWidget*>("directoryView")); Request child; view->open_ = [&](Request Input) { child = std::move(Input); };
        auto* open = window.findChild<QPushButton*>("directoryOpenFile"); QVERIFY(open->isEnabled()); QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(child.file1_, r.file1_ + "/src/nested/file051.cpp"); QCOMPARE(child.base_, r.base_ + "/src/nested/file051.cpp");
        child = {}; right->setFocus(); QTest::keyClick(right, Qt::Key_Return); QCOMPARE(child.file1_, r.file1_ + "/src/nested/file051.cpp");
        auto* query = window.findChild<QLineEdit*>("directoryQuery"); query->setText("only-left"); view->NavigateDifference(1); QVERIFY(open->isEnabled());
        QTest::mouseClick(open, Qt::LeftButton); QVERIFY(child.allowMissingInputs_[1]); QCOMPARE(child.file2_, r.file2_ + "/only-left.txt");
        QCOMPARE(right->currentIndex().siblingAtColumn(4).data().toString(), QString("없음"));
        query->clear(); left->collapseAll(); view->NavigateDifference(1); QVERIFY(left->currentIndex().isValid());
        query->setText("file099"); auto* status = window.findChild<QComboBox*>("directoryStatusFilter"); status->setCurrentIndex(2);
        view->NavigateDifference(1); QCOMPARE(left->currentIndex().data().toString(), QString("file099.cpp"));
        QVERIFY(left->isExpanded(left->currentIndex().parent())); QVERIFY(right->isExpanded(right->currentIndex().parent()));
        auto* mode = window.findChild<QComboBox*>("directoryViewMode"); mode->setCurrentIndex(1);
        auto* table = window.findChild<QTableView*>("directoryTable"); QCOMPARE(table->currentIndex().data().toString(), QString("src/nested/file099.cpp"));
        mode->setCurrentIndex(0); QCOMPARE(left->currentIndex().data().toString(), QString("file099.cpp"));
        query->clear(); status->setCurrentIndex(0); left->collapse(model->index(0, 0, model->index(0, 0))); left->verticalScrollBar()->setValue(0);
        QTest::qWait(50); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-tree-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-tree-dark.png")); window.close();
    }
    void directoryOneSidedFileMergesIntoEmptyDocument()
    {
        for (int missing = 0; missing < 2; ++missing)
        {
            QTemporaryDir dir; Request r; r.mode_ = "directory"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
            QVERIFY(QDir{}.mkpath(r.file1_)); QVERIFY(QDir{}.mkpath(r.file2_));
            const auto source = (missing ? r.file1_ : r.file2_) + "/nested"; QVERIFY(QDir{}.mkpath(source)); write(source + "/new.txt", "whole file\n");
            const auto comparison = Execute(r, std::make_shared<std::atomic_bool>(false), false); QCOMPARE(comparison.completion_, QString("Complete"));
            DirectoryView view; view.SetComparison(r, comparison); view.show(); Request child;
            view.open_ = [&](Request input) { child = std::move(input); };
            view.findChild<QLineEdit*>("directoryQuery")->setText("new.txt"); view.NavigateDifference(1); view.OpenCurrent();
            QVERIFY(child.allowMissingInputs_[missing]); const auto destination = missing ? child.file2_ : child.file1_;
            QVERIFY(!QFileInfo::exists(destination));
            CompareWindow window(child); window.resize(1100, 650); window.show(); QTRY_VERIFY(window.findChild<ChunkGutter*>());
            auto* target = window.findChild<ScintillaEditBase*>(missing ? "FILE 2" : "FILE 1"); QVERIFY(target); QCOMPARE(contents(target), QByteArray{});
            auto* gutter = window.findChild<ChunkGutter*>(); const auto name = QString("chunk1_action%1").arg(1 - missing);
            QTRY_VERIFY(gutter->findChild<QToolButton*>(name)); QTRY_VERIFY(gutter->findChild<QToolButton*>(name)->isVisible());
            QTest::mouseClick(gutter->findChild<QToolButton*>(name), Qt::LeftButton); QCOMPARE(contents(target), QByteArray("whole file\n"));
            QVERIFY(!QFileInfo::exists(destination)); target->send(SCI_UNDO); QCOMPARE(contents(target), QByteArray{}); target->send(SCI_REDO);
            auto* save = window.findChild<QAction*>(QString("saveInput%1").arg(missing + 1)); QTRY_VERIFY(save->isEnabled()); save->trigger();
            QTRY_VERIFY(QFileInfo::exists(destination)); QTRY_COMPARE(target->send(SCI_GETMODIFY), 0);
            QFile created(destination); QVERIFY(created.open(QIODevice::ReadOnly)); QCOMPARE(created.readAll(), QByteArray("whole file\n"));
            QTRY_COMPARE(window.ExitCode(), 0); window.close(); view.close();
        }
    }
    void extendedFindAcrossPanelsAndRegex()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b");
        write(r.file1_, QString("가 123\nleft 45\n").toUtf8()); write(r.file2_, "right 67\n");
        CompareWindow window(r); window.resize(1400, 850); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("FILE 2"));
        auto* first = window.findChild<ScintillaEditBase*>("FILE 1"); auto* second = window.findChild<ScintillaEditBase*>("FILE 2");
        auto* bar = window.findChild<FindBar*>(); auto* query = bar->findChild<QLineEdit*>("compareFindText"); auto* scope = bar->findChild<QComboBox*>("compareFindScope");
        auto* regex = bar->findChild<QCheckBox*>("compareFindRegex"); auto* feedback = bar->findChild<QLabel*>("compareFindFeedback"); auto* rows = window.findChild<QTreeWidget*>("compareFindResults");
        bar->Open(); scope->setCurrentIndex(2); regex->setChecked(true); query->setText("\\d+"); bar->FindAll();
        QTRY_COMPARE_WITH_TIMEOUT(feedback->text(), QString("3건 찾음"), 10000); QCOMPARE(rows->topLevelItemCount(), 3);
        QCOMPARE(rows->topLevelItem(0)->data(0, Qt::UserRole).toLongLong(), qint64(4)); QCOMPARE(rows->topLevelItem(2)->text(3), QString("FILE 2"));
        rows->setCurrentItem(rows->topLevelItem(2)); QTest::keyClick(rows, Qt::Key_Return); QTRY_COMPARE(second->send(SCI_GETSELECTIONSTART), sptr_t(6));
        QCOMPARE(scope->currentIndex(), 2); QCOMPARE(rows->topLevelItemCount(), 3);
        query->setText("["); bar->FindAll(); QTRY_VERIFY_WITH_TIMEOUT(feedback->text().contains("InvalidSearchExpression"), 10000);
        scope->setCurrentIndex(0); first->send(SCI_SETSEL, 0, 0); query->setText("^"); bar->Find(false);
        QTRY_COMPARE_WITH_TIMEOUT(feedback->text(), QString("찾음"), 10000); QCOMPARE(first->send(SCI_GETSELECTIONSTART), sptr_t(0));
        bar->Find(false); QTRY_VERIFY_WITH_TIMEOUT(first->send(SCI_GETSELECTIONSTART) > 0, 10000);
        regex->setChecked(false); query->setText("right"); scope->setCurrentIndex(2); bar->FindAll();
        QTRY_COMPARE_WITH_TIMEOUT(feedback->text(), QString("1건 찾음"), 10000);
        QVERIFY(!first->send(SCI_GETMODIFY)); QVERIFY(!second->send(SCI_GETMODIFY));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/extended-search.png")); window.close();
    }
    void directoryTextPolicySwitchAndFilter()
    {
        QTemporaryDir dir; Request r; r.mode_ = "directory"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.directory_.level_ = "text";
        QVERIFY(QDir().mkpath(r.file1_)); QVERIFY(QDir().mkpath(r.file2_)); write(r.file1_ + "/a.txt", "same\n"); write(r.file2_ + "/a.txt", QByteArray::fromHex("efbbbf") + "same\n");
        CompareWindow window(r); window.resize(1400, 850); window.show();
        auto* tree = window.findChild<QTreeView*>("directoryLeftTree"); auto* level = window.findChild<QComboBox*>("directoryComparisonLevel");
        QTRY_COMPARE(window.ExitCode(), 0); QTRY_VERIFY(level->isEnabled()); QCOMPARE(level->currentData().toString(), QString("text"));
        QVERIFY(tree->model()->index(0, 1).data().toString().contains("인코딩/BOM"));
        auto* filter = window.findChild<QComboBox*>("directoryStatusFilter"); filter->setCurrentIndex(1); QCOMPARE(tree->model()->rowCount(), 0);
        filter->setCurrentIndex(0); level->setCurrentIndex(level->findData("bytes")); QTRY_COMPARE(window.ExitCode(), 1);
        QTRY_VERIFY(level->isEnabled()); QCOMPARE(tree->model()->index(0, 1).data().toString(), QString("다름"));
        level->setCurrentIndex(level->findData("text")); QTRY_COMPARE(window.ExitCode(), 0); QTRY_VERIFY(level->isEnabled());
        QVERIFY(window.findChild<QAction*>("encoding2_CP949")->isEnabled());
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-text-policy.png")); window.close();
    }
    void directoryWindowKeepsStreamedModelAtCompletion()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right");
        QVERIFY(QDir().mkpath(request.file1_ + "/src")); QVERIFY(QDir().mkpath(request.file2_ + "/src"));
        for (int i = 0; i < 2000; ++i)
        { const auto name = QString("/src/file%1.txt").arg(i, 5, 10, QChar('0')); write(request.file1_ + name, "left"); write(request.file2_ + name, "right"); }
        CompareWindow window(request); window.show(); auto* tree = window.findChild<QTreeView*>("directoryLeftTree");
        auto* status = window.findChild<QLabel*>("compareStatus");
        QTRY_VERIFY_WITH_TIMEOUT(tree->isVisible(), 10000); QVERIFY(status->text().contains("중"));
        auto* model = static_cast<DirectoryTreeModel*>(window.findChild<QAbstractItemModel*>("directoryTreeModel"));
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        const QPersistentModelIndex selected(tree->model()->index(0, 0)); tree->setCurrentIndex(selected); tree->expand(selected);
        QTRY_VERIFY_WITH_TIMEOUT(status->text().startsWith("디렉터리 비교"), 15000);
        QCOMPARE(resets.size(), 0); QVERIFY(selected.isValid()); QCOMPARE(tree->currentIndex(), QModelIndex(selected)); QVERIFY(tree->isExpanded(selected));
        QCOMPARE(model->VisibleEntries(), 2001); window.close();
    }
    void directoryStreamingCompletionNavigationOrder()
    {
        DirectoryTreeModel model; QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        DirectoryProgress batch;
        for (const auto& path : {QString("a"), QString("b"), QString("b/y"), QString("a/x")})
        {
            const bool folder = !path.contains('/'); batch.rows_.append(QJsonObject{{"path", path}, {"status", folder ? "Equal" : "Different"},
                {"left", QJsonObject{{"present", true}, {"directory", folder}}}, {"right", QJsonObject{{"present", true}, {"directory", folder}}}});
        }
        model.ApplyProgress(batch); model.FinishProgress();
        const auto first = model.NextDifference({}, 1); QCOMPARE(first.data(DirectoryTreeModel::PathRole).toString(), QString("a/x"));
        const auto second = model.NextDifference(first, 1); QCOMPARE(second.data(DirectoryTreeModel::PathRole).toString(), QString("b/y"));
        QCOMPARE(model.NextDifference(second, -1), first);
    }
    void directoryStreamingInteractionLatency()
    {
        DirectoryView view; view.resize(1300, 700); Request request; view.BeginComparison(request); view.show();
        auto* tree = view.findChild<QTreeView*>("directoryLeftTree"); auto* source = static_cast<DirectoryTreeModel*>(view.findChild<QAbstractItemModel*>("directoryTreeModel"));
        QVector<qint64> cycles; QElapsedTimer total; total.start();
        for (int phase = 0; phase < 2; ++phase) for (int offset = 0; offset < 32768; offset += 512)
        {
            DirectoryProgress batch; batch.side_ = phase == 0 ? 0 : -1; batch.phase_ = phase == 0 ? "scan" : "compare";
            for (int i = offset; i < offset + 512; ++i)
            {
                DirectoryRow row; row.path_ = QString("src/file%1.cpp").arg(i, 6, 10, QChar('0')); row.statuses_[0] = phase == 0 ? 18 : 2;
                row.sides_[0].present_ = true; row.sides_[0].path_ = row.path_; row.sides_[0].size_ = 1024; batch.rows_.append(row);
            }
            QSignalSpy inserted(source, &QAbstractItemModel::rowsInserted), updated(source, &QAbstractItemModel::dataChanged);
            QElapsedTimer cycle; cycle.start(); view.ApplyProgress(batch);
            QVERIFY(inserted.size() <= 2); QVERIFY(updated.size() <= 2);
            const auto root = tree->model()->index(0, 0); tree->expand(root);
            tree->verticalScrollBar()->setValue(offset % 2 ? 0 : tree->verticalScrollBar()->maximum());
            QCoreApplication::processEvents(); cycles.append(cycle.elapsed());
        }
        std::sort(cycles.begin(), cycles.end());
        qInfo("Streaming 32768 expanded siblings: total %lld ms, cycle p95 %lld ms, max %lld ms", total.elapsed(), cycles[cycles.size() * 95 / 100], cycles.back());
        QCOMPARE(source->VisibleEntries(), 32768); QCOMPARE(tree->model()->rowCount(tree->model()->index(0, 0)), 32768);
        tree->setFocus(); QTest::keyClick(tree, Qt::Key_End); QVERIFY(tree->currentIndex().isValid());
        const QPersistentModelIndex selected(tree->currentIndex()); const auto selectedPath = selected.data(DirectoryTreeModel::PathRole).toString();
        QSignalSpy resets(source, &QAbstractItemModel::modelReset); Result completed; completed.entries_ = source->Entries(); completed.directoryComplete_ = true; completed.completion_ = "Complete";
        QElapsedTimer finish; finish.start(); view.SetComparison(request, completed); QCoreApplication::processEvents();
        qInfo("Streaming completion reusing 32768 entries: %lld ms", finish.elapsed()); QCOMPARE(resets.size(), 0); QVERIFY(selected.isValid()); QCOMPARE(tree->currentIndex(), QModelIndex(selected));
        auto* table = view.findChild<QTableView*>("directoryTable"); QCOMPARE(table->model()->rowCount(), 0);
        view.findChild<QComboBox*>("directoryViewMode")->setCurrentIndex(1); QCOMPARE(table->model()->rowCount(), 32768);
        QCOMPARE(table->currentIndex().data().toString(), selectedPath); view.findChild<QComboBox*>("directoryViewMode")->setCurrentIndex(0);
        QCOMPARE(tree->currentIndex().data(DirectoryTreeModel::PathRole).toString(), selectedPath); view.close();
    }
    void directoryLargeWorkspaceAcceptance()
    {
        const auto leftPath = qEnvironmentVariable("HC_COMPARE_LARGE_LEFT"), rightPath = qEnvironmentVariable("HC_COMPARE_LARGE_RIGHT");
        if (leftPath.isEmpty() || rightPath.isEmpty()) QSKIP("Set HC_COMPARE_LARGE_LEFT/RIGHT for the local large-directory acceptance test");
        Request r; r.mode_ = "directory"; r.file1_ = leftPath; r.file2_ = rightPath; r.timeoutMs_ = 0; r.directory_.level_ = qEnvironmentVariable("HC_COMPARE_LARGE_LEVEL", "bytes");
        std::uint64_t peakPrivate = 0; QTimer sampler;
        connect(&sampler, &QTimer::timeout, &sampler, [&] { PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) peakPrivate = std::max<std::uint64_t>(peakPrivate, counters.PrivateUsage); });
        sampler.start(100);
        QElapsedTimer elapsed; elapsed.start(); CompareWindow window(r); window.resize(1400, 850); window.show();
        auto* tree = window.findChild<QTreeView*>("directoryLeftTree");
        QVector<qint64> eventGaps; QElapsedTimer heartbeat; heartbeat.start(); QTimer interaction;
        bool navigationReady = false;
        int ticks = 0, scrollChanges = 0; connect(&interaction, &QTimer::timeout, &interaction, [&] {
            eventGaps.append(heartbeat.restart());
            if (navigationReady && tree->isVisible() && ++ticks % 10 == 0)
            {
                // Exercise additional branches while comparison remains active.
                for (int row = 0; row < std::min(3, tree->model()->rowCount()); ++row)
                {
                    const auto root = tree->model()->index(row, 0); if (tree->model()->rowCount(root)) tree->expand(root);
                    for (int child = 0; child < std::min(3, tree->model()->rowCount(root)); ++child)
                    { const auto branch = tree->model()->index(child, 0, root); if (tree->model()->rowCount(branch)) tree->expand(branch); }
                }
                auto* bar = tree->verticalScrollBar(); const auto before = bar->value();
                bar->setValue(ticks % 20 == 0 ? 0 : bar->maximum()); scrollChanges += bar->value() != before;
            }
        }); interaction.start(20);
        QTest::qWait(100);
        QTRY_VERIFY_WITH_TIMEOUT(tree->isVisible() || window.ExitCode() == 2, 30000);
        QVERIFY2(tree->isVisible(), qPrintable(window.findChild<QLabel*>("compareStatus")->text()));
        QVERIFY(tree->isEnabled());
        auto* streamedModel = window.findChild<QAbstractItemModel*>("directoryTreeModel"); QSignalSpy resets(streamedModel, &QAbstractItemModel::modelReset);
        if (QDir(leftPath).exists("src/qtbase/src/corelib"))
        {
            auto* source = static_cast<DirectoryTreeModel*>(streamedModel);
            auto* proxy = static_cast<QSortFilterProxyModel*>(tree->model());
            for (const auto& path : {QString("src"), QString("src/qtbase"), QString("src/qtbase/src"), QString("src/qtbase/src/corelib")})
            {
                QTRY_VERIFY_WITH_TIMEOUT(source->ForPath(path).isValid(), 20000);
                const auto index = proxy->mapFromSource(source->ForPath(path)); QVERIFY(index.isValid());
                tree->scrollTo(index); tree->setCurrentIndex(index); tree->expand(index);
                QVERIFY(tree->isExpanded(index)); QVERIFY(action(window, "취소")->isEnabled());
                qInfo("Interactive Qt branch %s at %lld ms", qPrintable(path), elapsed.elapsed());
            }
            const QString filePath = "src/qtbase/src/corelib/CMakeLists.txt";
            QTRY_VERIFY_WITH_TIMEOUT(source->ForPath(filePath).isValid(), 20000);
            const auto fileIndex = proxy->mapFromSource(source->ForPath(filePath)); tree->setCurrentIndex(fileIndex);
            QCOMPARE(source->Entry(source->ForPath(filePath))["status"].toString(), QString("Pending"));
            const auto existing = QApplication::topLevelWidgets();
            QVERIFY(QMetaObject::invokeMethod(tree, "activated", Qt::DirectConnection, Q_ARG(QModelIndex, fileIndex)));
            CompareWindow* opened = nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) if (!existing.contains(widget))
                if (auto* candidate = dynamic_cast<CompareWindow*>(widget)) { opened = candidate; break; }
            QVERIFY(opened);
            QTRY_VERIFY_WITH_TIMEOUT(opened->findChild<ScintillaEditBase*>("FILE 1"), 20000);
            QVERIFY(!contents(opened->findChild<ScintillaEditBase*>("FILE 1")).isEmpty());
            QVERIFY(!contents(opened->findChild<ScintillaEditBase*>("FILE 2")).isEmpty());
            QVERIFY(action(window, "취소")->isEnabled());
            qInfo("Pending Qt file opened in separate comparison at %lld ms", elapsed.elapsed());
            QVERIFY(opened->grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/large-priority-file.png"));
            opened->close();
        }
        navigationReady = true;
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(tree->model()->index(0, 0)).center());
        QVERIFY(tree->currentIndex().isValid());
        qInfo("First incremental directory display: %lld ms", elapsed.elapsed());
        QVERIFY(window.findChild<QLabel*>("compareStatus")->text().contains("중"));
        const auto first = tree->model()->index(0, 0); tree->setCurrentIndex(first); tree->expand(first);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/large-directory-progress.png"));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("compareStatus")->text().startsWith("디렉터리 비교") || window.ExitCode() == 2, 1200000);
        auto* model = static_cast<DirectoryTreeModel*>(window.findChild<QAbstractItemModel*>("directoryTreeModel")); QVERIFY(model);
        QCOMPARE(resets.size(), 0); QVERIFY(model->VisibleEntries() > 100000); QVERIFY(window.findChild<QLabel*>("compareStatus")->text().contains("디렉터리 비교"));
        qInfo("Large directory GUI ready: %lld ms, %d entries, sampled peak private %llu bytes", elapsed.elapsed(), model->VisibleEntries(), peakPrivate);
        interaction.stop(); eventGaps.append(heartbeat.elapsed()); std::sort(eventGaps.begin(), eventGaps.end());
        QVERIFY(scrollChanges >= 20); qInfo("Verified actual scroll position changes: %d", scrollChanges);
        QVERIFY(!eventGaps.isEmpty()); qInfo("During Qt comparison with repeated scrolling: event-loop gap p95 %lld ms, max %lld ms, %lld samples", eventGaps[eventGaps.size() * 95 / 100], eventGaps.back(), eventGaps.size());
        ProcessControllerMemory()->Check();
        auto* view = static_cast<DirectoryView*>(window.findChild<QWidget*>("directoryView"));
        view->NavigateDifference(1); QVERIFY(tree->currentIndex().isValid());
        const auto root = tree->model()->index(0, 0); tree->expand(root);
        QTest::qWait(100); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/large-directory-acceptance.png"));
        window.close();
    }
    void directoryPendingBranchesAndImmediateOpen()
    {
        QTemporaryDir dir; Request request; request.file1_ = dir.filePath("left"); request.file2_ = dir.filePath("right");
        request.directoryProgress_ = std::make_shared<DirectoryProgressChannel>();
        for (const auto& root : {request.file1_, request.file2_}) { QVERIFY(QDir().mkpath(root + "/src/deep/child")); write(root + "/src/deep/child/file.txt", "actual"); }
        DirectoryView view; view.BeginComparison(request); view.resize(1000, 600); view.show();
        auto* tree = view.findChild<QTreeView*>("directoryLeftTree");
        auto* source = static_cast<DirectoryTreeModel*>(view.findChild<QAbstractItemModel*>("directoryTreeModel"));
        QAbstractItemModelTester tester(source, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QPersistentModelIndex parent;
        for (const auto& path : {QString("src"), QString("src/deep"), QString("src/deep/child")})
        {
            DirectoryProgress progress; progress.phase_ = "scan"; progress.side_ = -2;
            DirectoryRow row; row.path_ = path; row.statuses_[0] = 18; row.sides_[0].present_ = true; row.sides_[0].directory_ = true;
            progress.rows_.append(row); view.ApplyProgress(progress);
            const auto index = tree->model()->index(0, 0, parent); QVERIFY(index.isValid());
            QCOMPARE(tree->model()->rowCount(index), 0); QVERIFY(tree->model()->hasChildren(index));
            tree->expand(index); QCoreApplication::processEvents(); QVERIFY(tree->isExpanded(index));
            QCOMPARE(request.directoryProgress_->Priority(), path); parent = index;
        }
        DirectoryProgress progress; progress.phase_ = "scan"; progress.side_ = -2;
        DirectoryRow row; row.path_ = "src/deep/child/file.txt"; row.statuses_[0] = 18; row.sides_[0].present_ = true;
        progress.rows_.append(row); view.ApplyProgress(progress);
        const auto file = tree->model()->index(0, 0, parent); tree->setCurrentIndex(file); QVERIFY(file.isValid());
        Request opened; bool called = false; view.open_ = [&](Request Input) { opened = std::move(Input); called = true; };
        QVERIFY(view.findChild<QPushButton*>("directoryOpenFile")->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(tree, "activated", Qt::DirectConnection, Q_ARG(QModelIndex, file))); QVERIFY(called);
        QCOMPARE(request.directoryProgress_->Priority(), row.path_);
        // Right has not been enumerated, but its real content must not become empty.
        const auto actual = Execute(opened, std::make_shared<std::atomic_bool>(false), false);
        QCOMPARE(actual.exitCode_, 0); QCOMPARE(actual.inputs_.size(), std::size_t(2)); QCOMPARE(actual.inputs_[1].utf8_, QByteArray("actual"));
        view.EndProgress(); QVERIFY(tree->isExpanded(parent)); view.close();
    }
    void directoryProgressKeepsSelectionAndUpdatesStatus()
    {
        DirectoryView view; Request request; request.file1_ = "D:/left"; request.file2_ = "D:/right";
        view.BeginComparison(request); view.show(); auto* tree = view.findChild<QTreeView*>("directoryLeftTree");
        auto* source = static_cast<DirectoryTreeModel*>(view.findChild<QAbstractItemModel*>("directoryTreeModel"));
        QAbstractItemModelTester tester(source, QAbstractItemModelTester::FailureReportingMode::QtTest);
        DirectoryProgress batch; batch.side_ = 0; batch.phase_ = "scan";
        batch.rows_.append(QJsonObject{{"path", "src/a.txt"}, {"status", "Pending"}, {"left", QJsonObject{{"present", true}, {"path", "src/a.txt"}}}});
        view.ApplyProgress(batch); const auto root = tree->model()->index(0, 0); tree->expand(root);
        const QPersistentModelIndex selected(tree->model()->index(0, 0, root)); tree->setCurrentIndex(selected);
        QVERIFY(view.findChild<QPushButton*>("directoryOpenFile")->isEnabled());
        batch.rows_ = {}; batch.rows_.append(QJsonObject{{"path", "src/b.txt"}, {"status", "Pending"}, {"left", QJsonObject{{"present", true}}}});
        view.ApplyProgress(batch); QVERIFY(selected.isValid()); QCOMPARE(tree->currentIndex(), QModelIndex(selected)); QVERIFY(tree->isExpanded(root));
        batch.side_ = -1; batch.phase_ = "compare"; batch.rows_ = {};
        batch.rows_.append(QJsonObject{{"path", "src/a.txt"}, {"status", "LeftOnly"}, {"left", QJsonObject{{"present", true}}}});
        view.ApplyProgress(batch); QVERIFY(view.findChild<QPushButton*>("directoryOpenFile")->isEnabled());
        QCOMPARE(tree->currentIndex().siblingAtColumn(1).data().toString(), QString("왼쪽만 존재"));
        Result final; final.entries_ = source->Entries(); view.SetComparison(request, final);
        QCOMPARE(tree->currentIndex().data(DirectoryTreeModel::PathRole).toString(), QString("src/a.txt"));
        QVERIFY(tree->isExpanded(tree->currentIndex().parent())); view.close();
    }
    void directoryTreeLargeModel()
    {
        QJsonArray rows;
        for (int i = 0; i < 100000; ++i) rows.append(QJsonObject{{"path", QString("src/file%1.cpp").arg(i, 6, 10, QChar('0'))}, {"status", i == 99999 ? "Conflict" : "Equal"}});
        DirectoryView view; view.resize(1200, 700); Result value; value.entries_ = rows;
        QElapsedTimer timer; timer.start(); view.SetComparison({}, value); const auto setupMs = timer.elapsed(); view.show(); const auto showMs = timer.elapsed();
        auto* left = view.findChild<QTreeView*>("directoryLeftTree"); auto* right = view.findChild<QTreeView*>("directoryRightTree");
        QCOMPARE(left->model()->rowCount(), 1); QCOMPARE(left->model()->rowCount(left->model()->index(0, 0)), 100000);
        auto* status = view.findChild<QComboBox*>("directoryStatusFilter"); status->setCurrentIndex(2); view.NavigateDifference(1);
        QCOMPARE(left->currentIndex().data().toString(), QString("file099999.cpp"));
        QCOMPARE(right->currentIndex().data(DirectoryTreeModel::PathRole), left->currentIndex().data(DirectoryTreeModel::PathRole));
        qInfo("100000-entry paired trees: setup %lld ms, show %lld ms, filter/navigation %lld ms", setupMs, showMs - setupMs, timer.elapsed() - showMs);
        view.findChild<QLineEdit*>("directoryQuery")->setText("absent"); QCOMPARE(left->model()->rowCount(), 0); view.NavigateDifference(-1);
        view.findChild<QLineEdit*>("directoryQuery")->clear(); view.NavigateDifference(1);
        view.SetComparison({}, {}); QCOMPARE(left->model()->rowCount(), 0); QVERIFY(!left->currentIndex().isValid());
        view.close();
    }
    void directoryVirtualModelAndNavigation()
    {
        DirectoryModel model; QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QJsonArray rows;
        for (int i = 0; i < 100000; ++i) rows.append(QJsonObject{{"path", QString("src/file%1.cpp").arg(i, 6, 10, QChar('0'))}, {"status", i == 99999 ? "Conflict" : "Equal"}});
        model.SetEntries(rows); QCOMPARE(model.rowCount(), 100000); QCOMPARE(model.columnCount(), 5);
        QCOMPARE(model.rowCount(model.index(0, 0)), 0); QVERIFY(!model.data({}).isValid());
        DirectoryView view; Request r; Result value; value.entries_ = rows; value.directoryLevel_ = "bytes"; view.SetComparison(r, value);
        view.findChild<QComboBox*>("directoryViewMode")->setCurrentIndex(1);
        auto* table = view.findChild<QTableView*>("directoryTable"); auto* filter = view.findChild<QComboBox*>("directoryStatusFilter");
        auto* query = view.findChild<QLineEdit*>("directoryQuery"); QCOMPARE(table->model()->rowCount(), 100000);
        filter->setCurrentIndex(2); QCOMPARE(table->model()->rowCount(), 1);
        view.NavigateDifference(1); QCOMPARE(table->currentIndex().data().toString(), QString("src/file099999.cpp"));
        query->setText("does-not-exist"); QCOMPARE(table->model()->rowCount(), 0); view.NavigateDifference(-1);
        filter->setCurrentIndex(0); query->setText("FILE09999"); QCOMPARE(table->model()->rowCount(), 10);
        model.SetEntries({}); QCOMPARE(model.rowCount(), 0);
    }
    void directoryWindowFiltersAndFileRequest()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.theme_ = "light";
        for (const auto& root : {r.file1_, r.file2_, r.base_}) { QVERIFY(QDir().mkpath(root + "/src")); write(root + "/same.txt", "same"); }
        write(r.file1_ + "/src/changed.cpp", "left"); write(r.file2_ + "/src/changed.cpp", "right"); write(r.base_ + "/src/changed.cpp", "base");
        write(r.file1_ + "/only-left.txt", "new"); CompareWindow window(r); window.show(); window.activateWindow();
        auto* table = window.findChild<QTableView*>("directoryTable"); QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 4, 10000);
        QTRY_VERIFY(window.findChild<QLabel*>("compareStatus")->text().startsWith("디렉터리 비교"));
        window.findChild<QComboBox*>("directoryViewMode")->setCurrentIndex(1);
        QVERIFY(window.findChild<QLabel*>("compareStatus")->text().contains("충돌 1개"));
        QVERIFY(window.findChildren<ScintillaEditBase*>().isEmpty()); QVERIFY(!window.findChild<QTreeWidget*>("compareChanges")->isVisible());
        auto* filter = window.findChild<QComboBox*>("directoryStatusFilter"); auto* query = window.findChild<QLineEdit*>("directoryQuery");
        filter->setCurrentIndex(2); QCOMPARE(table->model()->rowCount(), 1); action(window, "다음 차이")->trigger();
        auto* open = window.findChild<QPushButton*>("directoryOpenFile"); QVERIFY(open->isEnabled());
        auto* view = static_cast<DirectoryView*>(window.findChild<QWidget*>("directoryView"));
        Request child; view->open_ = [&](Request Input) { child = std::move(Input); }; QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(child.file1_, r.file1_ + "/src/changed.cpp"); QCOMPARE(child.base_, r.base_ + "/src/changed.cpp"); QCOMPARE(child.mode_, QString("compare"));
        filter->setCurrentIndex(0); query->setText("only-left"); table->setCurrentIndex(table->model()->index(0, 0)); QVERIFY(open->isEnabled());
        QTest::mouseClick(open, Qt::LeftButton); QVERIFY(child.allowMissingInputs_[1]);
        query->clear(); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/directory-dark.png"));
        write(r.file1_ + "/new.txt", "new"); action(window, "다시 비교")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 5, 10000); window.close();
    }
    void threeFilePairSelectionAndSnapshots()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("one.cpp"); r.file2_ = dir.filePath("two.cpp"); r.file3_ = dir.filePath("three.cpp"); r.theme_ = "light";
        const auto a = lines(0, 120), b = lines(0, 30) + QString::fromUtf8("value B 한글😀\r\n").toUtf8() + lines(31, 120);
        const auto c = QByteArray("prefix\r\nsecond\r\n") + lines(0, 30) + QString::fromUtf8("value C 한굴😃\r\n").toUtf8() + lines(31, 120) + "tail\r\n";
        write(r.file1_, a); write(r.file2_, b); write(r.file3_, c);
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 3"), 10000);
        auto* selector = window.findChild<QComboBox*>("comparePairSelector"); QTRY_VERIFY_WITH_TIMEOUT(selector->isVisible(), 10000); QCOMPARE(selector->count(), 3);
        const std::array<ScintillaEditBase*, 3> editors{window.findChild<ScintillaEditBase*>("FILE 1"), window.findChild<ScintillaEditBase*>("FILE 2"), window.findChild<ScintillaEditBase*>("FILE 3")};
        auto* bar = window.findChild<OverviewBar*>(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        editors[0]->setFocus(); QTest::qWait(50);
        for (auto* editor : editors) editor->send(SCI_SETSEL, 2, 4);
        write(r.file1_, "external one"); write(r.file2_, "external two"); write(r.file3_, "external three");
        const std::array<std::array<int, 2>, 3> pairs{{{0, 1}, {0, 2}, {1, 2}}};
        for (const auto index : {1, 2, 0, 2, 1})
        {
            selector->setFocus(); selector->setCurrentIndex(index); QTest::qWait(20);
            const auto first = pairs[index][0], second = pairs[index][1], inactive = 3 - first - second;
            QCOMPARE(tree->headerItem()->text(1), editors[first]->objectName()); QCOMPARE(tree->headerItem()->text(2), editors[second]->objectName());
            QVERIFY(bar->accessibleName().contains(QString::number(second + 1)));
            for (auto* editor : editors)
            {
                QCOMPARE(editor->send(SCI_GETANCHOR), sptr_t(2)); QCOMPARE(editor->send(SCI_GETCURRENTPOS), sptr_t(4));
                QCOMPARE(editor->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(editor->send(SCI_CANUNDO), sptr_t(0));
            }
            QCOMPARE(editors[inactive]->send(GetLeadingRows), sptr_t(0));
            for (sptr_t line = 0; line < editors[inactive]->send(SCI_GETLINECOUNT); ++line)
            {
                QVERIFY(!(editors[inactive]->send(SCI_MARKERGET, uptr_t(line)) & (1 << 8)));
                const auto position = editors[inactive]->send(SCI_POSITIONFROMLINE, uptr_t(line));
                QCOMPARE(editors[inactive]->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, position), sptr_t(0));
            }
            const auto inactiveTop = editors[inactive]->send(SCI_GETFIRSTVISIBLELINE);
            editors[first]->send(SCI_SETFIRSTVISIBLELINE, 45);
            QTRY_COMPARE(editors[second]->send(SCI_GETFIRSTVISIBLELINE), sptr_t(45));
            QCOMPARE(editors[inactive]->send(SCI_GETFIRSTVISIBLELINE), inactiveTop);
            QCOMPARE(contents(editors[0]), a); QCOMPARE(contents(editors[1]), b); QCOMPARE(contents(editors[2]), c);
        }
        // Actual keyboard pair selection, then F7 navigation on the selected pair only.
        selector->setFocus(); QTest::keyClick(selector, Qt::Key_Down); QCOMPARE(selector->currentIndex(), 2);
        editors[0]->send(SCI_SETFIRSTVISIBLELINE, 65); const auto inactiveTop = editors[0]->send(SCI_GETFIRSTVISIBLELINE);
        action(window, "다음 차이")->trigger(); QCOMPARE(editors[0]->send(SCI_GETFIRSTVISIBLELINE), inactiveTop);
        QVERIFY(tree->currentItem());
        QCOMPARE(editors[1]->send(SCI_LINEFROMPOSITION, uptr_t(editors[1]->send(SCI_GETCURRENTPOS))), sptr_t(tree->currentItem()->data(1, Qt::UserRole).toULongLong()));
        QCOMPARE(editors[2]->send(SCI_LINEFROMPOSITION, uptr_t(editors[2]->send(SCI_GETCURRENTPOS))), sptr_t(tree->currentItem()->data(2, Qt::UserRole).toULongLong()));
        bar->setFocus(); QTest::keyClick(bar, Qt::Key_End); QCOMPARE(editors[0]->send(SCI_GETFIRSTVISIBLELINE), inactiveTop);
        action(window, "왼쪽·오른쪽 표시 줄 정렬")->setChecked(false); action(window, "왼쪽·오른쪽 스크롤 동기화")->setChecked(false);
        const auto thirdTop = editors[2]->send(SCI_GETFIRSTVISIBLELINE); editors[1]->send(SCI_SETFIRSTVISIBLELINE, 10);
        QCOMPARE(editors[2]->send(SCI_GETFIRSTVISIBLELINE), thirdTop);
        action(window, "왼쪽·오른쪽 스크롤 동기화")->setChecked(true);
        editors[1]->send(SCI_SETFIRSTVISIBLELINE, 25); QTRY_VERIFY(editors[2]->send(SCI_GETFIRSTVISIBLELINE) < 40);
        QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/three-pair-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/three-pair-dark.png"));
        // Recompare adopts new disk snapshots and retains the selected pair.
        write(r.file1_, "same"); write(r.file2_, "different"); write(r.file3_, "same");
        QPointer<ScintillaEditBase> previous = editors[0]; action(window, "다시 비교")->trigger(); QVERIFY(!selector->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000); QVERIFY(!previous);
        QCOMPARE(selector->currentIndex(), 2); selector->setCurrentIndex(1);
        QCOMPARE(tree->topLevelItemCount(), 0); QCOMPARE(window.ExitCode(), 1);
        QCOMPARE(contents(window.findChild<ScintillaEditBase*>("FILE 3")), QByteArray("same"));
        window.close();
    }
    void threeFilePairFailureAndBaseIsolation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("a"); r.file2_ = dir.filePath("b"); r.file3_ = dir.filePath("c");
        write(r.file1_, ""); write(r.file2_, "x"); write(r.file3_, ""); CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 3"), 10000);
        auto* selector = window.findChild<QComboBox*>("comparePairSelector"); selector->setCurrentIndex(1);
        QCOMPARE(window.findChild<QTreeWidget*>("compareChanges")->topLevelItemCount(), 0);
        QVERIFY(QFile::remove(r.file3_)); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QVERIFY(selector->isHidden()); QVERIFY(window.findChildren<ScintillaEditBase*>().isEmpty());
        QVERIFY(window.findChild<OverviewBar*>()->isHidden());
        write(r.file3_, "z"); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 3"), 10000); QVERIFY(selector->isVisible()); window.close();
        r.base_ = r.file3_; r.file3_.clear(); r.mode_ = "merge"; r.output_ = dir.filePath("out");
        CompareWindow merge(r); merge.show(); QTRY_VERIFY_WITH_TIMEOUT(merge.findChild<ScintillaEditBase*>("RESULT"), 10000);
        QVERIFY(merge.findChild<QComboBox*>("comparePairSelector")->isHidden());
        action(merge, "왼쪽 전체 선택")->trigger(); action(merge, "결과 저장")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(merge.ExitCode(), 0, 10000); merge.close();
    }
    void overviewNavigationAndAppearance()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp"); r.theme_ = "light";
        const auto a = lines(0, 1000);
        const auto b = QByteArray("inserted\r\n") + lines(0, 180) + lines(180, 240, "changed") + lines(240, 600) + lines(640, 1000) + "tail\r\n";
        write(r.file1_, a); write(r.file2_, b);
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        auto* bar = window.findChild<OverviewBar*>(); QVERIFY(bar); QTRY_VERIFY(bar->isVisible());
        QVERIFY(!bar->accessibleName().isEmpty()); QVERIFY(bar->width() >= 48);
        left->setFocus(); QTest::qWait(50);
        left->send(SCI_SETSEL, 2, 8); right->send(SCI_SETSEL, 3, 9);
        QTest::qWait(30); const auto initial = bar->grab().toImage();
        // A one-bin insertion at BOF remains visible under the viewport marker.
        const auto scale = initial.devicePixelRatio();
        QCOMPARE(initial.pixelColor(qRound(bar->width() * 0.75 * scale), qCeil((bar->fontMetrics().height() + 8) * scale)), QColor("#31804b"));
        QCOMPARE(left->send(SCI_GETANCHOR), sptr_t(2));
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, QPoint(bar->width() / 4, bar->height() / 2));
        QTRY_VERIFY(left->send(SCI_GETFIRSTVISIBLELINE) > 400 && left->send(SCI_GETFIRSTVISIBLELINE) < 600);
        QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), left->send(SCI_GETFIRSTVISIBLELINE));
        QVERIFY(bar->hasFocus()); QVERIFY(initial != bar->grab().toImage());
        QCOMPARE(left->send(SCI_GETANCHOR), sptr_t(2));
        const auto before = left->send(SCI_GETFIRSTVISIBLELINE);
        QTest::keyClick(bar, Qt::Key_PageDown); QVERIFY(left->send(SCI_GETFIRSTVISIBLELINE) > before);
        const auto advanced = left->send(SCI_GETFIRSTVISIBLELINE);
        QTest::keyClick(bar, Qt::Key_Up); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), advanced - 1);
        QTest::keyClick(bar, Qt::Key_Down); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), advanced);
        QTest::keyClick(bar, Qt::Key_PageUp); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), before);
        QTest::keyClick(bar, Qt::Key_Home); QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        const auto map = AlignmentMap::Build(SplitLines(a.toStdString()).size(), SplitLines(b.toStdString()).size(), Diff(a.toStdString(), b.toStdString()));
        QTest::keyClick(bar, Qt::Key_End); QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(map.RowCount()));
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, QPoint(bar->width() / 4, bar->height() * 3 / 4));
        const QPoint drag(bar->width() / 4, bar->height() / 4);
        QMouseEvent move(QEvent::MouseMove, QPointF(drag), QPointF(bar->mapToGlobal(drag)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &move); QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, drag);
        QTRY_VERIFY(left->send(SCI_GETFIRSTVISIBLELINE) < 300);
        action(window, "왼쪽·오른쪽 표시 줄 정렬")->setChecked(false);
        action(window, "왼쪽·오른쪽 스크롤 동기화")->setChecked(false);
        QTest::keyClick(bar, Qt::Key_Right); QTest::keyClick(bar, Qt::Key_Home);
        const auto leftTop = left->send(SCI_GETFIRSTVISIBLELINE);
        QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0)); QVERIFY(leftTop > 0);
        QTest::keyClick(bar, Qt::Key_End); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(map.LineCount(1)));
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), leftTop);
        QTest::keyClick(bar, Qt::Key_Left); QTest::keyClick(bar, Qt::Key_Home);
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QCOMPARE(left->send(SCI_GETANCHOR), sptr_t(2));
        auto* toggle = action(window, "차이 개요 막대"); toggle->setChecked(false); QVERIFY(bar->isHidden());
        toggle->setChecked(true); QTRY_VERIFY(bar->isVisible());
        QCOMPARE(left->send(SCI_GETANCHOR), sptr_t(2)); QCOMPARE(left->send(SCI_GETCURRENTPOS), sptr_t(8));
        QCOMPARE(right->send(SCI_GETANCHOR), sptr_t(3)); QCOMPARE(right->send(SCI_GETCURRENTPOS), sptr_t(9));
        for (auto* editor : {left, right}) { QCOMPARE(editor->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(editor->send(SCI_CANUNDO), sptr_t(0)); }
        QCOMPARE(contents(left), a); QCOMPARE(contents(right), b);
        left->send(SCI_SETFIRSTVISIBLELINE, 170); right->send(SCI_SETFIRSTVISIBLELINE, 171);
        QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/overview-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/overview-dark.png")); window.close();
    }
    void overviewRecompareAndMergePreservation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.base_ = dir.filePath("base.cpp"); r.output_ = dir.filePath("out.cpp");
        const auto a = lines(0, 500), b = lines(0, 200) + QByteArray("right\r\n") + lines(201, 500);
        write(r.file1_, a); write(r.file2_, b); write(r.base_, lines(0, 200) + QByteArray("base\r\n") + lines(201, 500));
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* base = window.findChild<ScintillaEditBase*>("BASE");
        auto* left = window.findChild<ScintillaEditBase*>("LEFT"); auto* bar = window.findChild<OverviewBar*>();
        action(window, "왼쪽 전체 선택")->trigger(); output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n"));
        const auto text = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT);
        auto* tree = window.findChild<QTreeWidget*>("compareChanges"); QVERIFY(tree->topLevelItem(0));
        const auto decision = tree->topLevelItem(0)->text(3);
        output->send(SCI_SETFIRSTVISIBLELINE, 20); base->send(SCI_SETFIRSTVISIBLELINE, 15);
        bar->setFocus(); QTest::keyClick(bar, Qt::Key_End);
        QCOMPARE(output->send(SCI_GETFIRSTVISIBLELINE), sptr_t(20)); QCOMPARE(base->send(SCI_GETFIRSTVISIBLELINE), sptr_t(15));
        QCOMPARE(tree->topLevelItem(0)->text(3), decision); QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        write(r.file1_, "short left"); write(r.file2_, "short right"); action(window, "다시 비교")->trigger();
        QVERIFY(!bar->isEnabled()); QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QVERIFY(bar->isEnabled()); QTest::keyClick(bar, Qt::Key_End);
        QVERIFY(left->send(SCI_GETFIRSTVISIBLELINE) <= 1); QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        QVERIFY(QFile::remove(r.file1_)); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QVERIFY(bar->isEnabled()); QTest::keyClick(bar, Qt::Key_Home); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        write(r.file1_, "short left"); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        action(window, "전체 수동 완료")->trigger(); action(window, "결과 저장")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile saved(r.output_); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), text); window.close();
    }
    void overviewEmptyDisabledAndLifetime()
    {
        CompareWindow empty(Request{}); empty.show(); auto* emptyBar = empty.findChild<OverviewBar*>();
        QVERIFY(emptyBar->isHidden()); QVERIFY(!action(empty, "차이 개요 막대")->isEnabled()); empty.close();
        QTemporaryDir dir; QDir{}.mkpath(dir.filePath("one")); QDir{}.mkpath(dir.filePath("two"));
        Request r; r.file1_ = dir.filePath("one"); r.file2_ = dir.filePath("two"); r.mode_ = "directory";
        CompareWindow directory(r); directory.show();
        QTRY_VERIFY_WITH_TIMEOUT(directory.findChild<QWidget*>("directoryView")->isVisible(), 10000);
        QVERIFY(directory.findChild<OverviewBar*>()->isHidden()); QVERIFY(!action(directory, "차이 개요 막대")->isEnabled()); directory.close();
        auto* left = new ScintillaEditBase; auto* right = new ScintillaEditBase;
        setText(*left, ""); setText(*right, "abc"); left->send(SCI_SETENDATLASTLINE, 0); right->send(SCI_SETENDATLASTLINE, 0);
        auto map = std::make_shared<AlignmentMap>(AlignmentMap::Build(0, 1, {{0, 0, 0, 1, {}}}));
        OverviewBar bar(nullptr); bar.resize(60, 200); bar.SetSession(left, right, map); bar.show();
        QTest::keyClick(&bar, Qt::Key_End); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QTest::keyClick(&bar, Qt::Key_Right); bar.setEnabled(false); const auto before = right->send(SCI_GETFIRSTVISIBLELINE);
        QKeyEvent attempt(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier); QApplication::sendEvent(&bar, &attempt);
        QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), before);
        bar.setEnabled(true); delete left; QVERIFY(bar.isHidden());
        bar.SetSession(nullptr, nullptr, {}); delete right; bar.resize(48, 16); bar.show();
        QApplication::sendEvent(&bar, &attempt); QVERIFY(!bar.grab().isNull()); bar.close();
    }
    void inlineHighlightsThemeSearchAndToggle()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp"); r.theme_ = "light";
        const QByteArray a = QString::fromUtf8("int count = 10;\r\n// 한글😀 keep\r\nreturn count;\r\n").toUtf8();
        const QByteArray b = QString::fromUtf8("int count = 20;\r\n// 한굴😃 keep\r\nreturn count;\r\n").toUtf8();
        write(r.file1_, a); write(r.file2_, b);
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        const auto marked = [](ScintillaEditBase* Editor, int Position) { return Editor->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, Position); };
        for (auto* editor : {left, right})
        {
            QCOMPARE(marked(editor, 12), sptr_t(1)); QCOMPARE(marked(editor, 13), sptr_t(0));
            for (int i = 23; i < 30; ++i) QCOMPARE(marked(editor, i), sptr_t(1));
            QCOMPARE(marked(editor, 22), sptr_t(0)); QCOMPARE(marked(editor, 30), sptr_t(0));
            QCOMPARE(editor->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(editor->send(SCI_CANUNDO), sptr_t(0));
            QVERIFY(editor->send(SCI_MARKERGET, 0) & (1 << 8));
            editor->send(SCI_SETINDICATORCURRENT, 7); editor->send(SCI_SETINDICATORVALUE, 9);
        }
        auto* toggle = action(window, "줄 내부 차이 강조"); QVERIFY(toggle);
        toggle->setChecked(false); QCOMPARE(marked(left, 12), sptr_t(0));
        toggle->setChecked(true); QCOMPARE(marked(left, 12), sptr_t(1));
        QCOMPARE(left->send(SCI_GETINDICATORCURRENT), sptr_t(7)); QCOMPARE(left->send(SCI_GETINDICATORVALUE), sptr_t(9));
        left->setFocus(); left->send(SCI_SETSEL, 0, 0); action(window, "찾기")->trigger();
        auto* bar = window.findChild<FindBar*>(); auto* query = bar->findChild<QLineEdit*>("compareFindText");
        query->setText("10"); bar->Find(false); QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(12));
        QCOMPARE(left->send(SCI_GETSELECTIONEND), sptr_t(14)); QCOMPARE(marked(left, 12), sptr_t(1));
        QTest::qWait(60); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/inline-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(60);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/inline-dark.png"));
        QCOMPARE(marked(left, 12), sptr_t(1)); QCOMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(12));
        QCOMPARE(contents(left), a); QCOMPARE(contents(right), b);
        QCOMPARE(left->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(left->send(SCI_CANUNDO), sptr_t(0));
        QCOMPARE(window.ExitCode(), 1); window.close();
    }
    void inlineWordAnchorsPresentationOnly()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt");
        write(r.file1_, "alpha beta\n"); write(r.file2_, "beta alpha\n");
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        for (int i = 0; i < 10; ++i)
        {
            QCOMPARE(left->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, i), sptr_t(i < 6));
            QCOMPARE(right->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, i), sptr_t(i >= 4));
        }
        QCOMPARE(contents(left), QByteArray("alpha beta\n")); QCOMPARE(contents(right), QByteArray("beta alpha\n"));
        QCOMPARE(left->send(SCI_CANUNDO), sptr_t(0)); QCOMPARE(right->send(SCI_CANUNDO), sptr_t(0));
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/word-highlights.png"));
        window.close();
    }
    void inlineGraphemeIndicatorsAndSourcePreservation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt");
        const QByteArray a = QString::fromUtf8("x e\u0301 👩‍💻 👍🏻 🇰🇷 z\n").toUtf8();
        const QByteArray b = QString::fromUtf8("x e\u0300 👨‍💻 👍🏿 🇯🇵 z\n").toUtf8();
        write(r.file1_, a); write(r.file2_, b); CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        for (auto* editor : {left, right})
        {
            const auto text = contents(editor); int begin = 2;
            for (int token = 0; token < 4; ++token)
            {
                const int end = text.indexOf(' ', begin); QVERIFY(end > begin);
                for (int at = begin; at < end; ++at) QCOMPARE(editor->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, at), sptr_t(1));
                QCOMPARE(editor->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, end), sptr_t(0)); begin = end + 1;
            }
            QCOMPARE(editor->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(editor->send(SCI_CANUNDO), sptr_t(0));
        }
        QCOMPARE(contents(left), a); QCOMPARE(contents(right), b);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/grapheme-highlights.png")); window.close();
    }
    void inlineRecompareFallbackAndMergePreservation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.base_ = dir.filePath("base.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "head\naXb\nend"); write(r.file2_, "head\naYb\nend"); write(r.base_, "head\naZb\nend");
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("LEFT"); auto* output = window.findChild<ScintillaEditBase*>("RESULT");
        auto* base = window.findChild<ScintillaEditBase*>("BASE"); auto* status = window.findChild<QLabel*>("compareInlineStatus");
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, 6), sptr_t(1));
        QCOMPARE(base->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, 6), sptr_t(0));
        QVERIFY(status->isHidden()); action(window, "왼쪽 전체 선택")->trigger();
        output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n"));
        const auto before = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT);
        action(window, "줄 내부 차이 강조")->setChecked(false); action(window, "줄 내부 차이 강조")->setChecked(true);
        QCOMPARE(contents(output), before); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        for (sptr_t i = 0; i < before.size(); ++i) QCOMPARE(output->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, i), sptr_t(0));
        write(r.file1_, "prefix\nhead\naQb\nend"); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, 6), sptr_t(0));
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, 13), sptr_t(1));
        QCOMPARE(contents(output), before); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        write(r.file1_, QByteArray(20000, 'a')); write(r.file2_, QByteArray(20000, 'b'));
        action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QVERIFY(!status->isHidden()); QVERIFY(status->text().contains("1줄"));
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, sourceDifferenceIndicator, 13), sptr_t(0));
        QVERIFY(left->send(SCI_MARKERGET, 0) & (1 << 8)); QCOMPARE(contents(output), before);
        write(r.file1_, QByteArray(20000, 'b')); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QVERIFY(status->isHidden()); QCOMPARE(contents(output), before); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        action(window, "전체 수동 완료")->trigger(); action(window, "결과 저장")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile saved(r.output_); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), before); window.close();
    }
    void reintegrateResultPreservesManualUndoAndConflict()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        for (const auto& path : {r.file1_, r.file2_, r.base_}) write(path, "a\nkeep\nz\n");
        CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("RESULT"));
        auto* output = window.findChild<ScintillaEditBase*>("RESULT");
        output->send(SCI_SETTARGETRANGE, 0, 1); output->send(SCI_REPLACETARGET, 6, reinterpret_cast<sptr_t>("manual"));
        const auto manual = contents(output); QCOMPARE(manual, QByteArray("manual\nkeep\nz\n"));
        write(r.file1_, "a\nkeep\nnew\n"); write(r.file2_, "a\nkeep\nnew\n");
        action(window, "다시 비교")->trigger(); QTRY_VERIFY(!action(window, "취소")->isEnabled()); QCOMPARE(contents(output), manual);
        auto* reintegrate = window.findChild<QAction*>("reintegrateResult"); QVERIFY(reintegrate); QVERIFY(reintegrate->isEnabled()); reintegrate->trigger();
        QTRY_VERIFY(!action(window, "취소")->isEnabled()); const QByteArray expected = "manual\nkeep\nnew\n"; QCOMPARE(contents(output), expected);
        QVERIFY(!action(window, "결과 저장")->isEnabled()); QVERIFY(!QFileInfo::exists(r.output_));
        output->send(SCI_UNDO); QCOMPARE(contents(output), manual); output->send(SCI_REDO); QCOMPARE(contents(output), expected);
        const auto undo = output->send(SCI_GETUNDOCURRENT);
        write(r.file1_, "changed\nkeep\nnew\n"); write(r.file2_, "changed\nkeep\nnew\n"); reintegrate->trigger();
        QTRY_VERIFY(!action(window, "취소")->isEnabled()); QCOMPARE(contents(output), expected); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        QVERIFY(window.findChild<QLabel*>("compareStatus")->text().contains("겹칩니다")); QVERIFY(!QFileInfo::exists(r.output_));
    }
    void changedInputWorkRecoveryRequiresReview()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); const auto old = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        MergeWork work{QByteArray("manual result\r\n"), std::vector<BlockState>(old.plan_->blocks_.size(), {0, 0, Resolution::Manual, false}), false};
        const auto path = dir.filePath("work.hcmerge"); const auto checkpoint = EncodeMergeWork(r, old, work, {}); write(path, checkpoint);
        write(r.file1_, "new left\n"); write(r.file2_, "new right\n");
        CompareWindow window(r); window.show(); QTRY_VERIFY(window.findChild<ScintillaEditBase*>("RESULT"));
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); const auto initial = contents(output);
        auto* recover = window.findChild<QAction*>("recoverMergeWork"); QVERIFY(recover); QVERIFY(recover->isEnabled());
        window.LoadWork(path); QTRY_VERIFY(!action(window, "취소")->isEnabled()); QCOMPARE(contents(output), initial);
        window.RecoverWork(path); QTRY_COMPARE(contents(output), work.text_);
        QVERIFY(!action(window, "결과 저장")->isEnabled()); QVERIFY(!QFileInfo::exists(r.output_)); QCOMPARE(readBytes(path), checkpoint);
        output->send(SCI_UNDO); QCOMPARE(contents(output), initial); output->send(SCI_REDO); QCOMPARE(contents(output), work.text_);
        QVERIFY(!action(window, "결과 저장")->isEnabled());
        action(window, "전체 수동 완료")->trigger(); action(window, "결과 저장")->trigger(); QTRY_COMPARE(window.ExitCode(), 0);
        QCOMPARE(readBytes(r.output_), work.text_); QCOMPARE(readBytes(path), checkpoint); window.close();
    }
    void legacyWorkRestoresTextWithReviewAndUndo()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt"); r.output_ = dir.filePath("out.txt"); r.semantic_ = "off";
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); const auto session = Execute(r, std::make_shared<std::atomic_bool>(false), false);
        MergeWork work{QByteArray("legacy manual result\n"), std::vector<BlockState>(session.plan_->blocks_.size(), {0, 0, Resolution::Manual, false}), false};
        auto object = QJsonDocument::fromJson(EncodeMergeWork(r, session, work, {})).object(); auto identity = object["identity"].toObject();
        identity.remove("generation"); identity["engineVersion"] = "myers-bounded-2";
        identity["generation"] = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        object["identity"] = identity; const auto path = dir.filePath("legacy.hcmerge"); write(path, QJsonDocument(object).toJson());
        CompareWindow window(r); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); const auto initial = contents(output); window.LoadWork(path);
        QTRY_COMPARE_WITH_TIMEOUT(contents(output), work.text_, 10000); QVERIFY(!action(window, "결과 저장")->isEnabled());
        output->send(SCI_UNDO); QCOMPARE(contents(output), initial); output->send(SCI_REDO); QCOMPARE(contents(output), work.text_);
        action(window, "전체 수동 완료")->trigger(); QVERIFY(action(window, "결과 저장")->isEnabled());
        action(window, "결과 저장")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000); window.close();
    }
    void decisionPlanImportExportAndUndo()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out"); r.semantic_ = "off";
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); CompareWindow window(r); window.show();
        auto* importAction = window.findChild<QAction*>("importDecisionPlan"); auto* exportAction = window.findChild<QAction*>("exportDecisionPlan"); QVERIFY(importAction); QVERIFY(exportAction);
        QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000); auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output);
        action(window, "오른쪽 전체 선택")->trigger(); const auto undo = output->send(SCI_GETUNDOCURRENT);
        const auto path = dir.filePath("plan.json"); window.ExportDecisions(path); QTRY_VERIFY_WITH_TIMEOUT(exportAction->isEnabled(), 10000);
        QVERIFY(QFileInfo::exists(path)); QCOMPARE(contents(output), QByteArray("right\n")); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo); QVERIFY(!QFileInfo::exists(r.output_));
        action(window, "왼쪽 전체 선택")->trigger(); QCOMPARE(contents(output), QByteArray("left\n"));
        window.ImportDecisions(path); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000); QCOMPARE(contents(output), QByteArray("right\n")); QVERIFY(!QFileInfo::exists(r.output_));
        action(window, "실행 취소")->trigger(); QCOMPARE(contents(output), QByteArray("left\n"));
        action(window, "다시 실행")->trigger(); QCOMPARE(contents(output), QByteArray("right\n"));
        write(path, "invalid JSON"); const auto beforeFailure = output->send(SCI_GETUNDOCURRENT);
        window.ImportDecisions(path); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000);
        QCOMPARE(contents(output), QByteArray("right\n")); QCOMPARE(output->send(SCI_GETUNDOCURRENT), beforeFailure);
        // Plan export must not take ownership of the current .hcmerge save path.
        const auto workPath = dir.filePath("work.hcmerge"); window.SaveWork(workPath); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000);
        window.ExportDecisions(workPath, true); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000);
        QFile protectedWork(workPath); QVERIFY(protectedWork.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(protectedWork.readAll()).object()["kind"].toString(), QString("text-merge-work")); protectedWork.close();
        window.ExportDecisions(path, true); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000);
        window.SaveWork(workPath); QTRY_VERIFY_WITH_TIMEOUT(importAction->isEnabled(), 10000);
        QFile work(workPath); QVERIFY(work.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(work.readAll()).object()["kind"].toString(), QString("text-merge-work")); work.close();
        action(window, "결과 저장")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000); window.close();
    }
    void reportExportWorkingBuffersAndMergeState()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.txt"); r.file2_ = dir.filePath("right.txt"); r.semantic_ = "off";
        write(r.file1_, "left\n"); write(r.file2_, "right\n"); CompareWindow window(r); window.show();
        auto* exportAction = window.findChild<QAction*>("exportComparisonReport"); QVERIFY(exportAction);
        QTRY_VERIFY_WITH_TIMEOUT(exportAction->isEnabled(), 10000); auto* left = window.findChild<ScintillaEditBase*>("FILE 1");
        left->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("working\n")); QVERIFY(!exportAction->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(exportAction->isEnabled(), 10000); const auto undo = left->send(SCI_GETUNDOCURRENT); const auto text = contents(left);
        const auto path = dir.filePath("compare.json"); window.ExportReport(path); QTRY_VERIFY_WITH_TIMEOUT(exportAction->isEnabled(), 10000);
        QFile report(path); QVERIFY(report.open(QIODevice::ReadOnly)); const auto object = QJsonDocument::fromJson(report.readAll()).object(); report.close();
        QCOMPARE(object["viewState"].toObject()["scope"].toString(), QString("working-buffers"));
        QVERIFY(object["viewState"].toObject()["workingInputs"].toArray()[0].toObject()["modified"].toBool());
        QCOMPARE(object["viewState"].toObject()["workingInputs"].toArray()[0].toObject()["utf8Sha256"].toString(), QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(contents(left), text); QCOMPARE(left->send(SCI_GETUNDOCURRENT), undo); QVERIFY(left->send(SCI_GETMODIFY));
        window.ExportReport(r.file1_, true); QTRY_VERIFY(exportAction->isEnabled()); QFile original(r.file1_); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("left\n")); original.close();
        left->send(SCI_UNDO); QTRY_VERIFY(!left->send(SCI_GETMODIFY)); window.close();
        r.mode_ = "merge"; r.output_ = dir.filePath("out.txt"); CompareWindow merge(r); merge.show();
        auto* mergeExport = merge.findChild<QAction*>("exportComparisonReport"); QTRY_VERIFY_WITH_TIMEOUT(mergeExport->isEnabled(), 10000);
        auto* output = merge.findChild<ScintillaEditBase*>("RESULT");
        action(merge, "오른쪽 전체 선택")->trigger(); const auto mergeUndo = output->send(SCI_GETUNDOCURRENT);
        merge.ExportReport(dir.filePath("merge.json")); QTRY_VERIFY_WITH_TIMEOUT(mergeExport->isEnabled(), 10000);
        QFile mergeReport(dir.filePath("merge.json")); QVERIFY(mergeReport.open(QIODevice::ReadOnly)); const auto state = QJsonDocument::fromJson(mergeReport.readAll()).object(); mergeReport.close();
        QCOMPARE(state["unresolvedCount"].toInt(), 0); QCOMPARE(state["resolutions"].toArray()[0].toObject()["choice"].toString(), QString("right"));
        QVERIFY(!state["viewState"].toObject()["workingResult"].toObject()["saved"].toBool());
        QCOMPARE(contents(output), QByteArray("right\n")); QCOMPARE(output->send(SCI_GETUNDOCURRENT), mergeUndo); QVERIFY(!QFileInfo::exists(r.output_));
        action(merge, "결과 저장")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(merge.ExitCode(), 0, 10000); merge.close();
    }
    void findAllHighlightsResultsAndInvalidation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp"); r.semantic_ = "off"; r.theme_ = "light";
        const auto text = QString::fromUtf8("alpha alphabet Alpha\r\n한글😀 alpha\nlast alpha").toUtf8(); write(r.file1_, text); write(r.file2_, "prefix\n" + text);
        CompareWindow window(r); window.resize(1200, 720); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* bar = window.findChild<FindBar*>();
        auto* query = bar->findChild<QLineEdit*>("compareFindText"); auto* status = bar->findChild<QLabel*>("compareFindFeedback");
        auto* rows = window.findChild<QTreeWidget*>("compareFindResults"); auto* dock = window.findChild<QDockWidget*>("compareFindResultsDock");
        auto* word = bar->findChild<QCheckBox*>("compareFindWord"); auto* matchCase = bar->findChild<QCheckBox*>("compareFindCase");
        left->send(SCI_SETSEL, 0, 0); left->send(SCI_SETINDICATORCURRENT, 7); left->send(SCI_SETINDICATORVALUE, 9);
        left->send(SCI_SETTARGETRANGE, 2, 5); left->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
        word->setChecked(true); matchCase->setChecked(false); query->setText("alpha"); bar->FindAll();
        QTRY_COMPARE(status->text(), QString("4건 찾음")); QCOMPARE(rows->topLevelItemCount(), 4); QVERIFY(dock->isVisible());
        QCOMPARE(rows->topLevelItem(2)->text(0), QString("2")); QCOMPARE(rows->topLevelItem(2)->text(1), QString("5"));
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 0), sptr_t(1));
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 6), sptr_t(0));
        QCOMPARE(left->send(SCI_GETINDICATORCURRENT), sptr_t(7)); QCOMPARE(left->send(SCI_GETINDICATORVALUE), sptr_t(9));
        QCOMPARE(left->send(SCI_GETTARGETSTART), sptr_t(2)); QCOMPARE(left->send(SCI_GETTARGETEND), sptr_t(5));
        QCOMPARE(left->send(SCI_GETSEARCHFLAGS), sptr_t(SCFIND_MATCHCASE)); QCOMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(0));
        QVERIFY(!left->send(SCI_GETMODIFY)); QVERIFY(!left->send(SCI_CANUNDO));
        rows->setCurrentItem(rows->topLevelItem(3)); rows->setFocus(); QTest::keyClick(rows, Qt::Key_Return); QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.lastIndexOf("alpha")));
        bar->Find(false); QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(0)); QCOMPARE(rows->topLevelItemCount(), 4);
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 0), sptr_t(1));
        QTest::qWait(40); QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/find-all-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(40);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/../HyperCompare/find-all-dark.png"));
        QCOMPARE(rows->topLevelItemCount(), 4); QCOMPARE(contents(left), text);
        left->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("edit\n")); QCOMPARE(rows->topLevelItemCount(), 0);
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 5), sptr_t(0));
        left->send(SCI_UNDO); QCOMPARE(contents(left), text);
        query->setText(QString::fromUtf8("한글😀")); bar->FindAll(); QTRY_COMPARE(status->text(), QString("1건 찾음"));
        QCOMPARE(rows->topLevelItem(0)->text(0), QString("2")); QVERIFY(rows->topLevelItem(0)->text(2).contains(QString::fromUtf8("한글😀")));
        bar->Close(); QVERIFY(!dock->isVisible()); QCOMPARE(rows->topLevelItemCount(), 0);
        QCOMPARE(left->send(SCI_INDICATORVALUEAT, searchMatchIndicator, text.indexOf(QString::fromUtf8("한글").toUtf8())), sptr_t(0)); window.close();
    }
    void findAllLimitsBoundaryAndCancel()
    {
        QMainWindow window; auto* editor = new ScintillaEditBase(&window); editor->setObjectName("TEXT"); window.setCentralWidget(editor);
        auto* bar = new FindBar(&window); window.addToolBar(Qt::BottomToolBarArea, bar); bar->SetEditors({editor}); window.resize(1100, 600); window.show();
        auto* query = bar->findChild<QLineEdit*>("compareFindText"); auto* status = bar->findChild<QLabel*>("compareFindFeedback");
        auto* rows = window.findChild<QTreeWidget*>("compareFindResults"); bar->findChild<QCheckBox*>("compareFindWord")->setChecked(false);
        setText(*editor, std::string(3000, 'a')); query->setText("a"); bar->FindAll();
        QTRY_COMPARE(status->text(), QString("1000건 · 표시 제한")); QCOMPARE(rows->topLevelItemCount(), 1000);
        QVERIFY(rows->topLevelItem(999)->text(2).size() <= 130);
        QCOMPARE(editor->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 999), sptr_t(1)); QCOMPARE(editor->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 1000), sptr_t(0));
        query->setText("missing"); QCOMPARE(rows->topLevelItemCount(), 0); QCOMPARE(editor->send(SCI_INDICATORVALUEAT, searchMatchIndicator, 0), sptr_t(0));
        bar->FindAll(); QTRY_COMPARE(status->text(), QString("0건 찾음"));
        const auto pattern = QString::fromUtf8("한글😀").toUtf8(); const auto chunk = 262144 / pattern.size();
        QByteArray text(3 * chunk, 'x'); text.replace(chunk - 3, pattern.size(), pattern); text.replace(2 * chunk - 3, pattern.size(), pattern);
        setText(*editor, text.toStdString()); query->setText(QString::fromUtf8(pattern)); bar->FindAll();
        QTRY_COMPARE(status->text(), QString("2건 찾음")); QCOMPARE(rows->topLevelItem(0)->data(0, Qt::UserRole).toLongLong(), qlonglong(chunk - 3));
        QCOMPARE(rows->topLevelItem(1)->data(0, Qt::UserRole).toLongLong(), qlonglong(2 * chunk - 3));
        query->setText("x"); bar->FindAll(); bar->Close(); QTest::qWait(20); QCOMPARE(rows->topLevelItemCount(), 0);
        bar->FindAll(); bar->SetBusy(true); QTest::qWait(20); QCOMPARE(rows->topLevelItemCount(), 0); QCOMPARE(status->text(), QString("검색 중단"));
        bar->SetBusy(false); bar->SetEditors({}); QVERIFY(!bar->isVisible()); window.close();
    }
    void findNavigationOptionsAndUnicode()
    {
        QSettings{}.setValue("view/alignSourceRows", true);
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp"); r.theme_ = "light";
        const QByteArray text = QString::fromUtf8("Alpha alphabet alpha\r\n한글😀 Straße\r\nalpha .*").toUtf8();
        write(r.file1_, text); write(r.file2_, "prefix\n" + text);
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("FILE 1"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        auto* bar = window.findChild<FindBar*>(); auto* query = bar->findChild<QLineEdit*>("compareFindText");
        auto* scope = bar->findChild<QComboBox*>("compareFindScope"); auto* status = bar->findChild<QLabel*>("compareFindFeedback");
        auto* matchCase = bar->findChild<QCheckBox*>("compareFindCase"); auto* word = bar->findChild<QCheckBox*>("compareFindWord");
        QVERIFY(!bar->isVisible()); left->setFocus(); left->send(SCI_SETSEL, 0, 0);
        left->send(SCI_SETTARGETRANGE, 2, 5); left->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
        QTest::keyClick(left, Qt::Key_F, Qt::ControlModifier); QTRY_VERIFY(query->hasFocus());
        QCOMPARE(scope->currentText(), QString("FILE 1")); query->setText("alpha");
        QTest::keyClick(query, Qt::Key_Return); QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(0));
        QTRY_COMPARE(left->send(SCI_GETSELECTIONEND), sptr_t(5));
        QTest::keyClick(query, Qt::Key_F3); QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(6));
        word->setChecked(true); QTest::keyClick(query, Qt::Key_F3);
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.indexOf(" alpha\r") + 1));
        matchCase->setChecked(true); left->send(SCI_SETSEL, 0, 0); QTest::keyClick(query, Qt::Key_F3);
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.indexOf(" alpha\r") + 1));
        left->send(SCI_SETSEL, 0, 0); QTest::keyClick(query, Qt::Key_Return, Qt::ShiftModifier);
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.lastIndexOf("alpha"))); QCOMPARE(status->text(), QString("끝에서 계속 찾음"));
        matchCase->setChecked(false); QTest::keyClick(query, Qt::Key_F3);
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(0)); QCOMPARE(status->text(), QString("처음부터 계속 찾음"));
        word->setChecked(false); query->setText(QString::fromUtf8("한글😀")); QTest::keyClick(query, Qt::Key_Return);
        const auto unicode = QString::fromUtf8("한글😀").toUtf8();
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.indexOf(unicode)));
        QCOMPARE(left->send(SCI_GETSELECTIONEND), sptr_t(text.indexOf(unicode) + unicode.size()));
        query->setText("STRASSE"); QTest::keyClick(query, Qt::Key_Return);
        const auto street = QString::fromUtf8("Straße").toUtf8();
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.indexOf(street)));
        QCOMPARE(left->send(SCI_GETSELECTIONEND), sptr_t(text.indexOf(street) + street.size()));
        query->setText(".*"); QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(left->send(SCI_GETSELECTIONSTART), sptr_t(text.indexOf(".*")));
        const auto selection = left->send(SCI_GETSELECTIONSTART);
        query->setText("not-present"); QTest::keyClick(query, Qt::Key_Return); QTRY_COMPARE(status->text(), QString("일치 없음"));
        QCOMPARE(left->send(SCI_GETSELECTIONSTART), selection); QCOMPARE(left->send(SCI_GETTARGETSTART), sptr_t(2));
        QCOMPARE(left->send(SCI_GETTARGETEND), sptr_t(5)); QCOMPARE(left->send(SCI_GETSEARCHFLAGS), sptr_t(SCFIND_MATCHCASE));
        QTest::keyClick(query, Qt::Key_Escape); QTRY_VERIFY(!bar->isVisible()); QTRY_VERIFY(left->hasFocus());
        right->setFocus(); right->send(SCI_SETSEL, 0, 0); QTRY_COMPARE(scope->currentText(), QString("FILE 2"));
        query->setText(QString::fromUtf8("한글😀")); QTest::keyClick(right, Qt::Key_F3);
        QTRY_COMPARE(right->send(SCI_GETSELECTIONSTART), sptr_t(7 + text.indexOf(unicode)));
        QCOMPARE(left->send(SCI_GETSELECTIONSTART), selection);
        QVERIFY(bar->isVisible()); window.repaint(); QTest::qWait(40); QVERIFY(window.grab().save("find-light.png"));
        action(window, "Dark")->setChecked(true); window.repaint(); QTest::qWait(40); QVERIFY(window.grab().save("find-dark.png"));
        QTest::keyClick(right, Qt::Key_Escape); QTRY_VERIFY(!bar->isVisible());
        QCOMPARE(contents(left), text); QCOMPARE(contents(right), "prefix\n" + text);
        QVERIFY(!left->send(SCI_CANUNDO)); QVERIFY(!right->send(SCI_GETMODIFY)); window.close();
    }
    void findChunkBoundariesCancellationAndEmpty()
    {
        QMainWindow window; auto* editor = new ScintillaEditBase(&window); editor->setObjectName("TEXT"); window.setCentralWidget(editor);
        auto* bar = new FindBar(&window); window.addToolBar(Qt::BottomToolBarArea, bar);
        bar->SetEditors({editor}); window.resize(1000, 500); window.show();
        auto* query = bar->findChild<QLineEdit*>("compareFindText"); auto* status = bar->findChild<QLabel*>("compareFindFeedback");
        const auto pattern = QString::fromUtf8("한글😀").toUtf8(); const int chunk = 262144 / pattern.size();
        QByteArray text(3 * chunk, 'x'); for (int i = 79; i < text.size(); i += 80) text[i] = '\n';
        const int first = chunk - 3, last = 2 * chunk - 3; text.replace(first, pattern.size(), pattern); text.replace(last, pattern.size(), pattern);
        setText(*editor, text.toStdString()); query->setText(QString::fromUtf8(pattern)); bar->Find(false);
        QTRY_COMPARE(editor->send(SCI_GETSELECTIONSTART), sptr_t(first));
        editor->send(SCI_SETSEL, text.size(), text.size()); bar->Find(true);
        QTRY_COMPARE(editor->send(SCI_GETSELECTIONSTART), sptr_t(last));
        // A caret inside a match must still find it when wrapping in either direction.
        setText(*editor, "hello"); editor->send(SCI_SETSEL, 0, 5);
        editor->send(SCI_SETSELECTIONNCARETVIRTUALSPACE, 0, 4096); bar->Open();
        QCOMPARE(query->text(), QString("hello")); // Only document bytes seed the query.
        editor->send(SCI_SETSEL, 2, 2); bar->Find(false);
        QTRY_COMPARE(editor->send(SCI_GETSELECTIONEND), sptr_t(5)); QCOMPARE(status->text(), QString("처음부터 계속 찾음"));
        editor->send(SCI_SETSEL, 2, 2); bar->Find(true); QTRY_COMPARE(editor->send(SCI_GETSELECTIONSTART), sptr_t(0));
        QCOMPARE(status->text(), QString("끝에서 계속 찾음"));
        setText(*editor, ""); bar->Find(false); QTRY_COMPARE(status->text(), QString("일치 없음"));
        QByteArray large(4 * 1024 * 1024, 'a'); for (int i = 2047; i < large.size(); i += 2048) large[i] = '\n';
        setText(*editor, large.toStdString()); query->setText("retained"); editor->send(SCI_SELECTALL); bar->Open();
        QCOMPARE(query->text(), QString("retained")); // Large selection is not used as the query.
        editor->send(SCI_SETSEL, 0, 0); query->setText(QString(255, 'a') + 'b'); bar->Find(false);
        bool yielded = false; QElapsedTimer elapsed; elapsed.start();
        const auto connection = connect(bar->findChild<QTimer*>(), &QTimer::timeout, bar, [&] { yielded = true; bar->Close(); });
        QTRY_VERIFY(yielded); QVERIFY(!bar->isVisible()); QCOMPARE(editor->send(SCI_GETCURRENTPOS), sptr_t(0));
        disconnect(connection); qInfo("Search first chunk and close on 4 MiB repetitive text / 256-character query: %lld ms", elapsed.elapsed());
        query->setText("a"); bar->Find(false); editor->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("changed"));
        QTest::qWait(30); QVERIFY(status->text().isEmpty());
        bar->Find(false); bar->SetBusy(true); QTest::qWait(20); QCOMPARE(status->text(), QString("검색 중단"));
        bar->SetBusy(false); bar->Find(false); bar->SetEditors({}); delete window.takeCentralWidget();
        QTest::qWait(30); QVERIFY(!bar->isVisible()); bar->Find(false); // Deleted target is never dereferenced.
    }
    void findMergeResultAndRecompare()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.base_ = dir.filePath("base.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "head\nleft needle\nend"); write(r.file2_, "head\nright needle\nend"); write(r.base_, "head\nbase needle\nend");
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* left = window.findChild<ScintillaEditBase*>("LEFT");
        auto* base = window.findChild<ScintillaEditBase*>("BASE"); auto* bar = window.findChild<FindBar*>();
        auto* query = bar->findChild<QLineEdit*>("compareFindText"); auto* scope = bar->findChild<QComboBox*>("compareFindScope");
        auto* feedback = bar->findChild<QLabel*>("compareFindFeedback"); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QCOMPARE(scope->count(), 5); action(window, "왼쪽 전체 선택")->trigger();
        output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual needle\n")); output->send(SCI_SETSEL, 0, 0); output->setFocus();
        QTRY_COMPARE(scope->currentText(), QString("RESULT"));
        const auto before = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT); const auto resolution = tree->topLevelItem(0)->text(3);
        query->setText("needle"); bar->Find(false); QTRY_COMPARE(output->send(SCI_GETSELECTIONSTART), sptr_t(7));
        QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo); QCOMPARE(tree->topLevelItem(0)->text(3), resolution); QCOMPARE(contents(output), before);
        scope->setCurrentIndex(2); bar->Find(false); QTRY_COMPARE(base->send(SCI_GETSELECTIONSTART), sptr_t(10));
        QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        scope->setCurrentIndex(0); query->setText("missing"); bar->Find(false);
        write(r.file1_, "new needle\nhead\nleft needle\nend"); action(window, "다시 비교")->trigger();
        QVERIFY(!action(window, "다음 찾기")->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QCOMPARE(contents(output), before); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo); QVERIFY(feedback->text() != "검색 중…");
        left->send(SCI_SETSEL, 0, 0); query->setText("new needle"); bar->Find(false); QTRY_COMPARE(left->send(SCI_GETSELECTIONEND), sptr_t(10));
        action(window, "전체 수동 완료")->trigger(); action(window, "결과 저장")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        scope->setCurrentIndex(3); query->setText("needle"); bar->Find(false);
        QTRY_COMPARE(output->send(SCI_GETSELECTIONSTART), sptr_t(before.lastIndexOf("needle"))); QCOMPARE(window.ExitCode(), 0);
        QFile saved(r.output_); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), before); window.close();
    }
    void alignedSourceRowsAndEngineBoundaries()
    {
        const std::vector<std::pair<std::string, std::string>> cases{
            {"a\r\nb\nc\nz", "prefix\na\r\nB\nC\nD\nz\ntail"}, {"", "one\ntwo\n"}, {"one", ""},
            {"", ""}, {"a\n", "a\nextra"}, {"a", "a\nextra\n"}, {"a\nb\n", "b\n"},
            {"한글😀\r\n끝", "머리\n한글😀\r\n추가\n끝"}};
        for (const auto& [a, b] : cases)
        {
            ScintillaEditBase left, right; left.resize(400, 250); right.resize(400, 250);
            setText(left, a); setText(right, b); left.send(SCI_SETREADONLY, 1); right.send(SCI_SETREADONLY, 1);
            auto map = std::make_shared<AlignmentMap>(AlignmentMap::Build(SplitLines(a).size(), SplitLines(b).size(), Diff(a, b)));
            ScrollSynchronizer scroll(&left, &right, map, &left); scroll.SetAligned(true); scroll.SetEnabled(true);
            left.show(); right.show(); QTest::qWait(20);
            const std::array<ScintillaEditBase*, 2> editors{&left, &right};
            for (unsigned side = 0; side < 2; ++side)
            {
                auto* editor = editors[side];
                for (std::size_t line = 0; line < map->LineCount(side); ++line)
                    QCOMPARE(editor->send(SCI_VISIBLEFROMDOCLINE, line), sptr_t(map->DisplayRow(side, line)));
                // Both panes include one EOF display row, even without a final newline.
                const auto count = editor->send(SCI_GETLINECOUNT);
                QCOMPARE(editor->send(SCI_VISIBLEFROMDOCLINE, uptr_t(count)), sptr_t(map->RowCount() + 1));
                editor->send(SCI_SELECTALL);
                QByteArray selected(qsizetype(editor->send(SCI_GETSELTEXT)) + 1, '\0');
                editor->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(selected.data())); selected.chop(1);
                QCOMPARE(selected, QByteArray::fromStdString(side ? b : a));
                QCOMPARE(editor->send(SCI_GETMODIFY), sptr_t(0)); QCOMPARE(editor->send(SCI_CANUNDO), sptr_t(0));
            }
            // Enabling the observer does not choose a leader; establish the
            // initial common view as the window does before testing gestures.
            scroll.Synchronize(0);
            for (std::size_t row = 0; row <= map->RowCount(); ++row)
            {
                left.verticalScrollBar()->setValue(int(row));
                QTRY_VERIFY2_WITH_TIMEOUT(right.send(SCI_GETFIRSTVISIBLELINE) == sptr_t(row),
                    qPrintable(QString("a=%1 b=%2 row=%3 left=%4 right=%5 bar=%6")
                        .arg(QString::fromStdString(a), QString::fromStdString(b)).arg(row)
                        .arg(left.send(SCI_GETFIRSTVISIBLELINE)).arg(right.send(SCI_GETFIRSTVISIBLELINE)).arg(left.verticalScrollBar()->value())), 1000);
            }
            scroll.Reveal(0, 0); QCOMPARE(left.send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
            if (left.send(GetLeadingRows))
            {
                QCOMPARE(left.send(SCI_POSITIONFROMPOINTCLOSE, 80, 1), sptr_t(-1));
                QTest::mouseClick(left.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(80, 1));
                QCOMPARE(left.send(SCI_GETCURRENTPOS), sptr_t(0));
            }
            scroll.SetAligned(false);
            for (auto* editor : editors)
            {
                QCOMPARE(editor->send(GetLeadingRows), sptr_t(0));
                const auto count = editor->send(SCI_GETLINECOUNT);
                QCOMPARE(editor->send(SCI_VISIBLEFROMDOCLINE, uptr_t(count)), count);
            }
            QCOMPARE(contents(&left), QByteArray::fromStdString(a)); QCOMPARE(contents(&right), QByteArray::fromStdString(b));
        }
        // A large gap must neither overflow Scintilla's 16-bit annotation count
        // nor allocate/scan an annotation string for every painted blank row.
        ScintillaEditBase editor; setText(editor, "first\nlast"); editor.send(SCI_SETREADONLY, 1); editor.resize(500, 300); editor.show();
        QTest::qWait(20); QElapsedTimer timer; timer.start();
        QCOMPARE(editor.send(SetLeadingRows, 90000), sptr_t(0)); QCOMPARE(editor.send(SetTrailingRows, 0, 150000), sptr_t(0));
        QCOMPARE(editor.send(SCI_VISIBLEFROMDOCLINE, 1), sptr_t(240001));
        editor.send(SCI_SETFIRSTVISIBLELINE, 200000); editor.repaint(); QTest::qWait(20);
        QCOMPARE(editor.send(SCI_DOCLINEFROMVISIBLE, 200000), sptr_t(0));
        editor.send(SCI_STYLESETSIZE, STYLE_DEFAULT, 19); editor.repaint(); QTest::qWait(20);
        QCOMPARE(editor.send(SCI_VISIBLEFROMDOCLINE, 1), sptr_t(240001));
        qInfo("Sparse padding: 90000 leading + 150000 trailing rows, layout/paint/font change %lld ms (including 40 ms waits)", timer.elapsed());
        editor.send(ClearPaddingRows); QCOMPARE(editor.send(SCI_VISIBLEFROMDOCLINE, 1), sptr_t(1));
        QCOMPARE(editor.send(SetLeadingRows, 2000001), sptr_t(-1));
        editor.send(SCI_SETREADONLY, 0); QCOMPARE(editor.send(SetLeadingRows, 2), sptr_t(0));
        editor.send(ClearPaddingRows); editor.send(SCI_SETWRAPMODE, SC_WRAP_WORD); QCOMPARE(editor.send(SetLeadingRows, 2), sptr_t(-1));
        QCOMPARE(contents(&editor), QByteArray("first\nlast")); QCOMPARE(editor.send(SCI_CANUNDO), sptr_t(0));
    }
    void alignedWindowRecompareAndSave()
    {
        QSettings{}.remove("view/alignSourceRows"); // Product default: enabled.
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp"); r.theme_ = "light";
        const auto text = lines(0, 120); write(r.file1_, text);
        write(r.file2_, lines(0, 5, "prefix") + lines(0, 40) + lines(0, 7, "insert") + lines(40, 120));
        CompareWindow window(r); window.show(); window.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("LEFT"); auto* right = window.findChild<ScintillaEditBase*>("RIGHT");
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* cancel = action(window, "취소");
        auto* aligned = action(window, "왼쪽·오른쪽 표시 줄 정렬"); QVERIFY(aligned && aligned->isChecked());
        action(window, "왼쪽·오른쪽 스크롤 동기화")->setChecked(true);
        QTRY_COMPARE(left->send(SCI_VISIBLEFROMDOCLINE, 40), sptr_t(52));
        QCOMPARE(right->send(SCI_VISIBLEFROMDOCLINE, 52), sptr_t(52));
        left->verticalScrollBar()->setValue(47); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(47));
        const auto leftPos = left->send(SCI_POSITIONFROMLINE, 40), rightPos = right->send(SCI_POSITIONFROMLINE, 52);
        QCOMPARE(left->send(SCI_POINTYFROMPOSITION, 0, leftPos), right->send(SCI_POINTYFROMPOSITION, 0, rightPos));
        output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n"));
        const auto result = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT);
        action(window, "Dark")->setChecked(true); window.repaint(); QTest::qWait(40);
        QVERIFY(window.grab().save("aligned-rows-dark.png"));
        QCOMPARE(left->send(SCI_POINTYFROMPOSITION, 0, leftPos), right->send(SCI_POINTYFROMPOSITION, 0, rightPos));
        action(window, "Dark")->setChecked(false); left->verticalScrollBar()->setValue(0); window.repaint(); QTest::qWait(40);
        QVERIFY(window.grab().save("aligned-rows-light.png"));
        action(window, "다음 차이")->trigger();
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(45)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(45));
        action(window, "다음 차이")->trigger();
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        left->setFocus(); QTest::keyClick(left, Qt::Key_PageDown);
        QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), right->send(SCI_GETFIRSTVISIBLELINE));
        aligned->setChecked(false); QCOMPARE(left->send(SCI_VISIBLEFROMDOCLINE, 40), sptr_t(40));
        aligned->setChecked(true); QCOMPARE(left->send(SCI_VISIBLEFROMDOCLINE, 40), sptr_t(52));
        QCOMPARE(contents(output), result); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        write(r.file1_, text); write(r.file2_, lines(0, 3, "new") + text);
        action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(left->send(GetLeadingRows), sptr_t(3)); QCOMPARE(left->send(SCI_VISIBLEFROMDOCLINE, 40), sptr_t(43));
        QCOMPARE(contents(output), result); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        QVERIFY(QFile::rename(r.file2_, r.file2_ + ".away")); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000); QCOMPARE(left->send(SCI_VISIBLEFROMDOCLINE, 40), sptr_t(43));
        QVERIFY(QFile::rename(r.file2_ + ".away", r.file2_)); action(window, "전체 수동 완료")->trigger();
        action(window, "결과 저장")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), result);
        QCOMPARE(contents(left), text); window.close();
    }
    void sourceScrollCorrespondenceAndNavigation()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp");
        const auto leftText = lines(0, 260);
        const auto rightText = lines(0, 17, "prefix") + lines(0, 80) + lines(0, 7, "replacement") + lines(83, 160) + lines(170, 260) + lines(0, 9, "tail");
        write(r.file1_, leftText); write(r.file2_, rightText);
        CompareWindow window(r); window.show(); window.activateWindow();
        auto* tree = window.findChild<QTreeWidget*>("compareChanges"); QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 4, 10000);
        auto* left = window.findChild<ScintillaEditBase*>("FILE 1"); auto* right = window.findChild<ScintillaEditBase*>("FILE 2");
        QVERIFY(left && right); auto* sync = action(window, "왼쪽·오른쪽 스크롤 동기화"); QVERIFY(sync && sync->isEnabled());
        sync->setChecked(true); left->send(SCI_SETSEL, 2, 8); right->send(SCI_SETSEL, 4, 12);
        left->verticalScrollBar()->setValue(30);
        QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(47)); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(30));
        left->send(SCI_SETXOFFSET, 240); QTRY_COMPARE(right->send(SCI_GETXOFFSET), sptr_t(240));
        right->horizontalScrollBar()->setValue(120); QTRY_COMPARE(left->send(SCI_GETXOFFSET), sptr_t(120));
        QCOMPARE(left->send(SCI_GETCURRENTPOS), sptr_t(8)); QCOMPARE(right->send(SCI_GETCURRENTPOS), sptr_t(12));
        left->verticalScrollBar()->setValue(50); right->verticalScrollBar()->setValue(110);
        QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(89)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(110));
        // The reverse map of 81 is 99; delayed target notifications must not move 100 back to 99.
        right->verticalScrollBar()->setValue(100); QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(81));
        for (int i = 0; i < 3; ++i) { left->repaint(); right->repaint(); QTest::qWait(20); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(100)); }
        right->verticalScrollBar()->setValue(5); QTRY_COMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QTest::qWait(30); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(5));
        sync->setChecked(false); left->verticalScrollBar()->setValue(60); QTest::qWait(30);
        QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(5));
        left->setFocus(); sync->setChecked(true); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(77));
        // Navigation anchors both starts explicitly, including a zero-length BOF insertion.
        QTest::keyClick(&window, Qt::Key_F7, Qt::ShiftModifier); QTRY_COMPARE(tree->indexOfTopLevelItem(tree->currentItem()), 3);
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(260)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(271));
        QTest::keyClick(&window, Qt::Key_F7); QTRY_COMPARE(tree->indexOfTopLevelItem(tree->currentItem()), 0);
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QTest::qWait(30); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QTest::keyClick(&window, Qt::Key_F7); QTRY_COMPARE(tree->indexOfTopLevelItem(tree->currentItem()), 1);
        QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(80)); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(97));
        QTest::keyClick(&window, Qt::Key_F7, Qt::ShiftModifier); QTRY_COMPARE(tree->indexOfTopLevelItem(tree->currentItem()), 0);
        action(window, "다음 차이")->trigger(); left->setFocus(); QTest::keyClick(left, Qt::Key_PageDown);
        const auto map = AlignmentMap::Build(260, 280, Diff(leftText.toStdString(), rightText.toStdString()));
        QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(map.MapLine(0, std::size_t(left->send(SCI_GETFIRSTVISIBLELINE)))));
        const auto oldTop = left->send(SCI_GETFIRSTVISIBLELINE); const QPoint point(80, 80);
        QWheelEvent wheel(point, left->viewport()->mapToGlobal(point), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(left->viewport(), &wheel); QTRY_VERIFY(left->send(SCI_GETFIRSTVISIBLELINE) > oldTop);
        QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(map.MapLine(0, std::size_t(left->send(SCI_GETFIRSTVISIBLELINE)))));
        left->verticalScrollBar()->setValue(259); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(270));
        left->verticalScrollBar()->setValue(260); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(280));
        left->verticalScrollBar()->setValue(120); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(141));
        action(window, "Dark")->setChecked(false); QTest::qWait(50);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/scroll-sync-light.png"));
        action(window, "Dark")->setChecked(true); QTest::qWait(50);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/scroll-sync-dark.png"));
        QCOMPARE(contents(left), leftText); QCOMPARE(contents(right), rightText);
        left->send(SCI_SELECTALL); QByteArray selection(leftText.size() + 1, '\0');
        left->send(SCI_GETSELTEXT, 0, reinterpret_cast<sptr_t>(selection.data())); selection.chop(1); QCOMPARE(selection, leftText);
        QVERIFY(!left->send(SCI_GETMODIFY)); QVERIFY(!left->send(SCI_CANUNDO)); window.close();
    }
    void sourceScrollEmptyAndRecompare()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right"); r.output_ = dir.filePath("out");
        write(r.file1_, {}); write(r.file2_, lines(0, 100));
        CompareWindow window(r); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* left = window.findChild<ScintillaEditBase*>("LEFT"); auto* right = window.findChild<ScintillaEditBase*>("RIGHT");
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* cancel = action(window, "취소");
        right->verticalScrollBar()->setValue(50); QTest::qWait(50); QCOMPARE(left->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(50)); QCOMPARE(contents(left), QByteArray());
        action(window, "오른쪽 전체 선택")->trigger(); const auto resultText = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT);
        output->verticalScrollBar()->setValue(40); const auto resultTop = output->send(SCI_GETFIRSTVISIBLELINE);
        right->verticalScrollBar()->setValue(60); QTest::qWait(30); QCOMPARE(output->send(SCI_GETFIRSTVISIBLELINE), resultTop);
        action(window, "다음 차이")->trigger(); QCOMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(0));
        write(r.file1_, lines(0, 100)); write(r.file2_, lines(0, 20, "inserted") + lines(0, 100));
        action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        left->verticalScrollBar()->setValue(40); QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(60));
        QCOMPARE(contents(output), resultText); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo); QVERIFY(!action(window, "결과 저장")->isEnabled());
        // Failed recompare retains the old map, original documents and RESULT.
        QVERIFY(QFile::rename(r.file2_, r.file2_ + ".away")); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000); left->verticalScrollBar()->setValue(50);
        QTRY_COMPARE(right->send(SCI_GETFIRSTVISIBLELINE), sptr_t(70)); QCOMPARE(contents(output), resultText);
        QVERIFY(QFile::rename(r.file2_ + ".away", r.file2_)); action(window, "전체 수동 완료")->trigger();
        action(window, "결과 저장")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), resultText); window.close();
    }
    void workFileMenusRestoreAndUndo()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "head\na\nkeep\nz"); write(r.file2_, "head\nA\nkeep\nZ");
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        auto* status = window.findChild<QLabel*>("compareStatus"); auto* cancel = action(window, "취소"); auto* save = action(window, "결과 저장");
        auto* saveWork = action(window, "작업 저장..."); auto* loadWork = action(window, "작업 불러오기...");
        QVERIFY(saveWork && loadWork); QCOMPARE(saveWork->shortcut(), QKeySequence("Ctrl+Shift+S"));
        action(window, "블록 RIGHT")->trigger();
        const auto prefix = QString::fromUtf8("수동😀\r\n").toUtf8(); output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>(prefix.constData()));
        const auto text = contents(output); const auto before = output->send(SCI_GETUNDOCURRENT);
        const auto path = dir.filePath("session.hcmerge");
        chooseFile(saveWork, dir.filePath("session")); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QVERIFY2(QFile::exists(path), qPrintable(status->text() + " files: " + QDir(dir.path()).entryList(QDir::Files).join(',')));
        QVERIFY(!QFile::exists(r.output_)); QCOMPARE(window.ExitCode(), 3);
        QCOMPARE(output->send(SCI_GETUNDOCURRENT), before); QVERIFY(status->text().contains("작업 파일 저장 완료"));
        action(window, "왼쪽 전체 선택")->trigger(); save->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        chooseFile(loadWork, path); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(contents(output), text); QCOMPARE(tree->topLevelItem(0)->text(3), QString("RIGHT"));
        QCOMPARE(tree->topLevelItem(1)->text(3), QString("미해결")); QVERIFY(!save->isEnabled()); QCOMPARE(window.ExitCode(), 3);
        output->send(SCI_UNDO); QCOMPARE(contents(output), QByteArray("head\na\nkeep\nz")); QCOMPARE(window.ExitCode(), 0);
        output->send(SCI_REDO); QCOMPARE(contents(output), text); QVERIFY(!save->isEnabled());
        auto* fileMenu = window.menuBar()->actions().front()->menu(); fileMenu->popup(window.mapToGlobal(QPoint(8, 48))); QTest::qWait(70);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/work-light.png"));
        QVERIFY(fileMenu->grab().save(QCoreApplication::applicationDirPath() + "/work-menu-light.png")); fileMenu->hide();
        action(window, "Dark")->setChecked(true); QTest::qWait(70);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/work-dark.png"));
        fileMenu->popup(window.mapToGlobal(QPoint(8, 48))); QTest::qWait(70);
        QVERIFY(fileMenu->grab().save(QCoreApplication::applicationDirPath() + "/work-menu-dark.png")); fileMenu->hide();
        // A malformed file, changed original, and stale checkpoint leave Undo and text intact.
        const auto position = output->send(SCI_GETUNDOCURRENT); const auto bad = dir.filePath("bad.hcmerge"); write(bad, "{broken");
        window.LoadWork(bad); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), position); QVERIFY(status->text().contains("처리 중단"));
        window.LoadWork(path); cancel->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QVERIFY(status->text().contains("Canceled")); QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), position);
        write(r.file2_, "external input"); window.LoadWork(path); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QVERIFY(status->text().contains("InputChangedBeforeWorkLoad")); QCOMPARE(output->send(SCI_GETUNDOCURRENT), position);
        write(path, "external checkpoint"); window.SaveWork(path, true); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QVERIFY(status->text().contains("처리 중단")); QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), position);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("external checkpoint"));
        output->send(SCI_UNDO); QCOMPARE(window.ExitCode(), 0); window.close();
    }
    void workReviewReopenAndOutputExpectation()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left"); r.file2_ = dir.filePath("right");
        r.base_ = dir.filePath("base"); r.output_ = dir.filePath("out");
        write(r.file1_, "base\n"); write(r.file2_, "base\n"); write(r.base_, "base\n"); const auto path = dir.filePath("review.hcmerge");
        {
            CompareWindow window(r); window.show(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
            auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* cancel = action(window, "취소");
            write(r.file2_, "new independent\n"); action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
            QVERIFY(!action(window, "결과 저장")->isEnabled()); window.SaveWork(path); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
            QVERIFY(QFile::exists(path)); QVERIFY(!QFile::exists(r.output_));
        }
        CompareWindow reopened(r); reopened.show(); QTRY_VERIFY_WITH_TIMEOUT(reopened.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = reopened.findChild<ScintillaEditBase*>("RESULT"); auto* cancel = action(reopened, "취소"); auto* save = action(reopened, "결과 저장");
        QCOMPARE(contents(output), QByteArray("new independent\n")); reopened.LoadWork(path); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(contents(output), QByteArray("base\n")); QVERIFY(!save->isEnabled());
        output->send(SCI_UNDO); QCOMPARE(contents(output), QByteArray("new independent\n")); QVERIFY(save->isEnabled());
        output->send(SCI_REDO); QVERIFY(!save->isEnabled()); action(reopened, "전체 수동 완료")->trigger(); QVERIFY(save->isEnabled());
        // Loading a checkpoint must not refresh the output revision captured when comparison began.
        write(r.output_, "external output"); save->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!output->send(SCI_GETREADONLY), 10000);
        QCOMPARE(reopened.ExitCode(), 2); QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("external output"));
    }
    void workControllerRestoresReviewAndSavepoint()
    {
        ScintillaEditBase editor; setText(editor, "left\n");
        auto plan = std::make_shared<MergePlan>(TwoWayPlan("left\n", "right\n", Diff("left\n", "right\n")));
        MergeController controller(&editor, plan, nullptr); controller.ChooseAll(Resolution::Left, "left\n"); controller.MarkSaved();
        auto states = controller.Model().State(); states[0] = {0, 0, Resolution::Manual, false};
        controller.RestoreWork("manual\r\n", states, true); QVERIFY(!controller.Saved()); QVERIFY(controller.Model().ReviewRequired());
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray("left\n")); QVERIFY(controller.Saved());
        editor.send(SCI_REDO); QCOMPARE(controller.Model().State(), states); QVERIFY(controller.Model().ReviewRequired());
    }
    void twoWayLightDark()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("old.cpp"); r.file2_ = dir.filePath("new.cpp");
        write(r.file1_, "// original\nint add(int a, int b) {\n    return a + b;\n}\n");
        write(r.file2_, "// updated\nint add(int a, int b) {\n    return a + b + 1;\n}\n");
        CompareWindow window(r); window.show(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QTRY_VERIFY_WITH_TIMEOUT(tree->topLevelItemCount() > 0, 10000);
        QCOMPARE(window.findChildren<ScintillaEditBase*>().size(), qsizetype(2));
        for (auto* editor : window.findChildren<ScintillaEditBase*>()) { QVERIFY(editor->isVisible()); QVERIFY(editor->width() > 100); QVERIFY(editor->height() > 200); }
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/compare-light.png"));
        auto* dark = action(window, "Dark"); QVERIFY(dark); dark->setChecked(true); QTest::qWait(100);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/compare-dark.png"));
        window.close();
    }
    void recomparePreservesManualUndoAndFailure()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "left\n"); write(r.file2_, "right\n");
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT");
        auto* status = window.findChild<QLabel*>("compareStatus"); auto* retry = action(window, "다시 비교");
        auto* cancel = action(window, "취소"); auto* save = action(window, "결과 저장");
        action(window, "블록 RIGHT")->trigger();
        output->send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("manual\n"));
        const auto text = contents(output); const auto undo = output->send(SCI_GETUNDOCURRENT);
        retry->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(window.findChild<ScintillaEditBase*>("RESULT"), output); QCOMPARE(contents(output), text);
        QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo); QVERIFY(status->text().contains("입력 내용 동일"));
        output->send(SCI_UNDO); QCOMPARE(contents(output), QByteArray("right\n")); QVERIFY(save->isEnabled());
        output->send(SCI_REDO); QCOMPARE(contents(output), text); QVERIFY(!save->isEnabled());
        QVERIFY(QFile::rename(r.file2_, r.file2_ + ".away"));
        retry->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        QVERIFY(status->text().contains("재비교 중단")); QVERIFY(!output->send(SCI_GETREADONLY));
        QVERIFY(QFile::rename(r.file2_ + ".away", r.file2_));
        retry->trigger(); cancel->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(contents(output), text); QCOMPARE(output->send(SCI_GETUNDOCURRENT), undo);
        action(window, "전체 수동 완료")->trigger(); save->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        retry->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!cancel->isEnabled(), 10000);
        QCOMPARE(window.ExitCode(), 0); QCOMPARE(contents(output), text); window.close();
    }
    void recompareChangedInputsRequiresReview()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp");
        const auto middle = QByteArray("unchanged line\n").repeated(20);
        write(r.file1_, "a\n" + middle + "z\n"); write(r.file2_, "A\n" + middle + "Z\n");
        CompareWindow window(r); window.show(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 10000);
        action(window, "오른쪽 전체 선택")->trigger();
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); const auto text = contents(output);
        write(r.file1_, "a\n" + middle + "new tail\n");
        action(window, "다시 비교")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QCOMPARE(contents(output), text); QCOMPARE(tree->topLevelItem(0)->text(3), QString("RIGHT"));
        QVERIFY(tree->topLevelItem(1)->text(3).contains("위치 재검토"));
        QCOMPARE(contents(window.findChild<ScintillaEditBase*>("LEFT")), QByteArray("a\n" + middle + "new tail\n"));
        auto* save = action(window, "결과 저장"); QVERIFY(!save->isEnabled());
        tree->setCurrentItem(tree->topLevelItem(1)); QVERIFY(!action(window, "블록 RIGHT")->isEnabled());
        QTest::qWait(60);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/recompare-review.png"));
        // Old generation's native text Undo is usable but cannot resurrect trusted decisions.
        output->send(SCI_UNDO); QVERIFY(!save->isEnabled());
        output->send(SCI_REDO); QCOMPARE(contents(output), text); QVERIFY(!save->isEnabled());
        action(window, "전체 수동 완료")->trigger(); QVERIFY(save->isEnabled());
        output->send(SCI_UNDO); QVERIFY(!save->isEnabled()); output->send(SCI_REDO); QVERIFY(save->isEnabled());
        save->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), text); window.close();
    }
    void recompareWithoutConflictStillRequiresReview()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.base_ = dir.filePath("base.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "base\n"); write(r.file2_, "base\n"); write(r.base_, "base\n");
        CompareWindow window(r); window.show();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ScintillaEditBase*>("RESULT"), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); auto* save = action(window, "결과 저장");
        write(r.file2_, "new independent change\n"); action(window, "다시 비교")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!action(window, "취소")->isEnabled(), 10000);
        QCOMPARE(contents(output), QByteArray("base\n"));
        QCOMPARE(window.findChild<QTreeWidget*>("compareChanges")->topLevelItemCount(), 1); QVERIFY(!save->isEnabled());
        action(window, "오른쪽 전체 선택")->trigger(); QCOMPARE(contents(output), QByteArray("new independent change\n"));
        output->send(SCI_UNDO); QVERIFY(!save->isEnabled()); output->send(SCI_REDO); QVERIFY(save->isEnabled());
        save->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000); window.close();
    }
    void emptyRightMergeAndSave()
    {
        QTemporaryDir dir; Request r; r.file1_ = dir.filePath("left.cpp"); r.file2_ = dir.filePath("right.cpp");
        r.output_ = dir.filePath("result.cpp"); r.mode_ = "merge"; r.takeAll_ = "right";
        write(r.file1_, "left\n"); write(r.file2_, {});
        CompareWindow window(r); window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<ScintillaEditBase*>().size(), qsizetype(3), 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output); QCOMPARE(output->send(SCI_GETLENGTH), sptr_t(0));
        auto* choose = action(window, QString::fromUtf8("왼쪽 전체 선택")); QVERIFY(choose); choose->trigger();
        auto* save = action(window, QString::fromUtf8("결과 저장")); QVERIFY(save); save->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("left\n"));
        window.close();
    }
    void nativeUndoRestoresDecisions()
    {
        const std::string left = "a\nkeep\nz\n", right = "A\nextra\nkeep\nZ\n";
        ScintillaEditBase editor; setText(editor, left);
        auto plan = std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right)));
        MergeController controller(&editor, plan, nullptr);
        const auto initial = controller.Model().State();
        controller.Choose(1, controller.Model().Revision(), Resolution::Right);
        const auto chosen = controller.Model().State(); QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(1));
        QCOMPARE(contents(&editor), QByteArray("A\nextra\nkeep\nz\n"));
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray::fromStdString(left)); QCOMPARE(controller.Model().State(), initial);
        editor.send(SCI_REDO); QCOMPARE(controller.Model().State(), chosen);
        editor.send(SCI_INSERTTEXT, 1, reinterpret_cast<sptr_t>("manual"));
        QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(2));
        editor.send(SCI_UNDO); QCOMPARE(controller.Model().State(), chosen);
        editor.send(SCI_REDO); QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(2));
        controller.Choose(1, controller.Model().Revision(), Resolution::Manual);
        controller.Choose(2, controller.Model().Revision(), Resolution::Left);
        controller.MarkSaved(); QVERIFY(controller.Saved());
        editor.send(SCI_UNDO); QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(1)); QVERIFY(!controller.Saved());
        editor.send(SCI_REDO); QVERIFY(controller.Saved());
        editor.send(SCI_UNDO); editor.send(SCI_UNDO);
        QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(2));
        // Branching after Undo must discard the old decision states as well as native Redo.
        controller.ChooseAll(Resolution::Left, QByteArray::fromStdString(left)); QVERIFY(!editor.send(SCI_CANREDO));
        editor.send(SCI_UNDO); QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(2));
        editor.send(SCI_REDO); QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(0));
    }
    void manualCrossBoundaryAndUtf8()
    {
        const auto left = QString::fromUtf8("가\r\n유지\r\n끝").toUtf8().toStdString();
        const auto right = QString::fromUtf8("나\r\n추가\r\n유지\r\n마침").toUtf8().toStdString();
        ScintillaEditBase editor; setText(editor, left);
        MergeController controller(&editor, std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right))), nullptr);
        controller.Choose(1, controller.Model().Revision(), Resolution::Right);
        const auto before = controller.Model().State(); const auto text = contents(&editor);
        // Remove across both block boundaries; no remaining block may overwrite ambiguous text.
        editor.send(SCI_SETTARGETSTART, 3); editor.send(SCI_SETTARGETEND, uptr_t(editor.send(SCI_GETLENGTH)));
        editor.send(SCI_REPLACETARGET, 0, reinterpret_cast<sptr_t>(""));
        QVERIFY(!controller.Model().State()[0].mapped_); QVERIFY(!controller.Model().State()[1].mapped_);
        QVERIFY_EXCEPTION_THROWN(controller.Choose(1, controller.Model().Revision(), Resolution::Right), Failure);
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), text); QCOMPARE(controller.Model().State(), before);
        controller.Choose(2, controller.Model().Revision(), Resolution::Right);
        QCOMPARE(contents(&editor), QByteArray::fromStdString(right));
    }
    void twoWayBlockWindow()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.file1_, "a\nkeep\nz\n"); write(r.file2_, "A\nextra\nkeep\nZ\n");
        CompareWindow window(r); window.show(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 10000);
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output);
        auto* save = action(window, QString::fromUtf8("결과 저장")); QVERIFY(save); QVERIFY(!save->isEnabled());
        action(window, QString::fromUtf8("블록 RIGHT"))->trigger();
        tree->setCurrentItem(tree->topLevelItem(1)); action(window, QString::fromUtf8("블록 LEFT"))->trigger();
        QCOMPARE(contents(output), QByteArray("A\nextra\nkeep\nz\n")); QVERIFY(save->isEnabled()); save->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000);
        output->setFocus(); QTest::keyClick(output, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(!save->isEnabled()); QCOMPARE(window.ExitCode(), 3);
        output->send(SCI_REDO); QVERIFY(save->isEnabled()); QCOMPARE(window.ExitCode(), 0);
        QFile file(r.output_); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("A\nextra\nkeep\nz\n"));
        window.close();
    }
    void emptyBlocksAndPrefixEdits()
    {
        const std::string left = "head\nkeep\nend", right = "head\ninsert\nkeep\nEND";
        ScintillaEditBase editor; setText(editor, left);
        MergeController controller(&editor, std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right))), nullptr);
        const auto initial = controller.Model().State();
        QCOMPARE(initial[0].begin_, initial[0].end_);
        editor.send(SCI_INSERTTEXT, 0, reinterpret_cast<sptr_t>("prefix\n"));
        QCOMPARE(controller.Model().State()[0].begin_, initial[0].begin_ + 7);
        controller.Choose(1, controller.Model().Revision(), Resolution::Right);
        controller.Choose(2, controller.Model().Revision(), Resolution::Right);
        QCOMPARE(contents(&editor), QByteArray("prefix\nhead\ninsert\nkeep\nEND"));
        controller.Choose(1, controller.Model().Revision(), Resolution::Left);
        QCOMPARE(contents(&editor), QByteArray("prefix\nhead\nkeep\nEND"));
        editor.send(SCI_UNDO); editor.send(SCI_UNDO); editor.send(SCI_UNDO); editor.send(SCI_UNDO);
        QCOMPARE(contents(&editor), QByteArray::fromStdString(left)); QCOMPARE(controller.Model().State(), initial);
        // Joining both sides at an empty BASE must preserve missing final newlines.
        const auto merged = Merge("", "left", "right");
        ScintillaEditBase gap; setText(gap, "");
        MergeController gapController(&gap, std::make_shared<MergePlan>(ThreeWayPlan("", "left", "right", merged)), nullptr);
        gapController.Choose(1, gapController.Model().Revision(), Resolution::RightThenLeft);
        QCOMPARE(contents(&gap), QByteArray("rightleft")); gap.send(SCI_UNDO);
        QCOMPARE(contents(&gap), QByteArray{}); QCOMPARE(gapController.Model().UnresolvedCount(), std::size_t(1));
    }
    void boundedDecisionHistory()
    {
        const std::string left = "old\n", right = "new\n";
        ScintillaEditBase editor; setText(editor, left);
        MergeController controller(&editor, std::make_shared<MergePlan>(TwoWayPlan(left, right, Diff(left, right))), nullptr, 1);
        controller.Choose(1, controller.Model().Revision(), Resolution::Right);
        editor.send(SCI_UNDO); QCOMPARE(contents(&editor), QByteArray::fromStdString(left));
        QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(1)); QVERIFY(!controller.Model().State()[0].mapped_);
        QVERIFY_EXCEPTION_THROWN(controller.Choose(1, controller.Model().Revision(), Resolution::Right), Failure);
        controller.ChooseAll(Resolution::Manual); QCOMPARE(contents(&editor), QByteArray::fromStdString(left));
        QCOMPARE(controller.Model().UnresolvedCount(), std::size_t(0));
    }
    void threeWayBlockWindow()
    {
        QTemporaryDir dir; Request r; r.mode_ = "merge"; r.file1_ = dir.filePath("left.cpp");
        r.file2_ = dir.filePath("right.cpp"); r.base_ = dir.filePath("base.cpp"); r.output_ = dir.filePath("out.cpp");
        write(r.base_, "head\nbase1\nmiddle\nbase2\nend\n"); write(r.file1_, "head\nleft1\nmiddle\nleft2\nend\n");
        write(r.file2_, "head\nright1\nmiddle\nright2\nend\n");
        CompareWindow window(r); window.show(); auto* tree = window.findChild<QTreeWidget*>("compareChanges");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 2, 10000);
        action(window, "LEFT → RIGHT")->trigger();
        tree->setCurrentItem(tree->topLevelItem(1)); action(window, QString::fromUtf8("블록 BASE"))->trigger();
        auto* output = window.findChild<ScintillaEditBase*>("RESULT"); QVERIFY(output);
        QCOMPARE(contents(output), QByteArray("head\nleft1\nright1\nmiddle\nbase2\nend\n"));
        auto* save = action(window, QString::fromUtf8("결과 저장")); QVERIFY(save->isEnabled());
        output->send(SCI_UNDO); QVERIFY(!save->isEnabled()); output->send(SCI_REDO); QVERIFY(save->isEnabled());
        auto* dark = action(window, "Dark"); dark->setChecked(false); QTest::qWait(60);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/merge-blocks-light.png"));
        dark->setChecked(true); QTest::qWait(60);
        QVERIFY(window.grab().save(QCoreApplication::applicationDirPath() + "/merge-blocks-dark.png"));
        save->trigger(); QTRY_COMPARE_WITH_TIMEOUT(window.ExitCode(), 0, 10000); window.close();
    }
};
QTEST_MAIN(CompareUiTests)
#include "tstCompareUi.moc"
