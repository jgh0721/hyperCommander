#include "stdafx.h"
#include "updateArchive.hpp"

#include "HyperCommanderBuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>

namespace hc::update {
namespace {
void Require(bool Value, const char* Error) { if (!Value) throw std::runtime_error(Error); }
Path Native(const QString& Value) { return Path(Value.toStdWString()); }
struct Central { std::string name_; std::uint64_t size_; std::uint32_t crc_; bool directory_; };
std::vector<Central> ZipDirectory(const QString& Name) {
    QFile File(Name); Require(File.open(QIODevice::ReadOnly) && File.size() >= 22 && File.size() <= 1073741824, "update-zip-open");
    const auto At = [&](qint64 Offset, qint64 Size) {
        Require(Offset >= 0 && Size >= 0 && Offset <= File.size() - Size && File.seek(Offset), "update-zip-range");
        auto Bytes = File.read(Size); Require(Bytes.size() == Size, "update-zip-read"); return Bytes;
    };
    const auto U16 = [](const QByteArray& Bytes, qsizetype Offset) { return qFromLittleEndian<quint16>(Bytes.constData() + Offset); };
    const auto U32 = [](const QByteArray& Bytes, qsizetype Offset) { return qFromLittleEndian<quint32>(Bytes.constData() + Offset); };
    const auto TailSize = std::min<qint64>(65557, File.size()), TailStart = File.size() - TailSize;
    const auto Tail = At(TailStart, TailSize); const auto End = Tail.lastIndexOf(QByteArray("PK\5\6", 4));
    Require(End >= 0 && End + 22 <= Tail.size(), "update-zip-end");
    const auto Footer = Tail.mid(End);
    Require(U16(Footer, 4) == 0 && U16(Footer, 6) == 0 && U16(Footer, 8) == U16(Footer, 10)
        && U16(Footer, 10) > 0 && U16(Footer, 10) <= 4096 && Footer.size() == 22 + U16(Footer, 20), "update-zip-volume");
    const auto Directory = qint64(U32(Footer, 16)), DirectoryBytes = qint64(U32(Footer, 12));
    Require(Directory + DirectoryBytes == TailStart + End && At(0, 4) == QByteArray("PK\3\4", 4), "update-zip-layout");
    qint64 Cursor = Directory; std::vector<Central> Result;
    std::vector<std::pair<qint64, qint64>> Ranges;
    for (unsigned Index = 0; Index < U16(Footer, 10); ++Index) {
        const auto Header = At(Cursor, 46); Require(Header.left(4) == QByteArray("PK\1\2", 4), "update-zip-directory");
        const auto Flags = U16(Header, 8), Method = U16(Header, 10), NameSize = U16(Header, 28);
        Require(!(Flags & 1) && (Method == 0 || Method == 8) && NameSize > 0 && NameSize <= 2048 && U16(Header, 34) == 0, "update-zip-method");
        const auto Raw = At(Cursor + 46, NameSize); const auto Text = Raw.toStdString();
        if (!(Flags & 0x800)) for (const auto Ch : Text) Require(static_cast<unsigned char>(Ch) < 128, "update-zip-name-encoding");
        const bool DirectoryEntry = Text.ends_with('/'); const auto Relative = DirectoryEntry ? Text.substr(0, Text.size() - 1) : Text;
        CheckRelative(Relative);
        const auto Packed = U32(Header, 20), Size = U32(Header, 24), Crc = U32(Header, 16), LocalOffset = U32(Header, 42);
        Require(Size <= 536870912 && (Packed != 0xffffffff) && (Size != 0xffffffff), "update-zip-size");
        const auto Local = At(LocalOffset, 30);
        Require(Local.left(4) == QByteArray("PK\3\4", 4) && U16(Local, 6) == Flags && U16(Local, 8) == Method
            && U16(Local, 26) == NameSize && At(qint64(LocalOffset) + 30, NameSize) == Raw, "update-zip-local-header");
        const auto Payload = qint64(LocalOffset) + 30 + NameSize + U16(Local, 28);
        Require(Payload + Packed <= Directory && (!DirectoryEntry || (Size == 0 && Packed == 0)), "update-zip-payload");
        if (!(Flags & 8)) Require(U32(Local, 14) == Crc && U32(Local, 18) == Packed && U32(Local, 22) == Size, "update-zip-local-size");
        Ranges.emplace_back(LocalOffset, Payload + Packed);
        Result.push_back({Text, Size, Crc, DirectoryEntry});
        Cursor += 46 + NameSize + U16(Header, 30) + U16(Header, 32); Require(Cursor <= Directory + DirectoryBytes, "update-zip-directory");
    }
    Require(Cursor == Directory + DirectoryBytes, "update-zip-directory");
    std::sort(Ranges.begin(), Ranges.end());
    for (std::size_t Index = 1; Index < Ranges.size(); ++Index) Require(Ranges[Index - 1].second <= Ranges[Index].first, "update-zip-overlap");
    return Result;
}
}
void StageArchive(const QString& Zip, const QString& Stage, const QString& Runtime, const Manifest& Release, std::stop_token Stop) {
    CheckPlainPath(Native(Zip)); CheckPlainPath(Native(Stage));
    const auto ArchiveLock = CreateFileW(Native(Zip).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    Require(ArchiveLock != INVALID_HANDLE_VALUE, "update-zip-lock");
    struct Close { HANDLE value_; ~Close() { CloseHandle(value_); } } CloseArchive{ArchiveLock};
    Require(std::filesystem::file_size(Native(Zip)) == Release.zipBytes_ && HashFile(Native(Zip), 1073741824, Stop) == Release.zipHash_, "update-zip-hash");
    Require(QDir(Stage).exists() && QDir(Stage).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).empty(), "update-staging-not-empty");
    const auto Central = ZipDirectory(Zip);
    std::uint64_t Total = 0;
    for (const auto& Entry : Central) {
        Require(Entry.size_ <= Release.unpackedBytes_ - Total, "update-archive-total-size"); Total += Entry.size_;
    }
    Require(Total == Release.unpackedBytes_, "update-archive-total-size");
    const auto Library = Native(QDir(Runtime).filePath("Third-Party/SevenZip/7z.dll"));
    VerifyProductSignature(Library);
    const auto Module = LoadLibraryExW(Library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Require(Module != nullptr, "update-archive-library");
    struct Free { HMODULE value_; ~Free() { FreeLibrary(value_); } } FreeModule{Module};
    auto Backend = hc::plugins::MakeSevenZipBackend(Module); hc::storage::OperationContext Context;
    Context.stopToken_ = Stop; Context.maxReadBytes_ = 65536; Context.maxEnumerationEntries_ = 4096;
    Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    auto Listing = Backend->Enumerate(Zip.toStdString(), false, Context); Require(Listing.HasValue(), "update-archive-listing");
    std::vector<hc::plugins::ArchiveHeader> Headers;
    for (;;) {
        auto Page = Listing.Value()->Next(128, Context); Require(Page.HasValue(), "update-archive-listing");
        for (auto& Header : Page.Value().entries_) { Require(Headers.size() < Central.size(), "update-archive-count"); Headers.push_back(std::move(Header)); }
        if (Page.Value().end_) break;
    }
    Listing.Value().reset(); Require(Headers.size() == Central.size(), "update-archive-count");
    std::optional<std::uint32_t> InventoryOrdinal; std::vector<std::uint32_t> Ordinals;
    std::map<std::string, std::uint32_t> Members;
    const auto Prefix = Release.root_ + '/';
    for (std::uint32_t Index = 0; Index < Headers.size(); ++Index) {
        auto& Header = Headers[Index]; const auto& Raw = Central[Index];
        Require(Header.name_ == Raw.name_ && Header.size_ == Raw.size_ && Header.crc_ == Raw.crc_ && Header.directory_ == Raw.directory_
            && !Header.encrypted_ && !(Header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT), "update-archive-entry");
        if (Header.directory_) { Require(Header.name_ == Prefix || Header.name_.starts_with(Prefix), "update-archive-root"); continue; }
        Require(Header.name_.starts_with(Prefix), "update-archive-root"); const auto Relative = Header.name_.substr(Prefix.size()); CheckRelative(Relative);
        Require(Members.emplace(Relative, Index).second, "update-archive-duplicate"); Ordinals.push_back(Index);
        if (Relative == "product-inventory.json") { Require(Header.size_ <= 1048576, "update-inventory-size"); InventoryOrdinal = Index; }
    }
    Require(InventoryOrdinal.has_value() && Members.size() == Release.fileCount_, "update-archive-inventory");
    auto InventoryStream = Backend->OpenReadSession(Zip.toStdString(), {*InventoryOrdinal}, Context); Require(InventoryStream.HasValue(), "update-archive-stream");
    std::string InventoryBytes;
    for (;;) {
        auto Chunk = InventoryStream.Value()->Read(*InventoryOrdinal, 65536, Context); Require(Chunk.HasValue(), "update-archive-stream");
        Require(InventoryBytes.size() + Chunk.Value().bytes_.size() <= 1048576, "update-inventory-size");
        InventoryBytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()); if (Chunk.Value().eof_) break;
    }
    InventoryStream.Value().reset(); Require(HashBytes(InventoryBytes) == Release.inventoryHash_, "update-inventory-hash");
    const auto Inventory = ParseInventory(InventoryBytes); Require(Inventory.version_ == Release.version_ && Inventory.files_.size() + 1 == Members.size(), "update-inventory-release");
    std::map<std::string, ProductFile> Expected;
    for (const auto& File : Inventory.files_) Expected.emplace(File.path_, File);
    Expected.emplace("product-inventory.json", ProductFile{"product-inventory.json", Release.inventoryHash_, InventoryBytes.size()});
    for (const auto& [Name, Index] : Members) { const auto It = Expected.find(Name); Require(It != Expected.end() && It->second.size_ == Headers[Index].size_, "update-archive-unlisted"); }
    auto Stream = Backend->OpenReadSession(Zip.toStdString(), Ordinals, Context); Require(Stream.HasValue(), "update-archive-stream");
    for (const auto Index : Ordinals) {
        const auto Name = Headers[Index].name_.substr(Prefix.size()); const auto Destination = Native(Stage) / Wide(Name);
        CheckPlainPath(Destination); std::filesystem::create_directories(Destination.parent_path()); CheckPlainPath(Destination);
        QSaveFile Output(QString::fromStdWString(Destination.wstring())); Output.setDirectWriteFallback(false); Require(Output.open(QIODevice::WriteOnly), "update-staging-open");
        std::uint64_t Count = 0;
        for (;;) {
            auto Chunk = Stream.Value()->Read(Index, 65536, Context); Require(Chunk.HasValue(), "update-archive-stream");
            const auto& Bytes = Chunk.Value().bytes_; Count += Bytes.size(); Require(Count <= Headers[Index].size_, "update-archive-budget");
            Require(Output.write(reinterpret_cast<const char*>(Bytes.data()), qint64(Bytes.size())) == qint64(Bytes.size()), "update-staging-write");
            if (Chunk.Value().eof_) break;
        }
        Require(Count == Headers[Index].size_ && Output.commit(), "update-staging-commit");
    }
    Stream.Value().reset(); VerifyTree(Native(Stage), Release, true, Stop);
}
}
