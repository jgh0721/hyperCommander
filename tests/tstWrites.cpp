#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <Windows.h>
#include <winioctl.h>

using namespace hc::storage;
using namespace hc::operations;

namespace
{
    enum class WriteFault { Remove, Append, Metadata, Commit, AppendAndCleanup };
    class FaultSession final : public IWriteSession
    {
    public:
        FaultSession(std::unique_ptr<IWriteSession> Session, WriteFault Fault) : session_(std::move(Session)), fault_(Fault) {}
        Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
        {
            if (fault_ == WriteFault::Append || fault_ == WriteFault::AppendAndCleanup)
            {
                const auto Partial = session_->Append(Bytes.first((std::max)(std::size_t{1}, Bytes.size() / 2)), Context);
                if (!Partial.HasValue()) return Partial.Failure();
                return Error{ErrorCategory::Unavailable, "injected-disk-full"};
            }
            return session_->Append(Bytes, Context);
        }
        Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) override
        {
            if (fault_ == WriteFault::Metadata) return Error{ErrorCategory::AccessDenied, "injected-metadata"};
            return session_->SetMetadata(std::move(Metadata), Context);
        }
        Result<WriteReceipt> Commit(OperationContext Context) override
        {
            if (fault_ == WriteFault::Commit) return Error{ErrorCategory::Unavailable, "injected-commit"};
            return session_->Commit(Context);
        }
        Result<Unit> Abort() override
        {
            const auto Cleaned = session_->Abort();
            if (!Cleaned.HasValue()) return Cleaned.Failure();
            if (fault_ == WriteFault::AppendAndCleanup) return Error{ErrorCategory::AccessDenied, "injected-cleanup"};
            return Unit{};
        }
    private:
        std::unique_ptr<IWriteSession> session_;
        WriteFault fault_;
    };
    class ImmediateExecutor final : public hc::commands::IExecutor
    {
    public:
        bool Post(std::function<void()> Work) override { Work(); return true; }
    };
    class FailedRemoveWriter final : public IWritableStorage
    {
    public:
        explicit FailedRemoveWriter(std::shared_ptr<IWritableStorage> Writer, WriteFault Fault = WriteFault::Remove) : writer_(std::move(Writer)), fault_(Fault) {}
        ProviderId Id() const override { return writer_->Id(); }
        Result<SupportDecision> Supports(Mutation Operation, Locator At, OperationContext Context) override { return writer_->Supports(Operation, std::move(At), Context); }
        Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override { return writer_->Inspect(std::move(At), Context); }
        Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override
        {
            auto Session = writer_->BeginWrite(std::move(At), std::move(Expected), Context);
            if (!Session.HasValue()) return Session.Failure();
            return std::unique_ptr<IWriteSession>(new FaultSession(std::move(Session.Value()), fault_));
        }
        Result<Unit> MakeDirectory(Locator At, OperationContext Context) override { return writer_->MakeDirectory(std::move(At), Context); }
        Result<Unit> Remove(EntrySnapshot, OperationContext) override { return Error{ErrorCategory::AccessDenied, "injected-source-delete"}; }
        Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override { return writer_->Rename(std::move(Entry), std::move(To), Context); }
    private:
        std::shared_ptr<IWritableStorage> writer_;
        WriteFault fault_;
    };
}

class WriteTests : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> root_;
    std::shared_ptr<IStorageProvider> reader_;
    std::shared_ptr<IWritableStorage> writer_;
    LocatorKind kind_ = LocatorKind::Local;
    Locator at(const QString& Name) const { return {kind_, QDir::toNativeSeparators(root_->filePath(Name)).toStdString()}; }
    bool write(const QString& Name, const QByteArray& Bytes)
    {
        QFile File(root_->filePath(Name));
        return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
    }
    QByteArray read(const QString& Name)
    {
        QFile File(root_->filePath(Name));
        if (!File.open(QIODevice::ReadOnly)) return {};
        return File.readAll();
    }
    Result<Unit> append(IWriteSession& Session, const QByteArray& Bytes, OperationContext Context = {})
    { return Session.Append({reinterpret_cast<const std::byte*>(Bytes.constData()), static_cast<std::size_t>(Bytes.size())}, Context); }
    QStringList artifacts()
    { return QDir(root_->path()).entryList({".hc-*"}, QDir::Files | QDir::Hidden | QDir::System); }
