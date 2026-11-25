// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// Ported from PyCdlib headervd.py, dr.py and udf.py. Unsupported allocation
// schemes fail explicitly; no namespace fallback after a corrupt selected tree.
#include "image.hpp"
#include <cstring>
#include <functional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hc::iso {
namespace {
uint32_t Paired(std::span<const uint8_t> Data, size_t Offset, size_t Width)
{
    const auto Value = Get(Data, Offset, Width);
    Require(Value == Get(Data, Offset + Width, Width, true), "ISO endian copies disagree");
    return uint32_t(Value);
}
uint32_t DataBlock(std::span<const uint8_t> Record)
{
    const auto Base = Paired(Record, 2, 4);
    Require(Base <= UINT32_MAX - Record[1], "ISO extended attribute extent overflow");
    return Base + Record[1];
}
bool ExtendedModificationTime(std::span<const uint8_t> Field, int64_t& Millis)
{
    // ECMA-119 XAR long timestamp: decimal components, centiseconds and zone.
    unsigned Values[7]{};
    size_t Pos = 0;
    for (unsigned I = 0; I < 7; ++I) {
        for (unsigned J = 0; J < (I == 0 ? 4u : 2u); ++J) {
            const auto Digit = Field[Pos++];
            if (Digit < '0' || Digit > '9') return false;
            Values[I] = Values[I] * 10 + Digit - '0';
        }
    }
    if (Values[0] < 1900 || Values[0] > 2155) return false;
    const std::array<uint8_t, 7> Short{uint8_t(Values[0] - 1900), uint8_t(Values[1]), uint8_t(Values[2]),
        uint8_t(Values[3]), uint8_t(Values[4]), uint8_t(Values[5]), Field[16]};
    if (!DecodeTimestamp(Short, false, Millis)) return false;
    Millis += Values[6] * 10; return true;
}
}
DiskReader::DiskReader(const fs::path& Path)
{
#ifdef _WIN32
    handle_ = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle_ != INVALID_HANDLE_VALUE, "Cannot open ISO input", HC_ISO_IO);
#else
    file_.open(Path, std::ios::binary);
    Require(bool(file_), "Cannot open ISO input", HC_ISO_IO);
