#include "UniqueLibs/qtViewerProvider.hpp"
#include <QtTest>
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include <QBuffer>
#include <QImage>
#include <semaphore>
#include <cstring>

using namespace hc::viewer;
using namespace hc::storage;
using namespace std::chrono_literals;

namespace
{
    struct Probe
    {
        std::atomic<int> opens_{0}, liveStreams_{0}, verifies_{0};
        std::atomic<std::size_t> largestRead_{0};
        std::function<void()> onRead_;
        bool staleAfterRead_ = false, noProgress_ = false;
    };
    class Stream final : public IReadStream
    {
    public:
        Stream(QByteArray Data, std::shared_ptr<Probe> State) : data_(std::move(Data)), state_(std::move(State)) { ++state_->liveStreams_; }
        ~Stream() override { --state_->liveStreams_; }
        bool CanSeek() const override { return false; }
        Task<Unit> Seek(std::uint64_t, OperationContext) override { return Ready<Unit>(Error{ErrorCategory::Unsupported, "test-no-seek"}); }
        Task<ReadChunk> Read(std::size_t Maximum, OperationContext Context) override
        {
            state_->largestRead_ = std::max(state_->largestRead_.load(), Maximum);
            if (state_->onRead_) state_->onRead_();
            if (const auto Failure = Context.Check({})) return Ready<ReadChunk>(*Failure);
            if (state_->noProgress_) return Ready<ReadChunk>(ReadChunk{});
            const auto Count = std::min(Maximum, static_cast<std::size_t>(data_.size()) - offset_);
            ReadChunk Chunk; Chunk.bytes_.resize(Count);
            if (Count) std::memcpy(Chunk.bytes_.data(), data_.constData() + offset_, Count);
            offset_ += Count; Chunk.eof_ = offset_ == data_.size();
            return Ready<ReadChunk>(std::move(Chunk));
        }
    private:
        QByteArray data_;
        std::shared_ptr<Probe> state_;
        std::size_t offset_ = 0;
    };
    class Source final : public IContentSource
    {
    public:
        Source(QByteArray Data, std::string Name = "sample.txt") : data_(std::move(Data))
        {
            entry_.name_ = std::move(Name); entry_.size_ = data_.size();
            entry_.locator_ = {LocatorKind::Local, "viewer-test"};
        }
        EntrySnapshot Describe() const override { return entry_; }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext) override
        {
            ++state_->opens_;
            return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(std::make_shared<Stream>(data_, state_)));
        }
        Task<Unit> Verify(OperationContext Context) override
        {
            if (const auto Failure = Context.Check({})) return Ready<Unit>(*Failure);
            if (++state_->verifies_ > 1 && state_->staleAfterRead_) return Ready<Unit>(Error{ErrorCategory::StaleRevision, "test-source-changed"});
            return Ready<Unit>(Unit{});
        }
        std::shared_ptr<Probe> state_ = std::make_shared<Probe>();
        EntrySnapshot entry_;
    private:
        QByteArray data_;
    };
    class BrokenProvider final : public IViewerProvider
    {
    public:
        explicit BrokenProvider(int Mode) : mode_(Mode) {}
        std::string Id() const override { return "broken"; }
        Result<int> Probe(const EntrySnapshot&, std::span<const std::byte>) const override { if (mode_ == 0) throw std::runtime_error("probe"); return 100; }
        Result<ViewerDocument> Open(ViewerInput, OperationContext, std::function<void(std::uint64_t)>) override
        {
            if (mode_ == 1) throw std::runtime_error("open");
            ViewerDocument Document;
            if (mode_ == 2) { Document.kind_ = ViewerKind::Image; Document.width_ = 5; Document.height_ = 5; }
            else Document.text_ = u"value";
            return Document;
        }
        std::vector<std::string> Commands() const override { throw std::runtime_error("commands"); }
    private:
        int mode_;
    };
    struct Gate
    {
        std::binary_semaphore entered_{0}, release_{0};
        void Enter() { entered_.release(); if (!release_.try_acquire_for(3s)) throw std::runtime_error("test-gate-timeout"); }
    };
}
class ViewerTests final : public QObject
{
    Q_OBJECT
private slots:
    void localAndArchiveContent_data()
    {
        QTest::addColumn<bool>("archive"); QTest::addColumn<QString>("name"); QTest::addColumn<QByteArray>("bytes");
        QImage Image(4, 3, QImage::Format_RGB32); Image.fill(Qt::green);
        QByteArray Png; QBuffer Buffer(&Png); QVERIFY(Buffer.open(QIODevice::WriteOnly)); QVERIFY(Image.save(&Buffer, "PNG"));
        for (const bool Archive : {false, true})
        {
            const auto Prefix = Archive ? "archive-" : "local-";
            QTest::newRow(qPrintable(QString(Prefix) + "text")) << Archive << QString::fromUtf8("한글.txt") << QString::fromUtf8("한글 본문\n").toUtf8();
            QTest::newRow(qPrintable(QString(Prefix) + "markdown")) << Archive << QString("readme.md") << QByteArray("# title\n[link](https://example.invalid)");
            QTest::newRow(qPrintable(QString(Prefix) + "image")) << Archive << QString("image.png") << Png;
        }
    }
    void localAndArchiveContent()
    {
        QFETCH(bool, archive); QFETCH(QString, name); QFETCH(QByteArray, bytes);
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath(archive ? "viewer.hct" : name);
        QByteArray Stored = bytes;
        if (archive)
        {
            // Fixture little-endian format; no installed commercial plugin.
            Stored = QByteArray("HCWCX02\0", 8);
            const auto Number = [&](quint32 Value) { for (int Index = 0; Index < 4; ++Index) Stored.append(static_cast<char>((Value >> (Index * 8)) & 255)); };
            Number(1); Number(static_cast<quint32>(name.size())); Number(static_cast<quint32>(bytes.size())); Number(0);
            for (const auto Unit : name) { Stored.append(static_cast<char>(Unit.unicode() & 255)); Stored.append(static_cast<char>(Unit.unicode() >> 8)); }
            Stored += bytes;
        }
        QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Stored), Stored.size()); File.close();
        Locator At{LocatorKind::Local, Path.toStdString()};
        std::shared_ptr<IStorageProvider> Reader;
        if (archive)
        {
            Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}};
            Reader = MakeWcxReadProvider((QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64").toStdString(), Root, Directory.path().toStdString());
            At = Root; At.nesting_.back().entry_ = "/" + name.toStdString();
        }
        else Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        ViewerService Service(MakeBuiltinViewerRegistry());
        // Both purposes use the same source/session contract. Dropping the
        // lease schedules physical deletion on the materializer reclaimer.
        for (int Index = 0; Index < 12; ++Index)
        {
            auto Input = std::make_shared<ProviderContentSource>(Reader, Entry.Value());
            ViewerRequest Request{Input}; Request.purpose_ = Index % 2 ? ViewerPurpose::QuickPreview : ViewerPurpose::FullViewer;
            auto Open = Service.Open(Request); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready);
            auto Result = Open.Value().opened_.get(); QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
            const auto Document = Open.Value().session_->Document(); QVERIFY(Document);
            if (name.endsWith(".png")) { QCOMPARE(Document->kind_, ViewerKind::Image); QCOMPARE(Document->width_, 4U); }
            else { QCOMPARE(QString::fromStdU16String(Document->text_), QString::fromUtf8(bytes)); QCOMPARE(Document->kind_, name.endsWith(".md") ? ViewerKind::Markdown : ViewerKind::Text); }
            QTRY_VERIFY_WITH_TIMEOUT(QDir(Directory.filePath("hc-materialized-v1")).entryList({"hc-lease-v1-*"}, QDir::Dirs).isEmpty(), 3000);
            Open.Value().session_->Close();
        }
        QCOMPARE(Service.RetainedBytes(), 0ULL);
        QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Stored);
    }
    void rapidSwitchCancelsOldSession()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); auto Old = std::make_shared<Source>("old"); auto Barrier = std::make_shared<Gate>();
        Old->state_->onRead_ = [Barrier] { Barrier->Enter(); };
        auto First = Service.Open({Old}); QVERIFY(First.HasValue()); QVERIFY(Barrier->entered_.try_acquire_for(3s));
        First.Value().session_->Close();
        auto Second = Service.Open({std::make_shared<Source>("current")}); QVERIFY(Second.HasValue());
        QVERIFY(Second.Value().opened_.wait_for(3s) == std::future_status::ready); QVERIFY(Second.Value().opened_.get().HasValue());
        QCOMPARE(Second.Value().session_->Document()->text_, std::u16string(u"current"));
        Barrier->release_.release(); QVERIFY(First.Value().opened_.wait_for(3s) == std::future_status::ready); QVERIFY(!First.Value().opened_.get().HasValue());
        QVERIFY(!First.Value().session_->Document()); QCOMPARE(First.Value().session_->Snapshot().status_, ViewerStatus::Closed);
        QCOMPARE(Second.Value().session_->Document()->text_, std::u16string(u"current"));
    }
    void text_data()
    {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QString>("expected"); QTest::addColumn<QString>("encoding");
        QTest::newRow("utf8") << QString::fromUtf8("한글 🙂\ntext").toUtf8() << QString::fromUtf8("한글 🙂\ntext") << "utf-8";
        QTest::newRow("utf8-bom") << QByteArray::fromHex("efbbbf4142") << QString("AB") << "utf-8";
        QTest::newRow("utf16-le") << QByteArray::fromHex("fffe410000ac3dd842de") << QString::fromUtf8("A가🙂") << "utf-16le";
        QTest::newRow("utf16-be") << QByteArray::fromHex("feff0041ac00d83dde42") << QString::fromUtf8("A가🙂") << "utf-16be";
        QTest::newRow("utf32-le") << QByteArray::fromHex("fffe00004100000000ac000042f60100") << QString::fromUtf8("A가🙂") << "utf-32le";
        QTest::newRow("utf32-be") << QByteArray::fromHex("0000feff000000410000ac000001f642") << QString::fromUtf8("A가🙂") << "utf-32be";
        QTest::newRow("cp949") << QByteArray::fromHex("c7d1b1db") << QString::fromUtf8("한글") << "cp949";
        QTest::newRow("empty") << QByteArray{} << QString{} << "utf-8";
    }
    void text()
    {
        QFETCH(QByteArray, bytes); QFETCH(QString, expected); QFETCH(QString, encoding);
        ViewerService Service(MakeBuiltinViewerRegistry()); auto Input = std::make_shared<Source>(bytes);
        OperationContext Context; Context.maxReadBytes_ = 1;
        auto Open = Service.Open({Input}, Context); QVERIFY(Open.HasValue());
        auto& Handle = Open.Value(); QVERIFY(Handle.opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Handle.opened_.get(); QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        auto Document = Handle.session_->Document(); QVERIFY(Document);
        QCOMPARE(QString::fromStdU16String(Document->text_), expected);
        QCOMPARE(QString::fromStdString(Document->encoding_), encoding);
        QCOMPARE(Handle.session_->Snapshot().status_, ViewerStatus::Ready);
        QCOMPARE(Input->state_->opens_.load(), 1); QCOMPARE(Input->state_->liveStreams_.load(), 0);
        QCOMPARE(Input->state_->verifies_.load(), 2);
        Handle.session_->Close(); QVERIFY(!Handle.session_->Document());
        QCOMPARE(Service.RetainedBytes(), Document->Bytes());
        Document.reset(); QCOMPARE(Service.RetainedBytes(), 0ULL);
    }
    void malformedText_data()
    {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QString>("encoding");
        QTest::newRow("utf8-overlong") << QByteArray::fromHex("c0af") << "utf-8";
        QTest::newRow("utf8-truncated") << QByteArray::fromHex("eab0") << "utf-8";
        QTest::newRow("utf16-truncated") << QByteArray::fromHex("fffe4100ff") << "auto";
        QTest::newRow("utf16-high-surrogate") << QByteArray::fromHex("fffe00d8") << "auto";
        QTest::newRow("utf16-low-surrogate") << QByteArray::fromHex("feffdc00") << "auto";
        QTest::newRow("utf32-surrogate") << QByteArray::fromHex("fffe000000d80000") << "auto";
        QTest::newRow("utf32-overflow") << QByteArray::fromHex("0000feff00110000") << "auto";
        QTest::newRow("utf32-truncated") << QByteArray::fromHex("0000feff0000") << "auto";
    }
    void malformedText()
    {
        QFETCH(QByteArray, bytes); QFETCH(QString, encoding);
        ViewerService Service(MakeBuiltinViewerRegistry()); ViewerRequest Request{std::make_shared<Source>(bytes)};
        Request.provider_ = "builtin.text"; Request.encoding_ = encoding.toStdString();
        auto Open = Service.Open(Request); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Open.Value().opened_.get(); QVERIFY(!Result.HasValue());
        QCOMPARE(Result.Failure().code_, std::string("viewer-invalid-encoding")); QVERIFY(!Open.Value().session_->Document());
    }
    void markdownAndBinary()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); auto Open = Service.Open({std::make_shared<Source>("# title\n**body**", "readme.md")});
        QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(Open.Value().opened_.get().HasValue());
        QCOMPARE(Open.Value().session_->Document()->kind_, ViewerKind::Markdown);
        Open.Value().session_->Close();
        auto Binary = Service.Open({std::make_shared<Source>(QByteArray::fromHex("00010203"), "file.dat")});
        QVERIFY(Binary.HasValue()); QVERIFY(Binary.Value().opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Binary.Value().opened_.get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().code_, std::string("viewer-no-provider"));
    }
    void imageAndBudgets()
    {
        QImage Image(13, 7, QImage::Format_RGBA8888); Image.fill(QColor(23, 45, 67, 89));
        QByteArray Png; QBuffer Buffer(&Png); QVERIFY(Buffer.open(QIODevice::WriteOnly)); QVERIFY(Image.save(&Buffer, "PNG"));
        ViewerService Service(MakeBuiltinViewerRegistry()); ViewerRequest Request{std::make_shared<Source>(Png, "image.unknown")};
        auto Open = Service.Open(Request); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(Open.Value().opened_.get().HasValue());
        const auto Document = Open.Value().session_->Document(); QCOMPARE(Document->kind_, ViewerKind::Image);
        QCOMPARE(Document->width_, 13U); QCOMPARE(Document->height_, 7U); QCOMPARE(Document->pixels_.size(), 13U * 7 * 4);
        QCOMPARE(std::to_integer<int>(Document->pixels_[0]), 23); QCOMPARE(std::to_integer<int>(Document->pixels_[3]), 89);
        Request.limits_.maxImagePixels_ = 90;
        auto Limited = Service.Open(Request); QVERIFY(Limited.HasValue()); QVERIFY(Limited.Value().opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Limited.Value().opened_.get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::BudgetExceeded);
        auto Broken = Service.Open({std::make_shared<Source>(Png.left(24), "broken.png")}); QVERIFY(Broken.HasValue());
        QVERIFY(Broken.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(!Broken.Value().opened_.get().HasValue());
    }
    void readFailureAndStaleRevision()
    {
        for (const bool Stale : {false, true})
        {
            ViewerService Service(MakeBuiltinViewerRegistry()); auto Input = std::make_shared<Source>(QByteArray(9000, 'a'));
            Input->state_->staleAfterRead_ = Stale; Input->state_->noProgress_ = !Stale;
            auto Open = Service.Open({Input}); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready);
            auto Result = Open.Value().opened_.get(); QVERIFY(!Result.HasValue());
            QCOMPARE(Result.Failure().category_, Stale ? ErrorCategory::StaleRevision : ErrorCategory::Internal);
            QCOMPARE(Input->state_->liveStreams_.load(), 0); QCOMPARE(Service.RetainedBytes(), 0ULL);
        }
    }
    void providerFailuresAlwaysFinish()
    {
        for (int Mode = 0; Mode != 4; ++Mode)
        {
            auto Registry = std::make_shared<ViewerRegistry>(); QVERIFY(Registry->Register(std::make_shared<BrokenProvider>(Mode)).HasValue());
            QVERIFY(!Registry->Register(std::make_shared<BrokenProvider>(Mode)).HasValue());
            ViewerService Service(Registry); auto Input = std::make_shared<Source>("input");
            auto Open = Service.Open({Input}); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready);
            QVERIFY(!Open.Value().opened_.get().HasValue()); QCOMPARE(Input->state_->liveStreams_.load(), 0); QCOMPARE(Service.RetainedBytes(), 0ULL);
        }
    }
    void cancelAndCloseDuringRead()
    {
        for (const bool Close : {false, true})
        {
            ViewerService Service(MakeBuiltinViewerRegistry()); auto Input = std::make_shared<Source>("pending"); auto Barrier = std::make_shared<Gate>();
            Input->state_->onRead_ = [Barrier] { Barrier->Enter(); };
            auto Open = Service.Open({Input}); QVERIFY(Open.HasValue()); QVERIFY(Barrier->entered_.try_acquire_for(3s));
            QVERIFY(Open.Value().opened_.wait_for(0ms) == std::future_status::timeout);
            if (Close) Open.Value().session_->Close(); else Open.Value().session_->Cancel();
            Barrier->release_.release(); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready);
            auto Result = Open.Value().opened_.get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::Cancelled);
            QCOMPARE(Open.Value().session_->Snapshot().status_, Close ? ViewerStatus::Closed : ViewerStatus::Cancelled);
            QCOMPARE(Input->state_->liveStreams_.load(), 0); QCOMPARE(Service.RetainedBytes(), 0ULL);
            QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 3000);
        }
    }
    void sessionAndMemoryLimits()
    {
        ViewerService Service(MakeBuiltinViewerRegistry(), {1, 2, 12});
        auto Input = std::make_shared<Source>("123456"); auto First = Service.Open({Input}); QVERIFY(First.HasValue());
        QVERIFY(First.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(First.Value().opened_.get().HasValue());
        auto Second = Service.Open({Input}); QVERIFY(Second.HasValue()); QVERIFY(Second.Value().opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Second.Value().opened_.get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().code_, std::string("viewer-total-memory-budget"));
        QVERIFY(!Service.Open({Input}).HasValue()); Second.Value().session_->Close(); First.Value().session_->Close();
        QCOMPARE(Service.RetainedBytes(), 0ULL);
        auto Third = Service.Open({Input}); QVERIFY(Third.HasValue()); QVERIFY(Third.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(Third.Value().opened_.get().HasValue());
    }
    void boundedReadAndTextLimits()
    {
        ViewerService Service(MakeBuiltinViewerRegistry()); auto Input = std::make_shared<Source>(QByteArray(10000, 'a'));
        ViewerRequest Request{Input}; OperationContext Context; Context.maxReadBytes_ = 17;
        auto Open = Service.Open(Request, Context); QVERIFY(Open.HasValue()); QVERIFY(Open.Value().opened_.wait_for(5s) == std::future_status::ready); QVERIFY(Open.Value().opened_.get().HasValue());
        QCOMPARE(Input->state_->largestRead_.load(), 17U); QCOMPARE(Open.Value().session_->Snapshot().bytesRead_, 10000ULL);
        Request.limits_.maxTextCharacters_ = 9999;
        auto Limited = Service.Open(Request); QVERIFY(Limited.HasValue()); QVERIFY(Limited.Value().opened_.wait_for(5s) == std::future_status::ready);
        auto Result = Limited.Value().opened_.get(); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().code_, std::string("viewer-text-budget"));
        Request.limits_.maxInputBytes_ = 9999; QVERIFY(!Service.Open(Request).HasValue());
    }
};
QTEST_GUILESS_MAIN(ViewerTests)
#include "tstViewer.moc"
