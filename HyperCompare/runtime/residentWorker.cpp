#include "stdafx.h"
#include "workerHost.hpp"
#include "workerFrames.hpp"
#include "sourceTree.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        QString sha256(const QByteArray& Bytes) { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
        struct Inbox
        {
            std::mutex mutex_; std::condition_variable ready_;
            std::deque<WorkerFrame> frames_; bool closed_ = false;
            // Cancels are applied by the reader so a running analysis observes them.
            std::set<qint64> canceled_; qint64 current_ = -1; std::atomic_bool cancelCurrent_{false};
        };
        struct Document { std::unique_ptr<SourceTree> tree_; qint64 revision_ = 0; };
    }
    int ResidentWorkerMain()
    {
        const auto input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
        // Leaked on purpose: the reader may still block on stdin while the process exits.
        auto* inbox = new Inbox;
        std::thread([inbox, input] {
            for (;;)
            {
                WorkerFrame frame; const bool read = ReadWorkerFrame(input, frame);
                std::lock_guard lock(inbox->mutex_);
                if (!read) { inbox->closed_ = true; inbox->ready_.notify_all(); return; }
                if (frame.header_["type"] == QJsonValue("cancel"))
                {
                    const auto id = frame.header_["requestId"].toInteger(-1);
                    if (id == inbox->current_) inbox->cancelCurrent_ = true; else inbox->canceled_.insert(id);
                    continue;
                }
                inbox->frames_.push_back(std::move(frame)); inbox->ready_.notify_all();
            }
        }).detach();
        const auto next = [inbox](WorkerFrame& Frame) {
            std::unique_lock lock(inbox->mutex_);
            inbox->ready_.wait(lock, [inbox] { return !inbox->frames_.empty() || inbox->closed_; });
            if (inbox->frames_.empty()) return false;
            Frame = std::move(inbox->frames_.front()); inbox->frames_.pop_front(); return true;
        };
        WorkerFrame frame;
        if (!next(frame) || frame.header_["type"] != QJsonValue("hello") || frame.header_["schema"].toInt() != 1 || frame.header_["nonce"].toString().size() != 36) return 2;
        if (!WriteWorkerFrame(output, {{"type", "ready"}, {"schema", 1}, {"nonce", frame.header_["nonce"]}, {"parserVersion", sourceParserVersion}})) return 2;
        std::map<qint64, Document> documents;
        while (next(frame))
        {
            const auto& header = frame.header_; const auto type = header["type"].toString(); const auto id = header["doc"].toInteger(-1);
            if (type == "shutdown") return 0;
            if (type == "close") { documents.erase(id); continue; }
            if (type == "open")
            {
                // A rejected open leaves no document; the next analyze asks the host to resend.
                documents.erase(id);
                const auto language = header["language"].toString();
                if ((language != "c" && language != "cpp" && language != "python") || header["digest"].toString() != sha256(frame.body_)) continue;
                Document document; document.tree_ = std::make_unique<SourceTree>(language); document.tree_->Reset(std::move(frame.body_));
                document.revision_ = header["revision"].toInteger(-1); documents.emplace(id, std::move(document)); continue;
            }
            if (type == "edit")
            {
                const auto at = documents.find(id);
                if (at == documents.end()) continue;
                try
                {
                    if (at->second.revision_ != header["baseRevision"].toInteger(-1)) throw Failure("SourceRevision");
                    at->second.tree_->Edit(header["begin"].toInteger(-1), header["end"].toInteger(-1), frame.body_);
                    if (header["digest"].toString() != sha256(at->second.tree_->Text())) throw Failure("SourceDigest");
                    at->second.revision_ = header["revision"].toInteger(-1);
                }
                catch (const Failure&) { documents.erase(at); }
                continue;
            }
            if (type != "analyze") return 2;
            const auto request = header["requestId"].toInteger(-1); const auto revision = header["revision"].toInteger(-1);
            const auto at = documents.find(id);
            if (at == documents.end() || at->second.revision_ != revision)
            {
                if (!WriteWorkerFrame(output, {{"type", "resync"}, {"requestId", request}, {"doc", id}})) return 2;
                continue;
            }
            {
                std::lock_guard lock(inbox->mutex_);
                inbox->current_ = request; inbox->cancelCurrent_ = inbox->canceled_.erase(request) > 0;
            }
            QJsonObject result; const auto deadline = std::clamp(header["deadlineMs"].toInt(1500), 1, 30000);
            try
            {
                result = at->second.tree_->Analyze(std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline), &inbox->cancelCurrent_);
                result["language"] = at->second.tree_->Language(); result["parserVersion"] = sourceParserVersion;
            }
            catch (const Failure& error) { result = {{"status", "Unavailable"}, {"reason", QString::fromStdString(error.code_)}}; }
            { std::lock_guard lock(inbox->mutex_); inbox->current_ = -1; }
            if (!WriteWorkerFrame(output, {{"type", "analyzed"}, {"requestId", request}, {"doc", id}, {"revision", revision},
                {"reused", qint64(at->second.tree_->Reused())}}, QJsonDocument(result).toJson(QJsonDocument::Compact))) return 2;
        }
        return 0;
    }
}
