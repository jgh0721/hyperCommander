#include "isoRewrite.hpp"
#include "nativeIsoRuntime.hpp"
#include <QDir>

namespace hc::iso {
using namespace storage;
Task<std::shared_ptr<PreparedImage>> PrepareRewrite(RewriteRequest Request, OperationContext Context)
{
    return std::async(std::launch::async, [Request = std::move(Request), Context = std::move(Context)]() mutable -> Result<std::shared_ptr<PreparedImage>> {
        if (auto Error = Context.Check({"iso"})) return *Error;
        auto Module = LoadRuntime(Context); if (!Module.HasValue()) return Module.Failure();
        auto Directory = std::make_unique<QTemporaryDir>(QDir(Request.temporaryBase_.isEmpty() ? QDir::tempPath() : Request.temporaryBase_).filePath("hc-iso-XXXXXX"));
        if (!Directory->isValid()) return Error{ErrorCategory::Unavailable, "iso-staging-directory", {"iso"}};
        auto Prepared = std::make_shared<PreparedImage>(std::move(Directory));
        std::vector<HcIsoSource> Sources; Sources.reserve(Request.sources_.size());
        for (const auto& Source : Request.sources_) {
            if (Source.diskPath_.find('\0') != std::string::npos || Source.imagePath_.find('\0') != std::string::npos)
                return Error{ErrorCategory::InvalidRequest, "iso-source-nul", {"iso"}};
            Sources.push_back({Source.directory_ ? nullptr : Source.diskPath_.c_str(), Source.imagePath_.c_str(), uint32_t(Source.directory_)});
        }
        if (Request.volumeLabel_.find('\0') != std::string::npos)
            return Error{ErrorCategory::InvalidRequest, "iso-label-nul", {"iso"}};
        const HcIsoOptions Options{sizeof(HcIsoOptions), Request.profile_, Request.volumeLabel_.c_str(), Request.maxImageBytes_, Request.timestampUtc_,
            [](void* Opaque, uint64_t Completed, uint64_t) -> int {
                const auto& Context = *static_cast<OperationContext*>(Opaque);
                if (Context.Check({"iso"})) return 0;
                if (Context.progress_) Context.progress_(Completed);
                return Context.Check({"iso"}) ? 0 : 1;
            }, &Context};
        const auto Path = Prepared->Path().toUtf8();
        const uint32_t Flags = (Request.shareDuplicateContents_ ? HC_ISO_SHARE_DUPLICATE_CONTENTS : 0)
            | (Request.explicitTimestamp_ ? HC_ISO_EXPLICIT_TIMESTAMP : 0);
        if (Flags && !Module.Value()->buildEx_) return Error{ErrorCategory::Unsupported, "iso-extended-build-unsupported", {"iso"}};
        const HcIsoOptionsEx Extended{sizeof(HcIsoOptionsEx), 1, Options, Flags};
        const auto Status = Flags ? Module.Value()->buildEx_(Path.constData(), Sources.data(), Sources.size(), &Extended)
            : Module.Value()->build_(Path.constData(), Sources.data(), Sources.size(), &Options);
        if (auto Error = Context.Check({"iso"})) return *Error;
        if (Status != HC_ISO_OK) return EngineError(Status);
        return Prepared;
    });
}
}
