#pragma once

#include <QFontDatabase>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QResource>

namespace hc::fonts
{
    inline void RegisterBundledFonts()
    {
        const auto Resource = QDir(QCoreApplication::applicationDirPath()).filePath("Libs/Fonts/D2Coding.rcc");
        if (!QResource::registerResource(Resource))
        {
            qWarning().noquote() << "Could not load shared font resource:" << Resource;
            return;
        }
        // Register before reading preferences; no system-wide font installation is needed.
        for (const auto* Name : { "D2Coding-Regular.ttf", "D2Coding-Bold.ttf",
                                 "D2CodingLigature-Regular.ttf", "D2CodingLigature-Bold.ttf" })
        {
            const auto Path = QStringLiteral(":/hc/fonts/") + QLatin1String(Name);
            if (QFontDatabase::addApplicationFont(Path) < 0)
                qWarning().noquote() << "Could not load bundled font:" << Path;
        }
    }
}
