#include "scriptApi.hpp"
#include <cmath>

namespace hc::script {
namespace {
Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest,Code}; }
bool Utf8(std::string_view Text) {
    for(std::size_t Index=0;Index<Text.size();) {
        const auto First=static_cast<unsigned char>(Text[Index++]); if(First<0x80) continue;
        int Count=0; std::uint32_t Code=0,Minimum=0;
        if(First>=0xC2 && First<=0xDF) { Count=1; Code=First&31; Minimum=0x80; }
        else if(First>=0xE0 && First<=0xEF) { Count=2; Code=First&15; Minimum=0x800; }
        else if(First>=0xF0 && First<=0xF4) { Count=3; Code=First&7; Minimum=0x10000; }
        else return false;
        if(Text.size()-Index<static_cast<std::size_t>(Count)) return false;
        while(Count--) { const auto Next=static_cast<unsigned char>(Text[Index++]); if((Next&0xC0)!=0x80) return false; Code=(Code<<6)|(Next&63); }
        if(Code<Minimum || Code>0x10FFFF || (Code>=0xD800 && Code<=0xDFFF)) return false;
    }
    return true;
}
Result<Unit> ValidateNode(const ScriptValue& Value, std::size_t Depth, std::size_t& Nodes, std::size_t& Bytes) {
    if (Depth > 16 || ++Nodes > 4096 || Bytes > 1024*1024) return Error{ErrorCategory::BudgetExceeded,"script-value-limit"};
    if (const auto* Number=std::get_if<double>(&Value.data_); Number && !std::isfinite(*Number)) return Invalid("script-number-finite");
    if (const auto* Text=std::get_if<std::string>(&Value.data_)) { Bytes += Text->size(); if(!Utf8(*Text)) return Invalid("script-string-utf8"); }
    if (const auto* Array=std::get_if<ScriptValue::Array>(&Value.data_)) for(const auto& Child:*Array) {
        auto Result=ValidateNode(Child,Depth+1,Nodes,Bytes); if(!Result.HasValue()) return Result;
    }
    if (const auto* Object=std::get_if<ScriptValue::Object>(&Value.data_)) for(const auto& [Key,Child]:*Object) {
        if(Key.empty() || Key.size()>128 || Key.find('\0')!=std::string::npos || !Utf8(Key)) return Invalid("script-object-key");
        Bytes+=Key.size(); auto Result=ValidateNode(Child,Depth+1,Nodes,Bytes); if(!Result.HasValue()) return Result;
    }
    if(Bytes>1024*1024) return Error{ErrorCategory::BudgetExceeded,"script-value-limit"};
    return Unit{};
}
const std::string* Text(const ScriptValue::Object& Object, const char* Key) {
    const auto It=Object.find(Key); return It==Object.end() ? nullptr : std::get_if<std::string>(&It->second.data_);
}
}
Result<Unit> Validate(const ScriptValue& Value) { std::size_t Nodes=0,Bytes=0; return ValidateNode(Value,0,Nodes,Bytes); }
Result<commands::Parameters> ToParameters(const ScriptValue& Value) {
    auto Valid=Validate(Value); if(!Valid.HasValue()) return Valid.Failure();
    const auto* Object=std::get_if<ScriptValue::Object>(&Value.data_); if(!Object || Object->size()>64) return Invalid("script-parameters");
    commands::Parameters Result;
    for(const auto& [Key,Child]:*Object) {
        if(const auto* Bool=std::get_if<bool>(&Child.data_)) Result[Key]=*Bool;
        else if(const auto* Integer=std::get_if<std::int64_t>(&Child.data_)) Result[Key]=*Integer;
        else if(const auto* String=std::get_if<std::string>(&Child.data_)) {
            if(String->find('\0')!=std::string::npos) return Invalid("script-parameter-nul"); Result[Key]=*String;
        }
        else return Invalid("script-parameter-scalar");
    }
    return Result;
}
ScriptValue FromParameters(const commands::Parameters& Parameters) {
    ScriptValue::Object Result;
    for(const auto& [Key,Value]:Parameters) std::visit([&](const auto& Scalar) { Result.emplace(Key,ScriptValue(Scalar)); },Value);
    return Result;
}
HandleTable::HandleTable() {
    static std::atomic<std::uint64_t> Next{0}; prefix_="h"+std::to_string(++Next)+":";
}
CommandScriptApi::CommandScriptApi(automation::Host Host, std::set<std::string> Permissions)
    : host_(std::move(Host)), permissions_(std::move(Permissions)) {}
