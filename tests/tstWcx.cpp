#include <QtTest>
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"

using namespace hc::storage;
using namespace hc::operations;
namespace
{
    QString Fixture() { return QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64"; }
    bool Put(const QString& Path, QByteArray Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    Locator Location(const QString& Path, std::string Entry = "/") { return {LocatorKind::Archive, Path.toStdString(), {{"wcx", std::move(Entry)}}}; }
    QByteArray Bytes(const ReadChunk& Chunk) { return QByteArray(reinterpret_cast<const char*>(Chunk.bytes_.data()), Chunk.bytes_.size()); }
    int __stdcall Continue(wchar_t*, int) { return 1; }
}
class WcxTests : public QObject
{
    Q_OBJECT
private slots:
    void unicodeReadCopyAndLifetime()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        auto Cursor = Provider->Enumerate(Location(Path), {}).get(); QVERIFY2(Cursor.HasValue(), Cursor.HasValue() ? "" : Cursor.Failure().code_.c_str());
        const auto Page = Cursor.Value()->Next(10, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), 2u);
        auto Entry = Provider->Resolve(Location(Path, "/sub/한글.txt"), {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        const auto Chunk = Stream.Value()->Read(32, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("한글!\r\n"));
        QVERIFY(Stream.Value()->Seek(0, {}).get().HasValue());
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue());
        auto Other = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        QVERIFY(!Other->Stat(Entry.Value().id_, {}).get().HasValue());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).size() == 1);
        Stream.Value().reset(); QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
        const auto Destination = Directory.filePath("out"); QVERIFY(QDir().mkdir(Destination));
        auto Native = MakeNativeReadProvider(LocatorKind::Local);
        FileOperationRequest Request; Request.destination_ = {LocatorKind::Local, Destination.toStdString()};
        Request.destinationWriter_ = MakeNativeWritableStorage(LocatorKind::Local, Native);
        auto Folder = Provider->Resolve(Location(Path, "/sub"), {}).get(); QVERIFY(Folder.HasValue());
        Request.sources_.push_back({Folder.Value(), Provider, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>(), 1);
        auto Job = SubmitFileOperation(Scheduler, std::move(Request)); QVERIFY(Job.HasValue());
        QVERIFY(Job.Value().completion_.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        const auto Done = Job.Value().completion_.get(); QCOMPARE(Done.snapshot_.state_, JobState::Completed);
        QFile Copied(Destination + "/sub/한글.txt"); QVERIFY(Copied.open(QIODevice::ReadOnly)); QCOMPARE(Copied.readAll(), QByteArray("한글!\r\n"));
        QVERIFY(Put(Path, "changed archive"));
        const auto Stale = Provider->Stat(Entry.Value().id_, {}).get(); QVERIFY(!Stale.HasValue()); QCOMPARE(Stale.Failure().category_, ErrorCategory::StaleRevision);
    }
    void failures_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("MountFailure");
        QTest::newRow("traversal") << QByteArray("T") << true;
        QTest::newRow("duplicate") << QByteArray("D") << true;
        QTest::newRow("crc") << QByteArray("C") << false;
        QTest::newRow("short-output") << QByteArray("S") << false;
        QTest::newRow("split-volume") << QByteArray("V") << false;
    }
    void failures()
    {
        QFETCH(QByteArray, Mode); QFETCH(bool, MountFailure);
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01" + Mode));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        const auto Entry = Provider->Resolve(Location(Path, "/alpha.txt"), {}).get();
        if (MountFailure) QVERIFY(!Entry.HasValue());
        else { QVERIFY(Entry.HasValue()); const auto Read = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(!Read.HasValue()); }
        auto Manager = MakeNativeMaterializer(Directory.path().toStdString()); QVERIFY(Manager.HasValue());
        QVERIFY(Manager.Value()->DrainCleanup().errors_.empty());
        QVERIFY(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty());
        QVERIFY(!QFile::exists(Directory.filePath("escape.txt")));
    }
    void cancellationAndEntryBudget()
    {
        QTemporaryDir Directory; const auto Path = Directory.filePath("sample.hct"); QVERIFY(Put(Path, "HCWCX01"));
        auto Provider = MakeWcxReadProvider(Fixture().toStdString(), Location(Path), Directory.path().toStdString());
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        const auto Cancelled = Provider->Enumerate(Location(Path), Context).get(); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        Context = {}; Context.maxEnumerationEntries_ = 1;
        const auto Bounded = Provider->Enumerate(Location(Path), Context).get(); QVERIFY(!Bounded.HasValue()); QCOMPARE(Bounded.Failure().category_, ErrorCategory::BudgetExceeded);
        QVERIFY(Provider->Enumerate(Location(Path), {}).get().HasValue());
    }
    void realSevenZipPlugin()
    {
        const auto Plugin = qEnvironmentVariable("HC_WCX_PLUGIN"); if (Plugin.isEmpty()) QSKIP("Set HC_WCX_PLUGIN to the separately obtained x64 7zip WCX binary");
        QTemporaryDir Directory; QVERIFY(QDir().mkdir(Directory.filePath("sub")));
        QVERIFY(Put(Directory.filePath("alpha.txt"), "alpha")); QVERIFY(Put(Directory.filePath("sub/한글.txt"), "한글!\r\n"));
        const auto Module = LoadLibraryExW(QDir::toNativeSeparators(Plugin).toStdWString().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        QVERIFY(Module); const auto Cleanup = qScopeGuard([&] { FreeLibrary(Module); });
        const auto Pack = reinterpret_cast<int (__stdcall*)(wchar_t*, wchar_t*, wchar_t*, wchar_t*, int)>(GetProcAddress(Module, "PackFilesW"));
        const auto Progress = reinterpret_cast<void (__stdcall*)(HANDLE, tProcessDataProcW)>(GetProcAddress(Module, "SetProcessDataProcW"));
        QVERIFY(Pack); QVERIFY(Progress); Progress(nullptr, &Continue);
        auto Archive = QDir::toNativeSeparators(Directory.filePath("real.7z")).toStdWString();
        auto Source = QDir::toNativeSeparators(Directory.path() + "/").toStdWString();
        wchar_t List[] = L"alpha.txt\0sub\\한글.txt\0";
        QCOMPARE(Pack(Archive.data(), nullptr, Source.data(), List, PK_PACK_SAVE_PATHS), 0);
        auto Provider = MakeWcxReadProvider(Plugin.toStdString(), Location(QString::fromStdWString(Archive)), Directory.path().toStdString());
        auto Entry = Provider->Resolve(Location(QString::fromStdWString(Archive), "/sub/한글.txt"), {}).get();
        QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Chunk = Stream.Value()->Read(32, {}).get(); QVERIFY(Chunk.HasValue()); QCOMPARE(Bytes(Chunk.Value()), QByteArray("한글!\r\n"));
        const auto SmbRoot = qEnvironmentVariable("HC_WCX_SMB_ROOT");
        if (!SmbRoot.isEmpty())
        {
            QTemporaryDir Remote(QDir(SmbRoot).filePath("hc-wcx-XXXXXX")); QVERIFY(Remote.isValid());
            const auto RemoteArchive = Remote.filePath("real.7z"); QVERIFY(QFile::copy(QString::fromStdWString(Archive), RemoteArchive));
            auto RemoteProvider = MakeWcxReadProvider(Plugin.toStdString(), Location(RemoteArchive), Directory.path().toStdString());
            auto RemoteEntry = RemoteProvider->Resolve(Location(RemoteArchive, "/sub/한글.txt"), {}).get();
            QVERIFY2(RemoteEntry.HasValue(), RemoteEntry.HasValue() ? "" : RemoteEntry.Failure().code_.c_str());
            auto RemoteStream = RemoteProvider->OpenRead(RemoteEntry.Value().id_, {}).get();
            QVERIFY2(RemoteStream.HasValue(), RemoteStream.HasValue() ? "" : RemoteStream.Failure().code_.c_str());
            auto RemoteChunk = RemoteStream.Value()->Read(32, {}).get(); QVERIFY(RemoteChunk.HasValue()); QCOMPARE(Bytes(RemoteChunk.Value()), QByteArray("한글!\r\n"));
        }
    }
};
QTEST_GUILESS_MAIN(WcxTests)
#include "tstWcx.moc"
