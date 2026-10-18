#include "documentThemeAdapter.hpp"
#include <QApplication>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <fmstyle/ThemeManager.h>
namespace hc::document
{
    void DocumentThemeAdapter::Install(QApplication& App)
    {
        auto& Theme = fm::style::ThemeManager::instance();
        if (!Theme.isInstalled()) { Theme.setDesign(fm::style::Design::Watercolor); Theme.install(App); }
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
