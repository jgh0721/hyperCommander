#include "documentHostBroker.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QCryptographicHash>
#include <QUuid>
#include <QFile>

namespace hc::document
{
    QString DocumentHostBroker::Bind(storage::EntrySnapshot Source, std::shared_ptr<storage::IWritableStorage> Writer,
        std::shared_ptr<storage::IMaterializedFileLease> Lease, QByteArray ContentHash)
    {
        if (!Source.id_.revision_ || Source.directory_ || sources_.size() >= 16) return {};
        const auto Token = QUuid::createUuid().toString(QUuid::Id128);
        sources_.emplace(Token, Binding{std::move(Source), std::move(Writer), std::move(Lease), std::move(ContentHash)}); return Token;
    }
    QJsonObject DocumentHostBroker::Commit(const QJsonObject& Prepared, storage::OperationContext Context)
    {
        const auto Encoded = Prepared["bytes"].toString().toLatin1();
        if (Encoded.size() > ((HostChunkLimit + 2) / 3) * 4) return {{"error", "document-inline-save-budget"}, {"committed", false}};
        const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (Bytes.toBase64() != Encoded) return {{"error", "document-save-payload"}, {"committed", false}};
        return CommitBytes(Prepared, Bytes, Context);
    }
    QJsonObject DocumentHostBroker::CommitBytes(const QJsonObject& Prepared, const QByteArray& Bytes, storage::OperationContext Context)
    {
        QJsonObject Reply{{"action", "commitSave"}, {"documentId", Prepared["documentId"]},
            {"saveToken", Prepared["saveToken"]}, {"revision", Prepared["revision"]}, {"committed", false}};
        auto Fail = [&](const QString& Code, storage::ErrorCategory Category = storage::ErrorCategory::Unavailable) { Reply["error"] = Code; Reply["errorCategory"] = int(Category); return Reply; };
        const auto Token = Prepared["sourceToken"].toString(), SaveToken = Prepared["saveToken"].toString();
        auto At = sources_.find(Token);
        if (At == sources_.end() || !At->second.writer_ || SaveToken.size() != 32 || retiredSaves_.contains(SaveToken)) return Fail("document-stale-source-or-save-token");
        if (retiredSaves_.size() >= 4096) return Fail("p0-save-token-budget");
        auto& Binding = At->second;
        if (Bytes.size() > DocumentByteLimit) return Fail("document-save-byte-budget");
        if (Prepared.contains("byteLength") && Prepared["byteLength"].toString() != QString::number(Bytes.size())) return Fail("document-save-length");
        if (Prepared.contains("sha256") && Prepared["sha256"].toString().toLatin1() != QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()) return Fail("document-save-checksum");
        // Replay is rejected even after an unsuccessful attempt. A retry captures
        // a fresh immutable revision and obtains a new save token in the helper.
        retiredSaves_.insert(SaveToken);
        storage::Result<storage::WriteReceipt> Saved = storage::Error{storage::ErrorCategory::Internal, "document-save"};
        const storage::DestinationExpectation Expected{true, Binding.source_.id_.revision_};
        if (Binding.source_.locator_.kind_ == storage::LocatorKind::Local && !Binding.hash_.isEmpty())
        {
            QFile Current(QString::fromUtf8(Binding.source_.locator_.root_));
            if (!Current.open(QIODevice::ReadOnly) || Current.size() > DocumentByteLimit) return Fail("document-source-read");
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            while (!Current.atEnd()) {
                if (auto Stop = Context.Check({"document"})) return Fail(QString::fromStdString(Stop->code_), Stop->category_);
                const auto Chunk = Current.read(256 * 1024);
                if (Chunk.isEmpty() && !Current.atEnd()) return Fail("document-source-read");
                Hash.addData(Chunk);
            }
            if (Current.error() != QFileDevice::NoError) return Fail("document-source-read");
            if (Hash.result() != Binding.hash_) return Fail("document-external-content-change", storage::ErrorCategory::StaleRevision);
        }
        {
            Saved = SaveDocumentBytes(*Binding.writer_, Binding.source_.locator_, Expected,
                std::as_bytes(std::span(Bytes.constData(), std::size_t(Bytes.size()))), Context);
            if (!Saved.HasValue()) return Fail(QString::fromStdString(Saved.Failure().code_), Saved.Failure().category_);
            auto Fresh = Binding.writer_->Inspect(Binding.source_.locator_, {});
            if (Fresh.HasValue() && Fresh.Value()) Binding.source_ = *Fresh.Value();
            else { Binding.source_.id_.revision_.reset(); Saved.Value().cleanupErrors_.push_back({storage::ErrorCategory::Unavailable, "document-post-commit-inspect"}); }
        }
        Binding.hash_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256);
        Reply["committed"] = Saved.Value().destinationCommitted_;
        Reply["cleanupWarning"] = !Saved.Value().cleanupErrors_.empty(); return Reply;
    }
    void DocumentHostBroker::Retain(const std::set<QString>& Tokens) { std::erase_if(sources_, [&](const auto& Item) { return !Tokens.contains(Item.first); }); }
    void DocumentHostBroker::Close() { sources_.clear(); retiredSaves_.clear(); }
}
