#include "stdafx.h"
#include "environmentCommandFinder.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentPathProbe.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/Label.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QStandardItemModel>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentCommandFinder, "hc.ui.environmentCommandFinder")
namespace hc::environment {
QString FindEnvironmentCommand(QWidget* Parent, const std::vector<NamedSearchContext>& Contexts, std::shared_ptr<IPathProbe> Probe)
{
    if (Contexts.empty()) return {};
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.environment.commandFinder"); Dialog.setTitle(QObject::tr("명령 찾기"));
    Dialog.setPrimaryButtonText(QObject::tr("파일 위치 따라가기")); Dialog.setCloseButtonText(QObject::tr("닫기"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    auto* Command = new fm::ui::TextField(Body); Command->setObjectName("hc.environment.commandName"); Command->setPlaceholderText(QObject::tr("명령 이름 · 실행하지 않고 후보만 찾습니다"));
    auto* Context = new fm::ui::ChoiceBox(Body); for (const auto& Item : Contexts) Context->addItem(Item.name_);
    auto* Extensions = new fm::ui::WrapCheckBox(QObject::tr("PATHEXT 순서 적용"), Body); Extensions->setChecked(true);
    auto* Current = new fm::ui::WrapCheckBox(QObject::tr("선택한 환경의 현재 폴더부터 찾기"), Body);
    auto* Search = new fm::ui::Button(QObject::tr("찾기"), Body); Search->setObjectName("hc.environment.searchCommand");
    auto* Notice = new fm::ui::Label(QObject::tr("셸의 함수·별칭과 App Paths는 포함하지 않습니다."), fm::ui::Label::Help, Body);
    auto* Grid = new Qtitan::TreeGrid(Body); Grid->setObjectName("hc.environment.commandResults"); Grid->setMinimumSize(620, 260); Grid->setViewType(Qtitan::TreeGrid::TreeView);
    auto* View = Grid->view<Qtitan::GridTreeView>(); auto* Model = new QStandardItemModel(Body);
    Model->setHorizontalHeaderLabels({QObject::tr("순서"), QObject::tr("상태"), QObject::tr("파일"), QObject::tr("Path 항목")});
    View->options().setAutoCreateColumns(false); View->setModel(Model); View->options().setGroupsHeader(false);
    View->options().setDragEnabled(false); View->options().setDropEnabled(false); View->setFileManagerMode(true);
    for (int I = 0; I < 4; ++I) { auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal).toString())); Column->setWidth(I == 2 ? 350 : 100); }
    Layout->addWidget(Command); Layout->addWidget(Context); Layout->addWidget(Extensions); Layout->addWidget(Current); Layout->addWidget(Search); Layout->addWidget(Notice); Layout->addWidget(Grid);
    Dialog.setContentWidget(Body); Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(false);
    struct UnavailableProbe final : IPathProbe { DirectoryProbe Inspect(const std::wstring&) override { return {ProbeStatus::Denied}; } };
    if (!Probe) Probe = std::make_shared<UnavailableProbe>();
    auto Cancel = std::make_shared<std::atomic_bool>(false);
    const auto CancelOnClose = qScopeGuard([&] { Cancel->store(true); });
    quint64 Generation = 0; QString Selected;
    QObject::connect(View, &Qtitan::GridViewBase::focusRowChanged, &Dialog, [&] {
        const auto Row = View->focusedRow(); Selected = Row.isValid() ? Row.modelIndex(2).data(Qt::UserRole).toString() : QString();
        Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(!Selected.isEmpty());
    });
    const auto SearchNow = [&] {
        if (Command->text().trimmed().isEmpty()) return;
        Cancel->store(true); Cancel = std::make_shared<std::atomic_bool>(false); const auto Token = Cancel;
        const auto ThisGeneration = ++Generation; Selected.clear(); Model->removeRows(0, Model->rowCount());
        Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(false); Notice->setText(QObject::tr("찾는 중…"));
        const auto Query = Command->text().toStdWString(); const auto Snapshot = Contexts.at(Context->currentIndex()).context_;
        const bool UseExtensions = Extensions->isChecked(), CurrentFirst = Current->isChecked();
        auto* Watcher = new QFutureWatcher<CommandSearchResult>(&Dialog);
        QObject::connect(Watcher, &QFutureWatcher<CommandSearchResult>::finished, &Dialog, [&, Watcher, ThisGeneration, Query] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != ThisGeneration) return;
            auto* CommandRow = new QStandardItem(QString::fromStdWString(Query));
            QList<QStandardItem*> Summary{CommandRow, new QStandardItem(QObject::tr("후보 %1개").arg(Result.candidates_.size())), new QStandardItem, new QStandardItem};
            for (auto* Item : Summary) Item->setEditable(false); Model->appendRow(Summary);
            int Rank = 0;
            for (const auto& Item : Result.candidates_) {
                auto* File = new QStandardItem(QString::fromStdWString(Item.path_)); File->setData(File->text(), Qt::UserRole);
                QList<QStandardItem*> Row{new QStandardItem(QString::number(++Rank)), new QStandardItem(Item.first_ ? QObject::tr("먼저 찾음") : Item.duplicate_ ? QObject::tr("중복") : QObject::tr("뒤의 후보")), File, new QStandardItem(QString::number(Item.directoryIndex_ + 1))};
                if (Item.alias_) Row[1]->setText(Row[1]->text() + QObject::tr(" · 앱 실행 별칭"));
                for (auto* Cell : Row) Cell->setEditable(false); CommandRow->appendRow(Row);
            }
            for (const auto& [Directory, Status] : Result.unavailable_) {
                QList<QStandardItem*> Row{new QStandardItem(QStringLiteral("—")), new QStandardItem(Status == ProbeStatus::Timeout ? QObject::tr("시간 초과") : Status == ProbeStatus::Cancelled ? QObject::tr("취소됨") : QObject::tr("접근 불가")),
                    new QStandardItem(QString::fromStdWString(Directory)), new QStandardItem(QObject::tr("존재 여부 미확인"))};
                for (auto* Cell : Row) Cell->setEditable(false); CommandRow->appendRow(Row);
            }
            View->expand(CommandRow->index()); if (CommandRow->rowCount()) View->setFocusedRowIndex(1);
            Notice->setText(!Result.error_.empty() ? QObject::tr("현재 폴더를 읽지 못했거나 명령 이름이 올바르지 않습니다.")
                : QObject::tr("후보 %1개 · 확인하지 못한 폴더 %2개%3").arg(Result.candidates_.size()).arg(Result.unavailable_.size()).arg(Result.pathExtMissing_ ? QObject::tr(" · PATHEXT 없음") : QString()));
        });
        Watcher->setFuture(QtConcurrent::run([=] { BudgetedPathProbe Bounded(*Probe, Token.get()); return FindCommand(Query, Snapshot, UseExtensions, CurrentFirst, Bounded, Token.get()); }));
    };
    QObject::connect(Search, &QPushButton::clicked, &Dialog, SearchNow);
    QObject::connect(Command, &QLineEdit::returnPressed, &Dialog, SearchNow);
    QObject::connect(Context, &QComboBox::currentIndexChanged, &Dialog, [&](int Index) { Current->setEnabled(Contexts.at(Index).context_.currentDirectory_.has_value()); if (!Current->isEnabled()) Current->setChecked(false); });
    Current->setEnabled(Contexts.front().context_.currentDirectory_.has_value());
    return Dialog.exec() == fm::ui::ContentDialog::Primary ? Selected : QString();
}

