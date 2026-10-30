#define NOMINMAX
#include <QtTest>
#include <QBuffer>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include "commonLibs/document/documentFormatSupport.hpp"
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QScrollBar>
#include <QImageReader>
#include <QDataStream>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QLocalSocket>
#include <QtEndian>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QLineEdit>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <algorithm>
#include "pluginHost/imageArchiveBroker.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "image/QImageView.h"
#include <QtCore/private/qzipwriter_p.h>
#include "UniqueLibs/document/imageBook.hpp"
#include "UniqueLibs/document/imageZipRecovery.hpp"
#include "UniqueLibs/document/imagePartialDecoder.hpp"
#include "UniqueLibs/document/imageReadingView.hpp"
#include "UniqueLibs/document/imageFullscreen.hpp"
#include "UniqueLibs/document/imageFramePlayer.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/imageResampler.hpp"
#include "UniqueLibs/nativeStorageRevision.hpp"
#include "UniqueLibs/document/imageDecodeCache.hpp"
#include "UniqueLibs/document/documentStorageAdapter.hpp"
using namespace hc::document;
namespace
{
// Decoder-level fixture: the archive engine has already yielded decoded bytes,
// then reports damage or an authority failure. This contract is format-neutral.
namespace st=hc::storage;namespace plug=hc::plugins;
class InterruptedArchive final : public plug::IArchiveBackend {
public:
    QByteArray bytes_;st::ErrorCategory failure_=st::ErrorCategory::Unavailable;
    st::Result<std::unique_ptr<plug::IArchiveListing>> Enumerate(std::string,bool,st::OperationContext) override{return st::Error{st::ErrorCategory::Unsupported,"unused"};}
    st::Result<st::Unit> Extract(std::string,plug::ArchiveHeader,std::string,st::OperationContext) override{return st::Error{st::ErrorCategory::Unsupported,"unused"};}
    st::Result<st::Unit> Update(plug::ArchiveUpdate,st::OperationContext) override{return st::Error{st::ErrorCategory::Unsupported,"unused"};}
    st::Result<std::unique_ptr<plug::IArchiveReadSession>> OpenReadSession(std::string,std::vector<std::uint32_t>,st::OperationContext) override {
        struct Stream final:plug::IArchiveReadSession {
            QByteArray bytes_;st::ErrorCategory failure_;bool sent_=false;
            st::Result<st::ReadChunk> Read(std::uint32_t,std::size_t,st::OperationContext) override {
                if(sent_)return st::Error{failure_,"archive-interrupted"};sent_=true;
                const auto* First=reinterpret_cast<const std::byte*>(bytes_.constData());return st::ReadChunk{std::vector<std::byte>(First,First+bytes_.size()),false};
            }
        };
        auto Result=std::make_unique<Stream>();Result->bytes_=bytes_;Result->failure_=failure_;return std::unique_ptr<plug::IArchiveReadSession>(std::move(Result));
    }
};
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
    void archiveBrokerStreamsOrderedPageBatch()
    {
        QTemporaryDir Root;const auto Path=Root.filePath("batch.cbz");
        {QZipWriter Zip(Path);for(int I=0;I<3;++I)Zip.addFile(QString::number(I)+".png",Png(I==1?Qt::green:Qt::red));Zip.close();}
        auto Started=hc::plugins::ImageArchiveBroker::Start();QVERIFY(Started.HasValue());auto Broker=std::move(Started.Value());
        Broker->Allow(Path);Broker->BindClient(std::uint32_t(QCoreApplication::applicationPid()));
        auto Client=hc::plugins::MakeImageArchiveClient(Broker->Endpoint());
        auto Stream=Client->OpenReadSession(Path.toStdString(),{0,1,2},{});QVERIFY(Stream.HasValue());
        for(std::uint32_t I=0;I<3;++I){QByteArray Bytes;bool End=false;while(!End){auto Chunk=Stream.Value()->Read(I,192*1024,{});QVERIFY(Chunk.HasValue());
            Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),qsizetype(Chunk.Value().bytes_.size()));End=Chunk.Value().eof_;}
            QCOMPARE(Bytes,Png(I==1?Qt::green:Qt::red));}
        QVERIFY(!Client->OpenReadSession(Path.toStdString(),{2,1},{}).HasValue());
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
    void imagePresentationDoesNotWaitForRefinement()
    {
        DocumentViewPane Pane;Pane.resize(900,600);
        TextInput Input;Input.viewId_="image";Input.path_="presentation.png";
        Input.image_=Picture(Qt::blue,QSize(3500,2400));
        QVERIFY(Pane.Load(Input,false,false));
        QVERIFY(Pane.IsBusy());
        Pane.ShowLoadingPreview(Picture(Qt::red));
        QVERIFY(Pane.HasLoadingPreview());QVERIFY(Pane.ImagePresentationReady());QVERIFY(Pane.IsBusy());
        Pane.ShowLoadingPreview({});QVERIFY(!Pane.HasLoadingPreview());
        // The host keeps the pane hidden until this state is published.
        QTRY_VERIFY_WITH_TIMEOUT(Pane.ImagePresentationReady(),10000);
        QCOMPARE(Pane.findChild<QImageView*>()->Content(),Input.image_);
    }
    void failedInitialSizeRetriesAfterResize()
    {
        QImageView View;View.resize(30,30);
        View.LoadContent(Picture(Qt::blue,QSize(5000,7000)),"large.png",false);
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsRendering(),10000);
        QVERIFY(!View.RenderError().isEmpty());
        View.resize(1000,700);View.show();
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsRendering() && View.RenderError().isEmpty(),10000);
        QVERIFY(View.HasDisplayImage());
    }
    void commonCodecPreviewPreservesFullInput_data()
    {
        QTest::addColumn<QByteArray>("Format");
        for(const auto& Format:QList<QByteArray>{"PNG","JPEG","BMP","TIFF","WEBP"})
            QTest::newRow(Format.constData())<<Format;
    }
    void commonCodecPreviewPreservesFullInput()
    {
        QFETCH(QByteArray,Format);QTemporaryDir Root;
        const auto Path=Root.filePath("image."+QString::fromLatin1(Format).toLower());
        QImage Original=Picture(Qt::cyan,QSize(2400,1800));
        if(!Original.save(Path,Format.constData()))QSKIP("Optional codec writer unavailable");
        QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));const auto Bytes=File.readAll();File.close();
        ImageDecodeCache::Clear();int Previews=0;
        const auto Input=ReadDocumentFile(Path,{},false,[&](QImage Image){
            ++Previews;QVERIFY(!Image.isNull());QVERIFY(Image.width()<=1024);QVERIFY(Image.height()<=1024);
        });
        QVERIFY(Input.HasValue());QVERIFY(Previews>0);QCOMPARE(Input.Value().image_.size(),Original.size());
        QCOMPARE(Input.Value().bytes_,Bytes);QCOMPARE(Input.Value().hash_,QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256));
        ImageDecodeCache::Clear();std::stop_source Stop;hc::storage::OperationContext Context;Context.stopToken_=Stop.get_token();
        const auto Cancelled=ReadDocumentFile(Path,Context,false,[&](QImage){Stop.request_stop();});
        QVERIFY(!Cancelled.HasValue());QCOMPARE(Cancelled.Failure().category_,hc::storage::ErrorCategory::Cancelled);
        QVERIFY(!ImageDecodeCache::Find(Path).has_value());
    }
    void jpegPublishesBoundedPreviewBeforeCompletion()
    {
        QTemporaryDir Root;QImage Image(1400,1400,QImage::Format_RGB32);quint32 Seed=1;
        for(int Y=0;Y<Image.height();++Y){auto* Row=reinterpret_cast<QRgb*>(Image.scanLine(Y));
            for(int X=0;X<Image.width();++X){Seed=Seed*1664525U+1013904223U;Row[X]=0xff000000U|(Seed>>8);}}
        const auto Path=Root.filePath("large.jpg");QVERIFY(Image.save(Path,"JPEG",98));QVERIFY(QFileInfo(Path).size()>1024*1024);
        auto Book=OpenImageBook(Root.path());QVERIFY(Book.HasValue());int Count=0;QSize PreviewSize;
        auto Page=ReadImagePage(Book.Value(),0,{},[&](QImage Preview){++Count;PreviewSize=Preview.size();});
        QVERIFY(Page.HasValue());QVERIFY(Count>0);QVERIFY(PreviewSize.width()<=1024&&PreviewSize.height()<=1024);
        QCOMPARE(Page.Value().image_.size(),Image.size());QVERIFY(Page.Value().warning_.isEmpty());
        ImageDecodeCache::Clear();int StandalonePreviews=0;
        auto Single=ReadDocumentFile(Path,{},false,[&](QImage Preview){++StandalonePreviews;QVERIFY(Preview.width()<=1024);});
        QVERIFY(Single.HasValue());QVERIFY(StandalonePreviews>0);QCOMPARE(Single.Value().image_.size(),Image.size());
        ImageDecodeCache::Clear();std::stop_source Stop;hc::storage::OperationContext Context;Context.stopToken_=Stop.get_token();
        auto Cancelled=ReadDocumentFile(Path,Context,false,[&](QImage){Stop.request_stop();});
        QVERIFY(!Cancelled.HasValue());QCOMPARE(Cancelled.Failure().category_,hc::storage::ErrorCategory::Cancelled);
    }
    void partialPngAndWebpPreserveAvailableRows_data(){QTest::addColumn<QByteArray>("Format");QTest::newRow("png")<<QByteArray("PNG");QTest::newRow("webp")<<QByteArray("WEBP");}
    void partialPngAndWebpPreserveAvailableRows()
    {
        QFETCH(QByteArray,Format);QImage Original(320,360,QImage::Format_RGBA8888);quint32 Seed=31;
        for(int Y=0;Y<Original.height();++Y)for(int X=0;X<Original.width();++X){Seed=Seed*1664525U+1013904223U;Original.setPixelColor(X,Y,QColor(Seed&255,(Seed>>8)&255,(Seed>>16)&255));}
        QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);QVERIFY(Original.save(&Buffer,Format.constData(),95));Buffer.close();
        const auto Prefix=Bytes.left(Bytes.size()/2);auto Preview=ImagePartialDecoder::Decode(Prefix);
        QVERIFY2(!Preview.isNull(),Format.constData());const int Rows=Preview.text("PreviewRows").toInt();QVERIFY(Rows>0&&Rows<Original.height());
        QVERIFY(Preview.width()<=1024&&Preview.height()<=1024);QVERIFY(Preview.pixelColor(0,0).alpha()>0);QCOMPARE(Preview.pixelColor(0,Preview.height()-1).alpha(),0);
        QTemporaryDir Root;const auto Path=Root.filePath("partial."+QString::fromLatin1(Format).toLower());QFile File(Path);QVERIFY(File.open(QIODevice::WriteOnly));File.write(Prefix);File.close();
        auto Book=OpenImageBook(Root.path());QVERIFY(Book.HasValue());auto Page=ReadImagePage(Book.Value(),0);QVERIFY(Page.HasValue());QCOMPARE(Page.Value().warning_,QString("image-page-partial-preview"));
        QImage Standalone;auto Single=ReadDocumentFile(Path,{},false,[&](QImage Image){Standalone=std::move(Image);});QVERIFY(!Single.HasValue());QVERIFY(!Standalone.isNull());
        DocumentWorkspaceWidget Workspace(Root.filePath("recovery"));Workspace.resize(600,500);Workspace.show();
        QVERIFY(!Workspace.Handle({{"action","open"},{"path",Path}}).contains("error"));QTRY_VERIFY(Workspace.IsIdle());
        const auto State=Workspace.Snapshot()["documents"].toArray().first().toObject();QVERIFY(!State["error"].toString().isEmpty());QVERIFY(!State["canSave"].toBool());QVERIFY(!State["canSaveAs"].toBool());
        auto* Overlay=Workspace.findChild<QWidget*>("documentLoadingPreview");QVERIFY(Overlay);QVERIFY(Overlay->property("partialImage").toBool());QVERIFY(!Overlay->isHidden());
        Workspace.RequestClose(); QTRY_VERIFY(Workspace.IsIdle()); QVERIFY(Workspace.RequestClose());
        QVERIFY(ImagePartialDecoder::Decode(Prefix.left(25)).isNull());
    }
    void archiveDamageRetainsPixelsButAuthorityFailureDoesNot()
    {
        QTemporaryDir Root;Fixture(Root.path(),1);auto Book=OpenImageBook(Root.path());QVERIFY(Book.HasValue());
        auto Engine=std::make_shared<InterruptedArchive>();Engine->bytes_=Png(Qt::red);Engine->bytes_.chop(12);
        Book.Value()->archive_=Engine;Book.Value()->pages_[0].entry_.encrypted_=true;
        auto Partial=ReadImagePage(Book.Value(),0);QVERIFY(Partial.HasValue());QVERIFY(!Partial.Value().warning_.isEmpty());QVERIFY(!Partial.Value().image_.isNull());
        Engine->failure_=st::ErrorCategory::AccessDenied;auto Denied=ReadImagePage(Book.Value(),0);QVERIFY(!Denied.HasValue());QCOMPARE(Denied.Failure().category_,st::ErrorCategory::AccessDenied);
        Engine->failure_=st::ErrorCategory::Cancelled;auto Cancelled=ReadImagePage(Book.Value(),0);QVERIFY(!Cancelled.HasValue());QCOMPARE(Cancelled.Failure().category_,st::ErrorCategory::Cancelled);
    }
    void gridAvifPreviewUsesDecodedRows()
    {
        // libavif 1.4.2 BSD test corpus; copyright retained beside the fixture.
        QFile File(QFINDTESTDATA("fixtures/libavif/sofa_grid1x5_420.avif"));QVERIFY(File.open(QIODevice::ReadOnly));const auto Bytes=File.readAll();
        auto Complete=ImagePartialDecoder::Decode(Bytes);QVERIFY(!Complete.isNull());bool Found=false;
        for(int Part=2;Part<10;++Part){auto Preview=ImagePartialDecoder::Decode(Bytes.left(Bytes.size()*Part/10));
            if(!Preview.isNull()&&Preview.pixelColor(0,Preview.height()-1).alpha()==0){QVERIFY(Preview.text("PreviewRows").toInt()>0);Found=true;break;}}
        QVERIFY2(Found,"grid AVIF must expose completed rows before the remaining tiles arrive");
    }
    void jpegTrailingPayloadRemainsComplete_data()
    {
        QTest::addColumn<int>("TrailingBytes");
        QTest::newRow("plain") << 0;
        QTest::newRow("camera-footer") << 256;
        QTest::newRow("motion-payload") << 1024 * 1024;
    }
    void jpegTrailingPayloadRemainsComplete()
    {
        QFETCH(int, TrailingBytes);
        QTemporaryDir Root; QImage Image(120, 160, QImage::Format_RGB32); Image.fill(Qt::red);
        QByteArray Bytes; QBuffer Buffer(&Bytes); Buffer.open(QIODevice::WriteOnly); QVERIFY(Image.save(&Buffer, "JPEG")); Buffer.close();
        Bytes.insert(2, QByteArray::fromHex("ffe1000641ffd942"));
        Bytes.append(QByteArray(TrailingBytes, 'p'));
        QFile File(Root.filePath("page.jpg")); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        auto Book = OpenImageBook(Root.path()); QVERIFY(Book.HasValue()); auto Page = ReadImagePage(Book.Value(), 0);
        QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().image_.size(), Image.size()); QVERIFY(Page.Value().warning_.isEmpty());
        auto Single = ReadDocumentFile(File.fileName()); QVERIFY(Single.HasValue());
        QCOMPARE(Single.Value().image_.size(), Image.size()); QCOMPARE(Single.Value().bytes_, Bytes);
    }
    void truncatedJpegRemainsViewableWithWarning()
    {
        QTemporaryDir Root;QImage Image(120,160,QImage::Format_RGB32);Image.fill(Qt::red);
        QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);QVERIFY(Image.save(&Buffer,"JPEG"));
        QVERIFY(Bytes.endsWith(QByteArray("\xff\xd9",2)));Bytes.chop(2);
        Bytes.insert(2, QByteArray::fromHex("ffe1000641ffd942"));
        QFile File(Root.filePath("page.jpg"));QVERIFY(File.open(QIODevice::WriteOnly));File.write(Bytes);File.close();
        auto Book=OpenImageBook(Root.path());QVERIFY(Book.HasValue());auto Page=ReadImagePage(Book.Value(),0);
        QVERIFY(Page.HasValue());QVERIFY(!Page.Value().image_.isNull());QCOMPARE(Page.Value().warning_,QString("image-jpeg-incomplete"));
        QCOMPARE(QFileInfo(File).size(),qint64(Bytes.size()));
        auto Single=ReadDocumentFile(File.fileName());QVERIFY(!Single.HasValue());
        QCOMPARE(Single.Failure().code_,std::string("document-image-incomplete"));
    }
    void zip64AndDescriptorRecovery_data()
    {
        QTest::addColumn<bool>("Deflate");QTest::addColumn<bool>("Wide");QTest::addColumn<bool>("Signature");QTest::addColumn<bool>("Descriptor");
        for(bool D:{false,true})for(bool W:{false,true})for(bool S:{false,true})
            QTest::newRow(qPrintable(QString("descriptor-%1-%2-%3").arg(D).arg(W).arg(S)))<<D<<W<<S<<true;
        QTest::newRow("zip64-local-size")<<true<<true<<false<<false;
    }
    void zip64AndDescriptorRecovery()
    {
        QFETCH(bool,Deflate);QFETCH(bool,Wide);QFETCH(bool,Signature);QFETCH(bool,Descriptor);
        const auto Image=Png(Qt::red);auto Packed=Image;
        if(Deflate){const auto Z=qCompress(Image,6);Packed=Z.mid(6,Z.size()-10);}
        const auto Crc=quint32(crc32(0,reinterpret_cast<const Bytef*>(Image.constData()),uInt(Image.size())));
        const auto Number=[](quint64 V,int N){QByteArray Out;for(int I=0;I<N;++I)Out+=char(V>>(8*I));return Out;};
        QByteArray Extra;if(Wide)Extra=Number(1,2)+Number(16,2)+Number(Descriptor?0:Image.size(),8)+Number(Descriptor?0:Packed.size(),8);
        QByteArray Bytes("PK\x03\x04",4);Bytes+=Number(Wide?45:20,2)+Number(Descriptor?8:0,2)+Number(Deflate?8:0,2)+Number(0,4);
        Bytes+=Number(Descriptor?0:Crc,4)+Number(Wide?UINT32_MAX:Descriptor?0:Packed.size(),4)+Number(Wide?UINT32_MAX:Descriptor?0:Image.size(),4);
        Bytes+=Number(5,2)+Number(Extra.size(),2)+"1.png"+Extra+Packed;
        if(Descriptor){if(Signature)Bytes+=QByteArray("PK\x07\x08",4);Bytes+=Number(Crc,4)+Number(Packed.size(),Wide?8:4)+Number(Image.size(),Wide?8:4);}
        QTemporaryDir Root;QFile File(Root.filePath("broken.cbz"));QVERIFY(File.open(QIODevice::WriteOnly));File.write(Bytes);File.close();
        auto Entries=zipRecovery::Scan(File.fileName(),{});QVERIFY2(Entries.HasValue(),Entries.HasValue()?"":Entries.Failure().code_.c_str());QCOMPARE(Entries.Value().size(),1);
        auto Recovered=zipRecovery::Read(File.fileName(),Entries.Value().first(),{});QVERIFY(Recovered.HasValue());QCOMPARE(Recovered.Value().bytes_,Image);QVERIFY(Recovered.Value().warning_.isEmpty());
        QVERIFY(File.open(QIODevice::ReadOnly));QCOMPARE(File.readAll(),Bytes);File.close();
        Bytes[6]=char(quint8(Bytes[6])|1);QVERIFY(File.open(QIODevice::WriteOnly|QIODevice::Truncate));File.write(Bytes);File.close();
        QVERIFY(!zipRecovery::Scan(File.fileName(),{}).HasValue());
    }
    void missingZipDirectoryRetainsReadablePages()
    {
        QTemporaryDir Root;const auto Path=Root.filePath("damaged.cbz");
        {QZipWriter Zip(Path);Zip.addFile("1.png",Png(Qt::red));Zip.addFile("2.png",Png(Qt::green));Zip.close();}
        QFile File(Path);QVERIFY(File.open(QIODevice::ReadWrite));const auto Bytes=File.readAll();
        const auto Directory=Bytes.indexOf(QByteArray("PK\x01\x02",4));QVERIFY(Directory>0);
        QVERIFY(File.resize(Directory));File.close();
        auto Book=OpenImageBook(Path);QVERIFY2(Book.HasValue(),Book.HasValue()?"":Book.Failure().code_.c_str());
        QCOMPARE(Book.Value()->pages_.size(),2);
        auto First=ReadImagePage(Book.Value(),0),Second=ReadImagePage(Book.Value(),1);
        QVERIFY(First.HasValue());QVERIFY(Second.HasValue());
        QCOMPARE(First.Value().image_.pixelColor(0,0),QColor(Qt::red));
        QCOMPARE(Second.Value().image_.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(QFileInfo(Path).size(),qint64(Directory));
    }
    void tiffPagesAndChangedSources()
    {
        QTemporaryDir Root;
        QFile File(Root.filePath("1.tiff"));
        QVERIFY(File.open(QIODevice::WriteOnly));
        File.write(TwoPageTiff());
        File.close();
        auto Deferred=OpenImageBook(Root.path(),{},false);QVERIFY(Deferred.HasValue());
        QCOMPARE(Deferred.Value()->pages_.size(),1);QVERIFY(Deferred.Value()->subpagesPending_);
        ImageReadingView View;View.resize(350,400);View.Load(Deferred.Value());View.show();
        QTRY_VERIFY(!View.State()["discoveringPages"].toBool());
        QCOMPARE(View.State()["pageCount"].toInt(),2);
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
        View.GoTo(0);
        QTRY_VERIFY(!View.State()["pageError"].toString().isEmpty());
        QVERIFY(Picture(Qt::blue).save(Root.filePath("1.png")));
        View.RetryPage();
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsBusy() && !View.CurrentImage().isNull(),20000);
        QCOMPARE(View.CurrentImage().pixelColor(0,0),QColor(Qt::blue));
        QVERIFY(View.State()["pageError"].toString().isEmpty());
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
    void imageEditingToolbarDefaults_data()
    {
        QTest::addColumn<bool>("Hosted");
        QTest::newRow("HyperEditor") << false;
        QTest::newRow("HyperViewer") << true;
    }
    void imageEditingToolbarDefaults()
    {
        QFETCH(bool, Hosted);
        QTemporaryDir Root; Fixture(Root.path(), 2);
        DocumentWorkspaceWidget W(Root.filePath("recovery"), Hosted);
        W.resize(1200, 700); W.show();
        W.Handle({{"action", "open"}, {"path", Root.filePath("1.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"] == "ready", 20000);
        const auto Id = Document(W)["documentId"].toString();
        auto* Pane = W.findChild<DocumentViewPane*>(); QVERIFY(Pane);
        auto* Toolbar = Pane->findChild<QToolBar*>("viewToolbar"); QVERIFY(Toolbar);
        auto* EditBar = Pane->findChild<QToolBar*>("imageEditToolbar"); QVERIFY(EditBar);
        auto* Edit = Pane->findChild<QAction*>("imageEditMode"); QVERIFY(Edit);
        auto* Image = Pane->findChild<QImageView*>(); QVERIFY(Image);
        QCOMPARE(EditBar->isHidden(), Hosted);
        QCOMPARE(Edit->isChecked(), !Hosted);
        QVERIFY(Toolbar->actions().contains(Edit));
        QVERIFY(!Edit->icon().isNull());
        QVERIFY(Image->isEnabled());
        auto* Fit = Pane->findChild<QComboBox*>("imageReadingFit"); QVERIFY(Fit);
        QCOMPARE(Fit->count(), 5);
        Fit->setCurrentIndex(4); emit Fit->activated(4);
        QCOMPARE(Image->fitMode(), QImageView::StretchToWindow);
        auto* Button = qobject_cast<QToolButton*>(Toolbar->widgetForAction(Edit)); QVERIFY(Button);
        QCOMPARE(Button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        if (Hosted) QTest::mouseClick(Button, Qt::LeftButton);
        QVERIFY(Edit->isChecked()); QVERIFY(EditBar->isVisible());
        QVERIFY(!Document(W)["dirty"].toBool());
        Image->setEditTool(QImageView::Rectangle);
        QTest::mouseClick(Button, Qt::LeftButton);
        QVERIFY(EditBar->isHidden());
        QCOMPARE(Image->currentEditTool(), QImageView::None);
        QTest::mouseClick(Button, Qt::LeftButton);
        QVERIFY(EditBar->isVisible());
        emit Pane->OpenRequested(Root.filePath("2.png"));
        QTRY_COMPARE_WITH_TIMEOUT(Document(W)["path"].toString(), Root.filePath("2.png"), 20000);
        QCOMPARE(Document(W)["documentId"].toString(), Id);
        QCOMPARE(Pane->findChild<QToolBar*>("imageEditToolbar")->isHidden(), Hosted);
        QVERIFY(!Document(W)["dirty"].toBool());
        W.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); });
        W.RequestClose();
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
        QVERIFY(Pane.findChild<QToolBar*>("imageEditToolbar")->isHidden());
        QVERIFY(!Pane.findChild<QAction*>("imageEditMode")->isEnabled());
        QCOMPARE(Pane.findChild<QComboBox*>("imageReadingFit")->count(), 4);
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
    void imageSpaceAndBackspaceUseOriginalDirectory()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("original"), 3);
        Fixture(Root.filePath("private"), 2);
        const auto Original = Root.filePath("original/2.png");
        const auto Input = ReadDocumentFile(Root.filePath("private/2.png"));
        QVERIFY(Input.HasValue());
        for (const bool BindBeforeLoad : {true, false}) {
            DocumentViewPane Pane;
            Pane.resize(700, 500);
            if (BindBeforeLoad) Pane.SetReadingSource(Original, {});
            QVERIFY(Pane.Load(Input.Value(), false));
            if (!BindBeforeLoad) Pane.SetReadingSource(Original, {});
            Pane.show(); Pane.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&Pane)); Pane.Focus();
            auto* Image = Pane.findChild<QImageView*>();
            QVERIFY(Image);
            QSignalSpy Open(&Pane, &DocumentViewPane::OpenRequested);
            QTest::keyClick(Image, Qt::Key_Space);
            QCOMPARE(Open.size(), 1);
            QCOMPARE(Open.last().at(0).toString(), Root.filePath("original/3.png"));
            QTest::keyClick(Image, Qt::Key_Backspace);
            QCOMPARE(Open.size(), 2);
            QCOMPARE(Open.last().at(0).toString(), Root.filePath("original/1.png"));
            QTest::keyClick(Image, Qt::Key_Backspace, Qt::ControlModifier);
            QCOMPARE(Open.size(), 2);
            QLineEdit Field(Image); Field.show(); Field.setFocus();
            QTest::keyClick(&Field, Qt::Key_Space);
            QCOMPARE(Field.text(), QString(" "));
            QTest::keyClick(&Field, Qt::Key_Backspace);
            QVERIFY(Field.text().isEmpty());
            QCOMPARE(Open.size(), 2);
            Field.hide(); Pane.Focus();
            ImageFullscreen Fullscreen(&Pane);
            Fullscreen.Enter(); Pane.Focus();
            QTest::keyClick(Image, Qt::Key_Backspace);
            QCOMPARE(Open.size(), 3);
            QCOMPARE(Open.last().at(0).toString(), Root.filePath("original/1.png"));
            Fullscreen.Leave();
        }
    }
    void imageKeysOpenSiblingAndReturnToPrevious()
    {
        QTemporaryDir Root;
        Fixture(Root.filePath("images"), 3);
        DocumentWorkspaceWidget W(Root.filePath("recovery"));
        W.resize(800, 600); W.show(); W.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&W));
        const auto Current = [&] {
            const auto State = W.Snapshot();
            for (const auto& Item : State["documents"].toArray()) {
                const auto Doc = Item.toObject();
                if (Doc["documentId"] == State["activeDocumentId"]) return Doc;
            }
            return QJsonObject{};
        };
        W.Handle({{"action", "open"}, {"path", Root.filePath("images/2.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Current()["lifecycle"] == "ready", 20000);
        const auto Id = Current()["documentId"].toString();
        auto* Pane = W.findChild<DocumentViewPane*>(); QVERIFY(Pane); Pane->Focus();
        QTest::keyClick(Pane->InputWidget(), Qt::Key_Space);
        QTRY_COMPARE_WITH_TIMEOUT(Current()["path"].toString(), Root.filePath("images/3.png"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(Current()["lifecycle"] == "ready", 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!Current()["loading"].toBool(), 20000);
        QCOMPARE(W.Snapshot()["documents"].toArray().size(), 1);
        QCOMPARE(Current()["documentId"].toString(), Id);
        auto* Image = qobject_cast<QImageView*>(QApplication::focusWidget()); QVERIFY(Image);
        QTest::keyClick(Image, Qt::Key_Backspace);
        QTRY_COMPARE_WITH_TIMEOUT(Current()["path"].toString(), Root.filePath("images/2.png"), 20000);
        QCOMPARE(W.Snapshot()["documents"].toArray().size(), 1);
        QCOMPARE(Current()["documentId"].toString(), Id);
    }
    void navigationKeepsDirtyImageOnCancelOrDecodeFailure()
    {
        QTemporaryDir Root; Fixture(Root.path(), 3);
        DocumentWorkspaceWidget W(Root.filePath("recovery")); W.resize(800,600); W.show(); W.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&W));
        W.Handle({{"action","open"},{"path",Root.filePath("2.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"]=="ready",20000);
        const auto Id=Document(W)["documentId"].toString();
        W.Handle({{"action","image.rotate"},{"angle",90},{"documentId",Id}});
        QVERIFY(Document(W)["dirty"].toBool());
        auto* Pane=W.findChild<DocumentViewPane*>();QVERIFY(Pane);Pane->Focus();
        W.SetClosePrompt([](const QString&){return int(QMessageBox::Cancel);});
        QTest::keyClick(Pane->InputWidget(),Qt::Key_Space);
        QCOMPARE(Document(W)["path"].toString(),Root.filePath("2.png"));QVERIFY(Document(W)["dirty"].toBool());
        QFile Broken(Root.filePath("3.png"));QVERIFY(Broken.open(QIODevice::WriteOnly|QIODevice::Truncate));Broken.write("broken");Broken.close();
        W.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
        QTest::keyClick(Pane->InputWidget(),Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!Document(W)["loading"].toBool(),20000);
        QVERIFY(!Document(W)["error"].toString().isEmpty());
        QCOMPARE(Document(W)["path"].toString(),Root.filePath("2.png"));QVERIFY(Document(W)["dirty"].toBool());
        QCOMPARE(Pane->findChild<QImageView*>()->Content().size(),QSize(120,80));
        QVERIFY(Picture(Qt::blue).save(Root.filePath("3.png")));
        W.SetClosePrompt([](const QString&){return int(QMessageBox::Save);});
        QTest::keyClick(Pane->InputWidget(),Qt::Key_Space);
        QTRY_COMPARE_WITH_TIMEOUT(Document(W)["path"].toString(),Root.filePath("3.png"),20000);
        QCOMPARE(Document(W)["documentId"].toString(),Id);QVERIFY(!Document(W)["dirty"].toBool());
        QCOMPARE(W.Snapshot()["documents"].toArray().size(),1);
        QCOMPARE(QImage(Root.filePath("2.png")).size(),QSize(120,80));
        QCOMPARE(QImage(Root.filePath("3.png")).size(),QSize(80,120));
    }
    void tabAndWindowCloseDuringImageRendering()
    {
        QTemporaryDir Root; Fixture(Root.path(),1);
        for(const bool CloseWindow:{false,true}){
            DocumentWorkspaceWidget W(Root.filePath("recovery"),true);W.resize(800,600);W.show();
            W.Handle({{"action","open"},{"path",Root.filePath("1.png")}});
            QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"]=="ready",20000);
            auto* Image=W.findChild<QImageView*>();QVERIFY(Image);
            Image->SetDisplayFilter("spline64");QVERIFY(Image->IsRendering());
            if(CloseWindow){W.RequestViewerClose();QTRY_VERIFY_WITH_TIMEOUT(W.Snapshot()["closeRequested"].toBool(),20000);}
            else{auto* Tabs=W.findChild<QTabWidget*>("documentTabs");QVERIFY(Tabs);emit Tabs->tabCloseRequested(0);}
            QCOMPARE(W.Snapshot()["documents"].toArray().size(),0);
            QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        }
    }
    void tabCloseDuringPendingEditWaitsForSaveChoice()
    {
        QTemporaryDir Root; Fixture(Root.path(),1);
        DocumentWorkspaceWidget W(Root.filePath("recovery"));W.resize(800,600);W.show();
        W.Handle({{"action","open"},{"path",Root.filePath("1.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"]=="ready",20000);
        auto* Image=W.findChild<QImageView*>();QVERIFY(Image);
        QVERIFY(Image->ApplyResize(QSize(200,300),"spline16"));QVERIFY(Image->IsEditing());
        int Prompts=0;W.SetClosePrompt([&](const QString&){++Prompts;return int(QMessageBox::Cancel);});
        auto* Tabs=W.findChild<QTabWidget*>("documentTabs");QVERIFY(Tabs);emit Tabs->tabCloseRequested(0);
        QCOMPARE(W.Snapshot()["documents"].toArray().size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(Prompts,1,20000);
        QVERIFY(Document(W)["dirty"].toBool());
        QTest::qWait(50);QCOMPARE(Prompts,1);
        W.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});emit Tabs->tabCloseRequested(0);
        QCOMPARE(W.Snapshot()["documents"].toArray().size(),0);
    }
    void tabCloseDuringSaveCompletesTheWrite()
    {
        QTemporaryDir Root; Fixture(Root.path(),1);
        DocumentWorkspaceWidget W(Root.filePath("recovery"));W.resize(800,600);W.show();
        W.Handle({{"action","open"},{"path",Root.filePath("1.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"]=="ready",20000);
        const auto Id=Document(W)["documentId"].toString();
        W.Handle({{"action","image.rotate"},{"angle",90},{"documentId",Id}});
        W.Handle({{"action","save"},{"documentId",Id}});QVERIFY(Document(W)["saving"].toBool());
        auto* Tabs=W.findChild<QTabWidget*>("documentTabs");QVERIFY(Tabs);emit Tabs->tabCloseRequested(0);
        QCOMPARE(W.Snapshot()["documents"].toArray().size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(W.Snapshot()["documents"].toArray().size(),0,20000);
        QCOMPARE(QImage(Root.filePath("1.png")).size(),QSize(120,80));
    }
    void readingBackspaceMovesToPreviousPage()
    {
        QTemporaryDir Root; Fixture(Root.path(), 3);
        const auto Book = OpenImageBook(Root.path()); QVERIFY(Book.HasValue());
        ImageReadingView View; View.resize(400, 500); View.Load(Book.Value(), Picture());
        View.SetMode(ImageReadingView::Single); View.show(); View.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&View)); View.setFocus(); View.GoTo(2);
        QTest::keyClick(&View, Qt::Key_Backspace);
        QCOMPARE(View.State()["page"].toInt(), 1);
        QTest::keyClick(&View, Qt::Key_Space);
        QCOMPARE(View.State()["page"].toInt(), 2);
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
    void fullscreenKeepsWorkspaceChromeHidden_data()
    {
        QTest::addColumn<bool>("Hosted");
        QTest::addColumn<bool>("ShowChrome");
        QTest::newRow("viewer-show") << true << true;
        QTest::newRow("viewer-hide") << true << false;
        QTest::newRow("editor-show") << false << true;
        QTest::newRow("editor-hide") << false << false;
    }
    void fullscreenKeepsWorkspaceChromeHidden()
    {
        QFETCH(bool, Hosted);
        QFETCH(bool, ShowChrome);
        const auto Private = qApp->property("privateReading");
        qApp->setProperty("privateReading", true);
        const auto Restore = qScopeGuard([&] { qApp->setProperty("privateReading", Private); });
        QTemporaryDir Root;
        Fixture(Root.path(), 2);
        DocumentWorkspaceWidget W(Root.filePath("recovery"), Hosted);
        W.resize(1000, 700); W.show(); W.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&W));
        W.Handle({{"action", "open"}, {"path", Root.filePath("1.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(Document(W)["lifecycle"] == "ready", 20000);
        auto Settings = W.ChromeSettings();
        Settings["mainMenuMode"] = ShowChrome ? "show" : "hide";
        Settings["mainToolbarMode"] = ShowChrome ? "show" : "hide";
        Settings["viewOverrides"] = QJsonObject{};
        QVERIFY(W.ApplyChromeSettings(Settings));
        auto* Menu = W.findChild<QMenuBar*>("mainMenu");
        auto* Toolbar = W.findChild<QToolBar*>("mainToolbar");
        auto* Fullscreen = W.findChild<ImageFullscreen*>("imageFullscreen");
        QVERIFY(Menu && Toolbar && Fullscreen);
        QTest::keyClick(&W, Qt::Key_F11);
        QTRY_VERIFY(Fullscreen->Active());
        QTRY_VERIFY(W.isFullScreen());
        QVERIFY(Menu->isHidden());
        QVERIFY(Toolbar->isHidden());
        // Reapplying appearance must not expose normal controls over the overlay.
        QVERIFY(W.ApplyChromeSettings(Settings));
        QVERIFY(Menu->isHidden());
        QVERIFY(Toolbar->isHidden());
        if (auto* Chrome = W.findChild<QWidget*>("documentWindowChrome"))
            QVERIFY(Chrome->isHidden());
        QVERIFY(W.findChild<QWidget*>("documentStatusRow")->isHidden());
        const auto Artifacts = qEnvironmentVariable("HC_IMAGE_TEST_ARTIFACTS");
        if (!Artifacts.isEmpty())
            QVERIFY(W.grab().save(Artifacts + "/fullscreen-" + QTest::currentDataTag() + ".png"));
        QTest::keyClick(&W, Qt::Key_F11);
        QTRY_VERIFY(!Fullscreen->Active());
        QCOMPARE(Menu->isVisible(), ShowChrome);
        QCOMPARE(Toolbar->isVisible(), ShowChrome);
        QVERIFY(W.findChild<QWidget*>("documentStatusRow")->isVisible());
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
    void imageJobsAreReclaimedByTheHostedEventPump()
    {
        QImageView View;View.resize(800,600);View.show();
        const auto Source=Picture(Qt::blue,QSize(1200,800));View.LoadContent(Source,"pump.png",false);
        for(int I=0;I<8;++I)View.setZoomLevel(50+I*5);
        QElapsedTimer Deadline;Deadline.start();
        while(Deadline.elapsed()<10000&&(View.IsRendering()||!View.findChildren<QFutureWatcherBase*>().isEmpty()))
            QCoreApplication::processEvents(QEventLoop::AllEvents,5);
        QVERIFY(!View.IsRendering());QVERIFY(View.findChildren<QFutureWatcherBase*>().isEmpty());QCOMPARE(View.Content(),Source);
    }
    void imagePoolHasViewLifetimeInsteadOfDllStaticLifetime()
    {
        std::weak_ptr<QThreadPool> Weak;
        {
            QImageView First,Second;First.resize(800,600);First.show();
            {const auto Pool=ImageWorkerPool();Weak=Pool;QVERIFY(Pool->maxThreadCount()<=4);}
            First.LoadContent(Picture(Qt::blue,QSize(1200,800)),"pool.png",false);
            QTRY_VERIFY_WITH_TIMEOUT(!First.IsRendering(),10000);
            QVERIFY(!Weak.expired());
        }
        QVERIFY(Weak.expired());
    }
    void autoFilterUsesUniformScalingDespitePixelRounding()
    {
        const auto Rounded=QTransform::fromScale(1000./1920,667./1280);
        QCOMPARE(ImageFilterRegistry::Resolve("auto",Rounded,QSize(1920,1280)).id_,QString("lanczos3"));
        QCOMPARE(ImageFilterRegistry::Resolve("auto",QTransform::fromScale(.4,.7),QSize(1920,1280)).id_,QString("ewaRobidoux"));
        QTransform Rotated;Rotated.rotate(15);Rotated.scale(.5,.5);
        QCOMPARE(ImageFilterRegistry::Resolve("auto",Rotated,QSize(1920,1280)).id_,QString("ewaRobidoux"));
        QCOMPARE(ImageFilterRegistry::Resolve("ewaRobidouxSharp",Rounded,QSize(1920,1280)).id_,QString("ewaRobidouxSharp"));
    }
    void navigationInputWaitsForAsynchronousCatalog()
    {
        QTemporaryDir Root;Fixture(Root.path(),3);QImageView View;View.resize(800,600);View.LoadContent(Picture(),Root.filePath("2.png"),false);View.show();
        QSignalSpy Open(&View,&QBaseView::OpenRequested);View.SetNavigationSource(Root.filePath("2.png"));
        View.nextImage();QTRY_COMPARE_WITH_TIMEOUT(Open.size(),1,15000);QCOMPARE(Open.first().at(0).toString(),Root.filePath("3.png"));
    }
    void decodedCacheRejectsChangedAndReplacedFiles()
    {
        QTemporaryDir Root;const auto Path=Root.filePath("cache.bmp");
        ImageDecodeCache::Clear();QVERIFY(Picture(Qt::red).save(Path));
        const auto A=ReadDocumentFile(Path);QVERIFY(A.HasValue());QVERIFY(ImageDecodeCache::Find(Path).has_value());
        ImageDecodeCache::Remember(Path,{A.Value().bytes_,A.Value().hash_,{},A.Value().image_});
        QVERIFY(!ImageDecodeCache::Find(Path).has_value());
        QCOMPARE(ReadDocumentFile(Path).Value().hash_,A.Value().hash_);QVERIFY(ImageDecodeCache::Find(Path).has_value());
        QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));const auto Time=File.fileTime(QFileDevice::FileModificationTime);const auto Size=File.size();File.close();
        QVERIFY(Picture(Qt::blue).save(Path));QVERIFY(File.open(QIODevice::ReadWrite));QCOMPARE(File.size(),Size);
        QVERIFY(File.setFileTime(Time,QFileDevice::FileModificationTime));File.close();
        QVERIFY(!ImageDecodeCache::Find(Path).has_value());
        const auto B=ReadDocumentFile(Path);QVERIFY(B.HasValue());QCOMPARE(B.Value().image_.pixelColor(1,1),QColor(Qt::blue));QVERIFY(A.Value().hash_!=B.Value().hash_);
        QVERIFY(QFile::remove(Path));QVERIFY(Picture(Qt::green).save(Path));
        QVERIFY(!ImageDecodeCache::Find(Path).has_value());
        QCOMPARE(ReadDocumentFile(Path).Value().image_.pixelColor(1,1),QColor(Qt::green));
        QVERIFY(ImageDecodeCache::CostKiB()<=256*1024);ImageDecodeCache::Clear();
    }
    void redirectedMetadataRevisionRetainsSizeAndModificationChecks()
    {
        BY_HANDLE_FILE_INFORMATION First{};First.dwFileAttributes=FILE_ATTRIBUTE_NORMAL;First.dwVolumeSerialNumber=7;
        First.nFileIndexLow=1;First.nFileSizeLow=100;First.ftLastWriteTime.dwLowDateTime=12345;
        auto Second=First;Second.nFileIndexLow=2;
        const auto Strong=hc::storage::native::HandleRevision(First);
        QVERIFY(Strong!=hc::storage::native::HandleRevision(Second));
        const auto Metadata=hc::storage::native::MetadataRevision(Strong);
        QVERIFY(hc::storage::native::MatchesRevision(Metadata,Second));
        ++Second.nFileSizeLow;QVERIFY(!hc::storage::native::MatchesRevision(Metadata,Second));
        Second=First;++Second.ftLastWriteTime.dwLowDateTime;QVERIFY(!hc::storage::native::MatchesRevision(Metadata,Second));
    }
    void asynchronousPreviewPreservesOriginalAndPaints()
    {
        QImageView View;View.resize(960,720);View.show();
        QImage Source(2400,1600,QImage::Format_RGBA8888);Source.fill(QColor(30,100,170,127));Source.setColorSpace(QColorSpace::DisplayP3);
        QSignalSpy Paints(&View,&QImageView::imagePresented);int Ticks=0;QTimer Timer;connect(&Timer,&QTimer::timeout,this,[&]{++Ticks;});Timer.start(1);
        View.LoadContent(Source,"test-large.png",false);View.SetDisplayFilter("spline36");
        QTRY_VERIFY_WITH_TIMEOUT(!Paints.isEmpty(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsRendering(),15000);
        QVERIFY(Ticks>0);QCOMPARE(View.Content(),Source);QVERIFY(View.RenderError().isEmpty());
        View.setZoomLevel(160);View.setZoomLevel(50);View.SetDisplayFilter("lanczos3");
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsRendering(),15000);QCOMPARE(View.Content(),Source);
    }
    void cmykAndGrayImagesUseRgbDisplayWithoutChangingOriginal()
    {
        const auto Cmyk=Picture(QColor(35,100,180),QSize(240,160)).convertToFormat(QImage::Format_CMYK8888);
        const auto Rgb=Cmyk.convertToFormat(QImage::Format_RGBA8888);
        for(const auto& Filter:QStringList{"lanczos3","ewaRobidouxSharp"}){
            ImageRenderRequest Request{Cmyk,QSize(120,80),QTransform::fromScale(.5,.5),Filter};
            const auto Actual=ResampleImage(Request);Request.source_=Rgb;const auto Expected=ResampleImage(Request);
            QVERIFY2(Actual.error_.isEmpty(),qPrintable(Actual.error_));QCOMPARE(Actual.image_,Expected.image_);
        }
        QImage Gray(240,160,QImage::Format_Grayscale16);Gray.fill(QColor(100,100,100));
        Gray.setColorSpace(QColorSpace(QPointF(.3127,.3290),QColorSpace::TransferFunction::Gamma,2.2f));
        QVERIFY(Gray.colorSpace().isValid());QCOMPARE(Gray.colorSpace().colorModel(),QColorSpace::ColorModel::Gray);
        ImageRenderRequest Request{Gray,QSize(120,80),QTransform::fromScale(.5,.5),"spline36"};Request.targetColor_=QColorSpace::SRgb;
        const auto Rendered=ResampleImage(Request);QVERIFY2(Rendered.error_.isEmpty(),qPrintable(Rendered.error_));
        const auto Pixel=Rendered.image_.pixelColor(10,10);QVERIFY(std::abs(Pixel.red()-Pixel.green())<=1);QVERIFY(std::abs(Pixel.red()-Pixel.blue())<=1);
        const auto ExpectedGray=Gray.convertedToColorSpace(QColorSpace::SRgb,QImage::Format_RGBA8888).pixelColor(10,10);
        QVERIFY(std::abs(Pixel.red()-ExpectedGray.red())<=1);
        QImageView View;View.resize(800,600);View.show();QSignalSpy Paints(&View,&QImageView::imagePresented);
        View.LoadContent(Cmyk,"cmyk.jpg",false);View.SetDisplayFilter("spline36");
        QTRY_VERIFY_WITH_TIMEOUT(!View.IsRendering(),10000);QVERIFY2(View.RenderError().isEmpty(),qPrintable(View.RenderError()));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(Paints.begin(),Paints.end(),[](const auto& Row){return Row[0].toBool();}),10000);
        QCOMPARE(View.Content(),Cmyk);
    }
    void hiddenComicCanBePresentedByHost()
    {
        QTemporaryDir Root; Fixture(Root.path(),2);
        auto Book=OpenImageBook(Root.path()); QVERIFY(Book.HasValue());
        ImageReadingView View; View.SetPrivate(true); View.resize(1000,700);
        View.Load(Book.Value()); View.SetMode(ImageReadingView::Comic);
        QVERIFY(!View.isVisible()); QVERIFY(!View.IsBusy());
        QVERIFY(View.State()["transitioning"].toBool());
        View.show();
        QTRY_COMPARE_WITH_TIMEOUT(View.State()["presentedPage"].toInt(-1),0,20000);
        QTRY_VERIFY(!View.IsBusy());
    }
    void comicLandscapeAndGroupNavigation()
    {
        QTemporaryDir Root;
        const QList<QSize> Sizes{{80,120},{1470,1095},{1430,1095},{80,120},{120,200},{180,180}};
        for(int I=0;I<Sizes.size();++I) QVERIFY(Picture(I%2?Qt::red:Qt::blue,Sizes[I]).save(Root.filePath(QString::number(I)+".png")));
        auto Book=OpenImageBook(Root.path()); QVERIFY(Book.HasValue());
        ImageReadingView View; View.SetPrivate(true); View.resize(1000,700);
        View.Load(Book.Value()); View.SetMode(ImageReadingView::Comic); View.SetCover(false); View.show();
        QTRY_COMPARE_WITH_TIMEOUT(View.State()["presentedPage"].toInt(-1),0,20000);
        QCOMPARE(View.State()["presentedLastPage"].toInt(),0);
        for(int Page : {1,2,3}) {
            View.Move(1);
            QTRY_COMPARE_WITH_TIMEOUT(View.State()["presentedPage"].toInt(-1),Page,20000);
            QCOMPARE(View.State()["presentedLastPage"].toInt(),Page==3?4:Page);
        }
        View.Move(1); QTRY_COMPARE(View.State()["presentedPage"].toInt(),5);
        View.Move(-1); QTRY_COMPARE(View.State()["presentedPage"].toInt(),3);
        View.SetRightToLeft(true);
        QTRY_VERIFY(!View.IsBusy());
        const auto Frame=View.viewport()->grab().toImage();
        QVERIFY(Frame.pixelColor(Frame.width()/4,Frame.height()/2).blue()>200);
        QVERIFY(Frame.pixelColor(Frame.width()*3/4,Frame.height()/2).red()>200);
        View.resize(500,800); QTRY_VERIFY(!View.IsBusy());
        QCOMPARE(View.State()["presentedLastPage"].toInt(),3);
        View.GoTo(1); QTRY_COMPARE(View.State()["presentedPage"].toInt(),1);
        View.SetSplit(true); QTRY_VERIFY(!View.IsBusy());
        View.Move(1); QCOMPARE(View.State()["page"].toInt(),1); QCOMPARE(View.State()["part"].toInt(),1);
        View.Move(1); QTRY_COMPARE(View.State()["presentedPage"].toInt(),2);
    }
    void comicTransitionKeepsCompleteGroupAndReusesRenders()
    {
        QTemporaryDir Root;
        for(int I=0;I<4;++I) QVERIFY(Picture(I<2?Qt::red:Qt::blue,{400,600}).save(Root.filePath(QString::number(I)+".png")));
        auto Book=OpenImageBook(Root.path()); QVERIFY(Book.HasValue());
        ImageReadingView View; View.SetPrivate(true); View.resize(1200,800);
        View.Load(Book.Value()); View.SetMode(ImageReadingView::Comic); View.SetCover(false); View.show();
        QTRY_COMPARE_WITH_TIMEOUT(View.State()["presentedLastPage"].toInt(-1),1,20000);
        View.GoTo(2);
        QElapsedTimer Clock; Clock.start();
        do {
            const auto Frame=View.viewport()->grab().toImage();
            const auto Left=Frame.pixelColor(Frame.width()/4,Frame.height()/2);
            const auto Right=Frame.pixelColor(Frame.width()*3/4,Frame.height()/2);
            QVERIFY((Left.red()>200 && Right.red()>200) || (Left.blue()>200 && Right.blue()>200));
            QTest::qWait(2);
        } while(View.State()["presentedPage"].toInt()!=2 && Clock.elapsed()<20000);
        QCOMPARE(View.State()["presentedPage"].toInt(),2);
        QTRY_COMPARE(View.State()["renderJobs"].toInt(),0);
        QTRY_COMPARE(View.State()["sourceJobs"].toInt(),0);
        const auto Renders=View.State()["renderRequests"].toDouble();
        const auto Reads=View.State()["sourceReads"].toDouble();
        View.GoTo(0); QTRY_COMPARE(View.State()["presentedPage"].toInt(),0);
        View.GoTo(2); QTRY_COMPARE(View.State()["presentedPage"].toInt(),2);
        QCOMPARE(View.State()["renderRequests"].toDouble(),Renders);
        QCOMPARE(View.State()["sourceReads"].toDouble(),Reads);
        View.SetAdjustment("invert",1); QTRY_VERIFY(!View.IsBusy());
        const auto Inverted=View.viewport()->grab().toImage();
        const auto Pixel=Inverted.pixelColor(Inverted.width()/4,Inverted.height()/2);
        QVERIFY(Pixel.red()>200 && Pixel.green()>200 && Pixel.blue()<30);
        QVERIFY(View.State()["renderRequests"].toDouble()>Renders);
        View.GoTo(0); View.GoTo(2); View.GoTo(0); View.resize(1100,750);
        QTRY_COMPARE(View.State()["presentedPage"].toInt(),0); QTRY_VERIFY(!View.IsBusy());
        View.Cancel(); QTRY_VERIFY(!View.IsBusy());
    }
    void comicHeaderUsesTransformedDimensions()
    {
        QTemporaryDir Root;
        QByteArray Bytes; QBuffer Buffer(&Bytes); Buffer.open(QIODevice::WriteOnly);
        QVERIFY(Picture(Qt::red,{80,120}).save(&Buffer,"JPEG")); Buffer.close();
        Bytes.insert(2,QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        QFile File(Root.filePath("rotated.jpg")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write(Bytes); File.close();
        auto Book=OpenImageBook(Root.path()); QVERIFY(Book.HasValue());
        QSize Metadata; auto Page=ReadImagePage(Book.Value(),0,{}, {},[&](QSize Size){Metadata=Size;});
        QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().image_.size(),QSize(120,80)); QCOMPARE(Metadata,QSize(120,80));
    }
    void optionalComicTransitionBenchmark()
    {
        const auto Path = qEnvironmentVariable("HC_COMIC_BENCH_PATH");
        const auto Output = qEnvironmentVariable("HC_COMIC_BENCH_OUTPUT");
        if (Path.isEmpty() || Output.isEmpty()) QSKIP("Set HC_COMIC_BENCH_PATH and HC_COMIC_BENCH_OUTPUT");
        qApp->setProperty("privateReading", true);
        const auto Restore = qScopeGuard([] { qApp->setProperty("privateReading", false); });
        ImageReadingView View;
        View.SetPrivate(true);
        View.resize(qEnvironmentVariableIntValue("HC_COMIC_BENCH_4K") ? QSize(3840, 2160) : QSize(1920, 1080));
        View.show();
        QElapsedTimer Open; Open.start();
        View.OpenPath(Path, {}, ImageReadingView::Comic);
        QTRY_VERIFY_WITH_TIMEOUT(View.State()["pageCount"].toInt() >= 40, 30000);
        QJsonArray Rows;
        const auto Wait = [&](int Page) {
            QElapsedTimer Clock; Clock.start(); int Blank = 0, Samples = 0, Stable = 0;
            while (Clock.elapsed() < 30000 && Stable < 3) {
                QTest::qWait(5);
                const auto Frame = View.viewport()->grab().toImage();
                int Bright = 0;
                for (int Y = Frame.height()/10; Y < Frame.height()*9/10; Y += 47)
                    for (int X = Frame.width()/10; X < Frame.width()*9/10; X += 47)
                        if (qGray(Frame.pixel(X,Y)) > 120) ++Bright;
                if (Bright < 10) ++Blank;
                ++Samples;
                const auto S = View.State();
                Stable = !View.IsBusy() && !View.CurrentImage().isNull()
                    && (!S.contains("presentedPage") || S["presentedPage"].toInt() == Page) ? Stable + 1 : 0;
            }
            return QJsonObject{{"page",Page},{"ms",Clock.nsecsElapsed()/1e6},
                {"blankSamples",Blank},{"samples",Samples},{"ready",Stable>=3},{"state",View.State()}};
        };
        Rows.append(Wait(0));
        const auto OpenMs = Open.nsecsElapsed()/1e6;
        for (int Page = 1; Page <= 20; ++Page) { View.GoTo(Page); Rows.append(Wait(Page)); }
        for (int Page = 19; Page >= 0; --Page) { View.GoTo(Page); Rows.append(Wait(Page)); }
        for (int I = 0; I < 8; ++I) { const int Page=13+I%2; View.GoTo(Page); Rows.append(Wait(Page)); }
        QFile Report(Output); QVERIFY(Report.open(QIODevice::WriteOnly));
        Report.write(QJsonDocument(QJsonObject{{"path",Path},{"openMs",OpenMs},{"rows",Rows},
            {"dpr",View.devicePixelRatioF()},{"boundary","widget paint; 5ms polling and capture included; OS cache not flushed"}}).toJson());
        for (const auto& Row : Rows) QVERIFY(Row.toObject()["ready"].toBool());
        QVERIFY(View.grab().save(Output + ".png"));
    }
    void optionalWorkspacePresentationBenchmark()
    {
        const auto Root=qEnvironmentVariable("HC_IMAGE_BENCH_ROOT");if(Root.isEmpty())QSKIP("Set HC_IMAGE_BENCH_ROOT");
        QJsonArray Rows;
        for(const auto& Name:QStringList{"decode.jpg","decode.png","decode.webp","gray16.png","example.heic","example.avif","progressive.jpg","cmyk.jpg"}){
            const auto Path=Root+"/corpus/"+Name;if(!QFileInfo::exists(Path))continue;
            for(int Run=0;Run<3;++Run){
                ImageDecodeCache::Clear();QTemporaryDir Recovery;DocumentWorkspaceWidget Workspace(Recovery.path());Workspace.resize(1200,800);Workspace.show();
                QElapsedTimer Clock;Clock.start();double Presented=-1;double MaxGap=0,LastTick=0;
                QTimer Timer;connect(&Timer,&QTimer::timeout,this,[&]{const auto Now=Clock.nsecsElapsed()/1e6;MaxGap=std::max(MaxGap,Now-LastTick);LastTick=Now;});Timer.start(1);
                const auto Opened=Workspace.Handle({{"action","open"},{"path",Path}});QVERIFY(!Opened.contains("error"));
                const auto State=[&]{return Workspace.Snapshot()["documents"].toArray().first().toObject();};
                QTRY_VERIFY_WITH_TIMEOUT(State()["imagePresentationReady"].toBool() || !State()["error"].toString().isEmpty(),30000);
                QVERIFY2(State()["error"].toString().isEmpty(),qPrintable(State()["error"].toString()));Presented=Clock.nsecsElapsed()/1e6;
                const bool BusyAtPresentation=!Workspace.IsIdle();
                QTRY_VERIFY_WITH_TIMEOUT(Workspace.IsIdle(),30000);
                QVERIFY2(State()["view"].toObject()["renderError"].toString().isEmpty(),qPrintable(State()["view"].toObject()["renderError"].toString()));
                Rows.append(QJsonObject{{"file",Name},{"run",Run},{"presentation_ms",Presented},{"idle_ms",Clock.nsecsElapsed()/1e6},
                    {"busy_at_presentation",BusyAtPresentation},{"max_heartbeat_gap_ms",MaxGap}});
            }
        }
        QFile Output(Root+"/workspace-presentation.json");QVERIFY(Output.open(QIODevice::WriteOnly));Output.write(QJsonDocument(Rows).toJson());
    }
    void optionalStorageAndFirstPaintBenchmark()
    {
        const auto Root=qEnvironmentVariable("HC_IMAGE_BENCH_ROOT");if(Root.isEmpty())QSKIP("Set HC_IMAGE_BENCH_ROOT for isolated latency measurements");
        QJsonArray Rows;
        for(const auto& Name:QStringList{"decode.jpg","decode.png","decode.webp","gray16.png","example.heic","example.avif",
            "webtoon.png","natural.png","48mp.png","rgb16.tiff","large-avif.avif","24mp-avif.avif","progressive.jpg","cmyk.jpg"}){
            const auto Path=Root+"/corpus/"+Name;if(!QFileInfo::exists(Path))continue;
            for(int Run=0;Run<5;++Run){ImageDecodeCache::Clear();QElapsedTimer Clock;Clock.start();
                QImageView View;View.resize(1200,800);View.show();double First=-1,Final=-1,Loaded=-1,LastTick=0,MaxGap=0;int Heartbeats=0;QString Error;
                connect(&View,&QImageView::imagePresented,this,[&](bool Complete){if(First<0)First=Clock.nsecsElapsed()/1e6;if(Complete&&Final<0)Final=Clock.nsecsElapsed()/1e6;});
                QTimer Timer;connect(&Timer,&QTimer::timeout,this,[&]{const double Now=Clock.nsecsElapsed()/1e6;MaxGap=std::max(MaxGap,Now-LastTick);LastTick=Now;++Heartbeats;});Timer.start(1);
                QFutureWatcher<hc::storage::Result<TextInput>> Watcher;
                connect(&Watcher,&QFutureWatcherBase::finished,this,[&]{auto Input=Watcher.result();Loaded=Clock.nsecsElapsed()/1e6;if(!Input.HasValue())Error=QString::fromStdString(Input.Failure().code_);else View.LoadContent(Input.Value().image_,Path,false);});
                Watcher.setFuture(QtConcurrent::run([Path]{return ReadDocumentFile(Path);}));
                QTRY_VERIFY_WITH_TIMEOUT(Final>=0||!Error.isEmpty()||!View.RenderError().isEmpty(),30000);QVERIFY2(Error.isEmpty(),qPrintable(Name+": "+Error));
                QVERIFY2(View.RenderError().isEmpty(),qPrintable(Name+": "+View.RenderError()));
                Rows.append(QJsonObject{{"file",Name},{"run",Run},{"read_decode_ms",Loaded},{"first_paint_ms",First},{"final_paint_ms",Final},{"heartbeats",Heartbeats},{"max_heartbeat_gap_ms",MaxGap},{"boundary","async-storage-plus-widget; OS cache not flushed"}});
                QFile Partial(Root+"/viewer-latency.json");QVERIFY(Partial.open(QIODevice::WriteOnly));Partial.write(QJsonDocument(Rows).toJson());
            }
        }
        QFile Report(Root+"/viewer-latency.json");QVERIFY(Report.open(QIODevice::WriteOnly));Report.write(QJsonDocument(Rows).toJson());
    }
    void optionalDecodedCacheBenchmark()
    {
        const auto Root=qEnvironmentVariable("HC_IMAGE_BENCH_ROOT");if(Root.isEmpty())QSKIP("Set HC_IMAGE_BENCH_ROOT for isolated cache measurements");
        QJsonArray Rows;
        for(const auto& Name:QStringList{"decode.jpg","example.heic","example.avif","24mp-avif.avif","48mp.png"}){
            const auto Path=Root+"/corpus/"+Name;if(!QFileInfo::exists(Path))continue;
            ImageDecodeCache::Clear();QElapsedTimer Clock;Clock.start();const auto Cold=ReadDocumentFile(Path);const double ColdMs=Clock.nsecsElapsed()/1e6;
            QVERIFY(Cold.HasValue());QVERIFY(!Cold.Value().image_.isNull());QJsonArray Warm;
            const bool Metadata=ImageDecodeCache::Find(Cold.Value().path_).has_value();
            for(int I=0;I<7;++I){Clock.restart();const auto Cached=ReadDocumentFile(Path);Warm.append(Clock.nsecsElapsed()/1e6);QVERIFY(Cached.HasValue());QCOMPARE(Cached.Value().hash_,Cold.Value().hash_);QCOMPARE(Cached.Value().image_,Cold.Value().image_);}
            Rows.append(QJsonObject{{"file",Name},{"uncached_ms",ColdMs},{"cached_samples_ms",Warm},{"metadata_reuse",Metadata},{"boundary","ReadDocumentFile component; OS cache not flushed"}});
        }
        QFile Report(Root+"/viewer-cache.json");QVERIFY(Report.open(QIODevice::WriteOnly));Report.write(QJsonDocument(Rows).toJson());ImageDecodeCache::Clear();
        QTemporaryDir Scratch(Root+"/cache-consistency-XXXXXX");QVERIFY(Scratch.isValid());const auto Path=Scratch.filePath("source.bmp");
        QVERIFY(Picture(Qt::red).save(Path));const auto Before=ReadDocumentFile(Path);QVERIFY(Before.HasValue());
        QFile File(Path);QVERIFY(File.open(QIODevice::ReadOnly));const auto Time=File.fileTime(QFileDevice::FileModificationTime);const auto Size=File.size();File.close();
        QVERIFY(Picture(Qt::blue).save(Path));QVERIFY(File.open(QIODevice::ReadWrite));QCOMPARE(File.size(),Size);QVERIFY(File.setFileTime(Time,QFileDevice::FileModificationTime));File.close();
        const auto After=ReadDocumentFile(Path);QVERIFY(After.HasValue());QCOMPARE(After.Value().image_.pixelColor(1,1),QColor(Qt::blue));
        hc::storage::DestinationExpectation Expected;Expected.exists_=true;
        const auto Saved=SaveTextFile(Path,Before.Value().bytes_,Expected,Before.Value().hash_,{},true);
        QVERIFY(!Saved.HasValue());QCOMPARE(QString::fromStdString(Saved.Failure().code_),QString("document-external-content-change"));ImageDecodeCache::Clear();
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
