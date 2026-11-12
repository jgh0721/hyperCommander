#pragma once
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <qt_windows.h>
#include <functional>

namespace hc::document {
// The window that receives keys now. GetFocus() answers for the calling thread's queue only: a viewer host
// that has left HyperCommander's queue still names its own last window there.
inline HWND KeyboardFocusWindow()
{
    GUITHREADINFO Info{sizeof(GUITHREADINFO)};
    const auto Foreground = GetForegroundWindow();
    return Foreground && GetGUIThreadInfo(GetWindowThreadProcessId(Foreground, nullptr), &Info) ? Info.hwndFocus : nullptr;
}
inline bool KeyboardInside(HWND Native)
{
    const auto Focus = KeyboardFocusWindow();
    return Native && Focus && (Focus == Native || IsChild(Native, Focus));
}
// The Ctrl+Q preview shell (carries hc.quickPreview) containing Widget. Searched through the parents:
// a view's window() can be an intermediate Qt::Window rather than the shell.
inline const QWidget* QuickPreviewShell(const QWidget* Widget)
{
    for (auto* At = Widget; At; At = At->parentWidget()) if (At->property("hc.quickPreview").toBool()) return At;
    return nullptr;
}
// A Ctrl+Q preview loads while the file panel keeps the keyboard.
// Until the user enters it, nothing in the preview may take the keyboard.
inline bool PreviewKeepsPanelKeyboard(const QWidget* Widget)
{
    const auto* Shell = QuickPreviewShell(Widget);
    return Shell && !KeyboardInside(reinterpret_cast<HWND>(Shell->internalWinId()));
}
// Restore document input after Qt finishes its activation fallback. Repair the
// native HWND too: setFocus alone can keep a stale native input destination.
class DocumentFocusRestore final : public QObject, public QAbstractNativeEventFilter {
public:
    DocumentFocusRestore(QWidget* Scope,std::function<QWidget*()> Target,std::function<void()> After={})
        :QObject(Scope),scope_(Scope),target_(std::move(Target)),after_(std::move(After)) {
        qApp->installEventFilter(this);qApp->installNativeEventFilter(this);
        connect(qApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState State){if(State==Qt::ApplicationActive)request();});
    }
    ~DocumentFocusRestore() override {
        if(qApp){qApp->removeEventFilter(this);qApp->removeNativeEventFilter(this);}
    }
protected:
    bool eventFilter(QObject* Object,QEvent* Event) override {
        if(Object==scope_ && (Event->type()==QEvent::WindowActivate || Event->type()==QEvent::WindowUnblocked))request();
        if(Event->type()==QEvent::FocusIn && static_cast<QFocusEvent*>(Event)->reason()==Qt::ActiveWindowFocusReason)
            if(auto* Widget=qobject_cast<QWidget*>(Object);scope_ && Widget && (Widget==scope_ || scope_->isAncestorOf(Widget)))request();
        return false;
    }
    bool nativeEventFilter(const QByteArray& Type,void* Message,qintptr*) override {
        if(Type=="windows_generic_MSG" && scope_ && scope_->internalWinId()) {
            const auto* Native=static_cast<MSG*>(Message);
            if(Native->hwnd==reinterpret_cast<HWND>(scope_->internalWinId()) && Native->message==WM_ACTIVATE
                && LOWORD(Native->wParam)!=WA_INACTIVE)request();
        }
        return false;
    }
private:
    void request() {
        if(pending_)return;pending_=true;
        QTimer::singleShot(0,this,[this]{
            pending_=false;
            if(!scope_ || !scope_->isVisible() || !scope_->isEnabled() || scope_->window()->isMinimized()
                || QApplication::activeModalWidget() || QApplication::activePopupWidget())return;
            const auto Native=reinterpret_cast<HWND>(scope_->internalWinId());
            if(PreviewKeepsPanelKeyboard(scope_))return;
            // A shown view of another process inside this window (the EPUB reader) already has the keys.
            if(const auto Focus=KeyboardFocusWindow();Native && Focus && IsChild(Native,Focus) && IsWindowVisible(Focus)
                && GetWindowThreadProcessId(Focus,nullptr)!=GetCurrentThreadId())return;
            const auto Root=Native?GetAncestor(Native,GA_ROOT):nullptr;
            if(!Root || GetAncestor(GetForegroundWindow(),GA_ROOT)!=Root)return;
            if(QApplication::activeWindow()!=scope_->window())QApplication::setActiveWindow(scope_->window());
            SetActiveWindow(Root);
            QPointer<QWidget> Target=target_();
            while(Target && Target->focusProxy())Target=Target->focusProxy();
            if(!Target || !Target->isVisible() || !Target->isEnabled() || (Target!=scope_ && !scope_->isAncestorOf(Target)))return;
            SetFocus(reinterpret_cast<HWND>(Target->effectiveWinId()));
            if(Target)Target->setFocus(Qt::OtherFocusReason);
            if(after_)after_();
        });
    }
    QPointer<QWidget> scope_;
    std::function<QWidget*()> target_;
    std::function<void()> after_;
    bool pending_=false;
};
}
