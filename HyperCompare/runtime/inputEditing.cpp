#include "stdafx.h"
#include "inputEditing.hpp"
#include "controllerMemory.hpp"
#include "saveRecovery.hpp"
#include "semantic.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include <QDir>
#include <QFileInfo>
namespace hc::compare
{
    namespace
    {
        std::string_view view(const QByteArray& Text) { return {Text.constData(), std::size_t(Text.size())}; }
        void validate(const QByteArray& Text, const Budget& Limits)
        {
            Limits.Check(); if (Text.size() > 16 * 1024 * 1024) throw Failure("DecodedSizeLimit");
            if (Text.contains('\0')) throw Failure("BinaryInputUnsupported");
            CountLines(view(Text), Limits, 300000, 256 * 1024);
        }
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
    }
    Result CompareOpenBuffers(const Request& Input, Result Previous, const QVector<QByteArray>& Texts, const Budget& OriginalLimits)
    {
        auto Limits = OriginalLimits;
        Previous.error_.clear(); Previous.pairViews_ = {}; Previous.semantic_ = {}; Previous.comparisons_ = {};
        try
        {
            ConfigureMemoryBudget(Limits, Input.memoryMiB_);
            if (Texts.size() != Previous.inputs_.size() || Texts.size() < 2 || Texts.size() > 3) throw Failure("InvalidBufferCount");
            for (int i = 0; i < Texts.size(); ++i) { validate(Texts[i], Limits); Previous.inputs_[i].utf8_ = Texts[i]; }
            const std::array<std::array<int, 2>, 3> pairs{{{0, 1}, {0, 2}, {1, 2}}};
            std::uint64_t textBytes = 0; for (const auto& text : Texts) textBytes += std::uint64_t(text.size());
            const auto reservation = ProcessControllerMemory()->ReserveController(textBytes * 3 + 32 * 1024 * 1024);
            bool equal = true;
            for (std::size_t slot = 0; slot < pairs.size(); ++slot)
            {
                if (slot && (Texts.size() != 3 || !Input.base_.isEmpty())) continue;
                const auto [a, b] = pairs[slot]; const auto edits = Diff(view(Texts[a]), view(Texts[b]), Limits);
                auto pair = std::make_shared<PairComparison>(); pair->inputs_ = {a, b};
                pair->alignment_ = std::make_shared<AlignmentMap>(AlignmentMap::Build(CountLines(view(Texts[a]), Limits), CountLines(view(Texts[b]), Limits), edits, Limits));
                for (const auto& edit : edits) pair->changes_.push_back({edit.begin_, edit.end_, edit.otherBegin_, edit.otherEnd_});
                auto decoration = Limits; decoration.deadline_ = std::min(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
                pair->inline_ = BuildInlineDiff(view(Texts[a]), view(Texts[b]), edits, decoration);
                Previous.pairViews_[slot] = std::move(pair); equal &= Texts[a] == Texts[b];
                if (!slot) Previous.edits_ = edits;
                Previous.comparisons_.append(QJsonObject{{"fromFile", a + 1}, {"toFile", b + 1}, {"changeCount", qint64(edits.size())}});
            }
            bool rawEqual = true;
            for (int i = 1; i < Texts.size(); ++i) { equal &= Texts[i] == Texts[0]; rawEqual &= Previous.inputs_[i].raw_ == Previous.inputs_[0].raw_; }
            Previous.exitCode_ = equal && (Input.textEquality_ != "bytes" || rawEqual) ? 0 : 1;
            Previous.outcome_ = equal ? "EqualBuffers" : "DifferentBuffers";
            Previous.textComparison_ = {{"scope", "workingBuffers"}, {"contentEqual", equal}, {"encodingOnly", equal && !rawEqual}};
            Previous.completion_ = "Complete"; AnalyzeSourceRelations(Input, Previous, Limits);
        }
        catch (const std::exception& error) { Previous.error_ = QString::fromUtf8(error.what()); Previous.completion_ = "Failed"; Previous.exitCode_ = 2; }
        return Previous;
    }
    InputSaveResult SaveInputBuffer(const Snapshot& Original, const QByteArray& Utf8, const Budget& Limits)
    {
        InputSaveResult result; result.snapshot_ = Original;
        try
        {
            validate(Utf8, Limits);
            const auto text = require(document::DecodeDocumentText(Utf8, "UTF-8"));
            const auto encoded = require(document::EncodeDocumentText(text.text_, Original.encoding_, Original.bom_));
            if (encoded.size() > 16 * 1024 * 1024) throw Failure("EncodedSizeLimit");
            const storage::Locator target{Original.path_.startsWith("//") || Original.path_.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Original.path_.toStdString()};
            auto writer = storage::MakeNativeWritableStorage(target.kind_, storage::MakeNativeReadProvider(target.kind_));
            SaveRecoveryJournal journal; storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.writeCheckpoint_ = journal.Checkpoint();
            if (!Original.exists_)
            {
                // Creation happens only on explicit Save. Inspect propagates access errors;
                // native MakeDirectory/BeginWrite reject reparse parents and racing files.
                QStringList parents; auto parent = QFileInfo(Original.path_).absolutePath();
                for (;;)
                {
                    Limits.Check(); const storage::Locator at{target.kind_, parent.toStdString()};
                    const auto existing = require(writer->Inspect(at, ctx));
                    if (existing) { if (!existing->directory_) throw Failure("ExpectedDirectory"); break; }
                    parents.prepend(parent); const auto next = QFileInfo(parent).absolutePath();
                    if (next == parent) throw Failure("MissingDestinationRoot"); parent = next;
                }
                for (const auto& path : parents) { Limits.Check(); require(writer->MakeDirectory({target.kind_, path.toStdString()}, ctx)); }
            }
            const auto saved = document::SaveDocumentBytes(*writer, target, {Original.exists_, Original.entry_.id_.revision_},
                {reinterpret_cast<const std::byte*>(encoded.constData()), std::size_t(encoded.size())}, ctx);
            if (journal.Exists()) result.recovery_.append(journal.Path());
            const auto receipt = require(saved); result.committed_ = receipt.destinationCommitted_;
            for (const auto& error : receipt.cleanupErrors_) result.warnings_.append(QString::fromStdString(error.code_));
            for (const auto& artifact : receipt.preservedArtifacts_) result.recovery_.append(QString::fromStdString(artifact.root_));
            if (!result.committed_) throw Failure("NotCommitted");
            const auto updated = ReadTextSnapshot(Original.path_, Original.encoding_, Limits);
            if (updated.raw_ != encoded) throw Failure("InputChangedAfterSave");
            result.snapshot_ = updated;
            if (receipt.cleanupErrors_.empty() && receipt.preservedArtifacts_.empty())
            {
                if (journal.Remove()) result.recovery_ = {};
                else result.warnings_.append("SaveRecoveryJournalCleanupFailed");
            }
        }
        catch (const std::exception& error) { result.error_ = QString::fromUtf8(error.what()); }
        return result;
    }
}
