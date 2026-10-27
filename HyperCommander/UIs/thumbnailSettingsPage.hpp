#pragma once
#include "Components/thumbnailSettings.hpp"
#include <QtWidgets>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <fmfilelist/ThumbnailView.h>
#include <fmfilelist/FileRoles.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/StyleProps.h>

namespace hc::ui {
// Product-owned draft controls. This is not an fm::settings example dialog.
class ThumbnailSettingsPage final : public QWidget {
public:
    explicit ThumbnailSettingsPage(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("hc.settings.thumbnails"); auto* Layout = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("썸네일 보기"));
        Header->setSubtitle(tr("크기와 캡션 · 만드는 방법 · 캐시를 설정합니다. 변경은 적용/확인 후 반영합니다."));
        // Board heading: title at the left, cache and reset actions at the right.
        auto* Heading = new QHBoxLayout; Heading->addWidget(Header, 1); Layout->addLayout(Heading);
        auto* Scroll = new QScrollArea(this); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame); Layout->addWidget(Scroll, 1);
        Scroll->setObjectName("hc.thumbs.pageScroll");
        auto* Body = new QWidget(Scroll); Scroll->setWidget(Body); rows_ = new QGridLayout(Body);
        rows_->setContentsMargins(0, 0, 0, 0); rows_->setSpacing(12);
        // Left column: three tabs of titled cards; the preview card stays at the right for every tab.
        auto* Tabs = new QTabWidget(Body); settingsColumn_ = Tabs; Tabs->setObjectName("hc.thumbs.tabs"); Tabs->setDocumentMode(true);
        const auto TabPage = [Tabs](const QString& Caption) { auto* Page = new QWidget(Tabs); auto* Rows = new QVBoxLayout(Page); Rows->setContentsMargins(0, 10, 0, 0); Rows->setSpacing(12); Tabs->addTab(Page, Caption); return Rows; };
        auto* SettingsRows = TabPage(tr("보기"));
        auto* MakeRows = TabPage(tr("만들기 · 캐시"));
        auto* AutoRows = TabPage(tr("자동 전환"));
        auto* ViewCard = new QGroupBox(tr("크기 · 캡션"), settingsColumn_); SettingsRows->addWidget(ViewCard); auto* ViewRows = new QVBoxLayout(ViewCard);
        ViewRows->setContentsMargins(14, 12, 14, 14);
        auto* ChoiceGrid = new QGridLayout; ChoiceGrid->setVerticalSpacing(8); ViewRows->addLayout(ChoiceGrid);
        auto* Form = new QFormLayout; Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Form->setVerticalSpacing(8); ViewRows->addLayout(Form);
        // Caption at the left edge, switch at the right edge.
        const auto SwitchRow = [this](QFormLayout* Target, QCheckBox* Check, const QString& Hint) {
            auto* Row = new QWidget(this); auto* Line = new QHBoxLayout(Row); Line->setContentsMargins(0, 0, 0, 0);
            auto* Caption = new QLabel(Check->text(), Row); Caption->setBuddy(Check); Line->addWidget(Caption);
            if (!Hint.isEmpty()) { auto* Note = new QLabel(Hint, Row); Note->setStyleSheet(QStringLiteral("color: palette(placeholder-text);")); Line->addWidget(Note); }
            Line->addStretch(); Check->setAccessibleName(QString(Check->text()).remove('&')); Check->setText({}); fm::style::setSwitch(Check); Line->addWidget(Check);
            Target->addRow(Row);
        };
        QList<QPushButton*> ChoiceButtons;
        const auto Combo = [this, &ChoiceButtons, ChoiceGrid](QFormLayout* Form, const QString& Name, const QString& Label, const QStringList& Items) {
            auto* Result = new QComboBox(this); Result->setObjectName(Name); Result->addItems(Items);
            if (Name == "hc.settings.thumbMethod") { Form->addRow(Label, Result); return Result; }
            Result->hide(); auto* Choices = new QWidget(this); auto* Buttons = new QHBoxLayout(Choices); Buttons->setContentsMargins(0, 0, 0, 0); Buttons->setSpacing(2);
            auto* Group = new QButtonGroup(Choices);
            for (int I = 0; I < Items.size(); ++I) { const auto Text = Name == "hc.settings.thumbInfo" ? Items[I] : Items[I].section(QStringLiteral(" · "), 0, 0);
                auto* Button = new QPushButton(Text, Choices); Button->setObjectName(Name + ".choice" + QString::number(I)); Button->setCheckable(true); Button->setChecked(I == 0); Button->setToolTip(Items[I]); Button->setAccessibleName(Label + " · " + Items[I]); Group->addButton(Button, I); Buttons->addWidget(Button); ChoiceButtons.append(Button); }
            QList<QAbstractButton*> Segment; for (auto* Button : Group->buttons()) Segment.append(Button); fm::style::setSegments(Segment); Buttons->addStretch();
            connect(Group, &QButtonGroup::idClicked, Result, &QComboBox::setCurrentIndex);
            connect(Result, &QComboBox::currentIndexChanged, Choices, [Group](int I) { if (auto* Button = Group->button(I)) Button->setChecked(true); });
            const int Row = ChoiceGrid->rowCount(); ChoiceGrid->addWidget(new QLabel(Label, this), Row, 0); ChoiceGrid->addWidget(Choices, Row, 1); return Result;
        };
        size_ = Combo(Form, "hc.settings.thumbSize", tr("썸네일 크기"), {tr("작게 · 64 px"), tr("보통 · 96 px"), tr("크게 · 160 px"), tr("아주 크게 · 256 px")});
        lines_ = Combo(Form, "hc.settings.thumbLines", tr("이름 줄 수"), {tr("1줄"), tr("2줄"), tr("전체 · 가장 긴 이름에 높이 맞춤")});
        info_ = Combo(Form, "hc.settings.thumbInfo", tr("이름 아래 정보"), {tr("없음"), tr("크기"), tr("날짜"), tr("크기 · 날짜")});
        fit_ = Combo(Form, "hc.settings.thumbFit", tr("이미지 맞춤"), {tr("맞춤 · 전체 이미지"), tr("채움 · 중앙 잘라내기")});
        // Share one content-derived cell size across all four segmented rows.
        QSize ChoiceSize; for (auto* Button : ChoiceButtons) ChoiceSize = ChoiceSize.expandedTo(Button->sizeHint());
        for (auto* Button : ChoiceButtons) Button->setFixedSize(ChoiceSize);
        badges_ = new QCheckBox(tr("파일 종류 배지 표시"), this); badges_->setObjectName("hc.settings.thumbBadges"); SwitchRow(Form, badges_, tr("보통 크기 이상에서"));
        ViewCard->setToolTip(tr("크기: 64 · 96 · 160 · 256 px. 이름 전체는 가장 긴 이름에 맞추고, 채움은 이미지 중앙을 잘라 표시합니다."));
        previewCard_ = new QGroupBox(tr("미리보기"), Body); auto* PreviewRows = new QVBoxLayout(previewCard_); PreviewRows->setContentsMargins(12, 10, 12, 12);
        auto* States = new QHBoxLayout; PreviewRows->addLayout(States);
        inverseCursor_ = new QCheckBox(tr("커서 반전"), previewCard_); inverseCursor_->setObjectName("hc.thumbs.previewInverseCursor"); States->addWidget(inverseCursor_);
        inverseSelection_ = new QCheckBox(tr("선택 반전"), previewCard_); inverseSelection_->setObjectName("hc.thumbs.previewInverseSelection"); States->addWidget(inverseSelection_); States->addStretch();
        auto* PreviewHelp = new QLabel(tr("반전은 미리보기 전용입니다. 실제 표시는 파일 패널에서 설정합니다."), previewCard_); PreviewHelp->setWordWrap(true); PreviewRows->addWidget(PreviewHelp);
        preview_ = new fm::filelist::ThumbnailView(this); preview_->setObjectName("hc.settings.thumbPreview"); preview_->setInfoBarVisible(true); preview_->setSortText(tr("이름 · 예제")); preview_->setBackend(fm::filelist::ThumbnailView::QtList); preview_->setPreviewMode(false); preview_->setPaneActive(true);
        preview_->setAccessibleName(tr("썸네일 미리보기 · 예제 파일 9개"));
        using namespace fm::filelist;
        const QStringList Names{tr("Screenshots"), tr("Screenshot 2026-09-27 231455 - 듀얼 패널 레이아웃과 아주 긴 파일 이름.png"), tr("Recording_2026-09-26_아키텍처 리뷰 세션.mp4"), tr("2026년 3분기 보안 감사 보고서.pdf"), "IMG_2041.heic", "IMG_2042.heic", "QtitanDataGrid-9.3.0-Windows.7z", "font-pack_Cascadia-Code-NF.7z", "desktop.ini"};
        const std::array<Art, 9> Arts{Art::None, Art::Shot, Art::Video, Art::Pdf, Art::Photo, Art::Loading, Art::None, Art::None, Art::None};
        const std::array<Kind, 9> Kinds{Kind::Folder, Kind::Img, Kind::Doc, Kind::Pdf, Kind::Img, Kind::Img, Kind::Zip, Kind::Zip, Kind::Sys};
        const QStringList Badges{{}, "PNG", "1:42:08", "PDF", "HEIC", {}, {}, {}, {}};
        const std::array<qint64, 9> Sizes{-1, 2202009, 3070900000LL, 13002342, 3565158, 3250585, 195035136, 92274688, 282};
        auto* Model = new QStandardItemModel(Names.size(), ColumnCount, preview_);
        for (int Row = 0; Row < Names.size(); ++Row) for (int Column = 0; Column < ColumnCount; ++Column) {
            auto* Item = new QStandardItem; Model->setItem(Row, Column, Item);
            Item->setData(Names[Row], FullNameRole); Item->setData(Names[Row], Qt::ToolTipRole); Item->setData(Names[Row], Qt::DisplayRole);
            Item->setData(int(Arts[Row]), ArtRole); Item->setData(int(Kinds[Row]), KindRole); Item->setData(Row == 0, IsDirRole); Item->setData(Row == 8, HiddenRole);
            Item->setData(Row == 3 ? 0.72 : 1.6, AspectRole); Item->setData(Badges[Row], BadgeRole); Item->setData(Sizes[Row] < 0 ? tr("파일 폴더") : formatSize(Sizes[Row]), SizeTextRole); Item->setData("2026-09-27", DateTextRole);
            Item->setData(Row == 1 || Row == 3, MarkedRole);
        }
        preview_->setModel(Model); preview_->listView()->setModelColumn(NameColumn); preview_->setCursorRow(2); PreviewRows->addWidget(preview_, 1); preview_->setMinimumHeight(360);
        auto* ServiceCard = new QGroupBox(tr("만들기 · 캐시"), settingsColumn_); MakeRows->addWidget(ServiceCard); auto* Service = new QFormLayout(ServiceCard); Service->setRowWrapPolicy(QFormLayout::WrapLongRows);
        Service->setContentsMargins(14, 12, 14, 14); Service->setVerticalSpacing(8);
        method_ = Combo(Service, "hc.settings.thumbMethod", tr("만드는 방법"), {tr("Windows 셸"), tr("내장 이미지 읽기"), tr("Windows 셸 → 내장 이미지")});
        targetMenu_ = new QPushButton(this); targetMenu_->setObjectName("hc.settings.thumbTargets"); auto* Menu = new QMenu(targetMenu_); targetMenu_->setMenu(Menu);
        for (const auto& Name : {tr("이미지"), tr("동영상"), tr("PDF"), tr("글꼴"), tr("문서")}) {
            auto* Check = new QCheckBox(Name, this); Check->hide(); Check->setObjectName("hc.settings.thumbTarget" + QString::number(targets_.size())); targets_.append(Check);
            auto* Action = Menu->addAction(Name); Action->setCheckable(true); targetActions_.append(Action);
            connect(Action, &QAction::toggled, Check, &QCheckBox::setChecked);
            connect(Check, &QCheckBox::toggled, Action, &QAction::setChecked);
            connect(Check, &QCheckBox::toggled, this, [this] { updateTargets(); });
        }
        Service->addRow(tr("대상"), targetMenu_);
        support_ = new QLabel(this); support_->setWordWrap(true); Service->addRow(support_);
        connect(method_, &QComboBox::currentIndexChanged, this, [this] { updateTargets(); });
        skipSlow_ = new QCheckBox(tr("느린 드라이브에서 새로 만들지 않기"), this); skipSlow_->setObjectName("hc.settings.thumbSkipSlow");
        skipSlow_->setToolTip(tr("네트워크·이동식·광학 드라이브의 새 생성 요청을 건너뜁니다. 앱 메모리 캐시에 남아 있는 이미지는 사용합니다."));
        SwitchRow(Service, skipSlow_, {});
        threads_ = new QSpinBox(this); threads_->setObjectName("hc.settings.thumbThreads"); threads_->setRange(1, 4); Service->addRow(tr("동시 생성 수 · 보이는 타일 우선"), threads_);
        cache_ = new QSpinBox(this); cache_->setObjectName("hc.settings.thumbCacheMb"); cache_->setRange(4, 96); cache_->setSuffix(" MB"); Service->addRow(tr("표시 중인 탭별 메모리 한도"), cache_);
        auto* AutoCard = new QGroupBox(tr("자동으로 썸네일 보기"), settingsColumn_); AutoRows->addWidget(AutoCard); auto* AutoForm = new QFormLayout(AutoCard); AutoForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
        AutoForm->setContentsMargins(14, 12, 14, 14); AutoForm->setVerticalSpacing(8);
        autoSwitch_ = new QCheckBox(tr("미디어가 많은 폴더에서 자동 전환"), this); autoSwitch_->setObjectName("hc.settings.thumbAuto"); SwitchRow(AutoForm, autoSwitch_, {});
        percent_ = new QSpinBox(this); percent_->setObjectName("hc.settings.thumbPercent"); percent_->setRange(5, 100); percent_->setSuffix("%"); AutoForm->addRow(tr("이미지·동영상 비율"), percent_);
        auto* AutoHelp = new QLabel(tr("파일 패널의 ‘자동’ 보기에서만 전환합니다. 폴더를 제외한 전체 파일 중 현재 대상·생성 방식·드라이브 정책에 해당하는 이미지·동영상 비율을 사용합니다. 실제 생성 성공률은 아닙니다."), AutoCard); AutoHelp->setWordWrap(true); AutoForm->addRow(AutoHelp);
        clear_ = new QPushButton(this); clear_->setObjectName("hc.settings.thumbClear"); Heading->addWidget(clear_, 0, Qt::AlignTop);
        clearStatus_ = new QLabel(this); clearStatus_->setWordWrap(true); clearStatus_->setObjectName("hc.settings.thumbClearStatus"); Service->addRow(clearStatus_);
        connect(clear_, &QPushButton::clicked, this, [this] { clearRequested_ = !clearRequested_; updateClear(); });
        auto* Note = new QLabel(tr("숨긴 탭의 생성 요청과 이미지 메모리를 해제합니다. 압축·플러그인 항목은 아이콘으로 표시합니다."), this); Note->setWordWrap(true); PreviewRows->addWidget(Note);
        auto* Reset = new QPushButton(tr("이 페이지 기본값 복원(&R)"), this); Reset->setObjectName("hc.settings.thumbReset"); Heading->addWidget(Reset, 0, Qt::AlignTop);
        connect(Reset, &QPushButton::clicked, this, [this] {
            auto Draft = Save(); for (const auto* Key : {"size", "nameLines", "info", "fill", "badges", "method", "threads", "cacheMb", "skipSlow", "autoSwitch", "autoPercent", "targets"}) Draft.remove(Key);
            Load(Draft);
        }); SettingsRows->addStretch(); MakeRows->addStretch(); AutoRows->addStretch();
        for (auto* Box : {size_, lines_, info_, fit_}) connect(Box, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
        connect(badges_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        for (auto* Check : {inverseCursor_, inverseSelection_}) connect(Check, &QCheckBox::toggled, this, [this] { updatePreview(); });
        connect(preview_, &fm::filelist::ThumbnailView::sizeChanged, this, [this](int Size) { size_->setCurrentIndex(Size == 64 ? 0 : Size == 160 ? 2 : Size == 256 ? 3 : 1); });
        connect(autoSwitch_, &QCheckBox::toggled, percent_, &QWidget::setEnabled);
        arrangeColumns(); Load({});
    }
    void Load(const QJsonObject& Object)
    {
        original_ = Object; supported_ = !Object.contains("version") || Object.value("version") == QJsonValue(1); setEnabled(supported_);
        if (!supported_) setToolTip(tr("다른 버전의 썸네일 설정은 원본 그대로 보존합니다."));
        const auto Value = ReadThumbnailSettings(Object); const auto& A = Value.appearance_;
        size_->setCurrentIndex(A.size == 64 ? 0 : A.size == 160 ? 2 : A.size == 256 ? 3 : 1);
        lines_->setCurrentIndex(A.nameLines == 0 ? 2 : A.nameLines - 1); info_->setCurrentIndex(int(A.info)); fit_->setCurrentIndex(A.fill ? 1 : 0); badges_->setChecked(A.badges);
        method_->setCurrentIndex(int(Value.method_)); threads_->setValue(Value.threads_); cache_->setValue(Value.cacheMb_); skipSlow_->setChecked(Value.skipSlow_);
        autoSwitch_->setChecked(Value.autoSwitch_); percent_->setValue(Value.autoPercent_); percent_->setEnabled(Value.autoSwitch_);
        for (int Index = 0; Index < targets_.size(); ++Index) targets_[Index]->setChecked(Value.targets_ & (1 << Index));
        if (auto* Check = window()->findChild<QCheckBox*>("hc.settings.inverseCursor")) inverseCursor_->setChecked(Check->isChecked());
        if (auto* Check = window()->findChild<QCheckBox*>("hc.settings.inverseSelection")) inverseSelection_->setChecked(Check->isChecked());
        MarkApplied(); updateTargets(); updatePreview();
    }
    QJsonObject Save() const
    {
        if (!supported_) return original_;
        auto Result = original_; Result["version"] = 1;
        Result["size"] = fm::filelist::kThumbnailSizes[std::clamp(size_->currentIndex(), 0, 3)];
        Result["nameLines"] = lines_->currentIndex() == 2 ? 0 : lines_->currentIndex() + 1;
        Result["info"] = info_->currentIndex(); Result["fill"] = fit_->currentIndex() == 1; Result["badges"] = badges_->isChecked();
        Result["method"] = method_->currentIndex(); Result["threads"] = threads_->value(); Result["cacheMb"] = cache_->value();
        Result["skipSlow"] = skipSlow_->isChecked(); Result["autoSwitch"] = autoSwitch_->isChecked(); Result["autoPercent"] = percent_->value();
        int Mask = 0; for (int Index = 0; Index < targets_.size(); ++Index) if (targets_[Index]->isChecked()) Mask |= 1 << Index;
        Result["targets"] = Mask; return Result;
    }
    bool ClearRequested() const { return supported_ && clearRequested_; }
    void MarkApplied() { clearRequested_ = false; updateClear(); }
protected:
    void resizeEvent(QResizeEvent* Event) override { QWidget::resizeEvent(Event); arrangeColumns(); }
private:
    void updateClear() {
        clear_->setText(clearRequested_ ? tr("캐시 비우기 예약 취소(&C)") : tr("앱 캐시 비우기 예약(&C)"));
        clearStatus_->setText(clearRequested_ ? tr("적용/확인 시 앱 메모리 캐시를 비웁니다. 설정창을 취소하면 실행하지 않습니다.") : tr("Windows 셸 자체 캐시는 비우지 않습니다. 기본값 복원도 적용/확인 전에는 저장되지 않습니다."));
    }
    void updateTargets() {
        if (!support_) return;
        const bool Builtin = method_->currentIndex() == 1; QStringList Names;
        for (int I = 0; I < targets_.size(); ++I) { targetActions_[I]->setEnabled(!Builtin || I == 0); if (targets_[I]->isChecked() && (!Builtin || I == 0)) Names.append(targets_[I]->text()); }
        targetMenu_->setText(Names.isEmpty() ? tr("선택 없음") : Names.join(QStringLiteral(" · ")));
        support_->setText(Builtin ? tr("내장 읽기는 이미지만 지원합니다. 다른 대상의 선택은 보관되며 셸 방식으로 돌아오면 복원됩니다.") : tr("동영상·PDF·글꼴·문서 지원은 설치된 Windows 셸 처리기에 따라 다릅니다. 셸 → 내장은 셸 실패 시 이미지를 내장 읽기로 시도합니다."));
    }
    void arrangeColumns() {
        previewCard_->setMaximumHeight(std::max(480, height() - 100));
        const bool Stacked = width() < 760;
        if (arranged_ && stacked_ == Stacked) return; arranged_ = true; stacked_ = Stacked;
        rows_->removeWidget(settingsColumn_); rows_->removeWidget(previewCard_);
        rows_->addWidget(settingsColumn_, 0, 0); rows_->addWidget(previewCard_, Stacked ? 1 : 0, Stacked ? 0 : 1, Qt::AlignTop);
        rows_->setColumnStretch(0, 1); rows_->setColumnStretch(1, Stacked ? 0 : 1);
    }
    void updatePreview()
    {
        auto Appearance = ReadThumbnailSettings(Save()).appearance_;
        Appearance.invertCursor = inverseCursor_->isChecked(); Appearance.invertSelection = inverseSelection_->isChecked();
        if (Appearance.nameLines == 0) {
            Appearance.nameLines = 1; const auto Font = fm::style::pixelFont(preview_->font(), 12.5);
            for (int Row = 0; Row < preview_->model()->rowCount(); ++Row) Appearance.nameLines = std::max(Appearance.nameLines, ThumbnailNameLines(preview_->model()->index(Row, fm::filelist::NameColumn).data(fm::filelist::FullNameRole).toString(), Appearance.tileWidth() - 20, Font));
        }
        preview_->setAppearance(Appearance);
    }
    QComboBox *size_, *lines_, *info_, *fit_, *method_;
    QCheckBox *badges_, *skipSlow_, *autoSwitch_, *inverseCursor_, *inverseSelection_;
    QSpinBox *threads_, *cache_, *percent_;
    QPushButton* clear_; QLabel* clearStatus_;
    QVector<QCheckBox*> targets_;
    QVector<QAction*> targetActions_; QPushButton* targetMenu_; QLabel* support_ = nullptr;
    fm::filelist::ThumbnailView* preview_;
    QGridLayout* rows_; QWidget *settingsColumn_, *previewCard_; bool arranged_ = false, stacked_ = false;
    QJsonObject original_;
    bool clearRequested_ = false, supported_ = true;
};
}
