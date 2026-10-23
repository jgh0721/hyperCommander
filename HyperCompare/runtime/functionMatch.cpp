#include "stdafx.h"
#include "semantic.hpp"
#include "session.hpp"
#include <QHash>
#include <QMap>
#include <algorithm>

namespace hc::compare
{
    QJsonObject MatchFunctions(const QJsonObject& Left, const QJsonObject& Right, const Budget& Limits)
    {
        const auto ready = [](const QJsonObject& Value) { return Value["status"] == "Complete" || Value["status"] == "Partial"; };
        if (!ready(Left) || !ready(Right)) return {{"status", "Unavailable"}, {"relations", QJsonArray{}}};
        const auto left = Left["functions"].toArray(), right = Right["functions"].toArray();
        if (left.size() > 10000 || right.size() > 10000) throw Failure("FunctionLimit");
        struct Group { QVector<int> sides_[2]; };
        QMap<QString, Group> names;
        for (int side = 0; side < 2; ++side)
        {
            const auto& source = side ? right : left;
            for (int i = 0; i < source.size(); ++i) { Limits.Check(); names[source[i].toObject()["name"].toString()].sides_[side].push_back(i); }
        }
        QVector<int> partner(left.size(), -1); QVector<bool> used(right.size(), false), ambiguousLeft(left.size()), ambiguousRight(right.size());
        for (const auto& group : names)
        {
            Limits.Check(); QHash<QString, Group> signatures;
            for (int side = 0; side < 2; ++side)
                for (int index : group.sides_[side]) signatures[(side ? right : left)[index].toObject()["signature"].toString()].sides_[side].push_back(index);
            for (const auto& signature : signatures)
                if (signature.sides_[0].size() == 1 && signature.sides_[1].size() == 1)
                { const auto a = signature.sides_[0][0], b = signature.sides_[1][0]; partner[a] = b; used[b] = true; }
            QVector<int> remaining[2];
            for (int i : group.sides_[0]) if (partner[i] < 0) remaining[0].push_back(i);
            for (int i : group.sides_[1]) if (!used[i]) remaining[1].push_back(i);
            if (remaining[0].size() == 1 && remaining[1].size() == 1)
            { partner[remaining[0][0]] = remaining[1][0]; used[remaining[1][0]] = true; }
            else if (!remaining[0].isEmpty() && !remaining[1].isEmpty())
            { for (int i : remaining[0]) ambiguousLeft[i] = true; for (int i : remaining[1]) ambiguousRight[i] = true; }
        }
        // Relative order among matched functions in the same scope is stable
        // under added prefix lines/functions. LIS marks a minimal relocation set.
        QMap<QString, QVector<int>> scopes;
        for (int i = 0; i < partner.size(); ++i) if (partner[i] >= 0) scopes[left[i].toObject()["scope"].toString()].push_back(i);
        QVector<bool> moved(left.size(), false);
        for (const auto& indexes : scopes)
        {
            QVector<int> tails, previous(indexes.size(), -1);
            for (int i = 0; i < indexes.size(); ++i)
            {
                Limits.Check(); moved[indexes[i]] = true;
                const auto at = std::lower_bound(tails.begin(), tails.end(), partner[indexes[i]], [&](int Tail, int Value) { return partner[indexes[Tail]] < Value; });
                const int position = int(at - tails.begin()); if (position) previous[i] = tails[position - 1];
                if (at == tails.end()) tails.push_back(i); else *at = i;
            }
            for (int i = tails.isEmpty() ? -1 : tails.back(); i >= 0; i = previous[i]) moved[indexes[i]] = false;
        }
        QJsonArray relations; int moveCount = 0, modifiedCount = 0, ambiguousCount = 0;
        const bool complete = Left["status"] == "Complete" && Right["status"] == "Complete";
        for (int i = 0; i < left.size(); ++i)
        {
            Limits.Check(); const auto from = left[i].toObject(); const auto to = partner[i] < 0 ? QJsonObject{} : right[partner[i]].toObject();
            const bool modified = partner[i] >= 0 && (from["body"] != to["body"] || from["signature"] != to["signature"]);
            const QString status = ambiguousLeft[i] ? "Ambiguous" : partner[i] < 0 ? (complete ? "Removed" : "Unmatched") :
                moved[i] ? (modified ? "MovedModified" : "Moved") : modified ? "Modified" : "Unchanged";
            relations.append(QJsonObject{{"name", from["name"]}, {"status", status}, {"left", i}, {"right", partner[i]},
                {"match", partner[i] < 0 ? "None" : from["signature"] == to["signature"] ? "Signature" : "UniqueName"}});
            moveCount += moved[i]; modifiedCount += modified; ambiguousCount += ambiguousLeft[i];
        }
        for (int i = 0; i < right.size(); ++i) if (!used[i])
        {
            Limits.Check(); relations.append(QJsonObject{{"name", right[i].toObject()["name"]},
                {"status", ambiguousRight[i] ? "Ambiguous" : complete ? "Added" : "Unmatched"}, {"left", -1}, {"right", i}});
            ambiguousCount += ambiguousRight[i];
        }
        return {{"status", complete && !ambiguousCount ? "Complete" : "Partial"}, {"relations", relations}, {"moved", moveCount}, {"modified", modifiedCount},
            {"ambiguous", ambiguousCount}, {"method", "qualified-name-signature/scope-order-lis-1"}};
    }
    void AnalyzeSourceRelations(const Request& Input, Result& Value, const Budget& Limits)
    {
        if (Input.semantic_ == "off") { Value.semantic_ = {{"status", "Disabled"}}; return; }
        if (Value.inputs_.size() < 2) return;
        QJsonArray documents, pairs; bool supported = false, complete = true;
        auto budget = Limits; budget.deadline_ = std::min(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(6));
        for (const auto& input : Value.inputs_)
        {
            auto document = AnalyzeFunctions(input.path_, input.utf8_, budget);
            supported |= document["status"] != "Unsupported"; complete &= document["status"] == "Complete";
            documents.append(document);
        }
        if (!supported) { Value.semantic_ = {{"status", "Unsupported"}}; return; }
        for (const auto& pair : {std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 2}})
        {
            if (pair.second >= documents.size()) continue;
            QJsonObject relation;
            try { relation = MatchFunctions(documents[pair.first].toObject(), documents[pair.second].toObject(), budget); }
            catch (const Failure& failure) { relation = {{"status", "Unavailable"}, {"reason", QString::fromStdString(failure.code_)}}; }
            relation["inputs"] = QJsonArray{pair.first, pair.second}; pairs.append(relation); complete &= relation["status"] == "Complete";
        }
        Value.semantic_ = {{"schemaVersion", 1}, {"status", complete ? "Complete" : "Partial"}, {"documents", documents}, {"pairs", pairs},
            {"meaning", "syntactic-function-relations; no macro expansion or type resolution; annotations do not change merge decisions"}};
        if (!complete) Value.warnings_.append("SourceAnalysisPartial");
    }
}
