#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QCoreApplication>
#include <chrono>
#include <mutex>
namespace hc::document
{
    // Opt-in diagnostics. Nested spans and worker spans may overlap; consumers
    // must use their timestamps rather than adding durations to infer first paint.
    class ImageTraceSpan
    {
      public:
        explicit ImageTraceSpan(const char* Stage,QString Source={}) : stage_(Stage),source_(std::move(Source))
        {
            if(Enabled()){start_=std::chrono::steady_clock::now();timer_.start();}
        }
        ~ImageTraceSpan()
        {
            if(!timer_.isValid())return;
            const double Duration=timer_.nsecsElapsed()/1e6;
            static std::mutex Mutex;std::lock_guard Guard(Mutex);
            QFile File(Path());if(!File.open(QIODevice::WriteOnly|QIODevice::Append))return;
            const auto Start=std::chrono::duration_cast<std::chrono::microseconds>(start_.time_since_epoch()).count();
            const QJsonObject Row{{"stage",QString::fromLatin1(stage_)},{"source",source_},{"start_us",double(Start)},
                {"bytes",double(bytes_)},{"process",QString::number(QCoreApplication::applicationPid())},
                {"duration_ms",Duration},{"thread",QString::number(quintptr(QThread::currentThreadId()))}};
            File.write(QJsonDocument(Row).toJson(QJsonDocument::Compact)+"\n");
        }
        static bool Enabled(){return !Path().isEmpty();}
        void AddBytes(qint64 Bytes){bytes_+=Bytes;}
      private:
        static const QString& Path(){static const QString Value=qEnvironmentVariable("HC_DOCUMENT_TRACE",qEnvironmentVariable("HC_IMAGE_TRACE"));return Value;}
        qint64 bytes_=0;
        const char* stage_;
        QString source_;
        QElapsedTimer timer_;
        std::chrono::steady_clock::time_point start_;
    };
}
