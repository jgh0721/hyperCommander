#pragma once
#include "jobScheduler.hpp"

namespace hc::operations {
inline ItemOutcome EncodeWriteRecovery(const storage::WriteRecoveryRecord& Record)
{
    ItemOutcome Item; Item.item_ = storage::Serialize(Record.destination_);
    Item.destinationCommitted_ = Record.phase_ == storage::WritePhase::Published || Record.phase_ == storage::WritePhase::Finished;
    Item.source_.emplace(); auto& Entry = *Item.source_;
    Entry.id_.key_ = Record.transaction_; Entry.locator_ = Record.destination_;
    Entry.hints_ = {{"stage", storage::Serialize(Record.stage_)}, {"backup", storage::Serialize(Record.backup_)},
        {"stageIdentity", Record.stageIdentity_}, {"originalIdentity", Record.originalIdentity_}, {"phase", std::to_string(int(Record.phase_))}};
    return Item;
}
inline storage::Result<storage::WriteRecoveryRecord> DecodeWriteRecovery(const ItemOutcome& Item)
{
    using namespace storage;
    const auto Invalid = Error{ErrorCategory::InvalidRequest, "write-recovery-record"};
    if (!Item.source_) return Invalid;
    const auto& Entry = *Item.source_; const auto& Hints = Entry.hints_;
    for (const auto* Key : {"stage", "backup", "stageIdentity", "originalIdentity", "phase"}) if (!Hints.contains(Key)) return Invalid;
    const auto& Phase = Hints.at("phase");
    if (Entry.id_.key_.empty() || Hints.at("stageIdentity").empty()) return Invalid;
    if (Phase.size() != 1 || Phase[0] < '0' || Phase[0] > '0' + int(WritePhase::Aborted)) return Invalid;
    auto Stage = ParseLocator(Hints.at("stage")), Backup = ParseLocator(Hints.at("backup"));
    if (!Stage.HasValue() || !Backup.HasValue()) return Invalid;
    return WriteRecoveryRecord{Entry.id_.key_, Entry.locator_, Stage.Value(), Backup.Value(),
        Hints.at("stageIdentity"), Hints.at("originalIdentity"), WritePhase(Phase[0] - '0')};
}
}
