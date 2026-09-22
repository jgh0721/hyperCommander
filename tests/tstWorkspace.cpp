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
};
QTEST_GUILESS_MAIN(WorkspaceTests)
#include "tstWorkspace.moc"
