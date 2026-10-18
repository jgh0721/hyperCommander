#pragma once
#include "commonLibs/operations/jobScheduler.hpp"
#include <QString>

namespace hc::operations
{
    // Directory access occurs on scheduler/recovery workers. Failures are
    // reported before an operation may cross a side-effect boundary.
    std::shared_ptr<IJobJournal> MakeFileJobJournal(QString Directory, bool SaveProgress = false, bool OptimizeRecoveryIo = true, bool EnableRecovery = true);
    // Changes the default for jobs first checkpointed after this call. Existing
    // jobs retain their policy. The default factory preserves legacy callers;
    // the application explicitly supplies its default-off recovery preference.
    void SetFileJobPersistence(const std::shared_ptr<IJobJournal>& Journal, bool EnableRecovery, bool SaveProgress);
    void SetFileJobProgressPersistence(const std::shared_ptr<IJobJournal>& Journal, bool SaveProgress);
}
