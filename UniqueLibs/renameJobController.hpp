#pragma once
#include "permissionJobController.hpp"
#include "commonLibs/operations/jobScheduler.hpp"
#include <QString>

namespace hc::filetools {
// Dialogs depend only on this submission/lifetime interface. The concrete
// application controller owns the shared scheduler and background windows.
class RenameJobController {
public:
    virtual ~RenameJobController() = default;
    virtual QObject* RenameOwner() = 0;
    virtual operations::JobScheduler& RenameScheduler() = 0;
    virtual storage::Result<operations::JobHandle> SubmitRename(operations::PreparedJob Job, const QString& Label, bool Background = false) = 0;
    virtual bool BackgroundRename(const std::string& Id) = 0;
};
}
