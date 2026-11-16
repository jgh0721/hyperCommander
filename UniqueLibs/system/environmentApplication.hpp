#pragma once
#include "environmentSession.hpp"
#include "environmentRuntime.hpp"
#include "commonLibs/system/processEnvironment.hpp"
#include <QAbstractNativeEventFilter>
#include <QTimer>

namespace hc::environment {
class EnvironmentApplication final : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    struct TestInputs {
        std::shared_ptr<registry::IRegistryBackend> backend_;
        MergeInput defaults_;
        ValueMap inherited_, overlay_;
        QString callerSid_, machineId_;
        EnvironmentRuntime::Reader read_;
        EnvironmentRuntime::Writer write_;
        std::function<ProcessEnvironment(const hc::system::ProcessKey&)> process_;
    };
    static void ConfigureForTests(TestInputs Inputs);
    static EnvironmentApplication* Shared();
    std::shared_ptr<registry::IRegistryBackend> Backend() const { return inputs_.backend_; }
    QString CallerSid() const { return inputs_.callerSid_; }
    QString MachineId() const { return inputs_.machineId_; }
    ValueMap OwnEnvironment() const { return inputs_.read_(); }
    auto ProcessReader() const { return inputs_.process_; }
    bool Native() const { return native_; }
    std::function<void()> Notification() const;
    EnvironmentSession* Session() const { return session_; }
    void Refresh();
    QProcessEnvironment ChildEnvironment() const;
    quint32 LastError() const { return lastError_; }
    bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr* Result) override;
    ~EnvironmentApplication() override;
signals:
    void Rebuilt(quint32 Error);
private:
    explicit EnvironmentApplication(QObject* Parent);
    TestInputs inputs_;
    bool native_ = false;
    EnvironmentSession* session_;
    QTimer debounce_;
    ValueMap inherited_, overlay_;
    std::unique_ptr<EnvironmentRuntime> runtime_;
    quint32 lastError_ = 0;
};
}
