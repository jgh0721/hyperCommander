#pragma once
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QGuiApplication>
#include <QUuid>
#include <QtTest>
#include <TlHelp32.h>
#include <algorithm>
#include <functional>
namespace hc::test {
// Observes installed editor Ready; optional bounded startup tracing is diagnostic only.
// Never edits/saves originals, flushes caches or terminates product processes.
inline bool RunRhwpOpenTiming(QObject* Controller, const std::function<QWidget*(const QString&)>& Select) {
    const auto Root=qEnvironmentVariable("HC_RHWP_TIMING_ROOT"), Output=qEnvironmentVariable("HC_RHWP_TIMING_OUTPUT");
    const auto Expected=QFileInfo(qEnvironmentVariable("HC_RHWP_TIMING_EDITOR")).canonicalFilePath();
    const auto Rounds=qEnvironmentVariableIntValue("HC_RHWP_TIMING_ROUNDS");
    if(Root.isEmpty()||Output.isEmpty()||Expected.isEmpty()||Rounds<1||Rounds>3)return false;
    const auto Hash=[](const QString& Path) {QFile Input(Path);if(!Input.open(QIODevice::ReadOnly))return QByteArray{};
        QCryptographicHash H(QCryptographicHash::Sha256);if(!H.addData(&Input))return QByteArray{};return H.result().toHex();};
    const auto Files=QDir(Root).entryInfoList({"*.hwp","*.hwpx"},QDir::Files,QDir::Name);
    if(Files.isEmpty())return false;
    QJsonArray Sources,Samples;int FileNumber=0;
    for(const auto& File:Files) {const auto Digest=Hash(File.absoluteFilePath());if(Digest.isEmpty())return false;
        Sources.append(QJsonObject{{"fileId",QString("F%1").arg(++FileNumber)},{"path",File.absoluteFilePath()},
            {"bytes",double(File.size())},{"sha256Before",QString::fromLatin1(Digest)},{"mtimeBefore",File.lastModified().toUTC().toString(Qt::ISODateWithMs)}});}
    QJsonObject Report{{"schemaVersion",1},{"route","QTest::keyClick F3/F4 through actual HC grid/command controller; installed HyperEditor"},
        {"isInstalledHcOsKeyTiming",false},{"applicationDir",QCoreApplication::applicationDirPath()},
        {"harnessExecutable",QCoreApplication::applicationFilePath()},{"expectedEditor",Expected},
        {"sourceRoot",Root},{"requestedRounds",Rounds},{"requestedProbeIntervalMs",10},{"readyTimeoutMs",120000},
        {"closeTimeoutMs",30000},{"clockMonotonic",QElapsedTimer::isMonotonic()},{"clockType",int(QElapsedTimer::clockType())},
        {"cachePolicy","No cache clearing. Source pre-hash reads warm input bytes; earlier sessions may have warmed runtime/OS caches. First measured launch is not cold boot."},
        {"sources",Sources},{"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    const auto Flush=[&] {Report["samples"]=Samples;QSaveFile File(Output);if(!File.open(QIODevice::WriteOnly))return false;
        auto Bytes=QJsonDocument(Report).toJson(QJsonDocument::Indented);Bytes.replace("\n","\r\n");return File.write(Bytes)==Bytes.size()&&File.commit();};
    if(!Flush())return false;
    const auto Skip=qEnvironmentVariableIntValue("HC_RHWP_TIMING_SKIP_COUNT");
    if(Skip<0||Skip>=Rounds*Files.size()*2)return false;
    Report["skippedCompletedOrdinalCount"]=Skip;int PlannedOrdinal=0;
    const bool StopOnFailure=qEnvironmentVariableIsSet("HC_RHWP_TIMING_STOP_ON_FAILURE");
    Report["stopOnMeasurementFailure"]=StopOnFailure;
    bool All=true,Stop=false;
    for(int Round=1;Round<=Rounds&&!Stop;++Round)for(int Number=0;Number<Files.size()&&!Stop;++Number)for(bool Editable:{false,true}) {
        if(++PlannedOrdinal<=Skip)continue;
        const auto& Source=Files[Number];const auto Name=Source.fileName(), Path=QDir::toNativeSeparators(Source.absoluteFilePath());
        QJsonObject Row{{"fileId",Sources[Number].toObject()["fileId"]},{"round",Round},{"key",Editable?"F4":"F3"},
            {"firstForFileAndKey",Round==1},{"ordinal",PlannedOrdinal},{"path",Path},{"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
        const auto TraceRoot=qEnvironmentVariable("HC_RHWP_TIMING_TRACE_DIR");
        QString TraceRun;
        if(!TraceRoot.isEmpty()) {
            if(!QFileInfo(TraceRoot).isDir())return false;
            TraceRun=QUuid::createUuid().toString(QUuid::Id128);
            qputenv("HC_RHWP_STARTUP_TRACE_DIR",TraceRoot.toUtf8());qputenv("HC_RHWP_STARTUP_TRACE_ID",TraceRun.toUtf8());
            Row["startupTraceRunId"]=TraceRun;
        } else {qunsetenv("HC_RHWP_STARTUP_TRACE_DIR");qunsetenv("HC_RHWP_STARTUP_TRACE_ID");}
        // Preparation is outside the timed key dispatch. A stopped QProcess can
        // still await deferred deletion after its OS tree has already drained.
        QElapsedTimer Preparation;Preparation.start();QWidget* Receiver=nullptr;int Attempts=0;
        do {
            QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
            QCoreApplication::processEvents(QEventLoop::AllEvents,1);
            Receiver=Select(Name);++Attempts;
            if(Receiver&&!Controller->findChild<QProcess*>())break;
            QTest::qWait(10);
        }while(Preparation.elapsed()<5000);
        Row["preparationAttempts"]=Attempts;Row["preparationWaitMs"]=Preparation.nsecsElapsed()/1e6;
        if(!Receiver||Controller->findChild<QProcess*>()) {
            Row["status"]="selection-or-prior-process-error";Row["keyDispatched"]=false;
            Row["receiverMissing"]=!Receiver;
            if(const auto PriorProcess=Controller->findChild<QProcess*>())Row["priorQProcessState"]=int(PriorProcess->state());
            Samples.append(Row);All=false;Stop=true;Flush();break;
        }
        Row["keyDispatched"]=true;
        Row["receiverClass"]=Receiver->metaObject()->className();Row["receiverObjectName"]=Receiver->objectName();
        const auto ReadyTitle=Name+(Editable?QStringLiteral(" — 한글 문서 편집"):QStringLiteral(" — 한글 문서 보기"));
        QElapsedTimer Clock;Clock.start(); // Immediately before the actual Qt F-key dispatch.
        QTest::keyClick(Receiver,Editable?Qt::Key_F4:Qt::Key_F3);
        Row["keyDispatchReturnedMs"]=Clock.nsecsElapsed()/1e6;
        QPointer<QProcess> Viewer;HWND Window=nullptr;DWORD Pid=0;double Prior=0,MaxGap=0,Visible=-1,Ready=-1,Lower=-1;
        QString Title,Status="timeout";
        while(Clock.elapsed()<120000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents,1);
            const double Now=Clock.nsecsElapsed()/1e6;MaxGap=(std::max)(MaxGap,Now-Prior);
            if(!Viewer)Viewer=Controller->findChild<QProcess*>();
            if(Viewer&&Viewer->processId())Pid=DWORD(Viewer->processId());
            if(Pid&&!Window) {
                struct Probe {DWORD pid_;HWND window_=nullptr;} State{Pid};
                EnumWindows([](HWND Candidate,LPARAM Param)->BOOL {auto& P=*reinterpret_cast<Probe*>(Param);DWORD Owner=0;GetWindowThreadProcessId(Candidate,&Owner);
                    if(Owner==P.pid_&&IsWindowVisible(Candidate)&&GetWindowTextLengthW(Candidate)>0){P.window_=Candidate;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&State));
                Window=State.window_;
                if(Window) {Visible=Clock.nsecsElapsed()/1e6;Row["firstVisibleObservedMs"]=Visible;Row["firstVisibleLowerBoundMs"]=Prior;Row["windowHandle"]=QString::number(quintptr(Window));}
            }
            if(Window) {wchar_t Buffer[2048]{};GetWindowTextW(Window,Buffer,2048);Title=QString::fromWCharArray(Buffer);
                if(Title==ReadyTitle) {Ready=Clock.nsecsElapsed()/1e6;Lower=Prior;Status="ready";break;}
                if(Title.endsWith(QStringLiteral(" — 열기 실패"))) {Status="native-open-failed";break;}}
            if(Viewer&&Viewer->state()==QProcess::NotRunning){Status="process-exited-before-ready";break;}
            Prior=Now;QTest::qWait(10);
        }
        Row["status"]=Status;Row["readyTitleExpected"]=ReadyTitle;Row["lastObservedTitle"]=Title;
        Row["observationEndMs"]=Clock.nsecsElapsed()/1e6;Row["maxProbeGapMs"]=MaxGap;Row["nativePid"]=int(Pid);
        if(Ready>=0){Row["readyObservedMs"]=Ready;Row["readyLowerBoundMs"]=Lower;Row["readyObservationBracketMs"]=Ready-Lower;}
        // Routing proof and process-tree handles are acquired after the timed ready observation.
        QString ImagePath;HANDLE RootHandle=Pid?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,Pid):nullptr;
        if(RootHandle){wchar_t Buffer[32768]{};DWORD Length=32768;if(QueryFullProcessImageNameW(RootHandle,0,Buffer,&Length))ImagePath=QString::fromWCharArray(Buffer,int(Length));CloseHandle(RootHandle);}
        const auto Args=Viewer?Viewer->arguments():QStringList{};
        const bool Route=Viewer&&QFileInfo(Viewer->program()).canonicalFilePath().compare(Expected,Qt::CaseInsensitive)==0
            &&QFileInfo(ImagePath).canonicalFilePath().compare(Expected,Qt::CaseInsensitive)==0
            &&Args==(QStringList{Editable?"--rhwp-edit":"--rhwp-view","--",Path});
        Row["qProcessProgram"]=Viewer?Viewer->program():QString{};Row["qProcessArguments"]=QJsonArray::fromStringList(Args);
        Row["nativeImagePath"]=ImagePath;Row["routeVerified"]=Route;
        // Trace export/flush waits happen strictly after the timed Ready boundary.
        if(!TraceRun.isEmpty()&&Status=="ready") {
            const auto PostStart=Clock.nsecsElapsed()/1e6;
            QStringList Helpers;
            do {Helpers=QDir(TraceRoot).entryList({"startup-"+TraceRun+"-helper-*.json"},QDir::Files);
                if(Helpers.size()==1) {
                    QFile A(QDir(TraceRoot).filePath(Helpers.first()));
                    const auto Ack=A.open(QIODevice::ReadOnly)?QJsonDocument::fromJson(A.read(64*1024+1)).object():QJsonObject{};
                    if(Ack["runId"]==TraceRun&&Ack["collectorPid"].toInteger()==Pid&&Ack["collectionAck"]==true&&Ack["rendererReceived"]==true)break;
                    Helpers.clear();
                }
                QTest::qWait(10);
            }while(Clock.nsecsElapsed()/1e6-PostStart<5000);
            Row["traceDrainAfterReadyMs"]=Clock.nsecsElapsed()/1e6-PostStart;
            Row["helperTraceCaptured"]=Helpers.size()==1;
            if(Helpers.size()==1)Row["helperTracePath"]=QDir(TraceRoot).filePath(Helpers.first());
        }
        // Optional evidence is captured after the timing boundary, never as Ready proof.
        if(Status=="ready"&&!Editable&&Round==1&&qEnvironmentVariableIsSet("HC_RHWP_TIMING_SCREENSHOTS")) {
            QTest::qWait(150);
            const auto Screenshot=QFileInfo(Output).dir().filePath("installed-first-screen-"+Row["fileId"].toString()+".png");
            auto* Screen=QGuiApplication::primaryScreen();
            Row["screenshotSaved"]=Screen&&Screen->grabWindow(WId(quintptr(Window))).save(Screenshot);
            Row["screenshotPath"]=Screenshot;Row["screenshotObservedMs"]=Clock.nsecsElapsed()/1e6;
        }
        struct Held {DWORD pid_;HANDLE handle_;};QList<Held> Handles;QJsonArray Pids;
        QHash<DWORD,DWORD> Parents;HANDLE Snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(Snapshot!=INVALID_HANDLE_VALUE){PROCESSENTRY32W Entry{};Entry.dwSize=sizeof(Entry);if(Process32FirstW(Snapshot,&Entry))do {Parents.insert(Entry.th32ProcessID,Entry.th32ParentProcessID);}while(Process32NextW(Snapshot,&Entry));CloseHandle(Snapshot);}
        QSet<DWORD> Tree;if(Pid)Tree.insert(Pid);bool Changed=true;
        while(Changed){Changed=false;for(auto I=Parents.cbegin();I!=Parents.cend();++I)if(Tree.contains(I.value())&&!Tree.contains(I.key())){Tree.insert(I.key());Changed=true;}}
        for(const auto Child:Tree){Pids.append(int(Child));const auto Handle=OpenProcess(SYNCHRONIZE,FALSE,Child);if(Handle)Handles.append({Child,Handle});}
        Row["observedTreePids"]=Pids;
        const auto CloseStart=Clock.nsecsElapsed()/1e6;
        const bool CloseSent=Window&&IsWindow(Window)&&PostMessageW(Window,WM_CLOSE,0,0);
        Row["normalCloseRequested"]=CloseSent;
        bool Drained=false;
        while(Clock.nsecsElapsed()/1e6-CloseStart<30000){QCoreApplication::processEvents(QEventLoop::AllEvents,1);
            Drained=(!Viewer||Viewer->state()==QProcess::NotRunning);
            for(const auto& Held:Handles)Drained&=WaitForSingleObject(Held.handle_,0)==WAIT_OBJECT_0;
            if(Drained)break;QTest::qWait(10);}
        QJsonArray Remaining;for(const auto& Held:Handles){if(WaitForSingleObject(Held.handle_,0)!=WAIT_OBJECT_0)Remaining.append(int(Held.pid_));CloseHandle(Held.handle_);}
        Row["closeAndTreeReclaimMs"]=Clock.nsecsElapsed()/1e6-CloseStart;Row["treeReclaimed"]=Drained;Row["remainingTreePids"]=Remaining;
        if(!TraceRun.isEmpty()) {
            const auto ParentTrace=QDir(TraceRoot).filePath("startup-"+TraceRun+"-editor-"+QString::number(Pid)+".json");
            Row["editorTracePath"]=ParentTrace;Row["editorTraceCaptured"]=QFileInfo(ParentTrace).isFile();
            if(!Row["helperTraceCaptured"].toBool()||!Row["editorTraceCaptured"].toBool())All=false;
        }
        Row["sha256After"]=QString::fromLatin1(Hash(Source.absoluteFilePath()));
        Row["originalUnchanged"]=Row["sha256After"]==Sources[Number].toObject()["sha256Before"];
        if(Status!="ready"||!Route||!Drained||!Row["originalUnchanged"].toBool())All=false;
        if(!Drained||!Row["originalUnchanged"].toBool()){Stop=true;Report["stopReason"]="cleanup-or-source-integrity";}
        // A clean-cohort failure must be retained and stop before another key dispatch.
        // This opt-in affects only post-observation continuation, outside the clock boundary.
        if(StopOnFailure&&(Status!="ready"||!Route)){Stop=true;Report["stopReason"]="timing-failure";}
        Samples.append(Row);if(!Flush())return false;
        qInfo().noquote()<<"OPEN_TIMING"<<Row["ordinal"].toInt()<<Row["fileId"].toString()<<Row["key"].toString()<<Status<<Ready<<"ms; reclaimed="<<Drained;
    }
    for(int I=0;I<Files.size();++I){auto Source=Sources[I].toObject();Source["sha256After"]=QString::fromLatin1(Hash(Files[I].absoluteFilePath()));
        Source["unchanged"]=Source["sha256Before"]==Source["sha256After"];if(!Source["unchanged"].toBool())All=false;Sources[I]=Source;}
    Report["sources"]=Sources;Report["finishedUtc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Report["completed"]=!Stop;Report["allReadyAndUnchanged"]=All;return Flush()&&All;
}
}
