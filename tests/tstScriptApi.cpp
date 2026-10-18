#include "commonLibs/scripting/scriptApi.hpp"
#include <QtTest>

using namespace hc::script;
using namespace hc::storage;
using namespace hc::commands;
using Object=ScriptValue::Object;
class ScriptApiTests : public QObject {
    Q_OBJECT
private slots:
    void subscriptionCloseDuringPublication() {
        hc::automation::Host Host; Host.script_ = std::make_shared<Services>(); Host.script_->events_ = std::make_shared<EventHub>();
        const auto Events = Host.script_->events_; std::atomic<bool> Stop{false};
        std::thread Publisher([&] {
            while (!Stop.load()) { EventEnvelope Event; Event.id_="job.progress"; Event.payload_={{"value",std::string(8000,'x')}}; Events->Publish(std::move(Event)); }
        });
        bool Revoked = true;
        for (int I=0;I<100;++I) {
            CommandScriptApi Api(Host,{"navigation"});
            const auto Sub = Api.Call("events.subscribe",Object{{"event","job.progress"}});
            if (!Sub.HasValue()) { Revoked=false; break; }
            if (I%2) Api.Call("release",Object{{"handle",Sub.Value()}}); else Api.Close();
            EventEnvelope Event; Event.id_="job.progress";
            Revoked = Revoked && Events->Publish(std::move(Event)).accepted_==0;
        }
        Stop=true; Publisher.join(); QVERIFY(Revoked);
    }
    void workspaceViewerAndEvents() {
        hc::automation::Host Host; int Opened = 0, Closed = 0, Commands = 0; bool Alive = true;
        Host.capture_ = []() -> Result<CommandContext> { return CommandContext{"tab",{LocatorKind::Local,"C:/Temp"},{{{"local"},"text.txt"}}}; };
        Host.script_ = std::make_shared<Services>(); Host.script_->events_ = std::make_shared<EventHub>();
        Host.script_->workspace_ = []() -> Result<ScriptValue> { return ScriptValue(Object{{"panes",ScriptValue::Array{Object{{"active",true}}}}}); };
        Host.script_->viewer_ = [&](const auto&,const auto&) -> Result<ViewerAccess> {
            ++Opened; ViewerAccess Access;
            Access.snapshot_ = [&]() -> Result<ScriptValue> { if (!Alive) return Error{ErrorCategory::StaleRevision,"closed"}; return ScriptValue(Object{{"state","ready"}}); };
            Access.execute_ = [&](std::string_view) -> Result<Unit> { ++Commands; return Unit{}; };
            Access.close_ = [&] { ++Closed; }; return Access;
        };
        CommandScriptApi Api(Host,{"navigation"}), Foreign(Host,{"navigation"}), Denied(Host,{});
        QVERIFY(Api.Call("workspace.snapshot",Object{}).HasValue()); QVERIFY(!Denied.Call("workspace.snapshot",Object{}).HasValue());
        auto Context = Api.Call("capture",Object{}); QVERIFY(Context.HasValue());
        auto Resource = std::get<ScriptValue::Array>(std::get<Object>(Context.Value().data_).at("selection").data_)[0];
        auto Viewer = Api.Call("viewer.open",Object{{"resource",Resource}}); QVERIFY(Viewer.HasValue()); QCOMPARE(Opened,1);
        QVERIFY(Api.Call("viewer.snapshot",Object{{"handle",Viewer.Value()}}).HasValue());
        QVERIFY(!Foreign.Call("viewer.close",Object{{"handle",Viewer.Value()}}).HasValue());
        QVERIFY(!Api.Call("viewer.execute",Object{{"handle",Viewer.Value()},{"command","copy"}}).HasValue());
        QVERIFY(Api.Call("viewer.execute",Object{{"handle",Viewer.Value()},{"command","zoom-in"}}).HasValue()); QCOMPARE(Commands,1);
        Alive = false; QVERIFY(!Api.Call("viewer.snapshot",Object{{"handle",Viewer.Value()}}).HasValue());
        QVERIFY(Api.Call("release",Object{{"handle",Viewer.Value()}}).HasValue()); QCOMPARE(Closed,1);
        QVERIFY(!Api.Call("viewer.snapshot",Object{{"handle",Viewer.Value()}}).HasValue());
        Viewer = Api.Call("viewer.open",Object{{"resource",Resource}}); QVERIFY(Viewer.HasValue());
        auto Sub = Api.Call("events.subscribe",Object{{"event","job.progress"}}); QVERIFY(Sub.HasValue());
        QVERIFY(!Api.Call("events.subscribe",Object{{"event",""}}).HasValue());
        std::thread Producer([&] { for(int I=0;I<10000;++I) { EventEnvelope Event; Event.id_="job.progress"; Event.payload_={{"index",std::int64_t(I)}}; Host.script_->events_->Publish(std::move(Event)); } }); Producer.join();
        auto Batch = Api.Call("events.poll",Object{{"handle",Sub.Value()}}); QVERIFY(Batch.HasValue());
        const auto Data = std::get<Object>(Batch.Value().data_);
        QCOMPARE(std::get<ScriptValue::Array>(Data.at("events").data_).size(),32u); QCOMPARE(std::get<std::int64_t>(Data.at("dropped").data_),9968);
        QVERIFY(!Foreign.Call("events.poll",Object{{"handle",Sub.Value()}}).HasValue());
        QVERIFY(!Api.Call("events.unsubscribe",Object{{"handle",Viewer.Value()}}).HasValue());
        QVERIFY(Api.Call("release",Object{{"handle",Sub.Value()}}).HasValue());
        EventEnvelope Event; Event.id_="job.progress"; QCOMPARE(Host.script_->events_->Publish(Event).accepted_,0u);
        Sub = Api.Call("events.subscribe",Object{{"event","job.progress"}}); QVERIFY(Sub.HasValue());
        for (int I=1;I<16;++I) QVERIFY(Api.Call("events.subscribe",Object{{"event","job.progress"}}).HasValue());
        QVERIFY(!Api.Call("events.subscribe",Object{{"event","job.progress"}}).HasValue());
        Api.Close(); QCOMPARE(Closed,2); QCOMPARE(Host.script_->events_->Publish(Event).accepted_,0u);
    }
    void valuesAndHandles() {
        ScriptValue Tree=Object{{"한글",ScriptValue::Array{true,std::int64_t(INT64_MAX),"value",ScriptValue{}}}};
        QVERIFY(Validate(Tree).HasValue()); QVERIFY(!ToParameters(Tree).HasValue());
        QVERIFY(!Validate(std::numeric_limits<double>::infinity()).HasValue());
        QVERIFY(!Validate(std::string("\xED\xA0\x80")).HasValue());
        QVERIFY(!Validate(std::string(1024*1024+1,'a')).HasValue());
        for(int I=0;I<18;++I) Tree=ScriptValue::Array{std::move(Tree)}; QVERIFY(!Validate(Tree).HasValue());
        HandleTable First,Second; auto Id=First.Insert(HandleKind::Viewer,std::string("viewer")); QVERIFY(Id.HasValue());
        QVERIFY(First.Resolve<std::string>(Id.Value(),HandleKind::Viewer).HasValue());
        QVERIFY(!Second.Resolve<std::string>(Id.Value(),HandleKind::Viewer).HasValue());
        QVERIFY(!First.Resolve<std::string>(Id.Value(),HandleKind::Module).HasValue());
        QVERIFY(First.Release(Id.Value())); auto New=First.Insert(HandleKind::Viewer,std::string("new")); QVERIFY(New.Value()!=Id.Value());
        QVERIFY(!First.Resolve<std::string>(Id.Value(),HandleKind::Viewer).HasValue()); First.Close();
        QVERIFY(!First.Resolve<std::string>(New.Value(),HandleKind::Viewer).HasValue());
        for(int I=0;I<256;++I) QVERIFY(Second.Insert(HandleKind::Subscription,I).HasValue());
        QVERIFY(!Second.Insert(HandleKind::Subscription,257).HasValue());
    }
    void asyncPermissionsAndLifetime() {
        CommandContext Context{"tab",{LocatorKind::Local,"C:/Temp"},{{{"local"},"한글"}}};
        int Calls=0,Polls=0; bool Done=false,Cancelled=false; std::stop_token Token;
        hc::automation::Host Host; Host.capture_=[&]() -> Result<CommandContext> { return Context; };
        Host.schema_=[](const auto& Id) -> Result<CommandDescriptor> { return CommandDescriptor{Id,1,{{"text",{ParameterType::String}}},"file-write"}; };
        Host.dispatch_=[&](const CommandInvocation& Invocation) { ++Calls; Token=Invocation.operation_.stopToken_; return Ready<CommandOutcome>(CommandOutcome{CommandOutcome::Status::Accepted,"job"}); };
        Host.poll_=[&](const auto&,const auto&) -> Result<bool> { ++Polls; return Done; }; Host.cancel_=[&](const auto&,const auto&) { Cancelled=true; };
        Host.output_=[](const auto&,const auto&) -> Result<Parameters> { return Parameters{{"value",std::string("한글")}}; };
        CommandScriptApi Denied(Host,{}),Api(Host,{"file-write"});
        auto Snapshot=Api.Call("capture",Object{}); QVERIFY(Snapshot.HasValue()); const auto ContextId=std::get<Object>(Snapshot.Value().data_).at("context");
        Object Arguments{{"context",ContextId},{"command","test"},{"parameters",Object{{"text","한글"}}}};
        QVERIFY(!Denied.Call("invoke",Arguments).HasValue());
        auto DeniedSnapshot=Denied.Call("capture",Object{}); auto DeniedArgs=Arguments; DeniedArgs["context"]=std::get<Object>(DeniedSnapshot.Value().data_).at("context");
        QCOMPARE(Denied.Call("invoke",DeniedArgs).Failure().category_,ErrorCategory::AccessDenied); QCOMPARE(Calls,0);
        auto Job=Api.Call("invoke",Arguments); QVERIFY(Job.HasValue()); QCOMPARE(Calls,1);
        QCOMPARE(Api.Call("invoke",Arguments).Failure().code_,std::string("script-job-pending"));
        auto Pending=Api.Call("poll",Object{{"handle",Job.Value()}}); QVERIFY(Pending.HasValue()); QVERIFY(!std::get<bool>(std::get<Object>(Pending.Value().data_).at("done").data_));
        Done=true; auto Completed=Api.Call("poll",Object{{"handle",Job.Value()}}); QVERIFY(Completed.HasValue());
        QVERIFY(std::get<bool>(std::get<Object>(Completed.Value().data_).at("done").data_)); QVERIFY(!Cancelled);
        QVERIFY(Api.Call("release",Object{{"handle",Job.Value()}}).HasValue()); QVERIFY(!Api.Call("poll",Object{{"handle",Job.Value()}}).HasValue());
        Done=false; Job=Api.Call("invoke",Arguments); QVERIFY(Job.HasValue()); Api.Close(); QVERIFY(Token.stop_requested()); QVERIFY(Cancelled);
        QVERIFY(!Api.Call("capture",Object{}).HasValue());
        Context.location_.root_="C:/Other"; QCOMPARE(Denied.Call("invoke",DeniedArgs).Failure().category_,ErrorCategory::StaleRevision);
    }
    void ownerAndUnexpectedHostFailure() {
        hc::automation::Host Host; Host.capture_=[]() -> Result<CommandContext> { throw std::runtime_error("test"); };
        CommandScriptApi Api(Host,{}); QCOMPARE(Api.Call("capture",Object{}).Failure().code_,std::string("script-host-exception"));
        std::string Error; std::thread Thread([&] { Error=Api.Call("capture",Object{}).Failure().code_; }); Thread.join(); QCOMPARE(Error,std::string("script-owner-thread"));
    }
};
QTEST_GUILESS_MAIN(ScriptApiTests)
#include "tstScriptApi.moc"
