#include "stdafx.h"
#include "environmentSession.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "environmentStorage.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QThread>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentSession, "hc.environment.session")
namespace hc::environment {
EnvironmentSession::EnvironmentSession(std::shared_ptr<registry::IRegistryBackend> Backend, MergeInput Defaults,
    QString CallerSid, QObject* Parent) : QObject(Parent), defaults_(std::move(Defaults)), callerSid_(std::move(CallerSid))
{
    // A null backend must not silently turn a test into native registry access.
    if (!Backend) throw std::invalid_argument("environment backend required");
    service_ = new registry::RegistryService(std::move(Backend), this);
    connect(service_, &registry::RegistryService::Changed, this, [this] { publish(); });
}
bool EnvironmentSession::Pending() const
{
    for (auto Scope : {Scope::System, Scope::User}) {
        const auto* Entry = service_->Cached(EnvironmentLocation(Scope, callerSid_));
        if (!Entry || !Entry->loaded || Entry->pending) return true;
    }
    return false;
}
void EnvironmentSession::Refresh()
{
    Q_ASSERT(QThread::currentThread() == thread());
    refreshing_ = true;
    for (auto Scope : {Scope::System, Scope::User}) service_->Request(EnvironmentLocation(Scope, callerSid_), true);
    refreshing_ = false;
    emit LoadingChanged();
}
void EnvironmentSession::SetWatching(bool Enabled)
{
    service_->SetWatched(Enabled ? QList<registry::RegistryLocation>{EnvironmentLocation(Scope::User, callerSid_), EnvironmentLocation(Scope::System)} : QList<registry::RegistryLocation>{});
}
void EnvironmentSession::SetDefaults(MergeInput Defaults)
{
    defaults_ = std::move(Defaults); Refresh();
}
void EnvironmentSession::publish()
{
    if (refreshing_) return;
    emit LoadingChanged();
    if (Pending()) return;
    EnvironmentSnapshot Next; Next.input_ = defaults_;
    for (auto Scope : {Scope::System, Scope::User}) {
        const auto& Read = service_->Cached(EnvironmentLocation(Scope, callerSid_))->snapshot;
        auto& Error = Scope == Scope::System ? Next.systemError_ : Next.userError_;
        auto& Values = Scope == Scope::System ? Next.input_.system_ : Next.input_.user_;
        Values.clear();
        if (!Read) {
            if (Read.error != ERROR_FILE_NOT_FOUND && Read.error != ERROR_PATH_NOT_FOUND) Error = Read.error ? Read.error : ERROR_MORE_DATA;
            continue;
        }
        if (Read.value.info.link) { Error = ERROR_CANT_ACCESS_FILE; continue; }
        for (const auto& Raw : Read.value.values) {
            const auto Decoded = DecodeEnvironmentValue(Raw, Scope);
            if (Decoded) Values.push_back(Decoded.value); else Next.unsupported_.append(Raw.name);
        }
    }
    Next.merged_ = BuildEnvironment(Next.input_);
    snapshot_ = std::move(Next); ready_ = true; ++revision_;
    emit Changed();
}
}