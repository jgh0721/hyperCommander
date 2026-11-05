#include "stdafx.h"
#include "fileContainerOpen.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "nativeMaterializer.hpp"
#include "qtArchivePassword.hpp"
#include "qtPagedStorage.hpp"
#include "qtPluginHostSettings.hpp"
#include "builtinPluginCatalog.hpp"
#include "builtInFsModel.hpp"
#include "storageProviders.hpp"
#include "pluginHost/hostedDataProviders.hpp"

/// 공통 라이브러리
#include "commonLibs/storage/materializedContent.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QPointer>
#include <QSignalBlocker>
#include <QUuid>
#include <QtConcurrentRun>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::navigation
{
    using namespace hc::storage;
    using namespace hc::plugins;
    namespace
    {
        // Snapshot and lease destruction may close an isolated host. Never reap it on the GUI thread.
        template<class T> std::shared_ptr<T> WorkerOwned(std::shared_ptr<T> Value,
            std::shared_ptr<IMaterializedFileLease> Lease = {}, std::shared_ptr<IMaterializer> Materializer = {})
        {
            auto* Pointer = Value.get();
            return std::shared_ptr<T>(Pointer, [Value = std::move(Value), Lease = std::move(Lease), Materializer = std::move(Materializer)](T*) mutable {
                (void)QtConcurrent::run([Value = std::move(Value), Lease = std::move(Lease), Materializer = std::move(Materializer)] {});
            });
        }
        class PrimedProvider final : public IStorageProvider
        {
        public:
            PrimedProvider(std::shared_ptr<IStorageProvider> Reader, Locator Root, std::shared_ptr<PagedEntryIndex> Entries)
                : reader_(std::move(Reader)), root_(std::move(Root)), entries_(std::move(Entries)) {}
            ProviderId Id() const override { return reader_->Id(); }
            std::vector<PresentationColumn> Columns() const override { return reader_->Columns(); }
            ProviderCapabilities Capabilities() const override { return reader_->Capabilities(); }
            bool SupportsParallelReads() const override { return reader_->SupportsParallelReads(); }
            bool SupportsFreshEnumerationMetadata() const override { return reader_->SupportsFreshEnumerationMetadata(); }
            Task<CopyExtras> CopyExtrasAt(Locator At, OperationContext Context) override { return reader_->CopyExtrasAt(std::move(At), Context); }
            Task<SupportDecision> Supports(Operation Op, OperationContext Context) override { return reader_->Supports(Op, Context); }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Context) override { return reader_->Stat(std::move(Id), Context); }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override { return reader_->Resolve(std::move(At), Context); }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Context) override { return reader_->OpenRead(std::move(Id), Context); }
            bool UsesOrderedReads() const override { return reader_->UsesOrderedReads(); }
            Task<Unit> PrepareReadBatch(std::vector<EntrySnapshot> Entries, OperationContext Context) override { return reader_->PrepareReadBatch(std::move(Entries), Context); }
            Task<Unit> FinishReadBatch(OperationContext Context) override { return reader_->FinishReadBatch(Context); }
            Task<std::optional<ContentProducer>> PrepareContent(ResourceId Id, OperationContext Context) override { return reader_->PrepareContent(std::move(Id), Context); }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                std::shared_ptr<PagedEntryIndex> Entries;
                { std::lock_guard Lock(mutex_); if (At == root_) Entries = std::exchange(entries_, {}); }
                if (!Entries) return reader_->Enumerate(std::move(At), Context);
                class Cursor final : public IEntryCursor
                {
                public:
                    explicit Cursor(std::shared_ptr<PagedEntryIndex> Entries) : entries_(std::move(Entries)) {}
                    Task<EntryPage> Next(std::size_t Count, OperationContext Context) override {
                        auto Page = entries_->Read(offset_, Count, Context); if (Page.HasValue()) offset_ += Page.Value().entries_.size(); return Ready<EntryPage>(std::move(Page));
                    }
                private:
                    std::shared_ptr<PagedEntryIndex> entries_; std::uint64_t offset_ = 0;
                };
                return Ready<std::unique_ptr<IEntryCursor>>(std::unique_ptr<IEntryCursor>(new Cursor(std::move(Entries))));
            }
        private:
            std::shared_ptr<IStorageProvider> reader_; Locator root_; std::shared_ptr<PagedEntryIndex> entries_; std::mutex mutex_;
        };
        bool ExtensionMatches(const ContainerCandidate& Candidate, const QString& Name)
        {
            for (auto Ext : Candidate.extensions_) {
                if (Ext.startsWith('.')) Ext.remove(0, 1);
                if (!Ext.isEmpty() && Name.endsWith('.' + Ext, Qt::CaseInsensitive)) return true;
            }
            return Candidate.options_.approval_.reviewed_.format_ == PluginFormat::SevenZip && IsArchiveVolumeName(Name);
        }
        Result<QString> FormatHint(IContentSource& Source, OperationContext Context)
        {
            auto Stream = Source.OpenRead(Context).get(); if (!Stream.HasValue()) return Stream.Failure();
            QByteArray Prefix; const auto Limit = std::min<std::uint64_t>(Source.Describe().size_, 80 * 2048);
            while (std::uint64_t(Prefix.size()) < Limit) {
                const auto Count = std::min<std::size_t>(Limit - Prefix.size(), Context.maxReadBytes_);
                if (!Count) return Error{ErrorCategory::BudgetExceeded, "container-prefix-budget"};
                if (const auto Stop = Context.Check({})) return *Stop;
                auto Bytes = Stream.Value()->Read(Count, Context).get(); if (!Bytes.HasValue()) return Bytes.Failure();
                if (Bytes.Value().bytes_.size() > Count) return Error{ErrorCategory::InvalidRequest, "container-prefix-overread"};
                if (Bytes.Value().bytes_.empty() && !Bytes.Value().eof_) return Error{ErrorCategory::InvalidRequest, "container-prefix-progress"};
                Prefix.append(reinterpret_cast<const char*>(Bytes.Value().bytes_.data()), qsizetype(Bytes.Value().bytes_.size()));
                if (Bytes.Value().eof_) break;
            }
            // Ranking hints only: a plugin must still parse the complete archive successfully.
            if (Prefix.startsWith(QByteArray("PK\x03\x04", 4)) || Prefix.startsWith(QByteArray("PK\x05\x06", 4)) || Prefix.startsWith(QByteArray("PK\x07\x08", 4))) return QString("zip");
            if (Prefix.startsWith(QByteArray::fromHex("377abcaf271c"))) return QString("7z");
            if (Prefix.startsWith(QByteArray("Rar!\x1a\x07", 6))) return QString("rar");
            if (Prefix.startsWith("MSCF")) return QString("cab");
            if (Prefix.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1"))) return QString("msi");
            for (qsizetype Offset = 32769; Offset + 5 <= Prefix.size(); Offset += 2048)
                if (Prefix.mid(Offset, 5) == "CD001") return QString("iso");
            if (Prefix.startsWith(QByteArray("ALZ\x01", 4))) return QString("alz");
            if (Prefix.startsWith("EGGA")) return QString("egg");
            return QString{};
        }
        bool Mismatch(const Error& Error)
        {
            // BAD_DATA and password/permission errors are terminal, unlike an unknown format.
            return Error.code_ == "7zip-format-not-recognized" || Error.code_ == "wcx:" + std::to_string(E_UNKNOWN_FORMAT) || Error.code_ == "wcx:" + std::to_string(E_NOT_SUPPORTED)
                || Error.code_ == "dopus-vfs-format-unhandled" || Error.code_ == "dopus-vfs-read-directory:-3"
                || Error.code_ == "dopus-vfs-enumerate:-3" || Error.code_ == "unegg-format";
        }
    }
    std::vector<ContainerCandidate> FileContainerCandidates()
    {
        std::vector<ContainerCandidate> Candidates;
        for (const auto& Wcx : TyStPlugInMgr::GetInstance()->RetrieveWCX()) {
            if (!Wcx.hostEnabled_ || !Wcx.hostApproval_.approved_) continue;
            const auto Rule = PluginApplicationRule::Parse(Wcx.applicationRule_); if (!Rule.HasValue() || !Rule.Value().Enabled()) continue;
            ContainerCandidate Value; Value.wcx_ = Wcx; Value.application_ = Wcx.applicationRule_; Value.extensions_ = Wcx.Exts;
            Value.content_ = (Wcx.PackerCaps & PK_CAPS_BY_CONTENT) != 0;
            Value.options_.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
            Value.options_.binaryPath_ = Wcx.FileFullPath; Value.options_.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
            if (Value.options_.binaryPath_.endsWith(".wcx", Qt::CaseInsensitive)) Value.options_.binaryPath_ += "64";
            if (QDir::isRelativePath(Value.options_.binaryPath_)) Value.options_.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Value.options_.binaryPath_);
            Value.options_.manifestPath_ = Wcx.hostManifest_; Value.options_.approval_ = Wcx.hostApproval_; Candidates.push_back(std::move(Value));
        }
        for (const auto& Choice : QPluginHostSettings::DataChoices(hc::settings::AppSettings(), PluginFormat::DopusVfs, true)) {
            if (!Choice.options_.approval_.approved_) continue;
            ContainerCandidate Value; Value.dopus_ = Value.content_ = true; Value.application_ = Choice.options_.applicationRule_;
            Value.extensions_ = Choice.extensions_; Value.options_ = {Choice.options_.executable_, Choice.options_.manifestPath_, Choice.options_.binaryPath_, Choice.options_.approval_, Choice.options_.limits_, Choice.options_.defaultIniPath_};
            Candidates.push_back(std::move(Value));
        }
        return Candidates;
    }
    Result<OpenedContainer> OpenFileContainer(ContainerRequest Request, OperationContext Context)
    {
        if (Request.entry_.id_.key_.empty()) {
            auto Entry = Request.source_->Resolve(Request.entry_.locator_, Context).get(); if (!Entry.HasValue()) return Entry.Failure(); Request.entry_ = Entry.Value();
        }
        ProviderContentSource Source(Request.source_, Request.entry_);
        auto Verified = Source.Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
        auto Hint = FormatHint(Source, Context); if (!Hint.HasValue()) return Hint.Failure();
        const auto Name = QString::fromStdString(Request.entry_.name_);
        std::stable_partition(Request.candidates_.begin(), Request.candidates_.end(), [&](const auto& Choice) { return ExtensionMatches(Choice, Name); });
        std::shared_ptr<IMaterializer> Materializer; std::shared_ptr<IMaterializedFileLease> Lease;
        const bool Native = (Request.entry_.locator_.kind_ == LocatorKind::Local || Request.entry_.locator_.kind_ == LocatorKind::Smb)
            && Request.entry_.locator_.nesting_.empty();
        QString NativePath = Native ? QString::fromStdString(Request.entry_.locator_.root_) : QString{};
        for (auto Choice : Request.candidates_) {
            if (const auto Stop = Context.Check(Request.entry_.id_.providerId_)) return *Stop;
            const bool Extension = ExtensionMatches(Choice, Name);
            if (!Extension && !Choice.content_) continue;
            // ISO's WCX reports BAD_DATA for foreign files. Do not confuse that with a damaged ISO.
            if (Choice.options_.approval_.reviewed_.id_ == "hc.builtin.isoimage" && Hint.Value() != "iso" && (!Extension || !Hint.Value().isEmpty())) continue;
            if (Choice.options_.approval_.reviewed_.format_ == PluginFormat::UnEgg && Hint.Value() != "alz" && Hint.Value() != "egg" && (!Extension || !Hint.Value().isEmpty())) continue;
            const auto Rule = PluginApplicationRule::Parse(Choice.application_); if (!Rule.HasValue()) continue;
            auto Applies = Rule.Value().MatchesSource(Source, Context); if (!Applies.HasValue()) return Applies.Failure(); if (!Applies.Value()) continue;
            if (Choice.options_.approval_.reviewed_.format_ == PluginFormat::SevenZip && !BuiltinArchiveAllowed(Rule.Value(), Name)) continue;
            const bool Chain = !Choice.dopus_ && Request.entry_.locator_.kind_ == LocatorKind::Archive && !Request.ancestors_.empty();
            if (!Native && !Chain && !Lease) {
                auto Created = MakeNativeMaterializer(QDir::tempPath().toStdString()); if (!Created.HasValue()) return Created.Failure(); Materializer = Created.Value();
                auto Staged = Materializer->Materialize({std::make_shared<ProviderContentSource>(Request.source_, Request.entry_)}, Context).get();
                if (!Staged.HasValue()) return Staged.Failure(); Lease = Staged.Value(); NativePath = QString::fromStdString(Lease->LocalPath());
            }
            OpenedContainer Opened; Opened.choice_ = Choice; Opened.native_ = NativePath;
            if (Choice.dopus_) {
                auto Inspected = InspectManifestFile(Choice.options_.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
                if (QFileInfo(Choice.options_.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive)) return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch"};
                auto Started = PluginProcess::Start(Choice.options_.executable_, Choice.options_.manifestPath_, Choice.options_.approval_, Choice.options_.limits_, Context, Choice.options_.defaultIniPath_);
                if (!Started.HasValue()) return Started.Failure();
                auto Process = std::shared_ptr<PluginProcess>(std::move(Started.Value()));
                auto Store = MakeNativeContentStore(QDir::tempPath().toStdString()); if (!Store.HasValue()) return Store.Failure();
                auto Storage = MakeHostedDopusVfsProvider(Process, {QUuid::createUuid().toString(QUuid::Id128).toStdString()}, Store.Value(),
                    Native && Choice.options_.approval_.granted_.contains(Permission::WriteContent), NativePath, Hint.Value(), Context);
                if (!Storage.HasValue()) { if (Mismatch(Storage.Failure())) continue; return Storage.Failure(); }
                Opened.location_ = Storage.Value().root_; Opened.reader_ = Storage.Value().reader_;
                if (Native) Opened.writer_ = WorkerOwned(Storage.Value().writer_);
            } else {
                auto Options = Choice.options_; Options.passwordPrompt_ = AskArchivePassword;
                // Admission needs the reviewed manifest's grants. Only the mounted copy is read-only.
                if (!Native && !Chain) Opened.choice_.wcx_.hostApproval_.granted_.erase(Permission::WriteContent);
                Opened.ancestors_ = Chain ? Request.ancestors_ : WcxBackends{};
                Opened.backend_ = WorkerOwned(MakeApprovedWcxBackend(Options), Lease, Materializer);
                Opened.location_ = Chain ? Request.entry_.locator_ : Locator{LocatorKind::Archive, NativePath.toStdString()};
                Opened.location_.nesting_.push_back({"wcx", "/"});
                Opened.reader_ = MakeWcxReadProvider(Options.binaryPath_.toStdString(), Opened.location_, {}, Opened.backend_, Chain ? Request.ancestors_ : WcxBackends{});
            }
            auto Cursor = Opened.reader_->Enumerate(Opened.location_, Context).get();
            if (!Cursor.HasValue()) { if (Mismatch(Cursor.Failure())) continue; return Cursor.Failure(); }
            auto Entries = PagedEntryIndex::Build(*Cursor.Value(), Context);
            if (!Entries.HasValue()) { if (Mismatch(Entries.Failure())) continue; return Entries.Failure(); }
            Verified = Source.Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
            Opened.reader_ = WorkerOwned<IStorageProvider>(std::make_shared<PrimedProvider>(Opened.reader_, Opened.location_, Entries.Value()), Lease, Materializer);
            return Opened;
        }
        return Error{ErrorCategory::Unsupported, "file-container-not-supported"};
    }
}

