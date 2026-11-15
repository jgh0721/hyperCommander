#include "stdafx.h"
#include "tstRegistryAutorunsNative.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/registry/registryAutoruns.hpp"
#include "UniqueLibs/systemSessionService.hpp"
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QElapsedTimer>
#include "UniqueLibs/registry/registryLinks.hpp"
#include "UniqueLibs/registry/registryLinkCache.hpp"
#include <QFileInfo>
#include <QCoreApplication>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QUuid>
#include <QDir>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryAutorunsNative, "hc.tests.registryAutorunsNative")
using namespace hc::registry;
void RegistryAutorunsNativeTests::initTestCase()
{
    if (qEnvironmentVariable("HC_REGISTRY_AUTORUN_NATIVE") != "task_4ceba737038e") QSKIP("Explicit owned VM runtime required");
    QCOMPARE(QSysInfo::machineHostName().toLower(), QString("imonagr-win11"));
    HANDLE Token = nullptr; QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)); TOKEN_ELEVATION Elevation{}; DWORD Length = 0;
    const bool Valid = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Length); CloseHandle(Token); QVERIFY(Valid && Elevation.TokenIsElevated);
}
void RegistryAutorunsNativeTests::approvals()
{
    for (const auto& Source : QList<RegistryLocation>{{Hive::CurrentUser, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"},
        {Hive::LocalMachine, "Software\\Microsoft\\Windows\\CurrentVersion\\Run"}, {Hive::LocalMachine, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", View::Wow32}}) {
        WinRegistryBackend Backend; const auto Target = StartupApprovedCodec::LocationFor(Source).value();
        const auto Name = "HyperCommanderTestRegistryP6-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const RegistryValue Command{Name, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, "\"C:\\Windows\\System32\\cmd.exe\" /c exit 0").value};
        const auto Original = Backend.Read(Source, Name), Approval = Backend.Read(Target, Name);
        QVERIFY(Original.error == ERROR_FILE_NOT_FOUND || Original.error == ERROR_PATH_NOT_FOUND);
        QVERIFY(Approval.error == ERROR_FILE_NOT_FOUND || Approval.error == ERROR_PATH_NOT_FOUND);
        const bool SourceExisted = bool(Backend.Enumerate(Source)), TargetExisted = bool(Backend.Enumerate(Target));
        QCOMPARE(Backend.Create(Source), quint32(0));
        const auto Cleanup = qScopeGuard([&] {
            const auto Current = Backend.Read(Source, Name), Approved = Backend.Read(Target, Name);
            if (Current && Current.value != Command) { QTest::qFail("External change in owned Run value; retain", __FILE__, __LINE__); return; }
            if (Approved && StartupApprovedCodec::Decode(Approved.value).state == StartupApproval::State::Unknown) { QTest::qFail("Unknown approval change; retain", __FILE__, __LINE__); return; }
            if (Current) Backend.DeleteValue(Source, Name); if (Approved) Backend.DeleteValue(Target, Name);
            if (!SourceExisted) Backend.DeleteKey(Source); if (!TargetExisted) Backend.DeleteKey(Target);
            if (Backend.Read(Source, Name) || Backend.Read(Target, Name)) QTest::qFail("Autorun fixture restoration failed", __FILE__, __LINE__);
        });
        QCOMPARE(Backend.Write(Source, Command), quint32(0));
        hc::admin::SystemSessionClient Client(QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommanderAdmin.exe"));
        RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, Payload); });
        QTemporaryDir Directory; RegistryChangeStore Store(Directory.path());
        RegistryMutation Mutation(Backend, Store, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()), &Remote);
        RegistryAutorunEntry Entry; Entry.location = Source; Entry.value = Command; Entry.switchLocation = Target; Entry.canToggle = true;
        FILETIME Time{}; GetSystemTimeAsFileTime(&Time); const quint64 Ticks = (quint64(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
        const auto Disabled = RegistryAutoruns::Toggle(Mutation, Entry, Ticks); QCOMPARE(Disabled.applied.error, quint32(0));
        QCOMPARE(Backend.Read(Source, Name).value, Command); const auto Raw = Backend.Read(Target, Name); QVERIFY(Raw);
        QCOMPARE(StartupApprovedCodec::Decode(Raw.value).state, StartupApproval::State::Disabled); QCOMPARE(StartupApprovedCodec::Decode(Raw.value).disabledAt, Ticks);
        const auto Undo = Mutation.Undo(Disabled.record); QCOMPARE(Undo.applied.error, quint32(0)); QVERIFY(!Backend.Read(Target, Name));
        const auto Redo = Mutation.Redo(Disabled.record, Undo.record); QCOMPARE(Redo.applied.error, quint32(0)); QCOMPARE(Backend.Read(Target, Name).value, Raw.value);
        Entry.enabled = false; Entry.switchValue = Raw.value; const auto Enabled = RegistryAutoruns::Toggle(Mutation, Entry, Ticks); QCOMPARE(Enabled.applied.error, quint32(0));
        QCOMPARE(Backend.Read(Target, Name).value.bytes, QByteArray::fromHex("020000000000000000000000")); QCOMPARE(Backend.Read(Source, Name).value, Command);
        Client.Close();
    }
}
void RegistryAutorunsNativeTests::shellBlock()
{
    WinRegistryBackend Backend;
    const auto Id = QUuid::createUuid().toString(); const auto Name = "HyperCommanderTestRegistryP6-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const RegistryLocation Parent{Hive::CurrentUser, "Software\\HyperCommanderTests"}; QCOMPARE(Backend.Create(Parent), quint32(0));
    const auto Source = Parent.Child(Name); QCOMPARE(Backend.Create(Source), quint32(0));
    const RegistryValue Registration{{}, REG_SZ, RegistryValueCodec::EncodeText(REG_SZ, Id).value}; QCOMPARE(Backend.Write(Source, Registration), quint32(0));
    const RegistryLocation Target{Hive::LocalMachine, "Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked"};
    const auto Original = Backend.Read(Target, Id); QVERIFY(Original.error == ERROR_FILE_NOT_FOUND || Original.error == ERROR_PATH_NOT_FOUND);
    const bool Existed = bool(Backend.Enumerate(Target));
    const auto Cleanup = qScopeGuard([&] {
        const auto Current = Backend.Read(Target, Id);
        if (Current && (Current.value.type != REG_SZ || Current.value.bytes != QByteArray(2, char(0)))) { QTest::qFail("Owned CLSID block changed externally; retain", __FILE__, __LINE__); return; }
        if (Current) Backend.DeleteValue(Target, Id); if (!Existed) Backend.DeleteKey(Target);
        Backend.DeleteValue(Source, {}); Backend.DeleteKey(Source);
        if (Backend.Read(Target, Id) || Backend.Enumerate(Source)) QTest::qFail("Block fixture restoration failed", __FILE__, __LINE__);
    });
    hc::admin::SystemSessionClient Client(QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommanderAdmin.exe"));
    RegistryRemoteSession Remote([&](const QString& Kind, QJsonObject Payload) { return Client.Request(Kind, Payload); });
    QTemporaryDir Directory; RegistryChangeStore Store(Directory.path()); RegistryMutation Mutation(Backend, Store,
        QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()), &Remote);
    RegistryAutorunEntry Entry; Entry.location = Source; Entry.value = Registration; Entry.clsid = Id; Entry.switchLocation = Target; Entry.canToggle = true;
    const auto Block = RegistryAutoruns::Toggle(Mutation, Entry, 0); QCOMPARE(Block.applied.error, quint32(0)); QVERIFY(Backend.Read(Target, Id));
    QCOMPARE(Backend.Read(Source, {}).value, Registration);
    const auto Undo = Mutation.Undo(Block.record); QCOMPARE(Undo.applied.error, quint32(0)); QVERIFY(!Backend.Read(Target, Id));
    Client.Close();
}
void RegistryAutorunsNativeTests::linkIndex()
{
    WinRegistryBackend Backend;
    for (const auto Bits : {View::Native64, View::Wow32}) {
        QElapsedTimer Timer; Timer.start(); std::atomic<qsizetype> Progress{0};
        const auto Index = RegistryLinkResolver::BuildIndex(Backend, Bits, nullptr, 100000, {}, &Progress);
        qsizetype Edges = 0; for (const auto& Incoming : Index.incoming) Edges += Incoming.size();
        qInfo() << "index view" << int(Bits) << "keys" << Index.keys << "edges" << Edges << "ms" << Timer.elapsed() << "complete" << Index.complete << "diagnostics" << Index.diagnostics.size();
        QVERIFY(Index.keys > 100); QCOMPARE(Progress.load(), Index.keys);
        std::atomic_bool Cancel{true}; QCOMPARE(RegistryLinkResolver::BuildIndex(Backend, Bits, &Cancel).keys, qsizetype(0));
    }
}
int main(int Count, char** Arguments)
{
    QCoreApplication Application(Count, Arguments); RegistryAutorunsNativeTests Tests; return QTest::qExec(&Tests, Count, Arguments);
}

