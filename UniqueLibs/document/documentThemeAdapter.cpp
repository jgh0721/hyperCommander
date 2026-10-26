#include "documentThemeAdapter.hpp"
#include <QApplication>
#include <QPlainTextEdit>
#include <QWidget>
#include <QFontDatabase>
#include <QChildEvent>
#include <QStyle>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <fmstyle/ThemeManager.h>
#include "UniqueLibs/productTheme.hpp"
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
                style_(fm::style::createStyle(hc::theme::ProductDesign))
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
    namespace
    {
        QString& RequestedMode() { static QString Mode = QStringLiteral("product"); return Mode; }
        QString& ProductMode() { static QString Mode = QStringLiteral("system"); return Mode; }
        QJsonObject& AppliedProduct() { static QJsonObject Value; return Value; }
        void ApplyScheme(const QString& Mode)
        {
            fm::style::ThemeManager::instance().setScheme(Mode == "light" ? fm::style::ThemeManager::Scheme::Light : Mode == "dark"
                ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::System);
        }
        QJsonObject ReadRoot(const QString& Path)
        {
            QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {};
            return QJsonDocument::fromJson(File.readAll()).object();
        }
    }
    bool DocumentThemeAdapter::LoadProductTheme(const QString& Directory)
    {
        auto Root = ReadRoot(QDir(Directory).filePath(QStringLiteral("HyperCommander.json")));
        if (Root.isEmpty()) Root = ReadRoot(QDir(Directory).filePath(QStringLiteral("HyperEditor.json")));
        if (Root.isEmpty()) return false;
        const auto Appearance = Root["appearance"].toObject(); const auto General = Root["general"].toObject();
        const QJsonObject Product{{"scheme", Appearance["watercolorTheme"]}, {"mode", General["themeMode"]}, {"mono", General["monoFontFamily"]},
            {"monoPx", General["monoFontPx"]}, {"darkTitle", General["darkTitleBar"]}};
        if (Product == AppliedProduct()) return true;
        AppliedProduct() = Product;
        const auto Mode = General["themeMode"].toString();
        ProductMode() = Mode == "light" || Mode == "dark" ? Mode : QStringLiteral("system");
        auto& Theme = fm::style::ThemeManager::instance();
        fm::style::ThemeManager::Batch Batch(Theme);
        Theme.setColorScheme(hc::theme::ReadProductScheme(Appearance["watercolorTheme"].toObject()));
        Theme.setMonoFont(General["monoFontFamily"].toString("Cascadia Mono"), General["monoFontPx"].toInt(12));
        Theme.setDarkTitleBar(General["darkTitleBar"].toBool(true));
        if (RequestedMode() == "product") ApplyScheme(ProductMode());
        return true;
    }
    void DocumentThemeAdapter::Install(QApplication& App)
    {
        auto& Theme = fm::style::ThemeManager::instance();
        if (!Theme.isInstalled()) { Theme.setDesign(hc::theme::ProductDesign); Theme.install(App); }
    }
    void DocumentThemeAdapter::InstallLocal(QWidget& Widget)
    {
        fm::style::ThemeManager::instance().setDesign(hc::theme::ProductDesign);
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
        if (Mode != "product" && Mode != "system" && Mode != "light" && Mode != "dark") return false;
        RequestedMode() = Mode; ApplyScheme(Mode == "product" ? ProductMode() : Mode); return true;
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
