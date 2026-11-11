#include <QtTest>
#include "UniqueLibs/nativeMaterializer.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "UniqueLibs/storageProviders.hpp"
#include "memoryStorage.hpp"
#include "commonLibs/storage/resourceQuota.hpp"
#include "commonLibs/storage/providerSession.hpp"
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

namespace {
    bool Put(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    Result<std::shared_ptr<IMaterializedFileLease>> PrivateLease(IMaterializer& Service, std::vector<MaterializationAuxiliary> Auxiliaries = {{"-wal", 0}, {"-shm", 32}})
    { return MaterializePrivateContent(Service, {Content("data", "snapshot.db"), {}, {}, std::move(Auxiliaries)}, {}).get(); }
    bool MakeJunction(const QString& LinkPath, const QString& TargetPath)
    {
        const auto Link = QDir::toNativeSeparators(LinkPath).toStdWString(), Target = QDir::toNativeSeparators(TargetPath).toStdWString();
        const auto Substitute = std::wstring(L"\\??\\") + Target;
        struct JunctionData { DWORD tag_; WORD size_, reserved_, substituteOffset_, substituteLength_, printOffset_, printLength_; wchar_t paths_[1]; };
        const auto PathBytes = (Substitute.size() + Target.size() + 2) * sizeof(wchar_t); std::vector<std::byte> Buffer(16 + PathBytes);
        auto* Data = reinterpret_cast<JunctionData*>(Buffer.data()); Data->tag_ = IO_REPARSE_TAG_MOUNT_POINT; Data->size_ = static_cast<WORD>(8 + PathBytes);
        Data->substituteLength_ = static_cast<WORD>(Substitute.size() * sizeof(wchar_t)); Data->printOffset_ = Data->substituteLength_ + sizeof(wchar_t); Data->printLength_ = static_cast<WORD>(Target.size() * sizeof(wchar_t));
        std::memcpy(Data->paths_, Substitute.c_str(), Data->substituteLength_ + sizeof(wchar_t));
        std::memcpy(reinterpret_cast<std::byte*>(Data->paths_) + Data->printOffset_, Target.c_str(), Data->printLength_ + sizeof(wchar_t));
        const auto Handle = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (Handle == INVALID_HANDLE_VALUE) return false;
        DWORD Returned = 0; const auto Made = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer.data(), static_cast<DWORD>(Buffer.size()), nullptr, 0, &Returned, nullptr); CloseHandle(Handle); return bool(Made);
    }
}
class MaterializationTests : public QObject
{
    Q_OBJECT
private slots:
    void auxiliaryDeclarationsAndUnsupportedRequests()
    {
        QTemporaryDir Root; auto Service = Manager(Root.path()); QVERIFY(Service);
        const auto Ordinary = Service->Materialize({Content("x"), {}, {}, {{"-wal", 0}}}, {}).get();
        QVERIFY(!Ordinary.HasValue()); QCOMPARE(Ordinary.Failure().category_, ErrorCategory::Unsupported);
        class Unsupported final : public IMaterializer {
        public:
            unsigned calls_ = 0;
            Task<std::shared_ptr<IMaterializedFileLease>> Materialize(MaterializationRequest, OperationContext) override
            { ++calls_; return Ready<std::shared_ptr<IMaterializedFileLease>>(Error{ErrorCategory::Internal, "must-not-be-called"}); }
            MaterializationCleanup Recover() override { return {}; }
            MaterializationCleanup DrainCleanup() override { return {}; }
        } Other;
        const auto Rejected = PrivateLease(Other); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Other.calls_, 0u);
        const std::vector<std::vector<MaterializationAuxiliary>> Bad{
            {{"../outside", 0}}, {{"-wal:stream", 0}}, {{"-wal.", 0}}, {{std::string("-a\0b", 4), 0}},
            {{"-wal", 0}, {"-WAL", 0}}, {{"-shm", UINT64_MAX}}, {{"-a", 700000}, {"-b", 700000}},
            {{"-a", 0}, {"-b", 0}, {"-c", 0}, {"-d", 0}, {"-e", 0}}, {{std::string(40, 'a'), 0}}
        };
        for (const auto& Values : Bad) { const auto Made = PrivateLease(*Service, Values); QVERIFY(!Made.HasValue()); QVERIFY(LeaseDirectories(Root.path()).isEmpty()); }
        auto OverflowSource = std::make_shared<FaultContent>(); OverflowSource->entry_.size_ = UINT64_MAX;
        const auto Overflow = MaterializePrivateContent(*Service, {OverflowSource, {}, {}, {{"-cache", 1}}}, {}).get();
        QVERIFY(!Overflow.HasValue()); QCOMPARE(Overflow.Failure().code_, std::string("materialization-total-budget")); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
    }
    void auxiliaryRetirementAndLiveLeaseExclusion()
    {
        QTemporaryDir Root; auto Service = Manager(Root.path()); QVERIFY(Service);
        auto Made = PrivateLease(*Service); QVERIFY(Made.HasValue()); const auto Path = QString::fromUtf8(Made.Value()->LocalPath());
        QFile Owner(QFileInfo(Path).dir().filePath("owner.lock")); QVERIFY(!Owner.open(QIODevice::ReadOnly));
        QVERIFY(Put(Path + "-wal", {})); QVERIFY(Put(Path + "-shm", QByteArray(32, 'i')));
        auto Other = Manager(Root.path()); const auto Live = Other->Recover(); QCOMPARE(Live.busy_, 1u); QVERIFY(Live.errors_.empty());
        Made.Value().reset(); const auto Retired = Service->DrainCleanup(); QVERIFY(Retired.errors_.empty()); QCOMPARE(Retired.busy_, 0u);
        QVERIFY(!QFileInfo::exists(Path)); QVERIFY(!QFileInfo::exists(Path + "-wal")); QVERIFY(!QFileInfo::exists(Path + "-shm")); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
        auto Missing = PrivateLease(*Service); QVERIFY(Missing.HasValue()); Missing.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void auxiliaryInvalidEntryPreservesWorkspace_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"unknown", "oversize", "directory", "reparse", "hardlink", "identity-alias", "locked", "readonly-cache"}) QTest::newRow(Mode) << QString(Mode);
    }
    void auxiliaryInvalidEntryPreservesWorkspace()
    {
        QFETCH(QString, Mode); QTemporaryDir Root; MaterializationLimits Limits; Limits.maxTotalBytes_ = 36;
        auto Service = Manager(Root.path(), Limits); QVERIFY(Service); auto Made = PrivateLease(*Service); QVERIFY(Made.HasValue());
        const auto Path = QString::fromUtf8(Made.Value()->LocalPath()), Wal = Path + "-wal", Shm = Path + "-shm";
        const auto Owner = QFileInfo(Path).dir().filePath("owner.lock"), Unknown = QFileInfo(Path).dir().filePath("unexpected.keep"), Sentinel = Root.filePath("sentinel");
        QVERIFY(Put(Wal, {})); QVERIFY(Put(Sentinel, "original"));
        HANDLE Locked = INVALID_HANDLE_VALUE;
        if (Mode == "unknown") { QVERIFY(Put(Shm, QByteArray(32, 'i'))); QVERIFY(Put(Unknown, "keep")); }
        else if (Mode == "oversize") QVERIFY(Put(Shm, QByteArray(33, 'i')));
        else if (Mode == "directory" || Mode == "reparse") {
            QVERIFY(QDir().mkdir(Shm));
            if (Mode == "reparse") { QVERIFY(QDir().mkdir(Root.filePath("target"))); QVERIFY(Put(Root.filePath("target/keep"), "original")); QVERIFY(MakeJunction(Shm, Root.filePath("target"))); }
        }
        else if (Mode == "hardlink" || Mode == "identity-alias") {
            const auto Target = Mode == "hardlink" ? Sentinel : Wal;
            QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(Shm.utf16()), reinterpret_cast<LPCWSTR>(Target.utf16()), nullptr));
        } else if (Mode == "readonly-cache") {
            QVERIFY(Put(Shm, QByteArray(32, 'i'))); QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(Shm.utf16()), FILE_ATTRIBUTE_READONLY));
        } else {
            QVERIFY(Put(Shm, QByteArray(32, 'i')));
            Locked = CreateFileW(reinterpret_cast<LPCWSTR>(Shm.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(Locked != INVALID_HANDLE_VALUE);
        }
        const auto Unlock = qScopeGuard([&] { if (Locked != INVALID_HANDLE_VALUE) CloseHandle(Locked); });
        Made.Value().reset(); const auto Failed = Service->DrainCleanup(); QVERIFY(!Failed.errors_.empty());
        QVERIFY(QFileInfo::exists(Path)); QVERIFY(QFileInfo::exists(Owner)); QVERIFY(QFileInfo::exists(Shm));
        if (Mode != "readonly-cache") QVERIFY(QFileInfo::exists(Wal));
        else qInfo() << "Late cache disposition failure; earlier cache already retired" << !QFileInfo::exists(Wal);
        QFile Original(Sentinel); QVERIFY(Original.open(QIODevice::ReadOnly)); QCOMPARE(Original.readAll(), QByteArray("original")); Original.close();
        const auto Full = Service->Materialize({Content("x")}, {}).get(); QVERIFY(!Full.HasValue()); QCOMPARE(Full.Failure().code_, std::string("materialization-total-budget"));
        if (Mode == "unknown") QVERIFY(QFile::remove(Unknown));
        else if (Mode == "directory" || Mode == "reparse") QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(Shm.utf16())));
        else if (Mode == "locked") { CloseHandle(Locked); Locked = INVALID_HANDLE_VALUE; }
        else if (Mode == "readonly-cache") QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(Shm.utf16()), FILE_ATTRIBUTE_NORMAL));
        else QVERIFY(QFile::remove(Shm));
        const auto Cleaned = Service->DrainCleanup(); QVERIFY(Cleaned.errors_.empty()); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
        auto Again = Service->Materialize({Content("x")}, {}).get(); QVERIFY(Again.HasValue()); Again.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
        QVERIFY(QFileInfo::exists(Sentinel));
        if (Mode == "reparse") QVERIFY(QFileInfo::exists(Root.filePath("target/keep")));
    }
    void auxiliaryCrashRecoveryAndForgedManifest()
    {
        QTemporaryDir Root; QProcess Child;
        Child.start(QCoreApplication::applicationFilePath(), {"--crash-auxiliary-lease", Root.path()}); QVERIFY(Child.waitForStarted(5000)); QVERIFY(Child.waitForFinished(15000)); QCOMPARE(Child.exitCode(), 97);
        const auto Directories = LeaseDirectories(Root.path()); QCOMPARE(Directories.size(), 1);
        const auto Directory = QDir(Root.path() + "/hc-materialized-v1").filePath(Directories.front());
        QFile File(Directory + "/owner.lock"); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Bytes = File.readAll(); File.close();
        auto Object = QJsonDocument::fromJson(Bytes).object(); QCOMPARE(Object["schema"].toInt(), 2);
        const auto Valid = Object;
        std::vector<QJsonArray> InvalidArrays{
            {QJsonObject{{"name", "../../sentinel"}, {"maxBytes", "0"}}}, {QJsonObject{{"name", "content.db"}, {"maxBytes", "0"}}},
            {QJsonObject{{"name", "content.db-shm"}, {"maxBytes", "18446744073709551616"}}},
            {QJsonObject{{"name", "content.db-" + QString(40, 'a')}, {"maxBytes", "0"}}},
            {QJsonObject{{"name", "content.db-a"}, {"maxBytes", "700000"}}, QJsonObject{{"name", "content.db-b"}, {"maxBytes", "700000"}}}
        };
        QJsonArray TooMany; for (int Index = 0; Index < 5; ++Index) TooMany.append(QJsonObject{{"name", "content.db-" + QString::number(Index)}, {"maxBytes", "0"}}); InvalidArrays.push_back(TooMany);
        for (const auto& Bad : InvalidArrays) {
            Object = Valid; Object["auxiliaries"] = Bad; QVERIFY(Put(File.fileName(), QJsonDocument(Object).toJson(QJsonDocument::Compact)));
            auto Service = Manager(Root.path()); const auto Invalid = Service->Recover(); QVERIFY(!Invalid.errors_.empty()); QVERIFY(QFileInfo::exists(File.fileName())); QVERIFY(QFileInfo::exists(Directory + "/content.db-wal"));
        }
        for (const auto* Reserved : {"0", "+36", "18446744073709551616"}) {
            Object = Valid; Object["reservedBytes"] = QString(Reserved); QVERIFY(Put(File.fileName(), QJsonDocument(Object).toJson(QJsonDocument::Compact)));
            auto Service = Manager(Root.path()); QVERIFY(!Service->Recover().errors_.empty()); QVERIFY(QFileInfo::exists(Directory + "/content.db-wal"));
        }
        QVERIFY(Put(File.fileName(), QByteArray(4097, ' '))); { auto Service = Manager(Root.path()); QVERIFY(!Service->Recover().errors_.empty()); QVERIFY(QFileInfo::exists(Directory + "/content.db-wal")); }
        QVERIFY(Put(File.fileName(), Bytes)); auto Service = Manager(Root.path()); const auto Recovered = Service->Recover(); QVERIFY(Recovered.errors_.empty()); QCOMPARE(Recovered.removed_, 1u); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
    }
    void auxiliaryRecoveryFailureRetainsQuota()
    {
        QTemporaryDir Root; QProcess Child; Child.start(QCoreApplication::applicationFilePath(), {"--crash-auxiliary-lease", Root.path()});
        QVERIFY(Child.waitForStarted(5000)); QVERIFY(Child.waitForFinished(15000)); QCOMPARE(Child.exitCode(), 97);
        const auto Names = LeaseDirectories(Root.path()); QCOMPARE(Names.size(), 1);
        const auto Directory = QDir(Root.path() + "/hc-materialized-v1").filePath(Names.front()), Unknown = Directory + "/keep";
        QVERIFY(Put(Unknown, "unregistered")); MaterializationLimits Limits; Limits.maxTotalBytes_ = 36;
        auto Service = Manager(Root.path(), Limits); const auto Failed = Service->Recover(); QVERIFY(!Failed.errors_.empty());
        QVERIFY(QFileInfo::exists(Directory + "/content.db")); QVERIFY(QFileInfo::exists(Directory + "/owner.lock")); QVERIFY(QFileInfo::exists(Directory + "/content.db-wal"));
        const auto Full = Service->Materialize({Content("x")}, {}).get(); QVERIFY(!Full.HasValue()); QCOMPARE(Full.Failure().code_, std::string("materialization-total-budget"));
        QVERIFY(QFile::remove(Unknown)); QVERIFY(Service->DrainCleanup().errors_.empty()); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
        auto Again = Service->Materialize({Content("x")}, {}).get(); QVERIFY(Again.HasValue()); Again.Value().reset(); QVERIFY(Service->DrainCleanup().errors_.empty());
    }
    void auxiliaryUnderreportedContentIsPreserved()
    {
        QTemporaryDir Root; QProcess Child; Child.start(QCoreApplication::applicationFilePath(), {"--crash-auxiliary-lease", Root.path()});
        QVERIFY(Child.waitForStarted(5000)); QVERIFY(Child.waitForFinished(15000)); QCOMPARE(Child.exitCode(), 97);
        const auto Names = LeaseDirectories(Root.path()); QCOMPARE(Names.size(), 1);
        const auto Directory = QDir(Root.path() + "/hc-materialized-v1").filePath(Names.front()), Path = Directory + "/content.db", Owner = Directory + "/owner.lock";
        QFile File(Owner); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Original = File.readAll(); File.close(); auto Manifest = QJsonDocument::fromJson(Original).object();
        Manifest["reservedBytes"] = "35"; QVERIFY(Put(Owner, QJsonDocument(Manifest).toJson(QJsonDocument::Compact)));
        MaterializationLimits Limits; Limits.maxTotalBytes_ = 35; auto Service = Manager(Root.path(), Limits);
        const auto Rejected = Service->Recover(); QVERIFY(!Rejected.errors_.empty()); QCOMPARE(Rejected.errors_.front().code_, std::string("invalid-lease-file"));
        QVERIFY(QFileInfo::exists(Owner)); QVERIFY(QFileInfo::exists(Path + "-wal")); QVERIFY(QFileInfo::exists(Path + "-shm"));
        QFile ContentFile(Path); QVERIFY(ContentFile.open(QIODevice::ReadOnly)); QCOMPARE(ContentFile.readAll(), QByteArray("data")); ContentFile.close();
        const auto Full = Service->Materialize({Content("x")}, {}).get(); QVERIFY(!Full.HasValue()); QCOMPARE(Full.Failure().code_, std::string("materialization-total-budget"));
        Service.reset(); QVERIFY(QFileInfo::exists(Owner)); QVERIFY(QFileInfo::exists(Path));
        QVERIFY(Put(Owner, Original)); auto RecoveredService = Manager(Root.path()); const auto Recovered = RecoveredService->Recover(); QVERIFY(Recovered.errors_.empty()); QVERIFY(LeaseDirectories(Root.path()).isEmpty());
    }
    void explicitPolicyAndConcurrentReservations()
    {
        OperationContext Context; QCOMPARE(Context.EnumerationLimit(), SIZE_MAX);
        EntrySnapshot Entry; Entry.size_ = 5ULL * 1024 * 1024 * 1024; Entry.locator_.kind_ = LocatorKind::Archive;
        QVERIFY(!CheckMaterialization(Entry, 16, 1, {}, Context));
        Context.workload_.intent_ = WorkloadIntent::Automatic;
        QCOMPARE(Context.EnumerationLimit(), std::size_t(100000));
        QVERIFY(CheckMaterialization(Entry, 16, 1, {}, Context).has_value());
        ResourceQuota Quota({100, 10, 1, 2, 1000});
        for (unsigned Index = 0; Index < 1000; ++Index) {
            auto Lease = Quota.Acquire(ResourceKind::Buffer, 10); QVERIFY(Lease.HasValue());
            auto Busy = Quota.Acquire(ResourceKind::Buffer, 1); QVERIFY(!Busy.HasValue()); QVERIFY(Busy.Failure().retryable_);
            auto Moved = std::move(Lease.Value()); QCOMPARE(Quota.Used(ResourceKind::Buffer), std::uint64_t(10));
        }
        QCOMPARE(Quota.Used(ResourceKind::Buffer), std::uint64_t(0));
        auto TooLarge = Quota.Acquire(ResourceKind::Buffer, UINT64_MAX); QVERIFY(!TooLarge.HasValue()); QVERIFY(!TooLarge.Failure().retryable_);
        ProviderSessionSnapshot Session{"epoch", ProviderSessionState::Ready}; EntryId Id{"epoch", {"revision"}, "item"};
        QVERIFY(ValidateEntryId(Id, Session, {"revision"}).HasValue());
        Session.epoch_ = "restart"; QCOMPARE(ValidateEntryId(Id, Session, {"revision"}).Failure().category_, ErrorCategory::StaleRevision);
    }
    void explicitLargeSparseContentAndAutomaticGuard()
    {
        QTemporaryDir Directory; auto Store = MakeNativeContentStore(Directory.path().toStdString()); QVERIFY(Store.HasValue());
        EntrySnapshot Entry; Entry.name_ = "large.bin"; Entry.size_ = (1ULL << 32) + 7;
        OperationContext Automatic; Automatic.workload_.intent_ = WorkloadIntent::Automatic;
        bool Produced = false;
        const auto Producer = [&](const std::string& Path, OperationContext) -> Result<Unit> {
            Produced = true; HANDLE File = CreateFileW(QString::fromStdString(Path).toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (File == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "sparse-open"};
            DWORD Done = 0; LARGE_INTEGER Size; Size.QuadPart = Entry.size_;
            const bool Good = DeviceIoControl(File, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Done, nullptr)
                && SetFilePointerEx(File, Size, nullptr, FILE_BEGIN) && SetEndOfFile(File);
            CloseHandle(File); if (!Good) return Error{ErrorCategory::Unavailable, "sparse-create"}; return Unit{};
        };
        auto Guarded = Store.Value()->Produce(Entry, {}, {}, Automatic, Producer); QVERIFY(!Guarded.HasValue()); QVERIFY(!Produced);
        auto Large = Store.Value()->Produce(Entry, {}, {}, {}, Producer); QVERIFY2(Large.HasValue(), Large.HasValue() ? "" : Large.Failure().code_.c_str());
        QVERIFY(Produced); QCOMPARE(Large.Value()->Describe().size_, Entry.size_);
        Large.Value().reset(); auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue());
        QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
    }
    void lastReferenceReleasedOnDifferentThread()
    {
        QTemporaryDir Directory;
        for (int Index = 0; Index < 8; ++Index)
        {
            auto Service = Manager(Directory.path()); QVERIFY(Service);
            auto Made = Service->Materialize({Content("cross-thread lifetime")}, {}).get(); QVERIFY(Made.HasValue());
            auto Lease = std::move(Made.Value());
            const auto Path = QString::fromStdString(Lease->LocalPath());
            const auto Creator = std::this_thread::get_id();
            auto Destroyed = std::async(std::launch::async, [Owned = std::move(Service), Creator]() mutable {
                const bool Different = std::this_thread::get_id() != Creator;
                Owned.reset(); return Different;
            });
            QVERIFY(Destroyed.get());
            QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("cross-thread lifetime")); File.close();
            Lease.reset();
            auto Cleanup = Manager(Directory.path()); QVERIFY(Cleanup);
            QVERIFY(Cleanup->DrainCleanup().errors_.empty()); QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
        }
    }
    void failedProducerWorkspacePreservesManifest()
    {
        QTemporaryDir Directory;
        auto ManagerResult = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(ManagerResult.HasValue());
        auto Store = MakeNativeContentStore(Directory.path().toStdString()); QVERIFY(Store.HasValue());
        QString Workspace;
        const auto Failed = Store.Value()->Produce(Content("x")->Describe(), {}, {}, {}, [&](const std::string& Path, OperationContext) -> Result<Unit> {
            Workspace = QFileInfo(QString::fromStdString(Path)).dir().filePath("inputs");
            if (!QDir().mkdir(Workspace)) return Error{ErrorCategory::Unavailable, "mkdir-failed"};
            return Error{ErrorCategory::Unavailable, "injected-interrupted-producer"};
        }); QVERIFY(!Failed.HasValue()); QVERIFY(Failed.Failure().userAction_.find("cleanup-pending:") == 0);
        QVERIFY(QFile::exists(QFileInfo(Workspace).dir().filePath("owner.lock")));
        QVERIFY(QDir(Workspace).exists());
        QVERIFY(QDir().rmdir(Workspace)); QVERIFY(ManagerResult.Value()->DrainCleanup().errors_.empty());
        QVERIFY(LeaseDirectories(Directory.path()).isEmpty());
    }
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
        auto Bounded = Store->Produce(Content("12345")->Describe(), {}, {}, {}, [](const std::string& Path, OperationContext) -> Result<Unit> {
            QFile File(QString::fromStdString(Path)); if (!File.open(QIODevice::WriteOnly) || File.write("x") != 1) return Error{ErrorCategory::Unavailable, "write-failed"};
            return Unit{};
        }, ProducedSize::Bounded); QVERIFY(Bounded.HasValue()); QCOMPARE(Bounded.Value()->Describe().size_, 1u);
        auto Remaining = Manager->Materialize({Content("1234")}, {}).get(); QVERIFY(Remaining.HasValue());
        const auto ActualFull = Manager->Materialize({Content("x")}, {}).get(); QVERIFY(!ActualFull.HasValue());
        QCOMPARE(ActualFull.Failure().category_, ErrorCategory::BudgetExceeded);
        Remaining.Value().reset();
        Bounded.Value().reset(); QVERIFY(Manager->DrainCleanup().errors_.empty());
        const auto CeilingExceeded = Store->Produce(Content("1234")->Describe(), {}, {}, {}, Write, ProducedSize::Bounded);
        QVERIFY(!CeilingExceeded.HasValue()); QCOMPARE(CeilingExceeded.Failure().code_, "invalid-produced-content");
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
    void growingOutputChargesActualBytes()
    {
        QTemporaryDir Directory; MaterializationLimits Limits; Limits.maxTotalBytes_ = 8;
        auto Store = MakeNativeContentStore(Directory.path().toStdString(), Limits); QVERIFY(Store.HasValue());
        auto Entry = Content("x")->Describe(); Entry.size_ = UINT64_MAX;
        const auto Produce = [&](int Count) {
            return Store.Value()->Produce(Entry, {}, {}, {}, [Count](const std::string& Path, OperationContext Context) -> Result<Unit> {
                QFile File(QString::fromStdString(Path)); if (!File.open(QIODevice::WriteOnly) || File.write(QByteArray(Count, 'x')) != Count || !File.flush())
                    return Error{ErrorCategory::Unavailable, "test-write"};
                if (Context.progress_) Context.progress_(Count); return Unit{};
            }, ProducedSize::Growing);
        };
        auto First = Produce(5); QVERIFY(First.HasValue()); QCOMPARE(First.Value()->Describe().size_, 5ull);
        auto TooMuch = Produce(4); QVERIFY(!TooMuch.HasValue()); QCOMPARE(TooMuch.Failure().code_, "materialization-total-budget");
        auto Second = Produce(3); QVERIFY(Second.HasValue()); QCOMPARE(Second.Value()->Describe().size_, 3ull);
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
        OperationContext Automatic; Automatic.workload_.intent_ = WorkloadIntent::Automatic;
        auto Missing = Service->Materialize({Source}, Automatic).get(); QVERIFY(!Missing.HasValue());
        QCOMPARE(Missing.Failure().code_, std::string("compressed-size-required"));
        Source->entry_.size_ = 1001;
        auto Expanded = Service->Materialize({Source, {}, 1}, Automatic).get(); QVERIFY(!Expanded.HasValue());
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
    if (App.arguments().size() == 3 && (App.arguments()[1] == "--crash-lease" || App.arguments()[1] == "--crash-auxiliary-lease"))
    {
        auto Service = Manager(App.arguments()[2]); if (!Service) return 2;
        const bool Auxiliary = App.arguments()[1] == "--crash-auxiliary-lease";
        auto Made = Auxiliary ? PrivateLease(*Service) : Service->Materialize({Content("crash-recovery")}, {}).get(); if (!Made.HasValue()) return 3;
        if (Auxiliary) { const auto Path = QString::fromUtf8(Made.Value()->LocalPath()); if (!Put(Path + "-wal", {}) || !Put(Path + "-shm", QByteArray(32, 'i'))) return 5; }
        TerminateProcess(GetCurrentProcess(), 97); return 4;
    }
    MaterializationTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstMaterialization.moc"
