#pragma once

// Installed product layout, relative to the application directory (ROOT).
// This header is the single source: cmake/ProductLayout.cmake reads the same values.
// 경로는 '/' 구분, ROOT 기준 상대 경로이며 '..'를 쓰지 않는다.
namespace hc::layout
{
    inline constexpr auto PluginHost         = "hc-plugin-host-x64.exe";
    // License notices and source archives, one folder per component.
    inline constexpr auto Licenses           = "Licenses";
    // A plugin package folder is its manifest root and may hold no unpinned DLLs.
    inline constexpr auto IsoImagePackage    = "Addons/WCX/IsoImage";
    inline constexpr auto SevenZipPackage    = "Third-Party/SevenZip";
    inline constexpr auto AlzEggPackage      = "Third-Party/AlzEgg";
    // Holds HyperViewer.dll and its manifest; the manifest's package root is ROOT (helper HyperEditor.exe).
    inline constexpr auto HyperViewerPackage = "Addons/WLX/HyperViewer";
    // rhwp runtime root (ROOT itself: HyperEditorWeb shares the product Qt runtime), and its
    // inventory-pinned bundle relative to that root.
    inline constexpr auto RhwpRuntime        = ".";
    inline constexpr auto RhwpBundle         = "Libs/bundle";
}
