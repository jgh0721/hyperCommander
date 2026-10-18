#include "viewerHostBridge.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

namespace hc::document
{
    namespace
    {
        struct Handle
        {
            HANDLE value_ = nullptr;
            explicit Handle(HANDLE Value = nullptr) : value_(Value) {}
            ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
            bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
        };
        struct ProcessAttributes
        {
            std::vector<unsigned char> bytes_;
            LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
            ~ProcessAttributes() { if (list_) DeleteProcThreadAttributeList(list_); }
            bool Initialize(HANDLE* Job)
            {
                SIZE_T Size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &Size);
                if (!Size) return false;
                bytes_.resize(Size);
                auto* List = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes_.data());
                if (!InitializeProcThreadAttributeList(List, 1, 0, &Size)) return false;
                list_ = List;
                return UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                    Job, sizeof(HANDLE), nullptr, nullptr) != FALSE;
            }
        };
        std::wstring Quote(const std::wstring& Value)
        {
            std::wstring Output = L"\""; unsigned Slashes = 0;
            for (auto Ch : Value) { if (Ch == L'\\') { ++Slashes; continue; } Output.append(Ch == L'"' ? Slashes * 2 + 1 : Slashes, L'\\'); Slashes = 0; Output += Ch; }
            Output.append(Slashes * 2, L'\\'); return Output + L"\"";
        }
        std::string JsonPath(const wchar_t* Path)
        {
            const auto Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Path, -1, nullptr, 0, nullptr, nullptr);
            if (!Size || Size > 128 * 1024) return {};
            std::string Text(Size, 0); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Path, -1, Text.data(), Size, nullptr, nullptr); Text.pop_back();
            std::string Result = "\"";
            for (unsigned char Ch : Text) { if (Ch < 32) return {}; if (Ch == '"' || Ch == '\\') Result += '\\'; Result += char(Ch); }
            return Result + "\"";
        }
        bool Transfer(HANDLE Pipe, HANDLE Peer, HANDLE Stop, void* Bytes, DWORD Size, bool Writing, unsigned Timeout = HostExchangeTimeoutMs)
        {
            Handle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!Event.Valid()) return false;
            DWORD Offset = 0;
            const auto Deadline = GetTickCount64() + Timeout;
            while (Offset < Size)
            {
                OVERLAPPED Pending{}; Pending.hEvent = Event.value_; ResetEvent(Event.value_); DWORD Count = 0;
                const auto Done = Writing ? WriteFile(Pipe, static_cast<char*>(Bytes) + Offset, Size - Offset, &Count, &Pending)
                    : ReadFile(Pipe, static_cast<char*>(Bytes) + Offset, Size - Offset, &Count, &Pending);
                if (!Done && GetLastError() != ERROR_IO_PENDING) return false;
                if (!Done)
                {
                    HANDLE Events[]{Event.value_, Stop, Peer};
                    const auto Wait = WaitForMultipleObjects(Peer ? 3 : 2, Events, FALSE, DWORD(Deadline > GetTickCount64() ? Deadline - GetTickCount64() : 0));
                    if (Wait != WAIT_OBJECT_0) { CancelIoEx(Pipe, &Pending); GetOverlappedResult(Pipe, &Pending, &Count, TRUE); return false; }
                }
                if (!GetOverlappedResult(Pipe, &Pending, &Count, FALSE) || !Count || Count > Size - Offset) return false;
                Offset += Count;
            }
            return true;
        }
        HANDLE ServerPipe(const std::wstring& Name)
        {
            HANDLE Raw = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw)) return INVALID_HANDLE_VALUE;
            Handle Token(Raw); DWORD Size = 0; GetTokenInformation(Raw, TokenUser, nullptr, 0, &Size);
            std::vector<unsigned char> Bytes(Size); if (!GetTokenInformation(Raw, TokenUser, Bytes.data(), Size, &Size)) return INVALID_HANDLE_VALUE;
            LPWSTR Sid = nullptr; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Bytes.data())->User.Sid, &Sid)) return INVALID_HANDLE_VALUE;
            auto Sddl = L"D:P(A;;GA;;;" + std::wstring(Sid) + L")"; LocalFree(Sid);
            PSECURITY_DESCRIPTOR Descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Descriptor, nullptr)) return INVALID_HANDLE_VALUE;
            SECURITY_ATTRIBUTES Attributes{sizeof(Attributes), Descriptor, FALSE};
            auto Pipe = CreateNamedPipeW(Name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, HostPayloadLimit, HostPayloadLimit, 0, &Attributes);
            LocalFree(Descriptor); return Pipe;
        }
    }
    struct ViewerHostBridge::State
    {
        HWND container_;
        std::atomic<HWND> child_{nullptr};
        std::atomic<bool> failed_{false};
        std::atomic<unsigned> phase_{0}, error_{0}, exitCode_{STILL_ACTIVE};
        Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::mutex mutex_;
        std::condition_variable changed_;
        struct Request
        {
            std::string input_, output_;
            Handle done_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
            bool success_ = false, sdkLoad_ = false;
        };
        std::deque<std::shared_ptr<Request>> queue_;
        std::unique_ptr<Handle> abort_;
        bool abortDelivered_ = false;
        std::jthread worker_;
        explicit State(HWND Container) : container_(Container) {}
        void run()
        {
            std::array<unsigned char, 16> Connection{};
            if (!stop_.Valid() || BCryptGenRandom(nullptr, Connection.data(), Connection.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) { failed_ = true; return; }
            std::wstring Hex; for (auto Byte : Connection) { Hex += L"0123456789abcdef"[Byte >> 4]; Hex += L"0123456789abcdef"[Byte & 15]; }
            phase_ = 1;
            const auto Name = LR"(\\.\pipe\hc-hyperviewer-)" + Hex; Handle Pipe(ServerPipe(Name));
            HMODULE Module = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ServerPipe), &Module);
            std::wstring ModulePath(32768, 0); const auto Length = GetModuleFileNameW(Module, ModulePath.data(), DWORD(ModulePath.size()));
            ModulePath.resize(Length); const auto Directory = ModulePath.substr(0, ModulePath.find_last_of(L"\\/"));
            // The helper is resolved from this DLL only: beside it (flat packages), in HyperViewer\ (Opus
            // distributions), or at the product ROOT three levels up (ROOT\Addons\WLX\HyperViewer).
            std::wstring WorkingDirectory, Program;
            for (const auto* Relative : {L"", L"\\HyperViewer", L"\\..\\..\\.."}) {
                std::wstring Candidate(MAX_PATH * 4, 0);
                const auto Full = GetFullPathNameW((Directory + Relative).c_str(), DWORD(Candidate.size()), Candidate.data(), nullptr);
                if (!Full || Full >= Candidate.size()) continue;
                Candidate.resize(Full);
                if (GetFileAttributesW((Candidate + L"\\HyperEditor.exe").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                WorkingDirectory = Candidate; Program = Candidate + L"\\HyperEditor.exe"; break;
            }
            if (Program.empty()) { error_ = ERROR_FILE_NOT_FOUND; failed_ = true; return; }
            auto Command = Quote(Program) + L" --viewer-host --pipe " + Quote(Name) + L" --connection " + Hex
                + L" --parent " + std::to_wstring(reinterpret_cast<std::uintptr_t>(container_)) + L" --parent-process " + std::to_wstring(GetCurrentProcessId());
            std::wstring ConfigDirectory(32768, 0);
            const auto ConfigLength = GetEnvironmentVariableW(L"HC_DOCUMENT_CONFIG_DIR", ConfigDirectory.data(), DWORD(ConfigDirectory.size()));
            if (ConfigLength && ConfigLength < ConfigDirectory.size()) ConfigDirectory.resize(ConfigLength);
            else {
                const auto ExeLength = GetModuleFileNameW(nullptr, ConfigDirectory.data(), DWORD(ConfigDirectory.size()));
                if (!ExeLength || ExeLength >= ConfigDirectory.size()) { failed_ = true; return; }
                ConfigDirectory.resize(ExeLength); ConfigDirectory.resize(ConfigDirectory.find_last_of(L"\\/"));
            }
            Command += L" --config-dir " + Quote(ConfigDirectory);
            Handle ProductJob; BOOL InJob = FALSE;
            JOBOBJECT_BASIC_UI_RESTRICTIONS ExistingUi{};
            const bool RestrictedJob = IsProcessInJob(GetCurrentProcess(), nullptr, &InJob) && InJob
                && QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions, &ExistingUi, sizeof(ExistingUi), nullptr) && ExistingUi.UIRestrictionsClass;
            // UI-restricted HC already owns/reaps the complete process tree.
            // A general outer Job (launcher/test runner) is not our lifetime owner.
            // Windows permits nesting when neither Job sets UI limits:
            // https://learn.microsoft.com/windows/win32/procthread/nested-jobs
            if (!RestrictedJob) {
                ProductJob.value_ = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
                Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
#ifdef HC_RHWP_HOST_RUNTIME
                // One workspace plus the dedicated RHWP Job's eight slots.
                // Authorized for the qualified RHWP product configuration.
                Limits.BasicLimitInformation.ActiveProcessLimit = 9;
#else
                Limits.BasicLimitInformation.ActiveProcessLimit = 1;
#endif
                if (!ProductJob.Valid() || !SetInformationJobObject(ProductJob.value_, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits))) {
                    error_ = GetLastError(); failed_ = true; return;
                }
            }
            ProcessAttributes Attributes;
            if (ProductJob.Valid() && !Attributes.Initialize(&ProductJob.value_)) { error_ = GetLastError(); failed_ = true; return; }
            STARTUPINFOEXW Startup{};
            Startup.StartupInfo.cb = ProductJob.Valid() ? sizeof(Startup) : sizeof(STARTUPINFOW);
            Startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; Startup.StartupInfo.wShowWindow = SW_HIDE;
            Startup.lpAttributeList = Attributes.list_;
            PROCESS_INFORMATION Process{};
            // JOB_LIST binds lifetime ownership inside CreateProcess itself.
            // If the SDK host dies before ResumeThread, closing its last Job
            // handle still reaps the suspended child. No assign-after-create gap.
            const DWORD Flags = CREATE_NO_WINDOW | CREATE_SUSPENDED
                | (ProductJob.Valid() ? EXTENDED_STARTUPINFO_PRESENT : 0);
            if (!Pipe.Valid() || !CreateProcessW(Program.c_str(), Command.data(), nullptr, nullptr, FALSE,
                Flags, nullptr, WorkingDirectory.c_str(), &Startup.StartupInfo, &Process)) { error_ = GetLastError(); failed_ = true; return; }
            Handle Child(Process.hProcess), Thread(Process.hThread); phase_ = 2;
            // Do not execute the workspace before its lifetime Job owns it.
            if (ResumeThread(Thread.value_) == DWORD(-1)) {
                error_ = GetLastError(); TerminateProcess(Child.value_, 0xe0000102);
                WaitForSingleObject(Child.value_, 5000); failed_ = true; return;
            }
            // A killed SDK host must not strand an unrestricted DOpus helper.
            // The helper watches the parent; this owner also reaps on normal unload.
            Handle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED Connect{}; Connect.hEvent = Event.value_;
            bool Connected = ConnectNamedPipe(Pipe.value_, &Connect) != FALSE; auto Code = GetLastError();
            if (!Connected && Code == ERROR_PIPE_CONNECTED) Connected = true;
            if (!Connected && Code == ERROR_IO_PENDING)
            {
                HANDLE Events[]{Event.value_, stop_.value_, Child.value_};
                Connected = WaitForMultipleObjects(3, Events, FALSE, 10000) == WAIT_OBJECT_0;
                DWORD Count = 0; if (!Connected) CancelIoEx(Pipe.value_, &Connect);
                Connected = GetOverlappedResult(Pipe.value_, &Connect, &Count, TRUE) && Connected;
            }
            phase_ = 3; ULONG Peer = 0; HostFrame Ready;
            Connected = Connected && GetNamedPipeClientProcessId(Pipe.value_, &Peer) && Peer == Process.dwProcessId
                && Transfer(Pipe.value_, Child.value_, stop_.value_, &Ready, sizeof(Ready), false, 10000)
                && ValidFrame(Ready, Connection) && Ready.type_ == 0 && Ready.length_ == 0;
            phase_ = 4; DWORD Owner = 0; const auto Window = reinterpret_cast<HWND>(Ready.window_);
            Connected = Connected && Window && GetWindowThreadProcessId(Window, &Owner) && Owner == Peer && IsChild(container_, Window);
            if (Connected) { child_ = Window; phase_ = 5; }
            std::uint64_t Sequence = 0;
            while (Connected && WaitForSingleObject(stop_.value_, 0) == WAIT_TIMEOUT && WaitForSingleObject(Child.value_, 0) == WAIT_TIMEOUT)
            {
                std::shared_ptr<Request> Work;
                {
                    std::unique_lock Lock(mutex_); changed_.wait_for(Lock, std::chrono::milliseconds(50), [&] { return !queue_.empty() || WaitForSingleObject(stop_.value_, 0) != WAIT_TIMEOUT; });
                    if (abort_ && abort_->Valid()) {
                        const bool Cancelled = WaitForSingleObject(abort_->value_, 0) == WAIT_OBJECT_0;
                        if (Cancelled && !abortDelivered_) { Work = std::make_shared<Request>(); Work->input_ = R"({"action":"cancelHost"})"; }
                        abortDelivered_ = Cancelled;
                    }
                    if (!Work && !queue_.empty()) { Work = queue_.front(); queue_.pop_front(); }
                }
                if (!Work) continue;
                HostFrame Frame; Frame.connection_ = Connection; Frame.requestId_ = ++Sequence; Frame.length_ = unsigned(Work->input_.size());
                Connected = Transfer(Pipe.value_, Child.value_, stop_.value_, &Frame, sizeof(Frame), true)
                    && Transfer(Pipe.value_, Child.value_, stop_.value_, Work->input_.data(), Frame.length_, true);
                HostFrame Reply;
                Connected = Connected && Transfer(Pipe.value_, Child.value_, stop_.value_, &Reply, sizeof(Reply), false)
                    && ValidFrame(Reply, Connection) && Reply.type_ == 1 && Reply.requestId_ == Sequence;
                if (Connected) { Work->output_.resize(Reply.length_); Connected = Transfer(Pipe.value_, Child.value_, stop_.value_, Work->output_.data(), Reply.length_, false); }
                Work->success_ = Connected; SetEvent(Work->done_.value_);
            }
            DWORD ExitCode = STILL_ACTIVE; GetExitCodeProcess(Child.value_, &ExitCode); exitCode_ = ExitCode; error_ = GetLastError();
            failed_ = failed_ || WaitForSingleObject(stop_.value_, 0) == WAIT_TIMEOUT;
            child_ = nullptr;
            // Pipe loss asks the helper to checkpoint and exit. Force only after
            // a bounded grace period, and preserve the last completed snapshot.
            DisconnectNamedPipe(Pipe.value_);
            if (WaitForSingleObject(Child.value_, 2000) == WAIT_TIMEOUT) { TerminateProcess(Child.value_, 0xe0000100); WaitForSingleObject(Child.value_, 2000); }
            std::lock_guard Lock(mutex_); for (const auto& Request : queue_) SetEvent(Request->done_.value_); queue_.clear();
        }
    };
    ViewerHostBridge::ViewerHostBridge(HWND Container) : state_(std::make_unique<State>(Container)) {}
    ViewerHostBridge::~ViewerHostBridge() { Stop(); }
    void ViewerHostBridge::Start() { state_->worker_ = std::jthread([this] { state_->run(); }); }
    void ViewerHostBridge::Stop() { SetEvent(state_->stop_.value_); state_->changed_.notify_all(); if (state_->worker_.joinable()) state_->worker_.join(); }
    HWND ViewerHostBridge::Child() const { return state_->child_; }
    bool ViewerHostBridge::Failed() const { return state_->failed_; }
    std::string ViewerHostBridge::Diagnostic() const { return "{\"error\":\"document-helper-not-ready\",\"phase\":" + std::to_string(state_->phase_.load()) + ",\"nativeError\":" + std::to_string(state_->error_.load()) + ",\"exitCode\":" + std::to_string(state_->exitCode_.load()) + "}"; }
    void ViewerHostBridge::SetAbort(HANDLE Event)
    {
        HANDLE Copy = nullptr;
        if (Event && !DuplicateHandle(GetCurrentProcess(), Event, GetCurrentProcess(), &Copy, SYNCHRONIZE, FALSE, 0)) return;
        std::lock_guard Lock(state_->mutex_); state_->abort_ = std::make_unique<Handle>(Copy); state_->abortDelivered_ = false;
    }
    void ViewerHostBridge::Focus()
    {
        if(Failed()||WaitForSingleObject(state_->stop_.value_,0)!=WAIT_TIMEOUT)return;
        auto Request=std::make_shared<State::Request>();Request->input_=R"({"action":"focus"})";
        std::lock_guard Lock(state_->mutex_);
        for(const auto& Pending:state_->queue_)if(Pending->input_==Request->input_)return;
        if(state_->queue_.size()>=16)return;
        state_->queue_.push_back(Request);state_->changed_.notify_one();
    }
    bool ViewerHostBridge::Load(const wchar_t* Path)
    {
        if (!Path) return false;
        const auto Dot = wcsrchr(Path, L'.');
        const bool Rhwp = Dot && (!_wcsicmp(Dot, L".hwp") || !_wcsicmp(Dot, L".hwpx"));
#ifndef HC_RHWP_HOST_RUNTIME
        // SDK admission follows the same qualified package as the Job budget.
        if (Rhwp) return false;
#endif
        auto Escaped = JsonPath(Path); if (Escaped.empty() || Failed() || WaitForSingleObject(state_->stop_.value_, 0) != WAIT_TIMEOUT) return false;
        auto Request = std::make_shared<State::Request>(); Request->input_ = "{\"action\":\"openHost\",\"path\":" + Escaped + "}";
        Request->sdkLoad_ = true;
        std::lock_guard Lock(state_->mutex_);
        std::erase_if(state_->queue_, [](const auto& Pending) { return Pending->sdkLoad_; });
        if (state_->queue_.size() >= 16) return false;
        state_->queue_.push_back(Request); state_->changed_.notify_one(); return true;
    }
    bool ViewerHostBridge::Exchange(const char* Input, unsigned Size, std::string& Output)
    {
        if (!Child() || !Input || !Size || Size > HostPayloadLimit || Failed() || WaitForSingleObject(state_->stop_.value_, 0) != WAIT_TIMEOUT) return false;
        auto Request = std::make_shared<State::Request>(); Request->input_.assign(Input, Size);
        { std::lock_guard Lock(state_->mutex_); if (state_->queue_.size() >= 16) return false; state_->queue_.push_back(Request); }
        state_->changed_.notify_one(); const auto Deadline = GetTickCount64() + HostExchangeTimeoutMs;
        for (;;)
        {
            HANDLE Handles[]{Request->done_.value_, state_->stop_.value_};
            auto Wait = MsgWaitForMultipleObjectsEx(2, Handles, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (Wait == WAIT_OBJECT_0) { Output = std::move(Request->output_); return Request->success_; }
            if (Wait == WAIT_OBJECT_0 + 1) return false;
            if (GetTickCount64() >= Deadline) {
                // Quarantine the connection: an expired caller cannot enqueue
                // further writes while its previous request is still running.
                state_->failed_ = true; state_->error_ = ERROR_TIMEOUT; SetEvent(state_->stop_.value_); state_->changed_.notify_all(); return false;
            }
            if (Wait == WAIT_OBJECT_0 + 2) { MSG Message{}; for (unsigned Count = 0; Count < 32 && PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE); ++Count) { if (Message.message == WM_QUIT) return false; TranslateMessage(&Message); DispatchMessageW(&Message); } }
        }
    }
}
