#include "wcxStorage.hpp"
#include "operationMetrics.hpp"
#include "commonLibs/plugins/archiveSelectionDigest.hpp"
#include "wcxBackend.hpp"
#include "storageProviders.hpp"
#include "nativeMaterializer.hpp"
#include <QtCore>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <set>

namespace hc::storage
{
    namespace
    {
        Result<std::string> EntryPath(const std::string& Name)
        {
            const auto Text = QString::fromStdString(Name);
            if (Text.isEmpty() || Text.size() >= 1024 || Text.toUtf8().toStdString() != Name) return Error{ErrorCategory::InvalidRequest, "invalid-archive-name", {"wcx"}};
            auto Path = Text; Path.replace('\\', '/');
            if (Path.startsWith('/') || Path.contains(':') || Path.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "archive-traversal", {"wcx"}};
            if (Path.endsWith('/')) Path.chop(1);
            const auto Parts = Path.split('/');
            if (Parts.size() > 32) return Error{ErrorCategory::BudgetExceeded, "archive-depth-budget", {"wcx"}};
            for (const auto& Part : Parts)
                if (Part.isEmpty() || Part == "." || Part == "..") return Error{ErrorCategory::InvalidRequest, "archive-traversal", {"wcx"}};
            return ("/" + Path).toStdString();
        }
        std::string ParentOf(const std::string& Path) { const auto Slash = Path.find_last_of('/'); return Slash == 0 ? "/" : Path.substr(0, Slash); }
        class Cursor final : public IEntryCursor
        {
        public:
            explicit Cursor(std::vector<EntrySnapshot> Entries) : entries_(std::move(Entries)) {}
            Task<EntryPage> Next(std::size_t Count, OperationContext Context) override
            {
                if (const auto Failure = Context.Check({"wcx"})) return Ready<EntryPage>(*Failure);
                Count = std::min({Count, Context.maxPageItems_, entries_.size() - offset_});
                if (!Count && offset_ != entries_.size()) return Ready<EntryPage>(Error{ErrorCategory::InvalidRequest, "zero-page-size"});
                EntryPage Page; Page.entries_.assign(entries_.begin() + offset_, entries_.begin() + offset_ + Count);
                offset_ += Count; Page.end_ = offset_ == entries_.size(); return Ready<EntryPage>(std::move(Page));
            }
        private:
            std::vector<EntrySnapshot> entries_;
            std::size_t offset_ = 0;
        };
        class LeasedStream final : public IReadStream
        {
        public:
            LeasedStream(std::shared_ptr<IReadStream> Stream, std::shared_ptr<IMaterializedFileLease> Lease)
                : lease_(std::move(Lease)), stream_(std::move(Stream)) {}
            bool CanSeek() const override { return stream_->CanSeek(); }
            Task<ReadChunk> Read(std::size_t Count, OperationContext Context) override { return stream_->Read(Count, std::move(Context)); }
            Task<Unit> Seek(std::uint64_t At, OperationContext Context) override { return stream_->Seek(At, std::move(Context)); }
        private:
            // Stream closes before the last lease can schedule deletion.
            std::shared_ptr<IMaterializedFileLease> lease_;
            std::shared_ptr<IReadStream> stream_;
        };
        class ArchiveStream final : public IReadStream {
        public:
            using Restart = std::function<Result<std::unique_ptr<hc::plugins::IArchiveReadSession>>(OperationContext)>;
            ArchiveStream(std::shared_ptr<hc::plugins::IArchiveReadSession> Reader,std::uint32_t Ordinal,std::uint64_t Size,
                std::shared_ptr<IReadStream> Pin = {},Restart Reopen = {})
                : sourcePin_(std::move(Pin)),reader_(std::move(Reader)),ordinal_(Ordinal),size_(Size),restart_(std::move(Reopen)) {}
            bool CanSeek() const override { return bool(restart_); }
            // Seeking a single member replays the decoder instead of creating a
            // disk copy. Batch copies remain sequential; callers needing cheap
            // random access still see the underlying forward decoder constraint.
            ReadAccess Access() const override { return restart_?ReadAccess::ForwardSeek:ReadAccess::Sequential; }
            Task<Unit> Seek(std::uint64_t Offset,OperationContext Context) override {
                if (!restart_ || Offset>size_) return Ready<Unit>(Error{ErrorCategory::Unsupported,"archive-stream-seek"});
                if (const auto Stop=Context.Check({"wcx"})) return Ready<Unit>(*Stop);
                if (Offset<position_ || eof_) {
                    reader_.reset(); auto Opened=restart_(Context); if (!Opened.HasValue()) { error_=Opened.Failure(); return Ready<Unit>(*error_); }
                    reader_=std::shared_ptr<hc::plugins::IArchiveReadSession>(std::move(Opened.Value())); position_=0; eof_=false; error_.reset();
                }
                while (position_<Offset) {
                    auto Chunk=read(std::min<std::uint64_t>(Offset-position_,1024*1024),Context);
                    if (!Chunk.HasValue()) return Ready<Unit>(Chunk.Failure());
                    if (Chunk.Value().bytes_.empty() && position_<Offset) return Ready<Unit>(Error{ErrorCategory::StaleRevision,"archive-stream-size"});
                }
                return Ready<Unit>(Unit{});
            }
            Task<ReadChunk> Read(std::size_t Count,OperationContext Context) override { return Ready<ReadChunk>(read(Count,std::move(Context))); }
        private:
            Result<ReadChunk> read(std::size_t Count,OperationContext Context) {
                if (error_) return *error_;
                if (eof_) return ReadChunk{{},true};
                operations::OperationMetric Timing("archive.stream.read");
                auto Result=reader_->Read(ordinal_,Count,std::move(Context));
                if (Result.HasValue()) {
                    if (Result.Value().bytes_.size()>size_-position_) return Error{ErrorCategory::StaleRevision,"archive-stream-size"};
                    position_+=Result.Value().bytes_.size(); eof_=Result.Value().eof_; Timing.SetBytes(Result.Value().bytes_.size());
                    if (eof_ && position_!=size_) return Error{ErrorCategory::StaleRevision,"archive-stream-size"};
                }
                return Result;
            }
            std::shared_ptr<IReadStream> sourcePin_; std::shared_ptr<hc::plugins::IArchiveReadSession> reader_;
            std::uint32_t ordinal_; std::uint64_t size_,position_=0; Restart restart_; bool eof_=false; std::optional<Error> error_;
        };
        class BulkStream final : public IReadStream {
        public:
            BulkStream(std::shared_ptr<IReadStream> Stream, std::shared_ptr<QTemporaryDir> Directory) : directory_(std::move(Directory)), stream_(std::move(Stream)) {}
            bool CanSeek() const override { return stream_->CanSeek(); }
            Task<Unit> Seek(std::uint64_t At, OperationContext Context) override { return stream_->Seek(At, Context); }
            Task<ReadChunk> Read(std::size_t Count, OperationContext Context) override {
                operations::OperationMetric Timing("archive.staged.read"); auto Result=stream_->Read(Count,Context).get();
                if (Result.HasValue()) Timing.SetBytes(Result.Value().bytes_.size()); return Ready<ReadChunk>(std::move(Result));
            }
        private:
            std::shared_ptr<QTemporaryDir> directory_;
            std::shared_ptr<IReadStream> stream_;
        };
        class WcxProvider final : public IStorageProvider, public std::enable_shared_from_this<WcxProvider>
        {
        public:
            WcxProvider(std::string Plugin, Locator Root, std::string Temporary, std::shared_ptr<hc::plugins::IArchiveBackend> Backend, WcxBackends Ancestors, bool BatchRead, ArchiveReadPolicy Policy)
                : plugin_(std::move(Plugin)), root_(std::move(Root)), temporary_(std::move(Temporary)),
                session_(QUuid::createUuid().toString(QUuid::Id128).toStdString() + ":"),
                native_(MakeNativeReadProvider(root_.root_.starts_with("\\\\") || root_.root_.starts_with("//") ? LocatorKind::Smb : LocatorKind::Local)),
                backend_(Backend ? std::move(Backend) : hc::plugins::MakeInProcessWcxBackend(plugin_)), ancestors_(std::move(Ancestors)) {
                batchRead_ = BatchRead; streamUnsupported_=Policy==ArchiveReadPolicy::Materialized;}
            Task<WcxContainer> Container(OperationContext Context)
            {
                return run<WcxContainer>(Context, [](WcxProvider& Self, OperationContext Context) -> Result<WcxContainer> {
                    auto Mounted = Self.mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
                    return WcxContainer{Self.root_, Self.archive_, Self.native_, Self.backend_};
                });
            }
            ProviderId Id() const override { return {"wcx"}; }
            Task<SupportDecision> Supports(Operation Op, OperationContext Context) override
            {
                if (const auto Failure = Context.Check(Id())) return Ready<SupportDecision>(*Failure);
                return Ready<SupportDecision>(SupportDecision{Op == Operation::Write ? Availability::Unsupported : Availability::Conditional,
                    Op == Operation::Write ? "archive-write-unavailable" : "wcx-background-unicode-read-required", Op == Operation::OpenRead});
            }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                return run<std::unique_ptr<IEntryCursor>>(Context, [At = std::move(At)](WcxProvider& Self, OperationContext Context) -> Result<std::unique_ptr<IEntryCursor>> {
                    auto Valid = Self.mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                    if (!Self.validLocation(At)) return Error{ErrorCategory::InvalidRequest, "archive-location"};
                    const auto& Path = At.nesting_.back().entry_;
                    if (Path != "/" && (!Self.entries_.contains(Path) || !Self.entries_.at(Path).directory_)) return Error{ErrorCategory::NotFound, "archive-directory-not-found"};
                    std::vector<EntrySnapshot> Items;
                    const auto Prefix = Path == "/" ? std::string("/") : Path + "/";
                    for (auto It=Self.entries_.lower_bound(Prefix); It!=Self.entries_.end() && It->first.starts_with(Prefix); ++It)
                        if (ParentOf(It->first)==Path) Items.push_back(It->second);
                    return std::unique_ptr<IEntryCursor>(new Cursor(std::move(Items)));
                });
            }
            Task<EntrySnapshot> Stat(ResourceId Id, OperationContext Context) override
            {
                return run<EntrySnapshot>(Context, [Id = std::move(Id)](WcxProvider& Self, OperationContext Context) { return Self.stat(Id, Context); });
            }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override
            {
                return run<EntrySnapshot>(Context, [At = std::move(At)](WcxProvider& Self, OperationContext Context) -> Result<EntrySnapshot> {
                    auto Valid = Self.mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                    if (!Self.validLocation(At) || !Self.entries_.contains(At.nesting_.back().entry_)) return Error{ErrorCategory::NotFound, "archive-entry-not-found"};
                    return Self.entries_.at(At.nesting_.back().entry_);
                });
            }
            bool UsesOrderedReads() const override { return !streamUnsupported_; }
            Task<Unit> PrepareReadBatch(std::vector<EntrySnapshot> Entries,OperationContext Context) override {
                return run<Unit>(Context,[Entries=std::move(Entries)](WcxProvider& Self,OperationContext Context)->Result<Unit> {
                    if (Entries.empty() || Context.workload_.IsAutomatic() || (Self.streamUnsupported_ && !Context.producingContent_)) return Unit{};
                    auto Mounted=Self.mount(Context); if (!Mounted.HasValue()) return Mounted.Failure();
                    std::vector<std::uint32_t> Ordinals;
                    std::map<std::uint32_t, hc::plugins::ArchiveHeader> SelectedHeaders;
                    for (const auto& Entry:Entries) { auto Fresh=Self.stat(Entry.id_,Context); if (!Fresh.HasValue()) return Fresh.Failure();
                        if (Fresh.Value().directory_) continue;
                        const auto Ordinal = std::uint32_t(std::stoul(Fresh.Value().hints_.at("ordinal"))); Ordinals.push_back(Ordinal);
                        if (Context.producingContent_) {
                            hc::plugins::ArchiveHeader Header; Header.name_ = Fresh.Value().hints_.at("wcxName"); Header.size_ = Fresh.Value().size_;
                            Header.crc_ = std::uint32_t(std::stoul(Fresh.Value().hints_.at("crc"))); SelectedHeaders.emplace(Ordinal, std::move(Header));
                        }
                    }
                    if (Ordinals.empty()) return Unit{};
                    std::sort(Ordinals.begin(),Ordinals.end()); Ordinals.erase(std::unique(Ordinals.begin(),Ordinals.end()),Ordinals.end());
                    auto Direct = [&]() -> Result<Unit> {
                        hc::plugins::ArchiveSelectionDigest Digest;
                        for (const auto& [Ordinal, Header] : SelectedHeaders) Digest.Add(Ordinal, Header);
                        Self.directDigest_ = Digest.Result(); Self.directOrdinals_ = std::move(Ordinals); return Unit{};
                    };
                    if (Context.producingContent_ && Self.streamUnsupported_) return Direct();
                    auto Pinned=Context; Pinned.denyConcurrentMutation_=true; auto Pin=Self.native_->OpenRead(Self.archive_.id_,Pinned).get(); if (!Pin.HasValue()) return Pin.Failure();
                    // A native SDK stream already writes to the destination
                    // session. Measured file adoption costs more than its IPC.
                    auto Opened=Self.backend_->OpenReadSession(Self.archivePath_,Ordinals,Context);
                    if (!Opened.HasValue()) {
                        if (Opened.Failure().category_!=ErrorCategory::Unsupported || Opened.Failure().code_!="archive-stream-unsupported") return Opened.Failure();
                        Self.streamUnsupported_=true;
                        if (Context.producingContent_) { Self.directSourcePin_=std::move(Pin.Value()); return Direct(); }
                        return Unit{};
                    }
                    Self.bulkPin_=std::move(Pin.Value()); Self.streamingBatch_=std::shared_ptr<hc::plugins::IArchiveReadSession>(std::move(Opened.Value())); return Unit{};
                });
            }
            Task<Unit> FinishReadBatch(OperationContext Context) override {
                return run<Unit>(Context,[](WcxProvider& Self,OperationContext)->Result<Unit> {
                    Self.streamingBatch_.reset(); Self.directPin_.reset(); Self.directBulk_.reset(); Self.directSourcePin_.reset(); Self.directOrdinals_.clear();
                    if (!Self.batchRead_) Self.bulkPin_.reset(); return Unit{};
                });
            }
            Task<std::optional<ContentProducer>> PrepareContent(ResourceId Id, OperationContext Context) override
            {
                return run<std::optional<ContentProducer>>(Context, [Id = std::move(Id)](WcxProvider& Self, OperationContext Context) -> Result<std::optional<ContentProducer>> {
                    if (Context.workload_.IsAutomatic() || Self.streamingBatch_) return std::optional<ContentProducer>{};
                    auto Entry = Self.stat(Id, Context); if (!Entry.HasValue()) return Entry.Failure();
                    if (Entry.Value().directory_) return std::optional<ContentProducer>{};
                    Context.denyConcurrentMutation_ = true;
                    if (!Self.directSourcePin_) {
                        auto Pin = Self.native_->OpenRead(Self.archive_.id_, Context).get(); if (!Pin.HasValue()) return Pin.Failure();
                        Self.directSourcePin_ = std::move(Pin.Value());
                    }
                    ContentProducer Producer;
                    Producer.take_ = [Owner = Self.shared_from_this(), Entry = std::move(Entry.Value()), Pin = Self.directSourcePin_]
                        (const std::string& Parent, OperationContext Context) -> Result<ProducedContent> {
                        return Owner->run<ProducedContent>(Context, [Entry, Parent](WcxProvider& Self, OperationContext Context) -> Result<ProducedContent> {
                            if (!Self.directBulk_) {
                                auto Directory = std::make_shared<QTemporaryDir>(QDir(QString::fromStdString(Parent)).filePath("hc-wcx-direct-XXXXXX"));
                                if (!Directory->isValid()) return Error{ErrorCategory::Unavailable, "archive-direct-directory"};
                                const auto Path = QDir::toNativeSeparators(Directory->path()).toStdWString();
                                const auto Handle = CreateFileW(Path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                                if (Handle == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "archive-direct-directory-pin"};
                                auto Pin = std::shared_ptr<void>(Handle, [](void* Value) { CloseHandle(Value); });
                                if (!Self.directOrdinals_.empty() && !Self.directUnsupported_) {
                                    operations::OperationMetric Timing("archive.direct.decode");
                                    auto Extracted = Self.backend_->ExtractSelected(Self.archivePath_, Directory->path().toStdString(), Self.directOrdinals_, Context);
                                    if (!Extracted.HasValue()) {
                                        if (Extracted.Failure().category_ != ErrorCategory::Unsupported || Extracted.Failure().code_ != "archive-selected-extract-unsupported")
                                            return Extracted.Failure();
                                        Self.directUnsupported_ = true;
                                    } else {
                                        QFile Digest(QDir(Directory->path()).filePath("selection.sha256"));
                                        if (!Digest.open(QIODevice::ReadOnly) || Digest.read(65) != Self.directDigest_)
                                            return Error{ErrorCategory::StaleRevision, "archive-direct-selection-changed"};
                                    }
                                }
                                Self.directBulk_ = std::move(Directory); Self.directPin_ = std::move(Pin);
                            }
                            const auto From = QDir(Self.directBulk_->path()).filePath(QString::fromStdString(Entry.hints_.at("ordinal")) + ".bin");
                            if (Self.directOrdinals_.empty() || Self.directUnsupported_) {
                                hc::plugins::ArchiveHeader Expected; Expected.name_ = Entry.hints_.at("wcxName"); Expected.size_ = Entry.size_;
                                Expected.crc_ = std::uint32_t(std::stoul(Entry.hints_.at("crc")));
                                auto Extracted = Self.backend_->Extract(Self.archivePath_, std::move(Expected), From.toStdString(), Context);
                                if (!Extracted.HasValue()) return Extracted.Failure();
                            }
                            return ProducedContent{From.toStdString(), Self.shared_from_this()};
                        }).get();
                    };
                    return std::optional<ContentProducer>{std::move(Producer)};
                });
            }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Id, OperationContext Context) override
            {
                return run<std::shared_ptr<IReadStream>>(Context, [Id = std::move(Id)](WcxProvider& Self, OperationContext Context) -> Result<std::shared_ptr<IReadStream>> {
                    auto Entry = Self.stat(Id, Context); if (!Entry.HasValue()) return Entry.Failure();
                    if (Entry.Value().directory_) return Error{ErrorCategory::Unsupported, "file-required"};
                    if (!Self.streamUnsupported_) {
                        const auto Ordinal=std::uint32_t(std::stoul(Entry.Value().hints_.at("ordinal")));
                        auto Reader=Context.orderedBatchRead_?Self.streamingBatch_:std::shared_ptr<hc::plugins::IArchiveReadSession>{};
                        std::shared_ptr<IReadStream> StreamPin; ArchiveStream::Restart Restart;
                        if (!Reader) {
                            auto Pinned=Context; Pinned.denyConcurrentMutation_=true; auto Pin=Self.native_->OpenRead(Self.archive_.id_,Pinned).get(); if (!Pin.HasValue()) return Pin.Failure();
                            auto Opened=Self.backend_->OpenReadSession(Self.archivePath_,{Ordinal},Context);
                            if (!Opened.HasValue()) {
                                if (Opened.Failure().category_!=ErrorCategory::Unsupported || Opened.Failure().code_!="archive-stream-unsupported") return Opened.Failure();
                                Self.streamUnsupported_=true;
                            } else { Reader=std::shared_ptr<hc::plugins::IArchiveReadSession>(std::move(Opened.Value())); StreamPin=std::move(Pin.Value());
                                Restart=[Backend=Self.backend_,Path=Self.archivePath_,Ordinal](OperationContext Context){return Backend->OpenReadSession(Path,{Ordinal},std::move(Context));}; }
                        }
                        if (Reader) return std::shared_ptr<IReadStream>(new ArchiveStream(std::move(Reader),Ordinal,Entry.Value().size_,std::move(StreamPin),std::move(Restart)));
                    }
                    if (Self.batchRead_ && !Self.batchUnsupported_ && !Context.workload_.IsAutomatic()) {
                        if (!Self.bulk_) {
                            auto Base=QDir::fromNativeSeparators(QString::fromStdString(Self.temporary_));
                            const auto Volume=QDir::toNativeSeparators(Base.left(3));
                            if (Base.size()<3 || Base[1]!=':' || GetDriveTypeW(reinterpret_cast<LPCWSTR>(Volume.utf16()))!=DRIVE_FIXED) Base=QDir::tempPath();
                            auto Directory=std::make_shared<QTemporaryDir>(QDir(Base).filePath("hc-wcx-bulk-XXXXXX"));
                            if (!Directory->isValid()) Directory=std::make_shared<QTemporaryDir>(QDir(QDir::tempPath()).filePath("hc-wcx-bulk-XXXXXX"));
                            if (!Directory->isValid()) return Error{ErrorCategory::Unavailable,"archive-batch-stage-directory"};
                            const auto NativePath=QDir::toNativeSeparators(Directory->path());
                            SetFileAttributesW(reinterpret_cast<LPCWSTR>(NativePath.utf16()),FILE_ATTRIBUTE_HIDDEN);
                            auto Pinned=Context; Pinned.denyConcurrentMutation_=true;
                            auto Pin=Self.native_->OpenRead(Self.archive_.id_,Pinned).get(); if(!Pin.HasValue()) return Pin.Failure();
                            std::uint64_t Unpacked=0; for (const auto& [Key,Item]:Self.entries_) if (!Item.directory_) Unpacked+=Item.size_;
                            Result<Unit> Extracted=Unit{};
                            { operations::OperationMetric Timing("archive.materialize",Unpacked); Extracted=Self.backend_->ExtractAll(Self.archivePath_,Directory->path().toStdString(),Context); }
                            if (!Extracted.HasValue()) {
                                if (Extracted.Failure().category_!=ErrorCategory::Unsupported || Extracted.Failure().code_!="archive-batch-extract-unsupported") return Extracted.Failure();
                                Self.batchUnsupported_=true;
                            } else { Self.bulk_=std::move(Directory); Self.bulkPin_=std::move(Pin.Value()); }
                        }
                        if (Self.bulk_) {
                            const auto Path=QDir(Self.bulk_->path()).filePath(QString::fromStdString(Entry.Value().hints_.at("ordinal"))+".bin");
                            auto Reader=MakeNativeReadProvider(LocatorKind::Local); auto Local=Reader->Resolve({LocatorKind::Local,Path.toStdString()},Context).get();
                            if(!Local.HasValue())return Local.Failure();
                            if(Local.Value().directory_ || Local.Value().size_!=Entry.Value().size_)return Error{ErrorCategory::StaleRevision,"archive-batch-stage-size"};
                            auto Stream=Reader->OpenRead(Local.Value().id_,Context).get();if(!Stream.HasValue())return Stream.Failure();
                            return std::shared_ptr<IReadStream>(new BulkStream(std::move(Stream.Value()),Self.bulk_));
                        }
                    }
                    if (!Self.store_)
                    {
                        auto Store = MakeNativeContentStore(Self.batchRead_ || Self.temporary_.empty() ? QDir::tempPath().toStdString() : Self.temporary_);
                        if (!Store.HasValue()) return Store.Failure(); Self.store_ = std::move(Store.Value());
                    }
                    const auto Packed = std::stoull(Entry.Value().hints_.at("packedSize"));
                    auto Lease = Self.store_->Produce(Entry.Value(), {}, Packed, Context, [&](const std::string& Output, OperationContext Context) {
                        return Self.extract(Entry.Value(), Output, Context);
                    });
                    if (!Lease.HasValue()) return Lease.Failure();
                    auto Reader = MakeNativeReadProvider(LocatorKind::Local);
                    auto Local = Reader->Resolve({LocatorKind::Local, Lease.Value()->LocalPath()}, Context).get(); if (!Local.HasValue()) return Local.Failure();
                    auto Stream = Reader->OpenRead(Local.Value().id_, Context).get(); if (!Stream.HasValue()) return Stream.Failure();
                    return std::shared_ptr<IReadStream>(new LeasedStream(std::move(Stream.Value()), std::move(Lease.Value())));
                });
            }
        private:
            template<class T, class F> Task<T> run(OperationContext Context, F Work)
            {
                return std::async(std::launch::async, [Self = shared_from_this(), Context, Work = std::move(Work)]() mutable -> Result<T> {
                    try
                    {
                        std::unique_lock Lock(Self->mutex_, std::defer_lock);
                        while (!Lock.try_lock_for(std::chrono::milliseconds(25))) if (const auto Failure = Context.Check(Self->Id())) return *Failure;
                        if (const auto Failure = Context.Check(Self->Id())) return *Failure;
                        return Work(*Self, Context);
                    }
                    catch (...) { return Error{ErrorCategory::Internal, "wcx-provider-exception", {"wcx"}}; }
                });
            }
            bool validLocation(const Locator& At) const
            {
                if (At.kind_ != LocatorKind::Archive || At.root_ != root_.root_ || At.nesting_.empty() || At.nesting_.size() > 8 || At.nesting_.size() != root_.nesting_.size()) return false;
                for (std::size_t Index = 0; Index < At.nesting_.size(); ++Index)
                    if (At.nesting_[Index].provider_ != "wcx" || (Index + 1 < At.nesting_.size() && At.nesting_[Index] != root_.nesting_[Index])) return false;
                return true;
            }
            EntrySnapshot makeEntry(const std::string& Path, bool Directory) const
            {
                EntrySnapshot Entry; Entry.locator_ = root_; Entry.locator_.nesting_.back().entry_ = Path;
                Entry.name_ = Path.substr(Path.find_last_of('/') + 1); Entry.directory_ = Directory;
                Entry.attributes_ = Directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                Entry.id_ = {Id(), session_ + Path, IdentityScope::NavigationSession, ResourceRevision{archive_.id_.revision_->token_ + ":" + Path}};
                return Entry;
            }
            Result<Unit> mount(OperationContext Context)
            {
                if (mounted_) { if (bulkPin_) return Unit{}; const auto Fresh = native_->Stat(archive_.id_, Context).get(); return Fresh.HasValue() ? Result<Unit>(Unit{}) : Result<Unit>(Fresh.Failure()); }
                if (!validLocation(root_)) return Error{ErrorCategory::InvalidRequest, "archive-location"};
                Locator Container{native_->Id().value_ == "smb" ? LocatorKind::Smb : LocatorKind::Local, root_.root_};
                archivePath_ = root_.root_;
                if (root_.nesting_.size() > 1)
                {
                    if (!ancestors_.empty() && ancestors_.size() + 1 != root_.nesting_.size()) return Error{ErrorCategory::InvalidRequest, "archive-backend-chain"};
                    Container = root_; Container.nesting_.pop_back();
                    auto ParentRoot = Container; ParentRoot.nesting_.back().entry_ = "/";
                    auto Parents = ancestors_; auto Backend = backend_;
                    if (!Parents.empty()) { Backend = Parents.back(); Parents.pop_back(); if (!Backend) return Error{ErrorCategory::InvalidRequest, "archive-backend-required"}; }
                    native_ = MakeWcxReadProvider(plugin_, ParentRoot, temporary_, std::move(Backend), std::move(Parents));
                }
                auto Archive = native_->Resolve(Container, Context).get();
                if (!Archive.HasValue()) return Archive.Failure();
                if (Archive.Value().directory_ || Archive.Value().attributes_ & FILE_ATTRIBUTE_REPARSE_POINT || !Archive.Value().id_.revision_)
                    return Error{ErrorCategory::Unsupported, "archive-native-file-required"};
                archive_ = std::move(Archive.Value());
                Context.denyConcurrentMutation_ = true;
                auto Pin = native_->OpenRead(archive_.id_, Context).get(); if (!Pin.HasValue()) return Pin.Failure();
                // Whole extraction owns a frozen container. Holding the read pin
                // avoids opening/statting the same multi-GiB archive per child.
                if (batchRead_ && !Context.workload_.IsAutomatic() && root_.nesting_.size()==1) bulkPin_=Pin.Value();
                if (root_.nesting_.size() > 1)
                {
                    auto Store = MakeNativeContentStore(temporary_.empty() ? QDir::tempPath().toStdString() : temporary_); if (!Store.HasValue()) return Store.Failure();
                    // OpenRead already enforces the parent's expansion ratio;
                    // this producer copies those uncompressed bytes verbatim.
                    auto Lease = Store.Value()->Produce(archive_, {}, archive_.size_, Context, [&](const std::string& Path, OperationContext Context) -> Result<Unit> {
                        QFile File(QString::fromStdString(Path)); if (!File.open(QIODevice::WriteOnly)) return Error{ErrorCategory::Unavailable, "nested-archive-stage-open"};
                        std::uint64_t Written = 0;
                        for (;;) {
                            auto Chunk = Pin.Value()->Read(std::min(Context.maxReadBytes_, std::size_t{1024 * 1024}), Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                            if (Written > archive_.size_ || Chunk.Value().bytes_.size() > archive_.size_ - Written) return Error{ErrorCategory::BudgetExceeded, "nested-archive-size"};
                            if (File.write(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()) != qint64(Chunk.Value().bytes_.size())) return Error{ErrorCategory::Unavailable, "nested-archive-stage-write"};
                            Written += Chunk.Value().bytes_.size(); if (Chunk.Value().eof_) break;
                            if (Chunk.Value().bytes_.empty()) return Error{ErrorCategory::InvalidRequest, "nested-archive-no-progress"};
                        }
                        if (Written != archive_.size_ || !File.flush()) return Error{ErrorCategory::Unavailable, "nested-archive-stage-size"};
                        auto Fresh = native_->Stat(archive_.id_, Context).get(); return Fresh.HasValue() ? Result<Unit>(Unit{}) : Result<Unit>(Fresh.Failure());
                    });
                    if (!Lease.HasValue()) return Lease.Failure(); containerLease_ = std::move(Lease.Value()); archivePath_ = containerLease_->LocalPath();
                }
                entries_.clear(); std::uint64_t Ordinal=0;
                const auto Scanned = scan(Context, [&](const hc::plugins::ArchiveHeader& Header, const std::string& Path) -> Result<bool> {
                    if (entries_.size() >= Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                    const bool Directory = Header.directory_;
                    if (entries_.contains(Path) && !(Directory && entries_.at(Path).directory_ && entries_.at(Path).hints_.empty()))
                        return Error{ErrorCategory::InvalidRequest, "duplicate-archive-entry"};
                    auto Entry = makeEntry(Path, Directory);
                    Entry.attributes_ = Header.attributes_; Entry.size_ = Header.size_;
                    Entry.modifiedUtcMs_ = Header.modifiedUtcMs_;
                    Entry.hints_["packedSize"] = std::to_string(Header.packedSize_);
                    Entry.hints_["crc"] = std::to_string(Header.crc_);
                    if (Header.encrypted_) Entry.hints_["encrypted"] = "1";
                    if (Header.metadataRename_) Entry.hints_["metadataRename"] = "1";
                    if (Header.rebuildEncrypted_) Entry.hints_["rebuildEncrypted"] = "1";
                    // Deletion must use the plugin's exact directory marker:
                    // some WCX plugins expose directories without a final slash.
                    Entry.hints_["wcxName"] = Header.name_;
                    Entry.hints_["ordinal"] = std::to_string(Ordinal++);
                    Entry.id_.revision_->token_ += ":" + Entry.hints_["crc"] + ":" + std::to_string(Entry.size_);
                    entries_[Path] = std::move(Entry);
                    for (auto Parent = ParentOf(Path); Parent != "/"; Parent = ParentOf(Parent))
                    {
                        if (entries_.contains(Parent) && !entries_.at(Parent).directory_) return Error{ErrorCategory::InvalidRequest, "archive-file-parent"};
                        if (!entries_.contains(Parent)) entries_[Parent] = makeEntry(Parent, true);
                        if (entries_.size() > Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget"};
                    }
                    return false;
                });
                if (!Scanned.HasValue()) { entries_.clear(); return Scanned.Failure(); }
                mounted_ = true; return Unit{};
            }
            Result<EntrySnapshot> stat(const ResourceId& Resource, OperationContext Context)
            {
                if (Resource.providerId_ != Id() || !Resource.key_.starts_with(session_)) return Error{ErrorCategory::NotFound, "foreign-session"};
                const auto Valid = mount(Context); if (!Valid.HasValue()) return Valid.Failure();
                const auto It = entries_.find(Resource.key_.substr(session_.size()));
                if (It == entries_.end()) return Error{ErrorCategory::NotFound, "archive-entry-not-found"};
                if (It->second.id_ != Resource) return Error{ErrorCategory::StaleRevision, "revision-changed"};
                return It->second;
            }
            template<class Visitor> Result<Unit> scan(OperationContext Context, Visitor Visit)
            {
                auto Cursor = backend_->Enumerate(archivePath_, false, Context); if (!Cursor.HasValue()) return Cursor.Failure();
                for (;;)
                {
                    auto Page = Cursor.Value()->Next(Context.maxPageItems_, Context); if (!Page.HasValue()) return Page.Failure();
                    if (Page.Value().entries_.empty() && !Page.Value().end_) return Error{ErrorCategory::InvalidRequest, "archive-cursor-no-progress"};
                    for (const auto& Header : Page.Value().entries_)
                    {
                        auto Name = EntryPath(Header.name_); if (!Name.HasValue()) return Name.Failure();
                        auto Done = Visit(Header, Name.Value()); if (!Done.HasValue()) return Done.Failure();
                        if (Done.Value()) return Unit{};
                    }
                    if (Page.Value().end_) return Unit{};
                }
            }
            Result<Unit> extract(const EntrySnapshot& Entry, const std::string& Output, OperationContext Context)
            {
                Context.denyConcurrentMutation_ = true;
                auto Pin = native_->OpenRead(archive_.id_, Context).get(); if (!Pin.HasValue()) return Pin.Failure();
                hc::plugins::ArchiveHeader Expected; Expected.name_ = Entry.hints_.at("wcxName"); Expected.size_ = Entry.size_;
                Expected.crc_ = static_cast<std::uint32_t>(std::stoul(Entry.hints_.at("crc")));
                // Output is the content store's empty reservation inside its
                // pinned private directory. TotalSQX skips existing targets;
                // let the plugin create the file, then the store validates it.
                if (!QFile::remove(QString::fromStdString(Output)))
                    return Error{ErrorCategory::Unavailable, "archive-staging-placeholder-remove"};
                return backend_->Extract(archivePath_, std::move(Expected), Output, Context);
            }
            std::string plugin_;
            Locator root_;
            std::string temporary_, session_;
            std::shared_ptr<IStorageProvider> native_;
            std::shared_ptr<INativeContentStore> store_;
            std::shared_ptr<hc::plugins::IArchiveBackend> backend_;
            WcxBackends ancestors_;
            std::shared_ptr<IMaterializedFileLease> containerLease_;
            std::string archivePath_;
            EntrySnapshot archive_;
            std::map<std::string, EntrySnapshot> entries_;
            std::shared_ptr<QTemporaryDir> bulk_;
            std::shared_ptr<QTemporaryDir> directBulk_;
            std::shared_ptr<void> directPin_;
            std::shared_ptr<IReadStream> directSourcePin_;
            QByteArray directDigest_;
            std::vector<std::uint32_t> directOrdinals_;
            bool directUnsupported_ = false;
            std::shared_ptr<IReadStream> bulkPin_;
            bool batchRead_ = false, batchUnsupported_ = false, streamUnsupported_ = false;
            std::shared_ptr<hc::plugins::IArchiveReadSession> streamingBatch_;
            bool mounted_ = false;
            std::recursive_timed_mutex mutex_;
        };
    }
    std::shared_ptr<IStorageProvider> MakeWcxReadProvider(std::string PluginPath, Locator Root, std::string TemporaryBase, std::shared_ptr<hc::plugins::IArchiveBackend> Backend, WcxBackends Ancestors, bool BatchRead, ArchiveReadPolicy Policy)
    { return std::make_shared<WcxProvider>(std::move(PluginPath), std::move(Root), std::move(TemporaryBase), std::move(Backend), std::move(Ancestors), BatchRead, Policy); }
    Result<WcxContainer> ResolveWcxContainer(const std::shared_ptr<IStorageProvider>& Reader, OperationContext Context)
    {
        auto Wcx = std::dynamic_pointer_cast<WcxProvider>(Reader);
        if (!Wcx) return Error{ErrorCategory::InvalidRequest, "wcx-container-provider-required"};
        return Wcx->Container(std::move(Context)).get();
    }
}
