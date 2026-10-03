#pragma once
#include "../storage/storage.hpp"
#include <set>
#include <span>

namespace hc::plugins
{
    enum class PluginFormat { Wcx, Wlx, Wfx, Wdx, DopusVfs, DopusViewer, SevenZip };
    enum class Architecture { X64, X86, Arm64 };
    enum class ExecutionMode { Isolated, TrustedInProcess };
    enum class Permission { ReadContent, WriteContent, Network, UserInterface, ChildProcess };
    enum class Extension { ArchiveRead, ArchiveWrite, StorageRead, StorageWrite, Viewer, Metadata, ContentConvert };
    struct ProtocolRange
    {
        std::uint32_t major_ = 1, minimumMinor_ = 0, maximumMinor_ = 0;
        bool operator==(const ProtocolRange&) const = default;
    };
    struct ProtocolVersion
    {
        std::uint32_t major_ = 1, minor_ = 0;
        bool operator==(const ProtocolVersion&) const = default;
    };
    struct PluginManifest
    {
        std::uint32_t schemaVersion_ = 1;
        std::string id_, version_, binary_, sha256_;
        PluginFormat format_ = PluginFormat::Wcx;
        Architecture architecture_ = Architecture::X64;
        ExecutionMode execution_ = ExecutionMode::Isolated;
        ProtocolRange protocol_;
        std::set<Extension> extensions_;
        std::set<Permission> permissions_;
        // Relative package paths and hashes, included in the reviewed identity.
        std::map<std::string, std::string> dependencies_;
        // Executables are pinned but never passed to LoadLibrary. Child process
        // permission trusts the package; a Job process limit is not an EXE ACL.
        std::map<std::string, std::string> helpers_;
        std::string compatibility_;
        bool operator==(const PluginManifest&) const = default;
    };
    // Supplied by the application's trust store, never by the plugin manifest.
    // Bind the entire reviewed declaration and binary to prevent permission or
    // ABI declarations from silently changing under an existing approval.
    struct PluginApproval
    {
        PluginManifest reviewed_;
        bool approved_ = false;
        bool allowInProcess_ = false;
        std::set<Permission> granted_;
    };
    struct HostProfile
    {
        Architecture architecture_ = Architecture::X64;
        ProtocolRange protocol_;
        std::set<PluginFormat> formats_;
        std::set<Extension> extensions_;
        bool supportsIsolation_ = false;
        bool supportsInProcess_ = false;
    };
    struct PluginAdmission
    {
        ProtocolVersion protocol_;
        ExecutionMode execution_;
        std::set<Permission> permissions_;
    };
    storage::Result<storage::Unit> ArchiveWritePolicy(const PluginManifest& Manifest);
    storage::Result<storage::Unit> Validate(const PluginManifest& Manifest);
    storage::Result<ProtocolVersion> Negotiate(ProtocolRange Client, ProtocolRange Host);
    storage::Result<PluginAdmission> Admit(const PluginManifest& Manifest, const PluginApproval& Approval, const HostProfile& Host);
    // Header inspection only; does not load code or certify SDK compatibility.
    storage::Result<Architecture> InspectPe(std::span<const std::byte> Header, bool Executable = false);
}
