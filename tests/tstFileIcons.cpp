#include "stdafx.h"

/// Modules
#include "UniqueLibs/fileIconService.hpp"
#include "UniqueLibs/nativeIconExtractor.hpp"

/// Platform/framework headers
#include <QtTest>
#include <QApplication>
#include <QSemaphore>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QPainter>
#include <algorithm>
#include <atomic>

using namespace hc::icons;
namespace {
Result Colored(Qt::GlobalColor Color)
{
    Result Output; Output.image_ = QImage(32, 32, QImage::Format_ARGB32); Output.image_.fill(Color); return Output;
}
QByteArray IconResource(Qt::GlobalColor Color)
{
    QByteArray Data; QDataStream Stream(&Data, QIODevice::WriteOnly); Stream.setByteOrder(QDataStream::LittleEndian);
    Stream << quint32(40) << qint32(32) << qint32(64) << quint16(1) << quint16(32)
        << quint32(0) << quint32(4096) << qint32(0) << qint32(0) << quint32(0) << quint32(0);
    const auto Rgb = QColor(Color).rgba();
    for (int At = 0; At < 1024; ++At) Stream << quint32(Rgb);
    Data.append(QByteArray(128, '\0')); return Data;
}
bool SetIcon(const QString& Path, Qt::GlobalColor Color, bool Remove = false)
{
    const auto Bytes = IconResource(Color);
    QByteArray Group; QDataStream Stream(&Group, QIODevice::WriteOnly); Stream.setByteOrder(QDataStream::LittleEndian);
    Stream << quint16(0) << quint16(1) << quint16(1) << quint8(32) << quint8(32) << quint8(0) << quint8(0)
        << quint16(1) << quint16(32) << quint32(Bytes.size()) << quint16(101);
    const auto Update = BeginUpdateResourceW(reinterpret_cast<LPCWSTR>(Path.utf16()), FALSE); if (!Update) return false;
    const bool Ok = UpdateResourceW(Update, RT_ICON, MAKEINTRESOURCEW(101), 0, Remove ? nullptr : const_cast<char*>(Bytes.constData()), Remove ? 0 : DWORD(Bytes.size()))
        && UpdateResourceW(Update, RT_GROUP_ICON, MAKEINTRESOURCEW(101), 0, Remove ? nullptr : Group.data(), Remove ? 0 : DWORD(Group.size()));
    return EndUpdateResourceW(Update, !Ok) && Ok;
}
}
class FileIconTests : public QObject {
    Q_OBJECT
private slots:
    void sharedAcrossSubscribersAndWarmReads()
    {
        std::atomic<int> Loads = 0;
        FileIconService Service(nullptr, [&](const Request&) { ++Loads; return Colored(Qt::red); });
        QObject Left, Right; int Notifications = 0;
        Service.Subscribe(&Left, [&] { ++Notifications; }); Service.Subscribe(&Right, [&] { ++Notifications; });
        Request Query; Query.kind_ = Kind::Associated; Query.extension_ = "txt";
        for (int Row = 0; Row < 10000; ++Row) Service.Read(Query, Row % 2 ? &Left : &Right);
        QTRY_COMPARE(Loads.load(), 1); QTRY_VERIFY(!Service.Read(Query, &Left).pixmap_.isNull());
        const auto Pixmap = Service.Read(Query, &Left).pixmap_;
        for (int Row = 0; Row < 10000; ++Row) QCOMPARE(Service.Read(Query, &Right).pixmap_.cacheKey(), Pixmap.cacheKey());
        QCOMPARE(Service.Stats().loads_, quint64(1)); QCOMPARE(Service.Stats().conversions_, quint64(1));
        QVERIFY(Service.Stats().merged_ >= 9999); QTRY_COMPARE(Notifications, 2);
        QEvent Palette(QEvent::ApplicationPaletteChange); QCoreApplication::sendEvent(qApp, &Palette);
        QCOMPARE(Service.Read(Query, &Left).pixmap_.cacheKey(), Pixmap.cacheKey());
        QCOMPARE(Service.Stats().loads_, quint64(1));
    }
    void staleWhileRevalidateAndNegativeExpiry()
    {
        qint64 Now = 0; std::atomic<int> Loads = 0;
        FileIconService Service(nullptr, [&](const Request&) {
            const int Next = ++Loads;
            if (Next == 1) return Colored(Qt::red);
            if (Next == 2) return Colored(Qt::blue);
            Result Missing; Missing.state_ = State::NoEmbeddedIcon; return Missing;
        }, [&] { return Now; });
        QObject Owner; Service.Subscribe(&Owner, [] {});
        Request Query; Query.kind_ = Kind::Item; Query.path_ = "example.exe";
        Service.Read(Query, &Owner); QTRY_VERIFY(!Service.Read(Query, &Owner).pixmap_.isNull());
        const auto Old = Service.Read(Query, &Owner).pixmap_.cacheKey();
        Now = 60001;
        QCOMPARE(Service.Read(Query, &Owner).pixmap_.cacheKey(), Old);
        QTRY_VERIFY(Service.Read(Query, &Owner).pixmap_.cacheKey() != Old);
        Service.Invalidate({Query.Key()}); Service.Read(Query, &Owner);
        QTRY_COMPARE(Service.Read(Query, &Owner).state_, State::NoEmbeddedIcon);
        QVERIFY(Service.Read(Query, &Owner).pixmap_.isNull());
        const auto Before = Service.Stats().loads_; Now += 1999; Service.CheckExpired(); QCOMPARE(Service.Stats().loads_, Before);
        Now += 2; Service.CheckExpired(); QTRY_VERIFY(Service.Stats().loads_ > Before);
    }
    void invalidationRejectsLateWorker()
    {
        QSemaphore Started, FinishOld; std::atomic<int> Loads = 0;
        FileIconService Service(nullptr, [&](const Request&) {
            const auto Next = ++Loads;
            if (Next == 1) { Started.release(); FinishOld.acquire(); return Colored(Qt::red); }
            return Colored(Qt::blue);
        });
        const auto Release = qScopeGuard([&] { FinishOld.release(); });
        QObject Owner; Service.Subscribe(&Owner, [] {});
        Request Query; Query.kind_ = Kind::Item; Query.path_ = "rebuilt.exe";
        Service.Read(Query, &Owner); QTRY_VERIFY(Started.available() > 0);
        Service.InvalidatePaths({Query.path_}); Service.Read(Query, &Owner);
        QTRY_VERIFY(!Service.Read(Query, &Owner).pixmap_.isNull());
        QCOMPARE(Service.Read(Query, &Owner).pixmap_.toImage().pixelColor(16, 16), QColor(Qt::blue));
        FinishOld.release(); QTRY_COMPARE(Service.Stats().discarded_, quint64(1));
        QCOMPARE(Service.Read(Query, &Owner).pixmap_.toImage().pixelColor(16, 16), QColor(Qt::blue));
    }
    void overlayIsolationAndRemoval()
    {
        std::atomic<bool> Removed = false;
        FileIconService Service(nullptr, [&](const Request& Query) {
            if (Removed) { Result Output; Output.state_ = State::NoOverlay; return Output; }
            return Colored(Query.path_ == "one.lnk" ? Qt::red : Qt::blue);
        });
        QObject Owner; Service.Subscribe(&Owner, [] {});
        Request One; One.kind_ = Kind::Overlay; One.path_ = "one.lnk";
        auto Two = One; Two.path_ = "two.lnk";
        Service.Read(One, &Owner); Service.Read(Two, &Owner);
        QTRY_VERIFY(!Service.Read(One, &Owner).pixmap_.isNull() && !Service.Read(Two, &Owner).pixmap_.isNull());
        QPixmap Base(32, 32); Base.fill(Qt::green);
        const auto First = Service.Compose(Base, Service.Read(One, &Owner));
        const auto Second = Service.Compose(Base, Service.Read(Two, &Owner));
        QVERIFY(First.toImage() != Second.toImage());
        QCOMPARE(Service.Compose(Base, Service.Read(One, &Owner)).cacheKey(), First.cacheKey());
        Removed = true; Service.InvalidatePaths({One.path_}); Service.Read(One, &Owner);
        QTRY_COMPARE(Service.Read(One, &Owner).state_, State::NoOverlay);
        QCOMPARE(Service.Compose(Base, Service.Read(One, &Owner)).cacheKey(), Base.cacheKey());
    }
    void pendingAndMemoryBudgets()
    {
        FileIconService Service(nullptr, [](const Request&) { return Colored(Qt::red); });
        for (int At = 0; At < 2000; ++At) { Request Query; Query.path_ = QString::number(At); Service.Read(Query); }
        QVERIFY(Service.Pending() <= 1024); QVERIFY(Service.Stats().maxPending_ <= 1024);
        QTRY_COMPARE_WITH_TIMEOUT(Service.Pending(), 0, 15000); QVERIFY(Service.CacheCost() <= 32 * 1024);
    }
    void allSizesInvalidateAndAutomaticPause()
    {
        qint64 Now = 0;
        FileIconService Service(nullptr, [](const Request&) { return Colored(Qt::red); }, [&] { return Now; });
        QObject Owner; Service.Subscribe(&Owner, [] {});
        Request Small; Small.kind_ = Kind::Item; Small.path_ = "example.exe"; Small.pixels_ = 16;
        auto Large = Small; Large.pixels_ = 64;
        Service.Read(Small, &Owner); Service.Read(Large, &Owner);
        QTRY_COMPARE(Service.Pending(), 0); QCOMPARE(Service.Stats().loads_, quint64(2));
        Service.SetAutomaticRefresh(&Owner, false); Now = 60001; Service.CheckExpired();
        const auto Old = Service.Read(Small, &Owner).pixmap_.cacheKey(); QVERIFY(Old); QCOMPARE(Service.Pending(), 0);
        Service.Invalidate({Small.Key()}); Service.Read(Small, &Owner); Service.Read(Large, &Owner);
        QTRY_COMPARE(Service.Pending(), 0); QCOMPARE(Service.Stats().loads_, quint64(4));
        QCOMPARE(Service.Read(Small, &Owner).pixmap_.cacheKey(), Old);
        QCOMPARE(Service.Stats().conversions_, quint64(1));
    }
    void unsubscribedOwnerIsNotCalled()
    {
        FileIconService Service(nullptr, [](const Request&) { return Colored(Qt::red); });
        auto* Owner = new QObject; int Calls = 0; Service.Subscribe(Owner, [&] { ++Calls; });
        Request Query; Service.Read(Query, Owner); delete Owner;
        QTRY_COMPARE(Service.Pending(), 0); QCOMPARE(Calls, 0);
    }
    void remoteSlotDoesNotBlockLocalAndViewportTurnover()
    {
        QSemaphore ReleaseRemote; std::atomic<int> RemoteStarts = 0;
        FileIconService Service(nullptr, [&](const Request& Query) {
            if (Query.Remote()) { if (++RemoteStarts == 1) ReleaseRemote.acquire(); return Colored(Qt::blue); }
            return Colored(Qt::green);
        });
        const auto Release = qScopeGuard([&] { ReleaseRemote.release(); });
        QObject Owner; Service.Subscribe(&Owner, [] {});
        Request Remote; Remote.kind_ = Kind::Item; Remote.path_ = "//offline/a.exe";
        auto Second = Remote; Second.path_ = "//offline/b.exe";
        Request Local; Local.kind_ = Kind::Item; Local.path_ = "local.exe";
        Service.Read(Remote, &Owner); Service.Read(Second, &Owner); Service.Read(Local, &Owner);
        QTRY_VERIFY(!Service.Read(Local, &Owner).pixmap_.isNull()); QCOMPARE(RemoteStarts.load(), 1);
        ReleaseRemote.release(); QTRY_COMPARE(Service.Pending(), 0);

        FileIconService Scrolled(nullptr, [](const Request&) { return Colored(Qt::red); });
        QObject View; Request New; New.path_ = "new viewport";
        Scrolled.Subscribe(&View, [&] { Scrolled.Read(New, &View); });
        for (int At = 0; At < 1024; ++At) { Request Old; Old.path_ = QString::number(At); Scrolled.Read(Old, &View); }
        Scrolled.BeginRequests(&View); Scrolled.Read(New, &View);
        QTRY_VERIFY(!Scrolled.Read(New, &View).pixmap_.isNull());
        QVERIFY(Scrolled.Stats().maxPending_ <= 1024);
    }
    void nativeWarmListingMeasurements()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QList<Request> Queries;
        for (int At = 0; At < 256; ++At) {
            const auto Path = Directory.filePath(QString("file%1.txt").arg(At)); QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.close();
            Request Query; Query.kind_ = Kind::Overlay; Query.path_ = Path; Queries.append(Query);
        }
        Request Base; Base.kind_ = Kind::Associated; Base.extension_ = "txt";
        QList<qint64> BeforeUs, AfterUs; quint64 OldCalls = 0, OldConversions = 0;
        for (int Pass = 0; Pass < 5; ++Pass) {
            QElapsedTimer Timer; Timer.start(); const auto Background = ExtractNativeIcon(Base); ++OldCalls; QVERIFY(!Background.image_.isNull());
            for (const auto& Query : Queries) {
                const auto Layer = ExtractNativeIcon(Query); ++OldCalls;
                auto Image = Background.image_;
                if (!Layer.image_.isNull()) { Image = Image.copy(); QPainter Painter(&Image); Painter.drawImage(0, 0, Layer.image_); }
                const auto Pixmap = QPixmap::fromImage(Image); QVERIFY(!Pixmap.isNull()); ++OldConversions;
            }
            BeforeUs.append(Timer.nsecsElapsed() / 1000);
        }
        FileIconService Service; QObject Owner; Service.Subscribe(&Owner, [] {});
        Service.Read(Base, &Owner); for (const auto& Query : Queries) Service.Read(Query, &Owner);
        QTRY_COMPARE_WITH_TIMEOUT(Service.Pending(), 0, 15000);
        const auto Before = Service.Stats();
        for (int Pass = 0; Pass < 5; ++Pass) {
            QElapsedTimer Timer; Timer.start();
            for (const auto& Query : Queries) { const auto Pixmap = Service.Compose(Service.Read(Base, &Owner).pixmap_, Service.Read(Query, &Owner)); QVERIFY(!Pixmap.isNull()); }
            AfterUs.append(Timer.nsecsElapsed() / 1000);
        }
        QCOMPARE(Service.Stats().loads_, Before.loads_); QCOMPARE(Service.Stats().conversions_, Before.conversions_);
        std::sort(BeforeUs.begin(), BeforeUs.end()); std::sort(AfterUs.begin(), AfterUs.end());
        qInfo() << "Native icon pipeline only, 256 txt rows, overlays enabled, 5 warm passes; old/new median us:" << BeforeUs[2] << AfterUs[2]
            << "old/new extraction calls:" << OldCalls << Service.Stats().loads_ - Before.loads_
            << "old/new conversions:" << OldConversions << Service.Stats().conversions_ - Before.conversions_;
    }
    void actualExeResourcesAddReplaceRemove()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("rebuild.exe"); QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), Path));
        Request Query; Query.kind_ = Kind::Item; Query.path_ = Path; Query.pixels_ = 32;
        QVERIFY(SetIcon(Path, Qt::red)); const auto Red = ExtractNativeIcon(Query);
        QCOMPARE(Red.state_, State::Ready); QVERIFY(!Red.image_.isNull()); QCOMPARE(Red.image_.pixelColor(16, 16), QColor(Qt::red));
        const QFileInfo Before(Path); const auto Modified = Before.lastModified(); const auto Size = Before.size();
        QVERIFY(SetIcon(Path, Qt::blue));
        { QFile File(Path); QVERIFY(File.open(QIODevice::ReadWrite)); QVERIFY(File.setFileTime(Modified, QFileDevice::FileModificationTime)); }
        QCOMPARE(QFileInfo(Path).size(), Size); QCOMPARE(QFileInfo(Path).lastModified(), Modified);
        const auto Blue = ExtractNativeIcon(Query); QCOMPARE(Blue.state_, State::Ready); QCOMPARE(Blue.image_.pixelColor(16, 16), QColor(Qt::blue));
        QVERIFY(SetIcon(Path, Qt::blue, true)); const auto Empty = ExtractNativeIcon(Query);
        QCOMPARE(Empty.state_, State::NoEmbeddedIcon); QVERIFY(Empty.image_.isNull());
        QVERIFY(SetIcon(Path, Qt::red)); QCOMPARE(ExtractNativeIcon(Query).image_.pixelColor(16, 16), QColor(Qt::red));
    }
    void actualCustomFolderRemoval()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("icons.exe"); QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), Path)); QVERIFY(SetIcon(Path, Qt::red));
        const auto Folder = Directory.filePath("custom"); QVERIFY(QDir().mkpath(Folder));
        const auto Restore = qScopeGuard([&] { SetFileAttributesW(reinterpret_cast<LPCWSTR>(Folder.utf16()), FILE_ATTRIBUTE_NORMAL); });
        QFile Ini(QDir(Folder).filePath("desktop.ini")); QVERIFY(Ini.open(QIODevice::WriteOnly));
        QVERIFY(Ini.write(("[.ShellClassInfo]\r\nIconResource=" + QDir::toNativeSeparators(Path) + ",0\r\n").toUtf8()) > 0); Ini.close();
        QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(Folder.utf16()), FILE_ATTRIBUTE_READONLY));
        Request Query; Query.kind_ = Kind::Item; Query.path_ = Folder; Query.directory_ = true;
        const auto Red = ExtractNativeIcon(Query); QCOMPARE(Red.state_, State::Ready); QVERIFY(Red.customFolder_);
        QCOMPARE(Red.image_.pixelColor(16, 16), QColor(Qt::red));
        QVERIFY(Ini.remove()); Query.knownCustomFolder_ = true;
        const auto Removed = ExtractNativeIcon(Query); QCOMPARE(Removed.state_, State::NoCustomIcon); QVERIFY(Removed.image_.isNull());
    }
};
QTEST_MAIN(FileIconTests)
#include "tstFileIcons.moc"
