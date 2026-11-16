#include "stdafx.h"
#include "tstEnvironmentEditors.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/environmentEditors.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/EnvironmentInputs.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Button.h>
#include <fmfilelist/PathValueEditor.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <fmwidgets/Label.h>
#include <QSignalSpy>
#include <QClipboard>
#include <QApplication>
#include <QAbstractItemModelTester>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentEditorTests, "hc.tests.environmentEditors")
using namespace hc::environment;
void EnvironmentEditorTests::draftAndReadOnly()
{
    EnvironmentValueEditor Editor;
    const Value Original{L"Synthetic", L"before", ValueType::String, Scope::User};
    QVERIFY(Editor.SetValue(Original));
    auto* Text = Editor.findChild<fm::ui::TextField*>("hc.environment.text"); QVERIFY(Text);
    auto* Save = Editor.findChild<fm::ui::Button*>("hc.environment.save"); QVERIFY(Save);
    Text->setText("after"); QVERIFY(Editor.IsModified()); QVERIFY(Save->isEnabled());
    QVERIFY(!Editor.SetValue({L"Other", L"untouched", ValueType::String, Scope::User}));
    QCOMPARE(Editor.Draft().type_, ValueType::String);
    QSignalSpy Requested(&Editor, &EnvironmentValueEditor::SaveRequested); Save->click(); QCOMPARE(Requested.count(), 1);
    Editor.SetBusy(true); QVERIFY(!Save->isEnabled()); Editor.Discard(); QVERIFY(Editor.IsModified());
    Editor.SetBusy(false); Editor.Discard(); QVERIFY(!Editor.IsModified()); QCOMPARE(Editor.Draft(), Original);
    QVERIFY(Editor.SetValue(Original, true)); QVERIFY(!Text->isEnabled()); QVERIFY(!Save->isEnabled());
}
void EnvironmentEditorTests::variantsAndMask()
{
    EnvironmentValueEditor Editor; Editor.resize(680, 400); Editor.show();
    const Value Secret{L"SYNTHETIC_TOKEN", L"invented-secret", ValueType::ExpandString, Scope::User};
    QVERIFY(Editor.SetValue(Secret));
    auto* Mask = Editor.findChild<fm::ui::MaskedValueEdit*>("hc.environment.masked"); QVERIFY(Mask);
    QCOMPARE(Mask->editor()->echoMode(), QLineEdit::Password); QVERIFY(!Mask->revealed());
    QApplication::clipboard()->setText("unchanged"); Mask->editor()->selectAll();
    QTest::keyClick(Mask->editor(), Qt::Key_C, Qt::ControlModifier); QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
    auto* Reveal = Mask->findChild<fm::ui::Button*>(); QVERIFY(Reveal);
    QTest::mousePress(Reveal, Qt::LeftButton); QVERIFY(Mask->revealed());
    QTest::mouseRelease(Reveal, Qt::LeftButton); QVERIFY(!Mask->revealed());
    QVERIFY(Editor.SetValue(Secret, true)); QVERIFY(Mask->editor()->isReadOnly()); QVERIFY(Reveal->isEnabled());
    QTest::mousePress(Reveal, Qt::LeftButton); QVERIFY(Mask->revealed());
    QTest::mouseRelease(Reveal, Qt::LeftButton); QVERIFY(!Mask->revealed());
    for (const auto& Name : {L"TEMP", L"Path", L"PATHEXT", L"JAVA_HOME", L"Plain"}) {
        const Value Item{Name, L"C:\\Synthetic;D:\\Synthetic;", ValueType::ExpandString, Scope::User};
        QVERIFY(Editor.SetValue(Item)); QCOMPARE(Editor.Draft(), Item); QVERIFY(!Editor.IsModified());
    }
    const Value Known{L"SyntheticChoice", L"custom-value", ValueType::String, Scope::User};
    QVERIFY(Editor.SetValue(Known, false, false, {"yes", "no"})); QCOMPARE(Editor.Draft(), Known);
    QVERIFY(Mask->text().isEmpty());
}
void EnvironmentEditorTests::pathKeyboardAndMoves()
{
    hc::ui::PathListEditor Editor; Editor.resize(720, 420); Editor.show();
    QVERIFY(Editor.SetValue("A;;B;"));
    auto* List = Editor.findChild<fm::filelist::PathValueEditor*>("hc.pathList.rows"); QVERIFY(List);
    QAbstractItemModelTester Tester(Editor.Model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    List->setCurrentIndex(Editor.Model()->index(0));
    auto* Target = List->focusProxy(); QVERIFY(Target); Target->setFocus();
    QTest::keyClick(Target, Qt::Key_Down); QCOMPARE(List->currentIndex().row(), 1); QCOMPARE(Editor.Value(), QString("A;;B;"));
    QTest::keyClick(Target, Qt::Key_Down, Qt::ControlModifier); QCOMPARE(List->currentIndex().row(), 2); QCOMPARE(Editor.Value(), QString("A;B;;"));
    QVERIFY(List->MoveRow(2, 0)); QCOMPARE(Editor.Value(), QString(";A;B;")); QCOMPARE(List->currentIndex().row(), 0);
    QVERIFY(!List->MoveRow(0, -1)); QCOMPARE(Editor.Value(), QString(";A;B;"));
    QVERIFY(Editor.Model()->Insert(1, "C")); QVERIFY(Editor.Model()->Remove(2));
    QCOMPARE(Editor.Value(), QString(";C;B;"));
}
void EnvironmentEditorTests::folderProbeIsInjected()
{
    EnvironmentValueEditor Editor; Editor.show();
    const Value Folder{L"SYNTHETIC_FOLDER", L"C:\\Synthetic", ValueType::String, Scope::User};
    QVERIFY(Editor.SetValue(Folder, false, false, {}, true));
    int Calls = 0;
    Editor.SetResolver([&](const QString& Raw) {
        ++Calls; hc::ui::PathResolution Result; Result.expanded = Raw;
        Result.status = hc::ui::PathResolution::Status::Exists; Result.warning = "synthetic-expected-file"; return Result;
    });
    auto* Status = Editor.findChild<fm::ui::Label*>("hc.environment.folderStatus"); QVERIFY(Status);
    QTRY_VERIFY(Status->text().contains("synthetic-expected-file")); QVERIFY(Calls > 0);
    QCOMPARE(Editor.Draft(), Folder); QVERIFY(!Editor.IsModified());
}
QTEST_MAIN(EnvironmentEditorTests)
