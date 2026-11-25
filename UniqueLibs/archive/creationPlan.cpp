#include "stdafx.h"
#include "creationPlan.hpp"

#include <deque>
#include <tuple>
#include <unordered_set>
#include <QThreadPool>

namespace hc::archive {
using namespace storage;
using namespace operations;
namespace {
JobStep Failed(Error Failure) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"archive-create"}}; }
QString NativePath(QString Value)
{
    Value = QDir::fromNativeSeparators(Value);
    if (Value.startsWith("//?/UNC/", Qt::CaseInsensitive)) Value = "//" + Value.mid(8);
    else if (Value.startsWith("//?/")) Value = Value.mid(4);
    return QDir::cleanPath(Value).normalized(QString::NormalizationForm_C).toCaseFolded();
}
Locator Child(Locator Parent, const std::string& Name)
{
    auto& Path = Parent.kind_ == LocatorKind::Archive ? Parent.nesting_.back().entry_ : Parent.root_;
    if (!Path.ends_with('/') && !Path.ends_with('\\')) Path += '/'; Path += Name; return Parent;
}
bool SameLocation(Locator Left, Locator Right)
{
    if (Left == Right) return true;
    if (Left.kind_ == Right.kind_ && (Left.kind_ == LocatorKind::Local || Left.kind_ == LocatorKind::Smb)
        && Left.root_.size() == Right.root_.size() && std::equal(Left.root_.begin(), Left.root_.end(), Right.root_.begin(),
            [](char A, char B) { return A == B || ((A == '/' || A == '\\') && (B == '/' || B == '\\')); })) return true;
    if (Left.kind_ != Right.kind_) return false;
    if (Left.kind_ == LocatorKind::Local || Left.kind_ == LocatorKind::Smb)
        return NativePath(QString::fromStdString(Left.root_)) == NativePath(QString::fromStdString(Right.root_));
    return Left == Right;
}
}
QString ArchivePathKey(QString Path) { return Path.normalized(QString::NormalizationForm_C).toCaseFolded(); }
QString SuggestedArchiveName(const EntrySnapshot& Entry, const QString& Format)
{
    auto Name = QString::fromStdString(Entry.name_);
    const auto Dot = Name.lastIndexOf('.');
    if (!Entry.directory_ && Dot > 0) Name.truncate(Dot);
    return Name + '.' + Format;
}
QString ResolveArchiveDestination(QString Input, QString BaseDirectory)
{
    const auto Normalize = [](QString Path) {
        Path = QDir::fromNativeSeparators(Path);
        if (Path.startsWith("//?/UNC/", Qt::CaseInsensitive)) Path = "//" + Path.mid(8);
        else if (Path.startsWith("//?/")) Path = Path.mid(4); return Path;
    };
    auto Path = Normalize(Input); if (Path.trimmed().isEmpty() || Path.contains(QChar::Null)) return {};
    if (!QDir::isAbsolutePath(Path)) { if (Path.contains(':') || Path.startsWith('/')) return {}; Path = Normalize(BaseDirectory) + '/' + Path; }
    QString Root, Rest;
    const auto Drive = QRegularExpression("^[A-Za-z]:/").match(Path);
    if (Drive.hasMatch()) { Root = Path.left(3); Rest = Path.mid(3); }
    else if (Path.startsWith("//")) {
        const auto Parts = Path.mid(2).split('/', Qt::SkipEmptyParts);
        if (Parts.size() < 2 || !IsSafeArchiveName(Parts[0]) || !IsSafeArchiveName(Parts[1])) return {};
        Root = "//" + Parts[0] + '/' + Parts[1]; Rest = Parts.mid(2).join('/');
    } else return {};
    QStringList Parts;
    for (const auto& Part : Rest.split('/', Qt::SkipEmptyParts)) {
        if (Part == ".") continue; if (Part == "..") { if (!Parts.isEmpty()) Parts.removeLast(); continue; }
        if (!IsSafeArchiveName(Part)) return {}; Parts.append(Part);
    }
    return Root + (Root.endsWith('/') || Parts.isEmpty() ? QString{} : QString("/")) + Parts.join('/');
}
bool IsSafeArchiveName(const QString& Name)
{
    static const QRegularExpression InvalidCharacters("[<>:\"/\\\\|?*\\x00-\\x1f]");
    static const QRegularExpression DeviceName("^(COM|LPT)[1-9]$");
    if (Name.isEmpty() || Name == "." || Name == ".." || Name.endsWith('.') || Name.endsWith(' ')
        || Name.contains(InvalidCharacters)) return false;
    const auto Stem = Name.section('.', 0, 0).toUpper();
    return Stem != "CON" && Stem != "PRN" && Stem != "AUX" && Stem != "NUL"
        && !Stem.contains(DeviceName);
}
Result<Unit> ValidateOutputLocation(const Locator& Output, const std::vector<OperationSource>& Sources)
{
    if ((Output.kind_ != LocatorKind::Local && Output.kind_ != LocatorKind::Smb) || !Output.nesting_.empty()
        || Output.root_.find('\0') != std::string::npos || !QFileInfo(QString::fromStdString(Output.root_)).isAbsolute()) return Invalid("archive-create-invalid-output");
    const auto Target = NativePath(QString::fromStdString(Output.root_));
    for (const auto& Source : Sources) {
        if (!Source.reader_) return Invalid("archive-create-source-provider");
        const auto& At = Source.entry_.locator_;
        if (At.kind_ != LocatorKind::Local && At.kind_ != LocatorKind::Smb && At.kind_ != LocatorKind::Archive) continue;
        const auto Path = NativePath(QString::fromStdString(At.root_));
        if (Target == Path || (At.kind_ != LocatorKind::Archive && Source.entry_.directory_ && Target.startsWith(Path + '/')))
            return Invalid("archive-create-output-inside-input");
    }
    return Unit{};
}
struct CreationPlanner::State {
    struct OpenedDirectory { std::unique_ptr<IEntryCursor> cursor_; EntryPage first_; };
    using DirectoryFuture = std::future<Result<OpenedDirectory>>;
    struct Pending {
        OperationSource source_;
        QString member_;
        size_t group_ = 0, depth_ = 0;
        bool verified_ = false;
        std::shared_ptr<DirectoryFuture> ahead_;
    };
    PlanRequest request_;
    bool selected_ = false, initialized_ = false, done_ = false;
    uint64_t selectionOffset_ = 0, discovered_ = 0;
    std::deque<Pending> pending_;
    std::string currentPath_;
    std::optional<Pending> collision_;
    Pending directory_;
    std::deque<Pending> roots_;
    std::optional<size_t> activeGroup_;
    bool streamOutputs_ = false, ready_ = false;
    std::unique_ptr<IEntryCursor> cursor_;
    std::optional<EntryPage> firstPage_;
    QThreadPool directoryWorkers_;
    unsigned prefetched_ = 0;
    void prefetchDirectories(const OperationContext& ParentContext) {
        // Keep both handle count and retained pages bounded. Jobs enter the
        // normal FIFO in discovery order regardless of which I/O finishes first.
        constexpr unsigned MaxAhead = 4;
        if (prefetched_ >= MaxAhead) return;
        unsigned Looked = 0;
        for (auto& Candidate : pending_) {
            if (++Looked > 256 || prefetched_ >= MaxAhead) break;
            const auto& Entry = Candidate.source_.entry_;
            if (!Candidate.verified_ || Candidate.ahead_ || !Entry.directory_ || Entry.locator_.kind_ != LocatorKind::Local
                || (Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) || !Entry.hints_.contains("archiveWriteTicks")) continue;
            // The worker can outlive the scheduler slice. Retain its cancellation
            // and read budgets, without capturing scheduler/UI callbacks.
            OperationContext Read;
            Read.stopToken_ = ParentContext.stopToken_;
            Read.deadline_ = ParentContext.deadline_;
            Read.workload_ = ParentContext.workload_;
            Read.maxEnumerationEntries_ = ParentContext.maxEnumerationEntries_;
            Read.maxPageItems_ = ParentContext.maxPageItems_;
            Read.workerRead_ = Read.incrementalEnumeration_ = Read.archivePlanning_ = Read.freshEnumerationMetadata_ = true;
            auto Work = std::make_shared<std::packaged_task<Result<OpenedDirectory>()>>(
                [Reader = Candidate.source_.reader_, At = Entry.locator_, Read]() -> Result<OpenedDirectory> {
                    auto Cursor = Reader->Enumerate(At, Read).get();
                    if (!Cursor.HasValue()) return Cursor.Failure();
                    auto First = Cursor.Value()->Next(128, Read).get();
                    if (!First.HasValue()) return First.Failure();
                    return OpenedDirectory{std::move(Cursor.Value()), std::move(First.Value())};
                });
            Candidate.ahead_ = std::make_shared<DirectoryFuture>(Work->get_future());
            ++prefetched_;
            directoryWorkers_.start([Work] { (*Work)(); });
        }
    }
    std::vector<OutputPlan> outputs_;
    std::vector<std::unordered_set<QString>> names_;
    std::set<QString> outputNames_;
    std::set<std::tuple<size_t, std::string, std::string>> visitedDirectories_;
    State(PlanRequest Request, bool Stream) : request_(std::move(Request)), streamOutputs_(Stream) { directoryWorkers_.setMaxThreadCount(4); }
    JobStep progress() const {
        JobStep Step{JobState::Preparing}; Step.progress_ = JobProgress{0, {}, currentPath_};
        Step.progress_->discoveredItems_ = discovered_;
        if (request_.sources_.size() == 1) Step.progress_->source_ = Serialize(request_.sources_.front().entry_.locator_);
        return Step;
    }
    JobStep conflict() const {
        auto Step = progress(); Step.next_ = JobState::WaitingForUser;
        Step.interaction_ = InteractionRequest{"", InteractionKind::Rename,
            "압축 내부 경로가 겹칩니다. 다른 이름을 입력하거나 이 항목을 건너뛰십시오.",
            {InteractionChoice::Rename, InteractionChoice::Skip, InteractionChoice::Cancel}, collision_->source_.entry_.id_,
            collision_->member_.toStdString(), collision_->member_.toStdString()}; return Step;
    }
};
CreationPlanner::CreationPlanner(PlanRequest Request, bool StreamOutputs)
    : state_(std::make_unique<State>(std::move(Request), StreamOutputs)) {}
