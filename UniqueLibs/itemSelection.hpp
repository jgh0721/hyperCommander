#pragma once
#include "UniqueLibs/productDefaults.hpp"

#include "settings/fileColorRules.hpp"
#include <QBitArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace hc::selection {
enum class Operation { Add, Remove, Replace, Intersect };
enum class Syntax { Wildcard, Contains, Regex };
enum class Scope { Files, Folders, Both };

struct Criteria {
    Operation operation_ = static_cast<Operation>(hc::defaults::DefaultValue("selection/operation").toInt());
    Syntax syntax_ = static_cast<Syntax>(hc::defaults::DefaultValue("selection/syntax").toInt());
    Scope scope_ = static_cast<Scope>(hc::defaults::DefaultValue("selection/scope").toInt());
    QString pattern_ = hc::defaults::DefaultValue("selection/pattern").toString(), exclude_, groupId_;
    bool caseSensitive_ = hc::defaults::DefaultValue("selection/caseSensitive").toBool();
    bool advanced_ = hc::defaults::DefaultValue("selection/advanced").toBool();
    bool all_ = hc::defaults::DefaultValue("selection/all").toBool();
    QString size_;
    bool minimumSize_ = hc::defaults::DefaultValue("selection/minimumSize").toBool();
    QDate modified_;
    QJsonObject group_;
    int groupVersion_ = 2;
    // Advanced-tab conditions in the file color rule format, column field conditions included
    // (design 7.1). They join the name, size and date terms under the same AND/OR choice.
    QJsonArray conditions_;

    QJsonObject Save() const {
        QJsonObject Result{{"operation", int(operation_)}, {"syntax", int(syntax_)}, {"scope", int(scope_)},
            {"pattern", pattern_}, {"exclude", exclude_}, {"groupId", groupId_},
            {"caseSensitive", caseSensitive_}, {"advanced", advanced_}, {"all", all_},
            {"size", size_}, {"minimumSize", minimumSize_}, {"modified", modified_.toString(Qt::ISODate)}};
        if (!conditions_.isEmpty()) Result["conditions"] = conditions_;
        return Result;
    }
    static Criteria Load(const QJsonObject& Value) {
        Criteria C;
        C.operation_ = Operation(qBound(0, Value["operation"].toInt(hc::defaults::DefaultValue("selection/operation").toInt()), 3));
        C.syntax_ = Syntax(qBound(0, Value["syntax"].toInt(hc::defaults::DefaultValue("selection/syntax").toInt()), 2));
        C.scope_ = Scope(qBound(0, Value["scope"].toInt(hc::defaults::DefaultValue("selection/scope").toInt()), 2));
        C.pattern_ = Value["pattern"].toString(hc::defaults::DefaultValue("selection/pattern").toString()); C.exclude_ = Value["exclude"].toString(hc::defaults::DefaultValue("selection/exclude").toString());
        C.groupId_ = Value["groupId"].toString(hc::defaults::DefaultValue("selection/groupId").toString());
        // Retired built-in templates keep their saved name pattern as direct input.
        if (C.groupId_.startsWith("builtin.")) C.groupId_.clear();
        C.caseSensitive_ = Value["caseSensitive"].toBool(hc::defaults::DefaultValue("selection/caseSensitive").toBool());
        C.advanced_ = Value["advanced"].toBool(hc::defaults::DefaultValue("selection/advanced").toBool()); C.all_ = Value["all"].toBool(hc::defaults::DefaultValue("selection/all").toBool());
        C.size_ = Value["size"].toString(hc::defaults::DefaultValue("selection/size").toString()); C.minimumSize_ = Value["minimumSize"].toBool(hc::defaults::DefaultValue("selection/minimumSize").toBool());
        C.modified_ = QDate::fromString(Value["modified"].toString(hc::defaults::DefaultValue("selection/modified").toString()), Qt::ISODate);
        C.conditions_ = Value["conditions"].toArray();
        return C;
    }
};

struct Group { QString id_, name_; QJsonObject rule_; int version_ = 2; };
inline QList<Group> Groups(const QJsonObject& UserRules) {
    QList<Group> Result;
    const auto Rules = settings::FileColorRules::Upgrade(UserRules);
    if (settings::FileColorRules::Validate(Rules, false).isEmpty())
        for (const auto& Value : Rules["groups"].toArray()) {
            const auto Rule = Value.toObject();
            if (Rule["enabled"].toBool(hc::defaults::DefaultValue("fileColorGroup/enabled").toBool()))
                Result.append({"user:" + Rule["id"].toString(), Rule["name"].toString(), Rule, Rules["version"].toInt()});
        }
    return Result;
}

