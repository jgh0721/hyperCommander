#include "commonLibs/storage/search.hpp"
#include "UniqueLibs/qtSearch.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "memoryStorage.hpp"
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
        const auto Factory = hc::search::MakeEverythingSearch(QCoreApplication::applicationFilePath());
        auto Cursor = Factory->Search({{LocatorKind::Local, Dir.path().toStdString()}}, [](const auto&) { return true; }); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(2, {}).get(); QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().freshness_, SearchFreshness::IndexUnverified); QCOMPARE(Page.Value().matches_.size(), 1u);
        QCOMPARE(Page.Value().matches_[0].size_, 5u); QVERIFY(Page.Value().partial_); QVERIFY(!Page.Value().end_);
        Page = Cursor.Value()->Next(2, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_); QVERIFY(Page.Value().partial_); QVERIFY(Page.Value().matches_.empty());
        Cursor = Factory->Search({{LocatorKind::Local, Dir.path().toStdString()}}, [](const auto&) { return true; });
        qputenv("HC_SEARCH_TEST_HELPER", "slow"); std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Pending = Cursor.Value()->Next(2, Context); QTest::qWait(100); Stop.request_stop();
        QVERIFY(Pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
        QCOMPARE(Pending.get().Failure().category_, ErrorCategory::Cancelled);
        qunsetenv("HC_SEARCH_TEST_HELPER");
    }
};
int main(int Argc, char** Argv) {
    if (Argc == 4 && qEnvironmentVariableIsSet("HC_SEARCH_TEST_HELPER")) {
        QCoreApplication App(Argc, Argv);
        if (qEnvironmentVariable("HC_SEARCH_TEST_HELPER") == "slow") QThread::msleep(5000);
        QFile Fixture(QDir(App.arguments()[1]).filePath("index.json")); if (!Fixture.open(QIODevice::ReadOnly)) return 2;
        const auto All = QJsonDocument::fromJson(Fixture.readAll()).array(); QJsonArray Page;
        const int Offset = App.arguments()[2].toInt(), Limit = App.arguments()[3].toInt();
        for (int I = Offset; I < All.size() && I < Offset + Limit; ++I) Page.append(All[I]);
        QFile Output; Output.open(stdout, QIODevice::WriteOnly);
        Output.write(QJsonDocument(QJsonObject{{"paths", Page}, {"total", All.size()}}).toJson()); return 0;
    }
    QApplication App(Argc, Argv); SearchTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstSearch.moc"
