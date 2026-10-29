#pragma once
#include "quickFindSettings.hpp"
#include "quickFindHighlight.hpp"
#include "commandMgr.hpp"
#include "settings/appSettings.hpp"
#include "UIs/cmpPanel.hpp"
#include "UIs/Components/cmpView.hpp"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QAction>
#include <QFrame>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QPainter>
#include <QProcess>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QPainterPath>
#include <QVBoxLayout>
#include <functional>
#include <QtitanGrid.h>
#include <windows.h>

namespace hc::ui {
class QuickFindMode final : public QToolButton {
public:
    explicit QuickFindMode(QWidget* Parent) : QToolButton(Parent) {}
protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton Option; initStyleOption(&Option); Option.text.clear(); Option.icon = {};
        QPainter Painter(this); style()->drawComplexControl(QStyle::CC_ToolButton, &Option, &Painter, this);
        Painter.setRenderHint(QPainter::Antialiasing);
        Painter.setPen(Qt::NoPen); Painter.setBrush(palette().color(QPalette::Highlight));
        const qreal X = width() / 2.0, Y = 2;
        QPainterPath Glyph;
        if (property("cursorMode").toBool()) {
            Glyph.moveTo(X - 5, Y + 3); Glyph.lineTo(X, Y + 3); Glyph.lineTo(X, Y);
            Glyph.lineTo(X + 5, Y + 5); Glyph.lineTo(X, Y + 9); Glyph.lineTo(X, Y + 6); Glyph.lineTo(X - 5, Y + 6);
        } else {
            Glyph.moveTo(X - 6, Y); Glyph.lineTo(X + 6, Y); Glyph.lineTo(X + 2, Y + 5);
            Glyph.lineTo(X + 2, Y + 8); Glyph.lineTo(X - 1, Y + 9); Glyph.lineTo(X - 1, Y + 5);
        }
        Glyph.closeSubpath(); Painter.drawPath(Glyph);
        auto Font = font(); Font.setPixelSize(8); Painter.setFont(Font); Painter.setPen(palette().color(QPalette::ButtonText));
        Painter.drawText(QRect(0, height() - 10, width(), 10), Qt::AlignCenter, "Ctrl+S");
    }
};
class QuickFindProgress final : public QWidget {
public:
    explicit QuickFindProgress(QLineEdit* Input) : QWidget(Input), input_(Input)
    {
        setObjectName("hc.quickFind.progress");
        setAttribute(Qt::WA_TransparentForMouseEvents); setFocusPolicy(Qt::NoFocus);
        setAccessibleName(tr("Everything 검색 결과를 가져오는 중"));
        animation_.setInterval(30);
        connect(&animation_, &QTimer::timeout, this, [this] { update(); });
        hide();
    }
    void Place() { setGeometry(2, qMax(0, input_->height() - 5), qMax(0, input_->width() - 4), 2); }
    void SetBusy(bool Busy)
    {
        if (Busy == animation_.isActive()) return;
        input_->setProperty("hc.quickFind.busy", Busy);
        if (Busy) { Place(); elapsed_.start(); show(); raise(); animation_.start(); }
        else { animation_.stop(); hide(); }
    }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter Painter(this);
        const int Length = qMax(24, width() / 4);
        const int X = int((elapsed_.elapsed() % 1100) * (width() + Length) / 1100) - Length;
        Painter.fillRect(QRect(X, 0, Length, height()), palette().color(QPalette::Highlight));
    }
private:
    QLineEdit* input_;
    QTimer animation_;
    QElapsedTimer elapsed_;
};

