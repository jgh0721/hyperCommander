#include "stdafx.h"
#include "UniqueLibs/document/epubResourceLease.hpp"
#include <Windows.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <iostream>
using hc::document::EpubResourceLease;
namespace {
struct Child {
    QProcess process;
    Child(const QStringList& Args) {
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* P){P->flags|=CREATE_NO_WINDOW;});
        process.start(QCoreApplication::applicationFilePath(),Args);process.waitForStarted(5000);
    }
    ~Child(){if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(5000);}}
    QByteArray line(int Timeout=5000){QElapsedTimer T;T.start();while(!process.canReadLine() && T.elapsed()<Timeout)process.waitForReadyRead(50);return process.readLine().trimmed();}
};
int hold(const QString& Domain,bool Cancel) {
    EpubResourceLease Lease(Domain);QElapsedTimer Timer;Timer.start();
    if(!Lease.Acquire([&]{return Cancel && Timer.elapsed()>250;},[]{std::cout<<"queued"<<std::endl;})){std::cout<<Lease.Error().toStdString()<<std::endl;return Cancel?4:3;}
    HANDLE Job=CreateJobObjectW(nullptr,reinterpret_cast<LPCWSTR>(Lease.JobName().utf16()));if(!Job)return 5;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};Limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(Job,JobObjectExtendedLimitInformation,&Limits,sizeof(Limits)))return 6;
    auto Command=(QString('"')+QCoreApplication::applicationFilePath()+"\" --sleep").toStdWString();STARTUPINFOW Startup{sizeof(Startup)};PROCESS_INFORMATION Process{};
    if(!CreateProcessW(nullptr,Command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process))return 7;
    if(!AssignProcessToJobObject(Job,Process.hProcess)){TerminateProcess(Process.hProcess,1);return 8;}ResumeThread(Process.hThread);CloseHandle(Process.hThread);CloseHandle(Process.hProcess);
    std::cout<<"ready "<<Process.dwProcessId<<std::endl;QThread::sleep(300);CloseHandle(Job);return 0;
}
}
class ResourceTests final:public QObject {
    Q_OBJECT
private slots:
    void crossProcessBudgetAndCrash() {
        const auto Domain=QUuid::createUuid().toString(QUuid::Id128);
        Child First({"--hold",Domain}),Second({"--hold",Domain});const auto FirstLine=First.line();QVERIFY(FirstLine.startsWith("ready "));QVERIFY(Second.line().startsWith("ready "));
        const auto Pid=FirstLine.mid(6).toUInt();HANDLE Grandchild=OpenProcess(SYNCHRONIZE,FALSE,Pid);QVERIFY(Grandchild);
        Child Third({"--hold",Domain});QCOMPARE(Third.line(),QByteArray("queued"));QVERIFY(Third.line(150).isEmpty());
        Child Cancel({"--cancel",Domain});QCOMPARE(Cancel.line(),QByteArray("queued"));QCOMPARE(Cancel.line(),QByteArray("cancelled"));QVERIFY(Cancel.process.waitForFinished(5000));QCOMPARE(Cancel.process.exitCode(),4);
        First.process.kill();QVERIFY(First.process.waitForFinished(5000));QVERIFY(Third.line().startsWith("ready "));QCOMPARE(WaitForSingleObject(Grandchild,5000),DWORD(WAIT_OBJECT_0));CloseHandle(Grandchild);
    }
    void boundedQueue() {
        const auto Domain=QUuid::createUuid().toString(QUuid::Id128);std::vector<std::unique_ptr<Child>> Children;
        for(unsigned I=0;I<EpubResourceLease::TotalLimit;++I){auto C=std::make_unique<Child>(QStringList{"--hold",Domain});const auto Line=C->line();QVERIFY(I<2?Line.startsWith("ready "):Line=="queued");Children.push_back(std::move(C));}
        Child Overflow({"--hold",Domain});QCOMPARE(Overflow.line(),QByteArray("epub-resource-queue-full"));QVERIFY(Overflow.process.waitForFinished(5000));QCOMPARE(Overflow.process.exitCode(),3);
    }
};
int main(int argc,char** argv) {
    QCoreApplication App(argc,argv);const auto Args=App.arguments();
    if(Args.contains("--sleep")){QThread::sleep(300);return 0;}
    if(Args.size()==3 && (Args[1]=="--hold" || Args[1]=="--cancel"))return hold(Args[2],Args[1]=="--cancel");
    ResourceTests Tests;return QTest::qExec(&Tests,argc,argv);
}
#include "testResources.moc"
