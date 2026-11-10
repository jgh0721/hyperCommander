#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtGlobal>
#include <limits>

namespace hc::document
{
    // Bytes of a text that changed since a base revision, merged from the editor's insert and delete
    // notifications (ITextEditorBackend::textEdited) in O(1) each: [Start(), BaseLength() - tail) of
    // the base became [Start(), InsertedEnd()) of the current text. Edits in several places merge
    // into one range that covers them. A notification that does not fit the tracked length (a text
    // replaced behind the editor's back) makes the range invalid; the caller then sends the whole text.
    class DocumentEditRange
    {
    public:
        void Reset(qint64 Length) { base_ = length_ = Length; start_ = tail_ = Unset; valid_ = Length >= 0; }
        void Invalidate() { valid_ = false; }
        void Apply(qint64 Position, qint64 Removed, qint64 Inserted)
        {
            if(!valid_) return;
            if(Position < 0 || Removed < 0 || Inserted < 0 || Position > length_ - Removed) { valid_ = false; return; }
            start_ = qMin(start_, Position); tail_ = qMin(tail_, length_ - Position - Removed); length_ += Inserted - Removed;
        }
        bool Valid() const { return valid_; }
        bool Changed() const { return start_ != Unset; }
        qint64 BaseLength() const { return base_; }
        qint64 Length() const { return length_; }
        qint64 Start() const { return Changed() ? start_ : 0; }
        qint64 Removed() const { return Changed() ? base_ - start_ - tail_ : 0; }
        qint64 InsertedEnd() const { return Changed() ? length_ - tail_ : 0; }
    private:
        static constexpr qint64 Unset = std::numeric_limits<qint64>::max();
        qint64 base_ = 0, length_ = 0, start_ = Unset, tail_ = Unset;
        bool valid_ = false;
    };
}
