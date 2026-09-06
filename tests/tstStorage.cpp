#include "commonLibs/storage/storage.hpp"
#include "memoryStorage.hpp"
#include <QtTest>

using namespace hc::storage;
using namespace hc::storage::test;

class StorageTests : public QObject
{
    Q_OBJECT
private slots:
    void locatorRoundTrip()
    {
        const std::vector<Locator> Inputs{
            {LocatorKind::Local, "C:\\한글 폴더\\a:b%20.txt"},
            {LocatorKind::Smb, "\\\\DEVTESTER-QTS\\Public"},
            {LocatorKind::Shell, "shell:ComputerFolder"},
            {LocatorKind::Archive, "C:\\outer.zip", {{"wcx.zip", "inner.tar"}, {"wcx.tar", "a!b:c/한글.txt"}}}
        };
        for (const auto& Input : Inputs)
        {
            const auto Result = ParseLocator(Serialize(Input));
            QVERIFY(Result.HasValue());
            QVERIFY(Result.Value() == Input);
        }
        QVERIFY(!ParseLocator("hcl2").HasValue());
        QVERIFY(!ParseLocator("hcl11:01:xjunk").HasValue());
        QVERIFY(!ParseLocator("hcl11:099999999999999999999:x").HasValue());
        QVERIFY(!ParseLocator(Serialize({LocatorKind::Archive, "outer.zip"})).HasValue());
        QVERIFY(!ParseLocator(Serialize({LocatorKind::Local, "C:", {{"zip", "entry"}}})).HasValue());
        auto TooDeep = Inputs.back();
        TooDeep.nesting_.resize(33, {"zip", "entry"});
        QVERIFY(!ParseLocator(Serialize(TooDeep)).HasValue());
    }
    void identityRoundTrip()
    {
        ResourceId Id{{"smb"}, std::string("opaque\0key", 10), IdentityScope::NavigationSession, ResourceRevision{"r:2"}};
        auto Parsed = ParseResourceId(Serialize(Id));
        QVERIFY(Parsed.HasValue());
        QVERIFY(Parsed.Value() == Id);
        Id.revision_.reset();
        Id.scope_ = IdentityScope::Persistent;
        QVERIFY(ParseResourceId(Serialize(Id)).Value() == Id);
        QVERIFY(!ParseResourceId(Serialize(Id) + "x").HasValue());
        Id.providerId_.value_.clear();
        QVERIFY(!ParseResourceId(Serialize(Id)).HasValue());
    }
    void boundedPagingAndSnapshot()
    {
        MemoryProvider Provider;
        for (int Index = 0; Index < 5; ++Index)
            Provider.Add(Provider.Root(), std::to_string(Index));
        auto Cursor = Provider.Enumerate(Provider.Root(), {}).get();
        QVERIFY(Cursor.HasValue());
        Provider.Add(Provider.Root(), "after-snapshot");
        OperationContext Context;
        Context.maxPageItems_ = 2;
        auto First = Cursor.Value()->Next(1000, Context).get();
        QCOMPARE(First.Value().entries_.size(), 2u);
        QVERIFY(!First.Value().end_);
        QCOMPARE(Cursor.Value()->Next(1000, Context).get().Value().entries_.size(), 2u);
        auto Last = Cursor.Value()->Next(1000, Context).get();
        QCOMPARE(Last.Value().entries_.size(), 1u);
        QVERIFY(Last.Value().end_);
        auto End = Cursor.Value()->Next(10, Context).get();
        QVERIFY(End.Value().entries_.empty() && End.Value().end_);
        QVERIFY(!Cursor.Value()->Next(0, Context).get().HasValue());
        auto Missing = Provider.Root();
        Missing.root_ += "/absent";
        QCOMPARE(Provider.Enumerate(Missing, {}).get().Failure().category_, ErrorCategory::NotFound);
    }
    void independentReadAndRevision()
    {
        MemoryProvider Provider;
        const std::vector<std::byte> Bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        auto Id = Provider.Add(Provider.Root(), "sample", Bytes);
        auto A = Provider.OpenRead(Id, {}).get();
        auto B = Provider.OpenRead(Id, {}).get();
        QVERIFY(A.HasValue() && B.HasValue());
        OperationContext Context;
        Context.maxReadBytes_ = 1;
        auto Chunk = A.Value()->Read(100, Context).get();
        QCOMPARE(Chunk.Value().bytes_.size(), 1u);
        QVERIFY(!Chunk.Value().eof_);
        QCOMPARE(A.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{2});
        QCOMPARE(B.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{1});
        QVERIFY(A.Value()->Seek(3, {}).get().HasValue());
        Chunk = A.Value()->Read(1, {}).get();
        QVERIFY(Chunk.Value().eof_ && Chunk.Value().bytes_.empty());
        QVERIFY(!A.Value()->Seek(4, {}).get().HasValue());
        const auto Updated = Provider.Replace(Id, {std::byte{9}});
        QCOMPARE(Provider.Stat(Id, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        QCOMPARE(Provider.OpenRead(Id, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        QCOMPARE(Provider.Stat(Updated, {}).get().Value().size_, 1u);
        QVERIFY(A.Value()->Seek(0, {}).get().HasValue());
        QCOMPARE(A.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{1});
    }
    void cancellationAndConnectionLoss()
    {
        MemoryProvider Provider;
        const auto Id = Provider.Add(Provider.Root(), "sample", {std::byte{7}});
        auto Cursor = Provider.Enumerate(Provider.Root(), {}).get();
        auto Stream = Provider.OpenRead(Id, {}).get();
        std::stop_source Stop;
        OperationContext Context;
        Context.stopToken_ = Stop.get_token();
        Stop.request_stop();
        QCOMPARE(Provider.Enumerate(Provider.Root(), Context).get().Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Cursor.Value()->Next(1, Context).get().Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Stream.Value()->Read(1, Context).get().Failure().category_, ErrorCategory::Cancelled);
        Provider.SetFailure(Error{ErrorCategory::Unavailable, "connection-lost", Provider.Id(), Id, true, "reconnect"});
        auto Failed = Cursor.Value()->Next(1, {}).get();
        QCOMPARE(Failed.Failure().category_, ErrorCategory::Unavailable);
        QVERIFY(Failed.Failure().retryable_);
        QCOMPARE(Stream.Value()->Read(1, {}).get().Failure().code_, std::string("connection-lost"));
        Provider.SetFailure({});
        QCOMPARE(Cursor.Value()->Next(1, {}).get().Value().entries_.size(), 1u);
        QCOMPARE(Stream.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{7});
        Context = {};
        Context.deadline_ = std::chrono::steady_clock::now();
        QCOMPARE(Provider.Stat(Id, Context).get().Failure().code_, std::string("deadline-exceeded"));
    }
    void capabilitiesAndRegistry()
    {
        auto Provider = std::make_shared<MemoryProvider>();
        const auto Id = Provider->Add(Provider->Root(), "sample");
        const auto Write = Provider->Supports(Operation::Write, {}).get();
        QCOMPARE(Write.Value().availability_, Availability::Unsupported);
        QVERIFY(!Write.Value().reason_.empty());
        Provider->SetReadSupport({Availability::Conditional, "password-required", true, true, {"password-grant"}});
        const auto Read = Provider->Supports(Operation::OpenRead, {}).get();
        QCOMPARE(Read.Value().availability_, Availability::Conditional);
        QVERIFY(Read.Value().requiresUserInteraction_ && !Read.Value().constraints_.empty());
        QVERIFY(!Provider->OpenRead(Id, {}).get().HasValue());
        ProviderRegistry Registry;
        QVERIFY(Registry.Register(Provider).HasValue());
        QVERIFY(!Registry.Register(Provider).HasValue());
        QVERIFY(Registry.Resolve(Provider->Id()).HasValue());
        QCOMPARE(Registry.Resolve({"absent"}).Failure().category_, ErrorCategory::NotFound);
    }
};

QTEST_GUILESS_MAIN(StorageTests)
#include "tstStorage.moc"
