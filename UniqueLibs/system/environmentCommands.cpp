#include "stdafx.h"
#include "environmentCommands.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/cmpPanel.hpp"
#include "HyperCommander/UIs/Components/cmpEnvironmentView.hpp"
#include "HyperCommander/UIs/Components/cmpEnvironmentDetailView.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "HyperCommander/UIs/environmentEditors.hpp"
#include "HyperCommander/UIs/environmentDialogs.hpp"
#include "HyperCommander/UIs/environmentCommandFinder.hpp"
#include "HyperCommander/UIs/environmentProfileEditor.hpp"
#include "HyperCommander/UIs/registryDialogs.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
#include "UniqueLibs/systemSessionService.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/ui/systemChangePreview.hpp"
#include "environmentTransfer.hpp"
#include "environmentProcessProtocol.hpp"
#include "environmentApplication.hpp"
/// 공통 라이브러리
#include "commonLibs/system/environmentNotification.hpp"
#include "commonLibs/system/environmentSensitivity.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QMainWindow>
#include <QStatusBar>
#include <QCloseEvent>
#include <QFileDialog>
#include <QSaveFile>
#include <QSysInfo>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentCommands, "hc.environment.commands")
namespace hc::environment {
namespace {
QString Caller() { return EnvironmentApplication::Shared()->CallerSid(); }
QString Machine() { return EnvironmentApplication::Shared()->MachineId(); }
}
EnvironmentCommands::EnvironmentCommands(QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active)
    : QObject(Main), main_(Main), bridge_(Bridge), active_(std::move(Active)) { Main->installEventFilter(this); EnvironmentApplication::Shared(); }
bool EnvironmentCommands::Handles(const QString& Command) { return Command.startsWith("hc.systemEnvironment.") || Command.startsWith("hc.environment.") || Command == "hc.navigate.systemEnvironment"; }
QString EnvironmentCommands::directory() const { return QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("environment-backup"); }
CEnvironmentView* EnvironmentCommands::ActiveView() const
{
    auto* Panel = active_ ? active_() : nullptr; if (!Panel) return nullptr;
    if (auto* Detail = qobject_cast<CEnvironmentDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) return Detail->Source();
    return Panel->EnvironmentView();
}
bool EnvironmentCommands::Active() const
{
    auto* Panel = active_ ? active_() : nullptr;
    return Panel && (Panel->EnvironmentView() || qobject_cast<CEnvironmentDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex())));
}
CmpPanel* EnvironmentCommands::detailPanel() const
{
    auto* Panel = active_ ? active_() : nullptr; if (!Panel || !main_) return nullptr;
    if (qobject_cast<CEnvironmentDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) return Panel;
    auto* View = Panel->EnvironmentView(); if (!View) return nullptr;
    for (auto* Candidate : main_->findChildren<CmpPanel*>())
        if (auto* Detail = qobject_cast<CEnvironmentDetailView*>(Candidate->GetFocusView(Candidate->CurrentTabIndex())); Detail && Detail->Source() == View) return Candidate;
    return nullptr;
}
bool EnvironmentCommands::DetailOpen() const { return detailPanel() != nullptr; }
bool EnvironmentCommands::CloseDetail() { auto* Panel = detailPanel(); if (!Panel) return false; Panel->CloseQuickView(Panel->CurrentTabIndex()); return true; }
void EnvironmentCommands::ToggleDetail() { if (!CloseDetail()) if (auto* View = ActiveView()) { openDetail(View); View->EnsureKeyboardFocusOnView(); } }
CEnvironmentView* EnvironmentCommands::Open(const QVariant& Argument)
{
    auto* Panel = active_ ? active_() : nullptr; if (!Panel) return nullptr;
    const auto Options = Argument.toMap();
    if (Options.contains("pid")) {
        auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
        if (Left && Right) Panel = Panel == Left ? Right : Left;
    }
    auto* View = Panel->ShowEnvironmentView(); if (!View) return nullptr;
    if (Options.contains("scope")) View->SetScope(EnvironmentViewScope(Options["scope"].toInt()));
    if (Options.contains("name")) View->Select(Options["name"].toString());
    if (Options.contains("pid")) {
        ProcessEnvironment Empty; Empty.status_ = ProcessReadStatus::ReadFailed;
        View->SetProcess({Options["pid"].toUInt(), Options["created"].toString().toULongLong()}, Empty); process(View);
    }
    return View;
}
CEnvironmentDetailView* EnvironmentCommands::openDetail(CEnvironmentView* View)
{
    if (!View || !main_) return nullptr;
    auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
    auto* Other = Left && Left->EnvironmentView() == View ? Right : Left; if (!Other) return nullptr;
    auto* Detail = Other->OpenEnvironmentDetailView(Other->CurrentTabIndex(), View); if (!Detail) return nullptr;
    if (!Detail->property("hc.environment.connected").toBool()) {
        Detail->setProperty("hc.environment.connected", true);
        connect(Detail, &CEnvironmentDetailView::SaveRequested, this, [this, Detail](const Value& Original, const Value& Draft) {
            if (Detail->Source()) change(Detail->Source(), {{Original.scope_, QString::fromStdWString(Original.name_), Original, Draft}}, Detail);
        });
        connect(Detail, &CEnvironmentDetailView::FollowRequested, this, [this, Detail] { if (Detail->Source()) follow(Detail->Source()); });
    }
    return Detail;
}
void EnvironmentCommands::change(CEnvironmentView* View, QList<EnvironmentChange> Changes, CEnvironmentDetailView* Detail)
{
    if (busy_ || !View || Changes.empty()) return;
    busy_ = true;
    const auto Directory = directory(); const auto Sid = Caller(), MachineId = Machine();
    const auto Saved = Changes.size() == 1 ? Changes[0].after_ : std::optional<Value>();
    hc::sys::RunAsync(this, [Changes, Directory, Sid, MachineId, Backend = EnvironmentApplication::Shared()->Backend(), Notify = EnvironmentApplication::Shared()->Notification(), Native = EnvironmentApplication::Shared()->Native()] {
        hc::registry::RegistryChangeStore Store(Directory); EnvironmentStorage Storage(*Backend, Store, Sid, MachineId);
        return Storage.Preview(Changes);
    }, [this, View = QPointer<CEnvironmentView>(View), Detail = QPointer<CEnvironmentDetailView>(Detail), Saved](const EnvironmentPreview& Preview) {
        busy_ = false; if (!View) return;
        if (!Preview) { View->ShowNotice(tr("변경 미리 보기를 만들지 못했습니다: %1").arg(hc::sys::Win32ErrorText(Preview.error_))); return; }
        apply(View, Preview, Detail, Saved);
    });
}
void EnvironmentCommands::apply(CEnvironmentView* View, EnvironmentPreview Preview, CEnvironmentDetailView* Detail, std::optional<Value> Saved)
{
    if (busy_ || !View) return;
    if (!hc::ui::ConfirmSystemChanges(main_, Preview.plan_, tr("환경 변수 변경 미리 보기"))) return;
    busy_ = true; if (Detail) Detail->SetBusy(true);
    const auto Directory = directory(), Sid = Caller(), MachineId = Machine();
    hc::sys::RunAsync(this, [Preview, Directory, Sid, MachineId, Backend = EnvironmentApplication::Shared()->Backend(), Notify = EnvironmentApplication::Shared()->Notification(), Native = EnvironmentApplication::Shared()->Native()] {
        hc::registry::RegistryChangeStore Store(Directory);
        bool First = true;
        hc::registry::RegistryRemoteSession Remote([&First](const QString& Kind, QJsonObject Request) {
            if (First) { First = false; return hc::sys::AdminSession().Request(Kind, std::move(Request)); }
            return hc::sys::AdminSession().RequestIfConnected(Kind, std::move(Request), 60000);
        });
        EnvironmentStorage Storage(*Backend, Store, Sid, MachineId, Native ? &Remote : nullptr);
        auto Result = Storage.Commit(Preview);
        if (Result.applied_.completed) Notify();
        return Result;
    }, [this, View = QPointer<CEnvironmentView>(View), Detail = QPointer<CEnvironmentDetailView>(Detail), Saved](const EnvironmentCommit& Result) {
        busy_ = false;
        if (Result.applied_.completed) EnvironmentApplication::Shared()->Refresh();
        const auto Error = Result.applied_.error ? hc::sys::Win32ErrorText(Result.applied_.error) : QString();
        if (Detail && Saved) Detail->SaveFinished(*Saved, Error);
        if (View) { View->ShowNotice(Error.isEmpty() ? tr("저장했습니다. 실행 중인 프로그램은 새 환경을 받으려면 다시 시작해야 할 수 있습니다.") : tr("저장 실패: %1 · 적용된 단계 %2개. 변경 기록에서 복구 상태를 확인하세요.").arg(Error).arg(Result.applied_.completed)); View->Refresh(); }
        if (hc::sys::AdminSessionConnected()) hc::sys::NotifyAdminConnected();
    });
}
void EnvironmentCommands::profile(CEnvironmentView* View, const QString& Operation)
{
    if (busy_ || !View) return;
    const auto Original = View->CurrentProfile();
    auto Definition = Original;
    const bool EditDialog = Operation == "newProfile" || Operation == "editProfile" || Operation == "editValue";
    const bool Edit = EditDialog || Operation == "addValue" || Operation == "deleteValue" || Operation == "renameValue";
    const auto Selected = View->Current() && View->Current()->value_ ? QString::fromStdWString(View->Current()->value_->name_) : QString();
    if (EditDialog) {
        if (Operation != "newProfile" && !Definition) { View->ShowNotice(tr("먼저 프로필을 선택하거나 새 프로필을 만드세요.")); return; }
        Definition = EditEnvironmentProfile(main_, Operation == "newProfile" ? EnvironmentProfile{} : *Definition,
            Operation == "editValue" ? Selected : QString());
        if (!Definition) return;
    } else if (Edit) {
        if (!Definition) { View->ShowNotice(tr("먼저 새 프로필을 만드세요.")); return; }
        if (Operation == "addValue") {
            const auto Added = AskEnvironmentValue(main_, Scope::User); if (!Added) return;
            for (const auto& Item : Definition->values_) if (SameName(Item.name_, Added->name_)) { View->ShowNotice(tr("프로필에 같은 이름의 변수가 있습니다.")); return; }
            Definition->values_.push_back(*Added);
        } else {
            const auto Found = std::find_if(Definition->values_.begin(), Definition->values_.end(), [&](const auto& Item) { return SameName(Item.name_, Selected.toStdWString()); });
            if (Found == Definition->values_.end()) { View->ShowNotice(tr("이 변수는 프로필이 바꾸는 항목이 아닙니다.")); return; }
            if (Operation == "deleteValue") Definition->values_.erase(Found);
            else {
                const auto Name = hc::ui::AskRegistryName(main_, tr("프로필 변수 이름 바꾸기"), Selected, false); if (!Name) return;
                for (const auto& Item : Definition->values_) if (&Item != &*Found && SameName(Item.name_, Name->toStdWString())) { View->ShowNotice(tr("같은 이름이 있습니다.")); return; }
                Found->name_ = Name->toStdWString();
            }
        }
    }
    if (Edit && Definition->active_ && Definition->values_.empty()) { View->ShowNotice(tr("활성 프로필의 마지막 변수를 지우려면 먼저 프로필을 끄세요.")); return; }
    if (Operation != "load" && !Definition) return;
    if (Operation == "deleteProfile" && hc::ui::MessageDialog::question(main_, tr("프로필 삭제"),
        tr("프로필 '%1'을 삭제할까요? 활성 프로필은 먼저 꺼야 합니다.").arg(Definition->name_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    busy_ = true;
    const auto Directory = directory(), Sid = Caller(), MachineId = Machine();
    const auto File = QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("environment-profiles.json");
    struct Prepared {
        std::shared_ptr<EnvironmentProfiles> profiles_;
        EnvironmentPreview preview_;
        QString id_;
        quint32 error_ = 0;
        bool apply_ = false;
    };
    hc::sys::RunAsync(this, [File, Directory, Sid, MachineId, Definition, Original, Operation, Edit, Backend = EnvironmentApplication::Shared()->Backend(), Notify = EnvironmentApplication::Shared()->Notification(), Native = EnvironmentApplication::Shared()->Native()] {
        Prepared Result; Result.profiles_ = std::make_shared<EnvironmentProfiles>(File);
        hc::registry::RegistryChangeStore Store(Directory);
        EnvironmentStorage Recovery(*Backend, Store, Sid, MachineId); Result.error_ = Recovery.RecoverLocalHistory();
        if (!Result.error_) Result.error_ = Result.profiles_->Load(Store);
        if (Result.error_ || Operation == "load") return Result;
        Result.id_ = Definition->id_;
        if (Original && Operation != "newProfile") {
            const auto& Items = Result.profiles_->Items();
            const auto Fresh = std::find_if(Items.cbegin(), Items.cend(), [&](const auto& Item) { return Item.id_ == Original->id_; });
            if (Fresh == Items.cend() || Fresh->name_ != Original->name_ || Fresh->values_ != Original->values_
                || Fresh->active_ != Original->active_ || Fresh->pendingId_ != Original->pendingId_ || Fresh->backupId_ != Original->backupId_) {
                Result.error_ = ERROR_REVISION_MISMATCH; return Result;
            }
        }
        if (Operation == "deleteProfile") { Result.error_ = Result.profiles_->Remove(Result.id_); return Result; }
        if (Edit && !Definition->active_) { Result.error_ = Result.profiles_->Put(*Definition); return Result; }
        EnvironmentStorage Storage(*Backend, Store, Sid, MachineId);
        Result.preview_ = Edit ? Result.profiles_->PreviewUpdate(*Definition, Storage) : Result.profiles_->PreviewToggle(Result.id_, Storage, Store);
        Result.error_ = Result.preview_.error_; Result.apply_ = !Result.error_; return Result;
    }, [this, View = QPointer<CEnvironmentView>(View), Directory, Sid, MachineId](const Prepared& Result) {
        busy_ = false; if (!View) return;
        if (Result.error_) { View->ShowNotice(tr("프로필 작업을 진행할 수 없습니다: %1").arg(hc::sys::Win32ErrorText(Result.error_))); return; }
        if (!Result.apply_) {
            View->SetProfiles(Result.profiles_->Items());
            for (const auto& Item : Result.profiles_->Items()) if (Item.id_ == Result.id_) { View->SetProfile(Item.id_, Item.values_); break; }
            return;
        }
        if (!hc::ui::ConfirmSystemChanges(main_, Result.preview_.plan_, tr("프로필 변경 미리 보기"))) return;
        busy_ = true;
        hc::sys::RunAsync(this, [Result, Directory, Sid, MachineId, Backend = EnvironmentApplication::Shared()->Backend(), Notify = EnvironmentApplication::Shared()->Notification(), Native = EnvironmentApplication::Shared()->Native()] {
            hc::registry::RegistryChangeStore Store(Directory);
            EnvironmentStorage Storage(*Backend, Store, Sid, MachineId);
            const auto Applied = Result.profiles_->Toggle(Result.id_, Result.preview_, Storage);
            if (Applied.applied_.completed) Notify();
            return Applied;
        }, [this, View, Profiles = Result.profiles_](const EnvironmentCommit& Applied) {
            busy_ = false; if (Applied.applied_.completed) EnvironmentApplication::Shared()->Refresh();
            if (!View) return;
            View->SetProfiles(Profiles->Items()); View->Refresh();
            View->ShowNotice(Applied.applied_.error ? tr("프로필 적용 실패: %1").arg(hc::sys::Win32ErrorText(Applied.applied_.error))
                : tr("프로필을 적용했습니다. 이미 실행 중인 프로그램에는 다시 시작이 필요할 수 있습니다."));
        });
    });
}
void EnvironmentCommands::history(CEnvironmentView* View)
{
    const auto Directory = directory(), Sid = Caller(), MachineId = Machine();
    hc::sys::RunAsync(this, [Directory, Sid, MachineId, Backend = EnvironmentApplication::Shared()->Backend()] {
        hc::registry::RegistryChangeStore Store(Directory); EnvironmentStorage Storage(*Backend, Store, Sid, MachineId);
        const auto Error = Storage.RecoverLocalHistory(); auto Records = Store.History();
        if (Error) Records.error = Error;
        return Records;
    },
        [View = QPointer<CEnvironmentView>(View)](const auto& Result) { if (View) { View->SetHistory(Result.value); if (!Result) View->ShowNotice(QObject::tr("변경 기록을 읽지 못했습니다.")); } });
}
void EnvironmentCommands::undoRedo(CEnvironmentView* View, bool Redo)
{
    if (busy_) return;
    const auto Directory = directory(), Sid = Caller(), MachineId = Machine();
    const auto Selected = View->CurrentHistory();
    busy_ = true;
    hc::sys::RunAsync(this, [Directory, Sid, MachineId, Redo, Selected, Backend = EnvironmentApplication::Shared()->Backend(), Notify = EnvironmentApplication::Shared()->Notification(), Native = EnvironmentApplication::Shared()->Native()] {
        hc::registry::RegistryChangeStore Store(Directory);
        EnvironmentStorage Recovery(*Backend, Store, Sid, MachineId);
        if (const auto Error = Recovery.RecoverLocalHistory()) return EnvironmentPreview{{}, Error};
        const auto Records = Store.History(); if (!Records) return EnvironmentPreview{{}, Records.error};
        hc::registry::RegistryHistoryModel Model; Model.SetRecords(Records.value); EnvironmentStorage Storage(*Backend, Store, Sid, MachineId);
        if (!Redo && Selected) { const auto Disk = Store.Read(Selected->plan.id); if (Disk) return Storage.Undo(Disk.value); return EnvironmentPreview{{}, Disk.error}; }
        if (Redo) { const auto Candidate = Model.RedoCandidate(); if (Candidate) return Storage.Redo(Candidate->first, Candidate->second); }
        else { const auto Candidate = Model.UndoCandidate(); if (Candidate) return Storage.Undo(*Candidate); }
        return EnvironmentPreview{{}, ERROR_NOT_FOUND};
    }, [this, View = QPointer<CEnvironmentView>(View)](const EnvironmentPreview& Preview) {
        busy_ = false; if (!View) return; if (!Preview) View->ShowNotice(tr("되돌리기·다시 실행할 수 없습니다: %1").arg(hc::sys::Win32ErrorText(Preview.error_))); else apply(View, Preview);
    });
}
void EnvironmentCommands::follow(CEnvironmentView* View)
{
    const auto* Row = View->Current(); if (!Row || !Row->value_) return;
    auto* Panel = active_ ? active_() : nullptr; if (!Panel) return;
    if (Row->entry_) {
        if (Row->masked_) return;
        auto Path = Row->text_;
        if (const auto& Snapshot = View->Session()->Snapshot(); Row->value_->type_ == ValueType::ExpandString)
            Path = QString::fromStdWString(ExpandAtStage(Path.toStdWString(), Row->value_->scope_ == Scope::System ? Snapshot.merged_.afterSystemStrings_ : Snapshot.merged_.beforeUserExpansion_));
        if (Path.contains('%') || !QDir::isAbsolutePath(Path)) { View->ShowNotice(tr("해결되지 않았거나 상대적인 경로입니다.")); return; }
        Panel->AddTab(); Panel->RevealFile(Path); return;
    }
    Scope Source = Row->value_->scope_;
    if (Source != Scope::System && Source != Scope::User) {
        const auto Found = View->Session()->Snapshot().merged_.values_.find(Row->value_->name_);
        if (Found != View->Session()->Snapshot().merged_.values_.end()) for (auto It = Found->second.origins_.rbegin(); It != Found->second.origins_.rend(); ++It)
            if (It->value_.scope_ == Scope::System || It->value_.scope_ == Scope::User) { Source = It->value_.scope_; break; }
    }
    if (Source != Scope::System && Source != Scope::User) { View->ShowNotice(tr("이 값은 레지스트리의 환경 변수 항목에서 온 값이 아닙니다.")); return; }
    const auto Name = QString::fromStdWString(Row->value_->name_); const auto Location = EnvironmentLocation(Source);
    Panel->AddTab(); if (auto* Registry = Panel->ShowRegistryView()) Registry->Navigate(Location.Path(), Name);
}
void EnvironmentCommands::transfer(CEnvironmentView* View, bool Import)
{
    const auto Path = Import ? QFileDialog::getOpenFileName(main_, tr("환경 변수 가져오기"), {}, tr("환경 변수 (*.json *.reg *.env);;모든 파일 (*)"))
        : QFileDialog::getSaveFileName(main_, tr("환경 변수 내보내기"), {}, tr("JSON (*.json);;레지스트리 (*.reg);;환경 파일 (*.env)"));
    if (Path.isEmpty()) return;
    const auto Format = Path.endsWith(".reg", Qt::CaseInsensitive) ? EnvironmentFormat::Reg : Path.endsWith(".json", Qt::CaseInsensitive) ? EnvironmentFormat::Json : EnvironmentFormat::DotEnv;
    if (Import) {
        QFile File(Path); if (!File.open(QIODevice::ReadOnly) || File.size() > 16 * 1024 * 1024) { View->ShowNotice(tr("파일을 열 수 없거나 너무 큽니다.")); return; }
        const auto Parsed = ParseEnvironmentFile(File.readAll(), Format); if (!Parsed) { View->ShowNotice(tr("가져오기 오류: %1").arg(Parsed.error_)); return; }
        QList<EnvironmentChange> Changes; const auto& Input = View->Session()->Snapshot().input_;
        for (const auto& Item : Parsed.values_) {
            std::optional<Value> Before; const auto& Current = Item.scope_ == Scope::System ? Input.system_ : Input.user_;
            for (const auto& Value : Current) if (SameName(Value.name_, Item.name_.toStdWString())) { Before = Value; break; }
            Changes.append({Item.scope_, Item.name_, Before, Item.value_});
        }
        change(View, Changes); return;
    }
    auto Values = View->Session()->Snapshot().input_.system_; const auto& User = View->Session()->Snapshot().input_.user_; Values.insert(Values.end(), User.begin(), User.end());
    const auto Export = ExportEnvironmentFile(Values, Format); if (!Export) { View->ShowNotice(tr("이 형식으로 내보낼 수 없습니다.")); return; }
    QSaveFile File(Path); if (!File.open(QIODevice::WriteOnly) || File.write(Export.value) != Export.value.size() || !File.commit()) View->ShowNotice(tr("파일을 저장하지 못했습니다."));
}
void EnvironmentCommands::process(CEnvironmentView* View, bool StartHelper)
{
    if (!View->ProcessKey()) { View->ShowNotice(tr("프로세스 보기의 환경 변수 명령에서 비교할 프로세스를 선택하세요.")); return; }
    const auto Target = *View->ProcessKey();
    hc::sys::RunAsync(this, [Target, StartHelper, Reader = EnvironmentApplication::Shared()->ProcessReader(), Native = EnvironmentApplication::Shared()->Native()] {
        auto Result = Reader(Target);
        if (!Native || Result.status_ != ProcessReadStatus::Denied) return Result;
        if (!StartHelper && !hc::sys::AdminSessionConnected()) return Result;
        bool First = true;
        return ReadRemoteEnvironment(Target, [&](const QString& Kind, QJsonObject Payload) {
            const bool Launch = StartHelper && First; First = false;
            return Launch ? hc::sys::AdminSession().Request(Kind, std::move(Payload)) : hc::sys::AdminSession().RequestIfConnected(Kind, std::move(Payload));
        });
    }, [View = QPointer<CEnvironmentView>(View), Target](const ProcessEnvironment& Result) { if (View && View->ProcessKey() == Target) View->SetProcess(Target, Result); });
}
bool EnvironmentCommands::Execute(const QString& Command, const QVariant& Argument)
{
    if (Command == "hc.viewer.quick" && ActiveView()) { ToggleDetail(); return true; }
    if (!Handles(Command)) return false;
    if (Command == "hc.navigate.systemEnvironment") { Open(Argument); return true; }
    auto* View = ActiveView(); if (!View) return true;
    if (Command == "hc.environment.oldProcesses") {
        const auto Key = FindStaleEnvironmentProcess(main_, EffectiveEnvironment(View->Session()->Snapshot().merged_),
            EnvironmentApplication::Shared()->Native(), EnvironmentApplication::Shared()->ProcessReader());
        if (Key) { ProcessEnvironment Empty; Empty.status_ = ProcessReadStatus::ReadFailed; View->SetProcess(*Key, Empty); process(View); }
        return true;
    }
    if (Command == "hc.systemEnvironment.findCommand") {
        std::vector<NamedSearchContext> Contexts;
        auto Definitions = View->Session()->Snapshot().input_.system_;
        const auto& UserDefinitions = View->Session()->Snapshot().input_.user_;
        Definitions.insert(Definitions.end(), UserDefinitions.begin(), UserDefinitions.end());
        const bool SecretPath = SensitiveVariables(Definitions).contains(L"Path");
        const auto Add = [&](QString Name, const ValueMap& Values, std::optional<std::wstring> Directory) {
            SearchContext Context; Context.currentDirectory_ = std::move(Directory);
            if (const auto It = Values.find(L"Path"); It != Values.end()) if (!SecretPath) Context.path_ = ParseList(It->second).entries_;
            if (const auto It = Values.find(L"PATHEXT"); It != Values.end()) Context.pathExt_ = It->second;
            if (SecretPath) Name += tr(" · 비밀 참조 Path 검색 제외");
            Contexts.push_back({std::move(Name), std::move(Context)});
        };
        Add(tr("적용 결과"), EffectiveEnvironment(View->Session()->Snapshot().merged_), {});
        Add(tr("HyperCommander"), EnvironmentApplication::Shared()->OwnEnvironment(), QDir::currentPath().toStdWString());
        if (View->ProcessKey()) Add(tr("선택한 프로세스"), View->ProcessValues(), View->ProcessDirectory());
        const auto Path = FindEnvironmentCommand(main_, Contexts, View->PathProbe());
        if (!Path.isEmpty()) if (auto* Panel = active_()) { Panel->AddTab(); Panel->RevealFile(Path); }
        return true;
    }
    if (Command == "hc.environment.openOrigin") {
        const auto* Row = View->Current(); if (!Row || !Row->value_) return true;
        const auto Name = Row->value_->name_; const auto Found = View->Session()->Snapshot().merged_.values_.find(Name);
        if (Found != View->Session()->Snapshot().merged_.values_.end()) for (auto It = Found->second.origins_.rbegin(); It != Found->second.origins_.rend(); ++It) {
            if (It->value_.scope_ != Scope::User && It->value_.scope_ != Scope::System) continue;
            View->SetScope(It->value_.scope_ == Scope::User ? EnvironmentViewScope::User : EnvironmentViewScope::System);
            View->Select(QString::fromStdWString(Name)); return true;
        }
        View->ShowNotice(tr("이 값은 Windows 또는 프로세스가 만든 값입니다.")); return true;
    }
    for (const auto* Operation : {"newProfile", "editProfile", "deleteProfile"})
        if (Command == QStringLiteral("hc.environment.") + QString::fromLatin1(Operation)) { profile(View, QString::fromLatin1(Operation)); return true; }
    if (Command == "hc.environment.loadProfiles") { profile(View, "load"); return true; }
    if (View->Scope() == EnvironmentViewScope::Profile) {
        if (Command == "hc.systemEnvironment.profileToggle") { profile(View, "toggle"); return true; }
        for (const auto* Operation : {"new", "edit", "rename", "delete"})
            if (Command == QStringLiteral("hc.systemEnvironment.") + QString::fromLatin1(Operation)) { profile(View, QString::fromLatin1(Operation) == "new" ? "addValue" : QString::fromLatin1(Operation) + "Value"); return true; }
    }
    if (Command == "hc.environment.loadHistory") { history(View); return true; }
    if (Command == "hc.environment.elevateProcess") { process(View, true); return true; }
    if (Command == "hc.environment.refreshProcess") { process(View); return true; }
    if (Command == "hc.environment.import" || Command == "hc.environment.export") { transfer(View, Command.endsWith("import")); return true; }
    if (Command == "hc.systemEnvironment.undo" || Command == "hc.systemEnvironment.redo") { undoRedo(View, Command.endsWith("redo")); return true; }
    if (Command == "hc.systemEnvironment.follow") { follow(View); return true; }
    if (Command == "hc.systemEnvironment.describe" || Command == "hc.systemEnvironment.edit") {
        if (auto* Detail = openDetail(View)) { if (Command.endsWith("describe")) Detail->ShowDescription(); else Detail->ShowData(); } return true;
    }
    if (Command == "hc.systemEnvironment.scopeTree" || Command == "hc.systemEnvironment.copy") { View->ExecuteCommand(Command); return true; }
    if (busy_ || !View->CanExecuteCommand(Command)) return true;
    const auto* Row = View->Current();
    if (Command == "hc.systemEnvironment.new") {
        const auto Source = View->Scope() == EnvironmentViewScope::System ? Scope::System : Scope::User;
        const auto Value = AskEnvironmentValue(main_, Source); if (!Value) return true;
        change(View, {{Source, QString::fromStdWString(Value->name_), {}, *Value}}); return true;
    }
    if (!Row || !Row->value_) return true;
    const auto Before = *Row->value_; auto After = Before; const auto Name = QString::fromStdWString(Before.name_);
    if (Command == "hc.systemEnvironment.rename") {
        const auto New = hc::ui::AskRegistryName(main_, tr("환경 변수 이름 바꾸기"), Name, false); if (!New || New->isEmpty() || New->compare(Name, Qt::CaseInsensitive) == 0) return true;
        After.name_ = New->toStdWString(); change(View, {{Before.scope_, Name, Before, {}}, {Before.scope_, *New, {}, After}}); return true;
    }
    if (Command == "hc.systemEnvironment.moveScope") {
        After.scope_ = Before.scope_ == Scope::User ? Scope::System : Scope::User;
        change(View, {{Before.scope_, Name, Before, {}}, {After.scope_, Name, {}, After}}); return true;
    }
    if (Command == "hc.systemEnvironment.delete" && !Row->entry_) { change(View, {{Before.scope_, Name, Before, {}}}); return true; }
    auto Entries = QString::fromStdWString(Before.raw_).split(';', Qt::KeepEmptyParts);
    if (Command == "hc.systemEnvironment.addEntry") {
        const auto New = hc::ui::AskRegistryName(main_, tr("폴더 항목 추가"), {}, false); if (!New) return true;
        Entries.insert(Row->entry_ ? *Row->entry_ + 1 : Entries.size(), *New);
    } else if (Row->entry_ && Command == "hc.systemEnvironment.delete") Entries.removeAt(*Row->entry_);
    else if (Row->entry_ && (Command == "hc.systemEnvironment.moveUp" || Command == "hc.systemEnvironment.moveDown")) {
        const int To = *Row->entry_ + (Command.endsWith("moveUp") ? -1 : 1); if (To < 0 || To >= Entries.size()) return true; Entries.move(*Row->entry_, To);
    } else return true;
    After.raw_ = Entries.join(';').toStdWString(); change(View, {{Before.scope_, Name, Before, After}}); return true;
}
void EnvironmentCommands::RefreshState()
{
    auto* View = ActiveView();
    for (const auto* Name : {"moveUp", "moveDown", "rename", "describe", "edit", "new", "moveScope", "addEntry", "delete", "follow", "findCommand", "scopeTree", "copy", "undo", "redo", "profileToggle"}) {
        const auto Id = QStringLiteral("hc.systemEnvironment.") + QString::fromLatin1(Name);
        if (auto* Action = bridge_->Action(Id)) Action->setEnabled(View && !busy_ && View->CanExecuteCommand(Id));
    }
    if (View) for (const auto* Id : {"hc.navigate.refresh", "hc.navigate.forceRefresh", "hc.viewer.quick", "hc.search.focus"}) if (auto* Action = bridge_->Action(Id)) Action->setEnabled(true);
}
bool EnvironmentCommands::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == main_ && Event->type() == QEvent::Close) {
        if (busy_) { main_->statusBar()->showMessage(tr("환경 변수 저장이 끝난 뒤 닫을 수 있습니다.")); static_cast<QCloseEvent*>(Event)->ignore(); return true; }
        for (auto* Detail : main_->findChildren<CEnvironmentDetailView*>()) if (!Detail->RequestClose()) { static_cast<QCloseEvent*>(Event)->ignore(); return true; }
    }
    return QObject::eventFilter(Watched, Event);
}
}
