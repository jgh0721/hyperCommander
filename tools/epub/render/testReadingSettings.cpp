#include "stdafx.h"
#include "readingSettings.hpp"

/// Platform/framework headers
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

using hc::epub::poc::ReadingStore;
using hc::epub::poc::ReadingSettings;

class ReadingTests final : public QObject
{
    Q_OBJECT
private:
    const QString book = QString(64, 'a'), otherBook = QString(64, 'b');
    QByteArray bytes(const QString& Path) { QFile F(Path); if(!F.open(QIODevice::ReadOnly)) return {}; return F.readAll(); }
private slots:
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
