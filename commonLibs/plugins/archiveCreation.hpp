#pragma once

#include "pluginManifest.hpp"
#include <set>
#include <variant>

namespace hc::plugins
{
    enum class ArchiveCreatorKind { BuiltinSevenZip, BuiltinIso, Wcx, DopusVfs };
    enum class ArchiveConfigurationKind { None, BuiltinDialog, PluginDialog };
    enum class ArchiveSettingsKind { SevenZip, Iso, External };
    enum class ArchiveCreationEvidence { Unverified, BuiltinAdapter, VerifiedPackage };

    // Compare the entire reviewed package, including dependencies and helpers.
    // Native ISO has no plugin manifest; its loaded engine hash is still pinned.
    struct ArchiveCreatorIdentity
    {
        std::string engineSha256_;
        std::optional<PluginManifest> plugin_;
        bool operator==(const ArchiveCreatorIdentity&) const = default;
    };
    struct ArchiveCreateCapabilities
    {
        std::string format_, extension_;
        ArchiveSettingsKind settings_ = ArchiveSettingsKind::External;
        ArchiveCreationEvidence evidence_ = ArchiveCreationEvidence::Unverified;
        // Zero is the current legacy codec. Typed options require explicit
        // negotiation before any v1, v2 or streamed update is sent.
        std::uint32_t optionsVersion_ = 0;
        bool operator==(const ArchiveCreateCapabilities&) const = default;
    };
    struct ArchiveCreatorDescriptor
    {
        std::string id_, displayName_;
        ArchiveCreatorKind kind_ = ArchiveCreatorKind::Wcx;
        ArchiveCreatorIdentity identity_;
        ArchiveConfigurationKind configuration_ = ArchiveConfigurationKind::None;
        // Only independently verified new-archive formats belong here. Read
        // associations and manifest permission extensions are not this list.
        std::vector<ArchiveCreateCapabilities> formats_;
    };
    struct ArchiveCompositionOptions
    {
        bool preservePaths_ = true;
        bool includeSubdirectories_ = true;
        bool separateArchives_ = false;
        bool omitSelectedFolderRoot_ = false;
        bool OmitSelectedFolderRoot() const
        { return preservePaths_ && separateArchives_ && omitSelectedFolderRoot_; }
        bool operator==(const ArchiveCompositionOptions&) const = default;
    };

