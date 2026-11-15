#include "stdafx.h"
#include "registryFavorites.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QSet>
#include <QtEndian>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryFavorites, "hc.registry.favorites")

namespace hc::registry {
RegistryLocation RegistryFavorites::SharedLocation()
{
    return RegistryLocation::Parse(QStringLiteral("HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit\\Favorites")).value();
}
QList<RegistryFavorite> RegistryFavorites::Decode(const KeySnapshot& Snapshot, View SelectedView)
{
    QList<RegistryFavorite> Result;
    for (const auto& Value : Snapshot.values) {
        if (Value.type != REG_SZ || !Value.Warning().isEmpty()) continue;
        QString Path;
        for (qsizetype Offset = 0; Offset + 1 < Value.bytes.size(); Offset += 2) {
            const auto Unit = qFromLittleEndian<quint16>(Value.bytes.constData() + Offset);
            if (!Unit) break;
            Path.append(QChar(Unit));
        }
        if (auto Location = RegistryLocation::Parse(Path, SelectedView)) Result.append({Value.name, *Location});
    }
    std::sort(Result.begin(), Result.end(), [](const auto& A, const auto& B) {
        return QString::compare(A.name, B.name, Qt::CaseInsensitive) < 0;
    });
    return Result;
}
QList<RegistryOperation> RegistryFavorites::Toggle(const RegistryLocation& Store, const KeySnapshot& Snapshot,
    const RegistryLocation& Target)
{
    QList<RegistryOperation> Operations;
    for (const auto& Favorite : Decode(Snapshot, Target.view)) {
        if (Favorite.location.Id() == Target.Id())
            Operations.append({OperationKind::DeleteValue, Store, {Favorite.name}});
    }
    if (!Operations.isEmpty()) return Operations;
    QSet<QString> Names;
    for (const auto& Value : Snapshot.values) Names.insert(Value.name.toCaseFolded());
    const auto Base = Target.key.isEmpty() ? Target.Path() : Target.key.section('\\', -1);
    auto Name = Base;
    for (int Suffix = 2; Names.contains(Name.toCaseFolded()); ++Suffix) Name = Base + QStringLiteral(" (%1)").arg(Suffix);
    RegistryValue Value{Name, REG_SZ, {}};
    // Regedit stores full hive names and has no per-favorite WOW64 selector.
    const auto Path = Target.Path(); Value.bytes.resize((Path.size() + 1) * 2);
    for (qsizetype I = 0; I < Path.size(); ++I) qToLittleEndian<quint16>(Path[I].unicode(), Value.bytes.data() + I * 2);
    qToLittleEndian<quint16>(0, Value.bytes.data() + Path.size() * 2);
    Operations.append({OperationKind::CreateKey, Store});
    Operations.append({OperationKind::SetValue, Store, Value});
    return Operations;
}
} // namespace hc::registry
