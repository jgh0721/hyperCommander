#pragma once
#include "registryBatch.hpp"
#include <functional>
namespace hc::registry {
struct RegistryImportSource
{
    QString path, error;
    std::shared_ptr<void> keepAlive;
    std::function<bool()> verify;
};
using RegistryImportLoader = std::function<RegistryImportSource(const std::atomic_bool*)>;
struct RegistryImportPreview : RegistryBatchPreview
{
    QList<int> included;
};
class RegistryImport final
{
public:
    // Parse order remains intact, including repeated assignments/delete-create.
    // Exclusions are AST operation indexes; the preview is always re-planned.
    static RegistryImportPreview Prepare(IRegistryBackend& Backend, const QList<RegistryOperation>& Operations,
        const QSet<int>& Excluded, const QString& Sid, const QString& Machine, const std::atomic_bool* Cancel = nullptr);
};
}