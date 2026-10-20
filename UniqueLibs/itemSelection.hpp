#pragma once

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
    Operation operation_ = Operation::Add;
    Syntax syntax_ = Syntax::Wildcard;
    Scope scope_ = Scope::Files;
    QString pattern_ = "*", exclude_, groupId_;
    bool caseSensitive_ = false, advanced_ = false, all_ = true;
    QString size_;
    bool minimumSize_ = true;
    QDate modified_;
    QJsonObject group_;

    QJsonObject Save() const {
        return {{"operation", int(operation_)}, {"syntax", int(syntax_)}, {"scope", int(scope_)},
            {"pattern", pattern_}, {"exclude", exclude_}, {"groupId", groupId_},
            {"caseSensitive", caseSensitive_}, {"advanced", advanced_}, {"all", all_},
            {"size", size_}, {"minimumSize", minimumSize_}, {"modified", modified_.toString(Qt::ISODate)}};
    }
    static Criteria Load(const QJsonObject& Value) {
        Criteria C;
        C.operation_ = Operation(qBound(0, Value["operation"].toInt(), 3));
        C.syntax_ = Syntax(qBound(0, Value["syntax"].toInt(), 2));
        C.scope_ = Scope(qBound(0, Value["scope"].toInt(), 2));
        C.pattern_ = Value["pattern"].toString("*"); C.exclude_ = Value["exclude"].toString();
        C.groupId_ = Value["groupId"].toString();
        // Retired built-in templates keep their saved name pattern as direct input.
        if (C.groupId_.startsWith("builtin.")) C.groupId_.clear();
        C.caseSensitive_ = Value["caseSensitive"].toBool();
        C.advanced_ = Value["advanced"].toBool(); C.all_ = Value["all"].toBool(true);
        C.size_ = Value["size"].toString(); C.minimumSize_ = Value["minimumSize"].toBool(true);
        C.modified_ = QDate::fromString(Value["modified"].toString(), Qt::ISODate);
        return C;
    }
};

struct Group { QString id_, name_; QJsonObject rule_; };
inline QList<Group> Groups(const QJsonObject& UserRules) {
    QList<Group> Result;
    const auto Rules = settings::FileColorRules::Upgrade(UserRules);
    if (settings::FileColorRules::Validate(Rules).isEmpty())
        for (const auto& Value : Rules["groups"].toArray()) {
            const auto Rule = Value.toObject();
            if (Rule["enabled"].toBool(true))
                Result.append({"user:" + Rule["id"].toString(), Rule["name"].toString(), Rule});
        }
    return Result;
}

class Matcher {
public:
    QString Compile(const Criteria& Criteria) {
        criteria_ = Criteria; include_.clear(); exclude_.clear(); hasGroup_ = false; size_.reset();
        if (Criteria.pattern_.size() > 4096 || Criteria.exclude_.size() > 4096)
            return QObject::tr("이름 조건은 4096자 이내로 입력하십시오.");
        if (!Criteria.groupId_.isEmpty()) {
            if (Criteria.group_.isEmpty()) return QObject::tr("파일 그룹이 삭제되었거나 사용할 수 없습니다.");
            const QJsonObject Document{{"version", 2}, {"mode", "merge"}, {"groups", QJsonArray{Criteria.group_}}};
            const auto Error = settings::FileColorRules::Validate(Document);
            if (!Error.isEmpty()) return Error;
            group_.Load(Document); hasGroup_ = true;
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
    bool Matches(const settings::FileRuleInput& Input) const {
        if (Input.name_ == "..") return false;
        const bool Folder = (Input.attributes_ & 0x10u) != 0;
        if ((criteria_.scope_ == Scope::Files && Folder) || (criteria_.scope_ == Scope::Folders && !Folder)) return false;
        if (hit(exclude_, Input.name_)) return false;
        if (hasGroup_ && !group_.Evaluate(Input).matched_) return false;
        bool Any = false, All = true, Present = false;
        const auto Add = [&](bool Value) { Present = true; Any |= Value; All &= Value; };
        if (!include_.isEmpty()) Add(hit(include_, Input.name_));
        if (criteria_.advanced_ && size_) Add(!Folder && (criteria_.minimumSize_ ? Input.size_ >= *size_ : Input.size_ <= *size_));
        if (criteria_.advanced_ && date_) Add(Input.modifiedUtcMs_ && *Input.modifiedUtcMs_ >= *date_);
        return !Present || (criteria_.all_ || !criteria_.advanced_ ? All : Any);
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
    settings::FileColorRules group_;
    bool hasGroup_ = false;
    std::optional<quint64> size_;
    std::optional<qint64> date_;
};
} // namespace hc::selection
