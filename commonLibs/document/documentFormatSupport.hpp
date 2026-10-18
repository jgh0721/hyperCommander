#pragma once
#include <array>
#include <string_view>
namespace hc::document {
inline constexpr auto DocumentExtensions = std::to_array<std::string_view>({
    "txt", "text", "rst", "md", "markdown", "log", "json", "cpp", "hpp", "h", "c", "cc", "cxx", "hxx", "cmake",
    "xml", "py", "js", "ts", "sql", "ini", "cfg", "conf", "csv", "tsv", "yaml", "yml", "html", "htm", "css", "ps1",
    "go", "rs", "java", "cs", "qss", "qml", "ui", "bat", "cmd", "sh", "toml", "pdf",
    "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "ico", "svg", ""
});
}
