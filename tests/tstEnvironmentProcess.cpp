#include "stdafx.h"
#include "tstEnvironmentProcess.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentProcessProtocol.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentProcessTests, "hc.tests.environmentProcess")
using namespace hc::environment;
void EnvironmentProcessTests::chunkedSnapshot()
{
    int Reads = 0, Chunks = 0, Releases = 0;
    QString Id;
    ProcessEnvironmentServer Server([&](const hc::system::ProcessKey& Key) {
        ++Reads;
        ProcessEnvironment Result;
        if (Key.pid != 123 || Key.createTime != 456) return Result;
        Result.status_ = ProcessReadStatus::Ok;
        Result.values_[L"SYNTHETIC_LARGE"] = std::wstring(70000, L'x');
        Result.values_[L"=C:"] = L"C:\\Synthetic";
        Result.currentDirectory_ = L"C:\\Synthetic";
        Result.wow64_ = true;
        return Result;
    });
    const auto Result = ReadRemoteEnvironment({123, 456}, [&](const QString& Kind, QJsonObject Request) {
        if (Kind.endsWith(".chunk")) ++Chunks;
        if (Kind.endsWith(".release")) ++Releases;
        const auto Reply = Server.Handle(Kind, Request);
        if (Kind == "process.environment") Id = Reply["snapshot"].toString();
        return Reply;
    });
    QCOMPARE(Result.status_, ProcessReadStatus::Ok);
    QCOMPARE(Result.values_.at(L"SYNTHETIC_LARGE").size(), size_t(70000));
    QCOMPARE(Result.values_.at(L"=C:"), std::wstring(L"C:\\Synthetic"));
    QVERIFY(Result.wow64_);
    QCOMPARE(Reads, 1); QVERIFY(Chunks > 1); QCOMPARE(Releases, 1);
    QCOMPARE(Server.Handle("process.environment.chunk", {{"snapshot", Id}, {"offset", 0}})["outcome"].toString(), QString("Failed"));
}
void EnvironmentProcessTests::rejectsCorruptSnapshot()
{
    for (const int Mode : {0, 1, 2}) {
        int Releases = 0;
        ProcessEnvironmentServer Server([](const hc::system::ProcessKey&) {
            ProcessEnvironment Result; Result.status_ = ProcessReadStatus::Ok;
            Result.values_[L"SYNTHETIC"] = L"example"; return Result;
        });
        const auto Result = ReadRemoteEnvironment({123, 456}, [&](const QString& Kind, QJsonObject Request) {
            auto Reply = Server.Handle(Kind, Request);
            if (Kind.endsWith(".release")) ++Releases;
            if (Kind == "process.environment" && Mode == 0) Reply["sha256"] = QString(64, '0');
            if (Kind.endsWith(".chunk") && Mode == 1) Reply["offset"] = 1;
            if (Kind.endsWith(".chunk") && Mode == 2) Reply["data"] = "!invalid!";
            return Reply;
        });
        QVERIFY(Result.status_ != ProcessReadStatus::Ok); QVERIFY(Result.values_.empty()); QCOMPARE(Releases, 1);
    }
}
void EnvironmentProcessTests::rejectsInvalidIdentity()
{
    int Reads = 0;
    ProcessEnvironmentServer Server([&](const hc::system::ProcessKey&) { ++Reads; return ProcessEnvironment{}; });
    QCOMPARE(Server.Handle("process.environment", {{"pid", 0}, {"created", "1"}})["outcome"].toString(), QString("Failed"));
    QCOMPARE(Server.Handle("process.environment", {{"pid", 123}, {"created", "0"}})["outcome"].toString(), QString("Failed"));
    QCOMPARE(Server.Handle("process.environment", {{"pid", 123}, {"created", "invalid"}})["outcome"].toString(), QString("Failed"));
    QCOMPARE(Reads, 0);
}
void EnvironmentProcessTests::propagatesReadFailure()
{
    ProcessEnvironmentServer Server([](const hc::system::ProcessKey&) {
        ProcessEnvironment Result; Result.status_ = ProcessReadStatus::Denied; Result.nativeError_ = ERROR_ACCESS_DENIED; return Result;
    });
    const auto Result = ReadRemoteEnvironment({123, 456}, [&](const QString& Kind, QJsonObject Request) { return Server.Handle(Kind, Request); });
    QCOMPARE(Result.status_, ProcessReadStatus::Denied); QCOMPARE(Result.nativeError_, quint32(ERROR_ACCESS_DENIED)); QVERIFY(Result.values_.empty());
}
QTEST_GUILESS_MAIN(EnvironmentProcessTests)