    enum class SevenZipMethod { Automatic, Copy, Lzma, Lzma2, Ppmd, Bzip2, Deflate, Deflate64 };
    enum class ArchiveSolidMode { Off, All, BlockSize };
    struct SevenZipCreateSettings
    {
        unsigned level_ = 5;
        SevenZipMethod method_ = SevenZipMethod::Automatic;
        // Missing means SDK default, not a host memory budget. Method-specific
        // ranges are validated by the SevenZip adapter before SDK invocation.
        std::optional<std::uint64_t> dictionaryBytes_;
        std::optional<unsigned> wordSize_, threads_;
        ArchiveSolidMode solid_ = ArchiveSolidMode::All;
        std::optional<std::uint64_t> solidBlockBytes_;
        bool encrypt_ = false;
        bool encryptHeaders_ = true;
        bool operator==(const SevenZipCreateSettings&) const = default;
    };
    inline SevenZipMethod EffectiveSevenZipMethod(std::string_view Format, const SevenZipCreateSettings& Settings)
    {
        if (Settings.level_ == 0) return SevenZipMethod::Copy;
        return Settings.method_ == SevenZipMethod::Automatic
            ? (Format == "7z" ? SevenZipMethod::Lzma2 : SevenZipMethod::Deflate) : Settings.method_;
    }
    inline unsigned SevenZipThreadLimit(std::string_view Format, SevenZipMethod Method)
    {
        if (Format == "zip") return 128;
        if (Method == SevenZipMethod::Lzma2) return 512;
        if (Method == SevenZipMethod::Lzma) return 2;
        if (Method == SevenZipMethod::Bzip2) return 64;
        return 1;
    }
    inline storage::Result<storage::Unit> ValidateSevenZipSettings(std::string_view Format, const SevenZipCreateSettings& Settings)
    {
        const auto Invalid = [] { return storage::Error{storage::ErrorCategory::InvalidRequest, "sevenzip-invalid-settings", {"sevenzip"}}; };
        if ((Format != "7z" && Format != "zip") || (Settings.level_ != 0 && Settings.level_ != 1 && Settings.level_ != 3
            && Settings.level_ != 5 && Settings.level_ != 7 && Settings.level_ != 9)
            || int(Settings.method_) < 0 || int(Settings.method_) > 7 || int(Settings.solid_) < 0 || int(Settings.solid_) > 2) return Invalid();
        const auto Method = EffectiveSevenZipMethod(Format, Settings);
        if ((Format == "zip" && Method == SevenZipMethod::Lzma2)
            || (Settings.threads_ && (!*Settings.threads_ || *Settings.threads_ > SevenZipThreadLimit(Format, Method)))) return Invalid();
        if (Settings.dictionaryBytes_) {
            const auto Size = *Settings.dictionaryBytes_;
            switch (Method) {
            case SevenZipMethod::Lzma: case SevenZipMethod::Lzma2: if (Size < 4096 || Size > (15ull << 28)) return Invalid(); break;
            case SevenZipMethod::Ppmd: if (Size < (1u << 20) || Size > (Format == "zip" ? (1ull << 28) : (1ull << 32) - 1024)) return Invalid(); break;
            case SevenZipMethod::Bzip2: if (Size < 100 * 1024 || Size > 900 * 1024 || Size % (100 * 1024)) return Invalid(); break;
            case SevenZipMethod::Deflate: if (Size != 32768) return Invalid(); break;
            case SevenZipMethod::Deflate64: if (Size != 65536) return Invalid(); break;
            default: return Invalid();
            }
        }
        if (Settings.wordSize_) {
            const auto Size = *Settings.wordSize_;
            if (Method == SevenZipMethod::Lzma || Method == SevenZipMethod::Lzma2) { if (Size < 5 || Size > 273) return Invalid(); }
            else if (Method == SevenZipMethod::Ppmd) { if (Size < 2 || Size > (Format == "zip" ? 16u : 32u)) return Invalid(); }
            else if (Method == SevenZipMethod::Deflate || Method == SevenZipMethod::Deflate64) { if (Size < 3 || Size > (Method == SevenZipMethod::Deflate ? 258u : 257u)) return Invalid(); }
            else return Invalid();
        }
        if (Settings.solid_ == ArchiveSolidMode::BlockSize && (!Settings.solidBlockBytes_ || !*Settings.solidBlockBytes_)) return Invalid();
        if (Format == "zip" && Settings.solid_ != ArchiveSolidMode::Off) return Invalid();
        return storage::Unit{};
    }
    enum class IsoCreateProfile { Iso9660, Joliet, UdfBridge, UdfWithoutJoliet };
    struct IsoCreateSettings
    {
        IsoCreateProfile profile_ = IsoCreateProfile::UdfBridge;
        std::string volumeLabel_ = "HYPERCOMMANDER";
        bool shareDuplicateContents_ = false;
        std::int64_t timestampUtc_ = 946684800;
        // This bounds the physical output, including metadata and alignment.
        std::uint64_t maxImageBytes_ = 128ull * 1024 * 1024 * 1024;
        bool operator==(const IsoCreateSettings&) const = default;
    };
    // WCX/DOpus own their settings. Without a verified snapshot API they use
    // configuration at execution time, including when a request is queued.
    struct ExternalArchiveCreateSettings
    {
        bool operator==(const ExternalArchiveCreateSettings&) const = default;
    };
    using ArchiveCreateSettings = std::variant<SevenZipCreateSettings, IsoCreateSettings, ExternalArchiveCreateSettings>;
    enum class ArchiveCreateSchedule { Immediate, Queued };
    struct ArchiveCreateRequest
    {
        std::vector<storage::EntrySnapshot> sources_;
        storage::Locator sourceRoot_;
        ArchiveCreatorDescriptor creator_;
        std::string format_;
        // A file for combined output; a destination directory for separate output.
        storage::Locator output_;
        ArchiveCompositionOptions composition_;
        ArchiveCreateSettings settings_ = ExternalArchiveCreateSettings{};
        ArchiveCreateSchedule schedule_ = ArchiveCreateSchedule::Immediate;
        std::string correlationId_;
    };

