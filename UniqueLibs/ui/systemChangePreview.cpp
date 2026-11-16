#include "stdafx.h"
#include "systemChangePreview.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/registry/registryHistoryModel.hpp"
/// 공통 라이브러리
#include "commonLibs/system/environmentTypes.hpp"
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/ScrollPanel.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcSystemChangePreview, "hc.ui.systemChangePreview")
namespace hc::ui {
bool ConfirmSystemChanges(QWidget* Parent, const hc::registry::RegistryPlan& Plan, const QString& Title)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.system.changePreview"); Dialog.setTitle(Title);
    Dialog.setPrimaryButtonText(QObject::tr("백업하고 적용")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    auto* Notice = new fm::ui::Label(QObject::tr("변경 전 원본을 백업합니다. 미리 보기 이후 원본이 바뀌면 적용을 중단합니다."), fm::ui::Label::Help, Body);
    Notice->setWordWrap(true); Layout->addWidget(Notice);
    auto* Grid = new Qtitan::TreeGrid(Body); Grid->setObjectName("hc.system.changePreview.grid"); Grid->setViewType(Qtitan::TreeGrid::TreeView);
    Grid->setMinimumSize(660, 260); Layout->addWidget(Grid, 1);
    auto* Model = new hc::registry::RegistryHistoryModel(Body);
    hc::registry::RegistryHistoryRecord Record; Record.plan = Plan; Model->SetRecords({Record});
    auto* View = Grid->view<Qtitan::GridTreeView>(); View->options().setAutoCreateColumns(false); View->setModel(Model);
    View->options().setGroupsHeader(false); View->options().setDragEnabled(false); View->options().setDropEnabled(false);
    for (const int I : {int(hc::registry::RegistryHistoryModel::Action), int(hc::registry::RegistryHistoryModel::Path), int(hc::registry::RegistryHistoryModel::Change)}) {
        auto* Column = static_cast<Qtitan::GridTableColumn*>(View->addColumn(I, Model->headerData(I, Qt::Horizontal, Qt::DisplayRole).toString()));
        Column->setWidth(I == hc::registry::RegistryHistoryModel::Action ? 160 : 280);
    }
    auto* Scroll = new fm::ui::ScrollPanel(Body); Scroll->setMinimumHeight(130); Layout->addWidget(Scroll);
    auto* Content = new QWidget; auto* DetailLayout = new QVBoxLayout(Content);
    auto* Detail = new fm::ui::Label({}, fm::ui::Label::Body, Content); Detail->setWordWrap(true); Detail->setTextFormat(Qt::PlainText);
    Detail->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); DetailLayout->addWidget(Detail); Scroll->setContentWidget(Content);
    const auto Selection = [=] {
        const auto Row = View->focusedRow(); const auto Step = Row.isValid() ? Model->SelectedStep(Row.modelIndex(0)) : std::nullopt;
        if (!Step) { Detail->clear(); return; }
        const auto Text = [&](const auto& Value) {
            if (!Value) return QObject::tr("(없음)");
            if (Step->sensitive || hc::environment::SensitiveName(Step->name.toStdWString())) return QStringLiteral("••••••••");
            const auto Decoded = hc::registry::RegistryValueCodec::DecodeText(*Value);
            return Decoded.lossless ? Decoded.text : QObject::tr("바이너리 %1바이트").arg(Value->bytes.size());
        };
        Detail->setText(QObject::tr("%1\n변경 전\n%2\n\n변경 후\n%3").arg(Step->name, Text(Step->before), Text(Step->after)));
    };
    QObject::connect(View, &Qtitan::GridViewBase::focusRowChanged, &Dialog, Selection);
    View->expand(Model->index(0, 0)); View->setFocusedRowIndex(0); Selection();
    Dialog.setContentWidget(Body);
    return !Plan.steps.isEmpty() && Dialog.exec() == fm::ui::ContentDialog::Primary;
}
}
