#include "stdafx.h"
#include "updateProtocol.hpp"
#include "updateTransaction.hpp"

namespace hc::update {
namespace {
const auto String = Json::Kind::String, Number = Json::Kind::Number;
void Require(bool Condition, const char* Code) { if (!Condition) throw std::runtime_error(Code); }
bool Hash(std::string_view Value) { if (Value.size() != 64) return false; for (const auto Ch : Value) if (!((Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'))) return false; return true; }
struct LessPath {
    bool operator()(const std::wstring& Left, const std::wstring& Right) const {
        return CompareStringOrdinal(Left.data(), int(Left.size()), Right.data(), int(Right.size()), TRUE) == CSTR_LESS_THAN;
    }
};
}
Manifest ParseManifest(std::string_view Bytes) {
    Require(Bytes.size() <= 262144, "update-manifest-size"); const auto Root = ParseJson(Bytes); Manifest Result;
    Require(Root.At("schema", Number).number_ == 1 && Root.At("product", String).text_ == "HyperCommander" && Root.At("channel", String).text_ == "stable"
        && Root.At("platform", String).text_ == "windows" && Root.At("arch", String).text_ == "x64"
        && Root.At("updaterProtocol", Number).number_ == 1 && Root.At("layoutVersion", Number).number_ == 1, "update-manifest-compatibility");
    Result.version_ = Root.At("version", String).text_; Result.minimum_ = Root.At("minimumFromVersion", String).text_;
    ParseVersion(Result.version_); ParseVersion(Result.minimum_);
    Require(ParseVersion(Result.minimum_) <= ParseVersion(Result.version_), "update-manifest-minimum");
    Require(Root.At("tag", String).text_ == "v" + Result.version_, "update-manifest-tag");
    Result.notes_ = Root.At("notesUrl", String).text_;
    Result.inventoryHash_ = Root.At("inventorySha256", String).text_;
    const auto& Asset = Root.At("asset", Json::Kind::Object);
    Result.root_ = Asset.At("rootDir", String).text_; Result.url_ = Asset.At("url", String).text_; Result.zipHash_ = Asset.At("sha256", String).text_;
    Result.zipBytes_ = Asset.At("size", Number).number_; Result.unpackedBytes_ = Asset.At("unpackedSize", Number).number_; Result.fileCount_ = Asset.At("fileCount", Number).number_;
    Require(Asset.At("kind", String).text_ == "zip" && Hash(Result.zipHash_) && Hash(Result.inventoryHash_)
        && Result.zipBytes_ > 0 && Result.zipBytes_ <= 1073741824 && Result.unpackedBytes_ > 0 && Result.unpackedBytes_ <= 2147483648
        && Result.fileCount_ >= 2 && Result.fileCount_ <= 4096, "update-manifest-asset");
    Require(Result.root_ == "HyperCommander-" + Result.version_ + "-windows-x64-portable", "update-manifest-root");
    Require(Result.url_ == "https://github.com/jgh0721/hyperCommander/releases/download/v" + Result.version_ + "/" + Result.root_ + ".zip"
        && Result.notes_ == "https://github.com/jgh0721/hyperCommander/releases/tag/v" + Result.version_, "update-manifest-url");
    return Result;
}
Inventory ParseInventory(std::string_view Bytes) {
    const auto Root = ParseJson(Bytes); Inventory Result;
    Require(Root.At("schemaVersion", Number).number_ == 1 && Root.At("product", String).text_ == "HyperCommander" && Root.At("configuration", String).text_ == "Release"
        && Root.At("layoutVersion", Number).number_ == 1 && Root.At("updaterProtocol", Number).number_ == 1, "update-inventory-compatibility");
    Result.version_ = Root.At("version", String).text_; ParseVersion(Result.version_);
    const auto& Entries = Root.At("files", Json::Kind::Array).array_;
    Require(!Entries.empty() && Entries.size() < 4096, "update-inventory-count"); std::set<std::wstring, LessPath> Seen;
    for (const auto& Entry : Entries) {
        ProductFile File{Entry.At("path", String).text_, Entry.At("sha256", String).text_, Entry.At("size", Number).number_};
        CheckProductRelative(File.path_);
        Require(File.path_ != "product-inventory.json" && Seen.insert(Wide(File.path_)).second && Hash(File.hash_)
            && File.size_ <= 536870912 && File.size_ <= 2147483648 - Result.bytes_, "update-inventory-file");
        Result.bytes_ += File.size_; Result.files_.push_back(std::move(File));
    }
    Require(Seen.contains(L"HyperCommander.exe") && Seen.contains(L"HyperCommanderAdmin.exe") && Seen.contains(L"HyperEditor.exe")
        && Seen.contains(L"Libs/platforms/qwindows.dll"), "update-inventory-required");
    return Result;
}
Inventory VerifyTree(const Path& Root, const Manifest& Release, bool Strict, std::stop_token Stop) {
    CheckPlainPath(Root); const auto InventoryPath = Root / L"product-inventory.json";
    const auto Bytes = ReadBounded(InventoryPath, 1048576);
    Require(HashBytes(Bytes) == Release.inventoryHash_, "update-inventory-hash");
    auto Inventory = ParseInventory(Bytes);
    Require(Inventory.version_ == Release.version_ && Inventory.files_.size() + 1 == Release.fileCount_
        && Inventory.bytes_ + Bytes.size() == Release.unpackedBytes_, "update-inventory-release");
    std::set<std::wstring, LessPath> Expected{L"product-inventory.json"};
    for (const auto& File : Inventory.files_) {
        if (Stop.stop_requested()) throw std::runtime_error("update-cancelled");
        Expected.insert(Wide(File.path_)); VerifyProductFile(Root / Wide(File.path_), File.path_, File.hash_, File.size_, Stop);
    }
    VerifyImageVersion(Root / L"HyperCommander.exe", Release.version_);
    if (Strict) {
        std::size_t Count = 0;
        for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root)) {
            CheckPlainPath(Entry.path());
            if (Entry.is_directory()) continue;
            const auto Relative = Entry.path().lexically_relative(Root).generic_wstring();
            Require(Expected.contains(Relative) && ++Count <= Expected.size(), "update-staging-extra-file");
        }
        Require(Count == Expected.size(), "update-staging-missing-file");
    }
    return Inventory;
}
}
