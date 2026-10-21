#include "stdafx.h"
#include "UniqueLibs/passwords/passwordUi.hpp"

/// Archive engines
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "HyperCommanderBuiltInPlugin/AlzEgg/builtinUnEgg.hpp"
#include "pluginHost/approvedWcxBackend.hpp"

/// Qt
#include <QtTest>
#include <QApplication>

using namespace hc::storage;
using namespace hc::plugins;
using namespace hc::passwords;
namespace
{
    bool put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    OperationContext correct(const char* Text = "secret")
    {
        OperationContext Context; Context.passwordPrompt_ = [Text](const std::string&, unsigned, const OperationContext&) -> Result<Password> { return Password(Text); }; return Context;
    }
    OperationContext managed(unsigned& Asked)
    {
        OperationContext Context; Context.passwordPrompt_ = [&Asked](const std::string& Path, unsigned Attempt, const OperationContext& Operation) -> Result<Password> {
            ++Asked;
            // Never show a modal in this engine integration test.
            if (Attempt > 2) return Error{ErrorCategory::Cancelled, "test-password-fallback"};
            return AcquireArchivePassword(Path, Attempt, Operation);
        }; return Context;
    }
    bool seed(const QString& Path, const QString& Password)
    {
        Store Value(Path); if (!Value.Load() || !Value.Add("wrong", "wrong") || !Value.Add("correct", Password)) return false;
        RuntimeStore() = Value; ForgetSessionPasswords(); return true;
    }
}
class PasswordArchiveTests final : public QObject
{
    Q_OBJECT
private slots:
    void encryptedArchives_data()
    {
        QTest::addColumn<QString>("Format"); QTest::newRow("7z") << QString("7z"); QTest::newRow("7z-content") << QString("7z-content"); QTest::newRow("zip") << QString("zip");
    }
    void encryptedArchives()
    {
        QFETCH(QString, Format); QTemporaryDir Dir; const QByteArray Payload("password retry payload");
        QVERIFY(put(Dir.filePath("payload.txt"), Payload)); const auto Path = Dir.filePath("encrypted." + (Format == "7z-content" ? QString("7z") : Format));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Create{Path.toStdString(), Dir.path().toStdString(), {"payload.txt"}}; Create.encrypt_ = true;
        if (Format == "7z-content") { SevenZipCreateSettings Advanced; Advanced.encrypt_ = true; Advanced.encryptHeaders_ = false; Create.creation_ = ArchiveCreationOptions{5, true, Advanced}; }
        auto Created = Backend->Update(Create, correct()); QVERIFY2(Created.HasValue(), Created.HasValue() ? "" : Created.Failure().code_.c_str());
        QVERIFY(seed(Dir.filePath("passwords"), "secret"));
        unsigned Asked = 0; auto Context = managed(Asked);
        auto Listed = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        auto Page = Listed.Value()->Next(16, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); QCOMPARE(Page.Value().entries_.size(), size_t(1));
        const auto Header = Page.Value().entries_[0]; QVERIFY(Header.encrypted_); QVERIFY(Asked >= 2); Listed.Value().reset();
        Asked = 0; auto Extracted = Backend->Extract(Path.toStdString(), Header, Dir.filePath("output").toStdString(), Context);
        QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(read(Dir.filePath("output")), Payload); QVERIFY(Asked >= 2);
        Asked = 0; QVERIFY(QDir().mkpath(Dir.filePath("batch")));
        auto Batch = Backend->ExtractSelected(Path.toStdString(), Dir.filePath("batch").toStdString(), {0}, Context);
        QVERIFY2(Batch.HasValue(), Batch.HasValue() ? "" : Batch.Failure().code_.c_str()); QCOMPARE(read(Dir.filePath("batch/0.bin")), Payload); QVERIFY(Asked >= 2);
        Asked = 0; auto Stream = Backend->OpenReadSession(Path.toStdString(), {0}, Context); QVERIFY(Stream.HasValue());
        auto Chunk = Stream.Value()->Read(0, 1024, Context); QVERIFY2(Chunk.HasValue(), Chunk.HasValue() ? "" : Chunk.Failure().code_.c_str());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), qsizetype(Chunk.Value().bytes_.size())), Payload); QVERIFY(Asked >= 2); Stream.Value().reset();
        OperationContext Cancel = correct(); Cancel.passwordPrompt_ = [](const std::string&, unsigned Attempt, const OperationContext&) -> Result<Password> {
            if (Attempt == 1) return Password("wrong"); return Error{ErrorCategory::Cancelled, "test-password-cancel"};
        };
        auto Cancelled = Backend->Extract(Path.toStdString(), Header, Dir.filePath("cancelled").toStdString(), Cancel);
        QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled); QVERIFY(!QFileInfo::exists(Dir.filePath("cancelled")));
        QVERIFY(put(Dir.filePath("new.txt"), "new encrypted payload")); Asked = 0;
        auto Updated = Backend->Update({Path.toStdString(), Dir.path().toStdString(), {"new.txt"}}, Context);
        QVERIFY2(Updated.HasValue(), Updated.HasValue() ? "" : Updated.Failure().code_.c_str()); QVERIFY(Asked >= 2);
        auto UpdatedList = Backend->Enumerate(Path.toStdString(), true, correct()); QVERIFY(UpdatedList.HasValue()); auto UpdatedPage = UpdatedList.Value()->Next(16, correct());
        QVERIFY(UpdatedPage.HasValue()); QCOMPARE(UpdatedPage.Value().entries_.size(), size_t(2));
    }
    void encryptedEgg_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto* Name : {"lorem_long_aes128.egg", "lorem_long_aes256.egg", "lorem_long_store_lea128.egg", "lorem_long_store_lea256.egg", "lorem_long_zipEnc.egg"}) QTest::newRow(Name) << QString(Name);
    }
    void manyStoredCandidates()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("many.zip"); QVERIFY(put(Dir.filePath("payload.txt"), "payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Create{Path.toStdString(), Dir.path().toStdString(), {"payload.txt"}}; Create.encrypt_ = true;
        QVERIFY(Backend->Update(Create, correct()).HasValue());
        Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load());
        for (int Index = 0; Index < 17; ++Index) QVERIFY(Value.Add(QString::number(Index), "wrong-" + QString::number(Index)));
        QVERIFY(Value.Add("correct", "secret")); RuntimeStore() = Value; unsigned Asked = 0;
        OperationContext Context; Context.passwordPrompt_ = [&](const std::string& Archive, unsigned Attempt, const OperationContext& Operation) -> Result<Password> {
            ++Asked; if (Attempt > 18) return Error{ErrorCategory::Cancelled, "too-many-test-prompts"}; return AcquireArchivePassword(Archive, Attempt, Operation);
        };
        auto Listing = Backend->Enumerate(Path.toStdString(), false, Context); QVERIFY(Listing.HasValue()); auto Page = Listing.Value()->Next(16, Context); QVERIFY(Page.HasValue()); Listing.Value().reset();
        auto Done = Backend->Extract(Path.toStdString(), Page.Value().entries_.front(), Dir.filePath("output").toStdString(), Context);
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); QCOMPARE(Asked, 18u); QCOMPARE(read(Dir.filePath("output")), QByteArray("payload"));
    }
    void encryptedEgg()
    {
        QFETCH(QString, Name); const auto Corpus = qEnvironmentVariable("HC_UNEGG_CORPUS");
        if (Corpus.isEmpty() || !QFileInfo::exists(BuiltinUnEggOptions().binaryPath_)) QSKIP("UnEGG corpus not configured");
        const auto Path = QDir(QFileInfo(Corpus).absolutePath()).filePath(Name); QVERIFY(QFileInfo::exists(Path)); QTemporaryDir Dir;
        QVERIFY(seed(Dir.filePath("passwords"), "password12345!")); unsigned Asked = 0; auto Context = managed(Asked);
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); auto Listed = Backend->Enumerate(Path.toStdString(), false, Context); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, Context); QVERIFY(Page.HasValue()); QVERIFY(!Page.Value().entries_.empty());
        const auto Header = Page.Value().entries_.front(); QVERIFY(Header.encrypted_); Listed.Value().reset();
        auto Extracted = Backend->Extract(Path.toStdString(), Header, Dir.filePath("output").toStdString(), Context);
        QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(uint64_t(read(Dir.filePath("output")).size()), Header.size_); QCOMPARE(Asked, 2u);
    }
};
QTEST_MAIN(PasswordArchiveTests)
#include "tstPasswordArchives.moc"
