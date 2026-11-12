#include "rstTestRunner.hpp"
#include "rstDeferredWork.hpp"
#include "sphinxPreviewShadowSnapshot.hpp"
#include "sphinxPreviewController.hpp"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

class TestDeferredPreviewWork : public QObject
{
    Q_OBJECT
private slots:
    void coalescesBeforeBufferReads();
    void cancellationDropsPendingReads();
    void buildsKeepIndependentSnapshots();
};

void TestDeferredPreviewWork::coalescesBeforeBufferReads()
{
    hc::rst::DeferredPreviewWork work;
    int reads = 0;
    QString seen;
    QString buffer;
    for (int i = 0; i < 1000; ++i) {
        buffer = QString::number(i);
        work.schedule(25, [&] { ++reads; seen = buffer; });
    }
    QCOMPARE(reads, 0);
    QTRY_COMPARE_WITH_TIMEOUT(reads, 1, 1000);
    QCOMPARE(seen, QStringLiteral("999"));
    QTest::qWait(50);
    QCOMPARE(reads, 1);
}

void TestDeferredPreviewWork::cancellationDropsPendingReads()
{
    hc::rst::DeferredPreviewWork work;
    int reads = 0;
    work.schedule(0, [&] { ++reads; });
    work.cancel();
    QTest::qWait(30);
    QCOMPARE(reads, 0);
    {
        hc::rst::DeferredPreviewWork destroyed;
        destroyed.schedule(0, [&] { ++reads; });
    }
    QTest::qWait(30);
    QCOMPARE(reads, 0);
}

void TestDeferredPreviewWork::buildsKeepIndependentSnapshots()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    hc::rst::PreviewBuildRequest first, next;
    first.shadowSnapshot = hc::rst::PreviewShadowSnapshot::create(directory.path(), "first");
    next.shadowSnapshot = hc::rst::PreviewShadowSnapshot::create(directory.path(), "next");
    QVERIFY(first.shadowSnapshot && next.shadowSnapshot);
    first.shadowFile = first.shadowSnapshot->filePath();
    next.shadowFile = next.shadowSnapshot->filePath();
    QVERIFY(first.shadowFile != next.shadowFile);
    auto readFile = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    QCOMPARE(readFile(first.shadowFile), QByteArray("first"));
    QCOMPARE(readFile(next.shadowFile), QByteArray("next"));
    auto queued = first;
    first.shadowSnapshot.reset();
    QCOMPARE(readFile(first.shadowFile), QByteArray("first"));
    queued.shadowSnapshot.reset();
    QVERIFY(!QFileInfo::exists(first.shadowFile));
    QVERIFY(QFileInfo::exists(next.shadowFile));
    next.shadowSnapshot.reset();
    QVERIFY(!QFileInfo::exists(next.shadowFile));
    QVERIFY(!hc::rst::PreviewShadowSnapshot::create(QString{}, "no directory"));
}

HC_RST_REGISTER_TEST(TestDeferredPreviewWork);
#include "tstDeferredPreviewWork.moc"
