#pragma once
#include "../commands/automation.hpp"
#include <any>
#include <thread>

namespace hc::script {
using storage::Error;
using storage::ErrorCategory;
using storage::Result;
using storage::Unit;

// ABI-neutral data contract, not a binary C++ ABI promise. Bindings own copies.
struct ScriptValue {
    using Array = std::vector<ScriptValue>;
    using Object = std::map<std::string, ScriptValue>;
    using Data = std::variant<Unit, bool, std::int64_t, double, std::string, Array, Object>;
    Data data_;
    ScriptValue() = default;
    ScriptValue(const char* Text) : data_(std::string(Text)) {}
    template<class T> requires (!std::is_same_v<std::remove_cvref_t<T>, ScriptValue>)
    ScriptValue(T&& Value) : data_(std::forward<T>(Value)) {}
    bool operator==(const ScriptValue&) const = default;
};
Result<Unit> Validate(const ScriptValue& Value);
Result<commands::Parameters> ToParameters(const ScriptValue& Value);
ScriptValue FromParameters(const commands::Parameters& Parameters);

enum class HandleKind { Context, Resource, Job, Viewer, Module, Subscription };
class HandleTable {
public:
    HandleTable();
    template<class T> Result<std::string> Insert(HandleKind Kind, T Value) {
        if (closed_ || records_.size() >= 256) return Error{ErrorCategory::BudgetExceeded,"script-handle-limit"};
        const auto Id = prefix_ + std::to_string(++serial_);
        records_.emplace(Id, Record{Kind,std::any(std::move(Value))}); return Id;
    }
    template<class T> Result<T*> Resolve(const std::string& Id, HandleKind Kind) {
        const auto It = records_.find(Id);
        if (closed_ || It == records_.end() || It->second.kind_ != Kind)
            return Error{ErrorCategory::StaleRevision,"script-stale-handle"};
        auto* Value = std::any_cast<T>(&It->second.value_);
        if (!Value) return Error{ErrorCategory::InvalidRequest,"script-handle-type"};
        return Value;
    }
    bool Release(const std::string& Id) { return records_.erase(Id) != 0; }
    std::size_t Size() const { return records_.size(); }
    void Close() { closed_ = true; records_.clear(); }
private:
    struct Record { HandleKind kind_; std::any value_; };
    std::string prefix_;
    std::uint64_t serial_ = 0;
    bool closed_ = false;
    std::map<std::string,Record> records_;
};

// Calls and Close run on the owning thread; no script receives a QObject,
// model index, native pointer, or an executor. API errors are values.
class IScriptHostApi {
public:
    virtual ~IScriptHostApi() = default;
    virtual std::uint32_t ApiVersion() const { return 1; }
    virtual Result<ScriptValue> Call(std::string_view Method, const ScriptValue& Arguments) = 0;
    virtual void Close() = 0;
};
class CommandScriptApi final : public IScriptHostApi {
public:
    CommandScriptApi(automation::Host Host, std::set<std::string> Permissions);
    ~CommandScriptApi() override;
    Result<ScriptValue> Call(std::string_view Method, const ScriptValue& Arguments) override;
    void Close() override;
private:
    struct Resource { storage::CommandContext context_; storage::ResourceId id_; };
    struct Pending {
        commands::CommandInvocation invocation_;
        std::optional<storage::Task<commands::CommandOutcome>> future_;
        std::optional<commands::CommandOutcome> outcome_;
        std::optional<Result<ScriptValue>> terminal_;
        std::stop_source stop_;
    };
    Result<ScriptValue> call(std::string_view Method, const ScriptValue::Object& Arguments);
    Result<Unit> checkContext(const storage::CommandContext& Context);
    Result<ScriptValue> poll(Pending& Pending);
    void cancel(Pending& Pending);
    automation::Host host_;
    std::set<std::string> permissions_;
    std::thread::id thread_ = std::this_thread::get_id();
    HandleTable handles_;
    std::map<std::string,std::shared_ptr<Pending>> jobs_;
    bool closed_ = false;
};

struct RuntimeLimits {
    std::size_t heapBytes_ = 8 * 1024 * 1024;
    std::uint64_t instructions_ = 1000000;
};
struct RuntimeDescriptor {
    std::string id_, version_;
    bool heapQuota_ = false, instructionQuota_ = false;
};
class IScriptRuntime {
public:
    virtual ~IScriptRuntime() = default;
    virtual RuntimeDescriptor Describe() const = 0;
    virtual Result<Unit> Load(std::shared_ptr<IScriptHostApi> Api, RuntimeLimits Limits) = 0;
    virtual Result<ScriptValue> Evaluate(std::string_view Source) = 0;
    // The only method permitted from a watchdog thread; it never calls host APIs.
    virtual void RequestInterrupt() = 0;
    virtual void Unload() = 0;
};
}
