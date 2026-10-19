#include "stdafx.h"
#include "creationPlan.hpp"

#include <deque>
#include <tuple>

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
    if (Left.kind_ != Right.kind_) return false;
    if (Left.kind_ == LocatorKind::Local || Left.kind_ == LocatorKind::Smb)
        return NativePath(QString::fromStdString(Left.root_)) == NativePath(QString::fromStdString(Right.root_));
    return Left == Right;
}
}
QString ArchivePathKey(QString Path) { return Path.normalized(QString::NormalizationForm_C).toCaseFolded(); }
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
    if (Name.isEmpty() || Name == "." || Name == ".." || Name.endsWith('.') || Name.endsWith(' ')
        || Name.contains(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"))) return false;
    const auto Stem = Name.section('.', 0, 0).toUpper();
    return Stem != "CON" && Stem != "PRN" && Stem != "AUX" && Stem != "NUL"
        && !Stem.contains(QRegularExpression("^(COM|LPT)[1-9]$"));
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
    struct Pending { OperationSource source_; QString member_; size_t group_ = 0, depth_ = 0; };
    PlanRequest request_;
    bool selected_ = false, initialized_ = false, done_ = false;
    uint64_t selectionOffset_ = 0, discovered_ = 0;
    std::deque<Pending> pending_;
    std::optional<Pending> collision_;
    Pending directory_;
    std::unique_ptr<IEntryCursor> cursor_;
    std::vector<OutputPlan> outputs_;
    std::vector<std::set<QString>> names_;
    std::set<QString> outputNames_;
    std::set<std::tuple<size_t, std::string, std::string>> visitedDirectories_;
    explicit State(PlanRequest Request) : request_(std::move(Request)) {}
    JobStep progress() const {
        JobStep Step{JobState::Preparing}; Step.progress_ = JobProgress{0, {}, "압축 파일 목록 확인"};
        Step.progress_->discoveredItems_ = discovered_; return Step;
    }
    JobStep conflict() const {
        auto Step = progress(); Step.next_ = JobState::WaitingForUser;
        Step.interaction_ = InteractionRequest{"", InteractionKind::Rename,
            "압축 내부 경로가 겹칩니다. 다른 이름을 입력하거나 이 항목을 건너뛰십시오.",
            {InteractionChoice::Rename, InteractionChoice::Skip, InteractionChoice::Cancel}, collision_->source_.entry_.id_,
            collision_->member_.toStdString(), collision_->member_.toStdString()}; return Step;
    }
};
CreationPlanner::CreationPlanner(PlanRequest Request) : state_(std::make_unique<State>(std::move(Request))) {}
CreationPlanner::~CreationPlanner() = default;
std::vector<OutputPlan> CreationPlanner::Take() { return std::move(state_->outputs_); }
JobStep CreationPlanner::Advance(JobStepContext Context)
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
            S.selectionOffset_ += Page.Value().entries_.size(); return S.progress();
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
                auto Output = R.destination_; const auto FileName = Name + '.' + QString::fromStdString(R.format_);
                if (!S.outputNames_.insert(ArchivePathKey(FileName)).second) return Failed(Invalid("archive-create-output-name-collision"));
                Output.root_ = QDir(QString::fromStdString(Output.root_)).filePath(FileName).toStdString();
                if (R.completedOutputs_.contains(Serialize(Output))) continue;
                Group = S.outputs_.size(); S.outputs_.push_back({Output});
            } else if (R.completedOutputs_.contains(Serialize(R.destination_))) continue;
            const auto Member = Source.entry_.directory_ && (!R.composition_.preservePaths_ || R.composition_.OmitSelectedFolderRoot()) ? QString{} : Name;
            S.pending_.push_back({Source, Member, Group, 0});
        }
        if (!R.composition_.separateArchives_ && R.completedOutputs_.contains(Serialize(R.destination_))) S.outputs_.clear();
        S.names_.resize(S.outputs_.size()); S.initialized_ = true; return S.progress();
    }
    if (S.cursor_) {
        auto Page = S.cursor_->Next(128, Context.operation_).get(); if (!Page.HasValue()) return Failed(Page.Failure());
        if (Page.Value().entries_.empty() && !Page.Value().end_) return Failed(Invalid("archive-create-enumeration-no-progress"));
        for (auto& Entry : Page.Value().entries_) {
            const auto Name = QString::fromStdString(Entry.name_);
            if (!IsSafeArchiveName(Name) || Entry.id_.providerId_ != S.directory_.source_.reader_->Id()
                || !SameLocation(Child(S.directory_.source_.entry_.locator_, Entry.name_), Entry.locator_)) return Failed(Invalid("archive-create-child-container"));
            if (Entry.directory_ && !R.composition_.includeSubdirectories_) continue;
            const auto Member = !R.composition_.preservePaths_ || S.directory_.member_.isEmpty() ? Name : S.directory_.member_ + '/' + Name;
            S.pending_.push_back({{std::move(Entry), S.directory_.source_.reader_, {}}, Member, S.directory_.group_, S.directory_.depth_ + 1});
        }
        if (Page.Value().end_) S.cursor_.reset(); return S.progress();
    }
    State::Pending Next;
    if (S.collision_) {
        if (!Context.answer_) return S.conflict();
        if (Context.answer_->choice_ == InteractionChoice::Cancel) return Failed({ErrorCategory::Cancelled, "archive-create-cancelled"});
        if (Context.answer_->choice_ == InteractionChoice::Skip) { S.outputs_[S.collision_->group_].skippedEmpty_ = true; S.collision_.reset(); return S.progress(); }
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
        if (++S.discovered_ > Context.operation_.EnumerationLimit()) return Failed({ErrorCategory::BudgetExceeded, "archive-create-item-count"});
    }
    if (Next.source_.entry_.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Failed({ErrorCategory::Unsupported, "archive-create-source-link"});
    auto Fresh = Next.source_.reader_->Stat(Next.source_.entry_.id_, Context.operation_).get(); if (!Fresh.HasValue()) return Failed(Fresh.Failure());
    // Stat validates the supplied revision. Native listing revisions may be
    // upgraded to a handle-based revision; retain that stronger snapshot.
    if (Fresh.Value().id_.providerId_ != Next.source_.entry_.id_.providerId_ || Fresh.Value().id_.key_ != Next.source_.entry_.id_.key_
        || Fresh.Value().directory_ != Next.source_.entry_.directory_) return Failed({ErrorCategory::StaleRevision, "archive-create-source-changed"});
    Next.source_.entry_ = std::move(Fresh.Value());
    auto& Output = S.outputs_[Next.group_];
    if (!Next.member_.isEmpty() && (!Next.source_.entry_.directory_ || R.composition_.preservePaths_)) {
        if (!S.names_[Next.group_].insert(ArchivePathKey(Next.member_)).second) { S.collision_ = std::move(Next); return S.conflict(); }
        Output.entries_.push_back({Next.source_, Next.member_});
    }
    if (Next.source_.entry_.directory_) {
        if (!S.visitedDirectories_.insert({Next.group_, Next.source_.entry_.id_.providerId_.value_, Next.source_.entry_.id_.key_}).second)
            return Failed(Invalid("archive-create-directory-cycle"));
        Output.directories_.push_back(Next.source_); S.directory_ = std::move(Next);
        auto Cursor = S.directory_.source_.reader_->Enumerate(S.directory_.source_.entry_.locator_, Context.operation_).get(); if (!Cursor.HasValue()) return Failed(Cursor.Failure());
        S.cursor_ = std::move(Cursor.Value());
    }
    return S.progress();
}
}
