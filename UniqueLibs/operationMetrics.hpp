#pragma once
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <array>
#include <chrono>
#include <mutex>
#include <atomic>
#include <map>
#include <algorithm>
#include <string_view>

namespace hc::operations {
// Opt-in diagnostics: aggregate in memory, write once after the measured job.
// Inclusive and nested times are labelled separately; never sum both.
class OperationMetrics final {
public:
    struct Bucket { std::uint64_t count_ = 0, nanos_ = 0, minimum_ = UINT64_MAX, maximum_ = 0, bytes_ = 0; std::array<std::uint64_t, 11> distribution_{}; };
    static OperationMetrics& Instance() { static OperationMetrics Value; return Value; }
    void Reset() { std::lock_guard Lock(mutex_); buckets_.clear(); recoveryIntervals_.clear(); epoch_=std::chrono::steady_clock::now(); failed_=false; enabled_ = true; }
    void MarkFailed() noexcept { failed_=true; }
    bool Enabled() const { return enabled_; }
    void Add(const char* Name, std::chrono::steady_clock::time_point Started, std::uint64_t Nanos, std::uint64_t Bytes = 0) {
        std::lock_guard Lock(mutex_); auto& Value = buckets_[Name]; ++Value.count_; Value.nanos_ += Nanos; Value.bytes_ += Bytes;
        Value.minimum_ = std::min(Value.minimum_, Nanos); Value.maximum_ = std::max(Value.maximum_, Nanos);
        constexpr std::array<std::uint64_t, 10> Limits{100000,500000,1000000,2000000,5000000,10000000,25000000,50000000,100000000,250000000};
        const auto Index = std::lower_bound(Limits.begin(), Limits.end(), Nanos) - Limits.begin(); ++Value.distribution_[Index];
        const std::string_view Category(Name);
        if (Category.ends_with(".inclusive")) { const auto Begin=std::chrono::duration_cast<std::chrono::nanoseconds>(Started-epoch_).count(); recoveryIntervals_.push_back({std::uint64_t(std::max<std::int64_t>(0,Begin)),std::uint64_t(std::max<std::int64_t>(0,Begin))+Nanos}); }
    }
    bool Save(const QString& Path, double WallMs) {
        std::lock_guard Lock(mutex_); if (failed_) return false; QJsonObject Results;
        for (const auto& [Name, Value] : buckets_) {
            QJsonArray Distribution; for (auto Count : Value.distribution_) Distribution.append(double(Count));
            Results[QString::fromStdString(Name)] = QJsonObject{{"count",double(Value.count_)},{"totalMs",Value.nanos_/1e6},{"averageMs",Value.nanos_/1e6/Value.count_},
                {"minimumMs",Value.minimum_/1e6},{"maximumMs",Value.maximum_/1e6},{"bytes",double(Value.bytes_)},{"durationBuckets",Distribution}};
        }
        std::sort(recoveryIntervals_.begin(),recoveryIntervals_.end()); std::uint64_t Union=0, Begin=0, End=0;
        for (const auto& Interval:recoveryIntervals_) { if (Interval.first>End) { Union+=End-Begin; Begin=Interval.first; End=Interval.second; } else End=std::max(End,Interval.second); }
        Union+=End-Begin;
        QFile File(Path); if (!File.open(QIODevice::WriteOnly)) return false;
        auto Bytes = QJsonDocument(QJsonObject{{"wallMs",WallMs},{"recoveryUnionMs",Union/1e6},{"recoveryUnionPercent",WallMs>0?Union/1e6/WallMs*100:0},{"metrics",Results},{"bucketUpperMs",QJsonArray{0.1,0.5,1,2,5,10,25,50,100,250,"infinity"}}}).toJson();
        Bytes.replace("\n","\r\n");
        return File.write(Bytes)==Bytes.size() && File.flush();
    }
private:
    std::atomic<bool> enabled_{false},failed_{false}; std::mutex mutex_; std::map<std::string,Bucket> buckets_;
    std::chrono::steady_clock::time_point epoch_; std::vector<std::pair<std::uint64_t,std::uint64_t>> recoveryIntervals_;
};
class OperationMetric final {
public:
    explicit OperationMetric(const char* Name, std::uint64_t Bytes = 0) : name_(Name), bytes_(Bytes), active_(OperationMetrics::Instance().Enabled()) { if (active_) started_=std::chrono::steady_clock::now(); }
    void SetBytes(std::uint64_t Bytes) { bytes_=Bytes; }
    ~OperationMetric() { if (active_) { try { OperationMetrics::Instance().Add(name_,started_,std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started_).count(),bytes_); } catch (...) { OperationMetrics::Instance().MarkFailed(); } } }
private:
    const char* name_; std::uint64_t bytes_; bool active_; std::chrono::steady_clock::time_point started_;
};
}
