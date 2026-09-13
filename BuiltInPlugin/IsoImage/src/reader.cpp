// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// Ported from PyCdlib headervd.py, dr.py and udf.py. Unsupported allocation
// schemes fail explicitly; no namespace fallback after a corrupt selected tree.
#include "image.hpp"
#include <cstring>
#include <functional>

namespace hc::iso {
namespace {
uint32_t Paired(std::span<const uint8_t> Data, size_t Offset, size_t Width)
{
    const auto Value = Get(Data, Offset, Width);
    Require(Value == Get(Data, Offset + Width, Width, true), "ISO endian copies disagree");
    return uint32_t(Value);
}
}
Reader::Reader(const fs::path& Path, uint32_t Namespace) : stream_(Path, std::ios::binary)
{
    Require(bool(stream_), "Cannot open ISO", HC_ISO_IO);
    info_.structSize_ = sizeof(info_);
    info_.imageBytes_ = fs::file_size(Path);
    Require(info_.imageBytes_ >= 18 * Block && info_.imageBytes_ % Block == 0, "Truncated ISO image");
    Bytes Primary, Joliet;
    bool End = false, Udf = false;
    for (uint32_t Sector = 16; Sector < 80 && uint64_t(Sector + 1) * Block <= info_.imageBytes_; ++Sector) {
        const auto Data = ReadAt(uint64_t(Sector) * Block, Block);
        if (std::memcmp(Data.data() + 1, "CD001", 5) == 0 && !End) {
            Require(Data[6] == 1, "Unsupported ISO descriptor version", HC_ISO_UNSUPPORTED);
            if (Data[0] == 1) Primary = Data;
            if (Data[0] == 0) info_.features_ |= 1;
            if (Data[0] == 2 && Data[88] == '%' && Data[89] == '/' && (Data[90] == '@' || Data[90] == 'C' || Data[90] == 'E')) Joliet = Data;
            if (Data[0] == 255) End = true;
        }
        if (std::memcmp(Data.data() + 1, "NSR02", 5) == 0 || std::memcmp(Data.data() + 1, "NSR03", 5) == 0) Udf = true;
    }
    Require(!Primary.empty() && End, "Missing ISO primary descriptor or terminator");
    Require(Paired(Primary, 128, 2) == Block && uint64_t(Paired(Primary, 80, 4)) * Block <= info_.imageBytes_, "Invalid ISO volume extent");
    const auto PrimaryRoot = ReadAt(uint64_t(Paired(Primary, 158, 4)) * Block, Block);
    Require(PrimaryRoot[0] >= 34, "Missing primary root self record");
    if (PrimaryRoot[0] > 34 && std::any_of(PrimaryRoot.begin() + 34, PrimaryRoot.begin() + PrimaryRoot[0], [](auto B) { return B != 0; })) info_.features_ |= 2;
    std::string Label(reinterpret_cast<const char*>(Primary.data() + 40), 32);
    while (!Label.empty() && Label.back() == ' ') Label.pop_back();
    std::memcpy(info_.volumeLabel_, Label.data(), Label.size());
    info_.namespaces_ = 1u << HC_ISO_PRIMARY;
    if (!Joliet.empty()) info_.namespaces_ |= 1u << HC_ISO_JOLIET;
    if (Udf) info_.namespaces_ |= 1u << HC_ISO_UDF;
    if (Namespace == HC_ISO_AUTO) Namespace = Udf ? HC_ISO_UDF : !Joliet.empty() ? HC_ISO_JOLIET : HC_ISO_PRIMARY;
    Require(Namespace >= HC_ISO_PRIMARY && Namespace <= HC_ISO_UDF && (info_.namespaces_ & (1u << Namespace)), "Requested namespace absent", HC_ISO_UNSUPPORTED);
    info_.selectedNamespace_ = Namespace;
    if (Namespace == HC_ISO_UDF) readUdf(); else readIso(Namespace == HC_ISO_JOLIET ? Joliet : Primary, Namespace == HC_ISO_JOLIET);
    info_.entryCount_ = entries_.size();
}
Bytes Reader::ReadAt(uint64_t Offset, uint64_t Size)
{
    Require(Offset <= info_.imageBytes_ && Size <= info_.imageBytes_ - Offset && Size <= MaxMetadata, "Extent outside image or read budget");
    Bytes Data(static_cast<size_t>(Size));
    stream_.clear(); stream_.seekg(std::streamoff(Offset));
    stream_.read(reinterpret_cast<char*>(Data.data()), std::streamsize(Size));
    Require(uint64_t(stream_.gcount()) == Size, "Short ISO read", HC_ISO_IO);
    return Data;
}
uint32_t Reader::Read(size_t Index, uint64_t Offset, std::span<uint8_t> Buffer)
{
    Require(Index < entries_.size(), "Invalid entry index");
    const auto& Item = entries_[Index];
    Require(!Item.directory_ && Offset <= Item.size_, "Invalid file read");
    const auto Wanted = size_t(std::min<uint64_t>(Buffer.size(), Item.size_ - Offset));
    if (!Item.embedded_.empty()) { std::copy_n(Item.embedded_.begin() + size_t(Offset), Wanted, Buffer.begin()); return uint32_t(Wanted); }
    size_t Done = 0;
    for (const auto& Ext : Item.extents_) {
        if (Offset >= Ext.size_) { Offset -= Ext.size_; continue; }
        const auto Size = std::min<uint64_t>(Wanted - Done, Ext.size_ - Offset);
        const auto Data = ReadAt(Ext.offset_ + Offset, Size);
        std::copy(Data.begin(), Data.end(), Buffer.begin() + Done);
        Done += size_t(Size); Offset = 0;
        if (Done == Wanted) break;
    }
    Require(Done == Wanted, "Allocation shorter than file");
    return uint32_t(Done);
}
void Reader::add(Entry Value)
{
    CleanPath(Value.path_);
    Require(entries_.size() < MaxEntries && paths_.insert(Value.path_).second, "Duplicate path or entry budget exceeded");
    metadataBytes_ += Value.path_.size() + sizeof(Entry) + Value.extents_.size() * sizeof(Extent);
    Require(metadataBytes_ <= MaxMetadata, "Metadata budget exceeded");
    entries_.push_back(std::move(Value));
}
void Reader::readIso(const Bytes& Descriptor, bool Joliet)
{
    Require(Paired(Descriptor, 128, 2) == Block, "Unsupported ISO block size", HC_ISO_UNSUPPORTED);
    std::set<uint32_t> Visited;
    std::function<void(uint32_t, uint32_t, const std::string&, unsigned)> Walk;
    Walk = [&](uint32_t Sector, uint32_t Length, const std::string& Parent, unsigned Depth) {
        Require(Depth <= 128 && Visited.insert(Sector).second, "Cyclic or excessively deep ISO directory");
        metadataBytes_ += Length; Require(metadataBytes_ <= MaxMetadata, "Directory budget exceeded");
        const auto Data = ReadAt(uint64_t(Sector) * Block, Length);
        Entry Pending; bool Multi = false;
        for (size_t Offset = 0; Offset < Data.size();) {
            const auto Size = Data[Offset];
            if (!Size) { Offset = size_t(Round(Offset + 1)); continue; }
            Require(Size >= 34 && Size <= Data.size() - Offset && Offset % Block + Size <= Block, "Invalid ISO directory record");
            const auto Rec = std::span(Data).subspan(Offset, Size); Offset += Size;
            Require(Rec[1] == 0 && Rec[26] == 0 && Rec[27] == 0, "Extended attributes/interleaving unsupported", HC_ISO_UNSUPPORTED);
            const auto NameSize = Rec[32]; Require(NameSize && 33u + NameSize <= Rec.size(), "Invalid ISO identifier");
            if (NameSize == 1 && Rec[33] <= 1) continue;
            const auto NameData = Rec.subspan(33, NameSize);
            auto Name = Joliet ? DecodeUnicode(NameData, false) : std::string(reinterpret_cast<const char*>(NameData.data()), NameData.size());
            const bool Directory = (Rec[25] & 2) != 0;
            if (!Directory && !Joliet) { const auto Version = Name.rfind(';'); if (Version != std::string::npos) Name.resize(Version); if (!Name.empty() && Name.back() == '.') Name.pop_back(); }
            Require(Name.find('/') == std::string::npos, "Invalid ISO component");
            const auto Path = Parent.empty() ? Name : Parent + '/' + Name;
            const auto ExtentBlock = Paired(Rec, 2, 4), ExtentSize = Paired(Rec, 10, 4);
            Require(uint64_t(ExtentBlock) * Block <= info_.imageBytes_ && ExtentSize <= info_.imageBytes_ - uint64_t(ExtentBlock) * Block, "ISO file extent outside image");
            Require(Paired(Rec, 28, 2) == 1, "Multi-volume ISO unsupported", HC_ISO_UNSUPPORTED);
            if (Multi) Require(!Directory && Pending.path_ == Path, "Broken ISO multi-extent chain");
            else { Pending = {}; Pending.path_ = Path; Pending.directory_ = Directory; }
            Pending.size_ += ExtentSize; Pending.extents_.push_back({uint64_t(ExtentBlock) * Block, ExtentSize});
            Multi = (Rec[25] & 128) != 0;
            Require(!Directory || !Multi, "Multi-extent directory unsupported", HC_ISO_UNSUPPORTED);
            if (!Multi) { if (Directory) Pending.size_ = 0; add(std::move(Pending)); if (Directory) Walk(ExtentBlock, ExtentSize, Path, Depth + 1); }
        }
        Require(!Multi, "Unterminated ISO multi-extent chain");
    };
    Walk(Paired(Descriptor, 158, 4), Paired(Descriptor, 166, 4), {}, 0);
}
void Reader::readUdf()
{
    const auto Anchor = ReadAt(256ull * Block, Block); CheckTag(Anchor, 256, 2);
    const auto SequenceBytes = Get(Anchor, 16, 4), SequenceBlock = Get(Anchor, 20, 4);
    Require(SequenceBytes >= Block && SequenceBytes <= 256 * Block, "Invalid UDF descriptor sequence");
    Bytes Partition, Logical;
    bool Terminated = false;
    for (uint32_t I = 0; I < SequenceBytes / Block; ++I) {
        const uint32_t Sector = uint32_t(SequenceBlock + I);
        const auto Data = ReadAt(uint64_t(Sector) * Block, Block); CheckTag(Data, Sector);
        const auto Id = Get(Data, 0, 2);
        if (Id == 5) { Require(Partition.empty(), "Multiple UDF partitions unsupported", HC_ISO_UNSUPPORTED); Partition = Data; }
        if (Id == 6) Logical = Data;
        if (Id == 8) { Terminated = true; break; }
    }
    Require(Terminated && !Partition.empty() && !Logical.empty(), "Incomplete UDF descriptors");
    Require(Get(Logical, 212, 4) == Block, "Unsupported UDF block size", HC_ISO_UNSUPPORTED);
    Require(Get(Logical, 264, 4) == 6 && Get(Logical, 268, 4) == 1 && Logical[440] == 1 && Logical[441] == 6,
            "UDF virtual/metadata/sparable partition unsupported", HC_ISO_UNSUPPORTED);
    Require(Get(Logical, 444, 2) == Get(Partition, 22, 2) && Get(Logical, 256, 2) == 0, "Invalid UDF partition reference");
    info_.udfRevision_ = uint32_t(Get(Logical, 240, 2));
    const uint64_t Start = Get(Partition, 188, 4), Length = Get(Partition, 192, 4);
    Require(Start <= info_.imageBytes_ / Block && Length <= info_.imageBytes_ / Block - Start, "UDF partition outside image");
    auto ReadPartition = [&](uint32_t Sector, uint64_t Size) {
        Require(Sector <= Length && Size <= (Length - Sector) * Block, "Extent outside UDF partition");
        return ReadAt((Start + Sector) * Block, Size);
    };
    const uint32_t FsdBlock = uint32_t(Get(Logical, 252, 4));
    const auto Fsd = ReadPartition(FsdBlock, Block); CheckTag(Fsd, FsdBlock, 256);
    Require(Get(Fsd, 408, 2) == 0, "Invalid root partition reference");
    std::set<uint32_t> Visited;
    std::function<void(uint32_t, const std::string&, unsigned)> Walk;
    Walk = [&](uint32_t Sector, const std::string& Path, unsigned Depth) {
        Require(Depth <= 128 && Visited.insert(Sector).second, "Cyclic UDF tree or shared ICB unsupported");
        const auto Fe = ReadPartition(Sector, Block); CheckTag(Fe, Sector);
        const auto Id = Get(Fe, 0, 2); Require(Id == 261 || Id == 266, "Unsupported UDF ICB", HC_ISO_UNSUPPORTED);
        const bool Extended = Id == 266;
        Require(Get(Fe, 20, 2) == 4 && (Fe[27] == 4 || Fe[27] == 5), "UDF strategy/special file unsupported", HC_ISO_UNSUPPORTED);
        Entry Item; Item.path_ = Path; Item.directory_ = Fe[27] == 4; Item.size_ = Get(Fe, 56, 8);
        const size_t Header = Extended ? 216 : 176;
        const auto EaLength = Get(Fe, Header - 8, 4), AdLength = Get(Fe, Header - 4, 4);
        Require(EaLength <= Block - Header && AdLength <= Block - Header - EaLength, "Invalid UDF allocation length");
        Require(Get(Fe, 10, 2) >= Header - 16 + EaLength + AdLength, "UDF CRC excludes allocation descriptors");
        const auto Ads = std::span(Fe).subspan(Header + size_t(EaLength), size_t(AdLength));
        const auto Kind = Get(Fe, 34, 2) & 7;
        if (Kind == 3) { Require(Item.size_ <= Ads.size(), "Embedded UDF file truncated"); Item.embedded_.assign(Ads.begin(), Ads.begin() + size_t(Item.size_)); }
        else {
            Require(Kind <= 1, "Unsupported UDF allocation type", HC_ISO_UNSUPPORTED);
            const size_t Width = Kind == 0 ? 8 : 16; Require(Ads.size() % Width == 0, "Truncated allocation descriptor");
            uint64_t Allocated = 0;
            for (size_t Offset = 0; Offset < Ads.size(); Offset += Width) {
                const auto Size = Get(Ads, Offset, 4), Location = Get(Ads, Offset + 4, 4);
                Require((Size >> 30) == 0, "Sparse/continuation UDF allocation unsupported", HC_ISO_UNSUPPORTED);
                if (Kind == 1) Require(Get(Ads, Offset + 8, 2) == 0, "Invalid UDF allocation partition");
                Require(Location <= Length && Size <= (Length - Location) * Block, "Invalid UDF allocation extent");
                Item.extents_.push_back({(Start + Location) * Block, Size}); Allocated += Size;
            }
            Require(Allocated >= Item.size_ && Allocated - Item.size_ < Block, "UDF allocation/file size mismatch");
        }
        if (!Item.directory_) { add(std::move(Item)); return; }
        Require(Item.size_ <= MaxMetadata - std::min(metadataBytes_, MaxMetadata), "UDF directory budget exceeded"); metadataBytes_ += Item.size_;
        Bytes Data = Item.embedded_;
        if (Data.empty()) for (const auto& Ext : Item.extents_) {
            const auto Part = ReadAt(Ext.offset_, std::min<uint64_t>(Ext.size_, Item.size_ - Data.size())); Data.insert(Data.end(), Part.begin(), Part.end()); if (Data.size() == Item.size_) break;
        }
        if (!Path.empty()) { Item.size_ = 0; add(Item); }
        for (size_t Offset = 0; Offset < Data.size();) {
            Require(Data.size() - Offset >= 38, "Truncated UDF FID");
            const auto Tail = std::span(Data).subspan(Offset);
            const size_t NameSize = Tail[19], ImplSize = size_t(Get(Tail, 36, 2));
            const size_t Size = size_t(Round(38 + ImplSize + NameSize, 4));
            Require(Size <= Tail.size(), "UDF FID outside directory");
            uint32_t TagLocation = Sector;
            if (Item.embedded_.empty()) {
                uint64_t Position = Offset;
                for (const auto& Ext : Item.extents_) { if (Position < Ext.size_) { TagLocation = uint32_t((Ext.offset_ + Position) / Block - Start); break; } Position -= Ext.size_; }
            }
            CheckTag(Tail.first(Size), TagLocation, 257);
            Require(Get(Tail, 10, 2) >= Size - 16, "UDF CRC excludes file identifier");
            Require(Get(Tail, 16, 2) == 1 && Get(Tail, 28, 2) == 0, "Invalid UDF FID version/partition");
            Offset += Size;
            if (Tail[18] & (4 | 8)) continue;
            Require(NameSize != 0, "Empty UDF file identifier");
            const auto Name = DecodeUnicode(Tail.subspan(38 + ImplSize, NameSize), true);
            Require(Name.find('/') == std::string::npos, "Invalid UDF component");
            Walk(uint32_t(Get(Tail, 24, 4)), Path.empty() ? Name : Path + '/' + Name, Depth + 1);
        }
    };
    Walk(uint32_t(Get(Fsd, 404, 4)), {}, 0);
}
}