CommandScriptApi::~CommandScriptApi() { Close(); }
void CommandScriptApi::cancel(Pending& Pending) {
    if(Pending.terminal_) return;
    Pending.stop_.request_stop();
    try { if(host_.cancel_) host_.cancel_(Pending.invocation_,Pending.outcome_.value_or(commands::CommandOutcome{})); } catch(...) {}
    Pending.terminal_=Error{ErrorCategory::Cancelled,"cancelled"};
}
void CommandScriptApi::Close() {
    if(closed_) return; closed_=true;
    for(auto& [Id,Job]:jobs_) cancel(*Job);
    jobs_.clear(); handles_.Close(); host_={};
}
Result<Unit> CommandScriptApi::checkContext(const storage::CommandContext& Context) {
    if(!host_.capture_) return Invalid("script-host-capture");
    const auto Current=host_.capture_(); if(!Current.HasValue()) return Current.Failure();
    if(Current.Value().tabId_!=Context.tabId_ || Current.Value().location_!=Context.location_)
        return Error{ErrorCategory::StaleRevision,"script-context-changed"};
    return Unit{};
}
Result<ScriptValue> CommandScriptApi::Call(std::string_view Method, const ScriptValue& Arguments) {
    if(thread_!=std::this_thread::get_id()) return Invalid("script-owner-thread");
    if(closed_) return Error{ErrorCategory::Cancelled,"script-session-closed"};
    auto Valid=Validate(Arguments); if(!Valid.HasValue()) return Valid.Failure();
    const auto* Object=std::get_if<ScriptValue::Object>(&Arguments.data_); if(!Object) return Invalid("script-arguments-object");
    try {
        auto Result=call(Method,*Object); if(!Result.HasValue()) return Result;
        Valid=Validate(Result.Value()); if(!Valid.HasValue()) return Valid.Failure(); return Result;
    } catch(...) { return Error{ErrorCategory::Internal,"script-host-exception"}; }
}
Result<ScriptValue> CommandScriptApi::call(std::string_view Method, const ScriptValue::Object& Arguments) {
    if(Method=="capture") {
        if(!Arguments.empty() || !host_.capture_) return Invalid("script-capture-arguments");
        auto Context=host_.capture_(); if(!Context.HasValue()) return Context.Failure();
        if(handles_.Size()+Context.Value().selection_.size()+1>256) return Error{ErrorCategory::BudgetExceeded,"script-handle-limit"};
        auto Handle=handles_.Insert(HandleKind::Context,Context.Value()); if(!Handle.HasValue()) return Handle.Failure();
        ScriptValue::Array Selection;
        for(const auto& Id:Context.Value().selection_) Selection.emplace_back(handles_.Insert(HandleKind::Resource,Resource{Context.Value(),Id}).Value());
        return ScriptValue(ScriptValue::Object{{"context",Handle.Value()},{"path",Context.Value().location_.root_},{"selection",std::move(Selection)}});
    }
    if(Method=="invoke") {
        // The current product host owns one navigation/cancellation binding.
        // Parallel script jobs require the later workspace facade contract.
        for(const auto& [Id,Pending]:jobs_) if(!Pending->terminal_)
            return Error{ErrorCategory::Unavailable,"script-job-pending"};
        const auto* ContextId=Text(Arguments,"context"); const auto* Command=Text(Arguments,"command");
        const auto Params=Arguments.find("parameters");
        if(Arguments.size()!=3 || !ContextId || !Command || Params==Arguments.end() || !host_.schema_ || !host_.dispatch_ || !host_.poll_)
            return Invalid("script-invoke-arguments");
        auto Context=handles_.Resolve<storage::CommandContext>(*ContextId,HandleKind::Context); if(!Context.HasValue()) return Context.Failure();
        auto Current=checkContext(*Context.Value()); if(!Current.HasValue()) return Current.Failure();
        auto Schema=host_.schema_(*Command); if(!Schema.HasValue()) return Schema.Failure();
        auto Parameters=ToParameters(Params->second); if(!Parameters.HasValue()) return Parameters.Failure();
        auto Job=std::make_shared<Pending>(); auto& Invocation=Job->invocation_;
        Invocation.id_=*Command; Invocation.schemaVersion_=Schema.Value().schemaVersion_; Invocation.parameters_=std::move(Parameters.Value());
        Invocation.context_=*Context.Value(); Invocation.permissions_=permissions_; Invocation.source_="script";
        auto Valid=commands::ValidateInvocation(Schema.Value(),Invocation); if(!Valid.HasValue()) return Valid.Failure();
        auto Handle=handles_.Insert(HandleKind::Job,Job); if(!Handle.HasValue()) return Handle.Failure();
        Invocation.correlationId_="script-"+Handle.Value(); Invocation.operation_.stopToken_=Job->stop_.get_token();
        Invocation.operation_.deadline_=std::chrono::steady_clock::now()+std::chrono::minutes(5);
        jobs_.emplace(Handle.Value(),Job);
        try { Job->future_=host_.dispatch_(Invocation); }
        catch(...) { cancel(*Job); handles_.Release(Handle.Value()); jobs_.erase(Handle.Value()); throw; }
        return ScriptValue(Handle.Value());
    }
    const auto* Id=Text(Arguments,"handle"); if(!Id || Arguments.size()!=1) return Invalid("script-handle-arguments");
    if(Method=="release") {
        const auto It=jobs_.find(*Id); if(It!=jobs_.end()) { cancel(*It->second); jobs_.erase(It); }
        if(!handles_.Release(*Id)) return Error{ErrorCategory::StaleRevision,"script-stale-handle"}; return ScriptValue{};
    }
    if(Method=="resource") {
        auto Resource=handles_.Resolve<CommandScriptApi::Resource>(*Id,HandleKind::Resource); if(!Resource.HasValue()) return Resource.Failure();
        auto Current=checkContext(Resource.Value()->context_); if(!Current.HasValue()) return Current.Failure();
        return ScriptValue(ScriptValue::Object{{"provider",Resource.Value()->id_.providerId_.value_},{"key",Resource.Value()->id_.key_}});
    }
    if(Method!="poll" && Method!="cancel") return Error{ErrorCategory::Unsupported,"script-method"};
    auto Job=handles_.Resolve<std::shared_ptr<Pending>>(*Id,HandleKind::Job); if(!Job.HasValue()) return Job.Failure();
    if(Method=="cancel") { cancel(**Job.Value()); return ScriptValue{}; }
    return poll(**Job.Value());
}
Result<ScriptValue> CommandScriptApi::poll(Pending& Pending) {
    if(Pending.terminal_) return *Pending.terminal_;
    if(const auto Error=Pending.invocation_.operation_.Check({})) { cancel(Pending); Pending.terminal_=*Error; return *Pending.terminal_; }
    // Capture checks the production tab's original lifetime. A successful
    // navigation may change its location, so only tab identity is checked here.
    auto Current=host_.capture_();
    if(!Current.HasValue() || Current.Value().tabId_!=Pending.invocation_.context_.tabId_) {
        cancel(Pending); Pending.terminal_=Current.HasValue() ? Error{ErrorCategory::StaleRevision,"script-context-changed"}:Current.Failure(); return *Pending.terminal_;
    }
    if(Pending.future_) {
        if(!Pending.future_->valid()) { Pending.terminal_=Invalid("script-host-future"); return *Pending.terminal_; }
        if(Pending.future_->wait_for(std::chrono::seconds(0))!=std::future_status::ready) return ScriptValue(ScriptValue::Object{{"done",false}});
        auto Outcome=Pending.future_->get(); Pending.future_.reset();
        if(!Outcome.HasValue()) { Pending.terminal_=Outcome.Failure(); return *Pending.terminal_; } Pending.outcome_=Outcome.Value();
    }
    auto Done=host_.poll_(Pending.invocation_,*Pending.outcome_);
    if(!Done.HasValue()) { cancel(Pending); Pending.terminal_=Done.Failure(); return *Pending.terminal_; }
    if(!Done.Value()) return ScriptValue(ScriptValue::Object{{"done",false}});
    auto Output=host_.output_ ? host_.output_(Pending.invocation_,*Pending.outcome_) : Result<commands::Parameters>(commands::Parameters{});
    if(!Output.HasValue()) { Pending.terminal_=Output.Failure(); return *Pending.terminal_; }
    Output.Value()["jobId"]=Pending.outcome_->jobId_;
    auto Result=ScriptValue(ScriptValue::Object{{"done",true},{"output",FromParameters(Output.Value())}});
    auto Valid=Validate(Result); Pending.terminal_=Valid.HasValue() ? hc::script::Result<ScriptValue>(std::move(Result)) : hc::script::Result<ScriptValue>(Valid.Failure());
    return *Pending.terminal_;
}
}
