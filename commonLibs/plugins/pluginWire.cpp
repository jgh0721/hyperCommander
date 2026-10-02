#include "pluginWire.hpp"

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"plugin-host"}}; }
        bool ValidKind(WireKind Kind)
        {
            return IsArchiveCommand(Kind) || IsViewerCommand(Kind) || IsDataCommand(Kind) || IsSessionCommand(Kind)
                || Kind == WireKind::Hello || Kind == WireKind::Ping || Kind == WireKind::Probe || Kind == WireKind::Close
                || Kind == WireKind::Response || Kind == WireKind::Failure || Kind == WireKind::ArchivePassword;
        }
    }
    bool IsArchiveCommand(WireKind Kind)
    {
        switch (Kind) { case WireKind::ArchiveOpen: case WireKind::ArchiveNext: case WireKind::ArchiveRelease:
        case WireKind::ArchiveExtract: case WireKind::ArchiveUpdateBegin: case WireKind::ArchiveUpdateData: case WireKind::ArchiveUpdateApply: return true; default: return false; }
    }
    bool IsViewerCommand(WireKind Kind)
    {
        switch (Kind) { case WireKind::ViewerOpen: case WireKind::ViewerResize: case WireKind::ViewerFocus:
        case WireKind::ViewerCommand: case WireKind::ViewerClose: return true; default: return false; }
    }
    bool IsDataCommand(WireKind Kind)
    {
        switch (Kind) { case WireKind::StorageEnumerate: case WireKind::StorageGet: case WireKind::StorageMutate:
        case WireKind::MetadataQuery: case WireKind::ContentConvert: return true; default: return false; }
    }
    bool IsSessionCommand(WireKind Kind)
    {
        switch (Kind) { case WireKind::SessionInvoke: case WireKind::SessionCancel: case WireKind::SessionProgress:
        case WireKind::InteractionRequest: case WireKind::InteractionReply: case WireKind::StreamOpen: case WireKind::StreamRead:
        case WireKind::StreamSeek: case WireKind::StreamClose: case WireKind::StreamData: case WireKind::ArchiveUpdateBinary: return true; default: return false; }
    }
    Result<std::vector<std::byte>> EncodeHeader(const WireHeader& Header)
    {
        if (Header.version_ != ProtocolVersion{1, 0} && Header.version_ != ProtocolVersion{2, 0}) return Invalid("host-wire-version");
        if (Header.version_.major_ == 1 && IsSessionCommand(Header.kind_)) return Invalid("host-wire-version");
        if (!ValidKind(Header.kind_) || !Header.sequence_) return Invalid("host-wire-envelope");
        if (Header.length_ > WirePayloadLimit) return Invalid("host-wire-budget");
        std::vector<std::byte> Bytes(WireHeaderBytes);
        const auto Put = [&](std::size_t At, std::uint64_t Value, unsigned Count) {
            for (unsigned Index = 0; Index < Count; ++Index) Bytes[At + Index] = std::byte((Value >> (8 * Index)) & 255);
        };
        Put(0, 0x31504348, 4); Put(4, Header.version_.major_, 2); Put(6, Header.version_.minor_, 2);
        Put(8, static_cast<std::uint16_t>(Header.kind_), 2); Put(12, Header.sequence_, 8); Put(20, Header.length_, 4);
        return Bytes;
    }
    Result<WireHeader> DecodeHeader(std::span<const std::byte> Bytes)
    {
        if (Bytes.size() != WireHeaderBytes) return Invalid("host-wire-header-size");
        const auto Get = [&](std::size_t At, unsigned Count) {
            std::uint64_t Value = 0; for (unsigned Index = 0; Index < Count; ++Index) Value |= std::to_integer<std::uint64_t>(Bytes[At + Index]) << (8 * Index); return Value;
        };
        if (Get(0, 4) != 0x31504348 || Get(10, 2)) return Invalid("host-wire-magic");
        WireHeader Header{{static_cast<std::uint32_t>(Get(4, 2)), static_cast<std::uint32_t>(Get(6, 2))},
            static_cast<WireKind>(Get(8, 2)), Get(12, 8), static_cast<std::uint32_t>(Get(20, 4))};
        auto Valid = EncodeHeader(Header); if (!Valid.HasValue()) return Valid.Failure(); return Header;
    }
}
