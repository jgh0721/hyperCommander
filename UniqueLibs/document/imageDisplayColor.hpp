#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <QColorSpace>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QScreen>

namespace hc::document
{
// Called on the GUI thread. Keep monitor lookup out of page/animation workers.
inline QColorSpace ImageDisplayColorSpace(QScreen *Screen)
{
    struct Profile
    {
        QColorSpace color_;
        qint64 checked_ = 0;
    };
    static QHash<QString, Profile> Profiles;
    if (!Screen)
        return QColorSpace(QColorSpace::SRgb);
    auto &Cached = Profiles[Screen->name()];
    const auto Now = QDateTime::currentMSecsSinceEpoch();
    if (Now - Cached.checked_ < 60000 && Cached.color_.isValid())
        return Cached.color_;
    Cached.checked_ = Now;
    Cached.color_ = QColorSpace(QColorSpace::SRgb);
    const auto Name = Screen->name().toStdWString();
    HDC Dc = CreateDCW(L"DISPLAY", Name.c_str(), nullptr, nullptr);
    if (!Dc)
        return Cached.color_;
    DWORD Count = 0;
    GetICMProfileW(Dc, &Count, nullptr);
    if (Count && Count < 32768)
    {
        std::wstring Path(Count, L'\0');
        if (GetICMProfileW(Dc, &Count, Path.data()))
        {
            QFile File(QString::fromWCharArray(Path.c_str()));
            if (File.open(QIODevice::ReadOnly))
            {
                const auto Color = QColorSpace::fromIccProfile(File.readAll());
                if (Color.isValid())
                    Cached.color_ = Color;
            }
        }
    }
    DeleteDC(Dc);
    return Cached.color_;
}
} // namespace hc::document
