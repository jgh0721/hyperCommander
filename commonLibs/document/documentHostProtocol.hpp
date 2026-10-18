#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace hc::document
{
    // Product control frames are deliberately separate from WLX commands.
    // Native HWNDs are identity tokens, never pointers to cross-process objects.
    constexpr std::uint32_t HostMagic = 0x30445648;
    constexpr std::uint32_t HostVersion = 2;
    constexpr std::uint32_t HostPayloadLimit = 768 * 1024;
    // Large Scintilla loads stay on the GUI owner and are slower in Debug.
    // Control replies get a bounded grace interval distinct from long saves.
    constexpr std::uint32_t HostExchangeTimeoutMs = 10000;
    constexpr std::uint32_t HostChunkLimit = 192 * 1024;
    constexpr std::uint64_t ViewerMemoryLimit = 1024ULL * 1024 * 1024;
    constexpr std::uint32_t TextByteLimit = 16 * 1024 * 1024;
    constexpr std::uint32_t DocumentByteLimit = 64 * 1024 * 1024;
    struct HostFrame
    {
        std::uint32_t magic_ = HostMagic, version_ = HostVersion;
        std::uint32_t type_ = 1, length_ = 0;
        std::uint64_t requestId_ = 0, window_ = 0;
        std::array<unsigned char, 16> connection_{};
    };
    static_assert(sizeof(HostFrame) == 48);
    inline bool ValidFrame(const HostFrame& Frame, const std::array<unsigned char, 16>& Connection)
    {
        return Frame.magic_ == HostMagic && Frame.version_ == HostVersion
            && Frame.length_ <= HostPayloadLimit && Frame.connection_ == Connection
            && Frame.type_ <= 2;
    }
}