void RegistryAutorunsNativeTests::stagedIndex()
{
    WinRegistryBackend Backend; QTemporaryDir Directory;
    const auto Sid = QString::fromStdWString(hc::system::CurrentUserSid());
    const bool Background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const auto Priority = qScopeGuard([&] { if (Background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); });
    for (const auto Bits : {View::Native64, View::Wow32}) {
        RegistryLinkCache Cache(Bits); QElapsedTimer Total; Total.start(); bool FastLogged = false;
        while (!Cache.Complete()) {
            Cache.Advance(Backend, nullptr, 100000, 1000);
            if (Cache.FastComplete() && !FastLogged) { FastLogged = true; qInfo() << "fast view" << int(Bits) << "keys" << Cache.Keys() << "ms" << Total.elapsed(); }
        }
        const auto Path = Directory.filePath(QString::number(int(Bits)) + ".json");
        const auto FullMs = Total.elapsed(); QVERIFY(Cache.Save(Path, Sid));
        qInfo() << "full view" << int(Bits) << "keys" << Cache.Keys() << "ms" << FullMs << "cache bytes" << QFileInfo(Path).size();
        Total.restart(); RegistryLinkCache Warm(Bits); QVERIFY(Warm.Load(Path, Sid));
        const auto LoadMs = Total.elapsed(); Warm.Validate();
        while (!Warm.Complete()) Warm.Advance(Backend, nullptr, 100000, 1000);
        qInfo() << "warm view" << int(Bits) << "load ms" << LoadMs << "validate ms" << Total.elapsed() << "reread keys" << Warm.Reads();
        QVERIFY(Warm.Keys() > 100); QVERIFY(Warm.FastComplete()); QVERIFY(Warm.Complete());
    }
}