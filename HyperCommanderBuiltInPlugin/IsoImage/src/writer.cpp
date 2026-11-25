// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// Ported from PyCdlib dr.py, headervd.py, path_table_record.py and layout/write
// paths in pycdlib.py. Source snapshots and exclusive staging are C++ additions.
#include "image.hpp"
#include <cstring>
#include <functional>
#include <numeric>

namespace hc::iso {
namespace {
constexpr uint64_t IsoExtentMax = 0xfffff800ull;
Bytes Record(const Node& Item, const Bytes& Name, bool Joliet, uint64_t Offset = 0, bool More = false)
{
    const auto Size = Item.directory_ ? (Joliet ? Item.jolietData_.size() : Item.isoData_.size()) : std::min(IsoExtentMax, Item.size_ - Offset);
    const auto Sector = Item.directory_ ? (Joliet ? Item.jolietBlock_ : Item.isoBlock_) : Item.dataBlock_ + uint32_t(Offset / Block);
    Bytes Data(size_t(Round(33 + Name.size(), 2)));
    Require(Data.size() <= 255, "ISO directory identifier too long");
    Data[0] = uint8_t(Data.size()); Both(Data, 2, Sector, 4); Both(Data, 10, uint32_t(Size), 4);
    Timestamp(Data, 18, 0, false); Data[25] = uint8_t((Item.directory_ ? 2 : 0) | (More ? 128 : 0)); Both(Data, 28, 1, 2);
    Data[32] = uint8_t(Name.size()); std::copy(Name.begin(), Name.end(), Data.begin() + 33);
    const auto& Meta = Item.metadata_;
    if (Meta.hasModified_) {
        const auto Seconds = std::chrono::floor<std::chrono::seconds>(std::chrono::milliseconds(Meta.modifiedUtcMs_));
        // ISO short dates encode years 1900..2155. Keep the exact time in
        // System Use when the standard recording date must remain unspecified.
        if (Seconds.count() >= -2208988800LL && Seconds < std::chrono::sys_days(std::chrono::year(2156)/1/1).time_since_epoch())
            Timestamp(Data, 18, Seconds.count(), false, true);
        else std::fill_n(Data.begin() + 18, 7, uint8_t{});
    }
    if (Meta.attributes_ & 2) Data[25] |= 1;
    // Named records have room for this application-specific System Use field,
    // including a maximum-length Joliet name. Leave dot records interoperable.
    if ((Name.size() != 1 || Name[0] > 1) && (Meta.knownMask_ || Meta.hasModified_)) {
        const auto At = Data.size();
        Data.resize(At + 20);
        Text(Data, At, "HC", 2); Data[At + 2] = 20; Data[At + 3] = 1;
        Text(Data, At + 4, "HCMD", 4);
        Data[At + 8] = uint8_t(Meta.knownMask_);
        Data[At + 9] = uint8_t(Meta.attributes_);
        Data[At + 10] = uint8_t(Meta.hasModified_);
        Put(Data, At + 12, uint64_t(Meta.modifiedUtcMs_), 8);
        Data[0] = uint8_t(Data.size());
    }
    return Data;
}
std::string PrimaryName(const std::string& Name, bool Directory, size_t Suffix)
{
    std::string Result;
    for (const auto C : Name) { if (C >= 'a' && C <= 'z') Result += char(C - 'a' + 'A'); else if ((C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '.') Result += C; else Result += '_'; }
    if (Directory) std::replace(Result.begin(), Result.end(), '.', '_');
    else {
        const auto LastDot = Result.rfind('.');
        for (size_t I = 0; I < Result.size(); ++I) if (Result[I] == '.' && I != LastDot) Result[I] = '_';
    }
    const auto SuffixText = Suffix ? "_" + std::to_string(Suffix) : "";
    const size_t Limit = Directory ? 31 : 29;
    if (Result.size() + SuffixText.size() > Limit) Result.resize(Limit - SuffixText.size());
    Result += SuffixText;
    if (!Directory) { if (Result.find('.') == std::string::npos) Result += '.'; Result += ";1"; }
    return Result;
}
}
Writer::Writer(const HcIsoSource* Sources, uint64_t Count, const HcIsoOptions& Options, uint32_t Flags, const HcIsoMetadata* Metadata) : options_(Options),
    shareContents_((Flags & HC_ISO_SHARE_DUPLICATE_CONTENTS) != 0), explicitTimestamp_((Flags & HC_ISO_EXPLICIT_TIMESTAMP) != 0),
    verifyContents_((Flags & HC_ISO_VERIFY_STRUCTURE_ONLY) == 0)
{
    Require(Options.structSize_ == sizeof(HcIsoOptions) && Options.profile_ <= HC_ISO_UDF_NO_JOLIET, "Invalid ISO build options");
    label_ = Options.volumeLabel_ ? Options.volumeLabel_ : "HYPERCOMMANDER";
    Require(!label_.empty() && label_.size() <= 32 && std::all_of(label_.begin(), label_.end(), [](unsigned char C) { return C >= 32 && C < 127; }), "Volume label must contain 1..32 ASCII characters");
    options_.maxImageBytes_ = options_.maxImageBytes_ ? std::min(options_.maxImageBytes_, MaxImageBytes) : MaxImageBytes;
    if (hasJoliet()) Require(label_.size() <= 16, "Joliet volume label exceeds 16 characters");
    if (hasUdf()) Require(label_.size() <= 30, "UDF volume label exceeds 30 characters");
    Bytes Check(12); Timestamp(Check, 0, options_.timestampUtc_, true, explicitTimestamp_);
    inventory(Sources, Count, Metadata); connectPayloads(); layout();
}
void Writer::inventory(const HcIsoSource* Sources, uint64_t Count, const HcIsoMetadata* Metadata)
{
    Require(Count <= std::vector<size_t>().max_size() && (Sources || Count == 0), "Invalid source inventory");
    nodes_.push_back({}); nodes_[0].directory_ = true;
    std::map<std::string, size_t> Paths{{"", 0}};
    std::set<std::string> Explicit;
    auto Directory = [&](const std::string& Path) -> size_t {
        std::vector<size_t> Missing;
        size_t End = Path.size();
        auto Found = Paths.find(Path);
        while (Found == Paths.end()) {
            Missing.push_back(End);
            const auto Slash = Path.rfind('/', End - 1);
            End = Slash == std::string::npos ? 0 : Slash;
            Found = Paths.find(Path.substr(0, End));
        }
        size_t Parent = Found->second;
        Require(nodes_[Parent].directory_, "File/directory collision");
        while (!Missing.empty()) {
            const auto Length = Missing.back(); Missing.pop_back();
            const auto Begin = End ? End + 1 : 0;
            Node Item; Item.path_ = Path.substr(0, Length); Item.name_ = Path.substr(Begin, Length - Begin);
            Item.directory_ = true; Item.parent_ = Parent;
            const auto Index = nodes_.size();
            Paths.emplace(Item.path_, Index); nodes_[Parent].children_.push_back(Index);
            nodes_.push_back(std::move(Item)); Parent = Index; End = Length;
        }
        return Parent;
    };
    std::vector<size_t> Order(static_cast<size_t>(Count)); std::iota(Order.begin(), Order.end(), size_t(0));
    for (size_t I = 0; I < Count; ++I) Require(Sources[I].imagePath_ != nullptr, "Missing image path");
    std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return std::strcmp(Sources[A].imagePath_, Sources[B].imagePath_) < 0; });
    for (const auto I : Order) {
        Pulse(options_, I, Count);
        const auto& Source = Sources[I]; const auto Path = CleanPath(Source.imagePath_);
        Require(Explicit.insert(Path).second, "Duplicate source path");
        size_t Index;
        if (Source.directory_) Index = Directory(Path);
        else {
            Require(!Paths.contains(Path), "File/directory collision");
            const auto Slash = Path.rfind('/'); const auto Parent = Directory(Slash == std::string::npos ? "" : Path.substr(0, Slash));
            Node Item; Item.path_ = Path; Item.name_ = Path.substr(Slash == std::string::npos ? 0 : Slash + 1); Item.parent_ = Parent;
            Item.source_ = DiskPath(Source.diskPath_);
            Require(fs::is_regular_file(fs::symlink_status(Item.source_)), "Source is not a regular file");
            Item.size_ = fs::file_size(Item.source_); Item.modified_ = fs::last_write_time(Item.source_);
            Require(Item.size_ <= options_.maxImageBytes_ && payloadBytes_ <= UINT64_MAX - Item.size_, "Input size overflow");
            if (!shareContents_) Require(payloadBytes_ <= options_.maxImageBytes_ - Item.size_, "Input exceeds image budget");
            payloadBytes_ += Item.size_;
            Index = nodes_.size(); nodes_.push_back(std::move(Item)); nodes_[Parent].children_.push_back(Index); Paths.emplace(Path, Index);
        }
        if (Metadata) {
            auto& Meta = nodes_[Index].metadata_;
            Meta = Metadata[I];
            Require(!(Meta.knownMask_ & ~0x27u) && !(Meta.attributes_ & ~Meta.knownMask_) && Meta.hasModified_ <= 1, "Invalid source metadata");
            Require(!Meta.hasModified_ || (Meta.modifiedUtcMs_ >= -11644473600000LL && Meta.modifiedUtcMs_ <= 253402300799999LL), "Invalid source modification time");
        }
    }
    // Breadth-first directory numbering is required by ISO path tables.
    directories_.push_back(0);
    for (size_t D = 0; D < directories_.size(); ++D) {
        auto& Parent = nodes_[directories_[D]];
        std::sort(Parent.children_.begin(), Parent.children_.end(), [&](size_t A, size_t B) { return nodes_[A].name_ < nodes_[B].name_; });
        std::set<std::string> Names;
        for (const auto Index : Parent.children_) {
            auto& Item = nodes_[Index]; size_t Suffix = 0;
            do { Item.isoName_ = PrimaryName(Item.name_, Item.directory_, Suffix++); } while (!Names.insert(Item.isoName_).second);
            if (hasJoliet()) {
                Require(DecodeUtf8(Item.name_).size() <= 64, "Joliet component exceeds 64 UTF-16 units", HC_ISO_UNSUPPORTED);
            }
            if (hasUdf()) Require(Unicode(Item.name_, true).size() <= 255, "UDF identifier exceeds 255 bytes", HC_ISO_UNSUPPORTED);
            if (Item.directory_) directories_.push_back(Index);
        }
    }
    Require(directories_.size() <= 65535, "ISO path table directory limit exceeded");
}
Bytes Writer::directory(size_t Index, bool Joliet)
{
    const auto& Parent = nodes_[Index]; Bytes Data;
    auto Append = [&](Bytes Rec, const Node& Item) {
        if (!Item.metadata_.hasModified_) Timestamp(Rec, 18, options_.timestampUtc_, false, explicitTimestamp_);
        if (Data.size() % Block + Rec.size() > Block) Data.resize(size_t(Round(Data.size())));
        Data.insert(Data.end(), Rec.begin(), Rec.end());
    };
    Append(Record(Parent, Bytes{0}, Joliet), Parent); Append(Record(nodes_[Parent.parent_], Bytes{1}, Joliet), nodes_[Parent.parent_]);
    auto Children = Parent.children_;
    auto NameOf = [&](size_t Child) { const auto& N = nodes_[Child]; return Joliet ? Unicode(N.name_, false) : Bytes(N.isoName_.begin(), N.isoName_.end()); };
    std::sort(Children.begin(), Children.end(), [&](size_t A, size_t B) { return NameOf(A) < NameOf(B); });
    for (const auto Child : Children) {
        const auto& Item = nodes_[Child]; const auto Name = NameOf(Child);
        uint64_t Offset = 0;
        do { const bool More = !Item.directory_ && Item.size_ - Offset > IsoExtentMax; Append(Record(Item, Name, Joliet, Offset, More), Item); if (!More) break; Offset += IsoExtentMax; } while (true);
    }
    Require(Round(Data.size()) <= UINT32_MAX, "ISO directory record length overflow");
    Data.resize(size_t(Round(Data.size()))); return Data;
}
Bytes Writer::pathTable(bool Joliet, bool Big)
{
    Bytes Data;
    std::map<size_t, uint32_t> Numbers; for (size_t I = 0; I < directories_.size(); ++I) Numbers[directories_[I]] = uint32_t(I + 1);
    for (const auto Index : directories_) {
        const auto& Item = nodes_[Index];
        const auto Name = Index == 0 ? Bytes{0} : Joliet ? Unicode(Item.name_, false) : Bytes(Item.isoName_.begin(), Item.isoName_.end());
        Bytes Rec(size_t(Round(8 + Name.size(), 2))); Rec[0] = uint8_t(Name.size());
        Put(Rec, 2, Joliet ? Item.jolietBlock_ : Item.isoBlock_, 4, Big); Put(Rec, 6, Numbers.at(Item.parent_), 2, Big);
        std::copy(Name.begin(), Name.end(), Rec.begin() + 8); Data.insert(Data.end(), Rec.begin(), Rec.end());
    }
    return Data;
}
void Writer::connectPayloads()
{
    std::map<uint64_t, size_t> Sizes;
    for (const auto& Item : nodes_) if (!Item.directory_) ++Sizes[Item.size_];
    std::map<std::pair<uint64_t, uint64_t>, std::vector<size_t>> Candidates;
    Bytes Buffer, Other;
    const auto Snapshot = [](const Node& Item) {
        Require(fs::is_regular_file(fs::symlink_status(Item.source_)) && fs::file_size(Item.source_) == Item.size_
            && fs::last_write_time(Item.source_) == Item.modified_, "Source changed during duplicate comparison");
    };
    uint64_t Compared = 0;
    for (size_t Index = 0; Index < nodes_.size(); ++Index) {
        auto& Item = nodes_[Index]; if (Item.directory_) continue;
        Pulse(options_, Compared, payloadBytes_); Snapshot(Item);
        uint64_t Fingerprint = 14695981039346656037ull;
        const bool Compare = shareContents_ && Sizes[Item.size_] > 1;
        if (Compare && Buffer.empty()) { Buffer.resize(1024 * 1024); Other.resize(Buffer.size()); }
        // A pair needs just one byte comparison. Fingerprinting both first adds two full reads.
        if (Compare && Sizes[Item.size_] > 2) {
            DiskReader Input(Item.source_);
            for (uint64_t Offset = 0; Offset < Item.size_;) {
                const auto Count = size_t(std::min<uint64_t>(Buffer.size(), Item.size_ - Offset));
                Input.Read(Offset, std::span(Buffer).first(Count));
                // A fingerprint only indexes candidates; sharing requires exact bytes.
                for (size_t I = 0; I < Count; ++I) { Fingerprint ^= Buffer[I]; Fingerprint *= 1099511628211ull; }
                Offset += Count; Compared += Count; Pulse(options_, Compared, payloadBytes_);
            }
            Snapshot(Item);
        }
        if (Compare) {
            for (const auto Candidate : Candidates[{Item.size_, Fingerprint}]) {
                const auto& Canonical = nodes_[payloads_[Candidate].sourceNode_]; Snapshot(Canonical);
                DiskReader Left(Item.source_), Right(Canonical.source_);
                bool Equal = true;
                for (uint64_t Offset = 0; Offset < Item.size_;) {
                    const auto Count = size_t(std::min<uint64_t>(Buffer.size(), Item.size_ - Offset));
                    Left.Read(Offset, std::span(Buffer).first(Count));
                    Right.Read(Offset, std::span(Other).first(Count));
                    Equal = std::equal(Buffer.begin(), Buffer.begin() + Count, Other.begin());
                    Offset += Count; Pulse(options_, Compared, payloadBytes_); if (!Equal) break;
                }
                Snapshot(Item); Snapshot(Canonical);
                if (Equal) { Item.payload_ = Candidate; break; }
            }
        }
        if (Item.payload_ == SIZE_MAX) {
            Item.payload_ = payloads_.size(); payloads_.push_back({Index});
            if (Compare) Candidates[{Item.size_, Fingerprint}].push_back(Item.payload_);
        }
    }
}
uint32_t Writer::allocateBlocks(uint64_t Count)
{
    Require(Count <= UINT32_MAX - nextBlock_, "ISO volume block count overflow");
    const auto Start = nextBlock_;
    nextBlock_ += uint32_t(Count);
    return Start;
}
void Writer::layout()
{
    Require(!hasUdf() || nodes_.size() <= UINT32_MAX - 259, "Too many UDF file entries");
    nextBlock_ = hasUdf() ? 259 + uint32_t(nodes_.size()) : 32;
    for (size_t I = 0; I < nodes_.size(); ++I) nodes_[I].feBlock_ = uint32_t(2 + I);
    for (int I = 0; I < (hasJoliet() ? 4 : 2); ++I) {
        tableBlocks_[I] = allocateBlocks(Round(pathTable(I >= 2, I % 2 != 0).size()) / Block);
    }
    for (const auto Index : directories_) {
        auto& Item = nodes_[Index]; Item.isoData_ = directory(Index, false); Item.isoBlock_ = allocateBlocks(Item.isoData_.size() / Block);
        if (hasJoliet()) { Item.jolietData_ = directory(Index, true); Item.jolietBlock_ = allocateBlocks(Item.jolietData_.size() / Block); }
    }
    if (hasUdf()) udfLayout();
    uint64_t Physical = 0;
    for (auto& Payload : payloads_) {
        const auto& Item = nodes_[Payload.sourceNode_]; const auto Bytes = Round(Item.size_);
        Require(Bytes <= options_.maxImageBytes_ && Physical <= options_.maxImageBytes_ - Bytes, "Unique payload exceeds image budget");
        Physical += Bytes; Payload.dataBlock_ = allocateBlocks(Bytes / Block);
    }
    for (auto& Item : nodes_) if (!Item.directory_) Item.dataBlock_ = payloads_[Item.payload_].dataBlock_;
    Require(uint64_t(nextBlock_) + 1 + (hasUdf() ? 257 : 0) <= UINT32_MAX, "ISO volume block count overflow");
    partitionEnd_ = uint32_t(nextBlock_ + 1); totalBlocks_ = partitionEnd_ + (hasUdf() ? 257 : 0);
    Require(uint64_t(totalBlocks_) * Block <= options_.maxImageBytes_, "Image exceeds output budget");
    for (const auto Index : directories_) {
        nodes_[Index].isoData_ = directory(Index, false);
        if (hasJoliet()) nodes_[Index].jolietData_ = directory(Index, true);
    }
}
Bytes Writer::volume(bool Joliet)
{
    Bytes Data(Block); Data[0] = Joliet ? 2 : 1; Text(Data, 1, "CD001", 5); Data[6] = 1;
    Text(Data, 8, "HYPERCOMMANDER", 32, ' ');
    if (Joliet) { const auto Label = Unicode(label_, false); Require(Label.size() <= 32, "Joliet volume label exceeds 16 characters"); std::copy(Label.begin(), Label.end(), Data.begin() + 40); Text(Data, 88, "%/E", 3); }
    else Text(Data, 40, label_, 32, ' ');
    Both(Data, 80, totalBlocks_, 4); Both(Data, 120, 1, 2); Both(Data, 124, 1, 2); Both(Data, 128, Block, 2);
    Both(Data, 132, uint32_t(pathTable(Joliet, false).size()), 4);
    Put(Data, 140, tableBlocks_[Joliet ? 2 : 0], 4); Put(Data, 148, tableBlocks_[Joliet ? 3 : 1], 4, true);
    auto Root = Record(nodes_[0], Bytes{0}, Joliet); Timestamp(Root, 18, options_.timestampUtc_, false, explicitTimestamp_);
    std::copy(Root.begin(), Root.end(), Data.begin() + 156);
    Text(Data, 190, "", 128, ' '); Text(Data, 318, "", 128, ' '); Text(Data, 446, "", 128, ' '); Text(Data, 574, "HYPERCOMMANDER ISO C++", 128, ' ');
    Text(Data, 702, "", 111, ' ');
    for (const size_t Offset : {813, 830, 847, 864}) Text(Data, Offset, "0000000000000000", 16);
    Data[881] = 1; return Data;
}
void Writer::writeAt(std::ofstream& Stream, uint32_t Sector, const Bytes& Data)
{
    Require(uint64_t(Sector) * Block + Data.size() <= uint64_t(totalBlocks_) * Block, "Writer extent overflow");
    Stream.seekp(std::streamoff(uint64_t(Sector) * Block)); Stream.write(reinterpret_cast<const char*>(Data.data()), std::streamsize(Data.size()));
    Require(bool(Stream), "ISO write failed", HC_ISO_IO);
}
void Writer::Write(const fs::path& Path)
{
    // C++23 noreplace maps to exclusive creation; cleanup owns only this file.
    std::ofstream Stream(Path, std::ios::binary | std::ios::out | std::ios::noreplace);
    Require(bool(Stream), "Cannot exclusively create ISO staging file", HC_ISO_IO);
    try {
        writeAt(Stream, 16, volume(false));
        const uint32_t Terminator = hasJoliet() ? 18 : 17;
        if (hasJoliet()) writeAt(Stream, 17, volume(true));
        Bytes End(Block); End[0] = 255; Text(End, 1, "CD001", 5); End[6] = 1; writeAt(Stream, Terminator, End);
        for (int I = 0; I < (hasJoliet() ? 4 : 2); ++I) writeAt(Stream, tableBlocks_[I], pathTable(I >= 2, I % 2 != 0));
        for (const auto Index : directories_) { writeAt(Stream, nodes_[Index].isoBlock_, nodes_[Index].isoData_); if (hasJoliet()) writeAt(Stream, nodes_[Index].jolietBlock_, nodes_[Index].jolietData_); }
        if (hasUdf()) udfWrite(Stream);
        Bytes Buffer(1024 * 1024); uint64_t Done = 0;
        for (const auto& Payload : payloads_) {
            const auto& Item = nodes_[Payload.sourceNode_];
            Pulse(options_, Done, payloadBytes_ * 2);
            Require(fs::file_size(Item.source_) == Item.size_ && fs::last_write_time(Item.source_) == Item.modified_, "Source changed before writing");
            DiskReader Input(Item.source_);
            Stream.seekp(std::streamoff(uint64_t(Item.dataBlock_) * Block));
            for (uint64_t Offset = 0; Offset < Item.size_;) {
                const auto Size = std::min<uint64_t>(Buffer.size(), Item.size_ - Offset);
                Input.Read(Offset, std::span(Buffer).first(size_t(Size)));
                Stream.write(reinterpret_cast<const char*>(Buffer.data()), std::streamsize(Size)); Require(bool(Stream), "ISO payload write failed", HC_ISO_IO);
                Offset += Size; Done += Size; Pulse(options_, Done, payloadBytes_ * 2);
            }
            Require(fs::file_size(Item.source_) == Item.size_ && fs::last_write_time(Item.source_) == Item.modified_, "Source changed while writing");
        }
        // Extend to the final size after payload writes so a remote filesystem
        // need not initialize the entire data area ahead of the first file.
        if (hasUdf()) {
            udfAnchor(Stream, partitionEnd_);
            udfAnchor(Stream, totalBlocks_ - 1);
        } else {
            Stream.seekp(std::streamoff(uint64_t(totalBlocks_) * Block - 1));
            Stream.put(0);
        }
        Stream.flush(); Require(bool(Stream), "ISO flush failed", HC_ISO_IO); Stream.close();
        // Compare every emitted namespace with the inventory. Primary aliases are
        // deterministic and deliberately checked independently of Unicode names.
        Bytes Expected(verifyContents_ ? Buffer.size() : 0);
        for (uint32_t Namespace = HC_ISO_PRIMARY; Namespace <= (hasUdf() ? HC_ISO_UDF : options_.profile_ == HC_ISO_9660_JOLIET ? HC_ISO_JOLIET : HC_ISO_PRIMARY); ++Namespace) {
            if (Namespace == HC_ISO_JOLIET && !hasJoliet()) continue;
            Reader Image(Path, Namespace); Require(Image.Entries().size() + 1 == nodes_.size(), "Reopened inventory count mismatch");
            std::vector<std::string> Names(nodes_.size());
            for (const auto Parent : directories_) for (const auto Child : nodes_[Parent].children_) {
                const auto& Item = nodes_[Child]; auto Name = Namespace == HC_ISO_PRIMARY ? Item.isoName_ : Item.name_;
                if (Namespace == HC_ISO_PRIMARY && !Item.directory_) { Name.resize(Name.size() - 2); if (Name.back() == '.') Name.pop_back(); }
                Names[Child] = Names[Parent].empty() ? Name : Names[Parent] + '/' + Name;
                const auto Found = Image.Find(Names[Child]); Require(Found != UINT64_MAX, "Reopened path mismatch");
                const auto& Actual = Image.Entries()[size_t(Found)]; Require(Actual.directory_ == Item.directory_ && Actual.size_ == Item.size_, "Reopened file metadata mismatch");
                const auto& ExpectedMetadata = Item.metadata_;
                Require(((Actual.metadata_.attributes_ ^ ExpectedMetadata.attributes_) & ExpectedMetadata.knownMask_) == 0
                    && (!ExpectedMetadata.hasModified_ || (Actual.metadata_.hasModified_ && Actual.metadata_.modifiedUtcMs_ == ExpectedMetadata.modifiedUtcMs_)),
                    "Reopened entry attributes or modification time differ");
                if (Item.directory_) continue;
                // Each namespace may split an extent differently. Validate the
                // exact physical byte sequence before sharing its payload check.
                uint64_t Covered = 0;
                for (const auto& Extent : Actual.extents_) {
                    Require(Extent.size_ <= Item.size_ - Covered, "Reopened extent exceeds file");
                    if (Extent.size_) Require(Extent.offset_ == uint64_t(Item.dataBlock_) * Block + Covered, "Reopened extent location mismatch");
                    Covered += Extent.size_;
                }
                Require(Actual.embedded_.empty() && Covered == Item.size_, "Reopened extent coverage mismatch");
                if (Namespace != HC_ISO_PRIMARY || !verifyContents_) {
                    Require(fs::file_size(Item.source_) == Item.size_ && fs::last_write_time(Item.source_) == Item.modified_, "Source changed during verification");
                    Pulse(options_, Done, payloadBytes_ * 2);
                    continue;
                }
                DiskReader Input(Item.source_);
                for (uint64_t Offset = 0; Offset < Item.size_;) {
                    const auto Size = uint32_t(std::min<uint64_t>(Buffer.size(), Item.size_ - Offset));
                    Require(Image.Read(Found, Offset, std::span(Buffer).first(Size)) == Size, "Reopened payload truncated");
                    Input.Read(Offset, std::span(Expected).first(Size));
                    Require(std::equal(Buffer.begin(), Buffer.begin() + Size, Expected.begin()), "Reopened payload differs from source");
                    Offset += Size; Done = std::min(payloadBytes_ * 2, Done + Size); Pulse(options_, Done, payloadBytes_ * 2);
                }
                Require(fs::file_size(Item.source_) == Item.size_ && fs::last_write_time(Item.source_) == Item.modified_, "Source changed during verification");
            }
        }
        Pulse(options_, payloadBytes_ * 2, payloadBytes_ * 2);
    } catch (...) { Stream.close(); std::error_code Ignored; fs::remove(Path, Ignored); throw; }
}
}
