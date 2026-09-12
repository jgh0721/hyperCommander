#pragma once
#include "commonLibs/operations/jobScheduler.hpp"
#include <QString>

namespace hc::operations
{
    // Directory access occurs on scheduler/recovery workers. Failures are
    // reported before an operation may cross a side-effect boundary.
    std::shared_ptr<IJobJournal> MakeFileJobJournal(QString Directory);
}
