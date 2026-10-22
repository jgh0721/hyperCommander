#include "stdafx.h"
#include "widgets/compareWindow.hpp"
#include "widgets/mergeController.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QLabel>
#include <QTreeWidget>
#include <QAction>
#include <fmstyle/ThemeManager.h>
#include <ScintillaEditBase.h>

using namespace hc::compare;
class CompareUiTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir settings_;
    static void write(const QString& Path, const QByteArray& Text)
    { QFile file(Path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(Text), Text.size()); }
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
private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("HyperCompareTests"); QCoreApplication::setApplicationName("UI");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        auto& theme = fm::style::ThemeManager::instance(); theme.setDesign(fm::style::Design::Watercolor); theme.install(*qApp);
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
        QCOMPARE(window.findChild<QTreeWidget*>("compareChanges")->topLevelItemCount(), 0); QVERIFY(!save->isEnabled());
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
