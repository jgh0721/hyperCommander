#pragma once
#include "fileColorRules.hpp"
#include "UniqueLibs/productDefaults.hpp"

namespace hc::settings {
// Explicit opt-in examples. Existing user groups are never replaced or seeded
// implicitly. The preview clock is fixed so relative-date examples are repeatable.
class FileColorExamples
{
public:
    static qint64 ReferenceTime() { return QDateTime::fromString("2026-01-15T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch(); }
    static QList<FileRuleInput> Samples() {
        const auto Now = ReferenceTime(); QList<FileRuleInput> Result;
        const auto Add = [&](const QString& Name, quint64 Size, quint32 Attributes, int Hours) {
            FileRuleInput Input{Name, Attributes, Size, false}; Input.referenceUtcMs_ = Now; Input.modifiedUtcMs_ = Now - qint64(Hours) * 3600000; Result.append(Input);
        };
        Add("BandedPanelView.cpp", 24678, 0, 1);
        Add("Qt-6.12.0-windows-x64-installer.exe", 4015794422ull, 0, 48);
        Add("placeholder-notes.txt", 0, 0, 48);
        Add("Qt Creator 18.lnk", 1434, 0, 48);
        Add("font-pack_Cascadia-Code-NF_2026-07.7z", 92274688, 0, 48);
        Add("desktop.ini", 282, 6, 48);
        return Result;
    }
    static QJsonArray Groups() { return hc::defaults::ExampleFileGroups(); }
};
} // namespace hc::settings
