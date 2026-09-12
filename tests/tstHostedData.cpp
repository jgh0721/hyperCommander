#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include "hostedDataProviders.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/qtJobJournal.hpp"

using namespace hc::plugins;
using namespace hc::storage;
using namespace hc::operations;
class HostedDataTests : public QObject
{
    Q_OBJECT
private:
    Result<std::shared_ptr<PluginProcess>> start(QTemporaryDir& Directory, PluginFormat Format, bool Write = false, bool Approve = true, HostLimits Limits = {})
    {
        const auto Binary = Directory.filePath("data.dll");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_tc_data_fixture.dll", Binary)) return Error{ErrorCategory::Internal, "fixture-copy"};
        QFile File(Binary); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Internal, "fixture-open"};
        PluginManifest Manifest; Manifest.id_ = "hc.test.data"; Manifest.version_ = "1.0"; Manifest.binary_ = "data.dll"; Manifest.format_ = Format;
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Manifest.extensions_ = Format == PluginFormat::Wfx ? std::set{Extension::StorageRead} : std::set{Extension::Metadata};
        Manifest.permissions_ = {Permission::ReadContent}; if (Write) { Manifest.extensions_.insert(Extension::StorageWrite); Manifest.permissions_.insert(Permission::WriteContent); }
        PluginApproval Approval{Manifest, Approve, false, Manifest.permissions_};
        QFile Json(Directory.filePath("plugin.json")); if (!Json.open(QIODevice::WriteOnly)) return Error{ErrorCategory::Internal, "fixture-manifest"}; Json.write(EncodeManifest(Manifest)); Json.close();
        auto Process = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Json.fileName(), Approval, Limits, {}, Directory.filePath("plugin.ini"));
        if (!Process.HasValue()) return Process.Failure(); return std::shared_ptr<PluginProcess>(std::move(Process.Value()));
    }
