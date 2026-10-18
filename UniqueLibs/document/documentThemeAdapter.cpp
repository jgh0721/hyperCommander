#include "documentThemeAdapter.hpp"
#include <QApplication>
#include <QPlainTextEdit>
#include <QWidget>
#include <QFontDatabase>
#include <QChildEvent>
#include <QStyle>
#include <fmstyle/ThemeManager.h>
namespace hc::document
{
    namespace
    {
        // A viewer can live on the SDK GUI thread. Keep its style local rather
        // than changing QApplication's style from that thread. QWidget styles
        // do not inherit, so include children created by later document loads.
        class LocalTheme final : public QObject
        {
        public:
            explicit LocalTheme(QWidget& Root) : QObject(&Root), root_(Root),
                style_(fm::style::createStyle(fm::style::Design::Watercolor))
            {
                setObjectName("documentLocalTheme");
                style_->setParent(this);
                watch(&Root);
                Root.setPalette(fm::style::ThemeManager::instance().colors().toPalette());
                connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed,
                    this, [this] { root_.setPalette(fm::style::ThemeManager::instance().colors().toPalette()); root_.update(); });
            }
            void Detach()
            {
                // QWidgetAction reparents controls while toolbars are destroyed.
                // Drop their borrowed style before deleting its owner, while
                // the workspace and every control still have QWidget vtables.
                root_.removeEventFilter(this);
                for (auto* Child : root_.findChildren<QObject*>()) Child->removeEventFilter(this);
                const auto Widgets = root_.findChildren<QWidget*>();
                for (auto* Widget : Widgets) Widget->setStyle(nullptr);
                root_.setStyle(nullptr);
            }
        protected:
            bool eventFilter(QObject* Object, QEvent* Event) override
            {
                if (Event->type() == QEvent::ChildAdded || Event->type() == QEvent::ChildPolished)
                    watch(static_cast<QChildEvent*>(Event)->child());
                if (Event->type() == QEvent::Polish)
                    if (auto* Widget = qobject_cast<QWidget*>(Object); Widget && Widget->style() != style_)
                        Widget->setStyle(style_);
                return false;
            }
        private:
            void watch(QObject* Object)
            {
                if (Object == this || Object == style_) return;
                Object->installEventFilter(this);
                if (auto* Widget = qobject_cast<QWidget*>(Object); Widget && Widget->style() != style_)
                    Widget->setStyle(style_);
                const auto Children = Object->children();
                for (auto* Child : Children) watch(Child);
            }
            QWidget& root_;
            QStyle* style_;
        };
    }
    void DocumentThemeAdapter::Install(QApplication& App)
    {
        auto& Theme = fm::style::ThemeManager::instance();
        if (!Theme.isInstalled()) { Theme.setDesign(fm::style::Design::Watercolor); Theme.install(App); }
    }
    void DocumentThemeAdapter::InstallLocal(QWidget& Widget)
    {
        fm::style::ThemeManager::instance().setDesign(fm::style::Design::Watercolor);
        new LocalTheme(Widget);
    }
    void DocumentThemeAdapter::ReleaseLocal(QWidget& Widget)
    {
        auto* Theme = Widget.findChild<QObject*>("documentLocalTheme", Qt::FindDirectChildrenOnly);
        if (!Theme) return;
        static_cast<LocalTheme*>(Theme)->Detach();
        delete Theme;
    }
    bool DocumentThemeAdapter::SetMode(const QString& Mode)
    {
        auto& Theme = fm::style::ThemeManager::instance();
        if (Mode != "system" && Mode != "light" && Mode != "dark") return false;
        Theme.setScheme(Mode == "light" ? fm::style::ThemeManager::Scheme::Light : Mode == "dark"
            ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::System); return true;
    }
    void DocumentThemeAdapter::Attach(QPlainTextEdit* Editor)
    {
        auto& Theme = fm::style::ThemeManager::instance();
        auto Apply = [Editor] {
            const auto& Current = fm::style::ThemeManager::instance();
            auto Font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            if (!Current.monoFontFamily().isEmpty()) Font.setFamily(Current.monoFontFamily());
            Font.setPixelSize(Current.monoFontPx()); Editor->setFont(Font);
            Editor->setPalette(QApplication::palette());
        };
        QObject::connect(&Theme, &fm::style::ThemeManager::changed, Editor, Apply);
        QObject::connect(&Theme, &fm::style::ThemeManager::fontsChanged, Editor, Apply);
        Apply();
    }
}
