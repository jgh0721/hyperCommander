#include "stdafx.h"
#include "registryPolicyState.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicyState, "hc.registry.policyState")
namespace hc::registry {
namespace {
void CollectKeys(const AdmxNode& Node, const QString& Parent, QSet<QString>& Keys)
{
    const auto Key = Node.Attribute("key", Node.Attribute("defaultKey", Parent));
    if (!Key.isEmpty()) Keys.insert(Key.toCaseFolded());
    for (const auto& Child : Node.children) CollectKeys(Child, Key, Keys);
}
// Read-only ownership probing must also work for unsupported policy definitions.
// A definition alone is not evidence that a policy is applied.
bool References(const AdmxNode& Node, const QString& Parent, const QString& Key, const QString& Name)
{
    const auto CurrentKey = Node.Attribute("key", Node.Attribute("defaultKey", Parent));
    if (CurrentKey.compare(Key, Qt::CaseInsensitive) == 0
        && (Node.tag == "list" || (Node.attributes.contains("valueName")
            && Node.Attribute("valueName").compare(Name, Qt::CaseInsensitive) == 0))) return true;
    for (const auto& Child : Node.children) if (References(Child, CurrentKey, Key, Name)) return true;
    return false;
}
std::optional<RegistryValue> Value(const PolicyObservation& Observation, const QString& Key, const QString& Name)
{
    const auto Found = Observation.keys.constFind(Key.toCaseFolded());
    if (Found == Observation.keys.cend() || !Found.value()) return {};
    for (const auto& Item : Found->value.values) if (Item.name.compare(Name, Qt::CaseInsensitive) == 0) return Item;
    return {};
}
bool Same(const std::optional<RegistryValue>& Left, const std::optional<RegistryValue>& Right)
{
    if (bool(Left) != bool(Right)) return false;
    return !Left || (Left->type == Right->type && Left->bytes == Right->bytes);
}
bool Matches(const PolicyProgram& Program, const PolicyObservation& Observation, bool& Evidence,
    const QList<RegistryPolRecord>& LocalRecords = {})
{
    Evidence = false;
    if (!Program) return false;
    bool Match = true;
    for (const auto& Assignment : Program.assignments) {
        const auto Actual = Value(Observation, Assignment.key, Assignment.name);
        if (Actual && Assignment.value && Same(Actual, Assignment.value)) Evidence = true;
        if (!Actual && Assignment.explicitDeletion) for (const auto& Record : LocalRecords)
            if (Record.Directive() == PolicyDirective::DeleteValue
                && Record.key.compare(Assignment.key, Qt::CaseInsensitive) == 0
                && Record.value.name.mid(6).compare(Assignment.name, Qt::CaseInsensitive) == 0) Evidence = true;
        if (Assignment.soft && !Assignment.value) continue;
        if (Assignment.soft && Actual) Evidence = true;
        else if (!Same(Actual, Assignment.value)) Match = false;
    }
    for (const auto& Key : Program.clearKeys) {
        const auto Found = Observation.keys.constFind(Key.toCaseFolded());
        if (Found == Observation.keys.cend() || !Found.value()) continue;
        for (const auto& Actual : Found->value.values) {
            bool Owned = false;
            for (const auto& Assignment : Program.assignments)
                if (Assignment.key.compare(Key, Qt::CaseInsensitive) == 0
                    && Assignment.name.compare(Actual.name, Qt::CaseInsensitive) == 0 && Assignment.value) { Owned = true; break; }
            if (!Owned) Match = false;
        }
    }
    return Match;
}
AdmxPolicy SingleElement(const AdmxPolicy& Policy, const AdmxNode& Element)
{
    auto Result = Policy; Result.definition.attributes.remove("valueName"); Result.definition.children.clear();
    AdmxNode Elements; Elements.tag = "elements"; Elements.children.append(Element); Result.definition.children.append(Elements);
    return Result;
}
}
PolicyObservation RegistryPolicyInspector::Inspect(const AdmxPolicy& Policy, bool Machine, const QList<RegistryPolRecord>& LocalRecords)
{
    PolicyObservation Result; Result.options = RegistryPolicyDefinition::Defaults(Policy);
    QSet<QString> Keys; CollectKeys(Policy.definition, Policy.key, Keys);
    for (const auto& Key : Keys) {
        if (cancel_ && cancel_->load()) { Result.diagnostic = QObject::tr("취소됨"); return Result; }
        const auto Location = RegistryLocation::Parse((Machine ? "HKLM\\" : "HKCU\\") + Key);
        if (!Location || Key.isEmpty()) { Result.diagnostic = QObject::tr("잘못된 정책 경로"); return Result; }
        if (!cache_.contains(Location->Id())) cache_.insert(Location->Id(), backend_.Enumerate(*Location, cancel_));
        const auto Snapshot = cache_.value(Location->Id()); Result.keys.insert(Key, Snapshot);
        if ((!Snapshot && Snapshot.error != ERROR_FILE_NOT_FOUND && Snapshot.error != ERROR_PATH_NOT_FOUND) || Snapshot.value.info.link) {
            Result.diagnostic = QObject::tr("정책 키를 읽을 수 없음 (%1)").arg(Snapshot.error ? Snapshot.error : ERROR_NOT_SUPPORTED);
            return Result;
        }
    }
    const auto Unconfigured = RegistryPolicyDefinition::Compile(Policy, PolicyState::NotConfigured, {});

    const auto Owns = [&](const QString& Key, const QString& Name) {
        if (!Unconfigured) return References(Policy.definition, Policy.key, Key, Name);
        if (Unconfigured.clearKeys.contains(Key, Qt::CaseInsensitive) || Unconfigured.additiveKeys.contains(Key, Qt::CaseInsensitive)) return true;
        for (const auto& Assignment : Unconfigured.assignments)
            if (Assignment.key.compare(Key, Qt::CaseInsensitive) == 0 && Assignment.name.compare(Name, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    for (auto I = Result.keys.cbegin(); I != Result.keys.cend(); ++I)
        for (const auto& Item : I->value.values) if (Owns(I.key(), Item.name)) Result.present = true;
    for (const auto& Record : LocalRecords) {
        const auto Name = Record.Directive() == PolicyDirective::DeleteValue ? Record.value.name.mid(6) : Record.Directive() == PolicyDirective::SoftValue ? Record.value.name.mid(7) : Record.value.name;
        if (Owns(Record.key, Name)) Result.local = true;
    }
    Result.present |= Result.local;
    if (!Unconfigured) {
        if (Result.present) Result.diagnostic = Unconfigured.error;
        else Result.state = PolicyState::NotConfigured;
        return Result;
    }
    if (const auto* Elements = Policy.definition.Child("elements")) for (const auto& Element : Elements->children) {
        const auto Id = Element.Attribute("id"), Key = Element.Attribute("key", Policy.key);
        auto Current = Value(Result, Key, Element.Attribute("valueName"));
        if (Element.Attribute("soft") == "true" || Element.Attribute("soft") == "1")
            for (const auto& Record : LocalRecords) if (Record.Directive() == PolicyDirective::SoftValue
                && Record.key.compare(Key, Qt::CaseInsensitive) == 0
                && Record.value.name.mid(7).compare(Element.Attribute("valueName"), Qt::CaseInsensitive) == 0) Current = Record.value;
        if (Element.tag == "boolean" || Element.tag == "enum") {
            const auto IsBoolean = Element.tag == "boolean";
            int Count = IsBoolean ? 2 : 0;
            if (!IsBoolean) for (const auto& Item : Element.children) if (Item.tag == "item") ++Count;
            int Found = -1;
            for (int Choice = 0; Choice < Count; ++Choice) {
                bool Evidence = false;
                const QVariant Option = IsBoolean ? QVariant(Choice != 0) : QVariant(Choice);
                const auto Program = RegistryPolicyDefinition::Compile(SingleElement(Policy, Element), PolicyState::Enabled, {{Id, Option}});
                if (Matches(Program, Result, Evidence) && Evidence) { if (Found >= 0) { Found = -1; break; } Found = Choice; }
            }
            if (Found >= 0) Result.options[Id] = IsBoolean ? QVariant(Found != 0) : QVariant(Found);
            else Result.options.remove(Id);
        } else if (Element.tag == "list") {
            const bool Explicit = Element.Attribute("explicitValue") == "true" || Element.Attribute("explicitValue") == "1";
            const auto Prefix = Element.Attribute("valuePrefix");
            QMap<qulonglong, QVariant> Numbered; QVariantList Entries;
            auto ListValues = Result.keys.value(Key.toCaseFolded()).value.values;
            if (Element.Attribute("additive") == "true" || Element.Attribute("additive") == "1") {
                QList<RegistryValue> Stored;
                for (const auto& Record : LocalRecords) if (Record.key.compare(Key, Qt::CaseInsensitive) == 0 && Record.Directive() == PolicyDirective::Value) Stored.append(Record.value);
                if (!Stored.isEmpty()) ListValues = Stored;
            }
            for (const auto& Item : ListValues) {
                const auto Text = RegistryValueCodec::DecodeText(Item); if (!Text.lossless) continue;
                const QVariant Entry = QVariantMap{{"name", Item.name}, {"data", Text.text}};
                if (Explicit) Entries.append(Entry);
                else if (Item.name.startsWith(Prefix, Qt::CaseInsensitive)) {
                    bool Ok = false; const auto Number = Item.name.mid(Prefix.size()).toULongLong(&Ok);
                    if (Ok && Number > 0) Numbered.insert(Number, Entry);
                }
            }
            if (!Explicit) for (const auto& Entry : Numbered) Entries.append(Entry);
            Result.options[Id] = Entries;
        } else if (Current) {
            const auto Number = RegistryValueCodec::DecodeNumber(*Current);
            const auto Text = RegistryValueCodec::DecodeText(*Current);
            if ((Element.tag == "decimal" || Element.tag == "longDecimal") && Number) Result.options[Id] = QString::number(Number.value);
            else if (Text.lossless) Result.options[Id] = Text.text;
            else Result.options.remove(Id);
        } else Result.options.remove(Id);
    }
    if (!Result.present) { Result.state = PolicyState::NotConfigured; return Result; }
    bool EnabledEvidence = false, DisabledEvidence = false;
    const bool Enabled = Matches(RegistryPolicyDefinition::Compile(Policy, PolicyState::Enabled, Result.options), Result, EnabledEvidence, LocalRecords) && EnabledEvidence;
    const bool Disabled = Matches(RegistryPolicyDefinition::Compile(Policy, PolicyState::Disabled, {}), Result, DisabledEvidence, LocalRecords) && DisabledEvidence;
    Result.state = Enabled != Disabled ? (Enabled ? PolicyState::Enabled : PolicyState::Disabled) : PolicyState::Partial;
    if (!Enabled && !Disabled) Result.diagnostic = QObject::tr("정의와 일치하지 않는 값 또는 삭제형 정책 · 원본 값 확인 필요");
    return Result;
}
} // namespace hc::registry
