#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/operationMetrics.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <Windows.h>
#include <atomic>
#include <thread>

using namespace hc::storage;
using namespace hc::operations;

// Folder copy/move engine (ITreeTransfer) and the coordinator around it.
namespace
{
    constexpr qint64 MiB = 1024 * 1024;
    QByteArray Payload(qint64 Size, int Seed)
    {
        QByteArray Bytes(Size, Qt::Uninitialized);
        quint32 State = 2166136261u ^ quint32(Seed);
        for (qint64 Index = 0; Index < Size; ++Index) { State = State * 16777619u + 13u; Bytes[Index] = char(State >> 24); }
        return Bytes;
    }
    std::wstring Wide(const QString& Path) { return QDir::toNativeSeparators(Path).toStdWString(); }
    FILETIME Time(qint64 UtcMs)
    {
        const auto Ticks = std::uint64_t(UtcMs + 11644473600000LL) * 10000;
        return FILETIME{DWORD(Ticks), DWORD(Ticks >> 32)};
    }
    qint64 ModifiedMs(const QString& Path)
    {
        WIN32_FILE_ATTRIBUTE_DATA Data{};
        if (!GetFileAttributesExW(Wide(Path).c_str(), GetFileExInfoStandard, &Data)) return -1;
        return qint64(((std::uint64_t(Data.ftLastWriteTime.dwHighDateTime) << 32) | Data.ftLastWriteTime.dwLowDateTime) / 10000) - 11644473600000LL;
    }
    DWORD Attributes(const QString& Path) { return GetFileAttributesW(Wide(Path).c_str()); }
    // Relative path -> content, for every file below Root.
    std::map<QString, QByteArray> Snapshot(const QString& Root)
    {
        std::map<QString, QByteArray> Files;
        QDirIterator It(Root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (It.hasNext()) {
            const auto Path = It.next(); QFile File(Path);
            Files[QDir(Root).relativeFilePath(Path)] = File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray("<unreadable>");
        }
        return Files;
    }
    QStringList Folders(const QString& Root)
    {
        QStringList Paths;
        QDirIterator It(Root, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (It.hasNext()) Paths << QDir(Root).relativeFilePath(It.next());
        Paths.sort(); return Paths;
    }
    bool StageArtifacts(const QString& Root)
    {
        QDirIterator It(Root, {".hc-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        return It.hasNext();
    }
    void ClearReadOnly(const QString& Root)
    {
        QDirIterator It(Root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (It.hasNext()) { const auto Path = Wide(It.next()); SetFileAttributesW(Path.c_str(), FILE_ATTRIBUTE_NORMAL); }
    }
    // Metric counts of one job: the engine records "transfer.*" when detailed.
    std::map<QString, double> Counts(const QString& Scratch)
    {
        std::map<QString, double> Values;
        const auto Path = QDir(Scratch).filePath("metrics.json");
        if (!OperationMetrics::Instance().Save(Path, 1)) return Values;
        QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return Values;
        const auto Metrics = QJsonDocument::fromJson(File.readAll()).object()["metrics"].toObject();
        for (auto It = Metrics.begin(); It != Metrics.end(); ++It) Values[It.key()] = It.value().toObject()["count"].toDouble();
        return Values;
    }
}

class FileTransferTests : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> root_;
    std::shared_ptr<IStorageProvider> reader_;
    std::shared_ptr<IWritableStorage> writer_, target_;

    QString path(const QString& Name) const { return root_->filePath(Name); }
    Locator at(const QString& Name) const { return {LocatorKind::Local, QDir::toNativeSeparators(path(Name)).toStdString()}; }
    bool write(const QString& Name, const QByteArray& Bytes, DWORD Attributes = 0, std::optional<qint64> ModifiedUtcMs = {})
    {
        QDir().mkpath(QFileInfo(path(Name)).absolutePath());
        QFile File(path(Name));
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size()) return false;
        File.close();
        if (ModifiedUtcMs) {
            const HANDLE Handle = CreateFileW(Wide(path(Name)).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (Handle == INVALID_HANDLE_VALUE) return false;
            const auto Value = Time(*ModifiedUtcMs);
            const bool Set = SetFileTime(Handle, nullptr, nullptr, &Value);
            CloseHandle(Handle); if (!Set) return false;
        }
        return !Attributes || SetFileAttributesW(Wide(path(Name)).c_str(), Attributes);
    }
    // A tree with nested, empty, hidden, readonly, empty-file and multi-MiB entries.
    void buildTree(const QString& Base)
    {
        for (int Index = 0; Index < 40; ++Index)
            QVERIFY(write(QStringLiteral("%1/f%2/small-%3.txt").arg(Base).arg(Index % 4).arg(Index), Payload(100 + Index * 37, Index)));
        QVERIFY(write(Base + "/f1/deep/deeper/deepest/leaf.bin", Payload(4096, 99)));
        QVERIFY(write(Base + "/large.bin", Payload(3 * MiB + 17, 7)));
        QVERIFY(write(Base + "/empty.dat", {}));
        QVERIFY(write(Base + "/hidden.txt", "hidden", FILE_ATTRIBUTE_HIDDEN, 1500000000000LL));
        QVERIFY(write(Base + "/readonly.txt", "readonly", FILE_ATTRIBUTE_READONLY, 1400000000000LL));
        QVERIFY(QDir().mkpath(path(Base + "/f2/empty-folder")));
        // Empty folders finish right after their creation is reported; the
        // ledger must still receive every one of them.
        for (int Index = 0; Index < 50; ++Index) QVERIFY(QDir().mkpath(path(QStringLiteral("%1/f3/empty/%2").arg(Base).arg(Index))));
    }
    JobResult run(FileOperationRequest Request, OperationContext Context,
        const std::function<InteractionChoice(const InteractionRequest&)>& Answer = {})
    {
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2, 64);
        auto Job = SubmitFileOperation(Scheduler, std::move(Request), Context);
        if (!Job.HasValue()) { JobResult Failed; Failed.snapshot_.state_ = JobState::Failed; Failed.snapshot_.error_ = Job.Failure(); return Failed; }
        QElapsedTimer Timer; Timer.start();
        while (Job.Value().completion_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && Timer.elapsed() < 60000) {
            const auto State = Scheduler.Snapshot(Job.Value().id_);
            if (State.HasValue() && State.Value().interaction_ && Answer) {
                const auto Query = *State.Value().interaction_;
                Scheduler.Reply(Job.Value().id_, {Query.id_, Answer(Query), {}});
            }
            QTest::qWait(5);
        }
        return Job.Value().completion_.get();
    }
    FileOperationRequest request(FileOperation Operation, const QString& Source, const QString& Destination)
    {
        FileOperationRequest Request; Request.operation_ = Operation;
        // The panel's own listing entry, as product requests carry it.
        auto Entry = reader_->Resolve(at(Source), {}).get();
        if (Entry.HasValue()) Request.sources_.push_back({Entry.Value(), reader_, writer_});
        Request.destination_ = at(Destination); Request.destinationWriter_ = target_;
        Request.fileLockInteractions_ = true;
        return Request;
    }
    static OperationContext engine(std::size_t Workers = 4)
    {
        OperationContext Context; Context.flushFileContents_ = false; Context.fileTransferWorkers_ = Workers;
        return Context;
    }
    // Drives the engine directly until it finishes.
    static Result<TreeTransferBatch> drain(ITreeTransfer& Transfer, std::vector<TreeTransferEntry>& Done)
    {
        QElapsedTimer Timer; Timer.start();
        for (;;) {
            auto Batch = Transfer.Collect(128, std::chrono::milliseconds(50));
            if (!Batch.HasValue()) return Batch;
            for (auto& Entry : Batch.Value().done_) Done.push_back(std::move(Entry));
            if (Batch.Value().finished_) return Batch;
            if (Timer.elapsed() > 60000) return Error{ErrorCategory::Unavailable, "test-timeout"};
        }
    }

private slots:
    void initTestCase()
    {
        root_ = std::make_unique<QTemporaryDir>(); QVERIFY(root_->isValid());
        reader_ = MakeNativeReadProvider(LocatorKind::Local);
        writer_ = MakeNativeWritableStorage(LocatorKind::Local, reader_);
        target_ = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
    }
    void moveMixedSelectionToParent_data()
    {
        QTest::addColumn<bool>("useSmb");
        QTest::newRow("local") << false;
        if (qEnvironmentVariableIsSet("HC_TEST_LOOPBACK_SMB")) QTest::newRow("loopback-smb") << true;
    }
    void moveMixedSelectionToParent()
    {
        QFETCH(bool, useSmb);
        const QString Base = useSmb ? QStringLiteral("parent-smb") : QStringLiteral("parent-local");
        buildTree(Base + "/current/folder");
        QVERIFY(write(Base + "/current/file.txt", "selected"));
        const auto Expected = Snapshot(path(Base + "/current/folder"));
        const auto Kind = useSmb ? LocatorKind::Smb : LocatorKind::Local;
        const auto Reader = MakeNativeReadProvider(Kind);
        const auto Writer = MakeNativeWritableStorage(Kind, Reader);
        const auto Location = [&](const QString& Name) {
            auto Path = QDir::toNativeSeparators(path(Name));
            if (useSmb) Path = QStringLiteral("\\\\localhost\\") + Path.left(1) + '$' + Path.mid(2);
            return Locator{Kind, Path.toStdString()};
        };
        FileOperationRequest Request;
        Request.operation_ = FileOperation::Move;
        Request.destination_ = Location(Base);
        Request.destinationWriter_ = Writer;
        Request.fileLockInteractions_ = true;
        for (const auto Name : {Base + "/current/folder", Base + "/current/file.txt"}) {
            auto Entry = Reader->Resolve(Location(Name), {}).get();
            QVERIFY(Entry.HasValue());
            Request.sources_.push_back({Entry.Value(), Reader, Writer});
        }
        Request.copyOptions_.streams_ = true;
        Request.calculateSizeBefore_ = true;
        OperationMetrics::Instance().Reset(true);
        const auto Result = run(std::move(Request), engine());
        QCOMPARE(Result.snapshot_.state_, JobState::Completed);
        QCOMPARE(Result.snapshot_.completedItems_, 2u);
        QCOMPARE(Counts(path(Base))["native.inspect"], 4.0);
        QCOMPARE(Snapshot(path(Base + "/folder")), Expected);
        QVERIFY(QFileInfo::exists(path(Base + "/file.txt")));
        QVERIFY(QDir(path(Base + "/current")).isEmpty());
    }
    void copiesFolderLikeItemPath()
    {
        // The engine and the per-item path produce the same tree and ledger.
        buildTree("copy/source");
        QVERIFY(QDir().mkpath(path("copy/engine"))); QVERIFY(QDir().mkpath(path("copy/items")));
        OperationMetrics::Instance().Reset(true);
        const auto Engine = run(request(FileOperation::Copy, "copy/source", "copy/engine"), engine());
        const auto Metrics = Counts(path("copy"));
        QVERIFY2(Engine.snapshot_.state_ == JobState::Completed, Engine.snapshot_.error_ ? Engine.snapshot_.error_->code_.c_str() : "copy");
        QVERIFY2(Metrics.contains("transfer.folder") && Metrics.at("transfer.folder") > 0, "the folder engine did not run");
        const auto Items = run(request(FileOperation::Copy, "copy/source", "copy/items"), engine(1));
        QCOMPARE(Items.snapshot_.state_, JobState::Completed);
        QCOMPARE(Engine.snapshot_.completedItems_, Items.snapshot_.completedItems_);
        QCOMPARE(Engine.snapshot_.failedItems_, std::uint64_t(0));
        const auto Source = Snapshot(path("copy/source"));
        QCOMPARE(Snapshot(path("copy/engine/source")), Source);
        QCOMPARE(Snapshot(path("copy/items/source")), Source);
        QCOMPARE(Folders(path("copy/engine/source")), Folders(path("copy/source")));
        QVERIFY(!StageArtifacts(path("copy/engine")));
        for (const auto& Name : {"hidden.txt", "readonly.txt", "large.bin", "f1/small-1.txt"}) {
            const auto From = path(QStringLiteral("copy/source/%1").arg(Name)), To = path(QStringLiteral("copy/engine/source/%1").arg(Name));
            QCOMPARE(ModifiedMs(To), ModifiedMs(From));
            constexpr DWORD Kept = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM;
            QCOMPARE(Attributes(To) & Kept, Attributes(From) & Kept);
        }
    }
    void engineMovesAndRemovesSources()
    {
        // Cross-volume moves use Move mode; the engine itself is volume agnostic.
        buildTree("move/source");
        const auto Expected = Snapshot(path("move/source"));
        auto Root = reader_->Resolve(at("move/source"), {}).get(); QVERIFY(Root.HasValue());
        TreeTransferOptions Options; Options.mode_ = TreeTransferMode::Move; Options.workers_ = 4; Options.allowReadOnly_ = true; Options.countFirst_ = true;
        QVERIFY(QDir().mkpath(path("move/target")));
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), at("move/target/source"), Options, {});
        QVERIFY2(Transfer.HasValue(), Transfer.HasValue() ? "" : Transfer.Failure().code_.c_str());
        std::vector<TreeTransferEntry> Done;
        auto Last = drain(*Transfer.Value(), Done); QVERIFY(Last.HasValue());
        QVERIFY(Last.Value().deferred_.empty()); QVERIFY(Last.Value().remaining_.empty());
        QCOMPARE(Snapshot(path("move/target/source")), Expected);
        QVERIFY(!QFileInfo::exists(path("move/source")));
        // Every moved file and folder is reported once, the root last.
        QCOMPARE(qsizetype(Done.size()), qsizetype(Expected.size() + Folders(path("move/target/source")).size() + 1));
        QCOMPARE(Done.back().entry_.locator_, at("move/source"));
    }
    void moveKeepsLockedFileForItemPath()
    {
        QVERIFY(write("locked/source/a/kept.txt", "kept"));
        QVERIFY(write("locked/source/a/moved.txt", "moved"));
        QVERIFY(write("locked/source/b/moved.txt", "moved"));
        // No sharing: neither copy nor removal can open it.
        const HANDLE Lock = CreateFileW(Wide(path("locked/source/a/kept.txt")).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(Lock != INVALID_HANDLE_VALUE);
        const auto Release = qScopeGuard([&] { CloseHandle(Lock); });
        auto Root = reader_->Resolve(at("locked/source"), {}).get(); QVERIFY(Root.HasValue());
        TreeTransferOptions Options; Options.mode_ = TreeTransferMode::Move; Options.workers_ = 2;
        QVERIFY(QDir().mkpath(path("locked/target")));
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), at("locked/target/source"), Options, {});
        QVERIFY(Transfer.HasValue());
        std::vector<TreeTransferEntry> Done;
        auto Last = drain(*Transfer.Value(), Done); QVERIFY(Last.HasValue());
        QCOMPARE(Last.Value().deferred_.size(), std::size_t(1));
        QCOMPARE(Last.Value().deferred_.front().entry_.locator_, at("locked/source/a/kept.txt"));
        QCOMPARE(Last.Value().deferred_.front().destination_, at("locked/target/source/a/kept.txt"));
        // Its folders stay, ancestors first and with revisions for verified removal.
        QCOMPARE(Last.Value().remaining_.size(), std::size_t(2));
        QCOMPARE(Last.Value().remaining_[0].entry_.locator_, at("locked/source"));
        QCOMPARE(Last.Value().remaining_[1].entry_.locator_, at("locked/source/a"));
        for (const auto& Kept : Last.Value().remaining_) QVERIFY(Kept.entry_.id_.revision_);
        QVERIFY(QFileInfo::exists(path("locked/source/a/kept.txt")));
        QVERIFY(!QFileInfo::exists(path("locked/target/source/a/kept.txt")));
        QVERIFY(!QFileInfo::exists(path("locked/source/b")));
        QVERIFY(QFileInfo::exists(path("locked/target/source/b/moved.txt")));
        QVERIFY(QFileInfo::exists(path("locked/target/source/a/moved.txt")));
    }
    void moveJobFinishesDeferredEntries()
    {
        // The coordinator moves what the engine returned and removes the kept
        // folders. On one volume an existing destination folder selects Merge;
        // a readonly file there needs the user's answer, a renamed folder not.
        QVERIFY(write("deferred/source/readonly.txt", "readonly", FILE_ATTRIBUTE_READONLY));
        QVERIFY(write("deferred/source/x/plain.txt", "plain"));
        QVERIFY(write("deferred/source/y.txt", "y"));
        QVERIFY(QDir().mkpath(path("deferred/target/source")));
        auto Request = request(FileOperation::Move, "deferred/source", "deferred/target");
        Request.readOnly_ = ReadOnlyPolicy::Ask;
        int Questions = 0;
        const auto Result = run(std::move(Request), engine(), [&](const InteractionRequest& Query) {
            ++Questions; return Query.kind_ == InteractionKind::ReadOnlyRemoval ? InteractionChoice::Confirm : InteractionChoice::Cancel;
        });
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "move");
        QCOMPARE(Questions, 1);
        QVERIFY(!QFileInfo::exists(path("deferred/source")));
        QCOMPARE(Snapshot(path("deferred/target/source")).size(), std::size_t(3));
        ClearReadOnly(path("deferred/target"));
    }
    void mergeSkipsConflictsAndKeepsTheirFolders()
    {
        QVERIFY(write("merge/source/a/new.txt", "new"));
        QVERIFY(write("merge/source/a/clash.txt", "incoming"));
        QVERIFY(write("merge/source/b/sub/file.txt", "file"));
        QVERIFY(write("merge/source/top.txt", "top"));
        QVERIFY(write("merge/target/source/a/clash.txt", "existing"));
        QVERIFY(QDir().mkpath(path("merge/target/source/b")));
        auto Request = request(FileOperation::Move, "merge/source", "merge/target");
        Request.conflict_ = ConflictPolicy::Skip;
        OperationMetrics::Instance().Reset(true);
        const auto Result = run(std::move(Request), engine());
        const auto Metrics = Counts(path("merge"));
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "merge");
        QVERIFY2(Metrics.contains("transfer.merge-folder"), "the merge engine did not run");
        // The skipped file and the two source folders that keep it.
        QCOMPARE(Result.snapshot_.skippedItems_, std::uint64_t(3));
        QFile Existing(path("merge/target/source/a/clash.txt")); QVERIFY(Existing.open(QIODevice::ReadOnly)); QCOMPARE(Existing.readAll(), QByteArray("existing"));
        QVERIFY(QFileInfo::exists(path("merge/source/a/clash.txt")));
        QVERIFY(!QFileInfo::exists(path("merge/source/b")));
        QVERIFY(!QFileInfo::exists(path("merge/source/top.txt")));
        QVERIFY(QFileInfo::exists(path("merge/target/source/a/new.txt")));
        QVERIFY(QFileInfo::exists(path("merge/target/source/b/sub/file.txt")));
        QVERIFY(QFileInfo::exists(path("merge/target/source/top.txt")));
    }
    void copyIntoExistingFolderKeepsItemPath()
    {
        QVERIFY(write("existing/source/same.txt", "incoming"));
        QVERIFY(write("existing/target/source/same.txt", "existing"));
        auto Request = request(FileOperation::Copy, "existing/source", "existing/target");
        Request.conflict_ = ConflictPolicy::Skip;
        OperationMetrics::Instance().Reset(true);
        const auto Result = run(std::move(Request), engine());
        const auto Metrics = Counts(path("existing"));
        QCOMPARE(Result.snapshot_.state_, JobState::Completed);
        QVERIFY(!Metrics.contains("transfer.folder"));
        QCOMPARE(Result.snapshot_.skippedItems_, std::uint64_t(1));
        QFile Existing(path("existing/target/source/same.txt")); QVERIFY(Existing.open(QIODevice::ReadOnly)); QCOMPARE(Existing.readAll(), QByteArray("existing"));
    }
    void redirectedNewFolder()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE for a redirected copy fixture.");
        QVERIFY(!Share.contains('/') && !Share.contains('\\'));
        QTemporaryDir Remote(QString("//tsclient/%1/hc-rdp-tree-XXXXXX").arg(Share));
        QVERIFY(Remote.isValid());
        const auto At = [&](const QString& Name) {
            return Locator{LocatorKind::Smb, QDir::toNativeSeparators(Remote.filePath(Name)).toStdString()};
        };
        const auto RemoteWriter = MakeNativeWritableStorage(LocatorKind::Smb, MakeNativeReadProvider(LocatorKind::Smb));
        QVERIFY(write("rdp/source/ordinary", "ordinary", FILE_ATTRIBUTE_ARCHIVE, 1450000000000LL));
        QVERIFY(write("rdp/source/first", "complete", FILE_ATTRIBUTE_HIDDEN, 1500000000000LL));
        QVERIFY(write("rdp/source/sub/last", "readonly", FILE_ATTRIBUTE_READONLY, 1400000000000LL));
        QVERIFY(QDir().mkpath(path("rdp/source/empty")));
        auto Request = request(FileOperation::Copy, "rdp/source", "");
        Request.destination_ = At(""); Request.destinationWriter_ = RemoteWriter;
        // The UI enables elevation support even when no operation needs to elevate.
        // A normal copy must still select the non-elevated folder engine.
        class AvailableElevation final : public IFileElevationService {
        public:
            bool CanExecute(const FileElevationRequest&) const override { return true; }
            Result<std::unique_ptr<IFileElevationSession>> Begin(FileElevationRequest, OperationContext) override
            { return Error{ErrorCategory::Internal, "unexpected-elevation"}; }
        };
        Request.elevation_ = std::make_shared<AvailableElevation>();
        OperationMetrics::Instance().Reset(true);
        const auto Done = run(Request, engine());
        QCOMPARE(Done.snapshot_.state_, JobState::Completed);
        QCOMPARE(Counts(path("rdp"))["transfer.file"], 1.0);
        const auto Output = Remote.filePath("source");
        QCOMPARE(Snapshot(Output), Snapshot(path("rdp/source")));
        QCOMPARE(Folders(Output), Folders(path("rdp/source")));
        for (const auto Name : {"ordinary", "first", "sub/last"}) {
            const auto Original = path("rdp/source/" + QString::fromLatin1(Name));
            const auto Target = QDir(Output).filePath(Name);
            QCOMPARE(ModifiedMs(Target), ModifiedMs(Original));
            QCOMPARE(Attributes(Target) & 0x27, Attributes(Original) & 0x27);
        }
        // An existing root never enables bulk publication for its new descendants.
        QVERIFY(write("rdp/source/new/child", "new"));
        Request.conflict_ = ConflictPolicy::Skip;
        OperationMetrics::Instance().Reset(true);
        const auto Merge = run(Request, engine(), [](const InteractionRequest&) { return InteractionChoice::Skip; });
        QCOMPARE(Merge.snapshot_.state_, JobState::Completed);
        QCOMPARE(Counts(path("rdp"))["transfer.file"], 0.0);
        ClearReadOnly(Remote.path());
        QVERIFY(Remote.remove());
    }
    void redirectedSourceMetadata()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE for a redirected read fixture.");
        QVERIFY(!Share.contains('/') && !Share.contains('\\'));
        QTemporaryDir Remote(QString("//tsclient/%1/hc-rdp-read-XXXXXX").arg(Share));
        QVERIFY(Remote.isValid());
        QFile File(Remote.filePath("child.bin"));
        QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(File.write("first"), 5);
        File.close();
        const Locator Folder{LocatorKind::Smb, QDir::toNativeSeparators(Remote.path()).toStdString()};
        const auto Reader = MakeNativeReadProvider(LocatorKind::Smb);
        auto Context = engine();
        for (const bool Fresh : {false, true}) {
            Context.freshEnumerationMetadata_ = Fresh;
            auto Cursor = Reader->Enumerate(Folder, Context).get();
            QVERIFY(Cursor.HasValue());
            const auto Page = Cursor.Value()->Next(16, Context).get();
            QVERIFY(Page.HasValue());
            QCOMPARE(Page.Value().verifiedChildMetadata_, Fresh);
            QCOMPARE(Page.Value().entries_.size(), std::size_t(1));
            if (!Fresh) continue;
            const auto& Entry = Page.Value().entries_.front();
            QVERIFY(Entry.id_.revision_ && Entry.id_.revision_->token_.starts_with("rdp:"));
            QVERIFY(File.open(QIODevice::Append));
            QCOMPARE(File.write(" again"), 6);
            File.close();
            Context.denyConcurrentMutation_ = true;
            const auto Stale = Reader->OpenRead(Entry.id_, Context).get();
            QVERIFY(!Stale.HasValue());
            QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
        }
        const auto Root = Reader->Resolve(Folder, {}).get();
        QVERIFY(Root.HasValue());
        QVERIFY(QDir().mkpath(path("rdp-input")));
        FileOperationRequest Request;
        Request.operation_ = FileOperation::Copy;
        Request.sources_.push_back({Root.Value(), Reader, MakeNativeWritableStorage(LocatorKind::Smb, Reader)});
        Request.destination_ = at("rdp-input");
        Request.destinationWriter_ = target_;
        QCOMPARE(run(std::move(Request), engine()).snapshot_.state_, JobState::Completed);
        QCOMPARE(Snapshot(path("rdp-input/" + QFileInfo(Remote.path()).fileName())).at("child.bin"), QByteArray("first again"));
    }
    void redirectedSourceReadAhead()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE for a redirected read-ahead fixture.");
        QVERIFY(!Share.contains('/') && !Share.contains('\\'));
        QTemporaryDir Remote(QString("//tsclient/%1/hc-rdp-ahead-XXXXXX").arg(Share));
        QVERIFY(Remote.isValid());
        QStringList Names;
        for (int I = 0; I < 12; ++I) {
            const auto Name = QString("entry-%1.bin").arg(I);
            QFile File(Remote.filePath(Name));
            QVERIFY(File.open(QIODevice::NewOnly | QIODevice::WriteOnly));
            QCOMPARE(File.write(Name.toUtf8()), Name.size()); File.close();
            QVERIFY(SetFileAttributesW(Wide(File.fileName()).c_str(), FILE_ATTRIBUTE_ARCHIVE));
            Names.push_back(Name);
        }
        const auto Locked = [&] {
            int Count = 0;
            for (const auto& Name : Names) {
                const auto Handle = CreateFileW(Wide(Remote.filePath(Name)).c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
                else if (GetLastError() == ERROR_SHARING_VIOLATION) ++Count;
                else return -1;
            }
            return Count;
        };
        class ReaderProbe final : public IStorageProvider {
        public:
            std::shared_ptr<IStorageProvider> inner_ = MakeNativeReadProvider(LocatorKind::Smb);
            std::atomic<int> early_{0}, normal_{0};
            std::function<void(int)> opened_;
            std::function<void()> openingLarge_;
            std::atomic<bool> failNext_{false};
            ProviderId Id() const override { return inner_->Id(); }
            bool SupportsParallelReads() const override { return true; }
            Task<SupportDecision> Supports(Operation Op, OperationContext C) override { return inner_->Supports(Op, C); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator L, OperationContext C) override { return inner_->Enumerate(L, C); }
            Task<EntrySnapshot> Stat(ResourceId R, OperationContext C) override { return inner_->Stat(R, C); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId R, OperationContext C) override {
                if (C.workerRead_) {
                    ++normal_;
                    if (openingLarge_ && C.expectedWriteBytes_.value_or(0) > 16 * 1024 * 1024) openingLarge_();
                    return inner_->OpenRead(R, C);
                }
                if (failNext_.exchange(false)) return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::Unavailable, "injected-open"});
                auto Opened = inner_->OpenRead(R, C).get();
                if (Opened.HasValue()) { const auto N = ++early_; if (opened_) opened_(N); }
                return Ready<std::shared_ptr<IReadStream>>(std::move(Opened));
            }
        };
        const auto Reader = std::make_shared<ReaderProbe>();
        const auto Root = Reader->inner_->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(Remote.path()).toStdString()}, {}).get();
        QVERIFY(Root.HasValue());
        auto MakeRequest = [&](QString Destination) {
            FileOperationRequest Request;
            Request.operation_ = FileOperation::Copy; Request.fileLockInteractions_ = true;
            Request.sources_.push_back({Root.Value(), Reader, MakeNativeWritableStorage(LocatorKind::Smb, Reader->inner_)});
            Request.destination_ = at(Destination); Request.destinationWriter_ = target_; Request.destinationName_ = "new";
            return Request;
        };
        auto Context = engine(); Context.fileCopyWorkers_ = 0;
        for (const auto& Dest : {"ahead", "ahead-conflict", "ahead-stop"}) QVERIFY(QDir().mkpath(path(Dest)));
        int Peak = 0;
        Reader->opened_ = [&](int N) { if (N == 8) Peak = Locked(); };
        QCOMPARE(run(MakeRequest("ahead"), Context).snapshot_.state_, JobState::Completed);
        QCOMPARE(Peak, 8); QCOMPARE(Reader->early_.load(), 12); QCOMPARE(Reader->normal_.load(), 0);
        QCOMPARE(Snapshot(path("ahead/new")), Snapshot(Remote.path())); QCOMPARE(Locked(), 0);
        Reader->opened_ = {}; Reader->early_ = 0; Reader->normal_ = 0;
        auto Merge = MakeRequest("ahead"); Merge.conflict_ = ConflictPolicy::Replace;
        QCOMPARE(run(std::move(Merge), Context).snapshot_.state_, JobState::Completed);
        QCOMPARE(Reader->early_.load(), 0); QCOMPARE(Reader->normal_.load(), 12);
        std::atomic<bool> CompetitorsReady{true};
        Reader->opened_ = [&](int N) {
            if (N == 1) for (const auto& Name : Names) if (!write("ahead-conflict/new/" + Name, "competitor")) CompetitorsReady = false;
        };
        int AtQuestion = -1;
        const auto Conflict = run(MakeRequest("ahead-conflict"), Context, [&](const InteractionRequest&) {
            AtQuestion = Locked(); return InteractionChoice::SkipAll;
        });
        QVERIFY(CompetitorsReady.load()); QCOMPARE(Conflict.snapshot_.state_, JobState::Completed); QCOMPARE(AtQuestion, 0);
        for (const auto& [Name, Bytes] : Snapshot(path("ahead-conflict/new"))) QCOMPARE(Bytes, QByteArray("competitor"));
        Reader->early_ = 0;
        std::atomic<bool> Held{false}, Continue{false};
        Reader->opened_ = [&](int N) {
            if (N != 8) return;
            Held = true;
            QElapsedTimer Wait; Wait.start();
            while (!Continue && Wait.elapsed() < 15000) QThread::msleep(2);
        };
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 2, 64);
        const auto Job = SubmitFileOperation(Scheduler, MakeRequest("ahead-stop"), Context); QVERIFY(Job.HasValue());
        QElapsedTimer Wait; Wait.start();
        while (!Held && Wait.elapsed() < 15000) QTest::qSleep(2);
        const bool WasHeld = Held;
        const auto Cancelled = Scheduler.Cancel(Job.Value().id_); Continue = true;
        QVERIFY(WasHeld); QVERIFY(Cancelled.HasValue());
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QCOMPARE(Locked(), 0); QVERIFY(!StageArtifacts(path("ahead-stop")));

        QVERIFY(QDir().mkpath(path("ahead-pause")));
        Held = false; Continue = false; Reader->early_ = 0;
        const auto Paused = SubmitFileOperation(Scheduler, MakeRequest("ahead-pause"), Context); QVERIFY(Paused.HasValue());
        Wait.restart(); while (!Held && Wait.elapsed() < 15000) QTest::qSleep(2);
        const bool ReachedWindow = Held;
        const auto Pause = Scheduler.Pause(Paused.Value().id_, true); Continue = true;
        QVERIFY(ReachedWindow); QVERIFY(Pause.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Paused.Value().id_).Value().state_ == JobState::Paused, 15000);
        QCOMPARE(Locked(), 0);
        QVERIFY(Scheduler.Pause(Paused.Value().id_, false).HasValue());
        QCOMPARE(Paused.Value().completion_.get().snapshot_.state_, JobState::Completed);
        QCOMPARE(Snapshot(path("ahead-pause/new")), Snapshot(Remote.path()));
        Reader->opened_ = {}; Reader->early_ = 0; Reader->normal_ = 0; Reader->failNext_ = true;
        QVERIFY(QDir().mkpath(path("ahead-reopen")));
        QCOMPARE(run(MakeRequest("ahead-reopen"), Context).snapshot_.state_, JobState::Completed);
        QCOMPARE(Reader->normal_.load(), 1); QCOMPARE(Snapshot(path("ahead-reopen/new")), Snapshot(Remote.path()));

        QFile Large(Remote.filePath("000-large.bin")); QVERIFY(Large.open(QIODevice::NewOnly | QIODevice::WriteOnly));
        QVERIFY(Large.resize(16 * 1024 * 1024 + 1)); Large.close();
        int LockedBehindLarge = -1;
        Reader->openingLarge_ = [&] { LockedBehindLarge = Locked(); };
        QVERIFY(QDir().mkpath(path("ahead-large")));
        QCOMPARE(run(MakeRequest("ahead-large"), Context).snapshot_.state_, JobState::Completed);
        QCOMPARE(LockedBehindLarge, 0); QCOMPARE(Snapshot(path("ahead-large/new")), Snapshot(Remote.path()));
        Reader->openingLarge_ = {}; QVERIFY(Large.remove());

        // Two selections with the same basename share a destination. Only the
        // selection that created it may acquire its source handles early.
        Reader->opened_ = {}; Reader->early_ = 0; Reader->normal_ = 0;
        QVERIFY(QDir().mkpath(path("ahead-twins")));
        auto Twins = MakeRequest("ahead-twins"); Twins.sources_.clear(); Twins.destinationName_.clear();
        Twins.conflict_ = ConflictPolicy::Replace;
        for (const QString Name : {QStringLiteral("first"), QStringLiteral("second")}) {
            const auto Dir = Remote.filePath(Name + "/same");
            QVERIFY(QDir().mkpath(Dir));
            QFile File(Dir + "/" + Name); QVERIFY(File.open(QIODevice::NewOnly | QIODevice::WriteOnly));
            QCOMPARE(File.write(Name.toUtf8()), Name.size()); File.close();
            QVERIFY(SetFileAttributesW(Wide(File.fileName()).c_str(), FILE_ATTRIBUTE_ARCHIVE));
            const auto Entry = Reader->inner_->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(Dir).toStdString()}, {}).get();
            QVERIFY(Entry.HasValue());
            Twins.sources_.push_back({Entry.Value(), Reader, MakeNativeWritableStorage(LocatorKind::Smb, Reader->inner_)});
        }
        QCOMPARE(run(std::move(Twins), Context).snapshot_.state_, JobState::Completed);
        QCOMPARE(Reader->early_.load(), 1); QCOMPARE(Reader->normal_.load(), 1);
        const auto Copied = Snapshot(path("ahead-twins/same"));
        QCOMPARE(Copied.size(), 2); QCOMPARE(Copied.at("first"), QByteArray("first")); QCOMPARE(Copied.at("second"), QByteArray("second"));
        QVERIFY(Remote.remove());
    }
    void redirectedConcurrentName()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE for a redirected collision fixture.");
        QVERIFY(!Share.contains('/') && !Share.contains('\\'));
        QTemporaryDir Remote(QString("//tsclient/%1/hc-rdp-race-XXXXXX").arg(Share));
        QVERIFY(Remote.isValid());
        QVERIFY(write("rdp-race/source/ordinary", "copy"));
        QVERIFY(write("rdp-race/source/collision", "from source"));
        const auto Root = reader_->Resolve(at("rdp-race/source"), {}).get(); QVERIFY(Root.HasValue());
        const Locator Target{LocatorKind::Smb, QDir::toNativeSeparators(Remote.filePath("source")).toStdString()};
        TreeTransferOptions Options; Options.workers_ = 1;
        auto Context = engine();
        const auto RemoteReader = MakeNativeReadProvider(LocatorKind::Smb);
        const auto RemoteWriter = MakeNativeWritableStorage(LocatorKind::Smb, RemoteReader);
        const auto RemoteRoot = RemoteReader->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(Remote.path()).toStdString()}, {}).get();
        QVERIFY(RemoteRoot.HasValue());
        const auto Unsupported = RemoteWriter->BeginTreeTransfer(RemoteRoot.Value(), Target, Options, Context);
        QVERIFY(!Unsupported.HasValue());
        QCOMPARE(Unsupported.Failure().code_, std::string("tree-transfer-source"));
        Context.flushFileContents_ = true;
        QVERIFY(!writer_->BeginTreeTransfer(Root.Value(), Target, Options, Context).HasValue());
        QVERIFY(!QFileInfo::exists(Remote.filePath("source")));
        Context.flushFileContents_ = false;
        std::atomic<bool> Paused{true};
        Context.pauseRequested_ = [&] { return Paused.load(); };
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), Target, Options, Context); QVERIFY(Transfer.HasValue());
        QFile Competing(Remote.filePath("source/collision"));
        QVERIFY(Competing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(Competing.write("keep me"), 7); Competing.close();
        Paused = false;
        std::vector<TreeTransferEntry> Done;
        const auto Last = drain(*Transfer.Value(), Done); QVERIFY(Last.HasValue());
        QCOMPARE(Last.Value().deferred_.size(), std::size_t(1));
        QCOMPARE(Last.Value().deferred_.front().entry_.name_, std::string("collision"));
        Transfer.Value()->Release();
        const auto Files = Snapshot(Remote.filePath("source"));
        QCOMPARE(Files.at("collision"), QByteArray("keep me"));
        QCOMPARE(Files.at("ordinary"), QByteArray("copy"));
        QVERIFY(Remote.remove());
    }
    void redirectedCancelCopy()
    {
        const auto Share = qEnvironmentVariable("HC_TEST_RDP_SHARE");
        if (Share.isEmpty()) QSKIP("Set HC_TEST_RDP_SHARE for a redirected cancellation fixture.");
        QVERIFY(!Share.contains('/') && !Share.contains('\\'));
        QTemporaryDir Remote(QString("//tsclient/%1/hc-rdp-cancel-XXXXXX").arg(Share));
        QVERIFY(Remote.isValid());
        QVERIFY(write("rdp-stop/source/first", "completed"));
        QVERIFY(write("rdp-stop/source/later/active.bin", Payload(32 * MiB, 42)));
        const auto Root = reader_->Resolve(at("rdp-stop/source"), {}).get(); QVERIFY(Root.HasValue());
        const Locator Target{LocatorKind::Smb, QDir::toNativeSeparators(Remote.filePath("source")).toStdString()};
        TreeTransferOptions Options; Options.workers_ = 1;
        const auto Active = Wide(Remote.filePath("source/later/active.bin"));
        std::atomic<bool> WriterPaused{false};
        auto Context = engine(); std::stop_source Stop; Context.stopToken_ = Stop.get_token();
        // Pause inside the copy while its destination writer is still open.
        // A remote name lookup from the test thread can arrive after completion.
        Context.pauseRequested_ = [&] {
            if (WriterPaused.load()) return true;
            const auto Probe = CreateFileW(Active.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (Probe != INVALID_HANDLE_VALUE) CloseHandle(Probe);
            else if (GetLastError() == ERROR_SHARING_VIOLATION) WriterPaused = true;
            return WriterPaused.load();
        };
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), Target, Options, Context);
        QVERIFY(Transfer.HasValue());
        QElapsedTimer Timer; Timer.start();
        while (!WriterPaused.load() && Timer.elapsed() < 30000) QTest::qSleep(2);
        QVERIFY(WriterPaused.load());
        Stop.request_stop(); Transfer.Value()->Release();
        const auto Remaining = CreateFileW(Active.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const auto OpenError = GetLastError();
        if (Remaining != INVALID_HANDLE_VALUE) CloseHandle(Remaining);
        QVERIFY2(Remaining == INVALID_HANDLE_VALUE, "Destination file survived cancellation of its open writer");
        QCOMPARE(OpenError, DWORD(ERROR_FILE_NOT_FOUND));
        QFile First(Remote.filePath("source/first")); QVERIFY(First.open(QIODevice::ReadOnly));
        QCOMPARE(First.readAll(), QByteArray("completed")); First.close();
        QVERIFY(!StageArtifacts(Remote.path()));
        QVERIFY(Remote.remove());
    }
    void stopLeavesNoIncompleteFile()
    {
        // Cancelling removes only the files being copied; completed ones stay.
        for (int Index = 0; Index < 6; ++Index) QVERIFY(write(QStringLiteral("stop/source/part-%1.bin").arg(Index), Payload(48 * MiB, Index)));
        for (int Index = 0; Index < 200; ++Index) QVERIFY(write(QStringLiteral("stop/source/small/%1.txt").arg(Index), Payload(500, Index)));
        auto Root = reader_->Resolve(at("stop/source"), {}).get(); QVERIFY(Root.HasValue());
        QVERIFY(QDir().mkpath(path("stop/target")));
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        TreeTransferOptions Options; Options.workers_ = 4;
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), at("stop/target/source"), Options, Context);
        QVERIFY(Transfer.HasValue());
        QElapsedTimer Timer; Timer.start();
        std::vector<TreeTransferEntry> Done;
        while (Timer.elapsed() < 30000) {
            auto Batch = Transfer.Value()->Collect(16, std::chrono::milliseconds(5));
            QVERIFY(Batch.HasValue());
            for (auto& Entry : Batch.Value().done_) Done.push_back(std::move(Entry));
            if (Batch.Value().finished_ || (Batch.Value().bytes_ > 20 * MiB && Done.size() > 2)) break;
        }
        Stop.request_stop(); Transfer.Value()->Release();
        if (auto Rest = Transfer.Value()->Collect(1u << 20, std::chrono::milliseconds(0)); Rest.HasValue())
            for (auto& Entry : Rest.Value().done_) Done.push_back(std::move(Entry));
        const auto Source = Snapshot(path("stop/source")), Copied = Snapshot(path("stop/target/source"));
        std::size_t Files = 0;
        for (const auto& [Name, Bytes] : Copied) {
            QVERIFY2(Source.contains(Name) && Source.at(Name) == Bytes, qPrintable("incomplete copy left: " + Name));
            ++Files;
        }
        std::size_t ReportedFiles = 0;
        for (const auto& Entry : Done) if (!Entry.entry_.directory_) ++ReportedFiles;
        QCOMPARE(Files, ReportedFiles);
        QVERIFY(!StageArtifacts(path("stop/target")));
    }
    void doesNotFollowLinks()
    {
        QVERIFY(write("links/outside/secret.txt", "secret"));
        QVERIFY(write("links/source/plain.txt", "plain"));
        QProcess Link; Link.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(path("links/source/junction")), QDir::toNativeSeparators(path("links/outside"))});
        QVERIFY(Link.waitForFinished(10000)); QCOMPARE(Link.exitCode(), 0);
        auto Root = reader_->Resolve(at("links/source"), {}).get(); QVERIFY(Root.HasValue());
        QVERIFY(QDir().mkpath(path("links/target")));
        TreeTransferOptions Options; Options.workers_ = 2;
        auto Transfer = writer_->BeginTreeTransfer(Root.Value(), at("links/target/source"), Options, {});
        QVERIFY(Transfer.HasValue());
        std::vector<TreeTransferEntry> Done;
        auto Last = drain(*Transfer.Value(), Done); QVERIFY(Last.HasValue());
        QCOMPARE(Last.Value().deferred_.size(), std::size_t(1));
        QCOMPARE(Last.Value().deferred_.front().entry_.locator_, at("links/source/junction"));
        QVERIFY(!QFileInfo::exists(path("links/target/source/junction")));
        QVERIFY(QFileInfo::exists(path("links/target/source/plain.txt")));
        QVERIFY(RemoveDirectoryW(Wide(path("links/source/junction")).c_str()));
    }
    void schedulerHonoursShortDeferrals()
    {
        // A cooperative 1 ms delay must not wait for the 25 ms poll interval.
        class Deferring final : public IJobTask
        {
        public:
            JobStep Advance(JobStepContext) override
            {
                JobStep Step{++steps_ < 60 ? JobState::Running : JobState::Completed};
                if (Step.next_ == JobState::Running) Step.deferFor_ = std::chrono::milliseconds(1);
                return Step;
            }
            std::vector<Error> Finalize(JobState) override { return {}; }
            int steps_ = 0;
        };
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 16);
        QElapsedTimer Timer; Timer.start();
        JobRequest Request; Request.resourceLanes_ = {"deferral-test"};
        auto Job = Scheduler.Submit(Request, std::make_unique<Deferring>()); QVERIFY(Job.HasValue());
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Completed);
        // The 25 ms poll cost 25-31 ms per step before the change. A timed wait
        // still rounds up to the 15.6 ms default timer resolution.
        QVERIFY2(Timer.elapsed() < 60 * 22, qPrintable(QString::number(Timer.elapsed())));
    }
    void cleanupTestCase()
    {
        ClearReadOnly(root_->path());
        writer_.reset(); target_.reset(); reader_.reset();
        QVERIFY(root_->remove());
    }
};
QTEST_GUILESS_MAIN(FileTransferTests)
#include "tstFileTransfer.moc"
