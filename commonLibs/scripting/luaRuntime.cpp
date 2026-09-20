#include "luaRuntime.hpp"
#include <sol/sol.hpp>
#include <cstdlib>
#include <cmath>

namespace hc::script {
namespace {
using Clock = std::chrono::steady_clock;
struct ValueBudget {
    std::size_t nodes_ = 0, bytes_ = 0;
    void Charge(std::size_t Depth, std::size_t Bytes = 0) {
        bytes_ += Bytes;
        if (Depth > 16 || ++nodes_ > 4096 || bytes_ > 1024 * 1024) throw std::runtime_error("script-value-limit");
    }
};
ScriptValue Read(const sol::object& Value, ValueBudget& Budget, std::size_t Depth = 0) {
    Budget.Charge(Depth);
    switch (Value.get_type()) {
    case sol::type::userdata:
        if (Value.is<Unit>()) return ScriptValue{};
        throw std::runtime_error("script-value-type");
    case sol::type::nil: return {};
    case sol::type::boolean: return Value.as<bool>();
    case sol::type::number:
        if (Value.is<lua_Integer>()) return std::int64_t(Value.as<lua_Integer>());
        return Value.as<double>();
    case sol::type::string: {
        auto Text = Value.as<std::string>(); Budget.Charge(Depth, Text.size()); return Text;
    }
    case sol::type::table: {
        const auto Table = Value.as<sol::table>();
        ScriptValue::Object Object;
        std::map<std::int64_t, ScriptValue> Indexed;
        // raw iteration ignores __pairs and __index; depth/node bounds also reject cycles.
        for (const auto& [Key, Child] : Table) {
            if (Key.get_type() == sol::type::string) {
                auto Name = Key.as<std::string>(); Budget.Charge(Depth, Name.size());
                Object.emplace(std::move(Name), Read(Child, Budget, Depth + 1));
            } else if (Key.get_type() == sol::type::number && Key.is<lua_Integer>()) {
                Indexed.emplace(Key.as<lua_Integer>(), Read(Child, Budget, Depth + 1));
            } else throw std::runtime_error("script-table-key");
        }
        if (Indexed.empty()) return Object;
        if (!Object.empty()) throw std::runtime_error("script-table-mixed");
        ScriptValue::Array Array;
        for (auto& [Index, Child] : Indexed) {
            if (Index != std::int64_t(Array.size() + 1)) throw std::runtime_error("script-array-sparse");
            Array.push_back(std::move(Child));
        }
        return Array;
    }
    default: throw std::runtime_error("script-value-type");
    }
}
sol::object Write(lua_State* State, const ScriptValue& Value) {
    return std::visit([State](const auto& Data) -> sol::object {
        using T = std::decay_t<decltype(Data)>;
        if constexpr (std::is_same_v<T, Unit>) return sol::make_object(State, Unit{});
        else if constexpr (std::is_same_v<T, ScriptValue::Array>) {
            auto Table = sol::state_view(State).create_table(int(Data.size()), 0);
            int Index = 0; for (const auto& Child : Data) Table.raw_set(++Index, Write(State, Child)); return Table;
        } else if constexpr (std::is_same_v<T, ScriptValue::Object>) {
            auto Table = sol::state_view(State).create_table(0, int(Data.size()));
            for (const auto& [Key, Child] : Data) Table.raw_set(Key, Write(State, Child)); return Table;
        } else return sol::make_object(State, Data);
    }, Value.data_);
}
class LuaRuntime final : public IScriptRuntime {
public:
    ~LuaRuntime() override { Unload(); }
    RuntimeDescriptor Describe() const override { return {"sol2", "3.3.0 / " LUA_RELEASE, true, true}; }
    Result<Unit> Load(std::shared_ptr<IScriptHostApi> Api, RuntimeLimits Limits) override {
        Unload();
        if (!Api || Api->ApiVersion() != 1) return Error{ErrorCategory::InvalidRequest, "api-version"};
        if (Limits.heapBytes_ < 1024 * 1024 || Limits.heapBytes_ > 64 * 1024 * 1024 || !Limits.instructions_
            || Limits.wallTime_ <= std::chrono::milliseconds(0) || Limits.wallTime_ > std::chrono::minutes(5) || !Limits.hostCalls_)
            return Error{ErrorCategory::InvalidRequest, "runtime-limits"};
        api_ = std::move(Api); limits_ = Limits; interrupted_ = false; memory_ = false;
        state_ = lua_newstate(&allocate, this);
        if (!state_) { Unload(); return Error{ErrorCategory::BudgetExceeded, "runtime-memory"}; }
        lua_atpanic(state_, [](lua_State*) -> int { throw std::runtime_error("runtime-memory"); });
        *static_cast<LuaRuntime**>(lua_getextraspace(state_)) = this;
        // Initialization is protected too: a quota failure must never reach Lua panic.
        lua_pushcfunction(state_, &initialize);
        if (lua_pcall(state_, 0, 0, 0) != LUA_OK) {
            const auto Failure = error(lua_tostring(state_, -1)); Unload(); return Failure;
        }
        return Unit{};
    }
    Result<ScriptValue> Evaluate(std::string_view Source) override {
        if (!state_) return Error{ErrorCategory::Cancelled, "runtime-unloaded"};
        if (Source.size() > 65536 || Source.find('\0') != std::string_view::npos)
            return Error{ErrorCategory::InvalidRequest, "runtime-source"};
        // An interrupt is latched for this session; a fresh Load starts a new run.
        instructions_ = 0; calls_ = 0; memory_ = false; quota_ = false; expired_ = false;
        deadline_ = Clock::now() + limits_.wallTime_;
        lua_sethook(state_, &hook, LUA_MASKCOUNT, 1000);
        try {
            check();
            sol::state_view View(state_);
            auto Result = View.safe_script(std::string(Source), sol::script_pass_on_error, "script.lua", sol::load_mode::text);
            if (!Result.valid()) { sol::error Failure = Result; return error(Failure.what()); }
            check();
            if (Result.return_count() == 0) return ScriptValue{};
            ValueBudget Budget;
            auto Value = Read(Result.get<sol::object>(), Budget);
            auto Valid = Validate(Value); if (!Valid.HasValue()) return Valid.Failure();
            return Value;
        } catch (const std::exception& Failure) { return error(Failure.what()); }
        catch (...) { return error("runtime-error"); }
    }
    void RequestInterrupt() override { interrupted_ = true; }
    void Unload() override {
        if (api_) api_->Close();
        // Finalizers have no host rights and remain subject to the interrupted hook.
        interrupted_ = true;
        if (state_) lua_close(state_);
        state_ = nullptr; api_.reset(); bytes_ = 0;
    }
private:
    void check() {
        if (interrupted_) throw std::runtime_error("runtime-interrupted");
        if (Clock::now() >= deadline_) { expired_ = true; throw std::runtime_error("runtime-deadline"); }
        if (quota_) throw std::runtime_error("runtime-quota");
    }
    Error error(const char* Text) const {
        return {interrupted_ ? ErrorCategory::Cancelled : (quota_ || memory_ || expired_) ? ErrorCategory::BudgetExceeded : ErrorCategory::InvalidRequest,
            interrupted_ ? "runtime-interrupted" : expired_ ? "runtime-deadline" : memory_ ? "runtime-memory" : quota_ ? "runtime-quota" : Text ? std::string(Text).substr(0, 4096) : "runtime-error"};
    }
    static int initialize(lua_State* State) {
        auto* Self = *static_cast<LuaRuntime**>(lua_getextraspace(State));
        sol::state_view View(State);
        View.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::utf8);
        // Protected calls/coroutines would let scripts repeatedly swallow quota errors.
        // Host errors are returned as values by hc.call instead.
        for (const char* Name : {"dofile", "loadfile", "load", "collectgarbage", "print", "warn", "pcall", "xpcall"}) View[Name] = sol::nil;
        auto Hc = View.create_named_table("hc");
        Hc["apiVersion"] = 1;
        Hc["null"] = sol::make_object(State, Unit{});
        Hc.set_function("call", [Self, State](const std::string& Method, const sol::object& Arguments) {
            Self->check();
            if (Method.empty() || Method.size() > 128 || Method.find('\0') != std::string::npos) throw std::runtime_error("script-method");
            if (++Self->calls_ > Self->limits_.hostCalls_) { Self->quota_ = true; throw std::runtime_error("runtime-host-calls"); }
            ValueBudget Budget; auto Value = Read(Arguments, Budget);
            auto Valid = Validate(Value); if (!Valid.HasValue()) throw std::runtime_error(Valid.Failure().code_);
            auto Result = Self->api_->Call(Method, Value); Self->check();
            auto Reply = sol::state_view(State).create_table(); Reply["ok"] = Result.HasValue();
            if (Result.HasValue()) {
                Valid = Validate(Result.Value()); if (!Valid.HasValue()) throw std::runtime_error(Valid.Failure().code_);
                Reply["value"] = Write(State, Result.Value());
            } else Reply["error"] = Result.Failure().code_;
            return Reply;
        });
        Hc.set_function("sleep", [Self](int Milliseconds) {
            if (Milliseconds < 0 || Milliseconds > 1000) throw std::runtime_error("runtime-sleep-range");
            const auto Until = Clock::now() + std::chrono::milliseconds(Milliseconds);
            do { Self->check(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } while (Clock::now() < Until);
        });
        const auto Bootstrap = View.safe_script(R"lua(
local call, sleep, fail = hc.call, hc.sleep, error
function hc.request(method, args)
    local reply = call(method, args or {})
    if not reply.ok then fail(reply.error, 2) end
    return reply.value
end
local request = hc.request
function hc.capture() return request('capture') end
function hc.invoke(context, command, parameters)
    return request('invoke', {context=context, command=command, parameters=parameters or {}})
end
function hc.release(handle) return request('release', {handle=handle}) end
hc.job = {}
function hc.job.poll(handle) return request('poll', {handle=handle}) end
function hc.job.cancel(handle) return request('cancel', {handle=handle}) end
function hc.job.snapshot(handle) return request('job.snapshot', {handle=handle}) end
hc.workspace = {}
function hc.workspace.snapshot() return request('workspace.snapshot') end
hc.viewer = {}
function hc.viewer.open(resource) return request('viewer.open', {resource=resource}) end
function hc.viewer.snapshot(handle) return request('viewer.snapshot', {handle=handle}) end
function hc.viewer.execute(handle, command) return request('viewer.execute', {handle=handle, command=command}) end
function hc.viewer.close(handle) return request('viewer.close', {handle=handle}) end
hc.events = {}
function hc.events.subscribe(event) return request('events.subscribe', {event=event}) end
function hc.events.poll(handle) return request('events.poll', {handle=handle}) end
function hc.events.unsubscribe(handle) return request('events.unsubscribe', {handle=handle}) end
function hc.job.await(handle)
    while true do
        local result = request('poll', {handle=handle})
        if result.done then return result.output end
        sleep(10)
    end
end
hc.commands = {invoke=hc.invoke}
hc.selection = {snapshot=hc.capture}
hc.fs = {}
function hc.fs.resource(handle) return request('resource', {handle=handle}) end
local invoke = hc.invoke
for _, name in ipairs({'copy', 'move', 'rename', 'mkdir', 'delete'}) do
    local command = 'hc.file.' .. name
    hc.fs[name] = function(context, parameters) return invoke(context, command, parameters or {}) end
end
hc.jobs = hc.job
)lua", sol::script_pass_on_error);
        if (!Bootstrap.valid()) { sol::error Failure = Bootstrap; return luaL_error(State, "%s", Failure.what()); }
        return 0;
    }
    static void* allocate(void* User, void* Pointer, size_t Old, size_t New) {
        auto* Self = static_cast<LuaRuntime*>(User); if (!Pointer) Old = 0;
        if (!New) { std::free(Pointer); Self->bytes_ -= Old; return nullptr; }
        if (New > Self->limits_.heapBytes_ || Self->bytes_ - Old > Self->limits_.heapBytes_ - New) { Self->memory_ = true; return nullptr; }
        void* Result = std::realloc(Pointer, New);
        if (Result) Self->bytes_ = Self->bytes_ - Old + New; else Self->memory_ = true;
        return Result;
    }
    static void hook(lua_State* State, lua_Debug*) {
        auto* Self = *static_cast<LuaRuntime**>(lua_getextraspace(State));
        Self->instructions_ += 1000;
        if (Self->instructions_ > Self->limits_.instructions_) Self->quota_ = true;
        try { Self->check(); } catch (const std::exception& Failure) { luaL_error(State, "%s", Failure.what()); }
    }
    lua_State* state_ = nullptr;
    std::shared_ptr<IScriptHostApi> api_;
    RuntimeLimits limits_;
    Clock::time_point deadline_ = Clock::time_point::max();
    std::size_t bytes_ = 0, calls_ = 0;
    std::uint64_t instructions_ = 0;
    std::atomic<bool> interrupted_{false};
    bool memory_ = false, quota_ = false, expired_ = false;
};
}
std::unique_ptr<IScriptRuntime> MakeLuaRuntime() { return std::make_unique<LuaRuntime>(); }
Result<Unit> CheckLuaSyntax(std::string_view Source) {
    if (Source.size() > 65536 || Source.find('\0') != std::string_view::npos) return Error{ErrorCategory::InvalidRequest, "runtime-source"};
    auto* State = luaL_newstate(); if (!State) return Error{ErrorCategory::BudgetExceeded, "runtime-memory"};
    const int Status = luaL_loadbufferx(State, Source.data(), Source.size(), "script.lua", "t");
    Result<Unit> Result = Unit{};
    if (Status != LUA_OK) Result = Error{ErrorCategory::InvalidRequest, lua_tostring(State, -1)};
    lua_close(State); return Result;
}
}
