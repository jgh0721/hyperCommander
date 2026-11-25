// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// Port of PyCdlib 1.20.0 udf.py descriptor record/new methods. The upstream
// '2.60' API emits NSR02 and revision 0x0102; this writer reports that actual
// profile instead of claiming a metadata-partition UDF 2.60 implementation.
#include "image.hpp"

namespace hc::iso {
namespace {
constexpr uint32_t PartitionStart = 257;
constexpr uint64_t MaxShortExtent = 0x3ffff800;
constexpr size_t FileHeader = 176, AllocationHeader = 24;
void Entity(Bytes& Data, size_t Offset, std::string_view Name, bool Domain = false)
{
    Text(Data, Offset + 1, Name, 23);
    if (Domain) { Put(Data, Offset + 24, 0x0102, 2); Data[Offset + 26] = 3; }
}
void Charspec(Bytes& Data, size_t Offset) { Text(Data, Offset + 1, "OSTA Compressed Unicode", 63); }
void Dstring(Bytes& Data, size_t Offset, size_t Length, const std::string& Value)
{
    const auto Encoded = Unicode(Value, true); Require(Encoded.size() < Length, "UDF dstring too long");
    std::copy(Encoded.begin(), Encoded.end(), Data.begin() + Offset); Data[Offset + Length - 1] = uint8_t(Encoded.size());
}
void LongAd(Bytes& Data, size_t Offset, uint32_t Size, uint32_t Location)
{ Put(Data, Offset, Size, 4); Put(Data, Offset + 4, Location, 4); }
Bytes Fid(const Node& Item, bool Parent, uint32_t Location)
{
    const auto Name = Parent ? Bytes{} : Unicode(Item.name_, true);
    Bytes Data(size_t(Round(38 + Name.size(), 4))); Put(Data, 16, 1, 2);
    Data[18] = uint8_t((Item.directory_ ? 2 : 0) | (Parent ? 8 : 0) | ((Item.metadata_.attributes_ & 2) ? 1 : 0)); Data[19] = uint8_t(Name.size());
    LongAd(Data, 20, Block, Item.feBlock_); std::copy(Name.begin(), Name.end(), Data.begin() + 38);
    Tag(Data, 257, Location, Data.size()); return Data;
}
}
void Writer::udfLayout()
{
    for (const auto Index : directories_) {
        auto& Item = nodes_[Index]; Item.dataBlock_ = nextBlock_;
        Item.udfData_ = Fid(nodes_[Item.parent_], true, nextBlock_ - PartitionStart);
        for (const auto Child : Item.children_) {
            const auto Rec = Fid(nodes_[Child], false, nextBlock_ - PartitionStart + uint32_t(Item.udfData_.size() / Block));
            Item.udfData_.insert(Item.udfData_.end(), Rec.begin(), Rec.end());
        }
        allocateBlocks(Round(Item.udfData_.size()) / Block);
    }
    for (auto& Item : nodes_) {
        const uint64_t Size = Item.directory_ ? Item.udfData_.size() : Item.size_;
        const uint64_t Count = (Size + MaxShortExtent - 1) / MaxShortExtent;
        constexpr size_t Inline = (Block - FileHeader) / 8;
        constexpr size_t PerContinuation = (Block - AllocationHeader) / 8 - 1;
        if (Count > Inline)
            Item.allocationBlock_ = allocateBlocks((Count - Inline + PerContinuation - 1) / PerContinuation);
    }
}
void Writer::udfAnchor(std::ofstream& Stream, uint32_t Sector)
{
    Bytes Anchor(Block); LongAd(Anchor, 16, 16 * Block, 32); LongAd(Anchor, 24, 16 * Block, 48);
    Tag(Anchor, 2, Sector, 512); writeAt(Stream, Sector, Anchor);
}
void Writer::udfWrite(std::ofstream& Stream)
{
    for (uint32_t I = 0; I < 3; ++I) {
        // VRS follows the ISO terminator immediately; a zero-filled gap makes
        // Windows and PyCdlib stop before recognizing the UDF namespace.
        Bytes Data(Block); Text(Data, 1, I == 0 ? "BEA01" : I == 1 ? "NSR02" : "TEA01", 5); Data[6] = 1; writeAt(Stream, (hasJoliet() ? 19 : 18) + I, Data);
    }
    for (const uint32_t Base : {32u, 48u}) {
        Bytes Pvd(Block); Put(Pvd, 16, 0, 4); Dstring(Pvd, 24, 32, label_);
        Put(Pvd, 56, 1, 2); Put(Pvd, 58, 1, 2); Put(Pvd, 60, 2, 2); Put(Pvd, 62, 3, 2);
        Put(Pvd, 64, 1, 4); Put(Pvd, 68, 1, 4); Dstring(Pvd, 72, 128, "0000000000000000" + label_);
        Charspec(Pvd, 200); Charspec(Pvd, 264); Timestamp(Pvd, 376, options_.timestampUtc_, true, explicitTimestamp_); Entity(Pvd, 388, "*HyperCommander");
        Tag(Pvd, 1, Base, 512); writeAt(Stream, Base, Pvd);
        Bytes Iuv(Block); Put(Iuv, 16, 1, 4); Entity(Iuv, 20, "*UDF LV Info"); Put(Iuv, 44, 0x0102, 2);
        Charspec(Iuv, 52); Dstring(Iuv, 116, 128, label_); Entity(Iuv, 352, "*HyperCommander");
        Tag(Iuv, 4, Base + 1, 512); writeAt(Stream, Base + 1, Iuv);
        Bytes Pd(Block); Put(Pd, 16, 2, 4); Put(Pd, 20, 1, 2); Entity(Pd, 24, "+NSR02"); Pd[24] = 2;
        Put(Pd, 184, 1, 4); Put(Pd, 188, PartitionStart, 4); Put(Pd, 192, partitionEnd_ - PartitionStart, 4); Entity(Pd, 196, "*HyperCommander");
        Tag(Pd, 5, Base + 2, 512); writeAt(Stream, Base + 2, Pd);
        Bytes Lvd(Block); Put(Lvd, 16, 3, 4); Charspec(Lvd, 20); Dstring(Lvd, 84, 128, label_); Put(Lvd, 212, Block, 4); Entity(Lvd, 216, "*OSTA UDF Compliant", true);
        LongAd(Lvd, 248, 2 * Block, 0); Put(Lvd, 264, 6, 4); Put(Lvd, 268, 1, 4); Entity(Lvd, 272, "*HyperCommander");
        LongAd(Lvd, 432, 2 * Block, 64); Lvd[440] = 1; Lvd[441] = 6; Put(Lvd, 442, 1, 2);
        Tag(Lvd, 6, Base + 3, 446); writeAt(Stream, Base + 3, Lvd);
        Bytes Usd(Block); Put(Usd, 16, 4, 4); Tag(Usd, 7, Base + 4, 24); writeAt(Stream, Base + 4, Usd);
        Bytes End(Block); Tag(End, 8, Base + 5, 512); writeAt(Stream, Base + 5, End);
    }
    udfAnchor(Stream, 256);
    Bytes Integrity(Block); Timestamp(Integrity, 16, options_.timestampUtc_, true, explicitTimestamp_); Put(Integrity, 28, 1, 4);
    Put(Integrity, 40, nodes_.size() + 16, 8); Put(Integrity, 72, 1, 4); Put(Integrity, 76, 46, 4);
    Put(Integrity, 84, partitionEnd_ - PartitionStart, 4); Entity(Integrity, 88, "*HyperCommander");
    Put(Integrity, 120, nodes_.size() - directories_.size(), 4); Put(Integrity, 124, directories_.size(), 4);
    for (const size_t Offset : {128, 130, 132}) Put(Integrity, Offset, 0x0102, 2);
    Tag(Integrity, 9, 64, 134); writeAt(Stream, 64, Integrity);
    Bytes End(Block); Tag(End, 8, 65, 512); writeAt(Stream, 65, End);
    Bytes Fsd(Block); Timestamp(Fsd, 16, options_.timestampUtc_, true, explicitTimestamp_); Put(Fsd, 28, 3, 2); Put(Fsd, 30, 3, 2); Put(Fsd, 32, 1, 4); Put(Fsd, 36, 1, 4);
    Charspec(Fsd, 48); Dstring(Fsd, 112, 128, label_); Charspec(Fsd, 240); Dstring(Fsd, 304, 32, label_);
    LongAd(Fsd, 400, Block, nodes_[0].feBlock_); Entity(Fsd, 416, "*OSTA UDF Compliant", true);
    Tag(Fsd, 256, 0, 512); writeAt(Stream, PartitionStart, Fsd);
    End.assign(Block, 0); Tag(End, 8, 1, 512); writeAt(Stream, PartitionStart + 1, End);
    for (size_t I = 0; I < nodes_.size(); ++I) {
        const auto& Item = nodes_[I]; const uint64_t Size = Item.directory_ ? Item.udfData_.size() : Item.size_;
        Bytes Fe(Block); Put(Fe, 20, 4, 2); Put(Fe, 24, 1, 2); Fe[27] = Item.directory_ ? 4 : 5;
        Put(Fe, 36, UINT32_MAX, 4); Put(Fe, 40, UINT32_MAX, 4); const auto& Meta = Item.metadata_;
        const uint32_t WritePermissions = (Meta.attributes_ & 1) ? 0 : 0x842;
        Put(Fe, 44, 0x14a5 | WritePermissions, 4);
        Put(Fe, 34, (Meta.attributes_ & 32) | ((Meta.attributes_ & 4) ? 0x400 : 0), 2);
        uint32_t Links = 1; if (Item.directory_) for (const auto Child : Item.children_) if (nodes_[Child].directory_) ++Links;
        Put(Fe, 48, Links, 2); Put(Fe, 56, Size, 8); Put(Fe, 64, Round(Size) / Block, 8);
        for (const size_t Offset : {72, 84, 96}) Timestamp(Fe, Offset, options_.timestampUtc_, true, explicitTimestamp_);
        if (Meta.hasModified_) {
            const auto Millis = std::chrono::milliseconds(Meta.modifiedUtcMs_);
            const auto Seconds = std::chrono::floor<std::chrono::seconds>(Millis);
            Timestamp(Fe, 84, Seconds.count(), true, true);
            const auto Fraction = (Millis - Seconds).count();
            Fe[93] = uint8_t(Fraction / 10); Fe[94] = uint8_t((Fraction % 10) * 10);
        }
        Put(Fe, 108, 1, 4); Entity(Fe, 128, "*HyperCommander"); Put(Fe, 160, I == 0 ? 0 : I + 16, 8);
        size_t Header = FileHeader, AdOffset = Header;
        uint32_t Location = Item.feBlock_, Next = Item.allocationBlock_;
        uint16_t Type = 261;
        auto Flush = [&] {
            Put(Fe, Header - 4, AdOffset - Header, 4);
            Tag(Fe, Type, Location, AdOffset);
            writeAt(Stream, PartitionStart + Location, Fe);
        };
        // A short_ad has 30 size bits. Split large files at a block boundary.
        for (uint64_t Offset = 0; Offset < Size;) {
            const auto Chunk = std::min(Size - Offset, MaxShortExtent);
            if (AdOffset + 8 == Block && Chunk < Size - Offset) {
                Put(Fe, AdOffset, 0xc0000000u | Block, 4);
                Put(Fe, AdOffset + 4, Next - PartitionStart, 4);
                AdOffset += 8;
                Flush();
                Fe.assign(Block, 0);
                Location = Next++ - PartitionStart; Type = 258;
                Header = AllocationHeader; AdOffset = Header;
            }
            Put(Fe, AdOffset, Chunk, 4); Put(Fe, AdOffset + 4, Item.dataBlock_ - PartitionStart + Offset / Block, 4); AdOffset += 8; Offset += Chunk;
        }
        Flush();
        if (Item.directory_) writeAt(Stream, Item.dataBlock_, Item.udfData_);
    }
}
}
