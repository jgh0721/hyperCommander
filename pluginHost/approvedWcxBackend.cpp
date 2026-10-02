#include "approvedWcxBackend.hpp"
#include <QFileInfo>
#include <QThreadPool>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        class PromptListing final : public IArchiveListing
        {
        public:
            PromptListing(std::unique_ptr<IArchiveListing> Inner, PasswordPrompt Prompt) : inner_(std::move(Inner)), prompt_(std::move(Prompt)) {}
            Result<ArchivePage> Next(std::size_t Count, OperationContext Context) override
            { if (!Context.passwordPrompt_) Context.passwordPrompt_ = prompt_; return inner_->Next(Count, std::move(Context)); }
        private:
            std::unique_ptr<IArchiveListing> inner_;
            PasswordPrompt prompt_;
        };
        class Backend final : public IArchiveBackend
        {
        public:
            explicit Backend(ApprovedWcxOptions Options) : options_(std::move(Options)) {}
            Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Archive, bool Test, OperationContext Context) override
            {
                if (!Context.passwordPrompt_) Context.passwordPrompt_ = options_.passwordPrompt_;
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                auto Listed = Host.Value()->Enumerate(std::move(Archive), Test, Context); if (!Listed.HasValue()) return Listed.Failure();
                return std::unique_ptr<IArchiveListing>(new PromptListing(std::move(Listed.Value()), Context.passwordPrompt_));
            }
            Result<Unit> Extract(std::string Archive, ArchiveHeader Expected, std::string Output, OperationContext Context) override
            {
                if (!Context.passwordPrompt_) Context.passwordPrompt_ = options_.passwordPrompt_;
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                return Host.Value()->Extract(std::move(Archive), std::move(Expected), std::move(Output), Context);
            }
            Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override
            {
                if (!Context.passwordPrompt_) Context.passwordPrompt_ = options_.passwordPrompt_;
                if (!options_.approval_.approved_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wcx"}};
                auto Policy = ArchiveWritePolicy(options_.approval_.reviewed_); if (!Policy.HasValue()) return Policy;
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                return Host.Value()->Update(std::move(Request), Context);
            }
        private:
            Result<std::shared_ptr<IArchiveBackend>> connect(OperationContext Context)
            {
                std::unique_lock Lock(mutex_, std::defer_lock);
                while (!Lock.try_lock_for(std::chrono::milliseconds(25))) if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                if (!options_.approval_.approved_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wcx"}};
                auto Inspected = InspectManifestFile(options_.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
                if (Inspected.Value().manifest_ != options_.approval_.reviewed_)
                    return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wcx"}};
                const auto ExpectedBinary = QFileInfo(options_.binaryPath_).canonicalFilePath();
                if (ExpectedBinary.isEmpty() || ExpectedBinary.compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
                    return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch", {"wcx"}};
                if (process_ && process_->Snapshot().running_) return MakeHostedArchiveBackend(process_);
                // Faulted epochs are never replayed. A subsequent operation may
                // start a newly admitted host; old listings keep the old epoch.
                process_.reset();
                auto Started = PluginProcess::Start(options_.executable_, options_.manifestPath_, options_.approval_, options_.limits_, Context, options_.defaultIniPath_);
                if (!Started.HasValue()) return Started.Failure();
                process_ = std::shared_ptr<PluginProcess>(Started.Value().release(), [](PluginProcess* Value) {
                    QThreadPool::globalInstance()->start([Value] { delete Value; });
                });
                return MakeHostedArchiveBackend(process_);
            }
            ApprovedWcxOptions options_;
            std::timed_mutex mutex_;
            std::shared_ptr<PluginProcess> process_;
        };
    }
    std::shared_ptr<IArchiveBackend> MakeApprovedWcxBackend(ApprovedWcxOptions Options) { return std::make_shared<Backend>(std::move(Options)); }
}
