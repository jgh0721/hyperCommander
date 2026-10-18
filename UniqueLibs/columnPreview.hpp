#pragma once
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/storage/materializedContent.hpp"
namespace hc::columns {
    struct PreviewSample {
        QString identity_, path_, name_;
        bool directory_ = false;
        FieldValues values_;
        std::shared_ptr<hc::storage::IContentSource> source_;
    };
}
