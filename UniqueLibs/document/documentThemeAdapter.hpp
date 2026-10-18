#pragma once
#include <QString>
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
        static bool SetMode(const QString& Mode);
        static void Attach(QPlainTextEdit* Editor);
    };
}
