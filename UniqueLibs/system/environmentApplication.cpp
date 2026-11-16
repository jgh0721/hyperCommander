#include "stdafx.h"
#include "environmentApplication.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/environmentNotification.hpp"
#include "commonLibs/system/childEnvironment.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QStandardPaths>
#include <QSysInfo>
#include <stdexcept>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentApplication, "hc.environment.application")
namespace hc::environment {
namespace {
std::optional<EnvironmentApplication::TestInputs> TestConfiguration;
bool ApplicationCreated = false;
}
void EnvironmentApplication::ConfigureForTests(TestInputs Inputs)
{
    if (ApplicationCreated || !Inputs.backend_ || !Inputs.read_ || !Inputs.write_ || !Inputs.process_
        || Inputs.callerSid_.isEmpty() || Inputs.machineId_.isEmpty()) throw std::invalid_argument("incomplete environment test configuration");
    TestConfiguration = std::move(Inputs);
}
EnvironmentApplication* EnvironmentApplication::Shared()
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    static QPointer<EnvironmentApplication> Instance;
    if (!Instance) Instance = new EnvironmentApplication(QCoreApplication::instance());
    return Instance;
}
EnvironmentApplication::EnvironmentApplication(QObject* Parent) : QObject(Parent)
{
    ApplicationCreated = true;
    if (TestConfiguration) inputs_ = std::move(*TestConfiguration);
    else {
        if (QStandardPaths::isTestModeEnabled()) throw std::logic_error("environment tests must inject backend and runtime");
        native_ = true; inputs_.backend_ = std::make_shared<registry::WinRegistryBackend>();
        inputs_.defaults_ = ReadWindowsEnvironmentDefaults(); inputs_.inherited_ = ReadOwnEnvironment();
        inputs_.callerSid_ = QString::fromStdWString(hc::system::CurrentUserSid()); inputs_.machineId_ = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
        inputs_.read_ = ReadOwnEnvironment; inputs_.write_ = WriteOwnEnvironmentValue; inputs_.process_ = ReadProcessEnvironment;
        for (const auto* Name : {L"COMMANDER_PATH", L"QT_ENABLE_HIGHDPI_SCALING"}) {
            const auto Found = inputs_.inherited_.find(Name); if (Found != inputs_.inherited_.end()) inputs_.overlay_[Name] = Found->second;
        }
    }
    inherited_ = inputs_.inherited_; overlay_ = inputs_.overlay_;
    if (native_) PublishChildEnvironment(inherited_);
    session_ = new EnvironmentSession(inputs_.backend_, inputs_.defaults_, inputs_.callerSid_, this);
    debounce_.setSingleShot(true); debounce_.setInterval(200);
    connect(&debounce_, &QTimer::timeout, session_, &EnvironmentSession::Refresh);
    connect(session_, &EnvironmentSession::Changed, this, [this] {
        const auto& Snapshot = session_->Snapshot();
        if (Snapshot.systemError_ || Snapshot.userError_) { lastError_ = Snapshot.systemError_ ? Snapshot.systemError_ : Snapshot.userError_; emit Rebuilt(lastError_); return; }
        const auto Model = EffectiveEnvironment(Snapshot.merged_);
        if (!runtime_) runtime_ = std::make_unique<EnvironmentRuntime>(inherited_, Model, overlay_, inputs_.read_, inputs_.write_);
        else {
            lastError_ = runtime_->Rebuild(Model);
            if (!lastError_ && native_) PublishChildEnvironment(runtime_->Snapshot());
            emit Rebuilt(lastError_);
        }
    });
    if (native_) QCoreApplication::instance()->installNativeEventFilter(this);
    session_->SetWatching(native_); session_->Refresh();
}
EnvironmentApplication::~EnvironmentApplication()
{
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
}
std::function<void()> EnvironmentApplication::Notification() const
{
    return native_ ? std::function<void()>([] { hc::system::NotifyEnvironmentChanged(); }) : std::function<void()>([] {});
}
void EnvironmentApplication::Refresh() { debounce_.start(); }
QProcessEnvironment EnvironmentApplication::ChildEnvironment() const
{
    if (runtime_) return runtime_->ChildEnvironment();
    QProcessEnvironment Result;
    for (const auto& [Name, Text] : inherited_) Result.insert(QString::fromStdWString(Name), QString::fromStdWString(Text));
    return Result;
}
bool EnvironmentApplication::nativeEventFilter(const QByteArray&, void* Message, qintptr*)
{
    const auto* Event = static_cast<const MSG*>(Message);
    if (Event && Event->message == WM_SETTINGCHANGE && Event->lParam) {
        // WM_SETTINGCHANGE is delivered synchronously; lParam is valid only
        // during this callback and must never be retained by the timer.
        const auto* Section = reinterpret_cast<const wchar_t*>(Event->lParam);
        if (CompareStringOrdinal(Section, -1, L"Environment", -1, TRUE) == CSTR_EQUAL) Refresh();
    }
    return false;
}
}
