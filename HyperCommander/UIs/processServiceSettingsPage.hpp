#pragma once

/// 고유 라이브러리
#include "UniqueLibs/productDefaults.hpp"
#include "UniqueLibs/system/systemSupport.hpp"

/// 외부 라이브러리
#include <fmwidgets/DialogHeader.h>

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtWidgets>
#include <functional>

namespace hc::ui {
// 프로세스·서비스 보기 설정(general.processService). 단축키와 기능 키 막대는 키보드 페이지의 보기 범위에서 바꾼다.
class ProcessServiceSettingsPage final : public QWidget {
public:
    explicit ProcessServiceSettingsPage(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("hc.settings.processService"); auto* Layout = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("프로세스 · 서비스"));
        Header->setSubtitle(tr("패널 탭의 프로세스 트리와 서비스 목록. 위치 메뉴(Alt+F1/Alt+F2)나 도구 메뉴에서 엽니다."));
        Layout->addWidget(Header);

        auto* Refresh = new QGroupBox(tr("갱신"), this); auto* RefreshForm = new QFormLayout(Refresh); RefreshForm->setContentsMargins(14, 12, 14, 14);
        interval_ = new QComboBox(Refresh); interval_->setObjectName("hc.settings.processService.refreshMs");
        for (const int Ms : {500, 1000, 2000, 5000}) interval_->addItem(Ms < 1000 ? tr("%1초").arg(Ms / 1000.0) : tr("%1초").arg(Ms / 1000), Ms);
        RefreshForm->addRow(tr("기본 갱신 주기"), interval_);
        pauseHidden_ = new QCheckBox(tr("탭이 보이지 않으면 수집을 멈춤"), Refresh); pauseHidden_->setObjectName("hc.settings.processService.pauseWhenHidden");
        RefreshForm->addRow(pauseHidden_);
        cpu_ = new QComboBox(Refresh); cpu_->setObjectName("hc.settings.processService.cpuMethod");
        cpu_->addItem(tr("사이클 기반(작업 관리자·Process Explorer 방식)"), "auto"); cpu_->addItem(tr("시간 기반(커널+사용자 시간)"), "time");
        cpu_->setToolTip(tr("사이클 기반은 짧게 깨어나는 프로세스도 잡아냅니다. 값이 이상하면 자동으로 시간 기반으로 바꿔 표시합니다."));
        RefreshForm->addRow(tr("CPU 사용률"), cpu_);
        interrupts_ = new QCheckBox(tr("인터럽트·DPC 행 표시"), Refresh); interrupts_->setObjectName("hc.settings.processService.showInterrupts");
        RefreshForm->addRow(interrupts_);
        allUsers_ = new QCheckBox(tr("모든 사용자의 프로세스 표시"), Refresh); allUsers_->setObjectName("hc.settings.processService.allUsers");
        RefreshForm->addRow(allUsers_);
        treeLines_ = new QCheckBox(tr("트리 연결선 표시"), Refresh); treeLines_->setObjectName("hc.settings.processService.treeLines");
        RefreshForm->addRow(treeLines_);
        highlight_ = new QSpinBox(Refresh); highlight_->setObjectName("hc.settings.processService.highlightCycles"); highlight_->setRange(0, 10); highlight_->setSuffix(tr(" 회"));
        highlight_->setToolTip(tr("새 프로세스(초록)와 끝난 프로세스(빨강)를 강조하는 갱신 횟수. 0이면 강조하지 않습니다."));
        RefreshForm->addRow(tr("시작·종료 강조"), highlight_);
        Layout->addWidget(Refresh);

