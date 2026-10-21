#include "unEggBackend.hpp"
#include "unEggAbi.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <mutex>

namespace hc::plugins {
using namespace storage;
namespace {
Error Failure(std::string Code, ErrorCategory Category = ErrorCategory::Unavailable)
{ return {Category, std::move(Code), {"unegg"}}; }
Error EngineError(UINT Code) {
    return Failure("unegg-result:" + std::to_string(Code), Code == egg::Cancel ? ErrorCategory::Cancelled
        : Code == 9 ? ErrorCategory::AccessDenied : Code == 8 || Code == 5 ? ErrorCategory::Unsupported : ErrorCategory::Unavailable);
}
struct Api {
    egg::Create create_; egg::Destroy destroy_; egg::Validate validate_; egg::Open open_;
    egg::Action close_, extract_, test_; egg::Count count_; egg::GetHeader header_;
    explicit Api(HMODULE Module) :
        create_(reinterpret_cast<egg::Create>(GetProcAddress(Module, "EGG_CreateEgg"))),
        destroy_(reinterpret_cast<egg::Destroy>(GetProcAddress(Module, "EGG_DestroyEgg"))),
        validate_(reinterpret_cast<egg::Validate>(GetProcAddress(Module, "EGG_IsValidArchive"))),
        open_(reinterpret_cast<egg::Open>(GetProcAddress(Module, "EGG_OpenArchive"))),
        close_(reinterpret_cast<egg::Action>(GetProcAddress(Module, "EGG_CloseArchive"))),
        extract_(reinterpret_cast<egg::Action>(GetProcAddress(Module, "EGG_Extract"))),
        test_(reinterpret_cast<egg::Action>(GetProcAddress(Module, "EGG_Test"))),
        count_(reinterpret_cast<egg::Count>(GetProcAddress(Module, "EGG_GetFileCount"))),
        header_(reinterpret_cast<egg::GetHeader>(GetProcAddress(Module, "EGG_GetFileHeader"))) {}
    bool Valid() const { return create_ && destroy_ && validate_ && open_ && close_ && extract_ && test_ && count_ && header_; }
};
struct Archive {
    Api api_; egg::Handle handle_ = 0; HANDLE pinned_ = INVALID_HANDLE_VALUE;
    OperationContext context_; std::string path_; size_t count_ = 0;
    std::optional<Error> error_; std::recursive_mutex mutex_;
    std::optional<size_t> selected_; QString output_; uint64_t expected_ = 0;
    bool extracting_ = false, selectedSeen_ = false, selectedFinished_ = false;
    std::optional<storage::Password> password_; unsigned attempts_ = 0;
    explicit Archive(HMODULE Module) : api_(Module) {}
    ~Archive() { if (handle_) { api_.close_(handle_); api_.destroy_(&handle_); } if (pinned_ != INVALID_HANDLE_VALUE) CloseHandle(pinned_); }
    UINT Fail(Error Value) { if (!error_) error_ = std::move(Value); return egg::Cancel; }
    UINT Check(uint64_t Progress = 0) {
        if (!error_) error_ = context_.Check({"unegg"});
        if (error_) return egg::Cancel;
        if (extracting_) {
            const QFileInfo File(output_);
            Progress = File.exists() ? uint64_t(File.size()) : 0;
            if (Progress > expected_) return Fail(Failure("unegg-output-size", ErrorCategory::BudgetExceeded));
        }
        if (context_.progress_) context_.progress_(Progress);
        if (const auto Stop = context_.Check({"unegg"})) return Fail(*Stop);
        return egg::Success;
    }
    Result<Unit> ResultOf(UINT Code) {
        if (error_) return *error_;
        if (Code != egg::Success) return EngineError(Code);
        if (Check() != egg::Success) return *error_;
        return Unit{};
    }
    static UINT __cdecl Event(void* User, UINT Code, void* Parameter) noexcept {
        auto& Self = *static_cast<Archive*>(User);
        try {
            std::lock_guard Lock(Self.mutex_);
            if (Self.Check() != egg::Success) return egg::Cancel;
            if (!Parameter) return Self.Fail(Failure("unegg-null-event", ErrorCategory::InvalidRequest));
            switch (Code) {
            case 1: {
                auto& Value = *static_cast<egg::Extract*>(Parameter);
                if (!Self.extracting_ || Value.index_ != Self.selected_) return egg::Skip;
                if (Self.selectedSeen_) return Self.Fail(Failure("unegg-repeated-output", ErrorCategory::InvalidRequest));
                Self.selectedSeen_ = true;
                if (Self.output_.size() >= MAX_PATH) return Self.Fail(Failure("unegg-output-path-limit", ErrorCategory::Unsupported));
                wcscpy_s(Value.path_, reinterpret_cast<const wchar_t*>(Self.output_.utf16()));
                break;
            }
            case 2: {
                const auto& Value = *static_cast<egg::Progress*>(Parameter);
                return Self.Check(uint64_t(std::clamp<int>(Value.total_, 0, 10000)));
            }
            case 4: {
                const auto& Value = *static_cast<egg::Finish*>(Parameter);
                if (Value.result_ != egg::Success && Value.result_ != egg::Skip) return Self.Fail(EngineError(Value.result_));
                break;
            }
            case 6: {
                const auto& Value = *static_cast<egg::ActivityFinish*>(Parameter);
                if (Value.result_ != egg::Success && Value.result_ != egg::Skip) return Self.Fail(EngineError(Value.result_));
                if (Self.extracting_ && Value.index_ == Self.selected_ && Value.result_ == egg::Success) Self.selectedFinished_ = true;
                break;
            }
            case 7: {
                auto& Value = *static_cast<egg::Password*>(Parameter);
                SecureZeroMemory(Value.value_, sizeof(Value.value_));
                if (Value.attempt_ > 1) Self.password_.reset();
                if (!Self.password_) {
                    if (!Self.context_.passwordPrompt_) return Self.Fail(Failure("archive-password-required", ErrorCategory::AccessDenied));
                    if (++Self.attempts_ > 104) return Self.Fail(Failure("unegg-password-attempts", ErrorCategory::AccessDenied));
                    auto Reply = Self.context_.passwordPrompt_(Self.path_, Self.attempts_, Self.context_);
                    if (!Reply.HasValue()) return Self.Fail(Reply.Failure());
                    Self.password_.emplace(std::move(Reply.Value()));
                }
                auto Text = QString::fromStdString(Self.password_->Value()); auto Clear = qScopeGuard([&] { Text.fill(QChar::Null); });
                if (Text.size() >= 1024 || Text.contains(QChar::Null)) return Self.Fail(Failure("unegg-password-length", ErrorCategory::InvalidRequest));
                if (Self.Check() != egg::Success) return egg::Cancel;
                wcscpy_s(Value.value_, reinterpret_cast<const wchar_t*>(Text.utf16()));
                break;
            }
            case 8: return Self.Fail(EngineError(static_cast<egg::Error*>(Parameter)->code_));
            case 9: {
                auto& Value = *static_cast<egg::Test*>(Parameter);
                Value.skip_ = Self.selected_ && Value.index_ != *Self.selected_; break;
            }
            default: break;
            }
            return egg::Success;
        } catch (...) {
            try { std::lock_guard Lock(Self.mutex_); return Self.Fail(Failure("unegg-callback-exception", ErrorCategory::Internal)); }
            catch (...) { return egg::Cancel; }
        }
    }
    Result<ArchiveHeader> Header(size_t Index) {
        if (Check() != egg::Success) return *error_;
        egg::Header Value{}; Value.size_ = sizeof(Value);
        auto Done = ResultOf(api_.header_(handle_, Index, &Value)); if (!Done.HasValue()) return Done.Failure();
        if (!Value.name_) return Failure("unegg-entry-name", ErrorCategory::InvalidRequest);
        auto Name = QString::fromWCharArray(Value.name_); Name.replace('\\', '/');
        if (Name.isEmpty() || Name.startsWith('/') || Name.contains(':')) return Failure("unegg-entry-path", ErrorCategory::InvalidRequest);
        for (const auto& Part : Name.split('/')) if (Part == "." || Part == "..") return Failure("unegg-entry-path", ErrorCategory::InvalidRequest);
        if (Value.modified_ > INT64_MAX / 1000 || Value.modified_ < INT64_MIN / 1000) return Failure("unegg-entry-time", ErrorCategory::InvalidRequest);
        ArchiveHeader Header; Header.name_ = Name.toStdString(); Header.size_ = Value.unpacked_; Header.packedSize_ = Value.packed_;
        Header.attributes_ = Value.attributes_; Header.crc_ = Value.crc_; Header.modifiedUtcMs_ = Value.modified_ * 1000;
        Header.directory_ = (Value.attributes_ & FILE_ATTRIBUTE_DIRECTORY) != 0; Header.encrypted_ = Value.encrypted_ != FALSE;
        if (context_.workload_.IsAutomatic()) {
            if (Header.size_ > context_.workload_.automaticFileBytes_) return Failure("automatic-content-budget", ErrorCategory::BudgetExceeded);
            const auto Packed = Header.packedSize_ ? Header.packedSize_ : uint64_t(QFileInfo(QString::fromStdString(path_)).size());
            if (Header.size_ && (!Packed || (Header.size_ - 1) / Packed >= context_.workload_.automaticExpansionRatio_))
                return Failure("automatic-expansion-budget", ErrorCategory::BudgetExceeded);
        }
        return Header;
    }
};
Result<std::unique_ptr<Archive>> Open(HMODULE Module, std::string Path, OperationContext Context) {
    if (const auto Stop = Context.Check({"unegg"})) return *Stop;
    auto Value = std::make_unique<Archive>(Module); Value->context_ = Context; Value->path_ = Path;
    if (!Value->api_.Valid()) return Failure("unegg-exports", ErrorCategory::Unsupported);
    const auto Name = QDir::toNativeSeparators(QString::fromStdString(Path));
    if (!QFileInfo(Name).isAbsolute() || Name.contains(QChar::Null)) return Failure("unegg-absolute-path", ErrorCategory::InvalidRequest);
    Value->pinned_ = CreateFileW(reinterpret_cast<const wchar_t*>(Name.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (Value->pinned_ == INVALID_HANDLE_VALUE) return Failure("unegg-input-open", ErrorCategory::NotFound);
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(Value->pinned_, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        return Failure("unegg-input-type", ErrorCategory::Unsupported);
    auto Done = Value->ResultOf(Value->api_.create_(&Value->handle_, Archive::Event, Value.get())); if (!Done.HasValue()) return Done.Failure();
    if (!Value->handle_) return Failure("unegg-null-handle");
    int Type = 0; Done = Value->ResultOf(Value->api_.validate_(reinterpret_cast<const wchar_t*>(Name.utf16()), &Type));
    if (!Done.HasValue()) return Done.Failure();
    if (Type != 1 && Type != 2) return Failure("unegg-format", ErrorCategory::Unsupported);
    Done = Value->ResultOf(Value->api_.open_(Value->handle_, reinterpret_cast<const wchar_t*>(Name.utf16()))); if (!Done.HasValue()) return Done.Failure();
    Done = Value->ResultOf(Value->api_.count_(Value->handle_, &Value->count_)); if (!Done.HasValue()) return Done.Failure();
    if (Value->count_ > Context.EnumerationLimit()) return Failure("unegg-enumeration-budget", ErrorCategory::BudgetExceeded);
    return Value;
}
class Listing final : public IArchiveListing {
public:
    Listing(std::unique_ptr<Archive> Archive, bool Test) : archive_(std::move(Archive)), test_(Test) {}
    Result<ArchivePage> Next(size_t Count, OperationContext Context) override {
        if (!Count || !Context.maxPageItems_) return Failure("zero-page-size", ErrorCategory::InvalidRequest);
        archive_->context_ = Context; ArchivePage Page;
        if (test_) {
            for (size_t Index = 0; Index < archive_->count_; ++Index) { auto Header = archive_->Header(Index); if (!Header.HasValue()) return Header.Failure(); }
            auto Done = archive_->ResultOf(archive_->api_.test_(archive_->handle_)); if (!Done.HasValue()) return Done.Failure();
            test_ = false;
        }
        while (index_ < archive_->count_ && Page.entries_.size() < std::min(Count, Context.maxPageItems_)) {
            auto Header = archive_->Header(index_); if (!Header.HasValue()) return Header.Failure();
            Page.entries_.push_back(std::move(Header.Value())); ++index_;
        }
        Page.end_ = index_ == archive_->count_; return Page;
    }
private:
    std::unique_ptr<Archive> archive_; size_t index_ = 0; bool test_;
};
class Backend final : public IArchiveBackend {
public:
    explicit Backend(HMODULE Module) : module_(Module) {}
    Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Path, bool Test, OperationContext Context) override {
        auto Value = Open(module_, std::move(Path), Context); if (!Value.HasValue()) return Value.Failure();
        return std::unique_ptr<IArchiveListing>(new Listing(std::move(Value.Value()), Test));
    }
    Result<Unit> Extract(std::string Path, ArchiveHeader Expected, std::string Output, OperationContext Context) override {
        auto Value = Open(module_, std::move(Path), Context); if (!Value.HasValue()) return Value.Failure(); auto& Archive = *Value.Value();
        for (size_t Index = 0; Index < Archive.count_; ++Index) {
            auto Header = Archive.Header(Index); if (!Header.HasValue()) return Header.Failure(); const auto& Item = Header.Value();
            if (Item.name_ != Expected.name_) continue;
            if (Archive.selected_) return Failure("unegg-ambiguous-entry", ErrorCategory::InvalidRequest);
            if (Item.attributes_ & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return Failure("unegg-special-entry", ErrorCategory::Unsupported);
            if (Item.directory_ || Item.size_ != Expected.size_ || Item.crc_ != Expected.crc_) return Failure("unegg-entry-changed", ErrorCategory::StaleRevision);
            Archive.selected_ = Index;
        }
        if (!Archive.selected_) return Failure("unegg-entry-missing", ErrorCategory::NotFound);
        const auto Destination = QString::fromStdString(Output);
        if (!QFileInfo(Destination).isAbsolute() || Destination.contains(QChar::Null)) return Failure("unegg-output-path", ErrorCategory::InvalidRequest);
        // Keep staging under the caller's output lease so host termination also
        // reclaims it. The SDK receives this short name, never archive entry paths.
        QTemporaryDir Directory(QFileInfo(Destination).dir().filePath(".egg-XXXXXX"));
        if (!Directory.isValid()) return Failure("unegg-staging-directory");
        Archive.output_ = QDir::toNativeSeparators(Directory.filePath("payload")); Archive.expected_ = Expected.size_; Archive.extracting_ = true;
        if (Archive.output_.size() >= MAX_PATH) return Failure("unegg-output-path-limit", ErrorCategory::Unsupported);
        auto Done = Archive.ResultOf(Archive.api_.extract_(Archive.handle_)); if (!Done.HasValue()) return Done;
        if (!Archive.selectedSeen_ || !Archive.selectedFinished_) return Failure("unegg-selection-incomplete");
        QFile Input(Archive.output_);
        if (!Input.open(QIODevice::ReadOnly) || uint64_t(Input.size()) != Expected.size_) return Failure("unegg-output-size");
        QSaveFile Target(Destination); Target.setDirectWriteFallback(false);
        if (!Target.open(QIODevice::WriteOnly)) return Failure("unegg-output-open");
        uint64_t Copied = 0;
        while (!Input.atEnd()) {
            if (const auto Stop = Context.Check({"unegg"})) return *Stop;
            const auto Bytes = Input.read(1024 * 1024);
            if (Bytes.isEmpty() || Target.write(Bytes) != Bytes.size()) return Failure("unegg-output-copy");
            Copied += uint64_t(Bytes.size());
        }
        if (Copied != Expected.size_) return Failure("unegg-output-size");
        if (const auto Stop = Context.Check({"unegg"})) return *Stop;
        if (!Target.commit()) return Failure("unegg-output-commit");
        return Unit{};
    }
    Result<Unit> Update(ArchiveUpdate, OperationContext) override { return Failure("unegg-read-only", ErrorCategory::Unsupported); }
private:
    HMODULE module_;
};
}
std::shared_ptr<IArchiveBackend> MakeUnEggBackend(HMODULE Module) { return std::make_shared<Backend>(Module); }
}
