#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#define NOMINMAX
#include <Windows.h>
#include <psapi.h>
#include "UniqueLibs/qtPagedStorage.hpp"
#include "UniqueLibs/directoryRefreshOrder.hpp"
#include "UniqueLibs/qtItemOutcomeStore.hpp"
#include "commonLibs/columns/columnSort.hpp"

using namespace hc::storage;
using namespace std::string_literals;
namespace
{
    class GeneratedCursor final : public IEntryCursor
    {
        std::uint64_t count_, next_ = 0;
    public:
        explicit GeneratedCursor(std::uint64_t Count) : count_(Count) {}
        Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override
        {
            if (auto Failure = Context.Check({})) return Ready<EntryPage>(*Failure);
            EntryPage Page;
            for (std::size_t I = 0; I < MaxItems && next_ < count_; ++I, ++next_) {
                EntrySnapshot Entry; Entry.name_ = "file" + std::to_string(count_ - next_);
                Entry.id_ = {{"generated"}, Entry.name_, IdentityScope::Persistent, ResourceRevision{"revision"}};
                Entry.locator_ = {LocatorKind::Archive, "sample.iso", {{"iso", Entry.name_}}};
                Entry.size_ = count_ - next_; Page.entries_.push_back(std::move(Entry));
            }
            Page.end_ = next_ == count_; return Ready<EntryPage>(std::move(Page));
        }
    };
    class RevisionCursor final : public IEntryCursor
    {
        GeneratedCursor inner_{10};
        int pages_ = 0;
        bool change_;
    public:
        explicit RevisionCursor(bool Change) : change_(Change) {}
        Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override
        {
            auto Page = inner_.Next(MaxItems, Context).get();
            if (Page.HasValue()) Page.Value().revision_ = ResourceRevision{change_ && pages_++ ? "changed" : "snapshot"};
            return Ready<EntryPage>(std::move(Page));
        }
    };
    SIZE_T PeakMemory()
    {
        PROCESS_MEMORY_COUNTERS_EX Info{}; Info.cb = sizeof(Info);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Info), sizeof(Info));
        return Info.PeakWorkingSetSize;
    }
}
class PagedStorageTests : public QObject
{
    Q_OBJECT
private slots:
    void codecAndRecords()
    {
        EntrySnapshot Entry; Entry.id_ = {{"p"}, "opaque\0id"s, IdentityScope::Persistent, ResourceRevision{"r"}};
        Entry.locator_ = {LocatorKind::Archive, "C:/한글.egg", {{"egg", "a"}, {"iso", "b"}}};
        Entry.name_ = "한글.txt"; Entry.size_ = std::numeric_limits<std::uint64_t>::max();
        Entry.modifiedUtcMs_ = -123; Entry.createdUtcMs_ = 123; Entry.attributes_ = 0xffffffff;
        Entry.hints_ = {{"one", "two"}};
        const auto Bytes = EncodeEntry(Entry); auto Decoded = DecodeEntry(Bytes); QVERIFY(Decoded.HasValue());
        QCOMPARE(Decoded.Value().id_, Entry.id_); QCOMPARE(Decoded.Value().locator_, Entry.locator_);
        QCOMPARE(EncodeEntry(Decoded.Value()), Bytes); QVERIFY(!DecodeEntry(Bytes.chopped(1)).HasValue());
        QString Directory;
        {
            auto Store = DiskRecords::Create(); QVERIFY(Store.HasValue()); Directory = Store.Value()->Directory();
            QVERIFY(Store.Value()->Append("first").HasValue()); QVERIFY(Store.Value()->Append("second").HasValue());
            QVERIFY(Store.Value()->Replace(0, "a much larger replacement").HasValue());
            QCOMPARE(Store.Value()->Count(), 2ULL); QCOMPARE(Store.Value()->Read(0).Value(), "a much larger replacement");
            QCOMPARE(Store.Value()->Read(1).Value(), "second"); QVERIFY(!Store.Value()->Read(2).HasValue());
            QVERIFY(!Store.Value()->Replace(2, "bad").HasValue());
        }
        QVERIFY(!QDir(Directory).exists());
    }
    void appendNewEntriesAcrossPages()
    {
        OperationContext Context; Context.maxPageItems_ = 127;
        GeneratedCursor OldCursor(4200), NewCursor(4300);
        auto Old = PagedEntryIndex::Build(OldCursor, Context); QVERIFY(Old.HasValue());
        const auto All = [](const EntrySnapshot&) { return true; };
        auto Sorted = Old.Value()->Project([](const auto& A, const auto& B) { return A.name_ < B.name_; }, All, Context, {});
        QVERIFY(Sorted.HasValue());
        auto New = PagedEntryIndex::Build(NewCursor, Context); QVERIFY(New.HasValue());
        auto Ordered = hc::refresh::AppendNewEntries(New.Value(), Sorted.Value(), {}, Context); QVERIFY(Ordered.HasValue());
        QCOMPARE(Ordered.Value()->Count(), 4300ULL);
        for (std::uint64_t Offset = 0; Offset < 4200; Offset += 127) {
            const auto Count = std::min<std::uint64_t>(127, 4200 - Offset);
            auto Expected = Sorted.Value()->Read(Offset, Count, Context), Actual = Ordered.Value()->Read(Offset, Count, Context);
            QVERIFY(Expected.HasValue()); QVERIFY(Actual.HasValue());
            for (std::size_t I = 0; I < Count; ++I) QCOMPARE(Actual.Value().entries_[I].name_, Expected.Value().entries_[I].name_);
        }
        auto Tail = Ordered.Value()->Read(4200, 100, Context); QVERIFY(Tail.HasValue());
        for (const auto& Entry : Tail.Value().entries_) QVERIFY(QString::fromStdString(Entry.name_).mid(4).toInt() > 4200);
        // A second refresh preserves arrival order and removes deleted entries.
        GeneratedCursor ThirdCursor(4250); auto Third = PagedEntryIndex::Build(ThirdCursor, Context); QVERIFY(Third.HasValue());
        auto Again = hc::refresh::AppendNewEntries(Third.Value(), Ordered.Value(), {}, Context); QVERIFY(Again.HasValue());
        QCOMPARE(Again.Value()->Count(), 4250ULL);
        auto First = Again.Value()->Read(0, 20, Context); QVERIFY(First.HasValue());
        auto Expected = Sorted.Value()->Read(0, 20, Context); QVERIFY(Expected.HasValue());
        for (int I = 0; I < 20; ++I) QCOMPARE(First.Value().entries_[I].name_, Expected.Value().entries_[I].name_);
        std::stop_source Stop; Stop.request_stop(); Context.stopToken_ = Stop.get_token();
        QVERIFY(!hc::refresh::AppendNewEntries(New.Value(), Old.Value(), {}, Context).HasValue());
    }
    void stableMergeFilterSelectionAndCancellation()
    {
        GeneratedCursor Cursor(101); auto Index = PagedEntryIndex::Build(Cursor, {}); QVERIFY(Index.HasValue());
        auto Sorted = Index.Value()->Project([](const auto& A, const auto& B) { return A.size_ % 5 < B.size_ % 5; },
            [](const auto& E) { return E.size_ % 2 == 1; }, {}, {3, 1024, 2});
        QVERIFY(Sorted.HasValue()); QCOMPARE(Sorted.Value()->Count(), 51ULL);
        auto Page = Sorted.Value()->Read(0, 100); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().end_);
        for (std::size_t I = 1; I < Page.Value().entries_.size(); ++I) {
            const auto A = Page.Value().entries_[I - 1].size_, B = Page.Value().entries_[I].size_;
            QVERIFY(A % 5 < B % 5 || (A % 5 == B % 5 && A > B));
        }
        auto Selected = Sorted.Value()->Select({{1, 2}, {9, 3}}); QVERIFY(Selected.HasValue()); QCOMPARE(Selected.Value()->Count(), 5ULL);
        const auto ExpectedFirst = Page.Value().entries_[2].id_, ExpectedLast = Page.Value().entries_[11].id_;
        const auto Directory = Sorted.Value()->Directory(); Sorted.Value().reset(); QVERIFY(QDir(Directory).exists());
        auto SelectionPage = Selected.Value()->Read(1, 100); QVERIFY(SelectionPage.HasValue()); QVERIFY(SelectionPage.Value().end_);
        QCOMPARE(SelectionPage.Value().entries_.front().id_, ExpectedFirst); QCOMPARE(SelectionPage.Value().entries_.back().id_, ExpectedLast);
        Selected.Value().reset(); QVERIFY(!QDir(Directory).exists());
        QVERIFY(!Index.Value()->Select({{5, 4}, {7, 1}}).HasValue());
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Cancelled = Index.Value()->Project({}, {}, Context, {}); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!Index.Value()->Read(0, 1, Context).HasValue()); QCOMPARE(Index.Value()->Count(), 101ULL);
        Stop = std::stop_source{}; Context.stopToken_ = Stop.get_token(); int FilterCalls = 0;
        auto Interrupted = Index.Value()->Project({}, [&](const auto&) {
            if (++FilterCalls == 10) Stop.request_stop(); return true;
        }, Context, {3, 1024, 2});
        QVERIFY(!Interrupted.HasValue()); QCOMPARE(Interrupted.Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Index.Value()->Read(0, 1).Value().entries_.front().size_, 101ULL);
        GeneratedCursor Limited(100); Context = {}; Context.workload_.intent_ = WorkloadIntent::Automatic; Context.workload_.automaticEntries_ = 10;
        QVERIFY(!PagedEntryIndex::Build(Limited, Context).HasValue());
        GeneratedCursor Empty(0); auto EmptyIndex = PagedEntryIndex::Build(Empty, {}); QVERIFY(EmptyIndex.HasValue());
        QVERIFY(EmptyIndex.Value()->SelectAll().Value()->Read(0, 10).Value().end_);
    }
    void naturalComparator()
    {
        using namespace hc::columns;
        const auto Less = SortLess({Qt::AscendingOrder}, false, true);
        SortRow A{"a", false, false, {CellValue::Ready(QString("file2"))}};
        SortRow B{"b", false, false, {CellValue::Ready(QString("file10"))}};
        QVERIFY(Less(A, B)); QVERIFY(!Less(B, A));
        QCOMPARE(SortRanks({B, A}, {Qt::AscendingOrder}, false, true), QVector<int>({1, 0}));
    }
    void snapshotRevisionAndStreamingSelection()
    {
        OperationContext Context; Context.maxPageItems_ = 2;
        RevisionCursor Changed(true); auto Bad = PagedEntryIndex::Build(Changed, Context);
        QVERIFY(!Bad.HasValue()); QCOMPARE(Bad.Failure().category_, ErrorCategory::StaleRevision);
        RevisionCursor Stable(false); auto Index = PagedEntryIndex::Build(Stable, Context); QVERIFY(Index.HasValue());
        auto Sorted = Index.Value()->Project({}, {}, Context, {2, 1024, 2}); QVERIFY(Sorted.HasValue());
        QCOMPARE(Sorted.Value()->Read(0, 1).Value().revision_->token_, std::string("snapshot"));
        std::uint64_t Next = 0;
        auto Selected = Sorted.Value()->SelectRanges([&]() -> Result<std::optional<EntryRange>> {
            if (Next >= 10) return std::optional<EntryRange>{};
            const auto Row = Next; Next += 2; return std::optional<EntryRange>{{Row, 1}};
        });
        QVERIFY(Selected.HasValue()); QCOMPARE(Selected.Value()->Count(), 5ULL);
        auto Page = Selected.Value()->Read(2, 2); QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().entries_.front().size_, 6ULL); QCOMPARE(Page.Value().entries_.back().size_, 4ULL);
        QCOMPARE(Page.Value().revision_->token_, std::string("snapshot")); QVERIFY(!Page.Value().end_);
        QVERIFY(Selected.Value()->Read(4, 9).Value().end_);
        QTemporaryDir Temp; QVERIFY(!DiskRecords::Create(Temp.filePath("missing/child")).HasValue());
    }
    void pagedResults()
    {
        using namespace hc::operations;
        auto Store = PagedItemOutcomeStore::Create(); QVERIFY(Store.HasValue());
        ItemOutcome Item; Item.item_ = "file"; Item.destinationCommitted_ = true;
        Item.source_ = EntrySnapshot{}; Item.source_->id_ = {{"p"}, "id", IdentityScope::Persistent, ResourceRevision{"r"}};
        for (int I = 0; I < 100; ++I) QVERIFY(Store.Value()->Append(Item).HasValue());
        Item.sourceRemoved_ = true; Item.status_ = ItemOutcome::Status::Failed;
        Item.error_ = Error{ErrorCategory::AccessDenied, "denied", {"p"}, Item.source_->id_, true, "retry"};
        QVERIFY(Store.Value()->Replace(42, Item).HasValue()); QCOMPARE(Store.Value()->Count(), 100ULL);
        auto Page = Store.Value()->Read(41, 3); QVERIFY(Page.HasValue()); QVERIFY(!Page.Value().end_);
        QCOMPARE(Page.Value().items_.size(), 3u); QVERIFY(!Page.Value().items_[0].sourceRemoved_);
        QCOMPARE(Page.Value().items_[1].source_->id_, Item.source_->id_);
        QCOMPARE(Page.Value().items_[1].error_->resource_, Item.error_->resource_);
        QVERIFY(Page.Value().items_[1].sourceRemoved_); QCOMPARE(Page.Value().items_[1].error_->code_, std::string("denied"));
        QVERIFY(Store.Value()->Read(100, 3).Value().end_); QVERIFY(!Store.Value()->Replace(100, Item).HasValue());
    }
    void millionEntries()
    {
        const auto Before = PeakMemory();
        constexpr std::uint64_t Count = 1000000;
        GeneratedCursor Cursor(Count); OperationContext Context;
        auto Index = PagedEntryIndex::Build(Cursor, Context); QVERIFY(Index.HasValue()); QCOMPARE(Index.Value()->Count(), Count);
        auto Sorted = Index.Value()->Project([](const auto& A, const auto& B) { return A.size_ < B.size_; }, {}, Context, {});
        QVERIFY(Sorted.HasValue()); QCOMPARE(Sorted.Value()->Count(), Count);
        auto Selection = Sorted.Value()->SelectAll(); QVERIFY(Selection.HasValue());
        Index.Value().reset(); Sorted.Value().reset();
        std::uint64_t Seen = 0;
        while (Seen < Count) {
            auto Page = Selection.Value()->Read(Seen, 4096); QVERIFY(Page.HasValue()); QVERIFY(!Page.Value().entries_.empty());
            for (const auto& Entry : Page.Value().entries_) {
                ++Seen; QCOMPARE(Entry.size_, Seen); QCOMPARE(Entry.name_, "file" + std::to_string(Seen));
                QCOMPARE(Entry.id_.revision_->token_, std::string("revision"));
            }
            QCOMPARE(Page.Value().end_, Seen == Count);
        }
        const auto Growth = PeakMemory() - Before;
        qInfo() << "million entries verified; peak working set growth bytes:" << quint64(Growth);
        QVERIFY2(Growth < 256ULL * 1024 * 1024, "Entry count must not determine resident memory");
    }
};
QTEST_GUILESS_MAIN(PagedStorageTests)
#include "tstPagedStorage.moc"
