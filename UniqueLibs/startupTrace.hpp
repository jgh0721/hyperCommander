#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
struct State { std::mutex mutex_; QJsonArray events_; quint64 dropped_=0; bool ready_=false,done_=false; };
inline State& Data() { static State Value; return Value; }
inline void Record(const char* Name,QJsonObject Fields={}) {
    if(!Enabled())return;
    Fields["event"]=QString::fromLatin1(Name);Fields["qpc"]=QString::number(Tick());
    Fields["thread"]=QString::number(GetCurrentThreadId());
    auto& Value=Data();std::lock_guard Lock(Value.mutex_);
    if(Value.events_.size()<20000)Value.events_.append(Fields); else ++Value.dropped_;
}
inline void Flush() {
    if(!Enabled())return;
    QJsonArray Events; quint64 Dropped=0; {auto& Value=Data();std::lock_guard Lock(Value.mutex_);Events=Value.events_;Dropped=Value.dropped_;}
    QSaveFile File(qEnvironmentVariable("HC_STARTUP_TRACE"));
    if(File.open(QIODevice::WriteOnly)){
        File.write(QJsonDocument(QJsonObject{{"schemaVersion",1},{"clock","QueryPerformanceCounter"},
            {"frequency",QString::number(Frequency())},{"pid",int(GetCurrentProcessId())},
            {"displayEvidence","uiPaint-only"},{"droppedEvents",QString::number(Dropped)},{"events",Events}}).toJson());File.commit();
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
    explicit Span(const char* Name):name_(Name){if(Enabled())Record(name_,{{"phase","begin"}});}
    ~Span(){if(Enabled())Record(name_,{{"phase","end"}});}
private:const char* name_;
};
}
