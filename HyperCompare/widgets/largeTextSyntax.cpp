#include "stdafx.h"
#include "largeTextSyntax.hpp"
#include <ILexer.h>
#include <Lexilla.h>
#include <QFile>
#include <algorithm>
#include <cstring>

namespace hc::compare
{
    class LargeTextSyntax final : public Scintilla::IDocument
    {
    public:
        std::shared_ptr<const LargeTextIndex> index_;
        QByteArray name_;
        bool valid_ = true;
        std::uint64_t styled_ = 0;
        const Budget* limits_ = nullptr;
        LargeTextSyntax(std::shared_ptr<const LargeTextIndex> Index, QByteArray Name)
            : index_(std::move(Index)), name_(std::move(Name)), states_(index_->lines_.size() + 1, 0)
        {
            lexer_.reset(CreateLexer(name_.constData())); if (!lexer_) throw Failure("LargeTextLexerUnavailable");
            const auto& piece = index_->contiguous_;
            if (piece.bytes_) {
                source_.setFileName(piece.backing_->file_.fileName());
                if (!source_.open(QIODevice::ReadOnly) || !(text_ = source_.map(qint64(piece.begin_), qint64(piece.bytes_)))) throw Failure("LargeTextSyntaxMapFailed");
                if (!styles_.open()) throw Failure("LargeTextSyntaxMapFailed");
            }
            lexer_->WordListSet(0, "auto bool break case catch class const constexpr continue def default delete do else enum explicit extern false float for from if import in int lambda namespace new noexcept nullptr private protected public raise return static struct switch template this throw true try typedef typename using virtual void while with yield");
        }
        ~LargeTextSyntax() { if (text_) source_.unmap(text_); if (style_) styles_.unmap(style_); }
        int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
        void SCI_METHOD SetErrorStatus(int Status) override { if (Status) throw Failure("LargeTextLexerFailed"); }
        Sci_Position SCI_METHOD Length() const override { return Sci_Position(index_->contiguous_.bytes_); }
        void SCI_METHOD GetCharRange(char* Buffer, Sci_Position Position, Sci_Position Count) const override {
            check(); if (Position < 0 || Count < 0 || Position > Length() || Count > Length() - Position) throw Failure("LargeTextLexerRange");
            if (Count) std::memcpy(Buffer, text_ + Position, std::size_t(Count));
        }
        char SCI_METHOD StyleAt(Sci_Position Position) const override { return Position >= 0 && std::uint64_t(Position) < styleBytes_ ? char(style_[Position]) : 0; }
        Sci_Position SCI_METHOD LineFromPosition(Sci_Position Position) const override {
            if (Position >= Length() && Length() && text_[Length() - 1] != '\r' && text_[Length() - 1] != '\n') Position = Length() - 1;
            return Sci_Position(std::upper_bound(index_->lines_.begin(), index_->lines_.end(), std::uint64_t(std::clamp<Sci_Position>(Position, 0, Length()))) - index_->lines_.begin() - 1);
        }
        Sci_Position SCI_METHOD LineStart(Sci_Position Line) const override { return Line < 0 ? 0 : Line >= Sci_Position(index_->lines_.size()) ? Length() : Sci_Position(index_->lines_[Line]); }
        int SCI_METHOD GetLevel(Sci_Position) const override { return 0x400; }
        int SCI_METHOD SetLevel(Sci_Position, int) override { return 0x400; }
        int SCI_METHOD GetLineState(Sci_Position Line) const override { return Line >= 0 && Line < Sci_Position(states_.size()) ? states_[Line] : 0; }
        int SCI_METHOD SetLineState(Sci_Position Line, int State) override { check(); if (Line >= 0 && Line < Sci_Position(states_.size())) states_[Line] = State; return State; }
        void SCI_METHOD StartStyling(Sci_Position Position) override { at_ = Position; }
        bool SCI_METHOD SetStyleFor(Sci_Position Count, char Style) override { return put(Count, nullptr, Style); }
        bool SCI_METHOD SetStyles(Sci_Position Count, const char* Styles) override { return put(Count, Styles, 0); }
        void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
        void SCI_METHOD DecorationFillRange(Sci_Position, int, Sci_Position) override {}
        void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
        int SCI_METHOD CodePage() const override { return 65001; }
        bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
        const char* SCI_METHOD BufferPointer() override { return text_ ? reinterpret_cast<const char*>(text_) : ""; }
        int SCI_METHOD GetLineIndentation(Sci_Position Line) override {
            int amount = 0; for (auto at = LineStart(Line); at < Length(); ++at) { if (!(at & 4095)) check(); if (text_[at] == ' ') ++amount; else if (text_[at] == '\t') amount = (amount / 8 + 1) * 8; else break; } return amount;
        }
        Sci_Position SCI_METHOD LineEnd(Sci_Position Line) const override {
            auto end = LineStart(Line + 1); if (end > LineStart(Line) && text_[end - 1] == '\n') --end; if (end > LineStart(Line) && text_[end - 1] == '\r') --end; return end;
        }
        Sci_Position SCI_METHOD GetRelativePosition(Sci_Position Start, Sci_Position Offset) const override {
            while (Offset < 0) { if (Start <= 0) return -1; --Start; while (Start && (text_[Start] & 0xc0) == 0x80) --Start; ++Offset; }
            while (Offset > 0) { if (Start >= Length()) return -1; Sci_Position width = 1; GetCharacterAndWidth(Start, &width); Start += width; --Offset; } return Start;
        }
        int SCI_METHOD GetCharacterAndWidth(Sci_Position Position, Sci_Position* Width) const override {
            if (Width) *Width = 1; if (Position < 0 || Position >= Length()) return 0;
            const auto first = text_[Position]; const int width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            if (Width) *Width = width; if (width == 1) return first;
            int code = first & (0x7f >> width); for (int i = 1; i < width && Position + i < Length(); ++i) code = (code << 6) | (text_[Position + i] & 0x3f); return code;
        }
        QByteArray Styles(std::uint64_t Begin, std::uint64_t End, const Budget& Limits) {
            limits_ = &Limits;
            if (End > styled_) {
                valid_ = false;
                auto target = std::min<std::uint64_t>(index_->contiguous_.bytes_, End + 4096);
                const auto nextLine = std::uint64_t(LineStart(LineFromPosition(Sci_Position(End)) + 1));
                if (nextLine >= End && nextLine - End <= largeTextPageMaximum) target = std::max(target, nextLine);
                growStyles(target);
                // Restart at the last incomplete line. The lexer retains its
                // own state (including C++ raw-string delimiters) and sees all
                // previous bytes/styles/line states, even at a page boundary.
                const auto restart = LineStart(LineFromPosition(Sci_Position(styled_)));
                lexer_->Lex(Sci_PositionU(restart), Sci_Position(target) - restart, restart ? uchar(StyleAt(restart - 1)) : 0, this);
                Limits.Check(); styled_ = target; valid_ = true;
            }
            QByteArray result; if (End > Begin) result = QByteArray(reinterpret_cast<const char*>(style_ + Begin), qsizetype(End - Begin));
            limits_ = nullptr; return result;
        }
    private:
        void check() const { if (limits_) limits_->Check(); }
        void growStyles(std::uint64_t Required) {
            if (Required <= styleBytes_) return;
            check(); const auto capacity = std::min<std::uint64_t>(index_->contiguous_.bytes_, std::max({Required, styleBytes_ * 2, std::uint64_t(64 * 1024)}));
            if (style_ && !styles_.unmap(style_)) throw Failure("LargeTextSyntaxMapFailed"); style_ = nullptr;
            if (!styles_.resize(qint64(capacity)) || !(style_ = styles_.map(0, qint64(capacity)))) throw Failure("LargeTextSyntaxMapFailed");
            styleBytes_ = capacity;
        }
        bool put(Sci_Position Count, const char* Styles, char Style) {
            if (Count < 0 || at_ < 0 || at_ > Length() || Count > Length() - at_) throw Failure("LargeTextLexerRange");
            growStyles(std::uint64_t(at_ + Count));
            for (Sci_Position offset = 0; offset < Count; offset += 64 * 1024) { check(); const auto size = std::size_t(std::min<Sci_Position>(64 * 1024, Count - offset)); if (Styles) std::memcpy(style_ + at_ + offset, Styles + offset, size); else std::memset(style_ + at_ + offset, uchar(Style), size); }
            at_ += Count; return true;
        }
        QFile source_;
        QTemporaryFile styles_;
        uchar* text_ = nullptr;
        uchar* style_ = nullptr;
        std::uint64_t styleBytes_ = 0;
        std::unique_ptr<Scintilla::ILexer5, void(*)(Scintilla::ILexer5*)> lexer_{nullptr, [](Scintilla::ILexer5* Value) { if (Value) Value->Release(); }};
        std::vector<int> states_;
        Sci_Position at_ = 0;
    };
    QByteArray StyleLargeText(const LargeTextDocument& Document, const QByteArray& Lexer, std::uint64_t Begin, std::uint64_t End,
        std::shared_ptr<LargeTextSyntax>& Cache, const Budget& Limits)
    {
        Limits.Check(); if (Begin > End || End > Document.bytes_ || End - Begin > largeTextPageMaximum) throw Failure("LargeTextRangeInvalid");
        if (Lexer == "null") return QByteArray(qsizetype(End - Begin), 0);
        if (!Cache || !Cache->valid_ || Cache->index_ != Document.index_ || Cache->name_ != Lexer) {
            const auto indexed = IndexLargeText(Document, Limits);
            const auto reservation = Limits.memoryReserve_ ? Limits.memoryReserve_((indexed.index_->lines_.size() + 1) * sizeof(int) + 4 * 1024 * 1024) : std::shared_ptr<void>{};
            Cache = std::make_shared<LargeTextSyntax>(indexed.index_, Lexer);
        }
        return Cache->Styles(Begin, End, Limits);
    }
}
