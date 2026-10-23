#include "stdafx.h"
#include "mergeController.hpp"
#include <ScintillaEditBase.h>

namespace hc::compare
{
    MergeController::MergeController(ScintillaEditBase* Editor, std::shared_ptr<const MergePlan> Plan, QObject* Parent, std::size_t HistoryBytes)
        : QObject(Parent), editor_(Editor), model_(std::move(Plan)), historyLimit_(HistoryBytes)
    {
        remember();
        connect(editor_, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags Type,
            Scintilla::Position Position, Scintilla::Position Length, Scintilla::Position, const QByteArray&,
            Scintilla::Position, Scintilla::FoldLevel, Scintilla::FoldLevel) {
            modified(static_cast<int>(Type), static_cast<std::size_t>(Position), static_cast<std::size_t>(Length));
        });
    }
    void MergeController::remember()
    {
        const int at = static_cast<int>(editor_->send(SCI_GETUNDOCURRENT));
        const auto bytes = model_.State().size() * sizeof(BlockState) + 64;
        auto next = history_.lower_bound(at);
        for (auto it = next; it != history_.end(); ++it) historyBytes_ -= it->second.state_.size() * sizeof(BlockState) + 64;
        history_.erase(next, history_.end()); history_[at] = {model_.State(), model_.ReviewRequired()}; historyBytes_ += bytes;
        // Keep native text Undo intact. Older metadata beyond the budget becomes
        // untrusted if revisited, requiring explicit whole-result review or recompare.
        while (historyBytes_ > historyLimit_ && history_.size() > 1)
        { auto it = history_.begin(); historyBytes_ -= it->second.state_.size() * sizeof(BlockState) + 64; history_.erase(it); }
    }
    void MergeController::modified(int Type, std::size_t Position, std::size_t Length)
    {
        if (applying_) return;
        if (Type & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO))
        {
            if (!(Type & SC_LASTSTEPINUNDOREDO)) return;
            const auto at = history_.find(static_cast<int>(editor_->send(SCI_GETUNDOCURRENT)));
            if (at == history_.end()) model_.Invalidate(); else model_.Restore(at->second.state_, at->second.reviewRequired_);
        }
        else if (Type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
        {
            model_.TextChanged(Position, Type & SC_MOD_DELETETEXT ? Length : 0, Type & SC_MOD_INSERTTEXT ? Length : 0);
            remember();
        }
        else return;
        if (changed_) changed_();
    }
    void MergeController::replace(std::size_t Begin, std::size_t End, const QByteArray& Text, MergeModel Next, bool TextChange)
    {
        if (editor_->send(SCI_GETREADONLY)) throw Failure("ResultBusy");
        const auto length = static_cast<std::size_t>(editor_->send(SCI_GETLENGTH));
        if (Begin > End || End > length) throw Failure("InvalidMergeLocation");
        if (TextChange && length - (End - Begin) + std::size_t(Text.size()) > 32 * 1024 * 1024) throw Failure("ResultSizeLimit");
        applying_ = true; editor_->send(SCI_SETSTATUS, SC_STATUS_OK); editor_->send(SCI_BEGINUNDOACTION);
        auto written = sptr_t(Text.size());
        if (TextChange)
        {
            editor_->send(SCI_SETTARGETSTART, Begin); editor_->send(SCI_SETTARGETEND, End);
            written = editor_->send(SCI_REPLACETARGET, uptr_t(Text.size()), reinterpret_cast<sptr_t>(Text.constData()));
        }
        // A metadata-only decision must be undoable even when the text is unchanged.
        editor_->send(SCI_ADDUNDOACTION, 0, 0); editor_->send(SCI_ENDUNDOACTION);
        const auto status = editor_->send(SCI_GETSTATUS);
        if (status == SC_STATUS_FAILURE || status == SC_STATUS_BADALLOC || (TextChange && (written != Text.size() ||
            std::size_t(editor_->send(SCI_GETLENGTH)) != length - (End - Begin) + std::size_t(Text.size()))))
        {
            model_.Invalidate(); applying_ = false; remember(); if (changed_) changed_(); throw Failure("ResultEditFailed");
        }
        model_ = std::move(Next); applying_ = false; remember();
        if (changed_) changed_();
    }
    void MergeController::Choose(std::uint64_t Id, std::uint64_t Revision, Resolution Choice)
    {
        const auto edit = model_.Prepare(Id, Revision, Choice); auto next = model_; next.Apply(edit);
        replace(edit.begin_, edit.end_, QByteArray::fromStdString(edit.replacement_), std::move(next), Choice != Resolution::Manual);
    }
    void MergeController::ReplaceRange(std::uint64_t Id, std::uint64_t Revision, std::size_t Begin, std::size_t End, const QByteArray& Text)
    {
        const auto block = model_.Prepare(Id, Revision, Resolution::Manual);
        if (Begin < block.begin_ || End > block.end_ || Begin > End) throw Failure("InvalidMergeLocation");
        auto next = model_; next.TextChanged(Begin, End - Begin, std::size_t(Text.size()));
        // A partial source choice cannot certify the rest of a conflict. Keep
        // it unresolved, with text and decision metadata in the same undo step.
        replace(Begin, End, Text, std::move(next), true);
    }
    void MergeController::ChooseAll(Resolution Choice, const QByteArray& Text)
    {
        auto next = model_; next.SelectAll(Choice);
        replace(0, std::size_t(editor_->send(SCI_GETLENGTH)), Text, std::move(next), Choice != Resolution::Manual);
    }
    void MergeController::RestoreWork(const QByteArray& Text, const std::vector<BlockState>& State, bool ReviewRequired)
    {
        auto next = model_; next.Restore(State, ReviewRequired);
        replace(0, std::size_t(editor_->send(SCI_GETLENGTH)), Text, std::move(next), true);
    }
    void MergeController::Refresh(std::shared_ptr<const MergePlan> Plan)
    {
        MergeModel next(std::move(Plan)); next.Restore(next.State(), true);
        // Old text Undo remains available. Decisions from the old input generation
        // cannot become trusted again by traversing its native Undo/Redo history.
        history_.clear(); historyBytes_ = 0; model_ = std::move(next); saved_ = false;
        remember();
        if (changed_) changed_();
    }
    void MergeController::MarkSaved() { saved_ = true; editor_->send(SCI_SETSAVEPOINT); if (changed_) changed_(); }
    bool MergeController::Dirty() const { return editor_->send(SCI_GETMODIFY) != 0 || model_.ReviewRequired(); }
    bool MergeController::Saved() const { return saved_ && !Dirty() && model_.UnresolvedCount() == 0; }
}
