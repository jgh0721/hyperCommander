#pragma once

#include <string>
#include <QObject>
#include <QString>
#include <qt_windows.h>

namespace hc::ui
{
    inline QString FileOperationErrorText(const std::string& Code)
    {
        const auto Text = QString::fromStdString(Code);
        if (Code == "reparse-entry-denied") return QObject::tr("연결 또는 재분석 지점은 안전을 위해 삭제하지 않습니다. (%1)").arg(Text);
        if (!Text.startsWith("win32:")) return Text;
        bool Valid = false;
        const auto Number = Text.mid(6).toULong(&Valid);
        wchar_t Message[2048]{};
        if (Valid && FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, Number, 0, Message, 2048, nullptr))
            return QObject::tr("%1 (Windows 오류 %2)").arg(QString::fromWCharArray(Message).trimmed()).arg(Number);
        return Text;
    }
}
