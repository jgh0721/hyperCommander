#pragma once
#include "session.hpp"
#include <functional>
namespace hc::compare
{
    // Sink is called only by the controller, after all jobs pass preflight.
    using BatchSink = std::function<bool(const QJsonObject&)>;
    int RunBatch(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled, const BatchSink& Sink);
    int RunBatchAndReport(const Request& Input, const std::shared_ptr<std::atomic_bool>& Canceled);
}