        auto* Actions = new QGroupBox(tr("동작"), this); auto* ActionForm = new QFormLayout(Actions); ActionForm->setContentsMargins(14, 12, 14, 14);
        confirm_ = new QCheckBox(tr("프로세스 하나를 끝낼 때도 확인"), Actions); confirm_->setObjectName("hc.settings.processService.confirmSingleTerminate");
        ActionForm->addRow(confirm_);
        graceful_ = new QCheckBox(tr("창이 있으면 먼저 닫기를 요청"), Actions); graceful_->setObjectName("hc.settings.processService.gracefulClose");
        wait_ = new QSpinBox(Actions); wait_->setObjectName("hc.settings.processService.gracefulWaitSec"); wait_->setRange(1, 60); wait_->setSuffix(tr(" 초"));
        auto* GracefulRow = new QHBoxLayout; GracefulRow->addWidget(graceful_); GracefulRow->addWidget(wait_); GracefulRow->addStretch(1);
        ActionForm->addRow(GracefulRow);
        follow_ = new QSpinBox(Actions); follow_->setObjectName("hc.settings.processService.followDebounceMs"); follow_->setRange(100, 3000); follow_->setSingleStep(100); follow_->setSuffix(tr(" ms"));
        ActionForm->addRow(tr("위치 따라가기 지연"), follow_);
        helper_ = new QSpinBox(Actions); helper_->setObjectName("hc.settings.processService.helperIdleMinutes"); helper_->setRange(1, 120); helper_->setSuffix(tr(" 분"));
        helper_->setToolTip(tr("관리자 도우미는 이 시간 동안 요청이 없으면 스스로 끝납니다. 다음 요청에서 다시 UAC 확인을 받습니다."));
        ActionForm->addRow(tr("관리자 도우미 유휴 종료"), helper_);
        autoHelper_ = new QCheckBox(tr("프로세스·서비스 보기를 열 때 관리자 도우미 자동 시작"), Actions); autoHelper_->setObjectName("hc.settings.processService.autoStartHelper");
        autoHelper_->setToolTip(tr("표준 권한으로 실행 중이면 보기를 열 때 UAC 확인 후 관리자 도우미를 띄워 다른 사용자·관리자 프로세스의 상세 정보와 서비스 구성을 관리자 권한으로 읽습니다. 이미 연결된 도우미가 있으면 그것을 씁니다."));
        ActionForm->addRow(autoHelper_);
        Layout->addWidget(Actions);

        auto* Services = new QGroupBox(tr("서비스"), this); auto* ServiceRows = new QVBoxLayout(Services); ServiceRows->setContentsMargins(14, 12, 14, 14);
        delayed_ = new QCheckBox(tr("‘오류 중지’에서 지연된 자동 시작 서비스 제외"), Services); delayed_->setObjectName("hc.settings.processService.serviceProblemExcludesDelayed");
        auto* DelayedHint = new QLabel(tr("지연된 자동 시작 서비스는 부팅 직후 잠시 중지 상태로 보일 수 있습니다. 트리거 시작 서비스는 항상 제외합니다."), Services);
        DelayedHint->setWordWrap(true); DelayedHint->setForegroundRole(QPalette::PlaceholderText);
        ServiceRows->addWidget(delayed_); ServiceRows->addWidget(DelayedHint);
        Layout->addWidget(Services);

        auto* Colors = new QGroupBox(tr("분류 색"), this); auto* ColorGrid = new QGridLayout(Colors); ColorGrid->setContentsMargins(14, 12, 14, 14);
        using hc::sys::Category;
        int Index = 0;
        for (const auto Value : {Category::Own, Category::Service, Category::System, Category::Packaged, Category::Suspended, Category::New, Category::Exiting}) {
            const auto Key = hc::sys::CategoryKey(Value);
            auto* Button = new QToolButton(Colors); Button->setObjectName("hc.settings.processService.color." + Key);
            Button->setText(hc::sys::CategoryLabel(Value)); Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            Button->setToolTip(tr("눌러서 색을 고릅니다. 기본은 현재 테마에서 파생합니다."));
            connect(Button, &QToolButton::clicked, this, [this, Key, Value] {
                const auto Current = colors_.contains(Key) ? QColor(colors_[Key].toString()) : hc::sys::CategoryColor(Value);
                const auto Chosen = QColorDialog::getColor(Current, this, tr("분류 색"), QColorDialog::ShowAlphaChannel);
                if (!Chosen.isValid()) return;
                colors_[Key] = Chosen.name(QColor::HexArgb); refreshColors(); changed();
            });
            buttons_.append({Button, Value});
            ColorGrid->addWidget(Button, Index / 4, Index % 4); ++Index;
        }
        auto* ResetColors = new QPushButton(tr("테마 기본 색"), Colors); ResetColors->setObjectName("hc.settings.processService.resetColors");
        connect(ResetColors, &QPushButton::clicked, this, [this] { colors_ = {}; refreshColors(); changed(); });
        ColorGrid->addWidget(ResetColors, 2, 0, 1, 2);
        Layout->addWidget(Colors);
        Layout->addStretch(1);

