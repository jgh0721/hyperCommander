#include "stdafx.h"
#include "markdownProcessView.hpp"
#include <QLocalSocket>
#include <QProcess>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QUuid>
#include <QResizeEvent>
#include <QApplication>
#include <qt_windows.h>

namespace hc::document
{
MarkdownProcessView::MarkdownProcessView(QWidget *Parent) : QWidget(Parent)
{
    setObjectName("markdownPreview");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NativeWindow);
    startup_.setSingleShot(true);
    startup_.setInterval(15000);
    connect(&startup_, &QTimer::timeout, this, [this] {
        Stop();
        emit Reported(
            {{"type", "error"}, {"message", tr("미리보기 시작 시간이 초과되었습니다. 새로고침으로 다시 시도하세요.")}});
    });
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        auto *Incoming = server_.nextPendingConnection();
        if (socket_)
        {
            Incoming->deleteLater();
            return;
        }
        socket_ = Incoming;
        connect(Incoming, &QLocalSocket::readyRead, this, &MarkdownProcessView::receive);
    });
}
MarkdownProcessView::~MarkdownProcessView()
{
    Stop();
}
void MarkdownProcessView::start()
{
    if (process_)
        return;
    stopping_ = false;
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen("hc-markdown-" + token_))
    {
        emit Reported({{"type", "error"}, {"message", server_.errorString()}});
        return;
    }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
    {
        Stop();
        emit Reported({{"type", "error"}, {"message", tr("미리보기 프로세스 보호를 설정할 수 없습니다.")}});
        return;
    }
    auto *Process = new QProcess(qApp);
    process_ = Process;
    connect(Process, &QProcess::finished, Process, &QObject::deleteLater);
    connect(Process, &QProcess::started, this, [this, Process] {
        const auto Handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(Process->processId()));
        const bool Ok = Handle && AssignProcessToJobObject(job_, Handle);
        if (Handle)
            CloseHandle(Handle);
        if (!Ok)
        {
            Stop();
            emit Reported({{"type", "error"}, {"message", tr("미리보기 프로세스를 격리할 수 없습니다.")}});
        }
    });
    connect(Process, &QProcess::errorOccurred, this, [this, Process] {
        emit Reported({{"type", "error"}, {"message", Process->errorString()}});
        Stop();
    });
    connect(Process, &QProcess::finished, this, [this] {
        if (!stopping_)
            emit Reported(
                {{"type", "error"}, {"message", tr("미리보기가 종료되었습니다. 새로고침으로 다시 시도하세요.")}});
        Stop();
    });
    Process->setProgram(QCoreApplication::applicationDirPath() + "/Libs/Markdown/hc-markdown-render.exe");
    Process->setArguments({server_.serverName(), token_});
    Process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    startup_.start();
    Process->start();
}
void MarkdownProcessView::Stop()
{
    stopping_ = true;
    startup_.stop();
    ready_ = false;
    child_ = 0;
    input_.clear();
    server_.close();
    if (socket_)
    {
        disconnect(socket_, nullptr, this, nullptr);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (process_)
    {
        auto *Process = process_.data();
        disconnect(Process, nullptr, this, nullptr);
        if (Process->state() != QProcess::NotRunning)
            Process->kill();
        else
            Process->deleteLater();
        process_ = nullptr;
    }
    if (job_)
    {
        CloseHandle(job_);
        job_ = nullptr;
    }
}
void MarkdownProcessView::Restart()
{
    Stop();
    start();
}
void MarkdownProcessView::Send(const QJsonObject &Message)
{
    if (Message["type"] == "render")
        pending_ = Message;
    if (Message["type"] == "shortcuts")
        shortcuts_ = Message;
    if (!process_)
        start();
    if (ready_ && socket_ && socket_->bytesToWrite() < 32 * 1024 * 1024)
        socket_->write(QJsonDocument(Message).toJson(QJsonDocument::Compact) + '\n');
}
void MarkdownProcessView::receive()
{
    input_ += socket_->readAll();
    if (input_.size() > 4 * 1024 * 1024)
    {
        Stop();
        return;
    }
    while (input_.contains('\n'))
    {
        const auto End = input_.indexOf('\n');
        const auto Message = QJsonDocument::fromJson(input_.left(End)).object();
        input_.remove(0, End + 1);
        if (!ready_)
        {
            if (Message["type"] != "ready" || Message["token"].toString() != token_ || !process_)
            {
                Stop();
                return;
            }
            const auto Window = reinterpret_cast<HWND>(Message["window"].toString().toULongLong());
            DWORD Pid = 0;
            GetWindowThreadProcessId(Window, &Pid);
            if (!Window || Pid != DWORD(process_->processId()))
            {
                Stop();
                return;
            }
            SetWindowLongPtrW(Window, GWL_STYLE, (GetWindowLongPtrW(Window, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
            SetLastError(0);
            const auto Old = SetParent(Window, reinterpret_cast<HWND>(winId()));
            if (!Old && GetLastError() != 0)
            {
                Stop();
                return;
            }
            child_ = quintptr(Window);
            ready_ = true;
            startup_.stop();
            resizeChild();
            ShowWindow(Window, SW_SHOWNOACTIVATE);
            Send({{"type", "attached"}});
            if (!pending_.isEmpty())
                Send(pending_);
            if (!shortcuts_.isEmpty())
                Send(shortcuts_);
        }
        else
            emit Reported(Message);
    }
}
void MarkdownProcessView::resizeChild()
{
    if (child_)
    {
        RECT R{};
        if (GetClientRect(reinterpret_cast<HWND>(winId()), &R))
            SetWindowPos(reinterpret_cast<HWND>(child_), nullptr, 0, 0, R.right, R.bottom,
                         SWP_NOACTIVATE | SWP_NOZORDER);
    }
}
void MarkdownProcessView::resizeEvent(QResizeEvent *Event)
{
    QWidget::resizeEvent(Event);
    resizeChild();
}
bool MarkdownProcessView::HasFocus() const
{
    GUITHREADINFO Info{sizeof(GUITHREADINFO)};
    const auto Window = reinterpret_cast<HWND>(child_);
    return Window && GetGUIThreadInfo(0, &Info) && (Info.hwndFocus == Window || IsChild(Window, Info.hwndFocus));
}
void MarkdownProcessView::focusInEvent(QFocusEvent *Event)
{
    QWidget::focusInEvent(Event);
    if (child_)
        SetFocus(reinterpret_cast<HWND>(child_));
    Send({{"type", "focus"}});
}
} // namespace hc::document
