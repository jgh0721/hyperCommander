#pragma once
/// 고유 라이브러리
#include "registryResultModel.hpp"
#include "registryMutation.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QProcessEnvironment>
namespace hc::registry {
struct StartupApproval
{
    enum class State { Enabled, Disabled, Unknown };
    State state = State::Unknown;
    quint64 disabledAt = 0;
};
class StartupApprovedCodec final
{
public:
    static StartupApproval Decode(const std::optional<RegistryValue>& Value);
    static RegistryValue Encode(const QString& Name, bool Enabled, quint64 Timestamp);
    static std::optional<RegistryLocation> LocationFor(const RegistryLocation& Run);
};
struct RegistryAutorunEntry
{
    RegistryLocation location;
    RegistryValue value;
    QString id, category, name, image, publisher, status, warning, clsid;
    bool risk = false, microsoft = false, canToggle = false, enabled = true;
    quint32 error = 0;
    std::optional<RegistryLocation> switchLocation;
    std::optional<RegistryValue> switchValue;
    bool userBlocked = false;
};
struct RegistryAutorunScan
{
    QList<RegistryResultRow> rows;
    QHash<QString, RegistryAutorunEntry> entries;
    QStringList diagnostics;
};
class RegistryAutoruns final : public QObject
{
    Q_OBJECT
public:
    explicit RegistryAutoruns(QObject* Parent = nullptr);
    ~RegistryAutoruns() override;
    RegistryResultModel* Model() const { return model_; }
    bool Loading() const { return loading_; }
    QStringList Diagnostics() const { return data_.diagnostics; }
    std::optional<RegistryAutorunEntry> Entry(const QString& Id) const;
    void Reload();
    void SetActive(bool Active);
    static RegistryAutorunScan Scan(IRegistryBackend& Backend, const QProcessEnvironment& Environment,
        const std::atomic_bool* Cancel = nullptr, bool InspectFiles = true);
    static RegistryMutationResult Toggle(RegistryMutation& Mutation, const RegistryAutorunEntry& Entry, quint64 Timestamp);
signals:
    void Changing();
    void Changed();
private:
    void startWatches();
    struct Watches;
    std::unique_ptr<Watches> watches_;
    bool active_ = false;
    RegistryResultModel* model_;
    RegistryAutorunScan data_;
    bool loading_ = false;
    quint64 generation_ = 0;
    std::shared_ptr<std::atomic_bool> cancel_;
};
} // namespace hc::registry
