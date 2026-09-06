#include "stdafx.h"
#include <QtTest>
#include "HyperCommander.hpp"
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"
#include "memoryStorage.hpp"
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
            Model->ChangeDirectory("..");
            QTRY_VERIFY(!Model->IsLoading());
            QCOMPARE(names(Model), QStringList({"alpha.txt", "sub"}));
            QThreadPool::globalInstance()->waitForDone();
        }
        Panel->ChangeCurrentPath(unc_, {});
        QTRY_VERIFY(!Model->IsLoading());
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
