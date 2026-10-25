#include "stdafx.h"
#include "batch.hpp"
#include "controllerMemory.hpp"
#include "requestFile.hpp"
#include "decisionPlan.hpp"
#include "directoryPlan.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <condition_variable>
#include <future>
#include <thread>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        struct Job { QString id_; Request request_; };
        struct Access { QString path_, identity_; int job_ = -1; bool write_ = false; };
        int rank(int Code) { return Code == 2 ? 3 : Code == 3 ? 2 : Code == 1 ? 1 : 0; }
        void keys(const QJsonObject& Value, const QStringList& Allowed)
        { for (auto it = Value.begin(); it != Value.end(); ++it) if (!Allowed.contains(it.key())) throw Failure("UnknownBatchKey"); }
        std::vector<Job> prepare(const Request& Input, const Budget& Limits)
        {
            const auto manifest = CheckedDirectoryPath(Input.manifest_); const auto snapshot = ReadFileSnapshot(manifest, 4 * 1024 * 1024, Limits);
            const auto json = ReadCompareJson(snapshot.raw_, Limits); keys(json, {"schemaVersion", "jobs"});
            if (json["schemaVersion"] != 1 || !json["jobs"].isArray()) throw Failure("InvalidBatchSchema");
            const auto jobs = json["jobs"].toArray(); if (jobs.isEmpty() || jobs.size() > 1000) throw Failure("BatchJobLimit");
            QSet<QString> ids; std::vector<Job> result; QVector<Access> accesses;
            const auto add = [&](const QString& Path, int Job, bool Write) {
                if (Path.isEmpty() || Path == "-") return; Limits.Check(); Access item{CheckedDirectoryPath(Path), {}, Job, Write};
                if (QFileInfo::exists(item.path_))
                {
                    storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true;
                    const storage::Locator locator{item.path_.startsWith("//") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, item.path_.toUtf8().toStdString()};
                    const auto entry = storage::MakeNativeReadProvider(locator.kind_)->Resolve(locator, ctx).get(); if (!entry.HasValue()) throw Failure(entry.Failure().code_);
                    if (entry.Value().id_.revision_) { const auto token = QString::fromStdString(entry.Value().id_.revision_->token_); const auto at = token.indexOf(":identity:"); if (at >= 0) item.identity_ = token.mid(at); }
                }
                for (const auto& previous : accesses)
                    if ((Write || previous.write_) && (Job < 0 || previous.job_ < 0 || previous.job_ != Job) &&
                        (DirectoryPathsOverlap(item.path_, previous.path_) || (!item.identity_.isEmpty() && item.identity_ == previous.identity_))) throw Failure("BatchPathConflict");
                accesses.append(std::move(item));
            };
            add(manifest, -1, false); add(Input.report_, -1, true);
            const QDir root(QFileInfo(manifest).absolutePath());
            for (int i = 0; i < jobs.size(); ++i)
            {
                Limits.Check(); if (!jobs[i].isObject()) throw Failure("InvalidBatchJob"); const auto value = jobs[i].toObject(); keys(value, {"jobId", "request"});
                const auto id = value["jobId"].toString(), path = value["request"].toString();
                if (id.isEmpty() || id.size() > 128 || ids.contains(id) || path.isEmpty() || path.size() > 32767) throw Failure("InvalidBatchJob"); ids.insert(id);
                const auto requestPath = QDir::cleanPath(root.absoluteFilePath(path)); auto parsed = ParseRequestFile(requestPath, {"-headless", "-format", "json"}, true, Limits);
                if (!parsed.error_.isEmpty()) throw Failure(("BatchRequest:" + id + ':' + parsed.error_).toStdString()); auto request = parsed.request_;
                if (request.gui_ || request.help_ || request.version_ || request.mode_ == "capabilities" || request.mode_ == "batch" || request.mode_ == "recover") throw Failure("BatchModeUnsupported");
                request.protectedInputs_ << manifest; add(requestPath, -1, false);
                for (const auto& read : {request.file1_, request.file2_, request.file3_, request.base_, request.decisions_, request.applyPlan_}) add(read, i, false);
                for (const auto& write : {request.output_, request.planOut_}) add(write, i, true);
                if (request.mode_ == "apply")
                {
                    const auto bytes = ReadFileSnapshot(request.applyPlan_, directoryPlanLimit, Limits).raw_; const auto plan = ReadCompareJson(bytes, Limits);
                    request.applyPlanDigest_ = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(plan).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
                    const auto output = plan["output"].toObject()["path"].toString(); if (output.isEmpty() || !plan["roots"].isArray()) throw Failure("InvalidBatchApplyPlan");
                    add(output, i, true); for (const auto& item : plan["roots"].toArray()) { const auto path = item.toObject()["path"].toString(); if (path.isEmpty()) throw Failure("InvalidBatchApplyPlan"); add(path, i, false); }
                    add(request.journal_.isEmpty() ? request.applyPlan_ + ".journal" : request.journal_, i, true);
                }
                result.push_back({id, std::move(request)});
            }
            return result;
        }
    }
    int RunBatch(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled, const BatchSink& Sink)
    {
        if (Input.jobs_ < 1 || Input.jobs_ > 2) throw Failure("BatchConcurrencyLimit");
        Budget limits; limits.canceled_ = Canceled.get(); limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Input.timeoutMs_);
        ConfigureMemoryBudget(limits, Input.memoryMiB_);
        auto jobs = prepare(Input, limits); struct Running { int index_ = -1; std::future<Result> future_; }; std::vector<Running> active;
        int next = 0, code = 0, completed = 0, skipped = 0; std::array<int, 4> counts{}; bool failed = false, delivery = true;
        const auto send = [&](QJsonObject Event) { Event["schemaVersion"] = 1; if (delivery && !Sink(Event)) { delivery = false; Canceled->store(true); } return delivery; };
        const auto terminal = [&](int Index, Result Value, bool Skipped) {
            if (rank(Value.exitCode_) > rank(code)) code = Value.exitCode_; ++counts[Value.exitCode_]; ++completed; skipped += Skipped; failed |= Value.exitCode_ == 2;
            send({{"event", "result"}, {"jobId", jobs[Index].id_}, {"skipped", Skipped}, {"result", Value.Json(Input.includeContent_)}});
            send({{"event", "progress"}, {"completed", completed}, {"total", qint64(jobs.size())}});
        };
        while (next < int(jobs.size()) || !active.empty())
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(limits.deadline_ - std::chrono::steady_clock::now()).count();
            while (delivery && !Canceled->load() && remaining > 0 && !(Input.failFast_ && failed) && next < int(jobs.size()) && active.size() < std::size_t(Input.jobs_))
            {
                const int index = next++; if (!send({{"event", "started"}, {"jobId", jobs[index].id_}})) { --next; break; }
                const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(limits.deadline_ - std::chrono::steady_clock::now()).count();
                if (available <= 0) { --next; break; }
                auto request = jobs[index].request_; request.memoryMiB_ = std::min(request.memoryMiB_, Input.memoryMiB_); request.timeoutMs_ = int(std::min<qint64>(request.timeoutMs_, available));
                active.push_back({index, std::async(std::launch::async, [request, Canceled] { return Execute(request, Canceled, true); })});
            }
            bool ready = false;
            for (auto it = active.begin(); it != active.end();)
            {
                if (it->future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++it; continue; }
                Result result; try { result = it->future_.get(); } catch (const std::exception& error) { result.error_ = QString::fromUtf8(error.what()); }
                terminal(it->index_, std::move(result), false); it = active.erase(it); ready = true;
            }
            if (active.empty() && (Canceled->load() || remaining <= 0 || (Input.failFast_ && failed) || !delivery))
            {
                while (next < int(jobs.size()))
                {
                    Result result; result.requestId_ = jobs[next].request_.requestId_; result.operation_ = jobs[next].request_.mode_;
                    result.exitCode_ = Canceled->load() && delivery ? 3 : 2; result.completion_ = "NotStarted";
                    result.error_ = !delivery ? "BatchDeliveryFailed" : Canceled->load() ? "Canceled" : remaining <= 0 ? "Timeout" : "FailFastNotStarted";
                    terminal(next++, std::move(result), true);
                }
            }
            if (!ready && !active.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        send({{"event", "batchSummary"}, {"exitCode", code}, {"total", qint64(jobs.size())}, {"completed", completed}, {"skipped", skipped},
            {"success", counts[0]}, {"different", counts[1]}, {"errors", counts[2]}, {"canceled", counts[3]}, {"jobs", Input.jobs_}});
        return delivery ? code : 2;
    }
    int RunBatchAndReport(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled)
    {
        QFile standard; std::unique_ptr<QSaveFile> report; QIODevice* output = nullptr; bool summaryDelivered = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Input.timeoutMs_);
        try
        {
            const auto sink = [&](const QJsonObject& Event) {
                if (!output)
                {
                    if (Input.report_.isEmpty() || Input.report_ == "-") { if (!standard.open(stdout, QIODevice::WriteOnly)) return false; output = &standard; }
                    else { report = std::make_unique<QSaveFile>(Input.report_); report->setDirectWriteFallback(false); if (!report->open(QIODevice::WriteOnly)) return false; output = report.get(); }
                }
                const auto bytes = QJsonDocument(Event).toJson(QJsonDocument::Compact) + '\n'; if (bytes.size() > 64 * 1024 * 1024) return false;
                // One bounded write in flight. A stalled pipe never leaves an unbounded event queue.
                std::mutex mutex; std::condition_variable condition; bool done = false, success = false;
                std::thread writer([&] {
                    const bool written = output->write(bytes) == bytes.size() && (report ? report->flush() : standard.flush());
                    { std::lock_guard lock(mutex); success = written; done = true; } condition.notify_one();
                });
                bool interrupted = false;
                { std::unique_lock lock(mutex); while (!done)
                    {
                        condition.wait_for(lock, std::chrono::milliseconds(20));
                        if (!done && (Canceled->load() || std::chrono::steady_clock::now() >= deadline)) { interrupted = true; CancelSynchronousIo(writer.native_handle()); }
                    }
                }
                writer.join(); const bool delivered = success && !interrupted; if (delivered && Event["event"] == "batchSummary") summaryDelivered = true; return delivered;
            };
            const int result = RunBatch(Input, Canceled, sink); if (!summaryDelivered) return 2; if (report && !report->commit()) return 2; return result;
        }
        catch (const std::exception& error)
        { QFile diagnostic; if (diagnostic.open(stderr, QIODevice::WriteOnly)) { diagnostic.write(error.what()); diagnostic.write("\n"); } return Canceled->load() ? 3 : 2; }
    }
}