private slots:
    void initTestCase()
    {
        const auto Remote = qEnvironmentVariable("HC_MUTATION_ROOT");
        if (!Remote.isEmpty())
        {
            QVERIFY(Remote.compare("\\\\DEVTESTER-QTS\\Public", Qt::CaseInsensitive) == 0
                || Remote.compare("\\\\DEVTESTER-QTB4\\Pubs", Qt::CaseInsensitive) == 0);
            kind_ = LocatorKind::Smb;
            root_ = std::make_unique<QTemporaryDir>(QDir(Remote).filePath("hc-write-test-XXXXXX"));
        }
        else root_ = std::make_unique<QTemporaryDir>();
        QVERIFY(root_->isValid());
        reader_ = MakeNativeReadProvider(kind_);
        writer_ = MakeNativeWritableStorage(kind_, reader_);
        qInfo().noquote() << "Owned mutation fixture:" << root_->path();
    }
    void stagedCommitAndMetadata()
    {
        auto Session = writer_->BeginWrite(at("한글 staged.txt"), {}, {});
        QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
        QVERIFY(append(*Session.Value(), "first ").HasValue());
        QVERIFY(append(*Session.Value(), "second").HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("한글 staged.txt")));
        QVERIFY(Session.Value()->SetMetadata({1700000000123LL, FILE_ATTRIBUTE_ARCHIVE}, {}).HasValue());
        auto Result = Session.Value()->Commit({});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        QVERIFY(Result.Value().cleanupErrors_.empty());
        QCOMPARE(read("한글 staged.txt"), QByteArray("first second"));
        auto Entry = writer_->Inspect(at("한글 staged.txt"), {});
        QVERIFY(Entry.HasValue() && Entry.Value());
        QCOMPARE(Entry.Value()->modifiedUtcMs_, 1700000000123LL);
        auto Stream = reader_->OpenRead(Entry.Value()->id_, {}).get();
        QVERIFY(Stream.HasValue());
        auto Bytes = Stream.Value()->Read(64, {}).get();
        QVERIFY(Bytes.HasValue());
        QCOMPARE(Bytes.Value().bytes_.size(), std::size_t{12});
        QVERIFY(Session.Value()->Abort().HasValue());
        QCOMPARE(read("한글 staged.txt"), QByteArray("first second"));
        QVERIFY(artifacts().isEmpty());
    }
    void destinationAttributesAreVerified()
    {
        constexpr DWORD Mask = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY;
        for (const auto& Name : {QString("ordinary-attributes.txt"), QString(".dot-attributes.txt")})
        {
            for (const DWORD Attributes : {DWORD(FILE_ATTRIBUTE_ARCHIVE), DWORD(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN),
                DWORD(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)})
            {
                const auto Path = QDir::toNativeSeparators(root_->filePath(Name)).toStdWString();
                const auto Remove = qScopeGuard([&] { SetFileAttributesW(Path.c_str(), FILE_ATTRIBUTE_NORMAL); QFile::remove(QString::fromStdWString(Path)); });
                auto Session = writer_->BeginWrite(at(Name), {}, {});
                QVERIFY2(Session.HasValue(), Session.HasValue() ? "" : Session.Failure().code_.c_str());
                QVERIFY(append(*Session.Value(), "attribute fixture").HasValue());
                QVERIFY(Session.Value()->SetMetadata({1700000000123LL, Attributes}, {}).HasValue());
                auto Receipt = Session.Value()->Commit({});
                QVERIFY2(Receipt.HasValue(), Receipt.HasValue() ? "" : Receipt.Failure().code_.c_str());
                QCOMPARE(read(Name), QByteArray("attribute fixture"));
                const auto Actual = GetFileAttributesW(Path.c_str());
                QVERIFY(Actual != INVALID_FILE_ATTRIBUTES);
                const auto Difference = (Actual ^ Attributes) & Mask;
                qInfo() << Name << "requested" << Attributes << "actual" << Actual;
                if (kind_ == LocatorKind::Local) QCOMPARE(Difference, DWORD(0));
                if (!Difference) QVERIFY(Receipt.Value().cleanupErrors_.empty());
                else
                {
                    QCOMPARE(Receipt.Value().cleanupErrors_.size(), std::size_t(1));
                    QCOMPARE(Receipt.Value().cleanupErrors_.front().code_, Difference == FILE_ATTRIBUTE_HIDDEN && (Actual & FILE_ATTRIBUTE_HIDDEN)
                        ? std::string("destination-added-hidden-attribute") : std::string("destination-attributes-mismatch"));
                }
                QVERIFY(Session.Value()->Abort().HasValue());
                QCOMPARE(read(Name), QByteArray("attribute fixture"));
                QVERIFY(artifacts().isEmpty());
            }
        }
    }
    void coordinatorDotFileMetadata()
    {
        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
        auto LocalWriter = MakeNativeWritableStorage(LocatorKind::Local, LocalReader);
        for (const auto Operation : {FileOperation::Copy, FileOperation::Move})
        {
            QTemporaryDir SourceRoot; QVERIFY(SourceRoot.isValid());
            const auto Name = Operation == FileOperation::Copy ? ".copy-attributes.txt" : ".move-attributes.txt";
            const auto SourcePath = QDir::toNativeSeparators(SourceRoot.filePath(Name));
            { QFile File(SourcePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("dot file fixture"), 16); }
            QVERIFY(SetFileAttributesW(SourcePath.toStdWString().c_str(), FILE_ATTRIBUTE_ARCHIVE));
            auto Source = LocalWriter->Inspect({LocatorKind::Local, SourcePath.toStdString()}, {});
            QVERIFY(Source.HasValue() && Source.Value());
            QCOMPARE(Source.Value()->attributes_, std::uint32_t(FILE_ATTRIBUTE_ARCHIVE));
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
            FileOperationRequest Request;
            Request.operation_ = Operation;
            Request.sources_ = {{*Source.Value(), LocalReader, LocalWriter}};
            Request.destination_ = at(""); Request.destinationWriter_ = writer_;
            auto Submitted = SubmitFileOperation(Scheduler, Request); QVERIFY(Submitted.HasValue());
            QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
            const auto& Result = Submitted.Value().completion_.get();
            QCOMPARE(read(Name), QByteArray("dot file fixture"));
            const auto Actual = GetFileAttributesW(QDir::toNativeSeparators(root_->filePath(Name)).toStdWString().c_str());
            QVERIFY(Actual != INVALID_FILE_ATTRIBUTES);
            QCOMPARE(Result.items_.size(), std::size_t(1));
            QVERIFY(Result.items_.front().destinationCommitted_);
            if (Actual & FILE_ATTRIBUTE_HIDDEN)
            {
                QCOMPARE(Result.snapshot_.state_, JobState::Failed);
                QVERIFY(Result.snapshot_.error_);
                QVERIFY(Result.items_.front().error_);
                QCOMPARE(Result.items_.front().error_->code_, std::string("destination-added-hidden-attribute"));
                QVERIFY(QFileInfo::exists(SourcePath));
                QVERIFY(!Result.items_.front().sourceRemoved_);
            }
            else
            {
                QCOMPARE(Result.snapshot_.state_, JobState::Completed);
                QCOMPARE(QFileInfo::exists(SourcePath), Operation == FileOperation::Copy);
            }
        }
    }
    void overwriteRevalidatesDestination()
    {
        QVERIFY(write("replace.txt", "original"));
        const auto Old = writer_->Inspect(at("replace.txt"), {});
        QVERIFY(Old.HasValue() && Old.Value());
        auto Session = writer_->BeginWrite(at("replace.txt"), {true, Old.Value()->id_.revision_}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "replacement").HasValue());
        QCOMPARE(read("replace.txt"), QByteArray("original"));
        auto Committed = Session.Value()->Commit({});
        QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
        QVERIFY(Committed.Value().cleanupErrors_.empty());
        QCOMPARE(read("replace.txt"), QByteArray("replacement"));
        QVERIFY(artifacts().isEmpty());

        auto Stale = writer_->BeginWrite(at("replace.txt"), {true, Old.Value()->id_.revision_}, {});
        QVERIFY(Stale.HasValue());
        QVERIFY(append(*Stale.Value(), "must not win").HasValue());
        auto Rejected = Stale.Value()->Commit({});
        QVERIFY(!Rejected.HasValue());
        QCOMPARE(Rejected.Failure().category_, ErrorCategory::StaleRevision);
        QVERIFY(Stale.Value()->Abort().HasValue());
        QCOMPARE(read("replace.txt"), QByteArray("replacement"));
        QVERIFY(artifacts().isEmpty());
    }
    void newDestinationCannotReplaceConcurrentFile()
    {
        auto Session = writer_->BeginWrite(at("race.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "stage").HasValue());
        QVERIFY(write("race.txt", "concurrent"));
        QVERIFY(!Session.Value()->Commit({}).HasValue());
        QVERIFY(Session.Value()->Abort().HasValue());
        QCOMPARE(read("race.txt"), QByteArray("concurrent"));
        QVERIFY(artifacts().isEmpty());
    }
    void cancelAndAbortLeaveNoStage()
    {
        auto Session = writer_->BeginWrite(at("cancel.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "partial").HasValue());
        std::stop_source Stop;
        Stop.request_stop();
        OperationContext Context;
        Context.stopToken_ = Stop.get_token();
        QCOMPARE(append(*Session.Value(), "more", Context).Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(Session.Value()->Commit(Context).Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("cancel.txt")));
        QVERIFY(artifacts().isEmpty());
        auto Empty = writer_->BeginWrite(at("empty.txt"), {}, {});
        QVERIFY(Empty.HasValue());
        QVERIFY(Empty.Value()->Commit({}).HasValue());
        QVERIFY(QFileInfo::exists(root_->filePath("empty.txt")));
        QCOMPARE(QFileInfo(root_->filePath("empty.txt")).size(), 0);
    }
    void mkdirRenameRemoveAndStaleSource()
    {
        QVERIFY(writer_->MakeDirectory(at("directory"), {}).HasValue());
        QVERIFY(!writer_->MakeDirectory(at("directory"), {}).HasValue());
        QVERIFY(write("directory/source.txt", "source"));
        auto Source = writer_->Inspect(at("directory/source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        QVERIFY(writer_->Rename(*Source.Value(), at("directory/renamed.txt"), {}).HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("directory/source.txt")));
        QCOMPARE(read("directory/renamed.txt"), QByteArray("source"));
        auto Renamed = writer_->Inspect(at("directory/renamed.txt"), {});
        QVERIFY(Renamed.HasValue() && Renamed.Value());
        QVERIFY(write("directory/renamed.txt", "changed source"));
        QCOMPARE(writer_->Remove(*Renamed.Value(), {}).Failure().category_, ErrorCategory::StaleRevision);
        auto Directory = writer_->Inspect(at("directory"), {});
        QVERIFY(Directory.HasValue() && Directory.Value());
        QVERIFY(!writer_->Remove(*Directory.Value(), {}).HasValue());
        Renamed = writer_->Inspect(at("directory/renamed.txt"), {});
        QVERIFY(writer_->Remove(*Renamed.Value(), {}).HasValue());
        Directory = writer_->Inspect(at("directory"), {});
        QVERIFY(writer_->Remove(*Directory.Value(), {}).HasValue());
        QVERIFY(!QFileInfo::exists(root_->filePath("directory")));
    }
    void replacedDirectoryAndConcurrentSourceWritesAreRejected()
    {
        QVERIFY(writer_->MakeDirectory(at("identity"), {}).HasValue());
        auto Original = writer_->Inspect(at("identity"), {});
        QVERIFY(Original.HasValue() && Original.Value());
        QVERIFY(writer_->Rename(*Original.Value(), at("identity-old"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("identity"), {}).HasValue());
        QCOMPARE(writer_->Remove(*Original.Value(), {}).Failure().category_, ErrorCategory::StaleRevision);
        QVERIFY(QFileInfo::exists(root_->filePath("identity")));
        QVERIFY(write("locked.txt", "stable bytes"));
        auto File = writer_->Inspect(at("locked.txt"), {});
        QVERIFY(File.HasValue() && File.Value());
        OperationContext Context; Context.denyConcurrentMutation_ = true;
        auto Stream = reader_->OpenRead(File.Value()->id_, Context).get();
        QVERIFY(Stream.HasValue());
        QVERIFY(!write("locked.txt", "torn bytes"));
        Stream.Value().reset();
        QCOMPARE(read("locked.txt"), QByteArray("stable bytes"));
        QVERIFY(write("locked.txt", "after close"));
    }
    void invalidNamesAndRootMutations()
    {
        for (const auto& Name : {"../escape", "CON.txt", "bad:stream", "trailing.", "trailing ", "a/../b", "a//b", "bad*name"})
            QVERIFY2(!writer_->BeginWrite(at(Name), {}, {}).HasValue(), Name);
        Locator Invalid{kind_, "relative.txt"};
        QVERIFY(!writer_->BeginWrite(Invalid, {}, {}).HasValue());
        auto Missing = writer_->Inspect(at("missing.txt"), {});
        QVERIFY(Missing.HasValue() && !Missing.Value());
        QVERIFY(artifacts().isEmpty());
    }
    void pinnedParentCannotBeRenamed()
    {
        QVERIFY(writer_->MakeDirectory(at("pinned"), {}).HasValue());
        auto Session = writer_->BeginWrite(at("pinned/file.txt"), {}, {});
        QVERIFY(Session.HasValue());
        const auto From = QDir::toNativeSeparators(root_->filePath("pinned")).toStdWString();
        const auto To = QDir::toNativeSeparators(root_->filePath("pinned-moved")).toStdWString();
        QVERIFY(!MoveFileExW(From.c_str(), To.c_str(), 0));
        QVERIFY(Session.Value()->Abort().HasValue());
        QVERIFY(MoveFileExW(From.c_str(), To.c_str(), 0));
    }
    void junctionTraversalIsRejected()
    {
        if (kind_ != LocatorKind::Local) QSKIP("Local NTFS junction fixture; SMB reparse support depends on server");
        QVERIFY(writer_->MakeDirectory(at("junction-target"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("junction"), {}).HasValue());
        QVERIFY(write("junction-target/sentinel.txt", "preserve"));
        const auto Link = QDir::toNativeSeparators(root_->filePath("junction")).toStdWString();
        const auto Target = QDir::toNativeSeparators(root_->filePath("junction-target")).toStdWString();
        const auto Substitute = std::wstring(L"\\??\\") + Target;
        struct JunctionData
        {
            DWORD tag_;
            WORD size_, reserved_, substituteOffset_, substituteLength_, printOffset_, printLength_;
            wchar_t paths_[1];
        };
        const auto PathBytes = (Substitute.size() + Target.size() + 2) * sizeof(wchar_t);
        std::vector<std::byte> Buffer(16 + PathBytes);
        auto* Data = reinterpret_cast<JunctionData*>(Buffer.data());
        Data->tag_ = IO_REPARSE_TAG_MOUNT_POINT;
        Data->size_ = static_cast<WORD>(8 + PathBytes);
        Data->substituteLength_ = static_cast<WORD>(Substitute.size() * sizeof(wchar_t));
        Data->printOffset_ = Data->substituteLength_ + sizeof(wchar_t);
        Data->printLength_ = static_cast<WORD>(Target.size() * sizeof(wchar_t));
        memcpy(Data->paths_, Substitute.c_str(), Data->substituteLength_ + sizeof(wchar_t));
        memcpy(reinterpret_cast<std::byte*>(Data->paths_) + Data->printOffset_, Target.c_str(), Data->printLength_ + sizeof(wchar_t));
        const auto Handle = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0;
        const bool Set = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer.data(), static_cast<DWORD>(Buffer.size()), nullptr, 0, &Returned, nullptr);
        CloseHandle(Handle);
        QVERIFY(Set);
        const auto Cleanup = qScopeGuard([&] { RemoveDirectoryW(Link.c_str()); });
        auto Blocked = writer_->BeginWrite(at("junction/escaped.txt"), {}, {});
        QVERIFY(!Blocked.HasValue());
        QCOMPARE(Blocked.Failure().code_, std::string("reparse-parent-denied"));
        QVERIFY(!writer_->Inspect(at("junction"), {}).HasValue());
        QCOMPARE(read("junction-target/sentinel.txt"), QByteArray("preserve"));
        QVERIFY(!QFileInfo::exists(root_->filePath("junction-target/escaped.txt")));
    }
    void longPaths()
    {
        QString Path;
        for (int Index = 0; Index < 5; ++Index)
        {
            Path += (Path.isEmpty() ? "" : "/") + QString(55, QChar('a' + Index));
            QVERIFY(writer_->MakeDirectory(at(Path), {}).HasValue());
        }
        auto Session = writer_->BeginWrite(at(Path + "/long.txt"), {}, {});
        QVERIFY(Session.HasValue());
        QVERIFY(append(*Session.Value(), "long-path-data").HasValue());
        QVERIFY(Session.Value()->Commit({}).HasValue());
        auto Entry = writer_->Inspect(at(Path + "/long.txt"), {});
        QVERIFY(Entry.HasValue() && Entry.Value());
        QVERIFY(writer_->Remove(*Entry.Value(), {}).HasValue());
        for (int Index = 0; Index < 5; ++Index)
        {
            Entry = writer_->Inspect(at(Path), {});
            QVERIFY(Entry.HasValue() && Entry.Value());
            QVERIFY(writer_->Remove(*Entry.Value(), {}).HasValue());
            Path = Path.left(Path.lastIndexOf('/'));
        }
    }
    void coordinatorCopyMoveAndConfirmedDeleteTree()
    {
        QVERIFY(writer_->MakeDirectory(at("tree-source"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-source/inner"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-copy"), {}).HasValue());
        QVERIFY(writer_->MakeDirectory(at("tree-move"), {}).HasValue());
        QVERIFY(write("tree-source/a.txt", "alpha"));
        QVERIFY(write("tree-source/inner/b.txt", "beta"));
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        auto Source = writer_->Inspect(at("tree-source"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("tree-copy");
        Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Copied = Submitted.Value().completion_.get();
        if (Copied.snapshot_.error_ && Copied.snapshot_.error_->resource_)
        {
            auto Resource = *Copied.snapshot_.error_->resource_;
            const auto ExpectedRevision = Resource.revision_;
            Resource.revision_.reset();
            const auto Fresh = reader_->Stat(Resource, {}).get();
            qInfo().noquote() << "Copy failure resource:" << QString::fromStdString(Resource.key_)
                << "expected:" << (ExpectedRevision ? QString::fromStdString(ExpectedRevision->token_) : QString())
                << "fresh:" << (Fresh.HasValue() && Fresh.Value().id_.revision_ ? QString::fromStdString(Fresh.Value().id_.revision_->token_) : QString())
                << "items:" << Copied.items_.size() << "current:" << QString::fromStdString(Copied.snapshot_.progress_.currentItem_);
        }
        QVERIFY2(Copied.snapshot_.state_ == JobState::Completed, Copied.snapshot_.error_ ? Copied.snapshot_.error_->code_.c_str() : "copy did not complete");
        QCOMPARE(read("tree-copy/tree-source/a.txt"), QByteArray("alpha"));
        QCOMPARE(read("tree-copy/tree-source/inner/b.txt"), QByteArray("beta"));
        QCOMPARE(Copied.snapshot_.progress_.bytesDone_, 9u);
        Request.operation_ = FileOperation::Move;
        Source = writer_->Inspect(at("tree-copy/tree-source"), {});
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("tree-move");
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Moved = Submitted.Value().completion_.get();
        if (Moved.snapshot_.error_ && Moved.snapshot_.error_->resource_)
        {
            auto Resource = *Moved.snapshot_.error_->resource_;
            const auto ExpectedRevision = Resource.revision_;
            Resource.revision_.reset();
            const auto Fresh = reader_->Stat(Resource, {}).get();
            qInfo().noquote() << "Move failure resource:" << QString::fromStdString(Resource.key_)
                << "expected:" << (ExpectedRevision ? QString::fromStdString(ExpectedRevision->token_) : QString())
                << "fresh:" << (Fresh.HasValue() && Fresh.Value().id_.revision_ ? QString::fromStdString(Fresh.Value().id_.revision_->token_) : QString())
                << "items:" << Moved.items_.size() << "committed:" << (!Moved.items_.empty() && Moved.items_.back().destinationCommitted_);
        }
        QVERIFY2(Moved.snapshot_.state_ == JobState::Completed, Moved.snapshot_.error_ ? Moved.snapshot_.error_->code_.c_str() : "move did not complete");
        QVERIFY(!QFileInfo::exists(root_->filePath("tree-copy/tree-source")));
        QCOMPARE(read("tree-move/tree-source/inner/b.txt"), QByteArray("beta"));
        Request.operation_ = FileOperation::Delete;
        Source = writer_->Inspect(at("tree-move/tree-source"), {});
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
        QVERIFY(QFileInfo::exists(root_->filePath("tree-move/tree-source/a.txt")));
        auto Prompt = *Scheduler.Snapshot(Submitted.Value().id_).Value().interaction_;
        QCOMPARE(Prompt.kind_, InteractionKind::DeleteConfirmation);
        QVERIFY(Scheduler.Reply(Submitted.Value().id_, {Prompt.id_, InteractionChoice::Confirm}).HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Deleted = Submitted.Value().completion_.get();
        QVERIFY2(Deleted.snapshot_.state_ == JobState::Completed, Deleted.snapshot_.error_ ? Deleted.snapshot_.error_->code_.c_str() : "delete did not complete");
        QVERIFY(!QFileInfo::exists(root_->filePath("tree-move/tree-source")));
        QVERIFY(artifacts().isEmpty());
    }
    void coordinatorMkdirRenameAndPreflightBudget()
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        Request.operation_ = FileOperation::MakeDirectory;
        Request.destination_ = at("coordinator-directory"); Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(write("coordinator-directory/from.txt", "rename payload"));
        auto Source = writer_->Inspect(at("coordinator-directory/from.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.operation_ = FileOperation::Rename;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("coordinator-directory/to.txt");
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("coordinator-directory/to.txt"), QByteArray("rename payload"));
        QVERIFY(!QFileInfo::exists(root_->filePath("coordinator-directory/from.txt")));
        Source = writer_->Inspect(at("coordinator-directory/to.txt"), {});
        Request.operation_ = FileOperation::Copy;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at(""); Request.maxBytes_ = 2;
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.error_->category_, ErrorCategory::BudgetExceeded);
        QVERIFY(!QFileInfo::exists(root_->filePath("to.txt")));
    }
    void coordinatorConflictRenameAndCancel()
    {
        QVERIFY(writer_->MakeDirectory(at("conflict-target"), {}).HasValue());
        QVERIFY(write("conflict-source.txt", "new contents"));
        QVERIFY(write("conflict-target/conflict-source.txt", "old contents"));
        auto Hub = std::make_shared<hc::commands::EventHub>();
        JobScheduler Scheduler(Hub, 1);
        FileOperationRequest Request;
        auto Source = writer_->Inspect(at("conflict-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = at("conflict-target");
        Request.destinationWriter_ = writer_;
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Submitted.Value().id_).Value().state_, JobState::WaitingForUser);
        const auto Prompt = *Scheduler.Snapshot(Submitted.Value().id_).Value().interaction_;
        QVERIFY(Scheduler.Reply(Submitted.Value().id_, {Prompt.id_, InteractionChoice::Rename, "chosen.txt"}).HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("conflict-target/chosen.txt"), QByteArray("new contents"));
        QCOMPARE(read("conflict-target/conflict-source.txt"), QByteArray("old contents"));

        std::atomic<bool> Cancelled{false};
        auto Subscription = Hub->Subscribe("job.progress", std::make_shared<ImmediateExecutor>(), [&](const auto& Event) {
            if (std::get<std::int64_t>(Event.payload_.at("bytes")) > 0 && !Cancelled.exchange(true)) Scheduler.Cancel(Event.causationId_);
        });
        Request.conflict_ = ConflictPolicy::Replace;
        OperationContext Context;
        Context.maxReadBytes_ = 2;
        Submitted = SubmitFileOperation(Scheduler, Request, Context);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QVERIFY(Cancelled.load());
        QCOMPARE(read("conflict-target/conflict-source.txt"), QByteArray("old contents"));
        QCOMPARE(read("conflict-source.txt"), QByteArray("new contents"));
        QVERIFY(QDir(root_->filePath("conflict-target")).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void coordinatorPreservesCommittedCopyWhenSourceDeleteFails()
    {
        QVERIFY(writer_->MakeDirectory(at("partial-target"), {}).HasValue());
        QVERIFY(write("partial-source.txt", "preserve both"));
        auto Source = writer_->Inspect(at("partial-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request;
        Request.operation_ = FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, std::make_shared<FailedRemoveWriter>(writer_)}};
        Request.destination_ = at("partial-target"); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto& Result = Submitted.Value().completion_.get();
        QCOMPARE(Result.snapshot_.state_, JobState::Failed);
        QCOMPARE(Result.snapshot_.error_->code_, std::string("injected-source-delete"));
        QVERIFY(Result.items_.at(0).destinationCommitted_);
        QVERIFY(!Result.items_.at(0).sourceRemoved_);
        QCOMPARE(read("partial-source.txt"), QByteArray("preserve both"));
        QCOMPARE(read("partial-target/partial-source.txt"), QByteArray("preserve both"));
        Request.destination_ = at("");
        QVERIFY(!SubmitFileOperation(Scheduler, Request).HasValue());
    }
    void coordinatorFaultMatrixPreservesExistingDestination()
    {
        QVERIFY(writer_->MakeDirectory(at("fault-target"), {}).HasValue());
        QVERIFY(write("fault-source.txt", "immutable original source"));
        auto Source = writer_->Inspect(at("fault-source.txt"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        for (const auto Fault : {WriteFault::Append, WriteFault::Metadata, WriteFault::Commit, WriteFault::AppendAndCleanup})
        {
            QVERIFY(write("fault-target/fault-source.txt", "existing destination"));
            FileOperationRequest Request;
            Request.sources_ = {{*Source.Value(), reader_, writer_}};
            Request.destination_ = at("fault-target");
            Request.destinationWriter_ = std::make_shared<FailedRemoveWriter>(writer_, Fault);
            Request.conflict_ = ConflictPolicy::Replace;
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
            auto Submitted = SubmitFileOperation(Scheduler, Request);
            QVERIFY(Submitted.HasValue());
            QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            const auto& Result = Submitted.Value().completion_.get();
            QCOMPARE(Result.snapshot_.state_, JobState::Failed);
            QCOMPARE(Result.cleanupErrors_.empty(), Fault != WriteFault::AppendAndCleanup);
            QCOMPARE(read("fault-source.txt"), QByteArray("immutable original source"));
            QCOMPARE(read("fault-target/fault-source.txt"), QByteArray("existing destination"));
            QVERIFY(QDir(root_->filePath("fault-target")).entryList({".hc-*"}, QDir::Files | QDir::Hidden).isEmpty());
        }
    }
    void crossProviderLocalToSmbAndBack()
    {
        if (kind_ != LocatorKind::Smb) QSKIP("Run explicitly against DEVTESTER-QTS for cross-provider validation");
        QTemporaryDir Local;
        QVERIFY(Local.isValid());
        auto LocalReader = MakeNativeReadProvider(LocatorKind::Local);
        auto LocalWriter = MakeNativeWritableStorage(LocatorKind::Local, LocalReader);
        const auto LocalAt = [&](const QString& Name) { return Locator{LocatorKind::Local, QDir::toNativeSeparators(Local.filePath(Name)).toStdString()}; };
        QByteArray Payload(65536, '\0');
        for (qsizetype Index = 0; Index < Payload.size(); ++Index) Payload[Index] = static_cast<char>(Index % 251);
        QFile SourceFile(Local.filePath("cross.bin"));
        QVERIFY(SourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(SourceFile.write(Payload), Payload.size());
        SourceFile.close();
        auto Source = LocalWriter->Inspect(LocalAt("cross.bin"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        FileOperationRequest Request;
        Request.sources_ = {{*Source.Value(), LocalReader, LocalWriter}};
        Request.destination_ = at(""); Request.destinationWriter_ = writer_;
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(read("cross.bin"), Payload);
        QVERIFY(LocalWriter->MakeDirectory(LocalAt("returned"), {}).HasValue());
        Source = writer_->Inspect(at("cross.bin"), {});
        QVERIFY(Source.HasValue() && Source.Value());
        Request.operation_ = FileOperation::Move;
        Request.sources_ = {{*Source.Value(), reader_, writer_}};
        Request.destination_ = LocalAt("returned"); Request.destinationWriter_ = LocalWriter;
        Submitted = SubmitFileOperation(Scheduler, Request);
        QVERIFY(Submitted.HasValue());
        QVERIFY(Submitted.Value().completion_.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QVERIFY(!QFileInfo::exists(root_->filePath("cross.bin")));
        QFile Returned(Local.filePath("returned/cross.bin"));
        QVERIFY(Returned.open(QIODevice::ReadOnly));
        QCOMPARE(Returned.readAll(), Payload);
    }
    void cleanupTestCase()
    {
        QVERIFY(artifacts().isEmpty());
        writer_.reset();
        reader_.reset();
        QVERIFY(root_->remove());
    }
};
QTEST_GUILESS_MAIN(WriteTests)
#include "tstWrites.moc"
