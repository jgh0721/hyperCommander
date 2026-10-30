#include "stdafx.h"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/markdownDocumentView.hpp"
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "text/QTextView.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QFile>
#include <QListWidget>
#include <QFontComboBox>
#include <QScreen>
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "core/AppSettings.h"
using namespace hc::document;
class MarkdownTests final : public QObject
{
    Q_OBJECT
    QTemporaryDir config_;
    TextInput input()
    {
        TextInput I;
        I.path_ = config_.filePath("guide.md");
        I.text_ = "# 제목\n\n## 반복\n\n본문 $x^2$\n\n## 반복\n\n```mermaid\ngraph LR\n A-->B\n```\n\n```cpp\nint "
                  "main() {}\n```\n\n```text\n# 제목 아님\n```\n";
        I.bytes_ = I.text_.toUtf8();
        I.encoding_ = "UTF-8";
        I.eol_ = "LF";
        return I;
    }
  private slots:
    void initTestCase()
    {
        QVERIFY(config_.isValid());
        qApp->setProperty("documentViewsConfigDir", config_.path());
        QVERIFY2(DocumentShortcutRegistry::Validate({}).isEmpty(), qPrintable(DocumentShortcutRegistry::Validate({})));
    }
    void readerAndSource()
    {
        DocumentViewPane Pane;
        Pane.resize(1100, 800);
        QVERIFY(Pane.Load(input(), true, false));
        Pane.show();
        auto *Md = Pane.findChild<MarkdownDocumentView *>();
        QVERIFY(Md);
        QVERIFY(!Md->SourceVisible());
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 15000);
        QCOMPARE(Md->State()["problems"].toInt(), 0);
        auto *Tree = Md->findChild<QTreeWidget *>();
        QCOMPARE(Tree->topLevelItemCount(), 1);
        QCOMPARE(Tree->topLevelItem(0)->childCount(), 2);
        const auto Original = Pane.Content();
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));
        QVERIFY(Md->SourceVisible());
        QCOMPARE(Pane.Content(), Original);
        QVERIFY(Pane.RunViewCommand("markdown.toggleSource"));
        for (const auto &F : Pane.StatusFields())
            QVERIFY(F.id_ != "position" && F.id_ != "encoding");
        Pane.raise();
        Pane.activateWindow();
        QTest::qWait(500);
        Pane.screen()
            ->grabWindow(0, Pane.mapToGlobal(QPoint()).x(), Pane.mapToGlobal(QPoint()).y(), Pane.width(), Pane.height())
            .save(QDir::currentPath() + "/_build/markdown-implementation/viewer.png");
    }
    void splitAndSync()
    {
        DocumentViewPane Pane;
        Pane.resize(1280, 850);
        QVERIFY(Pane.Load(input(), true, true));
        Pane.show();
        auto *Md = Pane.findChild<MarkdownDocumentView *>();
        QCOMPARE(Md->State()["mode"].toString(), QString("split"));
        QVERIFY(Pane.RunViewCommand("markdown.positionSync"));
        QVERIFY(!Md->CanRun("markdown.syncScroll"));
        QVERIFY(Md->State()["scrollSync"].toBool());
        QVERIFY(Pane.RunViewCommand("markdown.positionSync"));
        QVERIFY(Md->CanRun("markdown.syncScroll"));
        QVERIFY(Pane.RunViewCommand("markdown.syncScroll"));
        QVERIFY(!Md->State()["scrollSync"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 30000);
        Pane.raise();
        Pane.activateWindow();
        QTest::qWait(500);
        Pane.screen()
            ->grabWindow(0, Pane.mapToGlobal(QPoint()).x(), Pane.mapToGlobal(QPoint()).y(), Pane.width(), Pane.height())
            .save(QDir::currentPath() + "/_build/markdown-implementation/editor.png");
        auto *Text = Pane.findChild<QTextView *>();
        QVERIFY(Text);
        const auto Original = Pane.Content();
        Text->ReplaceContent("# 수정\n\n$$invalid\\unknown$$\n");
        QTRY_VERIFY_WITH_TIMEOUT(Md->State()["renderedRevision"].toInteger(-2) == Md->State()["revision"].toInteger(-1),
                                 30000);
        QVERIFY(Md->State()["problems"].toInt() > 0);
        QVERIFY(Pane.Content() != Original);
        QVERIFY(Pane.RunViewCommand("markdown.preview"));
        QVERIFY(Pane.Content().contains("# "));
        QVERIFY(!Pane.Content().contains("<html"));
        Pane.grab().save(config_.filePath("editor.png"));
    }
    void settings()
    {
        AppSettings Stored;
        const auto Path = Stored.fileName();
        {
            DocumentSettingsDialog Dialog(Path, {});
            Dialog.resize(1100, 950);
            Dialog.show();
            auto *List = Dialog.findChild<QListWidget *>();
            QVERIFY(List);
            List->setCurrentRow(4);
            auto *Enabled = Dialog.findChild<QCheckBox *>("documentViews/markdown/previewFonts/enabled");
            auto *Mode = Dialog.findChild<QComboBox *>("documentViews/markdown/previewFonts/mode");
            auto *Font = Dialog.findChild<QFontComboBox *>("documentViews/markdown/previewFonts/bodyFamily");
            auto *Position = Dialog.findChild<QCheckBox *>("documentViews/markdown/positionSync");
            auto *Scroll = Dialog.findChild<QCheckBox *>("documentViews/markdown/scrollSync");
            QVERIFY(Enabled && Mode && Font && Position && Scroll);
            QTest::qWait(50);
            QVERIFY(!Enabled->isChecked());
            QVERIFY(!Mode->isEnabled());
            QVERIFY(!Font->isEnabled());
            Enabled->setChecked(true);
            Mode->setCurrentIndex(Mode->findData("user"));
            QVERIFY(Font->isEnabled());
            Scroll->setChecked(true);
            Position->setChecked(false);
            QVERIFY(!Scroll->isEnabled());
            QVERIFY(Scroll->isChecked());
            Dialog.grab().save(QDir::currentPath() + "/_build/markdown-implementation/settings.png");
            QVERIFY(Dialog.Apply());
        }
        AppSettings Reloaded;
        QVERIFY(Reloaded.value("markdown/previewFonts/enabled").toBool());
        QCOMPARE(Reloaded.value("markdown/previewFonts/mode").toString(), QString("user"));
        QVERIFY(!Reloaded.value("markdown/positionSync").toBool());
        QVERIFY(Reloaded.value("markdown/scrollSync").toBool());
        {
            DocumentSettingsDialog Cancel(Path, {});
            Cancel.findChild<QCheckBox *>("documentViews/markdown/previewFonts/enabled")->setChecked(false);
            Cancel.reject();
        }
        AppSettings AfterCancel;
        QVERIFY(AfterCancel.value("markdown/previewFonts/enabled").toBool());
    }
};
QTEST_MAIN(MarkdownTests)
#include "tstMarkdown.moc"
