#pragma once
#include <QDir>
#include <QJsonDocument>
#include "commonLibs/columns/columnProfileStorage.hpp"
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/storage/entryIndex.hpp"
#include "settings/fileColorRules.hpp"

namespace hc::columns {
inline constexpr auto AutoSelectionKey = "ColumnsV2/AutoSelection";
inline QJsonObject EmptyAutoSelection() { return {{"version", 1}, {"enabled", false}, {"rules", QJsonArray{}}}; }
inline QString ValidateAutoSelection(const QJsonObject& Policy, const QJsonObject& Library, const QJsonObject& Colors) {
    if (Policy["version"] != QJsonValue(1) || !Policy["enabled"].isBool() || !Policy["rules"].isArray()) return QObject::tr("자동 선택의 설정 형식을 확인하세요.");
    if (Policy["rules"].toArray().size() > 128) return QObject::tr("자동 조건은 최대 128개입니다.");
    if (!Policy["enabled"].toBool()) return {}; // Allow disabling a policy whose referenced group was removed.
    QSet<QString> Groups;
    for (const auto& Value : Colors["groups"].toArray()) if (Value.toObject()["enabled"].toBool(true)) Groups.insert(Value.toObject()["id"].toString());
    for (const auto& Value : Policy["rules"].toArray()) {
        const auto Rule = Value.toObject();
        if (!Value.isObject() || !Rule["enabled"].isBool() || !Rule["profile"].isString()
            || !Rule["path"].isString() || !Rule["group"].isString() || !Rule["percent"].isDouble()) return QObject::tr("자동 조건의 형식을 확인하세요.");
        if (!Rule["enabled"].toBool()) continue;
        const auto Profile = DecodeProfile(Library[Rule["profile"].toString()].toObject());
        if (!Profile || !IsSupportedProfile(*Profile)) return QObject::tr("자동 조건에서 사용할 열 세트를 선택하세요.");
        const auto Path = Rule["path"].toString().trimmed(), Group = Rule["group"].toString();
        if (Path.isEmpty() && Group.isEmpty()) return QObject::tr("폴더 경로 또는 파일 그룹 조건을 입력하세요.");
        if (Path.size() > 4096 || Path.contains(QChar::Null)) return QObject::tr("폴더 경로 조건이 너무 길거나 올바르지 않습니다.");
        if (!Group.isEmpty() && !Groups.contains(Group)) return QObject::tr("자동 조건의 파일 그룹이 삭제되었거나 비활성입니다.");
        const auto Percent = Rule["percent"].toDouble();
        if (Percent < 1 || Percent > 100 || Percent != int(Percent)) return QObject::tr("파일 그룹 비율은 1~100% 정수입니다.");
    }
    return {};
}
inline hc::storage::Result<std::optional<ColumnProfile>> SelectAutomaticColumns(const QJsonObject& Policy,
    const QJsonObject& Library, const QJsonObject& Colors, QString Path,
    const std::shared_ptr<hc::storage::IEntryIndex>& Index, const hc::storage::OperationContext& Context = {}) {
    using namespace hc::storage;
    if (!Policy["enabled"].toBool()) return std::optional<ColumnProfile>{};
    if (!ValidateAutoSelection(Policy, Library, Colors).isEmpty()) return Error{ErrorCategory::InvalidRequest, "invalid-auto-column-policy"};
    Path = QDir::fromNativeSeparators(Path); QMap<QString, hc::settings::FileColorRules> Groups;
    const auto Rules = Policy["rules"].toArray();
    for (const auto& Value : Rules) {
        const auto Rule = Value.toObject(); const auto Group = Rule["group"].toString();
        if (!Rule["enabled"].toBool() || Group.isEmpty()) continue;
        for (const auto& G : Colors["groups"].toArray()) if (G.toObject()["id"] == QJsonValue(Group)) {
            auto Document = hc::settings::FileColorRules::Empty(); Document["groups"] = QJsonArray{G};
            Document["version"] = Colors["version"];
            const auto Error = Groups[Group].Load(Document);
            if (!Error.isEmpty()) return hc::storage::Error{ErrorCategory::InvalidRequest, "invalid-auto-column-group", {}, {}, false, Error.toStdString()};
            break;
        }
    }
    QMap<QString, quint64> Hits; quint64 Files = 0; const auto ReferenceTime = QDateTime::currentMSecsSinceEpoch();
    QMap<QString, std::shared_ptr<hc::settings::FileColorRules::EvaluationContext>> Contexts;
    for (auto It = Groups.cbegin(); It != Groups.cend(); ++It) Contexts[It.key()] = std::make_shared<hc::settings::FileColorRules::EvaluationContext>();
    if (!Groups.isEmpty() && Index) {
        for (std::uint64_t Offset = 0; Offset < Index->Count();) {
            if (Context.stopToken_.stop_requested()) return Error{ErrorCategory::Cancelled, "auto-columns-cancelled"};
            const auto Page = Index->Read(Offset, 256, Context); if (!Page.HasValue()) return Page.Failure();
            if (Page.Value().entries_.empty()) return Error{ErrorCategory::Unavailable, "auto-columns-incomplete-snapshot"};
            for (const auto& Entry : Page.Value().entries_) {
                if (Entry.directory_ || Entry.name_ == "..") continue;
                ++Files;
                hc::settings::FileRuleInput Input{QString::fromStdString(Entry.name_), Entry.attributes_, Entry.size_, false}; Input.referenceUtcMs_ = ReferenceTime;
                Input.modifiedUtcMs_ = hc::settings::FileColorRules::MetadataTime(Entry.modifiedUtcMs_);
                if (const auto Tag = Entry.hints_.find("reparseTag"); Tag != Entry.hints_.end()) { bool Ok = false; const auto Value = QString::fromStdString(Tag->second).toUInt(&Ok); if (Ok) Input.reparseTag_ = Value; }
                for (auto It = Groups.begin(); It != Groups.end(); ++It) {
                    const auto Match = It.value().Matches(Input, Contexts[It.key()].get());
                    if (!Match.error_.isEmpty()) return Error{ErrorCategory::InvalidRequest, "auto-column-group-evaluation", {}, {}, false, Match.error_.toStdString()};
                    if (Match.matched_) ++Hits[It.key()];
                }
            }
            Offset += Page.Value().entries_.size();
        }
    }
    for (const auto& Value : Rules) {
        const auto Rule = Value.toObject(); if (!Rule["enabled"].toBool()) continue;
        const auto Pattern = QDir::fromNativeSeparators(Rule["path"].toString().trimmed());
        if (!Pattern.isEmpty() && !QRegularExpression(QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion), QRegularExpression::CaseInsensitiveOption).match(Path).hasMatch()) continue;
        const auto Group = Rule["group"].toString();
        if (!Group.isEmpty() && (!Files || 100.0L * Hits[Group] / Files < Rule["percent"].toDouble())) continue;
        return DecodeProfile(Library[Rule["profile"].toString()].toObject());
    }
    return std::optional<ColumnProfile>{};
}
} // namespace hc::columns
