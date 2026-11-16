#include "stdafx.h"
#include "environmentProfileEditor.hpp"

/// 모듈
/// UIs
#include "environmentEditors.hpp"
#include "environmentDialogs.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentModel.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/Label.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QSplitter>
#include <QUuid>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentProfileEditor, "hc.ui.environmentProfileEditor")
namespace hc::environment {
std::optional<EnvironmentProfile> EditEnvironmentProfile(QWidget* Parent, EnvironmentProfile Profile, const QString& InitialName)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.environment.profileEditor"); Dialog.setTitle(QObject::tr("환경 변수 프로필"));
    Dialog.setPrimaryButtonText(QObject::tr("프로필 저장")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    auto* Name = new fm::ui::TextField(Body); Name->setObjectName("hc.environment.profileName"); Name->setMaxLength(128); Name->setText(Profile.name_); Name->setPlaceholderText(QObject::tr("프로필 이름")); Layout->addWidget(Name);
    auto* Toolbar = new QHBoxLayout; Layout->addLayout(Toolbar);
    auto* Add = new fm::ui::Button(QObject::tr("변수 추가"), Body); auto* Remove = new fm::ui::Button(QObject::tr("변수 삭제"), Body); Toolbar->addWidget(Add); Toolbar->addWidget(Remove); Toolbar->addStretch();
    auto* Notice = new fm::ui::Label(QObject::tr("사용자 변수에 적용할 정의입니다. 값 편집기의 저장은 이 프로필 초안에만 반영합니다."), fm::ui::Label::Help, Body); Layout->addWidget(Notice);
    auto* Splitter = new QSplitter(Body); Layout->addWidget(Splitter);
    auto* Grid = new Qtitan::TreeGrid(Splitter); Grid->setObjectName("hc.environment.profileVariables"); Grid->setViewType(Qtitan::TreeGrid::TreeView); Grid->setMinimumSize(380, 300);
    auto* View = Grid->view<Qtitan::GridTreeView>(); auto* Model = new EnvironmentModel(Body);
    View->options().setAutoCreateColumns(false); View->setModel(Model); View->options().setGroupsHeader(false); View->options().setDragEnabled(false); View->options().setDropEnabled(false); View->setFileManagerMode(true);
    for (int I = 0; I < 2; ++I) { auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal, Qt::DisplayRole).toString())); Column->setWidth(I ? 230 : 160); }
    auto* Editor = new EnvironmentValueEditor(Splitter); Editor->setMinimumWidth(350); Editor->setEnabled(false); Dialog.setContentWidget(Body);
    std::wstring Selected = InitialName.toStdWString(); bool Rebuilding = false;
    const auto Validate = [&] {
        Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(!Name->text().trimmed().isEmpty() && !Editor->IsModified());
        Add->setEnabled(!Editor->IsModified()); Remove->setEnabled(!Selected.empty() && !Editor->IsModified());
    };
    const auto Rebuild = [&] {
        Rebuilding = true; EnvironmentSnapshot Snapshot; Snapshot.input_.user_ = Profile.values_; Snapshot.merged_ = BuildEnvironment(Snapshot.input_);
        Model->SetSnapshot(Snapshot, EnvironmentViewScope::User);
        const auto Index = Model->Find("0/" + QString::fromStdWString(Selected).toCaseFolded());
        if (Index.isValid()) { const auto Row = View->getRow(Index); if (Row.isValid()) View->setFocusedRowIndex(Row.rowIndex()); }
        else if (Model->rowCount()) View->setFocusedRowIndex(0);
        Rebuilding = false;
        const auto Focus = View->focusedRow(); const auto* Row = Focus.isValid() ? Model->Item(Focus.modelIndex(0)) : nullptr;
        if (Row && Row->value_ && Editor->SetValue(*Row->value_, false, Row->masked_)) { Selected = Row->value_->name_; Editor->setEnabled(true); }
        Validate();
    };
    QObject::connect(View, &Qtitan::GridViewBase::focusRowChanged, &Dialog, [&] {
        if (Rebuilding) return;
        const auto Focus = View->focusedRow(); const auto* Row = Focus.isValid() ? Model->Item(Focus.modelIndex(0)) : nullptr;
        if (!Row || !Row->value_) { Editor->setEnabled(false); Selected.clear(); Validate(); return; }
        if (!Editor->SetValue(*Row->value_, false, Row->masked_)) { Notice->setText(QObject::tr("현재 값의 초안을 저장하거나 버린 뒤 다른 변수를 선택하세요.")); return; }
        Selected = Row->value_->name_; Editor->setEnabled(true); Validate();
    });
    QObject::connect(Editor, &EnvironmentValueEditor::SaveRequested, &Dialog, [&](const Value& Original, const Value& Draft) {
        for (auto& Item : Profile.values_) if (SameName(Item.name_, Original.name_)) { Item = Draft; break; }
        Editor->AcceptSaved(Draft); Rebuild();
    });
    QObject::connect(Editor, &EnvironmentValueEditor::ModifiedChanged, &Dialog, [&](bool) { Validate(); });
    QObject::connect(Name, &QLineEdit::textChanged, &Dialog, Validate);
    QObject::connect(Add, &QPushButton::clicked, &Dialog, [&] {
        const auto Value = AskEnvironmentValue(&Dialog, Scope::User); if (!Value) return;
        for (const auto& Item : Profile.values_) if (SameName(Item.name_, Value->name_)) { Notice->setText(QObject::tr("같은 이름의 변수가 이미 있습니다.")); return; }
        Profile.values_.push_back(*Value); Selected = Value->name_; Editor->Discard(); Editor->SetValue(*Value); Editor->setEnabled(true); Rebuild();
    });
    QObject::connect(Remove, &QPushButton::clicked, &Dialog, [&] {
        std::erase_if(Profile.values_, [&](const Value& Item) { return SameName(Item.name_, Selected); });
        Selected.clear(); Editor->Discard(); Editor->setEnabled(false); Rebuild();
    });
    Rebuild(); Validate();
    if (Dialog.exec() != fm::ui::ContentDialog::Primary || Editor->IsModified()) return {};
    Profile.name_ = Name->text().trimmed(); if (Profile.id_.isEmpty()) Profile.id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return Profile;
}
}
