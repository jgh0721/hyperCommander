#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdio>
#include "UniqueLibs/document/documentWebProcess.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
using namespace hc::document;
class DocumentWebProcessTests final : public QObject
{
    Q_OBJECT
private:
    WebProcessRuntime prepare(QTemporaryDir& Directory,const QByteArray& Mode={})
    {
        WebProcessRuntime Runtime; Runtime.root_=Directory.path();
        const auto Program=Directory.filePath("HyperEditorWeb.exe");
        if(!QFile::copy(QCoreApplication::applicationDirPath()+"/hc_web_process_fixture.exe",Program)) return {};
        QFile File(Program); if(!File.open(QIODevice::ReadOnly)) return {};
        Runtime.sha256_["HyperEditorWeb.exe"]=QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256).toHex(); File.close();
        QFile Config(Directory.filePath("mode.txt")); if(!Config.open(QIODevice::WriteOnly) || Config.write(Mode)!=Mode.size()) return {};
        Config.close(); Runtime.sha256_["mode.txt"]=QCryptographicHash::hash(Mode,QCryptographicHash::Sha256).toHex(); return Runtime;
    }
    WebProcessLimits limits() { return {4,512ULL*1024*1024,1500}; }
private slots:
    void cancelledRuntimeVerification_data()
    { QTest::addColumn<int>("After");QTest::newRow("before-read")<<0;QTest::newRow("after-first-read")<<2; }
    void cancelledRuntimeVerification()
    {
        QFETCH(int,After);QTemporaryDir Directory;const auto Runtime=prepare(Directory);QVERIFY(!Runtime.sha256_.isEmpty());
        WebProcessError Error;std::unique_ptr<DocumentWebProcess> Pending;int Checks=0;
        auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error,&Pending,[&] { return ++Checks>After; });
        QVERIFY(!Owner);QVERIFY(!Pending);QCOMPARE(Error.code_,QString("web-owner-runtime-cancelled"));
        QFile Writable(Directory.filePath("HyperEditorWeb.exe"));QVERIFY(Writable.open(QIODevice::WriteOnly));
    }
    void exhaustedParentJobRejectedBeforeLaunch()
    {
        QTemporaryDir Directory; const auto Runtime = prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        QProcess Probe; Probe.setProgram(QCoreApplication::applicationFilePath());
        Probe.setArguments({"--single-process-job-probe", Directory.path()}); Probe.start();
        QVERIFY(Probe.waitForStarted(5000)); QVERIFY(Probe.waitForFinished(5000));
        QCOMPARE(Probe.exitStatus(), QProcess::NormalExit); QCOMPARE(Probe.exitCode(), 0);
        QCOMPARE(Probe.readAllStandardOutput().trimmed(), QByteArray("web-owner-parent-process-limit"));
        QFile File(Directory.filePath("HyperEditorWeb.exe")); QVERIFY(File.open(QIODevice::WriteOnly));
    }
    void approvedInventory_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"approved", "empty-pin", "wrong-pin", "mixed-authority", "candidate",
            "wrong-source", "wrong-profile", "missing-helper", "missing-renderer", "bundle-mismatch",
            "duplicate", "traversal", "trailing-dot", "nul", "bad-sha", "changed-binary", "changed-manifest", "oversized", "invalid-json"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void approvedInventory()
    {
        QFETCH(QString, Mode);
        QTemporaryDir Directory; auto Runtime = prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        const auto Hash = [](const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(); };
        const auto Write = [](const QString& Path, const QByteArray& Bytes) {
            QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
        };
        QVERIFY(QDir(Directory.path()).mkpath("Libs/bundle"));
        QVERIFY(Write(Directory.filePath("Libs/bundle/asset-manifest.json"), "fixture bundle"));
        QVERIFY(Write(Directory.filePath("QtWebEngineProcess.exe"), "fixture renderer"));
        Runtime.sha256_["Libs/bundle/asset-manifest.json"] = Hash("fixture bundle");
        Runtime.sha256_["QtWebEngineProcess.exe"] = Hash("fixture renderer");
        QJsonArray Files;
        for (auto At = Runtime.sha256_.cbegin(); At != Runtime.sha256_.cend(); ++At) {
            if ((Mode == "missing-helper" && At.key() == "HyperEditorWeb.exe")
                || (Mode == "missing-renderer" && At.key() == "QtWebEngineProcess.exe")) continue;
            Files.append(QJsonObject{{"path", At.key()}, {"sha256", QString::fromLatin1(At.value())}});
        }
        if (Mode == "duplicate") Files.append(QJsonObject{{"path", "hypereditorweb.exe"}, {"sha256", QString::fromLatin1(Hash("x"))}});
        if (Mode == "traversal" || Mode == "trailing-dot" || Mode == "nul" || Mode == "bad-sha") {
            const auto Path = Mode == "traversal" ? QString("../evil.dll") : Mode == "trailing-dot" ? QString("evil.dll.")
                : Mode == "nul" ? QString("evil") + QChar(0) + ".dll" : QString("extra.dll");
            Files.append(QJsonObject{{"path", Path}, {"sha256", Mode == "bad-sha" ? QString("x") : QString::fromLatin1(Hash("x"))}});
        }
        QJsonObject Manifest{{"schemaVersion", 1}, {"profile", Mode == "wrong-profile" ? "editor" : "native-read-only"},
            {"sourceCommit", Mode == "wrong-source" ? "wrong" : "1a76570e833917d15817415a53c09ad61ab3203f"},
            {"releaseEligible", Mode != "candidate"}, {"assetManifestSha256", QString::fromLatin1(Hash(Mode == "bundle-mismatch" ? "wrong" : "fixture bundle"))}, {"files", Files}};
        auto Bytes = QJsonDocument(Manifest).toJson(QJsonDocument::Compact);
        if (Mode == "oversized") Bytes = QByteArray(256 * 1024 + 1, ' ');
        if (Mode == "invalid-json") Bytes = "{";
        QVERIFY(Write(Directory.filePath("runtime-inventory.json"), Bytes));
        Runtime.approvedInventorySha256_ = Mode == "empty-pin" ? QByteArray{} : Mode == "wrong-pin" ? Hash("wrong") : Hash(Bytes);
        if (Mode != "mixed-authority") Runtime.sha256_.clear();
        if (Mode == "changed-binary") QVERIFY(Write(Directory.filePath("HyperEditorWeb.exe"), "changed"));
        if (Mode == "changed-manifest") QVERIFY(Write(Directory.filePath("runtime-inventory.json"), Bytes + " "));
        WebProcessError Error; auto Owner = DocumentWebProcess::Start(Runtime, limits(), 0, Error);
        if (Mode == "approved") {
            QVERIFY2(Owner, qPrintable(Error.code_)); QByteArray Reply;
            QVERIFY(Owner->Exchange("hello", Reply, 1000, Error)); QCOMPARE(Reply, QByteArray("hello"));
            QVERIFY(!Write(Directory.filePath("runtime-inventory.json"), "changed"));
            QVERIFY(!Write(Directory.filePath("Libs/bundle/asset-manifest.json"), "changed"));
            QVERIFY(Owner->Close()); QVERIFY(Write(Directory.filePath("runtime-inventory.json"), Bytes));
        } else {
            QVERIFY(!Owner);
            const QString Expected = Mode == "empty-pin" ? "rhwp-runtime-policy-unavailable"
                : Mode == "wrong-pin" || Mode == "changed-manifest" ? "rhwp-runtime-inventory-hash"
                : Mode == "mixed-authority" ? "rhwp-runtime-mixed-authority"
                : Mode == "candidate" || Mode == "wrong-source" || Mode == "wrong-profile" ? "rhwp-runtime-not-approved"
                : Mode == "missing-helper" || Mode == "missing-renderer" || Mode == "bundle-mismatch" ? "rhwp-runtime-required-files"
                : Mode == "changed-binary" ? "web-owner-runtime-hash"
                : Mode == "oversized" ? "rhwp-runtime-inventory-budget"
                : Mode == "invalid-json" ? "rhwp-runtime-inventory-json" : "rhwp-runtime-entry";
            QCOMPARE(Error.code_, Expected);
            // Rejected preparation leaves no runtime lease behind.
            QVERIFY(Write(Directory.filePath("runtime-inventory.json"), Bytes));
        }
    }
    void authenticatedExchangeAndPinnedFiles()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        qputenv("HC_TEST_SECRET","must-not-inherit");
        WebProcessError Error; auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error);
        qunsetenv("HC_TEST_SECRET"); QVERIFY2(Owner, qPrintable(Error.code_));
        QVERIFY(Owner->Snapshot().authenticated_); QVERIFY(Owner->Snapshot().running_);
        QByteArray Reply; QVERIFY(Owner->Exchange("hello",Reply,1000,Error)); QCOMPARE(Reply,QByteArray("hello"));
        QVERIFY(Owner->Exchange("environment",Reply,1000,Error)); QCOMPARE(Reply,QByteArray("clean"));
        // Without SystemDrive, Shell creates a literal "%SystemDrive%" folder under the runtime directory.
        QVERIFY(Owner->Exchange("system-drive",Reply,1000,Error)); QCOMPARE(Reply,qEnvironmentVariable("SystemDrive").toLocal8Bit());
        QVERIFY(Owner->Exchange("policy",Reply,1000,Error)); QCOMPARE(Reply,QByteArray("0,4"));
        QFile Mutate(Directory.filePath("mode.txt")); QVERIFY(!Mutate.open(QIODevice::WriteOnly));
        QVERIFY(!QFile::rename(Directory.filePath("mode.txt"),Directory.filePath("changed.txt")));
        QVERIFY(Owner->Close()); QVERIFY(Owner->Close()); QVERIFY(!Owner->Snapshot().running_);
        QVERIFY(Mutate.open(QIODevice::WriteOnly));
        QVERIFY(!Owner->Exchange("late",Reply,1000,Error)); QCOMPARE(Error.code_,QString("web-owner-closed"));
    }
    void invalidRuntimeRejectedBeforeLaunch()
    {
        QTemporaryDir Directory; auto Runtime=prepare(Directory); WebProcessError Error;
        Runtime.sha256_["mode.txt"]=QByteArray(64,'0');
        QVERIFY(!DocumentWebProcess::Start(Runtime,limits(),0,Error)); QCOMPARE(Error.code_,QString("web-owner-runtime-hash"));
        Runtime.sha256_.remove("mode.txt"); Runtime.sha256_["../outside.dll"]=QByteArray(64,'0');
        QVERIFY(!DocumentWebProcess::Start(Runtime,limits(),0,Error)); QCOMPARE(Error.code_,QString("web-owner-runtime-entry"));
        Runtime.sha256_.remove("../outside.dll"); Runtime.sha256_["hypereditorweb.exe"]=Runtime.sha256_["HyperEditorWeb.exe"];
        QVERIFY(!DocumentWebProcess::Start(Runtime,limits(),0,Error)); QCOMPARE(Error.code_,QString("web-owner-runtime-entry"));
    }
    void startupFailureReleasesLeases_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::newRow("nonce")<<QByteArray("wrong-nonce"); QTest::newRow("timeout")<<QByteArray("hang-ready"); QTest::newRow("peer-pid")<<QByteArray("proxy-peer");
    }
    void startupFailureReleasesLeases()
    {
        QFETCH(QByteArray,Mode); QTemporaryDir Directory; const auto Runtime=prepare(Directory,Mode);
        WebProcessError Error; QVERIFY(!DocumentWebProcess::Start(Runtime,limits(),0,Error));
        QVERIFY(Error.code_=="web-owner-ready" || Error.code_=="web-owner-connect" || Error.code_=="web-owner-peer");
        QFile File(Directory.filePath("HyperEditorWeb.exe")); QVERIFY(File.open(QIODevice::WriteOnly));
    }
    void protocolFailureReapsTree_data()
    {
        QTest::addColumn<QByteArray>("Request");
        for(auto Name:{"stale","oversize","hang"}) QTest::newRow(Name)<<QByteArray(Name);
    }
    void protocolFailureReapsTree()
    {
        QFETCH(QByteArray,Request); QTemporaryDir Directory; const auto Runtime=prepare(Directory);
        WebProcessError Error; auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error); QVERIFY2(Owner,qPrintable(Error.code_));
        QByteArray Reply; QVERIFY(Owner->Exchange("spawn",Reply,1000,Error));
        bool Parsed=false; const auto Pid=Reply.toUInt(&Parsed); QVERIFY(Parsed && Pid);
        HANDLE Child=OpenProcess(SYNCHRONIZE,FALSE,Pid); QVERIFY(Child);
        QVERIFY(!Owner->Exchange(Request,Reply,150,Error)); QCOMPARE(Error.code_,QString("web-owner-exchange"));
        QCOMPARE(WaitForSingleObject(Child,1000),DWORD(WAIT_OBJECT_0)); CloseHandle(Child);
        QVERIFY(!Owner->Snapshot().running_); QVERIFY(!Owner->Snapshot().authenticated_);
    }
    void boundedCloseCanBeRetriedWithoutLosingLeases()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); WebProcessError Error;
        auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error); QVERIFY2(Owner,qPrintable(Error.code_));
        QByteArray Reply; QVERIFY(Owner->Exchange("spawn",Reply,1000,Error));
        HANDLE Child=OpenProcess(SYNCHRONIZE,FALSE,Reply.toUInt()); QVERIFY(Child);
        const bool AlreadyRetired=Owner->Close(0);
        QVERIFY(!Owner->Snapshot().authenticated_);
        if(!AlreadyRetired) {
            QFile Mutate(Directory.filePath("mode.txt")); QVERIFY(!Mutate.open(QIODevice::WriteOnly));
        }
        QVERIFY(Owner->Close()); QCOMPARE(WaitForSingleObject(Child,0),DWORD(WAIT_OBJECT_0)); CloseHandle(Child);
        QVERIFY(Owner->Close(0));
        QFile Mutate(Directory.filePath("mode.txt")); QVERIFY(Mutate.open(QIODevice::WriteOnly));
    }
    void destructorReapsDescendants()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); WebProcessError Error;
        auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error); QVERIFY2(Owner,qPrintable(Error.code_));
        QByteArray Reply; QVERIFY(Owner->Exchange("spawn",Reply,1000,Error));
        HANDLE Child=OpenProcess(SYNCHRONIZE,FALSE,Reply.toUInt()); QVERIFY(Child);
        Owner.reset(); QCOMPARE(WaitForSingleObject(Child,1000),DWORD(WAIT_OBJECT_0)); CloseHandle(Child);
    }
    void ownerDeathReapsEntireJob()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        QProcess Owner; Owner.setProgram(QCoreApplication::applicationFilePath());
        Owner.setArguments({"--owner-death-probe",Directory.path()}); Owner.start(); QVERIFY(Owner.waitForStarted(5000));
        QByteArray Line; QElapsedTimer Timer; Timer.start();
        while(!Line.contains('\n') && Timer.elapsed()<10000)
        {
            Owner.waitForReadyRead(100); Line+=Owner.readAllStandardOutput();
            if(Owner.state()==QProcess::NotRunning) break;
        }
        const auto Pids=Line.trimmed().split(','); QVERIFY2(Pids.size()==2,Line.constData());
        struct ProcessHandle { HANDLE value_; ~ProcessHandle() { if(value_) CloseHandle(value_); } };
        ProcessHandle Helper{OpenProcess(SYNCHRONIZE,FALSE,Pids[0].toUInt())};
        ProcessHandle Renderer{OpenProcess(SYNCHRONIZE,FALSE,Pids[1].toUInt())};
        QVERIFY(Helper.value_); QVERIFY(Renderer.value_);
        Owner.kill(); QVERIFY(Owner.waitForFinished(5000));
        QCOMPARE(WaitForSingleObject(Helper.value_,5000),DWORD(WAIT_OBJECT_0));
        QCOMPARE(WaitForSingleObject(Renderer.value_,5000),DWORD(WAIT_OBJECT_0));
        QFile File(Directory.filePath("mode.txt")); QVERIFY(File.open(QIODevice::WriteOnly));
    }
    void parentAuthorityRejectedBeforeLaunch()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); WebProcessError Error;
        QVERIFY(!DocumentWebProcess::Start(Runtime,limits(),quintptr(GetDesktopWindow()),Error));
        QCOMPARE(Error.code_,QString("web-owner-parent-authority"));
    }
    void requestBudgetDoesNotDesynchronize()
    {
        QTemporaryDir Directory; const auto Runtime=prepare(Directory); WebProcessError Error;
        auto Owner=DocumentWebProcess::Start(Runtime,limits(),0,Error); QVERIFY2(Owner,qPrintable(Error.code_));
        QByteArray Reply; QVERIFY(!Owner->Exchange(QByteArray(HostPayloadLimit+1,'x'),Reply,1000,Error));
        QCOMPARE(Error.code_,QString("web-owner-request-budget"));
        QVERIFY(Owner->Exchange("after",Reply,1000,Error)); QCOMPARE(Reply,QByteArray("after"));
    }
};
int main(int Argc,char** Argv)
{
    QCoreApplication App(Argc,Argv);
    const auto Args=App.arguments();
    if(Args.size()==3 && Args[1]=="--single-process-job-probe")
    {
        const auto Job = CreateJobObjectW(nullptr, nullptr); if(!Job) return 10;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
        Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        Limits.BasicLimitInformation.ActiveProcessLimit = 1;
        if(!SetInformationJobObject(Job,JobObjectExtendedLimitInformation,&Limits,sizeof(Limits))
            || !AssignProcessToJobObject(Job,GetCurrentProcess())) { CloseHandle(Job); return 11; }
        WebProcessRuntime Runtime; Runtime.root_ = Args[2];
        for(const auto* Name : {"HyperEditorWeb.exe", "mode.txt"}) {
            QFile File(Runtime.root_+'/'+Name); if(!File.open(QIODevice::ReadOnly)) { CloseHandle(Job); return 12; }
            Runtime.sha256_[Name] = QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256).toHex();
        }
        WebProcessError Error;
        auto Owner = DocumentWebProcess::Start(Runtime,{4,512ULL*1024*1024,1500},0,Error);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
        const bool Unchanged = QueryInformationJobObject(Job,JobObjectBasicAccountingInformation,&Accounting,sizeof(Accounting),nullptr)
            && Accounting.TotalProcesses == 1 && Accounting.ActiveProcesses == 1;
        std::printf("%s\n",Error.code_.toUtf8().constData()); std::fflush(stdout);
        CloseHandle(Job);
        return !Owner && Unchanged && Error.code_ == "web-owner-parent-process-limit" ? 0 : 13;
    }
    if(Args.size()==3 && Args[1]=="--owner-death-probe")
    {
        WebProcessRuntime Runtime; Runtime.root_=Args[2];
        for(auto Name:{"HyperEditorWeb.exe","mode.txt"})
        {
            QFile File(Runtime.root_+'/'+Name); if(!File.open(QIODevice::ReadOnly)) return 2;
            Runtime.sha256_[Name]=QCryptographicHash::hash(File.readAll(),QCryptographicHash::Sha256).toHex();
        }
        WebProcessError Error; auto Owner=DocumentWebProcess::Start(Runtime,{4,512ULL*1024*1024,1500},0,Error);
        if(!Owner) return 3;
        QByteArray Reply; if(!Owner->Exchange("spawn",Reply,1000,Error)) return 4;
        std::printf("%lu,%s\n",static_cast<unsigned long>(Owner->Snapshot().processId_),Reply.constData()); std::fflush(stdout);
        Sleep(60000); return 5;
    }
    DocumentWebProcessTests Tests; return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstDocumentWebProcess.moc"
