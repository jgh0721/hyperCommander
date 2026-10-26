#pragma once
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <mutex>
#include <Windows.h>

namespace hc::startup {
inline bool Enabled() { static const bool Value=!qEnvironmentVariableIsEmpty("HC_STARTUP_TRACE"); return Value; }
inline bool Detail() { static const bool Value=qEnvironmentVariableIntValue("HC_STARTUP_DETAIL")!=0; return Value; }
inline qint64 Tick() { LARGE_INTEGER Value; QueryPerformanceCounter(&Value); return Value.QuadPart; }
inline qint64 Frequency() { LARGE_INTEGER Value; QueryPerformanceFrequency(&Value); return Value.QuadPart; }
struct State { std::mutex mutex_; QJsonArray events_; bool ready_=false,done_=false; };
inline State& Data() { static State Value; return Value; }
inline void Record(const char* Name,QJsonObject Fields={}) {
    if(!Enabled())return;
    Fields["event"]=QString::fromLatin1(Name);Fields["qpc"]=QString::number(Tick());
    Fields["thread"]=QString::number(GetCurrentThreadId());
    auto& Value=Data();std::lock_guard Lock(Value.mutex_);
    if(Value.events_.size()<20000)Value.events_.append(Fields);
}
inline void Flush() {
    if(!Enabled())return;
    QJsonArray Events;{auto& Value=Data();std::lock_guard Lock(Value.mutex_);Events=Value.events_;}
    QSaveFile File(qEnvironmentVariable("HC_STARTUP_TRACE"));
    if(File.open(QIODevice::WriteOnly)){
        File.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"clock","QueryPerformanceCounter"},
            {"frequency",QString::number(Frequency())},{"pid",int(GetCurrentProcessId())},
            {"displayEvidence","uiPaint-only"},{"events",Events}}).toJson());File.commit();
    }
}
inline void Signal(const char* Variable) {
    const auto Name=qEnvironmentVariable(Variable);if(Name.isEmpty())return;
    if(const auto Handle=OpenEventW(EVENT_MODIFY_STATE,FALSE,reinterpret_cast<LPCWSTR>(Name.utf16()))){SetEvent(Handle);CloseHandle(Handle);}
}
inline void Ready(QJsonObject Fields) {
    if(!Enabled()||Data().ready_)return;Data().ready_=true;Record("ready.uiPaint",Fields);Signal("HC_STARTUP_READY_EVENT");
}
inline void Used(QJsonObject Fields) {
    if(!Enabled()||Data().done_)return;Data().done_=true;Record("used.uiPaint",Fields);Signal("HC_STARTUP_DONE_EVENT");
}
class Span {
public:
    explicit Span(const char* Name):name_(Name){Record(name_,{{"phase","begin"}});}
    ~Span(){Record(name_,{{"phase","end"}});}
private:const char* name_;
};
}
