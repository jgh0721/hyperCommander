#include "stdafx.h"
#include "cmpEnvironmentDetailView.hpp"

/// 모듈
/// UIs
#include "cmpEnvironmentView.hpp"
#include "HyperCommander/UIs/environmentEditors.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/registry/registryCatalog.hpp"
#include "UniqueLibs/system/environmentStorage.hpp"
#include "UniqueLibs/system/environmentPathProbe.hpp"
#include "commonLibs/system/environmentSensitivity.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/Label.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/ScrollPanel.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QLocale>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentDetailView, "hc.ui.environmentDetail")
using namespace hc::environment;
CEnvironmentDetailView::CEnvironmentDetailView(QWidget* Parent) : CViewT(Parent)
{
    viewMode_ = VM_DETAIL; setObjectName("hc.environment.detail");
    auto* Layout = new QVBoxLayout(this);
    title_ = new fm::ui::Label(tr("환경 변수"), fm::ui::Label::Heading, this); Layout->addWidget(title_);
    notice_ = new fm::ui::Label({}, fm::ui::Label::Help, this); notice_->setWordWrap(true); Layout->addWidget(notice_); notice_->hide();
    tabs_ = new fm::ui::SegmentedControl({tr("설명"), tr("값"), tr("적용"), tr("출처")}, this); tabs_->setObjectName("hc.environment.detailTabs"); Layout->addWidget(tabs_);
    pages_ = new QStackedWidget(this); Layout->addWidget(pages_, 1);
    const auto LabelPage = [this](fm::ui::Label*& Label) {
        auto* Scroll = new fm::ui::ScrollPanel(this); auto* Content = new QWidget; auto* Body = new QVBoxLayout(Content);
        Label = new fm::ui::Label({}, fm::ui::Label::Body, Content); Label->setWordWrap(true); Label->setTextFormat(Qt::PlainText); Label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        Body->addWidget(Label); Body->addStretch(); Scroll->setContentWidget(Content); pages_->addWidget(Scroll);
    };
    LabelPage(description_);
    editor_ = new EnvironmentValueEditor(this); pages_->addWidget(editor_);
    LabelPage(stages_); LabelPage(origin_);
    auto* Follow = new fm::ui::Button(tr("원본 따라가기"), this); Follow->setObjectName("hc.environment.detailFollow"); Layout->addWidget(Follow);
    connect(Follow, &QPushButton::clicked, this, &CEnvironmentDetailView::FollowRequested);
    connect(tabs_, &fm::ui::SegmentedControl::currentIndexChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(editor_, &EnvironmentValueEditor::SaveRequested, this, &CEnvironmentDetailView::SaveRequested);
    connect(editor_, &EnvironmentValueEditor::CancelRequested, this, &CEnvironmentDetailView::Update);
    connect(editor_, &EnvironmentValueEditor::ExpansionRequested, this, [this] { updateExpansion(); });
    connect(editor_, &EnvironmentValueEditor::ModifiedChanged, this, [this](bool Modified) { if (Modified) updateExpansion(); });
    tabs_->setCurrentIndex(0);
}
void CEnvironmentDetailView::SetSource(CEnvironmentView* Source)
{
    if (source_ == Source) { Update(); return; }
    if (busy_ || editor_->IsModified()) { notice_->setText(tr("현재 초안을 저장하거나 버린 뒤 다른 항목으로 이동하세요.")); notice_->show(); return; }
    if (source_) disconnect(source_, nullptr, this, nullptr);
    source_ = Source;
    if (source_) {
        connect(source_, &CEnvironmentView::sigCursorChanged, this, &CEnvironmentDetailView::Update);
        connect(source_, &QObject::destroyed, this, [this] { source_.clear(); notice_->setText(tr("원본 보기가 닫혔습니다. 초안을 저장할 수 없습니다.")); notice_->show(); editor_->SetBusy(true); });
    }
    Update();
}
CEnvironmentView* CEnvironmentDetailView::Source() const { return source_; }
void CEnvironmentDetailView::Update()
{
    if (!source_ || !source_->Session()) return;
    const auto* Row = source_->Current();
    if (!Row || !Row->value_) {
        if (busy_ || editor_->IsModified()) return;
        description_->clear(); stages_->clear(); origin_->clear(); editor_->setEnabled(false); title_->setText(tr("환경 변수"));
        if (source_->Scope() == EnvironmentViewScope::History) if (const auto Record = source_->CurrentHistory()) {
            title_->setText(tr("변경 기록"));
            description_->setText(tr("상태: %1\n적용된 단계: %2 / %3\n오류 코드: %4").arg(Record->state).arg(Record->completed).arg(Record->plan.steps.size()).arg(Record->error));
            origin_->setText(Record->directory); stages_->setText(tr("작업 ID: %1\n원본 작업: %2").arg(Record->plan.id, Record->plan.parentId));
            if (const auto Step = source_->CurrentHistoryStep()) {
                const auto Raw = Step->after ? Step->after : Step->before;
                if (Raw) {
                    const auto Decoded = DecodeEnvironmentValue(*Raw, Step->location.hive == hc::registry::Hive::LocalMachine ? Scope::System : Scope::User);
                    if (Decoded) { editor_->SetValue(Decoded.value, true, Step->sensitive); editor_->setEnabled(true); }
                }
            }
        }
        return;
    }
    if (busy_ || editor_->IsModified()) {
        if (shownId_ != Row->id_) { notice_->setText(tr("초안을 유지하고 있습니다. 저장 또는 버리기를 먼저 선택하세요.")); notice_->show(); }
        return;
    }
    shownId_ = Row->id_; const auto Value = *Row->value_;
    const bool Writable = source_->Scope() == EnvironmentViewScope::User || source_->Scope() == EnvironmentViewScope::System;
    editor_->setEnabled(true);
    title_->setText(QString::fromStdWString(Value.name_));
    static const auto Catalog = hc::registry::BuiltinRegistryCatalog::Bundled();
    const auto Description = Catalog.Find(EnvironmentLocation(Value.scope_ == Scope::System ? Scope::System : Scope::User),
        QString::fromStdWString(Value.name_), quint32(Value.type_), QLocale().name().replace('_', '-'));
    QStringList Known;
    for (const auto& Option : Description.metadata["options"].toArray()) {
        if (Option.isString()) Known.append(Option.toString());
        else if (Option.isArray() && !Option.toArray().isEmpty()) Known.append(Option.toArray().first().toString());
        else if (Option.isObject()) Known.append(Option.toObject()["value"].toString());
    }
    editor_->SetValue(Value, !Writable, Row->masked_, Known, Description.metadata["environmentEditor"].toObject()["folder"].toBool());
    description_->setText(Description ? Description.title + "\n\n" + Description.text + "\n\n" + Description.warnings.join('\n')
        : tr("이 변수의 설명이 카탈로그에 없습니다. 사용하는 프로그램의 문서를 확인하세요."));
    const auto& Snapshot = source_->Session()->Snapshot();
    QStringList Names; for (const auto& [Name, Effective] : Snapshot.merged_.values_) Names.append(QString::fromStdWString(Name)); editor_->SetVariableNames(Names);
    QStringList Lines;
    const auto Found = Snapshot.merged_.values_.find(Value.name_);
    if (Found != Snapshot.merged_.values_.end()) {
        for (const auto& Origin : Found->second.origins_) {
            const auto Display = Row->masked_ ? QStringLiteral("••••••••") : QString::fromStdWString(Origin.expanded_);
            Lines.append(tr("%1단계 · %2%3\n%4").arg(Origin.stage_).arg(Origin.value_.scope_ == Scope::System ? tr("시스템") : Origin.value_.scope_ == Scope::User ? tr("사용자") : tr("기본·계정")).arg(Origin.appended_ ? tr(" · 이어 붙임") : QString()).arg(Display));
            for (const auto& Ref : Origin.unresolved_) Lines.append(tr("확장되지 않은 참조: %1 (사유 %2)").arg(QString::fromStdWString(Ref.name_)).arg(int(Ref.issue_)));
        }
        Lines.append(tr("최종 적용 결과\n%1").arg(Row->masked_ ? QStringLiteral("••••••••") : QString::fromStdWString(Found->second.value_)));
    }
    stages_->setText(Lines.join("\n\n"));
    const auto Location = EnvironmentLocation(Value.scope_ == Scope::System ? Scope::System : Scope::User);
    origin_->setText(tr("%1\n%2\nUTF-16 길이: %3\n%4").arg(Writable ? Location.Path() : source_->PathText())
        .arg(Value.type_ == ValueType::ExpandString ? "REG_EXPAND_SZ" : "REG_SZ").arg(Value.raw_.size()).arg(Description.sourceUrls.join('\n')));
    notice_->hide(); updateExpansion();
}
void CEnvironmentDetailView::updateExpansion()
{
    if (!source_ || !source_->Session() || source_->Scope() == EnvironmentViewScope::History) return;
    const auto Draft = editor_->Draft(); const auto Snapshot = source_->Session()->Snapshot();
    auto Definitions = Snapshot.input_.system_; Definitions.insert(Definitions.end(), Snapshot.input_.user_.begin(), Snapshot.input_.user_.end()); Definitions.push_back(Draft);
    const auto Sensitive = SensitiveVariables(Definitions);
    const auto Available = Draft.scope_ == Scope::System ? Snapshot.merged_.afterSystemStrings_ : Snapshot.merged_.beforeUserExpansion_;
    const auto Probe = source_->PathProbe();
    static const auto Catalog = hc::registry::BuiltinRegistryCatalog::Bundled();
    const auto Description = Catalog.Find(EnvironmentLocation(Draft.scope_ == Scope::System ? Scope::System : Scope::User),
        QString::fromStdWString(Draft.name_), quint32(Draft.type_), QLocale().name().replace('_', '-'));
    const auto Expected = Description.metadata["environmentEditor"].toObject()["expect"].toString();
    editor_->SetResolver([Available, Sensitive, Probe, Expected, Type = Draft.type_](const QString& Raw) {
        hc::ui::PathResolution Result;
        for (const auto& Ref : References(Raw.toStdWString())) if (SensitiveName(Ref) || Sensitive.contains(Ref)) {
            Result.expanded = QStringLiteral("••••••••"); Result.warning = QObject::tr("비밀 변수 참조"); Result.status = hc::ui::PathResolution::Status::Unresolved; return Result;
        }
        bool Missing = false;
        Result.expanded = QString::fromStdWString(Type == ValueType::ExpandString ? ExpandAtStage(Raw.toStdWString(), Available, [&](const auto&) { Missing = true; }) : Raw.toStdWString());
        if (Missing) { Result.warning = QObject::tr("이 단계에서 확장되지 않은 참조"); Result.status = hc::ui::PathResolution::Status::Unresolved; }
        else if (Probe) {
            BudgetedPathProbe Bounded(*Probe);
            const auto Read = Bounded.Inspect(Result.expanded.toStdWString());
            Result.status = Read.status_ == ProbeStatus::Exists ? hc::ui::PathResolution::Status::Exists : Read.status_ == ProbeStatus::Missing ? hc::ui::PathResolution::Status::Missing : hc::ui::PathResolution::Status::Unresolved;
            if (Read.status_ == ProbeStatus::Exists && !Expected.isEmpty()) {
                const auto Relative = Expected;
                const auto Slash = Relative.lastIndexOf(QChar(92));
                const auto Folder = Slash < 0 ? Result.expanded : Result.expanded + QChar(92) + Relative.left(Slash);
                const auto Name = Slash < 0 ? Relative : Relative.mid(Slash + 1);
                const auto Contents = Slash < 0 ? Read : Bounded.Inspect(Folder.toStdWString());
                const bool Found = std::any_of(Contents.files_.begin(), Contents.files_.end(),
                    [&](const auto& File) { return SameName(File, Name.toStdWString()); });
                Result.warning = Found ? QObject::tr("기대 파일 확인: %1").arg(Expected)
                    : Contents.status_ == ProbeStatus::Exists || Contents.status_ == ProbeStatus::Missing
                        ? QObject::tr("기대 파일 없음: %1 · 선택한 폴더를 확인하세요.").arg(Expected)
                        : QObject::tr("기대 파일 확인 불가: %1").arg(Expected);
            }
            if (Read.status_ == ProbeStatus::Denied || Read.status_ == ProbeStatus::Timeout) Result.warning = QObject::tr("접근 불가 또는 시간 초과 · 존재 여부 미확인");
        }
        return Result;
    });
    if (Sensitive.contains(Draft.name_)) { editor_->ShowError(tr("비밀 값을 포함하므로 확장값을 가립니다.")); return; }
    if (Draft.type_ != ValueType::ExpandString) return;
    QStringList Missing;
    const auto Expanded = ExpandAtStage(Draft.raw_, Available, [&](const auto& Name) { Missing.append(QString::fromStdWString(Name)); });
    editor_->ShowError(Missing.isEmpty() ? tr("이 단계의 확장값: %1").arg(QString::fromStdWString(Expanded))
        : tr("이 단계에서 확장되지 않은 참조: %1\n확장값: %2").arg(Missing.join(", "), QString::fromStdWString(Expanded)));
}
bool CEnvironmentDetailView::RequestClose()
{
    if (busy_) { notice_->setText(tr("저장이 끝난 뒤 닫을 수 있습니다.")); notice_->show(); return false; }
    if (!editor_->IsModified()) return true;
    if (hc::ui::MessageDialog::question(this, tr("환경 변수 편집"), tr("저장하지 않은 변경을 버리고 닫을까요?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
    editor_->Discard(); return true;
}
void CEnvironmentDetailView::CloseView() { if (source_) disconnect(source_, nullptr, this, nullptr); source_.clear(); }
void CEnvironmentDetailView::EnsureKeyboardFocusOnView() const { if (pages_->currentIndex() == 1) editor_->FocusEditor(); else pages_->currentWidget()->setFocus(); }
void CEnvironmentDetailView::ShowData(bool Focus) { tabs_->setCurrentIndex(1); if (Focus) EnsureKeyboardFocusOnView(); }
void CEnvironmentDetailView::ShowDescription() { tabs_->setCurrentIndex(0); }
void CEnvironmentDetailView::SetBusy(bool Busy) { busy_ = Busy; editor_->SetBusy(Busy); }
void CEnvironmentDetailView::SaveFinished(const Value& Value, const QString& Error)
{
    SetBusy(false);
    if (!Error.isEmpty()) { editor_->ShowError(Error); notice_->setText(tr("저장하지 못했습니다. 초안은 유지됩니다.")); notice_->show(); return; }
    editor_->AcceptSaved(Value); notice_->hide(); if (source_) source_->Refresh();
}
