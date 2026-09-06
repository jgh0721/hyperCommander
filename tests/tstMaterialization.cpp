#include <QtTest>
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "memoryStorage.hpp"
#include <cstring>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>

using namespace hc::storage;

namespace
{
    std::shared_ptr<IMaterializer> Manager(const QString& Root, MaterializationLimits Limits = {})
    {
        auto Made = MakeNativeMaterializer(Root.toStdString(), Limits);
        return Made.HasValue() ? std::move(Made.Value()) : nullptr;
    }
    std::shared_ptr<IContentSource> Content(const QByteArray& Bytes, const std::string& Name = "sample.txt")
    {
        auto Provider = std::make_shared<test::MemoryProvider>();
        std::vector<std::byte> Data(Bytes.size()); std::memcpy(Data.data(), Bytes.data(), Bytes.size());
        const auto Id = Provider->Add(Provider->Root(), Name, std::move(Data));
        return std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
    }
    QStringList LeaseDirectories(const QString& Root)
    { return QDir(Root + "/hc-materialized-v1").entryList({"hc-lease-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot); }
    class FailingStream final : public IReadStream
    {
    public:
        std::function<void()> onRead_;
        bool emptyProgress_ = false, oversized_ = false;
        bool CanSeek() const override { return false; }
        Task<Unit> Seek(std::uint64_t, OperationContext) override { return Ready<Unit>(Error{ErrorCategory::Unsupported, "no-seek"}); }
        Task<ReadChunk> Read(std::size_t, OperationContext) override
        {
            if (onRead_) onRead_();
            if (oversized_) return Ready<ReadChunk>(ReadChunk{std::vector<std::byte>(64), true});
            if (emptyProgress_) return Ready<ReadChunk>(ReadChunk{{}, false});
            if (count_++) return Ready<ReadChunk>(Error{ErrorCategory::Unavailable, "injected-read-failure"});
            return Ready<ReadChunk>(ReadChunk{std::vector<std::byte>(4), false});
        }
    private:
        int count_ = 0;
    };
    class FaultContent final : public IContentSource
    {
    public:
        std::shared_ptr<FailingStream> stream_ = std::make_shared<FailingStream>();
        EntrySnapshot entry_ = Content(QByteArray(8, 'x'))->Describe();
        EntrySnapshot Describe() const override { return entry_; }
        Task<Unit> Verify(OperationContext) override { return Ready<Unit>(Unit{}); }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext) override { return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(stream_)); }
    };
}

class MaterializationTests : public QObject
{
    Q_OBJECT
private slots:
    void producersShareRootQuotaAndValidateOutput()
    {
        QTemporaryDir Directory;
        MaterializationLimits Limits; Limits.maxTotalBytes_ = 5;
        auto ManagerResult = MakeNativeMaterializer(Directory.path().toStdString(), Limits); QVERIFY(ManagerResult.HasValue());
        auto ProducerResult = MakeNativeContentStore(Directory.path().toStdString(), Limits); QVERIFY(ProducerResult.HasValue());
        auto Manager = std::move(ManagerResult.Value()); auto Store = std::move(ProducerResult.Value());
        QVERIFY(!MakeNativeContentStore(Directory.path().toStdString()).HasValue());
        const auto Write = [](const std::string& Path, OperationContext) -> Result<Unit> {
            QFile File(QString::fromStdString(Path));
            if (!File.open(QIODevice::WriteOnly) || File.write("hello") != 5) return Error{ErrorCategory::Unavailable, "write-failed"};
            return Unit{};
        };
        auto Produced = Store->Produce(Content("hello")->Describe(), {}, {}, {}, Write); QVERIFY(Produced.HasValue());
        const auto Full = Manager->Materialize({Content("x")}, {}).get(); QVERIFY(!Full.HasValue()); QCOMPARE(Full.Failure().category_, ErrorCategory::BudgetExceeded);
        Produced.Value().reset(); QVERIFY(Manager->DrainCleanup().errors_.empty());
        const auto WrongSize = Store->Produce(Content("1234")->Describe(), {}, {}, {}, Write); QVERIFY(!WrongSize.HasValue()); QCOMPARE(WrongSize.Failure().code_, "invalid-produced-content");
        const auto Failed = Store->Produce(Content("hello")->Describe(), {}, {}, {}, [](const std::string&, OperationContext) -> Result<Unit> {
            throw std::runtime_error("producer failure");
        }); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().code_, "content-producer-exception");
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        const auto Cancelled = Store->Produce(Content("hello")->Describe(), {}, {}, Context, [&](const std::string& Path, OperationContext Context) {
            auto Written = Write(Path, Context); Stop.request_stop(); return Written;
        }); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(Manager->DrainCleanup().errors_.empty()); QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
        QVERIFY(Manager->Materialize({Content("hello")}, {}).get().HasValue());
    }
    void sharedLifetimeAndReadOnlyContent()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto Service = Manager(Directory.path()); QVERIFY(Service);
        auto Result = Service->Materialize({Content("hello", "한글.txt")}, {}).get(); QVERIFY(Result.HasValue());
        auto Lease = std::move(Result.Value());
        const auto Path = QString::fromStdString(Lease->LocalPath());
        QVERIFY(Path.endsWith("content.txt"));
        QFile Read(Path); QVERIFY(Read.open(QIODevice::ReadOnly)); QCOMPARE(Read.readAll(), QByteArray("hello")); Read.close();
        QFile Write(Path); QVERIFY(!Write.open(QIODevice::WriteOnly));
        auto Shared = Lease; Lease.reset();
        QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(QFile::exists(Path));
        Shared.reset(); QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(!QFile::exists(Path));
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void nestedParentLifetimeAndDepth()
    {
        QTemporaryDir Directory;
        MaterializationLimits Limits; Limits.maxDepth_ = 1;
        auto Service = Manager(Directory.path(), Limits); QVERIFY(Service);
        auto ParentResult = Service->Materialize({Content("parent")}, {}).get(); QVERIFY(ParentResult.HasValue());
        auto Parent = std::move(ParentResult.Value());
        const auto ParentPath = QString::fromStdString(Parent->LocalPath());
        auto ChildResult = Service->Materialize({Content("child"), {Parent}}, {}).get(); QVERIFY(ChildResult.HasValue());
        auto Child = std::move(ChildResult.Value()); QCOMPARE(Child->Depth(), 1u);
        Parent.reset(); QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(QFile::exists(ParentPath));
        const auto TooDeep = Service->Materialize({Content("too-deep"), {Child}}, {}).get();
        QVERIFY(!TooDeep.HasValue()); QCOMPARE(TooDeep.Failure().category_, ErrorCategory::BudgetExceeded);
        Child.reset(); QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(!QFile::exists(ParentPath));
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void sizeCountQuotaAndReuse()
    {
        QTemporaryDir Directory;
        MaterializationLimits Limits; Limits.maxFileBytes_ = 8; Limits.maxTotalBytes_ = 10; Limits.maxActiveFiles_ = 2;
        auto Service = Manager(Directory.path(), Limits); QVERIFY(Service);
        const auto Oversized = Service->Materialize({Content("123456789")}, {}).get(); QVERIFY(!Oversized.HasValue());
        auto First = Service->Materialize({Content("12345678")}, {}).get(); QVERIFY(First.HasValue());
        const auto Full = Service->Materialize({Content("123")}, {}).get(); QVERIFY(!Full.HasValue());
        auto Empty = Service->Materialize({Content({})}, {}).get(); QVERIFY(Empty.HasValue());
        const auto TooMany = Service->Materialize({Content({})}, {}).get(); QVERIFY(!TooMany.HasValue());
        First.Value().reset(); Empty.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
        auto Again = Service->Materialize({Content("12345678")}, {}).get(); QVERIFY(Again.HasValue());
        Again.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void expansionAndTraversalMetadata()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        auto Source = std::make_shared<FaultContent>(); Source->entry_.locator_.kind_ = LocatorKind::Archive;
        auto Missing = Service->Materialize({Source}, {}).get(); QVERIFY(!Missing.HasValue());
        QCOMPARE(Missing.Failure().code_, std::string("compressed-size-required"));
        Source->entry_.size_ = 1001;
        auto Expanded = Service->Materialize({Source, {}, 1}, {}).get(); QVERIFY(!Expanded.HasValue());
        QCOMPARE(Expanded.Failure().code_, std::string("materialization-expansion-budget"));
        auto Traversal = Service->Materialize({Content("safe", "../../outside.txt")}, {}).get(); QVERIFY(Traversal.HasValue());
        QVERIFY(QString::fromStdString(Traversal.Value()->LocalPath()).startsWith(Directory.path() + "/hc-materialized-v1/"));
        QVERIFY(!QFile::exists(Directory.path() + "/outside.txt"));
        Traversal.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void cancellationAndFailedStreamsCleanBeforeCompletion()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        for (int Mode = 0; Mode < 4; ++Mode)
        {
            auto Source = std::make_shared<FaultContent>();
            std::stop_source Stop;
            OperationContext Context; Context.stopToken_ = Stop.get_token(); Context.maxReadBytes_ = 8;
            if (Mode == 1) Source->stream_->onRead_ = [&Stop] { Stop.request_stop(); };
            if (Mode == 2) Source->stream_->emptyProgress_ = true;
            if (Mode == 3) Source->stream_->oversized_ = true;
            const auto Result = Service->Materialize({Source}, Context).get();
            QVERIFY(!Result.HasValue());
            if (Mode == 1) QCOMPARE(Result.Failure().category_, ErrorCategory::Cancelled);
            QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
        }
    }
    void staleNativeSourceAndIndependentLocalCopy()
    {
        QTemporaryDir Directory;
        QFile File(Directory.filePath("native.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("native"); File.close();
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Provider->Resolve({LocatorKind::Local, QDir::toNativeSeparators(File.fileName()).toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Source = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
        auto Service = Manager(Directory.path()); QVERIFY(Service);
        auto Made = Service->Materialize({Source}, {}).get(); QVERIFY(Made.HasValue());
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("changed-size"); File.close();
        QFile Copy(QString::fromStdString(Made.Value()->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), QByteArray("native")); Copy.close();
        auto Stale = Service->Materialize({Source}, {}).get(); QVERIFY(!Stale.HasValue());
        QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
        Made.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void cancellationAndDeadlineBeforeWork()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        std::stop_source Stop; Stop.request_stop();
        OperationContext Cancel; Cancel.stopToken_ = Stop.get_token();
        const auto Cancelled = Service->Materialize({Content("never-written")}, Cancel).get();
        QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        OperationContext Expired; Expired.deadline_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto TimedOut = Service->Materialize({Content("never-written")}, Expired).get();
        QVERIFY(!TimedOut.HasValue()); QCOMPARE(TimedOut.Failure().code_, std::string("deadline-exceeded"));
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void discardedFutureDoesNotWaitForProvider()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        auto Source = std::make_shared<FaultContent>();
        std::promise<void> Gate; const auto Released = Gate.get_future().share();
        std::atomic<bool> Entered = false;
        auto Release = qScopeGuard([&Gate] { Gate.set_value(); });
        Source->stream_->onRead_ = [&] { Entered = true; Released.wait(); };
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Pending = Service->Materialize({Source}, Context);
        QTRY_VERIFY(Entered.load());
        QElapsedTimer Timer; Timer.start(); Pending = {}; QVERIFY(Timer.elapsed() < 500);
        Stop.request_stop(); Gate.set_value(); Release.dismiss();
        QTRY_VERIFY(LeaseDirectories(Directory.path()).isEmpty());
        QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void recoverySkipsLiveLeaseAndRejectsForgedManifest()
    {
        QTemporaryDir Directory;
        auto Service = Manager(Directory.path()); QVERIFY(Service);
        auto Made = Service->Materialize({Content("live")}, {}).get(); QVERIFY(Made.HasValue());
        auto Other = Manager(Directory.path()); QVERIFY(Other);
        const auto Recovery = Other->Recover(); QCOMPARE(Recovery.busy_, 1u); QVERIFY(Recovery.errors_.empty());
        QVERIFY(QFile::exists(QString::fromStdString(Made.Value()->LocalPath())));
        const auto Rogue = Directory.path() + "/hc-materialized-v1/hc-lease-v1-" + QString(32, 'a');
        QVERIFY(QDir().mkpath(Rogue));
        QFile Manifest(Rogue + "/owner.lock"); QVERIFY(Manifest.open(QIODevice::WriteOnly));
        Manifest.write("{\"schema\":1,\"directory\":\"hc-lease-v1-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"file\":\"../../sentinel\"}"); Manifest.close();
        const auto Invalid = Other->Recover(); QVERIFY(!Invalid.errors_.empty()); QVERIFY(QFile::exists(Manifest.fileName()));
        Made.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void recoveryAfterActualProcessCrash()
    {
        QTemporaryDir Directory;
        QProcess Child;
        Child.start(QCoreApplication::applicationFilePath(), {"--crash-lease", Directory.path()});
        QVERIFY(Child.waitForStarted(5000)); QVERIFY(Child.waitForFinished(15000));
        QCOMPARE(Child.exitCode(), 97); QCOMPARE(LeaseDirectories(Directory.path()).size(), 1);
        auto Service = Manager(Directory.path()); QVERIFY(Service);
        const auto Recovery = Service->Recover(); QVERIFY(Recovery.errors_.empty()); QCOMPARE(Recovery.removed_, 1u);
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void cleanupFailureRetainsManifestAndQuotaUntilRetry()
    {
        QTemporaryDir Directory;
        MaterializationLimits Limits; Limits.maxActiveFiles_ = 1;
        auto Service = Manager(Directory.path(), Limits); QVERIFY(Service);
        auto Made = Service->Materialize({Content("in-use")}, {}).get(); QVERIFY(Made.HasValue());
        const auto Path = QString::fromStdString(Made.Value()->LocalPath());
        const auto External = CreateFileW(QDir::toNativeSeparators(Path).toStdWString().c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(External != INVALID_HANDLE_VALUE);
        auto Close = qScopeGuard([External] { CloseHandle(External); });
        Made.Value().reset();
        const auto Failed = Service->DrainCleanup(); QVERIFY(!Failed.errors_.empty());
        QVERIFY(QFile::exists(Path)); QCOMPARE(LeaseDirectories(Directory.path()).size(), 1);
        const auto Full = Service->Materialize({Content("another")}, {}).get(); QVERIFY(!Full.HasValue());
        CloseHandle(External); Close.dismiss();
        QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(!QFile::exists(Path));
        auto Again = Service->Materialize({Content("available")}, {}).get(); QVERIFY(Again.HasValue());
        Again.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void leaseOutlivesServiceAndConcurrentRequests()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        std::vector<Task<std::shared_ptr<IMaterializedFileLease>>> Futures;
        for (int Index = 0; Index < 16; ++Index) Futures.push_back(Service->Materialize({Content(QByteArray(1024, 'x'))}, {}));
        std::vector<std::shared_ptr<IMaterializedFileLease>> Leases;
        for (auto& Future : Futures) { auto Made = Future.get(); QVERIFY(Made.HasValue()); Leases.push_back(std::move(Made.Value())); }
        const auto Path = QString::fromStdString(Leases.front()->LocalPath());
        Service.reset(); QVERIFY(QFile::exists(Path));
        Leases.clear(); QVERIFY(!QFile::exists(Path));
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void repeatedReuseDoesNotFillJobHistory()
    {
        QTemporaryDir Directory; auto Service = Manager(Directory.path()); QVERIFY(Service);
        for (int Index = 0; Index < 160; ++Index)
        {
            auto Made = Service->Materialize({Content("repeat")}, {}).get(); QVERIFY(Made.HasValue());
            Made.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
        }
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
    void reparseRootAndRenamingAreRejected()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QVERIFY(QDir().mkpath(Directory.filePath("target"))); QVERIFY(QDir().mkpath(Directory.filePath("link")));
        const auto Link = QDir::toNativeSeparators(Directory.filePath("link")).toStdWString();
        const auto Target = QDir::toNativeSeparators(Directory.filePath("target")).toStdWString();
        const auto Substitute = std::wstring(L"\\??\\") + Target;
        struct JunctionData { DWORD tag_; WORD size_, reserved_, substituteOffset_, substituteLength_, printOffset_, printLength_; wchar_t paths_[1]; };
        const auto PathBytes = (Substitute.size() + Target.size() + 2) * sizeof(wchar_t);
        std::vector<std::byte> Buffer(16 + PathBytes);
        auto* Data = reinterpret_cast<JunctionData*>(Buffer.data());
        Data->tag_ = IO_REPARSE_TAG_MOUNT_POINT; Data->size_ = static_cast<WORD>(8 + PathBytes);
        Data->substituteLength_ = static_cast<WORD>(Substitute.size() * sizeof(wchar_t));
        Data->printOffset_ = Data->substituteLength_ + sizeof(wchar_t); Data->printLength_ = static_cast<WORD>(Target.size() * sizeof(wchar_t));
        std::memcpy(Data->paths_, Substitute.c_str(), Data->substituteLength_ + sizeof(wchar_t));
        std::memcpy(reinterpret_cast<std::byte*>(Data->paths_) + Data->printOffset_, Target.c_str(), Data->printLength_ + sizeof(wchar_t));
        const auto Handle = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0;
        const auto Set = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer.data(), static_cast<DWORD>(Buffer.size()), nullptr, 0, &Returned, nullptr);
        CloseHandle(Handle); QVERIFY(Set);
        const auto RemoveLink = qScopeGuard([&] { RemoveDirectoryW(Link.c_str()); });
        const auto Blocked = MakeNativeMaterializer(Directory.filePath("link").toStdString());
        QVERIFY(!Blocked.HasValue()); QCOMPARE(Blocked.Failure().code_, std::string("materialization-reparse-root-denied"));
        QVERIFY(!QDir(Directory.filePath("target/hc-materialized-v1")).exists());
        auto Service = Manager(Directory.path()); QVERIFY(Service);
        const auto From = QDir::toNativeSeparators(Directory.path()).toStdWString();
        const auto To = From + L"-moved";
        const auto Moved = MoveFileExW(From.c_str(), To.c_str(), 0);
        if (Moved) MoveFileExW(To.c_str(), From.c_str(), 0);
        QVERIFY(!Moved);
    }
};

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    if (App.arguments().size() == 3 && App.arguments()[1] == "--crash-lease")
    {
        auto Service = Manager(App.arguments()[2]); if (!Service) return 2;
        auto Made = Service->Materialize({Content("crash-recovery")}, {}).get(); if (!Made.HasValue()) return 3;
        TerminateProcess(GetCurrentProcess(), 97); return 4;
    }
    MaterializationTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstMaterialization.moc"
