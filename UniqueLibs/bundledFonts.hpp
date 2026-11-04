#pragma once

#include <QFontDatabase>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QResource>
#include <QSet>

namespace hc::fonts
{
    inline void RegisterBundledFonts(const QString& Family = {})
    {
        const bool All = Family.isEmpty();
        const bool Ligature = Family.compare("D2Coding ligature", Qt::CaseInsensitive) == 0;
        if (!All && !Ligature && Family.compare("D2Coding", Qt::CaseInsensitive) != 0) return;
        static QSet<QString> Registered;
        static bool ResourceReady = false;
        const auto Resource = QDir(QCoreApplication::applicationDirPath()).filePath("Libs/Fonts/D2Coding.rcc");
        if (!ResourceReady && !QResource::registerResource(Resource))
        {
            qWarning().noquote() << "Could not load shared font resource:" << Resource;
            return;
        }
        ResourceReady = true;
        // Register the selected family before layout; other families remain available on demand.
        for (const auto* Name : { "D2Coding-Regular.ttf", "D2Coding-Bold.ttf",
                                 "D2CodingLigature-Regular.ttf", "D2CodingLigature-Bold.ttf" })
        {
            const QString FileName = QLatin1String(Name);
            if (!All && FileName.startsWith("D2CodingLigature") != Ligature) continue;
            if (Registered.contains(FileName)) continue;
            const auto Path = QStringLiteral(":/hc/fonts/") + QLatin1String(Name);
            if (QFontDatabase::addApplicationFont(Path) < 0)
                qWarning().noquote() << "Could not load bundled font:" << Path;
            else Registered.insert(FileName);
        }
    }
}
