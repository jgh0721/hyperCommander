#include "UniqueLibs/document/hwpInputInspection.hpp"
#include <QBuffer>
#include <QFile>
#include <QtEndian>
#include <QtCore/private/qzipwriter_p.h>
#include <QtTest>
#include <cstring>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
using namespace hc::document;
namespace {
    template<class T> using Com = std::unique_ptr<T, void(*)(T*)>;
    QByteArray HwpHeader(quint32 Version = 0x05000300, quint32 Flags = 0)
    {
        QByteArray Header(256, '\0');
        std::memcpy(Header.data(), "HWP Document File", 17);
        qToLittleEndian<quint32>(Version, Header.data() + 32);
        qToLittleEndian<quint32>(Flags, Header.data() + 36);
        return Header;
    }
    QByteArray CfbFixture(const QByteArray& Header)
    {
        // Build a valid CFB in memory so header policy failures cannot be confused
        // with a damaged FAT, directory, or handcrafted compound-file container.
        ILockBytes* RawLock = nullptr;
        if(FAILED(CreateILockBytesOnHGlobal(nullptr, TRUE, &RawLock))) return {};
        Com<ILockBytes> Lock(RawLock, [](ILockBytes* Value) { Value->Release(); });
        IStorage* RawStorage = nullptr;
        if(FAILED(StgCreateDocfileOnILockBytes(Lock.get(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
            0, &RawStorage))) return {};
        Com<IStorage> Storage(RawStorage, [](IStorage* Value) { Value->Release(); });
        IStream* RawStream = nullptr;
        if(FAILED(Storage->CreateStream(L"FileHeader", STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
            0, 0, &RawStream))) return {};
        Com<IStream> Stream(RawStream, [](IStream* Value) { Value->Release(); });
        ULONG Written = 0;
        if(FAILED(Stream->Write(Header.constData(), static_cast<ULONG>(Header.size()), &Written))
            || Written != static_cast<ULONG>(Header.size()) || FAILED(Stream->Commit(STGC_DEFAULT))) return {};
        Stream.reset();
        if(FAILED(Storage->Commit(STGC_DEFAULT))) return {};
        Storage.reset();
        STATSTG Information{}; HGLOBAL Memory = nullptr;
        if(FAILED(Lock->Stat(&Information, STATFLAG_NONAME)) || Information.cbSize.HighPart
            || !Information.cbSize.LowPart || Information.cbSize.LowPart > 64 * 1024 * 1024
            || FAILED(GetHGlobalFromILockBytes(Lock.get(), &Memory))) return {};
        const auto* Data = static_cast<const char*>(GlobalLock(Memory));
        if(!Data) return {};
        const QByteArray Bytes(Data, static_cast<qsizetype>(Information.cbSize.LowPart));
        GlobalUnlock(Memory); return Bytes;
    }
    QByteArray Zip(const QString& Extra={}) {
        QByteArray Bytes; QBuffer Buffer(&Bytes); Buffer.open(QIODevice::WriteOnly);
        QZipWriter Writer(&Buffer); Writer.addFile("Contents/content.hpf",QByteArray("<package/>"));
        Writer.addFile("Contents/header.xml",QByteArray("<header/>"));
        if(!Extra.isEmpty()) Writer.addFile(Extra,QByteArray("x")); Writer.close(); return Bytes;
    }
}
class HwpInspectionTests:public QObject {
    Q_OBJECT
    HRESULT comInitialization_ = E_FAIL;
private slots:
    void initTestCase()
    {
        comInitialization_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QVERIFY(SUCCEEDED(comInitialization_) || comInitialization_ == RPC_E_CHANGED_MODE);
    }
    void cleanupTestCase()
    {
        if(SUCCEEDED(comInitialization_)) CoUninitialize();
    }
    void cfbProtection_data()
    {
        QTest::addColumn<quint32>("Flags"); QTest::addColumn<bool>("Restricted");
        QTest::newRow("plain") << quint32(0) << false;
        QTest::newRow("compressed") << quint32(0x01) << false;
        QTest::newRow("distribution") << quint32(0x04) << true;
        QTest::newRow("signature") << quint32(0x80) << true;
        QTest::newRow("drm") << quint32(0x10) << true;
        QTest::newRow("certificate-drm") << quint32(0x400) << true;
        QTest::newRow("compressed-certificate-drm") << quint32(0x401) << true;
        QTest::newRow("all-restricted") << quint32(0x494) << true;
    }
    void cfbProtection()
    {
        QFETCH(quint32, Flags); QFETCH(bool, Restricted);
        const auto Bytes = CfbFixture(HwpHeader(0x05000300, Flags)); QVERIFY(!Bytes.isEmpty());
        const auto Result = InspectHwpInput(Bytes, "hwp");
        QVERIFY2(Result.accepted_, qPrintable(Result.error_));
        QCOMPARE(Result.restricted_, Restricted); QVERIFY(Result.error_.isEmpty());
    }
    void cfbEncryptionStillRejected_data()
    {
        QTest::addColumn<quint32>("Flags");
        QTest::newRow("password") << quint32(0x02);
        QTest::newRow("certificate-encryption") << quint32(0x100);
        QTest::newRow("password-and-drm") << quint32(0x12);
    }
    void cfbEncryptionStillRejected()
    {
        QFETCH(quint32, Flags);
        const auto Bytes = CfbFixture(HwpHeader(0x05000300, Flags)); QVERIFY(!Bytes.isEmpty());
        const auto Result = InspectHwpInput(Bytes, "hwp");
        QVERIFY(!Result.accepted_); QCOMPARE(Result.error_, QString("rhwp-password-required"));
    }
    void cfbHeaderLength_data()
    {
        QTest::addColumn<int>("Length");
        for(const auto Length : {0, 17, 32, 39, 40, 255, 257})
            QTest::newRow(qPrintable(QString::number(Length))) << Length;
    }
    void cfbHeaderLength()
    {
        QFETCH(int, Length); auto Header = HwpHeader(); Header.resize(Length, '\0');
        const auto Bytes = CfbFixture(Header); QVERIFY(!Bytes.isEmpty());
        const auto Result = InspectHwpInput(Bytes, "hwp");
        QVERIFY(!Result.accepted_); QCOMPARE(Result.error_, QString("rhwp-invalid-cfb"));
    }
    void cfbUnsupportedVersion_data()
    {
        QTest::addColumn<quint32>("Version");
        QTest::newRow("zero") << quint32(0);
        QTest::newRow("major-4") << quint32(0x04000000);
        QTest::newRow("major-6") << quint32(0x06000000);
        QTest::newRow("major-255") << quint32(0xff000000);
        QTest::newRow("wrong-endian-5") << quint32(0x00000005);
    }
    void cfbUnsupportedVersion()
    {
        QFETCH(quint32, Version);
        const auto Bytes = CfbFixture(HwpHeader(Version)); QVERIFY(!Bytes.isEmpty());
        const auto Result = InspectHwpInput(Bytes, "hwp");
        QVERIFY(!Result.accepted_); QCOMPARE(Result.error_, QString("rhwp-unsupported-hwp-version"));
    }
    void cfbSupportedMajor_data()
    {
        QTest::addColumn<quint32>("Version");
        QTest::newRow("5.0.0.0") << quint32(0x05000000);
        QTest::newRow("5.0.3.0") << quint32(0x05000300);
        QTest::newRow("5.1.0.0") << quint32(0x05010000);
    }
    void cfbSupportedMajor()
    {
        QFETCH(quint32, Version);
        const auto Bytes = CfbFixture(HwpHeader(Version)); QVERIFY(!Bytes.isEmpty());
        const auto Result = InspectHwpInput(Bytes, "hwp");
        QVERIFY2(Result.accepted_, qPrintable(Result.error_)); QVERIFY(!Result.restricted_);
    }
    void validAndWrongExtension() {
        const auto Bytes=Zip(); QVERIFY(InspectHwpInput(Bytes,"hwpx").accepted_);
        QVERIFY(!InspectHwpInput(Bytes,"hwp").accepted_); QVERIFY(!InspectHwpInput("fake","hwp").accepted_);
    }
    void zipTraversalAndSignature() {
        QVERIFY(!InspectHwpInput(Zip("../escape"),"hwpx").accepted_);
        const auto Signed=InspectHwpInput(Zip("META-INF/signatures.xml"),"hwpx");
        QVERIFY(Signed.accepted_); QVERIFY(Signed.restricted_);
    }
    void corruptedAndEncrypted() {
        auto Data=Zip(); const auto Central=Data.indexOf(QByteArray::fromHex("504b0102")); QVERIFY(Central>=0);
        auto Broken=Data; Broken[Central+16]^=1; QVERIFY(!InspectHwpInput(Broken,"hwpx").accepted_);
        qToLittleEndian<quint16>(1,Data.data()+Central+8);
        QCOMPARE(InspectHwpInput(Data,"hwpx").error_,QString("rhwp-password-required"));
    }
    void forgedUncompressedSizeIsBounded() {
        QByteArray Data;QBuffer Buffer(&Data);Buffer.open(QIODevice::WriteOnly);QZipWriter Writer(&Buffer);
        Writer.setCompressionPolicy(QZipWriter::AlwaysCompress);
        Writer.addFile("Contents/content.hpf",QByteArray(1024*1024,'x'));
        Writer.addFile("Contents/header.xml",QByteArray("<header/>"));Writer.close();
        const auto Central=Data.indexOf(QByteArray::fromHex("504b0102"));QVERIFY(Central>=0);
        qToLittleEndian<quint32>(1,Data.data()+Central+24);qToLittleEndian<quint32>(1,Data.data()+22);
        QCOMPARE(InspectHwpInput(Data,"hwpx").error_,QString("rhwp-zip-output-budget-or-stream"));
    }
    void requiredXmlCannotBeDirectory() {
        auto Data=Zip();const auto Central=Data.indexOf(QByteArray::fromHex("504b0102"));QVERIFY(Central>=0);
        qToLittleEndian<quint32>(0x10,Data.data()+Central+38);QVERIFY(!InspectHwpInput(Data,"hwpx").accepted_);
    }
    void expansionBudget() {
        auto Data=Zip(); const auto Central=Data.indexOf(QByteArray::fromHex("504b0102"));
        qToLittleEndian<quint32>(17*1024*1024,Data.data()+Central+24);
        QCOMPARE(InspectHwpInput(Data,"hwpx").error_,QString("rhwp-zip-expansion-budget"));
    }
    void realCorpus_data() {
        QTest::addColumn<QString>("Name");QTest::addColumn<QString>("Format");
        QTest::newRow("body")<<"c02.hwp"<<"hwp";QTest::newRow("table")<<"c05.hwpx"<<"hwpx";
        QTest::newRow("five-page-hwp")<<"c06.hwp"<<"hwp";QTest::newRow("five-page-hwpx")<<"c07.hwpx"<<"hwpx";
    }
    void realCorpus() {
        QFETCH(QString,Name);QFETCH(QString,Format);const auto Root=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if(Root.isEmpty())QSKIP("Pinned local corpus not requested");
        QFile File(Root+'/'+Name);QVERIFY(File.open(QIODevice::ReadOnly));const auto Result=InspectHwpInput(File.readAll(),Format);
        QVERIFY2(Result.accepted_,qPrintable(Result.error_));QVERIFY(!Result.restricted_);
    }
};
QTEST_GUILESS_MAIN(HwpInspectionTests)
#include "tstHwpInputInspection.moc"
