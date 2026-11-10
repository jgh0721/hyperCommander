#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QTimer>

namespace hc::document
{
    /// GUI thread stall and input delay recorder, for diagnosis only. Header-only: the editor and
    /// the preview helpers (hc-markdown-render, HyperEpubWeb) record the same way.
    ///
    ///   ui.stall     The 50 ms heartbeat came at least 100 ms late: the event loop did not run.
    ///   input.delay  A key went down at least 50 ms before this thread took it from the queue
    ///                (MSG::time). Without a ui.stall around it the event loop was running and only
    ///                input was held back: another thread that shares this thread's input queue,
    ///                such as an embedded preview helper, had not taken its own input yet.
    ///
    /// The editor writes to HC_UI_STALL_TRACE=<file>. A helper writes to
    /// <TEMP>/HyperCommander-stall/<role>-<pid>.log when its owner asked for it; no path crosses
    /// into a helper's restricted environment. Key codes are never written.
    class DocumentStallTrace final : public QObject, public QAbstractNativeEventFilter
    {
    public:
        /// The editor process.
        static void Install()
        {
            const auto Path = qEnvironmentVariable("HC_UI_STALL_TRACE");
            if(!Path.isEmpty()) start(Path, QStringLiteral("editor"));
        }
        /// A preview helper. On when the owner passed HC_UI_STALL_TRACE_CHILD=1 into a restricted
        /// environment, or when the editor's variable was inherited.
        static void InstallHelper(const QString& Role)
        {
            if(qEnvironmentVariableIsEmpty("HC_UI_STALL_TRACE_CHILD") && qEnvironmentVariableIsEmpty("HC_UI_STALL_TRACE")) return;
            const QDir Directory(QDir::tempPath() + QStringLiteral("/HyperCommander-stall"));
            if(!Directory.exists() && !QDir().mkpath(Directory.path())) return;
            start(Directory.filePath(QStringLiteral("%1-%2.log").arg(Role).arg(GetCurrentProcessId())), Role);
        }
        bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr*) override
        {
            if(Type != "windows_generic_MSG" && Type != "windows_dispatcher_MSG") return false;
            const auto* Msg = static_cast<const MSG*>(Message);
            if(Msg->message != WM_KEYDOWN && Msg->message != WM_SYSKEYDOWN) return false;
            // Identifies one message across both filter calls without keeping the key itself.
            const quint64 Key = (quint64(Msg->time) << 32) ^ quint64(quintptr(Msg->hwnd)) ^ quint64(Msg->lParam);
            if(Key == lastKey_) return false;
            lastKey_ = Key;
            const DWORD Delay = GetTickCount() - Msg->time;
            if(Delay >= DWORD(InputDelayMs) && Delay < 60000) write(QStringLiteral("input.delay %1ms").arg(Delay));
            return false;
        }

    private:
        static void start(const QString& Path, const QString& Role)
        {
            static bool Installed = false;
            auto* App = QCoreApplication::instance();
            if(!App || Installed) return;
            auto* Trace = new DocumentStallTrace(Path, Role);
            if(!Trace->file_.isOpen()) { delete Trace; return; }
            Installed = true;
            App->installNativeEventFilter(Trace);
        }
        DocumentStallTrace(const QString& Path, const QString& Role) : QObject(QCoreApplication::instance()), file_(Path)
        {
            if(!file_.open(QIODevice::WriteOnly | QIODevice::Append)) return;
            write(QStringLiteral("start role=%1 pid=%2").arg(Role).arg(GetCurrentProcessId()));
            beat_.setTimerType(Qt::PreciseTimer);
            beat_.setInterval(BeatMs);
            connect(&beat_, &QTimer::timeout, this, [this] {
                const auto Gap = last_.restart();
                if(Gap >= BeatMs + StallMs) write(QStringLiteral("ui.stall %1ms").arg(Gap - BeatMs));
            });
            last_.start();
            beat_.start();
        }
        void write(const QString& Line)
        {
            file_.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + QLatin1Char(' ') + Line + QLatin1Char('\n')).toUtf8());
            file_.flush();
        }

        static constexpr int BeatMs = 50, StallMs = 100, InputDelayMs = 50;
        QFile file_;
        QTimer beat_;
        QElapsedTimer last_;
        // Native filters see a message in the dispatcher and again in the window procedure.
        quint64 lastKey_ = 0;
    };
}
