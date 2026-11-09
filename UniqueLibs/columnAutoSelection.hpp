#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include <QDir>
#include <QJsonDocument>
#include "commonLibs/columns/columnProfileStorage.hpp"
#include "commonLibs/columns/builtInColumns.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "commonLibs/storage/entryIndex.hpp"
#include "settings/fileColorRules.hpp"
#include "shellFolderToken.hpp"

namespace hc::columns {
inline constexpr auto AutoSelectionKey = "ColumnsV2/AutoSelection";
inline QJsonObject EmptyAutoSelection() { return hc::defaults::DefaultValue("columnAutoSelection").toObject(); }
inline QString NormalizeFolderConditionPath(QString Path) {
    Path.replace('\\', '/');
    while (Path.endsWith('/') && Path.size() > 1 && !(Path.size() == 3 && Path[1] == ':')) Path.chop(1);
    return Path;
}
inline bool MatchFolderCondition(const QString& Pattern, const QString& Path, const QHash<QString, QString>& Resolved = {}) {
    const auto Token = hc::shell::ParseFolderToken(Pattern);
    if (Token.kind_ == hc::shell::FolderToken::Kind::Invalid) return false;
    const auto Candidate = NormalizeFolderConditionPath(Path);
    const auto Wildcard = [](const QString& Text) {
        return QRegularExpression::wildcardToRegularExpression(Text,
            QRegularExpression::NonPathWildcardConversion | QRegularExpression::UnanchoredWildcardConversion);
    };
    QString Prefix, Rest;
    if (Token.kind_ == hc::shell::FolderToken::Kind::None) Rest = NormalizeFolderConditionPath(Pattern.trimmed());
    else {
        const auto Base = Resolved.value(Token.token_);
        if (Base.isEmpty()) return false;
        Prefix = QRegularExpression::escape(NormalizeFolderConditionPath(Base));
        Rest = Token.rest_;
        // A redirected drive root already supplies the separator.
        if (Base.endsWith('/') || Base.endsWith('\\')) {
            const auto Normal = NormalizeFolderConditionPath(Base);
            if (Normal.endsWith('/') && Rest.startsWith('/')) Rest.remove(0, 1);
        }
        Rest = NormalizeFolderConditionPath(Rest);
        if (Rest == "/") Rest.clear();
    }
    const auto Matches = [&](const QString& Tail) {
        return QRegularExpression("\\A" + Prefix + Wildcard(Tail) + "\\z", QRegularExpression::CaseInsensitiveOption).match(Candidate).hasMatch();
    };
    if (Matches(Rest)) return true;
    if (Rest.endsWith("/*")) return Matches(NormalizeFolderConditionPath(Rest.left(Rest.size() - 2)));
    return false;
}
inline bool MigrateDownloadsColumnSet(QJsonObject& Columns, const QString& DownloadsPath, const QHash<QString, QString>& ResolvedFolders = {}) {
    auto Migrations = Columns.value("ColumnsV2/Migrations").toArray();
    if (Migrations.contains("downloads-set-1")) return false;
    auto Library = Columns.value("ColumnsV2/Profiles").toObject();
    auto Policy = Columns.contains(AutoSelectionKey) ? Columns[AutoSelectionKey].toObject() : EmptyAutoSelection();
    auto Rules = Policy["rules"].toArray(); const auto Id = IdText(DownloadsProfileId);
    bool Exists = Library.contains(Id), UserMatches = false;
    for (const auto& Value : Rules) {
        const auto Rule = Value.toObject(); Exists |= Rule["profile"] == QJsonValue(Id);
        const auto Pattern = Rule["path"].toString();
        auto Resolved = ResolvedFolders;
        const auto Token = hc::shell::ParseFolderToken(Pattern);
        if (Token.id_ == QUuid("{374DE290-123F-4565-9164-39C4925E467B}") || Token.name_.compare("Downloads", Qt::CaseInsensitive) == 0)
            Resolved[Token.token_] = DownloadsPath;
        if (!DownloadsPath.isEmpty() && !Pattern.trimmed().isEmpty() && MatchFolderCondition(Pattern, DownloadsPath, Resolved)) UserMatches = true;
    }
    if (!Exists && Policy["version"] == QJsonValue(1) && Policy["rules"].isArray() && Rules.size() < 128) {
        const QJsonObject Rule{{"enabled", true}, {"profile", Id}, {"path", "{374DE290-123F-4565-9164-39C4925E467B}\\*"}, {"group", ""}, {"percent", 50}};
        if (Rules.isEmpty()) Policy["enabled"] = true;
        if (UserMatches) Rules.append(Rule); else Rules.prepend(Rule);
        Policy["rules"] = Rules; Columns[AutoSelectionKey] = Policy; Columns["ColumnsV2/Profiles"] = WithBuiltInProfiles(Library);
    }
    Migrations.append("downloads-set-1"); Columns["ColumnsV2/Migrations"] = Migrations;
    return true;
}
inline QString ProfileRuleSummary(const QUuid& Id, const QJsonObject& Policy, const QJsonObject& Colors,
    const QVector<hc::shell::KnownFolderChoice>& Choices) {
    QStringList Parts;
    if (IsBuiltInProfile(Id)) Parts.append(QObject::tr("기본 제공"));
    QString First; int Count = 0;
    if (Policy["enabled"].toBool()) for (const auto& Value : Policy["rules"].toArray()) {
        const auto Rule = Value.toObject();
        if (!Rule["enabled"].toBool() || QUuid(Rule["profile"].toString()) != Id) continue;
        ++Count; if (Count != 1) continue;
        QStringList Conditions;
        auto Path = Rule["path"].toString(); const auto Token = hc::shell::ParseFolderToken(Path);
        for (const auto& Choice : Choices) if ((Token.id_ == Choice.folder_ && !Token.id_.isNull())
            || (Token.kind_ == hc::shell::FolderToken::Kind::KnownFolderName && Token.name_.compare(Choice.id_, Qt::CaseInsensitive) == 0)) {
            Path = (Choice.id_ == "downloads" ? QObject::tr("사용자 다운로드 폴더") : Choice.label_)
                + (Token.rest_.isEmpty() || Token.rest_ == "/*" ? QString{} : Token.rest_); break;
        }
        if (!Path.isEmpty()) Conditions.append(Path);
        const auto GroupId = Rule["group"].toString();
        if (!GroupId.isEmpty()) {
            QString Name = GroupId;
            for (const auto& Value : Colors["groups"].toArray()) if (Value.toObject()["id"].toString() == GroupId)
                Name = Value.toObject()["name"].toString(GroupId);
            Conditions.append(QObject::tr("%1 %2% 이상").arg(Name).arg(Rule["percent"].toInt()));
        }
        First = QObject::tr("자동: %1").arg(Conditions.join(" · "));
    }
    Parts.append(Count ? First : QObject::tr("수동"));
    if (Count > 1) Parts.append(QObject::tr("외 %1개").arg(Count - 1));
    return Parts.join(" · ");
}
inline QString ValidateAutoSelection(const QJsonObject& Policy, const QJsonObject& Library, const QJsonObject& Colors) {
    if (Policy["version"] != QJsonValue(1) || !Policy["enabled"].isBool() || !Policy["rules"].isArray()) return QObject::tr("자동 선택의 설정 형식을 확인하세요.");
    if (Policy["rules"].toArray().size() > 128) return QObject::tr("자동 조건은 최대 128개입니다.");
    if (!Policy["enabled"].toBool(hc::defaults::DefaultValue("columnAutoSelection/enabled").toBool())) return {}; // Allow disabling a policy whose referenced group was removed.
    QSet<QString> Groups;
    for (const auto& Value : Colors["groups"].toArray()) if (Value.toObject()["enabled"].toBool(hc::defaults::DefaultValue("fileColorGroup/enabled").toBool())) Groups.insert(Value.toObject()["id"].toString());
    for (const auto& Value : Policy["rules"].toArray()) {
        const auto Rule = Value.toObject();
        if (!Value.isObject() || !Rule["enabled"].isBool() || !Rule["profile"].isString()
            || !Rule["path"].isString() || !Rule["group"].isString() || !Rule["percent"].isDouble()) return QObject::tr("자동 조건의 형식을 확인하세요.");
        if (!Rule["enabled"].toBool(hc::defaults::DefaultValue("columnAutoRule/enabled").toBool())) continue;
        const auto Profile = DecodeProfile(Library[Rule["profile"].toString()].toObject());
        if (!Profile || !IsSupportedProfile(*Profile)) return QObject::tr("자동 조건에서 사용할 열 세트를 선택하세요.");
        const auto Path = Rule["path"].toString().trimmed(), Group = Rule["group"].toString();
        if (Path.isEmpty() && Group.isEmpty()) return QObject::tr("폴더 경로 또는 파일 그룹 조건을 입력하세요.");
        if (Path.size() > 4096 || Path.contains(QChar::Null)) return QObject::tr("폴더 경로 조건이 너무 길거나 올바르지 않습니다.");
        if (hc::shell::ParseFolderToken(Path).kind_ == hc::shell::FolderToken::Kind::Invalid) return QObject::tr("셸 폴더 GUID 형식을 확인하세요.");
        if (!Group.isEmpty() && !Groups.contains(Group)) return QObject::tr("자동 조건의 파일 그룹이 삭제되었거나 비활성입니다.");
        const auto Percent = Rule["percent"].toDouble();
        if (Percent < 1 || Percent > 100 || Percent != int(Percent)) return QObject::tr("파일 그룹 비율은 1~100% 정수입니다.");
    }
    return {};
}
inline hc::storage::Result<std::optional<ColumnProfile>> SelectAutomaticColumns(const QJsonObject& Policy,
    const QJsonObject& Library, const QJsonObject& Colors, QString Path,
    const std::shared_ptr<hc::storage::IEntryIndex>& Index, const hc::storage::OperationContext& Context = {},
    const QHash<QString, QString>& Resolved = {}, int* MatchedRule = nullptr) {
    using namespace hc::storage;
    if (MatchedRule) *MatchedRule = -1;
    if (!Policy["enabled"].toBool(hc::defaults::DefaultValue("columnAutoSelection/enabled").toBool())) return std::optional<ColumnProfile>{};
    if (!ValidateAutoSelection(Policy, Library, Colors).isEmpty()) return Error{ErrorCategory::InvalidRequest, "invalid-auto-column-policy"};
    Path = QDir::fromNativeSeparators(Path); QMap<QString, hc::settings::FileColorRules> Groups;
    const auto Rules = Policy["rules"].toArray();
    for (const auto& Value : Rules) {
        const auto Rule = Value.toObject(); const auto Group = Rule["group"].toString();
        if (!Rule["enabled"].toBool(hc::defaults::DefaultValue("columnAutoRule/enabled").toBool()) || Group.isEmpty()) continue;
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
                FileSnapshot File; File.name_ = Input.name_; File.size_ = Entry.size_; File.attributes_ = Entry.attributes_; File.parentPath_ = QDir::toNativeSeparators(Path);
                if (Entry.createdUtcMs_) File.created_ = QDateTime::fromMSecsSinceEpoch(Entry.createdUtcMs_, QTimeZone::UTC);
                if (Input.modifiedUtcMs_) File.modified_ = QDateTime::fromMSecsSinceEpoch(*Input.modifiedUtcMs_, QTimeZone::UTC);
                for (auto It = Groups.begin(); It != Groups.end(); ++It) {
                    // A folder-wide ratio cannot wait for I/O: list fields are resolved here, queried
                    // fields count as unsupported, so groups that need them never match (design 6.5).
                    Input.fields_.clear();
                    for (const auto& Field : It.value().RequiredFields())
                        Input.fields_.append(QueryFamily(Field).isEmpty() ? Resolve(Field, File) : CellValue::Missing(ValueState::Unsupported, "auto-columns-no-io"));
                    const auto Match = It.value().Matches(Input, Contexts[It.key()].get());
                    if (!Match.error_.isEmpty()) return Error{ErrorCategory::InvalidRequest, "auto-column-group-evaluation", {}, {}, false, Match.error_.toStdString()};
                    if (Match.matched_) ++Hits[It.key()];
                }
            }
            Offset += Page.Value().entries_.size();
        }
    }
    for (int RuleIndex = 0; RuleIndex < Rules.size(); ++RuleIndex) {
        const auto Rule = Rules[RuleIndex].toObject(); if (!Rule["enabled"].toBool(hc::defaults::DefaultValue("columnAutoRule/enabled").toBool())) continue;
        const auto Pattern = QDir::fromNativeSeparators(Rule["path"].toString().trimmed());
        if (!Pattern.isEmpty() && !MatchFolderCondition(Pattern, Path, Resolved)) continue;
        const auto Group = Rule["group"].toString();
        if (!Group.isEmpty() && (!Files || 100.0L * Hits[Group] / Files < Rule["percent"].toDouble())) continue;
        if (MatchedRule) *MatchedRule = RuleIndex;
        return DecodeProfile(Library[Rule["profile"].toString()].toObject());
    }
    return std::optional<ColumnProfile>{};
}
} // namespace hc::columns
