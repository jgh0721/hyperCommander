#include "commonLibs/scripting/scriptExecution.hpp"
#include <QtTest>

using namespace hc::script;
using Object = ScriptValue::Object;
class EchoApi final : public IScriptHostApi {
public:
    std::uint32_t ApiVersion() const override { return version_; }
    Result<ScriptValue> Call(std::string_view Method, const ScriptValue& Arguments) override {
        if (closed_) return Error{ErrorCategory::Cancelled, "closed"};
        ++calls_; ownerCorrect_ = ownerCorrect_ && owner_ == std::this_thread::get_id();
        if (Method == "denied") return Error{ErrorCategory::AccessDenied, "permission-required"};
        return Arguments;
    }
    void Close() override { closed_ = true; }
    int calls_ = 0; bool closed_ = false, ownerCorrect_ = true;
    std::uint32_t version_ = 1;
    std::thread::id owner_ = std::this_thread::get_id();
};
class LuaTests : public QObject {
    Q_OBJECT
private slots:
    void compiledReadOnlyColumns() {
        auto Runtime = MakeLuaColumnRuntime(); RuntimeLimits Limits; Limits.instructions_ = 100000; Limits.wallTime_ = std::chrono::milliseconds(75);
        QVERIFY(Runtime->Prepare("local count=0; return function(item) count=count+1; return {value=item.value, count=count, safe=hc==nil and io==nil and os==nil and load==nil and debug==nil and setmetatable==nil} end", Limits).HasValue());
        for (int Index = 1; Index <= 3; ++Index) {
            auto Result = Runtime->Invoke(Object{{"value", std::int64_t(9007199254740993LL)}}); QVERIFY(Result.HasValue());
            const auto Values = std::get<Object>(Result.Value().data_);
            QCOMPARE(std::get<std::int64_t>(Values.at("value").data_), 9007199254740993LL);
            QCOMPARE(std::get<std::int64_t>(Values.at("count").data_), Index); QVERIFY(std::get<bool>(Values.at("safe").data_));
        }
        QVERIFY(!Runtime->Prepare("return 42", Limits).HasValue());
        for (const auto* Source : {"return function(item) while true do end end", "return function(item) return string.rep('x', 16777216) end"}) {
            QVERIFY(Runtime->Prepare(Source, Limits).HasValue());
            const auto Result = Runtime->Invoke(Object{}); QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::BudgetExceeded);
        }
        QVERIFY(Runtime->Prepare("return function(item) return {value=1} end", Limits).HasValue()); Runtime->RequestInterrupt();
        QCOMPARE(Runtime->Invoke(Object{}).Failure().category_, ErrorCategory::Cancelled);
    }

    void rejectedVersionAndFinalizerRevocation() {
        auto Future = std::make_shared<EchoApi>(); Future->version_ = 2;
        ScriptExecution Rejected(Future, "hc.capture()"); QVERIFY(!Rejected.IsRunning());
        QCOMPARE(Rejected.Outcome()->Failure().code_, std::string("api-version")); QVERIFY(Future->closed_); QCOMPARE(Future->calls_,0);
        auto Api = std::make_shared<EchoApi>(); auto Runtime = MakeLuaRuntime(); QVERIFY(Runtime->Load(Api,{}).HasValue());
        QVERIFY(Runtime->Evaluate("finalizer=setmetatable({}, {__gc=function() hc.call('echo',{}) end}); return true").HasValue());
        Runtime->Unload(); QVERIFY(Api->closed_); QCOMPARE(Api->calls_,0);
    }
    void nativeValuesAndRestrictedLibraries() {
        auto Runtime = MakeLuaRuntime(); auto Api = std::make_shared<EchoApi>();
        QVERIFY(Runtime->Load(Api, {}).HasValue());
        auto Value = Runtime->Evaluate("return hc.request('echo', {text='한글', number=9223372036854775807, list={true, false, 3.5}})");
        QVERIFY2(Value.HasValue(), Value.HasValue() ? "" : Value.Failure().code_.c_str());
        auto Data = std::get<Object>(Value.Value().data_); QCOMPARE(std::get<std::int64_t>(Data.at("number").data_), INT64_MAX);
        auto Nulls = Runtime->Evaluate("return hc.request('echo', {hc.null, 1, hc.null})"); QVERIFY(Nulls.HasValue());
        const auto Array = std::get<ScriptValue::Array>(Nulls.Value().data_); QCOMPARE(Array.size(), 3u); QVERIFY(std::holds_alternative<Unit>(Array[2].data_));
        QCOMPARE(std::get<std::string>(Data.at("text").data_), std::string("한글"));
        auto Safe = Runtime->Evaluate("return io==nil and os==nil and package==nil and debug==nil and load==nil and dofile==nil and coroutine==nil and pcall==nil");
        QVERIFY(Safe.HasValue()); QVERIFY(std::get<bool>(Safe.Value().data_));
        auto Denied = Runtime->Evaluate("return hc.call('denied', {}).error"); QVERIFY(Denied.HasValue());
        QCOMPARE(std::get<std::string>(Denied.Value().data_), std::string("permission-required"));
        for (const auto* Source : {"local t={}; t.self=t; return hc.call('echo',t)", "return {a=1,[1]=2}", "return {[100]=1}", "return 0/0", "return function() end"})
            QVERIFY(!Runtime->Evaluate(Source).HasValue());
        QVERIFY(CheckLuaSyntax("return 42").HasValue()); QVERIFY(!CheckLuaSyntax("return (").HasValue());
        QVERIFY(!Runtime->Evaluate(std::string("\x1bLua",4)).HasValue());
        Runtime->Unload(); QVERIFY(Api->closed_);
    }
    void quotasAndFreshSessions() {
        auto Runtime = MakeLuaRuntime();
        for (const auto* Source : {"while true do end", "return string.rep('x', 16*1024*1024)"}) {
            QVERIFY(Runtime->Load(std::make_shared<EchoApi>(), {}).HasValue());
            auto Value = Runtime->Evaluate(Source); QVERIFY(!Value.HasValue()); QCOMPARE(Value.Failure().category_, ErrorCategory::BudgetExceeded);
            QVERIFY(Runtime->Load(std::make_shared<EchoApi>(), {}).HasValue()); QVERIFY(Runtime->Evaluate("return 42").HasValue());
        }
        RuntimeLimits Limits; Limits.hostCalls_ = 2;
        QVERIFY(Runtime->Load(std::make_shared<EchoApi>(), Limits).HasValue());
        QVERIFY(!Runtime->Evaluate("for i=1,3 do hc.call('echo', {}) end").HasValue());
        Limits.wallTime_ = std::chrono::milliseconds(15);
        QVERIFY(Runtime->Load(std::make_shared<EchoApi>(), Limits).HasValue());
        auto Timeout = Runtime->Evaluate("hc.sleep(1000)"); QVERIFY(!Timeout.HasValue()); QCOMPARE(Timeout.Failure().code_, std::string("runtime-deadline"));
    }
    void mailboxThreadAndCancel() {
        auto Api = std::make_shared<EchoApi>();
        ScriptExecution Run(Api, "return hc.request('echo', {answer=42})");
        QTRY_VERIFY_WITH_TIMEOUT((Run.Tick(), !Run.IsRunning()), 5000); Run.Tick();
        QVERIFY(Run.Outcome()->HasValue()); QVERIFY(Api->ownerCorrect_); QVERIFY(Api->closed_); QCOMPARE(Api->calls_, 1);
        RuntimeLimits Limits; Limits.instructions_ = UINT64_MAX;
        auto PendingApi = std::make_shared<EchoApi>();
        ScriptExecution Pending(PendingApi, "while true do end", Limits);
        Pending.Cancel(); QTRY_VERIFY_WITH_TIMEOUT(!Pending.IsRunning(), 2000); QVERIFY(PendingApi->closed_);
        QVERIFY(!Pending.Outcome()->HasValue());
        // Destruction while waiting on a UI call cannot wait for a queued UI event.
        { ScriptExecution Waiting(std::make_shared<EchoApi>(), "hc.capture()", Limits); QTest::qSleep(20); }
    }
    void productHostPermissionsAndAwait() {
        using namespace hc::commands; using namespace hc::storage;
        hc::automation::Host Host; int Calls = 0, Polls = 0; bool Cancelled = false;
        Host.capture_ = []() -> Result<CommandContext> { return CommandContext{}; };
        Host.schema_ = [](const auto& Id) -> Result<CommandDescriptor> { return CommandDescriptor{Id,1,{},"file-write"}; };
        Host.dispatch_ = [&](const auto&) { ++Calls; return Ready<CommandOutcome>(CommandOutcome{}); };
        Host.poll_ = [&](const auto&,const auto&) -> Result<bool> { return ++Polls >= 3; };
        Host.cancel_ = [&](const auto&,const auto&) { Cancelled = true; };
        const auto Source = "local c=hc.capture(); local j=hc.invoke(c.context,'write',{}); local r=hc.job.await(j); hc.release(j); return r";
        ScriptExecution Denied(std::make_shared<CommandScriptApi>(Host,std::set<std::string>{}), Source);
        QTRY_VERIFY_WITH_TIMEOUT((Denied.Tick(),!Denied.IsRunning()),5000); Denied.Tick(); QVERIFY(!Denied.Outcome()->HasValue()); QCOMPARE(Calls,0);
        ScriptExecution Allowed(std::make_shared<CommandScriptApi>(Host,std::set<std::string>{"file-write"}), Source);
        QTRY_VERIFY_WITH_TIMEOUT((Allowed.Tick(),!Allowed.IsRunning()),5000); Allowed.Tick(); QVERIFY(Allowed.Outcome()->HasValue()); QCOMPARE(Calls,1); QVERIFY(!Cancelled);
    }
};
QTEST_GUILESS_MAIN(LuaTests)
#include "tstLuaRuntime.moc"
