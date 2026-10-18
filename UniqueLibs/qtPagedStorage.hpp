#pragma once
#include "commonLibs/storage/entryIndex.hpp"
#include <QByteArray>
#include <QString>
#include <functional>

namespace hc::storage
{
    // Shared disk record engine for entry indexes, selection manifests and
    // paged operation results. Calls are serialized; no record array is cached.
    // Ephemeral storage: durable recovery/journaling belongs to the job journal.
    // Writers must Flush before publishing to detect buffered write failures.
    class DiskRecords final
    {
    public:
        static Result<std::shared_ptr<DiskRecords>> Create(const QString& Parent = {});
        ~DiskRecords();
        std::uint64_t Count() const;
        Result<std::uint64_t> Append(const QByteArray& Bytes);
        Result<Unit> Replace(std::uint64_t Row, const QByteArray& Bytes);
        Result<QByteArray> Read(std::uint64_t Row) const;
        Result<Unit> Flush();
        QString Directory() const;
    private:
        struct State;
        explicit DiskRecords(std::unique_ptr<State> Value);
        std::unique_ptr<State> state_;
    };

    class PagedEntryIndex final : public IEntryIndex
    {
    public:
        using Less = std::function<bool(const EntrySnapshot&, const EntrySnapshot&)>;
        using Accept = std::function<bool(const EntrySnapshot&)>;
        using NextRange = std::function<Result<std::optional<EntryRange>>() >;
        struct SortBuffer {
            std::size_t entries_ = 8192;
            std::size_t bytes_ = 8 * 1024 * 1024;
            std::size_t fanIn_ = 16;
        };
        static Result<std::shared_ptr<PagedEntryIndex>> Build(IEntryCursor& Cursor,
            const OperationContext& Context, const QString& Parent = {});
        std::uint64_t Count() const override;
        Result<EntryPage> Read(std::uint64_t Offset, std::size_t MaxItems,
            const OperationContext& Context = {}) const override;
        Result<std::shared_ptr<PagedEntryIndex>> Project(const Less& Compare, const Accept& Filter,
            const OperationContext& Context, SortBuffer Buffer) const;
        Result<std::shared_ptr<ISelectionManifest>> Select(const std::vector<EntryRange>& Ranges,
            const OperationContext& Context = {}) const;
        Result<std::shared_ptr<ISelectionManifest>> SelectAll(const OperationContext& Context = {}) const;
        Result<std::shared_ptr<ISelectionManifest>> SelectRanges(const NextRange& Next, const OperationContext& Context = {}) const;
        QString Directory() const { return records_->Directory(); }
    private:
        explicit PagedEntryIndex(std::shared_ptr<DiskRecords> Records, std::optional<ResourceRevision> Revision = {})
            : records_(std::move(Records)), revision_(std::move(Revision)) {}
        std::shared_ptr<DiskRecords> records_;
        std::optional<ResourceRevision> revision_;
    };
    QByteArray EncodeEntry(const EntrySnapshot& Entry);
    Result<EntrySnapshot> DecodeEntry(const QByteArray& Bytes);
}
