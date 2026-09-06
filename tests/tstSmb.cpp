#include "stdafx.h"
#include <QtTest>
#include "HyperCommander.hpp"
#include "UIs/dlgMain.hpp"
#include "UIs/cmpPanel.hpp"

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
    }

    void shareNavigation()
    {
        const auto Expected = QDir(unc_).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        for (auto* Panel : panels_)
        {
            Panel->ChangeCurrentPath(unc_, {});
            auto* Model = model(Panel);
            QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Panel->RefreshSource(Panel->CurrentTabIndex());
            QCOMPARE(names(Model), Expected);
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory("..");
            QCOMPARE(Model->GetCurrentPathWithRoot(), "\\\\" + server_);
            QVERIFY(names(Model).contains(share_));
            QThreadPool::globalInstance()->waitForDone();
            Model->ChangeDirectory(share_);
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
        QThreadPool::globalInstance()->waitForDone();
        // Characterize the legacy failure: it publishes a nonexistent directory
        // as the current path. M1 must preserve the prior snapshot on read failure.
        QEXPECT_FAIL("", "Legacy navigation publishes failed paths without a typed error", Continue);
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        bool Tick = false;
        QTimer::singleShot(0, this, [&Tick] { Tick = true; });
        QTRY_VERIFY(Tick);
        Panel->ChangeCurrentPath(unc_, {});
        QCOMPARE(Model->GetCurrentPathWithRoot(), unc_);
        QVERIFY(!names(Model).isEmpty());
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(main_->grab().save(QCoreApplication::applicationDirPath() + "/smb-panels.png"));
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
