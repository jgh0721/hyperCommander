#pragma once

namespace hc::plugins
{
    // Only close gestures cross this native UI boundary. Never transport
    // pointers or general command IDs through an unauthenticated window message.
    inline constexpr auto WlxCloseMessage = L"HyperCommander.Wlx.CloseGesture.1";
}
