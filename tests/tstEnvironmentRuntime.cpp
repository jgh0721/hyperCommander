#include "stdafx.h"
#include "tstEnvironmentRuntime.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentRuntime.hpp"
#include "commonLibs/system/childEnvironment.hpp"
#include "UniqueLibs/system/environmentPathProbe.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTest>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentRuntimeTests, "hc.tests.environmentRuntime")
using namespace hc::environment;
void EnvironmentRuntimeTests::rebuildPreservesOverlayAndDeletes()
{
    ValueMap Current{{L"SYNTHETIC_OLD",L"old"},{L"SYNTHETIC_INHERITED",L"inherited"},{L"SYNTHETIC_CHANGED",L"before"}};
    EnvironmentRuntime Runtime(Current, {{L"SYNTHETIC_OLD",L"old"},{L"SYNTHETIC_CHANGED",L"before"}}, {{L"COMMANDER_PATH",L"C:\\Synthetic"}},
        [&] { return Current; }, [&](const std::wstring& Name, const std::optional<std::wstring>& Value) {
            if (Value) Current[Name] = *Value; else Current.erase(Name); return quint32(0);
        });
    Current[L"SYNTHETIC_PLUGIN"] = L"runtime";
    QCOMPARE(Runtime.Rebuild({{L"SYNTHETIC_CHANGED",L"after"}}), quint32(0));
    QVERIFY(!Current.contains(L"SYNTHETIC_OLD"));
    QCOMPARE(Current.at(L"SYNTHETIC_CHANGED"), std::wstring(L"after"));
    QCOMPARE(Current.at(L"SYNTHETIC_PLUGIN"), std::wstring(L"runtime"));
    QCOMPARE(Runtime.ChildEnvironment().value("COMMANDER_PATH"), QString("C:\\Synthetic"));
    QCOMPARE(Runtime.Generation(), quint64(1));
    const auto Frozen = Runtime.ChildEnvironment();
    const auto Snapshot = Runtime.Snapshot();
    QCOMPARE(Runtime.Rebuild({{L"SYNTHETIC_CHANGED",L"newer"}}), quint32(0));
    QCOMPARE(Snapshot.at(L"SYNTHETIC_CHANGED"), std::wstring(L"after"));
    QCOMPARE(Frozen.value("SYNTHETIC_CHANGED"), QString("after"));
    QCOMPARE(Runtime.ChildEnvironment().value("SYNTHETIC_CHANGED"), QString("newer"));
}
void EnvironmentRuntimeTests::failedWriteKeepsPublishedGeneration()
{
    ValueMap Current{{L"SYNTHETIC_A",L"old"},{L"SYNTHETIC_Z",L"old"}};
    const auto Before = Current;
    EnvironmentRuntime Runtime(Current, Current, {}, [&] { return Current; },
        [&](const std::wstring& Name, const std::optional<std::wstring>& Value) {
            if (Name == L"SYNTHETIC_Z" && Value == std::optional<std::wstring>(L"new")) { Current[Name] = L"partially-written"; return quint32(ERROR_ACCESS_DENIED); }
            if (Value) Current[Name] = *Value; else Current.erase(Name); return quint32(0);
        });
    QCOMPARE(Runtime.Rebuild({{L"SYNTHETIC_A",L"new"},{L"SYNTHETIC_Z",L"new"}}), quint32(ERROR_ACCESS_DENIED));
    QVERIFY(Current == Before);
    QCOMPARE(Runtime.Generation(), quint64(0));
    QCOMPARE(Runtime.ChildEnvironment().value("SYNTHETIC_A"), QString("old"));
}
void EnvironmentRuntimeTests::childBlockIsImmutableAndTerminated()
{
    const auto Restore = ChildEnvironmentSnapshot();
    const auto Cleanup = qScopeGuard([&] { childEnvironmentSnapshot.store(Restore); });
    PublishChildEnvironment({{L"SYNTHETIC_B", L"two"}, {L"SYNTHETIC_A", L""}});
    const auto Frozen = ChildEnvironmentSnapshot();
    const auto Block = NativeChildEnvironment();
    const std::wstring First(Block.data());
    QCOMPARE(First, std::wstring(L"SYNTHETIC_A="));
    QCOMPARE(Block.back(), wchar_t(0)); QCOMPARE(Block[Block.size() - 2], wchar_t(0));
    PublishChildEnvironment({});
    QCOMPARE(NativeChildEnvironment().size(), size_t(2));
    QCOMPARE(Frozen->at(L"SYNTHETIC_B"), std::wstring(L"two"));
}
void EnvironmentRuntimeTests::pathBudgetDoesNotProbeAfterExpiry()
{
    struct Probe final : IPathProbe { int calls_ = 0; DirectoryProbe Inspect(const std::wstring&) override { ++calls_; return {ProbeStatus::Exists}; } } Inner;
    std::atomic_bool Cancel{false};
    BudgetedPathProbe Expired(Inner, &Cancel, std::chrono::milliseconds(0));
    QCOMPARE(Expired.Inspect(L"C:\\Synthetic").status_, ProbeStatus::Timeout); QCOMPARE(Inner.calls_, 0);
    BudgetedPathProbe Available(Inner, &Cancel);
    QCOMPARE(Available.Inspect(L"C:\\Synthetic").status_, ProbeStatus::Exists); QCOMPARE(Inner.calls_, 1);
    Cancel.store(true); QCOMPARE(Available.Inspect(L"C:\\Synthetic").status_, ProbeStatus::Cancelled); QCOMPARE(Inner.calls_, 1);
}
QTEST_GUILESS_MAIN(EnvironmentRuntimeTests)
