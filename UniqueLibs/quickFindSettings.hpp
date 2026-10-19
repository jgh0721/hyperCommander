#pragma once
#include <QJsonObject>
#include <QString>

namespace hc::quickfind {
struct Settings {
    bool enabled_ = true;
    bool highlight_ = true;
    QString activation_ = "ctrl-alt", scope_ = "panel", match_ = "starts", result_ = "cursor";
    static Settings FromJson(const QJsonObject& Json)
    {
        Settings Value;
        Value.enabled_ = Json["enabled"].toBool(Value.enabled_);
        Value.highlight_ = Json["highlight"].toBool(Value.highlight_);
        if (Json["activation"] == "typing") Value.activation_ = "typing";
        if (Json["scope"] == "everything") Value.scope_ = "everything";
        if (Json["match"] == "contains" || Json["match"] == "ends") Value.match_ = Json["match"].toString();
        if (Json["result"] == "filter") Value.result_ = "filter";
        return Value;
    }
    QJsonObject ToJson() const
    { return {{"enabled", enabled_}, {"highlight", highlight_}, {"activation", activation_}, {"scope", scope_}, {"match", match_}, {"result", result_}}; }
};
inline bool Matches(const QString& Name, const QString& Query, const QString& Match)
{
    const auto Text = Name.normalized(QString::NormalizationForm_C).toCaseFolded();
    const auto Term = Query.normalized(QString::NormalizationForm_C).toCaseFolded();
    return Match == "starts" ? Text.startsWith(Term) : Match == "ends" ? Text.endsWith(Term) : Text.contains(Term);
}
}
