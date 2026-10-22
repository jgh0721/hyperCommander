#include "stdafx.h"
#include "widgets/compareWindow.hpp"
#include "widgets/mergeController.hpp"
#include "widgets/scrollSynchronizer.hpp"
#include "widgets/scintillaPadding.hpp"
#include "widgets/findBar.hpp"
#include <QtTest>
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
        settings.setValue("view/inlineHighlights", true);
        settings.setValue("find/caseSensitive", false); settings.setValue("find/wholeWord", false);
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
        QCOMPARE(scope->count(), 4); action(window, "왼쪽 전체 선택")->trigger();
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
        editor.send(SCI_SETREADONLY, 0); QCOMPARE(editor.send(SetLeadingRows, 2), sptr_t(-1));
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
