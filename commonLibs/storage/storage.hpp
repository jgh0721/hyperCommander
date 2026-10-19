#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// This boundary deliberately has no Qt, Win32, model indexes or native handles.
namespace hc::storage
{
    struct ProviderId
    {
        std::string value_;
        bool operator==(const ProviderId&) const = default;
    };

    enum class IdentityScope { Persistent, NavigationSession };
    struct ResourceRevision
    {
        std::string token_;
        bool operator==(const ResourceRevision&) const = default;
    };
    struct ResourceId
    {
        ProviderId providerId_;
        std::string key_;
        IdentityScope scope_ = IdentityScope::NavigationSession;
        std::optional<ResourceRevision> revision_;
        bool operator==(const ResourceId&) const = default;
    };

    enum class LocatorKind { Local, Shell, Smb, Archive, Plugin };
    struct ContainerHop
    {
        std::string provider_;
        std::string entry_;
        bool operator==(const ContainerHop&) const = default;
    };
    struct Locator
    {
        LocatorKind kind_ = LocatorKind::Local;
        std::string root_;
        std::vector<ContainerHop> nesting_;
        bool operator==(const Locator&) const = default;
    };
    inline bool SameArchiveContainer(const Locator& Left, const Locator& Right)
    {
        if (Left.kind_ != LocatorKind::Archive || Right.kind_ != LocatorKind::Archive || Left.root_ != Right.root_
            || Left.nesting_.empty() || Left.nesting_.size() != Right.nesting_.size()) return false;
        for (std::size_t Index = 0; Index < Left.nesting_.size(); ++Index)
            if (Left.nesting_[Index].provider_ != Right.nesting_[Index].provider_
                || (Index + 1 < Left.nesting_.size() && Left.nesting_[Index].entry_ != Right.nesting_[Index].entry_)) return false;
        return true;
    }

    enum class ErrorCategory { InvalidRequest, NotFound, AccessDenied, Unavailable, Cancelled, StaleRevision, Unsupported, BudgetExceeded, Internal };
    struct Error
    {
        ErrorCategory category_ = ErrorCategory::Internal;
        std::string code_;
        ProviderId provider_;
        std::optional<ResourceId> resource_;
        bool retryable_ = false;
        std::string userAction_;
    };
    template<class T> class Result
    {
    public:
        Result(T Value) : value_(std::move(Value)) {}
        Result(Error Failure) : value_(std::move(Failure)) {}
        bool HasValue() const { return std::holds_alternative<T>(value_); }
        T& Value() { return std::get<T>(value_); }
        const T& Value() const { return std::get<T>(value_); }
        const Error& Failure() const { return std::get<Error>(value_); }
    private:
        std::variant<T, Error> value_;
    };
    using Unit = std::monostate;
    enum class WritePhase { Writing, Prepared, BackedUp, Published, Finished, Aborted };
    struct WriteRecoveryRecord
    {
        std::string transaction_;
        Locator destination_, stage_, backup_;
        std::string stageIdentity_, originalIdentity_;
        WritePhase phase_ = WritePhase::Writing;
    };
    using WriteCheckpoint = std::function<Result<std::uint64_t>(const WriteRecoveryRecord&, std::optional<std::uint64_t>)>;
    template<class T> using Task = std::future<Result<T>>;
    template<class T> Task<T> Ready(Result<T> Value)
    {
        std::promise<Result<T>> Promise;
        Promise.set_value(std::move(Value));
        return Promise.get_future();
    }

