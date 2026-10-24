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
        const bool complete = Left["status"] == "Complete" && Right["status"] == "Complete";
        QVector<bool> renamed(left.size(), false), literalChanged(left.size(), false), tokenChanged(left.size(), false);
        bool similarityLimited = false;
        if (complete)
        {
            // Count all functions, including already matched ones: a duplicate body
            // must not become unique merely because another name matched first.
            QMap<QString, Group> candidates;
            for (int side = 0; side < 2; ++side)
            {
                const auto& source = side ? right : left;
                for (int i = 0; i < source.size(); ++i)
                {
                    Limits.Check(); const auto function = source[i].toObject();
                    if (function["renameSignature"].toString().size() != 64 || function["bodyTokens"].toInt() < 12) continue;
                    const auto key = function["scope"].toString() + QChar(0) + function["renameSignature"].toString() + function["body"].toString();
                    candidates[key].sides_[side].push_back(i);
                }
            }
            for (const auto& group : candidates)
            {
                Limits.Check();
                if (group.sides_[0].size() != 1 || group.sides_[1].size() != 1) continue;
                const int a = group.sides_[0][0], b = group.sides_[1][0];
                if (partner[a] >= 0 || used[b] || ambiguousLeft[a] || ambiguousRight[b] ||
                    left[a].toObject()["name"] == right[b].toObject()["name"]) continue;
                partner[a] = b; used[b] = true; renamed[a] = true;
            }
        }
        if (complete)
        {
            QMap<QString, Group> candidates;
            for (int side = 0; side < 2; ++side)
            {
                const auto& source = side ? right : left;
                for (int i = 0; i < source.size(); ++i)
                {
                    Limits.Check(); const auto function = source[i].toObject();
                    const auto tokens = function["bodyTokens"].toInt(), numbers = function["numberTokens"].toInt();
                    if (function["renameSignature"].toString().size() != 64 || function["literalShape"].toString().size() != 64 ||
                        tokens < 24 || tokens > 4096 || numbers < 1 || numbers > 4 || numbers * 10 > tokens) continue;
                    const auto key = function["scope"].toString() + QChar(0) + function["renameSignature"].toString() + function["literalShape"].toString();
                    candidates[key].sides_[side].push_back(i);
                }
            }
            // Include already matched functions so competing clones never become
            // unique merely because their names were paired earlier.
            for (const auto& group : candidates)
            {
                Limits.Check(); if (group.sides_[0].size() != 1 || group.sides_[1].size() != 1) continue;
                const int a = group.sides_[0][0], b = group.sides_[1][0];
                const auto from = left[a].toObject(), to = right[b].toObject();
                if (partner[a] >= 0 || used[b] || ambiguousLeft[a] || ambiguousRight[b] || from["name"] == to["name"] ||
                    from["body"] == to["body"] || from["bodyTokens"] != to["bodyTokens"] || from["numberTokens"] != to["numberTokens"]) continue;
                partner[a] = b; used[b] = true; renamed[a] = true; literalChanged[a] = true;
            }
        }
        if (complete)
        {
            QMap<QString, Group> groups;
            for (int side = 0; side < 2; ++side)
            {
                const auto& source = side ? right : left;
                for (int i = 0; i < source.size(); ++i)
                {
                    Limits.Check(); const auto f = source[i].toObject();
                    if (f["renameSignature"].toString().size() == 64)
                        groups[f["scope"].toString() + QChar(0) + f["renameSignature"].toString()].sides_[side].push_back(i);
                }
            }
            std::uint64_t work = 0;
            const auto step = [&] { if (++work > std::min<std::uint64_t>(Limits.steps_, 4'000'000)) throw Failure("FunctionSimilarityWorkLimit"); if (!(work & 1023)) Limits.Check(); };
            for (const auto& group : groups)
            {
                Limits.Check(); const auto n = group.sides_[0].size(), m = group.sides_[1].size(); if (!n || !m) continue;
                if (std::none_of(group.sides_[0].begin(), group.sides_[0].end(), [&](int I) { return partner[I] < 0; }) ||
                    std::none_of(group.sides_[1].begin(), group.sides_[1].end(), [&](int I) { return !used[I]; })) continue;
                if (n > 32 || m > 32) { similarityLimited = true; continue; }
                QVector<QJsonArray> tokens[2]; bool missing = false;
                for (int side = 0; side < 2; ++side) for (const auto i : group.sides_[side])
                {
                    const auto f = (side ? right : left)[i].toObject(); const auto sequence = f["bodySequence"].toArray();
                    if (sequence.isEmpty() && f["bodyTokens"].toInt() >= 32 && f["bodyTokens"].toInt() <= 256) similarityLimited = true;
                    missing |= sequence.size() < 32 || sequence.size() > 256 || sequence.size() != f["bodyTokens"].toInt();
                    tokens[side].push_back(sequence);
                }
                // Omitted token payload must not turn competing functions into a
                // seemingly unique match. Exact-name/body matches above still work.
                if (missing) continue;
                QVector<int> bestA(n), secondA(n), bestB(m), secondB(m), choiceA(n), choiceB(m);
                const auto best = [](int Score, int Index, int& First, int& Second, int& Choice) {
                    if (Score > First) { Second = First; First = Score; Choice = Index; } else Second = std::max(Second, Score);
                };
                for (int a = 0; a < n; ++a) for (int b = 0; b < m; ++b)
                {
                    const auto& x = tokens[0][a]; const auto& y = tokens[1][b]; qsizetype prefix = 0, suffix = 0;
                    while (prefix < x.size() && prefix < y.size()) { step(); if (x[prefix] != y[prefix]) break; ++prefix; }
                    while (suffix < x.size() - prefix && suffix < y.size() - prefix) { step(); if (x[x.size() - suffix - 1] != y[y.size() - suffix - 1]) break; ++suffix; }
                    const auto common = prefix + suffix;
                    const int score = x.size() - common <= 3 && y.size() - common <= 3 ? int(2000 * common / (x.size() + y.size())) : 0;
                    best(score, b, bestA[a], secondA[a], choiceA[a]); best(score, a, bestB[b], secondB[b], choiceB[b]);
                }
                for (int a = 0; a < n; ++a)
                {
                    const auto b = choiceA[a]; const auto i = group.sides_[0][a], j = group.sides_[1][b];
                    if (bestA[a] < 900 || bestA[a] < secondA[a] + 50 || choiceB[b] != a || bestB[b] < secondB[b] + 50 ||
                        partner[i] >= 0 || used[j] || ambiguousLeft[i] || ambiguousRight[j]) continue;
                    const auto from = left[i].toObject(), to = right[j].toObject();
                    if (from["name"] == to["name"] || from["body"] == to["body"]) continue;
                    partner[i] = j; used[j] = true; renamed[i] = true; literalChanged[i] = true; tokenChanged[i] = true;
                }
            }
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
        QJsonArray relations; int moveCount = 0, modifiedCount = 0, ambiguousCount = 0, renamedCount = 0;
        for (int i = 0; i < left.size(); ++i)
        {
            Limits.Check(); const auto from = left[i].toObject(); const auto to = partner[i] < 0 ? QJsonObject{} : right[partner[i]].toObject();
            const bool modified = partner[i] >= 0 && (from["body"] != to["body"] || (!renamed[i] && from["signature"] != to["signature"]));
            const QString status = ambiguousLeft[i] ? "Ambiguous" : partner[i] < 0 ? (complete ? "Removed" : "Unmatched") :
                renamed[i] ? (literalChanged[i] ? (moved[i] ? "MovedRenamedModified" : "RenamedModified") : (moved[i] ? "MovedRenamed" : "Renamed")) : moved[i] ? (modified ? "MovedModified" : "Moved") : modified ? "Modified" : "Unchanged";
            relations.append(QJsonObject{{"name", from["name"]}, {"status", status}, {"left", i}, {"right", partner[i]},
                {"newName", renamed[i] ? to["name"] : QJsonValue{}},
                {"match", partner[i] < 0 ? "None" : renamed[i] ? (tokenChanged[i] ? "UniqueTokenEdit" : literalChanged[i] ? "UniqueNumericLiteralShape" : "UniqueBodySignature") : from["signature"] == to["signature"] ? "Signature" : "UniqueName"}});
            renamedCount += renamed[i]; moveCount += moved[i]; modifiedCount += modified; ambiguousCount += ambiguousLeft[i];
        }
        for (int i = 0; i < right.size(); ++i) if (!used[i])
        {
            Limits.Check(); relations.append(QJsonObject{{"name", right[i].toObject()["name"]},
                {"status", ambiguousRight[i] ? "Ambiguous" : complete ? "Added" : "Unmatched"}, {"left", -1}, {"right", i}});
            ambiguousCount += ambiguousRight[i];
        }
        return {{"status", complete && !ambiguousCount && !similarityLimited ? "Complete" : "Partial"}, {"relations", relations}, {"moved", moveCount}, {"modified", modifiedCount},
            {"renamed", renamedCount}, {"ambiguous", ambiguousCount}, {"bodySimilarityLimited", similarityLimited}, {"method", "qualified-name-signature/unique-body-literal-or-token-edit/scope-order-lis-4"}};
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
        // Token payload is transient matching data, not part of exported reports.
        for (qsizetype i = 0; i < documents.size(); ++i)
        {
            auto document = documents[i].toObject(); auto functions = document["functions"].toArray();
            for (qsizetype j = 0; j < functions.size(); ++j) { auto f = functions[j].toObject(); f.remove("bodySequence"); functions[j] = f; }
            if (document.contains("functions")) document["functions"] = functions; documents[i] = document;
        }
        Value.semantic_ = {{"schemaVersion", 1}, {"status", complete ? "Complete" : "Partial"}, {"documents", documents}, {"pairs", pairs},
            {"meaning", "syntactic-function-relations; no macro expansion or type resolution; annotations do not change merge decisions"}};
        if (!complete) Value.warnings_.append("SourceAnalysisPartial");
    }
}
