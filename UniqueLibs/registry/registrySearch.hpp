#pragma once
/// 고유 라이브러리
#include "registryResultModel.hpp"
#include "registryMutation.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRegularExpression>
#include <QSet>
#include <QHash>
namespace hc::registry {
struct RegistrySearchOptions
{
    enum class Type { All, Text, Number, Binary };
    QString text, callerSid;
    QList<RegistryLocation> roots;
    bool keys = true, names = true, data = true, whole = false, regex = false;
    Type type = Type::All;
    quint64 since = 0;
};
struct RegistrySearchBatch
{
    QList<RegistryResultRow> rows;
    QStringList diagnostics;
    qsizetype scanned = 0, denied = 0;
    bool done = false, limited = false;
    QString error;
};
// One cursor is advanced on one worker at a time. Cancellation retains the
// current value position and pending keys so "continue" does not repeat work.
class RegistrySearchCursor final
{
public:
    explicit RegistrySearchCursor(RegistrySearchOptions Options);
    RegistrySearchBatch Advance(IRegistryBackend& Backend, const std::atomic_bool* Cancel,
        qsizetype Rows = 250, int Milliseconds = 250);
    const RegistrySearchOptions& Options() const { return options_; }
    QString Error() const { return error_; }
private:
    bool hit(const QString& Text);
    bool typeAllowed(quint32 Type) const;
    RegistryLocation sourceLocation(IRegistryBackend& Backend, const RegistryLocation& Location, const std::optional<RegistryValue>& Value);
    RegistrySearchOptions options_;
    QRegularExpression pattern_;
    QString error_;
    QList<RegistryLocation> pending_;
    QSet<QString> visited_, emitted_;
    std::optional<KeySnapshot> current_;
    RegistryLocation currentLocation_;
    qsizetype value_ = 0, scanned_ = 0, denied_ = 0, retained_ = 0, count_ = 0;
    bool keyChecked_ = false, limited_ = false;
    QHash<int, RegistryLocation> userClasses_, machineClasses_;
};
class RegistrySearch final : public QObject
{
    Q_OBJECT
public:
    explicit RegistrySearch(QObject* Parent = nullptr);
    ~RegistrySearch() override;
    bool Start(RegistrySearchOptions Options);
    void Stop();
    void Continue();
    RegistryResultModel* Model() const { return model_; }
    const QList<RegistryResultRow>& Rows() const { return rows_; }
    const RegistrySearchOptions& Options() const { return options_; }
    const RegistrySearchBatch& Status() const { return status_; }
    bool Running() const { return running_; }
signals:
    void BatchStarting();
    void BatchFinished();
    void Changed();
private:
    void next();
    RegistryResultModel* model_;
    RegistrySearchOptions options_;
    RegistrySearchBatch status_;
    QList<RegistryResultRow> rows_;
    std::shared_ptr<RegistrySearchCursor> cursor_;
    std::shared_ptr<std::atomic_bool> cancel_;
    quint64 generation_ = 0;
    bool running_ = false, inFlight_ = false;
};
} // namespace hc::registry