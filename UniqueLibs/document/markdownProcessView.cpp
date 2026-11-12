#include "stdafx.h"
#include "markdownProcessView.hpp"
#include "documentWebProcess.hpp"
#include "HyperCommanderUpdater/updatePromotion.hpp"
#include "HyperCommanderUpdater/updateTransaction.hpp"
#include <QFileInfo>
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
MarkdownProcessView::MarkdownProcessView(QWidget *Parent)
    // A restarted helper starts at 100 %; the owner's zoom and menu coordinates assume the last value.
    : MarkdownProcessView(QStringLiteral("hc-markdown-render"), {"render", "shortcuts", "zoom"}, Parent)
{
}
MarkdownProcessView::MarkdownProcessView(const QString &Helper, const QStringList &Replayed, QWidget *Parent)
    : QWidget(Parent), helper_(Helper), replayed_(Replayed)
{
    setObjectName("markdownPreview");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NativeWindow);
    // The owner draws the preview menu; nothing falls through to the window's menus.
    setContextMenuPolicy(Qt::CustomContextMenu);
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
    if (process_ || missing_)
        return;
    stopping_ = false;
    // Updaters older than the helper deliver it under Libs/RootFiles, as does a new portable ZIP;
    // HyperCommander copies it beside itself shortly after it starts. HyperEditor can open a
    // document first, so make the same verified copy here. The carried copy is never run: its
    // Qt runtime is the one beside the program.
    const auto Program = QCoreApplication::applicationDirPath() + "/" + helper_ + ".exe";
    if (!QFileInfo::exists(Program))
    {
        const auto Result = hc::update::PromoteRootFiles(hc::update::ExecutableRoot());
        if (!QFileInfo::exists(Program))
        {
            missing_ = true;
            const auto Name = helper_ + ".exe";
            const auto Reason = Result.status_ == hc::update::PromotionStatus::Busy
                ? tr("다른 미리보기나 HyperCompare가 실행 중이라 %1을 설치 폴더로 옮기지 못했습니다. 그 창을 닫은 뒤 다시 시도하세요.").arg(Name)
                : Result.win32_ == ERROR_ACCESS_DENIED
                ? tr("설치 폴더에 쓸 권한이 없어 %1을 옮기지 못했습니다. HyperCommander를 실행하면 관리자 권한으로 옮길 수 있습니다.").arg(Name)
                : QFileInfo::exists(QCoreApplication::applicationDirPath() + "/Libs/RootFiles/" + Name)
                ? tr("미리보기 프로그램 %1이 설치 폴더에 없습니다. HyperCommander를 한 번 실행하거나 Libs\\RootFiles의 %1을 설치 폴더로 복사한 뒤 다시 시도하세요.").arg(Name)
                : tr("미리보기 프로그램 %1이 설치 폴더에 없습니다. 최신 Portable ZIP으로 다시 설치하세요.").arg(Name);
            emit Reported({{"type", "error"}, {"message", Result.error_.empty() ? Reason : Reason + "\n" + QString::fromStdString(Result.error_)}});
            return;
        }
    }
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(helper_ + "-" + token_))
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
    Process->setProgram(Program);
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
    missing_ = false;
    Stop();
    start();
}
void MarkdownProcessView::Send(const QJsonObject &Message)
{
    if (const auto Type = Message["type"].toString(); replayed_.contains(Type))
        replay_[Type] = Message;
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
            resizeChild(true);
            ShowWindow(Window, SW_SHOWNOACTIVATE);
            Send({{"type", "attached"}});
            // A pending request remains valid only while the preview still owns Qt focus.
            const auto Root = GetAncestor(reinterpret_cast<HWND>(window()->effectiveWinId()), GA_ROOT);
            if (focusRequested_ && hasFocus() && isVisible() && Root && GetAncestor(GetForegroundWindow(), GA_ROOT) == Root
                && !QApplication::activeModalWidget() && !QApplication::activePopupWidget())
            {
                SetFocus(Window);
                Send({{"type", "focus"}});
            }
            for (const auto &Type : std::as_const(replayed_))
                if (const auto Kept = replay_.value(Type); !Kept.isEmpty())
                    Send(Kept);
        }
        else
            emit Reported(Message);
    }
}
void MarkdownProcessView::resizeChild(bool Now)
{
    if (!child_)
        return;
    const auto Child = reinterpret_cast<HWND>(child_);
    // The preview window belongs to the helper's GUI thread, and Windows joins that thread's
    // input queue with this one's. SetWindowPos on it waits until the helper has handled the
    // move; while the helper renders, keys typed in the source wait with it.
    static const auto Message = RegisterWindowMessageW(WebSurfaceResizeMessage);
    if (!Now && Message && PostMessageW(Child, Message, 0, 0))
        return;
    RECT R{};
    if (GetClientRect(reinterpret_cast<HWND>(winId()), &R))
        SetWindowPos(Child, nullptr, 0, 0, R.right, R.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
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
void MarkdownProcessView::FocusInput()
{
    focusRequested_ = true;
    setFocus(Qt::OtherFocusReason);
    if (ready_ && isVisible()) Send({{"type", "focus"}});
}
void MarkdownProcessView::focusInEvent(QFocusEvent *Event)
{
    QWidget::focusInEvent(Event);
    focusRequested_ = true;
    if (ready_) Send({{"type", "focus"}});
}
} // namespace hc::document