    inline storage::Result<storage::Unit> ValidateArchiveCreator(const ArchiveCreatorDescriptor& Creator)
    {
        const auto Invalid = [](const char* Code) -> storage::Result<storage::Unit> {
            return storage::Error{storage::ErrorCategory::InvalidRequest, Code, {"archive-create"}};
        };
        const auto Token = [](std::string_view Value) {
            return !Value.empty() && Value.size() <= 128 && Value.front() != '.' && Value.back() != '.'
                && Value.find("..") == Value.npos && std::ranges::all_of(Value, [](unsigned char Ch) {
                return (Ch >= 'a' && Ch <= 'z') || (Ch >= '0' && Ch <= '9') || Ch == '.' || Ch == '-'; });
        };
        const auto Hash = [](std::string_view Value) {
            return Value.size() == 64 && std::ranges::all_of(Value, [](unsigned char Ch) {
                return (Ch >= 'a' && Ch <= 'f') || (Ch >= '0' && Ch <= '9'); });
        };
        if (!Token(Creator.id_) || Creator.displayName_.empty()) return Invalid("archive-creator-invalid-name");
        if (!Hash(Creator.identity_.engineSha256_)) return Invalid("archive-creator-invalid-identity");
        bool Builtin = false;
        ArchiveSettingsKind Settings;
        switch (Creator.kind_) {
        case ArchiveCreatorKind::BuiltinSevenZip: Builtin = true; Settings = ArchiveSettingsKind::SevenZip; break;
        case ArchiveCreatorKind::BuiltinIso: Builtin = true; Settings = ArchiveSettingsKind::Iso; break;
        case ArchiveCreatorKind::Wcx: case ArchiveCreatorKind::DopusVfs: Settings = ArchiveSettingsKind::External; break;
        default: return Invalid("archive-creator-invalid-kind");
        }
        if ((Builtin && Creator.configuration_ != ArchiveConfigurationKind::BuiltinDialog)
            || (!Builtin && Creator.configuration_ != ArchiveConfigurationKind::None
                && Creator.configuration_ != ArchiveConfigurationKind::PluginDialog))
            return Invalid("archive-creator-invalid-configuration");
        if (Creator.kind_ == ArchiveCreatorKind::BuiltinIso) {
            if (Creator.identity_.plugin_) return Invalid("archive-creator-invalid-identity");
        } else {
            if (!Creator.identity_.plugin_) return Invalid("archive-creator-missing-manifest");
            const auto& Manifest = *Creator.identity_.plugin_;
            const auto Expected = Creator.kind_ == ArchiveCreatorKind::Wcx ? PluginFormat::Wcx
                : Creator.kind_ == ArchiveCreatorKind::DopusVfs ? PluginFormat::DopusVfs : PluginFormat::SevenZip;
            if (Manifest.format_ != Expected || Manifest.sha256_ != Creator.identity_.engineSha256_)
                return Invalid("archive-creator-invalid-identity");
            auto Checked = Validate(Manifest); if (!Checked.HasValue()) return Checked;
            const auto Write = Creator.kind_ == ArchiveCreatorKind::DopusVfs ? Extension::StorageWrite : Extension::ArchiveWrite;
            if (!Manifest.extensions_.contains(Write)) return Invalid("archive-creator-write-not-declared");
            if (Creator.configuration_ == ArchiveConfigurationKind::PluginDialog
                && !Manifest.permissions_.contains(Permission::UserInterface)) return Invalid("archive-creator-ui-not-declared");
        }
        std::set<std::string> Formats;
        for (const auto& Format : Creator.formats_) {
            if (!Token(Format.format_) || !Token(Format.extension_) || Format.extension_.find('.') != std::string::npos
                || !Formats.insert(Format.format_).second) return Invalid("archive-create-invalid-format");
            if (Format.settings_ != Settings || (Settings == ArchiveSettingsKind::External && Format.optionsVersion_ != 0))
                return Invalid("archive-create-settings-kind-mismatch");
            if (Format.evidence_ != (Builtin ? ArchiveCreationEvidence::BuiltinAdapter : ArchiveCreationEvidence::VerifiedPackage))
                return Invalid("archive-create-format-unverified");
            if ((Builtin && Format.extension_ != Format.format_)
                || (Settings == ArchiveSettingsKind::SevenZip && Format.format_ != "7z" && Format.format_ != "zip")
                || (Settings == ArchiveSettingsKind::Iso && Format.format_ != "iso"))
                return Invalid("archive-create-format-unsupported");
        }
        return storage::Unit{};
    }

    // This checks catalog evidence only. The worker must separately re-inspect
    // package files and perform normal approval/admission immediately before use.
    inline storage::Result<ArchiveCreateCapabilities> ValidateArchiveCreateSelection(
        const ArchiveCreatorDescriptor& Creator, const ArchiveCreatorIdentity& CurrentIdentity, std::string_view Format)
    {
        auto Checked = ValidateArchiveCreator(Creator); if (!Checked.HasValue()) return Checked.Failure();
        if (Creator.identity_ != CurrentIdentity)
            return storage::Error{storage::ErrorCategory::StaleRevision, "archive-creator-identity-changed", {"archive-create"}};
        const auto Found = std::ranges::find(Creator.formats_, Format, &ArchiveCreateCapabilities::format_);
        if (Found == Creator.formats_.end())
            return storage::Error{storage::ErrorCategory::Unsupported, "archive-create-format-unsupported", {"archive-create"}};
        return *Found;
    }
}
