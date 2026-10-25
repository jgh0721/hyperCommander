#include "stdafx.h"
#include "nativeArchive.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include "hcCompareSevenZipManifest.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <Windows.h>

namespace hc::compare
{
    namespace
    {
        constexpr quint64 entryLimit = 16 * 1024 * 1024, expandedLimit = 64 * 1024 * 1024;
        template<class T> T require(storage::Result<T> Value) { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        QString format(const QByteArray& Bytes) {
            if (Bytes.startsWith(QByteArray::fromHex("377abcaf271c"))) return "7z";
            if (Bytes.startsWith("Rar!")) return "rar";
            if (Bytes.startsWith("PK")) return "zip";
            throw Failure("ArchiveFormatUnsupported");
        }
        QString checkedName(QString Name, bool Directory, bool Windows = false) {
            if (Directory && Name.endsWith('/')) Name.chop(1);
            if (Name.isEmpty() || Name.size() > 1024 || Name.startsWith('/') || Name.contains('\\') || Name.contains(':')) throw Failure("ZipUnsafePath");
            for (const auto& part : Name.split('/')) {
                if (part.isEmpty() || part == "." || part == "..") throw Failure("ZipUnsafePath");
                for (const auto ch : part) if (ch.unicode() < 32 || ch.unicode() == 127) throw Failure("ZipUnsafePath");
                if (Windows && (part.endsWith('.') || part.endsWith(' ') || part.contains(QRegularExpression("[<>\"|?*]")) || QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption).match(part).hasMatch())) throw Failure("ArchiveWriteNameUnsupported");
            }
            return Name;
        }
        class Codec
        {
        public:
            HMODULE module_ = nullptr;
            std::shared_ptr<plugins::IArchiveBackend> backend_;
            Codec() {
                const auto path = QCoreApplication::applicationDirPath() + '/' + layout::SevenZipPackage + "/7z.dll";
                QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw Failure("ArchiveCodecUnavailable");
                QCryptographicHash hash(QCryptographicHash::Sha256); if (!hash.addData(&file)) throw Failure("ArchiveCodecUnavailable");
                const auto manifest = QJsonDocument::fromJson(QByteArray(HcBuiltinSevenZipManifest)).object();
                if (hash.result().toHex() != manifest["sha256"].toString().toLatin1()) throw Failure("ArchiveCodecIdentityMismatch");
                module_ = LoadLibraryExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!module_) throw Failure("ArchiveCodecUnavailable"); backend_ = plugins::MakeSevenZipBackend(module_);
            }
            ~Codec() { backend_.reset(); if (module_) FreeLibrary(module_); }
        };
        class Archive
        {
        public:
            QTemporaryDir directory_;
            QByteArray raw_;
            QString format_, path_, digest_;
            std::vector<plugins::ArchiveHeader> headers_;
            QJsonArray entries_;
            storage::OperationContext context_;
            std::shared_ptr<plugins::IArchiveBackend> backend_;
            Archive(Codec& Codec, QByteArray Bytes, QString Password, const Budget& Limits) : raw_(std::move(Bytes)), backend_(Codec.backend_) {
                if (!directory_.isValid() || raw_.size() > qsizetype(entryLimit)) throw Failure("ArchiveInputLimit");
                format_ = format(raw_); path_ = directory_.filePath("source." + format_); QFile output(path_);
                if (!output.open(QIODevice::WriteOnly) || output.write(raw_) != raw_.size()) throw Failure("ArchiveTemporaryWriteFailed"); output.close();
                context_.deadline_ = Limits.deadline_; context_.maxReadBytes_ = 64 * 1024;
                if (!Password.isNull()) context_.passwordPrompt_ = [Password](const std::string&, unsigned Attempt, const storage::OperationContext&) -> storage::Result<storage::Password> { if (Attempt > 2) return storage::Error{storage::ErrorCategory::AccessDenied, "archive-password-invalid"}; return storage::Password(Password.toUtf8().toStdString()); };
                digest_ = QString::fromLatin1(QCryptographicHash::hash(raw_, QCryptographicHash::Sha256).toHex());
                const auto listing = require(backend_->Enumerate(path_.toStdString(), false, context_)); quint64 expanded = 0;
                for (;;) {
                    Limits.Check(); const auto page = require(listing->Next(256, context_));
                    for (auto header : page.entries_) {
                        if (headers_.size() >= 10000) throw Failure("ArchiveEntryLimit");
                        if (header.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ArchiveSpecialEntryUnsupported");
                        header.name_ = checkedName(QString::fromStdString(header.name_), header.directory_).toStdString();
                        if (header.size_ > entryLimit || expanded > expandedLimit - header.size_) throw Failure("ArchiveExpandedLimit"); expanded += header.size_; headers_.push_back(std::move(header));
                    }
                    if (page.end_) break;
                }
                std::vector<std::uint32_t> ordinals; for (std::size_t i = 0; i < headers_.size(); ++i) if (!headers_[i].directory_) ordinals.push_back(std::uint32_t(i));
                auto stream = ordinals.empty() ? std::unique_ptr<plugins::IArchiveReadSession>{} : require(backend_->OpenReadSession(path_.toStdString(), ordinals, context_));
                QMap<QString, int> occurrences;
                for (std::size_t ordinal = 0; ordinal < headers_.size(); ++ordinal) {
                    Limits.Check(); const auto& header = headers_[ordinal]; const auto name = QString::fromStdString(header.name_); QCryptographicHash hash(QCryptographicHash::Sha256);
                    if (!header.directory_) {
                        QFile file(Data(int(ordinal))); if (!file.open(QIODevice::WriteOnly)) throw Failure("ArchiveTemporaryWriteFailed"); quint64 received = 0;
                        for (;;) {
                            Limits.Check(); const auto chunk = require(stream->Read(std::uint32_t(ordinal), 64 * 1024, context_));
                            if (chunk.bytes_.size() > header.size_ - received || (chunk.bytes_.empty() && !chunk.eof_)) throw Failure("ArchiveContentInvalid");
                            const QByteArrayView bytes(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size())); hash.addData(bytes);
                            if (file.write(bytes.data(), bytes.size()) != bytes.size()) throw Failure("ArchiveTemporaryWriteFailed"); received += chunk.bytes_.size();
                            if (chunk.eof_) { if (received != header.size_) throw Failure("ArchiveContentInvalid"); break; }
                        }
                    }
                    const auto contentHash = header.directory_ ? QString{} : QString::fromLatin1(hash.result().toHex());
                    const QJsonObject location{{"schema", "archive.entry.v1"}, {"version", 1}, {"resourceRevision", "sha256:" + digest_}, {"payload", QJsonObject{{"ordinal", int(ordinal)}, {"path", name}, {"contentSha256", contentHash}}}};
                    entries_.append(QJsonObject{{"path", name}, {"ordinal", int(ordinal)}, {"occurrence", occurrences[name]++}, {"directory", header.directory_}, {"size", QString::number(header.size_)}, {"sha256", contentHash}, {"encrypted", header.encrypted_}, {"location", location}});
                }
            }
            QString Data(int Ordinal) const { return directory_.filePath(QString::number(Ordinal) + ".bin"); }
            QByteArray Read(int Ordinal) const { if (Ordinal < 0 || Ordinal >= int(headers_.size()) || headers_[Ordinal].directory_) throw Failure("ArchiveEntryUnavailable"); QFile file(Data(Ordinal)); if (!file.open(QIODevice::ReadOnly)) throw Failure("ArchiveTemporaryReadFailed"); return file.readAll(); }
        };
        QJsonObject compare(const std::vector<std::unique_ptr<Archive>>& Archives) {
            QJsonArray documents, rows; std::vector<QMap<QString, QJsonObject>> maps;
            for (const auto& archive : Archives) {
                documents.append(QJsonObject{{"rawSha256", archive->digest_}, {"entryCount", archive->entries_.size()}, {"entries", Archives.size() == 1 ? archive->entries_ : QJsonArray{}}, {"format", archive->format_}});
                QMap<QString, QJsonObject> map; for (const auto& value : archive->entries_) { const auto entry = value.toObject(); map.insert(entry["path"].toString() + QChar::Null + QString::number(entry["occurrence"].toInt()), entry); } maps.push_back(std::move(map));
            }
            bool equal = true;
            if (Archives.size() == 2) {
                QMap<QString, bool> keys; for (const auto& map : maps) for (auto it = map.cbegin(); it != map.cend(); ++it) keys.insert(it.key(), true);
                for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
                    const auto left = maps[0].value(it.key()), right = maps[1].value(it.key());
                    const auto state = left.isEmpty() ? "RightOnly" : right.isEmpty() ? "LeftOnly" : left["directory"] != right["directory"] ? "TypeChanged" : left["sha256"] == right["sha256"] && left["size"] == right["size"] ? "Equal" : "Changed";
                    equal &= state == "Equal"; rows.append(QJsonObject{{"path", (left.isEmpty() ? right : left)["path"]}, {"state", state}, {"left", left}, {"right", right}});
                }
            }
            return {{"status", "Complete"}, {"payloadSchema", "archive.entries.v1"}, {"documents", documents}, {"entries", rows}, {"contentsEqual", equal}, {"rawEqual", Archives.size() == 1 || Archives[0]->raw_ == Archives[1]->raw_}, {"coverage", "verified ordinal streams; bounded nested ZIP/7z/RAR"}, {"merge", true}};
        }
        struct Replacement { QString path_; QByteArray bytes_; bool directory_ = false; };
        QByteArray rewrite(Archive& Target, const std::vector<Replacement>& Changes, const QStringList& Remove, const Budget& Limits) {
            if (Target.format_ != "zip" && Target.format_ != "7z") throw Failure("ArchiveTargetFormatReadOnly");
            QSet<QString> names; QMap<QString, bool> types;
            for (const auto& header : Target.headers_) { const auto name = checkedName(QString::fromStdString(header.name_), header.directory_, true).toCaseFolded(); if (names.contains(name)) throw Failure("ArchiveDuplicateTargetReadOnly"); names.insert(name); types.insert(name, header.directory_); }
            for (const auto& name : Remove) types.remove(name.toCaseFolded());
            for (const auto& change : Changes) types.insert(change.path_.toCaseFolded(), change.directory_);
            for (auto it = types.cbegin(); it != types.cend(); ++it) { auto parent = it.key(); while (parent.contains('/')) { parent = parent.left(parent.lastIndexOf('/')); if (types.contains(parent) && !types[parent]) throw Failure("ArchivePathTypeConflict"); } }
            QTemporaryDir files; if (!files.isValid()) throw Failure("ArchiveTemporaryWriteFailed");
            plugins::ArchiveUpdate update; update.archive_ = Target.path_.toStdString(); update.inputDirectory_ = files.path().toStdString(); update.maxOutputBytes_ = entryLimit;
            for (const auto& name : Remove) update.remove_.push_back(name.toStdString());
            QSet<QString> destinations;
            for (const auto& change : Changes) {
                Limits.Check(); const auto name = checkedName(change.path_, change.directory_, true); if (destinations.contains(name.toCaseFolded())) throw Failure("ArchiveDuplicateWritePath"); destinations.insert(name.toCaseFolded());
                const auto path = files.filePath(name); if (!QDir().mkpath(change.directory_ ? path : QFileInfo(path).absolutePath())) throw Failure("ArchiveTemporaryWriteFailed");
                if (!change.directory_) { QFile output(path); if (!output.open(QIODevice::WriteOnly) || output.write(change.bytes_) != change.bytes_.size()) throw Failure("ArchiveTemporaryWriteFailed"); }
                update.add_.push_back(name.toStdString());
                auto origin = std::find_if(Target.headers_.begin(), Target.headers_.end(), [&](const auto& Header) { return QString::fromStdString(Header.name_).compare(name, Qt::CaseInsensitive) == 0; });
                update.metadataOrigins_.push_back(origin == Target.headers_.end() ? std::string{} : origin->name_);
                if (origin != Target.headers_.end() && std::find(update.remove_.begin(), update.remove_.end(), origin->name_) == update.remove_.end()) update.remove_.push_back(origin->name_);
            }
            require(Target.backend_->Update(std::move(update), Target.context_)); QFile output(Target.path_); if (!output.open(QIODevice::ReadOnly)) throw Failure("ArchiveTemporaryReadFailed");
            const auto result = output.read(qint64(entryLimit) + 1); if (result.size() > qsizetype(entryLimit)) throw Failure("ArchiveInputLimit"); return result;
        }
        QByteArray rewriteNested(Codec& Codec, QByteArray Raw, const QJsonArray& Chain, const QJsonArray& Passwords, int Depth,
            const std::vector<Replacement>& Changes, const QStringList& Remove, const Budget& Limits, QJsonArray& NewChain) {
            const auto password = Depth < Passwords.size() && Passwords[Depth].isString() ? Passwords[Depth].toString() : QString{};
            Archive target(Codec, std::move(Raw), password, Limits);
            QByteArray result;
            if (Depth == Chain.size()) result = rewrite(target, Changes, Remove, Limits);
            else { const auto ordinal = Chain[Depth].toInt(-1); const auto bytes = rewriteNested(Codec, target.Read(ordinal), Chain, Passwords, Depth + 1, Changes, Remove, Limits, NewChain); result = rewrite(target, {{QString::fromStdString(target.headers_[ordinal].name_), bytes, false}}, {}, Limits); }
            Archive verified(Codec, result, password, Limits);
            if (Depth < Chain.size()) { const auto name = target.headers_[Chain[Depth].toInt()].name_; const auto found = std::find_if(verified.headers_.begin(), verified.headers_.end(), [&](const auto& Header) { return Header.name_ == name; }); if (found == verified.headers_.end()) throw Failure("ArchiveRewriteVerificationFailed"); NewChain[Depth] = int(found - verified.headers_.begin()); }
            if (Depth == Chain.size()) {
                for (const auto& change : Changes) {
                    const auto found = std::find_if(verified.entries_.begin(), verified.entries_.end(), [&](const QJsonValue& Entry) { return Entry.toObject()["path"] == change.path_; });
                    if (found == verified.entries_.end() || found->toObject()["directory"].toBool() != change.directory_ || (!change.directory_ && found->toObject()["sha256"].toString().toLatin1() != QCryptographicHash::hash(change.bytes_, QCryptographicHash::Sha256).toHex())) throw Failure("ArchiveRewriteVerificationFailed");
                }
                for (const auto& name : Remove) if (std::none_of(Changes.begin(), Changes.end(), [&](const auto& Change) { return Change.path_.compare(name, Qt::CaseInsensitive) == 0; })) for (const auto& entry : verified.entries_) if (entry.toObject()["path"].toString().compare(name, Qt::CaseInsensitive) == 0) throw Failure("ArchiveRewriteVerificationFailed");
            }
            return result;
        }
    }
    QJsonObject NativeArchiveWorkerRequest(const QJsonObject& Payload)
    {
        Codec codec; Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        const auto resources = Payload["resources"].toArray(); if (resources.isEmpty() || resources.size() > 2) throw Failure("ArchiveArityUnsupported");
        const auto item = [](const QJsonArray& Array, int Index) { return Index >= 0 && Index < Array.size() ? Array[Index] : QJsonValue{}; };
        std::vector<std::unique_ptr<Archive>> archives;
        for (int side = 0; side < resources.size(); ++side) {
            if (resources[side].toString().size() > 24 * 1024 * 1024) throw Failure("ArchiveInputLimit");
            auto decoded = QByteArray::fromBase64Encoding(resources[side].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded) throw Failure("InvalidInputEncoding");
            auto bytes = std::move(decoded.decoded); const auto chain = item(Payload["members"].toArray(), side).toArray(); if (chain.size() > 4) throw Failure("ArchiveNestingLimit");
            const auto passwords = item(Payload["passwords"].toArray(), side).toArray();
            for (int depth = 0;; ++depth) {
                try {
                    const auto password = item(passwords, depth);
                    auto archive = std::make_unique<Archive>(codec, std::move(bytes), password.isString() ? password.toString() : QString{}, limits);
                    if (depth == chain.size()) { archives.push_back(std::move(archive)); break; }
                    if (!chain[depth].isDouble() || chain[depth].toInt(-1) < 0) throw Failure("ArchiveMemberOrdinalInvalid"); bytes = archive->Read(chain[depth].toInt());
                } catch (const Failure& error) {
                    if (error.code_.find("password") != std::string::npos) throw Failure("ArchivePasswordRequired:" + std::to_string(side) + ":" + std::to_string(depth)); throw;
                }
            }
        }
        const auto operation = Payload["operation"].toString();
        if (operation == "read") {
            const int side = Payload["side"].toInt(-1), ordinal = Payload["ordinal"].toInt(-1); if (side < 0 || side >= int(archives.size())) throw Failure("ArchiveSideInvalid");
            return {{"status", "Complete"}, {"bytes", QString::fromLatin1(archives[side]->Read(ordinal).toBase64())}, {"name", archives[side]->entries_[ordinal].toObject()["path"]}};
        }
        if (operation == "rewrite") {
            const auto target = Payload["target"].toInt(-1); if (target < 0 || target >= int(archives.size())) throw Failure("ArchiveSideInvalid");
            const auto changes = Payload["changes"].toArray(); if (changes.isEmpty() || changes.size() > 10000) throw Failure("ArchiveChangeLimit");
            std::vector<Replacement> replacements; QStringList remove;
            for (const auto& value : changes) {
                const auto change = value.toObject(); const auto ordinal = change["ordinal"].toInt(-1);
                if (change["remove"].toBool()) { if (ordinal < 0 || ordinal >= int(archives[target]->headers_.size())) throw Failure("ArchiveEntryUnavailable"); remove.append(QString::fromStdString(archives[target]->headers_[ordinal].name_)); }
                else { const auto side = change["side"].toInt(-1); if (side < 0 || side >= int(archives.size()) || ordinal < 0 || ordinal >= int(archives[side]->headers_.size())) throw Failure("ArchiveEntryUnavailable"); const auto& header = archives[side]->headers_[ordinal]; replacements.push_back({QString::fromStdString(header.name_), header.directory_ ? QByteArray{} : archives[side]->Read(ordinal), header.directory_}); }
            }
            auto decoded = QByteArray::fromBase64Encoding(resources[target].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded) throw Failure("InvalidInputEncoding");
            auto chain = item(Payload["members"].toArray(), target).toArray(); auto newChain = chain;
            const auto result = rewriteNested(codec, std::move(decoded.decoded), chain, item(Payload["passwords"].toArray(), target).toArray(), 0, replacements, remove, limits, newChain);
            return {{"status", "Complete"}, {"bytes", QString::fromLatin1(result.toBase64())}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(result, QCryptographicHash::Sha256).toHex())}, {"members", newChain}};
        }
        if (!operation.isEmpty() && operation != "compare") throw Failure("ArchiveOperationUnsupported");
        return compare(archives);
    }
}
