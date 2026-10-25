#include "stdafx.h"
#include "largeText.hpp"
#include "requestFile.hpp"
#include "directoryPlan.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include <QFileInfo>
#include <algorithm>

namespace hc::compare
{
    namespace
    {
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        storage::OperationContext context(const Budget& Limits)
        { storage::OperationContext value; value.workerRead_ = true; value.denyConcurrentMutation_ = true; value.deadline_ = Limits.deadline_; return value; }
        LargeTextDocument range(const LargeTextDocument& Source, std::uint64_t Begin, std::uint64_t End, const Budget& Limits)
        { return ReplaceLargeText({}, 0, 0, Source, Begin, End, Limits); }
        void append(LargeTextDocument& Target, const LargeTextDocument& Source)
        {
            if (Source.bytes_ > largeTextMaximum - Target.bytes_) throw Failure("LargeTextSizeLimit");
            Target.bytes_ += Source.bytes_;
            for (const auto& piece : Source.pieces_) {
                if (!Target.pieces_.empty() && Target.pieces_.back().backing_ == piece.backing_ && Target.pieces_.back().begin_ + Target.pieces_.back().bytes_ == piece.begin_)
                    Target.pieces_.back().bytes_ += piece.bytes_;
                else Target.pieces_.push_back(piece);
                if (Target.pieces_.size() > 100000) throw Failure("LargeTextPieceLimit");
            }
        }
        bool equal(const LargeTextDocument& A, const LargeTextDocument& B, const Budget& Limits)
        {
            if (A.bytes_ != B.bytes_) return false;
            for (std::uint64_t offset = 0; offset < A.bytes_; offset += 64 * 1024) {
                Limits.Check(); const auto end = std::min(offset + 64 * 1024, A.bytes_);
                if (ReadLargeTextRange(A, offset, end, Limits) != ReadLargeTextRange(B, offset, end, Limits)) return false;
            }
            return true;
        }
        bool samePieces(const LargeTextDocument& A, const LargeTextDocument& B)
        {
            if (A.bytes_ != B.bytes_ || A.pieces_.size() != B.pieces_.size()) return false;
            for (std::size_t i = 0; i < A.pieces_.size(); ++i) if (A.pieces_[i].backing_ != B.pieces_[i].backing_ || A.pieces_[i].begin_ != B.pieces_[i].begin_ || A.pieces_[i].bytes_ != B.pieces_[i].bytes_) return false;
            return true;
        }
        bool overlaps(const Edit& A, const Edit& B)
        {
            if (A.begin_ == A.end_ && B.begin_ == B.end_) return A.begin_ == B.begin_;
            if (A.begin_ == A.end_) return B.begin_ < A.begin_ && A.begin_ < B.end_;
            if (B.begin_ == B.end_) return A.begin_ < B.begin_ && B.begin_ < A.end_;
            return std::max(A.begin_, B.begin_) < std::min(A.end_, B.end_);
        }
        LargeTextDocument variant(const LargeTextDocument& Base, const LargeTextDocument& Side,
            const std::vector<std::uint64_t>& BaseLines, const std::vector<std::uint64_t>& SideLines,
            std::size_t Begin, std::size_t End, const std::vector<const Edit*>& Changes, const Budget& Limits)
        {
            LargeTextDocument result; auto cursor = Begin;
            for (const auto* edit : Changes) {
                append(result, range(Base, BaseLines[cursor], BaseLines[edit->begin_], Limits));
                append(result, range(Side, SideLines[edit->otherBegin_], SideLines[edit->otherEnd_], Limits)); cursor = edit->end_;
            }
            append(result, range(Base, BaseLines[cursor], BaseLines[End], Limits)); return result;
        }
        void merge(LargeTextSession& Session, const Budget& Limits)
        {
            const auto& base = Session.documents_[2]; const auto& left = Session.documents_[0]; const auto& right = Session.documents_[1];
            const auto a = CompareLargeText({base, left}, Limits), b = CompareLargeText({base, right}, Limits);
            struct Tagged { const Edit* edit_; bool left_; }; std::vector<Tagged> all;
            const auto admission = Limits.memoryReserve_ ? Limits.memoryReserve_((a.changes_.size() + b.changes_.size()) * 4096) : std::shared_ptr<void>{};
            for (const auto& edit : a.changes_) all.push_back({&edit, true}); for (const auto& edit : b.changes_) all.push_back({&edit, false});
            std::stable_sort(all.begin(), all.end(), [](const Tagged& A, const Tagged& B) { return A.edit_->begin_ == B.edit_->begin_ ? A.edit_->end_ < B.edit_->end_ : A.edit_->begin_ < B.edit_->begin_; });
            LargeTextDocument result; result.original_ = left.original_; result.rawHash_.clear(); std::size_t cursor = 0;
            for (std::size_t i = 0; i < all.size();) {
                Limits.Check(); auto end = i + 1; const auto first = all[i].edit_->begin_; auto last = all[i].edit_->end_;
                while (end < all.size() && overlaps({first, last}, *all[end].edit_)) { last = std::max(last, all[end].edit_->end_); ++end; }
                std::vector<const Edit*> ls, rs; for (auto j = i; j < end; ++j) (all[j].left_ ? ls : rs).push_back(all[j].edit_);
                const auto l = variant(base, left, a.lines_[0], a.lines_[1], first, last, ls, Limits);
                const auto r = variant(base, right, b.lines_[0], b.lines_[1], first, last, rs, Limits);
                append(result, range(base, a.lines_[0][cursor], a.lines_[0][first], Limits));
                if (ls.empty()) append(result, r);
                else if (rs.empty() || equal(l, r, Limits)) append(result, l);
                else {
                    LargeTextConflict conflict; conflict.base_ = range(base, a.lines_[0][first], a.lines_[0][last], Limits); conflict.left_ = l; conflict.right_ = r; conflict.begin_ = result.bytes_;
                    append(result, conflict.base_); conflict.end_ = result.bytes_; Session.conflicts_.push_back(std::move(conflict));
                }
                cursor = last; i = end;
            }
            append(result, range(base, a.lines_[0][cursor], base.bytes_, Limits)); Session.documents_.push_back(std::move(result)); Session.titles_ << "RESULT"; Session.result_ = 3;
        }
    }
    std::shared_ptr<LargeTextComparison> PairLargeTextSession(LargeTextSession Session, std::array<int, 2> Pair, const Budget& Limits)
    {
        if (Pair[0] < 0 || Pair[1] < 0 || Pair[0] == Pair[1] || std::size_t(std::max(Pair[0], Pair[1])) >= Session.documents_.size()) throw Failure("LargeTextPairInvalid");
        auto value = std::make_shared<LargeTextComparison>(CompareLargeText({Session.documents_[Pair[0]], Session.documents_[Pair[1]]}, Limits));
        for (int side = 0; side < 2; ++side) Session.documents_[Pair[side]].contentHash_ = value->documents_[side].contentHash_;
        Session.comparisons_ = {};
        if (Session.result_ < 0) for (int a = 0; a < int(Session.documents_.size()); ++a) for (int b = a + 1; b < int(Session.documents_.size()); ++b) {
            auto count = value->changes_.size();
            if (Pair != std::array{a, b} && Pair != std::array{b, a}) { const auto comparison = CompareLargeText({Session.documents_[a], Session.documents_[b]}, Limits); count = comparison.changes_.size(); Session.documents_[a].contentHash_ = comparison.documents_[0].contentHash_; Session.documents_[b].contentHash_ = comparison.documents_[1].contentHash_; }
            Session.comparisons_.append(QJsonObject{{"fromFile", a + 1}, {"toFile", b + 1}, {"changeCount", double(count)}});
        }
        Session.pair_ = Pair; value->session_ = std::make_shared<LargeTextSession>(std::move(Session)); return value;
    }
    std::shared_ptr<LargeTextComparison> OpenLargeTextSession(const Request& Input, const Budget& Limits)
    {
        LargeTextSession session; session.documents_.push_back(ReadLargeText(Input.file1_, InputEncoding(Input, 0), Limits, Input.allowMissingInputs_[0]));
        session.documents_.push_back(ReadLargeText(Input.file2_, InputEncoding(Input, 1), Limits, Input.allowMissingInputs_[1])); session.titles_ = {"FILE 1", "FILE 2"};
        if (!Input.base_.isEmpty() || !Input.file3_.isEmpty()) {
            session.documents_.push_back(ReadLargeText(Input.base_.isEmpty() ? Input.file3_ : Input.base_, InputEncoding(Input, 2), Limits)); session.titles_ << (Input.base_.isEmpty() ? "FILE 3" : "BASE");
        }
        std::size_t total = 0;
        for (const auto& document : session.documents_) {
            bool previousCr = false, tail = false;
            StreamLargeText(document, Limits, [&](QByteArrayView Bytes) { for (const auto byte : Bytes) {
                if (byte == '\r') { ++total; previousCr = true; tail = false; }
                else if (byte == '\n') { if (!previousCr) ++total; previousCr = false; tail = false; }
                else { previousCr = false; tail = true; }
            } if (total > 2'000'000) throw Failure("LineLimit"); });
            if (tail && ++total > 2'000'000) throw Failure("LineLimit");
        }
        if (Input.mode_ == "merge") {
            if (!Input.base_.isEmpty()) merge(session, Limits);
            else {
                const auto comparison = CompareLargeText({session.documents_[0], session.documents_[1]}, Limits);
                auto output = session.documents_[Input.takeAll_ == "right" ? 1 : 0];
                if (Input.takeAll_.isEmpty()) for (const auto& edit : comparison.changes_) {
                    LargeTextConflict conflict; conflict.base_ = range(session.documents_[0], comparison.lines_[0][edit.begin_], comparison.lines_[0][edit.end_], Limits);
                    conflict.left_ = conflict.base_; conflict.right_ = range(session.documents_[1], comparison.lines_[1][edit.otherBegin_], comparison.lines_[1][edit.otherEnd_], Limits);
                    conflict.begin_ = comparison.lines_[0][edit.begin_]; conflict.end_ = comparison.lines_[0][edit.end_]; session.conflicts_.push_back(std::move(conflict));
                }
                session.result_ = 2; session.documents_.push_back(std::move(output)); session.titles_ << "RESULT";
            }
            auto& original = session.documents_[session.result_].original_; original.path_.clear(); original.exists_ = false; original.entry_ = {};
            auto result = PairLargeTextSession(std::move(session), {0, Input.base_.isEmpty() ? 2 : 3}, Limits);
            if (!Input.output_.isEmpty()) result = BindLargeTextOutput(*result, Input.output_, Input.overwrite_ || !Input.headless_, Input, Limits);
            return result;
        }
        return PairLargeTextSession(std::move(session), {0, 1}, Limits);
    }
    std::shared_ptr<LargeTextComparison> UpdateLargeTextSession(const LargeTextComparison& Previous, std::array<LargeTextDocument, 2> Documents, const Budget& Limits, bool Manual)
    {
        if (!Previous.session_) return std::make_shared<LargeTextComparison>(CompareLargeText(std::move(Documents), Limits));
        auto session = *Previous.session_;
        for (int side = 0; side < 2; ++side) {
            const auto index = session.pair_[side];
            if (samePieces(session.documents_[index], Documents[side]) && Manual) continue;
            if (Manual && session.result_ >= 0 && index != session.result_) throw Failure("LargeTextMergeSourceReadOnly");
            session.documents_[index] = std::move(Documents[side]);
            if (Manual && index == session.result_) session.reviewRequired_ = !session.conflicts_.empty();
        }
        return PairLargeTextSession(std::move(session), Previous.session_->pair_, Limits);
    }
    std::shared_ptr<LargeTextComparison> ResolveLargeTextConflict(const LargeTextComparison& Previous, std::size_t Index, Resolution Choice, const Budget& Limits)
    {
        if (!Previous.session_ || Previous.session_->result_ < 0 || Index >= Previous.session_->conflicts_.size() || Previous.session_->reviewRequired_) throw Failure("LargeTextConflictReviewRequired");
        auto session = *Previous.session_; auto& conflict = session.conflicts_[Index]; LargeTextDocument replacement;
        switch (Choice) {
        case Resolution::Left: replacement = conflict.left_; break;
        case Resolution::Right: replacement = conflict.right_; break;
        case Resolution::Base: replacement = conflict.base_; break;
        case Resolution::Omit: break;
        case Resolution::LeftThenRight: append(replacement, conflict.left_); append(replacement, conflict.right_); break;
        case Resolution::RightThenLeft: append(replacement, conflict.right_); append(replacement, conflict.left_); break;
        default: throw Failure("LargeTextConflictChoiceInvalid");
        }
        auto& result = session.documents_[session.result_]; result = ReplaceLargeText(result, conflict.begin_, conflict.end_, replacement, 0, replacement.bytes_, Limits);
        const auto oldEnd = conflict.end_; conflict.end_ = conflict.begin_ + replacement.bytes_; conflict.choice_ = Choice;
        const auto delta = std::int64_t(conflict.end_) - std::int64_t(oldEnd);
        for (auto i = Index + 1; i < session.conflicts_.size(); ++i) { session.conflicts_[i].begin_ += delta; session.conflicts_[i].end_ += delta; }
        return PairLargeTextSession(std::move(session), Previous.session_->pair_, Limits);
    }
    std::shared_ptr<LargeTextComparison> BindLargeTextOutput(const LargeTextComparison& Previous, const QString& Path, bool ReplaceExisting, const Request& Input, const Budget& Limits)
    {
        if (!Previous.session_ || Previous.session_->result_ < 0 || Path.isEmpty()) throw Failure("LargeTextOutputRequired");
        auto session = *Previous.session_; const auto path = CheckedDirectoryPath(Path); auto protectedInput = Input;
        for (int i = 0; i < session.result_; ++i) protectedInput.protectedInputs_.append(session.documents_[i].original_.path_);
        CheckProtectedInputs(protectedInput, {path}, Limits);
        const auto at = locator(path); const auto writer = storage::MakeNativeWritableStorage(at.kind_, storage::MakeNativeReadProvider(at.kind_)); const auto entry = require(writer->Inspect(at, context(Limits)));
        if (entry && (!ReplaceExisting || entry->directory_ || entry->attributes_ & FILE_ATTRIBUTE_REPARSE_POINT)) throw Failure("LargeTextOutputOverwriteRequired");
        auto& original = session.documents_[session.result_].original_; original.path_ = path; original.exists_ = bool(entry); original.entry_ = entry.value_or(storage::EntrySnapshot{});
        return PairLargeTextSession(std::move(session), Previous.session_->pair_, Limits);
    }
    std::size_t LargeTextUnresolved(const LargeTextComparison& Value)
    { if (!Value.session_) return 0; return std::count_if(Value.session_->conflicts_.begin(), Value.session_->conflicts_.end(), [](const auto& Conflict) { return Conflict.choice_ == Resolution::Unresolved; }); }
    void ValidateLargeTextInputs(const LargeTextSession& Session, const Budget& Limits)
    {
        for (std::size_t i = 0; i < Session.documents_.size(); ++i) {
            if (int(i) == Session.result_) continue; Limits.Check(); const auto& original = Session.documents_[i].original_; const auto at = locator(original.path_);
            const auto writer = storage::MakeNativeWritableStorage(at.kind_, storage::MakeNativeReadProvider(at.kind_)); const auto current = require(writer->Inspect(at, context(Limits)));
            if (bool(current) != original.exists_ || (current && current->id_.revision_ != original.entry_.id_.revision_)) throw Failure("InputChangedBeforeSave");
        }
    }
    QJsonObject LargeTextSessionJson(const LargeTextComparison& Value)
    {
        QJsonArray documents, conflicts;
        if (Value.session_) {
            for (std::size_t i = 0; i < Value.session_->documents_.size(); ++i) { const auto& doc = Value.session_->documents_[i]; documents.append(QJsonObject{{"role", Value.session_->titles_[int(i)]}, {"path", doc.original_.path_}, {"encoding", doc.original_.encoding_}, {"bom", doc.original_.bom_}, {"decodedBytes", double(doc.bytes_)}}); }
            for (const auto& conflict : Value.session_->conflicts_) conflicts.append(QJsonObject{{"begin", double(conflict.begin_)}, {"end", double(conflict.end_)}, {"choice", int(conflict.choice_)}});
        }
        return {{"documents", documents}, {"conflicts", conflicts}, {"unresolved", double(LargeTextUnresolved(Value))}, {"reviewRequired", Value.session_ && Value.session_->reviewRequired_}};
    }
}
