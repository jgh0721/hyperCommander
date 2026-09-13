#include "stdafx.h"
#include <QtTest>
#include <QtConcurrent/QtConcurrentRun>
#include "HyperCommander.hpp"
#include "UIs/dlgMain.hpp"
#include "UIs/dlgOpts.hpp"
#include "UIs/cmpPanel.hpp"
#include "memoryStorage.hpp"
#include "commandMgr.hpp"
#include "qtitanColumnAdapter.hpp"
#include "fileJobUi.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeMaterializer.hpp"
#include "storageProviders.hpp"
#include "qtViewerSurface.hpp"
#include "qtViewerHost.hpp"
#include "qtPluginHostSettings.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QtitanGrid.h>
#include <iostream>
#include <psapi.h>
#include "driveProbeFixture.hpp"
#ifdef _DEBUG
#include <crtdbg.h>
#endif

class GatedProvider : public hc::storage::IStorageProvider
{
public:
    std::promise<void> release_;
    std::shared_future<void> gate_ = release_.get_future().share();
    std::atomic<bool> started_{false};
    hc::storage::test::MemoryProvider inner_;
    hc::storage::ProviderId Id() const override { return inner_.Id(); }
    hc::storage::Task<hc::storage::SupportDecision> Supports(hc::storage::Operation Op, hc::storage::OperationContext Ctx) override { return inner_.Supports(Op, Ctx); }
    hc::storage::Task<std::unique_ptr<hc::storage::IEntryCursor>> Enumerate(hc::storage::Locator At, hc::storage::OperationContext Ctx) override
    {
        started_ = true;
        gate_.wait();
        return inner_.Enumerate(At, Ctx);
    }
    hc::storage::Task<hc::storage::EntrySnapshot> Stat(hc::storage::ResourceId Id, hc::storage::OperationContext Ctx) override { return inner_.Stat(Id, Ctx); }
    hc::storage::Task<std::shared_ptr<hc::storage::IReadStream>> OpenRead(hc::storage::ResourceId Id, hc::storage::OperationContext Ctx) override { return inner_.OpenRead(Id, Ctx); }
};
class GatedFs : public nsHC::CFileSystemT
{
public:
    explicit GatedFs(std::shared_ptr<GatedProvider> Provider) : CFileSystemT("C:"), provider_(std::move(Provider)) {}
    std::shared_ptr<hc::storage::IStorageProvider> ReadProvider() override { return provider_; }
    hc::storage::Locator ReadLocation(const QString&) const override { return provider_->inner_.Root(); }
private:
    std::shared_ptr<GatedProvider> provider_;
};

static QElapsedTimer StartupTimer;

class SmbTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        server_ = qEnvironmentVariable("HC_SMB_SERVER", "DEVTESTER-QTS");
        share_ = qEnvironmentVariable("HC_SMB_SHARE", "Public");
        QVERIFY(!server_.contains('\\') && !share_.contains('\\'));
        const auto LocalRoot = qEnvironmentVariable("HC_UI_LOCAL_ROOT");
        localOnly_ = !LocalRoot.isEmpty();
        unc_ = localOnly_ ? LocalRoot : "\\\\" + server_ + "\\" + share_;
        QVERIFY2(QDir(unc_).exists(), localOnly_ ? "Local UI test root must exist" : "Live SMB share must be accessible before running this suite");
        if (localOnly_)
            qInfo(qEnvironmentVariableIsSet("HC_TEST_MAPPED_IO")
                ? "Mapped-drive I/O mode: explicit mapping required; local folder used for recovery"
                : "Local UI mode: no SMB I/O coverage; only explicitly allowed scenarios may run");
        for (auto* Window : QApplication::topLevelWidgets())
            if (auto* Main = qobject_cast<QMainUI*>(Window))
                main_ = Main;
        QVERIFY(main_);
        const auto ExpectedDpr = qEnvironmentVariable("HC_UI_EXPECT_DPR");
        if (!ExpectedDpr.isEmpty()) QCOMPARE(main_->devicePixelRatioF(), ExpectedDpr.toDouble());
        qInfo() << "Product UI DPR:" << main_->devicePixelRatioF();
        panels_ = main_->findChildren<CmpPanel*>();
        QCOMPARE(panels_.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(model(panels_.front())->GetRoot(), 5000);
        for (auto* Panel : panels_) QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 15000);
    }

    void init()
    {
        if (localOnly_)
        {
            const QStringList Allowed{"isoPluginUi", "dopusPluginsUi", "dataPluginsUi", "networkDriveStartup", "archiveCommandsAndBatchUi", "archiveCrossContainerMovesUi", "viewerCommandsAndPreviewUi", "wcxHostSettingsPersistence", "wcxHostApprovalUi", "configuredHostNavigation", "viewerLifetimeSoak", "wlxViewerCommandsUi"};
            QVERIFY2(Allowed.contains(QString::fromLatin1(QTest::currentTestFunction())), "Local UI mode cannot execute an SMB scenario");
        }
    }

    void isoPluginUi_data()
    {
        QTest::addColumn<int>("ProfileIndex"); QTest::newRow("joliet-udf") << 2; QTest::newRow("long-name-udf") << 3;
    }
    void isoPluginUi()
    {
        QFETCH(int, ProfileIndex);
        using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Output = Directory.filePath("created.iso");
        { QFile File(Directory.filePath("한글.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("ISO UI input"), 12); }
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row) {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated(); return true; }
            }
            return false;
        };
        QVERIFY(Focus("한글.txt"));
        auto* Create = main_->findChild<QAction*>("hc.menu.iso.create"); QVERIFY(Create);
        auto& Scheduler = TyStCommandMgr::GetInstance()->Bridge()->FileJobs()->Scheduler();
        QSet<QString> ExistingJobs; for (const auto& Job : Scheduler.Snapshots()) ExistingJobs.insert(QString::fromStdString(Job.id_));
        bool DialogSeen = false;
        QTimer::singleShot(0, main_, [&] {
            auto* Dialog = main_->findChild<QDialog*>("hc.iso.create"); if (!Dialog) return;
            const auto CloseDialog = qScopeGuard([Dialog] { Dialog->accept(); });
            DialogSeen = true;
            auto* Path = Dialog->findChild<QLineEdit*>("hc.iso.create.path"); Path->setText(Output);
            QCOMPARE(Dialog->findChild<QComboBox*>("hc.iso.create.profile")->currentIndex(), 2);
            Dialog->findChild<QComboBox*>("hc.iso.create.profile")->setCurrentIndex(ProfileIndex);
            QCOMPARE(Dialog->findChild<QLineEdit*>("hc.iso.create.label")->text(), QString("HYPERCOMMANDER"));
            QVERIFY(Dialog->rect().contains(Path->mapTo(Dialog, QPoint(0, 0))));
            QVERIFY(Dialog->grab().save(QDir::current().filePath(QString("iso-create-%1.png").arg(ProfileIndex))));
            Dialog->accept();
        });
        Create->trigger(); QVERIFY(DialogSeen);
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshots().size() > size_t(ExistingJobs.size()), 5000);
        std::string Job; for (const auto& Value : Scheduler.Snapshots()) if (!ExistingJobs.contains(QString::fromStdString(Value.id_))) Job = Value.id_;
        QVERIFY(!Job.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Job).Value().state_), 30000);
        const auto Snapshot = Scheduler.Snapshot(Job).Value();
        QVERIFY2(Snapshot.state_ == JobState::Completed, Snapshot.error_ ? Snapshot.error_->code_.c_str() : "ISO job failed");
        for (auto* Dialog : main_->findChildren<QDialog*>("hc.fileJobs")) Dialog->hide();
        QTRY_VERIFY(!Model->IsLoading()); Model->Refresh(); QTRY_VERIFY(!Model->IsLoading() && names(Model).contains("created.iso"));
        QVERIFY(Focus("created.iso"));
        auto* Inspect = main_->findChild<QAction*>("hc.menu.iso.inspect"); QVERIFY(Inspect); Inspect->trigger();
        auto* Dialog = main_->findChild<QDialog*>("hc.iso.inspect"); QVERIFY(Dialog);
        auto* Status = Dialog->findChild<QLabel*>("hc.iso.inspect.status"); QVERIFY(Status);
        QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("구조 확인 완료"), 10000); QVERIFY(Status->text().contains("UDF 1.02"));
        auto* Table = Dialog->findChild<QTableWidget*>("hc.iso.inspect.entries"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1); QCOMPARE(Table->item(0, 0)->text(), QString("한글.txt"));
        QCOMPARE(Status->text().contains("Joliet"), ProfileIndex == 2);
        QVERIFY(Dialog->grab().save(QDir::current().filePath(QString("iso-inspect-%1.png").arg(ProfileIndex))));
        Dialog->findChild<QPushButton*>("hc.iso.inspect.readAll")->click(); QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("전체 읽기 검사 완료"), 10000);
        Dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!Model->IsLoading()); QVERIFY(Focus("created.iso"));
        auto Selected = Grid->BaseView()->focusedRow().modelIndex(0);
        Panel->ReturnOnCurrentTab(Selected);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading() && Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot()).kind_ == hc::storage::LocatorKind::Archive, 10000);
        QVERIFY(names(Model).contains("한글.txt"));
        qInfo() << "Built-in ISO create, inspect, read test and automatic archive navigation passed";
    }

    void networkDriveStartup()
    {
        const bool Synthetic = qEnvironmentVariableIsSet("HC_TEST_DISCONNECTED_DRIVE");
        const auto Root = Synthetic ? DriveProbeFixture::Root() : qEnvironmentVariable("HC_TEST_MAPPED_DRIVE");
        if (Root.isEmpty()) QSKIP("Run the network-startup fixture or select an existing mapped drive");
        QVERIFY2(Root.size() == 2 && Root[1] == ':', "Select a mapped drive or the disconnected-drive fixture");
        qInfo() << "Startup to panel initialization (ms):" << StartupTimer.elapsed()
                << "mapped drive:" << Root << "synthetic:" << Synthetic;
        if (Synthetic)
        {
            qInfo() << "Synthetic metadata calls:" << DriveProbeFixture::MetadataCalls()
                    << "on GUI:" << DriveProbeFixture::GuiMetadataCalls();
            QCOMPARE(DriveProbeFixture::MetadataCalls(), 0);
            QCOMPARE(DriveProbeFixture::AttributeCalls(), 0);
        }
        for (auto* Panel : panels_)
        {
            auto* Combo = Panel->findChild<QComboBoxForWidget*>("cbxVolume");
            QVERIFY(Combo);
            nsHC::TySpFileSystem Mapped;
            for (int Index = 0; Index < Combo->GetCount(); ++Index)
            {
                auto Fs = Combo->GetItem(Index)->GetUserData(Qt::UserRole).value<nsHC::TySpFileSystem>();
                if (Fs && Fs->GetRoot().compare(Root, Qt::CaseInsensitive) == 0) Mapped = Fs;
            }
            QVERIFY2(Mapped, "A disconnected drive must remain visible in both panels");
            QVERIFY(Mapped->IsRemote());
            QVERIFY(!model(Panel)->GetRoot()->IsRemote());
            // The native writer distinguishes drive-letter and UNC syntax.
            QCOMPARE(Mapped->ReadLocation(Root + '\\').kind_, hc::storage::LocatorKind::Local);
            QVERIFY(Mapped->ReadProvider());
            for (const auto& Path : QStringList{Root.toLower() + "/folder", "\\\\?\\" + Root + "\\folder"})
            {
                auto Fs = nsHC::TySpFileSystem(nsHC::CFileSystemT::MakeFileSystem(Path));
                QVERIFY(Fs && Fs->IsRemote());
                QCOMPARE(Fs->GetRoot().toUpper(), Root.toUpper());
            }
            if (Synthetic)
            {
                QElapsedTimer ChangeTimer;
                ChangeTimer.start();
                Panel->ChangeVolume(Root);
                QVERIFY2(ChangeTimer.elapsed() < 300, "Selecting a disconnected drive blocked the GUI");
                QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
                QVERIFY(static_cast<CFSModel*>(model(Panel))->LastNavigationError().has_value());
                Panel->ChangeCurrentPath(unc_, {});
                QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
                QCOMPARE(model(Panel)->GetCurrentPathWithRoot(), unc_);
                Panel->RefreshVolumeList();
            }
            if (!Synthetic && qEnvironmentVariableIsSet("HC_TEST_MAPPED_IO"))
            {
                QTemporaryDir Corpus(Root + "/hc-startup-XXXXXX");
                QVERIFY(Corpus.isValid());
                qInfo() << "Mapped drive corpus:" << Corpus.path();
                const QByteArray Payload("mapped drive read/write\r\n");
                QFile Source(Corpus.filePath("startup.txt"));
                QVERIFY(Source.open(QIODevice::WriteOnly));
                QCOMPARE(Source.write(Payload), Payload.size());
                Source.close();
                for (bool Enabled : {false, true})
                {
                    auto* Model = static_cast<CFSModel*>(model(Panel));
                    Model->SetProviderEnabled(Enabled);
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
                    Panel->ChangeCurrentPath(QDir::toNativeSeparators(Corpus.path()), {});
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
                    QCOMPARE(names(Model), QStringList{"startup.txt"});
                    if (Enabled)
                    {
                        auto Reader = Mapped->ReadProvider();
                        auto Writer = hc::storage::MakeNativeWritableStorage(hc::storage::LocatorKind::Local, Reader);
                        const auto Destination = Mapped->ReadLocation(QDir::toNativeSeparators(Corpus.filePath("created")));
                        auto Created = QtConcurrent::run([Writer, Destination] { return Writer->MakeDirectory(Destination, {}); });
                        QTRY_VERIFY_WITH_TIMEOUT(Created.isFinished(), 15000);
                        QVERIFY2(Created.result().HasValue(), "Mapped-drive mutation must retain drive-letter locator semantics");
                        for (int Row = 0; Row < Model->rowCount(); ++Row)
                        {
                            const auto Entry = Model->GetFileInfo(Model->index(Row, 0));
                            if (Entry->Name_ != "startup.txt") continue;
                            QVERIFY(Entry->storageEntry_.has_value());
                            auto Opened = Reader->OpenRead(Entry->storageEntry_->id_, {});
                            QTRY_VERIFY_WITH_TIMEOUT(Opened.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
                            auto Stream = Opened.get(); QVERIFY(Stream.HasValue());
                            auto Read = Stream.Value()->Read(1024, {});
                            QTRY_VERIFY_WITH_TIMEOUT(Read.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 15000);
                            auto Chunk = Read.get(); QVERIFY(Chunk.HasValue());
                            QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), Payload);
                        }
                    }
                    Panel->ChangeCurrentPath(unc_, {});
                    QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
                }
            }
        }
        if (Synthetic) QCOMPARE(DriveProbeFixture::MetadataCalls(), 0);
    }

    void shareNavigation_data()
    {
        QTest::addColumn<bool>("Enabled");
        QTest::newRow("legacy") << false;
        QTest::newRow("provider") << true;
    }
    void shareNavigation()
    {
        QFETCH(bool, Enabled);
        const auto Expected = QDir(unc_).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(Enabled);
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 15000);
            Panel->ChangeCurrentPath(unc_, {});
            auto* Model = model(Panel);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Panel->RefreshSource(Panel->CurrentTabIndex());
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory("..");
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), "\\\\" + server_);
            QVERIFY(names(Model).contains(share_));
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory(share_);
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
        }
    }

    void missingDirectoryAndRecovery()
    {
        auto* Panel = panels_.front();
        auto* Model = model(Panel);
        const auto Missing = unc_ + "\\__hc_missing_" + QUuid::createUuid().toString(QUuid::Id128);
        QVERIFY(!QDir(Missing).exists());
        QElapsedTimer Timer;
        Timer.start();
        Panel->ChangeCurrentPath(Missing, {});
        qInfo() << "Missing-directory UI call milliseconds:" << Timer.elapsed();
        QVERIFY(Timer.elapsed() < 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        const auto Failure = static_cast<CFSModel*>(Model)->LastNavigationError();
        QVERIFY(Failure.has_value());
        QCOMPARE(Failure->category_, hc::storage::ErrorCategory::NotFound);
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        bool Tick = false;
        QTimer::singleShot(0, this, [&Tick] { Tick = true; });
        QTRY_VERIFY(Tick);
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        QVERIFY(!names(Model).isEmpty());
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/smb-panels.png"));
    }

    void cancelledAndLateNavigation()
    {
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        for (bool Cancel : {true, false})
        {
            auto Provider = std::make_shared<GatedProvider>();
            auto Release = qScopeGuard([&] { Provider->release_.set_value(); });
            Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider)));
            Model->SetCurrentPath("\\");
            Model->ChangeDirectory("");
            QTRY_VERIFY(Provider->started_.load());
            QVERIFY(Model->IsLoading());
            bool Tick = false;
            QTimer::singleShot(0, this, [&] { Tick = true; });
            QTRY_VERIFY(Tick);
            if (Cancel) Model->CancelNavigation();
            else Panel->ChangeCurrentPath(unc_, {});
            QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 15000);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        }
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
    }

    void emptyAndWcxNavigation()
    {
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(names(Model).isEmpty());
        const auto ArchivePath = Directory.filePath("fixture.hcwcx");
        QFile Archive(ArchivePath);
        QVERIFY(Archive.open(QIODevice::WriteOnly));
        Archive.write("HCWCX01");
        Archive.close();
        TyPlugInWCX Spec;
        Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        auto Plugin = TySpWCX(new CTCPlugInWCX);
        QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u);
        QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto Pack = nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin));
        for (bool Enabled : {false, true})
        {
            Model->SetProviderEnabled(Enabled);
            QTRY_VERIFY(!Model->IsLoading());
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
            QTRY_VERIFY(!Model->IsLoading());
            Model->AppendRoot(Pack);
            Model->ChangeDirectory("/");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            Model->ChangeDirectory("sub");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({QString::fromUtf8("한글.txt")}));
            if (Enabled)
            {
                auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
                const auto Destination = Directory.filePath("extracted"); QVERIFY(QDir().mkdir(Destination));
                QModelIndex Selected;
                for (int Row = 0; Row < Model->rowCount(); ++Row)
                    if (Model->GetName(Model->index(Row, 0)) == QString::fromUtf8("한글.txt")) Selected = Model->index(Row, 0);
                QVERIFY(Selected.isValid());
                auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", Selected);
                QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
                auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
                auto& Scheduler = Bridge->FileJobs()->Scheduler();
                QTRY_VERIFY(hc::operations::IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_));
                QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, hc::operations::JobState::Completed);
                QFile Copied(Destination + "/한글.txt"); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("한글!\r\n"));
                QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
                QCOMPARE(names(Model), QStringList({QString::fromUtf8("한글.txt")}));
            }
            Model->ChangeDirectory("..");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            QThreadPool::globalInstance()->waitForDone();
        }
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Model->IsLoading());
    }

    void viewerCommandsAndPreviewUi()
    {
        const auto PrivateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX Counters{}; Counters.cb = sizeof(Counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters), sizeof(Counters))
                ? static_cast<quint64>(Counters.PrivateUsage) : quint64{};
        };
        const auto BeforeBytes = PrivateBytes(); QVERIFY(BeforeBytes > 0);
        auto PeakBytes = BeforeBytes;
        QTimer MemorySampler; MemorySampler.setInterval(25);
        connect(&MemorySampler, &QTimer::timeout, this, [&] { PeakBytes = std::max(PeakBytes, PrivateBytes()); }); MemorySampler.start();
        const auto CorpusRoot = localOnly_ ? QDir::tempPath() : unc_;
        QTemporaryDir Directory(QDir(CorpusRoot).filePath("hc-viewer-XXXXXX")); QVERIFY(Directory.isValid());
        qInfo() << "Viewer corpus location:" << Directory.path() << "SMB:" << !localOnly_;
        const auto Put = [&](const QString& Name, const QByteArray& Bytes) { QFile File(Directory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put("first.txt", "first viewer content")); QVERIFY(Put("second.md", "# Second\n\nMarkdown preview")); QVERIFY(Put("archive.hcwcx", "HCWCX01"));
        QImage Picture(160, 90, QImage::Format_RGB32); Picture.fill(QColor(40, 100, 180));
        QVERIFY(Picture.save(Directory.filePath("picture.png")));
        auto* SourcePanel = panels_.front(); auto* TargetPanel = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
            QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
        }
        auto* Model = static_cast<CFSModel*>(model(SourcePanel));
        auto* Grid = qobject_cast<CGridView*>(SourcePanel->GetFocusView(SourcePanel->CurrentTabIndex())); QVERIFY(Grid);
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
            {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.model() == Model && Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); return Index; }
            }
            return QModelIndex{};
        };
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        const auto Dispatch = [&](const QString& Id, QModelIndex Index = {}) {
            auto Future = Bridge->Dispatch(Id, {}, "test", Index); QElapsedTimer Wait; Wait.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Wait.elapsed() > 5000) return false; QTest::qWait(10); }
            auto Result = Future.get(); if (!Result.HasValue()) qWarning() << QString::fromStdString(Result.Failure().code_); return Result.HasValue();
        };
        auto First = Focus("first.txt"); QVERIFY(First.isValid());
        QVERIFY(QApplication::focusWidget()); QTest::keyClick(QApplication::focusWidget(), Qt::Key_F3);
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000);
        QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* FullSurface = Full->findChild<QtViewerSurface*>(); QVERIFY(FullSurface);
        QTRY_VERIFY_WITH_TIMEOUT(FullSurface->IsReady(), 5000);
        QCOMPARE(FullSurface->findChild<QPlainTextEdit*>()->toPlainText(), QString("first viewer content"));
        QTest::keyClick(Full.data(), Qt::Key_Escape); QTRY_VERIFY(Full.isNull());

        auto* Tabs = TargetPanel->findChild<QTabWidget*>("tabWidget"); QVERIFY(Tabs);
        const auto OriginalCount = Tabs->count(); QPointer<QWidget> FirstTab = Tabs->widget(0);
        TargetPanel->AddTab(); const auto PreviewIndex = TargetPanel->CurrentTabIndex();
        QTRY_VERIFY(!model(TargetPanel)->IsLoading());
        QPointer<QWidget> OriginalView = Tabs->widget(PreviewIndex);
        First = Focus("first.txt"); QVERIFY(First.isValid());
        auto* QuickAction = main_->findChild<QAction*>("hc.menu.hc.viewer.quick"); QVERIFY(QuickAction); QuickAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex)), 5000);
        QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex));
        auto* Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Tabs->currentIndex(), PreviewIndex); QCOMPARE(Tabs->widget(0), FirstTab.data()); QCOMPARE(Tabs->count(), OriginalCount + 1);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("first viewer content"));
        QVERIFY(Focus("second.md").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "second.md", 5000);
        QVERIFY(Surface->findChild<QTextBrowser*>()->toPlainText().contains("Second"));
        for (int Repeat = 0; Repeat < 12; ++Repeat)
        {
            QVERIFY(Focus("first.txt").isValid()); QTest::qWait(15);
            QVERIFY(Focus("picture.png").isValid()); QTest::qWait(15);
            QVERIFY(Focus("second.md").isValid()); QTest::qWait(15);
        }
        QVERIFY(Focus("picture.png").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "picture.png", 5000);
        QVERIFY(Surface->findChild<QScrollArea*>("hc.viewer.image")->isVisible());
        QVERIFY(Focus("second.md").isValid());
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && Surface->Snapshot().source_.name_ == "second.md", 5000);
        QVERIFY(main_->grab().save("viewer-product-quick.png"));
        Surface->findChild<QTextBrowser*>()->setFocus(); QTest::keyClick(QApplication::focusWidget(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(Quick.isNull(), 5000); QCOMPARE(Tabs->widget(PreviewIndex), OriginalView.data()); QCOMPARE(Tabs->widget(0), FirstTab.data());

        // Archive content follows the same commands; no fake native pathname.
        const auto ArchivePath = Directory.filePath("archive.hcwcx");
        TyPlugInWCX Spec; Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u); QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        Model->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin))); Model->ChangeDirectory("/"); QTRY_VERIFY(!Model->IsLoading());
        const auto Archived = Focus("alpha.txt"); QVERIFY(Archived.isValid()); QVERIFY(Dispatch("hc.viewer.open", Archived));
        Full = main_->findChild<QDialog*>("hc.viewer.full"); QVERIFY(Full); FullSurface = Full->findChild<QtViewerSurface*>();
        QTRY_VERIFY_WITH_TIMEOUT(FullSurface->IsReady(), 5000); QCOMPARE(FullSurface->findChild<QPlainTextEdit*>()->toPlainText(), QString("alpha")); Full->reject(); QTRY_VERIFY(Full.isNull());
        QVERIFY(Dispatch("hc.viewer.quick", Archived)); Quick = qobject_cast<CQuickView*>(TargetPanel->GetFocusView(PreviewIndex)); QVERIFY(Quick);
        Surface = Quick->findChild<QtViewerSurface*>(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("alpha"));
        TargetPanel->CloseTab(); QTRY_VERIFY(Quick.isNull()); QVERIFY(OriginalView.isNull()); QCOMPARE(Tabs->count(), OriginalCount); QCOMPARE(Tabs->widget(0), FirstTab.data());
        SourcePanel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        const auto AfterBytes = PrivateBytes(); PeakBytes = std::max(PeakBytes, AfterBytes); MemorySampler.stop();
        qInfo() << "Viewer product private bytes: before=" << BeforeBytes << "sampledPeak=" << PeakBytes << "afterIdle=" << AfterBytes;
    }

    void viewerLifetimeSoak()
    {
        const int Cycles = qEnvironmentVariableIntValue("HC_VIEWER_SOAK_CYCLES");
        if (!Cycles) QSKIP("Opt-in product lifetime measurement: set HC_VIEWER_SOAK_CYCLES");
        QVERIFY(Cycles >= 60 && Cycles <= 2000);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Put = [&](const QString& Name, const QByteArray& Bytes) { QFile File(Directory.filePath(Name)); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put("large.txt", QByteArray("Viewer lifetime UTF-8 한글 text line\n").repeated(8000)));
        QVERIFY(Put("document.md", QByteArray("# Viewer lifetime\n\n**Markdown** and `code`\n\n").repeated(300)));
        { QImage Picture(2048, 2048, QImage::Format_RGB32); Picture.fill(QColor(40, 100, 180)); QVERIFY(Picture.save(Directory.filePath("picture.png"))); }
        auto* Source = panels_.front(); auto* Target = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        }
        auto* Model = static_cast<CFSModel*>(model(Source));
        auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); QVERIFY(Bridge);
        const QStringList Names{"large.txt", "picture.png", "document.md"};
        const auto Focus = [&](const QString& Name) {
            for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
            {
                Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
                while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
                if (Index.model() == Model && Model->GetName(Index) == Name) { Grid->EnsureKeyboardFocusOnView(); return Index; }
            }
            return QModelIndex{};
        };
        const auto Dispatch = [&](const QString& Id, QModelIndex Index) {
            auto Future = Bridge->Dispatch(Id, {}, "test", Index); QElapsedTimer Wait; Wait.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Wait.elapsed() > 5000) return false; QTest::qWait(10); }
            return Future.get().HasValue();
        };
        struct Sample { quint64 bytes_; DWORD handles_, gdi_, user_; int widgets_; };
        const auto Measure = [] {
            PROCESS_MEMORY_COUNTERS_EX Counters{}; Counters.cb = sizeof(Counters); DWORD Handles = 0;
            const bool Valid = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters), sizeof(Counters))
                && GetProcessHandleCount(GetCurrentProcess(), &Handles);
            return Sample{Valid ? static_cast<quint64>(Counters.PrivateUsage) : 0, Handles,
                GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), static_cast<int>(QApplication::allWidgets().size())};
        };
        QFile Samples(QCoreApplication::applicationDirPath() + "/viewer-lifetime.jsonl"); QVERIFY(Samples.open(QIODevice::WriteOnly));
        QElapsedTimer Elapsed; Elapsed.start(); std::vector<Sample> Values;
        for (int Cycle = 0; Cycle < Cycles; ++Cycle)
        {
            for (const auto& Name : Names)
            {
                const auto Index = Focus(Name); QVERIFY(Index.isValid()); QVERIFY(Dispatch("hc.viewer.open", Index));
                QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full"); QVERIFY(Full);
                auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 10000);
                QCOMPARE(QString::fromStdString(Surface->Snapshot().source_.name_), Name);
                Full->reject(); QTRY_VERIFY(Full.isNull());
            }
            QVERIFY(Dispatch("hc.viewer.quick", Focus(Names.front())));
            QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex())); QVERIFY(Quick);
            auto* Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface);
            for (const auto& Name : Names)
            {
                QVERIFY(Focus(Name).isValid());
                QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady() && QString::fromStdString(Surface->Snapshot().source_.name_) == Name, 10000);
            }
            QVERIFY(Dispatch("hc.viewer.quick", Focus(Names.back()))); QTRY_VERIFY(Quick.isNull());
            QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 10000);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCoreApplication::processEvents();
            QVERIFY(main_->findChildren<QtViewerSurface*>().isEmpty());
            const auto Current = Measure(); QVERIFY(Current.bytes_ > 0); Values.push_back(Current);
            const QJsonObject Record{{"cycle", Cycle + 1}, {"elapsedMs", Elapsed.elapsed()}, {"privateBytes", static_cast<qint64>(Current.bytes_)},
                {"handles", static_cast<int>(Current.handles_)}, {"gdi", static_cast<int>(Current.gdi_)}, {"user", static_cast<int>(Current.user_)}, {"widgets", Current.widgets_}};
            const auto Line = QJsonDocument(Record).toJson(QJsonDocument::Compact) + "\r\n"; QCOMPARE(Samples.write(Line), Line.size()); QVERIFY(Samples.flush());
            if ((Cycle + 1) % 10 == 0) qInfo() << "Viewer lifetime cycle" << Cycle + 1 << "privateBytes" << Current.bytes_ << "handles" << Current.handles_;
        }
        // Compare two equally sized windows after warmup; do not trim heaps/caches to mask growth.
        const auto Median = [&](int Begin, auto Field) {
            std::vector<quint64> Window;
            for (int Index = Begin; Index < Begin + 20; ++Index) Window.push_back(Field(Values[Index]));
            std::sort(Window.begin(), Window.end()); return Window[Window.size() / 2];
        };
        const auto Early = Median(10, [](const Sample& Value) { return Value.bytes_; });
        const auto Late = Median(Cycles - 20, [](const Sample& Value) { return Value.bytes_; });
        qInfo() << "Viewer lifetime cycles" << Cycles << "milliseconds" << Elapsed.elapsed() << "warm median bytes" << Early << "final median bytes" << Late;
        QVERIFY2(Late <= Early + 16ULL * 1024 * 1024, "Post-warmup private-byte growth exceeded 16 MiB; inspect viewer-lifetime.jsonl");
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.handles_; }) <= Median(10, [](const Sample& Value) { return Value.handles_; }) + 8);
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.gdi_; }) <= Median(10, [](const Sample& Value) { return Value.gdi_; }) + 4);
        QVERIFY(Median(Cycles - 20, [](const Sample& Value) { return Value.user_; }) <= Median(10, [](const Sample& Value) { return Value.user_; }) + 4);
        QCOMPARE(Median(Cycles - 20, [](const Sample& Value) { return Value.widgets_; }), Median(10, [](const Sample& Value) { return Value.widgets_; }));
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!model(Panel)->IsLoading()); }
    }
    void configuredHostNavigation()
    {
        using namespace hc::plugins; using namespace hc::storage;
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, true));
        const auto ArchivePath = Directory.filePath("configured.hcwcx");
        { QFile File(ArchivePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        StSettings->beginGroup("PackerPlugins"); QVariantMap Original;
        for (const auto& Key : StSettings->allKeys()) Original.insert(Key, StSettings->value(Key));
        StSettings->endGroup();
        const auto Restore = qScopeGuard([&] {
            StSettings->beginGroup("PackerPlugins"); StSettings->remove("");
            for (auto It = Original.cbegin(); It != Original.cend(); ++It) StSettings->setValue(It.key(), It.value());
            StSettings->endGroup(); StSettings->sync(); TyStPlugInMgr::GetInstance()->Refresh();
        });
        StSettings->setValue("PackerPlugins/Count", 1); StSettings->setValue("PackerPlugins/0_Path", Spec.FileFullPath);
        StSettings->setValue("PackerPlugins/0_Exts", "hcwcx"); StSettings->setValue("PackerPlugins/0_HostEnabled", true);
        StSettings->setValue("PackerPlugins/0_HostManifest", Spec.hostManifest_);
        auto* Panel = panels_.front(); auto* Model = static_cast<CFSModel*>(model(Panel)); Model->SetProviderEnabled(true);
        for (bool Approved : {true, false})
        {
            StSettings->setValue("PackerPlugins/0_HostReviewed", Approved ? EncodeManifest(Spec.hostApproval_.reviewed_) : QByteArray{});
            StSettings->sync(); TyStPlugInMgr::GetInstance()->Refresh();
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {}); QTRY_VERIFY(!Model->IsLoading());
            QModelIndex Selected;
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == "configured.hcwcx") Selected = Model->index(Row, 0);
            QVERIFY(Selected.isValid()); Model->ChangeDirectory(Selected); QTRY_VERIFY(!Model->IsLoading());
            if (Approved) QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            else
            {
                QVERIFY(Model->LastNavigationError().has_value()); QCOMPARE(Model->LastNavigationError()->category_, ErrorCategory::AccessDenied);
                QVERIFY(names(Model).contains("configured.hcwcx"));
            }
            const auto Native = QDir::toNativeSeparators(Spec.FileFullPath).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        }
        {
            QMainOpts Options(main_); Options.show(); QCoreApplication::processEvents();
            auto* Categories = Options.findChild<QListWidget*>("lstOptCate"); QVERIFY(Categories);
            Categories->setCurrentRow(PAGE_PLUGIN_MGR.first); QCoreApplication::processEvents();
            auto* Table = Options.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            QVERIFY(Table->isVisible()); QVERIFY(Options.grab().save(QCoreApplication::applicationDirPath() + "/wcx-product-options.png"));
        }
        Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Model->IsLoading());
    }
    void wcxHostApprovalUi_data()
    {
        QTest::addColumn<bool>("Approve"); QTest::addColumn<bool>("RevokeDuringConfirmation"); QTest::addColumn<bool>("Wlx");
        for (const bool Wlx : {false, true})
        {
            const QByteArray Prefix = Wlx ? "wlx-" : "wcx-";
            QTest::newRow(Prefix + "approve") << true << false << Wlx;
            QTest::newRow(Prefix + "decline") << false << false << Wlx;
            QTest::newRow(Prefix + "revoke-pending-approval") << true << true << Wlx;
        }
    }
    void wcxHostApprovalUi()
    {
        QFETCH(bool, Approve); QFETCH(bool, RevokeDuringConfirmation); QFETCH(bool, Wlx);
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, true));
        if (Wlx)
        {
            hc::plugins::PluginViewerOptions Options; QVERIFY(configureWlx(Options, Directory));
            Spec.FileFullPath = Options.binaryPath_; Spec.hostManifest_ = Options.manifestPath_; Spec.hostApproval_ = Options.approval_;
        }
        QSettings Settings(Directory.filePath("settings.ini"), QSettings::IniFormat);
        const QString Group = Wlx ? "ListerPlugins/" : "PackerPlugins/"; const QString Prefix = Wlx ? "wlx" : "wcx";
        Settings.setValue(Group + "Count", 1); Settings.setValue(Group + "0_Path", Spec.FileFullPath);
        Settings.setValue(Group + (Wlx ? "0_Detect" : "0_Exts"), Wlx ? "EXT=\"TXT\"" : "hcwcx");
        QPluginHostSettings Widget(Wlx ? hc::plugins::PluginFormat::Wlx : hc::plugins::PluginFormat::Wcx);
        Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
        auto* Review = Widget.findChild<QPushButton*>(Prefix + "HostReview"); QVERIFY(Review);
        auto* Revoke = Widget.findChild<QPushButton*>(Prefix + "HostRevoke"); QVERIFY(Revoke);
        const bool PreviousNative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto Restore = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, PreviousNative); });
        bool Chosen = false, Confirmed = false, TimedOut = false, DefaultNo = false, UiPermission = false;
        QElapsedTimer Elapsed; Elapsed.start(); QTimer Driver; Driver.setInterval(20);
        connect(&Driver, &QTimer::timeout, &Widget, [&] {
            if (Elapsed.elapsed() > 10000)
            {
                TimedOut = true;
                for (auto* Dialog : Widget.findChildren<QDialog*>()) if (Dialog->isVisible()) Dialog->reject();
                return;
            }
            for (auto* Dialog : Widget.findChildren<QFileDialog*>()) if (Dialog->isVisible())
            {
                if (!Chosen) { Chosen = true; Dialog->setDirectory(QFileInfo(Spec.hostManifest_).absolutePath()); }
                if (auto* Name = Dialog->findChild<QLineEdit*>("fileNameEdit")) Name->setText(Spec.hostManifest_);
                QMetaObject::invokeMethod(Dialog, "accept", Qt::QueuedConnection);
            }
            for (auto* Message : Widget.findChildren<QMessageBox*>()) if (Message->isVisible() && !Confirmed)
            {
                Confirmed = true; DefaultNo = Message->defaultButton() == Message->button(QMessageBox::No);
                UiPermission = Message->text().contains("뷰어 화면 표시");
                if (RevokeDuringConfirmation) Revoke->click();
                auto* Choice = Message->button(Approve ? QMessageBox::Yes : QMessageBox::No);
                if (Choice) Choice->click(); else Message->reject();
            }
        });
        Driver.start(); QTest::mouseClick(Review, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(Confirmed || TimedOut, 12000); Driver.stop(); QVERIFY(!TimedOut); QVERIFY(Chosen); QVERIFY(DefaultNo);
        Widget.Save(Settings);
        if (Wlx) QVERIFY(UiPermission);
        const auto Reviewed = Settings.value(Group + "0_HostReviewed").toByteArray();
        if (Approve && !RevokeDuringConfirmation)
        {
            auto Parsed = hc::plugins::ParseManifest(Reviewed); QVERIFY(Parsed.HasValue());
            QVERIFY(Parsed.Value() == Spec.hostApproval_.reviewed_);
            QVERIFY(Settings.value(Group + "0_HostEnabled").toBool());
        }
        else QVERIFY(Reviewed.isEmpty());
        if (RevokeDuringConfirmation) QVERIFY(Settings.value(Group + "0_HostEnabled").toBool());
    }
    void wlxViewerCommandsUi_data()
    {
        QTest::addColumn<bool>("Automatic"); QTest::addColumn<bool>("NativeKeys");
        QTest::newRow("manual") << false << true; QTest::newRow("automatic") << true << true;
        // Content/approval coverage remains independent of desktop foreground
        // availability. The two native-input rows still enforce real key routing.
        QTest::newRow("automatic-content") << true << false;
    }
    void wlxViewerCommandsUi()
    {
        using namespace hc::plugins;
        QFETCH(bool, Automatic); QFETCH(bool, NativeKeys);
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(configureWlx(Options, Package));
        StSettings->beginGroup("ListerPlugins"); QVariantMap Original;
        for (const auto& Key : StSettings->allKeys()) Original.insert(Key, StSettings->value(Key));
        StSettings->endGroup();
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        const auto Restore = qScopeGuard([&] {
            Host->CloseAll();
            // Data rows also need isolation after an assertion fails: otherwise
            // findChild can select the previous row's closed viewer surface.
            for (auto* Window : main_->findChildren<QDialog*>("hc.viewer.full")) Window->close();
            for (auto* Preview : main_->findChildren<CQuickView*>())
                QMetaObject::invokeMethod(Preview, "CloseRequested", Qt::DirectConnection);
            while (!Host->IsIdle()) QTest::qWait(10);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            StSettings->beginGroup("ListerPlugins"); StSettings->remove("");
            for (auto It = Original.cbegin(); It != Original.cend(); ++It) StSettings->setValue(It.key(), It.value());
            StSettings->endGroup(); StSettings->sync(); TyStPlugInMgr::GetInstance()->Refresh();
        });
        StSettings->setValue("ListerPlugins/SelectionMode", Automatic ? "automatic" : "manual");
        // Install via the actual registration UI. The fixture deliberately has
        // no ListGetDetectString export: registration must never execute it.
        StSettings->setValue("ListerPlugins/Count", 1); StSettings->setValue("ListerPlugins/0_Path", "unrelated.wlx64");
        StSettings->setValue("ListerPlugins/0_HostEnabled", false); StSettings->sync(); TyStPlugInMgr::GetInstance()->Refresh();
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(PAGE_PLUGIN_MGR.first);
            const bool Previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            const auto RestoreDialogs = qScopeGuard([&] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, Previous); });
            bool Chosen = false, TimedOut = false; QTimer Driver; Driver.setInterval(20); QElapsedTimer Wait; Wait.start();
            connect(&Driver, &QTimer::timeout, &Dialog, [&] {
                for (auto* Picker : Dialog.findChildren<QFileDialog*>()) if (Picker->isVisible())
                {
                    if (Wait.elapsed() > 5000)
                    {
                        TimedOut = true; qWarning() << "WLX picker timeout" << Picker->directory().absolutePath() << Picker->selectedFiles();
                        Picker->reject(); continue;
                    }
                    if (!Chosen) { Chosen = true; Picker->setDirectory(Package.path()); }
                    if (auto* Name = Picker->findChild<QLineEdit*>("fileNameEdit")) Name->setText(Options.binaryPath_);
                    // QFileDialog's directory model loads asynchronously. An
                    // early accept can be ignored until the selected file exists
                    // in that model, especially in the optimized product build.
                    QMetaObject::invokeMethod(Picker, "accept", Qt::QueuedConnection);
                }
            });
            Driver.start(); Dialog.findChild<QPushButton*>("btnAddWLX")->click(); Driver.stop(); QVERIFY(Chosen); QVERIFY(!TimedOut);
            QCOMPARE(TyStPlugInMgr::GetInstance()->GetWLXCount(), 2u);
            QVERIFY(StSettings->value("ListerPlugins/1_HostEnabled").toBool());
            const auto Native = QDir::toNativeSeparators(Options.binaryPath_).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        }
        StSettings->setValue("ListerPlugins/1_HostManifest", Options.manifestPath_);
        StSettings->setValue("ListerPlugins/1_HostReviewed", EncodeManifest(Options.approval_.reviewed_)); StSettings->sync(); TyStPlugInMgr::GetInstance()->Refresh();
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(PAGE_PLUGIN_MGR.first);
            Dialog.findChild<QTableWidget*>("tblWLX")->selectRow(0); Dialog.findChild<QPushButton*>("btnRemoveWLX")->click();
            QCOMPARE(TyStPlugInMgr::GetInstance()->GetWLXCount(), 1u);
            QCOMPARE(StSettings->value("ListerPlugins/0_Path").toString(), Options.binaryPath_);
            QVERIFY(StSettings->value("ListerPlugins/0_HostEnabled").toBool());
            QCOMPARE(StSettings->value("ListerPlugins/0_HostReviewed").toByteArray(), EncodeManifest(Options.approval_.reviewed_));
        }
        StSettings->setValue("ListerPlugins/Count", 1); StSettings->setValue("ListerPlugins/0_Path", Options.binaryPath_);
        StSettings->setValue("ListerPlugins/0_HostManifest", Options.manifestPath_);
        StSettings->setValue("ListerPlugins/0_HostEnabled", true);
        StSettings->setValue("ListerPlugins/0_HostReviewed", EncodeManifest(Options.approval_.reviewed_)); StSettings->sync();
        TyStPlugInMgr::GetInstance()->Refresh();
        QTemporaryDir Content(QDir(localOnly_ ? QDir::tempPath() : unc_).filePath("hc-wlx-XXXXXX")); QVERIFY(Content.isValid());
        qInfo() << "WLX product content:" << Content.path() << "SMB:" << !localOnly_;
        { QFile File(Content.filePath("뷰어.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("WLX product content"), 19); }
        auto* Source = panels_.front(); auto* Target = panels_.back();
        for (auto* Panel : panels_)
        {
            static_cast<CFSModel*>(model(Panel))->SetProviderEnabled(true);
            Panel->ChangeCurrentPath(QDir::toNativeSeparators(Content.path()), {}); QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000);
        }
        auto* Model = model(Source); auto* Grid = qobject_cast<CGridView*>(Source->GetFocusView(Source->CurrentTabIndex())); QVERIFY(Grid);
        QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "뷰어.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); QVERIFY(QApplication::focusWidget());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F3);
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000);
        QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY2_WITH_TIMEOUT(Surface->IsReady(), Surface->Snapshot().error_
            ? Surface->Snapshot().error_->code_.c_str() : "viewer did not become ready", 5000);
        QCOMPARE(Surface->findChild<QtPluginViewerSurface*>()->IsReady(), Automatic);
        const auto SelectWlx = [](QtViewerSurface* Surface) {
            auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); if (!Menu || !Menu->menuAction()->isVisible()) return false;
            QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection);
            auto* Choice = Menu->findChild<QAction*>("hc.viewer.plugin.0"); if (!Choice) return false;
            Choice->trigger(); return true;
        };
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady()); QCOMPARE(Surface->Snapshot().source_.name_, std::string("뷰어.txt"));
        QVERIFY(Full->screen()->grabWindow(Full->winId()).save(QCoreApplication::applicationDirPath() + "/wlx-product-full.png"));
        if (NativeKeys) QVERIFY(sendWlxKey(Surface, VK_ESCAPE, false));
        else Full->close();
        QTRY_VERIFY(Full.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        Grid->EnsureKeyboardFocusOnView(); QTest::keyClick(QApplication::focusWidget(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex())), 5000);
        QPointer<CQuickView> Quick = qobject_cast<CQuickView*>(Target->GetFocusView(Target->CurrentTabIndex()));
        Surface = Quick->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QtPluginViewerSurface*>()->IsReady(), Automatic);
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady());
        QVERIFY(main_->screen()->grabWindow(main_->winId()).save(QCoreApplication::applicationDirPath() + "/wlx-product-quick.png"));
        Surface->findChild<QAction*>("hc.viewer.fallback")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("WLX product content"));
        {
            QMainOpts Dialog(main_); Dialog.show(); QCoreApplication::processEvents();
            Dialog.findChild<QListWidget*>("lstOptCate")->setCurrentRow(PAGE_PLUGIN_MGR.first);
            auto* Tabs = Dialog.findChild<QTabWidget*>("pluginHostTabs"); QVERIFY(Tabs); Tabs->setCurrentIndex(1);
            auto* Table = Dialog.findChild<QTableWidget*>("wlxHostTable"); QVERIFY(Table); QVERIFY(Table->isVisible()); QCOMPARE(Table->rowCount(), 1);
            Dialog.findChild<QPushButton*>("wlxHostRevoke")->click();
            Dialog.SaveSettings(); StSettings->sync();
            QVERIFY(StSettings->value("ListerPlugins/0_HostReviewed").toByteArray().isEmpty());
            QVERIFY(StSettings->value("ListerPlugins/0_HostEnabled").toBool());
            QVERIFY(Dialog.grab().save(QCoreApplication::applicationDirPath() + "/wlx-product-options.png"));
        }
        QSignalSpy Failed(Surface, &QtViewerSurface::Failed); QVERIFY(SelectWlx(Surface));
        QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QVERIFY(!Surface->IsReady()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        QMenu Legacy; QCOMPARE(TyStPlugInMgr::GetInstance()->ConstructWLXMenu(&Legacy, Content.filePath("뷰어.txt")), 0u);
        const auto Native = QDir::toNativeSeparators(Options.binaryPath_).toStdWString(); QVERIFY(!GetModuleHandleW(Native.c_str()));
        StSettings->setValue("ListerPlugins/0_HostReviewed", EncodeManifest(Options.approval_.reviewed_)); StSettings->sync();
        QVERIFY(SelectWlx(Surface)); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QVERIFY(Surface->findChild<QtPluginViewerSurface*>()->IsReady());
        if (NativeKeys) QVERIFY(sendWlxKey(Surface, 'Q', true));
        else QVERIFY(QMetaObject::invokeMethod(Surface, "CloseRequested", Qt::DirectConnection));
        QTRY_VERIFY(Quick.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 5000);
        for (auto* Panel : panels_) { Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 5000); }
    }
    void wcxHostSettingsPersistence()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QSettings Settings(Directory.filePath("settings.ini"), QSettings::IniFormat);
        using Selection = QtViewerSurface::PluginViewerSelectionMode;
        QCOMPARE(QPluginHostSettings::ViewerSelection(Settings), Selection::Manual);
        {
            QPluginHostSettings Wlx(hc::plugins::PluginFormat::Wlx); Wlx.Load(Settings);
            auto* Mode = Wlx.findChild<QComboBox*>("wlxSelectionMode"); QVERIFY(Mode);
            Mode->setCurrentIndex(1);
            QCOMPARE(QPluginHostSettings::ViewerSelection(Settings), Selection::Manual);
            Wlx.Save(Settings); Settings.sync();
            QSettings Reload(Settings.fileName(), QSettings::IniFormat);
            QCOMPARE(QPluginHostSettings::ViewerSelection(Reload), Selection::Automatic);
            Wlx.Load(Reload); QCOMPARE(Mode->currentIndex(), 1);
            Mode->setCurrentIndex(2); Wlx.Save(Reload); Reload.sync();
            QCOMPARE(QPluginHostSettings::ViewerSelection(Reload), Selection::Force);
            Reload.setValue("ListerPlugins/SelectionMode", "unknown"); Wlx.Load(Reload);
            QCOMPARE(Mode->currentIndex(), 0);
            Wlx.resize(720, 240); Wlx.show(); QCoreApplication::processEvents();
            QVERIFY(Wlx.grab().save(QCoreApplication::applicationDirPath() + "/wlx-selection-settings.png"));
        }
        hc::plugins::PluginManifest Manifest; Manifest.id_ = "hc.test.settings"; Manifest.version_ = "1";
        Manifest.binary_ = "fixture.wcx64"; Manifest.sha256_ = std::string(64, 'a');
        Manifest.extensions_ = {hc::plugins::Extension::ArchiveRead}; Manifest.permissions_ = {hc::plugins::Permission::ReadContent};
        const auto Reviewed = hc::plugins::EncodeManifest(Manifest);
        Settings.setValue("PackerPlugins/Count", 1); Settings.setValue("PackerPlugins/0_Path", "fixture.wcx64");
        Settings.setValue("PackerPlugins/0_Exts", "hcwcx"); Settings.setValue("PackerPlugins/0_HostEnabled", true);
        Settings.setValue("PackerPlugins/0_HostManifest", "plugin.json"); Settings.setValue("PackerPlugins/0_HostReviewed", Reviewed);
        {
            QPluginHostSettings Widget; Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
            auto* Table = Widget.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            auto* Revoke = Widget.findChild<QPushButton*>("wcxHostRevoke"); QVERIFY(Revoke);
            QTest::mouseClick(Revoke, Qt::LeftButton);
            QCOMPARE(Table->item(0, 2)->checkState(), Qt::Checked);
            // Closing/cancelling the options UI must not persist its draft.
        }
        QCOMPARE(Settings.value("PackerPlugins/0_HostReviewed").toByteArray(), Reviewed);
        QPluginHostSettings Widget; Widget.Load(Settings); Widget.resize(650, 180); Widget.show();
        auto* Table = Widget.findChild<QTableWidget*>("wcxHostTable"); QVERIFY(Table);
        auto* Revoke = Widget.findChild<QPushButton*>("wcxHostRevoke"); QVERIFY(Revoke); QTest::mouseClick(Revoke, Qt::LeftButton);
        Widget.Save(Settings); Settings.sync();
        QSettings Reload(Settings.fileName(), QSettings::IniFormat);
        QVERIFY(Reload.value("PackerPlugins/0_HostReviewed").toByteArray().isEmpty());
        QVERIFY(Reload.value("PackerPlugins/0_HostEnabled").toBool());
        QCOMPARE(Reload.value("PackerPlugins/0_Path").toString(), QString("fixture.wcx64"));
        Widget.Load(Reload); QCOMPARE(Table->item(0, 2)->checkState(), Qt::Checked);
        QVERIFY(Widget.grab().save(QCoreApplication::applicationDirPath() + "/wcx-host-settings.png"));
    }
    void dopusPluginsUi()
    {
        using namespace hc::plugins; using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Package, Root, Copied; const auto Binary = Package.filePath("dopus.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Binary));
        QFile Dll(Binary); QVERIFY(Dll.open(QIODevice::ReadOnly)); const auto Hash = QCryptographicHash::hash(Dll.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); Dll.close();
        { QFile Input(Root.filePath("sample.bitmap")); QVERIFY(Input.open(QIODevice::WriteOnly)); QCOMPARE(Input.write("dopus product"), 13); }
        const QStringList Groups{"DopusVfsPlugins", "DopusViewerPlugins", "ListerPlugins"}; QMap<QString, QVariant> Saved;
        for (const auto& Key : StSettings->allKeys()) for (const auto& Group : Groups) if (Key.startsWith(Group + '/')) Saved[Key] = StSettings->value(Key);
        auto* Panel = panels_.front(); Panel->sig_NotifyPanelActivated(); const auto PreviousTab = Panel->CurrentTabIndex();
        auto* Host = static_cast<QtViewerHost*>(main_->findChild<QObject*>("hc.viewer.host")); QVERIFY(Host);
        const auto Restore = qScopeGuard([&] {
            Host->CloseAll(); for (auto* Dialog : main_->findChildren<QDialog*>("hc.viewer.full")) Dialog->close();
            for (auto* Dialog : main_->findChildren<QDialog*>("hc.dataPluginDialog")) Dialog->close();
            while (!Host->IsIdle()) QTest::qWait(5);
            if (Panel->CurrentTabIndex() != PreviousTab) Panel->CloseTab();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QThreadPool::globalInstance()->waitForDone();
            for (const auto& Group : Groups) StSettings->remove(Group);
            for (auto It = Saved.cbegin(); It != Saved.cend(); ++It) StSettings->setValue(It.key(), It.value());
        });
        for (const auto Format : {PluginFormat::DopusVfs, PluginFormat::DopusViewer})
        {
            const bool Vfs = Format == PluginFormat::DopusVfs; const auto Group = Vfs ? QString("DopusVfsPlugins/") : QString("DopusViewerPlugins/");
            PluginManifest Manifest; Manifest.id_ = "hc.dopus.product"; Manifest.version_ = "1"; Manifest.binary_ = "dopus.dll"; Manifest.sha256_ = Hash; Manifest.format_ = Format;
            Manifest.extensions_ = Vfs ? std::set{Extension::StorageRead, Extension::StorageWrite} : std::set{Extension::Viewer, Extension::Metadata};
            Manifest.permissions_ = {Permission::ReadContent, Vfs ? Permission::WriteContent : Permission::UserInterface};
            const auto Path = Package.filePath(Vfs ? "vfs.json" : "viewer.json"); QFile Json(Path); QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Manifest)); Json.close();
            StSettings->setValue(Group + "Count", 1); StSettings->setValue(Group + "0_Path", Binary); StSettings->setValue(Group + "0_HostEnabled", true);
            StSettings->setValue(Group + "0_HostManifest", Path); StSettings->setValue(Group + "0_HostReviewed", EncodeManifest(Manifest));
            QPluginHostSettings Settings(Format); Settings.Load(*StSettings); Settings.Save(*StSettings);
            const auto Choices = QPluginHostSettings::DataChoices(*StSettings, Format); QCOMPARE(Choices.size(), 1u); QVERIFY(Choices.front().options_.approval_.approved_);
            QCOMPARE(Choices.front().options_.binaryPath_, Binary);
        }
        StSettings->setValue("ListerPlugins/Count", 0); StSettings->setValue("ListerPlugins/SelectionMode", "automatic");
        const auto ViewerChoices = QPluginHostSettings::ViewerChoices(*StSettings); QCOMPARE(ViewerChoices.size(), 1u); QCOMPARE(ViewerChoices.front().id_, QString("dopus-0"));
        QTimer Driver; Driver.setInterval(10); QElapsedTimer Timeout; Timeout.start();
        connect(&Driver, &QTimer::timeout, main_, [&] {
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Input = qobject_cast<QInputDialog*>(Widget))
            {
                if (!Input->isVisible()) continue;
                if (Timeout.elapsed() > 5000) { Input->reject(); continue; }
                if (Input->labelText().contains(tr("전체 경로"))) Input->setTextValue(Root.path());
                Input->accept();
            }
        });
        Driver.start(); auto* Open = main_->findChild<QAction*>("hc.menu.dopus.open"); QVERIFY(Open); Open->trigger(); Driver.stop();
        QTRY_VERIFY_WITH_TIMEOUT(Panel->CurrentTabIndex() != PreviousTab, 5000);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000); QVERIFY(names(Model).contains("sample.bitmap"));
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid); QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "sample.bitmap") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Copied.path()}}, "test", Selected);
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler(); QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_), 5000);
        QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, JobState::Completed);
        QFile CopyFile(Copied.filePath("sample.bitmap")); QVERIFY(CopyFile.open(QIODevice::ReadOnly)); QCOMPARE(CopyFile.readAll(), QByteArray("dopus product")); CopyFile.close();
        // Copy completion refreshes the panel model. Reacquire its index before
        // invoking a selection-based menu; the pre-copy index is no longer valid.
        QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
        Selected = {};
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "sample.bitmap") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        Timeout.restart(); Driver.start(); main_->findChild<QAction*>("hc.menu.dopus.query")->trigger(); Driver.stop();
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.dataPluginDialog"), 5000); QPointer<QDialog> Info = main_->findChild<QDialog*>("hc.dataPluginDialog");
        auto* Values = Info->findChild<QTableWidget*>(); QVERIFY(Values); QTRY_COMPARE_WITH_TIMEOUT(Values->rowCount(), 6, 5000);
        QVERIFY(Values->item(0, 1)->text().contains("fixture metadata")); Info->close(); QTRY_VERIFY(Info.isNull());
        auto View = Bridge->Dispatch("hc.viewer.open", {}, "test", Selected); QTRY_VERIFY(View.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); QVERIFY(View.get().HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.viewer.full"), 5000); QPointer<QDialog> Full = main_->findChild<QDialog*>("hc.viewer.full");
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->Snapshot().provider_.starts_with("dopus-viewer."));
        // QWidget::grab renders Qt children only. Capture the native window so
        // the separate plugin process is included in the visual evidence.
        QTest::qWait(300);
        QVERIFY(Full->screen()->grabWindow(Full->winId()).save(QCoreApplication::applicationDirPath() + "/dopus-product.png"));
        Full->close(); QTRY_VERIFY(Full.isNull()); QTRY_VERIFY_WITH_TIMEOUT(Host->IsIdle(), 4000);
        QMainOpts Settings(main_); Settings.show(); QCoreApplication::processEvents();
        QVERIFY(Settings.findChild<QTableWidget*>("dopus-vfsHostTable")); QVERIFY(Settings.findChild<QTableWidget*>("dopus-viewerHostTable"));
    }
    void dataPluginsUi()
    {
        using namespace hc::plugins; using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Directory; const auto Binary = Directory.filePath("data.dll");
        QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_tc_data_fixture.dll", Binary));
        QFile File(Binary); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Hash = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        QStringList SavedKeys; QMap<QString, QVariant> Saved;
        for (const auto& Key : StSettings->allKeys())
            if (Key.startsWith("FileSystemPlugins/") || Key.startsWith("ContentPlugins/")) { SavedKeys << Key; Saved[Key] = StSettings->value(Key); }
        const auto Restore = qScopeGuard([&] {
            StSettings->remove("FileSystemPlugins"); StSettings->remove("ContentPlugins");
            for (const auto& Key : SavedKeys) StSettings->setValue(Key, Saved[Key]);
        });
        for (const auto Format : {PluginFormat::Wfx, PluginFormat::Wdx})
        {
            const auto Group = Format == PluginFormat::Wfx ? QString("FileSystemPlugins/") : QString("ContentPlugins/");
            PluginManifest Manifest; Manifest.id_ = "hc.test.product-data"; Manifest.version_ = "1"; Manifest.binary_ = "data.dll"; Manifest.sha256_ = Hash; Manifest.format_ = Format;
            Manifest.extensions_ = Format == PluginFormat::Wfx ? std::set{Extension::StorageRead, Extension::StorageWrite} : std::set{Extension::Metadata};
            Manifest.permissions_ = {Permission::ReadContent}; if (Format == PluginFormat::Wfx) Manifest.permissions_.insert(Permission::WriteContent);
            const auto Path = Directory.filePath(Format == PluginFormat::Wfx ? "wfx.json" : "wdx.json");
            QFile Json(Path); QVERIFY(Json.open(QIODevice::WriteOnly)); Json.write(EncodeManifest(Manifest)); Json.close();
            StSettings->setValue(Group + "Count", 1); StSettings->setValue(Group + "0_Path", Binary); StSettings->setValue(Group + "0_HostEnabled", true);
            StSettings->setValue(Group + "0_HostManifest", Path); StSettings->setValue(Group + "0_HostReviewed", EncodeManifest(Manifest));
            QPluginHostSettings Settings(Format); Settings.Load(*StSettings);
            const auto Prefix = Format == PluginFormat::Wfx ? QString("wfx") : QString("wdx");
            auto* Table = Settings.findChild<QTableWidget*>(Prefix + "HostTable"); QVERIFY(Table); QCOMPARE(Table->rowCount(), 1);
            Settings.findChild<QPushButton*>(Prefix + "HostRevoke")->click();
            QVERIFY(QPluginHostSettings::DataChoices(*StSettings, Format).front().options_.approval_.approved_);
            Settings.Load(*StSettings); Settings.Save(*StSettings);
            QVERIFY(QPluginHostSettings::DataChoices(*StSettings, Format).front().options_.approval_.approved_);
        }
        auto* Panel = panels_.front(); Panel->sig_NotifyPanelActivated(); const auto PreviousTab = Panel->CurrentTabIndex();
        const auto ClosePluginUi = qScopeGuard([&] {
            for (auto* Dialog : main_->findChildren<QDialog*>("hc.dataPluginDialog")) Dialog->close();
            if (Panel->CurrentTabIndex() != PreviousTab) Panel->CloseTab();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThreadPool::globalInstance()->waitForDone();
        });
        const auto Pick = [&] {
            QTimer::singleShot(0, main_, [] {
                for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Input = qobject_cast<QInputDialog*>(Widget)) Input->accept();
            });
        };
        auto* Open = main_->findChild<QAction*>("hc.menu.wfx.open"); QVERIFY(Open); Pick(); Open->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Panel->CurrentTabIndex() != PreviousTab, 5000);
        auto* Model = static_cast<CFSModel*>(model(Panel)); QTRY_VERIFY_WITH_TIMEOUT(!Model->IsLoading(), 5000);
        QVERIFY(Model->GetRoot()->ReadLocation(Model->GetCurrentPathWithRoot()).kind_ == LocatorKind::Plugin);
        QVERIFY(names(Model).contains("hello.txt")); QVERIFY(Model->GetRoot()->WriteProvider());
        auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())); QVERIFY(Grid);
        QModelIndex Selected;
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "hello.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Directory.path()}}, "test", Selected);
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready); const auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_), 5000);
        QCOMPARE(Scheduler.Snapshot(Accepted.Value().jobId_).Value().state_, JobState::Completed);
        QFile Copied(Directory.filePath("hello.txt")); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("hello from WFX")); Copied.close();
        QTRY_VERIFY(!Model->IsLoading());
        Selected = {};
        for (int Row = 0; Row < Grid->BaseView()->modelController()->getGridRowCount(); ++Row)
        {
            Grid->BaseView()->setFocusedRowIndex(Row); auto Index = Grid->BaseView()->focusedRow().modelIndex(0);
            while (const auto Proxy = qobject_cast<const QAbstractProxyModel*>(Index.model())) Index = Proxy->mapToSource(Index);
            if (Model->GetName(Index) == "hello.txt") { Selected = Index; break; }
        }
        QVERIFY(Selected.isValid()); Grid->EnsureKeyboardFocusOnView(); Panel->sig_NotifyPanelActivated();
        auto* Query = main_->findChild<QAction*>("hc.menu.wdx.query"); QVERIFY(Query); Pick(); Query->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(main_->findChild<QDialog*>("hc.dataPluginDialog"), 5000);
        QPointer<QDialog> Info = main_->findChild<QDialog*>("hc.dataPluginDialog"); auto* Values = Info->findChild<QTableWidget*>(); QVERIFY(Values);
        QTRY_COMPARE_WITH_TIMEOUT(Values->rowCount(), 7, 5000); QCOMPARE(Values->item(3, 1)->text(), QString("한글 metadata"));
        Values->cellDoubleClicked(6, 1); QTRY_COMPARE_WITH_TIMEOUT(Values->item(6, 1)->text(), QString("resolved"), 5000);
        QVERIFY(Info->grab().save(QCoreApplication::applicationDirPath() + "/wdx-metadata.png")); Info->close(); QTRY_VERIFY(Info.isNull());
        const auto ArchivePath = Directory.filePath("data-transfer.hcwcx");
        { QFile Archive(ArchivePath); QVERIFY(Archive.open(QIODevice::WriteOnly)); Archive.write("HCWCX01"); }
        QTemporaryDir WcxPackage; TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, WcxPackage, true));
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u); QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto* Other = panels_.back(); Other->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        auto* ArchiveModel = static_cast<CFSModel*>(model(Other)); QTRY_VERIFY(!ArchiveModel->IsLoading()); ArchiveModel->SetProviderEnabled(true);
        ArchiveModel->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin))); ArchiveModel->ChangeDirectory("/"); QTRY_VERIFY(!ArchiveModel->IsLoading());
        const auto SubmitMove = [&](CFSModel* From, CFSModel* To) -> std::string {
            QModelIndex Index;
            for (int Row = 0; Row < From->rowCount(); ++Row) if (From->GetName(From->index(Row, 0)) == "hello.txt") { Index = From->index(Row, 0); break; }
            if (!Index.isValid()) return {};
            const auto Destination = Serialize(To->GetRoot()->ReadLocation(To->GetCurrentPathWithRoot()));
            auto Future = Bridge->Dispatch("hc.file.move", {{"destination", QString::fromStdString(Destination)}, {"destinationTab", To->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Destination)}}, "test", Index);
            QElapsedTimer Timer; Timer.start(); while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Timer.elapsed() > 5000) return {}; QTest::qWait(10); }
            const auto Result = Future.get(); if (!Result.HasValue()) { qWarning() << QString::fromStdString(Result.Failure().code_); return {}; } return Result.Value().jobId_;
        };
        auto Move = SubmitMove(Model, ArchiveModel); QVERIFY(!Move.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Move).Value().state_), 5000); QCOMPARE(Scheduler.Snapshot(Move).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && !ArchiveModel->IsLoading() && !names(Model).contains("hello.txt") && names(ArchiveModel).contains("hello.txt"));
        Move = SubmitMove(ArchiveModel, Model); QVERIFY(!Move.empty());
        QTRY_VERIFY_WITH_TIMEOUT(IsTerminal(Scheduler.Snapshot(Move).Value().state_), 5000); QCOMPARE(Scheduler.Snapshot(Move).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Model->IsLoading() && !ArchiveModel->IsLoading() && names(Model).contains("hello.txt") && !names(ArchiveModel).contains("hello.txt"));
        Other->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!ArchiveModel->IsLoading());
        Panel->CloseTab(); QTRY_VERIFY(Panel->CurrentTabIndex() == PreviousTab); QThreadPool::globalInstance()->waitForDone();
    }
    void archiveCommandsAndBatchUi_data()
    { QTest::addColumn<bool>("Hosted"); QTest::newRow("legacy") << false; QTest::newRow("hosted") << true; }
    void archiveCommandsAndBatchUi()
    {
        QFETCH(bool, Hosted);
        using namespace hc::storage;
        using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto ArchivePath = Directory.filePath("jobs.hcwcx");
        { QFile File(ArchivePath); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        const auto SourcePath = Directory.filePath("source"); QVERIFY(QDir().mkdir(SourcePath));
        { QFile File(SourcePath + "/new.txt"); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("archive input"), 13); }
        auto* Left = static_cast<CFSModel*>(model(panels_.front()));
        auto* Right = static_cast<CFSModel*>(model(panels_.back()));
        Left->SetProviderEnabled(true); Right->SetProviderEnabled(true);
        panels_.front()->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        panels_.back()->ChangeCurrentPath(QDir::toNativeSeparators(SourcePath), {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
        TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, Hosted));
        auto Plugin = TySpWCX(new CTCPlugInWCX); QCOMPARE(Plugin->LoadWCX(Spec).ErrorCode, 0u);
        QVERIFY(Plugin->OpenArchive(ArchivePath).Value.value_or(false));
        auto Pack = nsHC::TySpFileSystem(new nsHC::CFSPack(ArchivePath, Plugin));
        Left->AppendRoot(Pack); Left->ChangeDirectory("/"); QTRY_VERIFY(!Left->IsLoading());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto Index = [](CFSModel* Model, const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row) if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        const auto Submit = [&](const QString& Id, QVariantMap Parameters, CFSModel* Model, const QString& Name) -> std::string {
            auto Future = Bridge->Dispatch(Id, Parameters, "test", Index(Model, Name));
            QElapsedTimer Timer; Timer.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            {
                if (Timer.elapsed() >= 5000) return {};
                QTest::qWait(10);
            }
            const auto Accepted = Future.get(); return Accepted.HasValue() ? Accepted.Value().jobId_ : std::string{};
        };
        const auto Finished = [&](const std::string& Id) { const auto State = Scheduler.Snapshot(Id); return State.HasValue() && IsTerminal(State.Value().state_); };
        auto Job = Submit("hc.file.mkdir", {{"destination", "/target"}}, Left, "alpha.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && names(Left).contains("target"));
        Job = Submit("hc.file.rename", {{"destination", "renamed.txt"}}, Left, "alpha.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && names(Left).contains("renamed.txt"));
        Locator Target{LocatorKind::Archive, ArchivePath.toStdString(), {{"wcx", "/target"}}};
        Job = Submit("hc.file.move", {{"destination", QString::fromStdString(Serialize(Target))}}, Left, "renamed.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !names(Left).contains("renamed.txt"));
        QFile Before(ArchivePath); QVERIFY(Before.open(QIODevice::ReadOnly)); const auto Original = Before.readAll(); Before.close();
        Job = Submit("hc.file.delete", {}, Left, "target"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* Cancel = main_->findChild<QPushButton*>("hc.jobs.cancel"); QVERIFY(Cancel); QTest::mouseClick(Cancel, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Cancelled);
        QVERIFY(Before.open(QIODevice::ReadOnly)); QCOMPARE(Before.readAll(), Original); Before.close();
        QTest::qWait(150); QTRY_VERIFY(!Left->IsLoading());
        Job = Submit("hc.file.delete", {}, Left, "target"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Confirm))); QVERIFY(Confirm);
        QTRY_VERIFY(Confirm->isVisible()); QTest::mouseClick(Confirm, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !names(Left).contains("target"));
        Target.nesting_[0].entry_ = "/";
        const auto Parameters = [&] { return QVariantMap{{"destination", QString::fromStdString(Serialize(Target))},
            {"destinationTab", Left->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Serialize(Target))}}; };
        Job = Submit("hc.file.copy", Parameters(), Right, "new.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("new.txt"));
        Job = Submit("hc.file.copy", Parameters(), Right, "new.txt"); QVERIFY(!Job.empty());
        QTRY_COMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::WaitingForUser);
        auto* NewName = main_->findChild<QLineEdit*>("hc.jobs.newName"); QVERIFY(NewName); NewName->setText("copy.txt");
        auto* Rename = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Rename))); QVERIFY(Rename);
        QTRY_VERIFY(Rename->isVisible()); QTest::mouseClick(Rename, Qt::LeftButton);
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("copy.txt"));
        Left->ChangeDirectory("sub"); QTRY_VERIFY(!Left->IsLoading());
        auto Stale = Bridge->Dispatch("hc.file.copy", Parameters(), "test", Index(Right, "new.txt"));
        QTRY_VERIFY(Stale.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Rejected = Stale.get(); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, "archive-destination-location-changed");
        Left->ChangeDirectory(".."); QTRY_VERIFY(!Left->IsLoading());
        Right->AppendRoot(Pack); Right->ChangeDirectory("/"); QTRY_VERIFY(!Right->IsLoading());
        Right->Refresh(); QTRY_VERIFY(!Right->IsLoading());
        QVERIFY(Left->SnapshotProvider() != Right->SnapshotProvider());
        Job = Submit("hc.file.rename", {{"destination", "copy2.txt"}}, Left, "copy.txt"); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("copy2.txt") && names(Right).contains("copy2.txt"));
        const QVariantMap MoveParameters{{"destination", QString::fromStdString(Serialize(Target))},
            {"destinationTab", Right->property("hc.tabId").toString()}, {"destinationLocation", QString::fromStdString(Serialize(Target))}};
        Right->ChangeDirectory("sub"); QTRY_VERIFY(!Right->IsLoading());
        auto StaleMove = Bridge->Dispatch("hc.file.move", MoveParameters, "test", Index(Left, "new.txt"));
        QTRY_VERIFY(StaleMove.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        Rejected = StaleMove.get(); QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, "archive-destination-location-changed");
        panels_.back()->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!Right->IsLoading());
        if (!Hosted) Left->SetProviderEnabled(false);
        QTRY_VERIFY(!Left->IsLoading()); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        Left->Refresh(); QTRY_VERIFY(!Left->IsLoading()); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        QVERIFY(QFile::remove(ArchivePath)); Left->Refresh(); QTRY_VERIFY(!Left->IsLoading());
        if (Hosted)
        {
            // Failed asynchronous navigation keeps the last accepted snapshot and reports the error.
            QVERIFY(Left->LastNavigationError().has_value());
            QCOMPARE(Left->LastNavigationError()->category_, ErrorCategory::NotFound);
            QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        }
        else QVERIFY(names(Left).isEmpty());
        Left->SetProviderEnabled(true); QTRY_VERIFY(!Left->IsLoading());
        panels_.front()->ChangeCurrentPath(unc_, {}); panels_.back()->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
    }

    void archiveCrossContainerMovesUi_data()
    { archiveCommandsAndBatchUi_data(); }
    void archiveCrossContainerMovesUi()
    {
        QFETCH(bool, Hosted);
        using namespace hc::storage; using namespace hc::operations;
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Native = Directory.filePath("native"); QVERIFY(QDir().mkdir(Native));
        { QFile File(Native + "/move.txt"); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("move-content"), 12); }
        const auto A = Directory.filePath("a.hcwcx"), B = Directory.filePath("b.hcwcx");
        for (const auto& Path : {A, B}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write("HCWCX01"), 7); }
        auto* Left = static_cast<CFSModel*>(model(panels_.front())); auto* Right = static_cast<CFSModel*>(model(panels_.back()));
        Left->SetProviderEnabled(true); Right->SetProviderEnabled(true);
        panels_.front()->ChangeCurrentPath(Native, {}); panels_.back()->ChangeCurrentPath(Directory.path(), {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
        TyPlugInWCX Spec; QVERIFY(configureWcx(Spec, Directory, Hosted));
        const auto Mount = [&](CFSModel* Model, const QString& Path) -> bool {
            auto Plugin = TySpWCX(new CTCPlugInWCX);
            if (Plugin->LoadWCX(Spec).ErrorCode || !Plugin->OpenArchive(Path).Value.value_or(false)) return false;
            Model->AppendRoot(nsHC::TySpFileSystem(new nsHC::CFSPack(Path, Plugin))); Model->ChangeDirectory("/"); return true;
        };
        QVERIFY(Mount(Right, A)); QTRY_VERIFY(!Right->IsLoading());
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge(); auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto Move = [&](CFSModel* Source, const QString& Destination) -> std::string {
            QModelIndex Cursor;
            for (int Row = 0; Row < Source->rowCount(); ++Row) if (Source->GetName(Source->index(Row, 0)) == "move.txt") { Cursor = Source->index(Row, 0); break; }
            if (!Cursor.isValid()) return {};
            auto Future = Bridge->Dispatch("hc.file.move", {{"destination", Destination}}, "test", Cursor);
            QElapsedTimer Timer; Timer.start();
            while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { if (Timer.elapsed() > 5000) return {}; QTest::qWait(10); }
            auto Accepted = Future.get(); return Accepted.HasValue() ? Accepted.Value().jobId_ : std::string{};
        };
        const auto Finished = [&](const std::string& Id) { const auto State = Scheduler.Snapshot(Id); return State.HasValue() && IsTerminal(State.Value().state_); };
        auto Job = Move(Left, QString::fromStdString(Serialize({LocatorKind::Archive, A.toStdString(), {{"wcx", "/"}}}))); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Right).contains("move.txt")); QVERIFY(!QFile::exists(Native + "/move.txt"));
        panels_.front()->ChangeCurrentPath(Directory.path(), {}); QTRY_VERIFY(!Left->IsLoading());
        QVERIFY(Mount(Left, B)); QTRY_VERIFY(!Left->IsLoading());
        Job = Move(Right, QString::fromStdString(Serialize({LocatorKind::Archive, B.toStdString(), {{"wcx", "/"}}}))); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && names(Left).contains("move.txt") && !names(Right).contains("move.txt"));
        panels_.back()->ChangeCurrentPath(Native, {}); QTRY_VERIFY(!Right->IsLoading());
        Job = Move(Left, Native); QVERIFY(!Job.empty());
        QTRY_VERIFY(Finished(Job)); QCOMPARE(Scheduler.Snapshot(Job).Value().state_, JobState::Completed);
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading() && !names(Left).contains("move.txt") && names(Right).contains("move.txt"));
        QFile File(Native + "/move.txt"); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), "move-content"); File.close();
        panels_.front()->ChangeCurrentPath(unc_, {}); panels_.back()->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Left->IsLoading() && !Right->IsLoading());
    }

    void commandEntryPointsAndTabLifetime()
    {
        using namespace hc::commands;
        using namespace hc::storage;
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        QVERIFY(Bridge);
        std::vector<EventEnvelope> Events;
        auto Subscription = Bridge->Events()->Subscribe("", MakeQtExecutor(this), [&](const auto& Event) { Events.push_back(Event); });
        QVERIFY(Subscription.HasValue());
        for (const auto& Source : {QStringLiteral("menu"), QStringLiteral("toolbar"), QStringLiteral("shortcut")})
        {
            Events.clear();
            if (Source == "shortcut")
            {
                QKeyEvent Key(QEvent::KeyPress, Qt::Key_F5, Qt::NoModifier);
                QVERIFY(TyStCommandMgr::GetInstance()->ProcessKeyPressEvent(&Key, {}));
            }
            else
            {
                auto* Action = main_->findChild<QAction*>("hc." + Source + ".hc.navigate.refresh");
                QVERIFY(Action);
                Action->trigger();
            }
            const auto HasEvent = [&](const std::string& Id) {
                return std::any_of(Events.begin(), Events.end(), [&](const auto& Event) { return Event.id_ == Id; });
            };
            QTRY_VERIFY_WITH_TIMEOUT(HasEvent("navigation.completed"), 15000);
            const auto Started = std::find_if(Events.begin(), Events.end(), [](const auto& Event) { return Event.id_ == "command.started"; });
            QVERIFY(Started != Events.end());
            QCOMPARE(std::get<std::string>(Started->payload_.at("command")), std::string("hc.navigate.refresh"));
            QCOMPARE(std::get<std::string>(Started->payload_.at("source")), Source.toStdString());
            const auto Complete = std::find_if(Events.begin(), Events.end(), [](const auto& Event) { return Event.id_ == "navigation.completed"; });
            QCOMPARE(Complete->correlationId_, Started->correlationId_);
            QVERIFY(!Started->correlationId_.empty());
        }
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        auto Path = Bridge->Dispatch("hc.navigate.path", {{"path", unc_}}, "test", Model->index(0, 0));
        QTRY_VERIFY(Path.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Path.get().HasValue());
        QTRY_VERIFY(!Model->IsLoading());
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        auto Add = Bridge->Dispatch("hc.tab.new", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Add.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Add.get().HasValue());
        auto* NewModel = static_cast<CFSModel*>(model(Panel));
        QVERIFY(NewModel != Model);
        QTRY_VERIFY(!NewModel->IsLoading());
        CommandInvocation Closed;
        Closed.id_ = "hc.navigate.refresh";
        Closed.permissions_.insert("navigation");
        Closed.context_ = Bridge->Capture(NewModel->index(0, 0));
        QVERIFY(!Closed.context_.tabId_.empty());
        QPointer<CFSModel> Guard(NewModel);
        Panel->CloseTab();
        QTRY_VERIFY(Guard.isNull());
        auto Rejected = Bridge->DispatchInvocation(Closed);
        QTRY_VERIFY(Rejected.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Rejected.get().Failure().code_, std::string("tab-closed"));
        QCOMPARE(model(Panel), Model);
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m2-navigation.png"));
    }

    void queuedQtOwnerDestruction()
    {
        using namespace hc::commands;
        using namespace hc::storage;
        auto Registry = std::make_shared<CommandRegistry>();
        auto Hub = std::make_shared<EventHub>();
        auto* Owner = new QObject;
        bool Executed = false;
        QVERIFY(Registry->Register({"test"}, {{}, [&](const auto&) -> Result<CommandOutcome> { Executed = true; return CommandOutcome{}; }}, MakeQtExecutor(Owner)).HasValue());
        CommandDispatcher Dispatcher(Registry, Hub);
        CommandInvocation Invocation;
        Invocation.id_ = "test";
        Invocation.permissions_.insert("navigation");
        auto Pending = Dispatcher.Dispatch(Invocation);
        delete Owner;
        QTRY_VERIFY(Pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Pending.get().Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!Executed);
    }

    void commandCancellationCorrelation()
    {
        using namespace hc::commands;
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto* Model = static_cast<CFSModel*>(model(panels_.front()));
        std::vector<EventEnvelope> Events;
        auto Subscription = Bridge->Events()->Subscribe("", MakeQtExecutor(this), [&](const auto& Event) { Events.push_back(Event); });
        auto Provider = std::make_shared<GatedProvider>();
        auto Release = qScopeGuard([&] { Provider->release_.set_value(); });
        Model->SetRoot(nsHC::TySpFileSystem(new GatedFs(Provider)));
        Model->SetCurrentPath("\\");
        auto Started = Bridge->Dispatch("hc.navigate.refresh", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Outcome = Started.get();
        QVERIFY(Outcome.HasValue());
        QCOMPARE(Outcome.Value().status_, CommandOutcome::Status::Accepted);
        QTRY_VERIFY(Provider->started_.load());
        auto Invalid = Bridge->Dispatch("hc.navigate.path", {{"path", QStringLiteral("relative")}}, "test", Model->index(0, 0));
        QTRY_VERIFY(Invalid.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(!Invalid.get().HasValue());
        QVERIFY(Model->IsLoading());
        auto Cancelled = Bridge->Dispatch("hc.navigate.cancel", {}, "test", Model->index(0, 0));
        QTRY_VERIFY(Cancelled.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QVERIFY(Cancelled.get().HasValue());
        QVERIFY(!Model->IsLoading());
        const auto HasTerminal = [&] {
            return std::any_of(Events.begin(), Events.end(), [&](const auto& Event) {
                return Event.id_ == "navigation.failed" && Event.correlationId_ == Outcome.Value().jobId_;
            });
        };
        QTRY_VERIFY(HasTerminal());
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        QVERIFY(Model->property("hc.correlation").toString().isEmpty());
    }

    void fileCommandsAndProgressUi()
    {
        using namespace hc::operations;
        using namespace hc::commands;
        using namespace hc::storage;
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        const auto Source = Directory.filePath("source");
        const auto Destination = Directory.filePath("destination");
        QVERIFY(QDir().mkpath(Source)); QVERIFY(QDir().mkpath(Destination));
        for (const auto& Name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")})
        { QFile File(Source + '/' + Name); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Name.toUtf8()), Name.size()); }
        auto* Panel = panels_.front();
        auto* Model = static_cast<CFSModel*>(model(Panel));
        Model->SetProviderEnabled(true);
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Bridge = TyStCommandMgr::GetInstance()->Bridge();
        auto& Scheduler = Bridge->FileJobs()->Scheduler();
        const auto IndexFor = [&](const QString& Name) {
            for (int Row = 0; Row < Model->rowCount(); ++Row)
                if (Model->GetName(Model->index(Row, 0)) == Name) return Model->index(Row, 0);
            return QModelIndex{};
        };
        for (const auto& Name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")})
        {
            const auto Proxy = qobject_cast<QAbstractProxyModel*>(View->BaseView()->model());
            const auto Row = View->BaseView()->getRow(Proxy ? Proxy->mapFromSource(IndexFor(Name)) : IndexFor(Name));
            QVERIFY(Row.isValid()); View->BaseView()->selectRow(Row.rowIndex(), Qtitan::Select);
        }
        QCOMPARE(View->BaseView()->selection()->selectedRowIndexes().size(), 2);
        auto Copy = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", IndexFor("first.txt"));
        QTRY_VERIFY(Copy.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Accepted = Copy.get(); QVERIFY(Accepted.HasValue());
        QCOMPARE(Accepted.Value().status_, CommandOutcome::Status::Accepted);
        const auto Copied = Accepted.Value().jobId_;
        QTRY_VERIFY(IsTerminal(Scheduler.Snapshot(Copied).Value().state_));
        QCOMPARE(Scheduler.Snapshot(Copied).Value().state_, JobState::Completed);
        QCOMPARE(Scheduler.Snapshot(Copied).Value().completedItems_, 2u);
        QVERIFY(QFile::exists(Destination + "/first.txt")); QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());

        // Captured context is rejected after navigation; permission checks run first.
        CommandInvocation Stale;
        Stale.id_ = "hc.file.delete"; Stale.context_ = Bridge->Capture(IndexFor("first.txt"));
        auto Denied = Bridge->DispatchInvocation(Stale);
        QCOMPARE(Denied.get().Failure().category_, ErrorCategory::AccessDenied);
        Stale.permissions_.insert("file-write");
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Destination), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto Rejected = Bridge->DispatchInvocation(Stale);
        QTRY_VERIFY(Rejected.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Rejected.get().Failure().code_, std::string("navigation-context-changed"));
        QVERIFY(QFile::exists(Source + "/first.txt"));

        auto Rename = Bridge->Dispatch("hc.file.rename", {{"destination", Destination + "/renamed.txt"}}, "test", IndexFor("first.txt"));
        QTRY_VERIFY(Rename.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Renamed = Rename.get(); QVERIFY(Renamed.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Renamed.Value().jobId_).Value().state_, JobState::Completed);
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        QVERIFY(QFile::exists(Destination + "/renamed.txt"));
        auto Delete = Bridge->Dispatch("hc.file.delete", {}, "test", IndexFor("renamed.txt"));
        QTRY_VERIFY(Delete.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Deleted = Delete.get(); QVERIFY(Deleted.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::WaitingForUser);
        QVERIFY(QFile::exists(Destination + "/renamed.txt"));
        auto* Confirm = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Confirm)));
        QVERIFY(Confirm); QTRY_VERIFY(Confirm->isVisible());
        QTest::mouseClick(Confirm, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(Deleted.Value().jobId_).Value().state_, JobState::Completed);
        QVERIFY(!QFile::exists(Destination + "/renamed.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());

        auto Cancel = Bridge->Dispatch("hc.file.delete", {}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Cancel.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Cancelled = Cancel.get(); QVERIFY(Cancelled.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Cancelled.Value().jobId_).Value().state_, JobState::WaitingForUser);
        auto* CancelButton = main_->findChild<QPushButton*>("hc.jobs.cancel");
        QVERIFY(CancelButton); QTest::mouseClick(CancelButton, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(Cancelled.Value().jobId_).Value().state_, JobState::Cancelled);
        QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Source), {});
        QTRY_VERIFY(!Model->IsLoading());
        auto Conflict = Bridge->Dispatch("hc.file.copy", {{"destination", Destination}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Conflict.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Conflicting = Conflict.get(); QVERIFY(Conflicting.HasValue());
        const auto ConflictId = Conflicting.Value().jobId_;
        QTRY_COMPARE(Scheduler.Snapshot(ConflictId).Value().state_, JobState::WaitingForUser);
        auto* PauseButton = main_->findChild<QPushButton*>("hc.jobs.pause");
        QVERIFY(PauseButton); QTRY_VERIFY(PauseButton->isEnabled());
        QTest::mouseClick(PauseButton, Qt::LeftButton);
        QTRY_VERIFY(Scheduler.Snapshot(ConflictId).Value().pauseRequested_);
        QTest::mouseClick(PauseButton, Qt::LeftButton);
        QTRY_VERIFY(!Scheduler.Snapshot(ConflictId).Value().pauseRequested_);
        auto* NewName = main_->findChild<QLineEdit*>("hc.jobs.newName");
        auto* RenameChoice = main_->findChild<QPushButton*>("hc.jobs.choice." + QString::number(static_cast<int>(InteractionChoice::Rename)));
        QVERIFY(NewName); QVERIFY(RenameChoice); QTRY_VERIFY(NewName->isVisible());
        NewName->setText("alternate.txt"); QTest::mouseClick(RenameChoice, Qt::LeftButton);
        QTRY_COMPARE(Scheduler.Snapshot(ConflictId).Value().state_, JobState::Completed);
        QVERIFY(QFile::exists(Destination + "/alternate.txt"));
        QVERIFY(QFile::exists(Destination + "/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto Mkdir = Bridge->Dispatch("hc.file.mkdir", {{"destination", Destination + "/folder"}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Mkdir.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Made = Mkdir.get(); QVERIFY(Made.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Made.Value().jobId_).Value().state_, JobState::Completed);
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto Move = Bridge->Dispatch("hc.file.move", {{"destination", Destination + "/folder"}}, "test", IndexFor("second.txt"));
        QTRY_VERIFY(Move.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto Moved = Move.get(); QVERIFY(Moved.HasValue());
        QTRY_COMPARE(Scheduler.Snapshot(Moved.Value().jobId_).Value().state_, JobState::Completed);
        QVERIFY(!QFile::exists(Source + "/second.txt"));
        QVERIFY(QFile::exists(Destination + "/folder/second.txt"));
        QTest::qWait(150); QTRY_VERIFY(!Model->IsLoading());
        auto* Jobs = main_->findChild<QDialog*>("hc.fileJobs");
        QVERIFY(Jobs); QVERIFY(Jobs->grab().save(QCoreApplication::applicationDirPath() + "/m4-jobs.png"));
        Jobs->hide();
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Model->IsLoading());
    }

    void materializationCopiesRemoteContent()
    {
        using namespace hc::storage;
        QTemporaryDir Remote(unc_ + "/hc-materializer-test-XXXXXX"); QVERIFY(Remote.isValid());
        QTemporaryDir Local; QVERIFY(Local.isValid());
        QFile File(Remote.filePath("원격.txt")); QVERIFY(File.open(QIODevice::WriteOnly));
        const auto Bytes = QByteArray(65536, 'r'); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        auto Reader = MakeNativeReadProvider(LocatorKind::Smb);
        auto Entry = Reader->Resolve({LocatorKind::Smb, QDir::toNativeSeparators(File.fileName()).toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        auto Service = MakeNativeMaterializer(Local.path().toStdString()); QVERIFY(Service.HasValue());
        auto Made = Service.Value()->Materialize({std::make_shared<ProviderContentSource>(Reader, Entry.Value())}, {});
        bool Tick = false; QTimer::singleShot(0, this, [&Tick] { Tick = true; });
        QTRY_VERIFY(Tick);
        QTRY_VERIFY(Made.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        auto Lease = Made.get(); QVERIFY(Lease.HasValue());
        QFile Copy(QString::fromStdString(Lease.Value()->LocalPath())); QVERIFY(Copy.open(QIODevice::ReadOnly)); QCOMPARE(Copy.readAll(), Bytes); Copy.close();
        Lease.Value().reset(); QVERIFY(Service.Value()->DrainCleanup().errors_.empty());
        QVERIFY(!QFile::exists(Copy.fileName())); QVERIFY(QFile::exists(File.fileName()));
    }

    void closingWindowCancelsWaitingFileJob()
    {
        using namespace hc::operations;
        using namespace hc::storage;
        QTemporaryDir Directory;
        QVERIFY(Directory.isValid());
        QFile File(Directory.filePath("keep.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); File.write("keep"); File.close();
        Locator At{LocatorKind::Local, QDir::toNativeSeparators(File.fileName()).toStdString()};
        auto Reader = MakeNativeReadProvider(At.kind_);
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        QWidget Window; Window.setAttribute(Qt::WA_QuitOnClose, false); Window.show();
        FileJobUi Jobs(&Window, std::make_shared<hc::commands::EventHub>());
        FileOperationRequest Request;
        Request.operation_ = hc::operations::FileOperation::Delete;
        Request.sources_.push_back({Entry.Value(), Reader, MakeNativeWritableStorage(At.kind_, Reader)});
        auto Job = Jobs.Submit(std::move(Request), {}); QVERIFY(Job.HasValue());
        QTRY_COMPARE(Jobs.Scheduler().Snapshot(Job.Value().id_).Value().state_, JobState::WaitingForUser);
        QElapsedTimer Timer; Timer.start(); Window.close();
        QVERIFY(Timer.elapsed() < 500);
        QTRY_VERIFY(Job.Value().completion_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled);
        QTRY_VERIFY(!Window.isVisible());
        QVERIFY(QFile::exists(File.fileName()));
    }

    void columnWidthLayoutAndSettings()
    {
        using namespace hc::view;
        auto* View = qobject_cast<CGridView*>(panels_.front()->GetFocusView(panels_.front()->CurrentTabIndex()));
        auto* Adapter = View->ColumnWidths();
        auto* Native = View->BaseView();
        auto* Grid = Native->grid();
        QTRY_VERIFY(Adapter->Policies().size() == 4);
        const auto NameId = Adapter->ColumnId(0);
        const auto SizeId = Adapter->ColumnId(1);
        QVERIFY(Adapter->SetPolicy(NameId, {WidthUnit::Percent, WidthMode::Fixed, 25}));
        QVERIFY(Adapter->SetPolicy(SizeId, {WidthUnit::Percent, WidthMode::Flexible, 30, 15, 45}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(2), {WidthUnit::Pixels, WidthMode::Flexible, 120}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(3), {WidthUnit::Pixels, WidthMode::Flexible, 80}));
        const auto OriginalSize = main_->size();
        const auto Reset = qScopeGuard([&] { View->setMinimumWidth(0); View->setMaximumWidth(QWIDGETSIZE_MAX); main_->resize(OriginalSize); });
        QSignalSpy ModelReset(model(panels_.front()), &QAbstractItemModel::modelReset);
        const auto FrameWidth = View->width() - Adapter->AvailableWidth();
        for (int Width : {1000, 800, 1000, 800})
        {
            View->setFixedWidth(Width + FrameWidth);
            QTRY_COMPARE(Adapter->AvailableWidth(), Width);
            QTRY_COMPARE(Native->getColumnByModelColumn(0)->width(), Width / 4);
            QCOMPARE(Native->getColumnByModelColumn(2)->width(), 120);
            QVERIFY(!Native->getColumnByModelColumn(0)->isHorSizingEnabled());
            QVERIFY(Native->getColumnByModelColumn(1)->isHorSizingEnabled());
        }
        QCOMPARE(Adapter->Policies()[NameId].value_, 25.0);
        QCOMPARE(ModelReset.count(), 0);
        Grid->viewport()->repaint();
        QRect Header, Cell;
        for (const auto& Hit : Grid->hitInfoAll())
        {
            if (Hit.columnIndex() != 0) continue;
            if (Hit.info() == Qtitan::GridHitInfo::Column) Header = Hit.rect();
            if (Hit.info() == Qtitan::GridHitInfo::Cell) Cell = Hit.rect();
        }
        QVERIFY(!Header.isEmpty()); QVERIFY(!Cell.isEmpty());
        QCOMPARE(Header.left(), Cell.left()); QCOMPARE(Header.width(), Cell.width());
        QCOMPARE(Header.width(), 200);
        QRect SizeHeader;
        for (const auto& Hit : Grid->hitInfoAll())
            if (Hit.info() == Qtitan::GridHitInfo::Column && Hit.columnIndex() == 1) SizeHeader = Hit.rect();
        QVERIFY(!SizeHeader.isEmpty());
        auto* Surface = Grid->viewport();
        const QPoint Edge(SizeHeader.right(), SizeHeader.center().y());
        const QPoint Destination = Edge + QPoint(70, 0);
        QCursor::setPos(Surface->mapToGlobal(Edge));
        QTest::mouseMove(Surface, Edge);
        QTest::mousePress(Surface, Qt::LeftButton, Qt::NoModifier, Edge);
        QCursor::setPos(Surface->mapToGlobal(Destination));
        QTest::mouseMove(Surface, Destination, 30);
        QTest::mouseRelease(Surface, Qt::LeftButton, Qt::NoModifier, Destination);
        QTRY_VERIFY(Adapter->Policies()[SizeId].value_ > 30);
        const auto Dragged = Adapter->Policies()[SizeId].value_;
        QVERIFY(Dragged <= 45);
        View->setFixedWidth(1000 + FrameWidth);
        QTRY_COMPARE(Native->getColumnByModelColumn(1)->width(), qRound(Dragged * 10));
        View->setFixedWidth(800 + FrameWidth);
        QTRY_COMPARE(Native->getColumnByModelColumn(1)->width(), qRound(Dragged * 8));
        const auto Count = Adapter->ApplyCount();
        for (int Index = 0; Index < 100; ++Index) Adapter->Request();
        QTRY_VERIFY(Adapter->ApplyCount() > Count);
        QVERIFY(Adapter->ApplyCount() - Count <= 3);
        qInfo() << "Column apply count / last nanoseconds:" << Adapter->ApplyCount() << Adapter->LastApplyNanoseconds();
        Adapter->Reload();
        QCOMPARE(Adapter->Policies()[NameId].value_, 25.0);
        auto Hidden = Adapter->Policies()[NameId]; Hidden.hidden_ = true;
        QVERIFY(Adapter->SetPolicy(NameId, Hidden));
        QTRY_VERIFY(!Native->getColumnByModelColumn(0)->isVisible());
        Hidden.hidden_ = false;
        QVERIFY(Adapter->SetPolicy(NameId, Hidden));
        QTRY_VERIFY(Native->getColumnByModelColumn(0)->isVisible());
        for (int Index = 0; Index < 4; ++Index) QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(Index), {WidthUnit::Percent, WidthMode::Fixed, 40}));
        QTRY_VERIFY(Native->horizontalScrollBar()->maximum() > 0);
        for (int Index = 0; Index < 4; ++Index) QCOMPARE(Native->getColumnByModelColumn(Index)->width(), 320);
        QVERIFY(Adapter->SetPolicy(NameId, {WidthUnit::Percent, WidthMode::Fixed, 25}));
        QVERIFY(Adapter->SetPolicy(SizeId, {WidthUnit::Percent, WidthMode::Flexible, 30, 15, 45}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(2), {WidthUnit::Pixels, WidthMode::Flexible, 120}));
        QVERIFY(Adapter->SetPolicy(Adapter->ColumnId(3), {WidthUnit::Pixels, WidthMode::Flexible, 80}));
        Native->horizontalScrollBar()->setValue(0);
        QTRY_COMPARE(Native->getColumnByModelColumn(0)->width(), 200);
        bool DialogSeen = false;
        bool InvalidRejected = false;
        QTimer::singleShot(100, this, [&] {
            for (auto* Widget : QApplication::topLevelWidgets())
                if (auto* Dialog = qobject_cast<QDialog*>(Widget); Dialog && Dialog->objectName() == "hc.columnWidthsDialog")
                {
                    DialogSeen = Dialog->grab().save(QCoreApplication::applicationDirPath() + "/m3-column-settings.png");
                    auto* Table = Dialog->findChild<QTableWidget*>();
                    qobject_cast<QDoubleSpinBox*>(Table->cellWidget(0, 4))->setValue(30);
                    Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                    InvalidRejected = Dialog->isVisible() && !Dialog->findChild<QLabel*>("hc.columnWidths.error")->text().isEmpty();
                    Dialog->reject();
                }
        });
        View->ConfigureColumns();
        QVERIFY(DialogSeen);
        QVERIFY(InvalidRejected);
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m3-column-layout.png"));
    }

    void columnResizeVisibilityAndScrollbars()
    {
        using namespace hc::view;
        auto* Panel = panels_.front();
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        auto* Adapter = View->ColumnWidths();
        auto* Native = View->BaseView();
        const auto OriginalSize = main_->size();
        QTemporaryDir Directory;
        for (int Index = 0; Index < 100; ++Index) { QFile File(Directory.filePath(QString::number(Index) + ".txt")); QVERIFY(File.open(QIODevice::WriteOnly)); }
        Panel->ChangeCurrentPath(QDir::toNativeSeparators(Directory.path()), {});
        QTRY_VERIFY(!model(Panel)->IsLoading());
        QTRY_VERIFY(Native->verticalScrollBar()->isVisible());
        Native->setFocusedRowIndex(1);
        Native->selectRow(1);
        const auto Focused = Native->focusedRowIndex();
        QSignalSpy Reset(model(Panel), &QAbstractItemModel::modelReset);
        const auto Check = [&] { return Native->getColumnByModelColumn(0)->width() == qRound(Adapter->AvailableWidth() * 0.25); };
        main_->resize(1400, 800); QTest::qWait(30); QTRY_VERIFY(Check());
        auto* Splitter = main_->findChild<QSplitter*>("splitter");
        QVERIFY(Splitter);
        Splitter->setSizes({350, 700}); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->showMaximized(); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->showNormal(); QTest::qWait(30); QTRY_VERIFY(Check());
        View->hide(); QCOMPARE(Adapter->AvailableWidth(), 0);
        View->show(); QTest::qWait(30); QTRY_VERIFY(Check());
        QEvent DpiEvent(QEvent::DevicePixelRatioChange); QCoreApplication::sendEvent(View, &DpiEvent); QTRY_VERIFY(Check());
        QCOMPARE(Native->focusedRowIndex(), Focused);
        QVERIFY(Native->modelController()->isRowSelected(Native->getRow(1)));
        QCOMPARE(Reset.count(), 0);
        QCOMPARE(Adapter->Policies()[Adapter->ColumnId(0)].value_, 25.0);
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/m3-scrollbar-layout.png"));
        Panel->ChangeCurrentPath(unc_, {}); QTRY_VERIFY(!model(Panel)->IsLoading());
        Panel->AddTab();
        auto* NewView = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        QTRY_VERIFY(NewView->ColumnWidths()->Policies().contains(NewView->ColumnWidths()->ColumnId(0)));
        QCOMPARE(NewView->ColumnWidths()->Policies()[NewView->ColumnWidths()->ColumnId(0)].value_, 25.0);
        QTRY_COMPARE(NewView->BaseView()->getColumnByModelColumn(0)->width(), qRound(NewView->ColumnWidths()->AvailableWidth() * 0.25));
        Panel->CloseTab(); QTest::qWait(30); QTRY_VERIFY(Check());
        main_->resize(OriginalSize);
    }

private:
    bool sendWlxKey(QtViewerSurface* Surface, WORD Key, bool Control)
    {
        auto* Anchor = Surface->findChild<QWidget*>("hc.wlx.native-parent"); if (!Anchor) return false;
        const auto Container = GetWindow(reinterpret_cast<HWND>(Anchor->winId()), GW_CHILD);
        const auto Child = GetWindow(Container, GW_CHILD); DWORD Process = 0;
        const auto Thread = GetWindowThreadProcessId(Child, &Process); if (!Thread || Process == GetCurrentProcessId()) return false;
        auto* Top = Surface->window(); Top->raise(); Top->activateWindow();
        const auto TopWindow = reinterpret_cast<HWND>(Top->winId());
        const auto ForegroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto TestThread = GetCurrentThreadId();
        // Test harness only: synchronize activation with the interactive input
        // queue, then detach before injecting keys. Product code never attaches
        // to another application's queue. No input is sent until both the
        // foreground window and native viewer focus are verified below.
        const bool Attached = ForegroundThread && ForegroundThread != TestThread && AttachThreadInput(TestThread, ForegroundThread, TRUE);
        SetForegroundWindow(TopWindow);
        if (Attached) AttachThreadInput(TestThread, ForegroundThread, FALSE);
        QTest::qWait(50);
        // Attachment can reset native focus independently of Qt's cached focus.
        // Re-enter the same focus path as a user selecting the viewer surface.
        auto* Wlx = Surface->findChild<QtPluginViewerSurface*>(); Wlx->clearFocus(); Wlx->setFocus(Qt::OtherFocusReason);
        QElapsedTimer Wait; Wait.start();
        for (;;)
        {
            GUITHREADINFO Info{sizeof(GUITHREADINFO)};
            if (GetGUIThreadInfo(Thread, &Info) && (Info.hwndFocus == Child || IsChild(Child, Info.hwndFocus))
                && GetForegroundWindow() == reinterpret_cast<HWND>(Top->winId())) break;
            if (Wait.elapsed() > 3000)
            {
                DWORD ForegroundProcess = 0; GetWindowThreadProcessId(GetForegroundWindow(), &ForegroundProcess);
                qWarning() << "WLX focus timeout" << quintptr(Child) << quintptr(Info.hwndFocus) << quintptr(GetForegroundWindow())
                    << quintptr(Top->winId()) << Thread << GetLastError() << "foreground PID" << ForegroundProcess << "test PID" << GetCurrentProcessId(); return false;
            }
            QTest::qWait(10);
        }
        std::vector<INPUT> Inputs;
        const auto Add = [&](WORD VirtualKey, bool Up) { INPUT Input{}; Input.type = INPUT_KEYBOARD; Input.ki.wVk = VirtualKey; Input.ki.dwFlags = Up ? KEYEVENTF_KEYUP : 0; Inputs.push_back(Input); };
        if (Control) Add(VK_CONTROL, false); Add(Key, false); Add(Key, true); if (Control) Add(VK_CONTROL, true);
        qInfo() << "WLX native keyboard focus verified, PID:" << Process << "key:" << Key << "control:" << Control;
        return SendInput(static_cast<UINT>(Inputs.size()), Inputs.data(), sizeof(INPUT)) == Inputs.size();
    }
    bool configureWlx(hc::plugins::PluginViewerOptions& Options, const QTemporaryDir& Directory)
    {
        using namespace hc::plugins;
        Options.binaryPath_ = Directory.filePath("viewer.wlx64");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_wlx_fixture.wlx64", Options.binaryPath_)) return false;
        QFile File(Options.binaryPath_); if (!File.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.test.product-wlx"; Manifest.version_ = "1"; Manifest.binary_ = "viewer.wlx64";
        Manifest.format_ = PluginFormat::Wlx; Manifest.extensions_ = {Extension::Viewer}; Manifest.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        Options.manifestPath_ = Directory.filePath("viewer.json"); Options.approval_.reviewed_ = Manifest;
        Options.approval_.approved_ = true; Options.approval_.granted_ = Manifest.permissions_;
        QFile Declaration(Options.manifestPath_); const auto Bytes = EncodeManifest(Manifest);
        return Declaration.open(QIODevice::WriteOnly) && Declaration.write(Bytes) == Bytes.size();
    }
    bool configureWcx(TyPlugInWCX& Spec, const QTemporaryDir& Directory, bool Hosted)
    {
        Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
        if (!Hosted) return true;
        using namespace hc::plugins;
        const auto Binary = Directory.filePath("fixture.wcx64");
        if (!QFile::copy(Spec.FileFullPath, Binary)) return false;
        QFile File(Binary); if (!File.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.test.product-wcx"; Manifest.version_ = "1"; Manifest.binary_ = "fixture.wcx64";
        Manifest.sha256_ = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex().toStdString(); File.close();
        Manifest.extensions_ = {Extension::ArchiveRead, Extension::ArchiveWrite};
        Manifest.permissions_ = {Permission::ReadContent, Permission::WriteContent};
        Spec.FileFullPath = Binary; Spec.hostEnabled_ = true; Spec.hostManifest_ = Directory.filePath("plugin.json");
        Spec.hostApproval_.approved_ = true; Spec.hostApproval_.reviewed_ = Manifest; Spec.hostApproval_.granted_ = Manifest.permissions_;
        QFile Declaration(Spec.hostManifest_); const auto Bytes = EncodeManifest(Manifest);
        return Declaration.open(QIODevice::WriteOnly) && Declaration.write(Bytes) == Bytes.size();
    }
    CFsModelT* model(CmpPanel* Panel) const
    {
        auto* View = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
        return View ? View->BaseModel() : nullptr;
    }

    QStringList names(CFsModelT* Model) const
    {
        QStringList Names;
        for (int Row = 0; Row < Model->rowCount(); ++Row)
        {
            const auto Node = Model->GetFileInfo(Model->index(Row, 0));
            if (Node && Node->Name_ != "..")
                Names.push_back(Node->Name_);
        }
        Names.sort();
        return Names;
    }

    QMainUI* main_ = nullptr;
    bool localOnly_ = false;
    QList<CmpPanel*> panels_;
    QString server_;
    QString share_;
    QString unc_;
};

int main(int Argc, char** Argv)
{
    if (Argc == 2 && QByteArray(Argv[1]) == "--probe-dpr")
    {
        QApplication App(Argc, Argv);
        std::cout << App.primaryScreen()->devicePixelRatio() << std::endl;
        return 0;
    }
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const auto ComResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::unique_ptr<DriveProbeFixture> DriveFixture;
    if (qEnvironmentVariableIsSet("HC_TEST_DISCONNECTED_DRIVE"))
    {
        DriveFixture = std::make_unique<DriveProbeFixture>();
        if (!DriveFixture->IsInstalled()) return 90;
    }
    StartupTimer.start();
    // Keep recovery history inside each copied test runtime, separate from the
    // installed application's persistent journal.
    QStandardPaths::setTestModeEnabled(true);
    HyperCommanderApp App(Argc, Argv);
    SmbTests Tests;
    const auto Result = QTest::qExec(&Tests, Argc, Argv);
    QThreadPool::globalInstance()->waitForDone();
    if (SUCCEEDED(ComResult))
        CoUninitialize();
    return Result;
}

#include "tstSmb.moc"
