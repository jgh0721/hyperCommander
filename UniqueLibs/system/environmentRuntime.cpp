#include "stdafx.h"
#include "environmentRuntime.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <cstdlib>
#include <stdexcept>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentRuntime, "hc.environment.runtime")
namespace hc::environment {
ValueMap EffectiveEnvironment(const MergeResult& Result)
{
    ValueMap Values;
    for (const auto& [Name, Item] : Result.values_) Values[Name] = Item.value_;
    return Values;
}
EnvironmentRuntime::EnvironmentRuntime(ValueMap Inherited, ValueMap InitialModel, ValueMap Overlay, Reader Read, Writer Write)
    : inherited_(std::move(Inherited)), lastModel_(std::move(InitialModel)), overlay_(std::move(Overlay)),
      published_(inherited_), read_(std::move(Read)), write_(std::move(Write))
{
    if (!read_ || !write_) throw std::invalid_argument("environment runtime callbacks required");
}
quint32 EnvironmentRuntime::Rebuild(const ValueMap& NextModel)
{
    std::lock_guard Lock(mutex_);
    const auto Before = read_();
    const auto Next = ReconcileEnvironment(inherited_, lastModel_, NextModel, Before, overlay_);
    std::vector<std::pair<std::wstring, std::optional<std::wstring>>> Applied;
    auto Apply = [&](const std::wstring& Name, std::optional<std::wstring> Text) {
        const auto Previous = Before.find(Name);
        if (Previous != Before.end() && Text && Previous->second == *Text) return quint32(0);
        Applied.emplace_back(Name, Previous == Before.end() ? std::nullopt : std::optional(Previous->second));
        const auto Error = write_(Name, Text);
        return Error;
    };
    quint32 Error = 0;
    for (const auto& [Name, Text] : Before) if (!Next.contains(Name)) { Error = Apply(Name, {}); if (Error) break; }
    if (!Error) for (const auto& [Name, Text] : Next) { Error = Apply(Name, Text); if (Error) break; }
    if (Error) {
        // Best effort rollback; the published child snapshot remains coherent
        // even if an external DLL interferes with process-wide state.
        for (auto It = Applied.rbegin(); It != Applied.rend(); ++It) write_(It->first, It->second);
        return Error;
    }
    lastModel_ = NextModel; published_ = Next; ++generation_; return 0;
}
QProcessEnvironment EnvironmentRuntime::ChildEnvironment() const
{
    std::lock_guard Lock(mutex_);
    QProcessEnvironment Result;
    for (const auto& [Name, Text] : published_) Result.insert(QString::fromStdWString(Name), QString::fromStdWString(Text));
    return Result;
}
ValueMap EnvironmentRuntime::Snapshot() const { std::lock_guard Lock(mutex_); return published_; }
quint64 EnvironmentRuntime::Generation() const { std::lock_guard Lock(mutex_); return generation_; }
quint32 WriteOwnEnvironmentValue(const std::wstring& Name, const std::optional<std::wstring>& Text)
{
    // CRT and Windows keep different copies; update both, including the valid
    // empty-string case which _wputenv_s alone interprets as deletion.
    if (Name.empty() || Name.front() == L'=') return 0;
    const auto Before = ReadOwnEnvironment();
    const auto Old = Before.find(Name);
    const std::optional<std::wstring> Previous = Old == Before.end() ? std::nullopt : std::optional(Old->second);
    wchar_t* Crt = nullptr; size_t Length = 0;
    if (_wdupenv_s(&Crt, &Length, Name.c_str()) != 0) return ERROR_NOT_ENOUGH_MEMORY;
    const std::optional<std::wstring> PreviousCrt = Crt ? std::optional<std::wstring>(Crt) : std::nullopt;
    free(Crt);
    quint32 Error = _wputenv_s(Name.c_str(), Text ? Text->c_str() : L"") == 0 ? 0 : ERROR_INVALID_PARAMETER;
    if (!Error && !SetEnvironmentVariableW(Name.c_str(), Text ? Text->c_str() : nullptr)) Error = GetLastError();
    if (Error) {
        _wputenv_s(Name.c_str(), PreviousCrt ? PreviousCrt->c_str() : L"");
        SetEnvironmentVariableW(Name.c_str(), Previous ? Previous->c_str() : nullptr);
        return Error;
    }
    return 0;
}
}
