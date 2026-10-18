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
    void productDefaultsAndUserOverrides()
    {
        QTemporaryDir Temp; Repository Fresh(Temp.path());
        QCOMPARE(Fresh.Object(), hc::defaults::Settings());
        QVERIFY(!Fresh.Exists()); QVERIFY(!QFileInfo::exists(Fresh.FileName()));
        const QJsonObject Group{{"id", "custom.text"}, {"name", "사용자 문서"}, {"enabled", true}, {"join", "any"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "txt"}}}},
            {"light", QJsonObject{{"foreground", "#123456"}}}, {"dark", QJsonObject{{"foreground", "#abcdef"}}}, {"autoDark", false}};
        const QJsonObject Rules{{"version", 2}, {"mode", "merge"}, {"groups", QJsonArray{Group}}};
        const QJsonArray FileSets{QJsonObject{{"name", "사용자 집합"}, {"extensions", QJsonArray{"txt"}}}};
        QVERIFY(Fresh.Put(Section::FileSets, FileSets));
        QVERIFY(Fresh.Put(Section::Options, QJsonObject{{"copyVerify", true}, {"fileColorRules", Rules}}));
        QVERIFY(Fresh.Put(Section::Shortcuts, QJsonObject{{"F5", QJsonValue::Null}, {"Alt+C", "hc.file.copy"}}));
        auto Partial = Fresh.Object(); auto General = Partial["general"].toObject();
        General.remove("shortcutPreset"); General.remove("showFunctionBar"); General["thumbnails"] = QJsonObject{{"version", 99}, {"future", true}};
        Partial["general"] = General; const auto Original = QJsonDocument(Partial).toJson();
        QVERIFY(Write(Fresh.FileName(), Original)); Repository Reload(Temp.path());
        QCOMPARE(Reload.Read(Section::General).toObject()["shortcutPreset"].toString(), QString(hc::defaults::ExistingShortcutPreset));
        QVERIFY(Reload.Read(Section::General).toObject()["showFunctionBar"].toBool());
        QCOMPARE(Reload.Read(Section::General).toObject()["thumbnails"], General["thumbnails"]);
        QCOMPARE(Reload.Read(Section::FileSets).toArray(), FileSets);
        QCOMPARE(Reload.Read(Section::Options).toObject()["fileColorRules"].toObject(), Rules);
        QVERIFY(Reload.Read(Section::Options).toObject()["copyVerify"].toBool());
        QCOMPARE(Reload.Read(Section::Options).toObject()["jobNotification"], hc::defaults::Options()["jobNotification"]);
        QVERIFY(Reload.Read(Section::Shortcuts).toObject()["F5"].isNull());
        QCOMPARE(Bytes(Fresh.FileName()), Original);
        QVERIFY(Reload.Put(Section::FileSets, QJsonArray{}));
        QVERIFY(Reload.Put(Section::Options, QJsonObject{{"fileColorRules", QJsonObject{{"version", 2}, {"mode", "first"}, {"groups", QJsonArray{}}}}}));
        Repository Empty(Temp.path()); QVERIFY(Empty.Read(Section::FileSets).toArray().isEmpty());
        QVERIFY(Empty.Read(Section::Options).toObject()["fileColorRules"].toObject()["groups"].toArray().isEmpty());
    }
    void r8CopyDefaultsPersistenceAndValidation()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path());
        const QJsonObject Options{{"copyMetadata", false}, {"copyVerify", true}, {"copyAcl", true}, {"copyStreams", true}, {"copyPattern", "*.txt; 한글?.bin"}, {"copySpeedMiB", 8}};
        QVERIFY(Settings.Put(Section::Options, Options)); const auto Saved = Bytes(Settings.FileName()); Repository Reload(Temp.path()); for (auto It = Options.cbegin(); It != Options.cend(); ++It) QCOMPARE(Reload.Read(Section::Options).toObject()[It.key()], It.value());
        Reload.BeginEdit(); auto Invalid = Options; Invalid["copySpeedMiB"] = -1; QVERIFY(Reload.Put(Section::Options, Invalid)); QVERIFY(!Reload.Commit()); QCOMPARE(Bytes(Settings.FileName()), Saved); Reload.DiscardEdit();
        Reload.BeginEdit(); Invalid = Options; Invalid["copyPattern"] = "*.txt;"; QVERIFY(Reload.Put(Section::Options, Invalid)); QVERIFY(!Reload.Commit()); QCOMPARE(Bytes(Settings.FileName()), Saved); Reload.DiscardEdit();
        auto Document = Settings.Object(); Invalid = Options; Invalid["copyVerify"] = "true"; Invalid["copyPattern"] = "../*"; Invalid["copySpeedMiB"] = 1.5; Document["options"] = Invalid;
        const auto Broken = QJsonDocument(Document).toJson(); QVERIFY(Write(Settings.FileName(), Broken)); QVERIFY(Reload.Reload()); QVERIFY(!Reload.Issues().isEmpty());
        const auto Repaired = Reload.Read(Section::Options).toObject(); QVERIFY(!Repaired["copyVerify"].toBool()); QCOMPARE(Repaired["copyPattern"].toString(), QString("*")); QCOMPARE(Repaired["copySpeedMiB"].toInt(), 0); QCOMPARE(Bytes(Settings.FileName()), Broken);
    }

    void laboratoryRecoverySetting()
    {
        QTemporaryDir Directory; Repository Settings(Directory.path());
        QVERIFY(!Settings.Read(Section::Options).toObject()["enableJobRecovery"].toBool(false));
        auto Options=Settings.Read(Section::Options).toObject(); Options["enableJobRecovery"]=true;
        Settings.BeginEdit(); QVERIFY(Settings.Put(Section::Options,Options)); Settings.DiscardEdit();
        QVERIFY(!Settings.Read(Section::Options).toObject()["enableJobRecovery"].toBool(false));
        QVERIFY(Settings.Put(Section::Options,Options)); Repository Reload(Directory.path()); QVERIFY(Reload.Read(Section::Options).toObject()["enableJobRecovery"].toBool());
        Reload.BeginEdit(); Options["enableJobRecovery"]="true"; QVERIFY(Reload.Put(Section::Options,Options)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Options["enableJobRecovery"]=false; QVERIFY(Reload.Put(Section::Options,Options)); Repository Off(Directory.path()); QVERIFY(!Off.Read(Section::Options).toObject()["enableJobRecovery"].toBool(true));
    }
    void slowClickRenameSetting()
    {
        QTemporaryDir Directory; Repository Settings(Directory.path());
        QVERIFY(Settings.Read(Section::Options).toObject()["renameOnSlowClick"].toBool());
        auto Options = Settings.Read(Section::Options).toObject(); Options["renameOnSlowClick"] = false;
        QVERIFY(Settings.Put(Section::Options, Options)); Repository Reload(Directory.path());
        QVERIFY(!Reload.Read(Section::Options).toObject()["renameOnSlowClick"].toBool(true));
        Reload.BeginEdit(); Options["renameOnSlowClick"] = "false"; QVERIFY(Reload.Put(Section::Options, Options));
        QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        auto Document = Reload.Object(); Document["options"] = Options;
        const auto Broken = QJsonDocument(Document).toJson(); QVERIFY(Write(Reload.FileName(), Broken));
        QVERIFY(Reload.Reload()); QVERIFY(!Reload.Issues().isEmpty());
        QVERIFY(Reload.Read(Section::Options).toObject()["renameOnSlowClick"].toBool());
        QCOMPARE(Bytes(Reload.FileName()), Broken);
    }
    void delayedFileWritesSetting()
    {
        QTemporaryDir Directory; Repository Settings(Directory.path());
        QVERIFY(Settings.Read(Section::Options).toObject()["delayedFileWrites"].toBool(true));
        auto Options=Settings.Read(Section::Options).toObject(); Options["delayedFileWrites"]=false;
        Settings.BeginEdit(); QVERIFY(Settings.Put(Section::Options,Options)); Settings.DiscardEdit();
        QVERIFY(Settings.Read(Section::Options).toObject()["delayedFileWrites"].toBool(true));
        QVERIFY(Settings.Put(Section::Options,Options)); Repository Reload(Directory.path());
        QVERIFY(!Reload.Read(Section::Options).toObject()["delayedFileWrites"].toBool(true));
        Reload.BeginEdit(); Options["delayedFileWrites"]="false"; QVERIFY(Reload.Put(Section::Options,Options)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Options["delayedFileWrites"]=true; QVERIFY(Reload.Put(Section::Options,Options));
        Repository On(Directory.path()); QVERIFY(On.Read(Section::Options).toObject()["delayedFileWrites"].toBool(false));
    }
    void laboratoryProgressSetting()
    {
        QTemporaryDir Directory; Repository Settings(Directory.path());
        QVERIFY(!Settings.Read(Section::Options).toObject()["saveJobProgress"].toBool(false));
        auto Options=Settings.Read(Section::Options).toObject(); Options["saveJobProgress"]=true;
        Settings.BeginEdit(); QVERIFY(Settings.Put(Section::Options,Options)); QVERIFY(!Settings.CommittedObject()["options"].toObject()["saveJobProgress"].toBool(false));
        Settings.DiscardEdit(); QVERIFY(!Settings.Read(Section::Options).toObject()["saveJobProgress"].toBool(false));
        QVERIFY(Settings.Put(Section::Options,Options)); Repository Reload(Directory.path()); QVERIFY(Reload.Read(Section::Options).toObject()["saveJobProgress"].toBool());
        Reload.BeginEdit(); Options["saveJobProgress"]="true"; QVERIFY(Reload.Put(Section::Options,Options)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Options["saveJobProgress"]=false; QVERIFY(Reload.Put(Section::Options,Options)); Repository Off(Directory.path()); QVERIFY(!Off.Read(Section::Options).toObject()["saveJobProgress"].toBool(true));
    }
    void r13OptionsPersistenceAndValidation()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path()); const auto General = Settings.Read(Section::General).toObject(); auto Next = General; Next["singleInstance"] = true; Next["newTabPosition"] = "right";
        QVERIFY(Settings.Put(Section::General, Next)); const QJsonObject Options{{"deleteToRecycle", false}, {"jobNotification", "sound"}, {"queueInsertion", "first"}, {"jobConcurrency", 1}};
        QVERIFY(Settings.Put(Section::Options, Options)); Repository Reload(Temp.path()); QCOMPARE(Reload.Object(), Settings.Object());
        Reload.BeginEdit(); auto Invalid = Options; Invalid["jobConcurrency"] = 1.5; QVERIFY(Reload.Put(Section::Options, Invalid)); QCOMPARE(Reload.CommittedObject(), Settings.Object()); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Reload.BeginEdit(); auto BadGeneral = Next; BadGeneral["newTabPosition"] = "unknown"; QVERIFY(Reload.Put(Section::General, BadGeneral)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Reload.BeginEdit(); Invalid = Options; Invalid["jobNotification"] = "unknown"; QVERIFY(Reload.Put(Section::Options, Invalid)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Reload.BeginEdit(); Invalid = Options; Invalid["queueInsertion"] = "unknown"; QVERIFY(Reload.Put(Section::Options, Invalid)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
        Reload.BeginEdit(); Invalid = Options; Invalid["deleteToRecycle"] = "true"; QVERIFY(Reload.Put(Section::Options, Invalid)); QVERIFY(!Reload.Commit()); Reload.DiscardEdit();
    }

    void paths()
    {
        const auto Executable = QDir::currentPath() + "/bin";
        const auto User = QDir::currentPath() + "/user";
        QCOMPARE(ConfigPaths::Resolve({"app"}, Executable, User).directory_, Executable);
        QCOMPARE(ConfigPaths::Resolve({"app"}, Executable, User).legacy_, QStringList({User, Executable + "/config"}));
        QCOMPARE(ConfigPaths::Resolve({"app","--portable"}, Executable, User).directory_, Executable);
        QCOMPARE(ConfigPaths::Resolve({"app","--portable"}, Executable, User).legacy_, QStringList({Executable + "/config", User}));
        QCOMPARE(ConfigPaths::Resolve({"app","--portable","--config-dir",User}, Executable).directory_, User);
        QVERIFY(ConfigPaths::Resolve({"app","--portable","--config-dir",User}, Executable).legacy_.isEmpty());
        QCOMPARE(ConfigPaths::Resolve({"app","--config-dir=" + User}, Executable).directory_, User);
        QVERIFY(ConfigPaths::Resolve({"app"}, Executable, User, true).legacy_.isEmpty());
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
        QCOMPARE(Settings.SizeStyle(true), SizeFormats().indexOf("adaptive2")); QVERIFY(Settings.Plugins("wlx").isEmpty());
        const auto Appearance = Settings.Read(Section::Appearance).toObject();
        QCOMPARE(Appearance, BuiltInAppearance());
        QCOMPARE(Appearance["schemes"].toObject().size(), 2);
        QVERIFY(!QFileInfo::exists(Settings.FileName())); QCOMPARE(Bytes(Temp.filePath("HyperCommander.ini")), Ini);
        QCOMPARE(Settings.Read(Section::General).toObject()["shortcutPreset"].toString(), QString("prototype"));
        QVERIFY(Settings.Commit()); Repository Reload(Temp.path()); QCOMPARE(Reload.Object(), Settings.Object());
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
        Repository Restart(Temp.path());
        const auto Restored = Restart.Read(Section::Appearance).toObject();
        QCOMPARE(Restored["selected"], Appearance["selected"]);
        QVERIFY(Restored["schemes"].toObject().contains("a-first"));
        QVERIFY(Restored["schemes"].toObject().contains("z-selected"));
        for (const auto& Name : BuiltInAppearance()["schemes"].toObject().keys()) QVERIFY(Restored["schemes"].toObject().contains(Name));
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
            QVERIFY(Write(ProfileConfig::FileName(Temp.path()), Data)); Repository Settings(Temp.path());
            JsonStore Workspace(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Temp.path(), {}));
            QVERIFY(!Settings.Writable()); QVERIFY(!Settings.Commit()); QCOMPARE(Bytes(Settings.FileName()),Data);
            QVERIFY(!Workspace.Writable()); QVERIFY(!Workspace.Replace(QJsonObject{{"schemaVersion",1},{"workspace",QJsonObject{}}}));
            QCOMPARE(Bytes(Settings.FileName()),Data);
            QVERIFY(Settings.Plugins("wlx").isEmpty());
        }
        // A legacy file that cannot be migrated blocks both stores, so the original is never shadowed.
        for (const auto& Data : {QByteArray("{broken"),QByteArray("{\"schemaVersion\":2}")}) {
            QTemporaryDir Legacy; QVERIFY(Write(Legacy.filePath("settings.json"), Data));
            Repository Settings(Legacy.path());
            JsonStore Workspace(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Legacy.path(), {}));
            QVERIFY(!Settings.Writable()); QVERIFY(Settings.Error().contains("legacy-")); QVERIFY(!Settings.Commit());
            QVERIFY(!Workspace.Writable()); QVERIFY(!Workspace.Commit());
            QVERIFY(!QFileInfo::exists(Settings.FileName())); QCOMPARE(Bytes(Legacy.filePath("settings.json")), Data);
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
    void workspaceSharesFileAndUnknownKeysRetained()
    {
        QTemporaryDir Temp; Repository Settings(Temp.path());
        JsonStore Workspace(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Temp.path(), {}));
        QCOMPARE(QFileInfo(Settings.FileName()).fileName(), QString("HyperCommander.json"));
        auto Root = Settings.Object(); Root["futureExtension"] = QJsonObject{{"key",42}}; QVERIFY(Settings.Replace(Root));
        // Each store saves only its own keys; the other store's earlier save is not a conflict.
        const QJsonObject Geometry{{"geometry","AAAA"}};
        QVERIFY2(Workspace.Replace(QJsonObject{{"schemaVersion",1},{"workspace",Geometry}}), qPrintable(Workspace.Error()));
        QVERIFY2(Settings.Put(Section::Shortcuts,QJsonObject{{"F8","cm_Copy"}}), qPrintable(Settings.Error()));
        QVERIFY(!Settings.Object().contains("workspace")); QCOMPARE(Workspace.Object().keys(), QStringList({"schemaVersion","workspace"}));
        const auto Disk = QJsonDocument::fromJson(Bytes(Settings.FileName())).object();
        QCOMPARE(Disk["workspace"].toObject(), Geometry); QCOMPARE(Disk["shortcuts"].toObject()["F8"].toString(), QString("cm_Copy"));
        // A settings Replace carrying "workspace" never overwrites the workspace store's key.
        Root = Settings.Object(); Root["workspace"] = QJsonObject{{"geometry","BBBB"}}; Settings.Replace(Root);
        QCOMPARE(QJsonDocument::fromJson(Bytes(Settings.FileName())).object()["workspace"].toObject(), Geometry);
        Repository Reload(Temp.path()); QCOMPARE(Reload.Object()["futureExtension"].toObject()["key"].toInt(),42);
        QVERIFY(!Reload.Object().contains("workspace"));
        // Concurrent workspace writers still conflict on their own key.
        JsonStore Other(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Temp.path(), {}));
        QVERIFY(Other.Replace(QJsonObject{{"schemaVersion",1},{"workspace",QJsonObject{{"geometry","CCCC"}}}}));
        QVERIFY(!Workspace.Replace(QJsonObject{{"schemaVersion",1},{"workspace",QJsonObject{{"geometry","DDDD"}}}}));
        QVERIFY(Workspace.Error().contains("changed-on-disk"));
        QVERIFY(Settings.Put(Section::Shortcuts,QJsonObject{{"F7","cm_Copy"}}));
    }
    void legacyProfileMigrates()
    {
        QTemporaryDir Temp;
        const QByteArray Legacy("{\"schemaVersion\":1,\"general\":{\"sizeInHeader\":\"kbytes\"},\"futureExtension\":7}");
        const QByteArray LegacyWorkspace("{\"schemaVersion\":1,\"workspace\":{\"version\":2,\"id\":\"legacy\"}}");
        QVERIFY(Write(Temp.filePath("settings.json"), Legacy)); QVERIFY(Write(Temp.filePath("workspace.json"), LegacyWorkspace));
        Repository Settings(Temp.path());
        JsonStore Workspace(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Temp.path(), {}));
        QVERIFY(Settings.Writable()); QCOMPARE(Settings.SizeStyle(true), SizeFormats().indexOf("kbytes"));
        QCOMPARE(Settings.Read(Section::General).toObject()["shortcutPreset"].toString(), QString(hc::defaults::ExistingShortcutPreset));
        QCOMPARE(Settings.Object()["futureExtension"].toInt(), 7);
        QCOMPARE(Workspace.Object()["workspace"].toObject()["id"].toString(), QString("legacy"));
        const auto Disk = QJsonDocument::fromJson(Bytes(Settings.FileName())).object();
        QCOMPARE(Disk["schemaVersion"].toInt(), 1); QCOMPARE(Disk["general"].toObject()["sizeInHeader"].toString(), QString("kbytes"));
        QCOMPARE(Disk["workspace"].toObject()["version"].toInt(), 2);
        QCOMPARE(Bytes(Temp.filePath("settings.json")), Legacy); QCOMPARE(Bytes(Temp.filePath("workspace.json")), LegacyWorkspace);
        // Once HyperCommander.json exists, later legacy edits are ignored.
        QVERIFY(Write(Temp.filePath("settings.json"), "{\"schemaVersion\":1,\"general\":{\"sizeInHeader\":\"bytes\"}}"));
        Repository Again(Temp.path()); QCOMPARE(Again.SizeStyle(true), SizeFormats().indexOf("kbytes"));
    }
    void legacyProfileFromPreviousLocation()
    {
        QTemporaryDir Executable, User, Adjacent;
        const QByteArray UserSettings("{\"schemaVersion\":1,\"general\":{\"sizeInHeader\":\"kbytes\"}}");
        QVERIFY(Write(User.filePath("workspace.json"), "{\"schemaVersion\":1,\"workspace\":{\"id\":\"user\"}}"));
        QVERIFY(Write(User.filePath("settings.json"), UserSettings));
        QVERIFY(Write(Adjacent.filePath("settings.json"), "{\"schemaVersion\":1,\"general\":{\"sizeInHeader\":\"adaptive0\"}}"));
        // The first directory holding legacy files wins, and settings/workspace are not mixed across directories.
        Repository Settings(Executable.path(), {User.path(), Adjacent.path()});
        JsonStore Workspace(Settings.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone,
            ProfilePartition(true, Executable.path(), {User.path(), Adjacent.path()}));
        QCOMPARE(QFileInfo(Settings.FileName()).absolutePath(), QFileInfo(Executable.path()).absoluteFilePath());
        QCOMPARE(Settings.SizeStyle(true), SizeFormats().indexOf("kbytes"));
        QCOMPARE(Workspace.Object()["workspace"].toObject()["id"].toString(), QString("user"));
        QCOMPARE(Bytes(User.filePath("settings.json")), UserSettings);
        QVERIFY(!QFileInfo::exists(ProfileConfig::FileName(User.path())));
        // Legacy files beside the target take precedence over previous default locations.
        QTemporaryDir Local; QVERIFY(Write(Local.filePath("settings.json"), "{\"schemaVersion\":1,\"general\":{\"sizeInHeader\":\"adaptive1\"}}"));
        Repository Near(Local.path(), {User.path()}); QCOMPARE(Near.SizeStyle(true), SizeFormats().indexOf("adaptive1"));
        JsonStore NearWorkspace(Near.FileName(), {{"schemaVersion", 1}}, nullptr, JsonStore::Scope::Standalone, ProfilePartition(true, Local.path(), {User.path()}));
        QVERIFY(!NearWorkspace.Object().contains("workspace"));
    }
    void workspaceOnlyProfileIsFreshInstall()
    {
        QTemporaryDir Temp;
        QVERIFY(Write(ProfileConfig::FileName(Temp.path()), "{\"schemaVersion\":1,\"workspace\":{\"id\":\"w\"}}"));
        Repository Settings(Temp.path()); QVERIFY(Settings.Writable());
        QCOMPARE(Settings.Read(Section::General).toObject()["shortcutPreset"].toString(), QString(hc::defaults::FreshShortcutPreset));
        QVERIFY(Settings.Commit());
        QCOMPARE(QJsonDocument::fromJson(Bytes(Settings.FileName())).object()["workspace"].toObject()["id"].toString(), QString("w"));
        Repository Restart(Temp.path());
        QCOMPARE(Restart.Read(Section::General).toObject()["shortcutPreset"].toString(), QString(hc::defaults::FreshShortcutPreset));
    }
    void partialTypesUseDefaultsWithoutWriting()
    {
        QTemporaryDir Temp; const QByteArray Data("{\"schemaVersion\":1,\"general\":{\"useBuiltInShortcuts\":\"false\"},\"plugins\":\"invalid\"}");
        QVERIFY(Write(ProfileConfig::FileName(Temp.path()),Data)); Repository Settings(Temp.path());
        QVERIFY(Settings.Writable()); QVERIFY(!Settings.Issues().isEmpty());
        QVERIFY(Settings.Read(Section::General).toObject()["useBuiltInShortcuts"].toBool());
        QVERIFY(Settings.Plugins("wlx").isEmpty()); QCOMPARE(Bytes(Settings.FileName()),Data);
    }
};
QTEST_GUILESS_MAIN(SettingsTests)
#include "tstSettings.moc"
