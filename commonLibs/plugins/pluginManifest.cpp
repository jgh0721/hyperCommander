#include "pluginManifest.hpp"
#include <limits>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"plugin"}}; }
        bool Token(std::string_view Value)
        {
            return !Value.empty() && Value.size() <= 128 && Value.front() != '.' && Value.back() != '.'
                && Value.find("..") == Value.npos && std::ranges::all_of(Value, [](unsigned char Ch) {
                    return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '.' || Ch == '-'; });
        }
        bool BinaryPath(std::string_view Path)
        {
            if (Path.empty() || Path.size() > 1024 || Path.front() == '/' || Path.back() == '/') return false;
            if (std::ranges::any_of(Path, [](unsigned char Ch) { return Ch < 32 || Ch == 127 || std::string_view("\\:<>\"|?*").find(Ch) != std::string_view::npos; })) return false;
            std::size_t Start = 0, Count = 0;
            while (Start < Path.size())
            {
                auto End = Path.find('/', Start); if (End == Path.npos) End = Path.size();
                auto Part = Path.substr(Start, End - Start);
                if (Part.empty() || Part == "." || Part == ".." || Part.back() == '.' || Part.back() == ' ' || ++Count > 16) return false;
                std::string Stem(Part.substr(0, Part.find('.')));
                for (auto& Ch : Stem) if (Ch >= 'a' && Ch <= 'z') Ch -= 'a' - 'A';
                if (Stem == "CON" || Stem == "PRN" || Stem == "AUX" || Stem == "NUL"
                    || (Stem.size() == 4 && (Stem.starts_with("COM") || Stem.starts_with("LPT")) && Stem[3] >= '0' && Stem[3] <= '9')) return false;
                Start = End + 1;
            }
            return true;
        }
        bool ValidRange(ProtocolRange Range) { return Range.major_ > 0 && Range.minimumMinor_ <= Range.maximumMinor_; }
    }
    Result<Unit> ArchiveWritePolicy(const PluginManifest& Manifest)
    {
        if (!Manifest.extensions_.contains(Extension::ArchiveWrite) || !Manifest.permissions_.contains(Permission::WriteContent))
            return Error{ErrorCategory::AccessDenied, "host-archive-write-not-approved", {"wcx"}};
        auto Name = Manifest.binary_.substr(Manifest.binary_.find_last_of('/') + 1);
        for (auto& Ch : Name) if (Ch >= 'A' && Ch <= 'Z') Ch += 'a' - 'A';
        if ((Manifest.compatibility_ == "total7zip" || Name == "total7zip.wcx64") && Manifest.helpers_.empty())
            return Error{ErrorCategory::AccessDenied, "plugin-helper-required", {"wcx"}};
        return Unit{};
    }
    Result<Unit> Validate(const PluginManifest& Manifest)
    {
        if (Manifest.schemaVersion_ != 1) return Invalid("plugin-manifest-version");
        if (!Token(Manifest.id_) || !Token(Manifest.version_)) return Invalid("plugin-manifest-identity");
        if (!BinaryPath(Manifest.binary_)) return Invalid("plugin-binary-path");
        if (Manifest.sha256_.size() != 64 || !std::ranges::all_of(Manifest.sha256_, [](char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); })) return Invalid("plugin-binary-hash");
        if (Manifest.dependencies_.size() > 64) return Invalid("plugin-dependency-budget");
        std::set<std::string> Paths;
        auto Fold = [](std::string Path) { for (auto& Ch : Path) if (Ch >= 'A' && Ch <= 'Z') Ch += 'a' - 'A'; return Path; };
        Paths.insert(Fold(Manifest.binary_));
        for (const auto& [Path, Hash] : Manifest.dependencies_)
        {
            if (!BinaryPath(Path) || !Paths.insert(Fold(Path)).second) return Invalid("plugin-dependency-path");
            if (Hash.size() != 64 || !std::ranges::all_of(Hash, [](char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); })) return Invalid("plugin-dependency-hash");
        }
        if (Manifest.helpers_.size() > 1) return Invalid("plugin-helper-budget");
        for (const auto& [Path, Hash] : Manifest.helpers_)
        {
            if (!BinaryPath(Path) || !Paths.insert(Fold(Path)).second) return Invalid("plugin-helper-path");
            if (Hash.size() != 64 || !std::ranges::all_of(Hash, [](char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); })) return Invalid("plugin-helper-hash");
        }
        if (!Manifest.compatibility_.empty() && Manifest.compatibility_ != "total7zip") return Invalid("plugin-compatibility");
        const bool Helpers = !Manifest.helpers_.empty();
        if (Helpers != Manifest.permissions_.contains(Permission::ChildProcess)) return Invalid("plugin-helper-permission");
        if (Helpers && (Manifest.compatibility_ != "total7zip" || Manifest.format_ != PluginFormat::Wcx
            || Manifest.execution_ != ExecutionMode::Isolated || !Manifest.extensions_.contains(Extension::ArchiveWrite)
            || !Manifest.permissions_.contains(Permission::UserInterface)
            || !Manifest.helpers_.contains("64/7zG.exe") || !Manifest.dependencies_.contains("64/7z.dll"))) return Invalid("plugin-helper-profile");
        if (Manifest.compatibility_ == "total7zip" && Manifest.extensions_.contains(Extension::ArchiveWrite) && !Helpers)
            return Invalid("plugin-helper-required");
        if (!ValidRange(Manifest.protocol_)) return Invalid("plugin-protocol-range");
        if (Manifest.architecture_ != Architecture::X64 && Manifest.architecture_ != Architecture::X86 && Manifest.architecture_ != Architecture::Arm64) return Invalid("plugin-architecture");
        if (Manifest.execution_ != ExecutionMode::Isolated && Manifest.execution_ != ExecutionMode::TrustedInProcess) return Invalid("plugin-execution-mode");
        std::set<Extension> Allowed;
        switch (Manifest.format_)
        {
        case PluginFormat::Wcx: Allowed = {Extension::ArchiveRead, Extension::ArchiveWrite}; break;
        case PluginFormat::SevenZip: Allowed = {Extension::ArchiveRead}; break;
        case PluginFormat::DopusViewer: Allowed = {Extension::Viewer, Extension::Metadata, Extension::ContentConvert}; break;
        case PluginFormat::Wlx: Allowed = {Extension::Viewer}; break;
        case PluginFormat::Wfx:
        case PluginFormat::DopusVfs: Allowed = {Extension::StorageRead, Extension::StorageWrite}; break;
        case PluginFormat::Wdx: Allowed = {Extension::Metadata}; break;
        default: return Invalid("plugin-format");
        }
        if (Manifest.extensions_.empty() || !std::includes(Allowed.begin(), Allowed.end(), Manifest.extensions_.begin(), Manifest.extensions_.end())) return Invalid("plugin-extensions");
        for (auto Value : Manifest.permissions_)
            if (Value != Permission::ReadContent && Value != Permission::WriteContent && Value != Permission::Network && Value != Permission::UserInterface && Value != Permission::ChildProcess) return Invalid("plugin-permission");
        if (!Manifest.permissions_.contains(Permission::ReadContent)) return Invalid("plugin-read-permission-required");
        if ((Manifest.extensions_.contains(Extension::ArchiveWrite) || Manifest.extensions_.contains(Extension::StorageWrite)) && !Manifest.permissions_.contains(Permission::WriteContent)) return Invalid("plugin-write-permission-required");
        if (Manifest.extensions_.contains(Extension::Viewer) && !Manifest.permissions_.contains(Permission::UserInterface)) return Invalid("plugin-ui-permission-required");
        return Unit{};
    }
    Result<ProtocolVersion> Negotiate(ProtocolRange Client, ProtocolRange Host)
    {
        if (!ValidRange(Client) || !ValidRange(Host)) return Invalid("plugin-protocol-range");
        if (Client.major_ != Host.major_ || std::max(Client.minimumMinor_, Host.minimumMinor_) > std::min(Client.maximumMinor_, Host.maximumMinor_))
            return Error{ErrorCategory::Unsupported, "plugin-protocol-incompatible", {"plugin"}};
        return ProtocolVersion{Client.major_, std::min(Client.maximumMinor_, Host.maximumMinor_)};
    }
    Result<PluginAdmission> Admit(const PluginManifest& Manifest, const PluginApproval& Approval, const HostProfile& Host)
    {
        auto Valid = Validate(Manifest); if (!Valid.HasValue()) return Valid.Failure();
        auto Version = Negotiate(Manifest.protocol_, Host.protocol_); if (!Version.HasValue()) return Version.Failure();
        if (!Approval.approved_ || Manifest != Approval.reviewed_)
            return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"plugin"}};
        if (!std::includes(Approval.granted_.begin(), Approval.granted_.end(), Manifest.permissions_.begin(), Manifest.permissions_.end()))
            return Error{ErrorCategory::AccessDenied, "plugin-permission-denied", {"plugin"}};
        if (Manifest.architecture_ != Host.architecture_ || !Host.formats_.contains(Manifest.format_)
            || !std::includes(Host.extensions_.begin(), Host.extensions_.end(), Manifest.extensions_.begin(), Manifest.extensions_.end()))
            return Error{ErrorCategory::Unsupported, "plugin-host-capability", {"plugin"}};
        if ((Manifest.execution_ == ExecutionMode::Isolated && !Host.supportsIsolation_)
            || (Manifest.execution_ == ExecutionMode::TrustedInProcess && (!Host.supportsInProcess_ || !Approval.allowInProcess_)))
            return Error{ErrorCategory::AccessDenied, "plugin-execution-not-approved", {"plugin"}};
        return PluginAdmission{Version.Value(), Manifest.execution_, Manifest.permissions_};
    }
    Result<Architecture> InspectPe(std::span<const std::byte> Header, bool Executable)
    {
        const auto U16 = [&](std::size_t At) { return std::to_integer<std::uint32_t>(Header[At]) | (std::to_integer<std::uint32_t>(Header[At + 1]) << 8); };
        const auto U32 = [&](std::size_t At) { return U16(At) | (U16(At + 2) << 16); };
        if (Header.size() < 64 || U16(0) != 0x5a4d) return Invalid("plugin-pe-dos-header");
        const std::uint64_t Pe = U32(0x3c);
        if (Pe < 64 || Pe + 24 > Header.size() || U32(Pe) != 0x4550) return Invalid("plugin-pe-signature");
        const auto Sections = U16(Pe + 6), Optional = U16(Pe + 20), Flags = U16(Pe + 22);
        if (!Sections || Sections > 96 || Optional < 96 || Pe + 24 + Optional + 40ULL * Sections > Header.size() || (Flags & 0x2002) != (Executable ? 0x0002u : 0x2002u)) return Invalid("plugin-pe-dll-header");
        const auto Machine = U16(Pe + 4), Magic = U16(Pe + 24);
        if (Machine == 0x14c && Magic == 0x10b) return Architecture::X86;
        if (Optional < 112 || Magic != 0x20b) return Invalid("plugin-pe-optional-header");
        if (Machine == 0x8664) return Architecture::X64;
        if (Machine == 0xaa64) return Architecture::Arm64;
        return Error{ErrorCategory::Unsupported, "plugin-pe-machine", {"plugin"}};
    }
}
