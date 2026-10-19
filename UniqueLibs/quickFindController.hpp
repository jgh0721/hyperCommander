#pragma once
#include "quickFindSettings.hpp"
#include "quickFindHighlight.hpp"
#include "commandMgr.hpp"
#include "settings/appSettings.hpp"
#include "UIs/cmpPanel.hpp"
#include "UIs/Components/cmpView.hpp"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QFrame>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>
#include <windows.h>

namespace hc::ui {
class QuickFindController final : public QObject, public QAbstractNativeEventFilter {
public:
    QuickFindController(QWidget* Window, QLineEdit* Find, std::function<CmpPanel*()> Panel,
        std::function<void(const QString&)> Open)
        : QObject(Window), window_(Window), find_(Find), panel_(std::move(Panel)), open_(std::move(Open))
    {
        setObjectName("hc.quickFind.controller");
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
        cancel(); source_ = View; sourcePath_ = Path;
        quick_ = Panel && !Panel->SearchMatch().isEmpty(); scope_ = "panel"; cursor_ = false;
        const QSignalBlocker Block(find_); find_->setText(Panel ? Panel->Search() : QString{});
    }
    void FocusStandard()
    {
        quick_ = false; cursor_ = false; scope_ = "panel"; composing_ = false; cancel();
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
        if (Object == window_ && Event->type() == QEvent::Resize && results_ && results_->isVisible()) positionResults();
        if (results_ && Widget && (Event->type() == QEvent::FocusIn || Event->type() == QEvent::MouseButtonPress) &&
            Widget != find_ && !find_->isAncestorOf(Widget) && Widget != results_ && !results_->isAncestorOf(Widget)) results_->hide();
        if (Object == tree_ && Event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape) {
            find_->clear(); quick_ = false; cancel(); returnToList(); return true;
        }
        if (Object == find_) {
            if (Event->type() == QEvent::InputMethod) {
                composing_ = !static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();
                // Run after QLineEdit has processed commit/replacement text.
                if (!composing_) QTimer::singleShot(0, this, [this] { if (!composing_) apply(); });
            }
            if (!composing_ && (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) &&
                static_cast<QKeyEvent*>(Event)->key() == Qt::Key_Escape) {
                Event->accept();
                if (Event->type() == QEvent::KeyPress) {
                    find_->clear(); quick_ = false; cancel();
                    returnToList();
                }
                return true;
            }
            if ((Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride) &&
                configuration().enabled_ && (!quick_ || scope_ == "panel")) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (Key->key() == Qt::Key_S && Key->modifiers() == Qt::ControlModifier) {
                    Event->accept();
                    if (Event->type() == QEvent::KeyPress && !Key->isAutoRepeat()) {
                        cancel(); cursor_ = !quick_ || !cursor_; quick_ = true; scope_ = "panel";
                        if (auto* Panel = panel_()) { source_ = Panel->GetFocusView(Panel->CurrentTabIndex()); sourcePath_ = Panel->CurrentPath(); }
                        updateModeHint(); if (!composing_) apply();
                    }
                    return true;
                }
            }
            if (quick_ && (Event->type() == QEvent::KeyPress || Event->type() == QEvent::ShortcutOverride)) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (!composing_ && (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter ||
                    ((scope_ == "everything" || cursor_) && (Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Up)))) {
                    Event->accept();
                    if (Event->type() == QEvent::ShortcutOverride) return true;
                    if (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) {
                        if (scope_ == "everything") openCurrent(); else returnToList();
                    } else if (scope_ == "panel" && cursor_) moveCursor(Key->key() == Qt::Key_Down ? 1 : -1);
                    else if (tree_ && tree_->topLevelItemCount()) {
                        const int Row = tree_->indexOfTopLevelItem(tree_->currentItem());
                        tree_->setCurrentItem(tree_->topLevelItem(qBound(0, Row + (Key->key() == Qt::Key_Down ? 1 : -1), tree_->topLevelItemCount() - 1)));
                    }
                    return true;
                }
            }
            return false;
        }
        auto* View = listView(Widget);
        if (!View || transferring_) return false;
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
        if (quick_ && cursor_ && scope_ == "panel" && View == source_ && !find_->text().isEmpty() &&
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
        if (auto* Panel = panel_()) { source_ = Panel->GetFocusView(Panel->CurrentTabIndex()); sourcePath_ = Panel->CurrentPath(); }
        find_->setPlaceholderText(scope_ == "everything" ? tr("Everything 빠른 찾기") : tr("현재 목록 빠른 찾기"));
        updateModeHint();
        find_->clear(); find_->setFocus(Qt::ShortcutFocusReason);
    }
    void updateModeHint()
    {
        const auto Hint = scope_ == "everything" ? tr("Everything 검색 · ↑/↓: 결과 이동 · Enter: 위치 열기")
            : cursor_ ? tr("커서 이동 · ↑/↓: 이전/다음 일치 항목 · Ctrl+S: 필터링으로 전환")
            : tr("필터링 · Ctrl+S: 커서 이동으로 전환");
        find_->setToolTip(Hint); find_->setAccessibleDescription(Hint);
    }
    void returnToList()
    {
        if (auto* Panel = panel_())
            if (auto* View = Panel->GetFocusView(Panel->CurrentTabIndex())) View->EnsureKeyboardFocusOnView();
    }
    void moveCursor(int Direction)
    {
        if (auto* View = qobject_cast<CGridView*>(source_.data()))
            View->MoveQuickFindCursor(find_->text(), configuration().match_, Direction);
    }
    void updateHighlight(CGridView* View, const QString& Query)
    {
        if (!View) return;
        const auto Config = configuration();
        View->setProperty("hc.quickFind.text", Query); View->setProperty("hc.quickFind.match", Config.match_);
        View->setProperty("hc.quickFind.highlight", Config.highlight_);
        View->update();
        for (auto* Child : View->findChildren<QWidget*>()) Child->update();
    }
    void apply()
    {
        auto* Panel = panel_(); if (!Panel) return;
        const auto Config = configuration();
        updateHighlight(qobject_cast<CGridView*>(source_.data()), quick_ && scope_ == "panel" ? find_->text() : QString{});
        if (quick_ && !Config.enabled_) { quick_ = false; cancel(); }
        if (quick_ && scope_ == "everything") {
            cancel(); if (!find_->text().isEmpty()) debounce_.start(); return;
        }
        if (quick_ && cursor_) {
            applying_ = true;
            if (!Panel->Search().isEmpty() || !Panel->SearchMatch().isEmpty()) Panel->SetSearch({});
            applying_ = false; moveCursor(0); return;
        }
        applying_ = true; Panel->SetSearch(find_->text(), quick_ ? Config.match_ : QString{}); applying_ = false;
    }
    void cancel()
    {
        debounce_.stop(); ++generation_;
        if (auto* View = qobject_cast<CGridView*>(source_.data())) { View->CancelQuickFind(); updateHighlight(View, {}); }
        if (process_) { process_->disconnect(this); process_->kill(); process_->deleteLater(); process_ = nullptr; }
        if (results_) results_->hide();
    }
    void ensureResults()
    {
        if (results_) return;
        results_ = new QFrame(window_); results_->setObjectName("hc.quickFind.results"); results_->setFrameShape(QFrame::StyledPanel);
        results_->setAutoFillBackground(true);
        auto* Layout = new QVBoxLayout(results_);
        status_ = new QLabel(results_); status_->setWordWrap(true); status_->setObjectName("hc.quickFind.status"); Layout->addWidget(status_);
        tree_ = new QTreeWidget(results_); tree_->setObjectName("hc.quickFind.matches"); tree_->setHeaderLabels({tr("이름"), tr("위치")});
        tree_->setItemDelegate(new quickfind::ResultHighlightDelegate(tree_));
        tree_->setRootIsDecorated(false); Layout->addWidget(tree_);
        connect(tree_, &QTreeWidget::itemActivated, this, [this] { openCurrent(); });
    }
    void positionResults()
    {
        const auto Point = find_->mapTo(window_, QPoint(0, find_->height()));
        const int Width = qMin(720, window_->width() - 12);
        results_->setGeometry(qBound(0, Point.x(), window_->width() - Width), Point.y(), Width, qMin(320, window_->height() - Point.y() - 8));
        results_->raise();
    }
    void searchEverything()
    {
        if (!quick_ || scope_ != "everything" || find_->text().isEmpty()) return;
        ensureResults(); tree_->clear(); status_->setText(tr("Everything 검색 중…"));
        tree_->setProperty("hc.quickFind.text", find_->text()); tree_->setProperty("hc.quickFind.highlight", configuration().highlight_);
        positionResults(); if (find_->hasFocus()) results_->show();
        auto* Process = new QProcess(this); process_ = Process; const auto Generation = generation_;
        Process->setProgram(QCoreApplication::applicationDirPath() + "/hc-search-index.exe");
        Process->setArguments({"--quick-find", find_->text(), configuration().match_});
        const auto Failed = [this, Generation] {
            if (Generation == generation_) status_->setText(tr("Everything 검색을 사용할 수 없습니다. Everything 실행 상태를 확인한 뒤 다시 입력하세요."));
        };
        connect(Process, &QProcess::errorOccurred, this, [Failed](QProcess::ProcessError) { Failed(); });
        connect(Process, &QProcess::finished, this, [this, Process, Generation, Failed](int Code, QProcess::ExitStatus Exit) {
            Process->deleteLater(); if (Generation != generation_) return; process_ = nullptr;
            const auto Bytes = Process->readAllStandardOutput(); const auto Doc = QJsonDocument::fromJson(Bytes);
            if (Code || Exit != QProcess::NormalExit || Bytes.size() > 1024 * 1024 || !Doc.isObject() || !Doc["paths"].isArray()) { Failed(); return; }
            const auto Paths = Doc["paths"].toArray();
            for (const auto& Value : Paths) {
                const auto Path = Value.toString(); if (Path.isEmpty()) continue;
                auto* Item = new QTreeWidgetItem(tree_, {QFileInfo(Path).fileName(), QDir::toNativeSeparators(QFileInfo(Path).absolutePath())});
                Item->setData(0, Qt::UserRole, Path);
                Item->setToolTip(0, QDir::toNativeSeparators(Path)); Item->setToolTip(1, QDir::toNativeSeparators(Path));
            }
            status_->setText(tr("Everything · %1개 일치 · 최대 100개 표시 · Enter: 위치 열기").arg(Doc["total"].toDouble(), 0, 'f', 0));
            if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
            tree_->resizeColumnToContents(0);
        });
        Process->start(); QTimer::singleShot(5000, Process, [Process] { if (Process->state() != QProcess::NotRunning) Process->kill(); });
    }
    void openCurrent()
    {
        if (!tree_ || !tree_->currentItem()) return;
        const auto Path = tree_->currentItem()->data(0, Qt::UserRole).toString();
        quick_ = false; cancel(); open_(Path);
    }
    QPointer<QWidget> window_;
    QPointer<QLineEdit> find_;
    std::function<CmpPanel*()> panel_;
    std::function<void(const QString&)> open_;
    QPointer<CViewT> source_;
    QString sourcePath_, scope_;
    QTimer debounce_;
    QPointer<QProcess> process_;
    QPointer<QFrame> results_;
    QLabel* status_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    quint64 generation_ = 0;
    bool quick_ = false, cursor_ = false, composing_ = false, applying_ = false, transferring_ = false;
};
}
