#include "UniqueLibs/flatListingSession.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QtTest>

using namespace hc;

// 평면 보기 열거 세션: 고정 트리에서 항목 수 · 상대 경로 · 깊이 · 깊이 제한 · 표시 대상 · 취소.
class FlatListingTests : public QObject
{
    Q_OBJECT
    struct Run { std::vector<storage::EntrySnapshot> entries_; flat::Status status_; int batches_ = 0; };
    static Run list(flat::ListingOptions Options, int Interval = 10)
    {
        Run Result;
        flat::ListingSession Session(nullptr, storage::MakeNativeReadProvider(storage::LocatorKind::Local), std::move(Options));
        Session.append_ = [&](auto Entries) { ++Result.batches_; for (auto& Entry : Entries) Result.entries_.push_back(std::move(Entry)); };
        Session.status_ = [&](const flat::Status& Status) { Result.status_ = Status; };
        Session.Start(Interval);
        // 값을 돌려주는 함수라 QTRY_* 대신 직접 기다린다. 끝나지 않으면 호출부의 비교가 실패한다.
        QDeadlineTimer Deadline(15000);
        while (Session.Running() && !Deadline.hasExpired()) QTest::qWait(10);
        Result.status_ = Session.CurrentStatus();
        return Result;
    }
    QTemporaryDir dir_;
    QString base_;
private slots:
    void initTestCase()
    {
        QVERIFY(dir_.isValid()); QDir Root(dir_.path());
        for (const auto* Folder : {"d1/d2", "empty"}) QVERIFY(Root.mkpath(Folder));
        for (const auto* File : {"a.txt", "d1/b.txt", "d1/d2/c.txt"}) { QFile Out(Root.filePath(File)); QVERIFY(Out.open(QIODevice::WriteOnly)); Out.write(File); }
        base_ = flat::NormalizedBase(dir_.path());
    }
    void pathHelpers()
    {
        QCOMPARE(flat::NormalizedBase("C:/Work/"), QString("C:\\Work"));
        QCOMPARE(flat::NormalizedBase("C:"), QString("C:\\"));
        QCOMPARE(flat::NormalizedBase("C:\\"), QString("C:\\"));
        QCOMPARE(flat::RelativeParent("C:\\Work", "C:\\Work\\a\\b.txt"), QString("a"));
        QCOMPARE(flat::RelativeParent("C:\\Work", "C:\\Work\\b.txt"), QString());
        QCOMPARE(flat::RelativeParent("C:\\", "C:\\a\\b\\c.txt"), QString("a\\b"));
        QCOMPARE(flat::RelativeParent("c:\\work", "C:\\Work\\x\\y.txt"), QString("x"));
        QCOMPARE(flat::RelativeDepth(std::string("C:\\Work").size(), "C:\\Work\\file.txt"), 0);
        QCOMPARE(flat::RelativeDepth(std::string("C:\\Work").size(), "C:\\Work\\a\\b\\file.txt"), 2);
        QCOMPARE(flat::RelativeDepth(std::string("C:\\").size(), "C:\\a\\file.txt"), 1);
    }
    void filesWithDepth()
    {
        flat::ListingOptions Options; Options.base_ = base_; Options.roots_ = {{storage::LocatorKind::Local, base_.toStdString()}};
        const auto Result = list(Options);
        QVERIFY(Result.status_.completed_); QVERIFY(Result.status_.failure_.isEmpty());
        QCOMPARE(Result.entries_.size(), std::size_t(3)); QCOMPARE(Result.status_.published_, quint64(3));
        QCOMPARE(Result.status_.folders_, quint64(4)); // 기준 · d1 · d2 · empty
        QStringList Relative;
        for (const auto& Entry : Result.entries_) {
            QVERIFY(!Entry.directory_);
            QCOMPARE(QString::fromStdString(Entry.hints_.at("filesystemPath")), QString::fromStdString(Entry.locator_.root_));
            Relative.append(flat::RelativeParent(base_, QString::fromStdString(Entry.locator_.root_)) + "|"
                + QString::number(flat::RelativeDepth(base_.toStdString().size(), Entry.locator_.root_)));
        }
        Relative.sort();
        QCOMPARE(Relative, (QStringList{"d1\\d2|2", "d1|1", "|0"}));
    }
    void depthAndItems()
    {
        flat::ListingOptions Options; Options.base_ = base_; Options.roots_ = {{storage::LocatorKind::Local, base_.toStdString()}};
        Options.maxDepth_ = 1;
        QCOMPARE(list(Options).entries_.size(), std::size_t(2));
        Options.maxDepth_ = 0;
        QCOMPARE(list(Options).entries_.size(), std::size_t(1));
        Options.maxDepth_ = std::numeric_limits<int>::max(); Options.items_ = flat::Items::FilesAndFolders;
        QCOMPARE(list(Options).entries_.size(), std::size_t(6));
        Options.items_ = flat::Items::Folders;
        const auto Folders = list(Options);
        QCOMPARE(Folders.entries_.size(), std::size_t(3));
        for (const auto& Entry : Folders.entries_) QVERIFY(Entry.directory_);
    }
    void selectedRoots()
    {
        // 선택한 폴더만: 상대 경로는 여전히 기준 폴더 기준이다.
        flat::ListingOptions Options; Options.base_ = base_;
        Options.roots_ = {{storage::LocatorKind::Local, (base_ + "\\d1").toStdString()}};
        const auto Result = list(Options);
        QCOMPARE(Result.entries_.size(), std::size_t(2));
        for (const auto& Entry : Result.entries_) QVERIFY(flat::RelativeParent(base_, QString::fromStdString(Entry.locator_.root_)).startsWith("d1"));
    }
    void missingRootFails()
    {
        flat::ListingOptions Options; Options.base_ = base_ + "\\none";
        Options.roots_ = {{storage::LocatorKind::Local, (base_ + "\\none").toStdString()}};
        const auto Result = list(Options);
        QVERIFY(!Result.status_.failure_.isEmpty()); QVERIFY(!Result.status_.completed_); QCOMPARE(Result.entries_.size(), std::size_t(0));
    }
    void stopKeepsPartial()
    {
        flat::ListingOptions Options; Options.base_ = base_; Options.roots_ = {{storage::LocatorKind::Local, base_.toStdString()}};
        flat::ListingSession Session(nullptr, storage::MakeNativeReadProvider(storage::LocatorKind::Local), Options);
        int Appended = 0; Session.append_ = [&](auto Entries) { Appended += int(Entries.size()); };
        Session.Start(10); Session.Stop();
        QVERIFY(!Session.Running()); QVERIFY(Session.CurrentStatus().cancelled_);
        QTest::qWait(100);
        QVERIFY(Appended <= 3);
    }
};

QTEST_GUILESS_MAIN(FlatListingTests)
#include "tstFlatListing.moc"
