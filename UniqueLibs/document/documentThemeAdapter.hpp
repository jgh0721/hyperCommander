#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
class QApplication;
class QPlainTextEdit;
class QWidget;
namespace hc::document
{
    class DocumentThemeAdapter final
    {
    public:
        static void Install(QApplication& App);
        static void InstallLocal(QWidget& Widget);
        static void ReleaseLocal(QWidget& Widget);
        /// Tokens, accent, fonts and the light/dark choice come from the product settings (HyperCommander.json in
        /// Directory; HyperEditor.json there is the fallback of a standalone editor). Reapplied only when they change.
        /// Returns false when neither file is readable.
        static bool LoadProductTheme(const QString& Directory);
        /// LoadProductTheme in two steps: reading and parsing the file runs on any thread, applying
        /// on the GUI thread. An empty object means neither file is readable.
        static QJsonObject ReadProductTheme(const QString& Directory);
        static bool ApplyProductTheme(const QJsonObject& Product);
        /// The files LoadProductTheme reads, in the order it reads them.
        static QStringList ProductThemeFiles(const QString& Directory);
        /// "product" follows the product's light/dark; "system" · "light" · "dark" override it for this app only.
        static bool SetMode(const QString& Mode);
        static void Attach(QPlainTextEdit* Editor);
    };
}
