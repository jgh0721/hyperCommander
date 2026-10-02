#include <QtTest>
#include <QCryptographicHash>
#include "pluginHost/hostedDataProviders.hpp"

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    bool Put(const QString& Path, QByteArray Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    Result<std::shared_ptr<PluginProcess>> Start(QTemporaryDir& Directory, bool Write = true, bool ReadOnly = false, unsigned RequestMs = 3000, bool Batch = false)
    {
        const auto Name = ReadOnly ? "readonly.dll" : Batch ? "batch.dll" : "fixture.dll"; const auto Binary = Directory.filePath(Name);
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Binary)) return Error{ErrorCategory::Internal, "fixture-copy"};
        PluginManifest Manifest; Manifest.id_ = "hc.test.dopus"; Manifest.version_ = "1"; Manifest.binary_ = Name; Manifest.format_ = PluginFormat::DopusVfs;
        Manifest.sha256_ = QCryptographicHash::hash(Read(Binary), QCryptographicHash::Sha256).toHex().toStdString();
        Manifest.extensions_ = {Extension::StorageRead}; Manifest.permissions_ = {Permission::ReadContent};
        if (Write) { Manifest.extensions_.insert(Extension::StorageWrite); Manifest.permissions_.insert(Permission::WriteContent); }
        if (!Put(Directory.filePath("plugin.json"), EncodeManifest(Manifest))) return Error{ErrorCategory::Internal, "fixture-manifest"};
        HostLimits Limits; Limits.requestMs_ = RequestMs; PluginApproval Approval{Manifest, true, false, Manifest.permissions_};
        auto Process = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Directory.filePath("plugin.json"), Approval, Limits);
        if (!Process.HasValue()) return Process.Failure(); return std::shared_ptr<PluginProcess>(std::move(Process.Value()));
    }
}
class HostedDopusTests : public QObject
{
    Q_OBJECT
private slots:
    void customColumnsOwnTheirValues()
    {
        QTemporaryDir Package, Root, Cache; auto Process = Start(Package); QVERIFY(Process.HasValue());
        QVERIFY(Put(Root.filePath("one.txt"), "1234567890"));
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"columns"}, Store.Value(), false, Root.path()); QVERIFY(Mounted.HasValue());
        const auto Columns = Mounted.Value().reader_->Columns(); QCOMPARE(Columns.size(), 3u);
        QVERIFY(Columns[0].key_.starts_with("dopusvfs.")); QCOMPARE(Columns[0].flags_, 17u);
        auto At = Mounted.Value().root_; At.root_ += "one.txt";
        auto Entry = Mounted.Value().reader_->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        QCOMPARE(Entry.Value().hints_.at(Columns[0].valueKey_), std::string("10"));
        Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().forcedTermination_);
        QCOMPARE(Entry.Value().hints_.at(Columns[1].valueKey_), std::string("VFS value 한글"));
        QTemporaryDir Second; auto Other = Start(Second); QVERIFY(Other.HasValue());
        auto Remounted = MakeHostedDopusVfsProvider(Other.Value(), {"different-session"}, Store.Value(), false, Root.path()); QVERIFY(Remounted.HasValue());
        QCOMPARE(Remounted.Value().reader_->Columns()[0].key_, Columns[0].key_);
    }
    void invalidCustomColumnCount()
    {
        QTemporaryDir Package, Root, Cache; QVERIFY(QDir(Root.path()).mkdir("bad-column-count"));
        auto Process = Start(Package); QVERIFY(Process.HasValue()); auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"invalid-columns"}, Store.Value(), false, Root.path()); QVERIFY(Mounted.HasValue());
        auto At = Mounted.Value().root_; At.root_ += "bad-column-count";
        auto Result = Mounted.Value().reader_->Enumerate(At, {}).get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().code_, std::string("dopus-vfs-column-count"));
    }
    void batchWritesAndPerFileResults()
    {
        QTemporaryDir Package, Root, Cache; auto Process = Start(Package, true, false, 3000, true); QVERIFY(Process.HasValue());
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"batch"}, Store.Value(), true, Root.path()); QVERIFY(Mounted.HasValue());
        auto To = Mounted.Value().root_; To.root_ += "한글.txt";
        auto Write = Mounted.Value().writer_->BeginWrite(To, {}, {}); QVERIFY(Write.HasValue());
        const std::string Bytes = "batch content"; QVERIFY(Write.Value()->Append(std::as_bytes(std::span(Bytes)), {}).HasValue());
        QVERIFY(Write.Value()->Commit({}).HasValue()); QCOMPARE(Read(Root.filePath("한글.txt")), QByteArray::fromStdString(Bytes));
        auto Entry = Mounted.Value().reader_->Resolve(To, {}).get(); QVERIFY(Entry.HasValue());
        auto Input = Mounted.Value().reader_->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Input.HasValue(), Input.HasValue() ? "" : Input.Failure().code_.c_str());
        auto ReadResult = Input.Value()->Read(100, {}).get(); QVERIFY(ReadResult.HasValue()); QCOMPARE(ReadResult.Value().bytes_.size(), Bytes.size());
        QVERIFY(Mounted.Value().writer_->Remove(Entry.Value(), {}).HasValue()); QVERIFY(!QFileInfo::exists(Root.filePath("한글.txt")));
        for (const auto* Name : {"batch-each-a", "batch-each-b", "batch-partial", "batch-skip"}) QVERIFY(Put(Root.filePath(Name), "x"));
        auto Deleted = Process.Value()->DataCall(WireKind::StorageMutate, {{"action", "batch-delete"}, {"path", "\\"}, {"files", QJsonArray{"\\batch-each-a", "\\batch-each-b"}}});
        QVERIFY(Deleted.HasValue()); QCOMPARE(Deleted.Value()["processed"].toInt(), 2);
        QVERIFY(!QFileInfo::exists(Root.filePath("batch-each-a"))); QVERIFY(!QFileInfo::exists(Root.filePath("batch-each-b")));
        for (const auto* Name : {"batch-partial", "batch-skip"})
        {
            auto Failed = Process.Value()->DataCall(WireKind::StorageMutate, {{"action", "batch-delete"}, {"path", "\\"}, {"files", QJsonArray{QString("\\") + Name}}});
            QVERIFY(!Failed.HasValue()); QVERIFY(QFileInfo::exists(Root.filePath(Name)));
        }
        Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().forcedTermination_); QCOMPARE(Process.Value()->Snapshot().exitCode_.value_or(1), 0u);
    }
    void officialVfsSample()
    {
        const auto ManifestPath = qEnvironmentVariable("HC_DOPUS_VFS_MANIFEST");
        if (ManifestPath.isEmpty()) QSKIP("Set HC_DOPUS_VFS_MANIFEST and HC_DOPUS_VFS_ROOT for the separately built SDK sample");
        auto Inspected = InspectManifestFile(ManifestPath); QVERIFY(Inspected.HasValue());
        PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_};
        auto Started = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", ManifestPath, Approval);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Process = std::shared_ptr<PluginProcess>(std::move(Started.Value())); QTemporaryDir Cache;
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process, {"official"}, Store.Value(), true, qEnvironmentVariable("HC_DOPUS_VFS_ROOT"));
        QVERIFY2(Mounted.HasValue(), Mounted.HasValue() ? "" : Mounted.Failure().code_.c_str());
        std::vector<Locator> Pending{Mounted.Value().root_}; unsigned Files = 0;
        const auto Expected = qEnvironmentVariable("HC_DOPUS_VFS_EXPECTED");
        while (!Pending.empty())
        {
            auto Parent = Pending.back(); Pending.pop_back();
            auto Cursor = Mounted.Value().reader_->Enumerate(Parent, {}).get(); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
            for (;;)
            {
                auto Page = Cursor.Value()->Next(2, {}).get(); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
                for (const auto& Entry : Page.Value().entries_)
                {
                    if (Entry.directory_) { Pending.push_back(Entry.locator_); continue; }
                    QVERIFY(++Files < 1000);
                    auto Stream = Mounted.Value().reader_->OpenRead(Entry.id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
                    QByteArray Bytes;
                    for (;;) { auto Chunk = Stream.Value()->Read(65536, {}).get(); QVERIFY(Chunk.HasValue()); Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()); if (Chunk.Value().eof_) break; }
                    QCOMPARE(quint64(Bytes.size()), Entry.size_);
                    if (!Expected.isEmpty()) {
                        auto Relative = QString::fromStdString(Entry.locator_.root_.substr(Mounted.Value().root_.root_.size())); Relative.replace('\\', '/');
                        QCOMPARE(Bytes, Read(QDir(Expected).filePath(Relative)));
                    }
                }
                if (Page.Value().end_) break;
            }
        }
        QVERIFY(Files > 0); qInfo() << "Official RAR files verified:" << Files;
        auto Support = Mounted.Value().writer_->Supports(Mutation::CreateContent, Mounted.Value().root_, {}); QVERIFY(Support.HasValue()); QCOMPARE(Support.Value().availability_, Availability::Unsupported);
        Process->Close(); QVERIFY(!Process->Snapshot().forcedTermination_); QCOMPARE(Process->Snapshot().exitCode_.value_or(1), std::uint32_t(0));
    }
    void vfsReadWriteAndLifetime()
    {
        QTemporaryDir Package, Root, Cache; auto Process = Start(Package); QVERIFY2(Process.HasValue(), Process.HasValue() ? "" : Process.Failure().code_.c_str());
        QVERIFY(Put(Root.filePath("한글.txt"), "original")); QVERIFY(Put(Root.filePath("empty.txt"), {}));
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"fixture"}, Store.Value(), true, Root.path()); QVERIFY(Mounted.HasValue());
        const auto& Provider = Mounted.Value(); const auto At = [&](const char* Name) { auto Value = Provider.root_; Value.root_ += Name; return Value; };
        QCOMPARE(ParseLocator(Serialize(Provider.root_)).Value(), Provider.root_);
        {
            auto Cursor = Provider.reader_->Enumerate(Provider.root_, {}).get(); QVERIFY(Cursor.HasValue()); int Seen = 0;
            for (;;) { auto Page = Cursor.Value()->Next(1, {}).get(); QVERIFY(Page.HasValue()); Seen += int(Page.Value().entries_.size()); if (Page.Value().end_) break; }
            QCOMPARE(Seen, 2);
        }
        auto Entry = Provider.reader_->Resolve(At("한글.txt"), {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider.reader_->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Chunk.Value().bytes_.size(), 8u);
        const std::string Payload(8192, 'x');
        auto Write = Provider.writer_->BeginWrite(At("short-write.txt"), {}, {}); QVERIFY(Write.HasValue());
        QVERIFY(Write.Value()->Append(std::as_bytes(std::span(Payload)), {}).HasValue()); QVERIFY(Write.Value()->Commit({}).HasValue());
        QCOMPARE(Read(Root.filePath("short-write.txt")), QByteArray::fromStdString(Payload));
        auto Updated = Provider.writer_->BeginWrite(Entry.Value().locator_, {true, Entry.Value().id_.revision_}, {}); QVERIFY(Updated.HasValue());
        QVERIFY(Updated.Value()->Append(std::as_bytes(std::span(Payload)), {}).HasValue()); QVERIFY(Updated.Value()->Commit({}).HasValue());
        QVERIFY(!Provider.reader_->Stat(Entry.Value().id_, {}).get().HasValue());
        QVERIFY(Provider.writer_->MakeDirectory(At("nested"), {}).HasValue());
        auto Fresh = Provider.reader_->Resolve(At("short-write.txt"), {}).get(); QVERIFY(Fresh.HasValue());
        QVERIFY(Provider.writer_->Rename(Fresh.Value(), At("nested\\renamed.txt"), {}).HasValue());
        Fresh = Provider.reader_->Resolve(At("nested\\renamed.txt"), {}).get(); QVERIFY(Fresh.HasValue());
        QVERIFY(Provider.writer_->Remove(Fresh.Value(), {}).HasValue());
        for (int Index = 0; Index < 70; ++Index) { auto Early = Provider.reader_->Enumerate(Provider.root_, {}).get(); QVERIFY(Early.HasValue()); }
        Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().forcedTermination_);
        QCOMPARE(Process.Value()->Snapshot().exitCode_.value_or(1), std::uint32_t(0));
        QVERIFY(Stream.Value()->Seek(0, {}).get().HasValue()); QVERIFY(Stream.Value()->Read(20, {}).get().HasValue());
    }
    void vfsReadOnlyAndApproval_data()
    { QTest::addColumn<bool>("ReadOnly"); QTest::newRow("manifest-denial") << false; QTest::newRow("sdk-read-only") << true; }
    void vfsReadOnlyAndApproval()
    {
        QFETCH(bool, ReadOnly); QTemporaryDir Package, Root, Cache; auto Process = Start(Package, ReadOnly, ReadOnly); QVERIFY(Process.HasValue());
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"fixture"}, Store.Value(), ReadOnly, Root.path()); QVERIFY(Mounted.HasValue());
        auto Attempt = Process.Value()->DataCall(WireKind::StorageMutate, {{"action", "delete"}, {"path", "\\absent"}});
        QVERIFY(!Attempt.HasValue()); QCOMPARE(Attempt.Failure().category_, ReadOnly ? ErrorCategory::Unsupported : ErrorCategory::AccessDenied);
        auto Traversal = Process.Value()->DataCall(WireKind::StorageEnumerate, {{"action", "open"}, {"path", "\\..\\escape"}}); QVERIFY(!Traversal.HasValue());
        QVERIFY(Process.Value()->Ping().HasValue()); Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().forcedTermination_);
    }
    void vfsPartialWriteDoesNotReportCommit()
    {
        QTemporaryDir Package, Root, Cache; auto Process = Start(Package); QVERIFY(Process.HasValue());
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"fixture"}, Store.Value(), true, Root.path()); QVERIFY(Mounted.HasValue());
        auto To = Mounted.Value().root_; To.root_ += "fail-write.txt";
        auto Write = Mounted.Value().writer_->BeginWrite(To, {}, {}); QVERIFY(Write.HasValue());
        const std::string Bytes(512 * 1024, 'a'); QVERIFY(Write.Value()->Append(std::as_bytes(std::span(Bytes)), {}).HasValue());
        auto Failed = Write.Value()->Commit({}); QVERIFY(!Failed.HasValue()); QVERIFY(!Failed.Failure().retryable_);
        QCOMPARE(Failed.Failure().userAction_, std::string("inspect-remote-destination")); QVERIFY(QFileInfo::exists(Root.filePath("fail-write.txt")));
        Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().forcedTermination_);
    }
    void vfsCrashAndTimeout_data()
    { QTest::addColumn<QString>("Name"); QTest::newRow("crash") << "crash"; QTest::newRow("timeout") << "hang"; }
    void vfsCrashAndTimeout()
    {
        QFETCH(QString, Name); QTemporaryDir Package, Root, Cache; auto Process = Start(Package, false, false, 500); QVERIFY(Process.HasValue());
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Mounted = MakeHostedDopusVfsProvider(Process.Value(), {"fixture"}, Store.Value(), false, Root.path()); QVERIFY(Mounted.HasValue());
        QElapsedTimer Timer; Timer.start(); auto Failed = Process.Value()->DataCall(WireKind::StorageEnumerate, {{"action", "open"}, {"path", "\\" + Name}});
        QVERIFY(!Failed.HasValue()); QVERIFY(Timer.elapsed() < 3000); QTRY_VERIFY_WITH_TIMEOUT(!Process.Value()->Snapshot().running_, 2000);
    }
};
QTEST_GUILESS_MAIN(HostedDopusTests)
#include "tstHostedDopus.moc"
