#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include "UniqueLibs/qtWorkspaceState.hpp"
#include "UniqueLibs/qtDirectoryTracker.hpp"

using namespace hc::workspace;
class WorkspaceTests : public QObject {
    Q_OBJECT
private:
    State example() {
        State Value;
        TabState Left; Left.id_ = "left-1"; Left.location_ = {hc::storage::LocatorKind::Local, "C:\\data"};
        Left.selection_ = {"selection"}; Left.focus_ = "focused"; Left.view_ = {{"hidden", true}};
        Left.history_ = {{hc::storage::LocatorKind::Smb, "\\\\server\\share"}};
        Value.left_.tabs_.append(Left);
        auto Right = Left; Right.id_ = "right-1"; Right.location_ = {hc::storage::LocatorKind::Plugin, "\\\\plugin\\missing\\"};
        Right.provider_ = "removed-plugin"; Value.right_.tabs_.append(Right);
        Value.geometry_ = QByteArray("\0\1\2", 3); Value.layout_ = "layout";
        Value.defaults_ = {{"columns", QJsonObject{{"name", QJsonObject{{"value", 25}, {"unit", "percent"}}}}}};
        return Value;
    }
private slots:
    void recoveryParentLocations_data()
    {
        using hc::storage::LocatorKind;
        QTest::addColumn<int>("Kind"); QTest::addColumn<QString>("Path");
        QTest::addColumn<int>("ParentKind"); QTest::addColumn<QString>("Parent");
        const auto Drives = QStringLiteral("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}");
        QTest::newRow("missing-child") << int(LocatorKind::Local) << "Z:\\gone\\child" << int(LocatorKind::Local) << "Z:\\gone";
        QTest::newRow("drive-parent") << int(LocatorKind::Local) << "Z:/gone/" << int(LocatorKind::Local) << "Z:\\";
        QTest::newRow("disconnected-root") << int(LocatorKind::Local) << "Z:\\" << int(LocatorKind::Shell) << Drives;
        QTest::newRow("extended-root") << int(LocatorKind::Local) << "\\\\?\\Z:\\" << int(LocatorKind::Shell) << Drives;
        QTest::newRow("unc-child") << int(LocatorKind::Smb) << "\\\\server\\share\\gone" << int(LocatorKind::Smb) << "\\\\server\\share";
        QTest::newRow("unc-share") << int(LocatorKind::Smb) << "\\\\server\\share\\" << int(LocatorKind::Smb) << "\\\\server";
        QTest::newRow("unc-server") << int(LocatorKind::Smb) << "\\\\server" << int(LocatorKind::Shell) << Drives;
        QTest::newRow("extended-unc") << int(LocatorKind::Smb) << "\\\\?\\UNC\\server\\share" << int(LocatorKind::Smb) << "\\\\server";
        QTest::newRow("missing-plugin") << int(LocatorKind::Plugin) << "\\\\plugin\\missing" << int(LocatorKind::Shell) << Drives;
        QTest::newRow("missing-archive") << int(LocatorKind::Archive) << "C:/gone/archive.zip" << int(LocatorKind::Local) << "C:\\gone";
    }
    void recoveryParentLocations()
    {
        QFETCH(int, Kind); QFETCH(QString, Path); QFETCH(int, ParentKind); QFETCH(QString, Parent);
        const auto Value = RecoveryParentLocation({hc::storage::LocatorKind(Kind), Path.toStdString()});
        QCOMPARE(int(Value.kind_), ParentKind); QCOMPARE(QString::fromStdString(Value.root_), Parent);
        QVERIFY(Value.nesting_.empty());
    }