        for (auto* Check : {pauseHidden_, interrupts_, allUsers_, treeLines_, confirm_, graceful_, delayed_, autoHelper_}) connect(Check, &QCheckBox::toggled, this, [this] { changed(); });
        for (auto* Combo : {interval_, cpu_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        for (auto* Spin : {highlight_, wait_, follow_, helper_}) connect(Spin, &QSpinBox::valueChanged, this, [this] { changed(); });
        connect(graceful_, &QCheckBox::toggled, wait_, &QWidget::setEnabled);
    }
    void SetChanged(std::function<void()> Callback) { changed_ = std::move(Callback); }
    void Load(const QJsonObject& Stored)
    {
        const auto Value = hc::sys::Settings::Normalize(Stored);
        const QSignalBlocker A(interval_), B(pauseHidden_), C(cpu_), D(interrupts_), E(allUsers_), E2(treeLines_), F(highlight_), G(confirm_), H(graceful_), I(wait_), J(follow_), K(helper_), K2(autoHelper_), L(delayed_);
        interval_->setCurrentIndex(std::max(0, interval_->findData(Value["refreshMs"].toInt())));
        pauseHidden_->setChecked(Value["pauseWhenHidden"].toBool());
        cpu_->setCurrentIndex(std::max(0, cpu_->findData(Value["cpuMethod"].toString())));
        interrupts_->setChecked(Value["showInterrupts"].toBool());
        allUsers_->setChecked(Value["allUsers"].toBool());
        treeLines_->setChecked(Value["treeLines"].toBool(true));
        highlight_->setValue(Value["highlightCycles"].toInt());
        confirm_->setChecked(Value["confirmSingleTerminate"].toBool());
        graceful_->setChecked(Value["gracefulClose"].toBool()); wait_->setEnabled(graceful_->isChecked());
        wait_->setValue(Value["gracefulWaitSec"].toInt());
        follow_->setValue(Value["followDebounceMs"].toInt());
        helper_->setValue(Value["helperIdleMinutes"].toInt());
        autoHelper_->setChecked(Value["autoStartHelper"].toBool(true));
        delayed_->setChecked(Value["serviceProblemExcludesDelayed"].toBool());
        colors_ = Value["colors"].toObject(); refreshColors();
    }
    QJsonObject Save(QJsonObject Stored) const
    {
        Stored = hc::sys::Settings::Normalize(Stored);
        Stored["refreshMs"] = interval_->currentData().toInt();
        Stored["pauseWhenHidden"] = pauseHidden_->isChecked();
        Stored["cpuMethod"] = cpu_->currentData().toString();
        Stored["showInterrupts"] = interrupts_->isChecked();
        Stored["allUsers"] = allUsers_->isChecked();
        Stored["treeLines"] = treeLines_->isChecked();
        Stored["highlightCycles"] = highlight_->value();
        Stored["confirmSingleTerminate"] = confirm_->isChecked();
        Stored["gracefulClose"] = graceful_->isChecked();
        Stored["gracefulWaitSec"] = wait_->value();
        Stored["followDebounceMs"] = follow_->value();
        Stored["helperIdleMinutes"] = helper_->value();
        Stored["autoStartHelper"] = autoHelper_->isChecked();
        Stored["serviceProblemExcludesDelayed"] = delayed_->isChecked();
        Stored["colors"] = colors_;
        return Stored;
    }
private:
    void changed() { if (changed_) changed_(); }
    void refreshColors()
    {
        for (const auto& [Button, Value] : buttons_) {
            const auto Key = hc::sys::CategoryKey(Value);
            QPixmap Swatch(14, 14); Swatch.fill(hc::sys::CategoryColor(Value, colors_));
            Button->setIcon(QIcon(Swatch));
            Button->setProperty("hc.customColor", colors_.contains(Key));
        }
    }
    QComboBox* interval_ = nullptr;
    QComboBox* cpu_ = nullptr;
    QCheckBox* pauseHidden_ = nullptr;
    QCheckBox* interrupts_ = nullptr;
    QCheckBox* allUsers_ = nullptr;
    QCheckBox* treeLines_ = nullptr;
    QCheckBox* confirm_ = nullptr;
    QCheckBox* graceful_ = nullptr;
    QCheckBox* delayed_ = nullptr;
    QCheckBox* autoHelper_ = nullptr;
    QSpinBox* highlight_ = nullptr;
    QSpinBox* wait_ = nullptr;
    QSpinBox* follow_ = nullptr;
    QSpinBox* helper_ = nullptr;
    QJsonObject colors_;
    QVector<QPair<QToolButton*, hc::sys::Category>> buttons_;
    std::function<void()> changed_;
};
} // namespace hc::ui