bool CreationPlanner::HasReadyOutput() const { return state_->ready_; }
bool CreationPlanner::CanReadAhead() const
{
    if (state_->roots_.empty()) return false;
    const auto& Locator = state_->roots_.front().source_.entry_.locator_;
    return Locator.kind_ == LocatorKind::Local && Locator.nesting_.empty();
}
size_t CreationPlanner::OutputCount() const { return state_->outputs_.size(); }
std::optional<OutputPlan> CreationPlanner::TakeReadyOutput()
{
    if (!state_->ready_) return {};
    state_->ready_ = false;
    const auto Group = *state_->activeGroup_;
    state_->activeGroup_.reset();
    return std::move(state_->outputs_[Group]);
}
CreationPlanner::~CreationPlanner() = default;
std::vector<OutputPlan> CreationPlanner::Take() { return std::move(state_->outputs_); }
Result<Locator> SeparateArchiveOutput(const EntrySnapshot& Entry, Locator Directory, const std::string& Format,
    const std::map<std::string, std::string>& Names, std::set<QString>& UsedNames)
{
    const auto Extension = QString::fromStdString(Format);
    auto FileName = SuggestedArchiveName(Entry, Extension);
    const auto Custom = Names.find(Serialize(Entry.id_));
    if (Custom != Names.end()) FileName = QString::fromStdString(Custom->second);
    if (!IsSafeArchiveName(FileName) || QFileInfo(FileName).suffix().compare(Extension, Qt::CaseInsensitive))
        return Invalid("archive-create-invalid-output-name");
    const auto Base = FileName.left(FileName.size() - Extension.size() - 1);
    unsigned Suffix = 2;
    while (!UsedNames.insert(ArchivePathKey(FileName)).second) {
        if (Custom != Names.end()) return Invalid("archive-create-output-name-collision");
        FileName = Base + QString(" (%1).").arg(Suffix++) + Extension;
    }
    Directory.root_ = QDir(QString::fromStdString(Directory.root_)).filePath(FileName).toStdString();
    return Directory;
}
JobStep CreationPlanner::Advance(JobStepContext Context)
{
    Context.operation_.workerRead_ = true;
    Context.operation_.incrementalEnumeration_ = true;
    Context.operation_.archivePlanning_ = true;
    Context.operation_.freshEnumerationMetadata_ = true;
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    for (unsigned Count = 1; ; ++Count) {
        auto Step = advanceOne(Context);
        if (Step.next_ != JobState::Preparing || Step.nextLanes_ || HasReadyOutput() || Count == 1024
            || Context.pauseSignal_.stop_requested() || std::chrono::steady_clock::now() >= Deadline) {
            if (Step.next_ == JobState::Preparing && !Step.progress_) Step.progress_ = state_->progress().progress_;
            return Step;
        }
        Context.answer_.reset();
    }
}
JobStep CreationPlanner::advanceOne(const JobStepContext& Context)
{
    auto& S = *state_; auto& R = S.request_;
    if (const auto Stop = Context.operation_.Check({"archive-create"})) return Failed(*Stop);
    if (S.done_) return JobStep{JobState::Completed};
    if (!S.selected_) {
        if (R.selection_ && !R.selection_->Count()) return Failed(Invalid("archive-create-empty-selection"));
        if (R.selection_ && S.selectionOffset_ < R.selection_->Count()) {
            auto Reader = R.selection_->Provider(); if (!Reader) return Failed(Invalid("archive-create-selection-provider"));
            auto Page = R.selection_->Read(S.selectionOffset_, 128, Context.operation_); if (!Page.HasValue()) return Failed(Page.Failure());
            if (Page.Value().entries_.empty() || Page.Value().entries_.size() > R.selection_->Count() - S.selectionOffset_) return Failed(Invalid("archive-create-selection-page"));
            if (!S.selectionOffset_) R.sources_.clear();
            for (auto& Entry : Page.Value().entries_) {
                if (Entry.id_.providerId_ != Reader->Id() || !IsSafeArchiveName(QString::fromStdString(Entry.name_))
                    || !SameLocation(Child(R.sourceRoot_, Entry.name_), Entry.locator_)) return Failed(Invalid("archive-create-selection-container"));
                R.sources_.push_back({std::move(Entry), Reader, {}});
            }
            S.selectionOffset_ += Page.Value().entries_.size(); return JobStep{JobState::Preparing};
        }
        S.selected_ = true;
    }
    if (!S.initialized_) {
        if (R.sources_.empty()) return Failed(Invalid("archive-create-empty-selection"));
        auto Valid = ValidateOutputLocation(R.destination_, R.sources_); if (!Valid.HasValue()) return Failed(Valid.Failure());
        if (!R.composition_.separateArchives_) S.outputs_.push_back({R.destination_});
        for (const auto& Source : R.sources_) {
            const auto Name = QString::fromStdString(Source.entry_.name_); if (!IsSafeArchiveName(Name)) return Failed(Invalid("archive-create-invalid-name"));
            size_t Group = 0;
            if (R.composition_.separateArchives_) {
                auto Output = SeparateArchiveOutput(Source.entry_, R.destination_, R.format_, R.outputNames_, S.outputNames_);
                if (!Output.HasValue()) return Failed(Output.Failure());
                if (R.completedOutputs_.contains(Serialize(Output.Value()))) continue;
                Group = S.outputs_.size(); S.outputs_.push_back({std::move(Output.Value())});
                S.outputs_.back().source_ = Source.entry_.locator_;
            } else if (R.completedOutputs_.contains(Serialize(R.destination_))) continue;
            else S.outputs_[0].source_ = R.sources_.size() == 1 ? Source.entry_.locator_ : R.sourceRoot_;
            const auto Member = Source.entry_.directory_ && (!R.composition_.preservePaths_ || R.composition_.OmitSelectedFolderRoot()) ? QString{} : Name;
            S.pending_.push_back({Source, Member, Group, 0});
        }
        if (!R.composition_.separateArchives_ && R.completedOutputs_.contains(Serialize(R.destination_))) S.outputs_.clear();
        S.names_.resize(S.outputs_.size()); S.initialized_ = true;
        auto Step = S.progress();
        if (R.selection_) {
            auto& Lanes = Step.nextLanes_.emplace();
            for (const auto& Output : S.outputs_) Lanes.writes_.push_back(FileOperationLane(Output.destination_));
            for (const auto& Item : S.pending_) {
                const auto& At = Item.source_.entry_.locator_;
                auto& Claims = At.kind_ == LocatorKind::Plugin ? Lanes.writes_ : Lanes.reads_;
                Claims.push_back(FileOperationLane(At));
            }
        }
        S.streamOutputs_ = S.streamOutputs_ && R.composition_.separateArchives_ && S.outputs_.size() > 1;
        if (S.streamOutputs_) S.roots_.swap(S.pending_);
        return Step;
    }
    if (S.streamOutputs_ && !S.cursor_ && !S.collision_ && S.pending_.empty()) {
        if (S.activeGroup_) {
            const auto& Output = S.outputs_[*S.activeGroup_];
            if (Output.entries_.empty() && !Output.skippedEmpty_) return Failed(Invalid("archive-create-empty-selection"));
            S.ready_ = true;
            return JobStep{JobState::Preparing};
        }
        if (S.roots_.empty()) { S.done_ = true; return JobStep{JobState::Completed}; }
        S.activeGroup_ = S.roots_.front().group_;
        S.pending_.push_back(std::move(S.roots_.front()));
        S.roots_.pop_front();
    }
    if (S.cursor_) {
        S.currentPath_ = Serialize(S.directory_.source_.entry_.locator_);
        auto Page = S.firstPage_ ? Result<EntryPage>{std::move(*S.firstPage_)} : S.cursor_->Next(128, Context.operation_).get(); S.firstPage_.reset(); if (!Page.HasValue()) return Failed(Page.Failure());
        if (Page.Value().entries_.empty() && !Page.Value().end_) return Failed(Invalid("archive-create-enumeration-no-progress"));
        for (auto& Entry : Page.Value().entries_) {
            const auto Name = QString::fromStdString(Entry.name_);
            if (!IsSafeArchiveName(Name) || Entry.id_.providerId_ != S.directory_.source_.reader_->Id()
                || !SameLocation(Child(S.directory_.source_.entry_.locator_, Entry.name_), Entry.locator_)) return Failed(Invalid("archive-create-child-container"));
            if (Entry.directory_ && !R.composition_.includeSubdirectories_) continue;
            const auto Member = !R.composition_.preservePaths_ || S.directory_.member_.isEmpty() ? Name : S.directory_.member_ + '/' + Name;
            S.pending_.push_back({{std::move(Entry), S.directory_.source_.reader_, {}}, Member, S.directory_.group_, S.directory_.depth_ + 1, Page.Value().verifiedChildMetadata_});
        }
        if (Page.Value().end_) S.cursor_.reset(); return JobStep{JobState::Preparing};
    }
    State::Pending Next;
    if (S.collision_) {
        if (!Context.answer_) return S.conflict();
        if (Context.answer_->choice_ == InteractionChoice::Cancel) return Failed({ErrorCategory::Cancelled, "archive-create-cancelled"});
        if (Context.answer_->choice_ == InteractionChoice::Skip) { S.outputs_[S.collision_->group_].skippedEmpty_ = true; S.collision_.reset(); return JobStep{JobState::Preparing}; }
        const auto Name = QString::fromStdString(Context.answer_->value_);
        if (Context.answer_->choice_ != InteractionChoice::Rename || !IsSafeArchiveName(Name)) return S.conflict();
        Next = std::move(*S.collision_); S.collision_.reset(); const auto Slash = Next.member_.lastIndexOf('/');
        Next.member_ = (Slash < 0 ? QString{} : Next.member_.left(Slash + 1)) + Name;
    } else {
        if (S.pending_.empty()) {
            for (const auto& Output : S.outputs_) if (Output.entries_.empty() && !Output.skippedEmpty_) return Failed(Invalid("archive-create-empty-selection"));
            S.done_ = true; return JobStep{JobState::Completed};
        }
        Next = std::move(S.pending_.front()); S.pending_.pop_front();
        if (Next.ahead_) --S.prefetched_;
        if (++S.discovered_ > Context.operation_.EnumerationLimit()) return Failed({ErrorCategory::BudgetExceeded, "archive-create-item-count"});
    }
    S.currentPath_ = Serialize(Next.source_.entry_.locator_);
    if (Next.source_.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Failed({ErrorCategory::Unsupported, "archive-create-source-link"});
    if (!Next.verified_ || !Next.source_.entry_.id_.revision_) {
        auto Fresh = Next.source_.reader_->Stat(Next.source_.entry_.id_, Context.operation_).get(); if (!Fresh.HasValue()) return Failed(Fresh.Failure());
        if (Fresh.Value().id_.providerId_ != Next.source_.entry_.id_.providerId_ || Fresh.Value().id_.key_ != Next.source_.entry_.id_.key_
            || Fresh.Value().directory_ != Next.source_.entry_.directory_) return Failed({ErrorCategory::StaleRevision, "archive-create-source-changed"});
        Next.source_.entry_ = std::move(Fresh.Value());
    }
    auto& Output = S.outputs_[Next.group_];
    if (!Next.member_.isEmpty() && (!Next.source_.entry_.directory_ || R.composition_.preservePaths_)) {
        if (!S.names_[Next.group_].insert(ArchivePathKey(Next.member_)).second) { S.collision_ = std::move(Next); return S.conflict(); }
        if (!Next.source_.entry_.directory_) {
            Output.entries_.push_back({std::move(Next.source_), std::move(Next.member_)});
            return JobStep{JobState::Preparing};
        }
        Output.entries_.push_back({Next.source_, Next.member_});
    }
    if (Next.source_.entry_.directory_) {
        if (!S.visitedDirectories_.insert({Next.group_, Next.source_.entry_.id_.providerId_.value_, Next.source_.entry_.id_.key_}).second)
            return Failed(Invalid("archive-create-directory-cycle"));
        Output.directories_.push_back(Next.source_); S.directory_ = std::move(Next);
        S.prefetchDirectories(Context.operation_);
        if (S.directory_.ahead_) {
            auto Opened = S.directory_.ahead_->get();
            if (!Opened.HasValue()) return Failed(Opened.Failure());
            S.cursor_ = std::move(Opened.Value().cursor_);
            S.firstPage_ = std::move(Opened.Value().first_);
        } else {
            auto Cursor = S.directory_.source_.reader_->Enumerate(S.directory_.source_.entry_.locator_, Context.operation_).get();
            if (!Cursor.HasValue()) return Failed(Cursor.Failure());
            S.cursor_ = std::move(Cursor.Value());
        }
    }
    return JobStep{JobState::Preparing};
}
}
