#include "stdafx.h"
#include "registryPolicies.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryLocalPolicy.hpp"
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFutureWatcher>
#include <QtConcurrent>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicies, "hc.registry.policies")
namespace hc::registry {
RegistryPolicies::RegistryPolicies(QObject* Parent) : QObject(Parent)
{
    model_ = new RegistryResultModel({tr("정책 / 값"), tr("상태"), tr("범위"), tr("설명")}, this);
}
RegistryPolicies::~RegistryPolicies() { if (cancel_) cancel_->store(true); }
std::optional<RegistryPolicyEntry> RegistryPolicies::Entry(const QString& Id) const
{
    const auto Found = data_.entries.constFind(Id);
    return Found == data_.entries.cend() ? std::nullopt : std::optional<RegistryPolicyEntry>(*Found);
}
void RegistryPolicies::Reload(std::shared_ptr<const AdmxCatalog> Catalog)
{
    if (cancel_) cancel_->store(true);
    cancel_ = std::make_shared<std::atomic_bool>(false); const auto Cancel = cancel_;
    const auto Generation = ++generation_; loading_ = true;
    auto* Watcher = new QFutureWatcher<RegistryPolicyScan>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation != generation_) return;
        data_ = std::move(Result); loading_ = false; model_->SetRows(data_.rows); emit Changed();
    });
    Watcher->setFuture(QtConcurrent::run([Catalog, Cancel] {
        if (!Catalog) return RegistryPolicyScan{};
        WinRegistryBackend Backend;
        return Scan(*Catalog, Backend, LocalPolicyStore::Read(true), LocalPolicyStore::Read(false), Cancel.get());
    }));
}
RegistryPolicyScan RegistryPolicies::Scan(const AdmxCatalog& Catalog, IRegistryBackend& Backend,
    const RegistryResult<QByteArray>& MachinePolicy, const RegistryResult<QByteArray>& UserPolicy, const std::atomic_bool* Cancel)
{
    RegistryPolicyScan Result;
    RegistryPolicyInspector Inspector(Backend, Cancel);
    for (const bool Machine : {true, false}) {
        const auto& Local = Machine ? MachinePolicy : UserPolicy;
        const auto Parsed = Local && !Local.value.isEmpty() ? RegistryPolCodec::Read(Local.value) : RegistryPolResult{};
        const quint32 LocalError = !Local ? Local.error : !Parsed ? ERROR_INVALID_DATA : 0;
        const auto Scope = Machine ? tr("컴퓨터") : tr("사용자");
        if (LocalError) Result.diagnostics.append(Scope + tr(" Registry.pol 읽기 실패 (%1)").arg(LocalError));
        for (const auto& Policy : Catalog.policies) {
            if (Cancel && Cancel->load()) return Result;
            if (Policy.scope != "Both" && Policy.scope != (Machine ? "Machine" : "User")) continue;
            auto Observation = Inspector.Inspect(Policy, Machine, Parsed.records);
            // Include read failures as visible diagnostics, never as an absent policy.
            if (!Observation.present && Observation.diagnostic.isEmpty()) continue;
            RegistryResultRow Row; Row.id = (Machine ? "machine:" : "user:") + Policy.id;
            Row.groups = {Scope}; if (!Policy.category.isEmpty()) Row.groups.append(Policy.category.split(QStringLiteral(" › ")));
            Row.cells = {Policy.title, RegistryPolicyDefinition::StateText(Observation.state), Scope, Policy.explanation};
            Row.location = RegistryLocation::Parse((Machine ? "HKLM\\" : "HKCU\\") + Policy.key);
            Row.payload = {{"policyId", Policy.id}, {"machine", Machine}, {"known", true}};
            Row.searchText = Policy.id + '\n' + Policy.key + '\n' + Observation.diagnostic;
            Result.entries.insert(Row.id, {Policy, std::move(Observation), Machine, Local.value, LocalError});
            Result.rows.append(std::move(Row));
        }
        QList<RegistryLocation> Pending;
        for (const auto& Key : {QString("Software\\Policies"), QString("Software\\Microsoft\\Windows\\CurrentVersion\\Policies")})
            Pending.append(RegistryLocation::Parse((Machine ? "HKLM\\" : "HKCU\\") + Key).value());
        int Keys = 0, Values = 0;
        while (!Pending.isEmpty()) {
            if (Cancel && Cancel->load()) return Result;
            if (++Keys > 10000 || Values > 50000) { Result.diagnostics.append(Scope + tr(" 정책 검색 한도에 도달함 · 일부 결과")); break; }
            const auto Location = Pending.takeLast(); const auto Snapshot = Backend.Enumerate(Location, Cancel);
            if (!Snapshot) {
                if (Snapshot.error != ERROR_FILE_NOT_FOUND && Snapshot.error != ERROR_PATH_NOT_FOUND)
                    Result.diagnostics.append(Location.Path() + tr(" 읽기 실패 (%1)").arg(Snapshot.error));
                continue;
            }
            if (Snapshot.value.info.link) continue;
            for (const auto& Child : Snapshot.value.keys) if (!Child.link) Pending.append(Location.Child(Child.name));
            for (const auto& Value : Snapshot.value.values) {
                ++Values; if (!Catalog.Find(Location, Value.name).isEmpty()) continue;
                RegistryResultRow Row; Row.id = "unknown:" + Location.Id() + ':' + QString::fromLatin1(Value.name.toUtf8().toHex());
                Row.groups = {Scope, tr("해석하지 못한 정책 값")};
                const auto Text = RegistryValueCodec::DecodeText(Value); const auto Number = RegistryValueCodec::DecodeNumber(Value);
                const auto Display = Text.lossless ? Text.text : Number ? QString::number(Number.value) : QString::fromLatin1(Value.bytes.left(48).toHex(' '));
                Row.cells = {Value.name.isEmpty() ? tr("(기본값)") : Value.name, Display, Scope, Location.Path()};
                Row.location = Location; Row.value = Value; Row.payload = {{"known", false}, {"machine", Machine}};
                Row.searchText = Location.Path(); Result.rows.append(std::move(Row));
            }
        }
    }
    return Result;
}
} // namespace hc::registry
