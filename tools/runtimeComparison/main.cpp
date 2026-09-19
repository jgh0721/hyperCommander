#include "runtime.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace hc::script;
using namespace hc::script::comparison;
using namespace hc::commands;
using namespace hc::storage;
using Object=ScriptValue::Object;
namespace {
void Require(bool Value,const char* Message) { if(!Value) throw std::runtime_error(Message); }
class Queue final : public IExecutor {
public:
    bool Post(std::function<void()> Work) override { work_.push_back(std::move(Work)); return true; }
    void Drain() { while(!work_.empty()) { auto Work=std::move(work_.front()); work_.pop_front(); Work(); } }
private:
    std::deque<std::function<void()>> work_;
};
// A deterministic host fixture, not a production workspace/event facade.
// CommandScriptApi is exactly the implementation used by CommandUiBridge.
class Fixture final : public IScriptHostApi {
public:
    Fixture() {
        hc::automation::Host Host;
        Host.capture_=[this]() -> Result<CommandContext> { if(stale_) return Error{ErrorCategory::StaleRevision,"fixture-owner-closed"}; return CommandContext{"tab",{LocatorKind::Local,"C:/한글"},{{{"local"},"한글.txt"}}}; };
        Host.schema_=[](const std::string& Id) -> Result<CommandDescriptor> {
            if(Id!="read" && Id!="write") return Error{ErrorCategory::NotFound,"unknown-command"};
            return CommandDescriptor{Id,1,{{"text",{ParameterType::String}}},Id=="write"?"file-write":"navigation"};
        };
        Host.dispatch_=[this](const auto&) { polls_=0; ++calls_; return Ready<CommandOutcome>(CommandOutcome{CommandOutcome::Status::Accepted,"fixture-job"}); };
        Host.poll_=[this](const auto&,const auto&) -> Result<bool> { return ++polls_>=2; };
        Host.cancel_=[this](const auto&,const auto&) { ++cancelled_; };
        Host.output_=[](const auto&,const auto&) -> Result<Parameters> { return Parameters{{"text",std::string("한글")},{"large",std::int64_t(INT64_MAX)}}; };
        commands_=std::make_unique<CommandScriptApi>(std::move(Host),std::set<std::string>{"navigation"});
        queue_=std::make_shared<Queue>();
        auto Subscription=events_.Subscribe("fixture",queue_,[this](const auto&) { ++delivered_; },32);
        subscription_=std::move(Subscription.Value());
    }
    Result<ScriptValue> Call(std::string_view Method,const ScriptValue& Arguments) override {
        if(closed_) return Error{ErrorCategory::Cancelled,"fixture-closed"};
        auto Valid=Validate(Arguments); if(!Valid.HasValue()) return Valid.Failure();
        if(Method=="echo") { ++echoes_; return Arguments; }
        if(Method=="emit") {
            PublishResult Total;
            for(int I=0;I<10000;++I) { EventEnvelope Event; Event.id_="fixture"; auto Result=events_.Publish(std::move(Event)); Total.accepted_+=Result.accepted_; Total.rejected_+=Result.rejected_; }
            return ScriptValue(Object{{"accepted",std::int64_t(Total.accepted_)},{"rejected",std::int64_t(Total.rejected_)}});
        }
        if(Method=="drain") { queue_->Drain(); return ScriptValue(std::int64_t(delivered_)); }
        return commands_->Call(Method,Arguments);
    }
    void Close() override { if(closed_) return; closed_=true; subscription_.Reset(); queue_->Drain(); commands_->Close(); }
    bool stale_=false,closed_=false;
    int calls_=0,cancelled_=0,delivered_=0,echoes_=0;
private:
    int polls_=0;
    std::unique_ptr<CommandScriptApi> commands_;
    EventHub events_;
    std::shared_ptr<Queue> queue_;
    EventSubscription subscription_;
};
std::string Quote(const QByteArray& Text) {
    const auto Json=QJsonDocument(QJsonArray{QString::fromUtf8(Text)}).toJson(QJsonDocument::Compact);
    return Json.mid(1,Json.size()-2).toStdString();
}
Object HostCall(IScriptRuntime& Runtime,const std::string& Method,const ScriptValue& Arguments) {
    auto Value=Runtime.Evaluate("return hc("+Quote(QByteArray::fromStdString(Method))+","+Quote(Encode(Arguments))+")");
    Require(Value.HasValue(),Value.HasValue()?"":Value.Failure().code_.c_str());
    const auto* String=std::get_if<std::string>(&Value.Value().data_); Require(String,"host result string");
    auto Decoded=Decode(QByteArray::fromStdString(*String)); Require(Decoded.HasValue(),"host result json");
    return std::get<Object>(Decoded.Value().data_);
}
bool Ok(const Object& Value) { return std::get<bool>(Value.at("ok").data_); }
std::string Code(const Object& Value) { return std::get<std::string>(Value.at("code").data_); }
double Number(const ScriptValue& Value) { if(const auto* Integer=std::get_if<std::int64_t>(&Value.data_)) return static_cast<double>(*Integer); return std::get<double>(Value.data_); }
double Measure(const std::function<void()>& Work) { QElapsedTimer Timer; Timer.start(); Work(); return Timer.nsecsElapsed()/1000000.0; }
QJsonObject Memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX Counters{}; Counters.cb=sizeof(Counters);
    Require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&Counters),sizeof(Counters))!=FALSE,"process memory counters");
    return {{"privateBytes",static_cast<qint64>(Counters.PrivateUsage)},{"peakCommitBytes",static_cast<qint64>(Counters.PeakPagefileUsage)}};
