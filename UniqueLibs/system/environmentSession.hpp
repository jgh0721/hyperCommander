#pragma once
#include "environmentSnapshot.hpp"
#include "UniqueLibs/registry/registryService.hpp"

namespace hc::environment {
// One cached source serves every environment view and detail pane. Tests supply
// a fake backend and synthetic defaults; native watching is explicitly enabled.
class EnvironmentSession final : public QObject
{
    Q_OBJECT
public:
    EnvironmentSession(std::shared_ptr<registry::IRegistryBackend> Backend, MergeInput Defaults,
        QString CallerSid, QObject* Parent = nullptr);
    const EnvironmentSnapshot& Snapshot() const { return snapshot_; }
    bool Ready() const { return ready_; }
    bool Pending() const;
    quint64 Revision() const { return revision_; }
    void Refresh();
    void SetWatching(bool Enabled);
    void SetDefaults(MergeInput Defaults);
signals:
    void Changed();
    void LoadingChanged();
private:
    void publish();
    registry::RegistryService* service_;
    MergeInput defaults_;
    QString callerSid_;
    EnvironmentSnapshot snapshot_;
    quint64 revision_ = 0;
    bool ready_ = false, refreshing_ = false;
};
}