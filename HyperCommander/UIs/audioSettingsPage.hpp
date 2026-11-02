#pragma once
#include "UniqueLibs/productDefaults.hpp"

/// 고유 라이브러리
#include "UniqueLibs/audio/audioQueue.hpp"

/// 외부 라이브러리
#include <fmwidgets/DialogHeader.h>

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtWidgets>
#include <functional>

namespace hc::ui {
// 제목표시줄 오디오 미리 듣기 설정. 볼륨·반복 등 재생 중에 바뀌는 값은 세션이 저장하므로
// 여기서는 실행 연결·대상 확장자·끝나면 다음 곡·재생 중 표시·꺼내기 상한만 다룬다.
class AudioSettingsPage final : public QWidget {
public:
    explicit AudioSettingsPage(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("hc.settings.audio"); auto* Layout = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("오디오 미리 듣기"));
        Header->setSubtitle(tr("파일 패널의 오디오를 제목표시줄에서 재생합니다. 변경은 적용/확인 후 반영합니다."));
        Layout->addWidget(Header);
        auto* Start = new QGroupBox(tr("재생 시작"), this); auto* StartRows = new QVBoxLayout(Start); StartRows->setContentsMargins(14, 12, 14, 14);
        openInternally_ = new QCheckBox(tr("Enter·두 번 클릭으로 오디오를 제목표시줄에서 재생"), Start); openInternally_->setObjectName("hc.settings.audio.openInternally");
        auto* StartHint = new QLabel(tr("끄면 Enter·두 번 클릭은 연결 프로그램을 엽니다. Ctrl+Enter(오디오 미리 듣기 명령)와 도구 메뉴는 항상 쓸 수 있습니다."), Start);
        StartHint->setWordWrap(true); StartHint->setForegroundRole(QPalette::PlaceholderText);
        autoNext_ = new QCheckBox(tr("곡이 끝나면 같은 폴더의 다음 오디오 파일 재생"), Start); autoNext_->setObjectName("hc.settings.audio.autoNext");
        StartRows->addWidget(openInternally_); StartRows->addWidget(StartHint); StartRows->addWidget(autoNext_);
        Layout->addWidget(Start);
        auto* Display = new QGroupBox(tr("파일 패널 표시"), this); auto* DisplayRows = new QVBoxLayout(Display); DisplayRows->setContentsMargins(14, 12, 14, 14);
        animate_ = new QCheckBox(tr("재생 중인 파일 아이콘의 막대를 움직임"), Display); animate_->setObjectName("hc.settings.audio.animateNowPlaying");
        auto* AnimateHint = new QLabel(tr("끄면 정지된 막대로 표시합니다. 창이 비활성이거나 최소화되어 있으면 켜 두어도 움직이지 않습니다."), Display);
        AnimateHint->setWordWrap(true); AnimateHint->setForegroundRole(QPalette::PlaceholderText);
        DisplayRows->addWidget(animate_); DisplayRows->addWidget(AnimateHint);
        Layout->addWidget(Display);
        auto* Files = new QGroupBox(tr("대상 파일"), this); auto* Form = new QFormLayout(Files); Form->setContentsMargins(14, 12, 14, 14);
        Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        extensions_ = new QLineEdit(Files); extensions_->setObjectName("hc.settings.audio.extensions"); extensions_->setClearButtonEnabled(true);
        extensions_->setToolTip(tr("쉼표로 구분합니다. 비우면 기본 목록을 씁니다."));
        auto* Reset = new QPushButton(tr("기본값"), Files); Reset->setObjectName("hc.settings.audio.resetExtensions");
        auto* ExtensionRow = new QHBoxLayout; ExtensionRow->addWidget(extensions_, 1); ExtensionRow->addWidget(Reset);
        Form->addRow(tr("확장자"), ExtensionRow);
        limit_ = new QSpinBox(Files); limit_->setObjectName("hc.settings.audio.materializeLimit"); limit_->setRange(16, 8192); limit_->setSuffix(tr(" MiB")); limit_->setSingleStep(64);
        Form->addRow(tr("압축 파일·플러그인 위치에서 꺼낼 최대 크기"), limit_);
        auto* LimitHint = new QLabel(tr("압축 파일과 플러그인 위치의 오디오는 임시 파일로 꺼내 재생하고, 다음 곡·정지·종료 때 지웁니다."), Files);
        LimitHint->setWordWrap(true); LimitHint->setForegroundRole(QPalette::PlaceholderText); Form->addRow(LimitHint);
        Layout->addWidget(Files);
        Layout->addStretch(1);
        connect(Reset, &QPushButton::clicked, this, [this] { extensions_->setText(hc::audio::DefaultExtensionText()); });
        for (auto* Check : {openInternally_, autoNext_, animate_}) connect(Check, &QCheckBox::toggled, this, [this] { changed(); });
        connect(extensions_, &QLineEdit::textEdited, this, [this] { changed(); });
        connect(Reset, &QPushButton::clicked, this, [this] { changed(); });
        connect(limit_, &QSpinBox::valueChanged, this, [this] { changed(); });
    }
    void SetChanged(std::function<void()> Callback) { changed_ = std::move(Callback); }
    void Load(const QJsonObject& Audio, bool OpenInternally)
    {
        const QSignalBlocker A(openInternally_), B(autoNext_), C(extensions_), D(limit_), E(animate_);
        openInternally_->setChecked(OpenInternally);
        autoNext_->setChecked(Audio.value("autoNext").toBool(hc::defaults::DefaultValue("general/audioPreview/autoNext").toBool()));
        animate_->setChecked(Audio.value("animateNowPlaying").toBool(hc::defaults::DefaultValue("general/audioPreview/animateNowPlaying").toBool()));
        extensions_->setText(hc::audio::ParseExtensions(Audio.value("extensions").toString(hc::defaults::DefaultValue("general/audioPreview/extensions").toString())).join(", "));
        limit_->setValue(Audio.value("materializeLimitMiB").toInt(hc::defaults::DefaultValue("general/audioPreview/materializeLimitMiB").toInt()));
    }
    // 재생 중에 세션이 저장한 볼륨·반복 등은 그대로 둔다.
    QJsonObject Save(QJsonObject Audio) const
    {
        const auto Extensions = hc::audio::ParseExtensions(extensions_->text());
        Audio["extensions"] = Extensions.isEmpty() ? hc::audio::DefaultExtensionText() : Extensions.join(",");
        Audio["autoNext"] = autoNext_->isChecked();
        Audio["animateNowPlaying"] = animate_->isChecked();
        Audio["materializeLimitMiB"] = limit_->value();
        return Audio;
    }
    bool OpenInternally() const { return openInternally_->isChecked(); }
private:
    void changed() { if (changed_) changed_(); }
    QCheckBox* openInternally_ = nullptr;
    QCheckBox* autoNext_ = nullptr;
    QCheckBox* animate_ = nullptr;
    QLineEdit* extensions_ = nullptr;
    QSpinBox* limit_ = nullptr;
    std::function<void()> changed_;
};
} // namespace hc::ui
