#pragma once
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/plugins/archiveCreation.hpp"
#include <QString>
#include <set>

namespace hc::archive {
struct PlannedEntry { operations::OperationSource source_; QString member_; };
struct OutputPlan {
    storage::Locator destination_;
    std::vector<PlannedEntry> entries_;
    std::vector<operations::OperationSource> directories_;
    bool skippedEmpty_ = false;
    storage::Locator source_;
    std::vector<QString> inputPaths_;
    QString inputDirectory_;
};
struct PlanRequest {
    std::vector<operations::OperationSource> sources_;
    std::shared_ptr<storage::ISelectionManifest> selection_;
    storage::Locator sourceRoot_, destination_;
    std::string format_;
    plugins::ArchiveCompositionOptions composition_;
    std::set<std::string> completedOutputs_;
    std::map<std::string, std::string> outputNames_;
};
QString ArchivePathKey(QString Path);
QString SuggestedArchiveName(const storage::EntrySnapshot& Entry, const QString& Format);
QString ResolveArchiveDestination(QString Input, QString BaseDirectory);
bool IsSafeArchiveName(const QString& Name);
storage::Result<storage::Unit> ValidateOutputLocation(const storage::Locator& Output,
    const std::vector<operations::OperationSource>& Sources);
// Worker-side, paged discovery. No provider I/O occurs in construction.
class CreationPlanner final {
public:
    explicit CreationPlanner(PlanRequest Request);
    ~CreationPlanner();
    operations::JobStep Advance(operations::JobStepContext Context);
    std::vector<OutputPlan> Take();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
