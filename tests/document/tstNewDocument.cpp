#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include <QtEndian>
#include <QAction>
#include <QBuffer>
#include <QDir>
#include <QMessageBox>
#include <QLabel>
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
#include "text/QTextView.h"
#include "core/AppSettings.h"
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/document/epub/epubTemplate.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/markdownDocumentView.hpp"
#include "UniqueLibs/document/newDocumentChooser.hpp"
#include "UniqueLibs/document/newEpubDialog.hpp"
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
        QTest::keyClick(Chooser->focusWidget() ? Chooser->focusWidget() : Chooser.data(), Qt::Key_4);
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
    void dialogsShareTheProductFrame()
    {
        // Captures for review: set HC_NEW_DOCUMENT_CAPTURE to a folder.
        const auto Folder = qEnvironmentVariable("HC_NEW_DOCUMENT_CAPTURE");
        Profile Settings; QWidget Host; Host.resize(900, 700); Host.show(); QVERIFY(QTest::qWaitForWindowExposed(&Host));
        auto* Chooser = new NewDocumentChooser(&Host, {}); Chooser->show(); QVERIFY(QTest::qWaitForWindowExposed(Chooser));
        auto* Image = new NewImageDialog(&Host, {QSize(617, 700), {}, 1, {}}); Image->show(); QVERIFY(QTest::qWaitForWindowExposed(Image));
        auto* Epub = new NewEpubDialog(&Host, Settings.root_.path(), {}, "ko"); Epub->show(); QVERIFY(QTest::qWaitForWindowExposed(Epub));
        for (QWidget* Dialog : {static_cast<QWidget*>(Chooser), static_cast<QWidget*>(Image), static_cast<QWidget*>(Epub)}) {
            QVERIFY(Dialog->findChild<QWidget*>("hc.dialog.header")); QVERIFY(Dialog->findChild<QWidget*>("hc.dialog.footer"));
            // No label shows a raw "(&X)" mnemonic.
            for (auto* Label : Dialog->findChildren<QLabel*>()) if (Label->isVisible()) QVERIFY2(!(Label->text().contains("(&") && !Label->buddy()), qPrintable(Label->text()));
            if (!Folder.isEmpty()) { QTest::qWait(150); QVERIFY(Dialog->grab().save(QDir(Folder).filePath(Dialog->objectName() + ".png"))); }
        }
        for (auto* Dialog : {static_cast<QDialog*>(Chooser), static_cast<QDialog*>(Image), static_cast<QDialog*>(Epub)}) Dialog->reject();
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
