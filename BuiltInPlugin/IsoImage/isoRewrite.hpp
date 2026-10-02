#pragma once
#include "commonLibs/storage/storage.hpp"
#include "include/hcIso/isoImage.h"
#include <QString>
#include <QTemporaryDir>

namespace hc::iso {
struct RewriteSource { std::string diskPath_, imagePath_; bool directory_ = false; };
struct RewriteRequest {
    std::vector<RewriteSource> sources_;
    uint32_t profile_ = HC_ISO_UDF_BRIDGE;
    std::string volumeLabel_ = "HYPERCOMMANDER";
    uint64_t maxImageBytes_ = 0;
    QString temporaryBase_;
};
// Owns an exclusive, payload-validated staging image. Publication/replacement
// belongs to the common commit coordinator, not to the ISO engine.
class PreparedImage final {
public:
    explicit PreparedImage(std::unique_ptr<QTemporaryDir> Directory) : directory_(std::move(Directory)) {}
    QString Path() const { return directory_->filePath("image.iso"); }
private:
    std::unique_ptr<QTemporaryDir> directory_;
};
storage::Task<std::shared_ptr<PreparedImage>> PrepareRewrite(RewriteRequest Request,
    storage::OperationContext Context = {});
}