class QuickFindController final : public QObject, public QAbstractNativeEventFilter {
public:
    QuickFindController(QWidget* Window, QLineEdit* Find, std::function<CmpPanel*()> Panel,
        std::function<void(const QString&)> Open)
        : QObject(Window), window_(Window), find_(Find), panel_(std::move(Panel)), open_(std::move(Open))
    {
        setObjectName("hc.quickFind.controller");
        progress_ = new QuickFindProgress(Find);
        leadingAction_ = Find->actions().isEmpty() ? nullptr : Find->actions().last();
        mode_ = new QuickFindMode(Find); mode_->setObjectName("hc.quickFind.mode");
        mode_->setFocusPolicy(Qt::NoFocus); mode_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        mode_->setText("Ctrl+S"); mode_->setIconSize(QSize(12, 12));
        auto Font = mode_->font(); Font.setPointSizeF(qMax(6.0, Font.pointSizeF() - 2)); mode_->setFont(Font);
        mode_->hide(); connect(mode_, &QToolButton::clicked, this, [this] { toggleMode(); find_->setFocus(); });
        debounce_.setSingleShot(true); debounce_.setInterval(180);
        connect(&debounce_, &QTimer::timeout, this, [this] { searchEverything(); });
        connect(Find, &QLineEdit::textChanged, this, [this] { if (!composing_) apply(); });
        qApp->installEventFilter(this); qApp->installNativeEventFilter(this);
    }
    ~QuickFindController() override
    {
        qApp->removeEventFilter(this); qApp->removeNativeEventFilter(this);
        cancel();
    }
    void Synchronize()
    {
        if (applying_) return;
        auto* Panel = panel_();
        auto* View = Panel ? Panel->GetFocusView(Panel->CurrentTabIndex()) : nullptr;
        const auto Path = Panel ? Panel->CurrentPath() : QString{};
        if (quick_ && View == source_ && Path == sourcePath_ &&
            (scope_ == "everything" || (cursor_ && Panel->Search().isEmpty()))) return;
        if (auto* OldView = qobject_cast<CGridView*>(source_.data()); OldView && View != source_)
            if (auto* Old = qobject_cast<CFSModel*>(OldView->BaseModel())) Old->StopEverythingSearch();
        if (source_) {
            source_->setProperty("hc.quickFind.withinBase", within_ ? baseQuery_ : QString{});
            source_->setProperty("hc.quickFind.withinText", find_->text());
            source_->setProperty("hc.quickFind.withinCursor", cursor_);
        }
        cancel(); within_ = false; preedit_.clear(); composing_ = false; source_ = View; sourcePath_ = Path; observeSearchProgress();
        const auto* Grid = qobject_cast<CGridView*>(View);
        const auto* Model = Grid ? qobject_cast<CFSModel*>(Grid->BaseModel()) : nullptr;
        if (Model && Model->IsSearchResults()) {
            quick_ = true; cursor_ = false; scope_ = "everything";
            baseQuery_ = View->property("hc.quickFind.withinBase").toString();
            within_ = !baseQuery_.isEmpty() && baseQuery_.normalized(QString::NormalizationForm_C) == Model->EverythingQuery();
            if (within_) cursor_ = View->property("hc.quickFind.withinCursor").toBool();
            const QSignalBlocker Block(find_); find_->setText(within_ ? View->property("hc.quickFind.withinText").toString() : Model->EverythingQuery());
            updateModeHint(); updateHighlight(qobject_cast<CGridView*>(View), find_->text()); updateSearchProgress(); return;
        }
        quick_ = Panel && !Panel->SearchMatch().isEmpty(); scope_ = "panel"; cursor_ = false;
        const QSignalBlocker Block(find_); find_->setText(Panel ? Panel->Search() : QString{});
        updateModeHint();
    }
    void FocusEverything()
    {
        if (quick_ && scope_ == "everything" && within_) {
            within_ = false; cursor_ = false;
            { const QSignalBlocker Block(find_); find_->setText(baseQuery_); }
            if (auto* Panel = panel_()) { applying_ = true; Panel->SetSearch({}); applying_ = false; }
            updateModeHint(); updateHighlight(qobject_cast<CGridView*>(source_.data()), baseQuery_);
            find_->setFocus(Qt::ShortcutFocusReason); find_->selectAll(); return;
        }
        auto Config = configuration(); Config.scope_ = "everything"; begin(Config); find_->selectAll();
    }
    void FocusStandard()
    {
        if (configuration().scope_ == "everything") { FocusEverything(); return; }
        if (auto* Panel = panel_()) { applying_ = true; Panel->StopEverythingSearch(); applying_ = false; }
        quick_ = false; cursor_ = false; within_ = false; scope_ = "panel"; composing_ = false; preedit_.clear(); cancel();
        find_->setPlaceholderText(tr("현재 폴더에서 찾기  Ctrl+F"));
        updateModeHint();
        if (auto* Panel = panel_()) Panel->SetSearch(find_->text());
        find_->setFocus(Qt::ShortcutFocusReason); find_->selectAll();
    }
protected:
    bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr*) override
    {
        if (Type != "windows_generic_MSG" && Type != "windows_dispatcher_MSG") return false;
        const auto* Native = static_cast<MSG*>(Message);
        if (Native->message == WM_IME_STARTCOMPOSITION && listView(QApplication::focusWidget())) {
            const auto Config = configuration();
            if (Config.enabled_ && (Config.activation_ == "typing" ||
                (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier)) == (Qt::ControlModifier | Qt::AltModifier))) {
                // Move focus before the Windows input context starts composing.
                // Moving it after the first preedit would reset that context.
                begin(Config);
            }
        }
        return false;
    }
    bool eventFilter(QObject* Object, QEvent* Event) override
    {
        auto* Widget = qobject_cast<QWidget*>(Object);
        if (Object == find_) {
            if (Event->type() == QEvent::Resize || Event->type() == QEvent::Show) { progress_->Place(); placeMode(); }
            if (Event->type() == QEvent::InputMethod) {
                preedit_ = static_cast<QInputMethodEvent*>(Event)->preeditString();
                composing_ = !preedit_.isEmpty();
                // Run after QLineEdit has processed commit/replacement text.
                QTimer::singleShot(0, this, [this] { if (!composing_ || scope_ == "everything") apply(); });
            }
            if (!composing_ && (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) &&
                static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape) {
                Event->accept();
                if (Event->type() == QEvent::KeyPress) {
                    if (within_) { leaveWithin(); return true; }
                    find_->clear(); quick_ = false; cancel();
                    updateModeHint();
                    returnToList();
                }
                return true;
            }
            if ((Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) &&
                (configuration().enabled_ || (quick_ && scope_ == "everything"))) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (Key->key() == Qt::Key_S && Key->modifiers() == Qt::ControlModifier) {
                    Event->accept();
                    if (Event->type() == QEvent::KeyPress && !Key->isAutoRepeat()) {
                        toggleMode();
                    }
                    return true;
                }
            }
            if (quick_ && (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride)) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (!composing_ && scope_ == "everything" && Key->modifiers() == Qt::NoModifier &&
                    (Key->key() == Qt::Key_Home || Key->key() == Qt::Key_End || Key->key() == Qt::Key_PageUp || Key->key() == Qt::Key_PageDown)) {
                    Event->accept();
                    if (Event->type() == QEvent::KeyPress) if (auto* View = qobject_cast<CGridView*>(source_.data())) {
                        auto* Grid = View->BaseView();
                        if (Key->key() == Qt::Key_PageUp) Grid->navigatePageUp();
                        else if (Key->key() == Qt::Key_PageDown) Grid->navigatePageDown();
                        else if (Grid->getRowCount()) Grid->setFocusedRowIndex(Key->key() == Qt::Key_Home ? 0 : Grid->getRowCount() - 1);
                    }
                    return true;
                }
                if (!composing_ && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter ||
                    ((scope_ == "everything" || cursor_) && (Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Up)))) {
                    Event->accept();
                    if (Event->type() == QEvent::ShortcutOverride) return true;
                    if (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) {
                        returnToList();
                    } else if ((scope_ == "panel" || within_) && cursor_) moveCursor(Key->key() == Qt::Key_Down ? 1 : -1);
                    else if (auto* View = qobject_cast<CGridView*>(source_.data())) {
                        auto* Grid = View->BaseView();
                        if (Key->key() == Qt::Key_Down) Grid->navigateDown(Qt::NoModifier); else Grid->navigateUp(Qt::NoModifier);
                    }
                    return true;
                }
            }
            return false;
        }
        auto* View = listView(Widget);
        if (!View || transferring_) return false;
        if (quick_ && scope_ == "everything" && View == source_ &&
            (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) &&
            static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape) {
            Event->accept(); if (Event->type() == QEvent::KeyPress) {
                if (within_) { leaveWithin(); return true; }
                find_->clear(); quick_ = false; cancel(); returnToList();
                updateModeHint();
            }
            return true;
        }
        const auto Config = configuration(); if (!Config.enabled_) return false;
        if (Event->type() == QEvent::InputMethod && Config.activation_ == "typing") {
            const auto* Input = static_cast<QInputMethodEvent*>(Event);
            if (Input->preeditString().isEmpty() && Input->commitString().isEmpty()) return false;
            transferring_ = true; begin(Config);
            QInputMethodEvent Copy(Input->preeditString(), Input->attributes());
            Copy.setCommitString(Input->commitString(), Input->replacementStart(), Input->replacementLength());
            QApplication::sendEvent(find_, &Copy); transferring_ = false; return true;
        }
        if (Event->type() != QEvent::KeyPress && Event->type() != QEvent::ShortcutOverride) return false;
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if (quick_ && cursor_ && (scope_ == "panel" || within_) && View == source_ && !find_->text().isEmpty() &&
            Key->modifiers() == Qt::NoModifier && (Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Escape)) {
            Event->accept(); if (Event->type() == QEvent::ShortcutOverride) return true;
            if (Key->key() == Qt::Key_Escape) { find_->clear(); quick_ = false; cancel(); }
            else moveCursor(Key->key() == Qt::Key_Down ? 1 : -1);
            return true;
        }
        if (TyStCommandMgr::GetInstance()->Retrieve().contains(Key->keyCombination())) return false;
        const auto Modifiers = Key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const bool Chord = Config.activation_ == "ctrl-alt";
        if (Modifiers != (Chord ? Qt::ControlModifier | Qt::AltModifier : Qt::NoModifier)) return false;
        QString Text = Key->text();
        if (Chord && (Text.isEmpty() || !Text.front().isPrint()) && Key->key() >= Qt::Key_A && Key->key() <= Qt::Key_Z) {
            Text = QString(QChar(Key->key()));
            if (!Key->modifiers().testFlag(Qt::ShiftModifier)) Text = Text.toLower();
        }
        if (Text.isEmpty() || !Text.front().isPrint() || Text.front().isSpace()) return false;
        if (Event->type() == QEvent::ShortcutOverride) { Event->accept(); return true; }
        begin(Config);
        // This event is already translated text. IME preedit is handled above,
        // without flattening it to committed characters.
        QKeyEvent Copy(QEvent::KeyPress, Key->key(), Key->modifiers() & Qt::ShiftModifier, Text, Key->isAutoRepeat(), Key->count());
        QApplication::sendEvent(find_, &Copy); return true;
    }