    // Passwords are transient values, never part of locators, job events or journals.
    class Password final
    {
    public:
        explicit Password(std::string Value = {}) : value_(std::move(Value)) {}
        Password(Password&& Other) noexcept { value_.swap(Other.value_); }
        Password& operator=(Password&& Other) noexcept { clear(); value_.swap(Other.value_); return *this; }
        ~Password() { clear(); }
        const std::string& Value() const { return value_; }
    private:
        void clear() { volatile char* Bytes = value_.data(); for (std::size_t Index = 0; Index < value_.size(); ++Index) Bytes[Index] = 0; value_.clear(); }
        std::string value_;
    };
    struct OperationContext;
    using PasswordPrompt = std::function<Result<Password>(const std::string& Archive, unsigned Attempt, const OperationContext& Context)>;
    enum class WorkloadIntent { Explicit, Automatic };
    struct WorkloadPolicy
    {
        WorkloadIntent intent_ = WorkloadIntent::Explicit;
        std::uint64_t automaticFileBytes_ = 256ULL * 1024 * 1024;
        std::uint64_t automaticEntries_ = 100000;
        std::size_t automaticDepth_ = 8;
        std::uint64_t automaticExpansionRatio_ = 1000;
        bool IsAutomatic() const { return intent_ == WorkloadIntent::Automatic; }
    };
    enum class ProviderInteractionKind { Password, Credentials, Volume, Conflict };
    struct ProviderInteractionRequest
    {
        ProviderInteractionKind kind_ = ProviderInteractionKind::Password;
        std::string id_, subject_;
        unsigned attempt_ = 1;
    };
    struct ProviderInteractionReply
    {
        bool accepted_ = false;
        Password value_;
    };
    using InteractionPrompt = std::function<Result<ProviderInteractionReply>(const ProviderInteractionRequest&, const OperationContext&)>;
    using InteractionStart = std::function<Task<ProviderInteractionReply>(const ProviderInteractionRequest&, const OperationContext&)>;
    struct OperationContext
    {
        std::string requestId_;
        std::stop_token stopToken_;
        std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::time_point::max();
        std::size_t maxPageItems_ = 256;
        std::size_t maxReadBytes_ = 1024 * 1024;
        // Legacy caller-requested scope, not a product-wide enumeration cap.
        std::size_t maxEnumerationEntries_ = std::numeric_limits<std::size_t>::max();
        bool denyConcurrentMutation_ = false;
        // Explicit coordinator policy; native providers keep readonly entries otherwise.
        bool allowReadOnlyRemoval_ = false;
        // Known output size reserves physical staging space, not total work.
        std::optional<std::uint64_t> expectedWriteBytes_;
        // Applies to final payload and alternate streams at each commit.
        bool flushFileContents_ = true;
        WriteCheckpoint writeCheckpoint_;
        // External helpers observe a paused job between their own bounded I/O
        // units. The scheduler installs a value-only, weakly owned query.
        std::function<bool()> pauseRequested_;
        // Coordinator-local selection session hint, never serialized or an authority grant.
        bool orderedBatchRead_ = false;
        bool freshEnumerationMetadata_ = false;
        // Mutation coordinators may yield native directory enumeration a page
        // at a time. Browsing keeps its immutable snapshot by default.
        bool incrementalEnumeration_ = false;
        // Browsing may retain directory-enumeration metadata when opening an
        // individual child is denied. Such entries have no verified revision.
        // Mutation planners leave this false even when requesting fresh data.
        bool allowUnavailableChildMetadata_ = false;
        PasswordPrompt passwordPrompt_;
        WorkloadPolicy workload_;
        InteractionPrompt interactionPrompt_;
        // Scheduler/UI bridges return a promise-backed future immediately.
        // Its lifetime must not depend on a blocked caller or held I/O mutex.
        InteractionStart interactionStart_;
        bool providerInteractions_ = false;
        // Progress is monotonic work completed, distinct from host liveness.
        std::function<void(std::uint64_t)> progress_;
        std::size_t EnumerationLimit() const
        {
            return workload_.IsAutomatic() ? std::min(maxEnumerationEntries_,
                static_cast<std::size_t>(std::min<std::uint64_t>(workload_.automaticEntries_, std::numeric_limits<std::size_t>::max()))) : maxEnumerationEntries_;
        }
        std::optional<Error> Check(const ProviderId& Provider) const
        {
            if (stopToken_.stop_requested())
                return Error{ErrorCategory::Cancelled, "cancelled", Provider};
            if (std::chrono::steady_clock::now() >= deadline_)
                return Error{ErrorCategory::Unavailable, "deadline-exceeded", Provider, {}, true, "retry"};
            return {};
        }
    };

    struct EntrySnapshot
    {
        ResourceId id_;
        Locator locator_;
        std::string name_;
        bool directory_ = false;
        std::uint64_t size_ = 0;
        std::uint32_t attributes_ = 0;
        std::int64_t createdUtcMs_ = 0;
        std::int64_t modifiedUtcMs_ = 0;
        // Provider-specific presentation hints; no native pointer crosses the boundary.
        std::map<std::string, std::string> hints_;
    };
    struct EntryPage
    {
        std::vector<EntrySnapshot> entries_;
        std::optional<ResourceRevision> revision_;
        bool end_ = false;
    };
    struct ReadChunk
    {
        std::vector<std::byte> bytes_;
        bool eof_ = false;
    };
    enum class ReadAccess { Unknown, Sequential, ForwardSeek, RandomAccess, Materialized };
    enum class BatchSupport { None, Emulated, Native };
    enum class ProviderConcurrency { SerializedPerModule, SerializedPerSession, IndependentReaders };
    enum class CommitSemantics { ReadOnly, BestEffort, AtomicReplace };
    struct EngineConstraints
    {
        std::optional<std::uint64_t> maxFileBytes_, maxEntries_;
        std::optional<std::size_t> maxNameBytes_;
        std::string reason_;
    };
    struct ProviderCapabilities
    {
        ReadAccess read_ = ReadAccess::Unknown;
        BatchSupport batch_ = BatchSupport::None;
        ProviderConcurrency concurrency_ = ProviderConcurrency::SerializedPerModule;
        CommitSemantics commit_ = CommitSemantics::ReadOnly;
        EngineConstraints constraints_;
    };
    struct EntryId
    {
        std::string sessionEpoch_;
        ResourceRevision containerRevision_;
        std::string token_;
        bool operator==(const EntryId&) const = default;
    };
    enum class Operation { Enumerate, Stat, OpenRead, Write };
    enum class Availability { Supported, Conditional, Unsupported };
    struct SupportDecision
    {
        Availability availability_ = Availability::Unsupported;
        std::string reason_;
        bool requiresMaterialization_ = false;
        bool requiresUserInteraction_ = false;
        std::vector<std::string> constraints_;
    };

