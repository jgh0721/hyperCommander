#pragma once
#include "runtime/controllerMemory.hpp"
#include <ScintillaEditBase.h>
#include <QApplication>
#include <QClipboard>
#include <QDropEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QScopeGuard>
#include <QVariant>
#include <algorithm>
#include <cstring>
#include <Windows.h>
#include <functional>

namespace hc::compare
{
    // Guard editable growth before Scintilla changes text or records native Undo.
    // Existing text/Undo are retained when admission fails. Navigation and saving remain available.
    class BoundedEditor final : public ScintillaEditBase
    {
    public:
        explicit BoundedEditor(QWidget* Parent, int MemoryMiB, std::uint64_t DocumentBytes,
            std::shared_ptr<ControllerMemory> Pool = ProcessControllerMemory())
            : ScintillaEditBase(Parent), pool_(std::move(Pool)), memoryMiB_(MemoryMiB), documentLimit_(DocumentBytes)
        {
            connect(this, &ScintillaEditBase::modified, this, [this](Scintilla::ModificationFlags Flags,
                Scintilla::Position, Scintilla::Position Length) {
                if (!enabled_ || (int(Flags) & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) ||
                    !(int(Flags) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;
                historyBytes_ += std::uint64_t(std::max<Scintilla::Position>(0, Length));
                const auto actions = raw(SCI_GETUNDOACTIONS);
                if (actions > historyActions_) historyBytes_ += std::uint64_t(actions - historyActions_) * 64;
                historyActions_ = actions;
            });
        }
        void EnableProtection(std::uint64_t UndoBytes = 128 * 1024 * 1024)
        { enabled_ = true; historyLimit_ = UndoBytes; historyActions_ = raw(SCI_GETUNDOACTIONS); }
        struct Admission { std::shared_ptr<void> request_, bytes_; };
        Admission ReserveSnapshot(std::uint64_t Multiplier = 1) const
        {
            Admission lease; lease.request_ = pool_->Enter(std::uint64_t(memoryMiB_) * 1024 * 1024);
            lease.bytes_ = pool_->ReserveController((std::uint64_t(raw(SCI_GETLENGTH)) + 1) * Multiplier); return lease;
        }
        std::shared_ptr<void> BeginEdit(std::uint64_t Inserted, std::uint64_t Removed, bool Undo = false, bool UncertainRemoval = false, bool ProgrammaticReadOnly = false) const
        {
            if (!enabled_ || admitted_ || (raw(SCI_GETREADONLY) && !ProgrammaticReadOnly)) return {};
            const auto length = std::uint64_t(raw(SCI_GETLENGTH));
            const auto actions = raw(SCI_GETUNDOACTIONS);
            if (actions > historyActions_) historyBytes_ += std::uint64_t(actions - historyActions_) * 64;
            historyActions_ = actions;
            if (!Undo && raw(SCI_GETSELECTIONS) != 1) throw Failure("EditorSelectionLimit");
            if (!Undo && (Inserted > documentLimit_ || length - std::min(length, UncertainRemoval ? 0 : Removed) > documentLimit_ - Inserted))
                throw Failure("EditorSizeLimit");
            if (!Undo && (historyBytes_ > historyLimit_ || Inserted > historyLimit_ || Removed > historyLimit_ - Inserted ||
                Inserted + Removed + 256 > historyLimit_ - historyBytes_)) throw Failure("EditorUndoLimit");
            struct Gate {
                const BoundedEditor* editor_ = nullptr; Admission memory_;
                ~Gate() { if (editor_) editor_->admitted_ = false; }
            };
            auto gate = std::make_shared<Gate>(); gate->editor_ = nullptr;
            gate->memory_.request_ = pool_->Enter(std::uint64_t(memoryMiB_) * 1024 * 1024);
            gate->memory_.bytes_ = pool_->ReserveController(length * 4 + Inserted * 6 + Removed * 2 + 64 * 1024);
            admitted_ = true; gate->editor_ = this; return gate;
        }
        std::function<void(const QString&)> blocked_;
        sptr_t send(unsigned int Message, uptr_t W = 0, sptr_t L = 0) const override
        {
            if (!enabled_) return raw(Message, W, L);
            if (Message == SCI_EMPTYUNDOBUFFER) {
                const auto result = raw(Message, W, L); historyBytes_ = 0; historyActions_ = 0; return result;
            }
            if (admitted_ || raw(SCI_GETREADONLY)) return raw(Message, W, L);
            const auto length = std::uint64_t(raw(SCI_GETLENGTH));
            const auto selected = std::uint64_t(raw(SCI_GETSELECTIONEND) - raw(SCI_GETSELECTIONSTART));
            std::uint64_t inserted = 0, removed = 0; bool mutation = true, undo = false;
            switch (Message) {
            case SCI_ADDTEXT: case SCI_APPENDTEXT: inserted = W; break;
            case SCI_INSERTTEXT: inserted = boundedLength(L); break;
            case SCI_SETTEXT: inserted = boundedLength(L); removed = length; break;
            case SCI_REPLACESEL: inserted = boundedLength(L); removed = selected; break;
            case SCI_REPLACETARGETRE: return guarded(documentLimit_ + 1, 0, false, [] { return sptr_t(-1); });
            case SCI_REPLACETARGET: case SCI_REPLACETARGETMINIMAL:
                inserted = W == uptr_t(-1) ? boundedLength(L) : W;
                removed = std::uint64_t(std::abs(raw(SCI_GETTARGETEND) - raw(SCI_GETTARGETSTART))); break;
            case SCI_DELETERANGE: removed = std::uint64_t(std::max<sptr_t>(0, L)); break;
            case SCI_CLEAR: case SCI_CUT: removed = selected; break;
            case SCI_UNDO: case SCI_REDO: undo = true; inserted = historyBytes_; break;
            case SCI_PASTE: return paste([&] { return raw(Message, W, L); });
            default: mutation = false;
            }
            if (!mutation) return raw(Message, W, L);
            return guarded(inserted, removed, undo, [&] { return raw(Message, W, L); });
        }
        sptr_t sends(unsigned int Message, uptr_t W = 0, const char* Text = nullptr) const override
        { return send(Message, W, reinterpret_cast<sptr_t>(Text)); }
    protected:
        void keyPressEvent(QKeyEvent* Event) override
        {
            if (!enabled_ || raw(SCI_GETREADONLY)) { ScintillaEditBase::keyPressEvent(Event); return; }
            if (Event->matches(QKeySequence::Copy) || Event->matches(QKeySequence::SelectAll) ||
                Event->key() == Qt::Key_Left || Event->key() == Qt::Key_Right || Event->key() == Qt::Key_Up || Event->key() == Qt::Key_Down ||
                Event->key() == Qt::Key_Home || Event->key() == Qt::Key_End || Event->key() == Qt::Key_PageUp || Event->key() == Qt::Key_PageDown ||
                (Event->text().isEmpty() && !Event->modifiers() && Event->key() != Qt::Key_Delete && Event->key() != Qt::Key_Backspace)) {
                ScintillaEditBase::keyPressEvent(Event); return;
            }
            auto invoke = [&] { ScintillaEditBase::keyPressEvent(Event); return sptr_t(0); };
            if (Event->matches(QKeySequence::Paste)) { paste(invoke); return; }
            const bool undo = Event->matches(QKeySequence::Undo) || Event->matches(QKeySequence::Redo);
            const auto length = std::uint64_t(raw(SCI_GETLENGTH));
            auto inserted = utf8Bytes(Event->text());
            if (Event->key() == Qt::Key_Return || Event->key() == Qt::Key_Enter) inserted = 2;
            bool uncertain = false;
            auto removed = std::uint64_t(raw(SCI_GETSELECTIONEND) - raw(SCI_GETSELECTIONSTART));
            if (Event->key() == Qt::Key_Backspace || Event->key() == Qt::Key_Delete || Event->matches(QKeySequence::Cut)) {
                inserted = 0;
                if (Event->modifiers() & Qt::ControlModifier) removed = length;
                else {
                    const auto line = raw(SCI_LINEFROMPOSITION, raw(SCI_GETCURRENTPOS));
                    const auto lineBytes = std::uint64_t(std::max<sptr_t>(0, raw(SCI_LINELENGTH, uptr_t(line))));
                    removed = std::max<std::uint64_t>(removed, std::min(length, lineBytes + 4));
                }
            }
            // Unrecognised editing shortcuts may duplicate/delete whole lines.
            else if (Event->modifiers() & Qt::ControlModifier) { inserted = length; removed = length; uncertain = true; }
            if (undo) inserted = historyBytes_;
            guarded(inserted, removed, undo, invoke, uncertain);
        }
        void inputMethodEvent(QInputMethodEvent* Event) override
        {
            if (!enabled_ || raw(SCI_GETREADONLY)) { ScintillaEditBase::inputMethodEvent(Event); return; }
            // IME may also replace the preceding tentative composition range.
            const auto removed = std::uint64_t(raw(SCI_GETLENGTH));
            guarded(utf8Bytes(Event->commitString()) + utf8Bytes(Event->preeditString()),
                removed, false, [&] { ScintillaEditBase::inputMethodEvent(Event); return sptr_t(0); }, true);
        }
        void dropEvent(QDropEvent* Event) override
        {
            const auto text = Event->mimeData()->text();
            const auto result = guarded(utf8Bytes(text) + std::uint64_t(text.count('\n') + text.count('\r')) + 2,
                std::uint64_t(raw(SCI_GETLENGTH)), false,
                [&] { ScintillaEditBase::dropEvent(Event); return sptr_t(0); }, true);
            if (result < 0) Event->ignore(); // A rejected move must not delete the drag source.
        }
        void contextMenuEvent(QContextMenuEvent* Event) override
        {
            QMenu menu(this);
            const auto add = [&](const QString& Label, unsigned Message, bool Enabled) {
                auto* action = menu.addAction(Label, this, [this, Message] { send(Message); }); action->setEnabled(Enabled);
            };
            add(tr("실행 취소"), SCI_UNDO, raw(SCI_CANUNDO)); add(tr("다시 실행"), SCI_REDO, raw(SCI_CANREDO)); menu.addSeparator();
            add(tr("잘라내기"), SCI_CUT, !raw(SCI_GETREADONLY) && !raw(SCI_GETSELECTIONEMPTY));
            add(tr("복사"), SCI_COPY, !raw(SCI_GETSELECTIONEMPTY)); add(tr("붙여넣기"), SCI_PASTE, raw(SCI_CANPASTE));
            add(tr("삭제"), SCI_CLEAR, !raw(SCI_GETREADONLY) && !raw(SCI_GETSELECTIONEMPTY)); menu.addSeparator();
            add(tr("모두 선택"), SCI_SELECTALL, true); menu.exec(Event->globalPos());
        }
    private:
        sptr_t raw(unsigned Message, uptr_t W = 0, sptr_t L = 0) const
        { return ScintillaEditBase::send(Message, W, L); }
        std::uint64_t utf8Bytes(const QString& Text) const
        {
            std::uint64_t bytes = 0;
            for (qsizetype i = 0; i < Text.size() && bytes <= documentLimit_; ++i) {
                const auto c = Text[i].unicode();
                if (QChar::isHighSurrogate(c) && i + 1 < Text.size() && Text[i + 1].isLowSurrogate()) { bytes += 4; ++i; }
                else bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
            }
            return bytes;
        }
        std::uint64_t boundedLength(sptr_t Pointer) const
        {
            if (!Pointer) return 0;
            return strnlen_s(reinterpret_cast<const char*>(Pointer), std::size_t(documentLimit_ + 1));
        }
        template<class F> sptr_t guarded(std::uint64_t Inserted, std::uint64_t Removed, bool Undo, F Invoke, bool UncertainRemoval = false) const
        {
            try {
                const auto gate = BeginEdit(Inserted, Removed, Undo, UncertainRemoval);
                raw(SCI_SETSTATUS, SC_STATUS_OK); const auto value = Invoke();
                const_cast<BoundedEditor*>(this)->setProperty("memoryError", QVariant{}); return value;
            } catch (const std::exception& error) {
                raw(SCI_SETSTATUS, SC_STATUS_BADALLOC);
                const auto message = QString::fromUtf8(error.what()); const_cast<BoundedEditor*>(this)->setProperty("memoryError", message);
                if (blocked_) blocked_(message); return -1;
            }
        }
        template<class F> sptr_t paste(F Invoke) const
        {
            // Reserve the largest permitted clipboard conversion before asking Qt for text.
            try {
                Admission conversion; conversion.request_ = pool_->Enter(std::uint64_t(memoryMiB_) * 1024 * 1024);
                std::uint64_t conversionBytes = documentLimit_ * 6;
                // CF_UNICODETEXT gives a byte bound before Qt materialises clipboard text.
                // Small pastes must remain possible under a small request budget.
                if (OpenClipboard(nullptr)) {
                    const auto handle = GetClipboardData(CF_UNICODETEXT);
                    const auto nativeBytes = handle ? GlobalSize(handle) : 0;
                    CloseClipboard();
                    if (nativeBytes > documentLimit_ * 2 + 2) throw Failure("EditorSizeLimit");
                    if (nativeBytes) conversionBytes = std::max<std::uint64_t>(64 * 1024, std::uint64_t(nativeBytes) * 3);
                }
                conversion.bytes_ = pool_->ReserveController(conversionBytes);
                const auto text = QApplication::clipboard()->text();
                return guarded(utf8Bytes(text) + std::uint64_t(text.count('\n') + text.count('\r')) + 2,
                    std::uint64_t(raw(SCI_GETSELECTIONEND) - raw(SCI_GETSELECTIONSTART)), false, Invoke);
            } catch (const std::exception& error) {
                raw(SCI_SETSTATUS, SC_STATUS_BADALLOC); if (blocked_) blocked_(QString::fromUtf8(error.what())); return -1;
            }
        }
        std::shared_ptr<ControllerMemory> pool_;
        int memoryMiB_;
        std::uint64_t documentLimit_, historyLimit_ = 128 * 1024 * 1024;
        mutable std::uint64_t historyBytes_ = 0;
        mutable sptr_t historyActions_ = 0;
        mutable bool admitted_ = false;
        bool enabled_ = false;
    };
}
