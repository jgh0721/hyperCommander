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
#include <QThread>
#include <QUuid>
#include "../UniqueLibs/startupTrace.hpp"

namespace {
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
        const auto Begin=hc::startup::Tick();Process.start();
        if(!Process.waitForStarted(10000)){CloseHandle(Ready);CloseHandle(Done);qCritical("Start failed");return 1;}
        const auto Started=hc::startup::Tick();const auto Pid=DWORD(Process.processId());AllowSetForegroundWindow(Pid);
        qint64 Signal=0,Sent=0,Finished=0;HWND Window=nullptr;bool Foreground=false;
        while(hc::startup::Tick()-Begin<30*hc::startup::Frequency()&&Process.state()!=QProcess::NotRunning){
            QCoreApplication::processEvents();
            if(!Signal)Window=WindowFor(Pid);
            if(Window&&!Signal&&GetForegroundWindow()!=Window){
                const auto Thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
                const bool Attached=Thread&&Thread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),Thread,TRUE);
                SetForegroundWindow(Window);if(Attached)AttachThreadInput(GetCurrentThreadId(),Thread,FALSE);Foreground=GetForegroundWindow()==Window;
            }
            if(!Signal&&WaitForSingleObject(Ready,0)==WAIT_OBJECT_0){
                Signal=hc::startup::Tick();Foreground=Window&&GetForegroundWindow()==Window;
                if(!Foreground)break;
                INPUT Input[2]{};Input[0].type=Input[1].type=INPUT_KEYBOARD;
                Input[0].ki.wVk=Input[1].ki.wVk=Args.contains("up")?VK_UP:VK_DOWN;Input[1].ki.dwFlags=KEYEVENTF_KEYUP;
                Sent=hc::startup::Tick();if(SendInput(2,Input,sizeof(INPUT))!=2)break;
            }
            if(Signal&&WaitForSingleObject(Done,0)==WAIT_OBJECT_0){Finished=hc::startup::Tick();break;}
            QThread::msleep(1);
        }
        Foreground=Window&&GetForegroundWindow()==Window;
        if(Window&&Args.contains("detail"))App.primaryScreen()->grabWindow(WId(Window)).save(QDir(Args[3]).absoluteFilePath(QString("run-%1.png").arg(Run)));
        if(Window)PostMessageW(Window,WM_CLOSE,0,0);
        if(!Process.waitForFinished(5000)){Process.kill();Process.waitForFinished(5000);}
        CloseHandle(Ready);CloseHandle(Done);
        QFile File(Trace);QJsonObject Report;if(File.open(QIODevice::ReadOnly))Report=QJsonDocument::fromJson(File.readAll()).object();
        qint64 ReadyAt=0,UsedAt=0;for(const auto& Item:Report["events"].toArray()){auto Event=Item.toObject();if(Event["event"]=="ready.uiPaint")ReadyAt=Event["qpc"].toString().toLongLong();if(Event["event"]=="used.uiPaint")UsedAt=Event["qpc"].toString().toLongLong();}
        const auto Ms=[](qint64 T){return double(T)*1000/hc::startup::Frequency();};
        const bool Passed=Finished&&ReadyAt&&UsedAt&&Foreground;All&=Passed;
        Results.append(QJsonObject{{"run",Run},{"ok",Passed},{"foreground",Foreground},{"configuration",Config},{"templateSha256",QString::fromLatin1(TemplateHash)},{"qpcStart",QString::number(Begin)},
            {"qpcStarted",QString::number(Started)},{"qpcReadyReceived",QString::number(Signal)},{"qpcInputSent",QString::number(Sent)},
            {"readyMs",ReadyAt?Ms(ReadyAt-Begin):-1},{"useUiPaintMs",UsedAt?Ms(UsedAt-Begin):-1},
            {"inputMs",UsedAt&&Sent?Ms(UsedAt-Sent):-1},{"trace",Trace},{"displayEvidence","uiPaint-only"}});
        if(!Passed)break;
    }
    QFile Output(QDir(Args[3]).absoluteFilePath("summary.json"));if(!Output.open(QIODevice::WriteOnly))return 1;
    Output.write(QJsonDocument(QJsonObject{{"ok",All},{"runs",Results}}).toJson());qInfo("Startup probe: %s (%d runs)",All?"passed":"unverified",int(Results.size()));return All?0:2;
}
