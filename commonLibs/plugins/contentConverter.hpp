#pragma once
#include "../storage/materializedContent.hpp"

namespace hc::plugins
{
    enum class ConvertedTextType { Plain, Rich, Html };
    struct ConvertedContent
    {
        ConvertedTextType type_ = ConvertedTextType::Plain;
        std::vector<std::byte> bytes_;
    };
    class IContentConverter
    {
    public:
        virtual ~IContentConverter() = default;
        // Typed data only: consumers decide presentation and never execute HTML
        // or embedded content merely because a plugin returned it.
        virtual storage::Task<ConvertedContent> Convert(std::shared_ptr<storage::IContentSource> Source, storage::OperationContext Context) = 0;
    };
}
