#include "stdafx.h"
#include "registryBatchDialog.hpp"

/// 모듈
/// UIs
#include "Components/treeBranchStyle.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapCheckBox.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryBatchDialog, "hc.ui.registryBatch")
using namespace hc::registry;
namespace hc::ui {
std::optional<QString> AskRegistryReplacement(QWidget* Parent)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.registry.replaceInput"); Dialog.setTitle(QObject::tr("문자열 데이터 바꾸기"));
    Dialog.setPrimaryButtonText(QObject::tr("미리 보기")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    Layout->addWidget(new fm::ui::Label(QObject::tr("찾기 조건에 맞는 문자열 데이터만 바꿉니다. 빈 문자열로도 바꿀 수 있습니다."), fm::ui::Label::Help, Body));
    auto* Field = new fm::ui::TextField(Body); Field->setObjectName("hc.registry.replacement"); Layout->addWidget(Field);
    Dialog.setContentWidget(Body); QTimer::singleShot(0, Field, [Field] { Field->setFocus(); });
    return Dialog.exec() == fm::ui::ContentDialog::Primary ? std::optional<QString>(Field->text()) : std::nullopt;
}
bool ConfirmRegistryReplacement(QWidget* Parent, RegistryBatchPreview& Preview)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.registry.replacePreview"); Dialog.setTitle(QObject::tr("문자열 치환 미리 보기"));
    Dialog.setPrimaryButtonText(QObject::tr("백업하고 적용")); Dialog.setCloseButtonText(QObject::tr("취소")); Dialog.setDefaultButton(fm::ui::ContentDialog::CloseButton);
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body); Body->setMinimumSize(650, 320);
    auto* Summary = new fm::ui::Label(QObject::tr("변경 %1개 · 치환 대상이 아니거나 바뀌지 않은 결과 %2개").arg(Preview.rows.size()).arg(Preview.skipped), fm::ui::Label::Help, Body);
    Layout->addWidget(Summary);
    RegistryResultModel Model({QObject::tr("키 › 값"), QObject::tr("지금"), QObject::tr("바꾼 뒤"), QObject::tr("형식")});
    auto* Grid = new Qtitan::TreeGrid(Body); Grid->setObjectName("hc.registry.replaceGrid"); Grid->setViewType(Qtitan::TreeGrid::TreeView);
    auto* View = Grid->view<Qtitan::GridTreeView>(); View->options().setAutoCreateColumns(false); View->setModel(&Model);
    View->options().setGroupsHeader(false); View->options().setGridLines(Qtitan::LinesNone);
    View->tableOptions().setRowsQuickSelection(false); View->tableOptions().setColumnsQuickCustomization(false); View->setFileManagerMode(true);
    for (int I = 0; I < 4; ++I) { auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model.headerData(I, Qt::Horizontal).toString())); Column->setWidth(I == 0 ? 260 : 160); }
    Grid->setStyle(new TreeBranchStyle(Grid)); Layout->addWidget(Grid, 1);
    auto Rows = Preview.rows; for (auto& Row : Rows) Row.groups.clear(); Model.SetRows(Rows);
    auto* Exclude = new fm::ui::Button(QObject::tr("선택 항목 제외"), Body); Layout->addWidget(Exclude);
    auto* Backup = new fm::ui::WrapCheckBox(QObject::tr("registry-backup에 원본을 백업하고 한 묶음으로 변경합니다."), Body); Backup->setObjectName("hc.registry.replaceBackup"); Layout->addWidget(Backup);
    Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(false);
    const auto Update = [&] { Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(Backup->isChecked() && !Preview.rows.isEmpty()); };
    QObject::connect(Backup, &QCheckBox::toggled, &Dialog, Update);
    QObject::connect(Exclude, &QPushButton::clicked, &Dialog, [&] {
        const auto Focus = View->focusedRow(); if (!Focus.isValid()) return;
        const auto Id = Focus.modelIndex(0).data(RegistryTreeModel::IdRole).toString();
        for (qsizetype I = 0; I < Preview.rows.size(); ++I) if (Preview.rows[I].id == Id) {
            Preview.rows.removeAt(I); Preview.plan.steps.removeAt(I); Preview.operations.removeAt(I); Rows.removeAt(I); break;
        }
        Model.SetRows(Rows); Update();
    });
    Dialog.setContentWidget(Body);
    const bool Accepted = Dialog.exec() == fm::ui::ContentDialog::Primary;
    View->setModel(nullptr);
    return Accepted;
}
}