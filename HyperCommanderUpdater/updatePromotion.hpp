#pragma once
#include "updateTrust.hpp"

namespace hc::update {
/// Done: every ROOT copy matches its carried file. Pending: an active update cannot take the
/// copies yet. Busy: a copy runs or another promotion holds the root. Foreign: a ROOT copy is
/// not a product file and is kept. Failed: the copy could not be written.
enum class PromotionStatus { Done, Pending, Busy, Foreign, Failed };
struct PromotionResult {
    PromotionStatus status_ = PromotionStatus::Done;
    std::vector<std::string> promoted_;  // ROOT names this call replaced
    std::string error_;                  // Failed: the update-promote-* code and details
    unsigned long win32_ = 0;            // Failed: the Win32 error, or 0
};
/// Copies the carried root files that the installed inventory lists under layout::RootCarrier
/// to ROOT when the ROOT copy is missing or an older product-signed copy. A program and its
/// worker change together. Works on any writable volume and while an applied update awaits
/// its health check (the copies join that transaction). Never throws.
PromotionResult PromoteRootFiles(const Path& Root);
const char* PromotionStatusName(PromotionStatus Status);
}
