#include "approvedWcxBackend.hpp"
#include <QFileInfo>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        class Backend final : public IArchiveBackend
        {
        public:
            explicit Backend(ApprovedWcxOptions Options) : options_(std::move(Options)) {}
            Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Archive, bool Test, OperationContext Context) override
            {
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                return Host.Value()->Enumerate(std::move(Archive), Test, Context);
            }
            Result<Unit> Extract(std::string Archive, ArchiveHeader Expected, std::string Output, OperationContext Context) override
            {
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                return Host.Value()->Extract(std::move(Archive), std::move(Expected), std::move(Output), Context);
            }
            Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override
            {
                auto Host = connect(Context); if (!Host.HasValue()) return Host.Failure();
                return Host.Value()->Update(std::move(Request), Context);
            }
        private:
            Result<std::shared_ptr<IArchiveBackend>> connect(OperationContext Context) const
            {
                if (const auto Failure = Context.Check({"wcx"})) return *Failure;
                if (!options_.approval_.approved_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wcx"}};
                auto Inspected = InspectManifestFile(options_.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
                const auto ExpectedBinary = QFileInfo(options_.binaryPath_).canonicalFilePath();
                if (ExpectedBinary.isEmpty() || ExpectedBinary.compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
                    return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch", {"wcx"}};
                auto Started = PluginProcess::Start(options_.executable_, options_.manifestPath_, options_.approval_, options_.limits_, Context);
                if (!Started.HasValue()) return Started.Failure();
                return MakeHostedArchiveBackend(std::shared_ptr<PluginProcess>(std::move(Started.Value())));
            }
            ApprovedWcxOptions options_;
        };
    }
    std::shared_ptr<IArchiveBackend> MakeApprovedWcxBackend(ApprovedWcxOptions Options) { return std::make_shared<Backend>(std::move(Options)); }
}
