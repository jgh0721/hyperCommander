#include "stdafx.h"
#include "nativePathCompletion.hpp"

/// UIs
#include "dialogShell.hpp"

/// External libraries
#include <fmstyle/Glyphs.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>

/// Platform/framework headers
#include <QDirIterator>
#include <QDir>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <stop_token>

namespace hc::ui {
namespace {
struct CompletionQuery {
    QString input_, directory_, prefix_, leaf_, suffix_;
    int cursor_ = 0;
    QChar separator_ = '\\';
};
struct CompletionEntry { QString name_; bool directory_ = false; };

CompletionQuery ParseQuery(const QString& Input, int Cursor, const QString& Base)
{
    CompletionQuery Query;
    Query.input_ = Input; Query.cursor_ = Cursor;
    const auto IsSeparator = [](QChar Ch) { return Ch == '/' || Ch == '\\'; };
    int Start = Cursor, End = Cursor;
    while (Start > 0 && !IsSeparator(Input[Start - 1])) --Start;
    while (End < Input.size() && !IsSeparator(Input[End])) ++End;
    Query.prefix_ = Input.left(Start); Query.leaf_ = Input.mid(Start, Cursor - Start); Query.suffix_ = Input.mid(End);
    if (Start > 0) Query.separator_ = Input[Start - 1];
    Query.directory_ = ResolveNativeDestination(Query.prefix_.isEmpty() ? QStringLiteral(".") : Query.prefix_, Base);
    // Incomplete drive/UNC roots and invalid leaves never fall back to process cwd.
    if (Query.leaf_.contains(QRegularExpression(QStringLiteral("[<>:\"|?*\\x00-\\x1f]")))) Query.directory_.clear();
    return Query;
}

class CompletionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
    {
        auto Size = QStyledItemDelegate::sizeHint(Option, Index);
        Size.setHeight(std::max(Size.height(), QFontMetrics(Option.font).height() + 8));
        return Size;
    }
};

class NativePathCompletion final : public QObject {
public:
    NativePathCompletion(QLineEdit* Editor, QString Base, bool IncludeFiles)
        : QObject(Editor), editor_(Editor), base_(std::move(Base)), includeFiles_(IncludeFiles)
    {
        setObjectName("hc.pathCompletion");
        popup_ = new QListView(Editor);
        popup_->setWindowFlags(Qt::ToolTip);
        popup_->setAttribute(Qt::WA_ShowWithoutActivating);
        popup_->setObjectName("hc.pathCompletion.popup");
        popup_->setAccessibleName(tr("경로 자동완성 후보"));
        popup_->setFocusPolicy(Qt::NoFocus);
        popup_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        popup_->setSelectionMode(QAbstractItemView::SingleSelection);
        popup_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        popup_->setTextElideMode(Qt::ElideMiddle);
        popup_->setUniformItemSizes(true);
        popup_->setItemDelegate(new CompletionDelegate(popup_));
        model_ = new QStandardItemModel(this); popup_->setModel(model_);
        timer_.setSingleShot(true); timer_.setInterval(100);
        connect(&timer_, &QTimer::timeout, this, [this] { scan(); });
        connect(Editor, &QLineEdit::textEdited, this, [this] { if (!composing_ && !applying_) schedule(); });
        connect(Editor, &QLineEdit::textChanged, this, [this](const QString& Text) { if (Text != requestedText_) dismiss(); });
        connect(Editor, &QLineEdit::cursorPositionChanged, this, [this] {
            if (!applying_ && (popup_->isVisible() || timer_.isActive() || pending_)) schedule();
        });
        connect(popup_, &QListView::clicked, this, [this](const QModelIndex& Index) { complete(Index); });
        connect(&watcher_, &QFutureWatcher<QList<CompletionEntry>>::finished, this, [this] { showResults(); });
        auto& Themes = fm::style::ThemeManager::instance();
        connect(&Themes, &fm::style::ThemeManager::changed, this, [this] { applyAppearance(); });
        connect(&Themes, &fm::style::ThemeManager::fontsChanged, this, [this] { applyFont(); });
        applyFont();
        Editor->installEventFilter(this); Editor->window()->installEventFilter(this);
    }
    ~NativePathCompletion() override { stop_.request_stop(); delete popup_; }

protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override
    {
        if (Watched == editor_) {
            if (Event->type() == QEvent::InputMethod) {
                composing_ = !static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();
                editor_->setProperty("hc.pathCompletion.composing", composing_);
                if (composing_) dismiss();
            }
            if (Event->type() == QEvent::FocusOut || Event->type() == QEvent::Hide) dismiss();
            if ((Event->type() == QEvent::Move || Event->type() == QEvent::Resize) && popup_->isVisible()) positionPopup();
            if (Event->type() == QEvent::FontChange || Event->type() == QEvent::PaletteChange || Event->type() == QEvent::StyleChange) applyAppearance();
            if (Event->type() == QEvent::ShortcutOverride || Event->type() == QEvent::KeyPress) {
                auto* Key = static_cast<QKeyEvent*>(Event);
                const auto Modifiers = Key->modifiers();
                const bool Plain = Modifiers == Qt::NoModifier || Modifiers == Qt::KeypadModifier;
                const bool Open = Plain && (Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Up);
                const bool Trigger = Key->key() == Qt::Key_Space && Modifiers == Qt::ControlModifier;
                const bool Visible = popup_->isVisible();
                const bool Active = Visible || timer_.isActive() || pending_ || (watcher_.isRunning() && scanGeneration_ == generation_);
                const bool Commit = Active && Plain && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter || (Visible && Key->key() == Qt::Key_Tab));
                const bool Cancel = Active && Plain && Key->key() == Qt::Key_Escape;
                const bool Page = Visible && Plain && (Key->key() == Qt::Key_PageUp || Key->key() == Qt::Key_PageDown);
                if (composing_) return QObject::eventFilter(Watched, Event);
                if (Open || Trigger || Commit || Cancel || Page) {
                    if (Event->type() == QEvent::ShortcutOverride) { Key->accept(); return true; }
                    if (Cancel || (Commit && !Visible)) dismiss();
                    else if (Commit) complete(popup_->currentIndex());
                    else if (Visible && !Trigger) {
                        const int Step = Page ? 8 : 1;
                        const int Delta = Key->key() == Qt::Key_Up || Key->key() == Qt::Key_PageUp ? -Step : Step;
                        const auto Index = model_->index(std::clamp(popup_->currentIndex().row() + Delta, 0, model_->rowCount() - 1), 0);
                        popup_->setCurrentIndex(Index); popup_->scrollTo(Index);
                    } else { selectLast_ = Key->key() == Qt::Key_Up; schedule(); timer_.stop(); scan(); }
                    return true;
                }
                if (Event->type() == QEvent::KeyPress && ((Key->key() == Qt::Key_Down && Modifiers.testFlag(Qt::AltModifier))
                    || Key->key() == Qt::Key_Backtab || Key->key() == Qt::Key_Tab)) dismiss();
            }
        }
        if (Watched == editor_->window()) {
            if (Event->type() == QEvent::WindowDeactivate || Event->type() == QEvent::Hide) dismiss();
            else if ((Event->type() == QEvent::Move || Event->type() == QEvent::Resize) && popup_->isVisible()) positionPopup();
        }
        return QObject::eventFilter(Watched, Event);
    }

private:
    void dismiss()
    {
        ++generation_; stop_.request_stop(); timer_.stop(); pending_ = false; popup_->hide();
    }
    void schedule()
    {
        dismiss(); requestedText_ = editor_->text(); requestedCursor_ = editor_->cursorPosition(); timer_.start();
    }
    void scan()
    {
        if (!editor_->hasFocus() || composing_ || editor_->text() != requestedText_
            || editor_->cursorPosition() != requestedCursor_) return;
        if (watcher_.isRunning()) { pending_ = true; return; }
        query_ = ParseQuery(requestedText_, requestedCursor_, base_);
        if (query_.directory_.isEmpty()) return;
        stop_ = std::stop_source{}; scanGeneration_ = generation_;
        watcher_.setFuture(QtConcurrent::run([Query = query_, IncludeFiles = includeFiles_ && query_.suffix_.isEmpty(), Stop = stop_.get_token()] {
            QList<CompletionEntry> Entries;
            QElapsedTimer Budget; Budget.start();
            QDir::Filters Filters = QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
            if (IncludeFiles) Filters |= QDir::Files;
            QDirIterator Iterator(Query.directory_, Filters);
            int Inspected = 0;
            while (!Stop.stop_requested() && Entries.size() < 256 && Inspected++ < 50000 && Budget.elapsed() < 250 && Iterator.hasNext()) {
                Iterator.next();
                const auto Name = Iterator.fileName();
                if (!Name.startsWith(Query.leaf_, Qt::CaseInsensitive)) continue;
                const bool Directory = Iterator.fileInfo().isDir();
                if (Directory || IncludeFiles) Entries.append({Name, Directory});
            }
            std::sort(Entries.begin(), Entries.end(), [](const auto& Left, const auto& Right) {
                if (Left.directory_ != Right.directory_) return Left.directory_;
                const int Order = Left.name_.compare(Right.name_, Qt::CaseInsensitive);
                return Order == 0 ? Left.name_ < Right.name_ : Order < 0;
            });
            return Entries;
        }));
    }
    void showResults()
    {
        const auto Entries = watcher_.result();
        if (pending_) { pending_ = false; scan(); return; }
        if (scanGeneration_ != generation_ || !editor_->hasFocus() || composing_
            || editor_->text() != query_.input_ || editor_->cursorPosition() != query_.cursor_) return;
        model_->clear();
        for (const auto& Entry : Entries) {
            const auto Separator = Entry.directory_ && query_.suffix_.isEmpty() ? QString(query_.separator_) : QString{};
            const auto Value = query_.prefix_ + Entry.name_ + Separator + query_.suffix_;
            auto* Item = new QStandardItem(Value);
            Item->setData(Value, Qt::UserRole);
            Item->setData(int(query_.prefix_.size() + Entry.name_.size()) + (Entry.directory_ ? 1 : 0), Qt::UserRole + 1);
            Item->setData(Entry.directory_ && query_.suffix_.isEmpty(), Qt::UserRole + 2);
            Item->setData(Entry.directory_, Qt::UserRole + 3);
            Item->setToolTip(QDir::toNativeSeparators(QDir(query_.directory_).filePath(Entry.name_)));
            model_->appendRow(Item);
        }
        if (model_->rowCount() == 0) { popup_->hide(); return; }
        applyAppearance();
        popup_->setCurrentIndex(model_->index(selectLast_ ? model_->rowCount() - 1 : 0, 0)); selectLast_ = false;
        positionPopup(); popup_->show();
    }
    void complete(const QModelIndex& Index)
    {
        if (!Index.isValid() || scanGeneration_ != generation_) return;
        const auto Value = Index.data(Qt::UserRole).toString();
        const int Cursor = Index.data(Qt::UserRole + 1).toInt();
        const bool Descend = Index.data(Qt::UserRole + 2).toBool();
        dismiss(); applying_ = true;
        // insert() retains the edit's undo history, unlike setText().
        editor_->selectAll(); editor_->insert(Value); editor_->setCursorPosition(std::min(Cursor, int(Value.size())));
        applying_ = false;
        if (Descend) schedule();
    }
    void applyFont()
    {
        editor_->setFont(fm::style::monoFont(fm::style::ThemeManager::instance().monoFontPx())); applyAppearance();
    }
    void applyAppearance()
    {
        fm::style::ThemeScope::set(popup_, fm::style::themeColorsFor(editor_), false);
        auto Palette = editor_->palette();
        // Focus stays in the editor; candidates still need the active selection colors.
        for (const auto Role : {QPalette::Highlight, QPalette::HighlightedText})
            Palette.setColor(QPalette::Inactive, Role, Palette.color(QPalette::Active, Role));
        popup_->setPalette(Palette); popup_->setFont(editor_->font()); popup_->setStyle(editor_->style());
        const auto& Colors = fm::style::themeColorsFor(editor_);
        const auto FolderColor = Colors[fm::style::Token::Folder], FileColor = Colors[fm::style::Token::Fg];
        if (folderIcon_.isNull() || folderColor_ != FolderColor || fileColor_ != FileColor) {
            folderColor_ = FolderColor; fileColor_ = FileColor;
            folderIcon_ = fm::style::glyphIcon(fm::style::Glyph::Folder, FolderColor, 16);
            fileIcon_ = fm::style::glyphIcon(fm::style::Glyph::File, FileColor, 16);
        }
        for (int Row = 0; Row < model_->rowCount(); ++Row) {
            auto* Item = model_->item(Row); const bool Directory = Item->data(Qt::UserRole + 3).toBool();
            Item->setIcon(Directory ? folderIcon_ : fileIcon_);
        }
        if (popup_->isVisible()) positionPopup();
    }
    void positionPopup()
    {
        const auto Available = editor_->screen()->availableGeometry();
        const auto Below = editor_->mapToGlobal(QPoint(0, editor_->height()));
        const int RowHeight = editor_->fontMetrics().height() + 8;
        const int Width = std::min(std::max(editor_->width(), 200), Available.width());
        const int Height = std::min(std::min(model_->rowCount(), 8) * RowHeight + 2 * popup_->frameWidth(), Available.height());
        const int X = std::clamp(Below.x(), Available.left(), Available.right() - Width + 1);
        int Y = Below.y();
        if (Y + Height > Available.bottom() + 1) Y = editor_->mapToGlobal(QPoint()).y() - Height;
        Y = std::clamp(Y, Available.top(), Available.bottom() - Height + 1);
        popup_->setGeometry(X, Y, Width, Height);
    }
    QIcon folderIcon_, fileIcon_;
    QColor folderColor_, fileColor_;
    QLineEdit* editor_;
    QString base_, requestedText_;
    bool includeFiles_, composing_ = false, applying_ = false, pending_ = false, selectLast_ = false;
    int requestedCursor_ = 0;
    quint64 generation_ = 1, scanGeneration_ = 0;
    CompletionQuery query_;
    QListView* popup_;
    QStandardItemModel* model_;
    QTimer timer_;
    QFutureWatcher<QList<CompletionEntry>> watcher_;
    std::stop_source stop_;
};
}

void InstallNativePathCompletion(QLineEdit* Editor, const QString& BaseDirectory, bool IncludeFiles)
{
    if (Editor && !BaseDirectory.isEmpty() && !Editor->findChild<QObject*>("hc.pathCompletion"))
        new NativePathCompletion(Editor, BaseDirectory, IncludeFiles);
}
}
