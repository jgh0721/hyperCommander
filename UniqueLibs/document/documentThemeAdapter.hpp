#pragma once
#include <QString>
class QApplication;
class QPlainTextEdit;
namespace hc::document
{
    class DocumentThemeAdapter final
    {
    public:
        static void Install(QApplication& App);
        static bool SetMode(const QString& Mode);
        static void Attach(QPlainTextEdit* Editor);
    };
}
