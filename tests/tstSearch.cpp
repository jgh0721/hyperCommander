#include "commonLibs/storage/search.hpp"
#include "UniqueLibs/qtSearch.hpp"
#include "UniqueLibs/everythingSearchSession.hpp"
#include "everythingStreamFixture.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "memoryStorage.hpp"
#include "UniqueLibs/folderSize.hpp"
#include "tools/searchIndexPath.hpp"
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
#include <set>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QDialog>
#include <QLabel>

using namespace hc::storage;
class SearchTests : public QObject {
    Q_OBJECT
private slots:
    void everythingDisplayIntervalSettings() {
        using hc::quickfind::Settings;
        QCOMPARE(Settings::FromJson({}).everythingDisplayIntervalMs_, 500);
        for (const auto Value : {QJsonValue(0), QJsonValue(-1), QJsonValue(49), QJsonValue(5001), QJsonValue(500.5), QJsonValue("1000"), QJsonValue(true)})
            QCOMPARE(Settings::FromJson({{"everythingDisplayIntervalMs", Value}}).everythingDisplayIntervalMs_, 500);
        for (const int Value : {50, 501, 1000, 5000}) {
            const auto SettingsValue = Settings::FromJson({{"everythingDisplayIntervalMs", Value}});
            QCOMPARE(SettingsValue.everythingDisplayIntervalMs_, Value);
            QCOMPARE(Settings::FromJson(SettingsValue.ToJson()).everythingDisplayIntervalMs_, Value);
        }
    }
    void everythingStreamAllResultsAndCancellation() {
        QTemporaryDir Dir; QVERIFY(Dir.isValid()); QJsonArray Paths;
        for (int I = 0; I < 4101; ++I) {
            const auto Path = Dir.filePath(QStringLiteral("검색-%1.txt").arg(I));
            QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("test"); Paths.append(Path);
        }
        QFile Fixture(Dir.filePath("input.json")); QVERIFY(Fixture.open(QIODevice::WriteOnly));
        Fixture.write(QJsonDocument(Paths).toJson()); Fixture.close();
        qputenv("HC_EVERYTHING_TEST_INPUT", Fixture.fileName().toUtf8()); qputenv("HC_EVERYTHING_TEST_MODE", "slow");
        const auto Restore = qScopeGuard([] { qunsetenv("HC_EVERYTHING_TEST_INPUT"); qunsetenv("HC_EVERYTHING_TEST_MODE"); });
        hc::search::EverythingSession Session(nullptr, MakeNativeReadProvider(LocatorKind::Local), MakeNativeReadProvider(LocatorKind::Smb));
        std::set<std::string> Seen; QString Status; int Calls = 0;
        Session.append_ = [&](auto Entries) { ++Calls; for (const auto& Entry : Entries) { QVERIFY(Entry.id_.revision_); QVERIFY(Entry.hints_.contains("filesystemPath")); Seen.insert(Entry.locator_.root_); } };
        Session.status_ = [&](const QString& Text) { Status = Text; };
        Session.Start("검색", "contains", 5000, QCoreApplication::applicationFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!Seen.empty(), 5000); QVERIFY(Seen.size() < 4101);
        Session.SetDisplayInterval(50);
        QTRY_VERIFY_WITH_TIMEOUT(!Session.Running(), 30000);
        QCOMPARE(Seen.size(), size_t(4101)); QVERIFY(Calls > 1); QVERIFY(!Status.contains("부분"));
        hc::search::EverythingSession Cancelled(nullptr, MakeNativeReadProvider(LocatorKind::Local), MakeNativeReadProvider(LocatorKind::Smb));
        int Appended = 0; Cancelled.append_ = [&](auto Entries) { Appended += int(Entries.size()); };
        Cancelled.Start("검색", "starts", 50, QCoreApplication::applicationFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(Appended > 0, 5000); Cancelled.Cancel(); const auto Before = Appended;
        QTest::qWait(250); QCOMPARE(Appended, Before); QVERIFY(!Cancelled.Running());
    }
    void everythingStreamProtocolFailure() {
        QTemporaryDir Dir; QFile Fixture(Dir.filePath("input.json")); QVERIFY(Fixture.open(QIODevice::WriteOnly));
        Fixture.write(QJsonDocument(QJsonArray{Dir.filePath("missing.txt")}).toJson()); Fixture.close();
        qputenv("HC_EVERYTHING_TEST_INPUT", Fixture.fileName().toUtf8()); qputenv("HC_EVERYTHING_TEST_MODE", "bad-sequence");
        const auto Restore = qScopeGuard([] { qunsetenv("HC_EVERYTHING_TEST_INPUT"); qunsetenv("HC_EVERYTHING_TEST_MODE"); });
        hc::search::EverythingSession Session(nullptr, MakeNativeReadProvider(LocatorKind::Local), MakeNativeReadProvider(LocatorKind::Smb));
        QString Status; Session.status_ = [&](const QString& Text) { Status = Text; };
        Session.Start("a", "contains", 50, QCoreApplication::applicationFilePath());
        QTRY_VERIFY_WITH_TIMEOUT(!Session.Running(), 5000); QVERIFY(Status.contains("부분"));
    }
    void folderSizeTraversalAndPolicies() {
        test::MemoryProvider Provider;
        const auto Folder = Provider.Add(Provider.Root(), "folder", {}, true);
        const auto Root = Provider.Stat(Folder, {}).get().Value();
        Provider.Add(Root.locator_, "one", std::vector<std::byte>(3));
        const auto Nested = Provider.Add(Root.locator_, "nested", {}, true);
        Provider.Add(Provider.Stat(Nested, {}).get().Value().locator_, "two", std::vector<std::byte>(7));
        OperationContext Context; Context.maxPageItems_ = 1;
        auto Size = hc::folders::Measure(Provider, Root, Context); QVERIFY(Size.HasValue()); QCOMPARE(Size.Value(), 10u);
        const auto Empty = Provider.Add(Root.locator_, "empty", {}, true);
        Size = hc::folders::Measure(Provider, Provider.Stat(Empty, {}).get().Value(), Context); QVERIFY(Size.HasValue()); QCOMPARE(Size.Value(), 0u);
        auto Missing = Root; Missing.locator_.root_ += "/missing";
        QVERIFY(!hc::folders::Measure(Provider, Missing, Context).HasValue());
        Context.maxEnumerationEntries_ = 1; QVERIFY(!hc::folders::Measure(Provider, Root, Context).HasValue());
        std::stop_source Stop; Stop.request_stop(); Context.stopToken_ = Stop.get_token();
        Size = hc::folders::Measure(Provider, Root, Context); QVERIFY(!Size.HasValue()); QCOMPARE(Size.Failure().category_, ErrorCategory::Cancelled);
        const Locator Local{LocatorKind::Local, "C:\\fixture"}, Network{LocatorKind::Smb, "\\\\server\\share"};
        QVERIFY(!hc::folders::Automatic("manual", Local)); QVERIFY(hc::folders::Automatic("local", Local));
        QVERIFY(!hc::folders::Automatic("local", Network)); QVERIFY(!hc::folders::Automatic("all", Network));
        QVERIFY(!hc::folders::Automatic("everything", Network)); QVERIFY(!hc::folders::Automatic("all", {LocatorKind::Archive, "archive.zip"}));
        const Locator UncLocal{LocatorKind::Local, "\\\\server\\share\\folder"};
        const Locator ExtendedUnc{LocatorKind::Local, "\\\\?\\UNC\\server\\share\\folder"};
        for (const auto& At : {Network, UncLocal, ExtendedUnc}) {
            QVERIFY(hc::folders::IsNetwork(At));
            for (const auto* Mode : {"manual", "local", "all", "everything"}) QVERIFY(!hc::folders::Automatic(Mode, At));
            auto Remote = Root; Remote.locator_ = At;
            const auto Denied = hc::folders::Measure(Provider, Remote, {});
            QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("folder-size-network-disabled"));
            const auto IndexDenied = hc::folders::Indexed(QString::fromStdString(At.root_), {}, "must-not-start.exe");
            QVERIFY(!IndexDenied.HasValue()); QCOMPARE(IndexDenied.Failure().code_, std::string("folder-size-network-disabled"));
        }
        const auto Drives = GetLogicalDrives();
        for (int I = 0; I < 26; ++I) if (Drives & (1u << I)) {
            const auto Drive = QString(QChar('A' + I)) + ":\\";
            if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(Drive.utf16())) == DRIVE_REMOTE)
                QVERIFY(hc::folders::IsNetwork({LocatorKind::Local, (Drive + "folder").toStdString()}));
        }
    }
    void folderSizeIndexParentPaths() {
        QVERIFY(hc::search::IndexParentMatches("D:", "D:/"));
        QVERIFY(hc::search::IndexParentMatches("d:", "D:\\"));
        QVERIFY(hc::search::IndexParentMatches("D:\\", "D:/"));
        QVERIFY(hc::search::IndexParentMatches("D:\\AI", "d:/AI/"));
        QVERIFY(hc::search::IndexParentMatches("\\\\server\\share", "//server/share/"));
        QVERIFY(!hc::search::IndexParentMatches("C:", "D:/"));
        QVERIFY(!hc::search::IndexParentMatches("D:", "D:/AI"));
        QVERIFY(!hc::search::IndexParentMatches("D:\\AI\\nested", "D:/AI"));
        QVERIFY(!hc::search::IndexParentMatches("D:AI", "D:/AI"));
    }
    void folderSizeIndexProtocol() {
        const auto Previous = qgetenv("HC_FOLDER_SIZE_TEST_HELPER");
        const auto Restore = qScopeGuard([&] { if (Previous.isNull()) qunsetenv("HC_FOLDER_SIZE_TEST_HELPER"); else qputenv("HC_FOLDER_SIZE_TEST_HELPER", Previous); });
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        qputenv("HC_FOLDER_SIZE_TEST_HELPER", "valid");
        auto Sizes = hc::folders::Indexed(Directory.path(), {}, QCoreApplication::applicationFilePath());
        QVERIFY(Sizes.HasValue()); QCOMPARE(Sizes.Value().value("empty"), 0u); QCOMPARE(Sizes.Value().value("large"), 9007199254740993ull);
        qputenv("HC_FOLDER_SIZE_TEST_HELPER", "malformed");
        QVERIFY(!hc::folders::Indexed(Directory.path(), {}, QCoreApplication::applicationFilePath()).HasValue());
        qputenv("HC_FOLDER_SIZE_TEST_HELPER", "slow"); std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        Sizes = hc::folders::Indexed(Directory.path(), Context, QCoreApplication::applicationFilePath());
        QVERIFY(!Sizes.HasValue()); QCOMPARE(Sizes.Failure().category_, ErrorCategory::Cancelled);
    }
    void recursivePagesAndLimits() {
        auto Provider = std::make_shared<test::MemoryProvider>();
        const auto Root = Provider->Root();
        auto Folder = Provider->Add(Root, "folder", {}, true);
        const auto Child = Provider->Stat(Folder, {}).get().Value().locator_;
        for (int I = 0; I < 600; ++I) Provider->Add(I < 300 ? Root : Child, "file" + std::to_string(I) + ".txt");
        auto Factory = MakeEnumeratingSearch(Provider);
        auto Open = Factory->Search({Root}, [](const auto& Entry) { return !Entry.directory_; }); QVERIFY(Open.HasValue());
        std::set<std::string> Names;
        for (int Pages = 0; Pages < 100; ++Pages) {
            auto Page = Open.Value()->Next(17, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().matches_.size() <= 17);
            for (const auto& Entry : Page.Value().matches_) QVERIFY(Names.insert(Entry.name_).second);
            if (Page.Value().end_) break;
        }
        QCOMPARE(Names.size(), 600u);
        auto Shallow = Factory->Search({Root, false}, [](const auto&) { return true; });
        int Count = 0; bool End = false;
        while (!End) { auto Page = Shallow.Value()->Next(256, {}).get(); QVERIFY(Page.HasValue()); Count += int(Page.Value().matches_.size()); End = Page.Value().end_; }
        QCOMPARE(Count, 301);
        auto Limited = Factory->Search({Root, true, 32, 10}, [](const auto&) { return true; });
        auto Page = Limited.Value()->Next(256, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().partial_); QVERIFY(Page.Value().end_); QCOMPARE(Page.Value().visited_, 10u);
        auto Empty = Factory->Search({Root}, [](const auto&) { return false; });
        auto First = Empty.Value()->Next(3, {}).get(); QVERIFY(First.HasValue()); QVERIFY(!First.Value().end_); QCOMPARE(First.Value().visited_, 256u);
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        QCOMPARE(Open.Value()->Next(5, Context).get().Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Open.Value()->Next(0, {}).get().Failure().category_, ErrorCategory::InvalidRequest);
        Context = {}; Context.deadline_ = std::chrono::steady_clock::now();
        QCOMPARE(Open.Value()->Next(5, Context).get().Failure().code_, std::string("deadline-exceeded"));
    }
    void unicodeAndMetadata() {
        EntrySnapshot Entry; Entry.name_ = "한글 ÉTÉ.txt"; Entry.locator_ = {LocatorKind::Local, "C:/한글/ÉTÉ.txt"}; Entry.id_ = {{"local"}, "id", IdentityScope::Persistent, ResourceRevision{"1"}};
        hc::search::MetadataSnapshot Values{{QString::fromStdString(Serialize(Entry.id_)), {{"author", "작성자"}}}};
        auto Matcher = hc::search::Compile("name:été ext:TXT author:작성", Values); QVERIFY(Matcher.HasValue()); QVERIFY(Matcher.Value()(Entry));
        Entry.id_.revision_ = ResourceRevision{"2"}; QVERIFY(!Matcher.Value()(Entry));
        QVERIFY(!hc::search::Compile("name:").HasValue());
        Entry.directory_ = true; QVERIFY(!hc::search::Compile("ext:txt").Value()(Entry));
    }
    void archiveScope() {
        Locator Root{LocatorKind::Archive, "fixture.zip", {{"wcx", "/"}}}; auto Child = Root; Child.nesting_[0].entry_ = "/nested";
        EntrySnapshot Folder; Folder.directory_ = true; Folder.name_ = "nested"; Folder.locator_ = Child;
        EntrySnapshot File; File.name_ = "한글.txt"; File.locator_ = Child; File.locator_.nesting_[0].entry_ += "/한글.txt";
        auto Provider = MakeArchiveReadProvider(Root, {{Root, {Folder}}, {Child, {File}}});
        auto Cursor = MakeEnumeratingSearch(Provider)->Search({Root}, hc::search::Compile("ext:txt").Value()); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(10, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().matches_.size(), 1u); QCOMPARE(Page.Value().matches_[0].locator_, File.locator_);
        QVERIFY(!hc::search::MakeEverythingSearch("missing.exe")->Search({Root}, [](const auto&) { return true; }).HasValue());
    }
    void nativeFreshnessAndDialog() {
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        QFile File(Dir.filePath("search.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("before"); File.close();
        const Locator Root{LocatorKind::Local, QDir::toNativeSeparators(Dir.path()).toStdString()}; auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        auto Cursor = MakeEnumeratingSearch(Provider)->Search({Root}, [](const auto&) { return true; });
        auto Page = Cursor.Value()->Next(10, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().matches_.size(), 1u);
        const auto Original = Page.Value().matches_[0]; QVERIFY(File.open(QIODevice::WriteOnly)); File.write("different size"); File.close();
        QCOMPARE(Provider->Stat(Original.id_, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        QWidget Parent; bool Activated = false;
        hc::search::Show(&Parent, Provider, Root, {}, [&](const auto&) { Activated = true; });
        auto* Dialog = Parent.findChild<QDialog*>("hc.searchDialog"); QVERIFY(Dialog);
        Dialog->findChild<QLineEdit*>("hc.search.query")->setText("ext:txt");
        auto* Start = Dialog->findChild<QPushButton*>("hc.search.start"); Start->click();
        auto* Results = Dialog->findChild<QTreeWidget*>("hc.search.results");
        QTRY_COMPARE_WITH_TIMEOUT(Results->topLevelItemCount(), 1, 5000);
        QVERIFY(Dialog->findChild<QLabel*>("hc.search.status")->text().contains(QString::fromUtf8("완료")));
        QMetaObject::invokeMethod(Results, "itemDoubleClicked", Q_ARG(QTreeWidgetItem*, Results->topLevelItem(0)), Q_ARG(int, 0)); QVERIFY(Activated);
        QPointer<QDialog> Guard(Dialog); Dialog->close(); QTRY_VERIFY(Guard.isNull());
        auto Missing = hc::search::MakeEverythingSearch(Dir.filePath("absent.exe"))->Search({Root}, [](const auto&) { return true; });
        QVERIFY(Missing.HasValue()); QCOMPARE(Missing.Value()->Next(10, {}).get().Failure().code_, std::string("search-index-helper-unavailable"));
    }
    void indexPagesScopeAndCancellation() {
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        const auto Path = Dir.filePath(QString::fromUtf8("한글.txt"));
        { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("fresh"); }
        QJsonArray Paths{Path, Dir.path() + "-outside/escape.txt", Dir.filePath("removed.txt"), Path};
        { QFile Fixture(Dir.filePath("index.json")); QVERIFY(Fixture.open(QIODevice::WriteOnly)); Fixture.write(QJsonDocument(Paths).toJson()); }
        qputenv("HC_SEARCH_TEST_HELPER", "1");
        const auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        const auto Factory = hc::search::MakeEverythingSearch(QCoreApplication::applicationFilePath(), Reader);
        auto Cursor = Factory->Search({{LocatorKind::Local, Dir.path().toStdString()}}, [](const auto&) { return true; }); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(2, {}).get(); QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().freshness_, SearchFreshness::IndexUnverified); QCOMPARE(Page.Value().matches_.size(), 1u);
        QCOMPARE(Page.Value().matches_[0].size_, 5u); QVERIFY(Page.Value().partial_); QVERIFY(!Page.Value().end_);
        const auto Original = Reader->Resolve({LocatorKind::Local, QDir::toNativeSeparators(Path).toStdString()}, {}).get(); QVERIFY(Original.HasValue());
        QCOMPARE(Page.Value().matches_[0].id_, Original.Value().id_); // Reuse the tab's provider session for metadata identity.
        Page = Cursor.Value()->Next(2, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_); QVERIFY(Page.Value().partial_); QVERIFY(Page.Value().matches_.empty());
        Cursor = Factory->Search({{LocatorKind::Local, Dir.path().toStdString()}}, [](const auto&) { return true; });
        qputenv("HC_SEARCH_TEST_HELPER", "slow"); std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Pending = Cursor.Value()->Next(2, Context); QTest::qWait(100); Stop.request_stop();
        QVERIFY(Pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        QCOMPARE(Pending.get().Failure().category_, ErrorCategory::Cancelled);
        qunsetenv("HC_SEARCH_TEST_HELPER");
    }
    void externalProviders() {
        const auto Local = qEnvironmentVariable("HC_SEARCH_LIVE_SCOPE"), Smb = qEnvironmentVariable("HC_SEARCH_LIVE_SMB");
        if (Local.isEmpty() && Smb.isEmpty()) QSKIP("Opt-in real Everything/SMB environment validation");
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        if (!Local.isEmpty()) {
            auto Reader = MakeNativeReadProvider(LocatorKind::Local);
            auto Search = hc::search::MakeEverythingSearch(QCoreApplication::applicationDirPath() + "/hc-search-index.exe", Reader);
            auto Open = Search->Search({{LocatorKind::Local, Local.toStdString()}}, hc::search::Compile("ext:txt").Value()); QVERIFY(Open.HasValue());
            std::set<std::string> Names; bool End = false, Unicode = false;
            for (int I = 0; I < 20 && !End; ++I) {
                auto Page = Open.Value()->Next(64, Context).get(); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
                QCOMPARE(Page.Value().freshness_, SearchFreshness::IndexUnverified); QVERIFY(!Page.Value().partial_);
                for (const auto& Entry : Page.Value().matches_) {
                    QVERIFY(Names.insert(Serialize(Entry.locator_)).second); QVERIFY(Reader->Stat(Entry.id_, Context).get().HasValue());
                    if (Entry.name_ == "한글.txt") Unicode = true;
                }
                End = Page.Value().end_;
            }
            QVERIFY(End); QCOMPARE(Names.size(), 301u); QVERIFY(Unicode); qInfo() << "Real Everything: 301 files, paged, unique, revision checked";
        }
        if (!Smb.isEmpty()) {
            auto Search = MakeEnumeratingSearch(MakeNativeReadProvider(LocatorKind::Smb));
            auto Open = Search->Search({{LocatorKind::Smb, Smb.toStdString()}, false, 0, 256}, [](const auto&) { return true; }); QVERIFY(Open.HasValue());
            auto Page = Open.Value()->Next(32, Context).get(); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            QVERIFY(Page.Value().matches_.size() <= 32); QCOMPARE(Page.Value().freshness_, SearchFreshness::Enumerated);
            qInfo() << "Real SMB read-only search:" << Page.Value().matches_.size() << "results";
        }
    }
};
int main(int Argc, char** Argv) {
    if (Argc == 4 && QByteArray(Argv[1]) == "--quick-find-stream" && qEnvironmentVariableIsSet("HC_EVERYTHING_TEST_INPUT"))
        return EverythingStreamFixture(Argc, Argv);
    if (Argc == 3 && QByteArray(Argv[1]) == "--folder-sizes" && qEnvironmentVariableIsSet("HC_FOLDER_SIZE_TEST_HELPER")) {
        QCoreApplication App(Argc, Argv);
        const auto Mode = qgetenv("HC_FOLDER_SIZE_TEST_HELPER"); if (Mode == "slow") QThread::msleep(5000);
        QFile Output; if (!Output.open(stdout, QIODevice::WriteOnly)) return 6;
        Output.write(Mode == "malformed" ? "{\"sizes\":[{\"name\":\"bad\",\"bytes\":\"-1\"}]}"
            : "{\"sizes\":[{\"name\":\"empty\",\"bytes\":\"0\"},{\"name\":\"large\",\"bytes\":\"9007199254740993\"}]}");
        return 0;
    }
    if (Argc == 4 && qEnvironmentVariableIsSet("HC_SEARCH_TEST_HELPER")) {
        QCoreApplication App(Argc, Argv);
        if (qEnvironmentVariable("HC_SEARCH_TEST_HELPER") == "slow") QThread::msleep(5000);
        QFile Fixture(QDir(App.arguments()[1]).filePath("index.json")); if (!Fixture.open(QIODevice::ReadOnly)) return 2;
        const auto All = QJsonDocument::fromJson(Fixture.readAll()).array(); QJsonArray Page;
        const int Offset = App.arguments()[2].toInt(), Limit = App.arguments()[3].toInt();
        for (int I = Offset; I < All.size() && I < Offset + Limit; ++I) Page.append(All[I]);
        QFile Output; if (!Output.open(stdout, QIODevice::WriteOnly)) return 3;
        Output.write(QJsonDocument(QJsonObject{{"paths", Page}, {"total", All.size()}}).toJson()); return 0;
    }
    QApplication App(Argc, Argv); SearchTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstSearch.moc"
