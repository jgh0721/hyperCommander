#include "commonLibs/storage/storage.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include "memoryStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/mappedDriveRecovery.hpp"
#include <QtTest>
#include <Windows.h>
#include <AclAPI.h>

using namespace hc::storage;
using namespace hc::storage::test;

class StorageTests : public QObject
{
    Q_OBJECT
private slots:
    void mappedDriveListing()
    {
        const auto Path = qEnvironmentVariable("HC_TEST_MAPPED_DRIVE");
        if (Path.isEmpty()) QSKIP("Set HC_TEST_MAPPED_DRIVE to an existing mapped drive path; mappings are never disconnected by the test.");
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        for (int Attempt = 0; Attempt < 3; ++Attempt) {
            auto Cursor = Provider->Enumerate({LocatorKind::Local, Path.toStdString()}, {}).get();
            QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
            for (;;) {
                auto Page = Cursor.Value()->Next(128, {}).get();
                QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
                if (Page.Value().end_) break;
            }
        }
    }
    void healthyDriveNeedsNoRecovery()
    {
        int Queries = 0, Restores = 0;
        native::MappedDriveRecovery Recovery(L"Z:\\", [&](const auto&) { ++Queries; return true; }, [&](const auto&) { ++Restores; });
        const auto Value = Recovery.Invoke([] { return DWORD(FILE_ATTRIBUTE_DIRECTORY); },
            [](DWORD Value) { return Value == INVALID_FILE_ATTRIBUTES; }, {});
        QCOMPARE(Value, DWORD(FILE_ATTRIBUTE_DIRECTORY)); QCOMPARE(Queries, 0); QCOMPARE(Restores, 0);
    }
    void mappedDriveReconnect_data()
    {
        QTest::addColumn<QString>("Path");
        QTest::addColumn<quint32>("Code");
        QTest::addColumn<bool>("Mapped");
        QTest::addColumn<bool>("Retry");
        QTest::newRow("unexpected-network-error") << "Z:\\folder" << quint32(ERROR_UNEXP_NET_ERR) << true << true;
        QTest::newRow("disconnected") << "Z:\\" << quint32(ERROR_CONNECTION_UNAVAIL) << true << true;
        QTest::newRow("missing-mapped-root") << "Z:/" << quint32(ERROR_PATH_NOT_FOUND) << true << true;
        QTest::newRow("extended-path") << "\\\\?\\Z:\\folder" << quint32(ERROR_NETNAME_DELETED) << true << true;
        QTest::newRow("local-drive") << "C:\\missing" << quint32(ERROR_PATH_NOT_FOUND) << false << false;
        QTest::newRow("access-denied") << "Z:\\private" << quint32(ERROR_ACCESS_DENIED) << true << false;
        QTest::newRow("credentials") << "Z:\\" << quint32(ERROR_LOGON_FAILURE) << true << false;
        QTest::newRow("unc") << "\\\\server\\share" << quint32(ERROR_UNEXP_NET_ERR) << true << false;
        QTest::newRow("relative") << "Z:folder" << quint32(ERROR_UNEXP_NET_ERR) << true << false;
    }
    void mappedDriveReconnect()
    {
        QFETCH(QString, Path); QFETCH(quint32, Code); QFETCH(bool, Mapped); QFETCH(bool, Retry);
        int Probes = 0, Restores = 0;
        native::MappedDriveRecovery Recovery(Path.toStdWString(),
            [&](const std::wstring& Device) { return Mapped; },
            [&](const std::wstring& Root) { QCOMPARE(Root, std::wstring(L"Z:\\")); ++Restores; });
        const auto Work = [&]() -> DWORD {
            ++Probes;
            if (Restores) return FILE_ATTRIBUTE_DIRECTORY;
            SetLastError(Code); return INVALID_FILE_ATTRIBUTES;
        };
        const auto Failed = [](DWORD Value) { return Value == INVALID_FILE_ATTRIBUTES; };
        const auto Value = Recovery.Invoke(Work, Failed, {});
        QCOMPARE(Value, Retry ? DWORD(FILE_ATTRIBUTE_DIRECTORY) : INVALID_FILE_ATTRIBUTES);
        QCOMPARE(Probes, Retry ? 2 : 1); QCOMPARE(Restores, Retry ? 1 : 0);
        if (!Retry) QCOMPARE(GetLastError(), DWORD(Code));
        // A subsequent failure in FindFirstFile shares the same recovery budget.
        Recovery.Invoke([&] { SetLastError(Code); return INVALID_FILE_ATTRIBUTES; }, Failed, {});
        QCOMPARE(Restores, Retry ? 1 : 0);
    }
    void mappedDriveRecoveryFailureAndCancellation()
    {
        const auto Failed = [](DWORD Value) { return Value == INVALID_FILE_ATTRIBUTES; };
        int Probes = 0, Restores = 0;
        std::stop_source Stop;
        OperationContext Context; Context.stopToken_ = Stop.get_token();
        native::MappedDriveRecovery Recovery(L"Z:\\", [](const auto&) { return true; },
            [&](const auto&) { ++Restores; });
        auto Value = Recovery.Invoke([&] { SetLastError(++Probes == 1 ? ERROR_UNEXP_NET_ERR : ERROR_BAD_NETPATH); return INVALID_FILE_ATTRIBUTES; }, Failed, Context);
        QCOMPARE(Value, INVALID_FILE_ATTRIBUTES); QCOMPARE(GetLastError(), DWORD(ERROR_BAD_NETPATH));
        QCOMPARE(Probes, 2); QCOMPARE(Restores, 1);
        Stop.request_stop();
        native::MappedDriveRecovery Cancelled(L"Z:\\", [](const auto&) { return true; }, [&](const auto&) { ++Restores; });
        Cancelled.Invoke([&] { ++Probes; SetLastError(ERROR_UNEXP_NET_ERR); return INVALID_FILE_ATTRIBUTES; }, Failed, Context);
        QCOMPARE(Probes, 3); QCOMPARE(Restores, 1);
        std::stop_source During;
        Context.stopToken_ = During.get_token();
        native::MappedDriveRecovery Interrupted(L"Z:\\", [](const auto&) { return true; }, [&](const auto&) { ++Restores; During.request_stop(); });
        Interrupted.Invoke([&] { ++Probes; SetLastError(ERROR_UNEXP_NET_ERR); return INVALID_FILE_ATTRIBUTES; }, Failed, Context);
        QCOMPARE(Probes, 4); QCOMPARE(Restores, 2);
    }
    void redirectedClientShares()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE to an accessible drive redirected by the current RDP session.");
        auto Provider = MakeNativeReadProvider(LocatorKind::Smb);
        for (const auto& Server : {"\\\\tsclient", "\\\\TSCLIENT"}) {
            auto Cursor = Provider->Enumerate({LocatorKind::Smb, Server}, {}).get();
            QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
            bool Found = false;
            for (;;) {
                auto Page = Cursor.Value()->Next(1, {}).get(); QVERIFY(Page.HasValue());
                for (const auto& Entry : Page.Value().entries_) {
                    QVERIFY(Entry.directory_); QVERIFY(!Entry.id_.revision_);
                    QCOMPARE(Entry.locator_.root_, std::string(Server) + "\\" + Entry.name_);
                    QVERIFY(Entry.name_.find('\\') == std::string::npos);
                    Found |= QString::fromStdString(Entry.name_).compare(Share, Qt::CaseInsensitive) == 0;
                }
                if (Page.Value().end_) break;
            }
            QVERIFY(Found);
        }
        OperationContext Limited; Limited.maxEnumerationEntries_ = 0;
        auto LimitedResult = Provider->Enumerate({LocatorKind::Smb, "\\\\tsclient"}, Limited).get();
        QVERIFY(!LimitedResult.HasValue()); QCOMPARE(LimitedResult.Failure().category_, ErrorCategory::BudgetExceeded);
        std::stop_source Stop; Stop.request_stop();
        OperationContext Cancelled; Cancelled.stopToken_ = Stop.get_token();
        auto CancelledResult = Provider->Enumerate({LocatorKind::Smb, "\\\\tsclient"}, Cancelled).get();
        QVERIFY(!CancelledResult.HasValue()); QCOMPARE(CancelledResult.Failure().category_, ErrorCategory::Cancelled);
    }
    void protectedChildDoesNotHideParentListing_data()
    {
        QTest::addColumn<bool>("DirectoryChild");
        QTest::newRow("directory") << true;
        QTest::newRow("file") << false;
    }
    void protectedChildDoesNotHideParentListing()
    {
        QFETCH(bool, DirectoryChild);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Child = Directory.filePath("protected-child");
        if (DirectoryChild) { QVERIFY(QDir().mkdir(Child)); }
        else { QFile File(Child); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("protected"), 9); }
        const auto Handle = CreateFileW(QDir::toNativeSeparators(Child).toStdWString().c_str(), READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
        PACL OriginalAcl = nullptr;
        PSECURITY_DESCRIPTOR OriginalDescriptor = nullptr;
        QCOMPARE(GetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &OriginalAcl, nullptr, &OriginalDescriptor), DWORD(ERROR_SUCCESS));
        const auto Free = qScopeGuard([&] { LocalFree(OriginalDescriptor); });
        ACL EmptyAcl{};
        QVERIFY(InitializeAcl(&EmptyAcl, sizeof(EmptyAcl), ACL_REVISION));
        // An empty DACL on this owned fixture deterministically denies metadata
        // access. The already-open handle restores access even after an assertion.
        const auto Restore = qScopeGuard([&] {
            SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, OriginalAcl, nullptr);
        });
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &EmptyAcl, nullptr), DWORD(ERROR_SUCCESS));
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        const Locator Root{LocatorKind::Local, QDir::toNativeSeparators(Directory.path()).toStdString()};
        auto Cursor = Provider->Enumerate(Root, {}).get(); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(16, {}).get(); QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().entries_.size(), 1u);
        const auto& ChildEntry = Page.Value().entries_.front();
        QCOMPARE(ChildEntry.name_, std::string("protected-child")); QCOMPARE(ChildEntry.directory_, DirectoryChild);
        if (DirectoryChild) QVERIFY(!ChildEntry.id_.revision_);
        QVERIFY(!Provider->Stat(ChildEntry.id_, {}).get().HasValue());
        OperationContext Fresh; Fresh.freshEnumerationMetadata_ = true;
        auto Strict = Provider->Enumerate(Root, Fresh).get(); QVERIFY(!Strict.HasValue());
        QCOMPARE(Strict.Failure().code_, std::string("win32:5"));
        Fresh.allowUnavailableChildMetadata_ = true;
        auto Browse = Provider->Enumerate(Root, Fresh).get(); QVERIFY(Browse.HasValue());
        auto BrowsePage = Browse.Value()->Next(16, Fresh).get(); QVERIFY(BrowsePage.HasValue());
        QCOMPARE(BrowsePage.Value().entries_.size(), 1u);
        QCOMPARE(BrowsePage.Value().entries_.front().directory_, DirectoryChild);
        QVERIFY(!BrowsePage.Value().entries_.front().id_.revision_);
        QCOMPARE(Provider->Stat(BrowsePage.Value().entries_.front().id_, Fresh).get().Failure().category_, ErrorCategory::AccessDenied);
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, OriginalAcl, nullptr), DWORD(ERROR_SUCCESS));
    }
    void nativeReadProvider()
    {
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath(QString::fromUtf8("한글 sample.txt"));
        QFile File(Path);
        QVERIFY(File.open(QIODevice::WriteOnly));
        QCOMPARE(File.write("abcdef"), 6);
        File.close();
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        const Locator At{LocatorKind::Local, QDir::toNativeSeparators(Directory.path()).toStdString()};
        auto Cursor = Provider->Enumerate(At, {}).get();
        QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(1, {}).get();
        QCOMPARE(Page.Value().entries_.size(), 1u);
        const auto Id = Page.Value().entries_[0].id_;
        ProviderContentSource Content(Provider, Page.Value().entries_[0]);
        QVERIFY(Content.Verify({}).get().HasValue());
        QCOMPARE(Provider->Stat(Id, {}).get().Value().size_, 6u);
        auto A = Provider->OpenRead(Id, {}).get();
        auto B = Provider->OpenRead(Id, {}).get();
        QVERIFY(A.HasValue() && B.HasValue());
        QCOMPARE(A.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{'a'});
        QCOMPARE(A.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{'b'});
        QCOMPARE(B.Value()->Read(1, {}).get().Value().bytes_[0], std::byte{'a'});
        QVERIFY(A.Value()->Seek(6, {}).get().HasValue());
        QVERIFY(A.Value()->Read(1, {}).get().Value().eof_);
        QVERIFY(!MakeNativeReadProvider(LocatorKind::Local)->Stat(Id, {}).get().HasValue());
        QVERIFY(File.open(QIODevice::WriteOnly));
        File.write("changed length");
        File.close();
        QCOMPARE(Provider->Stat(Id, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        QCOMPARE(Provider->OpenRead(Id, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        QCOMPARE(Content.Verify({}).get().Failure().category_, ErrorCategory::StaleRevision);
        auto Missing = At;
        Missing.root_ += "\\missing";
        QCOMPARE(Provider->Enumerate(Missing, {}).get().Failure().category_, ErrorCategory::NotFound);
        OperationContext Context;
        Context.maxEnumerationEntries_ = 0;
        QCOMPARE(Provider->Enumerate(At, Context).get().Failure().category_, ErrorCategory::BudgetExceeded);
    }
    void nativeCreationTimeIsNotContentRevision_data()
    {
        QTest::addColumn<bool>("Fresh");
        QTest::newRow("cached-listing") << false;
        QTest::newRow("handle-listing") << true;
    }
    void nativeCreationTimeIsNotContentRevision()
    {
        QFETCH(bool, Fresh);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("creation-time.txt");
        QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly));
        QCOMPARE(File.write("abcdef"), 6); File.close();
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        OperationContext Context; Context.freshEnumerationMetadata_ = Fresh;
        const Locator Root{LocatorKind::Local, QDir::toNativeSeparators(Directory.path()).toStdString()};
        auto Cursor = Provider->Enumerate(Root, Context).get(); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(16, Context).get(); QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().entries_.size(), 1u);
        const auto Id = Page.Value().entries_.front().id_;
        auto Before = Provider->Stat(Id, {}).get(); QVERIFY(Before.HasValue());
        const auto Handle = CreateFileW(QDir::toNativeSeparators(Path).toStdWString().c_str(),
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
        BY_HANDLE_FILE_INFORMATION Info{}; QVERIFY(GetFileInformationByHandle(Handle, &Info));
        const auto Ticks = ((std::uint64_t(Info.ftCreationTime.dwHighDateTime) << 32) | Info.ftCreationTime.dwLowDateTime) + 10000000;
        const FILETIME Changed{DWORD(Ticks), DWORD(Ticks >> 32)};
        // Reproduce the SMB discrepancy without changing contents, mtime or ID.
        QVERIFY(SetFileTime(Handle, &Changed, nullptr, nullptr));
        auto After = Provider->Stat(Id, {}).get(); QVERIFY(After.HasValue());
        QCOMPARE(After.Value().id_.revision_, Before.Value().id_.revision_);
        QVERIFY(After.Value().createdUtcMs_ != Before.Value().createdUtcMs_);
        auto Stream = Provider->OpenRead(Id, {}).get(); QVERIFY(Stream.HasValue());
        auto Bytes = Stream.Value()->Read(16, {}).get(); QVERIFY(Bytes.HasValue());
        QCOMPARE(Bytes.Value().bytes_.size(), 6u); QCOMPARE(Bytes.Value().bytes_[0], std::byte{'a'});
        const auto WriteTicks = (std::uint64_t(Info.ftLastWriteTime.dwHighDateTime) << 32 | Info.ftLastWriteTime.dwLowDateTime) + 10000000;
        const FILETIME ChangedWrite{DWORD(WriteTicks), DWORD(WriteTicks >> 32)};
        QVERIFY(SetFileTime(Handle, nullptr, nullptr, &ChangedWrite));
        auto Stale = Provider->Stat(Id, {}).get(); QVERIFY(!Stale.HasValue());
        QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
        auto StaleRead = Provider->OpenRead(Id, {}).get(); QVERIFY(!StaleRead.HasValue());
        QCOMPARE(StaleRead.Failure().category_, ErrorCategory::StaleRevision);
    }
    void nativeHandleRevisionRejectsSameMetadataReplacement()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("replace.txt");
        QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("abcdef"), 6); File.close();
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        const Locator At{LocatorKind::Local, QDir::toNativeSeparators(Path).toStdString()};
        auto Original = Provider->Resolve(At, {}).get(); QVERIFY(Original.HasValue());
        const auto Handle = CreateFileW(QDir::toNativeSeparators(Path).toStdWString().c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Close = qScopeGuard([&] { CloseHandle(Handle); });
        BY_HANDLE_FILE_INFORMATION Info{}; QVERIFY(GetFileInformationByHandle(Handle, &Info));
        // Keep the old object alive to prevent ID reuse. The replacement copies
        // every timestamp and size, so only object identity exposes replacement.
        QVERIFY(QFile::rename(Path, Path + ".original"));
        QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("uvwxyz"), 6); File.close();
        const auto Replacement = CreateFileW(QDir::toNativeSeparators(Path).toStdWString().c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(Replacement != INVALID_HANDLE_VALUE);
        const auto CloseReplacement = qScopeGuard([&] { CloseHandle(Replacement); });
        QVERIFY(SetFileTime(Replacement, &Info.ftCreationTime, &Info.ftLastAccessTime, &Info.ftLastWriteTime));
        auto Current = Provider->Resolve(At, {}).get(); QVERIFY(Current.HasValue());
        QCOMPARE(Current.Value().size_, Original.Value().size_);
        QCOMPARE(Current.Value().createdUtcMs_, Original.Value().createdUtcMs_);
        QCOMPARE(Current.Value().modifiedUtcMs_, Original.Value().modifiedUtcMs_);
        auto Stale = Provider->Stat(Original.Value().id_, {}).get(); QVERIFY(!Stale.HasValue());
        QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
        auto Read = Provider->OpenRead(Original.Value().id_, {}).get(); QVERIFY(!Read.HasValue());
        QCOMPARE(Read.Failure().category_, ErrorCategory::StaleRevision);
    }
    void shellAndArchiveProviders()
    {
        const Locator Computer{LocatorKind::Shell, "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"};
        auto Shell = MakeShellReadProvider(Computer.root_);
        auto ShellCursor = Shell->Enumerate(Computer, {}).get();
        QVERIFY(ShellCursor.HasValue());
        QVERIFY(ShellCursor.Value()->Next(256, {}).get().HasValue());
        const Locator Root{LocatorKind::Archive, "fixture.zip", {{"wcx", "/"}}};
        auto Child = Root;
        Child.nesting_.back().entry_ = "/한글.txt";
        EntrySnapshot Item;
        Item.locator_ = Child;
        Item.name_ = "한글.txt";
        Item.size_ = 42;
        auto Archive = MakeArchiveReadProvider(Root, {{Root, {Item}}});
        auto Cursor = Archive->Enumerate(Root, {}).get();
        QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(2, {}).get();
        QCOMPARE(Page.Value().entries_.size(), 1u);
        QCOMPARE(Archive->Stat(Page.Value().entries_[0].id_, {}).get().Value().size_, 42u);
        const auto Support = Archive->Supports(Operation::OpenRead, {}).get().Value();
        QCOMPARE(Support.availability_, Availability::Conditional);
        QVERIFY(Support.requiresMaterialization_);
        auto Bad = Root;
        Bad.nesting_.back().entry_ = "/missing";
        QCOMPARE(Archive->Enumerate(Bad, {}).get().Failure().category_, ErrorCategory::NotFound);
    }
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
