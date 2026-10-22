#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hc::compare
{
    struct Failure : std::runtime_error
    {
        std::string code_;
        explicit Failure(std::string Code) : std::runtime_error(Code), code_(std::move(Code)) {}
    };
    struct Budget
    {
        std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const std::atomic_bool* canceled_ = nullptr;
        std::uint64_t traceBytes_ = 64 * 1024 * 1024, steps_ = 50'000'000;
        void Check() const;
    };
    // Lines retain their terminators: EOF and CRLF/LF differences remain edits.
    using Lines = std::vector<std::string_view>;
    Lines SplitLines(std::string_view Text);
    struct Edit
    {
        std::size_t begin_ = 0, end_ = 0, otherBegin_ = 0, otherEnd_ = 0;
        std::string replacement_;
        bool operator==(const Edit&) const = default;
    };
    std::vector<Edit> Diff(std::string_view Left, std::string_view Right, const Budget& Limits = {});
    std::string Apply(std::string_view Base, const std::vector<Edit>& Edits);
    struct Conflict
    {
        std::size_t baseBegin_ = 0, baseEnd_ = 0, resultBegin_ = 0, resultEnd_ = 0;
        std::size_t leftBegin_ = 0, rightBegin_ = 0;
        std::string base_, left_, right_;
    };
    struct MergeResult { std::string text_; std::vector<Conflict> conflicts_; };
    MergeResult Merge(std::string_view Base, std::string_view Left, std::string_view Right, const Budget& Limits = {});
    // Versioned opaque locations keep future image/Office engines out of the text model.
    struct Location { std::string schema_, resourceRevision_, payload_; std::uint32_t version_ = 1; };
    enum class Completion { Complete, Partial, Failed, Canceled };
    enum class Availability { Supported, ReadOnly, Unsupported };
    struct Capability
    {
        std::string engine_, format_, reason_;
        Availability compare_ = Availability::Unsupported, merge_ = Availability::Unsupported;
    };
}
