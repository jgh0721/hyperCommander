#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include "HyperCommanderBuiltInPlugin/AlzEgg/builtinUnEgg.hpp"
#include "HyperCommanderBuiltInPlugin/AlzEgg/unEggBackend.hpp"
#include "HyperCommanderBuiltInPlugin/AlzEgg/unEggAbi.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include <ctime>
#include <tchar.h>
#include <unegg.h>

using namespace hc::storage;
using namespace hc::plugins;
static_assert(sizeof(EGGFileHeader) == sizeof(egg::Header));
static_assert(offsetof(EGGFileHeader, lastModified) == offsetof(egg::Header, modified_));
static_assert(sizeof(eggevent::Extract) == sizeof(egg::Extract));
static_assert(sizeof(eggevent::QueryPassword) == sizeof(egg::Password));
static_assert(sizeof(eggevent::Error) == sizeof(egg::Error));
namespace {
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
OperationContext WithPassword(const QString& Text) {
    OperationContext Context; Context.passwordPrompt_ = [Text](const std::string&, unsigned, const OperationContext&) -> Result<Password> { return Password(Text.toStdString()); }; return Context;
}
Result<std::vector<ArchiveHeader>> Headers(IArchiveBackend& Backend, const QString& Path, bool Test = false, OperationContext Context = {}) {
    auto Listing = Backend.Enumerate(Path.toStdString(), Test, Context); if (!Listing.HasValue()) return Listing.Failure();
    std::vector<ArchiveHeader> All;
    for (;;) { auto Page = Listing.Value()->Next(2, Context); if (!Page.HasValue()) return Page.Failure();
        for (const auto& Header : Page.Value().entries_) All.push_back(Header); if (Page.Value().end_) return All; }
}
}
class UnEggTests : public QObject {
    Q_OBJECT
    QJsonArray cases_; QJsonObject expected_; QString root_;
private slots:
    void initTestCase() {
        const auto Path = qEnvironmentVariable("HC_UNEGG_CORPUS"); QVERIFY(QFileInfo::exists(Path));
        const auto Data = QJsonDocument::fromJson(Read(Path)).object(); cases_ = Data["cases"].toArray(); expected_ = Data["expected"].toObject();
        QVERIFY(cases_.size() >= 23); root_ = QFileInfo(Path).absolutePath(); QVERIFY(BuiltinUnEggOptions().approval_.approved_);
    }
    void corpus_data() {
        QTest::addColumn<int>("Row");
        for (int Index = 0; Index < cases_.size(); ++Index) QTest::newRow(QFileInfo(cases_[Index].toObject()["archive"].toString()).fileName().toUtf8()) << Index;
    }
    void corpus() {
        QFETCH(int, Row); const auto Case = cases_[Row].toObject(); const auto Path = Case["archive"].toString();
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); auto Context = WithPassword(Case["password"].toString()); QTemporaryDir Output;
        auto Listed = Headers(*Backend, Path, false, Context);
        if (QFileInfo(Path).fileName() == "small_kor.egg" || QFileInfo(Path).fileName() == "small_shift.egg") {
            // The pinned vendor DLL also rejects these locale-coded samples
            // when called directly (OpenArchive=7, error event=0x02000f00).
            QVERIFY(!Listed.HasValue()); QCOMPARE(Listed.Failure().code_, std::string("unegg-result:33558272")); return;
        }
        QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        QVERIFY(!Listed.Value().empty());
        for (const auto& Header : Listed.Value()) {
            if (Header.directory_) continue;
            const auto Target = Output.filePath("selected.bin"); auto Done = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), Context);
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); const auto Bytes = Read(Target); QCOMPARE(uint64_t(Bytes.size()), Header.size_);
            const auto Name = QFileInfo(QString::fromStdString(Header.name_)).fileName();
            if (expected_.contains(Name)) QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()).toUpper(), expected_[Name].toString());
        }
        auto Tested = Headers(*Backend, Path, true, Context); QVERIFY2(Tested.HasValue(), Tested.HasValue() ? "" : Tested.Failure().code_.c_str());
        QVERIFY(!Backend->Update({}, {}).HasValue()); QVERIFY(!GetModuleHandleW(L"UnEGG64.dll"));
    }
    void longInputAndOutputPaths() {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        auto Deep = Directory.path(); while (Deep.size() < 340) Deep += '/' + QString(45, QChar(0xAC00));
        QVERIFY(QDir().mkpath(Deep)); const auto Path = Deep + "/input.egg";
        QVERIFY(QFile::copy(QDir(root_).filePath("defaults.egg"), Path));
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions());
        auto Listed = Headers(*Backend, Path); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        bool Extracted = false;
        for (const auto& Header : Listed.Value()) {
            if (Header.directory_) continue;
            const auto Output = Deep + "/result.bin";
            const auto Done = Backend->Extract(Path.toStdString(), Header, Output.toStdString(), {});
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(uint64_t(Read(Output).size()), Header.size_); Extracted = true; break;
        }
        QVERIFY(Extracted);
    }
    void errorsAndPolicy() {
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); const auto Path = QDir(root_).filePath("lorem_long_aes256.egg"); QTemporaryDir Output;
        auto Listed = Headers(*Backend, Path); QVERIFY(Listed.HasValue()); const auto Header = Listed.Value().front(); QVERIFY(Header.encrypted_);
        const auto Target = Output.filePath("result");
        auto Required = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), {});
        QVERIFY(!Required.HasValue()); QCOMPARE(Required.Failure().category_, ErrorCategory::AccessDenied); QVERIFY(!QFileInfo::exists(Target));
        auto Wrong = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), WithPassword("wrong")); QVERIFY(!Wrong.HasValue()); QVERIFY(!QFileInfo::exists(Target));
        auto Decline = WithPassword(""); Decline.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<Password> { return Error{ErrorCategory::Cancelled, "declined"}; };
        auto Cancelled = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), Decline); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        auto Oversize = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), WithPassword(QString(1024, 'x'))); QVERIFY(!Oversize.HasValue()); QCOMPARE(Oversize.Failure().category_, ErrorCategory::InvalidRequest);
        unsigned Asked = 0; OperationContext Retry; Retry.passwordPrompt_ = [&](const std::string&, unsigned, const OperationContext&) -> Result<Password> { return Password(++Asked == 1 ? "wrong" : "password12345!"); };
        auto Correct = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), Retry); QVERIFY2(Correct.HasValue(), Correct.HasValue() ? "" : Correct.Failure().code_.c_str()); QCOMPARE(Asked, 2u);
        const auto Plain = QDir(root_).filePath("defaults.egg");
        OperationContext Limited; Limited.maxEnumerationEntries_ = 1; auto Budget = Backend->Enumerate(Plain.toStdString(), false, Limited); QVERIFY(!Budget.HasValue()); QCOMPARE(Budget.Failure().category_, ErrorCategory::BudgetExceeded);
        Limited = {}; Limited.workload_.intent_ = WorkloadIntent::Automatic; Limited.workload_.automaticFileBytes_ = 128;
        QVERIFY(!Headers(*Backend, Plain, false, Limited).HasValue());
        std::stop_source Stop; Stop.request_stop(); Limited = {}; Limited.stopToken_ = Stop.get_token();
        auto Early = Backend->Enumerate(Plain.toStdString(), false, Limited); QVERIFY(!Early.HasValue()); QCOMPARE(Early.Failure().category_, ErrorCategory::Cancelled);
        const auto Broken = Output.filePath("broken.egg"); QVERIFY(Put(Broken, Read(Plain).left(30))); QVERIFY(!Headers(*Backend, Broken, true).HasValue());
        const auto Missing = Output.filePath("number.vol1.egg"); QVERIFY(QFile::copy(QDir(root_).filePath("number.vol1.egg"), Missing)); QVERIFY(!Headers(*Backend, Missing, true).HasValue());
    }
    void corruptPayloadAndUnsafeNames() {
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); QTemporaryDir Directory;
        const auto Original = Read(QDir(root_).filePath("defaults_nocompress.egg")); auto Data = Original;
        const auto Block = Data.indexOf(QByteArray::fromHex("130cb502")); QVERIFY(Block >= 0 && Block + 86 < Data.size());
        Data[Block + 86] = char(static_cast<unsigned char>(Data[Block + 86]) ^ 0x80);
        const auto Path = Directory.filePath("corrupt.egg"); QVERIFY(Put(Path, Data));
        auto Listed = Headers(*Backend, Path); QVERIFY(Listed.HasValue()); bool Failed = false;
        for (const auto& Header : Listed.Value()) {
            if (Header.directory_) continue;
            const auto Target = Directory.filePath("output"); QVERIFY(Put(Target, "preserved"));
            auto Done = Backend->Extract(Path.toStdString(), Header, Target.toStdString(), {});
            if (!Done.HasValue()) { Failed = true; QCOMPARE(Read(Target), QByteArray("preserved")); }
        }
        QVERIFY(Failed); QVERIFY(!Headers(*Backend, Path, true).HasValue());
        Data = Original; const QByteArray Name("lorem_ipsum_short.txt"); const auto At = Data.indexOf(Name); QVERIFY(At > 0);
        Data.replace(At, Name.size(), QByteArray("../") + QByteArray(Name.size() - 3, 'x'));
        const auto Unsafe = Directory.filePath("unsafe.egg"); QVERIFY(Put(Unsafe, Data)); auto Sanitized = Headers(*Backend, Unsafe);
        // The vendor replaces '..' with '__' before exposing the name. Extraction
        // must still target only our supplied output, never that sanitized path.
        QVERIFY(Sanitized.HasValue());
        for (const auto& Header : Sanitized.Value()) QVERIFY(!QString::fromStdString(Header.name_).split('/').contains(".."));
        const auto Safe = std::ranges::find_if(Sanitized.Value(), [](const auto& Item) { return Item.size_ == 525; }); QVERIFY(Safe != Sanitized.Value().end());
        const auto SafeOutput = Directory.filePath("safe-output"); QVERIFY(Backend->Extract(Unsafe.toStdString(), *Safe, SafeOutput.toStdString(), {}).HasValue());
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(Read(SafeOutput), QCryptographicHash::Sha256).toHex()).toUpper(), expected_["lorem_ipsum_short.txt"].toString());
        Data = Original; QVERIFY(Data.contains("lorem_ipsum_long.pdf")); Data.replace("lorem_ipsum_long.pdf", "lorem_ipsum_long.tif");
        const auto Duplicate = Directory.filePath("duplicate.egg"); QVERIFY(Put(Duplicate, Data)); auto DuplicateList = Headers(*Backend, Duplicate); QVERIFY(DuplicateList.HasValue());
        const auto Chosen = std::ranges::find_if(DuplicateList.Value(), [](const auto& Item) { return Item.name_ == "lorem_ipsum_long.tif"; }); QVERIFY(Chosen != DuplicateList.Value().end());
        auto Ambiguous = Backend->Extract(Duplicate.toStdString(), *Chosen, Directory.filePath("duplicate-output").toStdString(), {});
        QVERIFY(!Ambiguous.HasValue()); QCOMPARE(Ambiguous.Failure().category_, ErrorCategory::InvalidRequest);
    }
    void unicodeName() {
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); QTemporaryDir Directory;
        auto Data = Read(QDir(root_).filePath("defaults.egg")); const QByteArray Old("lorem_ipsum_short.txt");
        const auto At = Data.indexOf(Old); QVERIFY(At > 0); auto Name = QByteArray::fromHex("ed959ceab880"); Name += QByteArray(Old.size() - Name.size(), 'x');
        Data.replace(At, Old.size(), Name); const auto Path = Directory.filePath("unicode.egg"); QVERIFY(Put(Path, Data));
        auto Listed = Headers(*Backend, Path); QVERIFY(Listed.HasValue());
        const auto Chosen = std::ranges::find_if(Listed.Value(), [&](const auto& Item) { return Item.name_ == Name.toStdString(); }); QVERIFY(Chosen != Listed.Value().end());
        auto Done = Backend->Extract(Path.toStdString(), *Chosen, Directory.filePath("output").toStdString(), {}); QVERIFY(Done.HasValue());
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(Read(Directory.filePath("output")), QCryptographicHash::Sha256).toHex()).toUpper(), expected_["lorem_ipsum_short.txt"].toString());
    }
    void commonProviderAndCancellation() {
        auto Backend = MakeApprovedWcxBackend(BuiltinUnEggOptions()); QTemporaryDir Cache; const auto Path = QDir(root_).filePath("defaults.egg");
        Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}}; auto Provider = MakeWcxReadProvider({}, Root, Cache.path().toStdString(), Backend);
        auto At = Root; At.nesting_.back().entry_ = "/lorem_ipsum_short.txt"; auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY(Stream.HasValue()); QVERIFY(Stream.Value()->CanSeek());
        QVERIFY(Stream.Value()->Seek(10, {}).get().HasValue()); auto Bytes = Stream.Value()->Read(20, {}).get(); QVERIFY(Bytes.HasValue()); QCOMPARE(Bytes.Value().bytes_.size(), size_t(20));
        auto Listed = Headers(*Backend, Path); QVERIFY(Listed.HasValue());
        const auto Large = std::ranges::find_if(Listed.Value(), [](const auto& Item) { return Item.size_ > 1000000; }); QVERIFY(Large != Listed.Value().end());
        const auto Module = LoadLibraryExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(BuiltinUnEggOptions().binaryPath_).utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        QVERIFY(Module); auto Unload = qScopeGuard([&] { FreeLibrary(Module); }); auto Native = MakeUnEggBackend(Module);
        std::stop_source Stop; OperationContext Active; Active.stopToken_ = Stop.get_token(); Active.progress_ = [&](uint64_t Done) { if (Done > 10000) Stop.request_stop(); };
        const auto Target = Cache.filePath("cancelled"); auto Cancelled = Native->Extract(Path.toStdString(), *Large, Target.toStdString(), Active);
        QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled); QVERIFY(!QFileInfo::exists(Target));
        QVERIFY(QDir(Cache.path()).entryList({".egg-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
};
QTEST_GUILESS_MAIN(UnEggTests)
#include "tstUnEgg.moc"
