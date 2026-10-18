#pragma once
#include "../operations/jobScheduler.hpp"
#include <span>

namespace hc::viewer
{
    using storage::Error;
    using storage::Result;
    enum class ViewerPurpose { QuickPreview, FullViewer };
    enum class ViewerStatus { Loading, Ready, Failed, Cancelled, Closed };
    enum class ViewerKind { Text, Markdown, Image };
    struct ViewerLimits
    {
        std::uint64_t maxInputBytes_ = 128ULL * 1024 * 1024;
        std::size_t maxTextCharacters_ = 16 * 1024 * 1024;
        std::size_t maxMarkdownCharacters_ = 512 * 1024;
        std::uint64_t maxImagePixels_ = 32ULL * 1024 * 1024;
        std::uint64_t maxDecodedBytes_ = 128ULL * 1024 * 1024;
    };
    struct ViewerRequest
    {
        std::shared_ptr<storage::IContentSource> source_;
        ViewerPurpose purpose_ = ViewerPurpose::FullViewer;
        std::string provider_;
        // auto, utf-8, utf-16le/be, utf-32le/be, cp949, system.
        std::string encoding_ = "auto";
        ViewerLimits limits_;
    };
    struct ViewerDocument
    {
        ViewerKind kind_ = ViewerKind::Text;
        std::u16string text_;
        std::string encoding_;
        // Straight-alpha RGBA8888, rows tightly packed, no Qt/native objects.
        std::vector<std::byte> pixels_;
        std::uint32_t width_ = 0, height_ = 0;
        std::string imageFormat_;
        std::uint64_t Bytes() const { return text_.size() * sizeof(char16_t) + pixels_.size(); }
    };
    struct ViewerSnapshot
    {
        std::string id_, provider_;
        ViewerStatus status_ = ViewerStatus::Loading;
        storage::EntrySnapshot source_;
        std::uint64_t bytesRead_ = 0;
        std::optional<Error> error_;
    };
    struct ViewerInput
    {
        ViewerRequest request_;
        storage::EntrySnapshot source_;
        // Reuse the probing cursor, including its archive lease. No second
        // extraction/open is required for the selected built-in provider.
        std::shared_ptr<storage::IReadStream> stream_;
        std::vector<std::byte> prefix_;
        bool eof_ = false;
    };
    class IViewerProvider
    {
    public:
        virtual ~IViewerProvider() = default;
        virtual std::string Id() const = 0;
        // Pure worker-side probe; 0 means unsupported, 1..100 is preference.
        virtual Result<int> Probe(const storage::EntrySnapshot& Source, std::span<const std::byte> Prefix) const = 0;
        virtual Result<ViewerDocument> Open(ViewerInput Input, storage::OperationContext Context,
            std::function<void(std::uint64_t)> Progress) = 0;
        virtual std::vector<std::string> Commands() const = 0;
    };
    class ViewerRegistry
    {
    public:
        Result<storage::Unit> Register(std::shared_ptr<IViewerProvider> Provider);
        std::vector<std::shared_ptr<IViewerProvider>> Providers() const;
    private:
        mutable std::mutex mutex_;
        std::map<std::string, std::shared_ptr<IViewerProvider>> providers_;
    };
    class IViewerSession
    {
    public:
        virtual ~IViewerSession() = default;
        virtual ViewerSnapshot Snapshot() const = 0;
        virtual std::shared_ptr<const ViewerDocument> Document() const = 0;
        virtual std::vector<std::string> Commands() const = 0;
        virtual void Cancel() = 0;
        virtual void Close() = 0;
    };
    struct ViewerOpenHandle
    {
        std::shared_ptr<IViewerSession> session_;
        // Ready means actual read/verification/decode and cursor cleanup ended.
        // Presentation readiness is separately reported by the Qt surface.
        storage::Task<storage::Unit> opened_;
    };
    struct ViewerServiceLimits
    {
        std::size_t workers_ = 2;
        std::size_t maxSessions_ = 16;
        std::uint64_t maxRetainedBytes_ = 256ULL * 1024 * 1024;
    };
    class ViewerService
    {
    public:
        // Create/destroy on the owning thread. GUI hosts cancel outstanding
        // sessions and wait asynchronously for IsIdle() before destruction.
        explicit ViewerService(std::shared_ptr<ViewerRegistry> Registry, ViewerServiceLimits Limits = {});
        ~ViewerService();
        Result<ViewerOpenHandle> Open(ViewerRequest Request, storage::OperationContext Context = {});
        void CancelAll();
        bool IsIdle() const;
        void Collect();
        std::uint64_t RetainedBytes() const;
    private:
        struct Backend;
        std::shared_ptr<Backend> backend_;
    };
}
