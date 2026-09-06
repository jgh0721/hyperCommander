#include "stdafx.h"
#include <QtTest>
#include "HyperCommander.hpp"
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"
#include "memoryStorage.hpp"
#include "commandMgr.hpp"
#include "qtitanColumnAdapter.hpp"
#include "fileJobUi.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeMaterializer.hpp"
#include "storageProviders.hpp"
#include <QtitanGrid.h>
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

class SmbTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        server_ = qEnvironmentVariable("HC_SMB_SERVER", "DEVTESTER-QTS");
        share_ = qEnvironmentVariable("HC_SMB_SHARE", "Public");
        QVERIFY(!server_.contains('\\') && !share_.contains('\\'));
        unc_ = "\\\\" + server_ + "\\" + share_;
        QVERIFY2(QDir(unc_).exists(), "Live SMB share must be accessible before running this suite");
        for (auto* Window : QApplication::topLevelWidgets())
            if (auto* Main = qobject_cast<QMainUI*>(Window))
                main_ = Main;
        QVERIFY(main_);
        panels_ = main_->findChildren<CmpPanel*>();
        QCOMPARE(panels_.size(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(model(panels_.front())->GetRoot(), 5000);
        for (auto* Panel : panels_) QTRY_VERIFY_WITH_TIMEOUT(!model(Panel)->IsLoading(), 15000);
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

    void archiveCommandsAndBatchUi()
    {
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
        TyPlugInWCX Spec; Spec.FileFullPath = QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64";
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
        Left->SetProviderEnabled(false); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        Left->Refresh(); QCOMPARE(names(Left), QStringList({"copy2.txt", "new.txt", "sub"}));
        QVERIFY(QFile::remove(ArchivePath)); Left->Refresh(); QVERIFY(names(Left).isEmpty());
        Left->SetProviderEnabled(true); QTRY_VERIFY(!Left->IsLoading());
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
    QList<CmpPanel*> panels_;
    QString server_;
    QString share_;
    QString unc_;
};

int main(int Argc, char** Argv)
{
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    const auto ComResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HyperCommanderApp App(Argc, Argv);
    SmbTests Tests;
    const auto Result = QTest::qExec(&Tests, Argc, Argv);
    QThreadPool::globalInstance()->waitForDone();
    if (SUCCEEDED(ComResult))
        CoUninitialize();
    return Result;
}

#include "tstSmb.moc"
