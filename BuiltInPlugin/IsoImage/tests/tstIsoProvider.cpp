#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "BuiltInPlugin/IsoImage/isoProvider.hpp"
#include "BuiltInPlugin/IsoImage/isoRewrite.hpp"
#include "BuiltInPlugin/IsoImage/builtinIso.hpp"
#include <Windows.h>

using namespace hc::storage;
namespace {
bool Put(const QString& Path, const QByteArray& Bytes)
{ QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
Locator Root(const QString& Path) { return {LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}}; }
QByteArray Bytes(const ReadChunk& Chunk) { return {reinterpret_cast<const char*>(Chunk.bytes_.data()), qsizetype(Chunk.bytes_.size())}; }
}
class IsoProviderTests : public QObject {
    Q_OBJECT
private slots:
    void profiles_data()
    {
        QTest::addColumn<uint32_t>("Profile");
        QTest::newRow("primary") << uint32_t(HC_ISO_9660);
        QTest::newRow("joliet") << uint32_t(HC_ISO_9660_JOLIET);
        QTest::newRow("udf-bridge") << uint32_t(HC_ISO_UDF_BRIDGE);
        QTest::newRow("udf-no-joliet") << uint32_t(HC_ISO_UDF_NO_JOLIET);
    }
    void profiles()
    {
        QFETCH(uint32_t, Profile);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        QByteArray Payload(2 * 1024 * 1024 + 71, Qt::Uninitialized);
        for (qsizetype I = 0; I < Payload.size(); ++I) Payload[I] = char(I * 31);
        const auto Source = Directory.filePath("source"); QVERIFY(Put(Source, Payload));
        hc::iso::RewriteRequest Request;
        Request.profile_ = Profile; Request.temporaryBase_ = Directory.path();
        Request.sources_ = {{Source.toStdString(), "DIR/DATA.BIN", false}, {"", "EMPTY", true}};
        auto Prepared = hc::iso::PrepareRewrite(Request).get(); QVERIFY(Prepared.HasValue());
        const auto Image = Prepared.Value()->Path();
        for (const auto Namespace : {HC_ISO_PRIMARY, HC_ISO_JOLIET, HC_ISO_UDF}) {
            if ((Namespace == HC_ISO_JOLIET && (Profile == HC_ISO_9660 || Profile == HC_ISO_UDF_NO_JOLIET))
                || (Namespace == HC_ISO_UDF && Profile < HC_ISO_UDF_BRIDGE)) continue;
            auto Provider = hc::iso::MakeReadProvider(Root(Image), Namespace);
            QCOMPARE(Provider->Id().value_, "iso"); QCOMPARE(Provider->Capabilities().read_, ReadAccess::RandomAccess);
            auto Cursor = Provider->Enumerate(Root(Image), {}).get(); QVERIFY(Cursor.HasValue());
            unsigned Count = 0;
            for (;;) { auto Page = Cursor.Value()->Next(1, {}).get(); QVERIFY(Page.HasValue()); Count += unsigned(Page.Value().entries_.size()); if (Page.Value().end_) break; }
            QCOMPARE(Count, 2u);
            auto FileAt = Root(Image); FileAt.nesting_.back().entry_ = "/DIR/DATA.BIN";
            auto File = Provider->Resolve(FileAt, {}).get(); QVERIFY(File.HasValue()); QCOMPARE(File.Value().size_, uint64_t(Payload.size()));
            auto First = Provider->OpenRead(File.Value().id_, {}).get(); QVERIFY(First.HasValue());
            auto Second = Provider->OpenRead(File.Value().id_, {}).get(); QVERIFY(Second.HasValue());
            QVERIFY(First.Value()->CanSeek()); QCOMPARE(First.Value()->Access(), ReadAccess::RandomAccess);
            QVERIFY(First.Value()->Seek(2047, {}).get().HasValue());
            auto Part = First.Value()->Read(79, {}).get(); QVERIFY(Part.HasValue()); QCOMPARE(Bytes(Part.Value()), Payload.mid(2047, 79));
            Part = Second.Value()->Read(19, {}).get(); QVERIFY(Part.HasValue()); QCOMPARE(Bytes(Part.Value()), Payload.first(19));
            QVERIFY(First.Value()->Seek(uint64_t(Payload.size() - 3), {}).get().HasValue());
            Part = First.Value()->Read(20, {}).get(); QVERIFY(Part.HasValue()); QVERIFY(Part.Value().eof_); QCOMPARE(Bytes(Part.Value()), Payload.last(3));
            Part = First.Value()->Read(20, {}).get(); QVERIFY(Part.HasValue()); QVERIFY(Part.Value().eof_); QVERIFY(Part.Value().bytes_.empty());
            QVERIFY(!First.Value()->Seek(uint64_t(Payload.size() + 1), {}).get().HasValue());
            auto Foreign = hc::iso::MakeReadProvider(Root(Image), Namespace)->OpenRead(File.Value().id_, {}).get();
            QVERIFY(!Foreign.HasValue()); QCOMPARE(Foreign.Failure().category_, ErrorCategory::StaleRevision);
            OperationContext Small; Small.maxReadBytes_ = 7;
            Part = Second.Value()->Read(100, Small).get(); QVERIFY(Part.HasValue()); QCOMPARE(Part.Value().bytes_.size(), size_t(7));
            std::stop_source Stop; Stop.request_stop(); OperationContext Cancel; Cancel.stopToken_ = Stop.get_token();
            Part = Second.Value()->Read(10, Cancel).get(); QVERIFY(!Part.HasValue()); QCOMPARE(Part.Failure().category_, ErrorCategory::Cancelled);
            auto LimitedContext = OperationContext{}; LimitedContext.maxEnumerationEntries_ = 1;
            auto Limited = Provider->Enumerate(Root(Image), LimitedContext).get(); QVERIFY(Limited.HasValue());
            QVERIFY(Limited.Value()->Next(1, {}).get().HasValue());
            auto Overflow = Limited.Value()->Next(1, {}).get(); QVERIFY(!Overflow.HasValue()); QCOMPARE(Overflow.Failure().category_, ErrorCategory::BudgetExceeded);
            Provider.reset(); // Outstanding streams and cursors own the image session.
            QVERIFY(Second.Value()->Seek(0, {}).get().HasValue());
            QByteArray Complete;
            for (;;) { auto Chunk = Second.Value()->Read(1024 * 1024, {}).get(); QVERIFY(Chunk.HasValue()); Complete += Bytes(Chunk.Value()); if (Chunk.Value().eof_) break; }
            QCOMPARE(Complete, Payload);
        }
        // Compare the native tree and complete bytes with the compatibility adapter.
        auto Backend = hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinIsoOptions());
        auto Wcx = Backend->Enumerate(Image.toStdString(), true, {}); QVERIFY(Wcx.HasValue());
        uint64_t WcxFiles = 0, WcxBytes = 0;
        for (;;) {
            auto Page = Wcx.Value()->Next(2, {}); QVERIFY(Page.HasValue());
            for (const auto& Entry : Page.Value().entries_) if (!Entry.directory_) { ++WcxFiles; WcxBytes += Entry.size_; }
            if (Page.Value().end_) break;
        }
        QCOMPARE(WcxFiles, 1ull); QCOMPARE(WcxBytes, uint64_t(Payload.size()));
        Wcx.Value().reset(); Backend.reset();
        Prepared.Value().reset(); QVERIFY(!QFile::exists(Image));
    }
    void sourceReplacement()
    {
        QTemporaryDir Directory; const auto Source = Directory.filePath("source"); QVERIFY(Put(Source, "old bytes"));
        hc::iso::RewriteRequest Request; Request.sources_ = {{Source.toStdString(), "DATA", false}};
        auto Prepared = hc::iso::PrepareRewrite(Request).get(); QVERIFY(Prepared.HasValue());
        const auto Path = Directory.filePath("image.iso"); QVERIFY(QFile::copy(Prepared.Value()->Path(), Path));
        auto Provider = hc::iso::MakeReadProvider(Root(Path)); auto At = Root(Path); At.nesting_.back().entry_ = "/DATA";
        auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue());
        const auto Writer = CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (Writer != INVALID_HANDLE_VALUE) CloseHandle(Writer);
        QCOMPARE(Writer, INVALID_HANDLE_VALUE); // No mixed data from in-place writes.
        const auto Replacement = Directory.filePath("replacement.iso"); QVERIFY(QFile::copy(Path, Replacement));
        // Even equal size/content/mtime replacements have a different file ID.
        // The common writer renames the old file to a backup before publishing
        // the new one. Legacy MoveFileEx cannot overwrite an open target.
        const auto From = QDir::toNativeSeparators(Replacement), To = QDir::toNativeSeparators(Path);
        const auto Backup = QDir::toNativeSeparators(Directory.filePath("old.iso"));
        QVERIFY(MoveFileExW(reinterpret_cast<LPCWSTR>(To.utf16()), reinterpret_cast<LPCWSTR>(Backup.utf16()), 0));
        QVERIFY(MoveFileExW(reinterpret_cast<LPCWSTR>(From.utf16()), reinterpret_cast<LPCWSTR>(To.utf16()), 0));
        auto Read = Stream.Value()->Read(4, {}).get(); QVERIFY(!Read.HasValue()); QCOMPARE(Read.Failure().category_, ErrorCategory::StaleRevision);
        auto Stat = Provider->Stat(Entry.Value().id_, {}).get(); QVERIFY(!Stat.HasValue()); QCOMPARE(Stat.Failure().category_, ErrorCategory::StaleRevision);
        auto Fresh = hc::iso::MakeReadProvider(Root(Path))->Resolve(At, {}).get(); QVERIFY(Fresh.HasValue());
        QVERIFY(Fresh.Value().id_ != Entry.Value().id_);
    }
    void failedRewriteCleanup()
    {
        QTemporaryDir Directory; const auto Source = Directory.filePath("source"); QVERIFY(Put(Source, QByteArray(1024 * 1024, 'x')));
        hc::iso::RewriteRequest Request; Request.temporaryBase_ = Directory.path(); Request.sources_ = {{Source.toStdString(), "DATA", false}};
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        Context.progress_ = [&](uint64_t) { Stop.request_stop(); };
        auto Cancelled = hc::iso::PrepareRewrite(Request, Context).get(); QVERIFY(!Cancelled.HasValue());
        QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        QCOMPARE(QDir(Directory.path()).entryList({"hc-iso-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        Request.sources_[0].imagePath_ = "../escape";
        auto Invalid = hc::iso::PrepareRewrite(Request).get(); QVERIFY(!Invalid.HasValue());
        QCOMPARE(QDir(Directory.path()).entryList({"hc-iso-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    }
};
QTEST_GUILESS_MAIN(IsoProviderTests)
#include "tstIsoProvider.moc"
