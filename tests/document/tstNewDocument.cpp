#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include <QtEndian>
#include <QAction>
#include <QBuffer>
#include <QDir>
#include <QMessageBox>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <type_traits>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QToolBar>
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "core/AppSettings.h"
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/document/epub/epubTemplate.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/markdownDocumentView.hpp"
#include "UniqueLibs/document/rstDocumentView.hpp"
#include "UniqueLibs/document/rstProjectDialogs.hpp"
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "UniqueLibs/document/newDocumentChooser.hpp"
#include "UniqueLibs/document/newEpubDialog.hpp"
#include "UniqueLibs/document/hwpInputInspection.hpp"
#include "UniqueLibs/document/documentViewKind.hpp"
#include "UniqueLibs/settings/appSettings.hpp"

using namespace hc::document;
namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Bytes(const QString& Path)
    { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    QJsonArray Documents(const DocumentWorkspaceWidget& Widget) { return Widget.Snapshot()["documents"].toArray(); }
    QJsonObject Document(const DocumentWorkspaceWidget& Widget, const QString& Id)
    { for (auto Item : Documents(Widget)) if (Item.toObject()["documentId"].toString() == Id) return Item.toObject(); return {}; }
    bool Ready(const DocumentWorkspaceWidget& Widget, const QString& Id)
    { return Document(Widget, Id)["lifecycle"].toString() == "ready"; }
    QString Name(const DocumentWorkspaceWidget& Widget, const QString& Id) { return Document(Widget, Id)["displayName"].toString(); }
    // Settings and recovery live in a temporary product directory for each test.
    struct Profile
    {
        QTemporaryDir root_;
        QVariant previous_ = qApp->property("documentViewsConfigDir");
        explicit Profile(const QByteArray& Views = "{}")
        {
            qApp->setProperty("documentViewsConfigDir", root_.path());
            Put(hc::settings::UnifiedConfig::FileName(root_.path()), R"({"schemaVersion":2,"documentViews":)" + Views + "}");
        }
        ~Profile() { qApp->setProperty("documentViewsConfigDir", previous_); }
        QString Path(const QString& Name) const { return root_.filePath(Name); }
    };
    QStringList Entries(const QByteArray& Zip)
    {
        // Local file headers in order (stored entries only, as the template writes them).
        QStringList Names; qsizetype At = 0;
        while (At + 30 <= Zip.size() && qFromLittleEndian<quint32>(Zip.constData() + At) == 0x04034b50) {
            const qsizetype Size = qFromLittleEndian<quint32>(Zip.constData() + At + 18), NameSize = qFromLittleEndian<quint16>(Zip.constData() + At + 26), Extra = qFromLittleEndian<quint16>(Zip.constData() + At + 28);
            Names << QString::fromUtf8(Zip.mid(At + 30, NameSize)); At += 30 + NameSize + Extra + Size;
        }
        return Names;
    }
}
class NewDocumentTests final : public QObject
{
    Q_OBJECT
private slots:
    void epubTemplateIsAValidStoredPackage()
    {
        epub::NewBook Book; Book.title_ = QStringLiteral("바다의 <기억> & \"노래\""); Book.creator_ = QStringLiteral("한결"); Book.language_ = "ko";
        Book.chapterTitle_ = QStringLiteral("1장");
        const auto Output = epub::CreateBook(Book); QVERIFY2(Output, qPrintable(Output.error_ + " " + Output.location_));
        const auto Names = Entries(Output.bytes_);
        QCOMPARE(Names.value(0), QString("mimetype"));
        QCOMPARE(qFromLittleEndian<quint16>(Output.bytes_.constData() + 8), quint16(0));   // stored
        QVERIFY(Names.contains("OEBPS/content.opf")); QVERIFY(Names.contains("OEBPS/nav.xhtml")); QVERIFY(Names.contains("OEBPS/Styles/book.css"));
        epub::EditModel Model; QVERIFY2(Model.Load(Output.bytes_), qPrintable(Model.Error()));
        const auto Values = Model.Metadata()["values"].toObject();
        QCOMPARE(Values["title"].toString(), Book.title_); QCOMPARE(Values["creator"].toString(), Book.creator_); QCOMPARE(Values["language"].toString(), QString("ko"));
        QCOMPARE(Model.Publication()["spine"].toArray().size(), 1);
        const auto Chapter = QString::fromUtf8(Model.Read(epub::NewBookFirstChapter));
        QVERIFY(Chapter.contains("<p></p>")); QVERIFY(Chapter.contains("../Styles/book.css")); QVERIFY(Chapter.contains(QStringLiteral("<h1>1장</h1>")));
        QVERIFY(QString::fromUtf8(Model.Read("OEBPS/nav.xhtml")).contains(QStringLiteral("1장")));
        // A chapter added later keeps the book's stylesheet and takes the next free number.
        QVERIFY(Model.Structure({{"operation", "chapter-add"}, {"resource", "OEBPS/Text/chapter-002.xhtml"}, {"title", QStringLiteral("2장")}, {"after", 0}}).isEmpty());
        QVERIFY(QString::fromUtf8(Model.Read("OEBPS/Text/chapter-002.xhtml")).contains("../Styles/book.css"));
    }
    void epubTemplateCoverAndRejections()
    {
        QImage Image(120, 180, QImage::Format_RGB32); Image.fill(Qt::darkBlue);
        QByteArray Png; QBuffer Buffer(&Png); Buffer.open(QIODevice::WriteOnly); QVERIFY(Image.save(&Buffer, "PNG"));
        epub::NewBook Book; Book.title_ = "Cover"; Book.language_ = "en-US"; Book.chapterTitle_ = "One"; Book.stylesheet_ = false; Book.coverImage_ = Png;
        const auto Output = epub::CreateBook(Book); QVERIFY2(Output, qPrintable(Output.error_ + " " + Output.location_));
        epub::EditModel Model; QVERIFY(Model.Load(Output.bytes_));
        QCOMPARE(Model.Publication()["spine"].toArray().size(), 2);
        QVERIFY(!Entries(Output.bytes_).contains("OEBPS/Styles/book.css"));
        QVERIFY(QString::fromUtf8(Model.Read(Model.Publication()["rootfile"].toString())).contains("cover-image"));
        auto Bad = Book; Bad.title_ = "  "; QCOMPARE(epub::CreateBook(Bad).error_, QString("edit-new-title"));
        Bad = Book; Bad.language_ = "한국어"; QCOMPARE(epub::CreateBook(Bad).error_, QString("edit-new-language"));
        Bad = Book; Bad.coverImage_ = "not an image"; QCOMPARE(epub::CreateBook(Bad).error_, QString("edit-new-cover-image"));
        Bad = Book; Bad.chapterTitle_ = QString("a\x01b"); QCOMPARE(epub::CreateBook(Bad).error_, QString("edit-new-chapter-title"));
    }
    void epubFileNames()
    {
        QCOMPARE(NewEpubFileName(QStringLiteral("a/b:c*?.  ")), QStringLiteral("a b c.epub"));
        QCOMPARE(NewEpubFileName("CON"), QStringLiteral("CON_.epub"));
        QCOMPARE(NewEpubFileName("   "), QObject::tr("제목 없는 책") + ".epub");
        QTemporaryDir Folder; QVERIFY(Put(Folder.filePath("book.epub"), "x")); QVERIFY(Put(Folder.filePath("book (2).epub"), "x"));
        QCOMPARE(UniqueEpubFileName(Folder.path(), "book.epub"), QStringLiteral("book (3).epub"));
    }
    void untitledNumbersAndNoGuid()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); }); Widget.show();
        QStringList Ids;
        for (int I = 0; I < 3; ++I) { Ids << Widget.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Ids.last())); }
        QCOMPARE(Name(Widget, Ids[0]), QObject::tr("새 문서 %1").arg(1)); QCOMPARE(Name(Widget, Ids[2]), QObject::tr("새 문서 %1").arg(3));
        QVERIFY(!Widget.Handle({{"action", "close"}, {"documentId", Ids[1]}}).contains("error")); QTRY_COMPARE(Documents(Widget).size(), 2);
        // Text and Markdown share one sequence; the lowest free number comes back first.
        const auto Markdown = Widget.Handle({{"action", "new"}, {"kind", "markdown"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Markdown));
        QCOMPARE(Name(Widget, Markdown), QObject::tr("새 문서 %1.md").arg(2));
        QVERIFY(Widget.findChild<MarkdownDocumentView*>());
        const auto Image = Widget.Handle({{"action", "new"}, {"kind", "image"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Image));
        QCOMPARE(Name(Widget, Image), QObject::tr("새 이미지 %1").arg(1)); QVERIFY(!Document(Widget, Image)["dirty"].toBool());
        for (const auto& Item : Documents(Widget)) QVERIFY2(!Item.toObject()["displayName"].toString().contains("untitled"), qPrintable(Item.toObject()["displayName"].toString()));
        auto* Tabs = Widget.findChild<QTabWidget*>("documentTabs");
        for (int I = 0; I < Tabs->count(); ++I) QVERIFY(!Tabs->tabText(I).contains("untitled"));
    }
    void untitledSaveAsAdoptsFile()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show();
        const auto Id = Widget.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "body"}}).contains("error"));
        const auto Output = Settings.Path("saved.txt");
        QVERIFY(!Widget.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Output}}).contains("error"));
        // New documents use the save defaults (UTF-8 with BOM by default).
        QTRY_VERIFY(Widget.IsIdle()); QVERIFY(Bytes(Output).endsWith("body"));
        QCOMPARE(Name(Widget, Id), QString("saved.txt"));
        // The temporary source is gone and the next untitled document reuses number 1.
        QVERIFY(QDir(Settings.Path("recovery")).entryList({"untitled-*"}, QDir::Files).isEmpty());
        const auto Next = Widget.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Next));
        QCOMPARE(Name(Widget, Next), QObject::tr("새 문서 %1").arg(1));
        QCOMPARE(QDir::cleanPath(hc::settings::UnifiedConfig::Value(QJsonDocument::fromJson(Bytes(hc::settings::UnifiedConfig::FileName(Settings.root_.path()))).object()["documentViews"].toObject(),
            {"common", "lastSaveFolder"}).toString()), QDir::cleanPath(Settings.root_.path()));
    }
    void saveAsSwitchesTextAndMarkdown()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show();
        const auto Id = Widget.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "# Title\r\nbody\r\n"}}).contains("error"));
        QPointer<QTextView> Editor = Widget.findChild<QTextView*>(); QVERIFY(Editor); QVERIFY(!Widget.findChild<MarkdownDocumentView*>());
        const auto SaveAs = [&](const QString& Name) {
            QVERIFY(!Widget.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Settings.Path(Name)}}).contains("error"));
            QTRY_VERIFY(Widget.IsIdle()); QTRY_VERIFY(!Document(Widget, Id)["dirty"].toBool());
        };
        // Text → Markdown: the same editor (content, caret, Undo) moves into the Markdown view.
        SaveAs("note.md");
        QTRY_VERIFY(Widget.findChild<MarkdownDocumentView*>()); QCOMPARE(Widget.findChild<QTextView*>(), Editor.data());
        QCOMPARE(Editor->Content(), QString("# Title\r\nbody\r\n")); QCOMPARE(Editor->currentLanguage(), QString("Markdown"));
        // Markdown → text: the Markdown view goes, the editor stays.
        SaveAs("note.txt");
        QTRY_VERIFY(!Widget.findChild<MarkdownDocumentView*>()); QVERIFY(Editor); QCOMPARE(Widget.findChild<QTextView*>(), Editor.data());
        QVERIFY(Widget.findChild<QToolBar*>("viewToolbar")); QCOMPARE(Editor->Content(), QString("# Title\r\nbody\r\n"));
        // Another text extension only changes the highlighting; a non-text extension (.epub) keeps the text view.
        const auto Before = Editor->currentLanguage();
        SaveAs("data.json"); QVERIFY(Editor->currentLanguage() != Before); QVERIFY(!Widget.findChild<MarkdownDocumentView*>());
        SaveAs("book.epub"); QCOMPARE(Widget.findChild<QTextView*>(), Editor.data()); QVERIFY(!Widget.findChild<MarkdownDocumentView*>());
        // And back to Markdown once more after the round trip.
        SaveAs("again.markdown"); QTRY_VERIFY(Widget.findChild<MarkdownDocumentView*>()); QCOMPARE(Widget.findChild<QTextView*>(), Editor.data());
    }
    void untitledMarkdownHotExitKeepsKindAndNumber()
    {
        Profile Settings(R"({"textView":{"hotExitPolicyVersion":1,"hotExitEnabled":true,"recoveryEnabled":false}})");
        const auto Recovery = Settings.Path("recovery"); QString Window; const auto Draft = QStringLiteral("# 제목\r\n본문\r\n");
        {
            DocumentWorkspaceWidget Widget(Recovery); Widget.show();
            const auto First = Widget.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, First));
            const auto Id = Widget.Handle({{"action", "new"}, {"kind", "markdown"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
            QVERIFY(!Widget.Handle({{"action", "close"}, {"documentId", First}}).contains("error")); QTRY_COMPARE(Documents(Widget).size(), 1);
            QCOMPARE(Name(Widget, Id), QObject::tr("새 문서 %1.md").arg(2));
            QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", Draft}}).contains("error"));
            Window = Widget.Snapshot()["workspaceId"].toString();
            QVERIFY(!Widget.RequestClose()); QTRY_VERIFY_WITH_TIMEOUT(Widget.RequestClose(), 10000);
        }
        DocumentWorkspaceWidget Restored(Recovery); Restored.show(); QVERIFY(Restored.RestoreHotExit(Window));
        QTRY_COMPARE(Documents(Restored).size(), 1);
        const auto Id = Documents(Restored).first().toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Restored, Id));
        // D8: number 2 survives (not renumbered to 1), and the Markdown view comes back (2.7).
        QCOMPARE(Name(Restored, Id), QObject::tr("새 문서 %1.md").arg(2));
        QVERIFY(Restored.findChild<MarkdownDocumentView*>());
        QCOMPARE(Restored.findChild<QTextView*>()->Content(), Draft);
        QVERIFY(Document(Restored, Id)["dirty"].toBool());
    }
    void recoveryRecordFieldsAndOldRecords()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); RecoverySnapshot Record;
        Record.document_.id_ = std::string(32, 'e'); Record.document_.generation_ = 2; Record.document_.revision_ = 3; Record.bytes_ = "text";
        Record.text_ = TextRecoveryState{}; Record.text_->suffix_ = "md"; Record.text_->untitledNumber_ = 4; Record.text_->firstSubLine_ = 2; Record.text_->untitled_ = true;
        Record.ownerWindow_ = std::string(32, 'f');
        auto Stored = Store.Store(Record); QVERIFY(Stored.HasValue());
        auto Read = Store.Read(Stored.Value().snapshotPath_); QVERIFY(Read.HasValue()); QVERIFY(Read.Value().text_);
        QCOMPARE(Read.Value().text_->suffix_, std::string("md")); QCOMPARE(Read.Value().text_->untitledNumber_, std::int64_t(4)); QCOMPARE(Read.Value().text_->firstSubLine_, std::int64_t(2));
        // A record written before these fields (nine-field text state) still reads, as unnumbered text.
        const auto Path = QString::fromStdString(Stored.Value().snapshotPath_);
        auto Json = QJsonDocument::fromJson(Bytes(Path)).object(); auto Text = Json["text"].toObject();
        Text.remove("suffix"); Text.remove("untitledNumber"); Text.remove("firstSubLine"); Json["text"] = Text;
        QVERIFY(Put(Path, QJsonDocument(Json).toJson(QJsonDocument::Compact)));
        Read = Store.Read(Stored.Value().snapshotPath_); QVERIFY(Read.HasValue()); QVERIFY(Read.Value().text_);
        QCOMPARE(Read.Value().text_->suffix_, std::string("txt")); QCOMPARE(Read.Value().text_->untitledNumber_, std::int64_t(0));
    }
    void chooserKeysCreateKinds()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show(); QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        auto* New = Widget.findChild<QAction*>("newDocument"); QVERIFY(New);
        const auto Visible = [&Widget](auto* Type) -> decltype(Type) {
            for (auto* Item : Widget.findChildren<std::remove_pointer_t<decltype(Type)>*>()) if (Item->isVisible()) return Item;
            return nullptr;
        };
        New->trigger();
        QPointer<NewDocumentChooser> Chooser; QTRY_VERIFY((Chooser = Visible(static_cast<NewDocumentChooser*>(nullptr))));
        // The product dialog frame: header, cards, footer buttons.
        QVERIFY(Chooser->findChild<QWidget*>("hc.dialog.header")); QVERIFY(Chooser->findChild<QWidget*>("hc.dialog.footer"));
        QVERIFY(Chooser->findChild<QAbstractButton*>("newDocument.hwpx"));
        QCOMPARE(NewDocumentKindFrom("hwpx"), NewDocumentKind::Hwpx);
        QCOMPARE(NewDocumentKindId(NewDocumentKind::Epub), QString("epub"));
        // A single click only selects (dialog convention); the button then names what follows.
        auto* Epub = Chooser->findChild<QAbstractButton*>("newDocument.epub"); QVERIFY(Epub);
        QTest::mouseClick(Epub, Qt::LeftButton); QVERIFY(Epub->isChecked()); QVERIFY(Chooser->isVisible());
        QVERIFY(Chooser->findChild<QPushButton*>("newDocument.create")->text().contains(QObject::tr("다음")));
        QTest::keyClick(Chooser->focusWidget() ? Chooser->focusWidget() : Chooser.data(), Qt::Key_2);
        QTRY_COMPARE(Documents(Widget).size(), 1); QTRY_VERIFY(!Chooser);
        const auto Id = Documents(Widget).first().toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QCOMPARE(Name(Widget, Id), QObject::tr("새 문서 %1.md").arg(1));
        // The dialog starts on the last kind; file.new again while it is open creates that kind.
        New->trigger(); QTRY_VERIFY((Chooser = Visible(static_cast<NewDocumentChooser*>(nullptr))));
        New->trigger();
        QTRY_COMPARE(Documents(Widget).size(), 2);
        QCOMPARE(Name(Widget, Documents(Widget).last().toObject()["documentId"].toString()), QObject::tr("새 문서 %1.md").arg(2));
        // Image: chosen in ‘새 문서’, it continues in its own ‘새 이미지’ dialog; Enter creates the remembered size.
        New->trigger(); QTRY_VERIFY((Chooser = Visible(static_cast<NewDocumentChooser*>(nullptr))));
        // 이미지는 일곱 번째 카드다( 텍스트 · Markdown · Sphinx 프로젝트 · reStructuredText · 아래한글 · EPUB · 이미지 ).
        QTest::keyClick(Chooser->focusWidget() ? Chooser->focusWidget() : Chooser.data(), Qt::Key_7);
        QPointer<NewImageDialog> Image; QTRY_VERIFY((Image = Visible(static_cast<NewImageDialog*>(nullptr))));
        QVERIFY(Image->findChild<QSpinBox*>("newImageWidth")); QVERIFY(!Image->findChild<QSpinBox*>("newImageWidth")->isEnabled());
        QTest::keyClick(Image->focusWidget() ? Image->focusWidget() : Image.data(), Qt::Key_Return);
        QTRY_COMPARE(Documents(Widget).size(), 3);
        const auto Created = Documents(Widget).last().toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Created));
        QCOMPARE(Name(Widget, Created), QObject::tr("새 이미지 %1").arg(1));
        // file.newImage opens the image dialog directly.
        Widget.findChild<QAction*>("file.newImage")->trigger(); QTRY_VERIFY((Image = Visible(static_cast<NewImageDialog*>(nullptr))));
        Image->reject(); QTRY_VERIFY(!Image); QCOMPARE(Documents(Widget).size(), 3);
    }
    void reStructuredTextKindsFollowMarkdown()
    {
        // 시안 ⑥: Markdown 아래에 Sphinx 프로젝트 · reStructuredText 문서. 순서가 키( 1 – 7 )다.
        QCOMPARE(int(NewDocumentKind::Markdown), 1); QCOMPARE(int(NewDocumentKind::SphinxProject), 2); QCOMPARE(int(NewDocumentKind::Rst), 3);
        QCOMPARE(NewDocumentKindId(NewDocumentKind::SphinxProject), QString("sphinx")); QCOMPARE(NewDocumentKindFrom("rst"), NewDocumentKind::Rst);
        QCOMPARE(NewDocumentKindFrom("image"), NewDocumentKind::Image);
        // 미리보기 환경은 시험용 빈 폴더를 본다: 사용자의 실제 환경을 쓰거나 내려받지 않는다.
        QTemporaryDir Runtime; QVERIFY(Runtime.isValid()); qputenv("HC_RST_RUNTIME_DIR", Runtime.path().toUtf8());
        const auto Restore = qScopeGuard([] { qunsetenv("HC_RST_RUNTIME_DIR"); });
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show(); QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        const auto Visible = [&Widget](auto* Type) -> decltype(Type) {
            for (auto* Item : Widget.findChildren<std::remove_pointer_t<decltype(Type)>*>()) if (Item->isVisible()) return Item;
            return nullptr;
        };
        auto* New = Widget.findChild<QAction*>("newDocument"); QVERIFY(New);
        New->trigger();
        QPointer<NewDocumentChooser> Chooser; QTRY_VERIFY((Chooser = Visible(static_cast<NewDocumentChooser*>(nullptr))));
        QVERIFY(Chooser->findChild<QAbstractButton*>("newDocument.sphinx")); QVERIFY(Chooser->findChild<QAbstractButton*>("newDocument.rst"));
        // 4: reStructuredText 문서. 이름은 '새 문서 N.rst', 보기는 reST 보기다.
        QTest::keyClick(Chooser->focusWidget() ? Chooser->focusWidget() : Chooser.data(), Qt::Key_4);
        QTRY_COMPARE(Documents(Widget).size(), 1);
        const auto Id = Documents(Widget).first().toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QCOMPARE(Name(Widget, Id), QObject::tr("새 문서 %1.rst").arg(1));
        auto* Rst = Widget.findChild<RstDocumentView*>(); QVERIFY(Rst);
        QCOMPARE(Rst->State()["mode"].toString(), QString("split")); QVERIFY(Rst->SourceVisible()); QVERIFY(Rst->PreviewVisible());
        // 환경이 없으면 미리보기 자리에 구성 안내가 있고, 스스로 내려받지 않는다.
        QVERIFY(!Rst->State()["environmentReady"].toBool());
        // reST 명령은 그 보기에서만 켜진다.
        auto* Source = Widget.findChild<QAction*>("rst.source"); QVERIFY(Source); QTRY_VERIFY(Source->isEnabled());
        Source->trigger(); QTRY_COMPARE(Rst->State()["mode"].toString(), QString("source")); QVERIFY(!Rst->PreviewVisible());
        // file.newRst 는 창 없이 바로 만든다.
        Widget.findChild<QAction*>("file.newRst")->trigger(); QTRY_COMPARE(Documents(Widget).size(), 2);
        QCOMPARE(Name(Widget, Documents(Widget).last().toObject()["documentId"].toString()), QObject::tr("새 문서 %1.rst").arg(2));
        // 3: Sphinx 프로젝트는 이어지는 대화상자에서 만든다.
        New->trigger(); QTRY_VERIFY((Chooser = Visible(static_cast<NewDocumentChooser*>(nullptr))));
        QTest::keyClick(Chooser->focusWidget() ? Chooser->focusWidget() : Chooser.data(), Qt::Key_3);
        QPointer<NewSphinxProjectDialog> Sphinx; QTRY_VERIFY((Sphinx = Visible(static_cast<NewSphinxProjectDialog*>(nullptr))));
        QVERIFY(Sphinx->findChild<QWidget*>("hc.dialog.header"));
        Sphinx->reject(); QTRY_VERIFY(!Sphinx); QCOMPARE(Documents(Widget).size(), 2);
    }
    void reStructuredTextDocumentInWorkspace()
    {
        QTemporaryDir Runtime, Project; QVERIFY(Runtime.isValid() && Project.isValid());
        qputenv("HC_RST_RUNTIME_DIR", Runtime.path().toUtf8());
        const auto Restore = qScopeGuard([] { qunsetenv("HC_RST_RUNTIME_DIR"); });
        // conf.py 가 있는 폴더: 그 아래의 .rst 는 그 프로젝트의 문서다( 워크스페이스 폴더 없이 파일에서 위로 찾는다 ).
        QVERIFY(Put(Project.filePath("conf.py"), "project = 'Demo'\nhtml_theme = 'alabaster'\n"));
        QVERIFY(Put(Project.filePath("index.rst"), "Demo\n====\n\n.. toctree::\n\n   guide/install\n\nSection\n-------\n\ntext\n"));
        QVERIFY(QDir().mkpath(Project.filePath("guide")));
        QVERIFY(Put(Project.filePath("guide/install.rst"), "Install\n=======\n\none\n\ntwo\n\nthree\n\nfour\n"));
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.resize(1100, 760); Widget.show(); QVERIFY(QTest::qWaitForWindowExposed(&Widget));
        const auto Index = Widget.Handle({{"action", "open"}, {"path", Project.filePath("index.rst")}})["documentId"].toString(); QVERIFY(!Index.isEmpty());
        QTRY_VERIFY(Ready(Widget, Index));
        auto* Rst = Widget.findChild<RstDocumentView*>(); QVERIFY(Rst);
        // 프로젝트를 찾았고 가상 프로젝트가 아니다. 프로젝트 폴더에는 아무것도 만들지 않는다( .multiroot 없음 ).
        QTRY_VERIFY(!Rst->State()["projectId"].toString().isEmpty()); QVERIFY(!Rst->State()["virtual"].toBool());
        QVERIFY(!QFileInfo::exists(Project.filePath(".multiroot")));
        // 개요: 환경이 없어도 문서의 제목은 나온다.
        QTRY_VERIFY(Rst->State()["outlineHeadings"].toInt() >= 1);
        // reST(R) 메뉴와 편집 · 보기 항목.
        QVERIFY(Widget.findChild<QMenu*>("rstModeMenu")); QVERIFY(Widget.findChild<QAction*>("rstEdit.complete")); QVERIFY(Widget.findChild<QAction*>("rstView.fullScreen"));
        QVERIFY(!Widget.findChild<QMenu*>("markdownModeMenu"));
        // 환경이 없으므로 빌드는 꺼져 있고, 프로젝트 폴더 열기는 켜져 있다.
        QVERIFY(!Rst->CanRun("rst.buildProject")); QVERIFY(Rst->CanRun("rst.openConf"));

        // 다른 문서의 줄로 열기( 개요 · 진단 · 참조 · 미리보기 클릭이 쓰는 길 ).
        emit Rst->OpenAtRequested(Project.filePath("guide/install.rst"), 7);
        QTRY_COMPARE(Documents(Widget).size(), 2);
        const auto Install = Documents(Widget).last().toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Install));
        // 새 탭이 앞에 나오고 그 소스 편집기가 보인다.
        QTextView* Text = nullptr;
        const auto Shown = [&Widget, &Text] {
            for (auto* View : Widget.findChildren<QTextView*>()) if (View->isVisible() && View->currentFilePath().endsWith("install.rst")) { Text = View; return true; }
            return false;
        };
        QTRY_VERIFY(Shown());
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        QTRY_COMPARE(Backend->lineFromPosition(Backend->currentPos()) + 1, 7);
        // 이미 열려 있는 문서면 탭을 더 만들지 않고 그 줄로 간다.
        RstDocumentView* Active = nullptr;
        for (auto* View : Widget.findChildren<RstDocumentView*>()) if (View->isVisible()) Active = View;
        QVERIFY(Active); QVERIFY(Active != Rst);
        emit Active->OpenAtRequested(Project.filePath("guide/install.rst"), 3);
        QTRY_COMPARE(Backend->lineFromPosition(Backend->currentPos()) + 1, 3); QCOMPARE(Documents(Widget).size(), 2);

        // 미리보기 전체 화면: 창이 전체 화면이 되고 그 탭의 미리보기만 남는다. 나오면 들어가기 전 보기로 돌아간다.
        QCOMPARE(Active->State()["mode"].toString(), QString("split"));
        auto* Full = Widget.findChild<QAction*>("rst.fullScreen"); QVERIFY(Full); QTRY_VERIFY(Full->isEnabled());
        Full->trigger();
        QTRY_VERIFY(Widget.window()->isFullScreen()); QVERIFY(Active->IsPreviewOnly()); QVERIFY(!Active->SourceVisible());
        // 전체 화면에서는 보기 방식을 바꾸지 않는다.
        QVERIFY(!Active->Run("rst.source")); QVERIFY(Active->IsPreviewOnly());
        Full->trigger();
        QTRY_VERIFY(!Widget.window()->isFullScreen()); QVERIFY(!Active->IsPreviewOnly()); QCOMPARE(Active->State()["mode"].toString(), QString("split"));
        // 소스만 보던 중이면 미리보기로 바꿔 들어가고, 나올 때 소스 보기로 돌아온다.
        QVERIFY(Active->Run("rst.source")); QCOMPARE(Active->State()["mode"].toString(), QString("source"));
        Full->trigger(); QTRY_VERIFY(Widget.window()->isFullScreen()); QVERIFY(Active->PreviewVisible());
        Full->trigger(); QTRY_VERIFY(!Widget.window()->isFullScreen()); QCOMPARE(Active->State()["mode"].toString(), QString("source"));

        // Markdown 에도 같은 명령이 있다.
        Widget.findChild<QAction*>("file.newMarkdown")->trigger(); QTRY_COMPARE(Documents(Widget).size(), 3);
        QTRY_VERIFY(Ready(Widget, Documents(Widget).last().toObject()["documentId"].toString()));
        MarkdownDocumentView* Markdown = nullptr;
        const auto MarkdownShown = [&Widget, &Markdown] {
            for (auto* View : Widget.findChildren<MarkdownDocumentView*>()) if (View->isVisible()) { Markdown = View; return true; }
            return false;
        };
        QTRY_VERIFY(MarkdownShown());
        auto* MdFull = Widget.findChild<QAction*>("markdown.fullScreen"); QVERIFY(MdFull); QTRY_VERIFY(MdFull->isEnabled());
        MdFull->trigger(); QTRY_VERIFY(Widget.window()->isFullScreen()); QVERIFY(Markdown->IsPreviewOnly()); QVERIFY(!Markdown->SourceVisible());
        MdFull->trigger(); QTRY_VERIFY(!Widget.window()->isFullScreen()); QVERIFY(!Markdown->IsPreviewOnly()); QVERIFY(Markdown->SourceVisible());
    }
    void sphinxProjectFilesAreWrittenOnce()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        QCOMPARE(SphinxFolderName(QStringLiteral("Demo Docs 2")), QString("demo-docs-2"));
        QCOMPARE(SphinxFolderName(QStringLiteral("문서")), QString("docs"));
        SphinxProjectSpec Spec; Spec.name_ = QStringLiteral("Demo's Docs"); Spec.author_ = QStringLiteral("작성자"); Spec.directory_ = Root.path();
        Spec.folder_ = QStringLiteral("demo"); Spec.theme_ = QStringLiteral("furo"); Spec.myst_ = true;
        QVERIFY2(Spec.Error().isEmpty(), qPrintable(Spec.Error()));
        QString Index; QCOMPARE(CreateSphinxProject(Spec, &Index), QString());
        QCOMPARE(QDir::cleanPath(Index), QDir::cleanPath(Root.filePath("demo/index.rst")));
        const auto Conf = QString::fromUtf8(Bytes(Root.filePath("demo/conf.py")));
        QVERIFY(Conf.contains("html_theme = 'furo'")); QVERIFY(Conf.contains("extensions = ['myst_parser']")); QVERIFY(Conf.contains("language = 'ko'"));
        // 이름의 작은따옴표는 Python 문자열을 끊지 않는다.
        QVERIFY(Conf.contains("project = ")); QVERIFY(!Conf.contains("project = 'Demo's Docs'"));
        QVERIFY(QFileInfo(Root.filePath("demo/_static")).isDir()); QVERIFY(QFileInfo(Root.filePath("demo/_templates")).isDir());
        // 제목 밑줄은 제목보다 짧지 않다.
        const auto Lines = QString::fromUtf8(Bytes(Index)).split(QLatin1Char(0x0a)); QVERIFY(Lines.size() > 2); QVERIFY(Lines[1].size() >= Lines[0].size());
        // 이미 있는 프로젝트는 덮어쓰지 않는다.
        QVERIFY(!Spec.Error().isEmpty()); QVERIFY(!CreateSphinxProject(Spec, nullptr).isEmpty());
        // source · build 분리.
        Spec.folder_ = QStringLiteral("split"); Spec.separate_ = true; QCOMPARE(CreateSphinxProject(Spec, &Index), QString());
        QCOMPARE(QDir::cleanPath(Index), QDir::cleanPath(Root.filePath("split/source/index.rst"))); QVERIFY(QFileInfo(Root.filePath("split/build")).isDir());
        // 위치가 없거나 이름이 비면 만들지 않는다.
        Spec.directory_ = Root.filePath("missing"); QVERIFY(!Spec.Error().isEmpty());
        Spec.directory_ = Root.path(); Spec.name_.clear(); QVERIFY(!Spec.Error().isEmpty());
    }
    void reStructuredTextShortcutsValidate()
    {
        // 기본 단축키는 충돌 없이 통과한다( 자동 완성 Ctrl+Space 는 제품이 정한 기본값이라 예약 목록 검사에서 빠진다 ).
        QCOMPARE(DocumentShortcutRegistry::Validate({}), QString());
        const auto Bindings = DocumentShortcutRegistry::Bindings({});
        QCOMPARE(Bindings["rst.complete"].toString(), QString("Ctrl+Space")); QCOMPARE(Bindings["rst.fullScreen"].toString(), QString("F11"));
        QCOMPARE(Bindings["markdown.fullScreen"].toString(), QString("F11")); QCOMPARE(Bindings["rst.toggleProblems"].toString(), QString("Ctrl+Alt+D"));
        QCOMPARE(DocumentShortcutRegistry::ScopeOf("rst.rebuild"), QString("rst"));
        // 마크다운 범위와 reST 범위는 함께 켜지지 않으므로 같은 키를 써도 충돌이 아니다. 텍스트 범위와는 함께 켜진다.
        QVERIFY(!DocumentShortcutRegistry::CoActive("rst", "markdown")); QVERIFY(DocumentShortcutRegistry::CoActive("rst", "text"));
        // 다른 명령은 여전히 예약 키를 가져갈 수 없다.
        QVERIFY(!DocumentShortcutRegistry::Validate({{"rst.rebuild", "Ctrl+Space"}}).isEmpty());
        // reST 명령끼리의 충돌은 잡는다.
        QVERIFY(!DocumentShortcutRegistry::Validate({{"rst.rebuild", "F11"}}).isEmpty());
    }
    void dialogsShareTheProductFrame()
    {
        // Captures for review: set HC_NEW_DOCUMENT_CAPTURE to a folder.
        const auto Folder = qEnvironmentVariable("HC_NEW_DOCUMENT_CAPTURE");
        Profile Settings; QWidget Host; Host.resize(900, 700); Host.show(); QVERIFY(QTest::qWaitForWindowExposed(&Host));
        auto* Chooser = new NewDocumentChooser(&Host, {}); Chooser->show(); QVERIFY(QTest::qWaitForWindowExposed(Chooser));
        auto* Image = new NewImageDialog(&Host, {QSize(617, 700), {}, 1, {}}); Image->show(); QVERIFY(QTest::qWaitForWindowExposed(Image));
        auto* Epub = new NewEpubDialog(&Host, Settings.root_.path(), {}, "ko"); Epub->show(); QVERIFY(QTest::qWaitForWindowExposed(Epub));
        NewSphinxProjectDialog::Options SphinxOptions; SphinxOptions.directory_ = Settings.root_.path();
        auto* Sphinx = new NewSphinxProjectDialog(&Host, SphinxOptions); Sphinx->show(); QVERIFY(QTest::qWaitForWindowExposed(Sphinx));
        auto* Build = new SphinxBuildDialog(QStringLiteral("Demo"), Settings.root_.path(), &Host); Build->show(); QVERIFY(QTest::qWaitForWindowExposed(Build));
        for (QWidget* Dialog : {static_cast<QWidget*>(Chooser), static_cast<QWidget*>(Image), static_cast<QWidget*>(Epub), static_cast<QWidget*>(Sphinx), static_cast<QWidget*>(Build)}) {
            QVERIFY(Dialog->findChild<QWidget*>("hc.dialog.header")); QVERIFY(Dialog->findChild<QWidget*>("hc.dialog.footer"));
            // No label shows a raw "(&X)" mnemonic.
            for (auto* Label : Dialog->findChildren<QLabel*>()) if (Label->isVisible()) QVERIFY2(!(Label->text().contains("(&") && !Label->buddy()), qPrintable(Label->text()));
            if (!Folder.isEmpty()) { QTest::qWait(150); QVERIFY(Dialog->grab().save(QDir(Folder).filePath(Dialog->objectName() + ".png"))); }
        }
        for (auto* Dialog : {static_cast<QDialog*>(Chooser), static_cast<QDialog*>(Image), static_cast<QDialog*>(Epub), static_cast<QDialog*>(Sphinx), static_cast<QDialog*>(Build)}) Dialog->reject();
    }
    void epubFileMenuOffersNewDocument()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery"));
        auto* New = Widget.findChild<QAction*>("newDocument");
        auto* FileMenu = Widget.findChild<QMenu*>("documentFileMenu");
        QVERIFY(New && FileMenu && FileMenu->actions().contains(New));
        QVERIFY(viewKinds::Applies(New, DocumentViewKind::EpubEdit));
        QVERIFY(viewKinds::Applies(New, DocumentViewKind::EpubRead));
    }
    void blankHwpxTemplateIsEditable()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery"));
        const auto Template = Bytes(":/templates/blank.hwpx");
        QVERIFY(!Template.isEmpty());
        const auto Inspection = InspectHwpInput(Template, "hwpx");
        QVERIFY2(Inspection.accepted_ && !Inspection.restricted_, qPrintable(Inspection.error_));
    }
    void hwpxChooserRequestsDedicatedEditor()
    {
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery"), true); Widget.show();
        Widget.findChild<QAction*>("newDocument")->trigger();
        QPointer<NewDocumentChooser> Chooser = Widget.findChild<NewDocumentChooser*>(); QVERIFY(Chooser);
        // 아래한글은 다섯 번째 카드다( Markdown 아래에 Sphinx 프로젝트 · reStructuredText 가 들어갔다 ).
        QTest::keyClick(Chooser, Qt::Key_5);
        QTRY_VERIFY(!Chooser);
        const auto Request = Widget.Snapshot()["editRequest"].toObject();
        QCOMPARE(Request["format"].toString(), QString("hwpx-new"));
        QVERIFY(!Request["token"].toString().isEmpty());
        QVERIFY(Documents(Widget).isEmpty());
        QVERIFY(Widget.Handle({{"action", "new"}, {"kind", "hwpx"}}).contains("error"));
        QVERIFY(!Widget.Handle({{"action", "editResult"}, {"token", Request["token"]}, {"accepted", true}}).contains("error"));
        QVERIFY(Widget.Snapshot()["editRequest"].toObject().isEmpty());
    }
    void chooserSettingSkipsPopup()
    {
        Profile Settings(R"({"common":{"newDocumentAction":"markdown"}})"); DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show();
        Widget.findChild<QAction*>("newDocument")->trigger();
        QTRY_COMPARE(Documents(Widget).size(), 1); QVERIFY(!Widget.findChild<NewDocumentChooser*>());
        QTRY_VERIFY(Widget.findChild<MarkdownDocumentView*>());
    }
    void newImageSizes()
    {
        QVERIFY(NewImageError(1920, 1080).isEmpty()); QVERIFY(!NewImageError(0, 10).isEmpty());
        QVERIFY(!NewImageError(16385, 10).isEmpty()); QVERIFY(!NewImageError(16384, 16384).isEmpty());
        Profile Settings; DocumentWorkspaceWidget Widget(Settings.Path("recovery")); Widget.show();
        AppSettings Defaults; Defaults.setValue("common/newImageWidth", 320); Defaults.setValue("common/newImageHeight", 200); Defaults.setValue("common/newImageBackground", "clear"); Defaults.sync();
        const auto Id = Widget.Handle({{"action", "new"}, {"kind", "image"}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        const auto Files = QDir(Settings.Path("recovery")).entryInfoList({"untitled-*.png"}, QDir::Files); QCOMPARE(Files.size(), 1);
        const QImage Created(Files.first().absoluteFilePath()); QCOMPARE(Created.size(), QSize(320, 200)); QVERIFY(Created.hasAlphaChannel());
        QCOMPARE(qAlpha(Created.pixel(5, 5)), 0);
    }
};
QTEST_MAIN(NewDocumentTests)
#include "tstNewDocument.moc"
