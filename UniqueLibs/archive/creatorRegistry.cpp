#include "stdafx.h"
#include "creatorRegistry.hpp"

#include "UniqueLibs/settings/appSettings.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/builtinIso.hpp"
#include "HyperCommanderBuiltInPlugin/IsoImage/isoRewrite.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "commonLibs/storage/materializedContent.hpp"

namespace hc::archive {
using namespace storage;
using namespace plugins;
namespace {
Error Failure(const char* Code) { return {ErrorCategory::Unavailable, Code, {"archive-create"}}; }
std::string IsoHash(const PluginManifest& Manifest)
{
    for (const auto& [Name, Hash] : Manifest.dependencies_) if (QFileInfo(QString::fromStdString(Name)).fileName() == "hc_iso_core.dll") return Hash;
    return {};
}
Result<Unit> CompareFile(const QString& Left, const QString& Right, OperationContext Context)
{
    QFile A(Left), B(Right); if (!A.open(QIODevice::ReadOnly) || !B.open(QIODevice::ReadOnly) || A.size() != B.size()) return Failure("archive-create-verification-size");
    while (!A.atEnd()) {
        if (const auto Stop = Context.Check({"archive-create"})) return *Stop;
        const auto Data = A.read(1024 * 1024), Other = B.read(1024 * 1024);
        if (Data.isEmpty() || Data != Other) return Failure("archive-create-verification-content");
    }
    return Unit{};
}
Result<Unit> VerifyWcx(const std::shared_ptr<IArchiveBackend>& Backend, const QString& Archive, const OutputPlan& Plan,
    const QString& Input, const QString& Temporary, OperationContext Context)
{
    std::map<QString, const PlannedEntry*> Expected;
    for (const auto& Entry : Plan.entries_) Expected.emplace(Entry.member_, &Entry);
    auto Listed = Backend->Enumerate(Archive.toStdString(), true, Context); if (!Listed.HasValue()) return Listed.Failure();
    std::vector<ArchiveHeader> Headers;
    for (;;) {
        auto Page = Listed.Value()->Next(128, Context); if (!Page.HasValue()) return Page.Failure();
        for (auto& Header : Page.Value().entries_) Headers.push_back(std::move(Header));
        if (Headers.size() > Expected.size()) return Failure("archive-create-verification-extra-entry");
        if (Page.Value().end_) break;
    }
    Listed.Value().reset(); if (Headers.size() != Expected.size()) return Failure("archive-create-verification-entry-count");
    for (const auto& Header : Headers) {
        auto Name = QString::fromStdString(Header.name_); Name.replace('\\', '/'); if (Name.endsWith('/')) Name.chop(1);
        const auto Found = Expected.find(Name); if (Found == Expected.end() || Found->second->source_.entry_.directory_ != Header.directory_) return Failure("archive-create-verification-path");
        if (!Header.directory_) {
            const auto Output = QDir(Temporary).filePath("verify.bin");
            auto Extracted = Backend->Extract(Archive.toStdString(), Header, Output.toStdString(), Context); if (!Extracted.HasValue()) return Extracted.Failure();
            auto Compared = CompareFile(Output, QDir(Input).filePath(Name), Context); if (!Compared.HasValue()) return Compared;
            if (!QFile::remove(Output)) return Failure("archive-create-verification-cleanup");
        }
        Expected.erase(Found);
    }
    return Expected.empty() ? Result<Unit>(Unit{}) : Result<Unit>(Failure("archive-create-verification-missing-entry"));
}
Result<Unit> VerifyDopus(std::shared_ptr<PluginProcess> Process, const QString& Archive, const OutputPlan& Plan,
    const QString& Input, const QString& Temporary, OperationContext Context)
{
    const auto Cache = QDir(Temporary).filePath("dopus-cache"); if (!QDir().mkdir(Cache)) return Failure("archive-create-dopus-cache");
    auto Store = MakeNativeContentStore(Cache.toStdString()); if (!Store.HasValue()) return Store.Failure();
    auto Mounted = MakeHostedDopusVfsProvider(Process, {"archive-create-dopus"}, Store.Value(), false, Archive); if (!Mounted.HasValue()) return Mounted.Failure();
    std::map<QString, const PlannedEntry*> Expected; for (const auto& Entry : Plan.entries_) Expected.emplace(Entry.member_, &Entry);
    std::vector<std::pair<Locator, QString>> Directories{{Mounted.Value().root_, {}}};
    for (size_t I = 0; I < Directories.size(); ++I) {
        auto Cursor = Mounted.Value().reader_->Enumerate(Directories[I].first, Context).get(); if (!Cursor.HasValue()) return Cursor.Failure();
        std::vector<EntrySnapshot> Entries;
        for (;;) {
            auto Page = Cursor.Value()->Next(128, Context).get(); if (!Page.HasValue()) return Page.Failure();
            Entries.insert(Entries.end(), Page.Value().entries_.begin(), Page.Value().entries_.end()); if (Page.Value().end_) break;
            if (Page.Value().entries_.empty()) return Failure("archive-create-dopus-list-no-progress");
        }
        Cursor.Value().reset();
        for (const auto& Entry : Entries) {
            const auto Leaf = QString::fromStdString(Entry.name_); if (!IsSafeArchiveName(Leaf)) return Failure("archive-create-verification-path");
            const auto Name = Directories[I].second.isEmpty() ? Leaf : Directories[I].second + '/' + Leaf;
            const auto Found = Expected.find(Name); if (Found == Expected.end() || Found->second->source_.entry_.directory_ != Entry.directory_) return Failure("archive-create-verification-path");
            if (Entry.directory_) Directories.emplace_back(Entry.locator_, Name);
            else {
                auto Stream = Mounted.Value().reader_->OpenRead(Entry.id_, Context).get(); if (!Stream.HasValue()) return Stream.Failure();
                QFile Source(QDir(Input).filePath(Name)); if (!Source.open(QIODevice::ReadOnly) || uint64_t(Source.size()) != Entry.size_) return Failure("archive-create-verification-size");
                for (;;) {
                    auto Chunk = Stream.Value()->Read(1024 * 1024, Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                    const auto Data = Source.read(qint64(Chunk.Value().bytes_.size()));
                    if (Data.size() != qsizetype(Chunk.Value().bytes_.size()) || !std::equal(Chunk.Value().bytes_.begin(), Chunk.Value().bytes_.end(), reinterpret_cast<const std::byte*>(Data.constData()))) return Failure("archive-create-verification-content");
                    if (Chunk.Value().eof_) { if (!Source.atEnd()) return Failure("archive-create-verification-truncated"); break; }
                    if (Chunk.Value().bytes_.empty()) return Failure("archive-create-dopus-read-no-progress");
                }
            }
            Expected.erase(Found);
        }
    }
    return Expected.empty() ? Result<Unit>(Unit{}) : Result<Unit>(Failure("archive-create-verification-missing-entry"));
}
}
QByteArray CreatorManifestIdentity(const PluginManifest& Manifest)
{ return QCryptographicHash::hash(EncodeManifest(Manifest), QCryptographicHash::Sha256).toHex(); }
CreatorRuntime ExternalCreator(const QJsonObject& Record, PluginFormat Format, const QString& SettingsDirectory)
{
    CreatorRuntime Out; Out.available_ = false;
    const auto Reviewed = ParseManifest(settings::JsonBytes(Record["hostReviewed"])); if (!Reviewed.HasValue() || Reviewed.Value().format_ != Format) { Out.status_ = QObject::tr("실행 승인 필요"); return Out; }
    auto& Options = Out.options_; Options.approval_ = {Reviewed.Value(), true, false, Reviewed.Value().permissions_};
    Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe"; Options.manifestPath_ = Record["hostManifest"].toString();
    const auto Root = Reviewed.Value().packageRoot_.empty() ? QFileInfo(Options.manifestPath_).absolutePath() : QString::fromStdString(Reviewed.Value().packageRoot_);
    Options.binaryPath_ = QDir(Root).filePath(QString::fromStdString(Reviewed.Value().binary_));
    const auto Id = (Format == PluginFormat::Wcx ? "wcx." : "dopus.") + Reviewed.Value().id_.substr(0, 96) + '.'
        + QCryptographicHash::hash(Options.manifestPath_.toUtf8(), QCryptographicHash::Sha256).toHex().left(12).toStdString();
    Options.defaultIniPath_ = QDir(SettingsDirectory).filePath("archive-plugins/" + QString::fromStdString(Id) + "/pkplugin.ini");
    auto& Descriptor = Out.descriptor_; Descriptor.id_ = Id; Descriptor.displayName_ = QFileInfo(Options.binaryPath_).fileName().toStdString();
    Descriptor.kind_ = Format == PluginFormat::Wcx ? ArchiveCreatorKind::Wcx : ArchiveCreatorKind::DopusVfs;
    Descriptor.identity_ = {Reviewed.Value().sha256_, Reviewed.Value()};
    const auto Evidence = Record["archiveCreation"].toObject();
    if (Evidence["version"] == 1 && Evidence["identity"].toString().toLatin1() == CreatorManifestIdentity(Reviewed.Value())) {
        Descriptor.configuration_ = Evidence["configure"].toBool() && Reviewed.Value().permissions_.contains(Permission::UserInterface) ? ArchiveConfigurationKind::PluginDialog : ArchiveConfigurationKind::None;
        for (const auto Value : Evidence["formats"].toArray()) { const auto Name = Value.toString().toStdString(); Descriptor.formats_.push_back({Name, Name, ArchiveSettingsKind::External, ArchiveCreationEvidence::VerifiedPackage}); }
    }
    const auto Rule = PluginApplicationRule::Parse(Record["application"]);
    Out.available_ = Record["hostEnabled"].toBool() && Rule.HasValue() && Rule.Value().Enabled() && ValidateArchiveCreator(Descriptor).HasValue();
    if (!Out.available_) Out.status_ = QObject::tr("설정 또는 실행 승인을 확인하십시오."); return Out;
}
std::vector<CreatorRuntime> CreatorCatalog(settings::Repository& Settings)
{
    std::vector<CreatorRuntime> Out;
    CreatorRuntime Seven; Seven.options_ = BuiltinSevenZipOptions(); Seven.descriptor_ = {"builtin.7zip", "7-Zip", ArchiveCreatorKind::BuiltinSevenZip,
        {Seven.options_.approval_.reviewed_.sha256_, Seven.options_.approval_.reviewed_}, ArchiveConfigurationKind::BuiltinDialog,
        {{"7z", "7z", ArchiveSettingsKind::SevenZip, ArchiveCreationEvidence::BuiltinAdapter, 1}, {"zip", "zip", ArchiveSettingsKind::SevenZip, ArchiveCreationEvidence::BuiltinAdapter, 1}}}; Out.push_back(Seven);
    CreatorRuntime Iso; Iso.options_ = BuiltinIsoOptions(); Iso.descriptor_ = {"builtin.iso", "ISO", ArchiveCreatorKind::BuiltinIso,
        {IsoHash(Iso.options_.approval_.reviewed_), {}}, ArchiveConfigurationKind::BuiltinDialog,
        {{"iso", "iso", ArchiveSettingsKind::Iso, ArchiveCreationEvidence::BuiltinAdapter, 1}}}; Out.push_back(Iso);
    for (const auto& [Group, Format] : {std::pair{"wcx", PluginFormat::Wcx}, {"dopusVfs", PluginFormat::DopusVfs}})
        for (const auto Value : Settings.Plugins(Group)) { auto Creator = ExternalCreator(Value.toObject(), Format, QFileInfo(Settings.FileName()).absolutePath()); if (!Creator.descriptor_.formats_.empty()) Out.push_back(std::move(Creator)); }
    return Out;
}
Result<Unit> RevalidateCreator(const CreatorRuntime& Creator, OperationContext Context)
{
    if (!Creator.available_) return Failure("archive-create-creator-unavailable");
    auto Inspected = InspectManifestFile(Creator.options_.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
    if (!Creator.options_.approval_.approved_ || Inspected.Value().manifest_ != Creator.options_.approval_.reviewed_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"archive-create"}};
    const auto Identity = Creator.descriptor_.kind_ == ArchiveCreatorKind::BuiltinIso
        ? ArchiveCreatorIdentity{IsoHash(Inspected.Value().manifest_), {}} : ArchiveCreatorIdentity{Inspected.Value().manifest_.sha256_, Inspected.Value().manifest_};
    if (Identity != Creator.descriptor_.identity_) return Error{ErrorCategory::StaleRevision, "archive-creator-identity-changed", {"archive-create"}};
    return Unit{};
}
Result<PreparedArchiveOutput> CreatePreparedArchive(const CreatorRuntime& Creator, const ArchiveCreateSettings& Settings,
    std::string Format, const OutputPlan& Plan, const QString& Input, const QString& Temporary, OperationContext Context)
{
    auto SettingsLease = AcquireCreatorSettings(Creator, Context); if (!SettingsLease.HasValue()) return SettingsLease.Failure();
    auto Checked = RevalidateCreator(Creator, Context); if (!Checked.HasValue()) return Checked.Failure();
    if (Creator.descriptor_.kind_ == ArchiveCreatorKind::BuiltinIso) {
        const auto& Value = std::get<IsoCreateSettings>(Settings); iso::RewriteRequest Request;
        Request.profile_ = uint32_t(Value.profile_); Request.volumeLabel_ = Value.volumeLabel_; Request.maxImageBytes_ = Value.maxImageBytes_;
        Request.shareDuplicateContents_ = Value.shareDuplicateContents_; Request.timestampUtc_ = Value.timestampUtc_; Request.explicitTimestamp_ = true;
        Request.temporaryBase_ = Temporary;
        for (const auto& Entry : Plan.entries_) Request.sources_.push_back({QDir(Input).filePath(Entry.member_).toStdString(), Entry.member_.toStdString(), Entry.source_.entry_.directory_});
        auto Prepared = iso::PrepareRewrite(std::move(Request), Context).get(); if (!Prepared.HasValue()) return Prepared.Failure();
        return PreparedArchiveOutput{Prepared.Value()->Path(), Prepared.Value()};
    }
    const auto Target = QDir(Temporary).filePath("archive." + QString::fromStdString(Format));
    if (Creator.descriptor_.kind_ == ArchiveCreatorKind::DopusVfs) {
        auto Started = PluginProcess::Start(Creator.options_.executable_, Creator.options_.manifestPath_, Creator.options_.approval_, Creator.options_.limits_, Context, Creator.options_.defaultIniPath_); if (!Started.HasValue()) return Started.Failure();
        auto Process = std::shared_ptr<PluginProcess>(std::move(Started.Value())); QJsonArray Names;
        for (const auto& Name : QDir(Input).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) Names.append(Name);
        auto Written = Process->DataCall(WireKind::StorageMutate, {{"action", "create-archive"}, {"archive", Target}, {"input", Input}, {"names", Names}}, Context); if (!Written.HasValue()) return Written.Failure();
        auto Verified = VerifyDopus(Process, Target, Plan, Input, Temporary, Context); if (!Verified.HasValue()) return Verified.Failure();
    } else {
        ArchiveUpdate Update; Update.archive_ = Target.toStdString(); Update.inputDirectory_ = Input.toStdString();
        for (const auto& Entry : Plan.entries_) Update.add_.push_back(Entry.member_.toStdString() + (Entry.source_.entry_.directory_ ? "/" : ""));
        if (Creator.descriptor_.kind_ == ArchiveCreatorKind::BuiltinSevenZip) {
            const auto& Value = std::get<SevenZipCreateSettings>(Settings); Update.encrypt_ = Value.encrypt_;
            Update.creation_ = ArchiveCreationOptions{Value.level_, Value.solid_ != ArchiveSolidMode::Off, Value};
        } else Update.createNew_ = true;
        auto Backend = MakeApprovedWcxBackend(Creator.options_); auto Written = Backend->Update(std::move(Update), Context); if (!Written.HasValue()) return Written.Failure();
        if (Creator.descriptor_.kind_ == ArchiveCreatorKind::Wcx) { auto Verified = VerifyWcx(Backend, Target, Plan, Input, Temporary, Context); if (!Verified.HasValue()) return Verified.Failure(); }
    }
    return PreparedArchiveOutput{Target, {}};
}
Result<std::shared_ptr<void>> AcquireCreatorSettings(const CreatorRuntime& Creator, OperationContext Context)
{
    if (Creator.descriptor_.kind_ != ArchiveCreatorKind::Wcx && Creator.descriptor_.kind_ != ArchiveCreatorKind::DopusVfs) return std::shared_ptr<void>{};
    const auto Key = QString::fromStdString(Creator.options_.approval_.reviewed_.packageRoot_).toCaseFolded() + '\n' + QDir::cleanPath(Creator.options_.defaultIniPath_).toCaseFolded();
    const auto Name = "Local\\HyperCommander.ArchiveSettings." + QCryptographicHash::hash(Key.toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto Mutex = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(QString::fromLatin1(Name).utf16())); if (!Mutex) return Failure("archive-create-settings-lock");
    for (;;) {
        if (const auto Stop = Context.Check({"archive-create"})) { CloseHandle(Mutex); return *Stop; }
        const auto Wait = WaitForSingleObject(Mutex, 50);
        if (Wait == WAIT_OBJECT_0 || Wait == WAIT_ABANDONED) return std::shared_ptr<void>(Mutex, [](void* Value) { ReleaseMutex(Value); CloseHandle(Value); });
        if (Wait != WAIT_TIMEOUT) { CloseHandle(Mutex); return Failure("archive-create-settings-lock-wait"); }
    }
}
Result<QJsonObject> VerifyCreationFormats(CreatorRuntime Creator, const QStringList& Formats, OperationContext Context)
{
    if (!Creator.options_.approval_.approved_ || !Creator.options_.approval_.granted_.contains(Permission::WriteContent)
        || (Creator.descriptor_.kind_ != ArchiveCreatorKind::Wcx && Creator.descriptor_.kind_ != ArchiveCreatorKind::DopusVfs))
        return Error{ErrorCategory::AccessDenied, "archive-create-probe-not-approved"};
    // This explicit probe establishes evidence; the normal catalog requires it already.
    Creator.available_ = true;
    auto SettingsLease = AcquireCreatorSettings(Creator, Context); if (!SettingsLease.HasValue()) return SettingsLease.Failure();
    auto Checked = RevalidateCreator(Creator, Context); if (!Checked.HasValue()) return Checked.Failure();
    auto Started = PluginProcess::Start(Creator.options_.executable_, Creator.options_.manifestPath_, Creator.options_.approval_, Creator.options_.limits_, Context, Creator.options_.defaultIniPath_); if (!Started.HasValue()) return Started.Failure();
    auto Probe = Started.Value()->Probe(Context); if (!Probe.HasValue()) return Probe.Failure(); Started.Value().reset();
    QTemporaryDir Temporary; if (!Temporary.isValid() || !QDir().mkdir(Temporary.filePath("input"))) return Failure("archive-create-probe-directory");
    const QByteArray Bytes("HyperCommander archive creation verification\r\n");
    if (!QDir().mkpath(Temporary.filePath("input/폴더/empty"))) return Failure("archive-create-probe-input");
    for (const auto& Name : {QString("first.txt"), QString("폴더/한글.txt")}) { QFile File(Temporary.filePath("input/" + Name)); if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly) || File.write(Bytes) != Bytes.size()) return Failure("archive-create-probe-input"); }
    OutputPlan Plan;
    for (const auto& Name : {QString("first.txt"), QString("폴더"), QString("폴더/empty"), QString("폴더/한글.txt")}) { EntrySnapshot Entry; Entry.name_ = Name.toStdString(); Entry.directory_ = !Name.endsWith(".txt"); Entry.size_ = Entry.directory_ ? 0 : Bytes.size(); Plan.entries_.push_back({{Entry, {}, {}}, Name}); }
    QJsonArray Verified; std::optional<Error> FirstFailure;
    for (const auto& Format : Formats) {
        if (!IsSafeArchiveName(Format) || Format.contains('.') || Format != Format.toLower()) return Error{ErrorCategory::InvalidRequest, "archive-create-probe-format"};
        QTemporaryDir Output(Temporary.filePath("format-XXXXXX")); if (!Output.isValid()) return Failure("archive-create-probe-output");
        auto Prepared = CreatePreparedArchive(Creator, ExternalArchiveCreateSettings{}, Format.toStdString(), Plan, Temporary.filePath("input"), Output.path(), Context);
        if (Prepared.HasValue()) Verified.append(Format); else { if (Prepared.Failure().category_ == ErrorCategory::Cancelled) return Prepared.Failure(); if (!FirstFailure) FirstFailure = Prepared.Failure(); }
    }
    if (Verified.isEmpty()) return FirstFailure ? *FirstFailure : Error{ErrorCategory::Unsupported, "archive-create-no-verified-formats"};
    return QJsonObject{{"version", 1}, {"identity", QString::fromLatin1(CreatorManifestIdentity(Creator.options_.approval_.reviewed_))},
        {"configure", Probe.Value()["configure"].toBool()}, {"formats", Verified}};
}
}
