#include "documentSaveCoordinator.hpp"

namespace hc::document
{
    using namespace storage;
    Result<WriteReceipt> SaveDocumentBytes(IWritableStorage& Writer, Locator Destination,
        DestinationExpectation Expected, std::span<const std::byte> Bytes, OperationContext Context)
    {
        if (auto Stop = Context.Check(Writer.Id())) return *Stop;
        auto Support = Writer.Supports(Mutation::CreateContent, Destination, Context);
        if (!Support.HasValue()) return Support.Failure();
        if (Support.Value().availability_ == Availability::Unsupported || !Writer.CopySupport().verify_)
            return Error{ErrorCategory::Unsupported, "document-verified-write-unavailable", Writer.Id()};
        Context.expectedWriteBytes_ = Bytes.size();
        CopyOptions Options; Options.metadata_ = false; Options.verify_ = true;
        auto Opened = Writer.BeginCopy(std::move(Destination), std::move(Expected), Options, Context);
        if (!Opened.HasValue()) return Opened.Failure();
        auto& Session = *Opened.Value();
        // Keep the original failure, but preserve cleanup evidence if abort also fails.
        auto Fail = [&](Error Failure) -> Result<WriteReceipt> {
            auto Aborted = Session.Abort();
            if (!Aborted.HasValue()) Failure.userAction_ += ";abort:" + Aborted.Failure().code_;
            return Failure;
        };
        const auto ChunkSize = std::min<std::size_t>(Context.maxReadBytes_, 1024 * 1024);
        if (!ChunkSize) return Fail(Error{ErrorCategory::InvalidRequest, "document-write-chunk"});
        for (std::size_t Offset = 0; Offset < Bytes.size(); Offset += ChunkSize)
        {
            if (auto Stop = Context.Check(Writer.Id())) return Fail(*Stop);
            const auto Chunk = Bytes.subspan(Offset, std::min(ChunkSize, Bytes.size() - Offset));
            auto Appended = Session.Append(Chunk, Context); if (!Appended.HasValue()) return Fail(Appended.Failure());
            auto Recorded = Session.RecordSourceBytes(Chunk, Context); if (!Recorded.HasValue()) return Fail(Recorded.Failure());
        }
        for (;;)
        {
            if (auto Stop = Context.Check(Writer.Id())) return Fail(*Stop);
            auto Extra = Session.AdvanceCopyExtras(0, Context); if (!Extra.HasValue()) return Fail(Extra.Failure());
            if (Extra.Value().done_) break;
        }
        for (;;)
        {
            if (auto Stop = Context.Check(Writer.Id())) return Fail(*Stop);
            auto Verified = Session.VerifyContent(Context); if (!Verified.HasValue()) return Fail(Verified.Failure());
            if (Verified.Value()) break;
        }
        auto Receipt = Session.Commit(Context);
        if (!Receipt.HasValue()) return Fail(Receipt.Failure());
        if (!Receipt.Value().destinationCommitted_) return Fail(Error{ErrorCategory::Unavailable, "document-not-committed"});
        return std::move(Receipt.Value());
    }
}
