#include "stdafx.h"
#include "UniqueLibs/fileLockService.hpp"
#include "UniqueLibs/nativeFileLocks.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "commonLibs/operations/fileOperation.hpp"
#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <iostream>

using namespace hc::filelocks;
using namespace hc::storage;
using namespace hc::operations;

namespace {
void write(const QString& Path) { QFile File(Path); if (File.open(QIODevice::WriteOnly)) File.write("disposable owned fixture\n"); }
struct Holder {
    QProcess process_;
    QJsonObject ready_;
    Holder(const QString& Path, int Share = 1, const QString& Mode = "file", const QString& Program = {}) {
        const auto Fixture = qEnvironmentVariable("HC_FILE_LOCK_HOLDER", QCoreApplication::applicationFilePath());
        process_.start(Program.isEmpty() ? Fixture : Program, {"--hold",Path,QString::number(Share),Mode});
        if (process_.waitForStarted(5000) && process_.waitForReadyRead(5000)) ready_ = QJsonDocument::fromJson(process_.readLine()).object();
    }
    QJsonObject Command(const QByteArray& Command) {
        process_.write(Command + '\n'); process_.waitForBytesWritten(1000);
        if (!process_.canReadLine()) process_.waitForReadyRead(5000);
        return QJsonDocument::fromJson(process_.readLine()).object();
    }
    ~Holder() { if (process_.state() != QProcess::NotRunning) { process_.write("release\n"); process_.waitForBytesWritten(1000); if (!process_.waitForFinished(5000)) { process_.kill(); process_.waitForFinished(5000); } } }
};
QJsonObject row(const QJsonObject& Result, qint64 Pid, const QString& Kind = "handle") {
    for (const auto& Value : Result["rows"].toArray()) {
        const auto Row = Value.toObject(); if (Row["pid"].toInt() == Pid && Row["kind"] == Kind) return Row;
    }
    return {};
}
std::shared_ptr<FileLockClient> session(const QString& Path) {
    return std::make_shared<FileLockClient>(QStringList{Path},false,
        qEnvironmentVariable("HC_FILE_LOCK_HELPER",QCoreApplication::applicationFilePath()));
}
JobStep advanceToStop(PreparedJob& Job, JobStepContext& Context) {
    for(int I=0;I<10000;++I){
        const auto Step=Job.task_->Advance(Context); Context.answer_.reset(); Context.committing_=Step.next_==JobState::Committing;
        if(Step.next_==JobState::WaitingForUser||IsTerminal(Step.next_))return Step;
    }
    return JobStep{JobState::Failed};
}
}