// A row's outcome. Unknown: a field value is still missing; needed_ names the fields to read.
struct Verdict { hc::columns::Truth truth_ = hc::columns::Truth::False; QStringList needed_; QString error_; };
using FieldValueMap = QHash<QString, hc::columns::CellValue>;

class Matcher {
public:
    QString Compile(const Criteria& Criteria) {
        criteria_ = Criteria; include_.clear(); exclude_.clear(); hasGroup_ = hasConditions_ = false; size_.reset();
        if (Criteria.pattern_.size() > 4096 || Criteria.exclude_.size() > 4096)
            return QObject::tr("이름 조건은 4096자 이내로 입력하십시오.");
        if (!Criteria.groupId_.isEmpty()) {
            if (Criteria.group_.isEmpty()) return QObject::tr("파일 그룹이 삭제되었거나 사용할 수 없습니다.");
            const QJsonObject Document{{"version", Criteria.groupVersion_}, {"mode", "merge"}, {"groups", QJsonArray{Criteria.group_}}};
            const auto Error = group_.Load(Document);
            if (!Error.isEmpty()) return Error;
            hasGroup_ = true;
        }
        if (Criteria.advanced_ && !Criteria.conditions_.isEmpty()) {
            // The conditions form one rule group joined like the other advanced terms.
            const QJsonObject Group{{"id", "selection"}, {"name", QObject::tr("선택 조건")}, {"join", Criteria.all_ ? "all" : "any"}, {"conditions", Criteria.conditions_}};
            const auto Error = conditions_.Load(QJsonObject{{"version", 4}, {"mode", "merge"}, {"groups", QJsonArray{Group}}});
            if (!Error.isEmpty()) return Error;
            hasConditions_ = true;
        }
        auto Error = compile(Criteria.pattern_, Criteria.syntax_, include_);
        if (!Error.isEmpty()) return Error;
        Error = compile(Criteria.exclude_, Syntax::Wildcard, exclude_);
        if (!Error.isEmpty()) return Error;
        if (Criteria.advanced_ && !Criteria.size_.trimmed().isEmpty()) {
            bool Ok = false; const auto Value = Criteria.size_.toDouble(&Ok);
            if (!Ok || !std::isfinite(Value) || Value < 0 || Value >= double(std::numeric_limits<quint64>::max()) / 1048576.0)
                return QObject::tr("파일 크기는 0 이상의 MiB 값이어야 합니다.");
            size_ = static_cast<quint64>(Value * 1048576.0);
        }
        date_ = Criteria.modified_.isValid() ? std::optional<qint64>(Criteria.modified_.startOfDay().toMSecsSinceEpoch()) : std::nullopt;
        return {};
    }
    bool Matches(const settings::FileRuleInput& Input, QString* Error = nullptr, settings::FileColorRules::EvaluationContext* Context = nullptr, bool SizeKnown = true) const {
        const auto Result = Evaluate(Input, {}, Context, SizeKnown);
        if (Error && !Result.error_.isEmpty()) *Error = Result.error_;
        return Result.truth_ == hc::columns::Truth::True;
    }
    // Field ids the selected group and the advanced conditions read, without duplicates.
    QStringList RequiredFields() const {
        QStringList Result;
        if (hasGroup_) Result += group_.RequiredFields();
        if (hasConditions_) for (const auto& Field : conditions_.RequiredFields()) if (!Result.contains(Field)) Result.append(Field);
        return Result;
    }
    Verdict Evaluate(const settings::FileRuleInput& Input, const FieldValueMap& Fields, settings::FileColorRules::EvaluationContext* Context = nullptr, bool SizeKnown = true) const {
        using hc::columns::Truth;
        Verdict Result;
        if (Input.name_ == "..") return Result;
        const bool Folder = (Input.attributes_ & 0x10u) != 0;
        if ((criteria_.scope_ == Scope::Files && Folder) || (criteria_.scope_ == Scope::Folders && !Folder)) return Result;
        if (hit(exclude_, Input.name_)) return Result;
        // Each rule set reads its own slot order; a missing value reads as pending.
        const auto Run = [&](const settings::FileColorRules& Rules, settings::FileColorRules::EvaluationContext* With) {
            auto In = Input; In.fields_.clear();
            for (const auto& Field : Rules.RequiredFields()) In.fields_.append(Fields.value(Field, hc::columns::CellValue::Missing(hc::columns::ValueState::Pending)));
            const auto Outcome = Rules.Matches(In, With);
            if (Outcome.pending_) for (int Slot = 0; Slot < Rules.RequiredFields().size() && Slot < 64; ++Slot)
                if ((Outcome.neededSlots_ & (quint64(1) << Slot)) && !Result.needed_.contains(Rules.RequiredFields()[Slot])) Result.needed_.append(Rules.RequiredFields()[Slot]);
            return Outcome;
        };
        Truth Group = Truth::True;
        if (hasGroup_) {
            const auto Outcome = Run(group_, Context);
            if (!Outcome.error_.isEmpty()) { Result.error_ = Outcome.error_; return Result; }
            Group = Outcome.pending_ ? Truth::Unknown : Outcome.matched_ ? Truth::True : Truth::False;
            if (Group == Truth::False) { Result.needed_.clear(); return Result; }
        }
        Truth Any = Truth::False, All = Truth::True; bool Present = false;
        const auto Add = [&](Truth Value) { Present = true; Any = hc::columns::Combine(Any, Value, false); All = hc::columns::Combine(All, Value, true); };
        const auto Of = [](bool Value) { return Value ? Truth::True : Truth::False; };
        if (!include_.isEmpty()) Add(Of(hit(include_, Input.name_)));
        if (criteria_.advanced_ && size_) Add(Of(SizeKnown && (criteria_.minimumSize_ ? Input.size_ >= *size_ : Input.size_ <= *size_)));
        if (criteria_.advanced_ && date_) Add(Of(Input.modifiedUtcMs_ && *Input.modifiedUtcMs_ >= *date_));
        if (hasConditions_) {
            const auto Outcome = Run(conditions_, nullptr);
            if (!Outcome.error_.isEmpty()) { Result.error_ = Outcome.error_; Result.needed_.clear(); return Result; }
            Add(Outcome.pending_ ? Truth::Unknown : Of(Outcome.matched_));
        }
        const auto Terms = !Present ? Truth::True : criteria_.all_ || !criteria_.advanced_ ? All : Any;
        Result.truth_ = hc::columns::Combine(Group, Terms, true);
        if (Result.truth_ != Truth::Unknown) Result.needed_.clear();
        return Result;
    }
    static bool Selected(Operation Op, bool Before, bool Match) {
        switch (Op) {
        case Operation::Add: return Before || Match;
        case Operation::Remove: return Before && !Match;
        case Operation::Replace: return Match;
        case Operation::Intersect: return Before && Match;
        }
        return Before;
    }
private:
    QString compile(const QString& Text, Syntax Mode, QList<QRegularExpression>& Out) const {
        if (Text.trimmed().isEmpty()) return {};
        const auto Flags = criteria_.caseSensitive_ ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption;
        auto Patterns = Mode == Syntax::Wildcard ? Text.split(';', Qt::SkipEmptyParts) : QStringList{Text};
        for (auto Pattern : Patterns) {
            if (Mode == Syntax::Wildcard) {
                Pattern = Pattern.trimmed(); if (Pattern.isEmpty()) continue;
                if (Pattern == "*.*") Pattern = "*";
                Pattern = QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion);
            } else if (Mode == Syntax::Contains) Pattern = QRegularExpression::escape(Pattern);
            QRegularExpression Regex(Pattern, Flags);
            if (!Regex.isValid()) return QObject::tr("이름 조건 오류: %1").arg(Regex.errorString());
            Out.append(Regex);
        }
        if (Out.isEmpty()) return QObject::tr("이름 조건을 입력하십시오.");
        return {};
    }
    static bool hit(const QList<QRegularExpression>& Patterns, const QString& Name) {
        return std::any_of(Patterns.cbegin(), Patterns.cend(), [&](const auto& Pattern) { return Pattern.match(Name).hasMatch(); });
    }
    Criteria criteria_;
    QList<QRegularExpression> include_, exclude_;
    settings::FileColorRules group_, conditions_;
    bool hasGroup_ = false, hasConditions_ = false;
    std::optional<quint64> size_;
    std::optional<qint64> date_;
};
} // namespace hc::selection
