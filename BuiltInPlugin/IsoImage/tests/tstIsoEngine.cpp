#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "hcIso/isoImage.h"
#include <memory>

namespace {
struct Close { void operator()(HcIsoImage* Image) const { hcIsoClose(Image); } };
using ImagePtr = std::unique_ptr<HcIsoImage, Close>;
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
HcIsoOptions Options(uint32_t Profile = HC_ISO_UDF_BRIDGE) { return {sizeof(HcIsoOptions), Profile, "HC_ISO", 32 * 1024 * 1024, 0, nullptr, nullptr}; }
}
class IsoEngineTests : public QObject {
    Q_OBJECT
private slots:
    void callbackInputAndChildren()
    {
        QTemporaryDir Directory; const auto Target = Directory.filePath("image.iso").toUtf8(); auto Settings = Options();
        const HcIsoSource Empty{nullptr, "parent/child", 1}; QCOMPARE(hcIsoBuild(Target.constData(), &Empty, 1, &Settings), HC_ISO_OK);
        struct Source { QByteArray bytes_; int mode_ = 0; uint64_t calls_ = 0; } Data{Read(QString::fromUtf8(Target))};
        const HcIsoInput Input{sizeof(HcIsoInput), uint64_t(Data.bytes_.size()),
            [](void* Opaque, uint64_t Offset, void* Buffer, uint32_t Capacity, uint32_t* Read) -> int {
                auto& Data = *static_cast<Source*>(Opaque); ++Data.calls_; *Read = 0;
                if (Data.mode_ == 1) return HC_ISO_CANCELLED;
                if (Data.mode_ == 2) return HC_ISO_OK; // No progress must fail, never spin.
                if (Offset > uint64_t(Data.bytes_.size())) return HC_ISO_IO;
                *Read = uint32_t(std::min({uint64_t(Capacity), uint64_t(17), uint64_t(Data.bytes_.size()) - Offset}));
                std::memcpy(Buffer, Data.bytes_.constData() + Offset, *Read); return HC_ISO_OK;
            }, &Data};
        HcIsoImage* Raw = nullptr; QCOMPARE(hcIsoOpenInput(&Input, HC_ISO_AUTO, &Raw), HC_ISO_OK); ImagePtr Image(Raw);
        QVERIFY(Data.calls_ > 100); uint64_t Parent = UINT64_MAX, Child = UINT64_MAX, Found = UINT64_MAX;
        QCOMPARE(hcIsoGetChild(Image.get(), UINT64_MAX, 0, &Parent), HC_ISO_OK);
        QCOMPARE(hcIsoFindEntry(Image.get(), "parent", &Found), HC_ISO_OK); QCOMPARE(Found, Parent);
        QCOMPARE(hcIsoGetChild(Image.get(), Parent, 0, &Child), HC_ISO_OK);
        QCOMPARE(hcIsoFindEntry(Image.get(), "parent/child", &Found), HC_ISO_OK); QCOMPARE(Found, Child);
        QCOMPARE(hcIsoGetChild(Image.get(), Parent, 1, &Found), HC_ISO_END);
        QCOMPARE(hcIsoFindEntry(Image.get(), "absent", &Found), HC_ISO_END);
        QCOMPARE(hcIsoFindEntry(Image.get(), "parent", nullptr), HC_ISO_INVALID);
        Image.reset(); Data.mode_ = 1; QCOMPARE(hcIsoOpenInput(&Input, HC_ISO_AUTO, &Raw), HC_ISO_CANCELLED); QVERIFY(!Raw);
        Data.mode_ = 2; QCOMPARE(hcIsoOpenInput(&Input, HC_ISO_AUTO, &Raw), HC_ISO_IO); QVERIFY(!Raw);
    }
    void udfWithoutJolietLongNames()
    {
        QTemporaryDir Directory; const auto Target = Directory.filePath("image.iso").toUtf8();
        const auto Disk = Directory.filePath("source").toUtf8(); QVERIFY(Put(QString::fromUtf8(Disk), "long-name-payload"));
        const auto Name = QByteArray(200, 'a') + ".dll"; const HcIsoSource Input{Disk.constData(), Name.constData(), 0};
        auto Settings = Options(); QCOMPARE(hcIsoBuild(Target.constData(), &Input, 1, &Settings), HC_ISO_UNSUPPORTED);
        Settings.profile_ = HC_ISO_UDF_NO_JOLIET; Settings.volumeLabel_ = "CCCOMA_X64FRE_KO-KR_DV9";
        QCOMPARE(hcIsoBuild(Target.constData(), &Input, 1, &Settings), HC_ISO_OK);
        const auto Bytes = Read(QString::fromUtf8(Target)); QCOMPARE(Bytes.mid(18 * 2048 + 1, 5), QByteArray("BEA01")); QCOMPARE(Bytes.mid(19 * 2048 + 1, 5), QByteArray("NSR02"));
        HcIsoImage* Raw = nullptr; QCOMPARE(hcIsoOpen(Target.constData(), HC_ISO_AUTO, &Raw), HC_ISO_OK); ImagePtr Image(Raw);
        HcIsoInfo Info{sizeof(HcIsoInfo)}; QCOMPARE(hcIsoGetInfo(Image.get(), &Info), HC_ISO_OK);
        QCOMPARE(Info.namespaces_, (1u << HC_ISO_PRIMARY) | (1u << HC_ISO_UDF)); QCOMPARE(Info.udfRevision_, 0x0102u);
        HcIsoEntry Entry{sizeof(HcIsoEntry)}; QCOMPARE(hcIsoGetEntry(Image.get(), 0, &Entry), HC_ISO_OK); QCOMPARE(QByteArray(Entry.path_), Name);
    }
    void profiles_data() { QTest::addColumn<int>("Profile"); QTest::newRow("iso9660") << 0; QTest::newRow("joliet") << 1; QTest::newRow("udf") << 2; }
    void profiles()
    {
        QFETCH(int, Profile); QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Source = Directory.filePath("source.bin").toUtf8(), Target = Directory.filePath("image.iso").toUtf8();
        QByteArray Payload(3 * 2048 + 19, Qt::Uninitialized); for (qsizetype I = 0; I < Payload.size(); ++I) Payload[I] = char(I * 31);
        QVERIFY(Put(QString::fromUtf8(Source), Payload));
        const HcIsoSource Sources[]{{nullptr, "empty", 1}, {Source.constData(), "한글/test;name.bin", 0}, {Source.constData(), "한글/TEST;NAME.BIN", 0}};
        auto Settings = Options(uint32_t(Profile));
        QCOMPARE(hcIsoBuild(Target.constData(), Sources, std::size(Sources), &Settings), HC_ISO_OK);
        const auto Original = Read(QString::fromUtf8(Target));
        QCOMPARE(hcIsoBuild(Target.constData(), Sources, std::size(Sources), &Settings), HC_ISO_IO); QCOMPARE(Read(QString::fromUtf8(Target)), Original);
        for (uint32_t Namespace = HC_ISO_PRIMARY; Namespace <= uint32_t(Profile + 1); ++Namespace) {
            HcIsoImage* Raw = nullptr; QCOMPARE(hcIsoOpen(Target.constData(), Namespace, &Raw), HC_ISO_OK); ImagePtr Image(Raw);
            HcIsoInfo Info{}; Info.structSize_ = sizeof(Info); QCOMPARE(hcIsoGetInfo(Image.get(), &Info), HC_ISO_OK);
            QCOMPARE(Info.entryCount_, 4ull); QCOMPARE(Info.selectedNamespace_, Namespace);
            if (Namespace == HC_ISO_UDF) QCOMPARE(Info.udfRevision_, 0x0102u);
            int Files = 0;
            for (uint64_t I = 0; I < Info.entryCount_; ++I) {
                HcIsoEntry Entry{}; Entry.structSize_ = sizeof(Entry); QCOMPARE(hcIsoGetEntry(Image.get(), I, &Entry), HC_ISO_OK);
                if (Entry.directory_) continue; ++Files;
                if (Namespace != HC_ISO_PRIMARY) QVERIFY(QString::fromUtf8(Entry.path_).startsWith("한글/"));
                QByteArray Bytes(Payload.size(), Qt::Uninitialized); uint32_t Count = 0;
                QCOMPARE(hcIsoRead(Image.get(), I, 0, Bytes.data(), uint32_t(Bytes.size()), &Count), HC_ISO_OK); QCOMPARE(Bytes, Payload);
                QCOMPARE(hcIsoRead(Image.get(), I, Payload.size() + 1, Bytes.data(), 1, &Count), HC_ISO_INVALID);
            }
            QCOMPARE(Files, 2);
        }
        const auto Second = Directory.filePath("second.iso").toUtf8(); QCOMPARE(hcIsoBuild(Second.constData(), Sources, std::size(Sources), &Settings), HC_ISO_OK); QCOMPARE(Read(QString::fromUtf8(Second)), Original);
    }
    void malformedDescriptors()
    {
        QTemporaryDir Directory; const auto Target = Directory.filePath("image.iso").toUtf8(); auto Settings = Options();
        const HcIsoSource Empty{nullptr, "empty", 1}; QCOMPARE(hcIsoBuild(Target.constData(), &Empty, 1, &Settings), HC_ISO_OK);
        const auto Valid = Read(QString::fromUtf8(Target));
        const auto Reject = [&](QByteArray Bytes, uint32_t Namespace) {
            const auto Bad = Directory.filePath("bad.iso").toUtf8(); if (!Put(QString::fromUtf8(Bad), Bytes)) return false;
            HcIsoImage* Raw = nullptr; const auto Status = hcIsoOpen(Bad.constData(), Namespace, &Raw); hcIsoClose(Raw); return Status != HC_ISO_OK;
        };
        QVERIFY(Reject(Valid.left(Valid.size() - 1), HC_ISO_AUTO));
        auto Endian = Valid; Endian[16 * 2048 + 84] ^= 1; QVERIFY(Reject(Endian, HC_ISO_PRIMARY));
        auto Tag = Valid; Tag[256 * 2048 + 4] ^= 1; QVERIFY(Reject(Tag, HC_ISO_UDF));
        auto Crc = Valid; Crc[35 * 2048 + 212] ^= 1; QVERIFY(Reject(Crc, HC_ISO_UDF));
        auto Fsd = Valid; Fsd[257 * 2048 + 404] ^= 1; QVERIFY(Reject(Fsd, HC_ISO_UDF));
        // A correct checksum over only the FE header must not leave allocation
        // locations outside the descriptor CRC's coverage.
        auto ShortCrc = Valid; const qsizetype RootFe = 259 * 2048;
        ShortCrc[RootFe + 10] = char(160); ShortCrc[RootFe + 11] = 0;
        uint16_t CrcValue = 0;
        for (qsizetype I = 16; I < 176; ++I) {
            CrcValue ^= uint16_t(uint8_t(ShortCrc[RootFe + I])) << 8;
            for (int Bit = 0; Bit < 8; ++Bit) CrcValue = uint16_t((CrcValue << 1) ^ ((CrcValue & 0x8000) ? 0x1021 : 0));
        }
        ShortCrc[RootFe + 8] = char(CrcValue); ShortCrc[RootFe + 9] = char(CrcValue >> 8);
        uint8_t Sum = 0; for (qsizetype I = 0; I < 16; ++I) if (I != 4) Sum += uint8_t(ShortCrc[RootFe + I]);
        ShortCrc[RootFe + 4] = char(Sum); QVERIFY(Reject(ShortCrc, HC_ISO_UDF));
    }
    void cancellationAndInputChange()
    {
        QTemporaryDir Directory; const auto Target = Directory.filePath("image.iso").toUtf8(), Source = Directory.filePath("source").toUtf8();
        QVERIFY(Put(QString::fromUtf8(Source), QByteArray(2 * 1024 * 1024, 'a')));
        const HcIsoSource Input{Source.constData(), "source", 0}; auto Settings = Options();
        Settings.progress_ = [](void*, uint64_t Done, uint64_t Total) -> int { return !(Total > 1024 && Done > 0); };
        QCOMPARE(hcIsoBuild(Target.constData(), &Input, 1, &Settings), HC_ISO_CANCELLED); QVERIFY(!QFile::exists(QString::fromUtf8(Target)));
        struct Context { QString path_; bool changed_ = false; } State{QString::fromUtf8(Source)};
        Settings.context_ = &State; Settings.progress_ = [](void* Raw, uint64_t Done, uint64_t Total) {
            auto& Value = *static_cast<Context*>(Raw); if (!Value.changed_ && Total > 1024 && Done > 0) { Value.changed_ = true; Put(Value.path_, "changed"); } return 1;
        };
        QVERIFY(hcIsoBuild(Target.constData(), &Input, 1, &Settings) != HC_ISO_OK); QVERIFY(State.changed_); QVERIFY(!QFile::exists(QString::fromUtf8(Target)));
    }
    void invalidInputs()
    {
        QTemporaryDir Directory; const auto Target = Directory.filePath("image.iso").toUtf8(); auto Settings = Options();
        for (const auto Name : {"../escape", "a/../escape", "/absolute", "a//b", "a\\b", "a:b", "a/", "a/./b"}) {
            const HcIsoSource Input{nullptr, Name, 1}; QCOMPARE(hcIsoBuild(Target.constData(), &Input, 1, &Settings), HC_ISO_INVALID); QVERIFY(!QFile::exists(QString::fromUtf8(Target)));
        }
        const HcIsoSource Duplicate[]{{nullptr, "same", 1}, {nullptr, "same", 1}};
        QCOMPARE(hcIsoBuild(Target.constData(), Duplicate, 2, &Settings), HC_ISO_INVALID);
        Settings.maxImageBytes_ = 2048; QCOMPARE(hcIsoBuild(Target.constData(), nullptr, 0, &Settings), HC_ISO_INVALID);
    }
};
QTEST_GUILESS_MAIN(IsoEngineTests)
#include "tstIsoEngine.moc"
