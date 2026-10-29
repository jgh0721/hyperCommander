#define NOMINMAX
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QtEndian>
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/wcxMutation.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/sevenZipJob.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "pluginHost/archiveCodec.hpp"
#include <Windows.h>
#include <winioctl.h>
#include <set>
#include <algorithm>
#include <atomic>
#include "msiBrowseFixture.hpp"
#include "UniqueLibs/archiveExtractJob.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "commonLibs/plugins/archiveSelectionDigest.hpp"

using namespace hc::storage;
using namespace hc::plugins;
namespace {
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
bool Put(const QString& Path, const QByteArray& Data) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Data) == Data.size(); }
OperationContext PasswordContext(const QString& Password) {
    OperationContext Context; Context.passwordPrompt_ = [Password](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> {
        return hc::storage::Password(Password.toStdString());
    }; return Context;
}
QJsonObject ZipxFixtures() { return QJsonDocument::fromJson(Read(QFINDTESTDATA("fixtures/zipx.json"))).object(); }
QJsonArray SplitFixtures() {
    auto Cases = QJsonDocument::fromJson(Read(QFINDTESTDATA("fixtures/split-volumes.json")))["cases"].toArray();
    for (const auto& Value : ZipxFixtures()["cases"].toArray()) {
        auto Case = Value.toObject(); if (!Case["name"].toString().contains("split")) continue;
        Case["first"] = Case["name"].toString() + ".zipx"; Cases.append(Case);
    }
    return Cases;
}
QString SplitName(QString Name, bool Upper, bool Padded) {
    if (Padded) {
        const QRegularExpression Pattern("\\.part([0-9]+)\\.rar$"); const auto Match = Pattern.match(Name);
        if (Match.hasMatch()) Name.replace(Match.capturedStart(1), Match.capturedLength(1), Match.captured(1).rightJustified(3, '0'));
        const QRegularExpression Numbered("\\.([0-9]{3})$"); const auto Number = Numbered.match(Name);
        if (Number.hasMatch()) Name.replace(Number.capturedStart(1), Number.capturedLength(1), Number.captured(1).rightJustified(4, '0'));
    }
    return Upper ? Name.toUpper() : Name;
}
bool WriteSplitFixture(const QJsonObject& Case, const QTemporaryDir& Directory, bool Upper = false, bool Padded = false) {
    for (const auto& Value : Case["volumes"].toArray()) {
        const auto Volume = Value.toObject(); const auto Bytes = QByteArray::fromBase64(Volume["data"].toString().toLatin1());
        if (QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Volume["sha256"].toString().toLatin1()
            || !Put(Directory.filePath(SplitName(Volume["name"].toString(), Upper, Padded)), Bytes)) return false;
    }
    return true;
}
QString WriteZipxFixture(const QJsonObject& Case, const QTemporaryDir& Directory, bool Upper = false) {
    for (const auto& Value : Case["volumes"].toArray()) {
        const auto Volume = Value.toObject(); const auto Bytes = QByteArray::fromBase64(Volume["data"].toString().toLatin1());
        if (QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Volume["sha256"].toString().toLatin1()) return {};
        auto Suffix = QFileInfo(Volume["name"].toString()).suffix(); if (Upper) Suffix = Suffix.toUpper();
        if (!Put(Directory.filePath("MiX.Ed." + Suffix), Bytes)) return {};
    }
    return Directory.filePath(Upper ? "MiX.Ed.ZIPX" : "MiX.Ed.zipx");
}
}
class SevenZipTests : public QObject {
    Q_OBJECT
    QJsonArray cases_;
    QString root_;
private slots:
    void msiBrowse_data() {
        QTest::addColumn<bool>("External"); QTest::addColumn<bool>("Loose");
        QTest::newRow("embedded") << false << false;
        QTest::newRow("external") << true << false;
        QTest::newRow("loose") << false << true;
    }
    void msiBrowse() {
        QFETCH(bool, External); QFETCH(bool, Loose); QTemporaryDir Input, Output, Cache;
        const auto Path = Input.filePath("sample.msi"); QCOMPARE(msiFixture::Create(Path, External, Loose), UINT(ERROR_SUCCESS));
        if (Loose) { QVERIFY(QDir().mkpath(Input.filePath("source/plugins"))); QVERIFY(Put(Input.filePath("source/MyApplication.exe"), "application\n")); QVERIFY(Put(Input.filePath("source/plugins/image.dll"), "plugin\n")); }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listing = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY2(Listing.HasValue(), Listing.HasValue() ? "" : Listing.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listing.Value()->Next(1, {}); QVERIFY(Page.HasValue()); for (const auto& H : Page.Value().entries_) Headers.push_back(H); if (Page.Value().end_) break; }
        Listing.Value().reset();
        std::vector<uint32_t> Selected; size_t Raw = 0;
        for (uint32_t I = 0; I < Headers.size(); ++I) {
            const auto& H = Headers[I];
            if (H.virtualRoot_) { Raw = I; QCOMPARE(H.name_, "<Browse>"); QVERIFY2(H.virtualError_.empty(), H.virtualError_.c_str()); QVERIFY(H.directory_); }
            else if (!H.directory_ && H.name_.starts_with("<Browse>/")) Selected.push_back(I);
        }
        QVERIFY(Raw > 3); QCOMPARE(Selected.size(), size_t(2));
        QCOMPARE(Headers[Selected[0]].name_, "<Browse>/MyApplication/MyApplication.exe");
        QCOMPARE(Headers[Selected[1]].name_, "<Browse>/MyApplication/plugins/image.dll");
        auto Batch = Backend->ExtractSelected(Path.toStdString(), Output.path().toStdString(), Selected, {});
        QVERIFY2(Batch.HasValue(), Batch.HasValue() ? "" : Batch.Failure().code_.c_str());
        QCOMPARE(Read(Output.filePath(QString::number(Selected[0]) + ".bin")), QByteArray("application\n"));
        QCOMPARE(Read(Output.filePath(QString::number(Selected[1]) + ".bin")), QByteArray("plugin\n"));
        ArchiveSelectionDigest Digest; for (auto I : Selected) Digest.Add(I, Headers[I]); QCOMPARE(Read(Output.filePath("selection.sha256")), Digest.Result());
        auto Extracted = Backend->Extract(Path.toStdString(), Headers[Selected[1]], Output.filePath("single.dll").toStdString(), {});
        QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(Read(Output.filePath("single.dll")), QByteArray("plugin\n"));
        // The original Compound stream remains extractable under its original ordinal.
        if (!External && !Loose) {
            auto Cab = std::find_if(Headers.begin(), Headers.begin() + Raw, [](const auto& H) { return H.name_ == "payload.cab"; }); QVERIFY(Cab != Headers.begin() + Raw);
            QVERIFY(Backend->Extract(Path.toStdString(), *Cab, Output.filePath("raw.cab").toStdString(), {}).HasValue()); QCOMPARE(Read(Output.filePath("raw.cab")), msiFixture::Cabinet());
        }
        Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}};
        auto Provider = MakeWcxReadProvider({}, Root, Cache.path().toStdString(), Backend);
        auto RootList = Provider->Enumerate(Root, {}).get(); QVERIFY(RootList.HasValue()); auto RootPage = RootList.Value()->Next(128, {}).get(); QVERIFY(RootPage.HasValue());
        auto Browse = std::find_if(RootPage.Value().entries_.begin(), RootPage.Value().entries_.end(), [](const auto& E) { return E.hints_.contains("virtualArchiveRoot"); }); QVERIFY(Browse != RootPage.Value().entries_.end());
        QCOMPARE(Browse->name_, "Browse"); QCOMPARE(Browse->hints_.at("displayName"), "<Browse>");
        QCOMPARE(Browse->locator_.nesting_.back().entry_, "/" + Browse->name_);
        auto Logical = Provider->Enumerate(Browse->locator_, {}).get(); QVERIFY(Logical.HasValue()); auto Page = Logical.Value()->Next(128, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1)); QCOMPARE(Page.Value().entries_[0].name_, "MyApplication");
        auto At = Root; At.nesting_.back().entry_ = "/Browse/MyApplication/plugins/image.dll";
        auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        auto Bytes = Stream.Value()->Read(128, {}).get(); QVERIFY(Bytes.HasValue()); QCOMPARE(QByteArray(reinterpret_cast<const char*>(Bytes.Value().bytes_.data()), Bytes.Value().bytes_.size()), QByteArray("plugin\n")); Stream.Value().reset();
        // Selecting Browse has the same extraction coordinator semantics as an archive.
        for (bool Separate : {false, true}) {
            QTemporaryDir Destination; hc::operations::ExtractRequest Request; Request.archives_ = {{*Browse, Provider, {}}}; Request.sourceRoot_ = Root;
            Request.destination_ = {LocatorKind::Local, Destination.path().toStdString()}; Request.writer_ = MakeNativeWritableStorage(LocatorKind::Local, MakeNativeReadProvider(LocatorKind::Local));
            Request.separateFolders_ = Separate; Request.readers_ = [Provider](const auto&) { return std::vector<std::shared_ptr<IStorageProvider>>{Provider}; };
            auto Prepared = hc::operations::PrepareExtract(std::move(Request)); QVERIFY(Prepared.HasValue());
            hc::operations::JobStepContext Context; hc::operations::JobState State = hc::operations::JobState::Preparing;
            for (int I = 0; I < 10000; ++I) {
                auto Step = Prepared.Value().task_->Advance(Context); State = Step.next_;
                QVERIFY2(State != hc::operations::JobState::Failed, Step.error_ ? Step.error_->code_.c_str() : "extract failed");
                QVERIFY(State != hc::operations::JobState::WaitingForUser);
                Context.committing_ = State == hc::operations::JobState::Committing;
                if (State == hc::operations::JobState::Completed) break;
                QTest::qWait(1); // Allow the asynchronous provider worker to finish.
            }
            QCOMPARE(State, hc::operations::JobState::Completed); QVERIFY(Prepared.Value().task_->Finalize(State).empty());
            const auto Prefix = Separate ? "Browse/" : "";
            QCOMPARE(Read(Destination.filePath(QString(Prefix) + "MyApplication/plugins/image.dll")), QByteArray("plugin\n"));
            QVERIFY(QFileInfo(Destination.filePath(QString(Prefix) + "MyApplication/empty")).isDir());
            QVERIFY(!QFileInfo::exists(Destination.filePath("<Browse>")));
        }
        if (External) {
            QVERIFY(QFile::remove(Input.filePath("payload.cab")));
            QVERIFY(Backend->Enumerate(Path.toStdString(), false, {}).HasValue());
            auto Missing = Backend->Extract(Path.toStdString(), Headers[Selected[0]], Output.filePath("missing").toStdString(), {}); QVERIFY(!Missing.HasValue()); QVERIFY(Missing.Failure().code_.starts_with("msi-source-missing:"));
        }
    }
    void msiInvalidLogicalTree() {
        for (bool Cycle : {false, true}) {
            QTemporaryDir Input; const auto Path = Input.filePath("invalid.msi"); QCOMPARE(msiFixture::Create(Path, false, false, !Cycle, Cycle), UINT(ERROR_SUCCESS));
            auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Listing = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Listing.HasValue());
            bool Marker = false; for (;;) { auto Page = Listing.Value()->Next(128, {}); QVERIFY(Page.HasValue()); for (const auto& H : Page.Value().entries_) if (H.virtualRoot_) { Marker = true; QVERIFY(!H.virtualError_.empty()); } if (Page.Value().end_) break; } QVERIFY(Marker);
            Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}}; auto Provider = MakeWcxReadProvider({}, Root, {}, Backend); QVERIFY(Provider->Enumerate(Root, {}).get().HasValue());
            Root.nesting_.back().entry_ = "/Browse"; auto Failed = Provider->Enumerate(Root, {}).get(); QVERIFY(!Failed.HasValue()); QVERIFY(Failed.Failure().code_.starts_with("msi-"));
        }
    }

    void splitVolumesRead_data() {
        QTest::addColumn<QJsonObject>("Case"); QTest::addColumn<QString>("Name"); QTest::addColumn<bool>("Upper"); QTest::addColumn<bool>("Padded");
        const auto Cases = SplitFixtures(); QCOMPARE(Cases.size(), 6);
        for (const auto& Value : Cases) {
            const auto Case = Value.toObject();
            for (bool Upper : {false, true}) for (bool Padded : {false, true}) {
                if (Padded && Case["name"].toString() != "rar5" && Case["name"].toString() != "seven") continue;
                for (const auto& Part : Case["volumes"].toArray()) {
                    const auto Name = SplitName(Part.toObject()["name"].toString(), Upper, Padded);
                    QTest::newRow(qPrintable(Case["name"].toString() + '-' + Name)) << Case << Name << Upper << Padded;
                }
            }
        }
    }
    void splitVolumesRead() {
        QFETCH(QJsonObject, Case); QFETCH(QString, Name); QFETCH(bool, Upper); QFETCH(bool, Padded);
        QTemporaryDir Directory, Individual, Batch; QVERIFY(WriteSplitFixture(Case, Directory, Upper, Padded));
        const auto Path = Upper ? QDir::toNativeSeparators(Directory.filePath(Name)) : Directory.filePath(Name);
        const auto Before = QDir(Directory.path()).entryList(QDir::Files);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Context = PasswordContext(Case["password"].toString());
        auto Listed = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(2, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            for (const auto& Header : Page.Value().entries_) { QVERIFY(!Header.metadataRename_); Headers.push_back(Header); }
            if (Page.Value().end_) break;
        }
        Listed.Value().reset(); const auto Expected = ZipxFixtures()["files"].toObject(); std::vector<std::uint32_t> Ordinals;
        for (std::uint32_t I = 0; I < Headers.size(); ++I) {
            const auto& Header = Headers[I]; if (Header.directory_) continue; Ordinals.push_back(I);
            const auto Key = QString::fromStdString(Header.name_); QVERIFY2(Expected.contains(Key), qPrintable(Key));
            const auto To = Individual.filePath(QString::number(I)); auto Done = Backend->Extract(Path.toStdString(), Header, To.toStdString(), Context);
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(QCryptographicHash::hash(Read(To), QCryptographicHash::Sha256).toHex(), Expected[Key].toObject()["sha256"].toString().toLatin1());
        }
        QCOMPARE(qsizetype(Ordinals.size()), Expected.size());
        auto All = Backend->ExtractAll(Path.toStdString(), Batch.path().toStdString(), Context); QVERIFY2(All.HasValue(), All.HasValue() ? "" : All.Failure().code_.c_str());
        auto Stream = Backend->OpenReadSession(Path.toStdString(), Ordinals, Context); QVERIFY(Stream.HasValue());
        for (const auto I : Ordinals) {
            const auto ExpectedBytes = Read(Individual.filePath(QString::number(I))); QCOMPARE(Read(Batch.filePath(QString::number(I) + ".bin")), ExpectedBytes);
            QByteArray Bytes; bool Eof = false;
            while (!Eof) { auto Chunk = Stream.Value()->Read(I, 513, Context); QVERIFY2(Chunk.HasValue(), Chunk.HasValue() ? "" : Chunk.Failure().code_.c_str());
                QVERIFY(Chunk.Value().eof_ || !Chunk.Value().bytes_.empty());
                Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), qsizetype(Chunk.Value().bytes_.size())); Eof = Chunk.Value().eof_;
            }
            QCOMPARE(Bytes, ExpectedBytes);
        }
        Stream.Value().reset(); auto Change = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {}, {"payload.bin"}}, Context);
        QVERIFY(!Change.HasValue()); QCOMPARE(Change.Failure().category_, ErrorCategory::Unsupported);
        QCOMPARE(QDir(Directory.path()).entryList(QDir::Files), Before);
        for (const auto& Part : Case["volumes"].toArray()) {
            const auto Volume = Part.toObject();
            QCOMPARE(QCryptographicHash::hash(Read(Directory.filePath(SplitName(Volume["name"].toString(), Upper, Padded))), QCryptographicHash::Sha256).toHex(), Volume["sha256"].toString().toLatin1());
        }
    }
    void splitVolumesMissingAnchor_data() {
        QTest::addColumn<QJsonObject>("Case");
        for (const auto& Value : SplitFixtures()) { const auto Case = Value.toObject(); QTest::newRow(qPrintable(Case["name"].toString())) << Case; }
    }
    void splitVolumesMissingAnchor() {
        QFETCH(QJsonObject, Case); QTemporaryDir Directory; QVERIFY(WriteSplitFixture(Case, Directory));
        const auto Parts = Case["volumes"].toArray(); const auto Name = Parts[Parts.size() / 2].toObject()["name"].toString();
        const auto First = Case["first"].toString(); QVERIFY(Name != First); QVERIFY(QFile::remove(Directory.filePath(First)));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Listed = Backend->Enumerate(Directory.filePath(Name).toStdString(), false, PasswordContext(Case["password"].toString()));
        QVERIFY(!Listed.HasValue()); QCOMPARE(Listed.Failure().category_, ErrorCategory::NotFound);
        QVERIFY2(QString::fromStdString(Listed.Failure().code_).contains(First), Listed.Failure().code_.c_str());
    }
    void splitVolumeUnrelatedAndAmbiguous() {
        QTemporaryDir Directory; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        const auto PlainZip = QByteArray::fromBase64(ZipxFixtures()["cases"].toArray()[0].toObject()["volumes"].toArray()[0].toObject()["data"].toString().toLatin1());
        QVERIFY(Put(Directory.filePath("book.zip"), PlainZip)); QVERIFY(Put(Directory.filePath("book.z03"), "unrelated file"));
        auto Listed = Backend->Enumerate(Directory.filePath("book.z03").toStdString(), false, {});
        QVERIFY(!Listed.HasValue()); QCOMPARE(Listed.Failure().code_, "7zip-volume-not-in-set:book.z03");
        QVERIFY(Put(Directory.filePath("book.rar"), "another candidate"));
        Listed = Backend->Enumerate(Directory.filePath("book.z03").toStdString(), false, {});
        QVERIFY(!Listed.HasValue()); QCOMPARE(Listed.Failure().code_, "7zip-ambiguous-volume-set");
        QVERIFY(Put(Directory.filePath("plain.001"), PlainZip)); QVERIFY(Put(Directory.filePath("plain.002"), "unrelated file"));
        Listed = Backend->Enumerate(Directory.filePath("plain.002").toStdString(), false, {}); QVERIFY(!Listed.HasValue());
    }
    void splitVolumesMissingData_data() { splitVolumesMissingAnchor_data(); }
    void splitVolumesMissingData() {
        QFETCH(QJsonObject, Case); QTemporaryDir Directory; QVERIFY(WriteSplitFixture(Case, Directory));
        const auto Parts = Case["volumes"].toArray(); const auto First = Case["first"].toString();
        const auto Missing = Parts[Parts.size() / 2].toObject()["name"].toString(); QVERIFY(Missing != First);
        QVERIFY(QFile::remove(Directory.filePath(Missing))); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        const auto Context = PasswordContext(Case["password"].toString()); auto Listed = Backend->Enumerate(Directory.filePath(First).toStdString(), true, Context);
        if (!Listed.HasValue()) return;
        for (;;) { auto Page = Listed.Value()->Next(16, Context); if (!Page.HasValue()) return; QVERIFY(!Page.Value().end_); }
    }
    void numberedVolumeWidthBoundary() {
        const auto Case = SplitFixtures()[1].toObject(); QByteArray Body;
        for (const auto& Value : Case["volumes"].toArray()) Body += QByteArray::fromBase64(Value.toObject()["data"].toString().toLatin1());
        QVERIFY(Body.size() > 1000); QTemporaryDir Directory;
        for (qsizetype I = 0; I < Body.size(); ++I) QVERIFY(Put(Directory.filePath("wide.7z." + QString::number(I + 1).rightJustified(3, '0')), Body.mid(I, 1)));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Path = Directory.filePath("wide.7z.1000");
        auto Listed = Backend->Enumerate(Path.toStdString(), true, {}); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
        QVERIFY(Page.Value().end_); QCOMPARE(Page.Value().entries_.size(), 4u); Listed.Value().reset();
        QVERIFY(Put(Directory.filePath("wide.7z.0001"), Body));
        Listed = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(!Listed.HasValue()); QCOMPARE(Listed.Failure().code_, "7zip-ambiguous-volume-set");
    }
    void zipxRead_data() {
        QTest::addColumn<QJsonObject>("Case"); QTest::addColumn<bool>("Upper");
        const auto Cases = ZipxFixtures()["cases"].toArray(); QCOMPARE(Cases.size(), 4);
        for (const auto& Value : Cases) for (bool Upper : {false, true}) {
            const auto Case = Value.toObject(); const auto Tag = Case["name"].toString() + (Upper ? "-upper" : "");
            QTest::newRow(qPrintable(Tag)) << Case << Upper;
        }
    }
    void zipxRead() {
        QFETCH(QJsonObject, Case); QFETCH(bool, Upper); QTemporaryDir Directory;
        const auto Path = WriteZipxFixture(Case, Directory, Upper); QVERIFY(!Path.isEmpty());
        QMap<QString, QByteArray> Before;
        for (const auto& Name : QDir(Directory.path()).entryList(QDir::Files)) Before[Name] = Read(Directory.filePath(Name));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Context = PasswordContext(Case["password"].toString());
        auto Listing = Backend->Enumerate(Path.toStdString(), false, Context); QVERIFY2(Listing.HasValue(), Listing.HasValue() ? "" : Listing.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listing.Value()->Next(2, Context); QVERIFY(Page.HasValue());
            for (const auto& Header : Page.Value().entries_) { QVERIFY(!Header.metadataRename_); Headers.push_back(Header); }
            if (Page.Value().end_) break;
        }
        Listing.Value().reset(); const auto Expected = ZipxFixtures()["files"].toObject();
        std::vector<std::uint32_t> Ordinals; QTemporaryDir Individual, Batch;
        for (std::uint32_t I = 0; I < Headers.size(); ++I) {
            const auto& Header = Headers[I]; if (Header.directory_) continue; Ordinals.push_back(I);
            const auto Name = QString::fromStdString(Header.name_); QVERIFY(Expected.contains(Name));
            QCOMPARE(Header.size_, std::uint64_t(Expected[Name].toObject()["size"].toInteger()));
            const auto To = Individual.filePath(QString::number(I)); auto Done = Backend->Extract(Path.toStdString(), Header, To.toStdString(), Context);
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(QCryptographicHash::hash(Read(To), QCryptographicHash::Sha256).toHex(), Expected[Name].toObject()["sha256"].toString().toLatin1());
        }
        QCOMPARE(qsizetype(Ordinals.size()), Expected.size());
        auto All = Backend->ExtractAll(Path.toStdString(), Batch.path().toStdString(), Context);
        QVERIFY2(All.HasValue(), All.HasValue() ? "" : All.Failure().code_.c_str());
        for (const auto I : Ordinals) QCOMPARE(Read(Batch.filePath(QString::number(I) + ".bin")), Read(Individual.filePath(QString::number(I))));
        auto Stream = Backend->OpenReadSession(Path.toStdString(), Ordinals, Context); QVERIFY(Stream.HasValue());
        for (const auto I : Ordinals) {
            QByteArray Bytes; bool Eof = false;
            while (!Eof) { auto Chunk = Stream.Value()->Read(I, 257, Context); QVERIFY2(Chunk.HasValue(), Chunk.HasValue() ? "" : Chunk.Failure().code_.c_str());
                QVERIFY(Chunk.Value().eof_ || !Chunk.Value().bytes_.empty());
                Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), qsizetype(Chunk.Value().bytes_.size())); Eof = Chunk.Value().eof_;
            }
            QCOMPARE(Bytes, Read(Individual.filePath(QString::number(I))));
        }
        Stream.Value().reset(); auto Tested = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY(Tested.HasValue());
        for (;;) { auto Page = Tested.Value()->Next(2, Context); QVERIFY(Page.HasValue()); if (Page.Value().end_) break; }
        Tested.Value().reset();
        QVERIFY(Put(Directory.filePath("added"), "new data"));
        auto Changed = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {"added"}}, Context);
        QVERIFY(!Changed.HasValue()); QCOMPARE(Changed.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Changed.Failure().code_, "7zip-format-read-only");
        Changed = Backend->Update({Path.toStdString(), Directory.path().toStdString(), {}, {"payload.bin"}}, Context);
        QVERIFY(!Changed.HasValue()); QCOMPARE(Changed.Failure().category_, ErrorCategory::Unsupported);
        for (auto I = Before.cbegin(); I != Before.cend(); ++I) QCOMPARE(Read(Directory.filePath(I.key())), I.value());
        std::stop_source Stop; Stop.request_stop(); OperationContext Cancelled; Cancelled.stopToken_ = Stop.get_token();
        auto Stopped = Backend->Enumerate(Path.toStdString(), false, Cancelled); QVERIFY(!Stopped.HasValue()); QCOMPARE(Stopped.Failure().category_, ErrorCategory::Cancelled);
        if (!Case["password"].toString().isEmpty()) {
            const auto Header = *std::find_if(Headers.begin(), Headers.end(), [](const auto& H) { return H.name_ == "payload.bin"; });
            auto Wrong = Backend->Extract(Path.toStdString(), Header, Individual.filePath("wrong").toStdString(), PasswordContext("wrong"));
            QVERIFY(!Wrong.HasValue()); QCOMPARE(Wrong.Failure().category_, ErrorCategory::AccessDenied);
        }
    }
    void zipxMissingVolumes_data() {
        QTest::addColumn<QString>("Suffix"); QTest::newRow("first") << QString("zx01"); QTest::newRow("middle") << QString("zx03");
    }
    void zipxMissingVolumes() {
        QFETCH(QString, Suffix); QTemporaryDir Directory;
        const auto Path = WriteZipxFixture(ZipxFixtures()["cases"].toArray()[2].toObject(), Directory); QVERIFY(!Path.isEmpty());
        const auto Missing = Directory.filePath("MiX.Ed." + Suffix); const auto Bytes = Read(Missing);
        // A ZIP with the same stem must never supply a missing ZIPX volume.
        QVERIFY(Put(Directory.filePath("MiX.Ed.z" + Suffix.mid(2)), Bytes));
        QVERIFY(QFile::remove(Missing)); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listing = Backend->Enumerate(Path.toStdString(), true, {});
        QVERIFY(!Listing.HasValue()); QCOMPARE(Listing.Failure().category_, ErrorCategory::NotFound);
        QVERIFY(QString::fromStdString(Listing.Failure().code_).contains(Suffix));
    }
    void zipxUnsupportedAndCorrupt() {
        QTemporaryDir Directory; const auto Path = WriteZipxFixture(ZipxFixtures()["cases"].toArray()[0].toObject(), Directory); QVERIFY(!Path.isEmpty());
        const auto Original = Read(Path); auto Bytes = Original; int DataStart = -1;
        // Preserve a real Bandizip layout while substituting an absent JPEG decoder.
        for (const auto& Signature : {QByteArray("PK\x03\x04", 4), QByteArray("PK\x01\x02", 4)}) {
            const bool Central = Signature[2] == '\x01'; const int HeaderSize = Central ? 46 : 30;
            for (qsizetype At = Bytes.indexOf(Signature); At >= 0; At = Bytes.indexOf(Signature, At + HeaderSize)) {
                const auto NameSize = qFromLittleEndian<quint16>(Bytes.constData() + At + (Central ? 28 : 26));
                if (Bytes.mid(At + HeaderSize, NameSize) != "payload.bin") continue;
                qToLittleEndian<quint16>(96, Bytes.data() + At + (Central ? 10 : 8));
                if (!Central) DataStart = int(At) + HeaderSize + NameSize + qFromLittleEndian<quint16>(Bytes.constData() + At + 28);
            }
        }
        QVERIFY(DataStart >= 0); QVERIFY(Put(Path, Bytes)); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listing = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Listing.HasValue()); auto Page = Listing.Value()->Next(16, {}); QVERIFY(Page.HasValue());
        std::uint32_t Ordinal = 0; bool Found = false;
        for (; Ordinal < Page.Value().entries_.size(); ++Ordinal) if (Page.Value().entries_[Ordinal].name_ == "payload.bin") { Found = true; break; }
        QVERIFY(Found); const auto Header = Page.Value().entries_[Ordinal]; Listing.Value().reset(); QTemporaryDir Output, Batch;
        auto One = Backend->Extract(Path.toStdString(), Header, Output.filePath("one").toStdString(), {});
        QVERIFY(!One.HasValue()); QCOMPARE(One.Failure().category_, ErrorCategory::Unsupported);
        auto All = Backend->ExtractAll(Path.toStdString(), Batch.path().toStdString(), {}); QVERIFY(!All.HasValue()); QCOMPARE(All.Failure().category_, ErrorCategory::Unsupported);
        auto Stream = Backend->OpenReadSession(Path.toStdString(), {Ordinal}, {}); QVERIFY(Stream.HasValue()); auto Chunk = Stream.Value()->Read(Ordinal, 4096, {});
        QVERIFY(!Chunk.HasValue()); QCOMPARE(Chunk.Failure().category_, ErrorCategory::Unsupported); Stream.Value().reset();
        Bytes = Original; Bytes[DataStart + 32] = char(uchar(Bytes[DataStart + 32]) ^ 0x80); QVERIFY(Put(Path, Bytes));
        auto Test = Backend->Enumerate(Path.toStdString(), true, {}); QVERIFY(Test.HasValue()); auto Bad = Test.Value()->Next(16, {}); QVERIFY(!Bad.HasValue()); Test.Value().reset();
        QVERIFY(Put(Path, "This is not a ZIPX")); auto Fake = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(!Fake.HasValue());
        const auto New = Directory.filePath("new.zipx"); QVERIFY(Put(Directory.filePath("added"), "new data"));
        auto Create = Backend->Update({New.toStdString(), Directory.path().toStdString(), {"added"}}, {});
        QVERIFY(!Create.HasValue()); QCOMPARE(Create.Failure().category_, ErrorCategory::Unsupported); QVERIFY(!QFileInfo::exists(New));
    }
    void advancedCreation_data()
    {
        QTest::addColumn<QString>("Format"); QTest::addColumn<int>("Method"); QTest::addColumn<bool>("V2");
        for (const auto Format : {QString("7z"), QString("zip")}) for (int Method = 1; Method <= 7; ++Method) {
            if (Format == "zip" && Method == int(SevenZipMethod::Lzma2)) continue;
            for (bool V2 : {false, true}) QTest::newRow(qPrintable(Format + '-' + QString::number(Method) + (V2 ? "-v2" : "-v1"))) << Format << Method << V2;
        }
    }
    void advancedCreation()
    {
        QFETCH(QString, Format); QFETCH(int, Method); QFETCH(bool, V2);
        QTemporaryDir Dir; const QByteArray Payload(256 * 1024, 'x'); QVERIFY(Put(Dir.filePath("input"), Payload));
        SevenZipCreateSettings Settings; Settings.method_ = SevenZipMethod(Method); Settings.solid_ = ArchiveSolidMode::Off;
        if (Method == int(SevenZipMethod::Lzma) || Method == int(SevenZipMethod::Lzma2)) { Settings.dictionaryBytes_ = 1u << 20; Settings.wordSize_ = 32; }
        if (Method == int(SevenZipMethod::Ppmd)) { Settings.dictionaryBytes_ = 1u << 20; Settings.wordSize_ = 6; }
        if (Method == int(SevenZipMethod::Bzip2)) Settings.dictionaryBytes_ = 100 * 1024;
        if (Method == int(SevenZipMethod::Deflate) || Method == int(SevenZipMethod::Deflate64)) Settings.wordSize_ = 32;
        Settings.threads_ = SevenZipThreadLimit(Format.toStdString(), Settings.method_) > 1 ? 2 : 1;
        ArchiveUpdate Update{Dir.filePath("result." + Format).toStdString(), Dir.path().toStdString(), {"input"}};
        Update.creation_ = ArchiveCreationOptions{5, false, Settings};
        auto Decoded = DecodeArchiveUpdate(EncodeArchiveUpdate(Update)); QVERIFY(Decoded.HasValue()); QCOMPARE(*Decoded.Value().creation_->advanced_, Settings);
        auto Options = BuiltinSevenZipOptions(); Options.limits_.enableSessionV2_ = V2;
        auto Backend = MakeApprovedWcxBackend(Options); auto Written = Backend->Update(Update, {});
        QVERIFY2(Written.HasValue(), Written.HasValue() ? "" : Written.Failure().code_.c_str());
        auto List = Backend->Enumerate(Update.archive_, true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(4, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), 1u);
        QVERIFY(Backend->Extract(Update.archive_, Page.Value().entries_.front(), Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), Payload);
        QProcess Official; Official.start(QDir(root_).filePath("../sevenzip-tools/official/7z.exe"), {"l", "-slt", QString::fromStdString(Update.archive_)});
        QVERIFY(Official.waitForFinished(15000)); QCOMPARE(Official.exitCode(), 0);
        const QByteArray Names[]{"", "Copy", "LZMA", "LZMA2", "PPMd", "BZip2", "Deflate", "Deflate64"};
        QVERIFY2(Official.readAllStandardOutput().toLower().contains(Names[Method].toLower()), Names[Method].constData());
    }
    void advancedWireRejectsUnknownOrLossyOptions()
    {
        SevenZipCreateSettings Settings; Settings.dictionaryBytes_ = (15ull << 28); Settings.threads_ = 512;
        auto Encoded = EncodeSevenZipSettings(Settings); auto Decoded = DecodeSevenZipSettings(Encoded); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value(), Settings);
        Encoded["version"] = 2; QVERIFY(!DecodeSevenZipSettings(Encoded).HasValue());
        Encoded = EncodeSevenZipSettings(Settings); Encoded["dictionary"] = double(1ull << 53); QVERIFY(!DecodeSevenZipSettings(Encoded).HasValue());
        Settings.method_ = SevenZipMethod::Ppmd; Settings.threads_ = 2; QVERIFY(!ValidateSevenZipSettings("7z", Settings).HasValue());
        Settings.method_ = SevenZipMethod::Lzma2; Settings.threads_ = 512; QVERIFY(ValidateSevenZipSettings("7z", Settings).HasValue());
        QVERIFY(!ValidateSevenZipSettings("zip", Settings).HasValue());
    }
    void streamingPasswordContext() {
        QTemporaryDir Input;QVERIFY(Put(Input.filePath("secret.bin"),"streamed secret"));const auto Archive=Input.filePath("encrypted.7z");
        auto Backend=MakeApprovedWcxBackend(BuiltinSevenZipOptions());ArchiveUpdate Create{Archive.toStdString(),Input.path().toStdString(),{"secret.bin"}};Create.encrypt_=true;
        auto Context=PasswordContext("secret");QVERIFY(Backend->Update(Create,Context).HasValue());auto Stream=Backend->OpenReadSession(Archive.toStdString(),{0},Context);QVERIFY(Stream.HasValue());
        auto Chunk=Stream.Value()->Read(0,1024,{});QVERIFY2(Chunk.HasValue(),Chunk.HasValue()?"":Chunk.Failure().code_.c_str());QVERIFY(Chunk.Value().eof_);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),Chunk.Value().bytes_.size()),QByteArray("streamed secret"));
    }
    void forwardStreamsSelectionCancellationAndCrc() {
        QTemporaryDir Input; QVERIFY(Input.isValid()); const auto Archive=Input.filePath("streams.7z");
        auto Backend=MakeApprovedWcxBackend(BuiltinSevenZipOptions()); std::vector<std::string> Names;
        for(int I=0;I<12;++I) { auto Name=QString("member-%1.bin").arg(I,2,10,QChar('0')); QVERIFY(Put(Input.filePath(Name),QByteArray(65536,char('a'+I)))); Names.push_back(Name.toStdString()); }
        ArchiveUpdate Create{Archive.toStdString(),Input.path().toStdString(),Names}; Create.creation_=ArchiveCreationOptions{5,true}; QVERIFY(Backend->Update(Create,{}).HasValue());
        auto Listing=Backend->Enumerate(Archive.toStdString(),false,{}); QVERIFY(Listing.HasValue()); std::vector<ArchiveHeader> Headers;
        for(;;) { auto Page=Listing.Value()->Next(32,{}); QVERIFY(Page.HasValue()); for(auto& H:Page.Value().entries_) Headers.push_back(H); if(Page.Value().end_)break; }
        QCOMPARE(Headers.size(),std::size_t(12));
        const auto Before=QDir(Input.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden);
        auto Stream=Backend->OpenReadSession(Archive.toStdString(),{0,5,11},{}); QVERIFY(Stream.HasValue());
        for(auto Ordinal:{0u,5u,11u}) { QByteArray Bytes; bool Eof=false;
            while(!Eof) { auto Chunk=Stream.Value()->Read(Ordinal,1000,{}); QVERIFY2(Chunk.HasValue(),Chunk.HasValue()?"":Chunk.Failure().code_.c_str()); Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),Chunk.Value().bytes_.size()); Eof=Chunk.Value().eof_; }
            QCOMPARE(Bytes,Read(Input.filePath(QString::fromStdString(Headers[Ordinal].name_))));
        }
        Stream.Value().reset(); QCOMPARE(QDir(Input.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),Before);
        Stream=Backend->OpenReadSession(Archive.toStdString(),{0,5,11},{}); QVERIFY(Stream.HasValue());
        QByteArray Last;bool Eof=false;while(!Eof) { auto Chunk=Stream.Value()->Read(11,8192,{});QVERIFY(Chunk.HasValue());Last.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),Chunk.Value().bytes_.size());Eof=Chunk.Value().eof_; }
        QCOMPARE(Last,Read(Input.filePath(QString::fromStdString(Headers[11].name_)))); Stream.Value().reset();
        Stream=Backend->OpenReadSession(Archive.toStdString(),{0,5,11},{});QVERIFY(Stream.HasValue());QVERIFY(Stream.Value()->Read(0,1,{}).HasValue());
        std::stop_source Stop;OperationContext Cancel;Cancel.stopToken_=Stop.get_token();Stop.request_stop();auto Cancelled=Stream.Value()->Read(0,1,Cancel);QVERIFY(!Cancelled.HasValue());QCOMPARE(Cancelled.Failure().category_,ErrorCategory::Cancelled);
        QElapsedTimer Clock;Clock.start();Stream.Value().reset();QVERIFY(Clock.elapsed()<3000);
        Locator Root{LocatorKind::Archive,Archive.toStdString(),{{"wcx","/"}}};auto Provider=MakeWcxReadProvider({},Root,Input.path().toStdString(),Backend);
        auto Location=Root;Location.nesting_.back().entry_="/"+Headers[0].name_;auto Entry=Provider->Resolve(Location,{}).get();QVERIFY(Entry.HasValue());
        for(int Round=0;Round<2;++Round) {
            QVERIFY(Provider->PrepareReadBatch({Entry.Value()},{}).get().HasValue());OperationContext Batch;Batch.orderedBatchRead_=true;
            auto File=Provider->OpenRead(Entry.Value().id_,Batch).get();QVERIFY(File.HasValue());QByteArray Content;bool Finished=false;
            while(!Finished){auto Chunk=File.Value()->Read(8192,Batch).get();QVERIFY(Chunk.HasValue());Content.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),Chunk.Value().bytes_.size());Finished=Chunk.Value().eof_;}
            QCOMPARE(Content,Read(Input.filePath(QString::fromStdString(Headers[0].name_))));File.Value().reset();QVERIFY(Provider->FinishReadBatch({}).get().HasValue());
        }
        auto Independent=Provider->OpenRead(Entry.Value().id_,{}).get();QVERIFY(Independent.HasValue());auto First=Independent.Value()->Read(1,{}).get();QVERIFY(First.HasValue());QVERIFY(Independent.Value()->CanSeek());QVERIFY(Independent.Value()->Seek(100,{}).get().HasValue());QVERIFY(Independent.Value()->Seek(0,{}).get().HasValue());
        auto Again=Independent.Value()->Read(1,{}).get();QVERIFY(Again.HasValue());QCOMPARE(Again.Value().bytes_,First.Value().bytes_);Independent.Value().reset();
        QCOMPARE(QDir(Input.path()).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden),Before);
        const auto Copy=Input.filePath("crc.7z");QVERIFY(Put(Input.filePath("crc.bin"),"STREAM-CRC-PAYLOAD-DO-NOT-CHANGE"));
        ArchiveUpdate Raw{Copy.toStdString(),Input.path().toStdString(),{"crc.bin"}};Raw.creation_=ArchiveCreationOptions{0,false};QVERIFY(Backend->Update(Raw,{}).HasValue());
        auto Damaged=Read(Copy);const auto At=Damaged.indexOf("STREAM-CRC-PAYLOAD");QVERIFY(At>=0);Damaged[At]^=1;QVERIFY(Put(Copy,Damaged));
        Stream=Backend->OpenReadSession(Copy.toStdString(),{0},{});QVERIFY(Stream.HasValue());auto Broken=Stream.Value()->Read(0,1024,{});QVERIFY(!Broken.HasValue());Stream.Value().reset();
    }
    void selectedDirectPause_data()
    {
        QTest::addColumn<bool>("Cancel"); QTest::newRow("resume") << false; QTest::newRow("cancel") << true;
    }
    void selectedDirectPause()
    {
        QFETCH(bool, Cancel); QTemporaryDir Input, Output; QVERIFY(Input.isValid() && Output.isValid());
        auto Options = BuiltinSevenZipOptions(); Options.limits_.noProgressMs_ = 1000; Options.limits_.requestMs_ = 3000;
        auto Backend = MakeApprovedWcxBackend(Options);
        for (int I = 0; I < 3; ++I) QVERIFY(Put(Input.filePath(QString("member-%1").arg(I)), QByteArray(8192, char('a' + I))));
        const auto Archive = Input.filePath("paused.7z");
        ArchiveUpdate Create{Archive.toStdString(), Input.path().toStdString(), {"member-0", "member-1", "member-2"}};
        Create.creation_ = ArchiveCreationOptions{5, true}; QVERIFY(Backend->Update(std::move(Create), {}).HasValue());
        std::atomic<bool> Paused = true; std::stop_source Stop; OperationContext Context;
        Context.pauseRequested_ = [&] { return Paused.load(); }; Context.stopToken_ = Stop.get_token();
        auto Future = std::async(std::launch::async, [&] { return Backend->ExtractSelected(Archive.toStdString(), Output.path().toStdString(), {0, 2}, Context); });
        const auto Unblock = qScopeGuard([&] { Paused = false; Stop.request_stop(); });
        QTest::qWait(1500);
        QVERIFY(Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        QVERIFY(QDir(Output.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty());
        if (Cancel) Stop.request_stop(); else Paused = false;
        QVERIFY(Future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        auto Done = Future.get();
        if (Cancel) { QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Cancelled); }
        else {
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(Read(Output.filePath("0.bin")), QByteArray(8192, 'a'));
            QCOMPARE(Read(Output.filePath("2.bin")), QByteArray(8192, 'c'));
        }
    }
    void selectedDirectExtraction()
    {
        QTemporaryDir Input, Output; QVERIFY(Input.isValid() && Output.isValid());
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        for (int I = 0; I < 4; ++I) QVERIFY(Put(Input.filePath(QString("member-%1").arg(I)), QByteArray(65536, char('a' + I))));
        const auto Archive = Input.filePath("selected.7z");
        ArchiveUpdate Create{Archive.toStdString(), Input.path().toStdString(), {"member-0", "member-1", "member-2", "member-3"}};
        Create.creation_ = ArchiveCreationOptions{5, true}; QVERIFY(Backend->Update(std::move(Create), {}).HasValue());
        auto Done = Backend->ExtractSelected(Archive.toStdString(), Output.path().toStdString(), {1, 3}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QCOMPARE(QDir(Output.path()).entryList(QDir::Files), (QStringList{"1.bin", "3.bin", "selection.sha256"}));
        QCOMPARE(Read(Output.filePath("1.bin")), QByteArray(65536, 'b'));
        QCOMPARE(Read(Output.filePath("3.bin")), QByteArray(65536, 'd'));
        QTemporaryDir Invalid;
        QVERIFY(!Backend->ExtractSelected(Archive.toStdString(), Invalid.path().toStdString(), {3, 1}, {}).HasValue());
        QVERIFY(!Backend->ExtractSelected(Archive.toStdString(), Invalid.path().toStdString(), {1, 1}, {}).HasValue());
        QVERIFY(!Backend->ExtractSelected(Archive.toStdString(), Invalid.path().toStdString(), {99}, {}).HasValue());
        QVERIFY(QDir(Invalid.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
    void bulkDecodeAndPrivateOutputGuards()
    {
        QTemporaryDir Input, OutputDirectory, AutomaticDirectory;QVERIFY(Input.isValid());QVERIFY(OutputDirectory.isValid());
        auto Backend=MakeApprovedWcxBackend(BuiltinSevenZipOptions());std::vector<std::string> Names;
        for(int I=0;I<12;++I){const auto Name=QString("member-%1").arg(I);QVERIFY(Put(Input.filePath(Name),QByteArray(32768,char('a'+I))));Names.push_back(Name.toStdString());}
        const auto Archive=Input.filePath("solid.7z");ArchiveUpdate Create;Create.archive_=Archive.toStdString();Create.inputDirectory_=Input.path().toStdString();Create.add_=Names;Create.creation_=ArchiveCreationOptions{5,true};
        QVERIFY(Backend->Update(std::move(Create),{}).HasValue());
        auto Automatic=OperationContext{};Automatic.workload_.intent_=WorkloadIntent::Automatic;
        auto Rejected=Backend->ExtractAll(Archive.toStdString(),AutomaticDirectory.path().toStdString(),Automatic);QVERIFY(!Rejected.HasValue());QCOMPARE(Rejected.Failure().code_,"archive-batch-extract-unsupported");QVERIFY(QDir(AutomaticDirectory.path()).entryList(QDir::Files).isEmpty());
        auto Done=Backend->ExtractAll(Archive.toStdString(),OutputDirectory.path().toStdString(),{});QVERIFY2(Done.HasValue(),Done.HasValue()?"":Done.Failure().code_.c_str());
        auto Listing=Backend->Enumerate(Archive.toStdString(),false,{});QVERIFY(Listing.HasValue());std::uint64_t Ordinal=0;int Files=0;
        for(;;){auto Page=Listing.Value()->Next(3,{});QVERIFY(Page.HasValue());for(const auto& Header:Page.Value().entries_){if(!Header.directory_){const auto Number=QString::fromStdString(Header.name_).mid(7).toInt();QCOMPARE(Read(QDir(OutputDirectory.path()).filePath(QString::number(Ordinal)+".bin")),QByteArray(32768,char('a'+Number)));++Files;}++Ordinal;}if(Page.Value().end_)break;}
        QCOMPARE(Files,12);const auto Before=Read(QDir(OutputDirectory.path()).filePath("0.bin"));
        Rejected=Backend->ExtractAll(Archive.toStdString(),OutputDirectory.path().toStdString(),{});QVERIFY(!Rejected.HasValue());QCOMPARE(Rejected.Failure().code_,"7zip-batch-private-directory");QCOMPARE(Read(QDir(OutputDirectory.path()).filePath("0.bin")),Before);
    }
    void specialEntries_data() {
        QTest::addColumn<QString>("Format"); QTest::addColumn<QByteArray>("Fixture");
        QTest::newRow("zip") << QString("zip") << QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAEAAAAa2VlcHByZXNlcnZlZCBwYXlsb2FkUEsDBBQAAAAAAIMYIlALSPDLBAAAAAQAAAAEAAAAbGlua2tlZXBQSwMEFAAAAAAAgxgiUAAAAAAAAAAAAAAAAAQAAABmaWZvUEsBAhQDFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAQAAAAAAAAAAAAAAKSBAAAAAGtlZXBQSwECFAMUAAAAAACDGCJQC0jwywQAAAAEAAAABAAAAAAAAAAAAAAA/6EzAAAAbGlua1BLAQIUAxQAAAAAAIMYIlAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAACkEVkAAABmaWZvUEsFBgAAAAADAAMAlgAAAHsAAAAAAA==");
        QTest::newRow("7z") << QString("7z") << QByteArray::fromBase64("N3q8ryccAATOxlo3FQAAAAAAAAB6AAAAAAAAAKDGtnRwcmVzZXJ2ZWQgcGF5bG9hZGtlZXABBAYAAQkVAAcLAQABAQAMFQAIDQIJEQoBdeJnzwtI8MsAAAUDDgEgDwGAER8AawBlAGUAcAAAAGwAaQBuAGsAAABmAGkAZgBvAAAAFQ4BAACApIEAgP+hAICkERQaAQAAl1xLGcHVAQCXXEsZwdUBAJdcSxnB1QEAAA==");
    }
    void specialEntries() {
        QFETCH(QString, Format); QFETCH(QByteArray, Fixture); QTemporaryDir Dir; const auto Target = Dir.filePath("special." + Format);
        QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("added"), "new data")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"added"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
        for (const auto& H : Page.Value().entries_) if (H.name_ == "link" || H.name_ == "fifo") {
            QVERIFY(H.metadataRename_); QVERIFY(H.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT);
            QVERIFY(!Backend->Extract(Target.toStdString(), H, Dir.filePath("unsafe").toStdString(), {}).HasValue()); QVERIFY(!QFileInfo::exists(Dir.filePath("unsafe")));
        }
        // Renaming links never materializes a native link or a regular file.
        Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}}; auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend);
        std::vector<ArchiveChange> Changes;
        for (const auto& Pair : {std::pair{"link", "renamed"}, {"fifo", "pipe"}}) {
            auto At = Root; At.nesting_.back().entry_ = std::string("/") + Pair.first; auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
            Changes.push_back({ArchiveChangeKind::Rename, At.nesting_.back().entry_, std::string("/") + Pair.second, {}, Entry.Value().id_});
        }
        auto Session = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        Done = Session.Value()->Prepare(Changes, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); QVERIFY(Session.Value()->Commit({}).HasValue());
        const auto Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe"); QProcess Process;
        Process.start(Cli, {"l", "-slt", Target}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); const auto Text = Process.readAllStandardOutput(); QVERIFY(Text.contains("lrwxrwxrwx")); QVERIFY(Text.contains("prw-r--r--"));
        Process.start(Cli, {"x", "-so", Target, "renamed"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); QCOMPARE(Process.readAllStandardOutput(), QByteArray("keep"));
        const auto Before = Read(Target); ArchiveUpdate Invalid{Target.toStdString(), Dir.path().toStdString(), {"sub/link"}, {"renamed"}}; Invalid.metadataOrigins_ = {"renamed"}; Invalid.reuseData_ = {true};
        auto Wire = EncodeArchiveUpdate(Invalid); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().reuseData_, Invalid.reuseData_);
        Done = Backend->Update(Invalid, {}); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().code_, std::string("7zip-special-parent-change")); QCOMPARE(Read(Target), Before);
        Invalid.reuseData_.clear(); Invalid.add_ = {"added"}; Done = Backend->Update(Invalid, {}); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"renamed", "pipe"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue()); Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(2));
    }
    void archiveEnvelopes_data() {
        QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Sfx");
        QTest::newRow("7z-trailer") << QString("7z") << false; QTest::newRow("7z-sfx") << QString("7z") << true;
        QTest::newRow("zip-trailer") << QString("zip") << false; QTest::newRow("zip-prefix") << QString("zip") << true;
    }
    void archiveEnvelopes() {
        QFETCH(QString, Format); QFETCH(bool, Sfx); QTemporaryDir Dir;
        const auto Bare = Dir.filePath("bare." + Format), Target = Dir.filePath("wrapped.exe");
        const auto Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe");
        QVERIFY(Put(Dir.filePath("keep"), "keep payload")); QVERIFY(Put(Dir.filePath("added"), "new payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); QVERIFY(Backend->Update({Bare.toStdString(), Dir.path().toStdString(), {"keep"}}, {}).HasValue());
        const auto Prefix = !Sfx ? QByteArray{} : Format == "7z" ? Read(QDir(root_).filePath("../sevenzip-tools/official/7z.sfx")) : QByteArray(512, 'x');
        if (Sfx) QVERIFY(!Prefix.isEmpty()); const QByteArray Trailer("opaque trailer\0bytes", 20);
        const auto Before = Prefix + Read(Bare) + Trailer; QVERIFY(Put(Target, Before));
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"added"}};
        auto Small = Change; Small.maxOutputBytes_ = Prefix.size() + Trailer.size(); QVERIFY(!Backend->Update(Small, {}).HasValue()); QCOMPARE(Read(Target), Before);
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        const auto Bytes = Read(Target); QVERIFY(Bytes.startsWith(Prefix)); QVERIFY(Bytes.endsWith(Trailer));
        QProcess Process; Process.start(Cli, {"t", Target}); QVERIFY(Process.waitForFinished(30000)); QVERIFY2(Process.exitCode() <= 1, Process.readAllStandardOutput().constData());
        Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}}; auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend);
        auto At = Root; At.nesting_.back().entry_ = "/added"; auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Session = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Session.HasValue());
        Done = Session.Value()->Prepare({{ArchiveChangeKind::Rename, "/added", "/renamed", {}, Entry.Value().id_}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); QVERIFY(Session.Value()->Commit({}).HasValue());
        QVERIFY(Read(Target).startsWith(Prefix)); QVERIFY(Read(Target).endsWith(Trailer));
        if (Sfx && Format == "7z") {
            auto Signed = Prefix; const auto Pe = qFromLittleEndian<quint32>(Signed.constData() + 60); const auto Optional = Pe + 24;
            const auto Directories = qFromLittleEndian<quint16>(Signed.constData() + Optional) == 0x20b ? 112 : 96;
            qToLittleEndian<quint32>(5, Signed.data() + Optional + Directories - 4); qToLittleEndian<quint32>(Before.size(), Signed.data() + Optional + Directories + 32); qToLittleEndian<quint32>(8, Signed.data() + Optional + Directories + 36);
            const auto Fixture = Signed + Read(Bare) + Trailer; QVERIFY(Put(Target, Fixture)); Done = Backend->Update(Change, {}); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().code_, std::string("7zip-sfx-signature-update")); QCOMPARE(Read(Target), Fixture);
        }
    }
    void mixedZipEncryption() {
        QTemporaryDir Dir; const auto Target = Dir.filePath("mixed.zip"), Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe");
        QProcess Process; Process.setWorkingDirectory(Dir.path());
        const QStringList Modes{"plain", "AES128", "AES192", "AES256", "ZipCrypto"};
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString()};
        for (const auto& Mode : Modes) {
            QVERIFY(Put(Dir.filePath(Mode), "original " + Mode.toUtf8())); QVERIFY(Put(Dir.filePath("new-" + Mode), "replacement " + Mode.toUtf8()));
            QStringList Args{"a", "-tzip", Target, Mode}; if (Mode != "plain") Args << "-psecret" << "-mem=" + Mode;
            Process.start(Cli, Args); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
            Change.add_.push_back(("new-" + Mode).toStdString()); Change.remove_.push_back(Mode.toStdString()); Change.metadataOrigins_.push_back(Mode.toStdString());
        }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Before = Read(Target);
        auto Done = Backend->Update(Change, PasswordContext("wrong")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        auto Limited = Change; Limited.maxOutputBytes_ = 20; Done = Backend->Update(Limited, PasswordContext("secret")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        Done = Backend->Update(Change, PasswordContext("secret")); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Change.archive_, true, PasswordContext("secret")); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); Listed.Value().reset(); QCOMPARE(Page.Value().entries_.size(), size_t(5));
        Process.start(Cli, {"l", "-slt", Target, "-psecret"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); const auto Listing = QString::fromUtf8(Process.readAllStandardOutput());
        for (const auto& H : Page.Value().entries_) {
            const auto Name = QString::fromStdString(H.name_), Mode = Name.mid(4); QCOMPARE(H.encrypted_, Mode != "plain");
            const auto At = Listing.indexOf("Path = " + Name + "\r\n"); QVERIFY2(At >= 0, qPrintable(Listing)); const auto Block = Listing.mid(At).section("\r\n\r\n", 0, 0);
            if (Mode != "plain") { auto Expected = Mode; if (Mode.startsWith("AES")) Expected.insert(3, '-'); QVERIFY2(Block.contains("Method = " + Expected), qPrintable(Block)); }
            Process.start(Cli, {"x", "-so", Target, Name, "-psecret"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); QCOMPARE(Process.readAllStandardOutput(), "replacement " + Mode.toUtf8());
        }
        const auto Context = PasswordContext("secret"); Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}};
        auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = "/new-plain";
        auto Entry = Reader->Resolve(At, Context).get(); QVERIFY(Entry.HasValue()); auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Batch.HasValue());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/new-plain", "/common-plain", {}, Entry.Value().id_}}, Context); QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit(Context).HasValue());
        Listed = Backend->Enumerate(Change.archive_, true, Context); QVERIFY(Listed.HasValue()); Page = Listed.Value()->Next(16, Context); QVERIFY(Page.HasValue()); bool Plain = false;
        for (const auto& H : Page.Value().entries_) if (H.name_ == "common-plain") { Plain = true; QVERIFY(!H.encrypted_); } QVERIFY(Plain);
    }
    void unknownSevenZipSecurityIsNotDiscarded() {
        // Draft ACL/unknown FilesInfo record 0x1a. The upstream decoder reports
        // UnsupportedFeatureWarning while still exposing the readable payload.
        const auto Fixture = QByteArray::fromBase64("N3q8ryccAATYnjGTEQAAAAAAAABqAAAAAAAAAP88NRtwcmVzZXJ2ZWQgcGF5bG9hZAEEBgABCREABwsBAAEBAAwRAAgKAXXiZ88AAAUBERMAawBlAGUAcAAuAHQAeAB0AAAAGQAUCgEA1B4a5zdT3QEVBgEAIIDpgRoeAAEAAAAUAAAAAQAEgAAAAAAAAAAAAAAAAAAAAAAAAAA=");
        QTemporaryDir Dir; const auto Target = Dir.filePath("security.7z"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("new"), "new"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
        QCOMPARE(Page.Value().entries_.size(), size_t(1)); QVERIFY(Backend->Extract(Target.toStdString(), Page.Value().entries_[0], Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), QByteArray("preserved payload"));
        for (const auto& Change : {ArchiveUpdate{Target.toStdString(), Dir.path().toStdString(), {"new"}}, ArchiveUpdate{Target.toStdString(), Dir.path().toStdString(), {}, {"keep.txt"}}}) {
            auto Done = Backend->Update(Change, {}); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported);
            QCOMPARE(Done.Failure().code_, std::string("7zip-unrepresented-metadata-update")); QCOMPARE(Read(Target), Fixture);
        }
    }
    void legacyZipMetadata() {
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlAVaixCBwAAAAcAAAAIACoAa2VlcC50eHRVWAwApV0NXqZdDV7oA+kDTlUWAE5VQ1gIAGsAZQBlAHAALgB0AHgAdABwYXlsb2FkUEsBAhQAFAAAAAAAgxgiUBVqLEIHAAAABwAAAAgARAAAAAAAAAAgAAAAAAAAAGtlZXAudHh0VVgIAKVdDV6mXQ1eTlU0AE5VQ1gIAA4AawBlAGUAcAAuAHQAeAB0AGwAZQBnAGEAYwB5ACAAYwBvAG0AbQBlAG4AdABQSwUGAAAAAAEAAQB6AAAAVwAAAAAA");
        QTemporaryDir Dir; const auto Target = Dir.filePath("legacy.zip"), Name = QString::fromUtf8("새 이름.txt"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath(Name), "payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {Name.toStdString()}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Bytes = Read(Target); const auto Local = Bytes.indexOf(QByteArray::fromHex("55580c00")); const auto Central = Bytes.indexOf(QByteArray::fromHex("55580800")); QVERIFY(Local >= 0 && Central >= 0);
            QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Local + 4), 1577934245u); QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 4), 1577934245u);
            const auto MTime = qFromLittleEndian<quint32>(Bytes.constData() + Local + 8); if (Step) QVERIFY(MTime > 1577934246u); else QCOMPARE(MTime, 1577934246u);
            QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 8), MTime); QCOMPARE(Bytes.mid(Local + 12, 4), QByteArray::fromHex("e803e903"));
            QByteArray Unicode(Name.size() * 2, 0); for (qsizetype I = 0; I < Name.size(); ++I) qToLittleEndian<quint16>(Name[I].unicode(), Unicode.data() + I * 2);
            QCOMPARE(Bytes.count(Unicode), 2); QVERIFY(Bytes.contains(QByteArray::fromHex("6c0065006700610063007900200063006f006d006d0065006e007400")));
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.front().name_, Name.toStdString()); QVERIFY(Backend->Extract(Change.archive_, Page.Value().entries_.front(), Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), Step ? QByteArray("replacement") : QByteArray("payload"));
            Change.remove_ = {Name.toStdString()}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath(Name), "replacement"));
        }
        auto Bad = Fixture; Bad.replace("NUCX", "BAD!"); QVERIFY(Put(Target, Bad)); Change.remove_ = {"keep.txt"}; Change.metadataOrigins_ = {"keep.txt"};
        QVERIFY(!Backend->Update(Change, {}).HasValue()); QCOMPARE(Read(Target), Bad);
    }
    void zipContentHashesAndSignatures() {
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlAVaixCBwAAAAcAAAAIABcAa2VlcC50eHRGSxMATUQ1Mhw89IbtUJFk7eweGYH+yHBheWxvYWRQSwECFAAUAAAAAACDGCJQFWosQgcAAAAHAAAACAAXAAAAAAAAACAAAAAAAAAAa2VlcC50eHRGSxMATUQ1Mhw89IbtUJFk7eweGYH+yFBLBQYAAAAAAQABAE0AAABEAAAAAAA=");
        QTemporaryDir Dir; const auto Target = Dir.filePath("hash.zip"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed"), "payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (const auto Data : {QByteArray("payload"), QByteArray("replacement")}) {
            QVERIFY(Put(Dir.filePath("renamed"), Data)); auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            QCOMPARE(Read(Target).count(QByteArray::fromHex("464b1300") + "MD5" + QCryptographicHash::hash(Data, QCryptographicHash::Md5)), 2);
            Change.remove_ = {"renamed"}; Change.metadataOrigins_.clear();
        }
        auto Signed = Fixture; Signed.replace(QByteArray::fromHex("464b1300"), QByteArray::fromHex("16001300")); QVERIFY(Put(Target, Signed));
        ArchiveUpdate Append{Target.toStdString(), Dir.path().toStdString(), {"renamed"}}; auto Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), Signed);
        // A central-directory signature and an APK signing-block marker are
        // archive-wide contracts, independent of metadata-origin mappings.
        for (bool Apk : {false, true}) {
            auto Bytes = Fixture; auto End = Bytes.indexOf(QByteArray::fromHex("504b0506")); const auto Central = qFromLittleEndian<quint32>(Bytes.constData() + End + 16);
            if (Apk) {
                QByteArray Block(32, 0); qToLittleEndian<quint64>(24, Block.data()); qToLittleEndian<quint64>(24, Block.data() + 8); Block.replace(16, 16, "APK Sig Block 42"); Bytes.insert(Central, Block); End += Block.size(); qToLittleEndian<quint32>(Central + Block.size(), Bytes.data() + End + 16);
            } else {
                const auto Block = QByteArray::fromHex("504b05050400") + "sign"; Bytes.insert(End, Block); End += Block.size(); const auto Size = qFromLittleEndian<quint32>(Bytes.constData() + End + 12); qToLittleEndian<quint32>(Size + Block.size(), Bytes.data() + End + 12);
            }
            QVERIFY(Put(Target, Bytes)); Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Bytes);
        }
        const auto Jar = Dir.filePath("signed.jar.zip"); QVERIFY(QDir().mkpath(Dir.filePath("META-INF"))); QVERIFY(Put(Dir.filePath("META-INF/TEST.SF"), "signed manifest"));
        QVERIFY(Backend->Update({Jar.toStdString(), Dir.path().toStdString(), {"META-INF/TEST.SF"}}, {}).HasValue()); const auto Before = Read(Jar); Append.archive_ = Jar.toStdString(); Failed = Backend->Update(Append, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Failed.Failure().code_, std::string("zip-jar-signature-update")); QCOMPARE(Read(Jar), Before);
    }
    void zipAlignment_data() {
        QTest::addColumn<QByteArray>("Fixture");
        QTest::newRow("apache") << qUncompress(QByteArray::fromBase64("AAAgknic7doxCsIwFIDhZ4x2EdqhglMprnoKV5dcIWAHtWAGF8HNk/QkHsJBcPAsJm0VK3iAwv+FkAfJS+Y8nlkPdSrBZTY3091yFfk4TC3PeF8ULqsesSQCAAAAAAB6y9lTebAb0ykDLK7mPPFxmFru8dFuy6y6UQYAAAAAAKCfwtc+/xQBBiqVf90A4zYjb9emN0CJJL953fLBd16UNC++80b1pvLDhfP11S9kWCOS"));
        QTest::newRow("android") << qUncompress(QByteArray::fromBase64("AAAghnic7dq9CsIwEMDxs0a7COlQwbG46toXcO2SVyi0g1qwQxfBzQfzIRykDj6LOb+wgg9Q+P9CyBFyyZzjXDY0sajjbO6mm+Uq9LFOIze7Lcs6ba9WIgEAAAAAAL1V5/tqlxeuUwZYnNxh4mOdRi62yddV2p4pAwAAAAAA0E/6tU8+RYBBEMu/boC35LVqb8BvRrdw8J0RRs+3XDYa617gR6EnH9fdAaxoInY="));
    }
    void zipAlignment() {
        QFETCH(QByteArray, Fixture); QTemporaryDir Dir; const auto Target = Dir.filePath("aligned.zip"); QVERIFY(Put(Target, Fixture));
        const auto Name = QString(180, 'n'); QVERIFY(Put(Dir.filePath(Name), QByteArray(8193, 'r'))); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {Name.toStdString()}, {"keep"}}; Change.metadataOrigins_ = {"keep"};
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        for (int Step = 0; Step < 2; ++Step) {
            const auto Bytes = Read(Target); const auto End = Bytes.lastIndexOf(QByteArray::fromHex("504b0506")); QVERIFY(End >= 0); auto At = qsizetype(qFromLittleEndian<quint32>(Bytes.constData() + End + 16));
            for (int I = 0; I < 2; ++I) {
                QCOMPARE(Bytes.mid(At, 4), QByteArray::fromHex("504b0102")); const auto Local = qFromLittleEndian<quint32>(Bytes.constData() + At + 42);
                const auto DataAt = Local + 30 + qFromLittleEndian<quint16>(Bytes.constData() + Local + 26) + qFromLittleEndian<quint16>(Bytes.constData() + Local + 28); QCOMPARE(DataAt % 4096, 0u);
                QCOMPARE(qFromLittleEndian<quint16>(Bytes.constData() + Local + 8), quint16(0));
                At += 46 + qFromLittleEndian<quint16>(Bytes.constData() + At + 28) + qFromLittleEndian<quint16>(Bytes.constData() + At + 30) + qFromLittleEndian<quint16>(Bytes.constData() + At + 32);
            }
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(4, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            for (const auto& H : Page.Value().entries_) { QVERIFY(Backend->Extract(Change.archive_, H, Dir.filePath("out").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("out")), H.name_ == "tail" ? QByteArray("tail payload") : H.name_ == "new" ? QByteArray("new") : QByteArray(8193, 'r')); }
            if (!Step) { QVERIFY(Put(Dir.filePath("new"), "new")); Done = Backend->Update({Change.archive_, Dir.path().toStdString(), {"new"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); }
        }
    }
    void codecChains_data() {
        QTest::addColumn<QStringList>("Options"); QTest::addColumn<QString>("Method");
        QTest::newRow("lzma") << QStringList{"-m0=LZMA:d1m:lc2:lp1:pb1"} << QString("LZMA:");
        QTest::newRow("ppmd") << QStringList{"-m0=PPMd:mem1m:o6"} << QString("PPMD:o6:");
        QTest::newRow("bzip2") << QStringList{"-m0=BZip2"} << QString("BZip2");
        QTest::newRow("deflate") << QStringList{"-m0=Deflate"} << QString("Deflate");
        QTest::newRow("bcj") << QStringList{"-m0=BCJ", "-m1=LZMA2:d1m"} << QString("BCJ LZMA2:");
        QTest::newRow("delta") << QStringList{"-m0=Delta:4", "-m1=LZMA2:d1m"} << QString("Delta:4 LZMA2:");
        QTest::newRow("arm64") << QStringList{"-m0=ARM64", "-m1=LZMA2:d1m"} << QString("ARM64 LZMA2:");
    }
    void codecChains() {
        QFETCH(QStringList, Options); QFETCH(QString, Method); QTemporaryDir Dir; const auto Target = Dir.filePath("codecs.7z");
        const auto Cli = QDir(root_).filePath("../sevenzip-tools/official/7z.exe"); QVERIFY(Put(Dir.filePath("keep"), QByteArray(256 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("remove"), QByteArray(4096, 'r')));
        QProcess Process; Process.setWorkingDirectory(Dir.path()); Process.start(Cli, QStringList{"a", Target, "keep", "remove", "-ms=on"} + Options); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"remove"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Put(Dir.filePath("renamed"), Read(Dir.filePath("keep")))); ArchiveUpdate Rename{Target.toStdString(), Dir.path().toStdString(), {"renamed"}, {"keep"}}; Rename.metadataOrigins_ = {"keep"};
        Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        Process.start(Cli, {"l", "-slt", Target}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); const auto Listing = Process.readAllStandardOutput(); QVERIFY2(Listing.contains(Method.toUtf8()), Listing.constData());
        if (Method == "LZMA:") QVERIFY(Listing.contains(":lc2:lp1:pb1"));
        Process.start(Cli, {"x", "-so", Target, "renamed"}); QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0); QCOMPARE(Process.readAllStandardOutput(), Read(Dir.filePath("keep")));
    }
    void storedArchiveSignature() {
        QTemporaryDir Dir; const auto Name = QString("test_read_format_rar5_compressed.rar");
        QVERIFY(QFile::copy(QDir(root_).filePath(Name), Dir.filePath(Name)));
        ArchiveUpdate Create{Dir.filePath("outer.zip").toStdString(), Dir.path().toStdString(), {Name.toStdString()}};
        Create.creation_ = ArchiveCreationOptions{0, false}; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1));
        QCOMPARE(Page.Value().entries_.front().name_, Name.toStdString());
    }
    void largeFile64Bit() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for the 4 GiB acceptance test");
        QTemporaryDir Dir; const auto Source = Dir.filePath("large.bin");
        constexpr qint64 Size = (qint64(1) << 32) + 4096;
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(Source.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0; const bool Sparse = DeviceIoControl(Handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Returned, nullptr);
        LARGE_INTEGER End; End.QuadPart = Size;
        const bool Sized = Sparse && SetFilePointerEx(Handle, End, nullptr, FILE_BEGIN) && SetEndOfFile(Handle); CloseHandle(Handle); QVERIFY(Sized);
        { QFile File(Source); QVERIFY(File.open(QIODevice::ReadWrite)); QCOMPARE(File.write("head"), 4); QVERIFY(File.seek(Size - 4)); QCOMPARE(File.write("tail"), 4); }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        for (const auto* Format : {"7z", "zip"}) {
            QElapsedTimer Timer; Timer.start(); const auto Target = Dir.filePath(QString("large.") + Format);
            ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"large.bin"}}; Create.creation_ = ArchiveCreationOptions{1, false};
            auto Made = Backend->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
            const auto CreateMs = Timer.restart(); auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue());
            auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(1));
            const auto Header = Page.Value().entries_.front(); QCOMPARE(Header.size_, uint64_t(Size)); Listed.Value().reset();
            const auto Restored = Dir.filePath("restored"); auto Extracted = Backend->Extract(Target.toStdString(), Header, Restored.toStdString(), {});
            QVERIFY2(Extracted.HasValue(), Extracted.HasValue() ? "" : Extracted.Failure().code_.c_str()); QCOMPARE(QFileInfo(Restored).size(), Size);
            QCryptographicHash Expected(QCryptographicHash::Sha256), Actual(QCryptographicHash::Sha256);
            QFile Input(Source), Output(Restored); QVERIFY(Input.open(QIODevice::ReadOnly)); QVERIFY(Output.open(QIODevice::ReadOnly));
            QVERIFY(Expected.addData(&Input)); QVERIFY(Actual.addData(&Output)); QCOMPARE(Actual.result(), Expected.result()); Output.close();
            qInfo() << "64-bit round trip" << Format << "bytes" << Size << "create-ms" << CreateMs << "read-verify-ms" << Timer.elapsed();
            QVERIFY(QFile::remove(Restored));
        }
    }
    void zip64EntryCount() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for the ZIP64 entry-count acceptance test");
        QTemporaryDir Dir; constexpr int Count = 65537; QElapsedTimer Timer; Timer.start();
        ArchiveUpdate Create{Dir.filePath("many.zip").toStdString(), Dir.path().toStdString()}; Create.creation_ = ArchiveCreationOptions{0, false};
        for (int I = 0; I < Count; ++I) { const auto Name = QString("entry-%1").arg(I, 6, 10, QChar('0')); QVERIFY(QDir(Dir.path()).mkdir(Name)); Create.add_.push_back(Name.toStdString() + '/'); }
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Made = Backend->Update(Create, {});
        QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str()); const auto CreateMs = Timer.restart();
        auto Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue()); std::set<std::string> Names;
        for (;;) { auto Page = Listed.Value()->Next(257, {}); QVERIFY(Page.HasValue()); for (const auto& Header : Page.Value().entries_) { QVERIFY(Header.directory_); QVERIFY(Names.insert(Header.name_).second); } if (Page.Value().end_) break; }
        QCOMPARE(Names.size(), size_t(Count));
        for (int I = 0; I < Count; ++I) QVERIFY(Names.contains(QString("entry-%1").arg(I, 6, 10, QChar('0')).toStdString()));
        qInfo() << "ZIP64 entries" << Count << "input-and-create-ms" << CreateMs << "paged-read-ms" << Timer.elapsed();
        Listed.Value().reset(); QVERIFY(QDir(Dir.path()).mkdir("renamed"));
        ArchiveUpdate Rename{Create.archive_, Dir.path().toStdString(), {"renamed/"}, {"entry-000000"}}; Rename.metadataOrigins_ = {"entry-000000"};
        auto Changed = Backend->Update(Rename, {}); QVERIFY2(Changed.HasValue(), Changed.HasValue() ? "" : Changed.Failure().code_.c_str());
        Listed = Backend->Enumerate(Create.archive_, true, {}); QVERIFY(Listed.HasValue()); Names.clear();
        for (;;) { auto Page = Listed.Value()->Next(257, {}); QVERIFY(Page.HasValue()); for (const auto& H : Page.Value().entries_) QVERIFY(Names.insert(H.name_).second); if (Page.Value().end_) break; }
        QCOMPARE(Names.size(), size_t(Count)); QVERIFY(Names.contains("renamed")); QVERIFY(!Names.contains("entry-000000"));
    }
    void zip64OffsetMetadata() {
        if (!qEnvironmentVariableIsSet("HC_ARCHIVE_STRESS")) QSKIP("Enable HC_ARCHIVE_STRESS for ZIP64 metadata offsets beyond 4 GiB");
        QTemporaryDir Dir; const auto Source = Dir.filePath("large"); constexpr qint64 Size = (qint64(1) << 32) + 4096;
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(Source.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        DWORD Returned = 0; LARGE_INTEGER End; End.QuadPart = Size;
        const bool Sized = DeviceIoControl(Handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &Returned, nullptr) && SetFilePointerEx(Handle, End, nullptr, FILE_BEGIN) && SetEndOfFile(Handle); CloseHandle(Handle); QVERIFY(Sized);
        QVERIFY(Put(Dir.filePath("meta"), "metadata payload")); QVERIFY(Put(Dir.filePath("renamed-meta"), "metadata payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); const auto Target = Dir.filePath("offsets.zip"); QElapsedTimer Timer; Timer.start();
        ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"large", "meta"}}; Create.creation_ = ArchiveCreationOptions{0, false};
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str()); QVERIFY(QFileInfo(Target).size() > Size);
        ArchiveUpdate Rename{Create.archive_, Dir.path().toStdString(), {"renamed-meta"}, {"meta"}}; Rename.metadataOrigins_ = {"meta"};
        Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QFile File(Target); QVERIFY(File.open(QIODevice::ReadOnly)); QVERIFY(File.seek(File.size() - 4096)); const auto Tail = File.readAll(); File.close();
        const auto Last = Tail.lastIndexOf(QByteArray::fromHex("504b0102")); QVERIFY(Last >= 0); QCOMPARE(qFromLittleEndian<quint32>(Tail.constData() + Last + 42), UINT32_MAX);
        QVERIFY(Tail.contains(QByteArray::fromHex("504b0606"))); QVERIFY(Tail.contains("renamed-meta"));
        // The official tool reads all stored bytes and checks CRCs independently.
        QProcess Check; Check.start(QDir(root_).filePath("../sevenzip-tools/official/7z.exe"), {"t", Target, "-y"}); QVERIFY(Check.waitForFinished(120000)); QCOMPARE(Check.exitCode(), 0);
        qInfo() << "ZIP64 metadata offset round trip bytes" << Size << "elapsed-ms" << Timer.elapsed();
    }
    void creation_data() {
        QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Solid"); QTest::addColumn<bool>("Encrypt"); QTest::addColumn<unsigned>("Level");
        for (const auto Format : {QString("7z"), QString("zip")}) for (const auto Level : {0u, 5u, 9u})
            QTest::newRow(qPrintable(Format + QString::number(Level))) << Format << false << false << Level;
        QTest::newRow("solid") << QString("7z") << true << false << 5u;
        QTest::newRow("7z-aes") << QString("7z") << true << true << 5u;
        QTest::newRow("zip-aes") << QString("zip") << false << true << 5u;
    }
    void creation() {
        QFETCH(QString, Format); QFETCH(bool, Solid); QFETCH(bool, Encrypt); QFETCH(unsigned, Level);
        QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input"))); QVERIFY(QDir().mkdir(Dir.filePath("input/empty")));
        const auto Data = QByteArray(2 * 1024 * 1024, 'z'); QVERIFY(Put(Dir.filePath("input/한글.txt"), Data)); QVERIFY(Put(Dir.filePath("input/zero"), {})); QVERIFY(Put(Dir.filePath("input/second"), "second payload"));
        const auto Target = Dir.filePath("result." + Format);
        ArchiveUpdate Update{Target.toStdString(), Dir.filePath("input").toStdString(), {"한글.txt", "zero", "empty/", "second"}};
        Update.encrypt_ = Encrypt; Update.creation_ = ArchiveCreationOptions{Level, Solid};
        const auto Wire = EncodeArchiveUpdate(Update); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().creation_->level_, Level);
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Context = PasswordContext("secret");
        auto Done = Backend->Update(Update, Context); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto Listed = Backend->Enumerate(Target.toStdString(), true, Context); QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        auto Page = Listed.Value()->Next(16, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); QCOMPARE(Page.Value().entries_.size(), size_t(4));
        Listed.Value().reset(); bool Found = false;
        for (const auto& H : Page.Value().entries_) if (H.name_ == "한글.txt") {
            Found = true; QCOMPARE(H.encrypted_, Encrypt); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), Context).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Data);
        }
        QVERIFY(Found); QVERIFY(!GetModuleHandleW(L"7z.dll"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()) + "." + Format), Read(Target))); }
    }
    void updateAndFailureAtomicity_data() { QTest::addColumn<QString>("Format"); QTest::newRow("7z") << QString("7z"); QTest::newRow("zip") << QString("zip"); }
    void updateAndFailureAtomicity() {
        QFETCH(QString, Format); QTemporaryDir Dir; QVERIFY(QDir().mkdir(Dir.filePath("input")));
        QVERIFY(Put(Dir.filePath("input/keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("input/remove"), "remove"));
        const auto Target = Dir.filePath("result." + Format); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.filePath("input").toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, false};
        auto Made = Backend->Update(Create, {}); QVERIFY2(Made.HasValue(), Made.HasValue() ? "" : Made.Failure().code_.c_str());
        const auto Before = Read(Target); QVERIFY(Put(Dir.filePath("input/new"), "new payload"));
        ArchiveUpdate Change{Target.toStdString(), Dir.filePath("input").toStdString(), {"new"}, {"remove"}};
        auto Limited = Change; Limited.maxOutputBytes_ = 10; auto Failed = Backend->Update(Limited, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Before);
        std::stop_source Stop; Stop.request_stop(); OperationContext Cancel; Cancel.stopToken_ = Stop.get_token(); QVERIFY(!Backend->Update(Change, Cancel).HasValue()); QCOMPARE(Read(Target), Before);
        auto Bad = Change; Bad.add_ = {"../escape"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        Bad = Change; Bad.add_ = {"KEEP"}; QVERIFY(!Backend->Update(Bad, {}).HasValue()); QCOMPARE(Read(Target), Before);
        auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), size_t(2)); List.Value().reset();
        for (const auto& H : Page.Value().entries_) { QVERIFY(H.name_ != "remove"); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Read(Dir.filePath("input/" + QString::fromStdString(H.name_)))); }
        ArchiveUpdate Empty{Target.toStdString(), Dir.filePath("input").toStdString(), {}, {"new", "keep"}};
        Done = Backend->Update(Empty, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); QVERIFY(Page.Value().entries_.empty());
    }
    void unsupportedUpdatesPreserveOriginal() {
        QTemporaryDir Dir; auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        QVERIFY(Put(Dir.filePath("new"), "new"));
        for (const auto* Name : {"test_read_format_rar5_compressed.rar", "split.7z.001"}) {
            const auto Source = QDir(root_).filePath(Name); if (!QFileInfo::exists(Source)) continue;
            const auto Target = Dir.filePath(Name); QVERIFY(QFile::copy(Source, Target)); const auto Before = Read(Target);
            auto Result = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, PasswordContext("secret"));
            QVERIFY(!Result.HasValue()); QCOMPARE(Read(Target), Before);
        }
    }
    void encryptedUpdate_data() {
        QTest::addColumn<QString>("Name");
        for (const auto* Name : {"encrypted.7z", "content-encrypted.7z", "aes.zip", "aes128.zip", "aes192.zip", "zipcrypto.zip"}) QTest::newRow(Name) << QString(Name);
    }
    void encryptedUpdate() {
        QFETCH(QString, Name); QTemporaryDir Dir; const auto Target = Dir.filePath(Name);
        if (Name == "content-encrypted.7z" || Name == "aes128.zip" || Name == "aes192.zip") {
            const auto Cli = qEnvironmentVariable("HC_SEVENZIP_CLI", QDir(root_).filePath("../sevenzip-tools/official/7z.exe")); QVERIFY(QFileInfo::exists(Cli));
            QStringList Args{"a", Target, "-psecret", "-mx=1", "-y"};
            if (Name.endsWith(".7z")) Args << "-t7z" << "-mhe=off";
            else Args << "-tzip" << (Name == "aes128.zip" ? "-mem=AES128" : "-mem=AES192");
            Args << "*"; QProcess Process; Process.setWorkingDirectory(QDir(root_).filePath("input")); Process.start(Cli, Args);
            QVERIFY(Process.waitForFinished(30000)); QCOMPARE(Process.exitCode(), 0);
        } else QVERIFY(QFile::copy(QDir(root_).filePath(Name), Target));
        const auto Before = Read(Target); QVERIFY(Put(Dir.filePath("new"), "new encrypted payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"new"}, {"payload.bin"}};
        auto Done = Backend->Update(Change, PasswordContext("wrong")); QVERIFY(!Done.HasValue()); QCOMPARE(Read(Target), Before);
        OperationContext Cancel; Cancel.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> { return Error{ErrorCategory::Cancelled, "cancel-password"}; };
        Done = Backend->Update(Change, Cancel); QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Cancelled); QCOMPARE(Read(Target), Before);
        const auto Context = PasswordContext("secret"); Done = Backend->Update(Change, Context); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto UnprotectedHeaders = Backend->Enumerate(Target.toStdString(), false, {});
        QCOMPARE(UnprotectedHeaders.HasValue(), Name != "encrypted.7z"); if (UnprotectedHeaders.HasValue()) UnprotectedHeaders.Value().reset();
        auto Listed = Backend->Enumerate(Target.toStdString(), true, Context); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(64, Context); QVERIFY(Page.HasValue()); Listed.Value().reset();
        bool Found = false;
        for (const auto& H : Page.Value().entries_) {
            QVERIFY(H.name_ != "payload.bin"); if (H.directory_) continue; if (H.size_ || Name.endsWith(".zip")) QVERIFY(H.encrypted_);
            Done = Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), Context); QVERIFY(Done.HasValue());
            if (H.name_ == "new") { Found = true; QCOMPARE(Read(Dir.filePath("restored")), QByteArray("new encrypted payload")); }
            else QCOMPARE(Read(Dir.filePath("restored")), Read(QDir(root_).filePath("input/" + QString::fromStdString(H.name_))));
        }
        QVERIFY(Found);
        for (const auto& Names : {std::pair{"new", "renamed-new"}, std::pair{"renamed-new", "new"}}) {
            Locator Root{LocatorKind::Archive, Target.toStdString(), {{"wcx", "/"}}};
            auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = std::string("/") + Names.first;
            auto Entry = Reader->Resolve(At, Context).get(); QVERIFY(Entry.HasValue());
            auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, Context, {}, Backend); QVERIFY(Batch.HasValue());
            auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, At.nesting_.back().entry_, std::string("/") + Names.second, {}, Entry.Value().id_}}, Context);
            QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QVERIFY(Batch.Value()->Commit(Context).HasValue());
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("updated-" + Name), Read(Target))); }
    }
    void solidUpdatePreservation() {
        QTemporaryDir Dir; QVERIFY(Put(Dir.filePath("keep"), QByteArray(1024 * 1024, 'k'))); QVERIFY(Put(Dir.filePath("remove"), "remove")); QVERIFY(Put(Dir.filePath("new"), "new"));
        const auto Target = Dir.filePath("solid.7z"); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Create{Target.toStdString(), Dir.path().toStdString(), {"keep", "remove"}}; Create.creation_ = ArchiveCreationOptions{5, true};
        auto Done = Backend->Update(Create, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        // Appending preserves the original block and its stored dictionary.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {}, {"remove"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        auto List = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(List.HasValue()); auto Page = List.Value()->Next(16, {}); QVERIFY(Page.HasValue()); List.Value().reset();
        QCOMPARE(Page.Value().entries_.size(), size_t(2));
        for (const auto& H : Page.Value().entries_) { QVERIFY(H.name_ != "remove"); QVERIFY(Backend->Extract(Target.toStdString(), H, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Read(Dir.filePath(QString::fromStdString(H.name_)))); }
    }
    void zipCommentsAndExtraFields() {
        QTemporaryDir Dir; const auto Target = Dir.filePath("comments.zip");
        // Stored ZIP with archive/file comments and an opaque 0xCAFE extra field
        // in both headers. This fixture is independent of the SDK writer.
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAAgAa2VlcC50eHT+ygQAQUJDRHByZXNlcnZlZCBwYXlsb2FkUEsBAhQAFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAgACAANAAAAAAAgAAAAAAAAAGtlZXAudHh0/soEAEFCQ0RlbnRyeSBjb21tZW50UEsFBgAAAAABAAEASwAAAD8AAAAPAGFyY2hpdmUgY29tbWVudA==");
        QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("new"), "new")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        const auto After = Read(Target); QVERIFY(After.endsWith("archive comment")); QVERIFY(After.contains("entry comment"));
        QCOMPARE(After.count(QByteArray::fromHex("feca040041424344")), 2); QVERIFY(After.contains("preserved payload"));
        // Unmapped metadata and opaque extra fields must not be silently lost.
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"new"}, {"keep.txt", "new"}}, {});
        QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), After);
        ArchiveUpdate Mapped{Target.toStdString(), Dir.path().toStdString(), {"new"}, {"keep.txt", "new"}}; Mapped.metadataOrigins_ = {"keep.txt"};
        Done = Backend->Update(Mapped, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QCOMPARE(Read(Target).count(QByteArray::fromHex("feca040041424344")), 2);
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("comments.zip"), Read(Target))); }
    }
    void sevenZipPosixMetadata() {
        // Copy archive with an uncompressed header carrying UNIX mode 0100751;
        // both header CRCs were rebuilt independently and checked by official 7z.
        const auto Fixture = QByteArray::fromBase64("N3q8ryccAARTh/QQEQAAAAAAAABKAAAAAAAAAPc9pPBwcmVzZXJ2ZWQgcGF5bG9hZAEEBgABCREABwsBAAEBAAwRAAgKAXXiZ88AAAUBERMAawBlAGUAcAAuAHQAeAB0AAAAGQAUCgEA1B4a5zdT3QEVBgEAIIDpgQAA");
        QTemporaryDir Dir; const auto Target = Dir.filePath("posix.7z"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.size(), size_t(1)); const auto& Header = Page.Value().entries_[0]; QCOMPARE(Header.attributes_ >> 16, 0100751u);
            QVERIFY(Backend->Extract(Change.archive_, Header, Dir.filePath("restored").toStdString(), {}).HasValue()); QCOMPARE(Read(Dir.filePath("restored")), Step ? QByteArray("replacement payload") : QByteArray("preserved payload"));
            Change.remove_ = {"renamed.txt"}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("posix.7z"), Read(Target))); }
    }
    void zipSecurityAndPosixMetadata_data() {
        QTest::addColumn<QByteArray>("Fixture");
        // Independent Python zipfile fixtures: UNIX 0100751, UID 1000/GID 1001,
        // a stored NT security descriptor and distinct local/central extensions.
        QTest::newRow("dos-times") << QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAD8Aa2VlcC50eHRTRB8AFAAAAAAAACW8ltsBAASAAAAAAAAAAAAAAAAAAAAAAHV4CwABBOgDAAAE6QMAAO++CQBsb2NhbC10YWdwcmVzZXJ2ZWQgcGF5bG9hZFBLAQIUAxQAAAAAAIMYIlB14mfPEQAAABEAAAAIAD8ADQAAAAAAIADpgQAAAABrZWVwLnR4dFNEHwAUAAAAAAAAJbyW2wEABIAAAAAAAAAAAAAAAAAAAAAAdXgLAAEE6AMAAATpAwAA774JAGNlbnRyLXRhZ2VudHJ5IGNvbW1lbnRQSwUGAAAAAAEAAQCCAAAAdgAAAA8AYXJjaGl2ZSBjb21tZW50");
        QTest::newRow("unix-times") << QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIAFAAa2VlcC50eHRTRB8AFAAAAAAAACW8ltsBAASAAAAAAAAAAAAAAAAAAAAAAHV4CwABBOgDAAAE6QMAAO++CQBsb2NhbC10YWdVVA0AB6ZdDV6lXQ1epF0NXnByZXNlcnZlZCBwYXlsb2FkUEsBAhQDFAAAAAAAgxgiUHXiZ88RAAAAEQAAAAgAUAANAAAAAAAgAOmBAAAAAGtlZXAudHh0U0QfABQAAAAAAAAlvJbbAQAEgAAAAAAAAAAAAAAAAAAAAAB1eAsAAQToAwAABOkDAADvvgkAY2VudHItdGFnVVQNAAemXQ1epV0NXqRdDV5lbnRyeSBjb21tZW50UEsFBgAAAAABAAEAkwAAAIcAAAAPAGFyY2hpdmUgY29tbWVudA==");
    }
    void zipSecurityAndPosixMetadata() {
        QFETCH(QByteArray, Fixture);
        QTemporaryDir Dir; const auto Target = Dir.filePath(QString("security-") + QTest::currentDataTag() + ".zip"); QVERIFY(Put(Target, Fixture));
        QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload")); auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Change{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Change.metadataOrigins_ = {"keep.txt"};
        for (int Step = 0; Step < 2; ++Step) {
            auto Done = Backend->Update(Change, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Bytes = Read(Target); QCOMPARE(Bytes.count(QByteArray::fromHex("53441f001400000000000025bc96db0100048000000000000000000000000000000000")), 2);
            QCOMPARE(Bytes.count(QByteArray::fromHex("75780b000104e803000004e9030000")), 2);
            QCOMPARE(Bytes.count("local-tag"), 1); QCOMPARE(Bytes.count("centr-tag"), 1);
            const auto Central = Bytes.indexOf(QByteArray::fromHex("504b0102")); QVERIFY(Central >= 0);
            QCOMPARE(quint8(Bytes[Central + 5]), quint8(3)); QCOMPARE(qFromLittleEndian<quint32>(Bytes.constData() + Central + 38), quint32((0100751u << 16) | 0x20));
            auto Listed = Backend->Enumerate(Change.archive_, true, {}); QVERIFY(Listed.HasValue()); auto Page = Listed.Value()->Next(2, {}); QVERIFY(Page.HasValue()); Listed.Value().reset();
            QCOMPARE(Page.Value().entries_.size(), size_t(1)); auto Extracted = Backend->Extract(Change.archive_, Page.Value().entries_[0], Dir.filePath("restored").toStdString(), {}); QVERIFY(Extracted.HasValue());
            QCOMPARE(Read(Dir.filePath("restored")), Step ? QByteArray("replacement payload") : QByteArray("preserved payload"));
            Change.remove_ = {"renamed.txt"}; Change.metadataOrigins_.clear(); QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        }
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS"); if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath(QString("security-") + QTest::currentDataTag() + ".zip"), Read(Target))); }
        Change.remove_ = {"keep.txt"}; Change.metadataOrigins_ = {"keep.txt"};
        // Payload hashes, alignment and malformed extension lengths cannot be
        // carried over as opaque metadata. Failure must leave the input intact.
        for (const auto& Id : {QByteArray::fromHex("464b0900"), QByteArray::fromHex("1ea10900"), QByteArray::fromHex("55580900"), QByteArray::fromHex("efbeffff")}) {
            auto Bad = Fixture; Bad.replace(QByteArray::fromHex("efbe0900"), Id); QVERIFY(Put(Target, Bad));
            auto Failed = Backend->Update(Change, {}); QVERIFY(!Failed.HasValue()); QCOMPARE(Read(Target), Bad);
        }
        auto Special = Fixture; const auto Central = Special.indexOf(QByteArray::fromHex("504b0102")); QVERIFY(Central >= 0);
        qToLittleEndian<quint32>((0010644u << 16) | 0x20, Special.data() + Central + 38); QVERIFY(Put(Target, Special));
        auto Unsupported = Backend->Update(Change, {}); QVERIFY(!Unsupported.HasValue()); QCOMPARE(Unsupported.Failure().category_, ErrorCategory::Unsupported); QCOMPARE(Read(Target), Special);
        QVERIFY(Put(Target, Fixture)); Change.maxOutputBytes_ = 250;
        auto Limited = Backend->Update(Change, {}); QVERIFY(!Limited.HasValue()); QCOMPARE(Limited.Failure().category_, ErrorCategory::BudgetExceeded); QCOMPARE(Read(Target), Fixture);
    }
    void metadataRenameAndReplacement() {
        // Independently generated ZIP: file/archive comments and three NTFS times.
        const auto Fixture = QByteArray::fromBase64("UEsDBBQAAAAAAIMYIlB14mfPEQAAABEAAAAIACQAa2VlcC50eHQKACAAAAAAAAEAGAAAAAVpNsDVAYBpbGg2wNUBANPTZzbA1QFwcmVzZXJ2ZWQgcGF5bG9hZFBLAQIUABQAAAAAAIMYIlB14mfPEQAAABEAAAAIACQADQAAAAAAIAAAAAAAAABrZWVwLnR4dAoAIAAAAAAAAQAYAAAABWk2wNUBgGlsaDbA1QEA09NnNsDVAWVudHJ5IGNvbW1lbnRQSwUGAAAAAAEAAQBnAAAAWwAAAA8AYXJjaGl2ZSBjb21tZW50");
        QTemporaryDir Dir; const auto Target = Dir.filePath("metadata.zip"); QVERIFY(Put(Target, Fixture)); QVERIFY(Put(Dir.filePath("renamed.txt"), "preserved payload"));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        ArchiveUpdate Rename{Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"keep.txt"}}; Rename.metadataOrigins_ = {"keep.txt"};
        auto Wire = EncodeArchiveUpdate(Rename); auto Decoded = DecodeArchiveUpdate(Wire); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().metadataOrigins_, Rename.metadataOrigins_);
        Wire["metadataOrigins"] = QJsonArray{}; QVERIFY(!DecodeArchiveUpdate(Wire).HasValue());
        auto Invalid = Rename; Invalid.metadataOrigins_ = {"missing"}; QVERIFY(!Backend->Update(Invalid, {}).HasValue()); QCOMPARE(Read(Target), Fixture);
        auto Unknown = Fixture; Unknown.replace(QByteArray::fromHex("0a0020000000000001001800"), QByteArray::fromHex("0a0020000000000002001800"));
        const auto UnknownTarget = Dir.filePath("unknown-ntfs.zip"); QVERIFY(Put(UnknownTarget, Unknown));
        auto Preserve = Rename; Preserve.archive_ = UnknownTarget.toStdString(); auto Preserved = Backend->Update(Preserve, {});
        QVERIFY2(Preserved.HasValue(), Preserved.HasValue() ? "" : Preserved.Failure().code_.c_str());
        QCOMPARE(Read(UnknownTarget).count(QByteArray::fromHex("020018000000056936c0d50180696c6836c0d50100d3d36736c0d501")), 2);
        auto Done = Backend->Update(Rename, {}); QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        QVERIFY(Put(Dir.filePath("renamed.txt"), "replacement payload"));
        Done = Backend->Update({Target.toStdString(), Dir.path().toStdString(), {"renamed.txt"}, {"renamed.txt"}}, {});
        QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
        QVERIFY(Read(Target).contains("entry comment")); QVERIFY(Read(Target).endsWith("archive comment"));
        // Exercise provenance through the common mutation coordinator and host.
        const auto Common = Dir.filePath("common.zip"); QVERIFY(Put(Common, Fixture)); Locator Root{LocatorKind::Archive, Common.toStdString(), {{"wcx", "/"}}};
        auto Reader = MakeWcxReadProvider({}, Root, Dir.path().toStdString(), Backend); auto At = Root; At.nesting_.back().entry_ = "/keep.txt";
        auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Batch = BeginWcxMutation({}, Root, Reader, Dir.path().toStdString(), {}, {}, {}, Backend); QVERIFY(Batch.HasValue());
        auto Prepared = Batch.Value()->Prepare({{ArchiveChangeKind::Rename, "/keep.txt", "/common-name.txt", {}, Entry.Value().id_}}, {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str()); QCOMPARE(Read(Common), Fixture); QVERIFY(Batch.Value()->Commit({}).HasValue());
        QVERIFY(Read(Common).contains("entry comment")); QVERIFY(Read(Common).endsWith("archive comment"));
        const auto Artifacts = qEnvironmentVariable("HC_SEVENZIP_WRITE_ARTIFACTS");
        if (!Artifacts.isEmpty()) { QVERIFY(QDir().mkpath(Artifacts)); QVERIFY(Put(QDir(Artifacts).filePath("metadata.zip"), Read(Target))); QVERIFY(Put(QDir(Artifacts).filePath("metadata-common.zip"), Read(Common))); }
    }
    void schedulerPublication() {
        using namespace hc::operations;
        QTemporaryDir Dir; const auto Source = Dir.filePath("source"), Target = Dir.filePath("result.zip"); QVERIFY(Put(Source, QByteArray(1024, 'a')));
        auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Entry = Reader->Resolve({LocatorKind::Local, Source.toStdString()}, {}).get(); QVERIFY(Entry.HasValue());
        hc::sevenzip::CreateRequest Request; Request.sources_.push_back({Entry.Value(), Reader, {}}); Request.destination_ = {LocatorKind::Local, Target.toStdString()}; Request.format_ = "zip";
        const auto Folder = Dir.filePath("folder"); QVERIFY(QDir().mkdir(Folder)); QVERIFY(Put(Folder + "/child", "child"));
        const auto Handle = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Folder).utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr); QVERIFY(Handle != INVALID_HANDLE_VALUE);
        const auto Ticks = (uint64_t(1577934246000LL) + 11644473600000ULL) * 10000; FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
        const bool Saved = SetFileTime(Handle, nullptr, nullptr, &Time); CloseHandle(Handle); QVERIFY(Saved);
        auto FolderEntry = Reader->Resolve({LocatorKind::Local, Folder.toStdString()}, {}).get(); QVERIFY(FolderEntry.HasValue()); Request.sources_.push_back({FolderEntry.Value(), Reader, {}});
        JobScheduler Scheduler(std::make_shared<hc::commands::EventHub>());
        auto Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue());
        auto Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        auto Result = Job.Value().completion_.get(); QVERIFY2(Result.snapshot_.state_ == JobState::Completed, Result.snapshot_.error_ ? Result.snapshot_.error_->code_.c_str() : "job state"); QVERIFY(Result.cleanupErrors_.empty());
        const auto Before = Read(Target); QVERIFY(!Before.isEmpty());
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); auto Listed = Backend->Enumerate(Target.toStdString(), true, {}); QVERIFY(Listed.HasValue());
        auto Page = Listed.Value()->Next(16, {}); QVERIFY(Page.HasValue()); bool DirectorySeen = false;
        for (const auto& Header : Page.Value().entries_) if (Header.directory_) { QCOMPARE(Header.modifiedUtcMs_, FolderEntry.Value().modifiedUtcMs_); DirectorySeen = true; }
        QVERIFY(DirectorySeen); Listed.Value().reset();
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        QVERIFY(Scheduler.Cancel(Job.Value().id_).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 5000); QCOMPARE(Job.Value().completion_.get().snapshot_.state_, JobState::Cancelled); QCOMPARE(Read(Target), Before);
        Prepared = hc::sevenzip::PrepareCreate(Request); QVERIFY(Prepared.HasValue()); Job = Scheduler.Submit(std::move(Prepared.Value().request_), std::move(Prepared.Value().task_)); QVERIFY(Job.HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Scheduler.Snapshot(Job.Value().id_).Value().state_ == JobState::WaitingForUser, 5000);
        const auto Waiting = Scheduler.Snapshot(Job.Value().id_).Value(); QVERIFY(Put(Target, "external change"));
        QVERIFY(Scheduler.Reply(Job.Value().id_, {Waiting.interaction_->id_, InteractionChoice::Replace, {}}).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Job.Value().completion_.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 30000);
        Result = Job.Value().completion_.get(); QCOMPARE(Result.snapshot_.state_, JobState::Failed); QCOMPARE(Read(Target), QByteArray("external change"));
    }
    void initTestCase() {
        const auto Path = qEnvironmentVariable("HC_SEVENZIP_CORPUS");
        QVERIFY2(QFileInfo::exists(Path), "Build hc_sevenzip_corpus or set HC_SEVENZIP_CORPUS to the generated corpus.json");
        cases_ = QJsonDocument::fromJson(Read(Path))["cases"].toArray(); QVERIFY(cases_.size() >= 17); root_ = QFileInfo(Path).absolutePath();
    }
    void corpus_data() {
        QTest::addColumn<int>("Row");
        for (int Index = 0; Index < cases_.size(); ++Index) QTest::newRow(QFileInfo(cases_[Index].toObject()["archive"].toString()).fileName().toUtf8()) << Index;
    }
    void corpus() {
        QFETCH(int, Row); const auto Case = cases_[Row].toObject(); const auto Path = Case["archive"].toString();
        QTemporaryDir Output; auto Options = BuiltinSevenZipOptions(); auto Context = PasswordContext(Case["password"].toString());
        auto Backend = MakeApprovedWcxBackend(Options); auto Listed = Backend->Enumerate(Path.toStdString(), false, Context);
        QVERIFY2(Listed.HasValue(), Listed.HasValue() ? "" : Listed.Failure().code_.c_str());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(2, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str());
            for (const auto& Header : Page.Value().entries_) Headers.push_back(Header); if (Page.Value().end_) break; }
        Listed.Value().reset(); QVERIFY(!Headers.empty());
        size_t Files = 0;
        for (const auto& Header : Headers) {
            if (Header.directory_) continue;
            const auto PathTo = Output.filePath("selected.bin"); auto Done = Backend->Extract(Path.toStdString(), Header, PathTo.toStdString(), Context);
            if (Header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) { QVERIFY(!Done.HasValue()); QCOMPARE(Done.Failure().category_, ErrorCategory::Unsupported); continue; }
            QVERIFY2(Done.HasValue(), Done.HasValue() ? "" : Done.Failure().code_.c_str());
            const auto Expected = QDir(Case["expected"].toString()).filePath(QString::fromStdString(Header.name_));
            QVERIFY2(QFileInfo::exists(Expected), qPrintable(Expected)); QCOMPARE(Read(PathTo), Read(Expected)); ++Files;
        }
        QVERIFY(Files > 0);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, Context); QVERIFY(Tested.HasValue());
        for (;;) { auto Page = Tested.Value()->Next(64, Context); QVERIFY2(Page.HasValue(), Page.HasValue() ? "" : Page.Failure().code_.c_str()); if (Page.Value().end_) break; }
        QVERIFY(!Backend->Update({Path.toStdString(), Output.path().toStdString()}, Context).HasValue());
        QVERIFY(!GetModuleHandleW(L"7z.dll")); // The application never loads archive engine code.
    }
    void errorsCancellationAndPolicy() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        const auto Plain = QDir(root_).filePath("plain.7z").toStdString(); const auto Encrypted = QDir(root_).filePath("encrypted.7z").toStdString();
        auto Required = Backend->Enumerate(Encrypted, false, {}); QVERIFY(!Required.HasValue()); QCOMPARE(Required.Failure().category_, ErrorCategory::AccessDenied);
        auto Wrong = Backend->Enumerate(Encrypted, false, PasswordContext("wrong")); QVERIFY(!Wrong.HasValue());
        auto Declined = PasswordContext(""); Declined.passwordPrompt_ = [](const std::string&, unsigned, const OperationContext&) -> Result<hc::storage::Password> { return Error{ErrorCategory::Cancelled, "test-cancel-password"}; };
        auto Cancelled = Backend->Enumerate(Encrypted, false, Declined); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        auto Correct = Backend->Enumerate(Encrypted, false, PasswordContext("secret")); QVERIFY(Correct.HasValue()); Correct.Value().reset();
        std::stop_source Stop; Stop.request_stop(); OperationContext Stopped; Stopped.stopToken_ = Stop.get_token();
        auto Early = Backend->Enumerate(Plain, false, Stopped); QVERIFY(!Early.HasValue()); QCOMPARE(Early.Failure().category_, ErrorCategory::Cancelled);
        OperationContext Limited; Limited.maxEnumerationEntries_ = 1; QVERIFY(!Backend->Enumerate(Plain, false, Limited).HasValue());
        QTemporaryDir Directory; const auto Broken = Directory.filePath("broken.7z"); QVERIFY(Put(Broken, Read(QString::fromStdString(Plain)).left(48)));
        auto Bad = Backend->Enumerate(Broken.toStdString(), true, {});
        if (Bad.HasValue()) QVERIFY(!Bad.Value()->Next(256, {}).HasValue());
        auto Listed = Backend->Enumerate(Plain, false, {}); QVERIFY(Listed.HasValue()); std::optional<ArchiveHeader> Chosen;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue()); for (const auto& Item : Page.Value().entries_) if (Item.name_ == "sub/zeros.bin") Chosen = Item; if (Page.Value().end_) break; }
        QVERIFY(Chosen.has_value()); Listed.Value().reset();
        OperationContext Automatic; Automatic.workload_.intent_ = WorkloadIntent::Automatic; Automatic.workload_.automaticFileBytes_ = 1024;
        auto Blocked = Backend->Extract(Plain, *Chosen, Directory.filePath("auto").toStdString(), Automatic); QVERIFY(!Blocked.HasValue()); QCOMPARE(Blocked.Failure().category_, ErrorCategory::BudgetExceeded);
        std::stop_source During; OperationContext Active; Active.stopToken_ = During.get_token(); Active.progress_ = [&](uint64_t Done) { if (Done > 1024 * 1024) During.request_stop(); };
        // Drive the native callback synchronously: the 8 MiB fixture can finish
        // before the host's 50 ms progress notification interval on fast disks.
        const auto Module = LoadLibraryExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(BuiltinSevenZipOptions().binaryPath_).utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        QVERIFY(Module); auto Unload = qScopeGuard([&] { FreeLibrary(Module); }); auto Native = MakeSevenZipBackend(Module);
        auto Interrupted = Native->Extract(Plain, *Chosen, Directory.filePath("cancelled").toStdString(), Active); QVERIFY(!Interrupted.HasValue());
        QCOMPARE(Interrupted.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!QFileInfo::exists(Directory.filePath("cancelled")));
    }
    void corruptedPayload() {
        QTemporaryDir Directory; auto Bytes = Read(QDir(root_).filePath("plain.7z")); QVERIFY(Bytes.size() > 128);
        // Damage the packed payload while preserving the start and next headers.
        Bytes[64] = char(static_cast<unsigned char>(Bytes[64]) ^ 0x80);
        const auto Path = Directory.filePath("corrupt.7z"); QVERIFY(Put(Path, Bytes));
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
        auto Listed = Backend->Enumerate(Path.toStdString(), false, {}); QVERIFY(Listed.HasValue());
        std::vector<ArchiveHeader> Headers;
        for (;;) { auto Page = Listed.Value()->Next(128, {}); QVERIFY(Page.HasValue());
            for (const auto& Header : Page.Value().entries_) if (!Header.directory_) Headers.push_back(Header);
            if (Page.Value().end_) break; }
        Listed.Value().reset(); bool Failed = false;
        for (const auto& Header : Headers) {
            const auto Output = Directory.filePath("output.bin");
            auto Done = Backend->Extract(Path.toStdString(), Header, Output.toStdString(), {});
            if (!Done.HasValue()) { Failed = true; QVERIFY(!QFileInfo::exists(Output)); }
        }
        QVERIFY(Failed);
        auto Tested = Backend->Enumerate(Path.toStdString(), true, {}); QVERIFY(Tested.HasValue());
        QVERIFY(!Tested.Value()->Next(128, {}).HasValue());
    }
    void commonProviderAndSplitMissing() {
        auto Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions()); QTemporaryDir Cache;
        const auto Path = QDir(root_).filePath("solid.7z"); Locator Root{LocatorKind::Archive, Path.toStdString(), {{"wcx", "/"}}};
        auto Provider = MakeWcxReadProvider({}, Root, Cache.path().toStdString(), Backend);
        auto At = Root; At.nesting_.back().entry_ = "/payload.bin"; auto Entry = Provider->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        auto Stream = Provider->OpenRead(Entry.Value().id_, {}).get(); QVERIFY2(Stream.HasValue(), Stream.HasValue() ? "" : Stream.Failure().code_.c_str());
        QVERIFY(Stream.Value()->CanSeek()); QVERIFY(Stream.Value()->Seek(100, {}).get().HasValue()); auto Chunk = Stream.Value()->Read(100, {}).get(); QVERIFY(Chunk.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()), Read(QDir(root_).filePath("input/payload.bin")).mid(100, 100));
        QTemporaryDir Missing;
        for (const auto* Name : {"split.7z.001", "test_read_format_rar5_multiarchive.part01.rar"}) {
            QVERIFY(QFile::copy(QDir(root_).filePath(Name), Missing.filePath(Name)));
            auto Bad = Backend->Enumerate(Missing.filePath(Name).toStdString(), true, {});
            if (Bad.HasValue()) { bool Failed = false; for (;;) { auto Page = Bad.Value()->Next(128, {}); if (!Page.HasValue()) { Failed = true; break; } if (Page.Value().end_) break; } QVERIFY(Failed); }
        }
    }
};
QTEST_GUILESS_MAIN(SevenZipTests)
#include "tstSevenZip.moc"
