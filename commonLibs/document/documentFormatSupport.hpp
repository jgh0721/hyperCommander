#pragma once
#include <array>
#include <string_view>
namespace hc::document {
inline constexpr auto ImageDocumentExtensions = std::to_array<std::string_view>({
    "png", "jpg", "jpeg", "jpe", "jfif", "bmp", "gif", "tif", "tiff", "webp", "ico", "svg", "svgz", "cur", "icns", "tga", "wbmp",
    "pbm", "pgm", "ppm", "xbm", "xpm", "heic", "heif", "avif"
});
inline constexpr auto ImageBookExtensions = std::to_array<std::string_view>({"cbz", "cbr", "cb7", "zip", "rar", "7z"});
inline constexpr auto DocumentExtensions = std::to_array<std::string_view>({
    "txt", "text", "rst", "md", "markdown", "log", "json", "cpp", "hpp", "h", "c", "cc", "cxx", "hxx", "cmake",
    "xml", "py", "js", "ts", "sql", "ini", "cfg", "conf", "csv", "tsv", "yaml", "yml", "html", "htm", "css", "ps1",
    "go", "rs", "java", "cs", "qss", "qml", "ui", "bat", "cmd", "sh", "toml", "pdf", "hwp", "hwpx", "epub",
    "png", "jpg", "jpeg", "jpe", "jfif", "bmp", "gif", "tif", "tiff", "webp", "ico", "svg", "svgz", "cur", "icns", "tga", "wbmp",
    "pbm", "pgm", "ppm", "xbm", "xpm", "heic", "heif", "avif", "cbz", "cbr", "cb7", "zip", "rar", "7z"
});
}