namespace
{
    class ContainerNativeParent final : public nsHC::CFileSystemT
    {
    public:
        ContainerNativeParent(const QString& Root, bool Remote) : CFileSystemT(Root) {
            cate_ = Remote ? nsHC::FS_CATE_REMOTE : nsHC::FS_CATE_LOCAL;
            features_ = nsHC::TyEnFSFeatures(nsHC::FS_FEA_LIST) | nsHC::FS_FEA_READABLE | nsHC::FS_FEA_WRITABLE | nsHC::FS_FEA_MOVABLE | nsHC::FS_FEA_DELETABLE | nsHC::FS_FEA_RENAMABLE;
        }
    };
}
hc::storage::Result<hc::storage::Unit> CFSModel::EnterFile(const QModelIndex& Index, std::function<void()> BeforeMount)
{
    using namespace hc::storage;
    const auto Node = GetFileInfo(Index);
    if (!Node || (Node->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) return Error{ErrorCategory::InvalidRequest, "file-container-selection"};
    hc::navigation::ContainerRequest Request; const auto FullPath = GetFileFullPath(Index);
    if (Node->storageEntry_) { Request.entry_ = *Node->storageEntry_; Request.source_ = ProviderFor(Request.entry_); }
    else {
        Request.entry_.locator_ = {FullPath.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local, FullPath.toStdString()};
        Request.entry_.name_ = Node->Name_.toStdString(); Request.source_ = MakeNativeReadProvider(Request.entry_.locator_.kind_);
    }
    if (!Request.source_) return Error{ErrorCategory::Unsupported, "file-container-source-unavailable"};
    if (Request.entry_.locator_.nesting_.size() >= 8 || vecFs_.size() > 8) return Error{ErrorCategory::BudgetExceeded, "file-container-depth"};
    Request.candidates_ = hc::navigation::FileContainerCandidates();
    if (Request.candidates_.empty()) return Error{ErrorCategory::Unsupported, "file-container-not-supported"};
    if (const auto* Pack = dynamic_cast<nsHC::CFSPack*>(GetRoot().get())) Request.ancestors_ = Pack->ArchiveBackends();
    const auto Roots = vecFs_; const auto ParentPath = path_;
    const bool Native = Request.entry_.locator_.kind_ == LocatorKind::Local || Request.entry_.locator_.kind_ == LocatorKind::Smb;
    const bool Rebase = Native && (IsSearchResults() || QDir::cleanPath(QDir::fromNativeSeparators(GetCurrentPathWithRoot())).compare(QFileInfo(FullPath).absolutePath(), Qt::CaseInsensitive));
    if (!providerEnabled_) {
        // Reuse the supported legacy-to-provider transition before taking worker ownership.
        const QSignalBlocker Quiet(this); SetProviderEnabled(true);
    }
    CancelNavigation(true); navigationStop_ = std::stop_source{};
    const auto Generation = ++generation_; OperationContext Context; Context.stopToken_ = navigationStop_.get_token();
    automaticRefresh_ = autoRefreshRequested_ = false; navigationError_.reset(); loading_ = true; tracker_.SetBusy(true);
    emit DirectoryReadStarted(FullPath, true);
    using OpenResult = Result<hc::navigation::OpenedContainer>;
    auto* Watcher = new QFutureWatcher<OpenResult>(this); auto Stop = navigationStop_;
    connect(Watcher, &QObject::destroyed, [Stop]() mutable { Stop.request_stop(); });
    connect(Watcher, &QFutureWatcher<OpenResult>::finished, this, [this, Watcher, Generation, Roots, ParentPath, FullPath, Rebase, BeforeMount = std::move(BeforeMount)] {
        auto Result = Watcher->future().takeResult(); Watcher->deleteLater(); if (Generation != generation_) return;
        loading_ = false; tracker_.SetBusy(selectionInputActive_);
        if (!Result.HasValue()) {
            navigationError_ = Result.Failure(); emit sigNavigationFailed(QString::fromStdString(Result.Failure().code_)); emit sigRefreshCompleted();
            // An ordinary file without a handler keeps keyboard focus on the current listing.
            if (Result.Failure().category_ != ErrorCategory::Cancelled && Result.Failure().code_ != "file-container-not-supported")
                emit DirectoryReadFailed(FullPath, false);
            return;
        }
        QPointer<CFSModel> Self(this); if (BeforeMount) BeforeMount(); if (!Self) return;
        auto& Opened = Result.Value(); vecFs_ = Roots; path_ = ParentPath;
        if (Rebase) {
            const auto Parent = QDir::toNativeSeparators(QFileInfo(FullPath).absolutePath()); const bool Remote = Parent.startsWith("\\\\");
            const auto Drive = Remote ? QString("\\") : Parent.left(2);
            SetRoot(nsHC::TySpFileSystem(new ContainerNativeParent(Drive, Remote))); SetCurrentPath(Parent.mid(Drive.size()));
        }
        if (Opened.choice_.dopus_) {
            auto* Root = new nsHC::CFSPlugin(Opened.location_, Opened.reader_, Opened.writer_);
            Root->setProperty("hc.fileContainer", true); Root->setProperty("hc.containerParentPath", path_);
            AppendRoot(nsHC::TySpFileSystem(Root)); SetCurrentPath("\\");
            setProperty("hc.workspaceNativeRoot", Opened.native_);
        } else {
            const auto Wcx = TySpWCX(new CTCPlugInWCX);
            Wcx->LoadWCX(Opened.choice_.wcx_, Opened.backend_); Wcx->OpenArchive(QString::fromStdString(Opened.location_.root_));
            auto* Pack = new nsHC::CFSPack(FullPath, Wcx); Pack->SetParent(path_);
            Pack->SetArchiveChain(Opened.location_, Opened.ancestors_); Pack->UseReadProvider(Opened.reader_);
            AppendRoot(nsHC::TySpFileSystem(Pack)); SetCurrentPath("/");
        }
        setProperty("hc.workspaceProvider", Opened.choice_.options_.manifestPath_); Refresh();
    });
    Watcher->setFuture(QtConcurrent::run([Request = std::move(Request), Context]() mutable -> OpenResult {
        try { return hc::navigation::OpenFileContainer(std::move(Request), Context); }
        catch (const std::exception&) { return Error{ErrorCategory::Internal, "file-container-exception"}; }
    }));
    return Unit{};
}