    void navigationHistoryBranchesAndRoundTrips() {
        using namespace hc::storage;
        TabState Tab; Tab.id_ = "history";
        const Locator A{LocatorKind::Local, "C:/A"}, B{LocatorKind::Local, "C:/B"}, C{LocatorKind::Local, "C:/C"};
        RecordNavigation(Tab, A); RecordNavigation(Tab, B); RecordNavigation(Tab, C);
        QCOMPARE(Tab.historyIndex_, 2); QCOMPARE(Tab.history_.size(), 3);
        RecordNavigation(Tab, B, 1); QCOMPARE(Tab.historyIndex_, 1); QCOMPARE(Tab.history_.size(), 3);
        RecordNavigation(Tab, B); QCOMPARE(Tab.history_.size(), 3);
        RecordNavigation(Tab, A); QCOMPARE(Tab.historyIndex_, 2); QCOMPARE(Tab.history_.back(), A);
        Tab.location_ = A;
        auto Value = example(); Value.left_.tabs_ = {Tab}; Value.commandHistory_ = {"echo hello", "cd C:/A"};
        const auto Restored = Decode(Encode(Value)); QVERIFY(Restored.writable_);
        QCOMPARE(Restored.state_.left_.tabs_[0].historyIndex_, 2); QCOMPARE(Restored.state_.commandHistory_, Value.commandHistory_);
        for (int Index = 0; Index < 70; ++Index) RecordNavigation(Tab, {LocatorKind::Local, "C:/" + std::to_string(Index)});
        QCOMPARE(Tab.history_.size(), 64); QCOMPARE(Tab.historyIndex_, 63);
        auto Json = QJsonDocument::fromJson(Encode(Value)).object(); auto Pane = Json["left"].toObject();
        auto Tabs = Pane["tabs"].toArray(); auto Item = Tabs[0].toObject(); Item.remove("historyIndex"); Tabs[0] = Item; Pane["tabs"] = Tabs; Json["left"] = Pane;
        const auto Old = Decode(QJsonDocument(Json).toJson()); QVERIFY(Old.writable_); QCOMPARE(Old.state_.left_.tabs_[0].historyIndex_, 2);
        Item["historyIndex"] = 99; Tabs[0] = Item; Pane["tabs"] = Tabs; Json["left"] = Pane;
        QVERIFY(!Decode(QJsonDocument(Json).toJson()).writable_);
    }
    void revisionMetadataAndSearch() {
        using namespace hc::storage;
        EntrySnapshot Entry; Entry.name_ = "Report.txt"; Entry.size_ = 42;
        Entry.locator_ = {LocatorKind::Local, "C:/Report.txt"};
        Entry.id_ = {{"disk"}, "file-id", IdentityScope::Persistent, ResourceRevision{"v1"}};
        DirectoryIndex Index; Index.Publish({Entry}); const auto Generation = Index.Generation();
        QVERIFY(Index.Put(Entry, {{"author", "Kim"}}, Generation));
        QVERIFY(Index.Matches(Entry, "ext:txt author:kim")); QVERIFY(!Index.Matches(Entry, "ext:png"));
        Index.Publish({Entry}); QCOMPARE(Index.Metadata(Entry).value("author"), QString("Kim"));
        QVERIFY(Index.Put(Entry, {{"author", "Kim"}}, Generation));
        const auto Old = Entry;
        Entry.id_.revision_->token_ = "v2"; Index.Publish({Entry}); QVERIFY(Index.Metadata(Entry).isEmpty());
        QVERIFY(!Index.Put(Old, {{"author", "stale"}}, Generation));
        QVERIFY(!Index.Matches(Entry, "author:kim"));
        Entry.id_.revision_.reset(); Index.Publish({Entry}); QVERIFY(Index.Put(Entry, {{"author", "Lee"}}, Index.Generation()));
        const auto UnversionedGeneration = Index.Generation();
        Index.Publish({Entry}); QVERIFY(Index.Metadata(Entry).isEmpty());
        QVERIFY(!Index.Put(Entry, {{"author", "stale"}}, UnversionedGeneration));
        Index.Publish({}); QVERIFY(!Index.Put(Entry, {{"author", "gone"}}, Index.Generation()));
    }
    void pluginSelectionSurvivesHostRestart() {
        hc::storage::EntrySnapshot Entry;
        Entry.locator_ = {hc::storage::LocatorKind::Plugin, "wfx://session-a/folder/file"};
        const auto Key = SelectionKey(Entry); Entry.locator_.root_ = "wfx://session-b/folder/file";
        QCOMPARE(SelectionKey(Entry), Key);
        Entry.locator_.root_ = "wfx:session-a:\\folder\\file";
        QCOMPARE(SelectionKey(Entry), Key);
        Entry.locator_.root_ = "dopus:session-b:\\folder\\file";
        QCOMPARE(SelectionKey(Entry), Key);
        Entry.locator_.root_ = "wfx:session-a:\\..\\file";
        QVERIFY(PluginPath(Entry.locator_).isEmpty());
    }
    void roundTripAndInheritance() {
        const auto Value = example(); const auto Bytes = Encode(Value); const auto Restored = Decode(Bytes);
        QVERIFY(Restored.writable_); QCOMPARE(Encode(Restored.state_), Bytes);
        const auto Merged = MergeView(Value.defaults_, {{"columns", QJsonObject{{"name", QJsonObject{{"value", 40}}}}}});
        QCOMPARE(Merged["columns"].toObject()["name"].toObject()["unit"].toString(), QString("percent"));
        QCOMPARE(Merged["columns"].toObject()["name"].toObject()["value"].toInt(), 40);
    }
    void builtInProviderPathsFollowTheInstallLayout() {
        // Archive tabs saved before the layout change name <ROOT>/BuiltInPlugin/<Name>/<manifest>.
        auto Value = example(); auto& Tab = Value.right_.tabs_[0];
        Tab.provider_ = "D:/Utils/HyperCommander/BuiltInPlugin/SevenZip/sevenzip.json";
        Tab.historyConnections_ = {{"key", QJsonObject{{"provider", R"(D:\Utils\HyperCommander\BuiltInPlugin\IsoImage\isoimage.json)"}, {"nativeRoot", ""}}}};
        const auto Restored = Decode(Encode(Value)); QVERIFY(Restored.writable_);
        const auto& Migrated = Restored.state_.right_.tabs_[0];
        QCOMPARE(Migrated.provider_, QString("D:/Utils/HyperCommander/Third-Party/SevenZip/sevenzip.json"));
        QCOMPARE(Migrated.historyConnections_["key"].toObject()["provider"].toString(), QString("D:/Utils/HyperCommander/Addons/WCX/IsoImage/isoimage.json"));
        // User plugin paths and nested folders below a built-in name are left alone.
        Tab.provider_ = "C:/Plugins/BuiltInPlugin/SevenZip/sub/other.json";
        QCOMPARE(Decode(Encode(Value)).state_.right_.tabs_[0].provider_, Tab.provider_);
    }
    void legacyMigration() {
        const QJsonObject Pane{{"active", 0}, {"tabs", QJsonArray{QJsonObject{{"path", "C:\\legacy"}}}}};
        const auto Loaded = Decode(QJsonDocument(QJsonObject{{"version", 1}, {"id", "legacy"}, {"activePane", 0}, {"left", Pane}, {"right", Pane}}).toJson());
        QVERIFY(Loaded.writable_); QVERIFY(Loaded.state_.left_.tabs_[0].id_ != Loaded.state_.right_.tabs_[0].id_);
        QCOMPARE(QJsonDocument::fromJson(Encode(Loaded.state_)).object()["version"].toInt(), 2);
    }
    void corruptAndFutureStayUntouched() {
        QTemporaryDir Directory;
        const auto Path = Directory.filePath("workspace.json");
        for (const auto& Bytes : {QByteArray("broken"), QByteArray("{\"schemaVersion\":99}"), QByteArray(16 * 1024 * 1024 + 1, 'x')}) {
            { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size()); }
            hc::settings::JsonStore Settings(Path); auto Loaded = Load(Settings);
            QVERIFY(!Loaded.writable_); QVERIFY(!Save(Settings, Loaded, example()));
            QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Bytes);
        }
        auto Object = QJsonDocument::fromJson(Encode(example())).object();
        Object["right"] = Object["left"]; QVERIFY(!Decode(QJsonDocument(Object).toJson()).writable_);
    }
    void lockedSaveCanBeRetried() {
        QTemporaryDir Directory; hc::settings::JsonStore Settings(Directory.filePath("workspace.json")); auto Previous = Load(Settings);
        { QLockFile Lock(Settings.FileName() + ".lock"); QVERIFY(Lock.tryLock()); QVERIFY(!Save(Settings, Previous, example())); }
        QVERIFY(Save(Settings, Previous, example()));
        hc::settings::JsonStore Restart(Settings.FileName()); QCOMPARE(Load(Restart).state_.id_, example().id_);
    }
    void concurrentSessionPreserved() {
        QTemporaryDir Directory; hc::settings::JsonStore Settings(Directory.filePath("workspace.json"));
        auto First = Load(Settings); auto Second = First; QVERIFY(Save(Settings, First, example()));
        QVERIFY(!Save(Settings, Second, example()));
        auto Value = example(); Value.id_ = "next"; QVERIFY(Save(Settings, First, Value));
        QCOMPARE(Load(Settings).state_.id_, QString("next"));
    }
    void sessionIdentityNotPersisted() {
        hc::storage::EntrySnapshot Entry; Entry.id_ = {{"provider"}, "session-1"}; Entry.locator_.root_ = "C:\\file";
        const auto Before = SelectionKey(Entry); Entry.id_.key_ = "session-2"; QCOMPARE(SelectionKey(Entry), Before);
        Entry.id_.scope_ = hc::storage::IdentityScope::Persistent;
        const auto Persistent = SelectionKey(Entry); Entry.locator_.root_ = "C:\\renamed"; QCOMPARE(SelectionKey(Entry), Persistent);
    }
    void watchCoalescingAndLostEventRescan() {
        QTemporaryDir Directory; QtDirectoryTracker Tracker; Tracker.SetIntervals(20, 10000);
        QSignalSpy Refresh(&Tracker, &QtDirectoryTracker::RefreshRequired);
        Tracker.SetLocation("directory", Directory.path());
        for (int I = 0; I < 20; ++I) Tracker.Invalidate();
        QTRY_COMPARE(Refresh.size(), 1);
        Tracker.SetLocation("other"); Tracker.Invalidate(); Tracker.SetLocation({});
        QTest::qWait(50); QCOMPARE(Refresh.size(), 1);
        hc::refresh::Settings Settings; Settings.fallbackEnabled = true; Tracker.Configure(Settings);
        Tracker.SetIntervals(10, 40); Tracker.SetLocation("unwatchable-provider");
        QTRY_VERIFY(Refresh.size() > 1);
        Tracker.SetLocation({});
    }
    void realDirectoryNotification() {
        QTemporaryDir Directory; QtDirectoryTracker Tracker; Tracker.SetIntervals(10, 10000);
        QSignalSpy Refresh(&Tracker, &QtDirectoryTracker::RefreshRequired); Tracker.SetLocation("local", Directory.path());
        QFile File(Directory.filePath("new.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("new"); File.close();
        QTRY_VERIFY_WITH_TIMEOUT(!Refresh.isEmpty(), 3000);
    }
    void refreshFallbackAndSuspension() {
        QtDirectoryTracker Tracker; Tracker.SetIntervals(5, 30);
        QSignalSpy Refresh(&Tracker, &QtDirectoryTracker::RefreshRequired);
        Tracker.SetLocation("unsupported");
        QVERIFY(!Tracker.PollingActive()); QTest::qWait(100); QVERIFY(Refresh.isEmpty());
        hc::refresh::Settings Settings; Settings.fallbackEnabled = true; Tracker.Configure(Settings);
        Tracker.SetIntervals(5, 30); QVERIFY(Tracker.PollingActive());
        QTRY_VERIFY(Refresh.size() >= 2);
        Tracker.SetSuspended(true); const auto Count = Refresh.size();
        for (int I = 0; I < 10; ++I) Tracker.Invalidate();
        QTest::qWait(100); QCOMPARE(Refresh.size(), Count); QVERIFY(!Tracker.PollingActive());
        Tracker.SetBusy(true); Tracker.SetSuspended(false);
        QTest::qWait(80); QCOMPARE(Refresh.size(), Count);
        Tracker.SetBusy(false); QTRY_VERIFY(Refresh.size() > Count);
        Settings.fallbackEnabled = false; Tracker.Configure(Settings);
        Tracker.SetLocation({}); const auto Stopped = Refresh.size();
        QTest::qWait(80); QCOMPARE(Refresh.size(), Stopped);
    }
    void notificationFiltersAndExclusions() {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("existing.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("a"); File.close();
        QtDirectoryTracker Tracker; hc::refresh::Settings Settings;
        Settings.metadata = false; Settings.fallbackEnabled = true; Tracker.Configure(Settings);
        Tracker.SetIntervals(5, 30); Tracker.SetLocation("local", Directory.path());
        QVERIFY(Tracker.NotificationsAvailable()); QVERIFY(!Tracker.PollingActive());
        QSignalSpy Refresh(&Tracker, &QtDirectoryTracker::RefreshRequired);
        QVERIFY(File.open(QIODevice::Append)); File.write("more"); File.close();
        QTest::qWait(120); QVERIFY(Refresh.isEmpty());
        QVERIFY(QFile::rename(File.fileName(), Directory.filePath("renamed.txt")));
        QTRY_VERIFY(!Refresh.isEmpty()); Refresh.clear();
        Settings.names = false; Settings.metadata = true; Tracker.Configure(Settings);
        QFile Renamed(Directory.filePath("renamed.txt")); QVERIFY(Renamed.open(QIODevice::Append)); Renamed.write("metadata"); Renamed.close();
        QTRY_VERIFY(!Refresh.isEmpty());
        Tracker.SetLocation({}); Refresh.clear();
        Settings.excludedDrives = "X:;Y:"; Tracker.Configure(Settings);
        Tracker.SetLocation("excluded", {}, "X:/folder");
        QVERIFY(!Tracker.PollingActive()); Tracker.Invalidate(); QTest::qWait(60); QVERIFY(Refresh.isEmpty());
        QVERIFY(Settings.Excludes("x:/folder")); QVERIFY(!Settings.Excludes("Z:/folder"));
        Settings.excludedDrives = "\\\\server\\share";
        QVERIFY(Settings.Excludes("\\\\SERVER\\share\\child")); QVERIFY(!Settings.Excludes("\\\\server\\share-other"));
    }
};
QTEST_GUILESS_MAIN(WorkspaceTests)
#include "tstWorkspace.moc"