private:
    quickfind::Settings configuration() const
    { return quickfind::Settings::FromJson(settings::AppSettings().Read(settings::Section::General).toObject()["quickFind"].toObject()); }
    CGridView* listView(QWidget* Widget) const
    {
        if (!Widget || Widget->window() != window_ || qobject_cast<QLineEdit*>(Widget) || QApplication::activeModalWidget()) return nullptr;
        for (auto* Parent = Widget; Parent; Parent = Parent->parentWidget())
            if (auto* View = qobject_cast<CGridView*>(Parent)) return View->IsEditing() ? nullptr : View;
        return nullptr;
    }
    void begin(const quickfind::Settings& Config)
    {
        cancel(); quick_ = true; scope_ = Config.scope_; cursor_ = Config.result_ == "cursor"; composing_ = false;
        within_ = false; baseQuery_.clear(); preedit_.clear();
        if (scope_ == "everything") cursor_ = false;
        if (auto* Panel = panel_()) { source_ = Panel->GetFocusView(Panel->CurrentTabIndex()); sourcePath_ = Panel->CurrentPath(); }
        observeSearchProgress();
        find_->setPlaceholderText(scope_ == "everything" ? tr("Everything 빠른 찾기") : tr("현재 목록 빠른 찾기"));
        updateModeHint();
        find_->clear(); find_->setFocus(Qt::ShortcutFocusReason);
    }
    void updateModeHint()
    {
        const auto Hint = within_ ? tr("Everything 결과 안에서 %1 · 기준 검색: %2 · Ctrl+S: 표시 전환 · Ctrl+Alt+F: 전체 검색 · Esc: 결과 내 검색 해제").arg(cursor_ ? tr("커서 이동") : tr("필터링"), baseQuery_)
            : scope_ == "everything" ? tr("Everything 검색 · Ctrl+S: 결과 안에서 커서 이동 · ↑/↓: 결과 이동 · Enter: 파일 패널 · Esc: 원래 폴더")
            : cursor_ ? tr("커서 이동 · ↑/↓: 이전/다음 일치 항목 · Ctrl+S: 필터링으로 전환")
            : tr("필터링 · Ctrl+S: 커서 이동으로 전환");
        find_->setToolTip(Hint); find_->setAccessibleDescription(Hint);
        mode_->setToolTip(Hint); mode_->setAccessibleName(Hint);
        mode_->setProperty("cursorMode", cursor_); mode_->update();
        if (leadingAction_) leadingAction_->setVisible(!quick_);
        mode_->setVisible(quick_); placeMode();
        if (scope_ == "everything") find_->setPlaceholderText(within_ ? tr("결과 내 찾기 · %1").arg(baseQuery_) : tr("Everything 빠른 찾기"));
    }
    void placeMode()
    {
        mode_->setGeometry(2, 2, 38, qMax(0, find_->height() - 6));
        const auto Margins = find_->textMargins(); find_->setTextMargins(quick_ ? 42 : 0, Margins.top(), Margins.right(), Margins.bottom());
    }
    QString effectiveQuery() const
    {
        const auto Text = find_->text();
        if (composing_) return Text.left(find_->cursorPosition()).normalized(QString::NormalizationForm_C)
            + preedit_.normalized(QString::NormalizationForm_C) + Text.mid(find_->cursorPosition()).normalized(QString::NormalizationForm_C);
        return Text.normalized(QString::NormalizationForm_C);
    }
    QString matchMode() const
    {
        const auto Config = configuration();
        if (scope_ != "everything" || !Config.everythingKorean_) return Config.match_;
        return "korean:" + Config.match_ + (composing_ && find_->cursorPosition() == find_->text().size()
            ? ":" + QString::number(effectiveQuery().size() - 1) : QString{});
    }
    void toggleMode()
    {
        if (composing_ && scope_ == "everything") return;
        if (quick_ && scope_ == "everything") {
            if (!within_) {
                if (find_->text().isEmpty()) return;
                if (debounce_.isActive()) { debounce_.stop(); searchEverything(); }
                baseQuery_ = find_->text(); within_ = true; cursor_ = true;
                const QSignalBlocker Block(find_); find_->clear();
            } else cursor_ = !cursor_;
        } else {
            cancel(); cursor_ = !quick_ || !cursor_; quick_ = true; scope_ = "panel";
            if (auto* Panel = panel_()) { source_ = Panel->GetFocusView(Panel->CurrentTabIndex()); sourcePath_ = Panel->CurrentPath(); }
        }
        updateModeHint(); if (!composing_) apply();
    }
    void leaveWithin()
    {
        within_ = false; cursor_ = false;
        { const QSignalBlocker Block(find_); find_->setText(baseQuery_); }
        if (auto* Panel = panel_()) { applying_ = true; Panel->SetSearch({}); applying_ = false; }
        updateModeHint(); updateHighlight(qobject_cast<CGridView*>(source_.data()), baseQuery_);
    }
    void returnToList()
    {
        if (auto* Panel = panel_())
            if (auto* View = Panel->GetFocusView(Panel->CurrentTabIndex())) View->EnsureKeyboardFocusOnView();
    }
    void moveCursor(int Direction)
    {
        if (auto* View = qobject_cast<CGridView*>(source_.data()))
            View->MoveQuickFindCursor(effectiveQuery(), matchMode(), Direction);
    }
    void updateHighlight(CGridView* View, const QString& Query)
    {
        if (!View) return;
        const auto Config = configuration();
        View->setProperty("hc.quickFind.text", Query); View->setProperty("hc.quickFind.match", matchMode());
        View->setProperty("hc.quickFind.highlight", Config.highlight_);
        View->update();
        for (auto* Child : View->findChildren<QWidget*>()) Child->update();
    }
    void apply()
    {
        auto* Panel = panel_(); if (!Panel) return;
        const auto Config = configuration();
        updateHighlight(qobject_cast<CGridView*>(source_.data()), quick_ ? effectiveQuery() : QString{});
        if (quick_ && scope_ == "everything" && !within_) {
            cancel();
            if (!effectiveQuery().isEmpty()) {
                if (auto* Grid = qobject_cast<CGridView*>(Panel->GetFocusView(Panel->CurrentTabIndex())))
                    if (auto* Model = qobject_cast<CFSModel*>(Grid->BaseModel())) Model->StopEverythingSearch();
                debounce_.start();
                updateSearchProgress();
            } else { applying_ = true; Panel->StopEverythingSearch(); applying_ = false; }
            return;
        }
        if (quick_ && cursor_) {
            applying_ = true;
            if (!Panel->Search().isEmpty() || !Panel->SearchMatch().isEmpty()) Panel->SetSearch({});
            applying_ = false; moveCursor(0); return;
        }
        applying_ = true; Panel->SetSearch(effectiveQuery(), quick_ ? matchMode() : QString{}); applying_ = false;
    }
    void cancel()
    {
        debounce_.stop(); ++generation_;
        if (progress_) progress_->SetBusy(false);
        if (auto* View = qobject_cast<CGridView*>(source_.data())) { View->CancelQuickFind(); updateHighlight(View, {}); }
    }
    void searchEverything()
    {
        if (!quick_ || scope_ != "everything" || within_ || effectiveQuery().isEmpty()) return;
        if (auto* Panel = panel_()) {
            applying_ = true;
            const auto Config = configuration();
            Panel->StartEverythingSearch(effectiveQuery(), matchMode(), Config.everythingDisplayIntervalMs_);
            applying_ = false;
            updateHighlight(qobject_cast<CGridView*>(source_.data()), effectiveQuery());
            updateSearchProgress();
        }
    }
    void observeSearchProgress()
    {
        auto* View = qobject_cast<CGridView*>(source_.data());
        auto* Model = View ? qobject_cast<CFSModel*>(View->BaseModel()) : nullptr;
        if (progressModel_ == Model) return;
        disconnect(progressConnection_); progressModel_ = Model;
        if (Model) progressConnection_ = connect(Model, &CFSModel::SearchResultsChanged, this, [this] { updateSearchProgress(); });
    }
    void updateSearchProgress()
    {
        auto* Panel = panel_();
        const bool Active = Panel && Panel->GetFocusView(Panel->CurrentTabIndex()) == source_;
        if (progress_) progress_->SetBusy(Active && quick_ && scope_ == "everything" && (!effectiveQuery().isEmpty() || within_)
            && (debounce_.isActive() || (progressModel_ && progressModel_->IsEverythingRunning())));
    }
    QPointer<QWidget> window_;
    QPointer<QLineEdit> find_;
    QPointer<QuickFindProgress> progress_;
    QPointer<QToolButton> mode_;
    QPointer<QAction> leadingAction_;
    QPointer<CFSModel> progressModel_;
    QMetaObject::Connection progressConnection_;
    std::function<CmpPanel*()> panel_;
    std::function<void(const QString&)> open_;
    QPointer<CViewT> source_;
    QString sourcePath_, scope_;
    QString preedit_, baseQuery_;
    bool within_ = false;
    QTimer debounce_;
    quint64 generation_ = 0;
    bool quick_ = false, cursor_ = false, composing_ = false, applying_ = false, transferring_ = false;
};
}
