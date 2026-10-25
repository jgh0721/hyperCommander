#include "stdafx.h"
#include "readingSettings.hpp"
#include "readingNavigation.hpp"

/// Platform/framework headers
#include <QComboBox>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QAccessible>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QImage>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

using hc::epub::poc::ReadingStore;
using hc::epub::poc::ReadingAssets;
using hc::epub::poc::ReadingSettings;

class ReadingTests final : public QObject
{
    Q_OBJECT
private:
    const QString book = QString(64, 'a'), otherBook = QString(64, 'b');
    QByteArray bytes(const QString& Path) { QFile F(Path); if(!F.open(QIODevice::ReadOnly)) return {}; return F.readAll(); }
private slots:
    void readingLayoutPersistence() {
        QTemporaryDir Temp;const auto Path=Temp.filePath("reading.json");
        QVERIFY(ReadingStore::Save(Path,book,true,{},{{"layout","paginated"},{"columns",2}}).error.isEmpty());
        const auto Saved=ReadingStore::Load(Path,book);QCOMPARE(ReadingStore::Resolve(Saved.global,{})["columns"].toInt(),2);
        QVERIFY(ReadingStore::Save(Path,book,false,{},{{"layout","scroll"}}).error.isEmpty());
        QCOMPARE(ReadingStore::Resolve(Saved.global,ReadingStore::Load(Path,book).book)["layout"].toString(),QString("scroll"));
        QVERIFY(!ReadingStore::Valid({{"columns",1.5}}));QVERIFY(!ReadingStore::Valid({{"layout","free"}}));
    }
    void hierarchicalTocAndCurrentLocation() {
        QJsonObject Request;hc::epub::poc::ReadingNavigation Navigation({},book,[&](const QJsonObject& Value){Request=Value;});
        Navigation.SetPublication({{"navigation",QJsonArray{QJsonObject{{"label","장"},{"path","a.xhtml"},{"depth",0}},QJsonObject{{"label","절"},{"path","a.xhtml"},{"fragment","part"},{"depth",1}}}}});
        auto* Toc=Navigation.findChild<QTreeWidget*>("epubToc");QVERIFY(Toc);QCOMPARE(Toc->topLevelItemCount(),1);QCOMPARE(Toc->topLevelItem(0)->childCount(),1);
        QVERIFY(!Navigation.Execute("toc",1));Navigation.Poll({{"status","ready"},{"tocIndex",1}});
        QCOMPARE(Toc->currentItem()->text(0),QString("절"));QVERIFY(Navigation.Execute("toc",1));QCOMPARE(Request["action"].toString(),QString("toc"));QCOMPARE(Request["value"].toInt(),1);QVERIFY(!Navigation.Execute("toc",9));
    }
    void imeAndAccessibleControls() {
        QJsonObject Request;hc::epub::poc::ReadingNavigation Navigation({},QString(64,'a'),[&](const QJsonObject& Value){Request=Value;});Navigation.show();
        Navigation.Poll({{"status","ready"},{"locator","epubcfi(/6/2!/4/2:0)"}});QVERIFY(Navigation.Execute("focus-search"));
        auto* Search=Navigation.findChild<QLineEdit*>("epubSearch");QVERIFY(Search);QVERIFY(Search->testAttribute(Qt::WA_InputMethodEnabled));
        QInputMethodEvent Preedit("고",{});QCoreApplication::sendEvent(Search,&Preedit);QVERIFY(Search->text().isEmpty());QVERIFY(Request.isEmpty());
        QInputMethodEvent Commit;Commit.setCommitString("골목");QCoreApplication::sendEvent(Search,&Commit);QCOMPARE(Search->text(),QString("골목"));QVERIFY(Request.isEmpty());
        QTest::keyClick(Search,Qt::Key_Return);QCOMPARE(Request["action"].toString(),QString("search"));QCOMPARE(Request["value"].toString(),QString("골목"));
        auto* Accessible=QAccessible::queryAccessibleInterface(Search);QVERIFY(Accessible);QCOMPARE(Accessible->text(QAccessible::Name),QString("책 전체 검색어"));
        ReadingSettings Settings({},QString(64,'a'),"light");auto* Css=Settings.findChild<QPlainTextEdit*>("customCss");QVERIFY(Css);
        QInputMethodEvent CssPreedit("문",{});QCoreApplication::sendEvent(Css,&CssPreedit);QVERIFY(Settings.Profile()["customCss"].toString().isEmpty());
        QInputMethodEvent CssCommit;CssCommit.setCommitString("/* 문단 */");QCoreApplication::sendEvent(Css,&CssCommit);QCOMPARE(Settings.Profile()["customCss"].toString(),QString("/* 문단 */"));
    }
    void durableReaderState() {
        QTemporaryDir Temp;QVERIFY(Temp.isValid());const auto Settings=Temp.filePath("epub-reading.json"),Book=QString(64,'a');
        using hc::epub::poc::ReadingNavigation;
        ReadingNavigation First(Settings,Book,[](const QJsonObject&){}),Second(Settings,Book,[](const QJsonObject&){});
        const QJsonObject State{{"status","ready"},{"locator","epubcfi(/6/2!/4/2:0)"}};First.Poll(State);Second.Poll(State);
        QVERIFY(First.Execute("bookmark-add","first"));QTest::qWait(900);QVERIFY(Second.Execute("bookmark-add","second"));QTest::qWait(900);
        const auto Path=Temp.filePath("epub-state/"+Book+".json");const auto Read=[&]{QFile File(Path);if(!File.open(QIODevice::ReadOnly))return QJsonObject{};return QJsonDocument::fromJson(File.readAll()).object();};
        QTRY_COMPARE(Read()["bookmarks"].toArray().size(),2);
        QVERIFY(First.Execute("bookmark-remove",First.Snapshot()["bookmarks"].toArray().first().toObject()["id"]));QTest::qWait(900);
        QCOMPARE(Read()["bookmarks"].toArray().size(),1);QCOMPARE(Read()["bookmarks"].toArray().first().toObject()["label"].toString(),QString("second"));
        QFile File(Path);QVERIFY(File.open(QIODevice::WriteOnly|QIODevice::Truncate));File.write("broken");File.close();
        {ReadingNavigation Broken(Settings,Book,[](const QJsonObject&){});QVERIFY(!Broken.Snapshot()["readingStateError"].toString().isEmpty());Broken.Poll(State);QVERIFY(Broken.Execute("bookmark-add","keep damaged original"));}
        QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),QByteArray("broken"));
    }

    void bookFontsSelectionAndRefresh()
    {
        QTemporaryDir Dir; QVERIFY(Dir.isValid()); const auto Path = Dir.filePath("fonts.json");
        ReadingSettings Panel(Path, book, "light"); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        auto* Fonts = Panel.findChild<QComboBox*>("font"); QVERIFY(Fonts);
        QSignalSpy Changes(&Panel, &ReadingSettings::ProfileChanged);
        const auto Initial = Panel.Profile();
        Panel.SetBookFonts({"HCFixtureFont", "HCFixtureFont", "bad\nfont", QString(129, 'x')});
        QCOMPARE(Panel.Profile(), Initial); QVERIFY(!Panel.IsDirty()); QCOMPARE(Changes.count(), 0);
        const auto Index = Fonts->findData("HCFixtureFont"); QVERIFY(Index >= 0);
        QVERIFY(Fonts->itemText(Index).contains(QString::fromUtf8("책 글꼴")));
        QVERIFY(Fonts->findData("bad\nfont") < 0);
        Fonts->setCurrentIndex(Index); QCOMPARE(Panel.Profile()["font"].toString(), QString("HCFixtureFont"));
        QVERIFY(Panel.IsDirty()); Panel.findChild<QPushButton*>("apply")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000); QVERIFY(!Panel.IsDirty());
        QCOMPARE(ReadingStore::Load(Path, book).book["font"].toString(), QString("HCFixtureFont"));
        Changes.clear(); Panel.SetBookFonts({}); QCOMPARE(Changes.count(), 0); QVERIFY(!Panel.IsDirty());
        QCOMPARE(Panel.Profile()["font"].toString(), QString("HCFixtureFont"));
        QVERIFY(Fonts->currentText().contains(QString::fromUtf8("확인되지 않음")));
        Panel.SetBookFonts({"HCFixtureFont"}); QCOMPARE(Changes.count(), 0); QVERIFY(!Panel.IsDirty());
        Panel.findChild<QPushButton*>("reset_font")->click(); QCOMPARE(Panel.Profile()["font"], Initial["font"]);
        Panel.findChild<QPushButton*>("cancel")->click(); QCOMPARE(Panel.Profile()["font"].toString(), QString("HCFixtureFont"));
        ReadingSettings Other({}, otherBook, "light");
        QVERIFY(Other.findChild<QComboBox*>("font")->findData("HCFixtureFont") < 0);
    }
    void backgroundsAndCssDrafts() {
        QTemporaryDir Dir; const auto Path=Dir.filePath("reading.json");
        QImage Image(32,24,QImage::Format_ARGB32);Image.fill(QColor(35,80,120,128));const auto Input=Dir.filePath("input.png");QVERIFY(Image.save(Input));
        const auto Before=bytes(Input);const auto Asset=ReadingAssets::Import(Input,Path);QVERIFY2(Asset.error.isEmpty(),qPrintable(Asset.error));
        QString Error;const auto Loaded=ReadingAssets::Load(Path,Asset.id,Error);QVERIFY(!Loaded.isEmpty());QCOMPARE(QImage::fromData(Loaded).size(),Image.size());QCOMPARE(bytes(Input),Before);
        QVERIFY(ReadingAssets::Load(Path,"../outside",Error).isEmpty());QVERIFY(!Error.isEmpty());
        ReadingSettings Panel(Path,book,"light");QTRY_VERIFY(!Panel.IsBusy());
        auto* Css=Panel.findChild<QPlainTextEdit*>("customCss");auto* Enabled=Panel.findChild<QCheckBox*>("cssEnabled");QVERIFY(Css&&Enabled);
        Css->setPlainText("p { line-height: 2.4; }");Enabled->setChecked(true);
        QVERIFY(!Panel.CssValid());QVERIFY(!Panel.findChild<QPushButton*>("apply")->isEnabled());
        Panel.SetCssValidation(Css->toPlainText(),{});QVERIFY(Panel.CssValid());Panel.findChild<QPushButton*>("apply")->click();QTRY_VERIFY(!Panel.IsBusy());
        QCOMPARE(ReadingStore::Load(Path,book).book["customCss"].toString(),Css->toPlainText());
        Css->setPlainText("@import 'https://invalid';");Panel.SetCssValidation(Css->toPlainText(),"1:1 unsupported @import");
        QVERIFY(!Panel.findChild<QPushButton*>("apply")->isEnabled());Panel.findChild<QPushButton*>("cancel")->click();
        QCOMPARE(Css->toPlainText(),QString("p { line-height: 2.4; }"));
        const auto Saved=ReadingStore::Save(Path,book,false,ReadingStore::Load(Path,book).book,{{"background","image"},{"backgroundAsset",Asset.id},{"backgroundColor","#f3eadb"}});
        QVERIFY2(Saved.error.isEmpty(),qPrintable(Saved.error));QCOMPARE(ReadingStore::Load(Path,book).book["backgroundAsset"].toString(),Asset.id);
        QImage Large(4097,1,QImage::Format_RGB32);Large.fill(Qt::white);QVERIFY(Large.save(Dir.filePath("large.png")));QVERIFY(!ReadingAssets::Import(Dir.filePath("large.png"),Path).error.isEmpty());
    }
    void applicationThemeInheritance() {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        ReadingSettings Panel(Path, book, "dark");
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QCOMPARE(Panel.Profile()["theme"].toString(), QString("system"));
        auto* Theme = Panel.findChild<QComboBox*>("theme"); QVERIFY(Theme);
        QVERIFY(Theme->currentText().contains(QString::fromUtf8("앱 테마")));
        Theme->setCurrentIndex(1); Panel.findChild<QPushButton*>("apply")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QCOMPARE(ReadingStore::Load(Path, book).book["theme"].toString(), QString("dark"));
        Panel.findChild<QPushButton*>("reset_theme")->click(); Panel.findChild<QPushButton*>("apply")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QVERIFY(!ReadingStore::Load(Path, book).book.contains("theme"));
        QCOMPARE(Panel.Profile()["theme"].toString(), QString("system"));
    }
    void inheritanceAndRestart()
    {
        QTemporaryDir Dir; QVERIFY(Dir.isValid()); const auto Path = Dir.filePath("reading.json");
        QVERIFY(ReadingStore::Load(Path, book).error.isEmpty()); QVERIFY(!QFile::exists(Path));
        QVERIFY(ReadingStore::Save(Path, book, true, {}, {{"line", 2.2}, {"after", 1.2}, {"override", false}}).error.isEmpty());
        QVERIFY(ReadingStore::Save(Path, book, false, {}, {{"after", 0}, {"override", true}}).error.isEmpty());
        auto Saved = ReadingStore::Load(Path, book);
        auto Values = ReadingStore::Resolve(Saved.global, Saved.book);
        QCOMPARE(Values["line"].toDouble(), 2.2); QCOMPARE(Values["after"].toDouble(), 0.0); QVERIFY(Values["override"].toBool());
        const auto Other = ReadingStore::Load(Path, otherBook);
        QCOMPARE(ReadingStore::Resolve(Other.global, Other.book)["after"].toDouble(), 1.2);
        QVERIFY(ReadingStore::Save(Path, book, false, Saved.book, {}).error.isEmpty());
        Saved = ReadingStore::Load(Path, book); QVERIFY(Saved.book.isEmpty());
        QCOMPARE(ReadingStore::Resolve(Saved.global, Saved.book)["after"].toDouble(), 1.2);
    }
    void mergeAndConflict()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        QVERIFY(ReadingStore::Save(Path, book, false, {}, {{"line", 2.0}}).error.isEmpty());
        QVERIFY(ReadingStore::Save(Path, book, false, {}, {{"after", 1.4}}).error.isEmpty());
        const auto Saved = ReadingStore::Load(Path, book); QCOMPARE(Saved.book.size(), 2);
        const auto Before = bytes(Path);
        QVERIFY(!ReadingStore::Save(Path, book, false, {}, {{"line", 1.4}}).error.isEmpty()); QCOMPARE(bytes(Path), Before);
        QVERIFY(ReadingStore::Save(Path, otherBook, false, {}, {{"size", 24}}).error.isEmpty());
        QCOMPARE(ReadingStore::Load(Path, book).book, Saved.book);
    }
    void lockCorruptionAndInvalidValues()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        QLockFile Lock(Path + ".lock"); QVERIFY(Lock.tryLock(0));
        QVERIFY(!ReadingStore::Save(Path, book, false, {}, {{"size", 24}}).error.isEmpty()); QVERIFY(!QFile::exists(Path)); Lock.unlock();
        QFile Bad(Path); QVERIFY(Bad.open(QIODevice::WriteOnly)); Bad.write("broken"); Bad.close();
        QVERIFY(!ReadingStore::Load(Path, book).error.isEmpty());
        QVERIFY(!ReadingStore::Save(Path, book, false, {}, {{"size", 24}}).error.isEmpty()); QCOMPARE(bytes(Path), QByteArray("broken"));
        const auto Fresh = Dir.filePath("fresh.json");
        for(const QJsonObject& Invalid : {QJsonObject{{"line", 100}}, QJsonObject{{"size", "24"}},
            QJsonObject{{"override", 1}}, QJsonObject{{"unknown", 1}}, QJsonObject{{"font", "x\ny"}}})
            QVERIFY(!ReadingStore::Save(Fresh, book, false, {}, Invalid).error.isEmpty());
        QVERIFY(!QFile::exists(Fresh));
        QVERIFY(!ReadingStore::Save(Fresh, "../book", false, {}, {{"size", 24}}).error.isEmpty());
    }
    void draftApplyCancelAndReopen()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        ReadingSettings Panel(Path, book, "light");
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        auto* Line = Panel.findChild<QDoubleSpinBox*>("line"); auto* After = Panel.findChild<QDoubleSpinBox*>("after");
        auto* Scope = Panel.findChild<QComboBox*>("scope"); auto* Apply = Panel.findChild<QPushButton*>("apply");
        auto* Cancel = Panel.findChild<QPushButton*>("cancel"); QVERIFY(Line && After && Scope && Apply && Cancel);
        Line->setValue(2.3); QCOMPARE(Panel.Profile()["after"].toDouble(), 0.8); QVERIFY(Panel.IsDirty()); QVERIFY(!Scope->isEnabled());
        QVERIFY(!QFile::exists(Path)); Cancel->click(); QCOMPARE(Line->value(), 1.8); QVERIFY(!Panel.IsDirty());
        Line->setValue(2.3); After->setValue(1.6); Apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QVERIFY(!Panel.IsDirty()); const auto SavedBytes = bytes(Path); QVERIFY(!SavedBytes.isEmpty());
        After->setValue(0.3); Cancel->click(); QCOMPARE(After->value(), 1.6); QCOMPARE(bytes(Path), SavedBytes);
        ReadingSettings Reopened(Path, book, "light"); QTRY_VERIFY_WITH_TIMEOUT(!Reopened.IsBusy(), 5000);
        QCOMPARE(Reopened.Profile()["line"].toDouble(), 2.3); QCOMPARE(Reopened.Profile()["after"].toDouble(), 1.6);
        Panel.findChild<QPushButton*>("reset_line")->click(); QCOMPARE(Line->value(), 1.8); QCOMPARE(After->value(), 1.6);
        Cancel->click(); QCOMPARE(Line->value(), 2.3);
        Panel.findChild<QPushButton*>("clear")->click(); Apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QVERIFY(ReadingStore::Load(Path, book).book.isEmpty());
        Scope->setCurrentIndex(1); Line->setValue(2.1); Apply->click(); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        Scope->setCurrentIndex(0); QCOMPARE(Panel.Profile()["line"].toDouble(), 2.1);
    }
    void uiSaveFailureKeepsDraft()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        ReadingSettings Panel(Path, book, "light"); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QLockFile Lock(Path + ".lock"); QVERIFY(Lock.tryLock(0));
        Panel.findChild<QDoubleSpinBox*>("line")->setValue(2.2); Panel.findChild<QPushButton*>("apply")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000); QVERIFY(Panel.IsDirty()); QCOMPARE(Panel.Profile()["line"].toDouble(), 2.2);
        QVERIFY(!QFile::exists(Path)); Panel.findChild<QPushButton*>("cancel")->click(); QCOMPARE(Panel.Profile()["line"].toDouble(), 1.8);
    }
    void incompatibleAndOversizedFilesPreserved()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("reading.json");
        for(const auto& Data : {QByteArray("{\"schemaVersion\":2,\"global\":{},\"books\":{}}"), QByteArray(2 * 1024 * 1024 + 1, ' ')})
        {
            QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Data), Data.size()); File.close();
            QVERIFY(!ReadingStore::Load(Path, book).error.isEmpty());
            QVERIFY(!ReadingStore::Save(Path, book, true, {}, {{"size", 24}}).error.isEmpty()); QCOMPARE(bytes(Path), Data);
        }
        ReadingSettings Panel(Path, book, "light"); QTRY_VERIFY_WITH_TIMEOUT(!Panel.IsBusy(), 5000);
        QVERIFY(!Panel.findChild<QPushButton*>("apply")->isEnabled()); QVERIFY(!Panel.findChild<QDoubleSpinBox*>("line")->isEnabled());
        QVERIFY(Panel.findChild<QPushButton*>("reload")->isEnabled());
    }
};
QTEST_MAIN(ReadingTests)
#include "testReadingSettings.moc"