#else
    return {};
#endif
}
}
int main(int Argc,char** Argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QCoreApplication App(Argc,Argv);
    // The supervisor may attach its own resource limits before evaluation.
    if(App.arguments().contains("--handshake")) {
        std::cout<<"ready"<<std::endl; std::string Go; std::getline(std::cin,Go);
        if(Go.starts_with("\xEF\xBB\xBF")) Go.erase(0,3);
        if(!Go.empty() && Go.back()=='\r') Go.pop_back();
        if(Go!="go") return 2;
    }
    auto Runtime=MakeRuntime(); const auto Descriptor=Runtime->Describe(); const bool Js=Descriptor.id_=="qjs";
    QJsonObject Report{{"candidate",QString::fromStdString(Descriptor.id_)},{"version",QString::fromStdString(Descriptor.version_)},
        {"qt",QT_VERSION_STR},{"nativeHeapQuota",Descriptor.heapQuota_},{"nativeInstructionQuota",Descriptor.instructionQuota_}};
    QJsonArray Checks;
    try {
        auto Host=std::make_shared<Fixture>(); RuntimeLimits Limits; Limits.instructions_=1000000000;
        Report["loadMs"]=Measure([&] { auto Loaded=Runtime->Load(Host,Limits); Require(Loaded.HasValue(),Loaded.HasValue()?"":Loaded.Failure().code_.c_str()); });
        if(App.arguments().contains("--heap")) {
            const auto Result=Runtime->Evaluate(Js ? "var a=[];for(var i=0;i<100000;i++)a.push(new Array(1024).fill(i));return a.length;"
                : "local a={} for i=1,100000 do a[i]=string.rep('x',4096)..i end return #a");
            Report["heapRejected"]=!Result.HasValue(); Report["heapError"]=Result.HasValue()?QString():QString::fromStdString(Result.Failure().code_);
            Require(!Descriptor.heapQuota_ || (!Result.HasValue() && Result.Failure().category_==ErrorCategory::BudgetExceeded),"heap quota not enforced");
        } else {
            auto Roundtrip=HostCall(*Runtime,"echo",Object{{"unicode","한글😀"},{"integer",std::int64_t(INT64_MAX)},{"wholeDouble",42.0},
                {"nested",ScriptValue::Array{true,ScriptValue{},Object{{"value",3.25}}}}});
            Require(Ok(Roundtrip),"echo failed"); const auto& Echo=std::get<Object>(Roundtrip.at("value").data_);
            Require(std::get<std::int64_t>(Echo.at("integer").data_)==INT64_MAX && std::get<std::string>(Echo.at("unicode").data_)=="한글😀","wire fidelity"); Checks.append("unicode-int64-value-tree");
            Require(std::holds_alternative<double>(Echo.at("wholeDouble").data_) && std::get<double>(Echo.at("wholeDouble").data_)==42.0,"integral double fidelity");
            Require(!Decode("9007199254740992").HasValue(),"unsafe JSON integer");
            Require(!Runtime->Evaluate(Js?"var = ;":"local =").HasValue(),"syntax diagnostic");
            Require(!Runtime->Evaluate(Js?"throw new Error('expected');":"error('expected')").HasValue(),"runtime diagnostic"); Checks.append("syntax-runtime-errors");
            Require(!Runtime->Evaluate("return 1/0").HasValue(),"finite output value");
            auto Exact=Runtime->Evaluate(Js?"return 9007199254740992 !== 9007199254740993;":"return 9007199254740992 ~= 9007199254740993");
            Require(Exact.HasValue(),"native integer check"); Report["nativeIntegerBeyond53Bits"]=std::get<bool>(Exact.Value().data_);
            auto Surface=Runtime->Evaluate(Js?"return typeof __bridge === 'undefined';":"return io == nil and os == nil and package == nil and debug == nil and loadfile == nil");
            Require(Surface.HasValue() && std::get<bool>(Surface.Value().data_),"restricted binding surface"); Checks.append("value-limits-binding-surface");
            const auto Snapshot=HostCall(*Runtime,"capture",Object{}); Require(Ok(Snapshot),"capture");
            const auto Context=std::get<Object>(Snapshot.at("value").data_).at("context");
            Object Invoke{{"context",Context},{"command","write"},{"parameters",Object{{"text","한글"}}}};
            auto Denied=HostCall(*Runtime,"invoke",Invoke); Require(!Ok(Denied) && Code(Denied)=="permission-required" && Host->calls_==0,"write permission"); Checks.append("permission-denial");
            Invoke["command"]="read"; const auto Job=HostCall(*Runtime,"invoke",Invoke); Require(Ok(Job),"invoke");
            const Object Handle{{"handle",Job.at("value")}};
            const auto Pending=HostCall(*Runtime,"poll",Handle); Require(Ok(Pending) && !std::get<bool>(std::get<Object>(Pending.at("value").data_).at("done").data_),"accepted not complete");
            const auto Done=HostCall(*Runtime,"poll",Handle); Require(Ok(Done) && std::get<bool>(std::get<Object>(Done.at("value").data_).at("done").data_),"completion"); Checks.append("async-job-poll");
            Require(Ok(HostCall(*Runtime,"release",Handle)) && !Ok(HostCall(*Runtime,"poll",Handle)),"released job");
            Host->stale_=true; Require(!Ok(HostCall(*Runtime,"invoke",Invoke)),"stale owner"); Host->stale_=false; Checks.append("stale-handle-owner");
            auto Flood=HostCall(*Runtime,"emit",Object{}); Require(Ok(Flood),"event flood");
            Require(std::get<std::int64_t>(std::get<Object>(Flood.at("value").data_).at("accepted").data_)==32,"bounded event queue");
            HostCall(*Runtime,"drain",Object{}); Require(Host->delivered_==32,"event delivery"); Checks.append("event-backpressure-10000");
            const auto NativeScript=Js?"var s=0;for(var i=1;i<=100000;i++)s=add(s,i);return s;":"local s=0 for i=1,100000 do s=add(s,i) end return s";
            QJsonArray NativeSamples,WireSamples;
            for(int I=0;I<5;++I) NativeSamples.append(Measure([&] { auto Value=Runtime->Evaluate(NativeScript); Require(Value.HasValue() && Number(Value.Value())==5000050000.0,"native sum"); }));
            const auto WireCall="hc(\"echo\",\"{\\\"text\\\":\\\"한글\\\",\\\"count\\\":42}\")";
            const auto WireScript=Js?std::string("var s;for(var i=0;i<10000;i++)s=")+WireCall+";return s;":std::string("local s for i=1,10000 do s=")+WireCall+" end return s";
            const int Before=Host->echoes_;
            for(int I=0;I<5;++I) WireSamples.append(Measure([&] { Require(Runtime->Evaluate(WireScript).HasValue(),"wire throughput"); }));
            Require(Host->echoes_-Before==50000,"wire call count");
            Report["native100kMs"]=NativeSamples; Report["wire10kMs"]=WireSamples;
            // A native engine quota and asynchronous interruption are separate cases.
            if(Descriptor.instructionQuota_) {
                auto QuotaRuntime=MakeRuntime(); auto QuotaHost=std::make_shared<Fixture>(); RuntimeLimits Quota; Quota.instructions_=10000;
                Require(QuotaRuntime->Load(QuotaHost,Quota).HasValue(),"quota load");
                auto Limited=QuotaRuntime->Evaluate("while true do end"); Require(!Limited.HasValue() && Limited.Failure().code_=="runtime-instructions","instruction limit"); Checks.append("instruction-quota");
            }
            std::chrono::steady_clock::time_point Requested,Finished;
            std::jthread Watchdog([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); Requested=std::chrono::steady_clock::now(); Runtime->RequestInterrupt(); });
            Report["interruptMs"]=Measure([&] { auto Result=Runtime->Evaluate(Js?"while(true){}":"while true do end"); Require(!Result.HasValue() && Result.Failure().category_==ErrorCategory::Cancelled,"interrupt"); });
            Finished=std::chrono::steady_clock::now(); Watchdog.join();
            Report["interruptResponseMs"]=std::chrono::duration<double,std::milli>(Finished-Requested).count(); Report["watchdogDelayMs"]=100;
            Require(Runtime->Evaluate("return 1").HasValue(),"interrupt recovery"); Checks.append("interrupt-recovery");
            HostCall(*Runtime,"emit",Object{}); auto PendingJob=HostCall(*Runtime,"invoke",Invoke); Require(Ok(PendingJob),"pending unload");
            const int EchoesBeforeUnload=Host->echoes_;
            if(!Js) Require(Runtime->Evaluate("finalizer = setmetatable({}, {__gc=function() hc('echo','{}') end})").HasValue(),"finalizer setup");
            Report["unloadMs"]=Measure([&] { Runtime->Unload(); });
            Require(Host->closed_ && Host->cancelled_==1 && Host->delivered_==32,"unload cleanup");
            Require(Host->echoes_==EchoesBeforeUnload,"finalizer host access after close");
            Require(!Runtime->Evaluate("return 1").HasValue() && !Host->Call("echo",Object{}).HasValue(),"unload access"); Checks.append("unload-cancel-subscriptions");
            Report["beforeReloadMemory"]=Memory();
            Report["reload50Ms"]=Measure([&] { for(int I=0;I<50;++I) { auto Next=std::make_shared<Fixture>(); Require(Runtime->Load(Next,Limits).HasValue(),"reload"); Require(Runtime->Evaluate("return 1").HasValue(),"reload eval"); Runtime->Unload(); Require(Next->closed_,"reload close"); } });
            Report["afterReloadMemory"]=Memory();
        }
        Runtime->Unload(); Report["passed"]=true;
    } catch(const std::exception& Error) { Report["passed"]=false; Report["error"]=QString::fromUtf8(Error.what()); }
    Report["checks"]=Checks;
    Report["memory"]=Memory();
    std::cout<<QJsonDocument(Report).toJson(QJsonDocument::Compact).constData()<<std::endl;
    return Report["passed"].toBool()?0:1;
}
