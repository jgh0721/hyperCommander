#include "wcxBackend.hpp"
#include <QtCore>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "externalLibs/TotalCommander/wcxhead.h"
#include <mutex>

std::recursive_timed_mutex& WcxCallMutex() { static std::recursive_timed_mutex Mutex; return Mutex; }

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Result<std::unique_lock<std::recursive_timed_mutex>> Enter(OperationContext Context)
        {
            std::unique_lock Lock(WcxCallMutex(), std::defer_lock);
            while (!Lock.try_lock_for(std::chrono::milliseconds(25))) if (const auto Failure = Context.Check({"wcx"})) return *Failure;
            if (const auto Failure = Context.Check({"wcx"})) return *Failure;
            return std::move(Lock);
        }
        Error WcxError(int Code) { return {Code == E_EABORTED ? ErrorCategory::Cancelled : Code == E_NOT_SUPPORTED ? ErrorCategory::Unsupported : ErrorCategory::Unavailable,
            "wcx:" + std::to_string(Code), {"wcx"}}; }
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wcx"}}; }
        std::wstring Native(const std::string& Path) { return QDir::toNativeSeparators(QString::fromStdString(Path)).toStdWString(); }
        bool Absolute(const std::string& Path) { return !Path.empty() && Path.find('\0') == Path.npos && QFileInfo(QString::fromStdString(Path)).isAbsolute(); }
        struct Callback
        {
            OperationContext context_;
            std::string output_;
            std::uint64_t limit_ = 0;
            bool volume_ = false, exceeded_ = false;
            unsigned attempts_ = 0;
            std::optional<Error> passwordFailure_;
            std::uint64_t completed_ = 0;
            int percent_ = 0, totalPercent_ = 0;
            std::optional<Error> progressFailure_;
        };
        thread_local Callback* Current = nullptr;
        // NO_UI must never open a dialog. This adapter deliberately has no
        // persistent password store; plugins may ask again with LOAD_PASSWORD.
        int __stdcall CryptW(int, int Mode, wchar_t* Archive, wchar_t* Buffer, int MaxLength)
        {
            if (!Current) return E_EABORTED;
            if (Mode == PK_CRYPT_LOAD_PASSWORD_NO_UI) return E_NO_FILES;
            if (Mode != PK_CRYPT_LOAD_PASSWORD) return E_NOT_SUPPORTED;
            try
            {
                auto& State = *Current;
                if (const auto Stop = State.context_.Check({"wcx"})) { State.passwordFailure_ = *Stop; return E_EABORTED; }
                if (!Archive || wcsnlen_s(Archive, 32768) == 32768 || !Buffer || MaxLength <= 0 || MaxLength > 65536) return E_SMALL_BUF;
                Buffer[0] = 0;
                if (!State.context_.passwordPrompt_) { State.passwordFailure_ = Error{ErrorCategory::AccessDenied, "archive-password-required", {"wcx"}, {}, true}; return E_EABORTED; }
                if (++State.attempts_ > 3) { State.passwordFailure_ = Error{ErrorCategory::AccessDenied, "archive-password-attempt-limit", {"wcx"}}; return E_EABORTED; }
                auto Reply = State.context_.passwordPrompt_(QString::fromWCharArray(Archive).toStdString(), State.attempts_, State.context_);
                if (!Reply.HasValue()) { State.passwordFailure_ = Reply.Failure(); return E_EABORTED; }
                if (const auto Stop = State.context_.Check({"wcx"})) { State.passwordFailure_ = *Stop; return E_EABORTED; }
                auto Text = QString::fromStdString(Reply.Value().Value());
                auto Erase = qScopeGuard([&] { Text.fill(QChar::Null); });
                if (Text.contains(QChar::Null) || Text.size() >= MaxLength || Text.size() > 4096) return E_SMALL_BUF;
                std::copy_n(reinterpret_cast<const wchar_t*>(Text.utf16()), Text.size(), Buffer); Buffer[Text.size()] = 0;
                return 0;
            }
            catch (...) { Current->passwordFailure_ = Error{ErrorCategory::Internal, "archive-password-callback-failed", {"wcx"}}; return E_EABORTED; }
        }
        int __stdcall CryptA(int Number, int Mode, char* Archive, char* Buffer, int MaxLength)
        {
            if (Mode != PK_CRYPT_LOAD_PASSWORD) return Mode == PK_CRYPT_LOAD_PASSWORD_NO_UI ? E_NO_FILES : E_NOT_SUPPORTED;
            if (!Archive || strnlen_s(Archive, 32768) == 32768 || !Buffer || MaxLength <= 0 || MaxLength > 65536) return E_SMALL_BUF;
            Buffer[0] = 0;
            try
            {
            auto Name = QString::fromLocal8Bit(Archive).toStdWString(); wchar_t Wide[4097]{};
            auto Erase = qScopeGuard([&] { SecureZeroMemory(Wide, sizeof(Wide)); });
            const auto Code = CryptW(Number, Mode, Name.data(), Wide, 4097); if (Code) return Code;
            auto Text = QString::fromWCharArray(Wide); auto Bytes = Text.toLocal8Bit();
            auto Clear = qScopeGuard([&] { Text.fill(QChar::Null); Bytes.fill(0); });
            if (QString::fromLocal8Bit(Bytes) != Text || Bytes.size() >= MaxLength) return E_SMALL_BUF;
            std::copy_n(Bytes.constData(), Bytes.size(), Buffer); Buffer[Bytes.size()] = 0; return 0;
            }
            catch (...) { if (Current) Current->passwordFailure_ = Error{ErrorCategory::Internal, "archive-password-callback-failed", {"wcx"}}; return E_EABORTED; }
        }
        int __stdcall Progress(wchar_t*, int Bytes)
        {
            if (!Current || Current->context_.Check({"wcx"})) return 0;
            // WCX also encodes progress bars as -1..-100 / -1000..-1100.
            // Changed percentages are work signals, not byte counts. Repeated
            // values must not disguise a stalled plugin as advancing work.
            std::uint64_t Delta = Bytes > 0 ? std::uint64_t(Bytes) : 0;
            if (Bytes >= -100 && Bytes <= -1 && std::exchange(Current->percent_, Bytes) != Bytes) Delta = 1;
            if (Bytes >= -1100 && Bytes <= -1000 && std::exchange(Current->totalPercent_, Bytes) != Bytes) Delta = 1;
            if (Delta <= UINT64_MAX - Current->completed_) Current->completed_ += Delta;
            if (Current->context_.progress_) {
                try { Current->context_.progress_(Current->completed_); }
                catch (...) { Current->progressFailure_ = Error{ErrorCategory::Internal, "archive-progress-callback-failed", {"wcx"}}; return 0; }
            }
            if (!Current->output_.empty() && uint64_t(QFileInfo(QString::fromStdString(Current->output_)).size()) > Current->limit_)
            { Current->exceeded_ = true; return 0; }
            return 1;
        }
        int __stdcall Volume(wchar_t*, int) { if (Current) Current->volume_ = true; return 0; }
        struct CallbackScope
        {
            Callback* previous_;
            explicit CallbackScope(Callback& State) : previous_(std::exchange(Current, &State)) {}
            ~CallbackScope() { Current = previous_; }
        };
        Result<Unit> Check(int Code, const Callback& State)
        {
            if (State.passwordFailure_) return *State.passwordFailure_;
            if (State.progressFailure_) return *State.progressFailure_;
            if (State.volume_) return Error{ErrorCategory::Unsupported, "archive-volume-required", {"wcx"}};
            if (State.exceeded_) return Error{ErrorCategory::BudgetExceeded, "archive-extraction-budget", {"wcx"}};
            if (const auto Stop = State.context_.Check({"wcx"})) return *Stop;
            if (Code) return WcxError(Code); return Unit{};
        }
        struct Module
        {
            HMODULE module_ = nullptr;
            std::recursive_mutex mutex_;
            HANDLE (__stdcall* open_)(tOpenArchiveDataW*) = nullptr;
            int (__stdcall* header_)(HANDLE, tHeaderDataExW*) = nullptr;
            int (__stdcall* process_)(HANDLE, int, wchar_t*, wchar_t*) = nullptr;
            int (__stdcall* close_)(HANDLE) = nullptr;
            void (__stdcall* progress_)(HANDLE, tProcessDataProcW) = nullptr;
            void (__stdcall* volume_)(HANDLE, tChangeVolProcW) = nullptr;
            int (__stdcall* pack_)(wchar_t*, wchar_t*, wchar_t*, wchar_t*, int) = nullptr;
            int (__stdcall* remove_)(wchar_t*, wchar_t*) = nullptr;
            int background_ = 0, caps_ = 0;
            bool rebuildEncrypted_ = false, missingEncryptionMetadata_ = false;
            ~Module() { std::lock_guard Lock(WcxCallMutex()); if (module_) FreeLibrary(module_); }
            Result<Unit> Load(const std::string& Path, bool Background)
            {
                if (module_) return Unit{};
                if (!Absolute(Path)) return Invalid("absolute-plugin-path-required");
                // Bind the workaround to reviewed bytes, never a filename or
                // an untrusted archive extension. Older SQX compatibility builds
                // omit encryption metadata and must be re-prepared before writes.
                QFile Image(QString::fromStdString(Path));
                if (!Image.open(QIODevice::ReadOnly)) return Invalid("wcx-binary-unreadable");
                QCryptographicHash Hash(QCryptographicHash::Sha256); if (!Hash.addData(&Image)) return Invalid("wcx-binary-unreadable");
                const auto Digest = Hash.result().toHex();
                rebuildEncrypted_ = Digest == "af75ba9493b6ce04626cf9a7c2d4a74e8483e4c4363545af43d80fe9478b0cfb";
                missingEncryptionMetadata_ = Digest == "a05f301e0c24df9dc7bbbea6472c1eb53bf4839ea92be62e27267fd84c754f85"
                    || Digest == "ab36c94e5d6850b3f1d86b6732fb9dd1871957abbb7abb788d9ab32b1b884452";
                module_ = LoadLibraryExW(Native(Path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!module_) return Error{ErrorCategory::Unavailable, "wcx-load:" + std::to_string(GetLastError()), {"wcx"}};
#define HC_BIND(Member, Name) Member = reinterpret_cast<decltype(Member)>(GetProcAddress(module_, Name))
                HC_BIND(open_, "OpenArchiveW"); HC_BIND(header_, "ReadHeaderExW"); HC_BIND(process_, "ProcessFileW"); HC_BIND(close_, "CloseArchive");
                HC_BIND(progress_, "SetProcessDataProcW"); HC_BIND(volume_, "SetChangeVolProcW"); HC_BIND(pack_, "PackFilesW"); HC_BIND(remove_, "DeleteFilesW");
#undef HC_BIND
                const auto Flags = reinterpret_cast<int(__stdcall*)()>(GetProcAddress(module_, "GetBackgroundFlags"));
                const auto Caps = reinterpret_cast<int(__stdcall*)()>(GetProcAddress(module_, "GetPackerCaps"));
                background_ = Flags ? Flags() : 0; caps_ = Caps ? Caps() : 0;
                const auto CryptWide = reinterpret_cast<void(__stdcall*)(tPkCryptProcW, int, int)>(GetProcAddress(module_, "PkSetCryptCallbackW"));
                const auto CryptAnsi = reinterpret_cast<void(__stdcall*)(tPkCryptProc, int, int)>(GetProcAddress(module_, "PkSetCryptCallback"));
                if (CryptWide) CryptWide(&CryptW, 1, 0);
                else if (CryptAnsi) CryptAnsi(&CryptA, 1, 0);
                if (!open_ || !header_ || !process_ || !close_ || !progress_ || !volume_ || (Background && !(background_ & BACKGROUND_UNPACK)))
                { FreeLibrary(module_); module_ = nullptr; return Error{ErrorCategory::Unsupported, "wcx-background-unicode-read-required", {"wcx"}}; }
                return Unit{};
            }
        };
        Result<ArchiveHeader> Decode(const tHeaderDataExW& Header)
        {
            const auto Length = wcsnlen_s(Header.FileName, 1024);
            if (!Length || Length == 1024) return Invalid("invalid-archive-name");
            const auto Name = QString::fromWCharArray(Header.FileName, Length);
            ArchiveHeader Out; Out.name_ = Name.toStdString(); Out.attributes_ = Header.FileAttr; Out.directory_ = !!(Header.FileAttr & FILE_ATTRIBUTE_DIRECTORY);
            // TC's extended header convention (also used by Total7zip and
            // TotalSQX's published fillHeaderEx) has a signature and bit 3.
            // Unqualified Flags bits have no portable encryption meaning.
            Out.encrypted_ = (static_cast<unsigned>(Header.Flags) & 0xffffff08U) == 0x12345608U;
            Out.size_ = (std::uint64_t(Header.UnpSizeHigh) << 32) | Header.UnpSize; Out.packedSize_ = (std::uint64_t(Header.PackSizeHigh) << 32) | Header.PackSize;
            Out.crc_ = static_cast<std::uint32_t>(Header.FileCRC);
            const QDate Date(1980 + ((Header.FileTime >> 25) & 127), (Header.FileTime >> 21) & 15, (Header.FileTime >> 16) & 31);
            const QTime Time((Header.FileTime >> 11) & 31, (Header.FileTime >> 5) & 63, (Header.FileTime & 31) * 2);
            if (Date.isValid() && Time.isValid()) Out.modifiedUtcMs_ = QDateTime(Date, Time).toMSecsSinceEpoch();
            return Out;
        }
        class Listing final : public IArchiveListing
        {
        public:
            Listing(std::shared_ptr<Module> Module, HANDLE Handle, bool Test, std::uint64_t ArchiveBytes) : module_(std::move(Module)), handle_(Handle), test_(Test), archiveBytes_(ArchiveBytes) {}
            ~Listing() override { std::lock_guard Gate(WcxCallMutex()); if (handle_) { std::lock_guard Lock(module_->mutex_); module_->close_(handle_); } }
            Result<ArchivePage> Next(std::size_t Count, OperationContext Context) override
            {
                if (!Count || !Context.maxPageItems_) return Invalid("zero-page-size");
                auto Gate = Enter(Context); if (!Gate.HasValue()) return Gate.Failure();
                std::lock_guard Lock(module_->mutex_); ArchivePage Page; if (!handle_) { Page.end_ = true; return Page; }
                Callback State{Context}; CallbackScope Scope(State);
                for (std::size_t Index = 0; Index < std::min({Count, Context.maxPageItems_, std::size_t{256}}); ++Index)
                {
                    if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                    State.attempts_ = 0;
                    tHeaderDataExW Raw{}; const auto Code = module_->header_(handle_, &Raw);
                    auto Checked = Check(Code == E_END_ARCHIVE ? 0 : Code, State); if (!Checked.HasValue()) return Checked.Failure();
                    if (Code == E_END_ARCHIVE)
                    { const auto Closed = module_->close_(std::exchange(handle_, nullptr)); if (Closed) return WcxError(Closed); Page.end_ = true; break; }
                    if (++count_ > Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget", {"wcx"}};
                    auto Header = Decode(Raw); if (!Header.HasValue()) return Header.Failure();
                    Header.Value().rebuildEncrypted_ = module_->rebuildEncrypted_;
                    // Reject expansion bombs before PK_TEST asks the plugin to
                    // decompress candidate content for integrity verification.
                    if (test_ && !Header.Value().directory_ && Header.Value().size_)
                    {
                        // Solid archives report a shared block's packed bytes on
                        // its first member and zero on subsequent members. Bound
                        // both each member and the cumulative expansion, using
                        // the physical archive when a member has no own size.
                        const auto Packed = Header.Value().packedSize_ ? Header.Value().packedSize_ : archiveBytes_;
                        if (Header.Value().size_ > UINT64_MAX - testedBytes_ || (Context.workload_.IsAutomatic()
                            && (Header.Value().size_ > Context.workload_.automaticFileBytes_ || !Packed
                            || (Header.Value().size_ - 1) / Packed >= Context.workload_.automaticExpansionRatio_ || !archiveBytes_
                            || (testedBytes_ + Header.Value().size_ - 1) / archiveBytes_ >= Context.workload_.automaticExpansionRatio_)))
                            return Error{ErrorCategory::BudgetExceeded, "archive-expansion-budget", {"wcx"}};
                        testedBytes_ += Header.Value().size_;
                    }
                    Checked = Check(module_->process_(handle_, test_ && !Header.Value().directory_ ? PK_TEST : PK_SKIP, nullptr, nullptr), State);
                    if (!Checked.HasValue()) return Checked.Failure(); Page.entries_.push_back(std::move(Header.Value()));
                }
                return Page;
            }
        private:
            std::shared_ptr<Module> module_;
            HANDLE handle_;
            bool test_;
            std::size_t count_ = 0;
            std::uint64_t archiveBytes_ = 0, testedBytes_ = 0;
        };
        class Backend final : public IArchiveBackend
        {
        public:
            // Keep DLL references in worker calls/listings. A provider may be
            // released by a UI owner after its last worker has completed.
            Backend(std::string Path, bool Background) : path_(std::move(Path)), background_(Background) {}
            Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Archive, bool Test, OperationContext Context) override
            {
                auto Gate = Enter(Context); if (!Gate.HasValue()) return Gate.Failure();
                auto Plugin = std::make_shared<Module>(); std::lock_guard Lock(Plugin->mutex_); auto Opened = open(Plugin, Archive, Test ? PK_OM_EXTRACT : PK_OM_LIST, Context); if (!Opened.HasValue()) return Opened.Failure();
                return std::unique_ptr<IArchiveListing>(new Listing(Plugin, Opened.Value(), Test, std::uint64_t(QFileInfo(QString::fromStdString(Archive)).size())));
            }
            Result<Unit> Extract(std::string Archive, ArchiveHeader Expected, std::string Output, OperationContext Context) override
            {
                if (!Absolute(Output)) return Invalid("absolute-output-path-required");
                auto Gate = Enter(Context); if (!Gate.HasValue()) return Gate.Failure();
                auto Plugin = std::make_shared<Module>(); std::lock_guard Lock(Plugin->mutex_); auto Opened = open(Plugin, Archive, PK_OM_EXTRACT, Context); if (!Opened.HasValue()) return Opened.Failure();
                const auto Handle = Opened.Value(); auto Cleanup = qScopeGuard([&] { Plugin->close_(Handle); });
                Callback State{Context, Output, Expected.size_}; CallbackScope Scope(State);
                for (std::size_t Count = 0;; ++Count)
                {
                    if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                    State.attempts_ = 0;
                    tHeaderDataExW Raw{}; const auto Code = Plugin->header_(Handle, &Raw); auto Checked = Check(Code == E_END_ARCHIVE ? 0 : Code, State);
                    if (!Checked.HasValue()) return Checked; if (Code == E_END_ARCHIVE) return Error{ErrorCategory::NotFound, "archive-entry-not-found", {"wcx"}};
                    if (Count >= Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "archive-entry-budget", {"wcx"}};
                    auto Header = Decode(Raw); if (!Header.HasValue()) return Header.Failure();
                    if (Header.Value().name_ != Expected.name_)
                    { Checked = Check(Plugin->process_(Handle, PK_SKIP, nullptr, nullptr), State); if (!Checked.HasValue()) return Checked; continue; }
                    if (Header.Value().directory_ || Header.Value().size_ != Expected.size_ || Header.Value().crc_ != Expected.crc_)
                        return Error{ErrorCategory::StaleRevision, "archive-entry-changed", {"wcx"}};
                    auto Target = Native(Output); Checked = Check(Plugin->process_(Handle, PK_EXTRACT, nullptr, Target.data()), State); if (!Checked.HasValue()) return Checked;
                    if (QFileInfo(QString::fromStdString(Output)).size() > static_cast<qint64>(Expected.size_)) return Error{ErrorCategory::BudgetExceeded, "archive-extraction-budget", {"wcx"}};
                    const auto Closed = Plugin->close_(Handle); Cleanup.dismiss();
                    Checked = Check(Closed, State); if (!Checked.HasValue()) return Checked;
                    // Some plugins defer extraction until CloseArchive. Check
                    // the final size, including silent skips of existing files.
                    const QFileInfo Produced(QString::fromStdString(Output));
                    if (!Produced.isFile() || Produced.size() != static_cast<qint64>(Expected.size_))
                        return Invalid("archive-extracted-size-mismatch");
                    return Unit{};
                }
            }
            Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override
            {
                if (std::ranges::any_of(Request.reuseData_, [](bool Flag) { return Flag; })) return Error{ErrorCategory::Unsupported, "wcx-metadata-rename-unsupported", {"wcx"}};
                auto Gate = Enter(Context); if (!Gate.HasValue()) return Gate.Failure();
                if (!Absolute(Request.archive_) || !Absolute(Request.inputDirectory_) || !Request.maxOutputBytes_) return Invalid("invalid-archive-update");
                auto Plugin = std::make_shared<Module>(); std::lock_guard Lock(Plugin->mutex_); auto Loaded = Plugin->Load(path_, background_); if (!Loaded.HasValue()) return Loaded;
                if (Plugin->missingEncryptionMetadata_) return Error{ErrorCategory::Unsupported, "wcx-encryption-metadata-upgrade-required", {"wcx"}};
                if (Request.createNew_) {
                    if (Request.creation_ || QFileInfo::exists(QString::fromStdString(Request.archive_)) || !Request.remove_.empty() || Request.add_.empty()) return Invalid("wcx-invalid-create");
                    if (!Plugin->pack_ || !(Plugin->caps_ & PK_CAPS_NEW) || (Request.add_.size() > 1 && !(Plugin->caps_ & PK_CAPS_MULTIPLE))) return Error{ErrorCategory::Unsupported, "wcx-new-archive-unsupported", {"wcx"}};
                } else if (!Plugin->pack_ || !Plugin->remove_ || (Plugin->caps_ & (PK_CAPS_MODIFY | PK_CAPS_DELETE)) != (PK_CAPS_MODIFY | PK_CAPS_DELETE))
                    return Error{ErrorCategory::Unsupported, "wcx-background-mutation-required", {"wcx"}};
                if (background_ && !(Plugin->background_ & BACKGROUND_PACK)) return Error{ErrorCategory::Unsupported, "wcx-background-mutation-required", {"wcx"}};
                if (Request.encrypt_ && !Request.add_.empty() && !(Plugin->caps_ & PK_CAPS_ENCRYPT))
                    return Error{ErrorCategory::Unsupported, "wcx-encrypted-write-unsupported", {"wcx"}};
                const auto Multi = [](const std::vector<std::string>& Names) -> Result<std::wstring> {
                    std::wstring Out;
                    for (const auto& Name : Names)
                    {
                        if (Name.empty() || Name.find('\0') != Name.npos || Name.size() > 4096) return Invalid("archive-update-name");
                        auto Text = QString::fromStdString(Name); Text.replace('\\', '/'); const auto Parts = Text.split('/');
                        if (Text.startsWith('/') || Text.contains(':')) return Invalid("archive-update-name");
                        for (qsizetype Index = 0; Index < Parts.size(); ++Index)
                            if (Parts[Index] == "." || Parts[Index] == ".." || (Parts[Index].isEmpty() && Index != Parts.size() - 1)) return Invalid("archive-update-name");
                        auto Value = QDir::toNativeSeparators(Text).toStdWString(); Out += Value; Out += L'\0';
                    }
                    Out += L'\0'; return Out;
                };
                auto Removed = Multi(Request.remove_); if (!Removed.HasValue()) return Removed.Failure(); auto Added = Multi(Request.add_); if (!Added.HasValue()) return Added.Failure();
                Callback State{Context, Request.archive_, Request.maxOutputBytes_}; CallbackScope Scope(State);
                // Packing/deletion has no OpenArchive handle. TC supplies the
                // INVALID_HANDLE_VALUE sentinel; nullptr can be dereferenced
                // by plugins that distinguish per-archive callback storage.
                Plugin->progress_(INVALID_HANDLE_VALUE, &Progress); Plugin->volume_(INVALID_HANDLE_VALUE, &Volume);
                auto Archive = Native(Request.archive_), Input = Native(Request.inputDirectory_ + '/');
                if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                if (!Request.remove_.empty()) { auto Checked = Check(Plugin->remove_(Archive.data(), Removed.Value().data()), State); if (!Checked.HasValue()) return Checked; }
                State.attempts_ = 0;
                if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                if (!Request.add_.empty()) { auto Checked = Check(Plugin->pack_(Archive.data(), nullptr, Input.data(), Added.Value().data(), PK_PACK_SAVE_PATHS | (Request.encrypt_ ? PK_PACK_ENCRYPT : 0)), State); if (!Checked.HasValue()) return Checked; }
                if (std::uint64_t(QFileInfo(QString::fromStdString(Request.archive_)).size()) > Request.maxOutputBytes_) return Error{ErrorCategory::BudgetExceeded, "archive-output-budget", {"wcx"}};
                return Check(0, State);
            }
        private:
            Result<HANDLE> open(const std::shared_ptr<Module>& Plugin, const std::string& Archive, int Mode, OperationContext Context)
            {
                if (const auto Stop = Context.Check({"wcx"})) return *Stop;
                if (!Absolute(Archive)) return Invalid("absolute-archive-path-required");
                auto Loaded = Plugin->Load(path_, background_); if (!Loaded.HasValue()) return Loaded.Failure();
                Callback State{Context}; CallbackScope Scope(State); auto Path = Native(Archive);
                tOpenArchiveDataW Open{}; Open.ArcName = Path.data(); Open.OpenMode = Mode; const auto Handle = Plugin->open_(&Open);
                auto Checked = Check(Handle ? 0 : Open.OpenResult ? Open.OpenResult : E_EOPEN, State);
                if (!Checked.HasValue()) { if (Handle) Plugin->close_(Handle); return Checked.Failure(); }
                Plugin->progress_(Handle, &Progress); Plugin->volume_(Handle, &Volume); return Handle;
            }
            std::string path_;
            bool background_;
        };
    }
    std::shared_ptr<IArchiveBackend> MakeInProcessWcxBackend(std::string PluginPath, bool RequireBackground)
    { return std::make_shared<Backend>(std::move(PluginPath), RequireBackground); }
}
