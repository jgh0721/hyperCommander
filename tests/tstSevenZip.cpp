#define NOMINMAX
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QtEndian>
#include "BuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/wcxMutation.hpp"
#include "BuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include "BuiltInPlugin/SevenZip/sevenZipJob.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "pluginHost/archiveCodec.hpp"
#include <Windows.h>
#include <winioctl.h>
#include <set>

using namespace hc::storage;
using namespace hc::plugins;
namespace {
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
OperationContext PasswordContext(const QString& Password) {
    OperationContext Context; Context.passwordPrompt_ = [Password](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> {
        return hc::storage::Password(Password.toStdString());
    }; return Context;
}
}
class SevenZipTests : public QObject {
    Q_OBJECT
    QJsonArray cases_;
    QString root_;
private slots:
    void mixedZipEncryption() {
        QTemporaryDir Dir; const auto Target = Dir.filePath("mixed.zip"), Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe");
        QProcess Process; Process.setWorkingDirectory(Dir.path());
        const QStringList Modes{"plain", "AES128", "AES192", "AES256", "ZipCrypto"};
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString()};
        for (const auto& Mode : Modes) {
            QVERIFY(Put(Dir.filePath(Mode), "original " + Mode.toUtf8())); QVERIFY(Put(Dir.filePath("new-" + Mode), "replacement " + Mode.toUtf8()));
            QStringList Args{"a", "-tzip", Target, Mode}; if (Mode != "plain") Args << "-psecret" << "-mem=" + Mode;
            Process.start(Cli, Args); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
            Change.add_.push_back(("new-" + Mode).toStdString()); Change.remove_.push_back(Mode.toStdString()); Change.metadataOrigins_.push_back(Mode.toStdString());
        }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Before = Read(Target);
        auto Done = Backend->Update(Change, PasswordContext("wrong")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        auto Limited = Change; Limited.maxOutputBytes_ = 20; Done = Backend->Update(Limited, PasswordContext("secret")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        Done = Backend->Update(Change, PasswordContext("secret")); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Change.archive_, true, PasswordContext("secret")); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); Listed.Value().reset(); QCOMPARE(Page.Value().entries_.size(), size_t(5));
        Process.start(Cli, {"l", "-slt", Target, "-psecret"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); const auto Listing = QString::fromUtf8(Process.readAllStandardOutput());
        for (const auto& H : Page.Value().entries_) {
            const auto Name = QString::fromStdString(H.name_), Mode = Name.mid(4); QCOMPARE(H.encrypted_, Mode != "plain");
            const auto At = Listing.indexOf("Path = " + Name + "\r\n"); QVERIFY2(At >= 0, qPrintable(Listing)); const auto Block = Listing.mid(At).section("\r\n\r\n", 0, 0);
            if (Mode != "plain") { auto Expected = Mode; if (Mode.startsWith("AES")) Expected.insert(3, '-'); QVERIFY2(Block.contains("Method = " + Expected), qPrintable(Block)); }
            Process.start(Cli, {"x", "-so", Target, Name, "-psecret"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); QCOMPARE(Process.readAllStandardOutput(), "replacement " + Mode.toUtf8());
        }
        const auto Context = PasswordContext("secret"); Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}};
        auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = "/new-plain";
        auto Entry = Reader->Resolve(At, Context).get(); QVERIFY(Entry.HasValue()); auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Batch.HasValue());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/new-plain", "/common-plain", {}, Entry.Value().id_}}, Context); QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit(Context).HasValue());
        Listed = Backend->Enumerate(Change.archive_, true, Context); QVERIFY(Listed.HasValue()); Page = Listed.Value()->Next(16, Context); QVERIFY(Page.HasValue()); bool Plain = false;
        for (const auto& H : Page.Value().entries_) if (H.name_ == "common-plain") { Plain = true; QVERIFY(!H.encrypted_); } QVERIFY(Plain);
    }
    void unknownSevenZipSecurityIsNotDiscarded() {
        // Draft ACL/unknown FilesInfo record 0x1a. The upstream decoder reports
        // UnsupportedFeatureWarning while still exposing the readable payload.
        const auto Fixture = QByteArray::fromBase64("N3q8ryccAATYnjGTEQAAAAAAAABqAAAAAAAAAP88NRtwcmVzZXJ2ZWQgcGF5bG9hZAEEBgABCREABwsBAAEBAAwRAAgKAXXiZ88AAAUBERMAawBlAGUAcAAuAHQAeAB0AAAAGQAUCgEA1B4a5zdT3QEVBgEAIIDpgRoeAAEAAAAUAAAAAQAEgAAAAAAAAAAAAAAAAAAAAAAAAAA=");
        QTemporaryDir Dir; const auto Target = Dir.filePath("security.7z"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("new"), "new"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
        QCOMPARE(Page.Value().entries_.size(), size_t(1)); QVERIFY(Backend->Extract(Target.toStdString(), Page.Value().entries_[0], Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), QByteArray("preserved payload"));
        for (const auto& Change : {ArchiveUpdate{Target.toStdString(), Dir.path().toStdString(), {"new"}}, ArchiveUpdate{Target.toStdString(), Dir.path().toStdString(), {}, {"keep.txt"}}}) {
            auto Done = Backend->Update(Change, {}); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported);
            QCOMPARE(Done.Failure().code_, std::string("7zip-unrepresented-metadata-update")); QCOMPARE(Read(Target), Fixture);
        }
    }
    void legacyZipMetadata() {
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlAVaixCBwAAAAcAAAAIACoAa2VlcC50eHRVWAwApV0NXqZdDV7oA+kDTlUWAE5VQ1gIAGsAZQBlAHAALgB0AHgAdABwYXlsb2FkUEsBAhQAFAAAAAAAgxgiUBVqLEIHAAAABwAAAAgARAAAAAAAAAAgAAAAAAAAAGtlZXAudHh0VVgIAKVdDV6mXQ1eTlU0AE5VQ1gIAA4AawBlAGUAcAAuAHQAeAB0AGwAZQBnAGEAYwB5ACAAYwBvAG0AbQBlAG4AdABQSwUGAAAAAAEAAQB6AAAAVwAAAAAA");
        QTemporaryDir Dir; const auto Target = Dir.filePath("legacy.zip"), Name = QString::fromUtf8("새 이름.txt"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath(Name), "payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {Name.toStdString()}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Bytes = Read(Target); const auto Local = Bytes.indexOf(QByteArray::fromHex("55580c00")); const auto Central = Bytes.indexOf(QByteArray::fromHex("55580800")); QVERIFY(Local >= 0 && Central >= 0);
            QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Local + 4), 1577934245u); QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 4), 1577934245u);
            const auto MTime = qFromLittleEndian<quint32>(Bytes.constData() + Local + 8); if (Step) QVERIFY(MTime > 1577934246u); else QCOMPARE(MTime, 1577934246u);
            QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 8), MTime); QCOMPARE(Bytes.mid(Local + 12, 4), QByteArray::fromHex("e803e903"));
            QByteArray Unicode(Name.size() * 2, 0); for (qsizetype I = 0; I < Name.size(); ++I) qToLittleEndian<quint16>(Name[I].unicode(), Unicode.data() + I * 2);
            QCOMPARE(Bytes.count(Unicode), 2); QVERIFY(Bytes.contains(QByteArray::fromHex("6c0065006700610063007900200063006f006d006d0065006e007400")));
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.front().name_, Name.toStdString()); QVERIFY(Backend->Extract(Change.archive_, Page.Value().entries_.front(), Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), Step ? QByteArray("replacement") : QByteArray("payload"));
            Change.remove_ = {Name.toStdString()}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath(Name), "replacement"));
        }
        auto Bad = Fixture; Bad.replace("NUCX", "BAD!"); QVERIFY(Put(Target, Bad)); Change.remove_ = {"keep.txt"}; Change.metadataOrigins_ = {"keep.txt"};
        QVERIFY(!Backend->Update(Change, {}).HasValue()); QCOMPARE(Read(Target), Bad);
    }
    void zipContentHashesAndSignatures() {
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlAVaixCBwAAAAcAAAAIABcAa2VlcC50eHRGSxMATUQ1Mhw89IbtUJFk7eweGYH+yHBheWxvYWRQSwECFAAUAAAAAACDGCJQFWosQgcAAAAHAAAACAAXAAAAAAAAACAAAAAAAAAAa2VlcC50eHRGSxMATUQ1Mhw89IbtUJFk7eweGYH+yFBLBQYAAAAAAQABAE0AAABEAAAAAAA=");
        QTemporaryDir Dir; const auto Target = Dir.filePath("hash.zip"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed"), "payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (const auto Data : {QByteArray("payload"), QByteArray("replacement")}) {
            QVERIFY(Put(Dir.filePath("renamed"), Data)); auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(Read(Target).count(QByteArray::fromHex("464b1300") + "MD5" + QCryptographicHash::hash(Data, QCryptographicHash::Md5)), 2);
            Change.remove_ = {"renamed"}; Change.metadataOrigins_.clear();
        }
        auto Signed = Fixture; Signed.replace(QByteArray::fromHex("464b1300"), QByteArray::fromHex("16001300")); QVERIFY(Put(Target, Signed));
        ArchiveUpdate Append{Target.toStdString(), Dir.path().toStdString(), {"renamed"}}; auto Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), Signed);
        // A central-directory signature and an APK signing-block marker are
        // archive-wide contracts, independent of metadata-origin mappings.
        for (bool Apk : {false, true}) {
            auto Bytes = Fixture; auto End = Bytes.indexOf(QByteArray::fromHex("504b0506")); const auto Central = qFromLittleEndian<quint32>(Bytes.constData() + End + 16);
            if (Apk) {
                QByteArray Block(32, 0); qToLittleEndian<quint64>(24, Block.data()); qToLittleEndian<quint64>(24, Block.data() + 8); Block.replace(16, 16, "APK Sig Block 42"); Bytes.insert(Central, Block); End += Block.size(); qToLittleEndian<quint32>(Central + Block.size(), Bytes.data() + End + 16);
            } else {
                const auto Block = QByteArray::fromHex("504b05050400") + "sign"; Bytes.insert(End, Block); End += Block.size(); const auto Size = qFromLittleEndian<quint32>(Bytes.constData() + End + 12); qToLittleEndian<quint32>(Size + Block.size(), Bytes.data() + End + 12);
            }
            QVERIFY(Put(Target, Bytes)); Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Bytes);
        }
        const auto Jar = Dir.filePath("signed.jar.zip"); QVERIFY(QDir().mkpath(Dir.filePath("META-INF"))); QVERIFY(Put(Dir.filePath("META-INF/TEST.SF"), "signed manifest"));
        QVERIFY(Backend->Update({Jar.toStdString(), Dir.path().toStdString(), {"META-INF/TEST.SF"}}, {}).HasValue()); const auto Before = Read(Jar); Append.archive_ = Jar.toStdString(); Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().code_, std::string("zip-jar-signature-update")); QCOMPARE(Read(Jar), Before);
    }
    void zipAlignment_data() {
        QTest::addColumn<QByteArray>("Fixture");
        QTest::newRow("apache") << qUncompress(QByteArray::fromBase64("AAAgknic7doxCsIwFIDhZ4x2EdqhglMprnoKV5dcIWAHtWAGF8HNk/QkHsJBcPAsJm0VK3iAwv+FkAfJS+Y8nlkPdSrBZTY3091yFfk4TC3PeF8ULqsesSQCAAAAAAB6y9lTebAb0ykDLK7mPPFxmFru8dFuy6y6UQYAAAAAAKCfwtc+/xQBBiqVf90A4zYjb9emN0CJJL953fLBd16UNC++80b1pvLDhfP11S9kWCOS"));
        QTest::newRow("android") << qUncompress(QByteArray::fromBase64("AAAghnic7dq9CsIwEMDxs0a7COlQwbG46toXcO2SVyi0g1qwQxfBzQfzIRykDj6LOb+wgg9Q+P9CyBFyyZzjXDY0sajjbO6mm+Uq9LFOIze7Lcs6ba9WIgEAAAAAAL1V5/tqlxeuUwZYnNxh4mOdRi62yddV2p4pAwAAAAAA0E/6tU8+RYBBEMu/boC35LVqb8BvRrdw8J0RRs+3XDYa617gR6EnH9fdAaxoInY="));
    }
    void zipAlignment() {
        QFETCH(QByteArray, Fixture); QTemporaryDir Dir; const auto Target = Dir.filePath("aligned.zip"); QVERIFY(Put(Target, Fixture));
        const auto Name = QString(180, 'n'); QVERIFY(Put(Dir.filePath(Name), QByteArray(8193, 'r'))); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {Name.toStdString()}, {"keep"}}; Change.metadataOrigins_ = {"keep"};
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        for (int Step = 0; Step < 2; ++Step) {
            const auto Bytes = Read(Target); const auto End = Bytes.lastIndexOf(QByteArray::fromHex("504b0506")); QVERIFY(End >= 0); auto At = qsizetype(qFromLittleEndian<quint32>(Bytes.constData() + End + 16));
            for (int I = 0; I < 2; ++I) {
                QCOMPARE(Bytes.mid(At, 4), QByteArray::fromHex("504b0102")); const auto Local = qFromLittleEndian<quint32>(Bytes.constData() + At + 42);
                const auto DataAt = Local + 30 + qFromLittleEndian<quint16>(Bytes.constData() + Local + 26) + qFromLittleEndian<quint16>(Bytes.constData() + Local + 28); QCOMPARE(DataAt % 4096, 0u);
                QCOMPARE(qFromLittleEndian<quint16>(Bytes.constData() + Local + 8), quint16(0));
                At += 46 + qFromLittleEndian<quint16>(Bytes.constData() + At + 28) + qFromLittleEndian<quint16>(Bytes.constData() + At + 30) + qFromLittleEndian<quint16>(Bytes.constData() + At + 32);
            }
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            for (const auto& H : Page.Value().entries_) { QVERIFY(Backend->Extract(Change.archive_, H, Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), H.name_ == "tail" ? QByteArray("tail payload") : H.name_ == "new" ? QByteArray("new") : QByteArray(8193, 'r')); }
            if (!Step) { QVERIFY(Put(Dir.filePath("new"), "new")); Done = Backend->Update({Change.archive_, Dir.path().toStdString(), {"new"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); }
        }
    }
    void codecChains_data() {
        QTest::addColumn<QStringList>("Options"); QTest::addColumn<QString>("Method");
        QTest::newRow("lzma") << QStringList{"-m0=LZMA:d1m:lc2:lp1:pb1"} << QString("LZMA:");
        QTest::newRow("ppmd") << QStringList{"-m0=PPMd:mem1m:o6"} << QString("PPMD:o6:");
        QTest::newRow("bzip2") << QStringList{"-m0=BZip2"} << QString("BZip2");
        QTest::newRow("deflate") << QStringList{"-m0=Deflate"} << QString("Deflate");
        QTest::newRow("bcj") << QStringList{"-m0=BCJ", "-m1=LZMA2:d1m"} << QString("BCJ LZMA2:");
        QTest::newRow("delta") << QStringList{"-m0=Delta:4", "-m1=LZMA2:d1m"} << QString("Delta:4 LZMA2:");
        QTest::newRow("arm64") << QStringList{"-m0=ARM64", "-m1=LZMA2:d1m"} << QString("ARM64 LZMA2:");
    }
    void codecChains() {
        QFETCH(QStringList, Options); QFETCH(QString, Method); QTemporaryDir Dir; const auto Target = Dir.filePath("codecs.7z");
        const auto Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe"); QVERIFY(Put(Dir.filePath("keep"), QByteArray(256 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("remove"), QByteArray(4096, 'r')));
        QProcess Process; Process.setWorkingDirectory(Dir.path()); Process.start(Cli, QStringList{"a", Target, "keep", "remove", "-ms=on"} + Options); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"remove"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Put(Dir.filePath("renamed"), Read(Dir.filePath("keep")))); ArchiveUpdate Rename{Target.toStdString(), Dir.path().toStdString(), {"renamed"}, {"keep"}}; Rename.metadataOrigins_ = {"keep"};
        Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        Process.start(Cli, {"l", "-slt", Target}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); const auto Listing = Process.readAllStandardOutput(); QVERIFY2(Listing.contains(Method.toUtf8()), Listing.constData());
        if (Method == "LZMA:") QVERIFY(Listing.contains(":lc2:lp1:pb1"));
        Process.start(Cli, {"x", "-so", Target, "renamed"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); QCOMPARE(Process.readAllStandardOutput(), Read(Dir.filePath("keep")));
    }
    void storedArchiveSignature() {
        QTemporaryDir Dir; const auto Name = QString("test_read_format_rar5_compressed.rar");
        QVERIFY(QFile::copy(QDir(root_).filePath(Name), Dir.filePath(Name)));
        ArchiveUpdate Create{Dir.filePath("outer.zip").toStdString(), Dir.path().toStdString(), {Name.toStdString()}};
        Create.creation_ = ArchiveCreationOptions{0, false}; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1));
        QCOMPARE(Page.Value().entries_.front().name_, Name.toStdString());
    }
    void largeFile64Bit() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for the 4 GiB acceptance test");
        QTemporaryDir Dir; const auto Source = Dir.filePath("large.bin");
        constexpr qint64 Size = (qint64(1) << 32) + 4096;
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(Source.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0; const bool Sparse = DeviceIoControl(Handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Returned, nullptr);
        LARGE_INTEGER End; End.QuadPart = Size;
        const bool Sized = Sparse && SetFilePointerEx(Handle, End, nullptr, FILE_BEGIN) && SetEndOfFile(Handle); CloseHandle(Handle); QVERIFY(Sized);
        { QFile File(Source); QVERIFY(File.open(QIODevice::ReadWrite)); QCOMPARE(File.write("head"), 4); QVERIFY(File.seek(Size - 4)); QCOMPARE(File.write("tail"), 4); }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        for (const auto* Format : {"7z", "zip"}) {
            QElapsedTimer Timer; Timer.start(); const auto Target = Dir.filePath(QString("large.") + Format);
            ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"large.bin"}}; Create.creation_ = ArchiveCreationOptions{1, false};
            auto Made = Backend->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
            const auto CreateMs = Timer.restart(); auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue());
            auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1));
            const auto Header = Page.Value().entries_.front(); QCOMPARE(Header.size_, uint64_t(Size)); Listed.Value().reset();
            const auto Restored = Dir.filePath("restored"); auto Extracted = Backend->Extract(Target.toStdString(), Header, Restored.toStdString(), {});
            QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(QFileInfo(Restored).size(), Size);
            QCryptographicHash Expected(QCryptographicHash::Sha256), Actual(QCryptographicHash::Sha256);
            QFile Input(Source), Output(Restored); QVERIFY(Input.open(QIODevice::ReadOnly)); QVERIFY(Output.open(QIODevice::ReadOnly));
            QVERIFY(Expected.addData(&Input)); QVERIFY(Actual.addData(&Output)); QCOMPARE(Actual.result(), Expected.result()); Output.close();
            qInfo() << "64-bit round trip" << Format << "bytes" << Size << "create-ms" << CreateMs << "read-verify-ms" << Timer.elapsed();
            QVERIFY(QFile::remove(Restored));
        }
    }
    void zip64EntryCount() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for the ZIP64 entry-count acceptance test");
        QTemporaryDir Dir; constexpr int Count = 65537; QElapsedTimer Timer; Timer.start();
        ArchiveUpdate Create{Dir.filePath("many.zip").toStdString(), Dir.path().toStdString()}; Create.creation_ = ArchiveCreationOptions{0, false};
        for (int I = 0; I < Count; ++I) { const auto Name = QString("entry-%1").arg(I, 6, 10, QChar('0')); QVERIFY(QDir(Dir.path()).mkdir(Name)); Create.add_.push_back(Name.toStdString() + '/'); }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Made = Backend->Update(Create, {});
        QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str()); const auto CreateMs = Timer.restart();
        auto Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue()); std::set<std::string> Names;
        for (;;) { auto Page = Listed.Value()->Next(257, {}); QVERIFY(Page.HasValue()); for (const auto& Header : Page.Value().entries_) { QVERIFY(Header.directory_); QVERIFY(Names.insert(Header.name_).second); } if (Page.Value().end_) break; }
        QCOMPARE(Names.size(), size_t(Count));
        for (int I = 0; I < Count; ++I) QVERIFY(Names.contains(QString("entry-%1").arg(I, 6, 10, QChar('0')).toStdString()));
        qInfo() << "ZIP64 entries" << Count << "input-and-create-ms" << CreateMs << "paged-read-ms" << Timer.elapsed();
        Listed.Value().reset(); QVERIFY(QDir(Dir.path()).mkdir("renamed"));
        ArchiveUpdate Rename{Create.archive_, Dir.path().toStdString(), {"renamed/"}, {"entry-000000"}}; Rename.metadataOrigins_ = {"entry-000000"};
        auto Changed = Backend->Update(Rename, {}); QVERIFY2(Changed.HasValue(), Changed.HasValue() ? "" : Changed.Failure().code_.c_str());
        Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue()); Names.clear();
        for (;;) { auto Page = Listed.Value()->Next(257, {}); QVERIFY(Page.HasValue()); for (const auto& H : Page.Value().entries_) QVERIFY(Names.insert(H.name_).second); if (Page.Value().end_) break; }
        QCOMPARE(Names.size(), size_t(Count)); QVERIFY(Names.contains("renamed")); QVERIFY(!Names.contains("entry-000000"));
    }
    void zip64OffsetMetadata() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for ZIP64 metadata offsets beyond 4 GiB");
        QTemporaryDir Dir; const auto Source = Dir.filePath("large"); constexpr qint64 Size = (qint64(1) << 32) + 4096;
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(Source.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0; LARGE_INTEGER End; End.QuadPart = Size;
        const bool Sized = DeviceIoControl(Handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Returned, nullptr) && SetFilePointerEx(Handle, End, nullptr, FILE_BEGIN) && SetEndOfFile(Handle); CloseHandle(Handle); QVERIFY(Sized);
        QVERIFY(Put(Dir.filePath("meta"), "metadata payload")); QVERIFY(Put(Dir.filePath("renamed-meta"), "metadata payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Target = Dir.filePath("offsets.zip"); QElapsedTimer Timer; Timer.start();
        ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"large", "meta"}}; Create.creation_ = ArchiveCreationOptions{0, false};
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); QVERIFY(QFileInfo(Target).size() > Size);
        ArchiveUpdate Rename{Create.archive_, Dir.path().toStdString(), {"renamed-meta"}, {"meta"}}; Rename.metadataOrigins_ = {"meta"};
        Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QFile File(Target); QVERIFY(File.open(QIODevice::ReadOnly)); QVERIFY(File.seek(File.size() - 4096)); const auto Tail = File.readAll(); File.close();
        const auto Last = Tail.lastIndexOf(QByteArray::fromHex("504b0102")); QVERIFY(Last >= 0); QCOMPARE(qFromLittleEndian<quint32>(Tail.constData() + Last + 42), UINT32_MAX);
        QVERIFY(Tail.contains(QByteArray::fromHex("504b0606"))); QVERIFY(Tail.contains("renamed-meta"));
        // The official tool reads all stored bytes and checks CRCs independently.
        QProcess Check; Check.start(QDir(root_).filePath("../sevenzip-tools/official/7z.exe"), {"t", Target, "-y"}); QVERIFY(Check.waitForFinished(120000)); QCOMPARE(Check.exitCode(), 0);
        qInfo() << "ZIP64 metadata offset round trip bytes" << Size << "elapsed-ms" << Timer.elapsed();
    }
    void creation_data() {
        QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Solid"); QTest::addColumn<bool>("Encrypt"); QTest::addColumn<unsigned>("Level");
        for (const auto Format : {QString("7z"), QString("zip")}) for (const auto Level : {0u, 5u, 9u})
            QTest::newRow(qPrintable(Format + QString::number(Level))) << Format << false << false << Level;
        QTest::newRow("solid") << QString("7z") << true << false << 5u;
        QTest::newRow("7z-aes") << QString("7z") << true << true << 5u;
        QTest::newRow("zip-aes") << QString("zip") << false << true << 5u;
    }
    void creation() {
        QFETCH(QString, Format); QFETCH(bool, Solid); QFETCH(bool, Encrypt); QFETCH(unsigned, Level);
        QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input"))); QVERIFY(QDir().mkdir(Dir.filePath("input/empty")));
        const auto Data = QByteArray(2 * 1024 * 1024, 'z'); QVERIFY(Put(Dir.filePath("input/한글.txt"), Data)); QVERIFY(Put(Dir.filePath("input/zero"), {})); QVERIFY(Put(Dir.filePath("input/second"), "second payload"));
        const auto Target = Dir.filePath("result." + Format);
        ArchiveUpdate Update{Target.toStdString(), Dir.filePath("input").toStdString(), {"한글.txt", "zero", "empty/", "second"}};
        Update.encrypt_ = Encrypt; Update.creation_ = ArchiveCreationOptions{Level, Solid};
        const auto Wire = EncodeArchiveUpdate(Update); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().creation_->level_, Level);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Context = PasswordContext("secret");
        auto Done = Backend->Update(Update, Context); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, Context); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        auto Page = Listed.Value()->Next(16, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); QCOMPARE(Page.Value().entries_.size(), size_t(4));
        Listed.Value().reset(); bool Found = false;
        for (const auto& H : Page.Value().entries_) if (H.name_ == "한글.txt") {
            Found = true; QCOMPARE(H.encrypted_, Encrypt); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), Context).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Data);
        }
        QVERIFY(Found); QVERIFY(!GetModuleHandleW(L"7z.dll"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()) + "." + Format), Read(Target))); }
    }
    void updateAndFailureAtomicity_data() { QTest::addColumn<QString>("Format"); QTest::newRow("7z") << QString("7z"); QTest::newRow("zip") << QString("zip"); }
    void updateAndFailureAtomicity() {
        QFETCH(QString, Format); QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input")));
        QVERIFY(Put(Dir.filePath("input/keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("input/remove"), "remove"));
        const auto Target = Dir.filePath("result." + Format); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.filePath("input").toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, false};
        auto Made = Backend->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
        const auto Before = Read(Target); QVERIFY(Put(Dir.filePath("input/new"), "new payload"));
        ArchiveUpdate Change{Target.toStdString(), Dir.filePath("input").toStdString(), {"new"}, {"remove"}};
        auto Limited = Change; Limited.maxOutputBytes_ = 10; auto Failed = Backend->Update(Limited, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Before);
        std::stop_source Stop; Stop.request_stop(); OperationContext Cancel; Cancel.stopToken_ = Stop.get_token(); QVERIFY(!Backend->Update(Change, Cancel).HasValue()); QCOMPARE(Read(Target), Before);
        auto Bad = Change; Bad.add_ = {"../escape"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        Bad = Change; Bad.add_ = {"KEEP"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(2)); List.Value().reset();
        for (const auto& H : Page.Value().entries_) { QVERIFY(H.name_ != "remove"); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Read(Dir.filePath("input/" + QString::fromStdString(H.name_)))); }
        ArchiveUpdate Empty{Target.toStdString(), Dir.filePath("input").toStdString(), {}, {"new", "keep"}};
        Done = Backend->Update(Empty, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.empty());
    }
    void unsupportedUpdatesPreserveOriginal() {
        QTemporaryDir Dir; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        QVERIFY(Put(Dir.filePath("new"), "new"));
        for (const auto* Name : {"test_read_format_rar5_compressed.rar", "split.7z.001"}) {
            const auto Source = QDir(root_).filePath(Name); if (!QFileInfo::exists(Source)) continue;
            const auto Target = Dir.filePath(Name); QVERIFY(QFile::copy(Source, Target)); const auto Before = Read(Target);
            auto Result = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, PasswordContext("secret"));
            QVERIFY(!Result.HasValue()); QCOMPARE(Read(Target), Before);
        }
    }
    void encryptedUpdate_data() {
        QTest::addColumn<QString>("Name");
        for (const auto* Name : {"encrypted.7z", "content-encrypted.7z", "aes.zip", "aes128.zip", "aes192.zip", "zipcrypto.zip"}) QTest::newRow(Name) << QString(Name);
    }
    void encryptedUpdate() {
        QFETCH(QString, Name); QTemporaryDir Dir; const auto Target = Dir.filePath(Name);
        if (Name == "content-encrypted.7z" || Name == "aes128.zip" || Name == "aes192.zip") {
            const auto Cli = qEnvironmentVariable("HC_SEVENZIP_CLI", QDir(root_).filePath("../sevenzip-tools/official/7z.exe")); QVERIFY(QFileInfo::exists(Cli));
            QStringList Args{"a", Target, "-psecret", "-mx=1", "-y"};
            if (Name.endsWith(".7z")) Args << "-t7z" << "-mhe=off";
            else Args << "-tzip" << (Name == "aes128.zip" ? "-mem=AES128" : "-mem=AES192");
            Args << "*"; QProcess Process; Process.setWorkingDirectory(QDir(root_).filePath("input")); Process.start(Cli, Args);
            QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
        } else QVERIFY(QFile::copy(QDir(root_).filePath(Name), Target));
        const auto Before = Read(Target); QVERIFY(Put(Dir.filePath("new"), "new encrypted payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"new"}, {"payload.bin"}};
        auto Done = Backend->Update(Change, PasswordContext("wrong")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        OperationContext Cancel; Cancel.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> { return Error{ErrorCategory::Cancelled, "cancel-password"}; };
        Done = Backend->Update(Change, Cancel); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Cancelled); QCOMPARE(Read(Target), Before);
        const auto Context = PasswordContext("secret"); Done = Backend->Update(Change, Context); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto UnprotectedHeaders = Backend->Enumerate(Target.toStdString(), false, {});
        QCOMPARE(UnprotectedHeaders.HasValue(), Name != "encrypted.7z"); if (UnprotectedHeaders.HasValue()) UnprotectedHeaders.Value().reset();
        auto Listed = Backend->Enumerate(Target.toStdString(), true, Context); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(64, Context); QVERIFY(Page.HasValue()); Listed.Value().reset();
        bool Found = false;
        for (const auto& H : Page.Value().entries_) {
            QVERIFY(H.name_ != "payload.bin"); if (H.directory_) continue; if (H.size_ || Name.endsWith(".zip")) QVERIFY(H.encrypted_);
            Done = Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), Context); QVERIFY(Done.HasValue());
            if (H.name_ == "new") { Found = true; QCOMPARE(Read(Dir.filePath("restored")), QByteArray("new encrypted payload")); }
            else QCOMPARE(Read(Dir.filePath("restored")), Read(QDir(root_).filePath("input/" + QString::fromStdString(H.name_))));
        }
        QVERIFY(Found);
        for (const auto& Names : {std::pair{"new", "renamed-new"}, std::pair{"renamed-new", "new"}}) {
            Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}};
            auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = std::string("/") + Names.first;
            auto Entry = Reader->Resolve(At, Context).get(); QVERIFY(Entry.HasValue());
            auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, At.nesting_.back().entry_, std::string("/") + Names.second, {}, Entry.Value().id_}}, Context);
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit(Context).HasValue());
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("updated-" + Name), Read(Target))); }
    }
    void solidUpdatePreservation() {
        QTemporaryDir Dir; QVERIFY(Put(Dir.filePath("keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("remove"), "remove")); QVERIFY(Put(Dir.filePath("new"), "new"));
        const auto Target = Dir.filePath("solid.7z"); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, true};
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        // Appending preserves the original block and its stored dictionary.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"remove"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); List.Value().reset();
        QCOMPARE(Page.Value().entries_.size(), size_t(2));
        for (const auto& H : Page.Value().entries_) { QVERIFY(H.name_ != "remove"); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Read(Dir.filePath(QString::fromStdString(H.name_)))); }
    }
    void zipCommentsAndExtraFields() {
        QTemporaryDir Dir; const auto Target = Dir.filePath("comments.zip");
        // Stored ZIP with archive/file comments and an opaque 0xCAFE extra field
        // in both headers. This fixture is independent of the SDK writer.
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAAgAa2VlcC50eHT+ygQAQUJDRHByZXNlcnZlZCBwYXlsb2FkUEsBAhQAFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAgACAANAAAAAAAgAAAAAAAAAGtlZXAudHh0/soEAEFCQ0RlbnRyeSBjb21tZW50UEsFBgAAAAABAAEASwAAAD8AAAAPAGFyY2hpdmUgY29tbWVudA==");
        QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("new"), "new")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        const auto After = Read(Target); QVERIFY(After.endsWith("archive comment")); QVERIFY(After.contains("entry comment"));
        QCOMPARE(After.count(QByteArray::fromHex("feca040041424344")), 2); QVERIFY(After.contains("preserved payload"));
        // Unmapped metadata and opaque extra fields must not be silently lost.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}, {"keep.txt", "new"}}, {});
        QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), After);
        ArchiveUpdate Mapped{Target.toStdString(), Dir.path().toStdString(), {"new"}, {"keep.txt", "new"}}; Mapped.metadataOrigins_ = {"keep.txt"};
        Done = Backend->Update(Mapped, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QCOMPARE(Read(Target).count(QByteArray::fromHex("feca040041424344")), 2);
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("comments.zip"), Read(Target))); }
    }
    void sevenZipPosixMetadata() {
        // Copy archive with an uncompressed header carrying UNIX mode 0100751;
        // both header CRCs were rebuilt independently and checked by official 7z.
        const auto Fixture = QByteArray::fromBase64("N3q8ryccAARTh/QQEQAAAAAAAABKAAAAAAAAAPc9pPBwcmVzZXJ2ZWQgcGF5bG9hZAEEBgABCREABwsBAAEBAAwRAAgKAXXiZ88AAAUBERMAawBlAGUAcAAuAHQAeAB0AAAAGQAUCgEA1B4a5zdT3QEVBgEAIIDpgQAA");
        QTemporaryDir Dir; const auto Target = Dir.filePath("posix.7z"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.size(), size_t(1)); const auto& Header = Page.Value().entries_[0]; QCOMPARE(Header.attributes_ >> 16, 0100751u);
            QVERIFY(Backend->Extract(Change.archive_, Header, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Step ? QByteArray("replacement payload") : QByteArray("preserved payload"));
            Change.remove_ = {"renamed.txt"}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("posix.7z"), Read(Target))); }
    }
    void zipSecurityAndPosixMetadata_data() {
        QTest::addColumn<QByteArray>("Fixture");
        // Independent Python zipfile fixtures: UNIX 0100751, UID 1000/GID 1001,
        // a stored NT security descriptor and distinct local/central extensions.
        QTest::newRow("dos-times") << QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAD8Aa2VlcC50eHRTRB8AFAAAAAAAACW8ltsBAASAAAAAAAAAAAAAAAAAAAAAAHV4CwABBOgDAAAE6QMAAO++CQBsb2NhbC10YWdwcmVzZXJ2ZWQgcGF5bG9hZFBLAQIUAxQAAAAAAIMYIlB14mfPEQAAABEAAAAIAD8ADQAAAAAAIADpgQAAAABrZWVwLnR4dFNEHwAUAAAAAAAAJbyW2wEABIAAAAAAAAAAAAAAAAAAAAAAdXgLAAEE6AMAAATpAwAA774JAGNlbnRyLXRhZ2VudHJ5IGNvbW1lbnRQSwUGAAAAAAEAAQCCAAAAdgAAAA8AYXJjaGl2ZSBjb21tZW50");
        QTest::newRow("unix-times") << QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAFAAa2VlcC50eHRTRB8AFAAAAAAAACW8ltsBAASAAAAAAAAAAAAAAAAAAAAAAHV4CwABBOgDAAAE6QMAAO++CQBsb2NhbC10YWdVVA0AB6ZdDV6lXQ1epF0NXnByZXNlcnZlZCBwYXlsb2FkUEsBAhQDFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAgAUAANAAAAAAAgAOmBAAAAAGtlZXAudHh0U0QfABQAAAAAAAAlvJbbAQAEgAAAAAAAAAAAAAAAAAAAAAB1eAsAAQToAwAABOkDAADvvgkAY2VudHItdGFnVVQNAAemXQ1epV0NXqRdDV5lbnRyeSBjb21tZW50UEsFBgAAAAABAAEAkwAAAIcAAAAPAGFyY2hpdmUgY29tbWVudA==");
    }
    void zipSecurityAndPosixMetadata() {
        QFETCH(QByteArray, Fixture);
        QTemporaryDir Dir; const auto Target = Dir.filePath(QString("security-") + QTest::currentDataTag() + ".zip"); QVERIFY(Put(Target, Fixture));
        QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Bytes = Read(Target); QCOMPARE(Bytes.count(QByteArray::fromHex("53441f001400000000000025bc96db0100048000000000000000000000000000000000")), 2);
            QCOMPARE(Bytes.count(QByteArray::fromHex("75780b000104e803000004e9030000")), 2);
            QCOMPARE(Bytes.count("local-tag"), 1); QCOMPARE(Bytes.count("centr-tag"), 1);
            const auto Central = Bytes.indexOf(QByteArray::fromHex("504b0102")); QVERIFY(Central >= 0);
            QCOMPARE(quint8(Bytes[Central + 5]), quint8(3)); QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 38), quint32((0100751u << 16) | 0x20));
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.size(), size_t(1)); auto Extracted = Backend->Extract(Change.archive_, Page.Value().entries_[0], Dir.filePath("restored").toStdString(), {}); QVERIFY(Extracted.HasValue());
            QCOMPARE(Read(Dir.filePath("restored")), Step ? QByteArray("replacement payload") : QByteArray("preserved payload"));
            Change.remove_ = {"renamed.txt"}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath(QString("security-") + QTest::currentDataTag() + ".zip"), Read(Target))); }
        Change.remove_ = {"keep.txt"}; Change.metadataOrigins_ = {"keep.txt"};
        // Payload hashes, alignment and malformed extension lengths cannot be
        // carried over as opaque metadata. Failure must leave the input intact.
        for (const auto& Id : {QByteArray::fromHex("464b0900"), QByteArray::fromHex("1ea10900"), QByteArray::fromHex("55580900"), QByteArray::fromHex("efbeffff")}) {
            auto Bad = Fixture; Bad.replace(QByteArray::fromHex("efbe0900"), Id); QVERIFY(Put(Target, Bad));
            auto Failed = Backend->Update(Change, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Bad);
        }
        auto Special = Fixture; const auto Central = Special.indexOf(QByteArray::fromHex("504b0102")); QVERIFY(Central >= 0);
        qToLittleEndian<quint32>((0010644u << 16) | 0x20, Special.data() + Central + 38); QVERIFY(Put(Target, Special));
        auto Unsupported = Backend->Update(Change, {}); QVERIFY(!Unsupported.HasValue()); QCOMPARE(Unsupported.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), Special);
        QVERIFY(Put(Target, Fixture)); Change.maxOutputBytes_ = 250;
        auto Limited = Backend->Update(Change, {}); QVERIFY(!Limited.HasValue()); QCOMPARE(Limited.Failure().category_, ErrorCategory::BudgetExceeded); QCOMPARE(Read(Target), Fixture);
    }
    void metadataRenameAndReplacement() {
        // Independently generated ZIP: file/archive comments and three NTFS times.
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIACQAa2VlcC50eHQKACAAAAAAAAEAGAAAAAVpNsDVAYBpbGg2wNUBANPTZzbA1QFwcmVzZXJ2ZWQgcGF5bG9hZFBLAQIUABQAAAAAAIMYIlB14mfPEQAAABEAAAAIACQADQAAAAAAIAAAAAAAAABrZWVwLnR4dAoAIAAAAAAAAQAYAAAABWk2wNUBgGlsaDbA1QEA09NnNsDVAWVudHJ5IGNvbW1lbnRQSwUGAAAAAAEAAQBnAAAAWwAAAA8AYXJjaGl2ZSBjb21tZW50");
        QTemporaryDir Dir; const auto Target = Dir.filePath("metadata.zip"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Rename{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Rename.metadataOrigins_ = {"keep.txt"};
        auto Wire = EncodeArchiveUpdate(Rename); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().metadataOrigins_, Rename.metadataOrigins_);
        Wire["metadataOrigins"] = QJsonArray{}; QVERIFY(!DecodeArchiveUpdate(Wire).HasValue());
        auto Invalid = Rename; Invalid.metadataOrigins_ = {"missing"}; QVERIFY(!Backend->Update(Invalid, {}).HasValue()); QCOMPARE(Read(Target), Fixture);
        auto Unknown = Fixture; Unknown.replace(QByteArray::fromHex("0a0020000000000001001800"), QByteArray::fromHex("0a0020000000000002001800"));
        const auto UnknownTarget = Dir.filePath("unknown-ntfs.zip"); QVERIFY(Put(UnknownTarget, Unknown));
        auto Preserve = Rename; Preserve.archive_ = UnknownTarget.toStdString(); auto Preserved = Backend->Update(Preserve, {});
        QVERIFY2(Preserved.HasValue(), Preserved.HasValue() ? "" : Preserved.Failure().code_.c_str());
        QCOMPARE(Read(UnknownTarget).count(QByteArray::fromHex("020018000000056936c0d50180696c6836c0d50100d3d36736c0d501")), 2);
        auto Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"renamed.txt"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        // Exercise provenance through the common mutation coordinator and host.
        const auto Common = Dir.filePath("common.zip"); QVERIFY(Put(Common, Fixture)); Locator Root{LocatorKind::Archive, Common.toStdString(), {{"wcx", "/"}}};
        auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = "/keep.txt";
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Batch.HasValue());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/keep.txt", "/common-name.txt", {}, Entry.Value().id_}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(Read(Common), Fixture); QVERIFY(Batch.Value()->Commit({}).HasValue());
        QVERIFY(Read(Common).contains("entry comment")); QVERIFY(Read(Common).endsWith("archive comment"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("metadata.zip"), Read(Target))); QVERIFY(Put(QDir(Artifacts).filePath("metadata-common.zip"), Read(Common))); }
    }
    void schedulerPublication() {
        using namespace hc::operations;
        QTemporaryDir Dir; const auto Source = Dir.filePath("source"), Target = Dir.filePath("result.zip"); QVERIFY(Put(Source, QByteArray(1024, 'a')));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Reader->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        hc::sevenzip::CreateRequest Request; Request.sources_.push_back({Entry.Value(), Reader, {}}); Request.destination_ = {LocatorKind::Local, Target.toStdString()}; Request.format_ = "zip";
        const auto Folder = Dir.filePath("folder"); QVERIFY(QDir().mkdir(Folder)); QVERIFY(Put(Folder + "/child", "child"));
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Folder).utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Ticks = (uint64_t(1577934246000LL) + 11644473600000ULL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
        const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle); QVERIFY(Saved);
        auto FolderEntry = Reader->Resolve({LocatorKind::Local, Folder.toStdString()}, {}).get(); QVERIFY(FolderEntry.HasValue()); Request.sources_.push_back({FolderEntry.Value(), Reader, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue());
        auto Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        auto Result = Job.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "job state"); QVERIFY(Result.cleanupErrors_.empty());
        const auto Before = Read(Target); QVERIFY(!Before.isEmpty());
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); bool DirectorySeen = false;
        for (const auto& Header : Page.Value().entries_) if (Header.directory_) { QCOMPARE(Header.modifiedUtcMs_, FolderEntry.Value().modifiedUtcMs_); DirectorySeen = true; }
        QVERIFY(DirectorySeen); Listed.Value().reset();
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 5000); QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(Read(Target), Before);
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        const auto Waiting = Scheduler.Snapshot(Job.Value().id_).Value(); QVERIFY(Put(Target, "external change"));
        QVERIFY(Scheduler.Reply(Job.Value().id_, {Waiting.interaction_->id_, InteractionChoice::Replace, {}}).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        Result = Job.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Read(Target), QByteArray("external change"));
    }
    void initTestCase() {
        const auto Path = qEnvironmentVariable("HC_SEVENZIP_CORPUS");
        QVERIFY2(QFileInfo::exists(Path), "Build hc_sevenzip_corpus or set HC_SEVENZIP_CORPUS to the generated corpus.json");
        cases_ = QJsonDocument::fromJson(Read(Path))["cases"].toArray(); QVERIFY(cases_.size() >= 17); root_ = QFileInfo(Path).absolutePath();
    }
    void corpus_data() {
        QTest::addColumn<int>("Row");
        for (int Index = 0; Index < cases_.size(); ++Index) QTest::newRow(QFileInfo(cases_[Index].toObject()["archive"].toString()).fileName().toUtf8()) << Index;
    }
    void corpus() {
        QFETCH(int, Row); const auto Case = cases_[Row].toObject(); const auto Path = Case["archive"].toString();
        QTemporaryDir Output; auto Options = BuiltinSevenZipOptions(); auto Context = PasswordContext(Case["password"].toString());
        auto Backend = MakeApprovedWcxBackend(Options); auto Listed = Backend->Enumerate(Path.toStdString(), false, Context);
        QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(2, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            for (const auto& Header : Page.Value().entries_) Headers.push_back(Header); if (Page.Value().end_) break; }
        Listed.Value().reset(); QVERIFY(!Headers.empty());
        size_t Files = 0;
        for (const auto& Header : Headers) {
            if (Header.directory_) continue;
            const auto PathTo = Output.filePath("selected.bin"); auto Done = Backend->Extract(Path.toStdString(), Header, PathTo.toStdString(), Context);
            if (Header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) { QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); continue; }
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Expected = QDir(Case["expected"].toString()).filePath(QString::fromStdString(Header.name_));
            QVERIFY2(QFileInfo::exists(Expected), qPrintable(Expected)); QCOMPARE(Read(PathTo), Read(Expected)); ++Files;
        }
        QVERIFY(Files > 0);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY(Tested.HasValue());
        for (;;) { auto Page = Tested.Value()->Next(64, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); if (Page.Value().end_) break; }
        QVERIFY(!Backend->Update({Path.toStdString(), Output.path().toStdString()}, Context).HasValue());
        QVERIFY(!GetModuleHandleW(L"7z.dll")); // The application never loads archive engine code.
    }
    void errorsCancellationAndPolicy() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        const auto Plain = QDir(root_).filePath("plain.7z").toStdString(); const auto Encrypted = QDir(root_).filePath("encrypted.7z").toStdString();
        auto Required = Backend->Enumerate(Encrypted, false, {}); QVERIFY(!Required.HasValue()); QCOMPARE(Required.Failure().category_, ErrorCategory::AccessDenied);
        auto Wrong = Backend->Enumerate(Encrypted, false, PasswordContext("wrong")); QVERIFY(!Wrong.HasValue());
        auto Declined = PasswordContext(""); Declined.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> { return Error{ErrorCategory::Cancelled, "test-cancel-password"}; };
        auto Cancelled = Backend->Enumerate(Encrypted, false, Declined); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        auto Correct = Backend->Enumerate(Encrypted, false, PasswordContext("secret")); QVERIFY(Correct.HasValue()); Correct.Value().reset();
        std::stop_source Stop; Stop.request_stop(); OperationContext Stopped; Stopped.stopToken_ = Stop.get_token();
        auto Early = Backend->Enumerate(Plain, false, Stopped); QVERIFY(!Early.HasValue()); QCOMPARE(Early.Failure().category_, ErrorCategory::Cancelled);
        OperationContext Limited; Limited.maxEnumerationEntries_ = 1; QVERIFY(!Backend->Enumerate(Plain, false, Limited).HasValue());
        QTemporaryDir Directory; const auto Broken = Directory.filePath("broken.7z"); QVERIFY(Put(Broken, Read(QString::fromStdString(Plain)).left(48)));
        auto Bad = Backend->Enumerate(Broken.toStdString(), true, {});
        if (Bad.HasValue()) QVERIFY(!Bad.Value()->Next(256, {}).HasValue());
        auto Listed = Backend->Enumerate(Plain, false, {}); QVERIFY(Listed.HasValue()); std::optional<ArchiveHeader> Chosen;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue()); for (const auto& Item : Page.Value().entries_) if (Item.name_ == "sub/zeros.bin") Chosen = Item; if (Page.Value().end_) break; }
        QVERIFY(Chosen.has_value()); Listed.Value().reset();
        OperationContext Automatic; Automatic.workload_.intent_ = WorkloadIntent::Automatic; Automatic.workload_.automaticFileBytes_ = 1024;
        auto Blocked = Backend->Extract(Plain, *Chosen, Directory.filePath("auto").toStdString(), Automatic); QVERIFY(!Blocked.HasValue()); QCOMPARE(Blocked.Failure().category_, ErrorCategory::BudgetExceeded);
        std::stop_source During; OperationContext Active; Active.stopToken_ = During.get_token(); Active.progress_ = [&](uint64_t Done) { if (Done > 1024 * 1024) During.request_stop(); };
        // Drive the native callback synchronously: the 8 MiB fixture can finish
        // before the host's 50 ms progress notification interval on fast disks.
        const auto Module = LoadLibraryExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(BuiltinSevenZipOptions().binaryPath_).utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        QVERIFY(Module); auto Unload = qScopeGuard([&] { FreeLibrary(Module); }); auto Native = MakeSevenZipBackend(Module);
        auto Interrupted = Native->Extract(Plain, *Chosen, Directory.filePath("cancelled").toStdString(), Active); QVERIFY(!Interrupted.HasValue());
        QCOMPARE(Interrupted.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!QFileInfo::exists(Directory.filePath("cancelled")));
    }
    void corruptedPayload() {
        QTemporaryDir Directory; auto Bytes = Read(QDir(root_).filePath("plain.7z")); QVERIFY(Bytes.size() > 128);
        // Damage the packed payload while preserving the start and next headers.
        Bytes[64] = char(static_cast<unsigned char>(Bytes[64]) ^ 0x80);
        const auto Path = Directory.filePath("corrupt.7z"); QVERIFY(Put(Path, Bytes));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listed = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Listed.HasValue());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue());
            for (const auto& Header : Page.Value().entries_) if (!Header.directory_) Headers.push_back(Header);
            if (Page.Value().end_) break; }
        Listed.Value().reset(); bool Failed = false;
        for (const auto& Header : Headers) {
            const auto Output = Directory.filePath("output.bin");
            auto Done = Backend->Extract(Path.toStdString(), Header, Output.toStdString(), {});
            if (!Done.HasValue()) { Failed = true; QVERIFY(!QFileInfo::exists(Output)); }
        }
        QVERIFY(Failed);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, {}); QVERIFY(Tested.HasValue());
        QVERIFY(!Tested.Value()->Next(128, {}).HasValue());
    }
    void commonProviderAndSplitMissing() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); QTemporaryDir Cache;
        const auto Path = QDir(root_).filePath("solid.7z"); Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}};
        auto Provider = MakeWcxReadProvider({}, Root, Cache.path().toStdString(), Backend);
        auto At = Root; At.nesting_.back().entry_ = "/payload.bin"; auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        QVERIFY(Stream.Value()->CanSeek()); QVERIFY(Stream.Value()->Seek(100, {}).get().HasValue()); auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), Read(QDir(root_).filePath("input/payload.bin")).mid(100, 100));
        QTemporaryDir Missing;
        for (const auto* Name : {"split.7z.001", "test_read_format_rar5_multiarchive.part01.rar"}) {
            QVERIFY(QFile::copy(QDir(root_).filePath(Name), Missing.filePath(Name)));
            auto Bad = Backend->Enumerate(Missing.filePath(Name).toStdString(), true, {});
            if (Bad.HasValue()) { bool Failed = false; for (;;) { auto Page = Bad.Value()->Next(128, {}); if (!Page.HasValue()) { Failed = true; break; } if (Page.Value().end_) break; } QVERIFY(Failed); }
        }
    }
};
QTEST_GUILESS_MAIN(SevenZipTests)
#include "tstSevenZip.moc"
