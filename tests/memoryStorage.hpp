#pragma once
#include "commonLibs/storage/storage.hpp"

namespace hc::storage::test
{
    struct FaultState
    {
        std::mutex mutex_;
        std::optional<Error> failure_;
        std::optional<Error> Check()
        {
            std::lock_guard Lock(mutex_);
            return failure_;
        }
    };
    class MemoryCursor final : public IEntryCursor
    {
    public:
        MemoryCursor(std::vector<EntrySnapshot> Entries, std::shared_ptr<FaultState> Fault)
            : entries_(std::move(Entries)), fault_(std::move(Fault)) {}
        Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override
        {
            if (const auto Failure = Context.Check({"memory"}))
                return Ready<EntryPage>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<EntryPage>(*Failure);
            if (!MaxItems || !Context.maxPageItems_)
                return Ready<EntryPage>(Error{ErrorCategory::InvalidRequest, "zero-page-size"});
            const auto Count = (std::min)({MaxItems, Context.maxPageItems_, entries_.size() - offset_});
            EntryPage Page;
            Page.entries_.assign(entries_.begin() + offset_, entries_.begin() + offset_ + Count);
            offset_ += Count;
            Page.end_ = offset_ == entries_.size();
            return Ready<EntryPage>(std::move(Page));
        }
    private:
        std::vector<EntrySnapshot> entries_;
        std::shared_ptr<FaultState> fault_;
        std::size_t offset_ = 0;
    };
    class MemoryStream final : public IReadStream
    {
    public:
        MemoryStream(std::vector<std::byte> Bytes, std::shared_ptr<FaultState> Fault)
            : bytes_(std::move(Bytes)), fault_(std::move(Fault)) {}
        bool CanSeek() const override { return true; }
        Task<ReadChunk> Read(std::size_t MaxBytes, OperationContext Context) override
        {
            if (const auto Failure = Context.Check({"memory"}))
                return Ready<ReadChunk>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<ReadChunk>(*Failure);
            if (!MaxBytes || !Context.maxReadBytes_)
                return Ready<ReadChunk>(Error{ErrorCategory::InvalidRequest, "zero-read-size"});
            const auto Count = (std::min)({MaxBytes, Context.maxReadBytes_, bytes_.size() - offset_});
            ReadChunk Chunk;
            Chunk.bytes_.assign(bytes_.begin() + offset_, bytes_.begin() + offset_ + Count);
            offset_ += Count;
            Chunk.eof_ = offset_ == bytes_.size();
            return Ready<ReadChunk>(std::move(Chunk));
        }
        Task<Unit> Seek(std::uint64_t Offset, OperationContext Context) override
        {
            if (const auto Failure = Context.Check({"memory"}))
                return Ready<Unit>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<Unit>(*Failure);
            if (Offset > bytes_.size())
                return Ready<Unit>(Error{ErrorCategory::InvalidRequest, "seek-out-of-range"});
            offset_ = static_cast<std::size_t>(Offset);
            return Ready<Unit>(Unit{});
        }
    private:
        std::vector<std::byte> bytes_;
        std::shared_ptr<FaultState> fault_;
        std::size_t offset_ = 0;
    };
    class MemoryProvider final : public IStorageProvider
    {
        struct Node
        {
            Locator parent_;
            EntrySnapshot snapshot_;
            std::vector<std::byte> bytes_;
        };
    public:
        ProviderId Id() const override { return {"memory"}; }
        ResourceId Add(const Locator& Parent, std::string Name, std::vector<std::byte> Bytes = {}, bool Directory = false)
        {
            std::lock_guard Lock(mutex_);
            const auto Key = std::to_string(++sequence_);
            const ResourceId Id{this->Id(), Key, IdentityScope::NavigationSession, ResourceRevision{"1"}};
            auto At = Parent;
            At.root_ += "/" + Name;
            EntrySnapshot Snapshot{Id, At, std::move(Name), Directory, Bytes.size()};
            nodes_.emplace(Key, Node{Parent, std::move(Snapshot), std::move(Bytes)});
            return Id;
        }
        ResourceId Replace(const ResourceId& Resource, std::vector<std::byte> Bytes)
        {
            std::lock_guard Lock(mutex_);
            auto& Node = nodes_.at(Resource.key_);
            Node.bytes_ = std::move(Bytes);
            Node.snapshot_.size_ = Node.bytes_.size();
            Node.snapshot_.id_.revision_ = ResourceRevision{std::to_string(++sequence_ + 1)};
            return Node.snapshot_.id_;
        }
        void SetFailure(std::optional<Error> Failure)
        {
            std::lock_guard Lock(fault_->mutex_);
            fault_->failure_ = std::move(Failure);
        }
        void SetReadSupport(SupportDecision Decision)
        {
            std::lock_guard Lock(mutex_);
            readSupport_ = std::move(Decision);
        }
        Task<SupportDecision> Supports(Operation Request, OperationContext Context) override
        {
            if (const auto Failure = Context.Check(Id()))
                return Ready<SupportDecision>(*Failure);
            std::lock_guard Lock(mutex_);
            if (Request == Operation::Write)
                return Ready<SupportDecision>(SupportDecision{Availability::Unsupported, "read-only-provider"});
            if (Request == Operation::OpenRead)
                return Ready<SupportDecision>(readSupport_);
            return Ready<SupportDecision>(SupportDecision{Availability::Supported, "memory-read"});
        }
        Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) override
        {
            if (const auto Failure = Context.Check(Id()))
                return Ready<std::unique_ptr<IEntryCursor>>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<std::unique_ptr<IEntryCursor>>(*Failure);
            std::lock_guard Lock(mutex_);
            if (!(At == root_))
            {
                const auto Found = std::find_if(nodes_.begin(), nodes_.end(), [&At](const auto& Item) {
                    return Item.second.snapshot_.directory_ && Item.second.snapshot_.locator_ == At;
                });
                if (Found == nodes_.end())
                    return Ready<std::unique_ptr<IEntryCursor>>(Error{ErrorCategory::NotFound, "directory-not-found", Id()});
            }
            std::vector<EntrySnapshot> Entries;
            for (const auto& [Key, Node] : nodes_)
                if (Node.parent_ == At)
                    Entries.push_back(Node.snapshot_);
            return Ready<std::unique_ptr<IEntryCursor>>(std::unique_ptr<IEntryCursor>(new MemoryCursor(std::move(Entries), fault_)));
        }
        Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) override
        {
            if (const auto Failure = Context.Check(Id()))
                return Ready<EntrySnapshot>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<EntrySnapshot>(*Failure);
            std::lock_guard Lock(mutex_);
            const auto It = nodes_.find(Resource.key_);
            if (!(Resource.providerId_ == Id()) || It == nodes_.end())
                return Ready<EntrySnapshot>(Error{ErrorCategory::NotFound, "resource-not-found", Id(), Resource});
            if (Resource.revision_ && Resource.revision_ != It->second.snapshot_.id_.revision_)
                return Ready<EntrySnapshot>(Error{ErrorCategory::StaleRevision, "revision-changed", Id(), Resource, false, "refresh"});
            return Ready<EntrySnapshot>(It->second.snapshot_);
        }
        Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) override
        {
            // Serialize validation and the copy so a replacement cannot race OpenRead.
            std::lock_guard Lock(mutex_);
            if (const auto Failure = Context.Check(Id()))
                return Ready<std::shared_ptr<IReadStream>>(*Failure);
            if (const auto Failure = fault_->Check())
                return Ready<std::shared_ptr<IReadStream>>(*Failure);
            if (readSupport_.availability_ != Availability::Supported)
                return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::Unsupported, readSupport_.reason_, Id(), Resource});
            const auto It = nodes_.find(Resource.key_);
            if (!(Resource.providerId_ == Id()) || It == nodes_.end())
                return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::NotFound, "resource-not-found", Id(), Resource});
            if (Resource.revision_ && Resource.revision_ != It->second.snapshot_.id_.revision_)
                return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::StaleRevision, "revision-changed", Id(), Resource});
            if (It->second.snapshot_.directory_)
                return Ready<std::shared_ptr<IReadStream>>(Error{ErrorCategory::Unsupported, "directory-stream", Id(), Resource});
            return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(new MemoryStream(It->second.bytes_, fault_)));
        }
        Locator Root() const { return root_; }
    private:
        std::mutex mutex_;
        std::map<std::string, Node> nodes_;
        std::uint64_t sequence_ = 0;
        Locator root_{LocatorKind::Local, "/memory"};
        std::shared_ptr<FaultState> fault_ = std::make_shared<FaultState>();
        SupportDecision readSupport_{Availability::Supported, "memory-read"};
    };
}
