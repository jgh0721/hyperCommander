#include "stdafx.h"
#include "sourceWorkerHost.hpp"
#include "controllerMemory.hpp"
#include "semantic.hpp"
#include "workerFrames.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
#include <vector>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        constexpr std::uint64_t mib = 1024 * 1024;
        // Process, frames and one transient response beyond the retained trees. The job
        // limit is the cap; the controller reservation follows the typical estimate.
        constexpr std::uint64_t residentBase = 64 * mib, residentCap = 512 * mib;
        QString sha256(const QByteArray& Bytes) { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
        class ResidentHost final
        {
        public:
            ~ResidentHost() { std::lock_guard lock(request_); stop(false); }
            // Ids are cheap; the document entry appears with its first analysis.
            std::uint64_t Register() { return nextDocument_++; }
            // Closing a window must not wait for another window's analysis: a busy
            // host closes the document when that request ends.
            void Release(std::uint64_t Document)
            {
                { std::lock_guard lock(pendingMutex_); pending_.push_back(Document); }
                if (request_.try_lock()) { flushReleases(); request_.unlock(); }
            }
            // Cached answers must not wait for another window's analysis.
            void CountCached() { ++cached_; }
            SourceWorkerStats Stats() { std::lock_guard lock(request_); auto value = stats_; value.cached_ = cached_; return value; }
            void Kill() { std::lock_guard lock(request_); if (process_) { TerminateJobObject(job_, 1); WaitForSingleObject(process_, 2000); } }
            void Fail(int Requests) { std::lock_guard lock(request_); failRequests_ = Requests; }
            bool Running() { std::lock_guard lock(request_); return process_ != nullptr; }
            QJsonObject Analyze(std::uint64_t Document, const QByteArray& Text, const QString& Language, const Budget& Limits)
            {
                const auto unavailable = [&](const QString& Reason) { return QJsonObject{{"status", "Unavailable"}, {"reason", Reason}, {"language", Language}}; };
                const auto stopped = [&]() -> QString {
                    if (Limits.canceled_ && Limits.canceled_->load()) return "Canceled";
                    if (std::chrono::steady_clock::now() >= Limits.deadline_) return "TimeLimit";
                    try { if (Limits.memoryCheck_) Limits.memoryCheck_(); }
                    catch (const Failure& error) { return QString::fromStdString(error.code_); }
                    return {};
                };
                std::unique_lock lock(request_, std::defer_lock);
                while (!lock.try_lock_for(std::chrono::milliseconds(20))) if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
                struct Flush { ResidentHost& host_; ~Flush() { host_.flushReleases(); } } flush{*this};
                if (const auto reason = stopped(); !reason.isEmpty()) return unavailable(reason);
                auto& document = documents_[Document]; const auto digest = sha256(Text);
                if (crashes_[digest] >= 2) return unavailable("WorkerCrashRepeated");
                // A worker that ended between requests (test kill, job limit) restarts quietly.
                if (process_ && broken()) stop(false);
                document.used_ = ++tick_;
                const auto needed = [&] {
                    auto bytes = residentBase + SourceTreeMemory(Language, Text.size());
                    for (const auto& [id, other] : documents_) if (id != Document && other.open_) bytes += SourceTreeMemory(other.language_, other.text_.size());
                    return bytes;
                };
                // Least recently analyzed documents of any window give way first.
                while (needed() > residentCap)
                {
                    auto victim = documents_.end();
                    for (auto it = documents_.begin(); it != documents_.end(); ++it)
                        if (it->first != Document && it->second.open_ && (victim == documents_.end() || it->second.used_ < victim->second.used_)) victim = it;
                    if (victim == documents_.end()) return unavailable("InputLimit");
                    if (process_) WriteWorkerFrame(input_, {{"type", "close"}, {"doc", qint64(victim->first)}});
                    victim->second.open_ = false; victim->second.text_.clear();
                }
                if (!process_) { if (const auto error = start(needed(), Limits); !error.isEmpty()) return unavailable(error); }
                else if (needed() > limit_ && !grow(needed())) return unavailable("WorkerMemoryCapacity");
                for (bool resent = false;;)
                {
                    if (!send(Document, document, Text, Language, digest)) return failed(digest, unavailable);
                    const auto request = qint64(nextRequest_++);
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Limits.deadline_ - std::chrono::steady_clock::now()).count();
                    // The worker stops before the host deadline and reports TimeLimit itself.
                    const auto deadline = int(std::clamp<qint64>(remaining - 100, 1, 30000));
                    if (!WriteWorkerFrame(input_, {{"type", "analyze"}, {"requestId", request}, {"doc", qint64(Document)}, {"revision", document.revision_}, {"deadlineMs", deadline}}))
                        return failed(digest, unavailable);
                    if (failRequests_ > 0) { --failRequests_; TerminateJobObject(job_, 1); }
                    for (;;)
                    {
                        WorkerFrame frame; QString reason;
                        const auto wait = waitFrame(frame, [&] { reason = stopped(); return !reason.isEmpty(); });
                        if (wait == Wait::Broken) return failed(digest, unavailable);
                        if (wait == Wait::Stopped)
                        {
                            // Cooperative stop first; an unresponsive worker is reclaimed within 1 s.
                            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                            bool answered = WriteWorkerFrame(input_, {{"type", "cancel"}, {"requestId", request}});
                            while (answered)
                            {
                                const auto next = waitFrame(frame, [&] { return std::chrono::steady_clock::now() >= until; });
                                if (next != Wait::Frame) { answered = false; break; }
                                if (frame.header_["requestId"].toInteger(-1) == request) break;
                            }
                            if (!answered) stop(false);
                            return unavailable(reason);
                        }
                        if (frame.header_["requestId"].toInteger(-1) != request) continue;
                        if (frame.header_["type"] == QJsonValue("resync"))
                        {
                            ++stats_.resyncs_; document.open_ = false;
                            if (resent) { stop(false); return unavailable("WorkerProtocol"); }
                            resent = true; break;
                        }
                        if (frame.header_["type"] != QJsonValue("analyzed")) { stop(false); return unavailable("WorkerProtocol"); }
                        QJsonParseError error; auto result = QJsonDocument::fromJson(frame.body_, &error).object();
                        if (error.error != QJsonParseError::NoError) { stop(false); return unavailable("WorkerProtocol"); }
                        if (result["status"] == QJsonValue("Unavailable")) { result["language"] = Language; return result; }
                        if (!ValidateFunctionList(result, Language, Text.size())) return unavailable("WorkerProtocol");
                        ++stats_.analyses_; stats_.reused_ += std::uint64_t(frame.header_["reused"].toInteger());
                        result["engine"] = "source.tree-sitter"; result["workerMemoryLimitBytes"] = double(limit_);
                        return result;
                    }
                }
            }
        private:
            struct Document { bool open_ = false; QByteArray text_; QString language_; qint64 revision_ = 0; std::uint64_t used_ = 0; };
            enum class Wait { Frame, Stopped, Broken };
            template<class Unavailable> QJsonObject failed(const QString& Digest, Unavailable& Make)
            { ++crashes_[Digest]; ++stats_.failures_; stop(false); return Make("WorkerFailure"); }
            // Brings the worker's copy to Text: unchanged, one replaced range, or a full open.
            bool send(std::uint64_t Id, Document& Value, const QByteArray& Text, const QString& Language, const QString& Digest)
            {
                if (Value.open_ && Value.language_ == Language && Value.text_ == Text) return true;
                if (Value.open_ && Value.language_ == Language)
                {
                    const auto& old = Value.text_; const auto shared = std::min(old.size(), Text.size());
                    const auto prefix = std::mismatch(old.cbegin(), old.cbegin() + shared, Text.cbegin()).first - old.cbegin();
                    qsizetype suffix = 0;
                    while (suffix < shared - prefix && old[old.size() - 1 - suffix] == Text[Text.size() - 1 - suffix]) ++suffix;
                    const auto replaced = std::max(old.size(), Text.size()) - prefix - suffix;
                    // A mostly rewritten document parses as fast from scratch.
                    if (replaced <= Text.size() / 2)
                    {
                        const auto base = Value.revision_;
                        if (!WriteWorkerFrame(input_, {{"type", "edit"}, {"doc", qint64(Id)}, {"baseRevision", base}, {"revision", base + 1},
                            {"begin", qint64(prefix)}, {"end", qint64(old.size() - suffix)}, {"digest", Digest}}, Text.mid(prefix, Text.size() - suffix - prefix))) return false;
                        Value.revision_ = base + 1; Value.text_ = Text; ++stats_.edits_; return true;
                    }
                }
                if (!WriteWorkerFrame(input_, {{"type", "open"}, {"doc", qint64(Id)}, {"revision", Value.revision_ + 1}, {"language", Language}, {"digest", Digest}}, Text)) return false;
                ++Value.revision_; Value.open_ = true; Value.language_ = Language; Value.text_ = Text; ++stats_.opens_; return true;
            }
            Wait waitFrame(WorkerFrame& Frame, const std::function<bool()>& Stop)
            {
                std::unique_lock lock(inboxMutex_);
                for (;;)
                {
                    if (!inbox_.empty()) { Frame = std::move(inbox_.front()); inbox_.pop_front(); return Wait::Frame; }
                    if (closed_) return Wait::Broken;
                    if (Stop()) return Wait::Stopped;
                    inboxReady_.wait_for(lock, std::chrono::milliseconds(20));
                }
            }
            bool broken() { std::lock_guard lock(inboxMutex_); return closed_ || WaitForSingleObject(process_, 0) == WAIT_OBJECT_0; }
            QString start(std::uint64_t Memory, const Budget& Limits)
            {
                auto memory = ProcessControllerMemory()->TryWorker(Memory, Memory, 32 * mib);
                if (!memory.lease_) return "WorkerMemoryCapacity";
                SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
                HANDLE childInput = nullptr, childOutput = nullptr;
                if (!CreatePipe(&childInput, &input_, &inherit, 1 << 20)) return "WorkerStart";
                if (!CreatePipe(&output_, &childOutput, &inherit, 1 << 20)) { CloseHandle(childInput); CloseHandle(input_); input_ = nullptr; return "WorkerStart"; }
                SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
                const auto nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
                // Only the three standard handles are inherited.
                SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                std::vector<char> list(size); auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
                HANDLE handles[]{childInput, childOutput, nul};
                const bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &size) &&
                    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
                STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startup.StartupInfo.hStdInput = childInput; startup.StartupInfo.hStdOutput = childOutput; startup.StartupInfo.hStdError = nul; startup.lpAttributeList = attributes;
                auto command = QString("\"%1\" -internal-worker-resident").arg(QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/HyperCompareCli.exe")).toStdWString();
                PROCESS_INFORMATION info{};
                // Suspended until the job holds it, so no child code runs without the memory limit.
                const bool created = listed && nul != INVALID_HANDLE_VALUE && CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
                if (listed) DeleteProcThreadAttributeList(attributes);
                CloseHandle(childInput); CloseHandle(childOutput); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
                if (!created) { CloseHandle(input_); CloseHandle(output_); input_ = output_ = nullptr; return "WorkerStart"; }
                process_ = info.hProcess; job_ = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
                limits.ProcessMemoryLimit = residentCap; limits.BasicLimitInformation.ActiveProcessLimit = 1;
                if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) || !AssignProcessToJobObject(job_, process_))
                { TerminateProcess(process_, 1); CloseHandle(info.hThread); stop(false); return "WorkerIsolation"; }
                ResumeThread(info.hThread); CloseHandle(info.hThread);
                memory_ = std::move(memory); limit_ = memory_.bytes_; closed_ = false; inbox_.clear();
                reader_ = std::thread([this, output = output_] {
                    for (;;)
                    {
                        WorkerFrame frame; const bool read = ReadWorkerFrame(output, frame);
                        std::lock_guard lock(inboxMutex_);
                        if (!read) { closed_ = true; inboxReady_.notify_all(); return; }
                        inbox_.push_back(std::move(frame)); inboxReady_.notify_all();
                    }
                });
                const auto nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                WorkerFrame ready;
                if (!WriteWorkerFrame(input_, {{"type", "hello"}, {"schema", 1}, {"nonce", nonce}}) ||
                    waitFrame(ready, [&] { return std::chrono::steady_clock::now() >= until || (Limits.canceled_ && Limits.canceled_->load()); }) != Wait::Frame ||
                    ready.header_["type"] != QJsonValue("ready") || ready.header_["nonce"] != QJsonValue(nonce))
                { stop(false); return "WorkerStart"; }
                ++stats_.starts_; return {};
            }
            bool grow(std::uint64_t Memory)
            {
                auto memory = ProcessControllerMemory()->TryWorker(Memory, Memory, 32 * mib);
                if (!memory.lease_) return false;
                // The reservation only grows while the worker runs; it is returned when the worker exits.
                memory_ = std::move(memory); limit_ = memory_.bytes_; return true;
            }
            void stop(bool Graceful)
            {
                if (!process_ && !input_) return;
                if (Graceful && input_) WriteWorkerFrame(input_, {{"type", "shutdown"}});
                if (input_) CloseHandle(input_); input_ = nullptr;
                if (process_ && WaitForSingleObject(process_, Graceful ? 1000 : 0) != WAIT_OBJECT_0)
                { if (job_) TerminateJobObject(job_, 1); else TerminateProcess(process_, 1); WaitForSingleObject(process_, 2000); }
                // The child's stdout closes with it, which ends the reader's blocking read.
                if (reader_.joinable()) reader_.join();
                for (auto* handle : {output_, process_, job_}) if (handle) CloseHandle(handle);
                output_ = process_ = job_ = nullptr; memory_ = {}; limit_ = 0;
                { std::lock_guard lock(inboxMutex_); inbox_.clear(); closed_ = false; }
                for (auto& [id, document] : documents_) { document.open_ = false; document.text_.clear(); }
            }
            void flushReleases()
            {
                std::vector<std::uint64_t> released;
                { std::lock_guard lock(pendingMutex_); released.swap(pending_); }
                for (const auto id : released)
                {
                    const auto at = documents_.find(id); if (at == documents_.end()) continue;
                    if (at->second.open_ && process_) WriteWorkerFrame(input_, {{"type", "close"}, {"doc", qint64(id)}});
                    documents_.erase(at);
                }
                // The worker and its memory lease exist only while a window holds documents.
                if (documents_.empty()) stop(true);
            }
            std::timed_mutex request_;
            std::mutex pendingMutex_; std::vector<std::uint64_t> pending_;
            std::map<std::uint64_t, Document> documents_;
            std::map<QString, int> crashes_;
            std::atomic<std::uint64_t> nextDocument_{1}, cached_{0};
            std::uint64_t nextRequest_ = 1, tick_ = 0, limit_ = 0;
            int failRequests_ = 0;
            SourceWorkerStats stats_;
            HANDLE process_ = nullptr, job_ = nullptr, input_ = nullptr, output_ = nullptr;
            ControllerMemory::WorkerReservation memory_;
            std::thread reader_;
            std::mutex inboxMutex_; std::condition_variable inboxReady_; std::deque<WorkerFrame> inbox_; bool closed_ = false;
        };
        ResidentHost& host() { static ResidentHost value; return value; }
    }
    SourceDocuments::~SourceDocuments() { for (auto& entry : entries_) if (entry.id_) host().Release(entry.id_); }
    QJsonObject SourceDocuments::Analyze(int Input, const CompareLanguage& Language, const QByteArray& Utf8, const Budget& Limits)
    {
        if (!Language.structure_) return {{"status", "Unsupported"}, {"reason", "Language"}};
        if (Input < 0 || Input >= int(entries_.size())) throw Failure("InvalidSourceInput");
        std::lock_guard lock(mutex_); auto& entry = entries_[std::size_t(Input)];
        if (!entry.result_.isEmpty() && entry.language_ == Language.id_ && entry.text_ == Utf8) { host().CountCached(); return entry.result_; }
        QJsonObject result;
        // A document the resident worker could only hold by itself runs one-shot instead.
        if (Utf8.size() > residentSourceLimit || residentBase + SourceTreeMemory(Language.id_, Utf8.size()) > residentCap)
        {
            if (entry.id_) { host().Release(entry.id_); entry.id_ = 0; }
            result = AnalyzeFunctionsAs(Language, Utf8, Limits);
        }
        else
        {
            if (!entry.id_) entry.id_ = host().Register();
            result = host().Analyze(entry.id_, Utf8, Language.id_, Limits);
        }
        const bool keep = result["status"] != QJsonValue("Unavailable");
        entry.text_ = keep ? Utf8 : QByteArray{}; entry.language_ = Language.id_; entry.result_ = keep ? result : QJsonObject{};
        return result;
    }
    SourceWorkerStats ResidentSourceWorkerStats() { return host().Stats(); }
    void KillResidentSourceWorkerForTesting() { host().Kill(); }
    void FailResidentSourceRequestsForTesting(int Requests) { host().Fail(Requests); }
    bool ResidentSourceWorkerRunningForTesting() { return host().Running(); }
}
