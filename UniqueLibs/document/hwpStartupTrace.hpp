#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <limits>

namespace hc::document::startup
{
    // Diagnostic data never grants admission, Ready, capabilities or retirement.
    // Native clocks are local to this process; renderer performance.now is a
    // separate domain. No file I/O occurs in Record/Begin/End or renderer intake.
    class Recorder final
    {
    public:
        static constexpr int Capacity = 192;
        explicit Recorder(bool Enabled) : enabled_(Enabled), epoch_(Clock::now()) {}
        bool Enabled() const { return enabled_; }
        double Now() const { return enabled_ ? std::chrono::duration<double, std::milli>(Clock::now()-epoch_).count() : 0; }
        void SetIdentity(QString Run, QString Role)
        { std::lock_guard Lock(mutex_); run_ = std::move(Run); role_ = std::move(Role); }
        void SetRuntimeHashBackend(bool Cng)
        { if(enabled_) { std::lock_guard Lock(mutex_); runtimeHashBackend_=Cng?"cng-sha256":"qt-sha256"; } }
        void SetBinding(QJsonObject Binding)
        { if(enabled_) { std::lock_guard Lock(mutex_); if(binding_.isEmpty()) binding_ = std::move(Binding); } }
        int Begin(const char* Name)
        {
            if(!enabled_) return -1;
            const auto At = Now(); std::lock_guard Lock(mutex_);
            if(count_ == Capacity) { if(dropped_<std::numeric_limits<int>::max())++dropped_; return -1; }
            const auto Index = count_++;
            rows_[Index] = {Name, At, -1, -1, -1, 0, quintptr(QThread::currentThreadId()), false};
            return Index;
        }
        void End(int Index, qint64 Bytes = -1, qint64 Files = -1)
        {
            if(!enabled_ || Index < 0) return;
            const auto At = Now(); std::lock_guard Lock(mutex_);
            if(Index >= count_ || rows_[Index].end_ >= 0) return;
            rows_[Index].end_ = At; rows_[Index].bytes_ = Bytes; rows_[Index].files_ = Files;
        }
        void EndNamed(const char* Name)
        {
            if(!enabled_) return;
            const auto At = Now(); std::lock_guard Lock(mutex_);
            for(int Index = count_-1; Index >= 0; --Index) {
                auto& Row = rows_[Index];
                if(!qstrcmp(Row.name_, Name) && Row.end_ < 0) { Row.end_ = At; return; }
            }
        }
        void Aggregate(const char* Name, double Duration, qint64 Bytes = -1, qint64 Files = -1)
        {
            if(!enabled_) return;
            std::lock_guard Lock(mutex_);
            if(count_ == Capacity) { if(dropped_<std::numeric_limits<int>::max())++dropped_; return; }
            rows_[count_++] = {Name, 0, Duration, Bytes, Files, Files, quintptr(QThread::currentThreadId()), true};
        }
        bool AcceptRenderer(const QJsonArray& Spans)
        {
            if(!enabled_ || Spans.isEmpty() || Spans.size() > 32) return false;
            static const QStringList Names{"js.inputTransfer","js.inputHash","js.wasmInit","js.parseLayout",
                "js.preflight","js.editableSetup","js.documentInfo","js.reservePages","js.page0Render","js.frameWait","js.firstFrameAck",
                "js.parsedAck","js.editorLoad","js.studioInitialize","js.connectEditor","js.moduleToAdapter"};
            for(const auto& Value : Spans) {
                if(!Value.isObject()) return false; const auto Span = Value.toObject();
                if(Span.size() != 5 || !Names.contains(Span["name"].toString())
                    || (Span["kind"] != "span" && Span["kind"] != "aggregate")) return false;
                for(const auto* Field : {"startMs","durationMs","count"}) {
                    const auto Number = Span[Field].toDouble(-1);
                    if(!Span[Field].isDouble() || !std::isfinite(Number) || Number < 0 || Number > 120000) return false;
                }
                if(Span["count"].toDouble() < 1 || std::floor(Span["count"].toDouble()) != Span["count"].toDouble()) return false;
            }
            std::lock_guard Lock(mutex_);
            if(!renderer_.isEmpty()) return false;
            renderer_ = Spans; return true;
        }
        void MissingHelper(QString Reason)
        {
            if(!enabled_)return;std::lock_guard Lock(mutex_);
            missingReason_=Reason.size()<=128?std::move(Reason):QString("diagnostic-reason-budget");
        }
        QJsonObject Snapshot() const
        {
            if(!enabled_) return {};
            std::lock_guard Lock(mutex_); QJsonArray Rows;
            for(int Index = 0; Index < count_; ++Index) {
                const auto& Row = rows_[Index];
                QJsonObject Value{{"name",Row.name_},{"kind",Row.aggregate_?"aggregate":"span"},
                    {"thread",QString::number(Row.thread_)}};
                Value["bytes"]=Row.bytes_>=0?QJsonValue(double(Row.bytes_)):QJsonValue(QJsonValue::Null);
                Value["files"]=Row.files_>=0?QJsonValue(double(Row.files_)):QJsonValue(QJsonValue::Null);
                if(!Row.aggregate_) Value["startMs"] = Row.start_;
                if(Row.end_ >= 0) Value["durationMs"] = Row.aggregate_?Row.end_:Row.end_-Row.start_;
                else Value["open"] = true;
                Rows.append(Value);
            }
            return {{"schemaVersion",1},{"runId",run_},{"role",role_},{"pid",double(QCoreApplication::applicationPid())},
                {"clockDomain","native-process-steady"},{"runtimeHashBackend",runtimeHashBackend_},{"binding",binding_},{"capacity",Capacity},{"dropped",dropped_},
                {"missingHelperReason",missingReason_},{"events",Rows},{"rendererClockDomain","renderer-performance"},{"renderer",renderer_},{"rendererReceived",!renderer_.isEmpty()}};
        }
    private:
        using Clock = std::chrono::steady_clock;
        struct Row { const char* name_ = nullptr; double start_ = 0, end_ = -1;
            qint64 bytes_ = -1, files_ = -1, count_ = 0; quintptr thread_ = 0; bool aggregate_ = false; };
        const bool enabled_;
        const Clock::time_point epoch_;
        mutable std::mutex mutex_;
        std::array<Row,Capacity> rows_{};
        int count_ = 0, dropped_ = 0;
        QString run_, role_, missingReason_, runtimeHashBackend_;
        QJsonObject binding_;
        QJsonArray renderer_;
    };
    inline bool ValidRun(const QString& Run)
    {
        if(Run.size() != 32) return false;
        for(const auto Char : Run) if(!((Char >= '0' && Char <= '9') || (Char >= 'a' && Char <= 'f'))) return false;
        return true;
    }
    inline bool Configured()
    {
        if(!ValidRun(qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_ID"))) return false;
        const auto Root = qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_DIR");
        return qgetenv("HC_RHWP_STARTUP_TRACE_CHILD") == "1"
            || (!Root.isEmpty() && QFileInfo(Root).isAbsolute() && QFileInfo(Root).isDir());
    }
    inline Recorder& Data() { static Recorder Value(Configured()); return Value; }
    inline bool Enabled() { return Data().Enabled(); }
    inline void Initialize(const QString& Role)
    { if(Enabled()) Data().SetIdentity(qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_ID"),Role); }
    class Scope final
    {
    public:
        explicit Scope(const char* Name) : index_(Data().Begin(Name)) {}
        ~Scope() { End(); }
        void End(qint64 Bytes = -1, qint64 Files = -1) { Data().End(std::exchange(index_,-1),Bytes,Files); }
    private:
        int index_;
    };
    inline bool Write(const QJsonObject& Value, const QString& Role, qint64 Pid)
    {
        if(!Enabled() || !ValidRun(Value["runId"].toString()) || Pid < 1 || (Role != "editor" && Role != "helper")) return false;
        const auto Root = qEnvironmentVariable("HC_RHWP_STARTUP_TRACE_DIR");
        if(Root.isEmpty() || !QFileInfo(Root).isAbsolute() || !QFileInfo(Root).isDir()) return false;
        auto Bytes = QJsonDocument(Value).toJson(QJsonDocument::Indented);
        Bytes.replace("\n","\r\n");
        if(Bytes.size() > 64*1024) return false;
        QSaveFile File(QDir(Root).filePath("startup-"+Value["runId"].toString()+"-"+Role+"-"+QString::number(Pid)+".json"));
        return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
    }
}
