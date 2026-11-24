#pragma once
#include "commonLibs/storage/storage.hpp"
#include "include/hcIso/isoImage.h"
#include <QString>
#include "commonLibs/storage/nativeTemporaryDirectory.hpp"

namespace hc::iso {
struct RewriteSource { std::string diskPath_, imagePath_; bool directory_ = false; HcIsoMetadata metadata_{}; };
struct RewriteRequest {
    std::vector<RewriteSource> sources_;
    uint32_t profile_ = HC_ISO_UDF_BRIDGE;
    std::string volumeLabel_ = "HYPERCOMMANDER";
    uint64_t maxImageBytes_ = 0;
    QString temporaryBase_;
    bool shareDuplicateContents_ = false;
    int64_t timestampUtc_ = 0;
    bool explicitTimestamp_ = false;
    bool verifyContents_ = true;
};
// Owns an exclusive staging image. Publication/replacement
// belongs to the common commit coordinator, not to the ISO engine.
class PreparedImage final {
public:
    explicit PreparedImage(std::unique_ptr<storage::native::TemporaryDirectory> Directory) : directory_(std::move(Directory)) {}
    QString Path() const { return directory_->FilePath("image.iso"); }
private:
    std::unique_ptr<storage::native::TemporaryDirectory> directory_;
};
storage::Task<std::shared_ptr<PreparedImage>> PrepareRewrite(RewriteRequest Request,
    storage::OperationContext Context = {});
}
