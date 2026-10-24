#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hwpStartupTrace.hpp"
#include "hwpRuntimeSha256.hpp"
#include <Windows.h>
#include <Sddl.h>
#include "documentWebProcess.hpp"
#include "epubProductPolicy.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include "commonLibs/win/restrictedEnvironment.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <mutex>
#include <vector>

namespace hc::document
{
    namespace
    {
        struct Handle
        {
            HANDLE value_ = nullptr;
            ~Handle() { if(Valid()) CloseHandle(value_); }
            bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
            Handle() = default;
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
        };
        std::wstring Quote(const QString& Value)
        {
            std::wstring Out=L"\""; unsigned Slashes=0;
            for(auto Ch:Value.toStdWString())
            {
                if(Ch==L'\\') { ++Slashes; continue; }
                Out.append(Ch==L'"'?Slashes*2+1:Slashes,L'\\'); Slashes=0; Out+=Ch;
            }
            Out.append(Slashes*2,L'\\'); return Out+L'"';
        }
        bool ReadApprovedInventory(WebProcessRuntime& Runtime, QString& Error)
        {
            const bool ProductEpub = Runtime.kind_ == WebProcessKind::EpubReadOnly;
            const bool Epub = ProductEpub || Runtime.kind_ == WebProcessKind::EpubPrototype;
            const auto InventoryName = ProductEpub ? QStringLiteral("epub-runtime-inventory.json")
                : Epub ? QStringLiteral("poc-runtime.json") : QStringLiteral("runtime-inventory.json");
            const auto Fail = [&](const char* Code) {
                Error = QString::fromLatin1(Code);
                if(Epub) Error.replace(0, 4, "epub");
                return false;
            };
            const auto Pin = *Runtime.approvedInventorySha256_;
            if (!Runtime.sha256_.isEmpty()) return Fail("rhwp-runtime-mixed-authority");
            if (Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin)
                return Fail("rhwp-runtime-policy-unavailable");
            if (!QFileInfo(Runtime.root_).isAbsolute()) return Fail("rhwp-runtime-root");
            QFile Manifest(QDir(Runtime.root_).filePath(InventoryName));
            if (!Manifest.open(QIODevice::ReadOnly)) return Fail("rhwp-runtime-inventory-missing");
            const auto Bytes = Manifest.read(256 * 1024 + 1);
            if (Manifest.error() != QFileDevice::NoError || !Manifest.atEnd() || Bytes.size() > 256 * 1024)
                return Fail("rhwp-runtime-inventory-budget");
            if (QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Pin)
                return Fail("rhwp-runtime-inventory-hash");
            QJsonParseError Parse;
            const auto Document = QJsonDocument::fromJson(Bytes, &Parse);
            if (Parse.error != QJsonParseError::NoError || !Document.isObject())
                return Fail("rhwp-runtime-inventory-json");
            const auto Object = Document.object();
            if(ProductEpub) {
                if(Object.value("schemaVersion") != QJsonValue(3)
                    || Object.value("profile") != QJsonValue("epub-passive-read-only")
                    || Object.value("protocolVersion") != QJsonValue(1)
                    || Object.value("diagnosticOnly") != QJsonValue(false)
                    || Object.value("releaseEligible") != QJsonValue(true)
                    || Object.value("readOnly") != QJsonValue(true)
                    || Object.value("readerEngine") != QJsonValue("epubjs")
                    || Object.value("readerVersion") != QJsonValue("0.3.93")
                    || Object.value("helper") != QJsonValue("HyperEpubWeb.exe"))
                    return Fail("rhwp-runtime-not-approved");
            } else if(Epub) {
                // Development-only authority, separate from the RHWP release policy.
                // The caller supplies this pin independently; adjacent manifests never grant trust.
                if(Object.value("schemaVersion") != QJsonValue(2)
                    || Object.value("profile") != QJsonValue("epub-passive-read-only-poc")
                    || Object.value("protocolVersion") != QJsonValue(1)
                    || Object.value("diagnosticOnly") != QJsonValue(true)
                    || Object.value("releaseEligible") != QJsonValue(false)
                    || Object.value("helper") != QJsonValue("hc_epub_render_poc.exe"))
                    return Fail("rhwp-runtime-not-approved");
            } else if (Object.value("schemaVersion") != QJsonValue(1)
                || (Object.value("profile") != QJsonValue("native-read-only") && Object.value("profile") != QJsonValue("native-editor"))
                || Object.value("sourceCommit") != QJsonValue("1a76570e833917d15817415a53c09ad61ab3203f")
                || Object.value("releaseEligible") != QJsonValue(true))
                return Fail("rhwp-runtime-not-approved");
            const auto BundlePin = Object.value(Epub ? "bundleManifestSha256" : "assetManifestSha256").toString().toLatin1();
            if (BundlePin.size() != 64 || QByteArray::fromHex(BundlePin).toHex() != BundlePin)
                return Fail("rhwp-runtime-bundle-pin");
            if (!Object.value("files").isArray()) return Fail("rhwp-runtime-files");
            const auto Files = Object.value("files").toArray();
            if (Files.isEmpty() || Files.size() > 511) return Fail("rhwp-runtime-files");
            QMap<QString, QByteArray> Pins; QSet<QString> Seen;
            for (const auto& Value : Files) {
                if (!Value.isObject()) return Fail("rhwp-runtime-entry");
                const auto Entry = Value.toObject();
                if (!Entry.value("path").isString() || !Entry.value("sha256").isString())
                    return Fail("rhwp-runtime-entry");
                const auto Path = Entry.value("path").toString();
                const auto Sha = Entry.value("sha256").toString().toLatin1();
                const auto Parts = Path.split('/');
                if (Path.isEmpty() || Path.contains('\\') || Path.contains(':') || QDir::isAbsolutePath(Path)
                    || Parts.size() > 32 || Parts.contains("") || Parts.contains(".") || Parts.contains("..")
                    || Seen.contains(Path.toCaseFolded()) || Path.compare(InventoryName, Qt::CaseInsensitive) == 0
                    || Sha.size() != 64 || QByteArray::fromHex(Sha).toHex() != Sha)
                    return Fail("rhwp-runtime-entry");
                for (const auto& Part : Parts) {
                    // Reject Win32 aliases and control characters before filesystem access.
                    if (Part.endsWith('.') || Part.endsWith(' ') || Part.contains(QChar(0)))
                        return Fail("rhwp-runtime-entry");
                    for (const auto Ch : Part)
                        if (Ch.unicode() < 32 || QStringLiteral("<>\"|?*").contains(Ch))
                            return Fail("rhwp-runtime-entry");
                }
                Seen.insert(Path.toCaseFolded()); Pins.insert(Path, Sha);
            }
            if (!Pins.contains(ProductEpub ? "HyperEpubWeb.exe" : Epub ? "hc_epub_render_poc.exe" : "HyperEditorWeb.exe") || !Pins.contains("QtWebEngineProcess.exe")
                || Pins.value(Epub ? QStringLiteral("bundle/bundle.json") : QString(hc::layout::RhwpBundle) + "/asset-manifest.json") != BundlePin
                || (Epub && !Pins.contains("bundle/reader.js"))
                || (ProductEpub && !Pins.contains("bundle/THIRD-PARTY-NOTICES.txt")))
                return Fail("rhwp-runtime-required-files");
            // Pin the inventory itself as well: the existing native acquisition
            // locks and rehashes ALL files before CreateProcess, closing the
            // manifest-read/launch replacement race. Helper independently admits
            // its bundle against the pin compiled into HyperEditorWeb.
            Pins.insert(InventoryName, Pin);
            Runtime.sha256_ = std::move(Pins);
            return true;
        }
        bool WaitIo(HANDLE Pipe, HANDLE Process, OVERLAPPED& Overlap, DWORD Timeout, DWORD& Count)
        {
            HANDLE Events[]{Overlap.hEvent,Process};
            if(WaitForMultipleObjects(2,Events,FALSE,Timeout)!=WAIT_OBJECT_0)
            {
                CancelIoEx(Pipe,&Overlap);
                GetOverlappedResult(Pipe,&Overlap,&Count,TRUE);
                return false;
            }
            return GetOverlappedResult(Pipe,&Overlap,&Count,FALSE)!=FALSE;
        }
        bool Transfer(HANDLE Pipe, HANDLE Process, void* Bytes, DWORD Size, bool Write, ULONGLONG Deadline)
        {
            Handle Event; Event.value_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!Event.Valid()) return false;
            DWORD Offset=0;
            while(Offset<Size)
            {
                const auto Now=GetTickCount64(); if(Now>=Deadline) return false;
                ResetEvent(Event.value_); OVERLAPPED Overlap{}; Overlap.hEvent=Event.value_; DWORD Count=0;
                const bool Done=(Write?WriteFile(Pipe,static_cast<char*>(Bytes)+Offset,Size-Offset,&Count,&Overlap)
                    :ReadFile(Pipe,static_cast<char*>(Bytes)+Offset,Size-Offset,&Count,&Overlap))!=FALSE;
                if(!Done && (GetLastError()!=ERROR_IO_PENDING || !WaitIo(Pipe,Process,Overlap,DWORD(Deadline-Now),Count))) return false;
                if(!Count) return false; Offset+=Count;
            }
            return true;
        }
    }
    struct DocumentWebProcess::State
    {
        mutable std::mutex mutex_;
        Handle job_, process_, pipe_;
        std::vector<std::unique_ptr<Handle>> leases_;
        std::array<unsigned char,16> connection_{};
        DWORD pid_=0;
        std::uintptr_t childWindow_=0;
        std::uint64_t sequence_=0;
        bool authenticated_=false, closed_=false, retired_=false;
        bool close(std::uint32_t TimeoutMs=5000)
        {
            if(retired_) return true;
            authenticated_=false; closed_=true;
            // Retain the Job handle and leases on timeout/query failure. Closing
            // the handle here would destroy the only proof of descendant exit.
            if(job_.Valid()) TerminateJobObject(job_.value_,0);
            // Also covers a suspended process whose Job assignment failed.
            if(process_.Valid()) TerminateProcess(process_.value_,0);
            const auto Deadline=GetTickCount64()+std::min(TimeoutMs,30000U);
            for(;;)
            {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
                if(job_.Valid() && !QueryInformationJobObject(job_.value_,JobObjectBasicAccountingInformation,
                    &Accounting,sizeof(Accounting),nullptr)) return false;
                const bool ProcessDone=!process_.Valid() || WaitForSingleObject(process_.value_,0)==WAIT_OBJECT_0;
                if(!Accounting.ActiveProcesses && ProcessDone) break;
                if(GetTickCount64()>=Deadline) return false;
                Sleep(10);
            }
            if(job_.Valid()) { CloseHandle(job_.value_); job_.value_=nullptr; }
            if(pipe_.Valid()) { CloseHandle(pipe_.value_); pipe_.value_=nullptr; }
            leases_.clear(); retired_=true; return true;
        }
        ~State()
        {
            // Worker-only RAII fallback must not release runtime leases or the
            // last Job handle while termination remains unconfirmed. Callers
            // needing bounded progress retain the owner and retry Close instead.
            while(!close(100)) Sleep(100);
        }
    };
    DocumentWebProcess::DocumentWebProcess(std::unique_ptr<State> Value):state_(std::move(Value)) {}
    DocumentWebProcess::~DocumentWebProcess() = default;
    WebProcessRuntime MakeProductEpubRuntime(const QString& ApplicationDirectory, const QByteArray& ApprovedInventorySha256)
    {
        WebProcessRuntime Runtime;
        Runtime.root_ = QDir(ApplicationDirectory).filePath("Libs/Epub");
        Runtime.kind_ = WebProcessKind::EpubReadOnly;
        Runtime.approvedInventorySha256_ = ApprovedInventorySha256;
        return Runtime;
    }
    WebProcessRuntime MakeProductEpubRuntime(const QString& ApplicationDirectory)
    {
        return MakeProductEpubRuntime(ApplicationDirectory, QByteArray(HC_EPUB_APPROVED_INVENTORY_SHA256));
    }
    std::unique_ptr<DocumentWebProcess> DocumentWebProcess::Start(const WebProcessRuntime& RequestedRuntime,
        WebProcessLimits Limits,std::uintptr_t ParentWindow,WebProcessError& Error,
        std::unique_ptr<DocumentWebProcess>* PendingRetirement, const std::function<bool()>& Cancelled)
    {
        startup::Scope StartupSpan("editor.nativeBackendStart");
        Error={};
        auto Runtime = RequestedRuntime;
        const auto Executable = Runtime.kind_ == WebProcessKind::Rhwp ? QStringLiteral("HyperEditorWeb.exe")
            : Runtime.kind_ == WebProcessKind::EpubPrototype ? QStringLiteral("hc_epub_render_poc.exe")
            : Runtime.kind_ == WebProcessKind::EpubReadOnly ? QStringLiteral("HyperEpubWeb.exe") : QString();
        if(Executable.isEmpty()) {
            Error.code_ = "web-owner-runtime-policy"; return {};
        }
        if(Runtime.kind_ == WebProcessKind::EpubReadOnly && !Runtime.approvedInventorySha256_) {
            Error.code_ = "epub-runtime-policy-unavailable"; return {};
        }
        { startup::Scope Inventory("runtime.inventory");
          if (Runtime.approvedInventorySha256_ && !ReadApprovedInventory(Runtime, Error.code_)) return {}; }
        auto S=std::make_unique<State>();
        auto Fail=[&](const char* Code) -> std::unique_ptr<DocumentWebProcess> {
            Error={QString::fromLatin1(Code),GetLastError()};
            DWORD Exit = STILL_ACTIVE;
            if (S->process_.Valid() && GetExitCodeProcess(S->process_.value_, &Exit) && Exit != STILL_ACTIVE)
                Error.childExitCode_ = Exit;
            if(!S->close() && PendingRetirement && !*PendingRetirement)
                *PendingRetirement=std::unique_ptr<DocumentWebProcess>(new DocumentWebProcess(std::move(S)));
            return {};
        };
        if((Limits.processes_&&(Limits.processes_<2 || Limits.processes_>64))
            || (Limits.memoryBytes_&&(Limits.memoryBytes_<256ULL*1024*1024||Limits.memoryBytes_>8ULL*1024*1024*1024))
            || !Limits.startupMs_ || Limits.startupMs_>30000) return Fail("web-owner-limits");
        BOOL InJob=FALSE;
        if(!IsProcessInJob(GetCurrentProcess(),nullptr,&InJob)) return Fail("web-owner-job-query");
        if(InJob)
        {
            JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{};
            if(!QueryInformationJobObject(nullptr,JobObjectBasicUIRestrictions,&Ui,sizeof(Ui),nullptr)) return Fail("web-owner-ui-query");
            if(Ui.UIRestrictionsClass) return Fail("web-owner-ui-restricted-caller");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION ParentLimits{};
            if(!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&ParentLimits,sizeof(ParentLimits),nullptr))
                return Fail("web-owner-parent-limits-query");
            if(ParentLimits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
                if(!QueryInformationJobObject(nullptr,JobObjectBasicAccountingInformation,&Accounting,sizeof(Accounting),nullptr))
                    return Fail("web-owner-parent-accounting-query");
                // In particular, the legacy SDK host's one-process Job cannot
                // contain a WebEngine helper. Do not relax or escape that Job.
                if(Accounting.ActiveProcesses >= ParentLimits.BasicLimitInformation.ActiveProcessLimit)
                    return Fail("web-owner-parent-process-limit");
            }
        }
        if(ParentWindow)
        {
            DWORD Owner=0; GetWindowThreadProcessId(reinterpret_cast<HWND>(ParentWindow),&Owner);
            if(Owner!=GetCurrentProcessId()) return Fail("web-owner-parent-authority");
        }
        const auto Root=QFileInfo(Runtime.root_).canonicalFilePath();
        if(Root.isEmpty() || !QFileInfo(Root).isDir() || Runtime.sha256_.isEmpty() || Runtime.sha256_.size()>512
            || !Runtime.sha256_.contains(Executable)) return Fail("web-owner-runtime");
        startup::Scope LeasesAndHash("runtime.leasesAndHash");
        startup::Scope ProviderInit("runtime.shaProviderInitialize");
        HwpRuntimeSha256 RuntimeHash;
        ProviderInit.End();startup::Data().SetRuntimeHashBackend(RuntimeHash.UsesCng());
        double HashMs = 0; qint64 HashBytes = 0, HashFiles = 0;
        QSet<QString> Seen;
        for(auto At=Runtime.sha256_.cbegin();At!=Runtime.sha256_.cend();++At)
        {
            const auto Relative=At.key(); const auto Parts=Relative.split('/');
            if(Relative.isEmpty() || Relative.contains('\\') || Relative.contains(':') || QDir::isAbsolutePath(Relative)
                || Parts.size()>32 || Parts.contains("..") || Parts.contains(".") || Parts.contains("") || Seen.contains(Relative.toCaseFolded())
                || At.value().size()!=64 || QByteArray::fromHex(At.value()).toHex()!=At.value()) return Fail("web-owner-runtime-entry");
            Seen.insert(Relative.toCaseFolded());
            // Do not follow junctions/symlinks supplied as runtime children. Keep
            // each directory open against rename while acquiring descendant leases.
            auto Current=Root;
            for(int I=-1;I<Parts.size();++I)
            {
                if(I>=0) Current+='/'+Parts[I];
                const auto Attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(Current.utf16()));
                if(Attributes==INVALID_FILE_ATTRIBUTES || (Attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return Fail("web-owner-runtime-reparse");
                auto Lease=std::make_unique<Handle>();
                const bool Directory=(Attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
                if(Directory!=(I<Parts.size()-1)) return Fail("web-owner-runtime-kind");
                Lease->value_=CreateFileW(reinterpret_cast<LPCWSTR>(Current.utf16()),Directory?FILE_READ_ATTRIBUTES:GENERIC_READ,
                    Directory?(FILE_SHARE_READ|FILE_SHARE_WRITE):FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                    (Directory?FILE_FLAG_BACKUP_SEMANTICS:FILE_FLAG_SEQUENTIAL_SCAN)|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(!Lease->Valid()) return Fail("web-owner-runtime-lease");
                BY_HANDLE_FILE_INFORMATION Info{};
                if(!GetFileInformationByHandle(Lease->value_,&Info)) return Fail("web-owner-runtime-info");
                if(Info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) return Fail("web-owner-runtime-reparse");
                if(((Info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=Directory) return Fail("web-owner-runtime-kind");
                if(!Directory)
                {
                    LARGE_INTEGER Size{};
                    if(!GetFileSizeEx(Lease->value_,&Size) || Size.QuadPart<0 || Size.QuadPart>512LL*1024*1024) return Fail("web-owner-runtime-size");
                    const auto HashStart = startup::Data().Now();
                    QByteArray Digest;
                    const auto Hashed=RuntimeHash.Read(Lease->value_,Size.QuadPart,Digest,Cancelled);
                    if(Hashed==RuntimeHashResult::ReadFailed) return Fail("web-owner-runtime-read");
                    if(Hashed==RuntimeHashResult::LengthMismatch) return Fail("web-owner-runtime-size");
                    if(Hashed==RuntimeHashResult::Cancelled) return Fail("web-owner-runtime-cancelled");
                    if(Hashed!=RuntimeHashResult::Success || Digest!=At.value()) return Fail("web-owner-runtime-hash");
                    HashMs += startup::Data().Now()-HashStart; HashBytes += Size.QuadPart; ++HashFiles;
                }
                S->leases_.push_back(std::move(Lease));
            }
        }
        startup::Scope ProviderCleanup("runtime.shaProviderCleanup");
        const bool ProviderClosed=RuntimeHash.Close();ProviderCleanup.End();
        if(!ProviderClosed) return Fail("web-owner-runtime-hash");
        if(Cancelled && Cancelled()) return Fail("web-owner-runtime-cancelled");
        LeasesAndHash.End(HashBytes,HashFiles);
        startup::Data().Aggregate("runtime.hashReadAndSha256",HashMs,HashBytes,HashFiles);
        startup::Scope Setup("helper.spawnSetup");
        Handle Original, Token;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY,&Original.value_)) return Fail("web-owner-token");
        BYTE Admin[SECURITY_MAX_SID_SIZE]; DWORD SidSize=sizeof(Admin);
        if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,Admin,&SidSize)) return Fail("web-owner-admin-sid");
        SID_AND_ATTRIBUTES Denied{Admin,0};
        if(!CreateRestrictedToken(Original.value_,DISABLE_MAX_PRIVILEGE,1,&Denied,0,nullptr,0,nullptr,&Token.value_)) return Fail("web-owner-token-filter");
        DWORD UserSize=0; GetTokenInformation(Original.value_,TokenUser,nullptr,0,&UserSize); std::vector<unsigned char> User(UserSize);
        if(!GetTokenInformation(Original.value_,TokenUser,User.data(),UserSize,&UserSize)) return Fail("web-owner-user");
        LPWSTR Sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(User.data())->User.Sid,&Sid)) return Fail("web-owner-user-sid");
        const auto Sddl=L"D:P(A;;GA;;;"+std::wstring(Sid)+L")"; LocalFree(Sid);
        PSECURITY_DESCRIPTOR Descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(),SDDL_REVISION_1,&Descriptor,nullptr)) return Fail("web-owner-pipe-acl");
        SECURITY_ATTRIBUTES Security{sizeof(Security),Descriptor,FALSE};
        const auto PipeName=QStringLiteral("\\\\.\\pipe\\hc-web-owner-")+QUuid::createUuid().toString(QUuid::Id128);
        S->pipe_.value_=CreateNamedPipeW(reinterpret_cast<LPCWSTR>(PipeName.utf16()),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,HostPayloadLimit,HostPayloadLimit,0,&Security);
        LocalFree(Descriptor); if(!S->pipe_.Valid()) return Fail("web-owner-pipe");
        const auto Nonce=QUuid::createUuid().toRfc4122(); std::memcpy(S->connection_.data(),Nonce.constData(),16);
        S->job_.value_=CreateJobObjectW(nullptr,Limits.jobName_.isEmpty()?nullptr:reinterpret_cast<LPCWSTR>(Limits.jobName_.utf16()));
        const auto JobError=GetLastError();if(!S->job_.Valid() || (!Limits.jobName_.isEmpty() && JobError==ERROR_ALREADY_EXISTS)) return Fail("web-owner-job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Job{};
        Job.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if(Limits.processes_)Job.BasicLimitInformation.LimitFlags|=JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        if(Limits.memoryBytes_)Job.BasicLimitInformation.LimitFlags|=JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY;
        Job.BasicLimitInformation.ActiveProcessLimit=Limits.processes_;
        Job.ProcessMemoryLimit=Job.JobMemoryLimit=SIZE_T(Limits.memoryBytes_);
        if(!SetInformationJobObject(S->job_.value_,JobObjectExtendedLimitInformation,&Job,sizeof(Job))) return Fail("web-owner-job-limits");
        // No UI restrictions and no breakaway flags: Chromium can nest its own
        // sandbox Job while this owner retains termination and resource limits.
        const auto Program=Root+"/"+Executable;
        auto Command=Quote(Program)+L" --web-owner --pipe "+Quote(PipeName)+L" --connection "+QString::fromLatin1(Nonce.toHex()).toStdWString()
            +L" --parent-process "+std::to_wstring(GetCurrentProcessId())+L" --parent "+std::to_wstring(ParentWindow);
        auto Environment=hc::win::RestrictedEnvironment(Root+";"+qEnvironmentVariable("SystemRoot")+"/System32");
        if(startup::Enabled()) {
            // No trace path or browser flags enter the restricted child environment.
            Environment.append("HC_RHWP_STARTUP_TRACE_CHILD=1");
            Environment.append("HC_RHWP_STARTUP_TRACE_ID="+qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_ID"));
        }
        Environment.sort(Qt::CaseInsensitive); std::wstring Block;
        for(const auto& Item:Environment) { Block+=Item.toStdWString(); Block.push_back(0); } Block.push_back(0);
        STARTUPINFOW Startup{}; Startup.cb=sizeof(Startup); Startup.dwFlags=STARTF_USESHOWWINDOW; Startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION Process{};
        // GPU drivers write logs (e.g. "NVIDIA Corporation/umdlogs") into the working directory.
        // Start in a per-user cache folder so the shared product ROOT stays clean.
        const auto Cache=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const auto WorkingDirectory=!Cache.isEmpty() && QDir().mkpath(Cache+"/rhwp-web") ? QDir::toNativeSeparators(Cache+"/rhwp-web") : Root;
        if(!CreateProcessAsUserW(Token.value_,reinterpret_cast<LPCWSTR>(Program.utf16()),Command.data(),nullptr,nullptr,FALSE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,Block.data(),reinterpret_cast<LPCWSTR>(WorkingDirectory.utf16()),&Startup,&Process)) return Fail("web-owner-create");
        S->process_.value_=Process.hProcess; S->pid_=Process.dwProcessId; Handle Thread; Thread.value_=Process.hThread;
        if(!AssignProcessToJobObject(S->job_.value_,S->process_.value_)) { const auto Code=GetLastError(); TerminateProcess(S->process_.value_,1); SetLastError(Code); return Fail("web-owner-assign"); }
        Setup.End();
        startup::Scope Handshake("helper.launchAndHandshake");
        if(ResumeThread(Thread.value_)==DWORD(-1)) return Fail("web-owner-resume");
        const auto Deadline=GetTickCount64()+Limits.startupMs_;
        Handle Event; Event.value_=CreateEventW(nullptr,TRUE,FALSE,nullptr); if(!Event.Valid()) return Fail("web-owner-event");
        OVERLAPPED Overlap{}; Overlap.hEvent=Event.value_; DWORD Count=0;
        const bool Connected=ConnectNamedPipe(S->pipe_.value_,&Overlap)!=FALSE;
        if(!Connected)
        {
            const auto Code=GetLastError();
            if(Code!=ERROR_PIPE_CONNECTED && (Code!=ERROR_IO_PENDING || !WaitIo(S->pipe_.value_,S->process_.value_,Overlap,Limits.startupMs_,Count))) return Fail("web-owner-connect");
        }
        ULONG Peer=0;
        if(!GetNamedPipeClientProcessId(S->pipe_.value_,&Peer) || Peer!=S->pid_) return Fail("web-owner-peer");
        HostFrame Ready;
        if(!Transfer(S->pipe_.value_,S->process_.value_,&Ready,sizeof(Ready),false,Deadline)
            || !ValidFrame(Ready,S->connection_) || Ready.type_!=0 || Ready.length_ || Ready.requestId_) return Fail("web-owner-ready");
        if(ParentWindow)
        {
            DWORD Owner=0; const auto Window=reinterpret_cast<HWND>(Ready.window_);
            GetWindowThreadProcessId(Window,&Owner);
            if(Owner!=Peer || !IsChild(reinterpret_cast<HWND>(ParentWindow),Window)) return Fail("web-owner-child-window");
        }
        else if(Ready.window_) return Fail("web-owner-unexpected-window");
        S->childWindow_=static_cast<std::uintptr_t>(Ready.window_);
        S->authenticated_=true;
        return std::unique_ptr<DocumentWebProcess>(new DocumentWebProcess(std::move(S)));
    }
    bool DocumentWebProcess::Exchange(const QByteArray& Request,QByteArray& Reply,std::uint32_t TimeoutMs,WebProcessError& Error)
    {
        auto& S=*state_; std::lock_guard Lock(S.mutex_); Error={}; Reply.clear();
        if(!S.authenticated_ || S.closed_) { Error.code_="web-owner-closed"; return false; }
        if(Request.size()>HostPayloadLimit || !TimeoutMs || TimeoutMs>30000) { Error.code_="web-owner-request-budget"; return false; }
        if(S.sequence_==UINT64_MAX) { S.close(); Error.code_="web-owner-sequence-exhausted"; return false; }
        HostFrame Frame; Frame.connection_=S.connection_; Frame.requestId_=++S.sequence_; Frame.length_=std::uint32_t(Request.size());
        const auto Deadline=GetTickCount64()+TimeoutMs; HostFrame Response;
        bool Ok=Transfer(S.pipe_.value_,S.process_.value_,&Frame,sizeof(Frame),true,Deadline)
            && Transfer(S.pipe_.value_,S.process_.value_,const_cast<char*>(Request.constData()),Frame.length_,true,Deadline)
            && Transfer(S.pipe_.value_,S.process_.value_,&Response,sizeof(Response),false,Deadline)
            && ValidFrame(Response,S.connection_) && Response.type_==2 && Response.requestId_==Frame.requestId_ && !Response.window_;
        if(Ok) { Reply.resize(Response.length_); Ok=Transfer(S.pipe_.value_,S.process_.value_,Reply.data(),Response.length_,false,Deadline); }
        if(!Ok) { Error={"web-owner-exchange",GetLastError()}; Reply.clear(); S.close(); }
        return Ok;
    }
    WebProcessSnapshot DocumentWebProcess::Snapshot() const
    {
        auto& S=*state_; std::lock_guard Lock(S.mutex_); WebProcessSnapshot Out; Out.processId_=S.pid_;
        Out.childWindow_=S.authenticated_?S.childWindow_:0;
        Out.authenticated_=S.authenticated_; Out.running_=S.process_.Valid() && WaitForSingleObject(S.process_.value_,0)==WAIT_TIMEOUT;
        if(S.job_.Valid())
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{}; JOBOBJECT_EXTENDED_LIMIT_INFORMATION Usage{};
            if(QueryInformationJobObject(S.job_.value_,JobObjectBasicAccountingInformation,&Accounting,sizeof(Accounting),nullptr)) Out.activeProcesses_=Accounting.ActiveProcesses;
            if(QueryInformationJobObject(S.job_.value_,JobObjectExtendedLimitInformation,&Usage,sizeof(Usage),nullptr)) Out.peakJobBytes_=Usage.PeakJobMemoryUsed;
        }
        return Out;
    }
    bool DocumentWebProcess::Close(std::uint32_t TimeoutMs) { auto& S=*state_; std::lock_guard Lock(S.mutex_); return S.close(TimeoutMs); }
}
