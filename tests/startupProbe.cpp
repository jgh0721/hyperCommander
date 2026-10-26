#include "stdafx.h"
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QPixmap>
#include <QImage>
#include <QThread>
#include <QUuid>
#include <dwmapi.h>
#include "../UniqueLibs/startupTrace.hpp"

namespace {
QJsonObject CompositionEvidence(HWND Window)
{
    QJsonObject Result{{"scope", "desktop-compositor-only"}, {"frameCorrelation", false}};
    const auto Library = LoadLibraryW(L"dwmapi.dll");
    if (!Library) return Result;
    const auto Flush = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(Library, "DwmFlush"));
    const auto Timing = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*)>(GetProcAddress(Library, "DwmGetCompositionTimingInfo"));
    if (Flush) { Result["flushHresult"] = qint64(Flush()); Result["qpcAfterFlush"] = QString::number(hc::startup::Tick()); }
    if (Timing) {
        DWM_TIMING_INFO Info{}; Info.cbSize = sizeof(Info);
        Result["windowTimingHresult"] = qint64(Timing(Window, &Info));
        Info = {}; Info.cbSize = sizeof(Info);
        Result["desktopTimingHresult"] = qint64(Timing(nullptr, &Info));
        Result["qpcVblank"] = QString::number(Info.qpcVBlank);
        Result["composedFrames"] = QString::number(Info.cFrame);
    }
    FreeLibrary(Library); return Result;
}
QJsonObject CaptureDisplayedClient(HWND Window, const QString& Path)
{
    QJsonObject Result{{"source", "desktop-screen-dc"}, {"exactPresentationTimestamp", false}};
    RECT Rect{}; POINT Origin{};
    if (!GetClientRect(Window, &Rect) || !ClientToScreen(Window, &Origin)) return Result;
    const int Width = Rect.right, Height = Rect.bottom;
    if (Width <= 0 || Height <= 0 || qint64(Width) * Height > 33554432) return Result;
    const auto Screen = GetDC(nullptr), Memory = CreateCompatibleDC(Screen);
    BITMAPINFO Info{}; Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    Info.bmiHeader.biWidth = Width; Info.bmiHeader.biHeight = -Height;
    Info.bmiHeader.biPlanes = 1; Info.bmiHeader.biBitCount = 32; Info.bmiHeader.biCompression = BI_RGB;
    void* Pixels = nullptr; const auto Bitmap = CreateDIBSection(Screen, &Info, DIB_RGB_COLORS, &Pixels, nullptr, 0);
    if (Screen && Memory && Bitmap && Pixels) {
        const auto Previous = SelectObject(Memory, Bitmap);
        Result["qpcBeforeCapture"] = QString::number(hc::startup::Tick());
        const bool Captured = BitBlt(Memory, 0, 0, Width, Height, Screen, Origin.x, Origin.y, SRCCOPY | CAPTUREBLT);
        Result["qpcAfterCapture"] = QString::number(hc::startup::Tick());
        Result["saved"] = Captured && QImage(static_cast<const uchar*>(Pixels), Width, Height, QImage::Format_RGB32).save(Path);
        Result["path"] = Path; Result["width"] = Width; Result["height"] = Height;
        SelectObject(Memory, Previous);
    }
    if (Bitmap) DeleteObject(Bitmap); if (Memory) DeleteDC(Memory); if (Screen) ReleaseDC(nullptr, Screen);
    return Result;
}
// Acquire foreground permission before the measured process is created.
// Only click our own verified window; never send input to another application.
bool PrepareForeground()
{
    WNDCLASSW Class{}; Class.lpfnWndProc = DefWindowProcW; Class.hInstance = GetModuleHandleW(nullptr);
    Class.lpszClassName = L"HCStartupProbeWindow"; Class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&Class);
    const auto Host = CreateWindowExW(WS_EX_TOPMOST, Class.lpszClassName, L"HyperCommander startup measurement",
        WS_POPUP | WS_VISIBLE, 40, 40, 360, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!Host) { fprintf(stderr, "Create foreground host failed: %lu\n", GetLastError()); return false; }
    SetForegroundWindow(Host);
    POINT Previous{}; GetCursorPos(&Previous);
    if (GetForegroundWindow() != Host) {
        RECT Bounds{}; GetWindowRect(Host, &Bounds);
        const POINT Point{Bounds.left + 40, Bounds.top + 40};
        if (GetAncestor(WindowFromPoint(Point), GA_ROOT) == Host && SetCursorPos(Point.x, Point.y)) {
            INPUT Click[2]{}; Click[0].type = Click[1].type = INPUT_MOUSE;
            Click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; Click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, Click, sizeof(INPUT));
            for (int Attempt = 0; Attempt < 100 && GetForegroundWindow() != Host; ++Attempt) {
                QCoreApplication::processEvents(); QThread::msleep(5);
            }
            SetCursorPos(Previous.x, Previous.y);
        }
    }
    const bool Granted = GetForegroundWindow() == Host;
    // Keep the owned foreground window alive through CreateProcess.
    if (Granted) SetWindowLongPtrW(Host, GWLP_USERDATA, 1);
    else DestroyWindow(Host);
    return Granted;
}
void CloseForegroundHost()
{
    EnumThreadWindows(GetCurrentThreadId(), [](HWND Window, LPARAM)->BOOL {
        if (GetWindowLongPtrW(Window, GWLP_USERDATA) == 1) DestroyWindow(Window);
        return TRUE;
    }, 0);
}
bool CopyConfiguration(const QString& Source, const QString& Target)
{
    if (!QDir(Source).exists() || !QDir().mkpath(Target)) return false;
    QDirIterator Files(Source, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (Files.hasNext()) {
        const auto From = Files.next(); const auto To = QDir(Target).absoluteFilePath(QDir(Source).relativeFilePath(From));
        if (!QDir().mkpath(QFileInfo(To).absolutePath()) || !QFile::copy(From, To)) return false;
    }
    return true;
}
HWND WindowFor(DWORD Pid) {
    struct Search { DWORD pid_; HWND window_=nullptr; LONG area_=0; } Search{Pid};
    EnumWindows([](HWND Window,LPARAM Parameter)->BOOL{
        auto& Value=*reinterpret_cast<struct Search*>(Parameter);DWORD Pid=0;GetWindowThreadProcessId(Window,&Pid);
        if(Pid==Value.pid_&&IsWindowVisible(Window)&&!GetWindow(Window,GW_OWNER)){RECT Rect{};GetWindowRect(Window,&Rect);const LONG Area=(Rect.right-Rect.left)*(Rect.bottom-Rect.top);if(Area>Value.area_){Value.window_=Window;Value.area_=Area;}}return TRUE;
    },reinterpret_cast<LPARAM>(&Search));return Search.window_;
}
}
int main(int argc,char** argv)
{
    QGuiApplication App(argc,argv);const auto Args=App.arguments();
    if(Args.size()<4){qCritical("Usage: hc_startup_probe <exe> <config directory> <output directory> [runs=1] [path|-] [up|down] [detail]");return 1;}
    const int Runs=Args.size()>4?qBound(1,Args[4].toInt(),100):1;QDir().mkpath(Args[3]);QJsonArray Results;
    const auto Template = QDir(QFileInfo(Args[2]).absoluteFilePath()).canonicalPath();
    const auto OutputRoot = QDir(QFileInfo(Args[3]).absoluteFilePath()).canonicalPath();
    if (Template.isEmpty() || OutputRoot.isEmpty() || OutputRoot.compare(Template,Qt::CaseInsensitive)==0
        || OutputRoot.startsWith(Template+'/',Qt::CaseInsensitive)) { qCritical("Output must be outside the configuration template"); return 1; }
    QFile TemplateFile(QDir(Template).filePath("HyperCommander.json")); QByteArray TemplateHash;
    if (TemplateFile.open(QIODevice::ReadOnly)) TemplateHash=QCryptographicHash::hash(TemplateFile.readAll(),QCryptographicHash::Sha256).toHex();
    QString ExpectedFocus;
    if (TemplateFile.isOpen()) {
        TemplateFile.seek(0); const auto Settings = QJsonDocument::fromJson(TemplateFile.readAll()).object();
        if (Settings["general"].toObject()["restoreWorkspace"].toBool(true)) {
            const auto Workspace = Settings["workspace"].toObject();
            const auto Pane = Workspace[Workspace["activePane"].toInt() == 1 ? "right" : "left"].toObject();
            const auto Tabs = Pane["tabs"].toArray(); const auto Active = Pane["active"].toInt();
            if (Active >= 0 && Active < Tabs.size()) ExpectedFocus = Tabs[Active].toObject()["focus"].toString();
        }
    }
    bool All=true;
    for(int Run=0;Run<Runs;++Run){
        const auto Id=QUuid::createUuid().toString(QUuid::Id128),Prefix="Local\\HCStartup-"+Id;
        // Each run starts from exactly the same workspace, including its
        // cursor, and never writes to the supplied profile directory.
        const auto Config = QDir(OutputRoot).absoluteFilePath(QString("config-%1-%2").arg(Run).arg(Id));
        if (!CopyConfiguration(Template, Config)) { qCritical("Cannot copy configuration template"); return 1; }
        const auto ReadyName=Prefix+"-ready",DoneName=Prefix+"-done";
        HANDLE Ready=CreateEventW(nullptr,TRUE,FALSE,reinterpret_cast<LPCWSTR>(ReadyName.utf16()));
        HANDLE Done=CreateEventW(nullptr,TRUE,FALSE,reinterpret_cast<LPCWSTR>(DoneName.utf16()));
        if(!Ready||!Done){if(Ready)CloseHandle(Ready);if(Done)CloseHandle(Done);return 1;}
        const auto Trace=QDir(Args[3]).absoluteFilePath(QString("run-%1.json").arg(Run));
        QProcess Process;auto Environment=QProcessEnvironment::systemEnvironment();
        Environment.insert("HC_STARTUP_TRACE",Trace);Environment.insert("HC_STARTUP_READY_EVENT",ReadyName);Environment.insert("HC_STARTUP_DONE_EVENT",DoneName);
        if(Args.contains("detail"))Environment.insert("HC_STARTUP_DETAIL","1");
        Process.setProcessEnvironment(Environment);Process.setProgram(QFileInfo(Args[1]).absoluteFilePath());
        QStringList Arguments{"--config-dir",Config};if(Args.size()>5&&!Args[5].isEmpty()&&Args[5]!="-")Arguments<<"--"<<Args[5];Process.setArguments(Arguments);
        Process.setStandardOutputFile(QDir(Args[3]).absoluteFilePath(QString("run-%1.stdout").arg(Run)));
        Process.setStandardErrorFile(QDir(Args[3]).absoluteFilePath(QString("run-%1.stderr").arg(Run)));
        if (!PrepareForeground()) { CloseHandle(Ready); CloseHandle(Done); qCritical("Cannot acquire foreground before measurement"); return 3; }
        const auto Begin=hc::startup::Tick();Process.start();
        if(!Process.waitForStarted(10000)){CloseHandle(Ready);CloseHandle(Done);qCritical("Start failed");return 1;}
        const auto Started=hc::startup::Tick();const auto Pid=DWORD(Process.processId());AllowSetForegroundWindow(Pid);
        qint64 Signal=0,Sent=0,Finished=0;HWND Window=nullptr;bool Foreground=false;
        QJsonObject ReadyDisplay, UsedDisplay;
        while(hc::startup::Tick()-Begin<30*hc::startup::Frequency()&&Process.state()!=QProcess::NotRunning){
            QCoreApplication::processEvents();
            if(!Signal)Window=WindowFor(Pid);
            if(Window&&!Signal&&GetForegroundWindow()!=Window){
                const auto Thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
                const bool Attached=Thread&&Thread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),Thread,TRUE);
                SetForegroundWindow(Window);if(Attached)AttachThreadInput(GetCurrentThreadId(),Thread,FALSE);Foreground=GetForegroundWindow()==Window;
                if (Foreground) CloseForegroundHost();
            }
            if(!Signal&&WaitForSingleObject(Ready,0)==WAIT_OBJECT_0){
                Signal=hc::startup::Tick();Foreground=Window&&GetForegroundWindow()==Window;
                if(!Foreground)break;
                if (Args.contains("detail")) ReadyDisplay = CaptureDisplayedClient(Window, QDir(OutputRoot).filePath(QString("run-%1-ready-desktop.png").arg(Run)));
                INPUT Input[2]{};Input[0].type=Input[1].type=INPUT_KEYBOARD;
                Input[0].ki.wVk=Input[1].ki.wVk=Args.contains("up")?VK_UP:VK_DOWN;Input[1].ki.dwFlags=KEYEVENTF_KEYUP;
                Sent=hc::startup::Tick();if(SendInput(2,Input,sizeof(INPUT))!=2)break;
            }
            if(Signal&&WaitForSingleObject(Done,0)==WAIT_OBJECT_0){Finished=hc::startup::Tick();break;}
            QThread::msleep(1);
        }
        Foreground=Window&&GetForegroundWindow()==Window; CloseForegroundHost();
        const auto Composition = Window && Args.contains("detail") ? CompositionEvidence(Window) : QJsonObject{};
        if (Window && Finished && Args.contains("detail")) UsedDisplay = CaptureDisplayedClient(Window, QDir(OutputRoot).filePath(QString("run-%1-used-desktop.png").arg(Run)));
        if(Window&&Args.contains("detail"))App.primaryScreen()->grabWindow(WId(Window)).save(QDir(Args[3]).absoluteFilePath(QString("run-%1.png").arg(Run)));
        if(Window)PostMessageW(Window,WM_CLOSE,0,0);
        if(!Process.waitForFinished(5000)){Process.kill();Process.waitForFinished(5000);}
        CloseHandle(Ready);CloseHandle(Done);
        QFile File(Trace);QJsonObject Report;if(File.open(QIODevice::ReadOnly))Report=QJsonDocument::fromJson(File.readAll()).object();
        QString ReadyFocus; qint64 ReadyAt=0,UsedAt=0;for(const auto& Item:Report["events"].toArray()){auto Event=Item.toObject();if(Event["event"]=="ready.uiPaint"){ReadyAt=Event["qpc"].toString().toLongLong(); ReadyFocus=Event["focus"].toString();}if(Event["event"]=="used.uiPaint")UsedAt=Event["qpc"].toString().toLongLong();}
        const auto Ms=[](qint64 T){return double(T)*1000/hc::startup::Frequency();};
        const bool Passed=Finished&&ReadyAt&&UsedAt&&Foreground&&(ExpectedFocus.isEmpty()||ExpectedFocus==ReadyFocus);All&=Passed;
        Results.append(QJsonObject{{"run",Run},{"expectedFocus",ExpectedFocus},{"readyFocus",ReadyFocus},{"ok",Passed},{"foreground",Foreground},{"configuration",Config},{"templateSha256",QString::fromLatin1(TemplateHash)},{"qpcStart",QString::number(Begin)},
            {"qpcStarted",QString::number(Started)},{"qpcReadyReceived",QString::number(Signal)},{"qpcInputSent",QString::number(Sent)},
            {"readyMs",ReadyAt?Ms(ReadyAt-Begin):-1},{"useUiPaintMs",UsedAt?Ms(UsedAt-Begin):-1},
            {"inputMs",UsedAt&&Sent?Ms(UsedAt-Sent):-1},{"trace",Trace},{"displayEvidence","uiPaint-only"},{"composition",Composition},
            {"readyDesktopCapture",ReadyDisplay},{"usedDesktopCapture",UsedDisplay},{"capturePerturbsInputTiming",Args.contains("detail")}});
        if(!Passed)break;
    }
    QFile Output(QDir(Args[3]).absoluteFilePath("summary.json"));if(!Output.open(QIODevice::WriteOnly))return 1;
    Output.write(QJsonDocument(QJsonObject{{"ok",All},{"runs",Results}}).toJson());qInfo("Startup probe: %s (%d runs)",All?"passed":"unverified",int(Results.size()));return All?0:2;
}
