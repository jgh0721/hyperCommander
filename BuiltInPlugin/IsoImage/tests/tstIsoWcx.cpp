#include <QtTest>
#include <QTemporaryDir>
#include "BuiltInPlugin/IsoImage/builtinIso.hpp"

using namespace hc::plugins;
using namespace hc::storage;
namespace {
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
}
class IsoWcxTests : public QObject {
    Q_OBJECT
private slots:
    void configurationProfiles()
    {
        QTemporaryDir Directory; QVERIFY(Put(Directory.filePath("SOURCE.TXT"), "profile"));
        for (int Profile = 0; Profile < 3; ++Profile) {
            auto Options = BuiltinIsoOptions(); Options.defaultIniPath_ = Directory.filePath("pkplugin.ini");
            QVERIFY(Put(Options.defaultIniPath_, "[IsoImage]\r\nProfile=" + QByteArray::number(Profile) + "\r\nVolumeLabel=PROFILE_TEST\r\n"));
            const auto Target = Directory.filePath(QString("profile-%1.iso").arg(Profile));
            const auto Result = MakeApprovedWcxBackend(Options)->Update({Target.toStdString(), Directory.path().toStdString(), {"SOURCE.TXT"}}, {});
            QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
            const auto Bytes = Read(Target); QCOMPARE(Bytes.mid(16 * 2048 + 40, 12), QByteArray("PROFILE_TEST"));
            QCOMPARE(uint8_t(Bytes[17 * 2048]), Profile == 0 ? uint8_t(255) : uint8_t(2));
            QCOMPARE(Bytes.mid(20 * 2048 + 1, 5) == QByteArray("NSR02"), Profile == 2);
        }
    }
    void hostedRoundTrip()
    {
        QTemporaryDir Directory; auto Options = BuiltinIsoOptions(); QVERIFY(Options.approval_.approved_);
        QVERIFY(!Options.approval_.granted_.contains(Permission::ChildProcess));
        auto Backend = MakeApprovedWcxBackend(Options);
        const auto Archive = Directory.filePath("image.iso");
        QVERIFY(QDir().mkpath(Directory.filePath("한글/empty")));
        QMap<QString, QByteArray> Expected{{"alpha.txt", "alpha"}, {"한글/test.txt", QByteArray(200000, 'z')}, {"zero", {}}};
        ArchiveUpdate Request{Archive.toStdString(), Directory.path().toStdString()}; Request.add_.push_back("한글/empty/");
        for (auto I = Expected.cbegin(); I != Expected.cend(); ++I) { QVERIFY(Put(Directory.filePath(I.key()), I.value())); Request.add_.push_back(I.key().toStdString()); }
        auto Updated = Backend->Update(Request, {}); QVERIFY2(Updated.HasValue(), Updated.HasValue() ? "" : Updated.Failure().code_.c_str());
        const auto Verify = [&]() -> bool {
            auto Cursor = Backend->Enumerate(Archive.toStdString(), true, {}); if (!Cursor.HasValue()) { qWarning() << Cursor.Failure().code_; return false; }
            std::vector<ArchiveHeader> Headers;
            for (;;) { auto Page = Cursor.Value()->Next(2, {}); if (!Page.HasValue()) { qWarning() << Page.Failure().code_; return false; } Headers.insert(Headers.end(), Page.Value().entries_.begin(), Page.Value().entries_.end()); if (Page.Value().end_) break; }
            Cursor.Value().reset(); QSet<QString> Seen;
            for (const auto& Header : Headers) {
                if (Header.directory_) continue; const auto Name = QString::fromStdString(Header.name_); if (!Expected.contains(Name)) { qWarning() << "Unexpected file" << Name; return false; }
                const auto Output = Directory.filePath("extract-" + QString::number(Seen.size())); QFile::remove(Output);
                const auto Result = Backend->Extract(Archive.toStdString(), Header, Output.toStdString(), {});
                if (!Result.HasValue()) { qWarning() << Result.Failure().code_; return false; }
                if (Read(Output) != Expected[Name]) { qWarning() << "Payload mismatch" << Name; return false; } Seen.insert(Name);
            }
            if (Seen.size() != Expected.size()) qWarning() << "Missing entries" << Seen << Expected.keys();
            return Seen.size() == Expected.size();
        };
        QVERIFY(Verify());
        Expected["alpha.txt"] = "replacement"; QVERIFY(Put(Directory.filePath("alpha.txt"), Expected["alpha.txt"]));
        QVERIFY(Backend->Update({Archive.toStdString(), Directory.path().toStdString(), {"alpha.txt"}}, {}).HasValue()); QVERIFY(Verify());
        QVERIFY(Backend->Update({Archive.toStdString(), Directory.path().toStdString(), {}, {"한글/test.txt"}}, {}).HasValue()); Expected.remove("한글/test.txt"); QVERIFY(Verify());
        const auto Before = Read(Archive); std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        QVERIFY(!Backend->Update(Request, Context).HasValue()); QCOMPARE(Read(Archive), Before);
        const auto Artifact = qEnvironmentVariable("HC_ISO_TEST_ARTIFACT"); if (!Artifact.isEmpty()) QVERIFY(QFile::copy(Archive, Artifact));
    }
    void compiledApproval()
    {
        auto Options = BuiltinIsoOptions(); auto Inspected = InspectManifestFile(Options.manifestPath_); QVERIFY(Inspected.HasValue());
        QVERIFY(Inspected.Value().manifest_ == Options.approval_.reviewed_);
        Options.approval_.reviewed_.version_ = "tampered";
        QTemporaryDir Directory; QVERIFY(!MakeApprovedWcxBackend(Options)->Enumerate(Directory.filePath("no.iso").toStdString(), false, {}).HasValue());
    }
};
QTEST_GUILESS_MAIN(IsoWcxTests)
#include "tstIsoWcx.moc"
