#include "runtime.hpp"
#include <atomic>
#include <cstdlib>
#include <mutex>
#ifdef HC_RUNTIME_QJS
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#else
// Lua is built as C++ for exception unwinding, with the same API in both arms.
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef HC_RUNTIME_SOL2
#include <sol/sol.hpp>
#endif
#endif

namespace hc::script::comparison {
Result<ScriptValue> Checked(ScriptValue Value) {
    const auto Valid=Validate(Value); if(!Valid.HasValue()) return Valid.Failure(); return Value;
}
#ifdef HC_RUNTIME_QJS
class Bridge final : public QObject {
    Q_OBJECT
public:
    explicit Bridge(std::shared_ptr<IScriptHostApi> Api) : api_(std::move(Api)) {}
    Q_INVOKABLE QString Call(const QString& Method,const QString& Arguments) {
        return QString::fromUtf8(comparison::Call(*api_,Method.toStdString(),Arguments.toUtf8()));
    }
    Q_INVOKABLE double Add(double Left,double Right) { return Left+Right; }
private:
    std::shared_ptr<IScriptHostApi> api_;
};
class Runtime final : public IScriptRuntime {
public:
    ~Runtime() override { Unload(); }
    RuntimeDescriptor Describe() const override { return {"qjs",QT_VERSION_STR,false,false}; }
    Result<Unit> Load(std::shared_ptr<IScriptHostApi> Api,RuntimeLimits) override {
        Unload(); if(!Api || Api->ApiVersion()!=1) return Error{ErrorCategory::InvalidRequest,"api-version"};
        std::lock_guard Lock(mutex_);
        api_=std::move(Api); bridge_=std::make_unique<Bridge>(api_); engine_=std::make_unique<QJSEngine>();
        QJSEngine::setObjectOwnership(bridge_.get(),QJSEngine::CppOwnership);
        engine_->globalObject().setProperty("__bridge",engine_->newQObject(bridge_.get()));
        const auto Init=engine_->evaluate("(function(g,b){g.hc=function(m,a){return b.Call(m,a);};g.add=function(a,c){return b.Add(a,c);};})(this,__bridge); delete this.__bridge;");
        if(Init.isError()) { const auto Message=Init.toString().toStdString(); engine_.reset(); bridge_.reset(); api_->Close(); api_.reset(); return Error{ErrorCategory::Internal,"qjs-load:"+Message}; } return Unit{};
    }
    Result<ScriptValue> Evaluate(std::string_view Source) override {
        if(!engine_) return Error{ErrorCategory::Cancelled,"runtime-unloaded"};
        if(Source.size()>65536) return Error{ErrorCategory::BudgetExceeded,"runtime-source"};
        engine_->setInterrupted(false);
        const auto Value=engine_->evaluate("(function(){"+QString::fromUtf8(Source.data(),Source.size())+"\n})()","comparison.js");
        if(Value.isError()) return Error{engine_->isInterrupted()?ErrorCategory::Cancelled:ErrorCategory::InvalidRequest,
            engine_->isInterrupted()?"runtime-interrupted":Value.toString().toStdString()};
        if(Value.isString()) return Checked(Value.toString().toStdString());
        if(Value.isBool()) return ScriptValue(Value.toBool());
        if(Value.isNumber()) return Checked(Value.toNumber());
        if(Value.isNull() || Value.isUndefined()) return ScriptValue{};
        return Error{ErrorCategory::Unsupported,"runtime-result-type"};
    }
    void RequestInterrupt() override { std::lock_guard Lock(mutex_); if(engine_) engine_->setInterrupted(true); }
    void Unload() override {
        std::lock_guard Lock(mutex_); if(api_) api_->Close(); engine_.reset(); bridge_.reset(); api_.reset();
    }
private:
    std::mutex mutex_;
    std::shared_ptr<IScriptHostApi> api_;
    std::unique_ptr<Bridge> bridge_;
    std::unique_ptr<QJSEngine> engine_;
};
#else
class Runtime final : public IScriptRuntime {
public:
    ~Runtime() override { Unload(); }
    RuntimeDescriptor Describe() const override {
#ifdef HC_RUNTIME_SOL2
        return {"sol2","3.3.0 / " LUA_RELEASE,true,true};
#else
        return {"lua",LUA_RELEASE,true,true};
#endif
    }
    Result<Unit> Load(std::shared_ptr<IScriptHostApi> Api,RuntimeLimits Limits) override {
        Unload(); if(!Api || Api->ApiVersion()!=1) return Error{ErrorCategory::InvalidRequest,"api-version"};
        if(Limits.heapBytes_<1024*1024 || !Limits.instructions_) return Error{ErrorCategory::InvalidRequest,"runtime-limits"};
        api_=std::move(Api); limits_=Limits; state_=lua_newstate(&allocate,this);
        if(!state_) { Unload(); return Error{ErrorCategory::BudgetExceeded,"runtime-memory"}; }
        *static_cast<Runtime**>(lua_getextraspace(state_))=this;
        luaL_requiref(state_,"_G",luaopen_base,1); lua_pop(state_,1);
        luaL_requiref(state_,LUA_TABLIBNAME,luaopen_table,1); lua_pop(state_,1);
        luaL_requiref(state_,LUA_STRLIBNAME,luaopen_string,1); lua_pop(state_,1);
        luaL_requiref(state_,LUA_MATHLIBNAME,luaopen_math,1); lua_pop(state_,1);
        luaL_requiref(state_,LUA_UTF8LIBNAME,luaopen_utf8,1); lua_pop(state_,1);
        for(const char* Name:{"dofile","loadfile","load","collectgarbage","print","warn"}) { lua_pushnil(state_); lua_setglobal(state_,Name); }
#ifdef HC_RUNTIME_SOL2
        sol::state_view View(state_);
        View.set_function("hc",[this](const std::string& Method,const std::string& Arguments) { return comparison::Call(*api_,Method,QByteArray::fromStdString(Arguments)).toStdString(); });
        View.set_function("add",[](std::int64_t Left,std::int64_t Right) {
            // The benchmark stays within exact integer range; avoid signed UB.
            return static_cast<lua_Integer>(static_cast<std::uint64_t>(Left)+static_cast<std::uint64_t>(Right));
        });
#else
        lua_pushcfunction(state_,&call); lua_setglobal(state_,"hc");
        lua_pushcfunction(state_,&add); lua_setglobal(state_,"add");
#endif
        lua_sethook(state_,&hook,LUA_MASKCOUNT,1000); return Unit{};
    }
    Result<ScriptValue> Evaluate(std::string_view Source) override {
        if(!state_) return Error{ErrorCategory::Cancelled,"runtime-unloaded"};
        if(Source.size()>65536 || (!Source.empty() && Source[0]=='\x1b')) return Error{ErrorCategory::InvalidRequest,"runtime-source"};
        interrupted_=false; instructions_=0; quota_=false; memory_=false;
#ifdef HC_RUNTIME_SOL2
        sol::state_view View(state_); auto Result=View.safe_script(std::string(Source),sol::script_pass_on_error);
        if(!Result.valid()) { sol::error Error=Result; return failure(Error.what()); }
        if(Result.return_count()==0) return ScriptValue{};
        const auto Value=Result.get<sol::object>();
        if(Value.get_type()==sol::type::string) return Checked(Value.as<std::string>());
        if(Value.get_type()==sol::type::boolean) return ScriptValue(Value.as<bool>());
        if(Value.get_type()==sol::type::number) { if(Value.is<lua_Integer>()) return ScriptValue(std::int64_t(Value.as<lua_Integer>())); return Checked(Value.as<double>()); }
        if(Value.get_type()==sol::type::nil) return ScriptValue{};
        return Error{ErrorCategory::Unsupported,"runtime-result-type"};
#else
        lua_settop(state_,0); int Status=luaL_loadbufferx(state_,Source.data(),Source.size(),"comparison.lua","t");
        if(Status==LUA_OK) Status=lua_pcall(state_,0,1,0);
        if(Status!=LUA_OK) { const auto Result=failure(lua_tostring(state_,-1)); lua_settop(state_,0); return Result; }
        hc::script::Result<ScriptValue> Result=Error{ErrorCategory::Unsupported,"runtime-result-type"};
        switch(lua_type(state_,-1)) {
        case LUA_TNIL: Result=ScriptValue{}; break;
        case LUA_TBOOLEAN: Result=ScriptValue(lua_toboolean(state_,-1)!=0); break;
        case LUA_TNUMBER: Result=lua_isinteger(state_,-1) ? ScriptValue(std::int64_t(lua_tointeger(state_,-1))) : ScriptValue(double(lua_tonumber(state_,-1))); break;
        case LUA_TSTRING: { size_t Size=0; const char* Text=lua_tolstring(state_,-1,&Size); Result=ScriptValue(std::string(Text,Size)); break; }
        }
        lua_settop(state_,0); return Result.HasValue()?Checked(std::move(Result.Value())):Result;
#endif
    }
    void RequestInterrupt() override { interrupted_=true; }
    void Unload() override {
        // Revoke host access before Lua runs __gc finalizers during lua_close.
        if(api_) api_->Close(); if(state_) lua_close(state_); state_=nullptr; api_.reset(); bytes_=0;
    }
private:
    Result<ScriptValue> failure(const char* Text) const {
        return Error{interrupted_?ErrorCategory::Cancelled:(quota_ || memory_)?ErrorCategory::BudgetExceeded:ErrorCategory::InvalidRequest,
            interrupted_?"runtime-interrupted":quota_?"runtime-instructions":memory_?"runtime-memory":Text?Text:"runtime-error"};
    }
    static void* allocate(void* User,void* Pointer,size_t Old,size_t New) {
        auto* Self=static_cast<Runtime*>(User); if(!Pointer) Old=0;
        if(!New) { std::free(Pointer); Self->bytes_-=Old; return nullptr; }
        if(New>Self->limits_.heapBytes_ || Self->bytes_-Old>Self->limits_.heapBytes_-New) { Self->memory_=true; return nullptr; }
        void* Result=std::realloc(Pointer,New); if(Result) Self->bytes_=Self->bytes_-Old+New; return Result;
    }
    static void hook(lua_State* State,lua_Debug*) {
        auto* Self=*static_cast<Runtime**>(lua_getextraspace(State)); Self->instructions_+=1000;
        if(Self->interrupted_) luaL_error(State,"runtime-interrupted");
        if(Self->instructions_>Self->limits_.instructions_) { Self->quota_=true; luaL_error(State,"runtime-instructions"); }
    }
    static int call(lua_State* State) {
        auto* Self=*static_cast<Runtime**>(lua_getextraspace(State)); size_t MethodSize=0,ArgumentSize=0;
        const char* Method=luaL_checklstring(State,1,&MethodSize); const char* Arguments=luaL_checklstring(State,2,&ArgumentSize);
        // The owned response outlives pushes; C API errors cannot strand RAII.
        Self->reply_=comparison::Call(*Self->api_,std::string_view(Method,MethodSize),QByteArray(Arguments,ArgumentSize));
        lua_pushlstring(State,Self->reply_.constData(),Self->reply_.size()); return 1;
    }
    static int add(lua_State* State) {
        const auto Left=luaL_checkinteger(State,1); const auto Right=luaL_checkinteger(State,2);
        lua_pushinteger(State,static_cast<lua_Integer>(static_cast<lua_Unsigned>(Left)+static_cast<lua_Unsigned>(Right))); return 1;
    }
    lua_State* state_=nullptr;
    std::shared_ptr<IScriptHostApi> api_;
    RuntimeLimits limits_;
    std::size_t bytes_=0;
    std::uint64_t instructions_=0;
    std::atomic<bool> interrupted_{false};
    bool quota_=false;
    bool memory_=false;
    QByteArray reply_;
};
#endif
std::unique_ptr<IScriptRuntime> MakeRuntime() { return std::make_unique<Runtime>(); }
}
#ifdef HC_RUNTIME_QJS
#include "runtime.moc"
#endif
