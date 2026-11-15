#include "stdafx.h"
#include "registryLinkPane.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/Card.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryLinkPane, "hc.ui.registryLinks")
namespace hc::registry {
RegistryLinkPane::RegistryLinkPane(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.registry.links"); layout_ = new QVBoxLayout(this); layout_->setContentsMargins(0, 0, 0, 0);
    links_ = new RegistryLinks(this); connect(links_, &RegistryLinks::Changed, this, &RegistryLinkPane::render);
    connect(links_, &RegistryLinks::ProgressChanged, this, [this] {
        if (auto* Status = findChild<fm::ui::Label*>("hc.registry.linkIndexStatus"))
            Status->setText(tr("색인 중 — 지금까지 %1건 · %2개 키 · %3").arg(links_->BackReferences().size()).arg(links_->IndexedKeys()).arg(links_->FastIndexComplete() ? tr("규칙 색인 완료, 전체 순회 중") : tr("규칙 색인 중")));
    });
    SetMessage(tr("연결 탭을 열면 연결 정보를 읽습니다."));
}
void RegistryLinkPane::hideEvent(QHideEvent* Event) { active_ = false; links_->Suspend(); QWidget::hideEvent(Event); }
void RegistryLinkPane::showEvent(QShowEvent* Event) { QWidget::showEvent(Event); Activate(); }
void RegistryLinkPane::SetSelection(const RegistryLocation& Location, std::optional<RegistryValue> Value)
{
    if (location_ == Location && value_ == Value) return;
    location_ = Location; value_ = std::move(Value); follow_ = false; displayLimit_ = 200;
    if (active_) { links_->Select(Location, value_); render(); }
}
void RegistryLinkPane::SetMessage(const QString& Text)
{
    links_->Suspend(); location_.reset(); value_.reset(); follow_ = false;
    while (auto* Item = layout_->takeAt(0)) { delete Item->widget(); delete Item; }
    auto* Label = new fm::ui::Label(Text, fm::ui::Label::Help, this); Label->setWordWrap(true); layout_->addWidget(Label); layout_->addStretch();
}
void RegistryLinkPane::Activate()
{
    active_ = true; if (location_) { links_->Select(*location_, value_); render(); }
}
void RegistryLinkPane::Follow()
{
    follow_ = true; Activate();
}
void RegistryLinkPane::render()
{
    if (!location_) return;
    QPointer<QScrollArea> Scroll;
    for (auto* Parent = parentWidget(); Parent; Parent = Parent->parentWidget())
        if ((Scroll = qobject_cast<QScrollArea*>(Parent))) break;
    const int ScrollValue = Scroll ? Scroll->verticalScrollBar()->value() : 0;
    if (follow_ && !links_->Loading()) {
        follow_ = false;
        for (const auto& Link : links_->Result().forward) if (!Link.error && (Link.target || !Link.file.isEmpty())) { if (navigate_) navigate_(Link); return; }
    }
    while (auto* Item = layout_->takeAt(0)) { delete Item->widget(); delete Item; }
    const auto Label = [](QVBoxLayout* Layout, QWidget* Parent, const QString& Text, bool Error = false) {
        const auto Display = Text.size() > 4096 ? Text.left(4096) + tr("… (표시 생략, 원문은 데이터 탭에서 확인)") : Text;
        auto* Item = new fm::ui::Label(Display, fm::ui::Label::Help, Parent); if (Error) Item->setTone(fm::ui::Label::Danger);
        Item->setTextFormat(Qt::PlainText); Item->setWordWrap(true); Item->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); Layout->addWidget(Item);
    };
    if (links_->Loading()) Label(layout_, this, tr("연결 정보를 읽는 중…"));
    const auto Group = [&](const QString& Title, const QList<RegistryLink>& Items, bool Reverse = false) {
        auto* Card = new fm::ui::Card(Title, this); Card->setMeta(QString::number(Items.size())); auto* Rows = new QVBoxLayout(Card); layout_->addWidget(Card);
        if (Items.isEmpty()) Label(Rows, Card, tr("없음"));
        int Shown = 0;
        for (const auto& Item : Items) {
            if (++Shown > displayLimit_) break;
            Label(Rows, Card, Item.label + (Item.fallback ? tr(" · 대체 경로") : QString()));
            Label(Rows, Card, Reverse ? Item.source.Path() + " › " + Item.sourceName : Item.target ? Item.target->Path() : !Item.file.isEmpty() ? Item.file : Item.text, Item.error != 0);
            if (!Item.text.isEmpty() && (Item.target || Reverse)) Label(Rows, Card, Item.text);
            if (Item.error) Label(Rows, Card, Item.error == ERROR_FILE_NOT_FOUND || Item.error == ERROR_PATH_NOT_FOUND ? tr("가리키는 대상 없음") : tr("읽기/해석 실패 (%1)").arg(Item.error), true);
            if (Reverse || (!Item.error && (Item.target || !Item.file.isEmpty()))) {
                auto Link = Item; if (Reverse) { Link.target = Item.source; Link.file.clear(); }
                auto* Open = new fm::ui::Button(Link.target ? tr("키 열기") : tr("파일 패널 새 탭에서 위치 열기"), Card); Open->setCompact(true); Rows->addWidget(Open);
                connect(Open, &QPushButton::clicked, this, [this, Link] { if (navigate_) navigate_(Link); });
            }
        }
        if (Items.size() > displayLimit_) {
            auto* More = new fm::ui::Button(tr("다음 200개 더 보기 (%1개 중 %2개)").arg(Items.size()).arg(displayLimit_), Card); Rows->addWidget(More);
            connect(More, &QPushButton::clicked, this, [this] { displayLimit_ += 200; render(); });
        }
    };
    const auto Result = links_->Result();
    if (!Result.chain.isEmpty()) Group(tr("연결 사슬"), Result.chain);
    Group(value_ ? tr("이 값이 가리키는 곳") : tr("이 키의 값이 가리키는 곳"), Result.forward);
    Group(tr("이 키를 가리키는 곳"), links_->BackReferences(), true);
    if (location_->hive == Hive::ClassesRoot) {
        auto* Card = new fm::ui::Card(tr("병합 보기의 실제 위치"), this); auto* Rows = new QVBoxLayout(Card); layout_->addWidget(Card);
        Label(Rows, Card, tr("편집할 실제 위치를 열어 확인하세요. 병합 키에는 직접 쓰지 않습니다."));
        for (const auto& Physical : Result.physicalClasses) {
            RegistryLink Link; Link.target = Physical;
            auto* Open = new fm::ui::Button(Physical.hive == Hive::LocalMachine ? tr("컴퓨터 등록 열기 (HKLM)") : tr("현재 사용자 등록 열기"), Card); Rows->addWidget(Open);
            Label(Rows, Card, Physical.Path());
            connect(Open, &QPushButton::clicked, this, [this, Link] { if (navigate_) navigate_(Link); });
        }
    }
    auto* IndexStatus = new fm::ui::Label(links_->IndexLoading() ? tr("색인 중 — 지금까지 %1건 · %2개 키 · %3").arg(links_->BackReferences().size()).arg(links_->IndexedKeys()).arg(links_->FastIndexComplete() ? tr("규칙 색인 완료, 전체 순회 중") : tr("규칙 색인 중")) : tr("연결 색인 · %1개 키 · %2").arg(links_->Index().keys).arg(links_->Index().complete ? tr("완료") : tr("일부만 읽음")), fm::ui::Label::Help, this);
    IndexStatus->setObjectName("hc.registry.linkIndexStatus"); IndexStatus->setWordWrap(true); layout_->addWidget(IndexStatus);
    auto* Pause = new fm::ui::Button(links_->IndexLoading() ? tr("색인 일시 중지") : tr("색인 다시 확인 / 계속"), this);
    Pause->setObjectName("hc.registry.indexPause"); layout_->addWidget(Pause);
    connect(Pause, &QPushButton::clicked, this, [this] {
        if (links_->IndexLoading()) { links_->Suspend(); render(); }
        else { const bool Complete = links_->Index().complete; links_->Select(*location_, value_); if (Complete) links_->Refresh(); render(); }
    });
    if (Scroll) QTimer::singleShot(0, this, [Scroll, ScrollValue] { if (Scroll) Scroll->verticalScrollBar()->setValue(ScrollValue); });
    for (const auto& Error : Result.diagnostics + links_->Index().diagnostics) Label(layout_, this, Error, true);
    layout_->addStretch();
}
}
