#pragma once
#include "pluginManifest.hpp"

namespace hc::plugins
{
    enum class WireKind : std::uint16_t { Hello = 1, Ping, Probe, Close, Response, Failure,
        ArchiveOpen, ArchiveNext, ArchiveRelease, ArchiveExtract, ArchiveUpdateBegin, ArchiveUpdateData, ArchiveUpdateApply,
        ViewerOpen, ViewerResize, ViewerFocus, ViewerCommand, ViewerClose,
        StorageEnumerate, StorageGet, StorageMutate, MetadataQuery, ContentConvert, ArchivePassword };
    constexpr std::size_t WireHeaderBytes = 24;
    constexpr std::uint32_t WirePayloadLimit = 1024 * 1024;
    struct WireHeader
    {
        ProtocolVersion version_;
        WireKind kind_ = WireKind::Hello;
        std::uint64_t sequence_ = 0;
        std::uint32_t length_ = 0;
    };
    storage::Result<std::vector<std::byte>> EncodeHeader(const WireHeader& Header);
    storage::Result<WireHeader> DecodeHeader(std::span<const std::byte> Bytes);
}