std::optional<hc::system::ProcessKey> FindStaleEnvironmentProcess(QWidget* Parent, ValueMap Expected, bool Native,
    std::function<ProcessEnvironment(const hc::system::ProcessKey&)> Read)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.environment.oldProcesses");
    Dialog.setTitle(QObject::tr("실행 중인 개발 도구의 환경")); Dialog.setPrimaryButtonText(QObject::tr("선택한 프로세스 비교"));
    Dialog.setCloseButtonText(QObject::tr("닫기"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    auto* Notice = new fm::ui::Label(QObject::tr("셸·편집기·빌드 도구만 읽습니다. 접근할 수 없는 프로세스는 자동으로 권한을 요청하지 않습니다."), fm::ui::Label::Help, Body);
    Notice->setWordWrap(true); Layout->addWidget(Notice);
    auto* Grid = new Qtitan::TreeGrid(Body); Grid->setMinimumSize(620, 300); Grid->setViewType(Qtitan::TreeGrid::TreeView);
    auto* View = Grid->view<Qtitan::GridTreeView>(); auto* Model = new QStandardItemModel(Body);
    Model->setHorizontalHeaderLabels({QObject::tr("프로세스"), QObject::tr("PID"), QObject::tr("환경 비교")});
    View->options().setAutoCreateColumns(false); View->setModel(Model); View->options().setGroupsHeader(false);
    View->options().setDragEnabled(false); View->options().setDropEnabled(false); View->setFileManagerMode(true);
    for (int I = 0; I < 3; ++I) { auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal).toString())); Column->setWidth(I == 1 ? 90 : 240); }
    Layout->addWidget(Grid); Dialog.setContentWidget(Body); Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(false);
    auto Cancel = std::make_shared<std::atomic_bool>(false); const auto Cleanup = qScopeGuard([&] { Cancel->store(true); });
    std::optional<hc::system::ProcessKey> Selected;
    QObject::connect(View, &Qtitan::GridViewBase::focusRowChanged, &Dialog, [&] {
        Selected.reset(); const auto Row = View->focusedRow();
        if (Row.isValid()) {
            const auto Index = Row.modelIndex(0);
            Selected = hc::system::ProcessKey{Index.data(Qt::UserRole).toUInt(), Index.data(Qt::UserRole + 1).toString().toULongLong()};
        }
        Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(Selected.has_value());
    });
    struct Entry { QString name_; hc::system::ProcessKey key_; int changes_ = 0; ProcessReadStatus status_ = ProcessReadStatus::ReadFailed; };
    struct Scan { QList<Entry> entries_; bool limited_ = false; quint32 error_ = 0; };
    auto* Watcher = new QFutureWatcher<Scan>(&Dialog);
    QObject::connect(Watcher, &QFutureWatcher<Scan>::finished, &Dialog, [&, Watcher] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        for (const auto& Entry : Result.entries_) {
            auto* Name = new QStandardItem(Entry.name_); Name->setData(Entry.key_.pid, Qt::UserRole);
            Name->setData(QString::number(Entry.key_.createTime), Qt::UserRole + 1);
            QList<QStandardItem*> Row{Name, new QStandardItem(QString::number(Entry.key_.pid)),
                new QStandardItem(Entry.status_ != ProcessReadStatus::Ok ? QObject::tr("읽기 불가 · 선택 후 직접 다시 읽기")
                    : Entry.changes_ ? QObject::tr("%1개 값이 다름").arg(Entry.changes_) : QObject::tr("적용 결과와 같음"))};
            for (auto* Item : Row) Item->setEditable(false); Model->appendRow(Row);
        }
        if (Model->rowCount()) View->setFocusedRowIndex(0);
        Notice->setText(Result.error_ ? QObject::tr("프로세스 목록을 읽지 못했습니다 (오류 %1).").arg(Result.error_)
            : QObject::tr("%1개 개발 도구 확인%2 · 프로그램 자체가 추가한 변수도 차이로 표시될 수 있습니다.")
                .arg(Result.entries_.size()).arg(Result.limited_ ? QObject::tr(" · 시간/개수 제한으로 일부만 확인") : QString()));
    });
    Watcher->setFuture(QtConcurrent::run([Expected = std::move(Expected), Read = std::move(Read), Native, Cancel] {
        Scan Result; if (!Native) return Result;
        hc::system::ProcessCollector Collector; hc::system::Snapshot Snapshot;
        if (!Collector.Sample(Snapshot, &Result.error_)) return Result;
        const std::set<std::wstring, NameLess> Names{L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"bash.exe", L"WindowsTerminal.exe",
            L"Code.exe", L"devenv.exe", L"notepad++.exe", L"HyperEditor.exe", L"cmake.exe", L"ninja.exe", L"MSBuild.exe", L"make.exe", L"java.exe", L"node.exe", L"python.exe"};
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (const auto& Process : Snapshot.processes) {
            if (Cancel->load()) break;
            if (!Names.contains(Process.sample.imageName)) continue;
            if (Result.entries_.size() >= 128 || std::chrono::steady_clock::now() >= Deadline) { Result.limited_ = true; break; }
            const auto Environment = Read(Process.sample.key);
            Entry Item{QString::fromStdWString(Process.sample.imageName), Process.sample.key, 0, Environment.status_};
            if (Environment.status_ == ProcessReadStatus::Ok) {
                for (const auto& [Name, Text] : Expected) {
                    const auto Found = Environment.values_.find(Name);
                    if (Found == Environment.values_.end() || Found->second != Text) ++Item.changes_;
                }
                for (const auto& [Name, Text] : Environment.values_) if (!Expected.contains(Name)) ++Item.changes_;
            }
            Result.entries_.append(std::move(Item));
        }
        return Result;
    }));
    return Dialog.exec() == fm::ui::ContentDialog::Primary ? Selected : std::nullopt;
}
}
