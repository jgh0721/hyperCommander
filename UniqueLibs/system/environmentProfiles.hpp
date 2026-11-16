#pragma once
#include "environmentProfilePlan.hpp"

namespace hc::environment {
struct EnvironmentProfile
{
    QString id_, name_;
    std::vector<Value> values_;
    bool active_ = false;
    QString backupId_, pendingId_;
    bool pendingActive_ = false;
    ProfileBaseline baseline_;
    QString pendingName_;
    std::vector<Value> pendingValues_;
    ProfileBaseline pendingBaseline_;
};
// A shared session serializes this store. Disk revisions also reject writes
// from a second HC instance; pending journal IDs make interrupted toggles visible.
class EnvironmentProfiles final
{
public:
    explicit EnvironmentProfiles(QString FileName);
    quint32 Load(registry::RegistryChangeStore& History);
    const QList<EnvironmentProfile>& Items() const { return profiles_; }
    quint32 Put(EnvironmentProfile Profile);
    quint32 Remove(const QString& Id);
    QStringList Conflicts(const QString& Id) const;
    EnvironmentPreview PreviewToggle(const QString& Id, EnvironmentStorage& Storage, registry::RegistryChangeStore& History);
    EnvironmentPreview PreviewUpdate(EnvironmentProfile Profile, EnvironmentStorage& Storage);
    EnvironmentCommit Toggle(const QString& Id, const EnvironmentPreview& Preview, EnvironmentStorage& Storage);
private:
    quint32 save();
    QString fileName_;
    QByteArray revision_;
    QList<EnvironmentProfile> profiles_;
    QHash<QString, QByteArray> previews_;
    QHash<QString, EnvironmentProfile> desired_;
};
}
