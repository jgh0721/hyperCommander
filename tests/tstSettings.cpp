#include "UniqueLibs/settings/appSettings.hpp"
#include <QtTest>
#include <QTemporaryDir>
using namespace hc::settings;
class SettingsTests : public QObject {
    Q_OBJECT
private:
    static QByteArray Bytes(const QString& Path) { QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {}; return File.readAll(); }
    static bool Write(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
private slots:
    void paths()
    {
        const auto Executable = QDir::currentPath() + "/bin";
        const auto User = QDir::currentPath() + "/user";
        QCOMPARE(ConfigPaths::Resolve({"app"}, Executable, User).directory_, User);
        QCOMPARE(ConfigPaths::Resolve({"app","--portable"}, Executable, User).directory_, Executable + "/config");
        QCOMPARE(ConfigPaths::Resolve({"app","--portable","--config-dir",User}, Executable).directory_, User);
        QCOMPARE(ConfigPaths::Resolve({"app","--config-dir=" + User}, Executable).directory_, User);
        QVERIFY(!ConfigPaths::Resolve({"app","--config-dir"}, Executable).error_.isEmpty());
        QVERIFY(!ConfigPaths::Resolve({"app","--config-dir="}, Executable).error_.isEmpty());
        QVERIFY(!ConfigPaths::Resolve({"app","--config-dir=x","--config-dir=y"}, Executable).error_.isEmpty());
    }
    void firstRunIgnoresIni()
    {
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        const QByteArray Ini("[Configuration]\r\nSizeInHeader=9\r\n[ListerPlugins]\r\nCount=1\r\n0_Path=evil.wlx64\r\n");
        QVERIFY(Write(Temp.filePath("HyperCommander.ini"), Ini));
        Repository Settings(Temp.path()); QVERIFY(Settings.Writable()); QVERIFY(!Settings.Exists());
        QCOMPARE(Settings.SizeStyle(true), 0); QVERIFY(Settings.Plugins("wlx").isEmpty());
        QVERIFY(!QFileInfo::exists(Settings.FileName())); QCOMPARE(Bytes(Temp.filePath("HyperCommander.ini")), Ini);
        QVERIFY(Settings.Commit()); Repository Reload(Temp.path()); QCOMPARE(Reload.Object(), Defaults());
    }
    void nativeObjectsAndUnicodeRoundTrip()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path()); Settings.BeginEdit();
        const QJsonObject Favorite{{"name",QString::fromUtf8("한글 즐겨찾기")},{"command","cd C:/long path"},{"path","C:/"}};
        QVERIFY(Settings.Put(Section::Favorites, QJsonObject{{"drivesOn","top"},{"items",QJsonArray{Favorite}}}));
        QVERIFY(Settings.SetPlugins("wcx", QJsonArray{QJsonObject{{"path","C:/plugin.wcx64"},{"hostEnabled",true},{"hostReviewed",QJsonObject{{"id","reviewed"}}}}}));
        QVERIFY(Settings.Put(Section::Shortcuts, QJsonObject{{"Ctrl+Alt+F8","cm_Copy"},{"F5",QJsonValue::Null}}));
        QVERIFY(!QFileInfo::exists(Settings.FileName())); QVERIFY(Settings.Commit());
        Repository Reload(Temp.path()); QCOMPARE(Reload.Object(), Settings.Object());
        QVERIFY(Reload.Plugins("wcx")[0].toObject()["hostReviewed"].isObject());
        QVERIFY(Reload.Plugins("wcx")[0].toObject()["hostEnabled"].isBool());
        QVERIFY(Reload.Read(Section::Automation).isObject());
        QCOMPARE(Reload.Read(Section::Favorites).toObject()["items"].toArray()[0].toObject()["name"].toString(), QString::fromUtf8("한글 즐겨찾기"));
    }
    void cancelAndApply()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path()); Settings.BeginEdit();
        Settings.Put(Section::Shortcuts, QJsonObject{{"F8","cm_Copy"}}); Settings.DiscardEdit();
        QVERIFY(Settings.Read(Section::Shortcuts).toObject().isEmpty()); QVERIFY(!Settings.Exists());
        Settings.BeginEdit(); Settings.Put(Section::Shortcuts,QJsonObject{{"F8","cm_Copy"}}); QVERIFY(Settings.Commit());
        Settings.Put(Section::Shortcuts,QJsonObject{}); Settings.DiscardEdit();
        QCOMPARE(Settings.Read(Section::Shortcuts).toObject()["F8"].toString(), QString("cm_Copy"));
    }
    void sparseAppearanceAndInvalidFieldDefaults()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path());
        const QJsonObject Appearance{{"selected","z-selected"},{"schemes",QJsonObject{{"a-first",QJsonObject{}},{"z-selected",QJsonObject{}}}}};
        QVERIFY2(Settings.Put(Section::Appearance, Appearance), qPrintable(Settings.Error()));
        Repository Restart(Temp.path()); QCOMPARE(Restart.Read(Section::Appearance).toObject(),Appearance);
        auto Root = Restart.Object();
        const QJsonObject Invalid{{"fonts",QJsonObject{{"list",QJsonObject{{"family",""},{"pointSize",-2}}}}},
            {"colors",QJsonObject{{"listText","not-a-color"}}}};
        Root["appearance"] = QJsonObject{{"selected","sample"},{"schemes",QJsonObject{{"sample",Invalid}}}};
        const auto Original = QJsonDocument(Root).toJson(); QVERIFY(Write(Settings.FileName(),Original)); QVERIFY(Restart.Reload());
        const auto Scheme = Restart.Read(Section::Appearance).toObject()["schemes"].toObject()["sample"].toObject();
        QCOMPARE(Scheme["fonts"].toObject()["list"].toObject()["pointSize"].toInt(),14);
        QCOMPARE(Scheme["colors"].toObject()["listText"].toString(),QString("#202020"));
        QVERIFY(!Restart.Issues().isEmpty()); QCOMPARE(Bytes(Settings.FileName()),Original);
    }
    void corruptAndFutureAreProtected()
    {
        QTemporaryDir Temp;
        for (const auto& Data : {QByteArray("{broken"),QByteArray("[]"),QByteArray("{\"schemaVersion\":2}"),QByteArray("{\"schemaVersion\":\"1\"}")}) {
            QVERIFY(Write(Temp.filePath("settings.json"), Data)); Repository Settings(Temp.path());
            QVERIFY(!Settings.Writable()); QVERIFY(!Settings.Commit()); QCOMPARE(Bytes(Settings.FileName()),Data);
            QVERIFY(Settings.Plugins("wlx").isEmpty());
        }
    }
    void competingWritersAndReload()
    {
        QTemporaryDir Temp; Repository First(Temp.path()), Second(Temp.path());
        QVERIFY(First.Put(Section::Shortcuts,QJsonObject{{"F8","cm_Copy"}}));
        const auto Original = Bytes(First.FileName());
        QVERIFY(!Second.Put(Section::Shortcuts,QJsonObject{{"F7","cm_Copy"}}));
        QVERIFY(Second.Error().contains("changed-on-disk")); QCOMPARE(Bytes(First.FileName()),Original);
        QVERIFY(Second.Read(Section::Shortcuts).toObject().contains("F7"));
        QVERIFY(Second.Reload()); QCOMPARE(Second.Object(),First.Object());
        QVERIFY(Second.Put(Section::Shortcuts,QJsonObject{{"F6","cm_Copy"}}));
        QVERIFY(!First.Commit());
    }
    void lockedAndUnwritableKeepOriginal()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path()); QVERIFY(Settings.Commit());
        const auto Original = Bytes(Settings.FileName());
        { QLockFile Lock(Settings.FileName()+".lock"); QVERIFY(Lock.tryLock());
          QVERIFY(!Settings.Put(Section::Shortcuts,QJsonObject{{"F8","cm_Copy"}})); QCOMPARE(Bytes(Settings.FileName()),Original); }
        QVERIFY(Settings.Commit());
        const auto Blocker = Temp.filePath("file-not-directory"); QVERIFY(Write(Blocker,"blocker"));
        Repository Blocked(Blocker); QVERIFY(!Blocked.Commit()); QCOMPARE(Bytes(Blocker),QByteArray("blocker"));
    }
    void workspaceIndependentAndUnknownKeysRetained()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path()); JsonStore Workspace(Temp.filePath("workspace.json"));
        auto Root = Settings.Object(); Root["futureExtension"] = QJsonObject{{"key",42}}; QVERIFY(Settings.Replace(Root));
        const auto Original = Bytes(Settings.FileName());
        QVERIFY(Workspace.Replace(QJsonObject{{"schemaVersion",1},{"workspace",QJsonObject{{"geometry","AAAA"}}}}));
        QCOMPARE(Bytes(Settings.FileName()),Original);
        QVERIFY(Settings.Put(Section::Shortcuts,QJsonObject{{"F8","cm_Copy"}}));
        Repository Reload(Temp.path()); QCOMPARE(Reload.Object()["futureExtension"].toObject()["key"].toInt(),42);
    }
    void partialTypesUseDefaultsWithoutWriting()
    {
        QTemporaryDir Temp; const QByteArray Data("{\"schemaVersion\":1,\"general\":{\"useBuiltInShortcuts\":\"false\"},\"plugins\":\"invalid\"}");
        QVERIFY(Write(Temp.filePath("settings.json"),Data)); Repository Settings(Temp.path());
        QVERIFY(Settings.Writable()); QVERIFY(!Settings.Issues().isEmpty());
        QVERIFY(Settings.Read(Section::General).toObject()["useBuiltInShortcuts"].toBool());
        QVERIFY(Settings.Plugins("wlx").isEmpty()); QCOMPARE(Bytes(Settings.FileName()),Data);
    }
};
QTEST_GUILESS_MAIN(SettingsTests)
#include "tstSettings.moc"
