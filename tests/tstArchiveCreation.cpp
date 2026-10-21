#include "stdafx.h"
#include "UniqueLibs/archive/creationJob.hpp"

#include "UniqueLibs/archive/creatorRegistry.hpp"
#include "UniqueLibs/archive/creationDialog.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QtTest>
#include <QtWidgets>
#include <QtWidgets>
#include <atomic>

using namespace hc::storage;
using namespace hc::operations;
using namespace hc::plugins;
using namespace hc::archive;
namespace {
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
class ProbeProvider final : public IStorageProvider {
public:
    std::shared_ptr<IStorageProvider> inner_ = MakeNativeReadProvider(LocatorKind::Local);
    std::atomic<unsigned> calls_{0}; bool failOnce_ = false; QString marker_, failKey_;
    ProviderId Id() const override { return inner_->Id(); }
    ProviderCapabilities Capabilities() const override { return inner_->Capabilities(); }
    Task<SupportDecision> Supports(Operation Op, OperationContext C) override { ++calls_; return inner_->Supports(Op, C); }
    Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext C) override { ++calls_; return inner_->Enumerate(At, C); }
    Task<EntrySnapshot> Stat(ResourceId Id, OperationContext C) override {
        ++calls_;
        if (failOnce_ && QString::fromStdString(Id.key_) == failKey_ && QFileInfo::exists(marker_)) { failOnce_ = false; return Ready<EntrySnapshot>(Error{ErrorCategory::Unavailable, "injected-second-output-failure"}); }
        return inner_->Stat(Id, C);
    }
    Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext C) override { ++calls_; return inner_->OpenRead(Id, C); }
    Task<EntrySnapshot> Resolve(Locator At, OperationContext C) override { ++calls_; return inner_->Resolve(At, C); }
};
OperationSource Source(const std::shared_ptr<IStorageProvider>& Reader, const QString& Path)
{
    auto Entry = Reader->Resolve({LocatorKind::Local, Path.toStdString()}, {}).get();
    return Entry.HasValue() ? OperationSource{Entry.Value(), Reader, {}} : OperationSource{};
}
std::vector<OutputPlan> Plan(PlanRequest Request, JobStep& Last, bool Rename = false)
{
    CreationPlanner Builder(std::move(Request)); JobStepContext C;
    for (unsigned Count = 0; Count < 10000; ++Count) {
        Last = Builder.Advance(C); C.answer_.reset();
        if (Last.next_ == JobState::Completed) return Builder.Take();
        if (Last.next_ == JobState::Failed) return {};
        if (Last.next_ == JobState::WaitingForUser) { if (!Rename) return {}; C.answer_ = InteractionAnswer{"", InteractionChoice::Rename, "renamed.txt"}; }
    }
    return {};
}
JobResult Run(PreparedJob Prepared)
{
    JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1); auto Handle = Scheduler.Submit(std::move(Prepared.request_), std::move(Prepared.task_));
    if (!Handle.HasValue()) return {};
    for (unsigned I = 0; I < 3000 && Handle.Value().completion_.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready; ++I) {
        auto Snapshot = Scheduler.Snapshot(Handle.Value().id_);
        if (Snapshot.HasValue() && Snapshot.Value().interaction_) {
            const auto& Query = *Snapshot.Value().interaction_;
            Scheduler.Reply(Handle.Value().id_, {Query.id_, Query.kind_ == InteractionKind::Password ? InteractionChoice::Confirm : InteractionChoice::Replace,
                Query.kind_ == InteractionKind::Password ? "archive-test-password" : ""});
        }
    }
    return Handle.Value().completion_.get();
}
CreateJobRequest Request(hc::settings::Repository& Settings, const std::vector<OperationSource>& Sources, const QString& Output, const std::string& Format)
{
    CreateJobRequest R; R.sources_ = Sources;
    for (const auto& Creator : CreatorCatalog(Settings)) for (const auto& Capability : Creator.descriptor_.formats_) if (Capability.format_ == Format) R.creator_ = Creator;
    R.value_.creator_ = R.creator_.descriptor_; R.value_.format_ = Format; R.value_.output_ = {LocatorKind::Local, Output.toStdString()};
    if (Format == "iso") R.value_.settings_ = IsoCreateSettings{};
    else { SevenZipCreateSettings Value; if (Format == "zip") Value.solid_ = ArchiveSolidMode::Off; R.value_.settings_ = Value; }
    return R;
}
}
class ArchiveCreationTests final : public QObject {
    Q_OBJECT
private slots:
    void unifiedDialogAndSettings()
    {
        QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings"));
        CreationDialog Dialog(Settings, {LocatorKind::Local, Dir.path().toStdString()}, Dir.path(), 3);
        Dialog.show(); auto* Create = Dialog.findChild<QPushButton*>("hc.archive.create.submit");
        auto* Queue = Dialog.findChild<QPushButton*>("hc.archive.create.queue");
        QTRY_VERIFY_WITH_TIMEOUT(Create->isEnabled(), 15000); QVERIFY(Queue->isEnabled());
        QVERIFY(!Dialog.findChild<QWidget*>("hc.archive.create.preview")); QVERIFY(!Dialog.findChild<QWidget*>("hc.archive.create.level"));
        QVERIFY(!Dialog.findChild<QCheckBox*>("hc.archive.create.queued"));
        const auto* Format = Dialog.findChild<QComboBox*>("hc.archive.create.format");
        const auto* Creator = Dialog.findChild<QComboBox*>("hc.archive.create.creator");
        QVERIFY(Format->mapTo(&Dialog, QPoint{}).y() < Creator->mapTo(&Dialog, QPoint{}).y());
        QVERIFY(Queue->x() < Create->mapTo(&Dialog, QPoint{}).x());
        const auto StableSize = Dialog.size(); const auto ButtonPosition = Create->mapTo(&Dialog, QPoint());
        auto* Path = Dialog.findChild<QLineEdit*>("hc.archive.create.path"); Path->setText(" "); QVERIFY(!Create->isEnabled()); QVERIFY(!Queue->isEnabled());
        QCoreApplication::processEvents(); QCOMPARE(Dialog.size(), StableSize); QCOMPARE(Create->mapTo(&Dialog, QPoint()), ButtonPosition);
        auto* Error = Dialog.findChild<QLabel*>("hc.archive.create.error"); QVERIFY(Error);
        QVERIFY(!Error->text().isEmpty()); QCOMPARE(Error->toolTip(), Error->text());
        QVERIFY(qAbs(Error->mapTo(&Dialog, Error->rect().center()).y() - Create->mapTo(&Dialog, Create->rect().center()).y()) <= 1);
        QVERIFY(Error->mapTo(&Dialog, Error->rect().topRight()).x() < ButtonPosition.x());
        Path->setText("result.7z"); QVERIFY(Create->isEnabled()); QCoreApplication::processEvents();
        QCOMPARE(Dialog.size(), StableSize); QCOMPARE(Create->mapTo(&Dialog, QPoint()), ButtonPosition);
        auto* Omit = Dialog.findChild<QCheckBox*>("hc.archive.create.omitRoot"); QVERIFY(!Omit->isEnabled());
        Dialog.findChild<QCheckBox*>("hc.archive.create.individual")->setChecked(true); QVERIFY(Omit->isEnabled());
        Dialog.findChild<QCheckBox*>("hc.archive.create.paths")->setChecked(false); QVERIFY(!Omit->isEnabled());
        Dialog.findChild<QCheckBox*>("hc.archive.create.individual")->setChecked(false);
        const auto Before = Dialog.Request().settings_;
        QTimer::singleShot(0, &Dialog, [&] { auto* S = Dialog.findChild<QDialog*>("hc.archive.settings.7z"); QVERIFY(S); S->findChild<QComboBox*>("hc.archive.settings.level")->setCurrentIndex(0); S->reject(); });
        Dialog.findChild<QPushButton*>("hc.archive.create.settings")->click(); QCOMPARE(Dialog.Request().settings_, Before);
        const auto Capture = qEnvironmentVariable("HC_ARCHIVE_CAPTURE_ROOT");
        for (int Width : {720, 390}) { Dialog.resize(Width, 540); QTest::qWait(50); QVERIFY(Dialog.width() <= Width); QVERIFY(Dialog.rect().contains(Queue->geometry())); if (!Capture.isEmpty()) QVERIFY(Dialog.grab().save(Capture + QString("/create-%1.png").arg(Width))); }
        Queue->click(); QCOMPARE(Dialog.result(), int(QDialog::Accepted)); QCOMPARE(Dialog.Request().schedule_, ArchiveCreateSchedule::Queued);
    }
    void nativeSettingsCapabilities()
    {
        for (const auto Format : {"7z", "zip", "iso"}) {
            ArchiveCreateSettings Settings; if (Format == std::string("iso")) Settings = IsoCreateSettings{};
            else { SevenZipCreateSettings S; if (Format == std::string("zip")) S.solid_ = ArchiveSolidMode::Off; Settings = S; }
            QTimer::singleShot(0, [&] {
                auto* D = qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(D);
                if (Format == std::string("iso")) {
                    auto* Share = D->findChild<QCheckBox*>("hc.archive.settings.share"); QVERIFY(Share); Share->setChecked(true); QVERIFY(D->findChild<QDateTimeEdit*>("hc.archive.settings.timestamp"));
                    const auto Size = D->size(); auto* Ok = D->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok); const auto Position = Ok->mapTo(D, QPoint());
                    auto* Label = D->findChild<QLineEdit*>("hc.archive.settings.label"); QVERIFY(Label); Label->setText("invalid 한글");
                    QCoreApplication::processEvents(); QVERIFY(!Ok->isEnabled()); QCOMPARE(D->size(), Size); QCOMPARE(Ok->mapTo(D, QPoint()), Position);
                    auto* Error = D->findChild<QLabel*>("hc.archive.settings.error"); QVERIFY(Error); QCOMPARE(Error->toolTip(), Error->text()); QVERIFY(!Error->text().isEmpty());
                    QVERIFY(qAbs(Error->mapTo(D, Error->rect().center()).y() - Ok->mapTo(D, Ok->rect().center()).y()) <= 1);
                    Label->setText("VALID"); QCoreApplication::processEvents(); QVERIFY(Ok->isEnabled()); QCOMPARE(D->size(), Size); QCOMPARE(Ok->mapTo(D, QPoint()), Position);
                }
                else { auto* Method = D->findChild<QComboBox*>("hc.archive.settings.method"); Method->setCurrentIndex(Method->findData(int(SevenZipMethod::Lzma))); QCOMPARE(D->findChild<QSpinBox*>("hc.archive.settings.threads")->maximum(), Format == std::string("7z") ? 2 : 128); QVERIFY(D->findChild<QComboBox*>("hc.archive.settings.dictionary")->count() > 10); }
                const auto Capture = qEnvironmentVariable("HC_ARCHIVE_CAPTURE_ROOT"); if (!Capture.isEmpty()) QVERIFY(D->grab().save(Capture + "/settings-" + Format + ".png"));
                D->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
            });
            QVERIFY(EditBuiltinCreationSettings(nullptr, Format, Settings));
            if (Format == std::string("iso")) QVERIFY(std::get<IsoCreateSettings>(Settings).shareDuplicateContents_);
            else QCOMPARE(std::get<SevenZipCreateSettings>(Settings).method_, SevenZipMethod::Lzma);
        }
    }
    void settingsPersistenceAndValidation()
    {
        QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings"));
        auto C = CreatorCatalog(Settings).front(); QJsonObject P{{"schemaVersion", 1}, {"unknownRoot", "preserved"}};
        const auto Key = QString::fromStdString(C.descriptor_.id_ + "/7z");
        P["creators"] = QJsonObject{{Key, QJsonObject{{"unknownRecord", 17}, {"value", QJsonObject{{"unknownOption", "preserved"}}}}}};
        SevenZipCreateSettings Value; Value.dictionaryBytes_ = 1ull << 30; Value.threads_ = 32;
        WriteCreatorSettings(P, C, "7z", Value); QVERIFY(SaveCreationPreferences(Settings, P).HasValue());
        auto Restored = CreationPreferences(Settings); QCOMPARE(Restored["unknownRoot"].toString(), QString("preserved"));
        const auto Record = Restored["creators"].toObject()[Key].toObject(); QCOMPARE(Record["unknownRecord"].toInt(), 17); QCOMPARE(Record["value"].toObject()["unknownOption"].toString(), QString("preserved"));
        auto Read = ReadCreatorSettings(Restored, C, "7z"); QVERIFY(Read.HasValue()); QCOMPARE(std::get<SevenZipCreateSettings>(Read.Value()), Value);
        P["schemaVersion"] = 2; QVERIFY(!SaveCreationPreferences(Settings, P).HasValue()); QCOMPARE(CreationPreferences(Settings)["schemaVersion"].toInt(), 1);
        auto R = Request(Settings, {}, Dir.filePath("out.iso"), "iso"); R.sources_.push_back({}); std::get<IsoCreateSettings>(R.value_.settings_).volumeLabel_ = "invalid 한글";
        auto Prepared = PrepareCreation(R); QVERIFY(!Prepared.HasValue()); QCOMPARE(Prepared.Failure().code_, std::string("iso-invalid-settings"));
    }
    void dopusCreationAndMultipleFormats()
    {
        QTemporaryDir Dir; const auto Binary = Dir.filePath("archive-multi.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Binary));
        auto Inspected = InspectPluginBinary(Binary, PluginFormat::DopusVfs); QVERIFY(Inspected.HasValue());
        auto Saved = SavePluginManifest(Inspected.Value(), Dir.path()); QVERIFY(Saved.HasValue());
        QJsonObject Record{{"path", Binary}, {"hostManifest", Saved.Value()}, {"hostReviewed", hc::settings::JsonObject(EncodeManifest(Inspected.Value().manifest_))}, {"hostEnabled", true}};
        auto Creator = ExternalCreator(Record, PluginFormat::DopusVfs, Dir.path());
        auto Verified = VerifyCreationFormats(Creator, {"hcvfs", "unsupported"}, {}); QVERIFY2(Verified.HasValue(), Verified.HasValue() ? "" : Verified.Failure().code_.c_str());
        QCOMPARE(Verified.Value()["formats"].toArray(), QJsonArray({"hcvfs"})); QVERIFY(Verified.Value()["configure"].toBool());
    }
    void actualTotal7zip()
    {
        const auto Binary = qEnvironmentVariable("HC_ARCHIVE_CREATOR_WCX"); if (Binary.isEmpty()) QSKIP("Set HC_ARCHIVE_CREATOR_WCX for the original Total7zip package");
        QTemporaryDir Dir; auto Inspected = InspectPluginBinary(Binary, PluginFormat::Wcx); QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        auto Saved = SavePluginManifest(Inspected.Value(), Dir.path()); QVERIFY(Saved.HasValue());
        QJsonObject Record{{"path", Binary}, {"hostManifest", Saved.Value()}, {"hostReviewed", hc::settings::JsonObject(EncodeManifest(Inspected.Value().manifest_))}, {"hostEnabled", true}};
        auto Creator = ExternalCreator(Record, PluginFormat::Wcx, Dir.path()); Creator.options_.defaultIniPath_.clear();
        auto Verified = VerifyCreationFormats(Creator, {"7z", "zip"}, {}); QVERIFY2(Verified.HasValue(), Verified.HasValue() ? "" : Verified.Failure().code_.c_str());
        QCOMPARE(Verified.Value()["formats"].toArray(), QJsonArray({"7z", "zip"})); QVERIFY(Verified.Value()["configure"].toBool());
        Record["archiveCreation"] = Verified.Value(); Creator = ExternalCreator(Record, PluginFormat::Wcx, Dir.path()); QVERIFY(Creator.available_); QCOMPARE(Creator.descriptor_.formats_.size(), 2u);
        auto Altered = Verified.Value(); Altered["identity"] = "wrong"; Record["archiveCreation"] = Altered; QVERIFY(ExternalCreator(Record, PluginFormat::Wcx, Dir.path()).descriptor_.formats_.empty());
    }
    void composition_data()
    {
        QTest::addColumn<bool>("Paths"); QTest::addColumn<bool>("Recursive"); QTest::addColumn<bool>("Separate"); QTest::addColumn<bool>("Omit");
        for (int I = 0; I < 16; ++I) QTest::newRow(qPrintable(QString::number(I))) << bool(I & 1) << bool(I & 2) << bool(I & 4) << bool(I & 8);
    }
    void composition()
    {
        QFETCH(bool, Paths); QFETCH(bool, Recursive); QFETCH(bool, Separate); QFETCH(bool, Omit);
        QTemporaryDir Dir; QVERIFY(QDir().mkpath(Dir.filePath("report/sub"))); QVERIFY(QDir().mkdir(Dir.filePath("report/empty")));
        QVERIFY(Put(Dir.filePath("report/summary.txt"), "summary")); QVERIFY(Put(Dir.filePath("report/sub/reference.txt"), "reference")); QVERIFY(Put(Dir.filePath("memo.txt"), "memo"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        PlanRequest R; R.sources_ = {Source(Reader, Dir.filePath("report")), Source(Reader, Dir.filePath("memo.txt"))};
        R.destination_ = {LocatorKind::Local, Dir.filePath(Separate ? "out" : "result.7z").toStdString()}; R.format_ = "7z"; R.composition_ = {Paths, Recursive, Separate, Omit};
        JobStep Last; const auto Outputs = Plan(R, Last); QCOMPARE(Last.next_, JobState::Completed); QCOMPARE(Outputs.size(), Separate ? 2u : 1u);
        QStringList Members; for (const auto& Output : Outputs) for (const auto& Entry : Output.entries_) Members << Entry.member_;
        const QString Prefix = Paths && !(Separate && Omit) ? "report/" : "";
        QVERIFY(Members.contains(Prefix + "summary.txt")); QVERIFY(Members.contains("memo.txt"));
        QCOMPARE(Members.contains(Prefix + (Paths ? "sub/" : "") + "reference.txt"), Recursive);
        QCOMPARE(Members.contains(Prefix + "empty"), Paths && Recursive);
        if (Separate) { QCOMPARE(QFileInfo(QString::fromStdString(Outputs[0].destination_.root_)).fileName(), "report.7z"); QCOMPARE(QFileInfo(QString::fromStdString(Outputs[1].destination_.root_)).fileName(), "memo.txt.7z"); }
    }
    void collisionsAndOutputGuards()
    {
        QTemporaryDir Dir; QVERIFY(QDir().mkpath(Dir.filePath("a/sub"))); QVERIFY(Put(Dir.filePath("a/one.txt"), "one")); QVERIFY(Put(Dir.filePath("a/sub/one.txt"), "two"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); PlanRequest R; R.sources_ = {Source(Reader, Dir.filePath("a"))}; R.destination_ = {LocatorKind::Local, Dir.filePath("out.7z").toStdString()}; R.format_ = "7z"; R.composition_.preservePaths_ = false;
        JobStep Last; QVERIFY(Plan(R, Last).empty()); QCOMPARE(Last.next_, JobState::WaitingForUser);
        const auto Renamed = Plan(R, Last, true); QCOMPARE(Last.next_, JobState::Completed); QCOMPARE(Renamed.front().entries_.size(), 2u);
        QVERIFY(!ValidateOutputLocation({LocatorKind::Local, Dir.filePath("a/out.7z").toStdString()}, R.sources_).HasValue());
        auto At = R.sources_[0]; At.entry_.locator_.root_ = "\\\\?\\UNC\\server\\share\\input"; At.entry_.locator_.kind_ = LocatorKind::Smb;
        QVERIFY(!ValidateOutputLocation({LocatorKind::Smb, "//server/share/input/out.7z"}, {At}).HasValue());
        QVERIFY(ValidateOutputLocation({LocatorKind::Smb, "//server/share/input-other/out.7z"}, {At}).HasValue());
        QVERIFY(!IsSafeArchiveName("CON.txt")); QVERIFY(!IsSafeArchiveName("../bad"));
        QCOMPARE(ArchivePathKey(QString::fromUtf8("가")), ArchivePathKey(QString::fromUtf8("가")));
    }
    void nativeCreation_data() { QTest::addColumn<QString>("Format"); for (const auto F : {"7z", "zip", "iso"}) QTest::newRow(F) << QString(F); }
    void nativeCreation()
    {
        QFETCH(QString, Format); QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings"));
        QVERIFY(QDir().mkpath(Dir.filePath("report/sub"))); QVERIFY(QDir().mkdir(Dir.filePath("report/empty")));
        QVERIFY(Put(Dir.filePath("report/sub/one.txt"), "archive job bytes")); QVERIFY(Put(Dir.filePath("memo.txt"), "memo"));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); const auto Target = Dir.filePath("result." + Format);
        auto R = Request(Settings, {Source(Reader, Dir.filePath("report")), Source(Reader, Dir.filePath("memo.txt"))}, Target, Format.toStdString());
        if (Format == "iso") std::get<IsoCreateSettings>(R.value_.settings_).shareDuplicateContents_ = true;
        auto Prepared = PrepareCreation(R); QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
        const auto Result = Run(std::move(Prepared.Value())); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "failed");
        QVERIFY(QFileInfo::exists(Target)); QCOMPARE(Result.items_.size(), 1u); QVERIFY(Result.items_[0].destinationCommitted_);
    }
    void queueAndPartialRetry()
    {
        QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings")); QVERIFY(QDir().mkdir(Dir.filePath("out")));
        QVERIFY(Put(Dir.filePath("first"), "first bytes")); QVERIFY(Put(Dir.filePath("second"), "second bytes"));
        auto Reader = std::make_shared<ProbeProvider>(); auto R = Request(Settings, {Source(Reader, Dir.filePath("first")), Source(Reader, Dir.filePath("second"))}, Dir.filePath("out"), "7z");
        R.value_.composition_.separateArchives_ = true; R.value_.schedule_ = ArchiveCreateSchedule::Queued;
        Reader->calls_ = 0; auto Prepared = PrepareCreation(R); QVERIFY(Prepared.HasValue()); QCOMPARE(Reader->calls_.load(), 0u);
        {
            JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1); auto Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
            QTest::qWait(50); QCOMPARE(Reader->calls_.load(), 0u); QVERIFY(!QFileInfo::exists(Dir.filePath("out/first.7z")));
            QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue()); Job.Value().completion_.wait();
        }
        R.value_.schedule_ = ArchiveCreateSchedule::Immediate; Reader->marker_ = Dir.filePath("out/first.7z"); Reader->failKey_ = QString::fromStdString(R.sources_[1].entry_.id_.key_); Reader->failOnce_ = true;
        Prepared = PrepareCreation(R); QVERIFY(Prepared.HasValue()); auto Failed = Run(std::move(Prepared.Value()));
        QCOMPARE(Failed.snapshot_.state_, JobState::Failed); QVERIFY(QFileInfo::exists(Reader->marker_)); QVERIFY(!QFileInfo::exists(Dir.filePath("out/second.7z")));
        auto Retry = RetryCreation(R, Failed); QCOMPARE(Retry.completedOutputs_.size(), 1u); QVERIFY(QFile::remove(Dir.filePath("first")));
        Prepared = PrepareCreation(Retry); QVERIFY(Prepared.HasValue()); auto Done = Run(std::move(Prepared.Value()));
        QVERIFY2(Done.snapshot_.state_ == JobState::Completed, Done.snapshot_.error_ ? Done.snapshot_.error_->code_.c_str() : "failed"); QVERIFY(QFileInfo::exists(Dir.filePath("out/second.7z")));
    }
    void readonlyStagingCleanup()
    {
        QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings")); const auto File = Dir.filePath("readonly.txt"); QVERIFY(Put(File, "readonly archive input"));
        const auto Native = QDir::toNativeSeparators(File).toStdWString(); QVERIFY(SetFileAttributesW(Native.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));
        const auto Reset = qScopeGuard([&] { SetFileAttributesW(Native.c_str(), FILE_ATTRIBUTE_NORMAL); });
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto R = Request(Settings, {Source(Reader, File)}, Dir.filePath("result.7z"), "7z");
        auto Prepared = PrepareCreation(R); QVERIFY(Prepared.HasValue()); auto Result = Run(std::move(Prepared.Value()));
        QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "failed"); QVERIFY(Result.cleanupErrors_.empty());
        QCOMPARE(GetFileAttributesW(Native.c_str()) & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN), DWORD(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN));
    }
    void encryptedCreation()
    {
        const auto Tool = QDir(QCoreApplication::applicationDirPath()).filePath("../sevenzip-tools/official/7z.exe"); QVERIFY(QFileInfo::exists(Tool));
        for (const auto Format : {"7z", "zip"}) {
            QTemporaryDir Dir; hc::settings::Repository Settings(Dir.filePath("settings")); const auto File = Dir.filePath("secret.txt"); QVERIFY(Put(File, "encrypted job bytes"));
            const auto Target = Dir.filePath(QString("encrypted.") + Format); auto Reader = MakeNativeReadProvider(LocatorKind::Local);
            auto R = Request(Settings, {Source(Reader, File)}, Target, Format); auto& Value = std::get<SevenZipCreateSettings>(R.value_.settings_); Value.encrypt_ = true;
            auto Prepared = PrepareCreation(R); QVERIFY(Prepared.HasValue()); Prepared.Value().request_.operation_.providerInteractions_ = true;
            auto Result = Run(std::move(Prepared.Value())); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "failed");
            QProcess Verify; Verify.start(Tool, {"t", "-parchive-test-password", Target}); QVERIFY(Verify.waitForFinished(15000)); QCOMPARE(Verify.exitCode(), 0);
        }
    }
};
QTEST_MAIN(ArchiveCreationTests)
#include "tstArchiveCreation.moc"
