#pragma once
#include <array>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace hc::update {
using Path = std::filesystem::path;
std::wstring Wide(std::string_view Text);
std::string Utf8(std::wstring_view Text);
void CheckRelative(std::string_view Relative);
void CheckPlainPath(const Path& File);
std::string ReadBounded(const Path& File, std::uint64_t Limit);
std::string HashFile(const Path& File, std::uint64_t Limit = 536870912, std::stop_token Stop = {});
std::string HashBytes(std::string_view Bytes);
void VerifyProductSignature(const Path& File, std::string_view Relative = {});
std::string AuthenticateManifest(const Path& Catalog, const Path& Manifest);
std::array<unsigned, 4> ParseVersion(std::string_view Version);
void VerifyImageVersion(const Path& File, std::string_view Expected);
void VerifyProductFile(const Path& File, std::string_view Relative, std::string_view ExpectedHash, std::uint64_t ExpectedSize, std::stop_token Stop = {});
}
