#pragma once

#include <QTimer>
#include <functional>
#include <utility>

namespace hc::rst {

/// Coalesces cheap request metadata before reading or serializing editor text.
class DeferredPreviewWork final
{
public:
    DeferredPreviewWork()
    {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
            auto work = std::move(work_);
            if (work) work();
        });
    }
    void schedule(int delayMs, std::function<void()> work)
    {
        work_ = std::move(work);
        timer_.start(qMax(0, delayMs));
    }
    void cancel()
    {
        timer_.stop();
        work_ = {};
    }
private:
    QTimer timer_;
    std::function<void()> work_;
};

} // namespace hc::rst
