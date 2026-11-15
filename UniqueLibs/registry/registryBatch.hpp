#pragma once
/// 고유 라이브러리
#include "registrySearch.hpp"
namespace hc::registry {
struct RegistryBatchPreview
{
    QList<RegistryOperation> operations;
    RegistryPlan plan;
    QList<RegistryResultRow> rows;
    QString error;
    qsizetype skipped = 0;
    explicit operator bool() const { return error.isEmpty(); }
};
class RegistryBatch final
{
public:
    static RegistryBatchPreview Replace(const QList<RegistryResultRow>& Rows,
        const RegistrySearchOptions& Options, const QString& Replacement, const QString& Sid, const QString& Machine);
    static RegistryResult<QList<RegistryOperation>> Export(IRegistryBackend& Backend,
        const QList<RegistryLocation>& Keys, const QList<RegistryResultRow>& Values = {}, const std::atomic_bool* Cancel = nullptr);
    static QList<RegistryOperation> Normalize(const QList<RegistryOperation>& Operations);
    static RegistryResult<QByteArray> Csv(const QList<RegistryResultRow>& Rows, const std::atomic_bool* Cancel = nullptr);
};
}