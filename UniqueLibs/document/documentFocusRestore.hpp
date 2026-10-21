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
