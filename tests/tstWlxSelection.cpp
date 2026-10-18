#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QWidget>
#include <QScreen>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include "pluginHost/hostedViewerSession.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "memoryStorage.hpp"

using namespace hc::plugins;
using namespace hc::storage;
using hc::viewer::ViewerStatus;
namespace
{
    struct Observation
    {
        std::thread::id gui_ = std::this_thread::get_id();
        std::atomic<unsigned> describes_ = 0, opens_ = 0, verifies_ = 0, bytes_ = 0;
        std::atomic<bool> onGui_ = false, reading_ = false;
        bool wait_ = false, noProgress_ = false, overRead_ = false, earlyEof_ = false, stale_ = false;
        void Touch() { if (std::this_thread::get_id() == gui_) onGui_ = true; }
    };
    class ObservedStream final : public IReadStream
    {
    public:
        ObservedStream(std::shared_ptr<IReadStream> Stream, std::shared_ptr<Observation> Value) : stream_(std::move(Stream)), value_(std::move(Value)) {}
        bool CanSeek() const override { return stream_->CanSeek(); }
        Task<Unit> Seek(std::uint64_t Offset, OperationContext Context) override { return stream_->Seek(Offset, Context); }
        Task<ReadChunk> Read(std::size_t MaxBytes, OperationContext Context) override
        {
            value_->Touch();
            if (value_->noProgress_) return Ready<ReadChunk>(ReadChunk{});
            if (value_->overRead_) return Ready<ReadChunk>(ReadChunk{std::vector<std::byte>(MaxBytes + 1), false});
            if (value_->earlyEof_) return Ready<ReadChunk>(ReadChunk{{}, true});
            auto Read = stream_->Read(std::min(MaxBytes, std::size_t(2)), Context).get();
            if (Read.HasValue()) value_->bytes_ += static_cast<unsigned>(Read.Value().bytes_.size());
            return Ready<ReadChunk>(std::move(Read));
        }
    private:
        std::shared_ptr<IReadStream> stream_;
        std::shared_ptr<Observation> value_;
    };
    class ObservedContent final : public IContentSource
    {
    public:
        explicit ObservedContent(std::shared_ptr<Observation> Value, const QByteArray& Bytes = "abc", std::string Name = "원본.TXT") : value_(std::move(Value))
        {
            auto Provider = std::make_shared<test::MemoryProvider>(); std::vector<std::byte> Data(Bytes.size());
            if (!Data.empty()) std::memcpy(Data.data(), Bytes.constData(), Bytes.size());
            const auto Id = Provider->Add(Provider->Root(), std::move(Name), std::move(Data));
            source_ = std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
        }
        EntrySnapshot Describe() const override { value_->Touch(); ++value_->describes_; return source_->Describe(); }
        Task<Unit> Verify(OperationContext Context) override
        {
            value_->Touch(); ++value_->verifies_;
            if (value_->stale_ && value_->bytes_) return Ready<Unit>(Error{ErrorCategory::StaleRevision, "selection-test-stale"});
            return source_->Verify(Context);
        }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override
        {
            value_->Touch(); ++value_->opens_; value_->reading_ = true;
            if (value_->wait_)
            {
                while (!Context.Check({"selection-test"})) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return Ready<std::shared_ptr<IReadStream>>(*Context.Check({"selection-test"}));
            }
            auto Opened = source_->OpenRead(Context).get();
            if (!Opened.HasValue()) return Ready<std::shared_ptr<IReadStream>>(Opened.Failure());
            return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(std::make_shared<ObservedStream>(Opened.Value(), value_)));
        }
    private:
        std::shared_ptr<IContentSource> source_;
        std::shared_ptr<Observation> value_;
    };
    QStringList Leases(const QString& Base)
    { return QDir(Base + "/hc-materialized-v1").entryList({"hc-lease-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot); }
}
class WlxSelectionTests : public QObject
{
    Q_OBJECT
private:
    bool candidate(const QString& Directory, PluginViewerOptions& Options, QByteArray Expression = "EXT=\"TXT\"", QByteArray Mode = "normal", bool Sdk = true)
    {
        if (!QDir().mkpath(Directory)) return false;
        const auto Root = QCoreApplication::applicationDirPath(); Options.executable_ = Root + "/hc-plugin-host-x64.exe";
        Options.binaryPath_ = Directory + "/test.wlx64"; Options.manifestPath_ = Directory + "/plugin.json";
        Options.defaultIniPath_ = Directory + "/lsplugin.ini";
        Options.limits_.startupMs_ = 1000; Options.limits_.requestMs_ = 500; Options.limits_.heartbeatMs_ = 200;
        if (!QFile::copy(Root + (Sdk ? "/hc_wlx_sdk_fixture.wlx64" : "/hc_wlx_fixture.wlx64"), Options.binaryPath_)) return false;
        QFile Binary(Options.binaryPath_); if (!Binary.open(QIODevice::ReadOnly)) return false;
        auto& Manifest = Options.approval_.reviewed_; Manifest.id_ = "hc.test.selection"; Manifest.version_ = "1.0";
        Manifest.binary_ = "test.wlx64"; Manifest.format_ = PluginFormat::Wlx; Manifest.extensions_ = {Extension::Viewer};
        Manifest.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Manifest.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Options.approval_.approved_ = true; Options.approval_.granted_ = Manifest.permissions_;
        QFile File(Options.manifestPath_); if (!File.open(QIODevice::WriteOnly) || File.write(EncodeManifest(Manifest)) <= 0) return false;
        QFile Ini(Options.defaultIniPath_);
        return Ini.open(QIODevice::WriteOnly) && Ini.write("[fixture]\r\nvalue=123\r\nmode=" + Mode + "\r\nexpression=" + Expression + "\r\n") > 0;
    }
private slots:
    void builtinProbeRunsOnWorker()
    {
        QTemporaryDir Directory; PluginViewerOptions Options;
        QVERIFY(candidate(Directory.filePath("plugin"), Options, "[0]=\"a\""));
        QWidget Parent; Parent.resize(320, 200); Parent.show();
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Observation = std::make_shared<struct Observation>(); std::atomic<unsigned> Probes = 0;
        PluginViewerSelectionPolicy Policy;
        Policy.builtinProbe_ = [&](const EntrySnapshot& Entry, std::span<const std::byte> Prefix) {
            Observation->Touch(); ++Probes;
            return Entry.size_ == 3 && Prefix.size() == 3 && Prefix.front() == std::byte{'a'};
        };
        auto Opened = Service.OpenAutomatic({Options}, std::make_shared<ObservedContent>(Observation), QString::number(Parent.winId(), 16), 320, 200, Policy);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Opened.Value()->Details().idle_, 5000);
        QVERIFY(Opened.Value()->Details().noMatch_); QCOMPARE(Probes.load(), 1u);
        QCOMPARE(Observation->opens_.load(), 1u); QCOMPARE(Observation->bytes_.load(), 3u);
        QVERIFY(!Observation->onGui_.load()); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void selection_data()
    {
        QTest::addColumn<QByteArray>("First"); QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("Second");
        QTest::addColumn<bool>("Force"); QTest::addColumn<bool>("Multimedia"); QTest::addColumn<int>("Expected"); QTest::addColumn<int>("Failures");
        const auto Row = [](const char* Name, QByteArray First, QByteArray Mode, bool Second, bool Force, bool Multimedia, int Expected, int Failures = 0) {
            QTest::newRow(Name) << First << Mode << Second << Force << Multimedia << Expected << Failures;
        };
        Row("original-extension", "EXT=\"TXT\"", "normal", false, false, false, 0);
        Row("ordered-miss", "EXT=\"BIN\"", "normal", true, false, false, 1);
        Row("first-match", "1", "normal", true, false, false, 0);
        Row("load-rejected", "1", "reject", true, false, false, 1);
        Row("only-rejected", "1", "reject", false, false, false, -1);
        Row("all-miss", "0", "normal", false, false, false, -1);
        Row("empty-definition", "0", "empty", false, false, false, 0);
        Row("absent-export", "0", "absent", false, false, false, 0);
        Row("malformed-next", "BROKEN", "normal", true, false, false, 1, 1);
        Row("hang-next", "1", "hang-detect", true, false, false, 1, 1);
        Row("crash-next", "1", "crash-detect", true, false, false, 1, 1);
        Row("revoked-next", "1", "revoked", true, false, false, 1, 1);
        Row("force-off", "FORCE", "require-force", false, false, false, -1);
        Row("force-on", "FORCE", "require-force", false, true, false, 0);
        Row("builtin-preserved", "1", "normal", false, false, true, -1);
        Row("multimedia-override", "MULTIMEDIA & EXT=\"TXT\"", "normal", false, false, true, 0);
        Row("forced-builtin-override", "1", "require-force", false, true, true, 0);
        Row("prefix-reuse", "[0]=0", "normal", true, false, false, 1);
    }
    void selection()
    {
        QFETCH(QByteArray, First); QFETCH(QByteArray, Mode); QFETCH(bool, Second); QFETCH(bool, Force);
        QFETCH(bool, Multimedia); QFETCH(int, Expected); QFETCH(int, Failures);
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); std::vector<PluginViewerOptions> Candidates(Second ? 2 : 1);
        QVERIFY(candidate(Directory.filePath("first"), Candidates[0], First, Mode, Mode != "absent"));
        if (Mode == "revoked") Candidates[0].approval_.approved_ = false;
        if (Second) QVERIFY(candidate(Directory.filePath("second"), Candidates[1], "[1]=\"b\" & EXT=\"TXT\""));
        auto Observation = std::make_shared<struct Observation>(); auto Source = std::make_shared<ObservedContent>(Observation);
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        std::atomic<unsigned> Factories = 0;
        PluginViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.OpenAutomatic(Candidates, Source, QString::number(Parent.winId(), 16), 320, 200, {Force, Multimedia});
        QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        if (Expected >= 0)
        {
            QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, ViewerStatus::Ready, 7000);
            QCOMPARE(Session->Details().selectedCandidate_, std::optional<std::size_t>(Expected));
            QCOMPARE(Session->Details().candidateFailures_.size(), std::size_t(Failures));
            QVERIFY(!Session->Details().noMatch_); QCOMPARE(Factories.load(), 1u);
            if (First == "[0]=0") { QCOMPARE(Observation->opens_.load(), 2u); QCOMPARE(Observation->bytes_.load(), 6u); }
            if (Expected == 1) { QFile Retired(Candidates[0].binaryPath_); QVERIFY(Retired.open(QIODevice::ReadWrite)); }
            Session->Close();
        }
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000);
        if (Expected < 0)
        {
            QVERIFY(Session->Details().noMatch_); QVERIFY(!Session->Snapshot().error_);
            QCOMPARE(Factories.load(), Mode == "reject" ? 1u : 0u);
        }
        QVERIFY(Leases(Directory.path()).isEmpty()); QVERIFY(!Observation->onGui_.load());
        QVERIFY(!Session->Details().host_.running_);
    }
    void contentFailures_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<QByteArray>("Code");
        QTest::newRow("no-progress") << QByteArray("no-progress") << QByteArray("wlx-detect-read-no-progress");
        QTest::newRow("over-read") << QByteArray("over-read") << QByteArray("wlx-detect-read-overflow");
        QTest::newRow("early-eof") << QByteArray("early-eof") << QByteArray("wlx-detect-premature-eof");
        QTest::newRow("stale") << QByteArray("stale") << QByteArray("selection-test-stale");
        QTest::newRow("budget") << QByteArray("budget") << QByteArray("wlx-detect-read-budget");
        QTest::newRow("revoked") << QByteArray("revoked") << QByteArray("plugin-review-required");
        QTest::newRow("invalid-detect") << QByteArray("invalid-detect") << QByteArray("wlx-detect-syntax");
        QTest::newRow("nul-name") << QByteArray("nul-name") << QByteArray("wlx-detect-name-encoding");
    }
    void contentFailures()
    {
        QFETCH(QByteArray, Mode); QFETCH(QByteArray, Code);
        QTemporaryDir Directory; PluginViewerOptions Options;
        QVERIFY(candidate(Directory.filePath("plugin"), Options, Mode == "invalid-detect" ? "BROKEN" : Mode == "nul-name" ? "EXT=\"TXT\"" : "[0]=\"a\""));
        if (Mode == "revoked") Options.approval_.approved_ = false;
        auto Observation = std::make_shared<struct Observation>();
        Observation->noProgress_ = Mode == "no-progress"; Observation->overRead_ = Mode == "over-read";
        Observation->earlyEof_ = Mode == "early-eof"; Observation->stale_ = Mode == "stale";
        QWidget Parent; std::atomic<unsigned> Factories = 0;
        PluginViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        OperationContext Context; if (Mode == "budget") Context.maxReadBytes_ = 2;
        const auto Name = Mode == "nul-name" ? std::string("original.txt\0bin", 16) : std::string("original.txt");
        auto Opened = Service.OpenAutomatic({Options}, std::make_shared<ObservedContent>(Observation, "abc", Name), QString::number(Parent.winId(), 16), 320, 200, {}, Context);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000);
        QCOMPARE(Opened.Value()->Snapshot().status_, ViewerStatus::Failed); QVERIFY(Opened.Value()->Snapshot().error_);
        QCOMPARE(Opened.Value()->Snapshot().error_->code_, Code.toStdString());
        QVERIFY(!Opened.Value()->Details().noMatch_); QCOMPARE(Factories.load(), 0u);
        if (Mode == "revoked") { QCOMPARE(Observation->opens_.load(), 0u); QCOMPARE(Observation->describes_.load(), 0u); }
        QVERIFY(!Observation->onGui_.load()); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void cancellationDuringPrefix()
    {
        QTemporaryDir Directory; PluginViewerOptions Options; QVERIFY(candidate(Directory.filePath("plugin"), Options, "[0]=1"));
        auto Observation = std::make_shared<struct Observation>(); Observation->wait_ = true;
        QWidget Parent; std::atomic<unsigned> Factories = 0;
        PluginViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.OpenAutomatic({Options}, std::make_shared<ObservedContent>(Observation), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Observation->reading_.load(), 4000);
        Opened.Value()->Cancel(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Opened.Value()->Snapshot().status_, ViewerStatus::Cancelled); QCOMPARE(Factories.load(), 0u);
        QVERIFY(!Opened.Value()->Details().host_.running_); QVERIFY(!Observation->onGui_.load());
    }
    void emptyCandidates()
    {
        QTemporaryDir Directory; QWidget Parent;
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        auto Observation = std::make_shared<struct Observation>();
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.OpenAutomatic({}, std::make_shared<ObservedContent>(Observation), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 1000);
        QVERIFY(Opened.Value()->Details().noMatch_); QCOMPARE(Observation->opens_.load(), 0u);
    }
    void callerDeadline()
    {
        QTemporaryDir Directory; PluginViewerOptions Options; QVERIFY(candidate(Directory.filePath("plugin"), Options, "1", "hang-detect"));
        QWidget Parent; std::atomic<unsigned> Factories = 0;
        PluginViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Observation = std::make_shared<struct Observation>();
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        QElapsedTimer Clock; Clock.start();
        auto Opened = Service.OpenAutomatic({Options, Options}, std::make_shared<ObservedContent>(Observation), QString::number(Parent.winId(), 16), 320, 200, {}, Context);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 3000);
        QCOMPARE(Opened.Value()->Snapshot().status_, ViewerStatus::Failed);
        QVERIFY(Opened.Value()->Snapshot().error_); QCOMPARE(Opened.Value()->Snapshot().error_->code_, std::string("deadline-exceeded"));
        QVERIFY(!Opened.Value()->Details().noMatch_); QCOMPARE(Factories.load(), 0u); QVERIFY(Clock.elapsed() < 2000);
        QVERIFY(!Opened.Value()->Details().host_.running_);
    }
    void byteOnlyUnicodeExtension()
    {
        QTemporaryDir Directory; PluginViewerOptions Options; QVERIFY(candidate(Directory.filePath("plugin"), Options, "SIZE=3 & [0]=\"a\""));
        QWidget Parent; Parent.resize(320, 200); Parent.show();
        PluginViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Observation = std::make_shared<struct Observation>();
        auto Opened = Service.OpenAutomatic({Options}, std::make_shared<ObservedContent>(Observation, "abc", "원본.😀"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_COMPARE_WITH_TIMEOUT(Opened.Value()->Snapshot().status_, ViewerStatus::Ready, 5000);
        QCOMPARE(Opened.Value()->Snapshot().source_.name_, std::string("원본.😀")); Opened.Value()->Close();
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 3000); QVERIFY(!Observation->onGui_.load());
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    WlxSelectionTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstWlxSelection.moc"
