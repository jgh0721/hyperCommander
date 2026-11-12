#include "rstTestRunner.hpp"
#include "rstProcessReaper.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QProcess>
#include <QTest>

class TestProcessReaper : public QObject
{
    Q_OBJECT
private slots:
    void completedProcessesAreDestroyed();
    void activeProcessIsReapedWithoutWaiting();
    void startingProcessIsReaped();
};

void TestProcessReaper::completedProcessesAreDestroyed()
{
    for (int i = 0; i < 100; ++i) {
        auto process = std::make_unique<QProcess>();
        QPointer<QProcess> guard(process.get());
        hc::rst::abandonProcess(std::move(process));
        QVERIFY(guard.isNull());
    }
    auto failed = std::make_unique<QProcess>();
    failed->start(QStringLiteral("mrst-nonexistent-program-reaper-test"));
    QVERIFY(!failed->waitForStarted(5000));
    QPointer<QProcess> guard(failed.get());
    hc::rst::abandonProcess(std::move(failed));
    QVERIFY(guard.isNull());
}

void TestProcessReaper::activeProcessIsReapedWithoutWaiting()
{
    auto process = std::make_unique<QProcess>();
    process->start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--process-reaper-child")});
    QVERIFY(process->waitForStarted(5000));
    QPointer<QProcess> guard(process.get());
    QElapsedTimer timer;
    timer.start();
    hc::rst::abandonProcess(std::move(process));
    QVERIFY(timer.elapsed() < 500);
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 5000);
}

void TestProcessReaper::startingProcessIsReaped()
{
    auto process = std::make_unique<QProcess>();
    process->start(QCoreApplication::applicationFilePath(),
                   {QStringLiteral("--process-reaper-child")});
    QPointer<QProcess> guard(process.get());
    hc::rst::abandonProcess(std::move(process));
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 5000);
}

HC_RST_REGISTER_TEST(TestProcessReaper);
#include "tstProcessReaper.moc"
