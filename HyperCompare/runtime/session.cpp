#include "stdafx.h"
#include "session.hpp"
#include "decisionPlan.hpp"
#include "mergeWork.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace hc::compare
{
    namespace
    {
        constexpr qsizetype textLimit = 16 * 1024 * 1024;
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        std::shared_ptr<storage::IStorageProvider> reader(const QString& Path)
        { return storage::MakeNativeReadProvider(locator(Path).kind_); }
        template<class T> T require(storage::Result<T> Value)
        {
            if (!Value.HasValue()) throw Failure(Value.Failure().code_);
            return std::move(Value.Value());
        }
        QString identity(const storage::EntrySnapshot& Entry)
        {
            if (!Entry.id_.revision_) return {};
            const auto token = QString::fromStdString(Entry.id_.revision_->token_);
            const auto at = token.indexOf(":identity:");
            return at < 0 ? QString{} : token.mid(at);
        }
        storage::OperationContext context(const Budget& Limits)
        {
            storage::OperationContext result; result.deadline_ = Limits.deadline_;
            result.workerRead_ = true; result.denyConcurrentMutation_ = true;
            result.incrementalEnumeration_ = true; result.maxEnumerationEntries_ = 100000;
            return result;
        }
        Snapshot readSnapshot(const QString& Path, qsizetype MaxBytes, const Budget& Limits)
        {
            Limits.Check(); const auto provider = reader(Path); auto ctx = context(Limits);
            Snapshot result; result.path_ = Path;
            result.entry_ = require(provider->Resolve(locator(Path), ctx).get());
            if (result.entry_.directory_) throw Failure("ExpectedFile");
            if (result.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseInputUnsupported");
            if (result.entry_.size_ > std::uint64_t(MaxBytes)) throw Failure("InputSizeLimit");
            const auto stream = require(provider->OpenRead(result.entry_.id_, ctx).get());
            for (;;)
            {
                Limits.Check(); const auto chunk = require(stream->Read(1024 * 1024, ctx).get());
                if (chunk.bytes_.size() > std::size_t(MaxBytes - result.raw_.size())) throw Failure("InputSizeLimit");
                result.raw_.append(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size()));
                if (chunk.eof_) break;
                if (chunk.bytes_.empty()) throw Failure("ReadMadeNoProgress");
            }
            const auto after = require(provider->Stat(result.entry_.id_, ctx).get());
            if (after.id_.revision_ != result.entry_.id_.revision_) throw Failure("ChangedDuringRead");
            return result;
        }
        Snapshot load(const QString& Path, const QString& Encoding, const Budget& Limits)
        {
            auto result = readSnapshot(Path, textLimit, Limits);
            auto encoding = Encoding;
            if (encoding.isEmpty())
            {
                if (result.raw_.startsWith("\xff\xfe")) encoding = "UTF-16LE";
                else if (result.raw_.startsWith("\xfe\xff")) encoding = "UTF-16BE";
                else encoding = "UTF-8";
            }
            const auto decoded = require(document::DecodeDocumentText(result.raw_, encoding));
            result.encoding_ = decoded.encoding_; result.bom_ = decoded.bom_; result.utf8_ = decoded.text_.toUtf8();
            if (result.utf8_.contains('\0')) throw Failure("BinaryInputUnsupported");
            if (result.utf8_.size() > textLimit) throw Failure("DecodedSizeLimit");
            const auto lines = SplitLines(std::string_view(result.utf8_.constData(), std::size_t(result.utf8_.size())));
            if (lines.size() > 300000) throw Failure("LineLimit");
            for (const auto line : lines) if (line.size() > 256 * 1024) throw Failure("LongLineLimit");
            return result;
        }
        std::string_view view(const QByteArray& Text) { return {Text.constData(), std::size_t(Text.size())}; }
        void checkArtifactPaths(const Request& Input, const Budget& Limits)
        {
            if (Input.planOut_.isEmpty() && Input.decisions_.isEmpty()) return;
            struct Path { QString path_, identity_; bool write_; };
            QVector<Path> paths;
            for (const auto& entry : {std::pair{Input.file1_, false}, {Input.file2_, false}, {Input.file3_, false}, {Input.base_, false},
                {Input.decisions_, false}, {Input.output_, true}, {Input.planOut_, true}, {Input.report_ == "-" ? QString{} : Input.report_, true}})
            {
                if (entry.first.isEmpty()) continue; Limits.Check(); const QFileInfo info(entry.first);
                const auto canonical = info.canonicalFilePath();
                Path path{canonical.isEmpty() ? info.absoluteFilePath() : canonical, {}, entry.second};
                if (info.exists()) path.identity_ = identity(require(reader(entry.first)->Resolve(locator(entry.first), context(Limits)).get()));
                for (const auto& previous : paths)
                    if ((previous.write_ || path.write_) && (previous.path_.compare(path.path_, Qt::CaseInsensitive) == 0 ||
                        (!path.identity_.isEmpty() && path.identity_ == previous.identity_))) throw Failure("ArtifactPathAlias");
                paths.push_back(std::move(path));
            }
        }
        void writePlan(const Request& Input, Result& Value, const Budget& Limits)
        {
            auto data = QJsonDocument(MakeDecisionPlan(Input, Value, Limits)).toJson(); data.replace("\n", "\r\n");
            if (data.size() > decisionPlanBytes) throw Failure("DecisionPlanSizeLimit");
            const auto provider = reader(Input.planOut_); const auto writer = storage::MakeNativeWritableStorage(locator(Input.planOut_).kind_, provider);
            if (require(writer->Inspect(locator(Input.planOut_), context(Limits)))) throw Failure("PlanAlreadyExists");
            // Plan export never inherits -overwrite, which authorizes RESULT only.
            const auto receipt = require(document::SaveDocumentBytes(*writer, locator(Input.planOut_), {},
                {reinterpret_cast<const std::byte*>(data.constData()), std::size_t(data.size())}, context(Limits)));
            if (!receipt.destinationCommitted_) throw Failure("PlanNotCommitted");
            Value.planWritten_ = Input.planOut_;
            for (const auto& error : receipt.cleanupErrors_) Value.warnings_.append(QString::fromStdString(error.code_));
            for (const auto& artifact : receipt.preservedArtifacts_) Value.recovery_.append(QString::fromStdString(artifact.root_));
        }
        void inspectOutput(const Request& Request, Result& Result, const Budget& Limits)
        {
            const auto provider = reader(Request.output_);
            const auto writer = storage::MakeNativeWritableStorage(locator(Request.output_).kind_, provider);
            const auto existing = require(writer->Inspect(locator(Request.output_), context(Limits)));
            if (existing)
            {
                if (!Request.overwrite_) throw Failure("OutputExists");
                if (existing->directory_) throw Failure("OutputIsDirectory");
                for (const auto& input : Result.inputs_)
                    if (Request.output_.compare(input.path_, Qt::CaseInsensitive) == 0 ||
                        (!identity(*existing).isEmpty() && identity(*existing) == identity(input.entry_))) throw Failure("InPlaceNotSupported");
                if (Result.decisionsSnapshot_ && identity(*existing) == identity(Result.decisionsSnapshot_->entry_)) throw Failure("ArtifactPathAlias");
                Result.expected_ = {true, existing->id_.revision_};
            }
            Result.hasOutputExpectation_ = true;
        }
        using Tree = QMap<QString, storage::EntrySnapshot>;
        Tree enumerate(const QString& Root, const Budget& Limits)
        {
            Tree result; const auto provider = reader(Root); const auto ctx = context(Limits);
            const auto root = require(provider->Resolve(locator(Root), ctx).get());
            if (!root.directory_) throw Failure("ExpectedDirectory");
            if (root.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
            std::vector<std::pair<storage::Locator, QString>> pending{{root.locator_, {}}};
            while (!pending.empty())
            {
                Limits.Check(); auto [at, prefix] = std::move(pending.back()); pending.pop_back();
                const auto cursor = require(provider->Enumerate(at, ctx).get());
                for (;;)
                {
                    Limits.Check(); auto page = require(cursor->Next(256, ctx).get());
                    for (auto& entry : page.entries_)
                    {
                        if (result.size() >= 100000) throw Failure("DirectoryEntryLimit");
                        if (entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("ReparseDirectoryUnsupported");
                        const auto relative = prefix + QString::fromStdString(entry.name_);
                        const auto key = relative.toCaseFolded();
                        if (result.contains(key)) throw Failure("CaseCollision");
                        entry.hints_["relativePath"] = relative.toUtf8().toStdString();
                        if (entry.directory_) pending.push_back({entry.locator_, relative + '/'});
                        result.insert(key, std::move(entry));
                    }
                    if (page.end_) break;
                }
            }
            return result;
        }
        bool sameContent(const storage::EntrySnapshot& A, const storage::EntrySnapshot& B, const Budget& Limits)
        {
            if (A.size_ != B.size_) return false;
            const auto pa = reader(QString::fromStdString(A.locator_.root_)), pb = reader(QString::fromStdString(B.locator_.root_));
            const auto ctx = context(Limits);
            // Resource keys are provider-session scoped. Rebind locators and verify captured revisions.
            const auto boundA = require(pa->Resolve(A.locator_, ctx).get()), boundB = require(pb->Resolve(B.locator_, ctx).get());
            if (boundA.id_.revision_ != A.id_.revision_ || boundB.id_.revision_ != B.id_.revision_) throw Failure("ChangedDuringRead");
            auto a = require(pa->OpenRead(boundA.id_, ctx).get()), b = require(pb->OpenRead(boundB.id_, ctx).get());
            // Streams may return different short-read sizes, so compare a rolling byte window.
            std::vector<std::byte> aa, bb; std::size_t ai = 0, bi = 0; bool ae = false, be = false, equal = true;
            for (;;)
            {
                Limits.Check();
                if (ai == aa.size() && !ae) { auto chunk = require(a->Read(1024 * 1024, ctx).get()); aa = std::move(chunk.bytes_); ai = 0; ae = chunk.eof_; if (aa.empty() && !ae) throw Failure("ReadMadeNoProgress"); }
                if (bi == bb.size() && !be) { auto chunk = require(b->Read(1024 * 1024, ctx).get()); bb = std::move(chunk.bytes_); bi = 0; be = chunk.eof_; if (bb.empty() && !be) throw Failure("ReadMadeNoProgress"); }
                const auto count = std::min(aa.size() - ai, bb.size() - bi);
                if (count && !std::equal(aa.begin() + ai, aa.begin() + ai + count, bb.begin() + bi)) { equal = false; break; }
                ai += count; bi += count;
                if (ae && ai == aa.size() && be && bi == bb.size()) break;
                if ((ae && ai == aa.size() && bi < bb.size()) || (be && bi == bb.size() && ai < aa.size())) { equal = false; break; }
            }
            if (require(pa->Stat(boundA.id_, ctx).get()).id_.revision_ != A.id_.revision_ ||
                require(pb->Stat(boundB.id_, ctx).get()).id_.revision_ != B.id_.revision_) throw Failure("ChangedDuringRead");
            return equal;
        }
        void directory(const Request& Request, Result& Value, const Budget& Limits)
        {
            if (!Request.base_.isEmpty() || !Request.file3_.isEmpty()) throw Failure("DirectoryThreeWayNotImplemented");
            const auto left = enumerate(Request.file1_, Limits), right = enumerate(Request.file2_, Limits);
            auto keys = left.keys(); for (const auto& key : right.keys()) if (!left.contains(key)) keys.push_back(key);
            std::sort(keys.begin(), keys.end()); bool different = false;
            for (const auto& key : keys)
            {
                Limits.Check(); QString status;
                const auto a = left.constFind(key), b = right.constFind(key);
                if (a == left.end()) status = "RightOnly";
                else if (b == right.end()) status = "LeftOnly";
                else if (a->directory_ != b->directory_) status = "TypeConflict";
                else if (a->name_ != b->name_) status = "NameDifferent";
                else if (a->directory_) status = "Equal";
                else status = sameContent(*a, *b, Limits) ? "Equal" : "Different";
                different |= status != "Equal";
                const auto& entry = a == left.end() ? *b : *a;
                Value.entries_.append(QJsonObject{{"path", QString::fromStdString(entry.hints_.at("relativePath"))}, {"status", status}});
            }
            Value.exitCode_ = different ? 1 : 0; Value.outcome_ = different ? "Different" : "Equal";
        }
    }

    QJsonObject Capabilities()
    {
        return {{"schemaVersion", 1}, {"phase", "foundation"}, {"textCompare2", true}, {"textCompare3", true},
            {"textMerge3", true}, {"textMerge2", "explicit-block-decisions-or-whole-side"}, {"directoryCompare2", true},
            {"textDecisionPlans", true}, {"decisionPlanSchema", 1}, {"maxDecisionPlanBytes", double(decisionPlanBytes)},
            {"maxDecisionBlocks", double(decisionBlockLimit)}, {"maxManualReplacementBytes", 1048576},
            {"guiBlockMerge", true}, {"guiResolutionUndo", true},
            {"guiRecomparePreservesResult", true}, {"guiRecomparePolicy", "retain-result-review-changed-inputs"},
            {"directoryMerge", false}, {"sourceMovement", false}, {"image", false}, {"office", false}, {"archive", false},
            {"maxDecodedBytes", double(textLimit)}, {"maxLines", 300000}, {"maxLineBytes", 262144}};
    }
    Result Execute(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled, bool Save)
    {
        Result result; Budget limits; limits.canceled_ = Canceled.get();
        limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Input.timeoutMs_);
        try
        {
            if (!Input.planOut_.isEmpty() || !Input.decisions_.isEmpty())
            {
                if (!Input.headless_ || Input.mode_ != "merge" || !Input.takeAll_.isEmpty() ||
                    (!Input.planOut_.isEmpty() && !Input.decisions_.isEmpty())) throw Failure("InvalidDecisionOptions");
                checkArtifactPaths(Input, limits);
                if (!Input.decisions_.isEmpty()) result.decisionsSnapshot_ = readSnapshot(Input.decisions_, decisionPlanBytes, limits);
            }
            const auto first = require(reader(Input.file1_)->Resolve(locator(Input.file1_), context(limits)).get());
            const auto second = require(reader(Input.file2_)->Resolve(locator(Input.file2_), context(limits)).get());
            if (first.directory_ != second.directory_) throw Failure("MixedInputKinds");
            if (Input.mode_ == "directory" || (Input.mode_ == "auto" && first.directory_))
            { result.operation_ = "directory2"; directory(Input, result, limits); }
            else
            {
                result.operation_ = Input.mode_ == "merge" ? (Input.base_.isEmpty() ? "merge2" : "merge3") : "compare2";
                result.inputs_.push_back(load(Input.file1_, Input.encoding_, limits));
                result.inputs_.push_back(load(Input.file2_, Input.encoding_, limits));
                if (!Input.base_.isEmpty()) result.inputs_.push_back(load(Input.base_, Input.encoding_, limits));
                else if (!Input.file3_.isEmpty()) result.inputs_.push_back(load(Input.file3_, Input.encoding_, limits));
                const auto left = view(result.inputs_[0].utf8_), right = view(result.inputs_[1].utf8_);
                result.edits_ = Diff(left, right, limits);
                if (!Input.headless_) result.alignment_ = std::make_shared<AlignmentMap>(
                    AlignmentMap::Build(SplitLines(left).size(), SplitLines(right).size(), result.edits_, limits));
                bool different = !result.edits_.empty() || result.inputs_[0].raw_ != result.inputs_[1].raw_;
                if (result.inputs_.size() == 3)
                {
                    if (!Input.base_.isEmpty()) result.merged_ = Merge(view(result.inputs_[2].utf8_), left, right, limits);
                    else
                    {
                        for (const auto& pair : {std::pair{0, 1}, std::pair{1, 2}, std::pair{0, 2}})
                        {
                            const auto changes = Diff(view(result.inputs_[pair.first].utf8_), view(result.inputs_[pair.second].utf8_), limits);
                            result.comparisons_.append(QJsonObject{{"fromFile", pair.first + 1}, {"toFile", pair.second + 1}, {"changeCount", double(changes.size())}});
                        }
                    }
                    different |= result.inputs_[2].raw_ != result.inputs_[0].raw_;
                    if (Input.mode_ != "merge") result.operation_ = Input.base_.isEmpty() ? "compare3" : "compareBase3";
                }
                result.exitCode_ = different ? 1 : 0; result.outcome_ = different ? "Different" : "Equal";
                if (Input.mode_ == "merge")
                {
                    inspectOutput(Input, result, limits);
                    if (Input.base_.isEmpty())
                    {
                        const auto seed = Input.takeAll_ == "right" ? Resolution::Right : Input.takeAll_ == "left" ? Resolution::Left : Resolution::Unresolved;
                        result.plan_ = std::make_shared<MergePlan>(TwoWayPlan(left, right, result.edits_, seed));
                        if (left != right && Input.takeAll_.isEmpty()) { result.exitCode_ = 1; result.outcome_ = "Conflicted"; result.error_ = "DecisionsRequired"; }
                        else { result.output_ = Input.takeAll_ == "right" ? result.inputs_[1].utf8_ : result.inputs_[0].utf8_; result.exitCode_ = 0; result.outcome_ = "Planned"; }
                    }
                    else
                    {
                        result.plan_ = std::make_shared<MergePlan>(ThreeWayPlan(view(result.inputs_[2].utf8_), left, right, result.merged_));
                        result.output_ = QByteArray::fromStdString(result.merged_.text_);
                        result.exitCode_ = result.merged_.conflicts_.empty() ? 0 : 1;
                        result.outcome_ = result.exitCode_ == 0 ? "Planned" : "Conflicted";
                    }
                    if (result.decisionsSnapshot_) ApplyDecisionPlan(Input, result, result.decisionsSnapshot_->raw_, limits);
                    if (!Input.planOut_.isEmpty()) writePlan(Input, result, limits);
                    if (result.exitCode_ == 0 && Save && !Input.dryRun_) { limits.Check(); SaveResult(Input, result, result.output_); }
                }
            }
            result.completion_ = "Complete";
        }
        catch (const Failure& error)
        { result.error_ = QString::fromStdString(error.code_); result.exitCode_ = error.code_ == "Canceled" ? 3 : 2; result.completion_ = result.exitCode_ == 3 ? "Canceled" : "Failed"; }
        catch (const std::exception& error)
        { result.error_ = QString::fromUtf8(error.what()); result.exitCode_ = 2; result.completion_ = "Failed"; }
        return result;
    }
    MergeWorkFile LoadMergeWorkFile(const QString& Path, const Request& Input, const Result& Session, const Budget& Limits)
    {
        auto paths = Input; paths.planOut_ = Path; checkArtifactPaths(paths, Limits);
        const auto snapshot = readSnapshot(Path, decisionPlanBytes, Limits); MergeWorkFile file;
        file.work_ = DecodeMergeWork(Input, Session, snapshot.raw_, Limits);
        for (const auto& input : Session.inputs_)
        {
            Limits.Check(); const auto current = require(reader(input.path_)->Resolve(locator(input.path_), context(Limits)).get());
            if (current.id_.revision_ != input.entry_.id_.revision_) throw Failure("InputChangedBeforeWorkLoad");
        }
        file.expected_ = {true, snapshot.entry_.id_.revision_}; Limits.Check(); return file;
    }
    MergeWorkFile SaveMergeWorkFile(const QString& Path, const Request& Input, const Result& Session, const MergeWork& Work,
        const std::optional<storage::DestinationExpectation>& Expected, bool ReplaceExisting, const Budget& Limits)
    {
        auto paths = Input; paths.planOut_ = Path; checkArtifactPaths(paths, Limits);
        const auto provider = reader(Path); const auto writer = storage::MakeNativeWritableStorage(locator(Path).kind_, provider);
        auto expected = Expected.value_or(storage::DestinationExpectation{});
        if (!Expected && ReplaceExisting)
        {
            const auto entry = require(writer->Inspect(locator(Path), context(Limits)));
            if (entry) expected = {true, entry->id_.revision_};
        }
        const auto data = EncodeMergeWork(Input, Session, Work, Limits); Limits.Check();
        // A checkpoint may preserve work based on old inputs. Loading requires a
        // matching live input generation; saving does not discard such work.
        const auto receipt = require(document::SaveDocumentBytes(*writer, locator(Path), expected,
            {reinterpret_cast<const std::byte*>(data.constData()), std::size_t(data.size())}, context(Limits)));
        if (!receipt.destinationCommitted_) throw Failure("WorkNotCommitted");
        MergeWorkFile file;
        for (const auto& error : receipt.cleanupErrors_) file.warnings_.append(QString::fromStdString(error.code_));
        for (const auto& artifact : receipt.preservedArtifacts_) file.recovery_.append(QString::fromStdString(artifact.root_));
        // Re-read the committed bytes before adopting a revision for the next save.
        // If another writer won the race, leave that revision untrusted.
        try
        {
            const auto updated = readSnapshot(Path, decisionPlanBytes, Budget{});
            if (updated.raw_ == data) file.expected_ = {true, updated.entry_.id_.revision_};
            else file.warnings_.append("WorkChangedAfterSave");
        }
        catch (const std::exception& error) { file.warnings_.append(QString::fromUtf8(error.what())); }
        return file;
    }
    void SaveResult(const Request& Input, Result& Value, const QByteArray& Utf8)
    {
        if (!Value.hasOutputExpectation_ || Value.inputs_.isEmpty()) throw Failure("MissingOutputExpectation");
        if (Value.decisionsSnapshot_ && (Value.exitCode_ != 0 || !Value.plan_ || std::any_of(Value.plan_->initial_.begin(), Value.plan_->initial_.end(),
            [](const auto& Block) { return Block.resolution_ == Resolution::Unresolved; }))) throw Failure("DecisionsRequired");
        if (Input.dryRun_) throw Failure("DryRunCannotSave");
        if (Utf8.size() > 32 * 1024 * 1024) throw Failure("ResultSizeLimit");
        Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Input.timeoutMs_);
        const auto ctx = context(limits);
        for (const auto& input : Value.inputs_)
        {
            const auto current = require(reader(input.path_)->Resolve(locator(input.path_), ctx).get());
            if (current.id_.revision_ != input.entry_.id_.revision_) throw Failure("InputChangedBeforeSave");
        }
        if (Value.decisionsSnapshot_)
        {
            const auto& decision = *Value.decisionsSnapshot_;
            const auto current = require(reader(decision.path_)->Resolve(locator(decision.path_), ctx).get());
            if (current.id_.revision_ != decision.entry_.id_.revision_) throw Failure("DecisionsChangedBeforeSave");
        }
        const auto decoded = require(document::DecodeDocumentText(Utf8, "UTF-8"));
        const auto& format = Value.inputs_[Input.takeAll_ == "right" ? 1 : 0];
        const auto encoded = require(document::EncodeDocumentText(decoded.text_, format.encoding_, format.bom_));
        const auto provider = reader(Input.output_);
        const auto writer = storage::MakeNativeWritableStorage(locator(Input.output_).kind_, provider);
        Value.commit_ = "NotStarted";
        const auto receipt = require(document::SaveDocumentBytes(*writer, locator(Input.output_), Value.expected_,
            {reinterpret_cast<const std::byte*>(encoded.constData()), std::size_t(encoded.size())}, ctx));
        if (!receipt.destinationCommitted_) throw Failure("NotCommitted");
        Value.commit_ = "Committed"; Value.outcome_ = "Saved"; Value.exitCode_ = 0;
        if (Value.planWritten_.isEmpty()) { Value.warnings_ = {}; Value.recovery_ = {}; }
        for (const auto& error : receipt.cleanupErrors_) Value.warnings_.append(QString::fromStdString(error.code_));
        for (const auto& artifact : receipt.preservedArtifacts_) Value.recovery_.append(QString::fromStdString(artifact.root_));
        const auto updated = require(writer->Inspect(locator(Input.output_), ctx));
        if (updated) Value.expected_ = {true, updated->id_.revision_};
    }
    QJsonObject Result::Json() const
    {
        QJsonArray changes, conflicts, inputs, resolutions; qsizetype unresolved = 0;
        for (const auto& input : inputs_) inputs.append(QJsonObject{{"path", input.path_}, {"encoding", input.encoding_},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(input.raw_, QCryptographicHash::Sha256).toHex())}});
        for (const auto& edit : edits_) changes.append(QJsonObject{{"begin", double(edit.begin_)}, {"end", double(edit.end_)},
            {"otherBegin", double(edit.otherBegin_)}, {"otherEnd", double(edit.otherEnd_)}});
        for (const auto& conflict : merged_.conflicts_) conflicts.append(QJsonObject{{"baseBegin", double(conflict.baseBegin_)}, {"baseEnd", double(conflict.baseEnd_)}});
        if (plan_)
        {
            const QStringList names{"unresolved", "left", "right", "base", "leftThenRight", "rightThenLeft", "manual"};
            for (std::size_t i = 0; i < plan_->initial_.size(); ++i)
            {
                const auto resolution = plan_->initial_[i].resolution_; unresolved += resolution == Resolution::Unresolved;
                resolutions.append(QJsonObject{{"block", qint64(i + 1)}, {"choice", names.at(int(resolution))}});
            }
        }
        return {{"schemaVersion", 1}, {"engineVersion", "myers-bounded-1"}, {"operation", operation_}, {"completion", completion_},
            {"outcome", outcome_}, {"exitCode", exitCode_}, {"commit", commit_}, {"error", error_},
            {"coverage", QJsonObject{{"scopeComplete", completion_ == "Complete"}}}, {"inputs", inputs},
            {"changes", changes}, {"conflicts", conflicts}, {"entries", entries_}, {"comparisons", comparisons_},
            {"planWritten", planWritten_}, {"decisionSource", decisionsSnapshot_ ? decisionsSnapshot_->path_ : QString{}},
            {"resolutions", resolutions}, {"unresolvedCount", unresolved},
            {"warnings", warnings_}, {"recovery", recovery_}};
    }
    int WriteReport(const Request& Input, const Result& Value)
    {
        QByteArray data = Input.format_ == "json" ? QJsonDocument(Value.Json()).toJson(QJsonDocument::Compact) + '\n'
            : QString("%1: %2 (exit %3)%4\n").arg(Value.operation_, Value.outcome_).arg(Value.exitCode_).arg(Value.error_.isEmpty() ? QString{} : ": " + Value.error_).toUtf8();
        if (Input.report_.isEmpty() || Input.report_ == "-")
        { QFile output; if (!output.open(stdout, QIODevice::WriteOnly) || output.write(data) != data.size() || !output.flush()) return 2; }
        else
        {
            try { checkArtifactPaths(Input, Budget{}); } catch (...) { return 2; }
            // Never replace a report that aliases source content through a hard link.
            if (QFileInfo::exists(Input.report_))
            {
                try
                {
                    const auto existing = require(reader(Input.report_)->Resolve(locator(Input.report_), {}).get());
                    for (const auto& input : Value.inputs_)
                        if (!identity(existing).isEmpty() && identity(existing) == identity(input.entry_)) return 2;
                }
                catch (...) { return 2; }
            }
            QSaveFile output(Input.report_); output.setDirectWriteFallback(false);
            if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) return 2;
        }
        return Value.exitCode_;
    }
}
