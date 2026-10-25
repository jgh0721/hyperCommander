#pragma once
#include "session.hpp"
namespace hc::compare
{
    Parsed ParseRequestFile(const QString& Path, const QStringList& Overrides, bool Console, const Budget& Limits = {});
    void CheckProtectedInputs(const Request& Input, const QStringList& Writes, const Budget& Limits);
}
