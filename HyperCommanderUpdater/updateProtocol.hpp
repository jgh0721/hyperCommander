#pragma once
#include "updateJson.hpp"
#include "updateTrust.hpp"

namespace hc::update {
struct Manifest {
    std::string version_, minimum_, root_, url_, notes_, inventoryHash_, zipHash_;
    std::uint64_t zipBytes_ = 0, unpackedBytes_ = 0, fileCount_ = 0;
};
struct ProductFile { std::string path_, hash_; std::uint64_t size_ = 0; };
struct Inventory { std::string version_; std::vector<ProductFile> files_; std::uint64_t bytes_ = 0; };
Manifest ParseManifest(std::string_view Bytes);
Inventory ParseInventory(std::string_view Bytes);
Inventory VerifyTree(const Path& Root, const Manifest& Release, bool Strict = true, std::stop_token Stop = {});
}
