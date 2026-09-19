#include "commonLibs/scripting/scriptApi.hpp"
#include <QtTest>

using namespace hc::script;
using namespace hc::storage;
using namespace hc::commands;
using Object=ScriptValue::Object;
class ScriptApiTests : public QObject {
    Q_OBJECT
private slots:
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
