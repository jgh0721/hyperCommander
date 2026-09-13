// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// C++ port of PyCdlib 1.20.0. See packaging/source-lock.json.
#pragma once
#include "hcIso/isoImage.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace hc::iso {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t Block = 2048;
constexpr uint64_t MaxMetadata = 256ull * 1024 * 1024;
constexpr uint64_t MaxEntries = 100000;
struct Error : std::runtime_error {
    int status_;
    Error(int Status, const std::string& Message) : runtime_error(Message), status_(Status) {}
};
inline void Require(bool Value, const char* Message, int Status = HC_ISO_INVALID)
{ if (!Value) throw Error(Status, Message); }
inline uint64_t Round(uint64_t Value, uint64_t Alignment = Block)
{ Require(Value <= UINT64_MAX - Alignment, "Size overflow"); return (Value + Alignment - 1) / Alignment * Alignment; }
uint64_t Get(std::span<const uint8_t> Data, size_t Offset, size_t Width, bool Big = false);
void Put(Bytes& Data, size_t Offset, uint64_t Value, size_t Width, bool Big = false);
void Both(Bytes& Data, size_t Offset, uint32_t Value, size_t Width);
void Text(Bytes& Data, size_t Offset, std::string_view Value, size_t Width, uint8_t Pad = 0);
std::u16string DecodeUtf8(std::string_view Value);
std::string EncodeUtf8(std::u16string_view Value);
Bytes Unicode(std::string_view Value, bool Osta);
std::string DecodeUnicode(std::span<const uint8_t> Value, bool Osta);
std::string CleanPath(std::string_view Value);
fs::path DiskPath(const char* Value);
uint16_t Crc16(std::span<const uint8_t> Value);
void Tag(Bytes& Data, uint16_t Id, uint32_t Location, size_t Length);
void CheckTag(std::span<const uint8_t> Data, uint32_t Location, uint16_t Id = 0);
void Timestamp(Bytes& Data, size_t Offset, int64_t Epoch, bool Udf);
void Pulse(const HcIsoOptions& Options, uint64_t Done, uint64_t Total);

struct Extent { uint64_t offset_; uint64_t size_; };
struct Entry {
    std::string path_;
    bool directory_ = false;
    uint64_t size_ = 0;
    std::vector<Extent> extents_;
    Bytes embedded_;
};
class Reader {
public:
    Reader(const fs::path& Path, uint32_t Namespace);
    Bytes ReadAt(uint64_t Offset, uint64_t Size);
    uint32_t Read(size_t Index, uint64_t Offset, std::span<uint8_t> Buffer);
    const std::vector<Entry>& Entries() const { return entries_; }
    const HcIsoInfo& Info() const { return info_; }
private:
    void readIso(const Bytes& Descriptor, bool Joliet);
    void readUdf();
    void add(Entry Value);
    std::ifstream stream_;
    HcIsoInfo info_{};
    std::vector<Entry> entries_;
    std::set<std::string> paths_;
    uint64_t metadataBytes_ = 0;
};
struct Node {
    std::string name_, path_, isoName_;
    fs::path source_;
    fs::file_time_type modified_{};
    uint64_t size_ = 0;
    bool directory_ = false;
    size_t parent_ = 0;
    std::vector<size_t> children_;
    uint32_t dataBlock_ = 0, isoBlock_ = 0, jolietBlock_ = 0, feBlock_ = 0;
    Bytes isoData_, jolietData_, udfData_;
};
class Writer {
public:
    Writer(const HcIsoSource* Sources, uint64_t Count, const HcIsoOptions& Options);
    void Write(const fs::path& Path);
private:
    void inventory(const HcIsoSource* Sources, uint64_t Count);
    bool hasJoliet() const { return options_.profile_ == HC_ISO_9660_JOLIET || options_.profile_ == HC_ISO_UDF_BRIDGE; }
    bool hasUdf() const { return options_.profile_ == HC_ISO_UDF_BRIDGE || options_.profile_ == HC_ISO_UDF_NO_JOLIET; }
    void layout();
    Bytes directory(size_t Index, bool Joliet);
    Bytes pathTable(bool Joliet, bool Big);
    Bytes volume(bool Joliet);
    void udfLayout();
    void udfWrite(std::ofstream& Stream);
    void writeAt(std::ofstream& Stream, uint32_t Sector, const Bytes& Data);
    HcIsoOptions options_;
    std::string label_;
    std::vector<Node> nodes_;
    std::vector<size_t> directories_;
    uint32_t nextBlock_ = 0, totalBlocks_ = 0, partitionEnd_ = 0;
    std::array<uint32_t, 4> tableBlocks_{};
    uint64_t payloadBytes_ = 0;
};
}
