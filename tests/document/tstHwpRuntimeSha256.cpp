#include "hwpRuntimeSha256.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <cstring>
using namespace hc::document;
namespace
{
    enum class Fault { None, Open, Property, HashLength, ObjectZero, ObjectHuge, Copied,
        Create, Update, Finish, Destroy, Close, Read, ReadAfterChunk, OversizeRead };
    thread_local Fault fault_ = Fault::None;
    thread_local int providers_ = 0, hashes_ = 0, opens_ = 0, closes_ = 0, reads_ = 0, updates_ = 0;
    thread_local qint64 bytes_ = 0;
    thread_local bool namedProvider_ = false;
    constexpr NTSTATUS Failed = static_cast<NTSTATUS>(0xc000000du);
    NTSTATUS WINAPI open(BCRYPT_ALG_HANDLE* Handle,LPCWSTR Algorithm,LPCWSTR Implementation,ULONG Flags)
    {
        ++opens_;namedProvider_=!std::wcscmp(Algorithm,BCRYPT_SHA256_ALGORITHM) && Implementation
            && !std::wcscmp(Implementation,MS_PRIMITIVE_PROVIDER) && !Flags;
        if(fault_==Fault::Open) { *Handle=nullptr;return Failed; }
        const auto Status=BCryptOpenAlgorithmProvider(Handle,Algorithm,Implementation,Flags);if(!Status)++providers_;return Status;
    }
    NTSTATUS WINAPI property(BCRYPT_HANDLE Handle,LPCWSTR Name,PUCHAR Value,ULONG Size,ULONG* Copied,ULONG Flags)
    {
        if(fault_==Fault::Property)return Failed;
        const auto Status=BCryptGetProperty(Handle,Name,Value,Size,Copied,Flags);
        if(!Status && Size==4) {
            if(fault_==Fault::HashLength && !std::wcscmp(Name,BCRYPT_HASH_LENGTH))*reinterpret_cast<DWORD*>(Value)=31;
            if(!std::wcscmp(Name,BCRYPT_OBJECT_LENGTH)) {
                if(fault_==Fault::ObjectZero)*reinterpret_cast<DWORD*>(Value)=0;
                if(fault_==Fault::ObjectHuge)*reinterpret_cast<DWORD*>(Value)=1024*1024+1;
            }
            if(fault_==Fault::Copied)*Copied=3;
        }
        return Status;
    }
    NTSTATUS WINAPI create(BCRYPT_ALG_HANDLE Algorithm,BCRYPT_HASH_HANDLE* Hash,PUCHAR Object,ULONG ObjectBytes,PUCHAR Secret,ULONG SecretBytes,ULONG Flags)
    {
        if(fault_==Fault::Create) { *Hash=nullptr;return Failed; }
        const auto Status=BCryptCreateHash(Algorithm,Hash,Object,ObjectBytes,Secret,SecretBytes,Flags);if(!Status)++hashes_;return Status;
    }
    NTSTATUS WINAPI update(BCRYPT_HASH_HANDLE Hash,PUCHAR Data,ULONG Size,ULONG Flags)
    { ++updates_;return fault_==Fault::Update?Failed:BCryptHashData(Hash,Data,Size,Flags); }
    NTSTATUS WINAPI finish(BCRYPT_HASH_HANDLE Hash,PUCHAR Digest,ULONG Size,ULONG Flags)
    { return fault_==Fault::Finish?Failed:BCryptFinishHash(Hash,Digest,Size,Flags); }
    NTSTATUS WINAPI destroy(BCRYPT_HASH_HANDLE Hash)
    {
        const auto Status=BCryptDestroyHash(Hash);if(!Status)--hashes_;
        // Fault occurs after actual cleanup so a test never leaks real CNG state.
        return fault_==Fault::Destroy?Failed:Status;
    }
    NTSTATUS WINAPI close(BCRYPT_ALG_HANDLE Algorithm,ULONG Flags)
    {
        ++closes_;const auto Status=BCryptCloseAlgorithmProvider(Algorithm,Flags);if(!Status)--providers_;
        return fault_==Fault::Close?Failed:Status;
    }
    BOOL WINAPI read(HANDLE File,LPVOID Buffer,DWORD Size,LPDWORD Count,LPOVERLAPPED Overlap)
    {
        ++reads_;
        if(fault_==Fault::Read || (fault_==Fault::ReadAfterChunk && reads_==2)) { *Count=0;SetLastError(ERROR_READ_FAULT);return FALSE; }
        if(fault_==Fault::OversizeRead) { *Count=Size+1;return TRUE; }
        const auto Ok=ReadFile(File,Buffer,Size,Count,Overlap);if(Ok)bytes_+=*Count;return Ok;
    }
    RuntimeSha256Api api()
    { RuntimeSha256Api Api;Api.open_=open;Api.property_=property;Api.create_=create;Api.add_=update;Api.finish_=finish;Api.destroy_=destroy;Api.close_=close;Api.read_=read;return Api; }
    struct File
    {
        HANDLE value_=INVALID_HANDLE_VALUE;
        explicit File(const QString& Path) { value_=CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr); }
        ~File() { if(value_!=INVALID_HANDLE_VALUE)CloseHandle(value_); }
    };
    QByteArray payload(int Size)
    { QByteArray Data(Size,Qt::Uninitialized);for(int I=0;I<Size;++I)Data[I]=char(I%251);return Data; }
    bool writeFile(const QString& Path,const QByteArray& Bytes)
    { QFile File(Path);return File.open(QIODevice::WriteOnly) && File.write(Bytes)==Bytes.size(); }
}
class RuntimeShaTests final : public QObject
{
    Q_OBJECT
private slots:
    void init()
    { QCOMPARE(providers_,0);QCOMPARE(hashes_,0);fault_=Fault::None;opens_=closes_=reads_=updates_=0;bytes_=0;namedProvider_=false; }
    void cleanup() { QCOMPARE(providers_,0);QCOMPARE(hashes_,0); }
    void knownAndBoundaryData_data()
    {
        QTest::addColumn<int>("Size");QTest::addColumn<bool>("Native");
        for(int Size:{0,1,63,64,65535,65536,65537,1048576})for(bool Native:{false,true})
            QTest::newRow(qPrintable(QString::number(Size)+(Native?"-cng":"-qt")))<<Size<<Native;
    }
    void knownAndBoundaryData()
    {
        QFETCH(int,Size);QFETCH(bool,Native);QTemporaryDir Dir;const auto Path=Dir.filePath("input");const auto Data=payload(Size);QVERIFY(writeFile(Path,Data));
        const auto Api=api();File Input(Path);QVERIFY(Input.value_!=INVALID_HANDLE_VALUE);
        HwpRuntimeSha256 Hash(Native?RuntimeHashBackend::PreferCng:RuntimeHashBackend::QtOnly,Api,true);QCOMPARE(Hash.UsesCng(),Native);
        QByteArray Hex;QCOMPARE(Hash.Read(Input.value_,Size,Hex),RuntimeHashResult::Success);
        QCOMPARE(Hex,QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex());QCOMPARE(bytes_,qint64(Size));
        QCOMPARE(Hash.Metrics().bytes_,qint64(Size));QVERIFY(Hash.Close());
        if(Native){QVERIFY(namedProvider_);QCOMPARE(opens_,1);QCOMPARE(closes_,1);}else QCOMPARE(opens_,0);
        BY_HANDLE_FILE_INFORMATION Info{};QVERIFY(GetFileInformationByHandle(Input.value_,&Info));
    }
    void providerFailureFallsBackBeforeRead_data()
    {
        QTest::addColumn<int>("Injected");
        for(auto FaultValue:{Fault::Open,Fault::Property,Fault::HashLength,Fault::ObjectZero,Fault::ObjectHuge,Fault::Copied})
            QTest::newRow(qPrintable(QString::number(int(FaultValue))))<<int(FaultValue);
    }
    void providerFailureFallsBackBeforeRead()
    {
        QFETCH(int,Injected);fault_=Fault(Injected);QTemporaryDir Dir;const auto Path=Dir.filePath("input");const auto Data=payload(65537);QVERIFY(writeFile(Path,Data));
        const auto Api=api();File Input(Path);HwpRuntimeSha256 Hash(RuntimeHashBackend::PreferCng,Api,true);
        QVERIFY(!Hash.UsesCng());QCOMPARE(reads_,0);QByteArray Hex;
        QCOMPARE(Hash.Read(Input.value_,Data.size(),Hex),RuntimeHashResult::Success);
        QCOMPARE(Hex,QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex());QCOMPARE(bytes_,qint64(Data.size()));QCOMPARE(updates_,0);QVERIFY(Hash.Close());
    }
    void failureNeverReturnsDigest_data()
    {
        QTest::addColumn<int>("Injected");QTest::addColumn<int>("Expected");
        for(auto FaultValue:{Fault::Create,Fault::Update,Fault::Finish,Fault::Destroy})
            QTest::newRow(qPrintable(QString::number(int(FaultValue))))<<int(FaultValue)<<int(RuntimeHashResult::HashFailed);
        QTest::newRow("read")<<int(Fault::Read)<<int(RuntimeHashResult::ReadFailed);
        QTest::newRow("read-after-chunk")<<int(Fault::ReadAfterChunk)<<int(RuntimeHashResult::ReadFailed);
        QTest::newRow("oversize-read")<<int(Fault::OversizeRead)<<int(RuntimeHashResult::LengthMismatch);
    }
    void failureNeverReturnsDigest()
    {
        QFETCH(int,Injected);QFETCH(int,Expected);fault_=Fault(Injected);QTemporaryDir Dir;const auto Path=Dir.filePath("input");QVERIFY(writeFile(Path,payload(65537)));
        const auto Api=api();File Input(Path);HwpRuntimeSha256 Hash(RuntimeHashBackend::PreferCng,Api,true);QVERIFY(Hash.UsesCng());QByteArray Hex="sentinel";
        QCOMPARE(int(Hash.Read(Input.value_,65537,Hex)),Expected);QVERIFY(Hex.isEmpty());QVERIFY(Hash.Close());
        BY_HANDLE_FILE_INFORMATION Info{};QVERIFY(GetFileInformationByHandle(Input.value_,&Info));
    }
    void changedLength_data()
    { QTest::addColumn<int>("Expected");QTest::newRow("short-eof")<<65538;QTest::newRow("excess-bytes")<<65536;QTest::newRow("negative")<<-1; }
    void changedLength()
    {
        QFETCH(int,Expected);QTemporaryDir Dir;const auto Path=Dir.filePath("input");QVERIFY(writeFile(Path,payload(65537)));File Input(Path);HwpRuntimeSha256 Hash;QByteArray Hex;
        QCOMPARE(Hash.Read(Input.value_,Expected,Hex),RuntimeHashResult::LengthMismatch);QVERIFY(Hex.isEmpty());QVERIFY(Hash.Close());
    }
    void cancelAndDeadline_data()
    { QTest::addColumn<int>("Mode");QTest::newRow("cancel-before-read")<<0;QTest::newRow("cancel-after-chunk")<<1;QTest::newRow("deadline-expired")<<2; }
    void cancelAndDeadline()
    {
        QFETCH(int,Mode);QTemporaryDir Dir;const auto Path=Dir.filePath("input");QVERIFY(writeFile(Path,payload(65537)));const auto Api=api();File Input(Path);
        HwpRuntimeSha256 Hash(RuntimeHashBackend::PreferCng,Api,true);int Checks=0;QDeadlineTimer Deadline(0,Qt::PreciseTimer);QByteArray Hex;
        QCOMPARE(Hash.Read(Input.value_,65537,Hex,[&]{++Checks;return Mode==0 || (Mode==1 && Checks>2) || (Mode==2 && Deadline.hasExpired());}),RuntimeHashResult::Cancelled);
        QVERIFY(Hex.isEmpty());QCOMPARE(reads_,Mode==1?1:0);QVERIFY(Hash.Close());
    }
    void digestMismatchStaysMismatch()
    {
        QTemporaryDir Dir;const auto Path=Dir.filePath("input");QVERIFY(writeFile(Path,"abc"));File Input(Path);HwpRuntimeSha256 Hash;QByteArray Hex;
        QCOMPARE(Hash.Read(Input.value_,3,Hex),RuntimeHashResult::Success);
        QCOMPARE(Hex,QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QVERIFY(Hex!=QCryptographicHash::hash(QByteArray("abd"),QCryptographicHash::Sha256).toHex());QVERIFY(Hash.Close());
    }
    void providerCleanupFailureIsReportedOnce()
    {
        fault_=Fault::Close;const auto Api=api();HwpRuntimeSha256 Hash(RuntimeHashBackend::PreferCng,Api,true);QVERIFY(Hash.UsesCng());
        QVERIFY(!Hash.Close());QVERIFY(!Hash.Close());QCOMPARE(closes_,1);QCOMPARE(providers_,0);
    }
};
QTEST_GUILESS_MAIN(RuntimeShaTests)
#include "tstHwpRuntimeSha256.moc"
