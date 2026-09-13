#include "hostedDataProviders.hpp"
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonArray>
#include <QSet>
#include <cstring>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"hosted-data"}}; }
        Error Unsupported(const char* Code) { return {ErrorCategory::Unsupported, Code, {"hosted-data"}}; }
        QString S(const std::string& Text) { return QString::fromStdString(Text); }
        std::string Root(ProviderId Id, const std::string& Scheme = "wfx") { return Scheme + ":" + Id.value_ + ":"; }
        Result<QString> Remote(const Locator& At, ProviderId Id, const std::string& Scheme = "wfx")
        {
            const auto Prefix = Root(Id, Scheme);
            if (At.kind_ != LocatorKind::Plugin || !At.nesting_.empty() || !At.root_.starts_with(Prefix)) return Invalid("hosted-storage-locator");
            const auto Name = S(At.root_.substr(Prefix.size()));
            if (!Name.startsWith('\\') || Name.startsWith("\\\\") || Name.size() > 1023 || Name.contains(QChar::Null) || Name.contains('/')) return Invalid("hosted-storage-path");
            for (const auto& Part : Name.split('\\')) if (Part == "." || Part == "..") return Invalid("hosted-storage-path-traversal");
            return Name;
        }
        Result<EntrySnapshot> DecodeEntry(QJsonObject Object, Locator Parent, ProviderId Id)
        {
            const auto Name = Object["name"].toString();
            if (Name.isEmpty() || Name == "." || Name == ".." || Name.contains('\\') || Name.contains('/') || Name.contains(QChar::Null)) return Invalid("hosted-storage-entry-name");
            EntrySnapshot Entry; Entry.name_ = Name.toStdString(); Entry.locator_ = Parent;
            if (!Entry.locator_.root_.ends_with('\\')) Entry.locator_.root_ += '\\'; Entry.locator_.root_ += Entry.name_;
            Entry.id_ = {Id, Entry.locator_.root_, IdentityScope::NavigationSession};
            bool Parsed = false; Entry.size_ = Object["size"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("hosted-storage-entry-size");
            const auto Modified = Object["modified"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("hosted-storage-entry-time");
            const auto Created = Object["created"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("hosted-storage-entry-time");
            Entry.attributes_ = std::uint32_t(Object["attributes"].toInteger()); Entry.directory_ = (Entry.attributes_ & 16) != 0;
            Entry.modifiedUtcMs_ = std::int64_t(Modified / 10000) - 11644473600000LL;
            Entry.createdUtcMs_ = std::int64_t(Created / 10000) - 11644473600000LL;
            Entry.id_.revision_ = ResourceRevision{std::to_string(Modified) + ":" + std::to_string(Entry.size_) + ":" + std::to_string(Entry.attributes_)};
            return Entry;
        }
        class Cursor final : public IEntryCursor
        {
        public:
            Cursor(std::shared_ptr<PluginProcess> Process, ProviderId Id, Locator At, int Handle, std::size_t Budget)
                : process_(std::move(Process)), id_(std::move(Id)), at_(std::move(At)), handle_(Handle), budget_(Budget) {}
            ~Cursor() override { if (handle_) process_->DataCall(WireKind::StorageEnumerate, {{"action", "close"}, {"cursor", handle_}}); }
            Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(id_)) return Ready<EntryPage>(*Stop);
                if (!MaxItems || !Context.maxPageItems_) return Ready<EntryPage>(Invalid("hosted-storage-page-size"));
                EntryPage Page; Page.end_ = !handle_ || end_; if (Page.end_) return Ready<EntryPage>(std::move(Page));
                auto Reply = process_->DataCall(WireKind::StorageEnumerate, {{"action", "next"}, {"cursor", handle_},
                    {"count", int(std::min({MaxItems, Context.maxPageItems_, std::size_t(256)}))}}, Context);
                if (!Reply.HasValue()) return Ready<EntryPage>(Reply.Failure());
                if (!Reply.Value()["entries"].isArray() || !Reply.Value()["end"].isBool()) return Ready<EntryPage>(Invalid("hosted-storage-page-envelope"));
                for (const auto& Item : Reply.Value()["entries"].toArray())
                {
                    if (++seen_ > std::min(budget_, Context.maxEnumerationEntries_)) return Ready<EntryPage>(Error{ErrorCategory::BudgetExceeded, "hosted-storage-enumeration-budget", id_});
                    auto Entry = DecodeEntry(Item.toObject(), at_, id_); if (!Entry.HasValue()) return Ready<EntryPage>(Entry.Failure());
                    Page.entries_.push_back(std::move(Entry.Value()));
                }
                Page.end_ = end_ = Reply.Value()["end"].toBool(); return Ready<EntryPage>(std::move(Page));
            }
        private:
            std::shared_ptr<PluginProcess> process_;
            ProviderId id_; Locator at_; int handle_;
            std::size_t budget_, seen_ = 0;
            bool end_ = false;
        };
        class LeaseStream final : public IReadStream
        {
        public:
            explicit LeaseStream(std::shared_ptr<IMaterializedFileLease> Lease) : lease_(std::move(Lease)) {}
            bool CanSeek() const override { return true; }
            Task<ReadChunk> Read(std::size_t MaxBytes, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(lease_->Describe().id_.providerId_)) return Ready<ReadChunk>(*Stop);
                const auto Count = std::min({MaxBytes, Context.maxReadBytes_, std::size_t(1024 * 1024)});
                if (!Count) return Ready<ReadChunk>(Invalid("hosted-storage-read-size"));
                QFile File(S(lease_->LocalPath()));
                if (!File.open(QIODevice::ReadOnly) || !File.seek(qint64(offset_))) return Ready<ReadChunk>(Error{ErrorCategory::Unavailable, "hosted-storage-lease-read"});
                const auto Bytes = File.read(qint64(Count)); if (File.error() != QFileDevice::NoError) return Ready<ReadChunk>(Error{ErrorCategory::Unavailable, "hosted-storage-lease-read"});
                ReadChunk Chunk; Chunk.bytes_.resize(Bytes.size()); std::memcpy(Chunk.bytes_.data(), Bytes.constData(), Bytes.size());
                offset_ += Bytes.size(); Chunk.eof_ = offset_ >= lease_->Describe().size_; return Ready<ReadChunk>(std::move(Chunk));
            }
            Task<Unit> Seek(std::uint64_t Offset, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(lease_->Describe().id_.providerId_)) return Ready<Unit>(*Stop);
                if (Offset > lease_->Describe().size_) return Ready<Unit>(Invalid("hosted-storage-seek")); offset_ = Offset; return Ready<Unit>(Unit{});
            }
        private:
            std::shared_ptr<IMaterializedFileLease> lease_;
            std::uint64_t offset_ = 0;
        };
        enum class StorageFeature { ReadFile, WriteFile, CreateDirectory, DeleteFile, RemoveDirectory, Rename, Attributes, Modified };
        class HostedStorageProvider final : public IStorageProvider, public IWritableStorage, public std::enable_shared_from_this<HostedStorageProvider>
        {
        public:
            HostedStorageProvider(std::shared_ptr<PluginProcess> Process, ProviderId Id, std::shared_ptr<INativeContentStore> Store, std::set<StorageFeature> Features, bool Writable, std::string Scheme = "wfx")
                : process_(std::move(Process)), id_(std::move(Id)), store_(std::move(Store)), features_(std::move(Features)), writable_(Writable), scheme_(std::move(Scheme)) {}
            ProviderId Id() const override { return id_; }
            Task<SupportDecision> Supports(Operation Request, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(id_)) return Ready<SupportDecision>(*Stop);
                const bool Available = Request != Operation::Write && (Request != Operation::OpenRead || features_.contains(StorageFeature::ReadFile));
                return Ready<SupportDecision>(SupportDecision{Available ? Availability::Supported : Availability::Unsupported, Available ? "" : "hosted-storage-capability-unavailable", Request == Operation::OpenRead});
            }
            Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
            {
                auto Path = Remote(At, id_, scheme_); if (!Path.HasValue()) return Ready<std::unique_ptr<IEntryCursor>>(Path.Failure());
                auto Reply = process_->DataCall(WireKind::StorageEnumerate, {{"action", "open"}, {"path", Path.Value()}}, Context);
                if (!Reply.HasValue()) return Ready<std::unique_ptr<IEntryCursor>>(Reply.Failure());
                const int Handle = Reply.Value()["cursor"].toInt(-1); if (Handle < 0) return Ready<std::unique_ptr<IEntryCursor>>(Invalid("hosted-storage-cursor-envelope"));
                return Ready<std::unique_ptr<IEntryCursor>>(std::unique_ptr<IEntryCursor>(std::make_unique<Cursor>(process_, id_, std::move(At), Handle, Context.maxEnumerationEntries_)));
            }
            Task<EntrySnapshot> Resolve(Locator At, OperationContext Context) override
            {
                auto Path = Remote(At, id_, scheme_); if (!Path.HasValue()) return Ready<EntrySnapshot>(Path.Failure());
                if (Path.Value() == "\\")
                {
                    if (const auto Stop = Context.Check(id_)) return Ready<EntrySnapshot>(*Stop);
                    EntrySnapshot Entry; Entry.locator_ = At; Entry.id_ = {id_, At.root_, IdentityScope::NavigationSession}; Entry.directory_ = true; Entry.name_ = id_.value_;
                    return Ready<EntrySnapshot>(std::move(Entry));
                }
                const auto Slash = At.root_.find_last_of('\\'); const auto Name = At.root_.substr(Slash + 1);
                Locator Parent = At; Parent.root_.resize(std::max(Slash, Root(id_, scheme_).size() + 1));
                auto Entries = Enumerate(Parent, Context).get(); if (!Entries.HasValue()) return Ready<EntrySnapshot>(Entries.Failure());
                for (;;)
                {
                    auto Page = Entries.Value()->Next(256, Context).get(); if (!Page.HasValue()) return Ready<EntrySnapshot>(Page.Failure());
                    for (auto& Entry : Page.Value().entries_) if (Entry.name_ == Name) return Ready<EntrySnapshot>(std::move(Entry));
                    if (Page.Value().end_) return Ready<EntrySnapshot>(Error{ErrorCategory::NotFound, "hosted-storage-entry-missing", id_});
                }
            }
            Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) override
            {
                if (Resource.providerId_ != id_) return Ready<EntrySnapshot>(Invalid("hosted-storage-resource-provider"));
                auto Fresh = Resolve({LocatorKind::Plugin, Resource.key_}, Context).get(); if (!Fresh.HasValue()) return Ready<EntrySnapshot>(Fresh.Failure());
                if (Resource.revision_ && Resource.revision_ != Fresh.Value().id_.revision_) return Ready<EntrySnapshot>(Error{ErrorCategory::StaleRevision, "hosted-storage-resource-changed", id_, Resource});
                return Ready<EntrySnapshot>(std::move(Fresh.Value()));
            }
            Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) override
            {
                if (!features_.contains(StorageFeature::ReadFile)) return Ready<std::shared_ptr<IReadStream>>(Unsupported("hosted-storage-get-unavailable"));
                auto Entry = Stat(Resource, Context).get(); if (!Entry.HasValue()) return Ready<std::shared_ptr<IReadStream>>(Entry.Failure());
                if (Entry.Value().directory_) return Ready<std::shared_ptr<IReadStream>>(Invalid("hosted-storage-read-directory"));
                auto Path = Remote(Entry.Value().locator_, id_, scheme_); if (!Path.HasValue()) return Ready<std::shared_ptr<IReadStream>>(Path.Failure());
                auto Lease = store_->Produce(Entry.Value(), {}, {}, Context, [&](const std::string& Local, OperationContext Operation) -> Result<Unit>
                {
                    const auto& Item = Entry.Value(); QJsonObject Info{{"size", QString::number(Item.size_)}, {"attributes", qint64(Item.attributes_)},
                        {"modified", QString::number((Item.modifiedUtcMs_ + 11644473600000LL) * 10000)}};
                    auto Reply = process_->DataCall(WireKind::StorageGet, {{"path", Path.Value()}, {"local", S(Local)}, {"info", Info}}, Operation);
                    if (!Reply.HasValue()) return Reply.Failure();
                    auto Fresh = Stat(Item.id_, Operation).get(); if (!Fresh.HasValue()) return Fresh.Failure(); return Unit{};
                });
                if (!Lease.HasValue()) return Ready<std::shared_ptr<IReadStream>>(Lease.Failure());
                return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(std::make_shared<LeaseStream>(std::move(Lease.Value()))));
            }
            Result<SupportDecision> Supports(Mutation Request, Locator At, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(id_)) return *Stop;
                auto Path = Remote(At, id_, scheme_); if (!Path.HasValue()) return Path.Failure();
                const auto Feature = Request == Mutation::CreateContent ? StorageFeature::WriteFile : Request == Mutation::MakeDirectory ? StorageFeature::CreateDirectory : Request == Mutation::Rename ? StorageFeature::Rename : StorageFeature::DeleteFile;
                const bool Available = writable_ && features_.contains(Feature);
                return SupportDecision{Available ? Availability::Conditional : Availability::Unsupported, Available ? "hosted-storage-no-atomic-revision-commit" : "hosted-storage-write-unavailable",
                    Request == Mutation::CreateContent, false, {"remote provider has no atomic compare-and-swap or rollback"}};
            }
            Result<std::optional<EntrySnapshot>> Inspect(Locator At, OperationContext Context) override
            {
                auto Value = Resolve(std::move(At), Context).get(); if (Value.HasValue()) return std::optional<EntrySnapshot>{std::move(Value.Value())};
                if (Value.Failure().category_ == ErrorCategory::NotFound) return std::optional<EntrySnapshot>{}; return Value.Failure();
            }
            Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context) override;
            Result<Unit> MakeDirectory(Locator At, OperationContext Context) override
            { return Mutate(At, {{"action", "mkdir"}}, Context); }
            Result<Unit> Remove(EntrySnapshot Entry, OperationContext Context) override
            {
                auto Fresh = Stat(Entry.id_, Context).get(); if (!Fresh.HasValue()) return Fresh.Failure();
                return Mutate(Fresh.Value().locator_, {{"action", Fresh.Value().directory_ ? "rmdir" : "delete"}}, Context);
            }
            Result<Unit> Rename(EntrySnapshot Entry, Locator To, OperationContext Context) override
            {
                auto Fresh = Stat(Entry.id_, Context).get(); if (!Fresh.HasValue()) return Fresh.Failure();
                auto Target = Remote(To, id_, scheme_); if (!Target.HasValue()) return Target.Failure();
                return Mutate(Fresh.Value().locator_, {{"action", "rename"}, {"to", Target.Value()}}, Context);
            }
            Result<Unit> Mutate(Locator At, QJsonObject Request, OperationContext Context)
            {
                if (!writable_) return Error{ErrorCategory::AccessDenied, "hosted-storage-write-denied", id_};
                auto Path = Remote(At, id_, scheme_); if (!Path.HasValue()) return Path.Failure(); if (Path.Value() == "\\") return Invalid("hosted-storage-mutate-root");
                const auto Action = Request["action"].toString();
                const auto Feature = Action == "put" ? StorageFeature::WriteFile : Action == "mkdir" ? StorageFeature::CreateDirectory
                    : Action == "rmdir" ? StorageFeature::RemoveDirectory : Action == "delete" ? StorageFeature::DeleteFile
                    : Action == "rename" ? StorageFeature::Rename : Action == "attributes" ? StorageFeature::Attributes : StorageFeature::Modified;
                if (!features_.contains(Feature)) return Unsupported("hosted-storage-operation-unavailable");
                Request.insert("path", Path.Value()); auto Reply = process_->DataCall(WireKind::StorageMutate, Request, Context);
                if (!Reply.HasValue()) return Reply.Failure(); return Unit{};
            }
        private:
            std::shared_ptr<PluginProcess> process_; ProviderId id_; std::shared_ptr<INativeContentStore> store_; std::set<StorageFeature> features_; bool writable_;
            std::string scheme_;
        };
        class WriteSession final : public IWriteSession
        {
        public:
            WriteSession(std::shared_ptr<HostedStorageProvider> Provider, Locator At, DestinationExpectation Expected)
                : provider_(std::move(Provider)), at_(std::move(At)), expected_(std::move(Expected)) {}
            Result<Unit> Open()
            {
                if (!directory_.isValid()) return Error{ErrorCategory::Unavailable, "hosted-storage-stage-create"};
                file_.setFileName(directory_.filePath("upload"));
                if (!file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return Error{ErrorCategory::Unavailable, "hosted-storage-stage-create"}; return Unit{};
            }
            Result<Unit> Append(std::span<const std::byte> Bytes, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(provider_->Id())) return *Stop;
                if (done_ || !file_.isOpen()) return Invalid("hosted-storage-write-closed");
                if (Bytes.size() > Context.maxReadBytes_ || std::uint64_t(file_.size()) + Bytes.size() > 256ULL * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "hosted-storage-write-budget"};
                if (file_.write(reinterpret_cast<const char*>(Bytes.data()), qint64(Bytes.size())) != qint64(Bytes.size())) return Error{ErrorCategory::Unavailable, "hosted-storage-stage-write"}; return Unit{};
            }
            Result<Unit> SetMetadata(FileMetadata Metadata, OperationContext Context) override
            {
                if (const auto Stop = Context.Check(provider_->Id())) return *Stop;
                if (done_) return Invalid("hosted-storage-write-closed");
                if (Metadata.modifiedUtcMs_ && (*Metadata.modifiedUtcMs_ < -11644473600000LL || *Metadata.modifiedUtcMs_ > 900000000000000LL)) return Invalid("hosted-storage-metadata-time");
                metadata_ = Metadata; return Unit{};
            }
            Result<WriteReceipt> Commit(OperationContext Context) override
            {
                if (done_ || !file_.isOpen()) return Invalid("hosted-storage-write-closed");
                auto Existing = provider_->Inspect(at_, Context); if (!Existing.HasValue()) return Existing.Failure();
                if (bool(Existing.Value()) != expected_.exists_ || (expected_.exists_ && (!expected_.revision_ || Existing.Value()->id_.revision_ != expected_.revision_)))
                    return Error{ErrorCategory::StaleRevision, "hosted-storage-destination-changed", provider_->Id()};
                if (!file_.flush()) return Error{ErrorCategory::Unavailable, "hosted-storage-stage-flush"}; file_.close();
                // The SDK owns the remote commit. Failure may mean a partial
                // remote write; never retry or delete the destination blindly.
                done_ = true;
                auto Written = provider_->Mutate(at_, {{"action", "put"}, {"local", file_.fileName()}, {"replace", expected_.exists_}}, Context);
                if (!Written.HasValue()) { auto Error = Written.Failure(); Error.retryable_ = false; Error.userAction_ = "inspect-remote-destination"; return Error; }
                WriteReceipt Receipt{at_};
                if (metadata_.modifiedUtcMs_)
                {
                    auto Applied = provider_->Mutate(at_, {{"action", "modified"}, {"value", QString::number((*metadata_.modifiedUtcMs_ + 11644473600000LL) * 10000)}}, Context);
                    if (!Applied.HasValue()) Receipt.cleanupErrors_.push_back(Applied.Failure());
                }
                if (metadata_.attributes_)
                {
                    auto Applied = provider_->Mutate(at_, {{"action", "attributes"}, {"value", int(*metadata_.attributes_ & 0xffff)}}, Context);
                    if (!Applied.HasValue()) Receipt.cleanupErrors_.push_back(Applied.Failure());
                }
                return Receipt;
            }
            Result<Unit> Abort() override { done_ = true; file_.close(); if (!file_.fileName().isEmpty() && QFile::exists(file_.fileName()) && !file_.remove()) return Error{ErrorCategory::Unavailable, "hosted-storage-stage-cleanup"}; return Unit{}; }
        private:
            std::shared_ptr<HostedStorageProvider> provider_; Locator at_; DestinationExpectation expected_; QTemporaryDir directory_; QFile file_; FileMetadata metadata_; bool done_ = false;
        };
        Result<std::unique_ptr<IWriteSession>> HostedStorageProvider::BeginWrite(Locator At, DestinationExpectation Expected, OperationContext Context)
        {
            auto Support = Supports(Mutation::CreateContent, At, Context); if (!Support.HasValue()) return Support.Failure();
            if (Support.Value().availability_ == Availability::Unsupported) return Unsupported("hosted-storage-write-unavailable");
            auto Session = std::make_unique<WriteSession>(shared_from_this(), std::move(At), std::move(Expected)); auto Opened = Session->Open(); if (!Opened.HasValue()) return Opened.Failure();
            return std::unique_ptr<IWriteSession>(std::move(Session));
        }
        MetadataType Type(int Sdk)
        {
            switch (Sdk) { case 1: case 2: return MetadataType::Integer; case 3: return MetadataType::Real; case 4: return MetadataType::Date;
            case 5: return MetadataType::Time; case 6: return MetadataType::Boolean; case 9: case 12: return MetadataType::FullText; case 10: return MetadataType::Timestamp; default: return MetadataType::Text; }
        }
        class HostedMetadataProvider final : public IMetadataProvider
        {
        public:
            HostedMetadataProvider(std::shared_ptr<PluginProcess> Process, std::shared_ptr<IMaterializer> Materializer) : process_(std::move(Process)), materializer_(std::move(Materializer)) {}
            Task<std::vector<MetadataField>> Fields(OperationContext Context) override
            {
                auto Reply = process_->DataCall(WireKind::MetadataQuery, {{"action", "fields"}}, Context);
                if (!Reply.HasValue()) return Ready<std::vector<MetadataField>>(Reply.Failure());
                if (!Reply.Value()["fields"].isArray()) return Ready<std::vector<MetadataField>>(Invalid("hosted-metadata-fields-envelope"));
                std::vector<MetadataField> Fields;
                for (const auto& Item : Reply.Value()["fields"].toArray())
                {
                    const auto Object = Item.toObject(); MetadataField Field; Field.index_ = Object["index"].toInt(); Field.name_ = Object["name"].toString().toStdString();
                    Field.type_ = Type(Object["type"].toInt()); Field.descending_ = Object["descending"].toBool();
                    for (const auto& Unit : Object["units"].toString().split('|')) Field.units_.push_back(Unit.toStdString()); Fields.push_back(std::move(Field));
                }
                return Ready<std::vector<MetadataField>>(std::move(Fields));
            }
            Task<MetadataValue> Query(std::shared_ptr<IContentSource> Source, MetadataRequest Request, OperationContext Context) override
            {
                if (!Source || Request.field_ < 0 || Request.field_ > 255 || Request.unit_ < 0) return Ready<MetadataValue>(Invalid("hosted-metadata-query"));
                auto Verified = Source->Verify(Context).get(); if (!Verified.HasValue()) return Ready<MetadataValue>(Verified.Failure());
                // Materialization changes names and attributes. Local metadata
                // uses the verified original; virtual metadata uses a copy.
                const auto Description = Source->Describe();
                auto Path = S(Description.locator_.root_);
                std::shared_ptr<IMaterializedFileLease> Lease;
                if (Description.locator_.kind_ != LocatorKind::Local || !Description.locator_.nesting_.empty() || !QFileInfo(Path).isAbsolute())
                {
                    auto Materialized = materializer_->Materialize({Source}, Context).get(); if (!Materialized.HasValue()) return Ready<MetadataValue>(Materialized.Failure());
                    Lease = Materialized.Value(); Path = S(Lease->LocalPath());
                }
                auto Reply = process_->DataCall(WireKind::MetadataQuery, {{"action", "value"}, {"path", Path},
                    {"field", Request.field_}, {"unit", Request.unit_}, {"defer", Request.deferSlow_}}, Context);
                // Keep the lease until the host reply or watchdog teardown.
                if (!Reply.HasValue()) return Ready<MetadataValue>(Reply.Failure());
                Verified = Source->Verify(Context).get(); if (!Verified.HasValue()) return Ready<MetadataValue>(Verified.Failure());
                const int Sdk = Reply.Value()["type"].toInt(-99); MetadataValue Out;
                if (Sdk == -3 || Sdk == -4 || Sdk == 0) { Out.status_ = Sdk == -3 ? MetadataStatus::Empty : Sdk == -4 ? MetadataStatus::OnDemand : MetadataStatus::Delayed; return Ready<MetadataValue>(Out); }
                if (Sdk < 1 || Sdk > 12) return Ready<MetadataValue>(Invalid("hosted-metadata-value-envelope"));
                Out.status_ = MetadataStatus::Value; Out.type_ = Type(Sdk); const auto Value = Reply.Value()["value"];
                if (Out.type_ == MetadataType::Integer || Out.type_ == MetadataType::Timestamp)
                { bool Parsed; Out.value_ = std::int64_t(Value.toString().toLongLong(&Parsed)); if (!Parsed) return Ready<MetadataValue>(Invalid("hosted-metadata-integer-envelope")); }
                else if (Out.type_ == MetadataType::Real) Out.value_ = Value.toDouble();
                else if (Out.type_ == MetadataType::Boolean) Out.value_ = Value.toBool();
                else Out.value_ = Value.toString().toStdString();
                return Ready<MetadataValue>(std::move(Out));
            }
        private:
            std::shared_ptr<PluginProcess> process_; std::shared_ptr<IMaterializer> materializer_;
        };
    }
    Result<HostedStorage> MakeHostedWfxProvider(std::shared_ptr<PluginProcess> Process, ProviderId Id, std::shared_ptr<INativeContentStore> Store, bool Writable)
    {
        if (!Process || !Store || Id.value_.empty() || Id.value_.find_first_of(":\\/\0", 0, 4) != std::string::npos) return Invalid("wfx-provider-options");
        auto Probe = Process->Probe(); if (!Probe.HasValue()) return Probe.Failure(); if (Probe.Value()["format"] != "wfx") return Invalid("wfx-provider-format");
        QSet<QString> Exports; for (const auto& Item : Probe.Value()["exports"].toArray()) Exports.insert(Item.toString());
        std::set<StorageFeature> Features;
        for (const auto& [Name, Feature] : std::initializer_list<std::pair<const char*, StorageFeature>>{
            {"FsGetFileW", StorageFeature::ReadFile}, {"FsPutFileW", StorageFeature::WriteFile},
            {"FsMkDirW", StorageFeature::CreateDirectory}, {"FsDeleteFileW", StorageFeature::DeleteFile},
            {"FsRemoveDirW", StorageFeature::RemoveDirectory}, {"FsRenMovFileW", StorageFeature::Rename},
            {"FsSetAttrW", StorageFeature::Attributes}, {"FsSetTimeW", StorageFeature::Modified}})
            if (Exports.contains(Name)) Features.insert(Feature);
        auto Provider = std::make_shared<HostedStorageProvider>(std::move(Process), Id, std::move(Store), std::move(Features), Writable);
        return HostedStorage{Provider, Provider, {LocatorKind::Plugin, Root(Id) + "\\"}};
    }
    Result<HostedStorage> MakeHostedDopusVfsProvider(std::shared_ptr<PluginProcess> Process, ProviderId Id,
        std::shared_ptr<INativeContentStore> Store, bool Writable, const QString& NativeRoot)
    {
        if (!Process || !Store || Id.value_.empty() || Id.value_.find_first_of(":\\/\0", 0, 4) != std::string::npos) return Invalid("dopus-vfs-provider-options");
        auto Probe = Process->Probe(); if (!Probe.HasValue()) return Probe.Failure();
        if (Probe.Value()["format"] != "dopus-vfs" || !Probe.Value()["readOnly"].isBool()) return Invalid("dopus-vfs-provider-format");
        QSet<QString> Native; for (const auto& Item : Probe.Value()["exports"].toArray()) Native.insert(Item.toString());
        // Only the SDK boundary interprets export names. Shared storage code
        // receives neutral features and never treats a DOpus export as a TC ABI.
        std::set<StorageFeature> Features;
        if (Native.contains("VFS_CreateFileW") && Native.contains("VFS_CloseFile"))
        {
            if (Native.contains("VFS_ReadFile")) Features.insert(StorageFeature::ReadFile);
            if (Native.contains("VFS_WriteFile")) Features.insert(StorageFeature::WriteFile);
        }
        for (const auto& [Name, Feature] : std::initializer_list<std::pair<const char*, StorageFeature>>{
            {"VFS_CreateDirectoryW", StorageFeature::CreateDirectory}, {"VFS_DeleteFileW", StorageFeature::DeleteFile},
            {"VFS_RemoveDirectoryW", StorageFeature::RemoveDirectory}, {"VFS_MoveFileW", StorageFeature::Rename},
            {"VFS_SetFileAttrW", StorageFeature::Attributes}, {"VFS_SetFileTimeW", StorageFeature::Modified}})
            if (Native.contains(Name)) Features.insert(Feature);
        auto Mounted = Process->DataCall(WireKind::StorageEnumerate, {{"action", "mount"}, {"root", NativeRoot}});
        if (!Mounted.HasValue()) return Mounted.Failure();
        if (Mounted.Value()["batchAdd"].toBool()) Features.insert(StorageFeature::WriteFile);
        if (Mounted.Value()["batchExtract"].toBool()) Features.insert(StorageFeature::ReadFile);
        if (Mounted.Value()["batchDelete"].toBool()) Features.insert(StorageFeature::DeleteFile);
        auto Provider = std::make_shared<HostedStorageProvider>(std::move(Process), Id, std::move(Store), std::move(Features), Writable && !Probe.Value()["readOnly"].toBool(), "dopus");
        return HostedStorage{Provider, Provider, {LocatorKind::Plugin, Root(Id, "dopus") + "\\"}};
    }
    Result<std::shared_ptr<IMetadataProvider>> MakeHostedWdxProvider(std::shared_ptr<PluginProcess> Process, std::shared_ptr<IMaterializer> Materializer)
    {
        if (!Process || !Materializer) return Invalid("wdx-provider-options");
        auto Probe = Process->Probe(); if (!Probe.HasValue()) return Probe.Failure(); if (Probe.Value()["format"] != "wdx") return Invalid("wdx-provider-format");
        return std::shared_ptr<IMetadataProvider>(std::make_shared<HostedMetadataProvider>(std::move(Process), std::move(Materializer)));
    }
    Result<std::shared_ptr<IMetadataProvider>> MakeHostedDopusMetadataProvider(std::shared_ptr<PluginProcess> Process, std::shared_ptr<IMaterializer> Materializer)
    {
        if (!Process || !Materializer) return Invalid("dopus-metadata-provider-options");
        auto Probe = Process->Probe(); if (!Probe.HasValue()) return Probe.Failure();
        if (Probe.Value()["format"] != "dopus-viewer") return Invalid("dopus-metadata-provider-format");
        return std::shared_ptr<IMetadataProvider>(std::make_shared<HostedMetadataProvider>(std::move(Process), std::move(Materializer)));
    }
    namespace
    {
        class HostedConverter final : public IContentConverter
        {
        public:
            HostedConverter(std::shared_ptr<PluginProcess> Process, std::shared_ptr<IMaterializer> Materializer)
                : process_(std::move(Process)), materializer_(std::move(Materializer)) {}
            Task<ConvertedContent> Convert(std::shared_ptr<IContentSource> Source, OperationContext Context) override
            {
                if (!Source) return Ready<ConvertedContent>(Invalid("hosted-convert-source"));
                auto Lease = materializer_->Materialize({Source}, Context).get(); if (!Lease.HasValue()) return Ready<ConvertedContent>(Lease.Failure());
                auto Reply = process_->DataCall(WireKind::ContentConvert, {{"path", S(Lease.Value()->LocalPath())}}, Context);
                if (!Reply.HasValue()) return Ready<ConvertedContent>(Reply.Failure());
                auto Verified = Source->Verify(Context).get(); if (!Verified.HasValue()) return Ready<ConvertedContent>(Verified.Failure());
                const auto Type = Reply.Value()["textType"].toInt(-1);
                if (Type < 0 || Type > 2 || Reply.Value()["textType"].toDouble() != Type || !Reply.Value()["bytes"].isString())
                    return Ready<ConvertedContent>(Invalid("hosted-convert-envelope"));
                auto Bytes = QByteArray::fromBase64Encoding(Reply.Value()["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                if (!Bytes) return Ready<ConvertedContent>(Invalid("hosted-convert-encoding"));
                if (std::uint64_t(Bytes.decoded.size()) > Context.maxReadBytes_) return Ready<ConvertedContent>(Error{ErrorCategory::BudgetExceeded, "hosted-convert-budget"});
                ConvertedContent Out{static_cast<ConvertedTextType>(Type)}; Out.bytes_.resize(Bytes.decoded.size());
                if (!Out.bytes_.empty()) std::memcpy(Out.bytes_.data(), Bytes.decoded.constData(), Out.bytes_.size());
                return Ready<ConvertedContent>(std::move(Out));
            }
        private:
            std::shared_ptr<PluginProcess> process_; std::shared_ptr<IMaterializer> materializer_;
        };
    }
    Result<std::shared_ptr<IContentConverter>> MakeHostedDopusConverter(std::shared_ptr<PluginProcess> Process, std::shared_ptr<IMaterializer> Materializer)
    {
        if (!Process || !Materializer) return Invalid("dopus-converter-options");
        auto Probe = Process->Probe(); if (!Probe.HasValue()) return Probe.Failure();
        if (Probe.Value()["format"] != "dopus-viewer") return Invalid("dopus-converter-format");
        return std::shared_ptr<IContentConverter>(std::make_shared<HostedConverter>(std::move(Process), std::move(Materializer)));
    }

}
