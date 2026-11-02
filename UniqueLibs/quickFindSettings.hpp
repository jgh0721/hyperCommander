#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include <QJsonObject>
#include "koreanSearchPattern.hpp"
#include <QString>

namespace hc::quickfind {
struct Settings {
    bool enabled_ = hc::defaults::DefaultValue("general/quickFind/enabled").toBool();
    bool highlight_ = hc::defaults::DefaultValue("general/quickFind/highlight").toBool();
    bool everythingKorean_ = hc::defaults::DefaultValue("general/quickFind/everythingKorean").toBool();
    static constexpr int MinimumDisplayInterval = 50, MaximumDisplayInterval = 5000;
    int everythingDisplayIntervalMs_ = hc::defaults::DefaultValue("general/quickFind/everythingDisplayIntervalMs").toInt();
    QString activation_ = hc::defaults::DefaultValue("general/quickFind/activation").toString();
    QString scope_ = hc::defaults::DefaultValue("general/quickFind/scope").toString();
    QString match_ = hc::defaults::DefaultValue("general/quickFind/match").toString();
    QString result_ = hc::defaults::DefaultValue("general/quickFind/result").toString();
    static Settings FromJson(const QJsonObject& Json)
    {
        Settings Value;
        Value.enabled_ = Json["enabled"].toBool(Value.enabled_);
        Value.highlight_ = Json["highlight"].toBool(Value.highlight_);
        Value.everythingKorean_ = Json["everythingKorean"].toBool(hc::defaults::DefaultValue("general/quickFind/everythingKorean").toBool());
        const auto Interval = Json["everythingDisplayIntervalMs"];
        if (Interval.isDouble() && Interval.toDouble() >= MinimumDisplayInterval && Interval.toDouble() <= MaximumDisplayInterval
            && Interval.toDouble() == double(Interval.toInt())) Value.everythingDisplayIntervalMs_ = Interval.toInt();
        if (QStringList{"typing", "ctrl-alt"}.contains(Json["activation"].toString())) Value.activation_ = Json["activation"].toString();
        if (QStringList{"panel", "everything"}.contains(Json["scope"].toString())) Value.scope_ = Json["scope"].toString();
        if (QStringList{"starts", "contains", "ends"}.contains(Json["match"].toString())) Value.match_ = Json["match"].toString();
        if (QStringList{"cursor", "filter"}.contains(Json["result"].toString())) Value.result_ = Json["result"].toString();
        return Value;
    }
    QJsonObject ToJson() const
    { return {{"enabled", enabled_}, {"highlight", highlight_}, {"activation", activation_}, {"scope", scope_}, {"match", match_}, {"result", result_},
        {"everythingDisplayIntervalMs", everythingDisplayIntervalMs_}, {"everythingKorean", everythingKorean_}}; }
};
// Internal match descriptor; only the Everything path enables Korean matching.
inline QString MatchKind(const QString& Match) { return Match.startsWith("korean:") ? Match.section(':', 1, 1) : Match; }
inline QString SearchPattern(const QString& Query, const QString& Match)
{
    const auto Kind = MatchKind(Match);
    if (Match.startsWith("korean:")) {
        bool Ok = false; const auto Composing = Match.section(':', 2, 2).toInt(&Ok);
        return KoreanSearchPattern(Query, Kind, Ok ? Composing : -1);
    }
    return (Kind == "starts" ? "^" : "") + QRegularExpression::escape(Query.normalized(QString::NormalizationForm_C)) + (Kind == "ends" ? "$" : "");
}
inline bool Matches(const QString& Name, const QString& Query, const QString& Match)
{
    if (Match.startsWith("korean:")) {
        thread_local QString LastQuery, LastMatch;
        thread_local QRegularExpression Pattern;
        if (Query != LastQuery || Match != LastMatch) {
            LastQuery = Query; LastMatch = Match;
            Pattern = QRegularExpression(SearchPattern(Query, Match), QRegularExpression::CaseInsensitiveOption);
        }
        return Pattern.match(Name).hasMatch();
    }
    const auto Text = Name.normalized(QString::NormalizationForm_C).toCaseFolded();
    const auto Term = Query.normalized(QString::NormalizationForm_C).toCaseFolded();
    return Match == "starts" ? Text.startsWith(Term) : Match == "ends" ? Text.endsWith(Term) : Text.contains(Term);
}
}
