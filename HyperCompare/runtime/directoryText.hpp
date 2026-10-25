#pragma once
#include "directory.hpp"
#include <QByteArray>
#include <array>
#include <optional>

namespace hc::compare
{
    // LEFT, RIGHT, BASE. Nullopt means unsupported text, bounded analysis fallback, or a real conflict.
    // Provider errors, stale revisions and request cancellation remain failures.
    std::optional<QByteArray> MergeDirectoryText(const std::array<storage::EntrySnapshot, 3>& Inputs,
        const QString& Encoding, const Budget& Limits);
}
