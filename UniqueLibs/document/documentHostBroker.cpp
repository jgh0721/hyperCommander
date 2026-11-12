#include "documentHostBroker.hpp"
#include "archiveDraftDirectory.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QCryptographicHash>
#include <QUuid>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QDateTime>

namespace hc::document
{
    std::optional<DocumentHostBroker::EditorInput> DocumentHostBroker::ResolveEditorInput(const QString& SourceToken) const
    {
        const auto At=sources_.find(SourceToken);if(At==sources_.end())return {};
        const auto& Binding=At->second;
        const auto Kind=Binding.source_.locator_.kind_;
        if(Kind==storage::LocatorKind::Local||Kind==storage::LocatorKind::Smb)
            return EditorInput{QString::fromUtf8(Binding.source_.locator_.root_),true,{}};
        if(Binding.lease_)return EditorInput{QString::fromUtf8(Binding.lease_->LocalPath()),false,Binding.lease_};
        return {};
    }
    QString DocumentHostBroker::Bind(storage::EntrySnapshot Source, std::shared_ptr<storage::IWritableStorage> Writer,
        std::shared_ptr<storage::IMaterializedFileLease> Lease, QByteArray ContentHash, storage::ProviderContentSource::ArchiveWriteback ArchiveWriteback)
    {
        if (!Source.id_.revision_ || Source.directory_) return {};
        const auto Token = QUuid::createUuid().toString(QUuid::Id128);
        sources_.emplace(Token, Binding{std::move(Source), std::move(Writer), std::move(Lease), std::move(ContentHash), std::move(ArchiveWriteback)}); return Token;
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
        if (At == sources_.end() || (!At->second.writer_ && !At->second.archiveWriteback_) || SaveToken.size() != 32 || retiredSaves_.contains(SaveToken)) return Fail("document-stale-source-or-save-token");
        if (retiredSaves_.size() >= 4096) return Fail("p0-save-token-budget");
        auto& Binding = At->second;
        if (Prepared.contains("byteLength") && Prepared["byteLength"].toString() != QString::number(Bytes.size())) return Fail("document-save-length");
        if (Prepared.contains("sha256") && Prepared["sha256"].toString().toLatin1() != QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()) return Fail("document-save-checksum");
        // Replay is rejected even after an unsuccessful attempt. A retry captures
        // a fresh immutable revision and obtains a new save token in the helper.
        retiredSaves_.insert(SaveToken);
        if (Binding.archiveWriteback_) {
            if (!Binding.draft_) {
                // The edited copy is the user's only one until the archive is updated: keep it with the
                // other recovery copies (owner.lock tells a live session from one whose process ended).
                Binding.draft_ = std::make_shared<ArchiveDraftDirectory>(ArchiveDraftRoot());
                if (!Binding.draft_->isValid()) return Fail("archive-draft-directory");
                if (!QDir(Binding.draft_->path()).mkdir("content")) return Fail("archive-draft-directory");
                Binding.draftPath_ = Binding.draft_->filePath("content/" + QFileInfo(QString::fromUtf8(Binding.source_.name_)).fileName());
                Binding.archiveHash_ = Binding.hash_;
                if (Binding.archiveHash_.isEmpty() && Binding.lease_) {
                    QFile Original(QString::fromStdString(Binding.lease_->LocalPath()));
                    QCryptographicHash Hash(QCryptographicHash::Sha256);
                    if (!Original.open(QIODevice::ReadOnly) || !Hash.addData(&Original)) { Binding.draft_.reset(); return Fail("archive-draft-baseline"); }
                    Binding.archiveHash_ = Hash.result();
                }
            }
            QSaveFile Draft(Binding.draftPath_);
            if (!Draft.open(QIODevice::WriteOnly) || Draft.write(Bytes) != Bytes.size() || !Draft.commit()) return Fail("archive-draft-save");
            Binding.archivePending_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256) != Binding.archiveHash_;
            // A failed update or helper crash must not delete the user's only edited copy.
            Binding.draft_->setAutoRemove(!Binding.archivePending_);
            const auto& Locator = Binding.source_.locator_;
            WriteArchiveDraftManifest(Binding.draft_->path(), QJsonObject{{"schemaVersion", 2},
                {"purpose", "archive-preview-edit"}, {"createdUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"owner", Token}, {"retention", "until-recovered-or-explicitly-discarded"}, {"source", QString::fromStdString(storage::Serialize(Locator))},
                {"name", QFileInfo(Binding.draftPath_).fileName()}, {"archivePath", Locator.kind_ == storage::LocatorKind::Archive ? QString::fromUtf8(Locator.root_) : QString{}},
                {"entryPath", Locator.nesting_.empty() ? QString{} : QString::fromUtf8(Locator.nesting_.back().entry_)}, {"nesting", int(Locator.nesting_.size())},
                {"baselineSha256", QString::fromLatin1(Binding.archiveHash_.toHex())}, {"draft", Binding.draftPath_}, {"pending", Binding.archivePending_}});
            Reply["committed"] = true; Reply["archivePending"] = Binding.archivePending_; Reply["draftPath"] = Binding.draftPath_;
            return Reply;
        }
        storage::Result<storage::WriteReceipt> Saved = storage::Error{storage::ErrorCategory::Internal, "document-save"};
        const storage::DestinationExpectation Expected{true, Binding.source_.id_.revision_};
        if ((Binding.source_.locator_.kind_ == storage::LocatorKind::Local || Binding.source_.locator_.kind_ == storage::LocatorKind::Smb) && !Binding.hash_.isEmpty())
        {
            QFile Current(QString::fromUtf8(Binding.source_.locator_.root_));
            if (!Current.open(QIODevice::ReadOnly)) return Fail("document-source-read");
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
    void DocumentHostBroker::CaptureLoadedHash(const QString& Token, const QByteArray& Hash)
    {
        const auto At = sources_.find(Token);
        if (At != sources_.end() && At->second.hash_.isEmpty() && Hash.size() == 32) At->second.hash_ = Hash;
    }
    QJsonObject DocumentHostBroker::CommitArchive(const QString& Token, bool Discard, storage::OperationContext Context)
    {
        const auto At = sources_.find(Token);
        if (At == sources_.end()) return {{"error", "archive-source-expired"}};
        auto& Binding = At->second;
        if (!Binding.archivePending_) return {{"committed", true}};
        if (!Discard) {
            const auto Saved = Binding.archiveWriteback_(Binding.draftPath_.toStdString(), Context);
            if (!Saved.HasValue()) return {{"error", QString::fromStdString(Saved.Failure().code_)}, {"draftPath", Binding.draftPath_}};
        }
        Binding.archivePending_ = false;
        Binding.draft_->setAutoRemove(true);
        return {{"committed", true}};
    }
    void DocumentHostBroker::Retain(const std::set<QString>& Tokens) { std::erase_if(sources_, [&](const auto& Item) { return !Tokens.contains(Item.first); }); }
    void DocumentHostBroker::Close() { sources_.clear(); retiredSaves_.clear(); }
}