#endif
}
DiskReader::~DiskReader()
{
#ifdef _WIN32
    CloseHandle(handle_);
#endif
}
void DiskReader::Read(uint64_t Offset, std::span<uint8_t> Data)
{
#ifdef _WIN32
    if (Offset != position_) {
        LARGE_INTEGER At{};
        At.QuadPart = static_cast<LONGLONG>(Offset);
        Require(SetFilePointerEx(handle_, At, nullptr, FILE_BEGIN), "ISO seek failed", HC_ISO_IO);
    }
    position_ = Offset;
    while (!Data.empty()) {
        DWORD Count = 0;
        Require(ReadFile(handle_, Data.data(), DWORD(std::min<size_t>(Data.size(), MAXDWORD)), &Count, nullptr) && Count, "Short ISO read", HC_ISO_IO);
        position_ += Count;
        Data = Data.subspan(Count);
    }
#else
    if (!file_ || Offset != position_) { file_.clear(); file_.seekg(std::streamoff(Offset)); }
    file_.read(reinterpret_cast<char*>(Data.data()), std::streamsize(Data.size()));
    position_ = Offset + uint64_t(file_.gcount());
    Require(size_t(file_.gcount()) == Data.size(), "Short ISO read", HC_ISO_IO);
#endif
}
Reader::Reader(const fs::path& Path, uint32_t Namespace) : stream_(std::make_unique<DiskReader>(Path))
{
    info_.imageBytes_ = fs::file_size(Path);
    initialize(Namespace);
}
Reader::Reader(const HcIsoInput& Input, uint32_t Namespace) : input_(Input)
{
    Require(Input.structSize_ == sizeof(Input) && Input.readAt_, "Invalid random-access source");
    info_.imageBytes_ = Input.size_;
    initialize(Namespace);
}
void Reader::initialize(uint32_t Namespace)
{
    info_.structSize_ = sizeof(info_);
    Require(info_.imageBytes_ >= 18 * Block && info_.imageBytes_ % Block == 0, "Truncated ISO image");
    Bytes Primary, Joliet;
    bool End = false, Udf = false, Recognition = false;
    // The descriptor search window is contiguous. One read avoids a channel
    // round trip per sector for callback-backed images on SMB and RDP.
    const auto EndSector = info_.imageBytes_ / Block;
    Bytes Window;
    for (uint64_t Sector = 16; Sector < EndSector; ++Sector) {
        const auto InWindow = (Sector - 16) % 64;
        if (!InWindow) Window = ReadAt(Sector * Block, std::min<uint64_t>(64, EndSector - Sector) * Block);
        const auto Data = std::span(Window).subspan(size_t(InWindow) * Block, Block);
        if (std::memcmp(Data.data() + 1, "CD001", 5) == 0 && !End) {
            Require(Data[6] == 1, "Unsupported ISO descriptor version", HC_ISO_UNSUPPORTED);
            if (Data[0] == 1) Primary.assign(Data.begin(), Data.end());
            if (Data[0] == 0) info_.features_ |= 1;
            if (Data[0] == 2 && Data[88] == '%' && Data[89] == '/' && (Data[90] == '@' || Data[90] == 'C' || Data[90] == 'E')) Joliet.assign(Data.begin(), Data.end());
            if (Data[0] == 255) End = true;
        }
        if (std::memcmp(Data.data() + 1, "BEA01", 5) == 0) Recognition = true;
        if (std::memcmp(Data.data() + 1, "NSR02", 5) == 0 || std::memcmp(Data.data() + 1, "NSR03", 5) == 0) Udf = true;
        if (std::memcmp(Data.data() + 1, "TEA01", 5) == 0) Recognition = false;
        if (End && Udf) break;
        if (InWindow == 63 && !Recognition && std::memcmp(Data.data() + 1, "CD001", 5) != 0) break;
    }
    Require(!Primary.empty() && End, "Missing ISO primary descriptor or terminator");
    Require(Paired(Primary, 128, 2) == Block && uint64_t(Paired(Primary, 80, 4)) * Block <= info_.imageBytes_, "Invalid ISO volume extent");
    const auto PrimaryRoot = ReadAt(uint64_t(DataBlock(std::span(Primary).subspan(156, 34))) * Block, Block);
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
    for (size_t I = 0; I < entries_.size(); ++I) {
        const auto Slash = entries_[I].path_.find_last_of('/');
        if (Slash == std::string::npos) rootChildren_.push_back(I);
        else {
            const auto Parent = paths_.find(entries_[I].path_.substr(0, Slash));
            Require(Parent != paths_.end() && entries_[Parent->second].directory_, "Missing parent directory");
            entries_[Parent->second].children_.push_back(I);
        }
    }
}
uint64_t Reader::Find(const std::string& Path) const
{ const auto It = paths_.find(Path); return It == paths_.end() ? UINT64_MAX : uint64_t(It->second); }
uint64_t Reader::Child(uint64_t Directory, uint64_t Index) const
{
    Require(Directory == UINT64_MAX || (Directory < entries_.size() && entries_[size_t(Directory)].directory_), "Invalid parent directory");
    const auto& Children = Directory == UINT64_MAX ? rootChildren_ : entries_[size_t(Directory)].children_;
    return Index < Children.size() ? uint64_t(Children[size_t(Index)]) : UINT64_MAX;
}
Bytes Reader::ReadAt(uint64_t Offset, uint64_t Size)
{
    Require(Offset <= info_.imageBytes_ && Size <= info_.imageBytes_ - Offset, "Extent outside image");
    Require(Size <= Bytes().max_size(), "Extent exceeds addressable memory");
    Bytes Data(static_cast<size_t>(Size));
    readInto(Offset, Data);
    return Data;
}
void Reader::readInto(uint64_t Offset, std::span<uint8_t> Data)
{
    const auto Size = Data.size();
    Require(Offset <= info_.imageBytes_ && Size <= info_.imageBytes_ - Offset, "Extent outside image");
    if (input_.readAt_) {
        uint64_t Done = 0;
        while (Done < Size) {
            const auto Capacity = uint32_t(std::min<uint64_t>(Size - Done, 1024 * 1024)); uint32_t Read = 0;
            const int Status = input_.readAt_(input_.context_, Offset + Done, Data.data() + size_t(Done), Capacity, &Read);
            Require(Status == HC_ISO_OK, "ISO source read failed", Status);
            Require(Read && Read <= Capacity, "Invalid source read progress", HC_ISO_IO); Done += Read;
        }
        return;
    }
    stream_->Read(Offset, Data);
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
        readInto(Ext.offset_ + Offset, Buffer.subspan(Done, size_t(Size)));
        Done += size_t(Size); Offset = 0;
        if (Done == Wanted) break;
    }
    Require(Done == Wanted, "Allocation shorter than file");
    return uint32_t(Done);
}
void Reader::add(Entry Value)
{
    CleanPath(Value.path_);
    Require(paths_.emplace(Value.path_, entries_.size()).second, "Duplicate path");
    entries_.push_back(std::move(Value));
}
void Reader::readIso(const Bytes& Descriptor, bool Joliet)
{
    Require(Paired(Descriptor, 128, 2) == Block, "Unsupported ISO block size", HC_ISO_UNSUPPORTED);
    std::set<uint32_t> Visited;
    struct DirectoryTask { uint32_t sector, length; std::string path; };
    std::vector<DirectoryTask> Queue;
    auto Walk = [&](uint32_t Sector, uint32_t Length, const std::string& Parent) {
        Require(Visited.insert(Sector).second, "Cyclic ISO directory");
        const auto Data = ReadAt(uint64_t(Sector) * Block, Length);
        Entry Pending; bool Multi = false;
        for (size_t Offset = 0; Offset < Data.size();) {
            const auto Size = Data[Offset];
            if (!Size) { Offset = size_t(Round(Offset + 1)); continue; }
            Require(Size >= 34 && Size <= Data.size() - Offset && Offset % Block + Size <= Block, "Invalid ISO directory record");
            const auto Rec = std::span(Data).subspan(Offset, Size); Offset += Size;
            Require(Rec[26] == 0 && Rec[27] == 0, "Interleaving unsupported", HC_ISO_UNSUPPORTED);
            const auto NameSize = Rec[32]; Require(NameSize && 33u + NameSize <= Rec.size(), "Invalid ISO identifier");
            if (NameSize == 1 && Rec[33] <= 1) continue;
            const auto NameData = Rec.subspan(33, NameSize);
            auto Name = Joliet ? DecodeUnicode(NameData, false) : std::string(reinterpret_cast<const char*>(NameData.data()), NameData.size());
            const bool Directory = (Rec[25] & 2) != 0;
            if (!Directory && !Joliet) { const auto Version = Name.rfind(';'); if (Version != std::string::npos) Name.resize(Version); if (!Name.empty() && Name.back() == '.') Name.pop_back(); }
            Require(Name.find('/') == std::string::npos, "Invalid ISO component");
            const auto Path = Parent.empty() ? Name : Parent + '/' + Name;
            const auto ExtentBlock = DataBlock(Rec), ExtentSize = Paired(Rec, 10, 4);
            Require(uint64_t(ExtentBlock) * Block <= info_.imageBytes_ && ExtentSize <= info_.imageBytes_ - uint64_t(ExtentBlock) * Block, "ISO file extent outside image");
            Require(Paired(Rec, 28, 2) == 1, "Multi-volume ISO unsupported", HC_ISO_UNSUPPORTED);
            if (Multi) Require(!Directory && Pending.path_ == Path, "Broken ISO multi-extent chain");
            else {
                Pending = {}; Pending.path_ = Path; Pending.directory_ = Directory;
                auto& Meta = Pending.metadata_;
                Meta.knownMask_ = 2; Meta.attributes_ = (Rec[25] & 1) ? 2 : 0;
                Meta.hasModified_ = DecodeTimestamp(Rec.subspan(18, 7), false, Meta.modifiedUtcMs_);
                if (Rec[1]) {
                    info_.features_ |= 2; // Rebuilding an external XAR could discard other fields.
                    const auto Xar = ReadAt(uint64_t(Paired(Rec, 2, 4)) * Block, 250);
                    int64_t Modified = 0;
                    if (Xar[180] == 1 && ExtendedModificationTime(std::span(Xar).subspan(27, 17), Modified)) {
                        Meta.modifiedUtcMs_ = Modified; Meta.hasModified_ = 1;
                    }
                }
                const auto Extra = Rec.subspan(std::min(Rec.size(), size_t(Round(33 + NameSize, 2))));
                if (Extra.size() == 20 && !std::memcmp(Extra.data(), "HC\x14\x01HCMD", 8)
                    && !(Extra[8] & ~0x27) && !(Extra[9] & ~Extra[8]) && Extra[10] <= 1) {
                    Meta.knownMask_ = Extra[8]; Meta.attributes_ = Extra[9]; Meta.hasModified_ = Extra[10];
                    if (Extra[10]) {
                        const auto Millis = int64_t(Get(Extra, 12, 8));
                        Require(Millis >= -11644473600000LL && Millis <= 253402300799999LL, "Invalid ISO modification timestamp");
                        Meta.modifiedUtcMs_ = Millis; Meta.hasModified_ = 1;
                    }
                }
            }
            Pending.size_ += ExtentSize; Pending.extents_.push_back({uint64_t(ExtentBlock) * Block, ExtentSize});
            Multi = (Rec[25] & 128) != 0;
            Require(!Directory || !Multi, "Multi-extent directory unsupported", HC_ISO_UNSUPPORTED);
            if (!Multi) { if (Directory) Pending.size_ = 0; add(std::move(Pending)); if (Directory) Queue.push_back({ExtentBlock, ExtentSize, Path}); }
        }
        Require(!Multi, "Unterminated ISO multi-extent chain");
    };
    Walk(DataBlock(std::span(Descriptor).subspan(156, 34)), Paired(Descriptor, 166, 4), {});
    while (!Queue.empty()) {
        auto Directory = std::move(Queue.back()); Queue.pop_back();
        Walk(Directory.sector, Directory.length, Directory.path);
    }
}
void Reader::readUdf()
{
    const auto Anchor = ReadAt(256ull * Block, Block); CheckTag(Anchor, 256, 2);
    const auto SequenceBytes = Get(Anchor, 16, 4), SequenceBlock = Get(Anchor, 20, 4);
    Require(SequenceBytes >= Block && SequenceBlock + (SequenceBytes - 1) / Block <= UINT32_MAX && SequenceBlock * Block + SequenceBytes <= info_.imageBytes_, "Invalid UDF descriptor sequence");
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
    struct IcbTask { uint32_t sector; std::string path; bool hidden; };
    std::vector<IcbTask> Queue;
    auto Walk = [&](uint32_t Sector, const std::string& Path, bool Hidden) {
        Require(Visited.insert(Sector).second, "Cyclic UDF tree or shared ICB unsupported");
        const auto Fe = ReadPartition(Sector, Block); CheckTag(Fe, Sector);
        const auto Id = Get(Fe, 0, 2); Require(Id == 261 || Id == 266, "Unsupported UDF ICB", HC_ISO_UNSUPPORTED);
        const bool Extended = Id == 266;
        Require(Get(Fe, 20, 2) == 4 && (Fe[27] == 4 || Fe[27] == 5), "UDF strategy/special file unsupported", HC_ISO_UNSUPPORTED);
        Entry Item; Item.path_ = Path; Item.directory_ = Fe[27] == 4; Item.size_ = Get(Fe, 56, 8);
        const auto Flags = Get(Fe, 34, 2);
        auto& Meta = Item.metadata_;
        Meta.knownMask_ = 0x27;
        Meta.attributes_ = uint32_t(Flags & 32) | (Hidden ? 2 : 0) | ((Flags & 0x400) ? 4 : 0)
            | ((Get(Fe, 44, 4) & 0x842) ? 0 : 1);
        Meta.hasModified_ = DecodeTimestamp(std::span(Fe).subspan(Extended ? 92 : 84, 12), true, Meta.modifiedUtcMs_);
        const size_t Header = Extended ? 216 : 176;
        const auto EaLength = Get(Fe, Header - 8, 4), AdLength = Get(Fe, Header - 4, 4);
        Require(EaLength <= Block - Header && AdLength <= Block - Header - EaLength, "Invalid UDF allocation length");
        Require(Get(Fe, 10, 2) >= Header - 16 + EaLength + AdLength, "UDF CRC excludes allocation descriptors");
        auto Ads = std::span(Fe).subspan(Header + size_t(EaLength), size_t(AdLength));
        const auto Kind = Get(Fe, 34, 2) & 7;
        if (Kind == 3) { Require(Item.size_ <= Ads.size(), "Embedded UDF file truncated"); Item.embedded_.assign(Ads.begin(), Ads.begin() + size_t(Item.size_)); }
        else {
            Require(Kind <= 1, "Unsupported UDF allocation type", HC_ISO_UNSUPPORTED);
            const size_t Width = Kind == 0 ? 8 : 16; Require(Ads.size() % Width == 0, "Truncated allocation descriptor");
            uint64_t Allocated = 0;
            Bytes Continuation;
            std::set<uint32_t> AllocationBlocks;
            for (size_t Offset = 0; Offset < Ads.size();) {
                const auto Size = Get(Ads, Offset, 4), Location = Get(Ads, Offset + 4, 4);
                if (Kind == 1) Require(Get(Ads, Offset + 8, 2) == 0, "Invalid UDF allocation partition");
                Offset += Width;
                if ((Size >> 30) == 3) {
                    Require(Offset == Ads.size() && AllocationBlocks.insert(uint32_t(Location)).second, "Invalid UDF allocation chain");
                    Continuation = ReadPartition(uint32_t(Location), Size & 0x3fffffff);
                    CheckTag(Continuation, uint32_t(Location), 258);
                    const auto LengthBytes = Get(Continuation, 20, 4);
                    Require(LengthBytes <= Continuation.size() - 24 && LengthBytes % Width == 0, "Invalid UDF allocation descriptors");
                    Ads = std::span<const uint8_t>(Continuation).subspan(24, size_t(LengthBytes));
                    Offset = 0;
                    continue;
                }
                Require((Size >> 30) == 0, "Sparse UDF allocation unsupported", HC_ISO_UNSUPPORTED);
                Require(Location <= Length && Size <= (Length - Location) * Block && Size <= UINT64_MAX - Allocated, "Invalid UDF allocation extent");
                Item.extents_.push_back({(Start + Location) * Block, Size}); Allocated += Size;
            }
            Require(Allocated >= Item.size_ && Allocated - Item.size_ < Block, "UDF allocation/file size mismatch");
        }
        if (!Item.directory_) { add(std::move(Item)); return; }
        Bytes Data = Item.embedded_;
        if (Data.empty()) for (const auto& Ext : Item.extents_) {
            const auto Part = ReadAt(Ext.offset_, std::min<uint64_t>(Ext.size_, Item.size_ - Data.size())); Data.insert(Data.end(), Part.begin(), Part.end()); if (Data.size() == Item.size_) break;
        }
        if (!Path.empty()) { Item.size_ = 0; add(Item); }
        const auto FirstChild = Queue.size();
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
            Queue.push_back({uint32_t(Get(Tail, 24, 4)), Path.empty() ? Name : Path + '/' + Name, (Tail[18] & 1) != 0});
        }
        std::reverse(Queue.begin() + FirstChild, Queue.end());
    };
    Walk(uint32_t(Get(Fsd, 404, 4)), {}, false);
    while (!Queue.empty()) {
        auto Item = std::move(Queue.back()); Queue.pop_back();
        Walk(Item.sector, Item.path, Item.hidden);
    }
}
}