    class IEntryCursor
    {
    public:
        virtual ~IEntryCursor() = default;
        // One outstanding Next per cursor. Pages are values owned by the caller.
        virtual Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) = 0;
    };
    class IReadStream
    {
    public:
        virtual ~IReadStream() = default;
        // One outstanding operation per stream; every OpenRead creates a new cursor.
        virtual bool CanSeek() const = 0;
        // CanSeek alone does not prove direct/random access to the source.
        virtual ReadAccess Access() const { return ReadAccess::Unknown; }
        virtual Task<ReadChunk> Read(std::size_t MaxBytes, OperationContext Context) = 0;
        virtual Task<Unit> Seek(std::uint64_t Offset, OperationContext Context) = 0;
    };
    struct PresentationColumn { std::string key_, title_, valueKey_; std::uint32_t flags_ = 0; };
    class IStorageProvider
    {
    public:
        virtual ~IStorageProvider() = default;
        // A copied mount-time schema; this accessor performs no I/O.
        virtual std::vector<PresentationColumn> Columns() const { return {}; }
        virtual ProviderId Id() const = 0;
        virtual ProviderCapabilities Capabilities() const { return {}; }
        virtual Task<SupportDecision> Supports(Operation Request, OperationContext Context) = 0;
        virtual Task<std::unique_ptr<IEntryCursor>> Enumerate(Locator At, OperationContext Context) = 0;
        virtual Task<EntrySnapshot> Stat(ResourceId Resource, OperationContext Context) = 0;
        virtual Task<std::shared_ptr<IReadStream>> OpenRead(ResourceId Resource, OperationContext Context) = 0;
        // Explicit copy coordinators may prepare ordered archive reads. Ordinary
        // providers retain independent OpenRead calls and require no setup.
        virtual bool UsesOrderedReads() const { return false; }
        virtual Task<Unit> PrepareReadBatch(std::vector<EntrySnapshot>, OperationContext) { return Ready<Unit>(Unit{}); }
        virtual Task<Unit> FinishReadBatch(OperationContext) { return Ready<Unit>(Unit{}); }

        // Resolve a locator at a new operation boundary. Callers still compare
        // the captured revision before mutating or reading selected content.
        virtual Task<EntrySnapshot> Resolve(Locator, OperationContext)
        { return Ready<EntrySnapshot>(Error{ErrorCategory::Unsupported, "resolve-unavailable", Id()}); }
    };
    class IContentSource
    {
    public:
        virtual ~IContentSource() = default;
        virtual EntrySnapshot Describe() const = 0;
        virtual Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) = 0;
        virtual Task<Unit> Verify(OperationContext)
        { return Ready<Unit>(Error{ErrorCategory::Unsupported, "content-verification-unavailable"}); }
    };
    class ISelectionManifest;
    struct CommandContext
    {
        std::string tabId_;
        Locator location_;
        std::vector<ResourceId> selection_;
        std::shared_ptr<ISelectionManifest> selectionManifest_;
    };

    class IProviderRegistry
    {
    public:
        virtual ~IProviderRegistry() = default;
        virtual Result<Unit> Register(std::shared_ptr<IStorageProvider> Provider) = 0;
        virtual Result<std::shared_ptr<IStorageProvider>> Resolve(const ProviderId& Id) const = 0;
    };
    class ProviderRegistry final : public IProviderRegistry
    {
    public:
        Result<Unit> Register(std::shared_ptr<IStorageProvider> Provider) override
        {
            if (!Provider || Provider->Id().value_.empty())
                return Error{ErrorCategory::InvalidRequest, "invalid-provider"};
            std::lock_guard Lock(mutex_);
            const auto Id = Provider->Id();
            if (!providers_.emplace(Id.value_, std::move(Provider)).second)
                return Error{ErrorCategory::InvalidRequest, "duplicate-provider", Id};
            return Unit{};
        }
        Result<std::shared_ptr<IStorageProvider>> Resolve(const ProviderId& Id) const override
        {
            std::lock_guard Lock(mutex_);
            const auto It = providers_.find(Id.value_);
            if (It == providers_.end())
                return Error{ErrorCategory::NotFound, "provider-not-registered", Id};
            return It->second;
        }
    private:
        mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<IStorageProvider>> providers_;
    };

    // Length framing preserves container boundaries, delimiters and UTF-8 bytes.
    // This is a versioned persistence format, not a display path or a stable ID.
    inline void AppendField(std::string& Output, std::string_view Field)
    {
        Output += std::to_string(Field.size()) + ":";
        Output.append(Field);
    }
    inline bool TakeField(std::string_view& Input, std::string& Output)
    {
        const auto Colon = Input.find(':');
        if (Colon == std::string_view::npos || Colon == 0 || Colon > 7)
            return false;
        std::size_t Size = 0;
        for (std::size_t Index = 0; Index < Colon; ++Index)
        {
            if (Input[Index] < '0' || Input[Index] > '9')
                return false;
            Size = Size * 10 + static_cast<std::size_t>(Input[Index] - '0');
        }
        Input.remove_prefix(Colon + 1);
        if (Size > Input.size())
            return false;
        Output.assign(Input.substr(0, Size));
        Input.remove_prefix(Size);
        return true;
    }
    inline std::string Serialize(const Locator& Value)
    {
        std::string Output = "hcl1";
        AppendField(Output, std::to_string(static_cast<int>(Value.kind_)));
        AppendField(Output, Value.root_);
        for (const auto& Hop : Value.nesting_)
        {
            AppendField(Output, Hop.provider_);
            AppendField(Output, Hop.entry_);
        }
        return Output;
    }
    inline Result<Locator> ParseLocator(std::string_view Input)
    {
        const auto Invalid = Error{ErrorCategory::InvalidRequest, "invalid-locator"};
        if (Input.size() > 1024 * 1024 || !Input.starts_with("hcl1"))
            return Invalid;
        Input.remove_prefix(4);
        std::string Kind;
        Locator Value;
        if (!TakeField(Input, Kind) || Kind.size() != 1 || Kind[0] < '0' || Kind[0] > '4'
            || !TakeField(Input, Value.root_) || Value.root_.empty())
            return Invalid;
        Value.kind_ = static_cast<LocatorKind>(Kind[0] - '0');
        while (!Input.empty())
        {
            ContainerHop Hop;
            if (Value.nesting_.size() >= 32 || !TakeField(Input, Hop.provider_) || Hop.provider_.empty()
                || !TakeField(Input, Hop.entry_) || Hop.entry_.empty())
                return Invalid;
            Value.nesting_.push_back(std::move(Hop));
        }
        if ((Value.kind_ == LocatorKind::Archive) != !Value.nesting_.empty())
            return Invalid;
        return Value;
    }
    inline std::string Serialize(const ResourceId& Value)
    {
        std::string Output = "hci1";
        AppendField(Output, Value.providerId_.value_);
        AppendField(Output, Value.key_);
        AppendField(Output, Value.scope_ == IdentityScope::Persistent ? "p" : "s");
        AppendField(Output, Value.revision_ ? "1" : "0");
        if (Value.revision_)
            AppendField(Output, Value.revision_->token_);
        return Output;
    }
    inline Result<ResourceId> ParseResourceId(std::string_view Input)
    {
        const auto Invalid = Error{ErrorCategory::InvalidRequest, "invalid-resource-id"};
        if (Input.size() > 1024 * 1024 || !Input.starts_with("hci1"))
            return Invalid;
        Input.remove_prefix(4);
        ResourceId Value;
        std::string Scope, Revision;
        if (!TakeField(Input, Value.providerId_.value_) || Value.providerId_.value_.empty()
            || !TakeField(Input, Value.key_) || Value.key_.empty() || !TakeField(Input, Scope)
            || (Scope != "p" && Scope != "s") || !TakeField(Input, Revision) || (Revision != "0" && Revision != "1"))
            return Invalid;
        Value.scope_ = Scope == "p" ? IdentityScope::Persistent : IdentityScope::NavigationSession;
        if (Revision == "1")
        {
            ResourceRevision Token;
            if (!TakeField(Input, Token.token_))
                return Invalid;
            Value.revision_ = std::move(Token);
        }
        if (!Input.empty())
            return Invalid;
        return Value;
    }
}