class FileLockTests : public QObject {
    Q_OBJECT
private slots:
    void localPathsOnly() {
        QVERIFY(!IsLocalLockPath("")); QVERIFY(!IsLocalLockPath("C:relative"));
        QVERIFY(!IsLocalLockPath("\\\\server\\share\\file")); QVERIFY(!IsLocalLockPath("C:/file:stream"));
        QVERIFY(!IsLocalLockPath("C:/a/../b"));
        QTemporaryDir Root; QVERIFY(IsLocalLockPath(Root.path()));
    }
    void closesOnlySelectedHandleAndDeduplicates() {
        QTemporaryDir Root; const auto Path=Root.filePath("locked.txt"); write(Path); Holder Owner(Path);
        QVERIFY(Owner.ready_["ready"].toBool()); auto Client=session(Path); const auto Found=Client->Query();
        QCOMPARE(Found["outcome"].toString(),QString("Completed")); const auto Row=row(Found,Owner.process_.processId());
        QVERIFY2(!Row.isEmpty(),qPrintable(QString::fromUtf8(QJsonDocument(Found).toJson()))); QVERIFY(Row["closeAllowed"].toBool());
        const auto Request=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto Closed=Client->Act(Row["capability"].toString(),Found["generation"].toString(),"close",Request);
        QCOMPARE(Closed["outcome"].toString(),QString("Completed"));
        QCOMPARE(Owner.process_.state(),QProcess::Running); QVERIFY(!Owner.Command("status")["valid"].toBool());
        QCOMPARE(Client->Act(Row["capability"].toString(),Found["generation"].toString(),"close",Request)["outcome"].toString(),QString("Completed"));
        QCOMPARE(Client->Act(Row["capability"].toString(),Found["generation"].toString(),"close",QUuid::createUuid().toString(QUuid::WithoutBraces))["outcome"].toString(),QString("Rejected"));
        QVERIFY(QFile::rename(Path,Path+".renamed")); QVERIFY(QFile::remove(Path+".renamed"));
    }
    void sharedDeleteIsUsageNotProofOfBlocking() {
        QTemporaryDir Root; const auto Path=Root.filePath("shared.txt"); write(Path); Holder Owner(Path,7);
        QVERIFY(Owner.ready_["ready"].toBool()); const auto Found=session(Path)->Query();
        QVERIFY(!row(Found,Owner.process_.processId()).isEmpty());
        QVERIFY(QFile::rename(Path,Path+".renamed")); QVERIFY(QFile::rename(Path+".renamed",Path)); QVERIFY(QFile::remove(Path));
        QVERIFY(Owner.Command("status")["valid"].toBool());
    }
    void folderFindsDescendantAndImageIsNotAHandle() {
        QTemporaryDir Root; const auto Path=Root.filePath("child.txt"); write(Path); Holder Owner(Path);
        QVERIFY(Owner.ready_["ready"].toBool()); auto Client=session(Root.path());const auto Found=Client->Query();
        QVERIFY(!row(Found,Owner.process_.processId()).isEmpty());
        Client->Cancel(); Client.reset();
        const auto ImagePath=QCoreApplication::applicationFilePath();
        Holder Image(ImagePath,1,"image"); QVERIFY(Image.ready_["ready"].toBool());
        const auto Images=session(ImagePath)->Query(); const auto Ref=row(Images,Image.process_.processId(),"image");
        QVERIFY(!Ref.isEmpty()); QVERIFY(!Ref["closeAllowed"].toBool());
    }
    void rejectsStaleCapabilityAndReusedHandle() {
        QTemporaryDir Root; const auto Path=Root.filePath("old.txt"), Other=Root.filePath("other.txt");write(Path);write(Other);Holder Owner(Path);
        QVERIFY(Owner.ready_["ready"].toBool());auto Client=session(Path);const auto Found=Client->Query();const auto Ref=row(Found,Owner.process_.processId());
        QVERIFY(!Ref.isEmpty());
        auto Forged=Client->Act(QUuid::createUuid().toString(QUuid::WithoutBraces),Found["generation"].toString(),"close",QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCOMPARE(Forged["outcome"].toString(),QString("Rejected"));
        const auto Reused=Owner.Command("reuse "+Other.toUtf8());QVERIFY(Reused["valid"].toBool());
        const auto Closed=Client->Act(Ref["capability"].toString(),Found["generation"].toString(),"close",QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(Closed["outcome"]=="TargetChanged"||Closed["outcome"]=="AlreadyGone"); QVERIFY(Owner.Command("status")["valid"].toBool());
    }
    void terminatesOnlyChosenDisposableProcess() {
        QTemporaryDir Root;const auto Path=Root.filePath("terminated.txt");write(Path);Holder Owner(Path);
        QVERIFY(Owner.ready_["ready"].toBool());auto Client=session(Path);const auto Found=Client->Query();const auto Ref=row(Found,Owner.process_.processId());
        QVERIFY(Ref["terminateAllowed"].toBool());
        QCOMPARE(Client->Act(Ref["capability"].toString(),Found["generation"].toString(),"terminate",QUuid::createUuid().toString(QUuid::WithoutBraces))["outcome"].toString(),QString("Completed"));
        QVERIFY(Owner.process_.waitForFinished(5000)||Owner.process_.state()==QProcess::NotRunning);QVERIFY(QFile::remove(Path));
    }
    void cancellationAndUntrustedCaller() {
        QTemporaryDir Root;const auto Path=Root.filePath("cancel.txt");write(Path);auto Client=session(Path);Client->Cancel();
        QCOMPARE(Client->Query()["outcome"].toString(),QString("Unavailable"));
        const QJsonObject Request{{"version",1},{"nonce",QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"pid",int(GetCurrentProcessId())},
            {"created",ProcessCreationTime(GetCurrentProcess())},{"callerSid",ProcessUserSid(GetCurrentProcess())},{"paths",QJsonArray{Path}}};
        QCOMPARE(ExecuteFileLockSession(Request,Root.filePath("untrusted.exe")),DWORD(ERROR_ACCESS_DENIED));
    }
    void disconnectedActionIsUnknownAndNotReissued() {
        QTemporaryDir Root;const auto Path=Root.filePath("unknown.txt");write(Path);Holder Owner(Path);QVERIFY(Owner.ready_["ready"].toBool());
        HANDLE Observed=nullptr;
        const auto Cleanup=qScopeGuard([&]{if(Observed)CloseHandle(Observed);});
        auto Launcher=[&](const QString& Program,const QString& Arguments,bool)->void*{
            auto Command=('"'+QDir::toNativeSeparators(Program)+"\" "+Arguments).toStdWString();
            STARTUPINFOW Startup{};Startup.cb=sizeof(Startup);PROCESS_INFORMATION Process{};
            if(!CreateProcessW(reinterpret_cast<LPCWSTR>(Program.utf16()),Command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process))return nullptr;
            CloseHandle(Process.hThread);DuplicateHandle(GetCurrentProcess(),Process.hProcess,GetCurrentProcess(),&Observed,0,FALSE,DUPLICATE_SAME_ACCESS);return Process.hProcess;
        };
        auto Client=std::make_shared<FileLockClient>(QStringList{Path},false,QCoreApplication::applicationFilePath(),Launcher);
        const auto Found=Client->Query();const auto Ref=row(Found,Owner.process_.processId());QVERIFY(!Ref.isEmpty());QVERIFY(Observed);
        QVERIFY(TerminateProcess(Observed,77));QCOMPARE(WaitForSingleObject(Observed,5000),DWORD(WAIT_OBJECT_0));
        const auto Result=Client->Act(Ref["capability"].toString(),Found["generation"].toString(),"close",QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCOMPARE(Result["outcome"].toString(),QString("Unknown"));QVERIFY(Owner.Command("status")["valid"].toBool());
    }
    void waitingRenameDeleteAndMove_data() {
        QTest::addColumn<int>("Operation");
        QTest::newRow("rename")<<int(hc::operations::FileOperation::Rename);QTest::newRow("delete")<<int(hc::operations::FileOperation::Delete);QTest::newRow("move-source-cleanup")<<int(hc::operations::FileOperation::Move);
    }
    void waitingRenameDeleteAndMove() {
        QFETCH(int,Operation);QTemporaryDir Root;const auto Path=Root.filePath("source.txt");write(Path);QVERIFY(QDir(Root.path()).mkdir("destination"));
        const auto Native=QDir::toNativeSeparators(Path).toStdWString();HANDLE Held=CreateFileW(Native.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(Held!=INVALID_HANDLE_VALUE);const auto Guard=qScopeGuard([&]{if(Held!=INVALID_HANDLE_VALUE)CloseHandle(Held);});
        auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);
        auto Source=Reader->Resolve({LocatorKind::Local,Path.toStdString()},{}).get();QVERIFY(Source.HasValue());
        FileOperationRequest Request;Request.operation_=hc::operations::FileOperation(Operation);Request.sources_={{Source.Value(),Reader,Writer}};
        Request.fileLockInteractions_=true;Request.confirmDelete_=false;Request.destinationWriter_=Writer;
        Request.destination_={LocatorKind::Local,Root.filePath(Operation==int(hc::operations::FileOperation::Rename)?"renamed.txt":"destination").toStdString()};
        auto Prepared=PrepareFileOperation(Request);QVERIFY(Prepared.HasValue());auto Job=std::move(Prepared.Value());JobStepContext Context;Context.jobId_="owned-lock-test";
        const auto Cleanup=qScopeGuard([&]{Job.task_->Finalize(JobState::Cancelled);});
        auto Step=advanceToStop(Job,Context);QCOMPARE(Step.next_,JobState::WaitingForUser);QVERIFY(Step.interaction_);QVERIFY(Step.interaction_->reason_.starts_with("file-in-use:"));
        const auto Destination=Root.filePath("destination/source.txt");
        if(Operation==int(hc::operations::FileOperation::Move))QVERIFY(QFile::exists(Destination));
        CloseHandle(Held);Held=INVALID_HANDLE_VALUE;Context.answer_=InteractionAnswer{"",InteractionChoice::Retry,{}};
        Step=advanceToStop(Job,Context);QCOMPARE(Step.next_,JobState::Completed);
        QVERIFY(!QFile::exists(Path));
        if(Operation==int(hc::operations::FileOperation::Rename))QVERIFY(QFile::exists(Root.filePath("renamed.txt")));
        if(Operation==int(hc::operations::FileOperation::Move))QVERIFY(QFile::exists(Destination));
    }
    void changedSourceIsNotDeletedAfterWait() {
        QTemporaryDir Root;const auto Path=Root.filePath("source.txt");write(Path);Holder Owner(Path);QVERIFY(Owner.ready_["ready"].toBool());
        auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);auto Source=Reader->Resolve({LocatorKind::Local,Path.toStdString()},{}).get();QVERIFY(Source.HasValue());
        FileOperationRequest Request;Request.operation_=hc::operations::FileOperation::Delete;Request.sources_={{Source.Value(),Reader,Writer}};Request.fileLockInteractions_=true;Request.confirmDelete_=false;
        auto Prepared=PrepareFileOperation(Request);QVERIFY(Prepared.HasValue());auto Job=std::move(Prepared.Value());JobStepContext Context;
        const auto Cleanup=qScopeGuard([&]{Job.task_->Finalize(JobState::Cancelled);});
        QCOMPARE(advanceToStop(Job,Context).next_,JobState::WaitingForUser);
        Owner.process_.write("release\n");Owner.process_.waitForBytesWritten(1000);QVERIFY(Owner.process_.waitForFinished(5000));
        QVERIFY(QFile::rename(Path,Path+".original"));write(Path);
        for (int Attempt = 0; Attempt < 2; ++Attempt) {
            Context.answer_=InteractionAnswer{"",InteractionChoice::Retry,{}};
            const auto Step=advanceToStop(Job,Context);
            QCOMPARE(Step.next_,JobState::WaitingForUser);QVERIFY(Step.interaction_);
            QCOMPARE(Step.interaction_->kind_,InteractionKind::RetryFailure);
            QCOMPARE(Step.interaction_->reason_,std::string("delete-failure:revision-changed"));
            QVERIFY(QFile::exists(Path));QVERIFY(QFile::exists(Path+".original"));
        }
        Context.answer_=InteractionAnswer{"",InteractionChoice::Skip,{}};
        QCOMPARE(advanceToStop(Job,Context).next_,JobState::Completed);
        QVERIFY(QFile::exists(Path));QVERIFY(QFile::exists(Path+".original"));
    }
    void waitingSkipAndCancel_data() {
        QTest::addColumn<bool>("Move");QTest::addColumn<bool>("Skip");
        QTest::newRow("delete-skip")<<false<<true;QTest::newRow("delete-cancel")<<false<<false;
        QTest::newRow("move-skip")<<true<<true;QTest::newRow("move-cancel")<<true<<false;
    }
    void waitingSkipAndCancel() {
        QFETCH(bool,Move);QFETCH(bool,Skip);QTemporaryDir Root;const auto Path=Root.filePath("source.txt");write(Path);QVERIFY(QDir(Root.path()).mkdir("destination"));
        const auto Native=QDir::toNativeSeparators(Path).toStdWString();HANDLE Held=CreateFileW(Native.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(Held!=INVALID_HANDLE_VALUE);const auto Close=qScopeGuard([&]{CloseHandle(Held);});
        auto Reader=MakeNativeReadProvider(LocatorKind::Local);auto Writer=MakeNativeWritableStorage(LocatorKind::Local,Reader);
        const auto Source=Reader->Resolve({LocatorKind::Local,Path.toStdString()},{}).get();QVERIFY(Source.HasValue());
        FileOperationRequest Request;Request.operation_=Move?hc::operations::FileOperation::Move:hc::operations::FileOperation::Delete;
        Request.sources_={{Source.Value(),Reader,Writer}};Request.fileLockInteractions_=true;Request.confirmDelete_=false;
        Request.destinationWriter_=Writer;Request.destination_={LocatorKind::Local,Root.filePath("destination").toStdString()};
        auto Prepared=PrepareFileOperation(Request);QVERIFY(Prepared.HasValue());auto Job=std::move(Prepared.Value());JobStepContext Context;
        const auto Cleanup=qScopeGuard([&]{Job.task_->Finalize(Skip?JobState::Completed:JobState::Cancelled);});
        QCOMPARE(advanceToStop(Job,Context).next_,JobState::WaitingForUser);
        Context.answer_=InteractionAnswer{"",Skip?InteractionChoice::Skip:InteractionChoice::Cancel,{}};
        QCOMPARE(advanceToStop(Job,Context).next_,Skip?JobState::Completed:JobState::Cancelled);
        QVERIFY(QFile::exists(Path));if(Move)QVERIFY(QFile::exists(Root.filePath("destination/source.txt")));
    }
};

int main(int Argc,char** Argv) {
    QCoreApplication App(Argc,Argv);const auto Args=App.arguments();
    if(Args.size()==3&&Args[1]=="--file-lock-session"){
        const auto Request=QJsonDocument::fromJson(QByteArray::fromBase64(Args[2].toLatin1(),QByteArray::Base64UrlEncoding)).object();
        return int(ExecuteFileLockSession(Request,QCoreApplication::applicationFilePath()));
    }
    if(Args.size()==5&&Args[1]=="--hold"){
        HANDLE File=INVALID_HANDLE_VALUE;const auto Path=QDir::toNativeSeparators(Args[2]).toStdWString();
        if(Args[4]!="image")File=CreateFileW(Path.c_str(),GENERIC_READ,DWORD(Args[3].toUInt()),nullptr,OPEN_EXISTING,Args[4]=="directory"?FILE_FLAG_BACKUP_SEMANTICS:0,nullptr);
        const bool Ready=File!=INVALID_HANDLE_VALUE||Args[4]=="image";
        std::cout<<QJsonDocument(QJsonObject{{"ready",Ready},{"pid",int(GetCurrentProcessId())}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        std::string Command;
        while(std::getline(std::cin,Command)&&Command!="release"){
            if(Command.starts_with("reuse ")){if(File!=INVALID_HANDLE_VALUE)CloseHandle(File);const auto Other=QDir::toNativeSeparators(QString::fromStdString(Command.substr(6))).toStdWString();File=CreateFileW(Other.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);}
            BY_HANDLE_FILE_INFORMATION Info{};const bool Valid=File!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(File,&Info);
            std::cout<<QJsonDocument(QJsonObject{{"valid",Valid}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        }
        // The selected handle may have been remotely closed and reused. Avoid a
        // second close in the holder fixture; process teardown owns the remainder.
        return Ready?0:3;
    }
    FileLockTests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstFileLocks.moc"
