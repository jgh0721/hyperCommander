#include "UniqueLibs/qtViewerSurface.hpp"
#include "UniqueLibs/qtViewerProvider.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include "memoryStorage.hpp"
#include <QtTest>
#include <QBuffer>
#include <QClipboard>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTimer>
#include <QAction>
#include <QDialog>
#include <QScopeGuard>
#include <QScreen>
#include <iostream>
#include <cstring>
#include <thread>

using namespace hc::storage;
using namespace hc::viewer;
namespace
{
    std::shared_ptr<IContentSource> Content(const QByteArray& Bytes, std::string Name)
    {
        auto Provider = std::make_shared<hc::storage::test::MemoryProvider>();
        std::vector<std::byte> Data(Bytes.size()); if (!Data.empty()) std::memcpy(Data.data(), Bytes.constData(), Data.size());
        const auto Id = Provider->Add(Provider->Root(), std::move(Name), std::move(Data));
        return std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
    }
    void Capture(QWidget& Widget, const QString& Name)
    {
        const auto Root = qEnvironmentVariable("HC_VIEWER_SCREENSHOTS"); if (Root.isEmpty()) return;
        QDir().mkpath(Root); const auto Pixmap = Widget.grab();
        qInfo() << Name << "logical size" << Widget.size() << "DPR" << Widget.devicePixelRatioF() << "pixels" << Pixmap.size();
        QVERIFY(Pixmap.save(Root + '/' + Name + ".png"));
    }
    class GatedSource final : public IContentSource
    {
    public:
        std::atomic<bool> entered_{false}, release_{false};
        EntrySnapshot Describe() const override { return inner_->Describe(); }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override { return inner_->OpenRead(Context); }
        Task<Unit> Verify(OperationContext Context) override
        {
            entered_ = true;
            const auto End = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!release_ && std::chrono::steady_clock::now() < End) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return inner_->Verify(Context);
        }
    private:
        std::shared_ptr<IContentSource> inner_ = Content("gated file", "pending.txt");
    };
}
class ViewerSurfaceTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        bool Specified = false; const auto Expected = qEnvironmentVariable("HC_VIEWER_EXPECT_DPR").toDouble(&Specified);
        const auto Actual = QApplication::primaryScreen()->devicePixelRatio(); qInfo() << "screen DPR" << Actual;
        if (Specified) QVERIFY2(std::abs(Actual - Expected) < 0.01, "Actual DPR must match the requested rendering scale");
    }
    void largeFontNarrowLayout()
    {
        const auto Original = QApplication::font(); auto Large = Original; Large.setPointSize(20); QApplication::setFont(Large);
        const auto Restore = qScopeGuard([Original] { QApplication::setFont(Original); });
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(240, 400); Surface.show();
        Surface.Open(Content(QString::fromUtf8("큰 글꼴 보기\n본문 확인").toUtf8(), "large-font.txt")); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000);
        QCOMPARE(Surface.width(), 240); QVERIFY(Surface.findChild<QPlainTextEdit*>()->font().pointSizeF() >= 20);
        Capture(Surface, "large-font-narrow");
    }
    void closeDrainsWithoutBlockingGui()
    {
        QWidget Main; auto* Host = new QtViewerHost(&Main); auto* Surface = Host->CreateSurface(&Main); Main.show(); Surface->show();
        auto Source = std::make_shared<GatedSource>(); Surface->Open(Source); QTRY_VERIFY_WITH_TIMEOUT(Source->entered_.load(), 3000);
        QTimer::singleShot(30, this, [Source] { Source->release_ = true; });
        QElapsedTimer Elapsed; Elapsed.start(); Main.close(); QVERIFY(Elapsed.elapsed() < 100); QVERIFY(Main.isVisible());
        QVERIFY(Main.property("hc.viewer.closePending").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!Main.isVisible() && Host->IsIdle(), 3000); QVERIFY(!Surface->IsReady());
        QVERIFY(!Main.property("hc.viewer.closePending").toBool());
    }
    void fullAndEmbeddedHost()
    {
        QWidget Main; Main.resize(800, 600); auto* Host = new QtViewerHost(&Main); Main.show();
        QPointer<QDialog> Full = Host->OpenFull(Content("full window", "full.txt"));
        auto* FullSurface = Full->findChild<QtViewerSurface*>(); QVERIFY(FullSurface);
        auto* Embedded = Host->CreateSurface(&Main); Embedded->resize(600, 400); Embedded->show();
        Embedded->Open(Content("quick preview", "quick.txt"), ViewerPurpose::QuickPreview);
        QTRY_VERIFY_WITH_TIMEOUT(FullSurface->IsReady() && Embedded->IsReady(), 3000);
        QCOMPARE(FullSurface->findChild<QPlainTextEdit*>()->toPlainText(), QString("full window"));
        QCOMPARE(Embedded->findChild<QPlainTextEdit*>()->toPlainText(), QString("quick preview"));
        QVERIFY(Full->windowTitle().startsWith("full.txt")); QTest::keyClick(Full.data(), Qt::Key_Escape); QTRY_VERIFY(Full.isNull());
        QVERIFY(Embedded->IsReady()); Main.close(); QTRY_VERIFY_WITH_TIMEOUT(!Main.isVisible() && Host->IsIdle(), 3000);
        QVERIFY(!Embedded->IsReady());
    }
    void textAndKeyboard()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(780, 510); Surface.show();
        QSignalSpy Ready(&Surface, &QtViewerSurface::Ready);
        Surface.Open(Content(QString::fromUtf8("한글 텍스트\n첫 번째 줄\n찾을 문장\n읽기 전용 파일입니다. 🙂\n").toUtf8(), "sample.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000); QCOMPARE(Ready.count(), 1);
        auto* Text = Surface.findChild<QPlainTextEdit*>("hc.viewer.text"); QVERIFY(Text); QVERIFY(Text->isReadOnly());
        const auto Original = Text->toPlainText(); Text->setFocus(); QTest::keyClicks(Text, "mutate"); QCOMPARE(Text->toPlainText(), Original);
        QTest::keyClick(Text, Qt::Key_F, Qt::ControlModifier);
        auto* Search = Surface.findChild<QLineEdit*>("hc.viewer.search"); QTRY_VERIFY(Search->isVisible()); Search->setText(QString::fromUtf8("찾을 문장"));
        QTest::keyClick(Search, Qt::Key_Return); QCOMPARE(Text->textCursor().selectedText(), QString::fromUtf8("찾을 문장"));
        QTest::keyClick(Search, Qt::Key_Escape); QVERIFY(!Search->isVisible());
        Surface.Execute("wrap"); QCOMPARE(Text->lineWrapMode(), QPlainTextEdit::WidgetWidth);
        Surface.Execute("zoom-in"); Capture(Surface, "text"); Surface.Close(); QVERIFY(!Surface.IsReady());
        QCOMPARE(Surface.Snapshot().status_, ViewerStatus::Closed); QCOMPARE(Service.RetainedBytes(), 0ULL);
    }
    void markdownResourceAndLinkPolicy()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(780, 510); Surface.show();
        QSignalSpy Links(&Surface, &QtViewerSurface::LinkRequested);
        Surface.Open(Content("# Markdown\n\n**Read only** preview\n\n[web](https://example.invalid)\n\n![blocked](file:///C:/Windows/win.ini)", "sample.md"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000); auto* Browser = Surface.findChild<QTextBrowser*>("hc.viewer.markdown"); QVERIFY(Browser->isVisible());
        QVERIFY(Browser->toPlainText().contains("Read only")); QVERIFY(!Browser->openLinks()); QVERIFY(!Browser->openExternalLinks());
        for (const auto& Address : {"file:///C:/Windows/win.ini", "https://example.invalid/image.png", "data:image/png;base64,iVBORw0KGgo="})
        {
            const auto Resource = Browser->document()->resource(QTextDocument::ImageResource, QUrl(Address));
            QVERIFY2(Resource.canConvert<QByteArray>() && Resource.toByteArray().isEmpty(), Address);
        }
        for (const auto& Address : {"file:///C:/Windows/win.ini", "javascript:alert(1)", "mailto:a@example.invalid", "//example.invalid/path"})
            QVERIFY(QMetaObject::invokeMethod(Browser, "anchorClicked", Q_ARG(QUrl, QUrl(Address))));
        QCOMPARE(Links.count(), 0);
        QVERIFY(QMetaObject::invokeMethod(Browser, "anchorClicked", Q_ARG(QUrl, QUrl("https://example.invalid/path")))); QCOMPARE(Links.count(), 1);
        Capture(Surface, "markdown"); Surface.Execute("source"); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000);
        auto* Text = Surface.findChild<QPlainTextEdit*>("hc.viewer.text"); QVERIFY(Text->isVisible()); QVERIFY(Text->toPlainText().startsWith("# Markdown"));
        Surface.Execute("source"); QVERIFY(Surface.IsReady()); QVERIFY(Browser->isVisible());
    }
    void imageFitResizeAndZoom_data()
    {
        QTest::addColumn<bool>("extreme"); QTest::newRow("ordinary") << false; QTest::newRow("very-wide") << true;
    }
    void imageFitResizeAndZoom()
    {
        QFETCH(bool, extreme);
        QImage Image(extreme ? 40000 : 600, extreme ? 1 : 400, QImage::Format_RGB32); Image.fill(QColor(38, 124, 160));
        QByteArray Png; QBuffer Buffer(&Png); QVERIFY(Buffer.open(QIODevice::WriteOnly)); QVERIFY(Image.save(&Buffer, "PNG"));
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(780, 510); Surface.show();
        Surface.Open(Content(Png, "image.png")); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000);
        auto* Area = Surface.findChild<QScrollArea*>("hc.viewer.image"); QVERIFY(Area->isVisible());
        QTRY_VERIFY(Area->widget()->width() <= Area->viewport()->width()); QTRY_VERIFY(Area->widget()->height() <= Area->viewport()->height());
        Surface.resize(350, 270); QTRY_VERIFY(Area->widget()->width() <= Area->viewport()->width()); QTRY_VERIFY(Area->widget()->height() <= Area->viewport()->height());
        Surface.Execute("original-size"); QCOMPARE(Area->widget()->size(), Image.size());
        Surface.Execute("zoom-in"); QCOMPARE(Area->widget()->size(), QSize(extreme ? 50000 : 750, extreme ? 1 : 500)); Surface.Execute("fit");
        QTRY_VERIFY(Area->widget()->width() <= Area->viewport()->width()); if (!extreme) Capture(Surface, "image-narrow");
        Surface.Close(); QCOMPARE(Service.RetainedBytes(), 0ULL);
    }
    void incrementalPresentationAndSwitch_data()
    {
        QTest::addColumn<bool>("singleLine"); QTest::newRow("many-lines") << false; QTest::newRow("single-long-line") << true;
    }
    void incrementalPresentationAndSwitch()
    {
        QFETCH(bool, singleLine);
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(700, 400); Surface.show();
        QByteArray Large; if (singleLine) Large = QByteArray(2 * 1024 * 1024, 'x') + "unique-tail";
        else for (int Index = 0; Index < 50000; ++Index) Large += "A long text line for asynchronous read-only presentation.\n";
        QSignalSpy Ready(&Surface, &QtViewerSurface::Ready); int Heartbeats = 0; QTimer Beat; Beat.setInterval(1);
        QElapsedTimer Clock; Clock.start(); qint64 Last = 0, MaxGap = 0;
        connect(&Beat, &QTimer::timeout, this, [&] { ++Heartbeats; const auto Now = Clock.elapsed(); MaxGap = std::max(MaxGap, Now - Last); Last = Now; }); Beat.start();
        Surface.Open(Content(Large, "large.txt")); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 10000);
        MaxGap = std::max(MaxGap, Clock.elapsed() - Last); QTest::qWait(30); qInfo() << "presentation ms" << Clock.elapsed() << "largest GUI timer gap ms" << MaxGap;
        QVERIFY(MaxGap < 400);
        QVERIFY(Heartbeats > 2); auto* Text = Surface.findChild<QPlainTextEdit*>("hc.viewer.text");
        if (singleLine)
        {
            QVERIFY(Text->toPlainText().size() <= 32768);
            QVERIFY(Surface.findChild<QAction*>("hc.viewer.next-part")->isVisible());
            Surface.Execute("next-part"); QVERIFY(Surface.findChild<QAction*>("hc.viewer.previous-part")->isEnabled());
            Surface.Execute("find"); auto* Search = Surface.findChild<QLineEdit*>("hc.viewer.search"); Search->setText("unique-tail");
            QTest::keyClick(Search, Qt::Key_Return); QCOMPARE(Text->textCursor().selectedText(), QString("unique-tail"));
            Text->selectAll(); Surface.Execute("copy"); QCOMPARE(QApplication::clipboard()->text().toUtf8(), Large);
            QApplication::clipboard()->clear();
            Surface.Execute("wrap"); Surface.Execute("zoom-in"); QTest::qWait(30);
            MaxGap = std::max(MaxGap, Clock.elapsed() - Last); QVERIFY(MaxGap < 400);
            Surface.Execute("wrap"); Surface.Execute("zoom-out");
        }
        else QCOMPARE(Text->toPlainText().toUtf8(), Large);
        QCOMPARE(Ready.count(), 1);
        connect(&Surface, &QtViewerSurface::Presenting, this, [&Surface](const QString&) {
            if (Surface.Snapshot().source_.name_ == "cancelled.txt")
                Surface.Open(Content("new selection", "current.txt"), ViewerPurpose::QuickPreview);
        });
        Surface.Open(Content(Large, "cancelled.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000); QCOMPARE(Text->toPlainText(), QString("new selection"));
        QCOMPARE(Surface.Snapshot().source_.name_, std::string("current.txt")); QCOMPARE(Ready.count(), 2);
        Surface.Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 3000); QCOMPARE(Service.RetainedBytes(), 0ULL);
    }
    void errorAndEncodingFallback()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(400, 300); Surface.show();
        QSignalSpy Failed(&Surface, &QtViewerSurface::Failed);
        Surface.Open(Content("plain data", "unsupported.pdf")); QTRY_COMPARE_WITH_TIMEOUT(Failed.count(), 1, 3000);
        QVERIFY(!Surface.IsReady()); Surface.findChild<QAction*>("hc.viewer.fallback")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000); QCOMPARE(Surface.findChild<QPlainTextEdit*>()->toPlainText(), QString("plain data"));
        Surface.Open(Content(QByteArray::fromHex("c7d1b1db"), "korean.txt")); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000);
        Surface.findChild<QAction*>("hc.viewer.encoding.utf-8")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(Failed.count(), 2, 3000);
        Surface.findChild<QAction*>("hc.viewer.encoding.cp949")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(Surface.IsReady(), 3000);
        QCOMPARE(Surface.findChild<QPlainTextEdit*>()->toPlainText(), QString::fromUtf8("한글"));
    }
    void pagedUnicodeAndNewlines()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); QtViewerSurface Surface(Service); Surface.resize(700, 400); Surface.show();
        const auto Original = QString(32767, QChar('x')) + QString::fromUtf8("😀\r\n한글 needle ") + QString(40000, QChar('y')) + " end-marker";
        Surface.Open(Content(Original.toUtf8(), "unicode.txt")); QTRY_VERIFY(Surface.IsReady());
        auto* Text = Surface.findChild<QPlainTextEdit*>("hc.viewer.text"); QCOMPARE(Text->toPlainText(), QString(32767, QChar('x')));
        Surface.Execute("next-part"); QVERIFY(Text->toPlainText().startsWith(QString::fromUtf8("😀\n한글 needle")));
        Surface.Execute("find"); auto* Search = Surface.findChild<QLineEdit*>("hc.viewer.search"); Search->setText("end-marker");
        QTest::keyClick(Search, Qt::Key_Return); QCOMPARE(Text->textCursor().selectedText(), QString("end-marker"));
        Search->setText(QString::fromUtf8("한글 needle")); QTest::keyClick(Search, Qt::Key_Return);
        QCOMPARE(Text->textCursor().selectedText(), QString::fromUtf8("한글 needle"));
        Text->selectAll(); Surface.Execute("copy"); QCOMPARE(QApplication::clipboard()->text(), Original); QApplication::clipboard()->clear();
        Surface.Close(); QVERIFY(!Surface.findChild<QAction*>("hc.viewer.next-part")->isVisible()); QCOMPARE(Service.RetainedBytes(), 0ULL);
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv); App.setAttribute(Qt::AA_Use96Dpi, true);
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    ViewerSurfaceTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstViewerSurface.moc"
