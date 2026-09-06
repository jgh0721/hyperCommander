#include "hostedArchiveBackend.hpp"
#include "archiveCodec.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QScopeGuard>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid() { return {ErrorCategory::InvalidRequest, "host-archive-response", {"wcx"}}; }
        OperationContext CleanupContext() { OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250); return Context; }
        class Listing final : public IArchiveListing
        {
        public:
            Listing(std::shared_ptr<PluginProcess> Process, std::uint64_t Id) : process_(std::move(Process)), id_(Id) {}
            ~Listing() override { if (!end_) { auto Context = CleanupContext(); process_->ArchiveCall(WireKind::ArchiveRelease, {{"cursor", QString::number(id_)}, {"context", EncodeContext(Context)}}, Context); } }
            Result<ArchivePage> Next(std::size_t Count, OperationContext Context) override
            {
                if (!Count || !Context.maxPageItems_) return Error{ErrorCategory::InvalidRequest, "zero-page-size", {"wcx"}};
                if (end_) return ArchivePage{{}, true};
                auto Result = process_->ArchiveCall(WireKind::ArchiveNext, {{"cursor", QString::number(id_)}, {"count", QString::number(std::min({Count, Context.maxPageItems_, std::size_t{128}}))}, {"context", EncodeContext(Context)}}, Context);
                if (!Result.HasValue()) return Result.Failure(); const auto& Object = Result.Value();
                if (Object.size() != 2 || !Object["entries"].isArray() || !Object["end"].isBool() || Object["entries"].toArray().size() > 128) return Invalid();
                ArchivePage Page; Page.end_ = Object["end"].toBool();
                for (const auto Value : Object["entries"].toArray()) { if (!Value.isObject()) return Invalid(); auto Header = DecodeArchiveHeader(Value.toObject()); if (!Header.HasValue()) return Header.Failure(); Page.entries_.push_back(std::move(Header.Value())); }
                if (Page.entries_.empty() && !Page.end_) return Invalid(); end_ = Page.end_; return Page;
            }
        private:
            std::shared_ptr<PluginProcess> process_;
            std::uint64_t id_;
            bool end_ = false;
        };
        class Backend final : public IArchiveBackend
        {
        public:
            explicit Backend(std::shared_ptr<PluginProcess> Process) : process_(std::move(Process)) {}
            Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Archive, bool Test, OperationContext Context) override
            {
                if (!process_) return Invalid();
                auto Result = process_->ArchiveCall(WireKind::ArchiveOpen, {{"archive", QString::fromStdString(Archive)}, {"test", Test}, {"context", EncodeContext(Context)}}, Context);
                if (!Result.HasValue()) return Result.Failure(); if (Result.Value().size() != 1) return Invalid(); auto Id = WireUnsigned(Result.Value()["cursor"]);
                if (!Id.HasValue() || !Id.Value()) return Invalid(); return std::unique_ptr<IArchiveListing>(new Listing(process_, Id.Value()));
            }
            Result<Unit> Extract(std::string Archive, ArchiveHeader Expected, std::string Output, OperationContext Context) override
            {
                if (!process_) return Invalid();
                auto Result = process_->ArchiveCall(WireKind::ArchiveExtract, {{"archive", QString::fromStdString(Archive)}, {"expected", EncodeArchiveHeader(Expected)}, {"output", QString::fromStdString(Output)}, {"context", EncodeContext(Context)}}, Context);
                if (!Result.HasValue()) return Result.Failure(); return Result.Value().size() == 1 && Result.Value()["complete"].toBool() ? storage::Result<Unit>(Unit{}) : storage::Result<Unit>(Invalid());
            }
            Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override
            {
                if (!process_) return Invalid();
                const auto Bytes = QJsonDocument(EncodeArchiveUpdate(Request)).toJson(QJsonDocument::Compact);
                if (Bytes.size() > 128 * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "host-archive-update-budget", {"wcx"}};
                auto Begun = process_->ArchiveCall(WireKind::ArchiveUpdateBegin, {{"size", QString::number(Bytes.size())}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex())}, {"context", EncodeContext(Context)}}, Context);
                if (!Begun.HasValue()) return Begun.Failure(); auto Id = WireUnsigned(Begun.Value()["upload"]); if (!Id.HasValue() || !Id.Value()) return Invalid();
                auto Cleanup = qScopeGuard([&] { auto CleanContext = CleanupContext(); process_->ArchiveCall(WireKind::ArchiveRelease, {{"upload", QString::number(Id.Value())}, {"context", EncodeContext(CleanContext)}}, CleanContext); });
                for (qsizetype Offset = 0; Offset < Bytes.size(); Offset += 48 * 1024)
                {
                    auto Written = process_->ArchiveCall(WireKind::ArchiveUpdateData, {{"upload", QString::number(Id.Value())}, {"offset", QString::number(Offset)},
                        {"data", QString::fromLatin1(Bytes.mid(Offset, 48 * 1024).toBase64())}, {"context", EncodeContext(Context)}}, Context);
                    if (!Written.HasValue()) return Written.Failure(); auto Received = WireUnsigned(Written.Value()["received"]);
                    if (!Received.HasValue() || Received.Value() != std::uint64_t(std::min(Offset + 48 * 1024, Bytes.size()))) return Invalid();
                }
                auto Applied = process_->ArchiveCall(WireKind::ArchiveUpdateApply, {{"upload", QString::number(Id.Value())}, {"context", EncodeContext(Context)}}, Context);
                if (!Applied.HasValue()) return Applied.Failure(); Cleanup.dismiss();
                return Applied.Value().size() == 1 && Applied.Value()["complete"].toBool() ? Result<Unit>(Unit{}) : Result<Unit>(Invalid());
            }
        private:
            std::shared_ptr<PluginProcess> process_;
        };
    }
    std::shared_ptr<IArchiveBackend> MakeHostedArchiveBackend(std::shared_ptr<PluginProcess> Process) { return std::make_shared<Backend>(std::move(Process)); }
}
