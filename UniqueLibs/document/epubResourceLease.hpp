#pragma once
#include <QString>
#include <functional>
#include <memory>
namespace hc::document {
// One lease per worker thread. Limits apply across product processes in a user session.
class EpubResourceLease final {
public:
    static constexpr unsigned ActiveLimit=2, TotalLimit=8;
    explicit EpubResourceLease(const QString& Domain={});
    ~EpubResourceLease();
    bool Acquire(const std::function<bool()>& Cancelled,const std::function<void()>& Queued={});
    QString Error()const;
    QString JobName()const;
    EpubResourceLease(const EpubResourceLease&)=delete;
    EpubResourceLease& operator=(const EpubResourceLease&)=delete;
private:struct State;std::unique_ptr<State> state_;
};
}
