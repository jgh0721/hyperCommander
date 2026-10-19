#define NOMINMAX
#include <QtTest>
#include <QBuffer>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include "commonLibs/document/documentFormatSupport.hpp"
#include <QTemporaryDir>
#include <QToolBar>
#include <QScrollBar>
#include <QImageReader>
#include <QDataStream>
#include <QAction>
#include <QMenu>
#include <QLocalSocket>
#include <QtEndian>
#include <QMouseEvent>
#include <QScopeGuard>
#include "pluginHost/imageArchiveBroker.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "image/QImageView.h"
#include <QtCore/private/qzipwriter_p.h>
#include "UniqueLibs/document/imageBook.hpp"
#include "UniqueLibs/document/imageReadingView.hpp"
#include "UniqueLibs/document/imageFullscreen.hpp"
#include "UniqueLibs/document/imageFramePlayer.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/imageResampler.hpp"
using namespace hc::document;
namespace
{
QImage Picture(QColor Color = Qt::red, QSize Size = {80, 120})
{
    QImage I(Size, QImage::Format_RGBA8888);
    I.fill(Color);
    return I;
}
QByteArray Png(QColor Color = Qt::red)
{
    QByteArray Bytes;
    QBuffer B(&Bytes);
    B.open(QIODevice::WriteOnly);
    Picture(Color).save(&B, "PNG");
    return Bytes;
}
QByteArray TwoPageTiff()
{
    QByteArray Bytes;
    QDataStream S(&Bytes, QIODevice::WriteOnly);
    S.setByteOrder(QDataStream::LittleEndian);
    S.writeRawData("II", 2);
    S << quint16(42) << quint32(8);
    for (int Page = 0; Page < 2; ++Page)
    {
        S << quint16(9);
        const auto Tag = [&](quint16 Id, quint16 Type, quint32 Count, quint32 Value)
        { S << Id << Type << Count << Value; };
        Tag(256, 4, 1, 1);
        Tag(257, 4, 1, 1);
        Tag(258, 3, 3, 236);
        Tag(259, 3, 1, 1);
        Tag(262, 3, 1, 2);
        Tag(273, 4, 1, 242 + 3 * Page);
        Tag(277, 3, 1, 3);
        Tag(278, 4, 1, 1);
        Tag(279, 4, 1, 3);
        S << quint32(Page == 0 ? 122 : 0);
    }
    S << quint16(8) << quint16(8) << quint16(8);
    S.writeRawData("\xff\0\0\0\xff\0", 6);
    return Bytes;
}
void Fixture(const QString &Path, int Count = 3)
{
    QDir().mkpath(Path);
    for (int I = 1; I <= Count; ++I)
        Picture(QColor::fromHsv(I * 50 % 360, 220, 200)).save(Path + QString("/%1.png").arg(I));
}
QJsonObject Document(const DocumentWorkspaceWidget &W)
{
    const auto Docs = W.Snapshot()["documents"].toArray();
    return Docs.isEmpty() ? QJsonObject{} : Docs.first().toObject();
}
} // namespace
class ImageReadingTests : public QObject
{
    Q_OBJECT
  private slots:
    void initTestCase()
    {
        QCoreApplication::instance()->setProperty("documentViewsConfigDir",
                                                  qEnvironmentVariable("TEMP") + "/image-reading-config");
        QDir().mkpath(qEnvironmentVariable("TEMP") + "/image-reading-config");
    }
    void addedKernelsAndIdentity()
    {
        const auto Filters = ImageFilterRegistry::Filters();
        QCOMPARE(Filters.size(), size_t(9));
        const std::pair<QString, int> Splines[] = {{"spline16", 2}, {"spline36", 3}, {"spline64", 4}};
        for (const auto &[Name, Radius] : Splines)
        {
            const auto F = ImageFilterRegistry::Resolve(Name, {});
            QVERIFY(F.kernel_);
            QCOMPARE(F.support_, double(Radius));
            QCOMPARE(F.kernel_(0), 1.);
            QCOMPARE(F.kernel_(Radius), 0.);
            QVERIFY(F.kernel_(1.5) < 0);
            QCOMPARE(F.kernel_(-.25), F.kernel_(.25));
        }
        const auto Sharp = ImageFilterRegistry::Resolve("ewaRobidouxSharp", {});
        QVERIFY(Sharp.elliptical_);
        QCOMPARE(Sharp.support_, 2.);
        auto Input = Picture(QColor(70, 110, 150, 80));
        for (const auto &F : Filters)
        {
            ImageRenderRequest R;
            R.source_ = Input;
            R.outputSize_ = Input.size();
            R.filter_ = F.id_;
            QCOMPARE(ResampleImage(R).image_, Input);
            R.outputSize_ = QSize(47, 91);
            R.sourceToOutput_.scale(47. / 80, 91. / 120);
            auto Out = ResampleImage(R);
            QVERIFY2(Out.error_.isEmpty(), qPrintable(Out.error_));
            QCOMPARE(Out.image_.pixelColor(20, 30), Input.pixelColor(0, 0));
        }
    }
    void archiveBrokerAuthenticatesAndReclaimsCancelledReplies()
    {
        QTemporaryDir Root, Outside;
        const auto Path = Root.filePath("comic.cbz");
        {
            QZipWriter Zip(Path);
            Zip.addFile("1.png", Png(Qt::green));
            Zip.close();
        }
        auto Started = hc::plugins::ImageArchiveBroker::Start();
        QVERIFY(Started.HasValue());
        auto Broker = std::move(Started.Value());
        Broker->Allow(Path);
        Broker->BindClient(std::uint32_t(QCoreApplication::applicationPid()));
        const auto Endpoint = Broker->Endpoint();
        auto Client = hc::plugins::MakeImageArchiveClient(Endpoint);
        {
            QZipWriter Zip(Outside.filePath("other.cbz"));
            Zip.addFile("1.png", Png());
            Zip.close();
        }
        QVERIFY(!Client->Enumerate(Outside.filePath("other.cbz").toStdString(), false, {}).HasValue());
        auto Forged = Endpoint;
        Forged["token"] = "wrong";
        hc::storage::OperationContext Short;
        Short.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        QVERIFY(!hc::plugins::CallImageArchive(Forged, {{"op", "open"}, {"path", Path}}, Short).HasValue());
        for (int Attempt = 0; Attempt < 40; ++Attempt)
        {
            QLocalSocket Socket;
            Socket.connectToServer(Endpoint["address"].toString());
            QVERIFY(Socket.waitForConnected(1000));
            const auto Json = QJsonDocument(QJsonObject{{"op", "open"}, {"path", Path}, {"token", Endpoint["token"]}})
                                  .toJson(QJsonDocument::Compact);
            QByteArray Packet(4, Qt::Uninitialized);
            qToLittleEndian<quint32>(quint32(Json.size()), Packet.data());
            Packet += Json;
            QCOMPARE(Socket.write(Packet), Packet.size());
            Socket.waitForBytesWritten(1000);
            QVERIFY(Socket.waitForReadyRead(5000));
            Socket.abort();
        }
        auto Listing = Client->Enumerate(Path.toStdString(), false, {});
        QVERIFY(Listing.HasValue());
        auto Page = Listing.Value()->Next(128, {});
        QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().entries_.size(), size_t(1));
        auto Stream = Client->OpenReadSession(Path.toStdString(), {0}, {});
        QVERIFY(Stream.HasValue());
        auto Data = Stream.Value()->Read(0, 192 * 1024, {});
        QVERIFY(Data.HasValue());
        QVERIFY(Data.Value().eof_);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(Data.Value().bytes_.data()),
                            qsizetype(Data.Value().bytes_.size())),
                 Png(Qt::green));
        QVERIFY(!Client->Update({}, {}).HasValue());
        Stream.Value().reset();
        Listing.Value().reset();
        Broker.reset();
    }
    void nativeCatalogNaturalOrderAndFormats()
    {
        QTemporaryDir Root;
        QVERIFY(Root.isValid());
        Fixture(Root.path(), 2);
        Picture(Qt::blue).save(Root.filePath("10.png"));
        Picture().save(Root.filePath(".hidden.png"));
        QDir().mkdir(Root.filePath("__MACOSX"));
        auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        QCOMPARE(Book.Value()->pages_.size(), 3);
        QCOMPARE(Book.Value()->pages_[1].name_, QString("2.png"));
        QCOMPARE(Book.Value()->pages_[2].name_, QString("10.png"));
        auto Page = ReadImagePage(Book.Value(), 2);
        QVERIFY(Page.HasValue());
        QCOMPARE(Page.Value().image_.pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(ImageExtensions().contains("xpm"));
        QVERIFY(ImageExtensions().contains("ppm"));
        QVERIFY(!ImageExtensions().contains("pdf"));
        std::stop_source Stop;
        Stop.request_stop();
        hc::storage::OperationContext Context;
        Context.stopToken_ = Stop.get_token();
        QVERIFY(!OpenImageBook(Root.path(), Context).HasValue());
    }
    void modernCodecLoadingAndFixtures()
    {
        const auto Formats = QImageReader::supportedImageFormats();
        for (const auto &Extension : ImageExtensions())
        {
            bool Listed = false;
            for (const auto Known : ImageDocumentExtensions)
                Listed |= Extension == QString::fromUtf8(Known.data(), qsizetype(Known.size()));
            QVERIFY2(Listed, qPrintable(Extension));
        }
        const auto Artifacts = qEnvironmentVariable("HC_IMAGE_TEST_ARTIFACTS");
        if (!Artifacts.isEmpty())
        {
            QFile Report(Artifacts + "/codec-report.json");
            QVERIFY(Report.open(QIODevice::WriteOnly));
            Report.write(QJsonDocument(QJsonObject{{"qt", qVersion()},
                                                   {"formats", QJsonArray::fromStringList(ImageExtensions())},
                                                   {"heic", "libheif/libde265 read-only"},
                                                   {"avif", "libavif/dav1d read-only, animated first frame"}})
                             .toJson());
        }
        QVERIFY(Formats.contains("heic"));
        QVERIFY(Formats.contains("avif"));
        const QString Sources = QStringLiteral(HC_IMAGE_CODEC_SOURCES);
        const QStringList Files = {Sources + "/libheif-1.23.6/examples/example.heic",
                                   Sources + "/libheif-1.23.6/examples/example.avif",
                                   Sources + "/libavif-1.4.2/tests/data/white_1x1.avif",
                                   Sources + "/libavif-1.4.2/tests/data/paris_icc_exif_xmp.avif",
                                   Sources + "/libavif-1.4.2/tests/data/colors-animated-12bpc-keyframes-0-2-3.avif"};
        for (const auto &Path : Files)
        {
            QVERIFY2(QFile::exists(Path), qPrintable(Path));
            QImageReader Reader(Path);
            Reader.setAutoTransform(true);
            const auto Image = Reader.read();
            QVERIFY2(!Image.isNull(), qPrintable(Path + ": " + Reader.errorString()));
            if (Path.endsWith("white_1x1.avif"))
            {
                QCOMPARE(Image.size(), QSize(1, 1));
                QVERIFY(Image.pixelColor(0, 0).red() > 250);
            }
            if (Path.contains("12bpc"))
            {
                QCOMPARE(Image.format(), QImage::Format_RGBA64);
                QVERIFY(!Image.text("ImagePreview").isEmpty());
            }
        }
        const QByteArray Invalid("\0\0\0\x18"
                                 "ftypavif\0\0\0\0avifmif1",
                                 24);
        QBuffer B;
        B.setData(Invalid);
        B.open(QIODevice::ReadOnly);
        QImageReader Bad(&B);
        QVERIFY(Bad.read().isNull());
    }
    void animatedFramesDoNotBlockOrModify()
    {
        const auto Bytes =
            QByteArray::fromHex("47494638396101000100800000ff000000ff0021ff0b4e45545343415045322e30030100000021f904000a"
                                "0000002c000000000100010000020244010021f904000a0000002c00000000010001000002024c01003b");
        ImageFramePlayer Player;
        QSignalSpy Frames(&Player, &ImageFramePlayer::Frame);
        Player.Load(Bytes);
        QTRY_COMPARE(Frames.count(), 1);
        QVERIFY(Player.Supported());
        const auto First = qvariant_cast<QImage>(Frames.at(0).at(0));
        Player.Step(1);
        QTRY_COMPARE(Frames.count(), 2);
        const auto Second = qvariant_cast<QImage>(Frames.at(1).at(0));
        QVERIFY(First != Second);
        Player.Play(true);
        QTRY_VERIFY(Frames.count() > 2);
        Player.Play(false);
        QVERIFY(!Player.Playing());
    }
    void animatedWebpFramePlayback()
    {
        QFile File(QFINDTESTDATA("fixtures/animated-colors.webp"));
        QVERIFY(File.open(QIODevice::ReadOnly));ImageFramePlayer Player;QSignalSpy Frames(&Player,&ImageFramePlayer::Frame);
        Player.Load(File.readAll());QTRY_COMPARE(Frames.count(),1);QVERIFY(Player.Supported());
        const auto First=qvariant_cast<QImage>(Frames.at(0).at(0));Player.Step(1);QTRY_COMPARE(Frames.count(),2);
        const auto Second=qvariant_cast<QImage>(Frames.at(1).at(0));QVERIFY(First!=Second);
        QVERIFY(First.pixelColor(0,0).red()>200);QVERIFY(Second.pixelColor(0,0).green()>200);
        Player.Play(true);QTRY_VERIFY(Frames.count()>2);Player.Clear();QVERIFY(!Player.Playing());
    }
    void archiveCatalogReadsSortedOrdinals()
    {
        QTemporaryDir Root;
        const auto Path = Root.filePath("book 01권.cbz");
        {
            QZipWriter Zip(Path);
            Zip.addFile("10.png", Png(Qt::blue));
            Zip.addFile("2.png", Png(Qt::green));
            Zip.addFile("1.png", Png(Qt::red));
            Zip.addFile("__MACOSX/ignored.png", Png());
            Zip.close();
        }
        auto Book = OpenImageBook(Path);
        QVERIFY2(Book.HasValue(), Book.HasValue() ? "" : Book.Failure().code_.c_str());
        auto Index = [&](QString Name)
        {
            for (int I = 0; I < Book.Value()->pages_.size(); ++I)
                if (Book.Value()->pages_[I].name_ == Name)
                    return I;
            return -1;
        };
        QVERIFY(Index("__MACOSX/ignored.png") == -1);
        auto Last = ReadImagePage(Book.Value(), Index("10.png"));
        QVERIFY2(Last.HasValue(), Last.HasValue() ? "" : Last.Failure().code_.c_str());
        QCOMPARE(Last.Value().image_.pixelColor(0, 0), QColor(Qt::blue));
        auto Previous = ReadImagePage(Book.Value(), Index("1.png"));
        QVERIFY(Previous.HasValue());
        QCOMPARE(Previous.Value().image_.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(!QFile::exists(Root.filePath("escape.png")));
    }
    void nextVolumeConservativeMatching()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("1984 01권"));
        Fixture(Root.filePath("1984 02권"));
        Fixture(Root.filePath("1984 04권"));
        Fixture(Root.filePath("1984 02화"));
        QCOMPARE(NextImageBooks(Root.filePath("1984 01권")), QStringList{Root.filePath("1984 02권")});
        QVERIFY(NextImageBooks(Root.filePath("1984 02권")).isEmpty());
        QFile Z(Root.filePath("1984 02권.cbz"));
        QVERIFY(Z.open(QIODevice::WriteOnly));
        Z.close();
        QCOMPARE(NextImageBooks(Root.filePath("1984 01권")).size(), 2);
    }
    void virtualScrollModeAndPosition()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 12);
        auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        ImageReadingView View;
        View.resize(400, 500);
        View.Load(Book.Value(), Picture());
        View.SetMode(ImageReadingView::Webtoon);
        View.show();
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsBusy(), 20000);
        View.GoTo(7, .25);
        QCOMPARE(View.State()["page"].toInt(), 7);
        View.SavePosition();
        QVERIFY(View.verticalScrollBar()->maximum() > 0);
        View.SetFilter("spline36");
        View.resize(520, 600);
        QCOMPARE(View.State()["page"].toInt(), 7);
        View.Move(1, true);
        QCOMPARE(View.State()["page"].toInt(), 8);
        View.SetRightToLeft(true);
        View.SetMode(ImageReadingView::Comic);
        QCOMPARE(View.State()["page"].toInt(), 8);
        View.Cancel();
    }
    void tiffPagesAndChangedSources()
    {
        QTemporaryDir Root;
        QFile File(Root.filePath("1.tiff"));
        QVERIFY(File.open(QIODevice::WriteOnly));
        File.write(TwoPageTiff());
        File.close();
        auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        QCOMPARE(Book.Value()->pages_.size(), 2);
        QVERIFY(Book.Value()->pages_[0].id_ != Book.Value()->pages_[1].id_);
        auto First = ReadImagePage(Book.Value(), 0), Second = ReadImagePage(Book.Value(), 1);
        QVERIFY(First.HasValue());
        QVERIFY(Second.HasValue());
        QCOMPARE(First.Value().image_.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(Second.Value().image_.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(File.open(QIODevice::Append));
        File.write("changed");
        File.close();
        QVERIFY(!ReadImagePage(Book.Value(), 0).HasValue());
    }
    void corruptPageDoesNotDisableBook()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 2);
        QFile Broken(Root.filePath("1.png"));
        QVERIFY(Broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
        Broken.write("broken");
        Broken.close();
        auto Input = ReadDocumentFile(Root.path());
        QVERIFY(Input.HasValue());
        QVERIFY(Input.Value().book_);
        ImageReadingView View;
        View.resize(350, 400);
        View.Load(Input.Value().book_);
        View.show();
        QTest::qWait(100);
        View.GoTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!View.CurrentImage().isNull(), 20000);
        QCOMPARE(View.State()["page"].toInt(), 1);
        View.OpenVolume(1);
        QTRY_VERIFY(!View.IsBusy());
        View.SetFilter("spline16");
        View.GoTo(1);
        QTRY_VERIFY(!View.CurrentImage().isNull());
    }
    void nextVolumeSurvivesRenderChanges()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("Series 01권"));
        Fixture(Root.filePath("Series 02권"));
        auto Book = OpenImageBook(Root.filePath("Series 01권"));
        QVERIFY(Book.HasValue());
        ImageReadingView View;
        View.resize(400, 500);
        View.Load(Book.Value(), Picture());
        View.show();
        View.OpenVolume(1);
        View.resize(450, 550);
        View.SetFilter("spline36");
        QTRY_COMPARE_WITH_TIMEOUT(View.State()["book"].toString(), Root.filePath("Series 02권"), 20000);
        QCOMPARE(View.State()["page"].toInt(), 0);
    }
    void horizontalPanAndPageLayout()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 2);
        QVERIFY(Picture(Qt::green, {400, 1400}).save(Root.filePath("2.png")));
        auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        ImageReadingView View;
        View.resize(400, 500);
        View.Load(Book.Value(), Picture());
        View.SetFit(0);
        View.show();
        View.GoTo(1);
        QTRY_COMPARE_WITH_TIMEOUT(View.CurrentImage().height(), 1400, 20000);
        QVERIFY(View.verticalScrollBar()->maximum() > 0);
        View.GoTo(0);
        QTRY_COMPARE(View.CurrentImage().height(), 120);
        QCOMPARE(View.verticalScrollBar()->maximum(), 0);
        View.Zoom(16);
        QVERIFY(View.horizontalScrollBar()->maximum() > 0);
        View.horizontalScrollBar()->setValue(View.horizontalScrollBar()->maximum());
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsBusy(), 20000);
        View.Bookmark();
        View.GoTo(1);
        View.OpenBookmark();
        QCOMPARE(View.State()["page"].toInt(), 0);
        View.SavePosition();
    }
    void tileHaloMatchesWholeRender()
    {
        auto Source = Picture(Qt::red, {57, 83});
        for (int Y = 0; Y < 83; ++Y)
            for (int X = 0; X < 57; ++X)
                Source.setPixelColor(X, Y, QColor(X * 4, Y * 3, (X + Y) % 256, (X * Y) % 256));
        for (const auto &Filter : ImageFilterRegistry::Filters())
        {
            ImageRenderRequest R;
            R.source_ = Source;
            R.outputSize_ = {91, 117};
            R.sourceToOutput_.translate(3, 7);
            R.sourceToOutput_.rotate(8);
            R.sourceToOutput_.scale(1.2, 1.1);
            R.filter_ = Filter.id_;
            const auto Whole = ResampleImage(R);
            QVERIFY(Whole.error_.isEmpty());
            R.tile_ = {13, 17, 43, 61};
            const auto Tile = ResampleImage(R);
            QVERIFY(Tile.error_.isEmpty());
            QCOMPARE(Tile.image_, Whole.image_.copy(R.tile_));
        }
    }
    void animationClearRejectsPendingFrames()
    {
        const auto Bytes =
            QByteArray::fromHex("47494638396101000100800000ff000000ff0021ff0b4e45545343415045322e30030100000021f904000a"
                                "0000002c000000000100010000020244010021f904000a0000002c00000000010001000002024c01003b");
        ImageFramePlayer P;
        QSignalSpy Frames(&P, &ImageFramePlayer::Frame);
        P.Load(Bytes);
        P.Clear();
        QTRY_VERIFY(!P.IsBusy());
        QCOMPARE(Frames.size(), 0);
        P.Load(Bytes);
        QTRY_COMPARE(Frames.size(), 1);
        QTemporaryDir Root;
        Fixture(Root.path(), 1);
        auto Book = OpenImageBook(Root.path());
        ImageReadingView V;
        V.Load(Book.Value(), Picture(Qt::blue));
        V.ToggleAnimation();
        V.Load(Book.Value(), Picture(Qt::green));
        QTest::qWait(100);
        QCOMPARE(V.CurrentImage().pixelColor(0, 0), QColor(Qt::green));
    }
    void fullscreenUsesSelectedReader()
    {
        QTemporaryDir Root;
        Fixture(Root.path());
        auto Book = OpenImageBook(Root.path());
        QWidget W;
        W.resize(600, 400);
        auto *A = new ImageReadingView(&W);
        auto *B = new ImageReadingView(&W);
        A->Load(Book.Value(), Picture());
        B->Load(Book.Value(), Picture());
        A->hide();
        B->setGeometry(W.rect());
        B->show();
        W.show();
        ImageFullscreen F(&W, [B] { return B; });
        F.Enter();
        auto *Controls = W.findChild<QToolBar *>("imageFullscreenControls");
        QVERIFY(Controls);
        Controls->actions().at(2)->trigger();
        QCOMPARE(B->State()["page"].toInt(), 1);
        QCOMPARE(A->State()["page"].toInt(), 0);
        QTest::keyClick(B, Qt::Key_Escape);
        QVERIFY(!F.Active());
        QVERIFY(W.isVisible());
    }
    void readingToolbarCopyAndEditGuard()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 2);
        auto Input = ReadDocumentFile(Root.filePath("1.png"));
        QVERIFY(Input.HasValue());
        DocumentViewPane Pane;
        Pane.resize(900, 600);
        QVERIFY(Pane.Load(Input.Value(), false));
        Pane.show();
        QVERIFY(!Pane.Execute({{"action", "reading.mode"}, {"mode", 1}}).contains("error"));
        auto *Reader = Pane.findChild<ImageReadingView *>();
        QTRY_COMPARE(Reader->State()["pageCount"].toInt(), 2);
        Reader->GoTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!Reader->CurrentImage().isNull(), 20000);
        QVERIFY(Pane.findChild<QAction *>("imageRightToLeft")
                    ->associatedObjects()
                    .contains(Pane.findChild<QMenu *>("imageReadingOptions")));
        QImage Copied;
        QVERIFY(Copied.loadFromData(QByteArray::fromBase64(Pane.CaptureSelection()["bytes"].toString().toLatin1())));
        QCOMPARE(Copied, Reader->CurrentImage());
        QVERIFY(Pane.Execute({{"action", "image.rotate"}, {"angle", 90}}).contains("error"));
        DocumentViewPane Edited;
        QVERIFY(Edited.Load(Input.Value(), false));
        QVERIFY(!Edited.Execute({{"action", "image.rotate"}, {"angle", 90}}).contains("error"));
        QVERIFY(Edited.Execute({{"action", "reading.mode"}, {"mode", 2}}).contains("error"));
    }
    void restoredPageUsesItsOwnPixels()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 3);
        auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        {
            ImageReadingView V;
            V.Load(Book.Value(), Picture(Qt::red));
            V.GoTo(2);
            V.SavePosition();
        }
        ImageReadingView Restored;
        Restored.resize(400, 500);
        Restored.Load(Book.Value(), Picture(Qt::red));
        Restored.show();
        QCOMPARE(Restored.State()["page"].toInt(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!Restored.CurrentImage().isNull(), 20000);
        QVERIFY(Restored.CurrentImage().pixelColor(0, 0) != QColor(Qt::red));
    }
    void privateReadingIsScopedToItsView()
    {
        QTemporaryDir Root;
        Fixture(Root.path(), 2);
        const auto Book = OpenImageBook(Root.path());
        QVERIFY(Book.HasValue());
        {
            ImageReadingView Private;
            Private.SetPrivate(true);
            Private.Load(Book.Value(), Picture());
            Private.GoTo(1);
            Private.SavePosition();
            ImageReadingView Fresh;
            Fresh.Load(Book.Value(), Picture());
            QCOMPARE(Fresh.State()["page"].toInt(), 0);
            Fresh.GoTo(1);
            Fresh.SavePosition();
        }
        ImageReadingView Restored;
        Restored.Load(Book.Value(), Picture());
        QCOMPARE(Restored.State()["page"].toInt(), 1);
        QVERIFY(!QCoreApplication::instance()->property("privateReading").toBool());
    }
    void readingExportKeepsBookIdentity()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("book"), 2);
        DocumentWorkspaceWidget W(Root.filePath("recovery"));
        W.resize(900, 650);
        W.show();
        W.Handle({{"action", "open"}, {"path", Root.filePath("book")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"] == "ready", 20000);
        auto *Reader = W.findChild<ImageReadingView *>();
        QVERIFY(Reader);
        QTRY_VERIFY_WITH_TIMEOUT(!Reader->CurrentImage().isNull(), 20000);
        const auto Before = Document(W);
        const auto Destination = Root.filePath("export.png");
        const auto Id = Before["documentId"].toString();
        const auto Saved = W.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Destination}});
        QVERIFY2(!Saved.contains("error"), qPrintable(Saved["error"].toString()));
        Reader->GoTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Destination), 20000);
        QTRY_VERIFY(W.IsIdle());
        QCOMPARE(Document(W)["path"], Before["path"]);
        QVERIFY(Document(W)["sourceToken"] == Before["sourceToken"]);
        QFile F(Destination);
        QVERIFY(F.open(QIODevice::ReadOnly));
        const auto Export = F.readAll();
        F.close();
        W.Handle({{"action", "save"}, {"documentId", Id}});
        QTest::qWait(50);
        QVERIFY(F.open(QIODevice::ReadOnly));
        QCOMPARE(F.readAll(), Export);
        const auto Artifacts = qEnvironmentVariable("HC_IMAGE_TEST_ARTIFACTS");
        if (!Artifacts.isEmpty())
            QVERIFY(W.grab().save(Artifacts + "/comic-view.png"));
    }
    void fullscreenAutoHideAndEdgeReveal()
    {
        QWidget W;W.setFocusPolicy(Qt::StrongFocus);W.resize(900,600);W.show();W.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&W));ImageFullscreen F(&W);F.Enter();W.setFocus();
        const auto Cursor=QCursor::pos();const auto Cleanup=qScopeGuard([&]{F.Leave();QCursor::setPos(Cursor);});QCursor::setPos(W.mapToGlobal(W.rect().center()));
        QTest::mouseMove(&W,W.rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(!F.State()["toolbar"].toBool()&&!F.State()["status"].toBool(),5000);
        QVERIFY(F.State()["cursorHidden"].toBool());
        const QPointF Local(W.width()/2,1),Global=W.mapToGlobal(Local.toPoint());
        QMouseEvent Edge(QEvent::MouseMove,Local,Global,Qt::NoButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(&W,&Edge);
        QTRY_VERIFY_WITH_TIMEOUT(F.State()["toolbar"].toBool(),1000);QVERIFY(!F.State()["cursorHidden"].toBool());
        auto* Pin=W.findChild<QAction*>("fullscreenPinTop");QVERIFY(Pin);Pin->setChecked(true);W.setFocus();
        F.Leave();QCursor::setPos(Cursor);QVERIFY(!F.Active());QVERIFY(W.cursor().shape()!=Qt::BlankCursor);
    }
    void fullscreenRoundTripAndCursor()
    {
        QWidget W;
        W.resize(600, 400);
        W.show();
        ImageFullscreen F(&W);
        F.Enter();
        QVERIFY(F.Active());
        QVERIFY(W.isFullScreen());
        F.Leave();
        QVERIFY(!F.Active());
        QVERIFY(!W.isFullScreen());
        QVERIFY(W.cursor().shape() != Qt::BlankCursor);
        QCOMPARE(W.size(), QSize(600, 400));
    }
    void workspaceModeCommandsPreserveOriginal()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("book"));
        DocumentWorkspaceWidget W(Root.filePath("recovery"));
        W.resize(700, 600);
        W.show();
        auto Reply = W.Handle({{"action", "open"}, {"path", Root.filePath("book/1.png")}});
        QVERIFY(!Reply.contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"] == "ready", 20000);
        const auto Id = Document(W)["documentId"].toString();
        auto Mode = W.Handle({{"action", "reading.mode"}, {"mode", 2}, {"documentId", Id}});
        QVERIFY2(!Mode.contains("error"), qPrintable(Mode["error"].toString()));
        QTRY_COMPARE_WITH_TIMEOUT(Document(W)["view"].toObject()["reading"].toObject()["pageCount"].toInt(), 3, 20000);
        W.Handle({{"action", "reading.page"}, {"page", 2}, {"documentId", Id}});
        QVERIFY(!Document(W)["dirty"].toBool());
        QVERIFY(!Document(W)["view"].toObject()["reading"].toObject().isEmpty());
        W.Handle({{"action", "cancel"}, {"documentId", Id}});
    }
};
QTEST_MAIN(ImageReadingTests)
#include "tstImageReading.moc"
