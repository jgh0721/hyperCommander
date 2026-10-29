#pragma once

#include "commonLibs/document/documentFormatSupport.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "commonLibs/plugins/archiveVolumeNames.hpp"

namespace hc::plugins {
// Display catalog only: document handlers still detect content dynamically.
inline QStringList BuiltinExtensions(const QString& Id)
{
    if (Id == "builtin.sevenzip") return {"7z", "zip", "zipx", "rar", "001", "msi", "cab"};
    if (Id == "builtin.iso") return {"iso"};
    if (Id == "builtin.alzegg") return {"alz", "egg"};
    if (Id == "builtin.rhwp") return {"hwp", "hwpx"};
    QStringList Result;
    if (Id == "builtin.hyperviewer" || Id == "builtin.hypereditor")
        for (const auto Ext : hc::document::DocumentExtensions) Result.append(QString::fromUtf8(Ext.data(), qsizetype(Ext.size())));
    Result.removeDuplicates(); Result.sort(Qt::CaseInsensitive); return Result;
}

inline QString BuiltinDocumentSupport()
{
    return QStringLiteral("텍스트·소스 코드 · PDF · 이미지 · 만화/압축 이미지 · EPUB · HWP/HWPX (내용 감지)");
}

inline QJsonValue BuiltinApplication(const QJsonObject& Plugins, const QString& Id)
{
    const auto Builtins = Plugins.value("builtin");
    if (Builtins.isUndefined()) return {};
    if (!Builtins.isObject()) return QJsonArray{}; // Invalid policies fail closed.
    const auto Record = Builtins.toObject().value(Id);
    if (Record.isUndefined()) return {};
    if (!Record.isObject()) return QJsonArray{};
    const auto Rule = Record.toObject().value("application");
    const auto Mode = Rule.toObject().value("mode").toString("all");
    if (Mode != "all" && Mode != "excludeExtensions") return QJsonArray{};
    return Rule;
}

inline bool BuiltinArchiveAllowed(const PluginApplicationRule& Rule, const QString& Name)
{
    const auto Matches = [&](const QString& Candidate) {
        const auto Result = Rule.Matches(Candidate); return Result.HasValue() && Result.Value();
    };
    if (!Matches(Name)) return false;
    const auto Volume = ParseArchiveVolumeName(Name);
    if (Volume.kind_ == ArchiveVolumeKind::None) return true;
    if (!Matches(Volume.firstName_)) return false;
    if (Volume.kind_ == ArchiveVolumeKind::Numbered)
        return Matches(Volume.firstName_.left(Volume.firstName_.lastIndexOf('.')));
    return true;
}
}
