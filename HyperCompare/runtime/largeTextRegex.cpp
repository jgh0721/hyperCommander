#include "stdafx.h"
#include "largeText.hpp"
#include <QFile>
#include <algorithm>
#include <exception>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace hc::compare
{
    LargeTextMatch FindLargeTextRegex(const LargeTextDocument& Document, const QString& Query, std::uint64_t Start,
        bool Backwards, Qt::CaseSensitivity Case, const Budget& Limits)
    {
        Limits.Check(); const auto pattern = Query.toUtf8();
        if (pattern.isEmpty() || pattern.size() > 4096 || !Query.isValidUtf16()) throw Failure("LargeTextSearchQueryLimit");
        const auto admission = Limits.memoryReserve_ ? Limits.memoryReserve_(20 * 1024 * 1024) : std::shared_ptr<void>{};
        const auto indexed = IndexLargeText(Document, Limits); const auto& piece = indexed.index_->contiguous_;
        QFile file; uchar* mapped = nullptr;
        if (Document.bytes_) {
            file.setFileName(piece.backing_->file_.fileName());
            if (!file.open(QIODevice::ReadOnly) || !(mapped = file.map(qint64(piece.begin_), qint64(piece.bytes_)))) throw Failure("LargeTextMapFailed");
        }
        struct Unmap { QFile& file_; uchar* data_; ~Unmap() { if (data_) file_.unmap(data_); } } unmap{file, mapped};
        const auto* subject = mapped ? mapped : reinterpret_cast<const uchar*>("");
        int error = 0; PCRE2_SIZE offset = 0;
        const std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> compileContext(pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
        if (!compileContext) throw Failure("LargeTextRegexAllocationFailed");
        pcre2_set_max_pattern_compiled_length(compileContext.get(), 1024 * 1024);
        const auto options = PCRE2_UTF | PCRE2_UCP | PCRE2_AUTO_CALLOUT | PCRE2_NO_START_OPTIMIZE | PCRE2_NEVER_BACKSLASH_C |
            (Case == Qt::CaseInsensitive ? PCRE2_CASELESS : 0);
        const std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.constData()), pattern.size(), options, &error, &offset, compileContext.get()), pcre2_code_free);
        if (!code) { PCRE2_UCHAR message[256]{}; pcre2_get_error_message(error, message, sizeof(message)); throw Failure("LargeTextInvalidRegex:" + std::to_string(offset) + ":" + reinterpret_cast<const char*>(message)); }
        const std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(pcre2_match_context_create(nullptr), pcre2_match_context_free);
        const std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
        if (!context || !data) throw Failure("LargeTextRegexAllocationFailed");
        pcre2_set_heap_limit(context.get(), 16 * 1024); pcre2_set_depth_limit(context.get(), 1000);
        pcre2_set_match_limit(context.get(), uint32_t(std::min<std::uint64_t>(Limits.steps_, 10'000'000)));
        struct Callout { const Budget& limits_; std::uint64_t steps_ = 0; std::exception_ptr error_; } callout{Limits};
        pcre2_set_callout(context.get(), [](pcre2_callout_block*, void* User) -> int {
            auto& state = *static_cast<Callout*>(User);
            try { if (++state.steps_ > state.limits_.steps_) throw Failure("LargeTextRegexWorkLimit"); if (!(state.steps_ & 1023)) state.limits_.Check(); return 0; }
            catch (...) { state.error_ = std::current_exception(); return PCRE2_ERROR_CALLOUT; }
        }, &callout);
        Start = std::min(Start, Document.bytes_);
        while (Start < Document.bytes_ && (subject[Start] & 0xc0) == 0x80) ++Start;
        const auto next = [&](std::uint64_t At) { if (At >= Document.bytes_) return Document.bytes_ + 1; do { ++At; } while (At < Document.bytes_ && (subject[At] & 0xc0) == 0x80); return At; };
        const auto scan = [&](std::uint64_t From, std::uint64_t Until, bool Reverse, bool IncludeEnd = false) {
            LargeTextMatch result;
            while (From <= Document.bytes_) {
                Limits.Check(); const auto status = pcre2_match(code.get(), subject, Document.bytes_, From, PCRE2_NO_UTF_CHECK, data.get(), context.get());
                if (callout.error_) std::rethrow_exception(callout.error_);
                if (status == PCRE2_ERROR_NOMATCH) break;
                if (status < 0) throw Failure("LargeTextRegexLimit:" + std::to_string(status));
                const auto* locations = pcre2_get_ovector_pointer(data.get()); const auto begin = std::uint64_t(locations[0]), end = std::uint64_t(locations[1]);
                if (begin > Until) break;
                if (!Reverse) return LargeTextMatch{true, false, begin, end};
                if (end <= Until && (begin < Until || IncludeEnd)) result = {true, false, begin, end};
                From = next(begin);
            }
            return result;
        };
        auto result = Backwards ? scan(0, Start, true) : scan(Start, Document.bytes_, false);
        if (!result.found_ && (Backwards || Start != 0)) { result = scan(0, Document.bytes_, Backwards, true); result.wrapped_ = result.found_; }
        return result;
    }
}
