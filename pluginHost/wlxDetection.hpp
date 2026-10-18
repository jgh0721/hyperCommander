#pragma once
#include "commonLibs/storage/storage.hpp"
#include <QByteArray>

namespace hc::plugins
{
    struct WlxDetectionInput
    {
        // The original resource's ANSI extension, without '.', not the name of
        // a materialized temporary file. Prefix must contain min(size, 8192)
        // bytes from the same revision. No I/O or plugin calls occur here.
        QByteArray extension_, prefix_;
        std::uint64_t size_ = 0;
        bool force_ = false;
    };
    struct WlxDetectionMatch
    {
        // A byte beyond actual EOF is unknown, never a synthetic zero byte.
        // Unknown propagates through NOT; decisive AND/OR operands still work.
        std::optional<bool> matches_;
        bool multimedia_ = false;
    };
    class WlxDetectionExpression final
    {
    public:
        static storage::Result<WlxDetectionExpression> Parse(const QByteArray& Expression);
        storage::Result<WlxDetectionMatch> Evaluate(const WlxDetectionInput& Input) const;
        bool NeedsPrefix() const;
        bool NeedsExtension() const;
    private:
        struct Program;
        explicit WlxDetectionExpression(std::shared_ptr<const Program> Value) : program_(std::move(Value)) {}
        std::shared_ptr<const Program> program_;
    };
}
