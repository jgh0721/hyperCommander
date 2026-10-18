#include "stdafx.h"
#include "updateTrust.hpp"
#include "updateSigningPolicy.hpp"

#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <algorithm>
#include <charconv>
#include <cwctype>

namespace hc::update {
namespace {
[[noreturn]] void Fail(const char* Code) { throw std::runtime_error(Code); }
class FileLock {
public:
    explicit FileLock(const Path& File) {
        value_ = CreateFileW(File.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (value_ == INVALID_HANDLE_VALUE) Fail("update-file-open");
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(value_, &Info) || Info.nNumberOfLinks != 1 || (Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; Fail("update-file-type");
        }
    }
    ~FileLock() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    HANDLE value_ = INVALID_HANDLE_VALUE;
};
std::string Hex(const BYTE* Bytes, std::size_t Count) {
    constexpr char Digits[] = "0123456789abcdef"; std::string Text; Text.reserve(Count * 2);
    for (std::size_t Index = 0; Index < Count; ++Index) { Text += Digits[Bytes[Index] >> 4]; Text += Digits[Bytes[Index] & 15]; }
    return Text;
}
void Signature(const Path& File, const std::vector<std::string>& Pins) {
    CheckPlainPath(File); FileLock Locked(File);
    WINTRUST_FILE_INFO Info{sizeof(Info)}; Info.pcwszFilePath = File.c_str(); Info.hFile = Locked.value_;
    WINTRUST_DATA Data{sizeof(Data)}; Data.dwUIChoice = WTD_UI_NONE; Data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    Data.dwUnionChoice = WTD_CHOICE_FILE; Data.pFile = &Info; Data.dwStateAction = WTD_STATEACTION_VERIFY;
    Data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID Action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG Status = WinVerifyTrust(nullptr, &Action, &Data);
    bool Publisher = false, Timestamp = false;
    if (Status == ERROR_SUCCESS) {
        const auto Provider = WTHelperProvDataFromStateData(Data.hWVTStateData);
        const auto Signer = Provider ? WTHelperGetProvSignerFromChain(Provider, 0, FALSE, 0) : nullptr;
        if (Signer && Signer->csCertChain && Signer->pasCertChain[0].pCert) {
            std::array<BYTE, 32> Digest{}; DWORD Size = DWORD(Digest.size());
            if (CertGetCertificateContextProperty(Signer->pasCertChain[0].pCert, CERT_SHA256_HASH_PROP_ID, Digest.data(), &Size) && Size == Digest.size()) {
                const auto Pin = Hex(Digest.data(), Size); Publisher = std::find(Pins.begin(), Pins.end(), Pin) != Pins.end();
            }
            for (DWORD Index = 0; Index < Signer->csCounterSigners; ++Index) {
                const auto Counter = WTHelperGetProvSignerFromChain(Provider, 0, TRUE, Index);
                if (Counter && Counter->dwError == ERROR_SUCCESS && Counter->csCertChain) Timestamp = true;
            }
        }
    }
    Data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &Action, &Data);
    if (Status != ERROR_SUCCESS) Fail("update-signature-untrusted");
    if (!Publisher) Fail("update-signature-publisher");
    if (!Timestamp) Fail("update-signature-timestamp");
}
std::vector<std::string> ProductPins() { return {std::begin(policy::ProductPins), std::end(policy::ProductPins)}; }
}
std::wstring Wide(std::string_view Text) {
    if (Text.empty()) return {};
    const auto Count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), nullptr, 0);
    if (Count <= 0) Fail("update-utf8");
    std::wstring Result(Count, 0); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), int(Text.size()), Result.data(), Count); return Result;
}
std::string Utf8(std::wstring_view Text) {
    if (Text.empty()) return {};
    const auto Count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), int(Text.size()), nullptr, 0, nullptr, nullptr);
    if (Count <= 0) Fail("update-utf16");
    std::string Result(Count, 0); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), int(Text.size()), Result.data(), Count, nullptr, nullptr); return Result;
}
void CheckRelative(std::string_view Relative) {
    if (Relative.empty() || Relative.size() > 2048 || Relative.front() == '/' || Relative.find_first_of("\\:") != Relative.npos) Fail("update-path");
    for (const auto Ch : Relative) if (static_cast<unsigned char>(Ch) < 32 || Ch == 127) Fail("update-path");
    const auto Text = Wide(Relative); std::size_t Start = 0; unsigned Depth = 0;
    for (;;) {
        const auto End = Text.find(L'/', Start); auto Part = Text.substr(Start, End == Text.npos ? Text.npos : End - Start);
        if (Part.empty() || Part.size() > 255 || ++Depth > 32 || Part == L"." || Part == L".." || Part.back() == L'.' || Part.back() == L' '
            || Part.find_first_of(L"<>\"|?*") != Part.npos) Fail("update-path");
        Part.resize(Part.find(L'.') == Part.npos ? Part.size() : Part.find(L'.'));
        std::transform(Part.begin(), Part.end(), Part.begin(), [](wchar_t Ch) { return wchar_t(std::towupper(Ch)); });
        if (Part == L"CON" || Part == L"PRN" || Part == L"AUX" || Part == L"NUL" || Part == L"CONIN$" || Part == L"CONOUT$" || Part == L"CLOCK$") Fail("update-path-device");
        if (Part.size() == 4 && (Part.starts_with(L"COM") || Part.starts_with(L"LPT"))) {
            const auto Ch = Part[3]; if ((Ch >= L'1' && Ch <= L'9') || Ch == 0x00b9 || Ch == 0x00b2 || Ch == 0x00b3) Fail("update-path-device");
        }
        if (End == Text.npos) break; Start = End + 1;
    }
}
void CheckPlainPath(const Path& File) {
    auto Current = std::filesystem::absolute(File).lexically_normal();
    for (;;) {
        const auto Attributes = GetFileAttributesW(Current.c_str());
        if (Attributes != INVALID_FILE_ATTRIBUTES) {
            if (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) Fail("update-path-linked");
            if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const auto Handle = CreateFileW(Current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                BY_HANDLE_FILE_INFORMATION Info{}; const bool Plain = Handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(Handle, &Info)
                    && Info.nNumberOfLinks == 1 && !(Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
                if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle); if (!Plain) Fail("update-path-linked-or-inaccessible");
            }
        }
        else if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) Fail("update-path-access");
        const auto Parent = Current.parent_path(); if (Parent == Current || Parent.empty()) break; Current = Parent;
    }
}
std::string ReadBounded(const Path& File, std::uint64_t Limit) {
    CheckPlainPath(File); FileLock Locked(File); LARGE_INTEGER Size{};
    if (!GetFileSizeEx(Locked.value_, &Size) || Size.QuadPart < 0 || std::uint64_t(Size.QuadPart) > Limit) Fail("update-file-size");
    std::string Bytes(std::size_t(Size.QuadPart), 0); std::size_t Offset = 0;
    while (Offset < Bytes.size()) { DWORD Read = 0; if (!ReadFile(Locked.value_, Bytes.data() + Offset, DWORD(std::min<std::size_t>(65536, Bytes.size() - Offset)), &Read, nullptr) || !Read) Fail("update-file-read"); Offset += Read; }
    return Bytes;
}
std::string HashFile(const Path& File, std::uint64_t Limit, std::stop_token Stop) {
    CheckPlainPath(File); FileLock Locked(File); LARGE_INTEGER Size{};
    if (!GetFileSizeEx(Locked.value_, &Size) || Size.QuadPart < 0 || std::uint64_t(Size.QuadPart) > Limit) Fail("update-file-size");
    BCRYPT_ALG_HANDLE Algorithm = nullptr; BCRYPT_HASH_HANDLE Hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) Fail("update-hash-init");
    if (BCryptCreateHash(Algorithm, &Hash, nullptr, 0, nullptr, 0, 0) < 0) { BCryptCloseAlgorithmProvider(Algorithm, 0); Fail("update-hash-init"); }
    std::array<BYTE, 65536> Buffer{}; std::array<BYTE, 32> Digest{}; bool Ok = true;
    std::uint64_t Total = 0;
    for (;;) { DWORD Count = 0; if (Stop.stop_requested()) { Ok = false; break; } if (!ReadFile(Locked.value_, Buffer.data(), DWORD(Buffer.size()), &Count, nullptr)) { Ok = false; break; }
        if (!Count) break; Total += Count; if (Total > Limit || BCryptHashData(Hash, Buffer.data(), Count, 0) < 0) { Ok = false; break; } }
    if (Ok) Ok = BCryptFinishHash(Hash, Digest.data(), DWORD(Digest.size()), 0) >= 0;
    BCryptDestroyHash(Hash); BCryptCloseAlgorithmProvider(Algorithm, 0);
    if (!Ok) Fail(Stop.stop_requested() ? "update-cancelled" : "update-hash");
    return Hex(Digest.data(), Digest.size());
}
void VerifyProductSignature(const Path& File, std::string_view Relative) {
    auto Pins = ProductPins();
    if (Relative == "Everything64.dll") Pins.insert(Pins.end(), std::begin(policy::EverythingPins), std::end(policy::EverythingPins));
    Signature(File, Pins);
}
std::string HashBytes(std::string_view Bytes) {
    if (Bytes.size() > 1048576) Fail("update-hash-size");
    BCRYPT_ALG_HANDLE Algorithm = nullptr; std::array<BYTE, 32> Digest{};
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) Fail("update-hash-init");
    const auto Status = BCryptHash(Algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(Bytes.data())), ULONG(Bytes.size()), Digest.data(), ULONG(Digest.size()));
    BCryptCloseAlgorithmProvider(Algorithm, 0); if (Status < 0) Fail("update-hash");
    return Hex(Digest.data(), Digest.size());
}
std::string AuthenticateManifest(const Path& CatalogPath, const Path& ManifestPath) {
    CheckPlainPath(CatalogPath); CheckPlainPath(ManifestPath);
    if (std::filesystem::file_size(CatalogPath) > 1048576) Fail("update-catalog-size");
    CheckPlainPath(CatalogPath); FileLock CatalogLock(CatalogPath); FileLock ManifestLock(ManifestPath);
    Signature(CatalogPath, ProductPins()); const auto Expected = HashFile(ManifestPath, 262144);
    auto Name = CatalogPath.wstring(); const auto Catalog = CryptCATOpen(Name.data(), CRYPTCAT_OPEN_EXISTING, 0, CRYPTCAT_VERSION_2, 0);
    if (Catalog == INVALID_HANDLE_VALUE) Fail("update-catalog-open");
    unsigned Matches = 0, Count = 0; CRYPTCATMEMBER* Member = nullptr;
    while ((Member = CryptCATEnumerateMember(Catalog, Member))) {
        if (++Count > 16) break; const auto Indirect = Member->pIndirectData;
        if (Indirect && Indirect->DigestAlgorithm.pszObjId && std::string_view(Indirect->DigestAlgorithm.pszObjId) == szOID_NIST_sha256
            && Indirect->Digest.cbData == 32 && Hex(Indirect->Digest.pbData, 32) == Expected) ++Matches;
    }
    CryptCATClose(Catalog); if (Count > 16 || Matches != 1) Fail("update-catalog-member");
    return ReadBounded(ManifestPath, 262144);
}
std::array<unsigned, 4> ParseVersion(std::string_view Text) {
    std::array<unsigned, 4> Parts{}; std::size_t Start = 0;
    for (unsigned Index = 0; Index < 4; ++Index) {
        const auto End = Text.find('.', Start); const auto Part = Text.substr(Start, End == Text.npos ? Text.npos : End - Start);
        if (Part.empty() || Part.size() > 5) Fail("update-version");
        for (const auto Ch : Part) if (Ch < '0' || Ch > '9') Fail("update-version");
        const auto Result = std::from_chars(Part.data(), Part.data() + Part.size(), Parts[Index]);
        if (Result.ec != std::errc{} || Result.ptr != Part.data() + Part.size() || Parts[Index] > 65535 || ((End == Text.npos) != (Index == 3))) Fail("update-version");
        Start = End + 1;
    }
    return Parts;
}
void VerifyImageVersion(const Path& File, std::string_view Expected) {
    const auto Size = GetFileVersionInfoSizeW(File.c_str(), nullptr); if (!Size || Size > 1048576) Fail("update-image-version");
    std::vector<BYTE> Bytes(Size); if (!GetFileVersionInfoW(File.c_str(), 0, Size, Bytes.data())) Fail("update-image-version");
    VS_FIXEDFILEINFO* Info = nullptr; UINT Count = 0;
    if (!VerQueryValueW(Bytes.data(), L"\\", reinterpret_cast<void**>(&Info), &Count) || Count < sizeof(*Info) || Info->dwSignature != 0xfeef04bd) Fail("update-image-version");
    if (std::array<unsigned, 4>{HIWORD(Info->dwFileVersionMS), LOWORD(Info->dwFileVersionMS), HIWORD(Info->dwFileVersionLS), LOWORD(Info->dwFileVersionLS)} != ParseVersion(Expected)) Fail("update-image-version");
}
void VerifyProductFile(const Path& File, std::string_view Relative, std::string_view ExpectedHash, std::uint64_t ExpectedSize, std::stop_token Stop) {
    CheckRelative(Relative); CheckPlainPath(File); FileLock Locked(File); LARGE_INTEGER Size{};
    if (!GetFileSizeEx(Locked.value_, &Size) || Size.QuadPart < 0 || std::uint64_t(Size.QuadPart) != ExpectedSize) Fail("update-file-size");
    if (HashFile(File, 536870912, Stop) != ExpectedHash) Fail("update-file-hash");
    std::array<BYTE, 64> Header{}; DWORD Count = 0;
    if (!ReadFile(Locked.value_, Header.data(), DWORD(Header.size()), &Count, nullptr)) Fail("update-file-read");
    bool Pe = Count >= 2 && Header[0] == 'M' && Header[1] == 'Z';
    if (Pe) {
        if (Count != Header.size()) Fail("update-pe-header");
        DWORD Offset = 0; memcpy(&Offset, Header.data() + 60, sizeof(Offset));
        if (std::uint64_t(Offset) + 6 > ExpectedSize) Fail("update-pe-header");
        LARGE_INTEGER Position{}; Position.QuadPart = Offset;
        std::array<BYTE, 6> Magic{};
        if (!SetFilePointerEx(Locked.value_, Position, nullptr, FILE_BEGIN) || !ReadFile(Locked.value_, Magic.data(), DWORD(Magic.size()), &Count, nullptr)
            || Count != Magic.size() || memcmp(Magic.data(), "PE\0\0", 4) != 0 || Magic[4] != 0x64 || Magic[5] != 0x86) Fail("update-pe-architecture");
        VerifyProductSignature(File, Relative);
    } else {
        auto Extension = Path(Wide(Relative)).extension().wstring();
        std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Ch) { return wchar_t(std::towupper(Ch)); });
        if (Extension == L".EXE" || Extension == L".DLL" || Extension == L".WCX64" || Extension == L".WLX64" || Extension == L".WFX64" || Extension == L".WDX64") Fail("update-pe-header");
    }
}
}
