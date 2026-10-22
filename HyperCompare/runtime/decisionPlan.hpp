#pragma once
#include "session.hpp"

namespace hc::compare
{
    inline constexpr qsizetype decisionPlanBytes = 64 * 1024 * 1024;
    inline constexpr std::size_t decisionBlockLimit = 100000;
    QJsonObject ReadCompareJson(const QByteArray& Json, const Budget& Limits);
    QJsonObject DecisionIdentity(const Request& Input, const Result& Session, const Budget& Limits);
    QJsonObject MakeDecisionPlan(const Request& Input, const Result& Session, const Budget& Limits);
    void ApplyDecisionPlan(const Request& Input, Result& Session, const QByteArray& Json, const Budget& Limits);
}
