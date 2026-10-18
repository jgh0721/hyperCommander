#pragma once
#include "nativeFileElevation.hpp"
#include "nativeWritableStorage.hpp"
#include "commonLibs/storage/entryIndex.hpp"
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <set>
#include <thread>

namespace hc::attributes
{
    using namespace storage;
    enum class Decision { Elevate, Skip, Stop };
    using Approval = std::function<Decision(const EntrySnapshot&, bool)>;
    struct ItemResult { EntrySnapshot entry_; QString state_, detail_; bool retry_ = false; };
    struct Plan { std::vector<EntrySnapshot> entries_; std::vector<ItemResult> issues_; bool cancelled_ = false, blocked_ = false; };
    struct Outcome { std::vector<ItemResult> items_; std::size_t changed_ = 0, unchanged_ = 0, failed_ = 0, skipped_ = 0, pending_ = 0; };
    inline QString Key(const Locator& At) { return QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(At.root_))).toCaseFolded(); }
    inline bool Strong(const EntrySnapshot& Entry) { return Entry.id_.revision_ && (Entry.id_.revision_->token_.starts_with("directory:") || Entry.id_.revision_->token_.find(":identity:") != std::string::npos); }
    inline Plan Collect(std::vector<EntrySnapshot> Roots, std::shared_ptr<IStorageProvider> Reader,
        bool Recursive, OperationContext Context, Approval Ask,
        std::shared_ptr<const ISelectionManifest> Manifest = {}, permissions::FileElevationLauncher DirectoryLauncher = {})
    {
        Plan Result;
        constexpr std::size_t Limit = 100000;
        Context.freshEnumerationMetadata_ = true;
        Context.allowUnavailableChildMetadata_ = true;
        Context.maxEnumerationEntries_ = Limit;
        if (Manifest) {
            if (Manifest->Count() > Limit) { Result.blocked_ = true; Result.issues_.push_back({{}, "실패", "선택 항목이 100,000개를 초과합니다. 범위를 줄이세요."}); return Result; }
            for (std::uint64_t Offset = 0; Offset < Manifest->Count();) {
                auto Page = Manifest->Read(Offset, 256, Context);
                if (!Page.HasValue() || Page.Value().entries_.empty()) { Result.blocked_ = true; Result.issues_.push_back({{}, "실패", "선택 목록을 읽지 못했습니다."}); return Result; }
                Offset += Page.Value().entries_.size();
                for (auto& E : Page.Value().entries_) Roots.push_back(std::move(E));
            }
        }
        std::set<QString> Seen;
        std::vector<std::shared_ptr<permissions::DirectoryElevationSession>> Elevated;
        // Iterative traversal avoids stack exhaustion. Depth ordering below also
        // handles explicitly selected descendants appearing before their parents.
        while (!Roots.empty()) {
            if (Context.Check(Reader->Id())) { Result.cancelled_ = true; break; }
            auto Entry = std::move(Roots.back()); Roots.pop_back();
            if (!Seen.insert(Key(Entry.locator_)).second) continue;
            if (Seen.size() > Limit) { Result.blocked_ = true; Result.issues_.push_back({Entry, "실패", "대상이 100,000개를 초과합니다. 범위를 줄이세요."}); break; }
            if ((Entry.attributes_ & 0x400) || QFileInfo(QString::fromStdString(Entry.locator_.root_)).isRoot()) {
                Result.issues_.push_back({Entry, "제외", "루트 또는 링크 항목은 변경하지 않습니다."}); continue;
            }
            auto Fresh = Reader->Resolve(Entry.locator_, Context).get();
            if (Fresh.HasValue()) {
                if (Strong(Entry) && Entry.id_.revision_ != Fresh.Value().id_.revision_) {
                    Result.issues_.push_back({Entry, "실패", "선택 이후 항목이 변경되었습니다."}); continue;
                }
                Entry = std::move(Fresh.Value());
            } else if (Fresh.Failure().category_ == ErrorCategory::AccessDenied && !Strong(Entry) && Entry.locator_.kind_ == LocatorKind::Local) {
                // A listing without a revision is presentation data, never write
                // authority. Obtain a fresh handle identity in the approved helper.
                const auto Choice = Ask(Entry, true);
                if (Choice == Decision::Stop) { Result.cancelled_ = true; break; }
                bool Found = false;
                if (Choice == Decision::Elevate) {
                    const auto Parent = QFileInfo(QString::fromStdString(Entry.locator_.root_)).absolutePath();
                    auto Opened = permissions::OpenElevatedDirectorySession(Parent, Context, DirectoryLauncher);
                    if (!Opened.HasValue() && Opened.Failure().category_ == ErrorCategory::Cancelled) { Result.cancelled_ = true; break; }
                    if (Opened.HasValue()) {
                        Elevated.push_back(Opened.Value());
                        auto Cursor = Opened.Value()->Enumerate(Parent, Context, Reader);
                        if (Cursor.HasValue()) while (true) {
                            auto Page = Cursor.Value()->Next(256, Context).get();
                            if (!Page.HasValue()) break;
                            for (const auto& Child : Page.Value().entries_) if (Key(Child.locator_) == Key(Entry.locator_) && Strong(Child)) { Entry = Child; Found = true; break; }
                            if (Found || Page.Value().end_ || Page.Value().entries_.empty()) break;
                        }
                    }
                }
                if (!Found) { Result.issues_.push_back({Entry, "제외", "관리자 조회에서 파일 신원을 확인하지 못했습니다. 하위 항목도 제외합니다."}); continue; }
            } else if (Fresh.Failure().category_ != ErrorCategory::AccessDenied || !Strong(Entry)) {
                Result.issues_.push_back({Entry, "실패", QString::fromStdString(Fresh.Failure().code_)}); continue;
            }
            if (Entry.attributes_ & 0x400) { Result.issues_.push_back({Entry, "제외", "링크 항목은 변경하지 않습니다."}); continue; }
            Result.entries_.push_back(Entry);
            if (!Recursive || !Entry.directory_) continue;
            auto Cursor = Reader->Enumerate(Entry.locator_, Context).get();
            if (!Cursor.HasValue() && Cursor.Failure().category_ == ErrorCategory::AccessDenied && Entry.locator_.kind_ == LocatorKind::Local) {
                std::shared_ptr<permissions::DirectoryElevationSession> Session;
                for (const auto& Existing : Elevated) if (Existing->Contains(QString::fromStdString(Entry.locator_.root_))) { Session = Existing; break; }
                if (!Session) {
                    const auto Choice = Ask(Entry, true);
                    if (Choice == Decision::Stop) { Result.cancelled_ = true; break; }
                    if (Choice == Decision::Elevate) {
                        auto Opened = permissions::OpenElevatedDirectorySession(QString::fromStdString(Entry.locator_.root_), Context, DirectoryLauncher);
                        if (Opened.HasValue()) { Session = Opened.Value(); Elevated.push_back(Session); }
                        else Cursor = Opened.Failure();
                    }
                }
                if (Session) Cursor = Session->Enumerate(QString::fromStdString(Entry.locator_.root_), Context, Reader);
            }
            if (!Cursor.HasValue()) {
                if (Cursor.Failure().category_ == ErrorCategory::Cancelled) { Result.cancelled_ = true; break; }
                Result.issues_.push_back({Entry, "하위 제외", QString::fromStdString(Cursor.Failure().code_) + " · 하위 개수 미확인"}); continue;
            }
            while (true) {
                auto Page = Cursor.Value()->Next(256, Context).get();
                if (!Page.HasValue()) { Result.blocked_ = true; Result.issues_.push_back({Entry, "실패", QString::fromStdString(Page.Failure().code_)}); break; }
                if (Seen.size() + Roots.size() + Page.Value().entries_.size() > Limit) { Result.blocked_ = true; Result.issues_.push_back({Entry, "실패", "대상 수집 한도를 초과했습니다."}); break; }
                for (auto& Child : Page.Value().entries_) Roots.push_back(std::move(Child));
                if (Page.Value().end_) break;
                if (Page.Value().entries_.empty()) { Result.blocked_ = true; break; }
            }
            if (Result.blocked_) break;
        }
        const auto Depth = [](const EntrySnapshot& Entry) {
            return std::count_if(Entry.locator_.root_.begin(), Entry.locator_.root_.end(), [](char C) { return C == '/' || C == '\\'; });
        };
        std::stable_sort(Result.entries_.begin(), Result.entries_.end(), [&](const auto& A, const auto& B) { return Depth(A) > Depth(B); });
        return Result;
    }
    inline Result<Unit> ElevatedPatch(const EntrySnapshot& Entry, MetadataPatch Patch,
        std::shared_ptr<operations::IFileElevationService> Service, OperationContext Context)
    {
        operations::FileElevationRequest Request;
        Request.jobId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        Request.action_ = operations::FileElevationAction::Metadata;
        Request.source_ = Entry; Request.destination_ = Entry.locator_; Request.metadataPatch_ = Patch;
        auto Started = Service->Begin(std::move(Request), Context);
        if (!Started.HasValue()) return Started.Failure();
        auto Session = std::move(Started.Value()); bool Committed = false;
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
        while (std::chrono::steady_clock::now() < Deadline) {
            // After committing, obtain the actual outcome before stopping the batch.
            if (Context.stopToken_.stop_requested() && !Committed) { Session->Cancel(); Session->Close(); return Error{ErrorCategory::Cancelled, "elevation-cancelled"}; }
            auto Status = Session->Poll();
            if (!Status.HasValue()) { Session->Cancel(); Session->Close(); return Status.Failure(); }
            if (Status.Value().finished_) { Session->Close(); return Status.Value().error_ ? Result<Unit>(*Status.Value().error_) : Result<Unit>(Unit{}); }
            if (Status.Value().readyToCommit_ && !Committed) {
                auto Commit = Session->Commit();
                if (!Commit.HasValue()) { Session->Cancel(); Session->Close(); return Commit.Failure(); }
                Committed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        Session->Cancel(); Session->Close();
        return Error{ErrorCategory::Unavailable, Committed ? "elevation-outcome-unknown-inspect-before-retry" : "elevation-timeout"};
    }
    inline Outcome Apply(const Plan& PlanValue, MetadataPatch Patch, std::shared_ptr<IWritableStorage> Writer,
        std::shared_ptr<operations::IFileElevationService> Service, OperationContext Context, Approval Ask,
        std::function<void(std::size_t, const EntrySnapshot&)> Progress = {})
    {
        Outcome Result;
        if (!Patch.Valid() || PlanValue.blocked_ || PlanValue.cancelled_) { Result.pending_ = PlanValue.entries_.size(); return Result; }
        for (const auto& Entry : PlanValue.entries_) {
            if (Context.stopToken_.stop_requested()) break;
            if (Progress) Progress(Result.items_.size(), Entry);
            const auto Desired = (Entry.attributes_ | Patch.setMask_) & ~Patch.clearMask_;
            if (Desired == Entry.attributes_ && (!Patch.modifiedUtcMs_ || *Patch.modifiedUtcMs_ == Entry.modifiedUtcMs_)) {
                ++Result.unchanged_; Result.items_.push_back({Entry, "변경 없음", {}}); continue;
            }
            auto Changed = Writer->PatchMetadata(Entry, Patch, Context);
            if (!Changed.HasValue() && Changed.Failure().category_ == ErrorCategory::AccessDenied
                && Entry.locator_.kind_ == LocatorKind::Local && Service) {
                const auto Choice = Ask(Entry, false);
                if (Choice == Decision::Stop) break;
                if (Choice == Decision::Skip) { ++Result.skipped_; Result.items_.push_back({Entry, "건너뜀", "관리자 변경을 요청하지 않았습니다."}); continue; }
                Changed = ElevatedPatch(Entry, Patch, Service, Context);
            }
            if (Changed.HasValue()) { ++Result.changed_; Result.items_.push_back({Entry, "성공", {}}); }
            else {
                const auto& E = Changed.Failure(); ++Result.failed_;
                const bool Ambiguous = E.code_.find("unknown") != std::string::npos || E.code_.find("applied") != std::string::npos;
                Result.items_.push_back({Entry, "실패", QString::fromStdString(E.code_), !Ambiguous && E.category_ != ErrorCategory::StaleRevision});
                if (E.category_ == ErrorCategory::Cancelled || Ambiguous) break;
            }
        }
        Result.pending_ = PlanValue.entries_.size() - Result.items_.size();
        return Result;
    }
}