private slots:
    void compatibilityPackage()
    {
        const auto ManifestPath = qEnvironmentVariable("HC_DATA_MANIFEST");
        if (ManifestPath.isEmpty()) QSKIP("Set HC_DATA_MANIFEST for an explicitly selected official SDK sample");
        auto Inspected = InspectManifestFile(ManifestPath); QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        PluginApproval Approval{Inspected.Value().manifest_, true, false, Inspected.Value().manifest_.permissions_}; QTemporaryDir Directory, Cache;
        auto Started = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", ManifestPath, Approval, {}, {}, Directory.filePath("sample.ini"));
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Process = std::shared_ptr<PluginProcess>(std::move(Started.Value())); auto Probe = Process->Probe(); QVERIFY(Probe.HasValue()); qInfo() << Probe.Value();
        const auto FilePath = Directory.filePath("source.txt"); QFile File(FilePath); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("official SDK sample content"); File.close();
        if (Approval.reviewed_.format_ == PluginFormat::Wfx)
        {
            auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
            auto Provider = MakeHostedWfxProvider(Process, {"official"}, Store.Value(), true); QVERIFY(Provider.HasValue());
            auto RootCursor = Provider.Value().reader_->Enumerate(Provider.Value().root_, {}).get(); QVERIFY(RootCursor.HasValue());
            auto Roots = RootCursor.Value()->Next(256, {}).get(); QVERIFY(Roots.HasValue()); QVERIFY(!Roots.Value().entries_.empty());
            auto At = Provider.Value().root_; At.root_ += QDir::toNativeSeparators(FilePath).toStdString();
            auto Entry = Provider.Value().reader_->Resolve(At, {}).get(); QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str());
            auto Stream = Provider.Value().reader_->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
            auto Read = Stream.Value()->Read(1024, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Read.Value().bytes_.size(), 27u);
            auto Target = Provider.Value().root_; Target.root_ += QDir::toNativeSeparators(Directory.filePath("written.txt")).toStdString();
            auto Write = Provider.Value().writer_->BeginWrite(Target, {}, {}); QVERIFY(Write.HasValue());
            const std::string Data = "official WFX write"; QVERIFY(Write.Value()->Append(std::as_bytes(std::span(Data)), {}).HasValue());
            auto Commit = Write.Value()->Commit({}); QVERIFY2(Commit.HasValue(), Commit.HasValue() ? "" : Commit.Failure().code_.c_str());
            QVERIFY(Write.Value()->Abort().HasValue()); QVERIFY(Write.Value()->Abort().HasValue());
            auto Written = Provider.Value().reader_->Resolve(Target, {}).get(); QVERIFY(Written.HasValue()); QCOMPARE(Written.Value().size_, Data.size());
            QVERIFY(Provider.Value().writer_->Remove(Written.Value(), {}).HasValue()); QVERIFY(!QFileInfo::exists(Directory.filePath("written.txt")));
        }
        else
        {
            auto Materializer = MakeNativeMaterializer(Cache.path().toStdString()); QVERIFY(Materializer.HasValue());
            auto Provider = MakeHostedWdxProvider(Process, Materializer.Value()); QVERIFY(Provider.HasValue());
            auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Reader->Resolve({LocatorKind::Local, FilePath.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
            auto Source = std::make_shared<ProviderContentSource>(Reader, Entry.Value());
            auto Fields = Provider.Value()->Fields({}).get(); QVERIFY2(Fields.HasValue(), Fields.HasValue() ? "" : Fields.Failure().code_.c_str()); QVERIFY(!Fields.Value().empty());
            unsigned Values = 0;
            for (const auto& Field : Fields.Value())
            {
                const auto Value = Provider.Value()->Query(Source, {Field.index_, 0, true}, {}).get();
                // The official RAW+JPG field deliberately reports ft_fileerror
                // for a text fixture. Other field errors remain failures.
                if (Field.index_ == 25 && !Value.HasValue())
                { QCOMPARE(Value.Failure().code_, std::string("host-wdx-value--2")); qInfo() << "RAW+JPG: expected ft_fileerror for text"; continue; }
                QVERIFY2(Value.HasValue(), Value.HasValue() ? "" : Value.Failure().code_.c_str());
                if (Value.Value().status_ == MetadataStatus::Value) ++Values;
                if (Field.index_ == 0) QCOMPARE(std::get<std::string>(Value.Value().value_), std::string("source.txt"));
                qInfo() << QString::fromStdString(Field.name_) << "status" << int(Value.Value().status_);
            }
            QVERIFY(Values > 0);
        }
        Process->Close(); QVERIFY(!Process->Snapshot().forcedTermination_);
    }
    void journalRecoveryAndHostFailure()
    {
        QTemporaryDir Directory, History; auto Process = start(Directory, PluginFormat::Wfx); QVERIFY(Process.HasValue());
        auto Journal = MakeFileJobJournal(History.path());
        class FaultTask final : public IJobTask
        {
        public:
            explicit FaultTask(std::shared_ptr<PluginProcess> Process) : process_(std::move(Process)) {}
            JobStep Advance(JobStepContext Context) override
            {
                auto Reply = process_->DataCall(WireKind::StorageEnumerate, {{"action", "open"}, {"path", "\\crash"}}, Context.operation_);
                JobStep Step{JobState::Failed}; Step.error_ = Reply.HasValue() ? Error{ErrorCategory::Internal, "expected-crash"} : Reply.Failure(); return Step;
            }
            std::vector<Error> Finalize(JobState) override { process_->Close(); return {}; }
        private:
            std::shared_ptr<PluginProcess> process_;
        };
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 16, Journal);
        auto Submitted = Scheduler.Submit({}, std::make_unique<FaultTask>(Process.Value())); QVERIFY(Submitted.HasValue());
        auto Done = Submitted.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Failed); QVERIFY(!Process.Value()->Snapshot().running_);
        auto Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue()); QCOMPARE(Recovered.Value().size(), 1u);
        QCOMPARE(Recovered.Value()[0].snapshot_.error_->code_, Done.snapshot_.error_->code_);
        JobResult Interrupted; Interrupted.snapshot_.id_ = "hc-job-interrupted"; Interrupted.snapshot_.state_ = JobState::Committing;
        Interrupted.items_.push_back({"destination", ItemOutcome::Status::Completed, {}, true, false});
        QVERIFY(Journal->Checkpoint(Interrupted).HasValue()); Recovered = Journal->Recover(); QVERIFY(Recovered.HasValue());
        auto Found = std::ranges::find_if(Recovered.Value(), [](const auto& Item) { return Item.snapshot_.id_ == "hc-job-interrupted"; }); QVERIFY(Found != Recovered.Value().end());
        QCOMPARE(Found->snapshot_.error_->code_, std::string("job-interrupted")); QVERIFY(Found->items_[0].destinationCommitted_); QVERIFY(!Found->items_[0].sourceRemoved_);
        QVERIFY(Journal->Forget("hc-job-interrupted").HasValue()); QCOMPARE(Journal->Recover().Value().size(), 1u);
        QFile Bad(History.filePath("hc-job-bad.json")); QVERIFY(Bad.open(QIODevice::WriteOnly)); Bad.write("{"); Bad.close(); QVERIFY(!Journal->Recover().HasValue());
    }
    void journalFailurePreventsExecution()
    {
        QTemporaryDir Directory; QFile Block(Directory.filePath("file")); QVERIFY(Block.open(QIODevice::WriteOnly)); Block.close();
        auto Journal = MakeFileJobJournal(Block.fileName()); bool Executed = false;
        class Task final : public IJobTask
        {
        public:
            explicit Task(bool& Executed) : executed_(Executed) {}
            JobStep Advance(JobStepContext) override { executed_ = true; return {JobState::Completed}; }
            std::vector<Error> Finalize(JobState) override { return {}; }
        private:
            bool& executed_;
        };
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1, 16, Journal);
        auto Submitted = Scheduler.Submit({}, std::make_unique<Task>(Executed)); QVERIFY(Submitted.HasValue());
        QCOMPARE(Submitted.Value().completion_.get().snapshot_.state_, JobState::Failed); QVERIFY(!Executed);
    }
    void dependencyPinning()
    {
        QTemporaryDir Directory; auto Initial = start(Directory, PluginFormat::Wfx); QVERIFY(Initial.HasValue()); Initial.Value()->Close();
        QFile Json(Directory.filePath("plugin.json")); QVERIFY(Json.open(QIODevice::ReadOnly)); auto Parsed = ParseManifest(Json.readAll()); Json.close(); QVERIFY(Parsed.HasValue());
        const auto Dependency = Directory.filePath("helper.dll"); QVERIFY(QFile::copy(Directory.filePath("data.dll"), Dependency));
        QCOMPARE(InspectManifestFile(Json.fileName()).Failure().code_, std::string("plugin-unreviewed-adjacent-dll"));
        Parsed.Value().dependencies_["helper.dll"] = Parsed.Value().sha256_; QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Parsed.Value())); Json.close();
        PluginApproval Approval{Parsed.Value(), true, false, Parsed.Value().permissions_};
        auto Process = PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Json.fileName(), Approval); QVERIFY2(Process.HasValue(), Process.HasValue() ? "" : Process.Failure().code_.c_str());
        QFile Change(Dependency); QVERIFY(!Change.open(QIODevice::WriteOnly)); Process.Value()->Close();
        QVERIFY(Change.open(QIODevice::Append)); Change.write("changed"); Change.close();
        QCOMPARE(InspectManifestFile(Json.fileName()).Failure().code_, std::string("plugin-binary-hash-mismatch"));
        Parsed.Value().dependencies_["../outside.dll"] = Parsed.Value().sha256_; QVERIFY(!Validate(Parsed.Value()).HasValue());
    }
    void storageReadWrite()
    {
        QTemporaryDir Directory, Cache; auto Process = start(Directory, PluginFormat::Wfx, true); QVERIFY2(Process.HasValue(), Process.HasValue() ? "" : Process.Failure().code_.c_str());
        auto Store = MakeNativeContentStore(Cache.path().toStdString()); QVERIFY(Store.HasValue());
        auto Provider = MakeHostedWfxProvider(Process.Value(), {"fixture"}, Store.Value(), true); QVERIFY(Provider.HasValue());
        const auto Root = Provider.Value().root_; QCOMPARE(ParseLocator(Serialize(Root)).Value(), Root);
        auto Cursor = Provider.Value().reader_->Enumerate(Root, {}).get(); QVERIFY(Cursor.HasValue());
        std::vector<EntrySnapshot> Entries;
        for (;;) { auto Page = Cursor.Value()->Next(1, {}).get(); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.size() <= 1); for (auto& Item : Page.Value().entries_) Entries.push_back(Item); if (Page.Value().end_) break; }
        QCOMPARE(Entries.size(), 3u);
        auto At = Root; At.root_ += "hello.txt"; auto Entry = Provider.Value().reader_->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider.Value().reader_->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Read = Stream.Value()->Read(5, {}).get(); QVERIFY(Read.HasValue()); QCOMPARE(Read.Value().bytes_.size(), 5u); QVERIFY(!Read.Value().eof_);
        QVERIFY(Stream.Value()->Seek(0, {}).get().HasValue()); Read = Stream.Value()->Read(1024, {}).get(); QVERIFY(Read.HasValue()); QVERIFY(Read.Value().eof_);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Read.Value().bytes_.data()), Read.Value().bytes_.size()), QByteArray("hello from WFX"));
        auto New = Root; New.root_ += "new.txt"; auto Session = Provider.Value().writer_->BeginWrite(New, {}, {}); QVERIFY(Session.HasValue());
        const std::string Bytes = "written"; QVERIFY(Session.Value()->Append(std::as_bytes(std::span(Bytes)), {}).HasValue()); auto Committed = Session.Value()->Commit({}); QVERIFY2(Committed.HasValue(), Committed.HasValue() ? "" : Committed.Failure().code_.c_str());
        auto Written = Provider.Value().reader_->Resolve(New, {}).get(); QVERIFY(Written.HasValue()); QCOMPARE(Written.Value().size_, 7u);
        auto To = Root; To.root_ += "renamed.txt"; QVERIFY(Provider.Value().writer_->Rename(Written.Value(), To, {}).HasValue());
        auto Renamed = Provider.Value().reader_->Resolve(To, {}).get(); QVERIFY(Renamed.HasValue()); QVERIFY(Provider.Value().writer_->Remove(Renamed.Value(), {}).HasValue());
        QCOMPARE(Provider.Value().reader_->Resolve(To, {}).get().Failure().category_, ErrorCategory::NotFound);
        At.root_ = Root.root_ + "..\\outside"; QVERIFY(!Provider.Value().reader_->Resolve(At, {}).get().HasValue());
        auto Stale = Entry.Value().id_; Stale.revision_ = ResourceRevision{"changed"}; QCOMPARE(Provider.Value().reader_->OpenRead(Stale, {}).get().Failure().category_, ErrorCategory::StaleRevision);
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token(); QCOMPARE(Cursor.Value()->Next(1, Context).get().Failure().category_, ErrorCategory::Cancelled);
    }
    void permissions()
    {
        QTemporaryDir Denied; QCOMPARE(start(Denied, PluginFormat::Wfx, false, false).Failure().category_, ErrorCategory::AccessDenied);
        QTemporaryDir Directory; auto Process = start(Directory, PluginFormat::Wfx); QVERIFY(Process.HasValue());
        auto Reply = Process.Value()->DataCall(WireKind::StorageMutate, {{"action", "delete"}, {"path", "\\hello.txt"}});
        QVERIFY(!Reply.HasValue()); QCOMPARE(Reply.Failure().category_, ErrorCategory::AccessDenied);
    }
    void metadata()
    {
        QTemporaryDir Directory, Cache; auto Process = start(Directory, PluginFormat::Wdx); QVERIFY2(Process.HasValue(), Process.HasValue() ? "" : Process.Failure().code_.c_str());
        auto Materializer = MakeNativeMaterializer(Cache.path().toStdString()); QVERIFY(Materializer.HasValue());
        auto Provider = MakeHostedWdxProvider(Process.Value(), Materializer.Value()); QVERIFY(Provider.HasValue());
        auto Fields = Provider.Value()->Fields({}).get(); QVERIFY(Fields.HasValue()); QCOMPARE(Fields.Value().size(), 7u);
        QFile File(Directory.filePath("source.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("metadata"); File.close();
        auto Storage = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Storage->Resolve({LocatorKind::Local, File.fileName().toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Source = std::make_shared<ProviderContentSource>(Storage, Entry.Value());
        auto Value = Provider.Value()->Query(Source, {0}, {}).get(); QVERIFY2(Value.HasValue(), Value.HasValue() ? "" : Value.Failure().code_.c_str()); QCOMPARE(std::get<std::int64_t>(Value.Value().value_), 9007199254740993LL);
        Value = Provider.Value()->Query(Source, {3}, {}).get(); QVERIFY(Value.HasValue()); QCOMPARE(std::get<std::string>(Value.Value().value_), std::string("한글 metadata"));
        Value = Provider.Value()->Query(Source, {4}, {}).get(); QVERIFY(Value.HasValue()); QCOMPARE(std::get<std::string>(Value.Value().value_), std::string("2026-09-12"));
        Value = Provider.Value()->Query(Source, {6}, {}).get(); QVERIFY(Value.HasValue()); QCOMPARE(Value.Value().status_, MetadataStatus::Delayed);
        Value = Provider.Value()->Query(Source, {6, 0, false}, {}).get(); QVERIFY(Value.HasValue()); QCOMPARE(std::get<std::string>(Value.Value().value_), std::string("resolved"));
        Value = Provider.Value()->Query(Source, {3, 99}, {}).get(); QVERIFY(!Value.HasValue()); QCOMPARE(Value.Failure().code_, std::string("host-wdx-value-termination"));
        QVERIFY(Process.Value()->Ping().HasValue());
    }
    void faults_data()
    {
        QTest::addColumn<bool>("Wfx"); QTest::addColumn<QString>("Fault");
        for (bool Wfx : {true, false}) for (const auto Fault : {"crash", "hang"}) QTest::newRow((QByteArray(Wfx ? "wfx-" : "wdx-") + Fault).constData()) << Wfx << QString(Fault);
    }
    void faults()
    {
        QFETCH(bool, Wfx); QFETCH(QString, Fault); QTemporaryDir Directory; HostLimits Limits; Limits.requestMs_ = 300;
        auto Process = start(Directory, Wfx ? PluginFormat::Wfx : PluginFormat::Wdx, false, true, Limits); QVERIFY(Process.HasValue());
        auto Reply = Wfx ? Process.Value()->DataCall(WireKind::StorageEnumerate, {{"action", "open"}, {"path", "\\" + Fault}})
            : Process.Value()->DataCall(WireKind::MetadataQuery, {{"action", "value"}, {"path", Directory.filePath(Fault + ".txt")}, {"field", 0}, {"unit", 0}, {"defer", false}});
        QVERIFY(!Reply.HasValue()); Process.Value()->Close(); QVERIFY(!Process.Value()->Snapshot().running_);
    }
};
QTEST_GUILESS_MAIN(HostedDataTests)
#include "tstHostedData.moc"
