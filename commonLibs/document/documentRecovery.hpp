#pragma once
#include "documentTypes.hpp"

namespace hc::document
{
    inline constexpr std::uint32_t RecoveryEngineSchema = 1;
    // Recovery text is lossless UTF-8, independent of the selected save encoding.
    struct TextRecoveryState
    {
        std::string encoding_ = "UTF-8", lineEnding_ = "LF", baselineSha256_;
        bool bom_ = false, untitled_ = false;
        std::int64_t anchor_ = 0, caret_ = 0, firstLine_ = 0, xOffset_ = 0;
        // Wrapped row of firstLine_ shown at the top (word wrap keeps the exact scroll position).
        std::int64_t firstSubLine_ = 0;
        // Restored copies take this extension so the view (text or Markdown) survives: txt · md · markdown.
        std::string suffix_ = "txt";
        // "새 문서 N" of an untitled document; 0 when none was recorded.
        std::int64_t untitledNumber_ = 0;
    };
    struct RecoverySnapshot
    {
        DocumentSnapshot document_;
        std::string viewId_ = "text", bytes_, checksum_, snapshotPath_;
        std::int64_t utcMs_ = 0;
        // ExternalModel requires a native-validated model snapshot, never bytes
        // alone. These pins describe recovery compatibility, not write authority.
        std::string bundleSha256_, format_;
        std::uint32_t engineSchema_ = 0;
        std::optional<TextRecoveryState> text_;
        std::string ownerWindow_;
    };
    class IRecoveryStore
    {
    public:
        virtual ~IRecoveryStore() = default;
        virtual storage::Result<RecoverySnapshot> Store(RecoverySnapshot Snapshot) = 0;
        virtual storage::Result<std::vector<RecoverySnapshot>> List() = 0;
        virtual storage::Result<RecoverySnapshot> Read(const std::string& Path) = 0;
        virtual storage::Result<storage::Unit> Remove(const DocumentId& Id, std::uint64_t ThroughRevision) = 0;
    };
}
