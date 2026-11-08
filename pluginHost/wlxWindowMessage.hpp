#pragma once

namespace hc::plugins
{
    // Only lifecycle/focus gestures cross this native UI boundary. Never transport
    // pointers or general command IDs through an unauthenticated window message.
    inline constexpr auto WlxCloseMessage = L"HyperCommander.Wlx.CloseGesture.1";
    inline constexpr auto WlxReturnMessage = L"HyperCommander.Wlx.ReturnFocus.1";
    inline constexpr auto WlxInteractionMessage = L"HyperCommander.Wlx.Interaction.1";
}
