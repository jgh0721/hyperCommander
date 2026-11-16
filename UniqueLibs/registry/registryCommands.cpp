#include "stdafx.h"
#include "registryCommands.hpp"
#include "registryPolicyEdit.hpp"
#include "registryBatch.hpp"
#include "HyperCommander/UIs/registryBatchDialog.hpp"
#include "HyperCommander/UIs/registryPolicyEditor.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/cmpPanel.hpp"
#include "HyperCommander/UIs/registryDialogs.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryDetailView.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryFavorites.hpp"
#include "registryMutation.hpp"
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/systemSessionService.hpp"
#include "UniqueLibs/system/environmentModel.hpp"
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/system/environmentNotification.hpp"
/// 외부 라이브러리
#include <fmwidgets/Button.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>
#include <QClipboard>
#include <QMainWindow>
#include <QStatusBar>
#include <QSysInfo>
#include <QCloseEvent>
#include <QTabWidget>
#include <QFileDialog>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryCommands, "hc.registry.commands")

namespace hc::registry {
namespace {
RegistryMutationResult NotifyEnvironment(RegistryMutationResult Result)
{
    for (int Index = 0; Index < Result.applied.completed && Index < Result.record.plan.steps.size(); ++Index) {
        const auto& Location = Result.record.plan.steps[Index].location;
        const auto Key = Location.key.toLower();
        const bool User = Location.hive == Hive::CurrentUser && Key == "environment";
        const bool NamedUser = Location.hive == Hive::Users && Key == Result.record.plan.callerSid.toLower() + "\\environment";
        const bool Machine = Location.hive == Hive::LocalMachine && Key.startsWith("system\\")
            && Key.endsWith("\\control\\session manager\\environment");
        if (User || NamedUser || Machine) { Result.notificationError = hc::system::NotifyEnvironmentChanged(); break; }
    }
    return Result;
}
}
RegistryCommands::RegistryCommands(QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active)
    : QObject(Main), main_(Main), bridge_(Bridge), active_(std::move(Active))
{
    Main->installEventFilter(this);
    policyCancelButton_ = new fm::ui::Button(tr("정책 작업 취소"), Main);
    policyCancelButton_->setObjectName("hc.registry.policy.cancel");
    Main->statusBar()->addPermanentWidget(policyCancelButton_); policyCancelButton_->hide();
    connect(policyCancelButton_, &QPushButton::clicked, this, [this] {
        if (policyCancel_) policyCancel_->store(true);
        policyCancelButton_->setEnabled(false);
        if (main_) main_->statusBar()->showMessage(policyCancelButton_->property("registryTransfer").toBool() ? tr("취소 요청 중…") : tr("취소 요청 중… 정책 서비스가 이미 시작한 처리는 계속될 수 있습니다."));
    });
}
bool RegistryCommands::Handles(const QString& Command)
{
    return Command.startsWith("hc.registry.") || Command == "hc.system.registry.open" || Command == "hc.navigate.systemRegistry";
}
CRegistryView* RegistryCommands::Open(const QVariant& Argument)
{
    auto* Panel = active_ ? active_() : nullptr; if (!Panel) return nullptr;
    auto* View = Panel->ShowRegistryView(); if (!View) return nullptr;
    const auto Options = Argument.toMap();
    if (Options.contains("view")) {
        const auto Bits = Options["view"].toString();
        View->SetRegistryView(Bits == "32" || Bits.compare("wow32", Qt::CaseInsensitive) == 0 ? hc::registry::View::Wow32 : hc::registry::View::Native64);
    }
    const auto Path = Argument.metaType().id() == QMetaType::QVariantMap ? Options["path"].toString() : Argument.toString();
    const auto Value = Options.contains("value") ? std::optional<QString>(Options["value"].toString())
        : Options.contains("valueName") ? std::optional<QString>(Options["valueName"].toString()) : std::optional<QString>();
    if (!Path.isEmpty()) View->Navigate(Path, Value);
    return View;
}
bool RegistryCommands::Execute(const QString& Command, const QVariant& Argument)
{
    if (!Handles(Command)) return false;
    if (Command == "hc.system.registry.open" || Command == "hc.navigate.systemRegistry") { Open(Argument); return true; }
    if (Command == "hc.registry.importPreview") { const auto Path = QFileDialog::getOpenFileName(main_, tr("가져오기 미리 보기"), {}, tr("레지스트리 파일 (*.reg)")); if (!Path.isEmpty()) OpenImport(Path); return true; }
    auto* View = ActiveView();
    if (!View) return true;
    if (Command == "hc.registry.export" || Command == "hc.registry.exportResults") { exportData(View, Command == "hc.registry.exportResults"); return true; }
    if (Command == "hc.registry.compare") { compare(View); return true; }
    if (Command == "hc.registry.replaceResults") {
        if (busy_ || !View->CanExecuteCommand(Command)) return true;
        const auto Replacement = hc::ui::AskRegistryReplacement(main_); if (!Replacement) return true;
        auto Preview = RegistryBatch::Replace(View->Search()->Rows(), View->Search()->Options(), *Replacement,
            QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()));
        if (!Preview) { hc::ui::MessageDialog::warning(main_, tr("치환 미리 보기"), Preview.error); return true; }
        if (hc::ui::ConfirmRegistryReplacement(main_, Preview))
            runMutation(View, [Preview](RegistryMutation& Mutation) { return Mutation.ApplyPreview(Preview.operations, Preview.plan); });
        return true;
    }
    if (Command == "hc.registry.toggleAutorun") {
        const auto Entry = View->CurrentAutorun();
        if (!busy_ && Entry && Entry->canToggle) {
            const auto Target = Entry->switchLocation->Path();
            if (hc::ui::MessageDialog::question(main_, tr("자동 실행 사용 전환"),
                Entry->name + "\n" + Target + "\n" + tr("원래 등록은 보존하고 승인/차단 값만 변경합니다. 변경 전에 백업합니다.") + "\n" + Entry->warning,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
                FILETIME Time{}; GetSystemTimeAsFileTime(&Time); const quint64 Ticks = (quint64(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
                runMutation(View, [Entry = *Entry, Ticks](RegistryMutation& Mutation) { return RegistryAutoruns::Toggle(Mutation, Entry, Ticks); });
            }
        }
        return true;
    }
    if (Command == "hc.registry.open" && View->SearchShown()) return View->ExecuteCommand(Command);
    if (Command == "hc.registry.follow") {
        const auto Index = View->CurrentSource();
        const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView());
        if (Location && Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value)) {
            const bool User = Location->hive == Hive::CurrentUser && Location->key.compare("Environment", Qt::CaseInsensitive) == 0;
            const bool System = Location->hive == Hive::LocalMachine && Location->key.compare("SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", Qt::CaseInsensitive) == 0;
            if (User || System) {
                const auto Scope = User ? hc::environment::EnvironmentViewScope::User : hc::environment::EnvironmentViewScope::System;
                bridge_->ExecuteSystemCommand("hc.navigate.systemEnvironment", QVariantMap{{"scope", int(Scope)}, {"name", Index.data(RegistryTreeModel::ValueNameRole).toString()}});
                return true;
            }
        }
        if (auto* Detail = openDetail(View)) Detail->ShowLinks(true); return true;
    }
    if (Command == "hc.registry.open") if (const auto Entry = View->CurrentAutorun()) {
        View->SetRegistryView(Entry->location.view); View->Navigate(Entry->location.Path(), Entry->value.name); return true;
    }
    if (Command == "hc.registry.describe" || Command == "hc.registry.edit"
        || (Command == "hc.registry.open" && View->CurrentPolicy().has_value())
        || (Command == "hc.registry.open" && View->CurrentSource().data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value))) {
        if (auto* Detail = openDetail(View)) {
            if (Command == "hc.registry.describe") Detail->ShowDescription(); else Detail->ShowData();
        }
        return true;
    }
    if (Command == "hc.registry.undo" || Command == "hc.registry.redo") { if (!busy_) undoRedo(View, Command == "hc.registry.redo"); return true; }
    if (Command == "hc.registry.rename") {
        if (!busy_) {
            disconnect(View, &CRegistryView::sigRenameRequested, this, nullptr);
            connect(View, &CRegistryView::sigRenameRequested, this, [this, View](const QModelIndex& Index, const QString& Name) { rename(View, Index, Name); });
            View->BeginRename();
        }
        return true;
    }
    if (Command == "hc.registry.newKey" || Command == "hc.registry.newValue") {
        if (!busy_) create(View, Command == "hc.registry.newKey"); return true;
    }
    if (Command == "hc.registry.delete") { if (!busy_) remove(View); return true; }
    if (Command == "hc.registry.copyName" || Command == "hc.registry.copyData" || Command == "hc.registry.copyReg") {
        const auto Index = View->CurrentSource();
        if (Index.isValid() && Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value)) {
            const RegistryValue Value{Index.data(RegistryTreeModel::ValueNameRole).toString(), Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()};
            QString Text = Value.name;
            if (Command == "hc.registry.copyData") {
                const auto Decoded = RegistryValueCodec::DecodeText(Value);
                Text = Decoded.lossless ? Decoded.text : QString::fromLatin1(Value.bytes.toHex(' '));
            } else if (Command == "hc.registry.copyReg") {
                const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView());
                if (!Location) return true;
                Text = RegFileWriter::Text({{OperationKind::CreateKey, *Location}, {OperationKind::SetValue, *Location, Value}});
            }
            QApplication::clipboard()->setText(Text);
        }
        return true;
    }
    if (Command == "hc.registry.favorite") { if (!busy_) favorite(View); return true; }
    View->ExecuteCommand(Command); return true;
}
void RegistryCommands::RefreshState()
{
    auto* View = ActiveView();
    for (const auto& Id : {"hc.registry.describe", "hc.registry.edit", "hc.registry.rename", "hc.registry.newKey", "hc.registry.newValue", "hc.registry.delete", "hc.registry.copyName", "hc.registry.copyData", "hc.registry.copyReg", "hc.registry.replaceResults", "hc.registry.exportResults", "hc.registry.export", "hc.registry.compare", "hc.registry.importPreview"})
        if (auto* Action = bridge_->Action(Id)) Action->setEnabled(View && !busy_ && View->CanExecuteCommand(Id));
    if (auto* Action = bridge_->Action("hc.registry.undo")) Action->setEnabled(View && !busy_ && !View->History()->Loading() && View->History()->UndoCandidate().has_value());
    if (auto* Action = bridge_->Action("hc.registry.redo")) Action->setEnabled(View && !busy_ && !View->History()->Loading() && View->History()->RedoCandidate().has_value());
    if (auto* Action = bridge_->Action("hc.registry.favorite"))
        Action->setEnabled(View && !busy_ && RegistryLocation::Parse(View->PathText()).has_value());
}
CRegistryView* RegistryCommands::ActiveView() const
{
    auto* Panel = active_ ? active_() : nullptr; if (!Panel) return nullptr;
    if (auto* Detail = qobject_cast<CRegistryDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex()))) return Detail->Source();
    return Panel->RegistryView();
}
bool RegistryCommands::Active() const
{
    auto* Panel = active_ ? active_() : nullptr;
    return Panel && (qobject_cast<CRegistryDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex())) || Panel->RegistryView());
}
CmpPanel* RegistryCommands::currentDetailPanel() const
{
    auto* ActivePanel = active_ ? active_() : nullptr;
    if (!ActivePanel || !main_) return nullptr;
    if (qobject_cast<CRegistryDetailView*>(ActivePanel->GetFocusView(ActivePanel->CurrentTabIndex()))) return ActivePanel;
    auto* View = ActivePanel->RegistryView();
    if (!View) return nullptr;
    for (auto* Panel : main_->findChildren<CmpPanel*>())
        if (auto* Detail = qobject_cast<CRegistryDetailView*>(Panel->GetFocusView(Panel->CurrentTabIndex()));
            Detail && Detail->Source() == View) return Panel;
    return nullptr;
}
bool RegistryCommands::DetailOpen() const { return currentDetailPanel() != nullptr; }
bool RegistryCommands::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == main_ && Event->type() == QEvent::Close && (busy_ || compareProcesses_)) {
        if (main_) main_->statusBar()->showMessage(compareProcesses_ ? tr("HyperCompare가 임시 비교 파일을 사용 중입니다. 비교 창을 닫은 뒤 종료하세요.") : tr("레지스트리 작업이 끝난 뒤 닫을 수 있습니다."), 5000);
        static_cast<QCloseEvent*>(Event)->ignore(); return true;
    }
    if (Watched == main_ && Event->type() == QEvent::Close) {
        for (auto* Detail : main_->findChildren<CRegistryDetailView*>()) if (!Detail->RequestClose()) {
            static_cast<QCloseEvent*>(Event)->ignore(); return true;
        }
    }
    return QObject::eventFilter(Watched, Event);
}
CRegistryDetailView* RegistryCommands::openDetail(CRegistryView* View)
{
    if (!View || !main_) return nullptr;
    CmpPanel* SourcePanel = nullptr;
    for (auto* Panel : main_->findChildren<CmpPanel*>()) if (Panel->RegistryView() == View) { SourcePanel = Panel; break; }
    auto* Left = main_->findChild<CmpPanel*>("cmpLeftPanel"); auto* Right = main_->findChild<CmpPanel*>("cmpRightPanel");
    auto* Other = SourcePanel == Left ? Right : SourcePanel == Right ? Left : nullptr; if (!Other) return nullptr;
    auto* Detail = Other->OpenRegistryDetailView(Other->CurrentTabIndex(), View); if (!Detail) return nullptr;
    if (!Detail->property("hc.registry.commandsConnected").toBool()) {
        Detail->setProperty("hc.registry.commandsConnected", true);
        connect(Detail, &CRegistryDetailView::LinkRequested, this, [this, Detail](const RegistryLink& Link) {
            if (Link.target && Detail->Source()) {
                auto* View = Detail->Source(); View->SetRegistryView(Link.target->view);
                View->Navigate(Link.target->Path(), Link.target == Link.source ? std::optional<QString>(Link.sourceName) : std::nullopt);
            } else if (!Link.file.isEmpty()) {
                auto* Panel = active_ ? active_() : nullptr;
                if (Panel) { Panel->AddTab(); Panel->RevealFile(Link.file); emit Panel->sig_NotifyPanelActivated(); Panel->SetFocusView(Panel->CurrentTabIndex()); }
            }
        });
        connect(Detail, &CRegistryDetailView::PolicySaveRequested, this, [this, Detail] { savePolicy(Detail); });
        connect(Detail, &CRegistryDetailView::PolicyCancelRequested, policyCancelButton_, &QPushButton::click);
        connect(Detail, &CRegistryDetailView::SaveRequested, this,
            [this, Detail](const RegistryLocation& Location, const RegistryValue& Original, const RegistryValue& Draft) { saveValue(Detail, Location, Original, Draft); });
    }
    return Detail;
}
void RegistryCommands::ToggleDetail()
{
    if (CloseDetail()) return;
    if (auto* View = ActiveView()) { openDetail(View); View->EnsureKeyboardFocusOnView(); }
}
bool RegistryCommands::CloseDetail()
{
    auto* Panel = currentDetailPanel();
    if (!Panel) return false;
    Panel->CloseQuickView(Panel->CurrentTabIndex());
    return true;
}
void RegistryCommands::savePolicy(CRegistryDetailView* Detail)
{
    if (busy_ || !Detail || !Detail->Source()) return;
    const auto Entry = Detail->PolicyEditor()->Entry();
    const auto Plan = RegistryPolicyEdit::Prepare(Entry.policy, Entry.machine, Detail->PolicyEditor()->State(),
        Detail->PolicyEditor()->Options(), Entry.observation, Entry.localPolicy);
    if (!Plan) { Detail->PolicyEditor()->ShowError(Plan.error); return; }
    QStringList Preview{tr("로컬 %1 정책과 실제 값 %2개를 변경합니다. Registry.pol과 값의 원본을 먼저 백업합니다.")
        .arg(Entry.machine ? tr("컴퓨터") : tr("사용자")).arg(Plan.expectedValues.size())};
    for (const auto& Step : Plan.expectedValues) {
        const auto Format = [](const std::optional<RegistryValue>& Value) {
            if (!Value) return tr("(없음)"); const auto Text = RegistryValueCodec::DecodeText(*Value); const auto Number = RegistryValueCodec::DecodeNumber(*Value);
            return Text.lossless ? Text.text.left(160) : Number ? QString::number(Number.value) : QString::fromLatin1(Value->bytes.left(32).toHex(' '));
        };
        Preview.append(Step.location.Path() + " › " + Step.name + "\n" + Format(Step.before) + QStringLiteral(" → ") + Format(Step.after));
    }
    if (hc::ui::MessageDialog::question(Detail, tr("정책 변경 미리 보기"), Preview.join("\n\n"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    Detail->SetBusy(true);
    runMutation(Detail->Source(), [Machine = Entry.machine, Plan](RegistryMutation& Mutation) {
        return Mutation.ApplyPolicy(Machine, Plan.beforePolicy, Plan.afterPolicy, Plan.operations, Plan.expectedValues);
    }, [Guard = QPointer<CRegistryDetailView>(Detail)](CRegistryView*, const QString& Error) {
        if (Guard) Guard->PolicySaveFinished(Error);
    });
}
void RegistryCommands::saveValue(CRegistryDetailView* Detail, const RegistryLocation& Location, const RegistryValue& Original, const RegistryValue& Draft)
{
    if (busy_) return;
    const auto Directory = QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-backup");
    busy_ = true; Detail->SetBusy(true); RefreshState();
    hc::sys::RunAsync(this, [Location, Original, Draft, Directory] {
        WinRegistryBackend Backend; RegistryChangeStore Store(Directory);
        // Only the initial request may open UAC. All remaining chunks/apply/status
        // stay on that session and never reconnect/replay a prepared operation.
        RegistryRemoteSession Remote([Started = false](const QString& Kind, QJsonObject Payload) mutable {
            const bool First = !std::exchange(Started, true);
            return First ? hc::sys::AdminSession().Request(Kind, std::move(Payload), 120000)
                : hc::sys::AdminSession().RequestIfConnected(Kind, std::move(Payload), 60000);
        });
        RegistryMutation Mutation(Backend, Store, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()), &Remote);
        return NotifyEnvironment(Mutation.SaveValue(Location, Original, Draft));
    }, [this, Guard = QPointer<CRegistryDetailView>(Detail), Draft](const RegistryMutationResult& Result) {
        busy_ = false; RefreshState(); QString Error;
        if (Result.applied.error) {
            Error = Result.applied.conflict ? tr("편집을 시작한 뒤 원본이 바뀌었습니다. 현재 값을 다시 읽고 변경 내용을 확인하세요.") : hc::sys::Win32ErrorText(Result.applied.error);
            if (Result.applied.error == ERROR_CANCELLED && Result.elevated && Result.record.directory.isEmpty())
                Error = tr("관리자 권한 요청(UAC)이 취소되었습니다. 레지스트리를 변경하지 않았습니다.");
            if (!Result.record.directory.isEmpty()) Error += tr("\n백업: %1").arg(Result.record.directory);
            if (Result.applied.uncertain) Error += tr("\n처리 결과를 확인할 수 없습니다. 변경 기록에서 복구 상태를 확인하세요.");
        }
        if (Guard) { Guard->SaveFinished(Draft, Error); if (Guard->Source()) Guard->Source()->ReloadHistory(); }
        if (Result.notificationError && main_) main_->statusBar()->showMessage(tr("환경 변수 변경은 저장됐지만 일부 프로그램에 알리지 못했습니다. 필요한 프로그램을 다시 시작하세요."), 12000);
        if (hc::sys::AdminSessionConnected()) hc::sys::NotifyAdminConnected();
    });
}
void RegistryCommands::runMutation(CRegistryView* View, std::function<RegistryMutationResult(RegistryMutation&)> Work,
    std::function<void(CRegistryView*, const QString&)> Done)
{
    if (busy_) return;
    const auto Directory = QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-backup");
    busy_ = true; policyCancel_ = std::make_shared<std::atomic_bool>(false); RefreshState();
    hc::sys::RunAsync(this, [Directory, Work = std::move(Work), Guard = QPointer<RegistryCommands>(this), Cancel = policyCancel_] {
        WinRegistryBackend Backend; RegistryChangeStore Store(Directory);
        RegistryRemoteSession Remote([Started = false](const QString& Kind, QJsonObject Payload) mutable {
            const bool First = !std::exchange(Started, true);
            return First ? hc::sys::AdminSession().Request(Kind, std::move(Payload), 120000)
                : hc::sys::AdminSession().RequestIfConnected(Kind, std::move(Payload), 60000);
        });
        Remote.SetPolicyControl(Cancel.get(), [Guard, Cancel](int Phase) {
            if (!Guard) return;
            QMetaObject::invokeMethod(Guard, [Guard, Cancel, Phase] {
                if (!Guard || Guard->policyCancel_ != Cancel || !Guard->main_) return;
                Guard->policyCancelButton_->show(); Guard->policyCancelButton_->setEnabled(!Cancel->load());
                const QStringList Stages{tr("로컬 정책 저장 중…"), tr("정책 새로 고침 완료를 기다리는 중…"), tr("적용된 정책 값을 검증하는 중…")};
                Guard->main_->statusBar()->showMessage(Stages.value(Phase));
            }, Qt::QueuedConnection);
        });
        RegistryMutation Mutation(Backend, Store, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()), &Remote);
        return NotifyEnvironment(Work(Mutation));
    }, [this, Guard = QPointer<CRegistryView>(View), Done = std::move(Done)](const RegistryMutationResult& Result) {
        busy_ = false; policyCancel_.reset(); policyCancelButton_->hide();
        if (main_) main_->statusBar()->clearMessage();
        RefreshState(); QString Error;
        if (Result.applied.error) {
            Error = Result.applied.conflict ? tr("원본이나 대상이 바뀌었습니다. 다시 읽고 확인하세요.") : hc::sys::Win32ErrorText(Result.applied.error);
            if (Result.applied.error == ERROR_CANCELLED) Error = tr("정책 작업을 취소했습니다. 변경 기록에서 적용 상태를 확인하세요.");
            if (Result.applied.error == ERROR_TIMEOUT) Error = tr("정책 새로 고침 대기 시간이 지났습니다. 변경 기록에서 적용 상태를 확인하세요.");
            if (Result.applied.error == ERROR_CANCELLED && Result.elevated && Result.record.directory.isEmpty())
                Error = tr("관리자 권한 요청(UAC)이 취소되었습니다. 레지스트리를 변경하지 않았습니다.");
            if (!Result.record.directory.isEmpty()) Error += tr("\n백업: %1").arg(Result.record.directory);
            if (Result.applied.uncertain) Error += tr("\n처리 결과를 확인할 수 없습니다. 변경 기록에서 복구 상태를 확인하세요.");
        }
        if (Guard) {
            if (Done) Done(Guard, Error);
            else if (!Error.isEmpty()) hc::ui::MessageDialog::warning(main_, tr("레지스트리 변경 실패"), Error);
            Guard->Refresh();
        }
        if (Result.notificationError && main_) main_->statusBar()->showMessage(tr("환경 변수 변경은 저장됐지만 일부 프로그램에 알리지 못했습니다. 필요한 프로그램을 다시 시작하세요."), 12000);
        if (hc::sys::AdminSessionConnected()) hc::sys::NotifyAdminConnected();
    });
}
void RegistryCommands::rename(CRegistryView* View, const QModelIndex& Index, const QString& Name)
{
    if (busy_) { View->FinishRename(tr("다른 작업이 끝난 뒤 다시 시도하세요.")); return; }
    const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView());
    const bool Value = Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value);
    if (!Location || !WinRegistryBackend::ValidName(Name, !Value) || Name.isEmpty()) { View->FinishRename(tr("이름이 올바르지 않습니다.")); return; }
    const RegistryValue Original{Index.data(RegistryTreeModel::ValueNameRole).toString(), Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()};
    const auto Revision = View->NavigationRevision();
    runMutation(View, [Location = *Location, Value, Original, Name](RegistryMutation& Mutation) {
        return Value ? Mutation.RenameValue(Location, Original, Name) : Mutation.Rename(Location, Name);
    }, [Location = *Location, Value, Name, Revision](CRegistryView* View, const QString& Error) {
        const bool StillCurrent = View->NavigationRevision() == Revision;
        // A late rename completion must not return focus to the old tree after navigation.
        if (!StillCurrent) View->EnsureKeyboardFocusOnView();
        View->FinishRename(Error);
        if (!Error.isEmpty()) return;
        if (const auto Parent = Location.Parent()) View->Service()->Invalidate(*Parent);
        if (!StillCurrent) return;
        if (Value) View->Navigate(Location.Path(), Name);
        else if (const auto Parent = Location.Parent()) {
            View->Service()->Invalidate(*Parent);
            View->Navigate(Parent->Path());
        }
    });
}
void RegistryCommands::undoRedo(CRegistryView* View, bool Redo)
{
    if (View->History()->Loading()) return;
    if (Redo) {
        const auto Candidate = View->History()->RedoCandidate(); if (!Candidate) return;
        runMutation(View, [Candidate = *Candidate](RegistryMutation& Mutation) { return Mutation.Redo(Candidate.first, Candidate.second); });
    } else {
        auto Candidate = View->CurrentHistory();
        if (!Candidate || Candidate->plan.purpose != "edit" || Candidate->inFlight >= 0 || Candidate->completed <= 0
            || (Candidate->state != "committed" && Candidate->state != "partial" && Candidate->state != "conflict")) Candidate = View->History()->UndoCandidate();
        if (!Candidate) return;
        runMutation(View, [Candidate = *Candidate](RegistryMutation& Mutation) { return Mutation.Undo(Candidate); });
    }
}
void RegistryCommands::create(CRegistryView* View, bool Key)
{
    const auto Location = RegistryLocation::Parse(View->PathText(), View->SelectedRegistryView()); if (!Location) return;
    if (Key) {
        const auto Name = hc::ui::AskRegistryName(main_, tr("새 키"), {}, true); if (!Name) return;
        const auto Revision = View->NavigationRevision();
        const auto Target = Location->Child(*Name);
        runMutation(View, [Target](RegistryMutation& Mutation) { return Mutation.CreateKey(Target); },
            [Target, Revision](CRegistryView* View, const QString& Error) {
                if (!Error.isEmpty()) { hc::ui::MessageDialog::warning(View, tr("새 키 만들기 실패"), Error); return; }
                if (const auto Parent = Target.Parent()) View->Service()->Invalidate(*Parent);
                if (View->NavigationRevision() == Revision) View->Navigate(Target.Path());
            });
    } else {
        const auto Value = hc::ui::AskRegistryNewValue(main_); if (!Value) return;
        const auto Revision = View->NavigationRevision();
        runMutation(View, [Location = *Location, Value = *Value](RegistryMutation& Mutation) { return Mutation.SaveValue(Location, {}, Value); },
            [this, Location = *Location, Value = *Value, Revision](CRegistryView* View, const QString& Error) {
                if (!Error.isEmpty()) { hc::ui::MessageDialog::warning(View, tr("새 값 만들기 실패"), Error); return; }
                View->Service()->Invalidate(Location);
                if (View->NavigationRevision() != Revision) return;
                View->Navigate(Location.Path(), Value.name);
                if (auto* Detail = openDetail(View)) Detail->ShowData();
            });
    }
}
void RegistryCommands::remove(CRegistryView* View)
{
    QList<RegistryOperation> Operations; QStringList Names;
    for (const auto& Index : View->SelectedSources()) {
        const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView());
        if (!Location) continue;
        const bool Value = Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value);
        if (!Value && Location->key.isEmpty()) continue;
        RegistryValue Raw{Index.data(RegistryTreeModel::ValueNameRole).toString(), Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()};
        Operations.append({Value ? OperationKind::DeleteValue : OperationKind::DeleteKey, *Location, Raw});
        Names.append(Location->Path() + (Value ? " : " + (Raw.name.isEmpty() ? tr("(기본값)") : Raw.name) : QString()));
    }
    if (Operations.isEmpty() || !hc::ui::ConfirmRegistryDelete(main_, Names)) return;
    runMutation(View, [Operations](RegistryMutation& Mutation) { return Mutation.DeleteSelected(Operations); });
}
void RegistryCommands::favorite(CRegistryView* View)
{
    const auto Target = RegistryLocation::Parse(View->PathText()); if (!Target) return;
    const auto Directory = QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-backup");
    busy_ = true; RefreshState();
    hc::sys::RunAsync(this, [Target = *Target, Directory] {
        WinRegistryBackend Backend; const auto Location = RegistryFavorites::SharedLocation();
        auto Snapshot = Backend.Enumerate(Location);
        if (!Snapshot && Snapshot.error != ERROR_FILE_NOT_FOUND && Snapshot.error != ERROR_PATH_NOT_FOUND) {
            RegistryMutationResult Error; Error.applied.error = Snapshot.error; return Error;
        }
        const auto Operations = RegistryFavorites::Toggle(Location, Snapshot.value, Target);
        RegistryChangeStore Store(Directory);
        RegistryMutation Mutation(Backend, Store, QString::fromStdWString(hc::system::CurrentUserSid()), QString::fromLatin1(QSysInfo::machineUniqueId().toHex()));
        return Mutation.Apply(Operations);
    }, [this, Guard = QPointer<CRegistryView>(View)](const RegistryMutationResult& Result) {
        busy_ = false; RefreshState();
        if (Guard) { Guard->Service()->Invalidate(RegistryFavorites::SharedLocation()); Guard->ReloadHistory(); }
        if (Result.applied.error) {
            QString Text = hc::sys::Win32ErrorText(Result.applied.error);
            if (!Result.record.directory.isEmpty()) Text += tr("\n백업: %1").arg(Result.record.directory);
            if (Result.applied.uncertain) Text += tr("\n처리 결과를 확인할 수 없습니다. 변경 기록에서 복구 상태를 확인하세요.");
            hc::ui::MessageDialog::warning(main_, tr("즐겨찾기 변경 실패"), Text);
        } else if (main_) main_->statusBar()->showMessage(tr("레지스트리 즐겨찾기를 갱신했습니다."), 5000);
    });
}
} // namespace hc::registry
