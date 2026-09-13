#pragma once
#include "pluginHost/approvedWcxBackend.hpp"
#include <QStringList>

namespace hc::plugins {
// Pure construction. Approval comes from a build-generated compiled manifest;
// the usual inspector verifies on-disk binaries on every host connection.
ApprovedWcxOptions BuiltinIsoOptions();
struct BuiltinIsoInspection {
    QString volumeLabel_;
    QStringList namespaces_;
    unsigned udfRevision_ = 0;
    uint64_t files_ = 0, directories_ = 0, bytes_ = 0;
    std::vector<ArchiveHeader> preview_;
};
storage::Result<BuiltinIsoInspection> InspectBuiltinIso(const QString& Path, bool ReadAll,
    storage::OperationContext Context = {});
}
