#pragma once
#include <QtWidgets>
#include "UniqueLibs/qtMessageDialog.hpp"
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/SettingsWidgets.h>
#include <fmstyle/ColorScheme.h>
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeManager.h>
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/productTheme.hpp"
#include "themePreview.hpp"

namespace hc::ui {
class ThemeSettingsPage final : public QWidget {
public:
    explicit ThemeSettingsPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        using namespace fm::style; setObjectName("hc.settings.themeColors"); auto* Root = new QVBoxLayout(this);
        auto* Heading = new QHBoxLayout; Root->addLayout(Heading);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("테마 색상")); Header->setSubtitle(tr("기준 색 11개를 고르면 나머지 색이 규칙에 따라 맞춰집니다. 라이트·다크는 따로 저장합니다.")); Heading->addWidget(Header, 1);
        variant_ = new QComboBox(this); variant_->setObjectName("hc.theme.variant"); variant_->addItems({tr("라이트"), tr("다크")}); variant_->hide();
        // Board layout: scheme tools and the light/dark segment share the heading row.
        auto* Presets = new QHBoxLayout; Presets->setSpacing(6); Heading->addLayout(Presets);
        presets_ = new QComboBox(this); presets_->setObjectName("hc.theme.preset"); presets_->setAccessibleName(tr("색 구성표")); presets_->setMinimumContentsLength(10); presets_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); Presets->addWidget(presets_);
        auto* SaveAs = new QPushButton(tr("다른 이름으로 저장..."), this); SaveAs->setObjectName("hc.theme.saveAs"); Presets->addWidget(SaveAs);
        connect(presets_, &QComboBox::activated, this, [this](int I) { const auto Id = presets_->itemData(I).toString(); if (supported_) { if (scheme_.id == "builtin") builtinDraft_ = Save(); else library_[scheme_.id] = Save(); } Load(Id == "builtin" ? builtinDraft_ : library_[Id].toObject()); });
        connect(SaveAs, &QPushButton::clicked, this, [this] { bool Ok = false; const auto Name = hc::ui::InputDialog::getText(this, tr("색 구성표 이름"), tr("이름"), QLineEdit::Normal, scheme_.name, &Ok).trimmed(); if (!Ok || Name.isEmpty() || Name.size() > 128) return;
            SavePresetAs(Name);
        });
        all_ = new QCheckBox(tr("모든 토큰 51"), this); all_->setObjectName("hc.theme.allTokens"); all_->hide();
        auto* ImportButton = new QPushButton(tr("가져오기(&I)..."), this); auto* ExportButton = new QPushButton(tr("내보내기(&E)..."), this);
        Presets->addWidget(ImportButton); Presets->addWidget(ExportButton);
        auto* Variants = new QButtonGroup(this); Variants->setExclusive(true); QList<QAbstractButton*> VariantButtons;
        for (int I = 0; I < 2; ++I) { auto* Button = new QPushButton(variant_->itemText(I), this); Button->setCheckable(true); Button->setChecked(I == 0); Button->setToolTip(tr("편집 중인 변형")); Variants->addButton(Button, I); Presets->addWidget(Button); VariantButtons.append(Button); }
        fm::style::setSegments(VariantButtons);
        connect(Variants, &QButtonGroup::idClicked, variant_, &QComboBox::setCurrentIndex);
        connect(variant_, &QComboBox::currentIndexChanged, this, [Variants](int I) { Variants->button(I)->setChecked(true); });
        // Underline tabs: seeds · all tokens · contrast. The mode buttons stay as the programmatic switches.
        modeTabs_ = new QTabBar(this); modeTabs_->setObjectName("hc.theme.modeTabs"); modeTabs_->setDrawBase(true); modeTabs_->setExpanding(false);
        modeTabs_->addTab(tr("기준 색 11")); modeTabs_->addTab(tr("모든 토큰 51")); modeTabs_->addTab(tr("대비 검사"));
        Root->addWidget(modeTabs_);
        auto* Seeds = new QPushButton(tr("기준 색 11"), this); auto* Tokens = new QPushButton(tr("모든 토큰 51"), this);
        Seeds->setObjectName("hc.theme.seedsMode"); Tokens->setObjectName("hc.theme.tokensMode");
        auto* ModeGroup = new QButtonGroup(this); ModeGroup->setExclusive(true);
        for (auto* Button : {Seeds, Tokens}) { Button->setCheckable(true); ModeGroup->addButton(Button); Button->hide(); } Seeds->setChecked(true);
        connect(Seeds, &QPushButton::clicked, this, [this] { all_->setChecked(false); }); connect(Tokens, &QPushButton::clicked, this, [this] { all_->setChecked(true); });
        connect(all_, &QCheckBox::toggled, this, [this, Seeds, Tokens](bool On) { (On ? Tokens : Seeds)->setChecked(true); if (modeTabs_->currentIndex() != 2) { const QSignalBlocker Block(modeTabs_); modeTabs_->setCurrentIndex(On ? 1 : 0); } });
        connect(modeTabs_, &QTabBar::currentChanged, this, [this](int Index) {
            if (Index < 2) all_->setChecked(Index == 1);
            editor_->setVisible(Index < 2); contrastCard_->setVisible(Index == 2);
        });
        auto* Split = new QSplitter(this); Split->setObjectName("hc.theme.split"); Split->setChildrenCollapsible(false); Root->addWidget(Split, 1);
        auto* Left = new QGroupBox(tr("기준 색"), Split); leftCard_ = Left; auto* LeftRows = new QVBoxLayout(Left); LeftRows->setContentsMargins(8, 8, 8, 10);
        Left->setMinimumWidth(240); Left->setMaximumWidth(380);
        finder_ = new QWidget(Left); auto* FindRows = new QVBoxLayout(finder_); FindRows->setContentsMargins(0, 0, 0, 0); LeftRows->addWidget(finder_);
        search_ = new QLineEdit(finder_); search_->setObjectName("hc.theme.search"); search_->setPlaceholderText(tr("이름 · 토큰 · 용도로 찾기")); search_->setAccessibleName(tr("테마 색 검색")); search_->setClearButtonEnabled(true); FindRows->addWidget(search_);
        auto* Filters = new QHBoxLayout; FindRows->addLayout(Filters); auto* FilterGroup = new QButtonGroup(this);
        for (int I = 0; I < 3; ++I) { filters_[I] = new QPushButton(finder_); filters_[I]->setObjectName(QString("hc.theme.filter%1").arg(I)); filters_[I]->setCheckable(true); FilterGroup->addButton(filters_[I], I); Filters->addWidget(filters_[I]); } filters_[0]->setChecked(true);
        tree_ = new QTreeWidget(Left); tree_->setObjectName("hc.theme.tokenTree"); tree_->setHeaderHidden(true); tree_->setIconSize(QSize(20, 20)); tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); LeftRows->addWidget(tree_, 1);
        connect(search_, &QLineEdit::textChanged, this, [this] { filterTokens(); });
        connect(FilterGroup, &QButtonGroup::idClicked, this, [this](int I) { filterMode_ = I; filterTokens(); });
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { if (!loading_ && all_->isChecked()) loadColor(); });
        connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* Item) { if (!loading_ && !Item->parent()) collapsed_.insert(Item->data(0, Qt::UserRole).toInt()); });
        connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* Item) { if (!loading_ && !Item->parent()) collapsed_.remove(Item->data(0, Qt::UserRole).toInt()); });
        list_ = new QListWidget(Left); list_->setObjectName("hc.theme.tokens"); LeftRows->addWidget(list_, 1);
        list_->setIconSize(QSize(22, 22)); list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        individual_ = new QWidget(Left); auto* IndividualRows = new QVBoxLayout(individual_); IndividualRows->setContentsMargins(0, 0, 0, 0); LeftRows->addWidget(individual_);
        IndividualRows->addWidget(new QLabel(tr("개별 색 · 규칙 없이 하나씩 · 12개"), individual_));
        for (int Group = 0; Group < 2; ++Group) {
            auto* Button = new QPushButton(Group == 0 ? tr("파일 아이콘 · 8개") : tr("진행 · 권한 · 그림자 · 4개"), individual_);
            Button->setObjectName(Group == 0 ? "hc.theme.icons" : "hc.theme.effects"); auto* Menu = new QMenu(Button); Button->setMenu(Menu); IndividualRows->addWidget(Button);
            for (const auto& Info : allTokens()) if (!roleOfToken(Info.token) && (Info.group == TokenGroup::FileIcons) == (Group == 0)) {
                auto* Action = Menu->addAction(tokenLabel(Info.token)); Action->setData(int(Info.token));
                connect(Action, &QAction::triggered, this, [this, TokenValue = Info.token] { all_->setChecked(true); search_->clear(); filterMode_ = 0; filters_[0]->setChecked(true); filterTokens(); selectToken(int(TokenValue)); });
            }
        }
        auto* Reset = new QPushButton(tr("이 변형의 지정 초기화(&R)"), Left); Reset->setObjectName("hc.theme.reset"); Reset->setToolTip(tr("현재 라이트/다크의 기준 색과 토큰 직접 지정을 지웁니다. 공통 강조색·Windows 연동과 다른 변형은 유지합니다.")); LeftRows->addWidget(Reset);
        auto* Scroll = new QScrollArea(Split); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        auto* Right = new QWidget; Scroll->setWidget(Right); auto* Rows = new QVBoxLayout(Right);
        Scroll->setMinimumWidth(300); Split->setStretchFactor(0, 0); Split->setStretchFactor(1, 1); Split->setSizes({280, 560});
        auto* Editor = new QGroupBox(Right); editor_ = Editor; Editor->setObjectName("hc.theme.editor"); auto* Form = new QFormLayout(Editor); editorForm_ = Form; Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Rows->addWidget(Editor);
        Form->setContentsMargins(14, 12, 14, 12); Form->setVerticalSpacing(8);
        title_ = new QLabel(Editor); title_->setWordWrap(true); title_->hide();
        // One row: picker · HEX value; the next row: scope and the automatic / direct actions.
        auto* ColorRow = new QHBoxLayout; ColorRow->setSpacing(8);
        color_ = new fm::ui::ColorPickButton(Editor); color_->setAllowNone(true); color_->setObjectName("hc.theme.color"); color_->setAccessibleName(tr("선택한 색 편집")); color_->setToolTip(tr("지정 안 함은 자동 색을 사용합니다.")); ColorRow->addWidget(color_, 1);
        auto* HexCaption = new QLabel(tr("값 · HEX"), Editor); ColorRow->addWidget(HexCaption);
        hex_ = new QLineEdit(Editor); hex_->setObjectName("hc.theme.hex"); hex_->setAccessibleName(tr("색상 값")); hex_->setMaximumWidth(120); HexCaption->setBuddy(hex_); ColorRow->addWidget(hex_);
        Form->addRow(tr("색 선택"), ColorRow);
        scope_ = new QLabel(Editor); scope_->setObjectName("hc.theme.scope"); scope_->setWordWrap(true);
        auto* ColorActions = new QHBoxLayout; ColorActions->setSpacing(6); ColorActions->addWidget(scope_, 1); Form->addRow(ColorActions);
        auto* ResetColor = new QPushButton(tr("자동으로"), Editor); ResetColor->setObjectName("hc.theme.resetColor"); ColorActions->addWidget(ResetColor);
        direct_ = new QPushButton(tr("직접 지정"), Editor); direct_->setObjectName("hc.theme.direct"); ColorActions->addWidget(direct_);
        source_ = new QPushButton(tr("기준 색 편집"), Editor); source_->setObjectName("hc.theme.source"); ColorActions->addWidget(source_);
        picker_ = new fm::ui::AccentPicker(Editor); Form->addRow(tr("강조색 견본"), picker_);
        follow_ = new QCheckBox(tr("강조색과 연동"), Editor); follow_->setObjectName("hc.theme.follow"); Form->addRow(follow_);
        derived_ = new QLabel(Editor); derived_->setTextFormat(Qt::PlainText); derived_->setWordWrap(true); Form->addRow(derived_);
        derivedTable_ = new QTableWidget(0, 3, Editor); derivedTable_->setObjectName("hc.theme.derived"); derivedTable_->setHorizontalHeaderLabels({tr("파생 색"), tr("값"), tr("규칙 · 출처")});
        derivedTable_->verticalHeader()->hide(); derivedTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); derivedTable_->setSelectionMode(QAbstractItemView::NoSelection);
        derivedTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); derivedTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        derivedTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); Form->addRow(derivedTable_);
        derivedTable_->setWordWrap(false); derivedTable_->setAccessibleName(tr("파생 색과 적용 규칙"));
        auto* PreviewCard = new QGroupBox(tr("미리보기 · 적용 전 예제 파일과 행 상태"), Right); auto* PreviewRows = new QVBoxLayout(PreviewCard); Rows->addWidget(PreviewCard);
        PreviewRows->setContentsMargins(10, 10, 10, 10);
        preview_ = new ThemePreview(PreviewCard); preview_->setObjectName("hc.theme.preview"); PreviewRows->addWidget(preview_);
        contrast_ = new QLabel(PreviewCard); contrast_->setTextFormat(Qt::PlainText); contrast_->setWordWrap(true); contrast_->setObjectName("hc.theme.contrast"); PreviewRows->addWidget(contrast_);
        // The contrast table is its own tab on the board; the preview keeps a one-line pass count.
        contrastCard_ = new QGroupBox(tr("대비 검사 · 본문 글자 기준 4.5:1"), Right); contrastCard_->setObjectName("hc.theme.contrastCard"); auto* ContrastRows = new QVBoxLayout(contrastCard_);
        ContrastRows->setContentsMargins(10, 10, 10, 10); Rows->insertWidget(0, contrastCard_); contrastCard_->hide();
        contrastTable_ = new QTableWidget(0, 3, contrastCard_); contrastTable_->setObjectName("hc.theme.contrastTable"); contrastTable_->setHorizontalHeaderLabels({tr("검사 항목"), tr("대비"), tr("결과")});
        contrastTable_->verticalHeader()->hide(); contrastTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); contrastTable_->setSelectionMode(QAbstractItemView::NoSelection);
        contrastTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); ContrastRows->addWidget(contrastTable_);
        fix_ = new QCheckBox(tr("파생 색의 대비 자동 보정"), Editor); fix_->setToolTip(tr("직접 지정한 색은 보정하지 않습니다.")); fix_->setObjectName("hc.theme.fixContrast"); Form->addRow(fix_);
        Rows->addStretch();
        status_ = new QLabel(this); status_->setObjectName("hc.theme.status"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); Root->addWidget(status_);
        connect(variant_, &QComboBox::currentIndexChanged, this, [this] { refresh(selected()); });
        connect(all_, &QCheckBox::toggled, this, [this] { refresh(); });
        connect(list_, &QListWidget::currentRowChanged, this, [this] { if (!loading_ && !all_->isChecked()) loadColor(); });
        connect(color_, &fm::ui::ColorPickButton::colorChanged, this, [this](const auto& Color) { if (!loading_) editColor(Color); });
        connect(hex_, &QLineEdit::editingFinished, this, [this] { if (!hex_->isModified()) return; const auto Color = parseColorHex(hex_->text()); if (Color.isValid()) editColor(Color); else { showStatus(tr("색상은 #RRGGBB 또는 #AARRGGBB로 입력하세요.")); } });
        connect(ResetColor, &QPushButton::clicked, this, [this] { editColor(std::nullopt); });
        connect(direct_, &QPushButton::clicked, this, [this] { const auto Colors = deriveDraft().colors; editColor(all_->isChecked() ? Colors[Token(selected())] : seedColor(SeedRole(selected()), Colors, effectiveSeeds())); });
        connect(source_, &QPushButton::clicked, this, [this] { const auto Role = roleOfToken(Token(selected())); if (Role) { all_->setChecked(false); list_->setCurrentRow(int(*Role)); } });
        connect(picker_, &fm::ui::AccentPicker::accentChosen, this, [this](const auto& Color) { editColor(Color); });
        connect(picker_, &fm::ui::AccentPicker::customRequested, this, [this] { const auto Color = QColorDialog::getColor(seedColor(SeedRole::Accent, deriveDraft().colors, effectiveSeeds()), this); if (Color.isValid()) editColor(Color); });
        connect(follow_, &QCheckBox::toggled, this, [this](bool On) { if (loading_) return; auto& S = scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()); if (selected() == int(SeedRole::Sel)) S.selFollowsAccent = On; else S.invSelFollowsAccent = On; refresh(selected()); });
        connect(fix_, &QCheckBox::toggled, this, [this](bool On) { if (loading_) return; scheme_.seeds.fixContrast = On; refresh(selected()); });
        connect(Reset, &QPushButton::clicked, this, [this] { scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()) = {}; scheme_.overridesFor(hc::theme::ProductDesign, variant()) = {}; refresh(); });
        connect(ImportButton, &QPushButton::clicked, this, [this] { const auto Path = QFileDialog::getOpenFileName(this, tr("테마 가져오기"), {}, "JSON (*.json)"); if (!Path.isEmpty()) ImportFile(Path); });
        connect(ExportButton, &QPushButton::clicked, this, [this] { const auto Path = QFileDialog::getSaveFileName(this, tr("테마 내보내기"), {}, "JSON (*.json)"); if (!Path.isEmpty()) Export(Path); });
        Load({});
    }
    void LoadLibrary(const QJsonObject& Library) { library_ = Library; builtinDraft_ = {}; }
    bool SavePresetAs(const QString& Value) {
        const auto Name = Value.trimmed(); if (!supported_ || Name.isEmpty() || Name.size() > 128) return false;
        for (const auto Entry : library_) if (Entry.toObject()["name"].toString().compare(Name, Qt::CaseInsensitive) == 0) { showStatus(tr("같은 이름의 색 구성표가 있습니다.")); return false; }
        if (scheme_.id == "builtin") builtinDraft_ = Save();
        scheme_.id = "hc." + QUuid::createUuid().toString(QUuid::WithoutBraces); scheme_.name = Name; library_[scheme_.id] = Save(); refresh(selected()); return true;
    }
    bool ImportFile(const QString& Path) {
        QFile File(Path); if (!File.open(QIODevice::ReadOnly) || File.size() > 1024 * 1024) { showStatus(tr("테마 파일을 읽을 수 없습니다.")); return false; }
        QJsonParseError Error; const auto Doc = QJsonDocument::fromJson(File.readAll(), &Error); if (Error.error != QJsonParseError::NoError || !Doc.isObject()) { showStatus(tr("JSON 형식을 확인하세요.")); return false; } return Import(Doc.object());
    }
    bool Export(const QString& Path) {
        if (!supported_) return false;
        if (QFileInfo(Path).absoluteFilePath().compare(QFileInfo(settings::AppSettings().FileName()).absoluteFilePath(), Qt::CaseInsensitive) == 0) { showStatus(tr("사용 중인 설정 파일은 적용/확인으로 저장하세요.")); return false; }
        QSaveFile File(Path); File.setDirectWriteFallback(false); auto Data = QJsonDocument(Save()).toJson(); Data.replace("\n", "\r\n");
        if (!File.open(QIODevice::WriteOnly) || File.write(Data) != Data.size() || !File.commit()) { showStatus(tr("테마 내보내기에 실패했습니다.")); return false; } return true;
    }
    QJsonObject SaveLibrary() const { auto Library = library_; if (supported_ && scheme_.id != "builtin") Library[scheme_.id] = Save(); return Library; }
    void Load(const QJsonObject& Object) {
        original_ = Object; supported_ = Object.isEmpty() || (Object["format"] == "fm-color-scheme" && Object["version"] == QJsonValue(1));
        scheme_ = supported_ ? hc::theme::ReadProductScheme(Object) : fm::style::ColorScheme{};
        if (supported_ && scheme_.id == "builtin") builtinDraft_ = Object;
        setEnabled(supported_); showStatus(supported_ ? QString() : tr("지원하지 않는 테마 형식은 원본 그대로 보존합니다.")); refresh();
    }
    bool Import(const QJsonObject& Object) {
        QString Error;
        if (Object["version"] != QJsonValue(1) || Object["format"] != "fm-color-scheme") Error = tr("지원하지 않는 테마 형식입니다.");
        const auto Parsed = fm::style::ColorScheme::fromJson(Object, &Error);
        // The library tolerates older partial files; reject malformed explicit
        // colors here so imports never silently lose a requested color.
        std::function<bool(const QJsonObject&)> Colors = [&](const QJsonObject& O) { for (auto It = O.begin(); It != O.end(); ++It) {
            if (It.key() == "seeds" || It.key() == "overrides") { const auto Values = It.value().toObject(); for (const auto V : Values) if (!V.isString() || !fm::style::parseColorHex(V.toString()).isValid()) return false; }
            else if (It.value().isObject() && !Colors(It.value().toObject())) return false;
        } return true; };
        const QJsonValue Accent = Object["accent"].toObject().value("color");
        if (!Colors(Object) || (!Accent.isNull() && !Accent.isUndefined() && (!Accent.isString() || !fm::style::parseColorHex(Accent.toString()).isValid()))) Error = tr("직접 지정한 색상 형식이 올바르지 않습니다.");
        if (!Parsed || !Error.isEmpty()) { showStatus(Error); return false; } Load(Object); return true;
    }
    QJsonObject Save() const {
        if (!supported_) return original_; auto Result = original_; auto Designs = Result["designs"].toObject();
        // Remove known optional values before overlaying the new draft so Reset
        // really clears them, while unknown metadata remains round-trippable.
        for (const auto& D : {QString("standard"), QString("watercolor"), QString("paperfluent")}) {
            auto Variants = Designs[D].toObject();
            for (const auto& V : {QString("light"), QString("dark"), QString("navy")}) { auto O = Variants[V].toObject(); auto Seeds = O["seeds"].toObject(); auto Overrides = O["overrides"].toObject();
                for (const auto& R : fm::style::seedRoles()) Seeds.remove(QString::fromLatin1(R.id)); for (const auto& T : fm::style::allTokens()) Overrides.remove(fm::style::tokenCssName(T.token));
                auto Follow = O["follow"].toObject(); Follow.remove("sel"); Follow.remove("invsel"); O["seeds"] = Seeds; O["overrides"] = Overrides; O["follow"] = Follow; Variants[V] = O;
            } Designs[D] = Variants;
        } Result["designs"] = Designs;
        std::function<QJsonObject(QJsonObject, const QJsonObject&)> Overlay = [&](QJsonObject Base, const QJsonObject& New) { for (auto It = New.begin(); It != New.end(); ++It) Base[It.key()] = It.value().isObject() ? QJsonValue(Overlay(Base[It.key()].toObject(), It.value().toObject())) : It.value(); return Base; };
        return Overlay(Result, scheme_.toJson());
    }
    std::function<void()> onChanged_;
    const fm::style::ColorScheme& Scheme() const { return scheme_; }
    void SetAccent(std::optional<QColor> Color) { scheme_.seeds.accent = Color; refresh(selected()); }
    void SetSystemAccent(bool On) { scheme_.seeds.useSystemAccent = On; refresh(selected()); }
private:
    // The footer already states the draft rule; this line only carries import / value errors.
    void showStatus(const QString& Text) { status_->setText(Text); status_->setVisible(!Text.isEmpty()); }
    fm::style::ThemeSeeds effectiveSeeds() const {
        auto Seeds = scheme_.seeds;
        if (Seeds.useSystemAccent) if (const auto System = fm::style::ThemeManager::systemAccent()) Seeds.accent = *System;
        return Seeds;
    }
    fm::style::DerivedTheme deriveDraft() const { return fm::style::deriveTheme(variant(), effectiveSeeds(), scheme_.overridesFor(hc::theme::ProductDesign, variant()), hc::theme::ProductDesign); }
    void editColor(std::optional<QColor> Color) {
        using namespace fm::style;
        if (all_->isChecked()) scheme_.overridesFor(hc::theme::ProductDesign, variant())[std::size_t(selected())] = Color;
        else {
            auto& Seeds = scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()); Seeds.seed[std::size_t(selected())] = Color;
            // A selected seed is only used by the engine when accent following is off.
            if (selected() == int(SeedRole::Sel)) Seeds.selFollowsAccent = !Color.has_value();
            if (selected() == int(SeedRole::InvSel)) Seeds.invSelFollowsAccent = !Color.has_value();
        }
        refresh(selected());
    }
    fm::style::Variant variant() const { return variant_->currentIndex() == 1 ? fm::style::Variant::Dark : fm::style::Variant::Light; }
    int selected() const { return all_->isChecked() ? (tree_->currentItem() && tree_->currentItem()->parent() ? tree_->currentItem()->data(0, Qt::UserRole).toInt() : 0) : (list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toInt() : 0); }
    void selectToken(int Value) {
        for (int Group = 0; Group < tree_->topLevelItemCount(); ++Group) { auto* Parent = tree_->topLevelItem(Group);
            for (int Row = 0; Row < Parent->childCount(); ++Row) if (Parent->child(Row)->data(0, Qt::UserRole).toInt() == Value) { tree_->setCurrentItem(Parent->child(Row)); Parent->setExpanded(true); tree_->scrollToItem(Parent->child(Row)); return; }
        }
    }
    void filterTokens() {
        if (!tree_ || !editor_) return;
        const auto Query = search_->text().trimmed(); QTreeWidgetItem* First = nullptr;
        for (int Group = 0; Group < tree_->topLevelItemCount(); ++Group) { auto* Parent = tree_->topLevelItem(Group); int Visible = 0;
            for (int Row = 0; Row < Parent->childCount(); ++Row) { auto* Item = Parent->child(Row);
                const bool Match = Item->data(0, Qt::UserRole + 1).toString().contains(Query, Qt::CaseInsensitive) && (filterMode_ == 0 || Item->data(0, Qt::UserRole + 1 + filterMode_).toBool());
                Item->setHidden(!Match); if (Match) { ++Visible; if (!First) First = Item; }
            }
            Parent->setHidden(Visible == 0); if (!Query.isEmpty() || filterMode_) Parent->setExpanded(true);
        }
        if (all_->isChecked()) { auto* Current = tree_->currentItem(); if (!Current || !Current->parent() || Current->isHidden() || Current->parent()->isHidden()) tree_->setCurrentItem(First); editor_->setEnabled(First != nullptr); }
    }
    QIcon swatch(const QColor& Color) const { QPixmap Pixmap(22, 22); Pixmap.fill(Color); return QIcon(Pixmap); }
    QString seedState(fm::style::SeedRole Role) const {
        using namespace fm::style; const auto& Seeds = scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant());
        if ((Role == SeedRole::Sel && Seeds.selFollowsAccent) || (Role == SeedRole::InvSel && Seeds.invSelFollowsAccent)) return tr("강조 연동");
        if (Seeds.seed[std::size_t(Role)]) return tr("직접 지정");
        if (Role == SeedRole::Accent && scheme_.seeds.useSystemAccent) return tr("Windows 연동");
        if (Role == SeedRole::Accent && scheme_.seeds.accent) return tr("공통 강조색");
        return tr("기본");
    }
    void refresh(int Row = 0) {
        using namespace fm::style; loading_ = true; const auto Colors = deriveDraft().colors;
        list_->clear();
        individual_->setVisible(!all_->isChecked()); list_->setVisible(!all_->isChecked()); finder_->setVisible(all_->isChecked()); tree_->setVisible(all_->isChecked());
        leftCard_->setTitle((all_->isChecked() ? tr("모든 토큰") : tr("기준 색")) + tr(" · 편집 중 ") + (variant() == Variant::Light ? tr("라이트") : tr("다크")));
        tree_->clear(); const QStringList GroupNames{tr("바탕 · 면"), tr("글자"), tr("강조 · 선택"), tr("역상 표시"), tr("레코드 틴트"), tr("상태"), tr("권한"), tr("파일 아이콘"), tr("효과")};
        std::array<QTreeWidgetItem*, 9> Groups{}; for (int I = 0; I < 9; ++I) { Groups[I] = new QTreeWidgetItem(tree_); Groups[I]->setData(0, Qt::UserRole, I); Groups[I]->setFlags(Qt::ItemIsEnabled); }
        QSet<int> Warnings; for (const auto& Check : checkContrast(Colors)) if (!Check.passes) { Warnings.insert(int(Check.foreground)); Warnings.insert(int(Check.background)); }
        int Changed = 0;
        for (const auto& Info : allTokens()) {
            const auto Role = roleOfToken(Info.token); const auto Override = scheme_.overridesFor(hc::theme::ProductDesign, variant())[indexOf(Info.token)];
            const bool Modified = Override.has_value() || Colors[Info.token] != ThemeColors::builtin(variant(), hc::theme::ProductDesign)[Info.token] || (Role && scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()).seed[std::size_t(*Role)].has_value());
            const auto State = Override ? tr("직접 지정") : Role ? (tokenSource(Info.token, hc::theme::ProductDesign).isSeed ? seedState(*Role) : tr("자동 · ") + QString::fromUtf8(seedRoleInfo(*Role).label)) : tr("기본");
            auto* Item = new QTreeWidgetItem(Groups[int(Info.group)]); Item->setIcon(0, swatch(Colors[Info.token])); Item->setText(0, tokenLabel(Info.token) + " · " + State + "\n" + tokenCssName(Info.token) + "  " + tokenDisplayValue(Info.token, Colors[Info.token]));
            Item->setData(0, Qt::UserRole, int(Info.token)); Item->setData(0, Qt::UserRole + 1, tokenLabel(Info.token) + " " + tokenCssName(Info.token) + " " + tokenUsage(Info.token)); Item->setData(0, Qt::UserRole + 2, Modified); Item->setData(0, Qt::UserRole + 3, Warnings.contains(int(Info.token))); Item->setToolTip(0, Item->text(0) + "\n" + tokenUsage(Info.token));
            if (Modified) ++Changed;
        }
        for (int I = 0; I < 9; ++I) { Groups[I]->setText(0, GroupNames[I] + " · " + QString::number(Groups[I]->childCount())); Groups[I]->setExpanded(!collapsed_.contains(I)); }
        filters_[0]->setText(tr("전체 51")); filters_[1]->setText(tr("바뀜 %1").arg(Changed)); filters_[2]->setText(tr("경고 %1").arg(Warnings.size()));
        for (const auto& Info : seedRoles()) {
            const auto Color = seedColor(Info.role, Colors, effectiveSeeds());
            auto* Item = new QListWidgetItem(swatch(Color), QString::fromUtf8(Info.label) + "  ·  " + seedState(Info.role) + "\n" + colorHex(Color) + tr("  ·  %1개 토큰").arg(Info.tokenCount), list_);
            Item->setData(Qt::UserRole, int(Info.role)); Item->setToolTip(QString::fromUtf8(Info.description));
            Item->setSizeHint(QSize(0, fontMetrics().height() * 2 + 8));
        }
        list_->setCurrentRow(qBound(0, Row, list_->count() - 1)); fix_->setChecked(scheme_.seeds.fixContrast); picker_->setCurrent(scheme_.seeds.accent);
        preview_->SetColors(Colors); const auto Checks = checkContrast(Colors); contrastTable_->setRowCount(Checks.size());
        const auto Adjusted = deriveDraft().adjusted; int ContrastHeight = contrastTable_->horizontalHeader()->height() + 4;
        for (int I = 0; I < Checks.size(); ++I) { const auto& Check = Checks[I];
            contrastTable_->setItem(I, 0, new QTableWidgetItem(Check.label)); contrastTable_->setItem(I, 1, new QTableWidgetItem(QString::number(Check.ratio, 'f', 1) + ":1"));
            const auto Result = (Check.passes ? tr("통과") : tr("낮음")) + (Adjusted.test(indexOf(Check.foreground)) || Adjusted.test(indexOf(Check.background)) ? tr(" · 보정됨") : QString());
            contrastTable_->setItem(I, 2, new QTableWidgetItem(Result)); contrastTable_->setRowHeight(I, fontMetrics().height() + 6); ContrastHeight += contrastTable_->rowHeight(I);
        }
        contrastTable_->setFixedHeight(ContrastHeight);
        const auto Passed = std::count_if(Checks.begin(), Checks.end(), [](const auto& Check) { return Check.passes; });
        contrast_->setText(tr("대비 %1/%2 통과 · 본문 글자 기준 4.5:1 · 직접 지정 색은 자동 보정하지 않습니다.").arg(Passed).arg(Checks.size()));
        presets_->clear(); presets_->addItem(tr("기본 (내장)"), "builtin"); for (auto It = library_.begin(); It != library_.end(); ++It) if (fm::style::ColorScheme::fromJson(It.value().toObject())) presets_->addItem(It.value().toObject()["name"].toString(), It.key());
        int PresetIndex = presets_->findData(scheme_.id); if (PresetIndex < 0) { presets_->addItem(scheme_.name.isEmpty() ? tr("가져온 구성표") : scheme_.name, scheme_.id); PresetIndex = presets_->count() - 1; }
        const bool Modified = scheme_.id == "builtin" ? !scheme_.isPristine() : !scheme_.sameColors(ColorScheme::fromJson(library_[scheme_.id].toObject()).value_or(ColorScheme{}));
        if (Modified) presets_->setItemText(PresetIndex, presets_->itemText(PresetIndex) + tr(" — 수정됨")); presets_->setCurrentIndex(PresetIndex);
        if (all_->isChecked()) selectToken(Row); loading_ = false; filterTokens(); loadColor(); if (supported_) showStatus({}); if (onChanged_) onChanged_();
    }
    void loadColor() {
        editor_->setEnabled(!all_->isChecked() || (tree_->currentItem() && tree_->currentItem()->parent())); if (!editor_->isEnabled()) return;
        using namespace fm::style; loading_ = true; const auto Draft = deriveDraft(); const auto& Colors = Draft.colors;
        QList<Token> Related;
        if (all_->isChecked()) { const auto T = Token(selected()); title_->setText(tokenLabel(T) + " · " + tokenUsage(T)); editor_->setTitle(title_->text()); color_->setColor(scheme_.overridesFor(hc::theme::ProductDesign, variant())[std::size_t(T)]); derived_->setText(tokenCssName(T) + " · " + (tokenInfo(T).qtRole ? QString::fromLatin1(tokenInfo(T).qtRole) : tr("전용 그리기"))); Related.append(T); hex_->setText(colorHex(Colors[T])); }
        else { const auto R = SeedRole(selected()); const auto& I = seedRoleInfo(R); title_->setText(QString::fromUtf8(I.label) + " · " + QString::fromUtf8(I.description)); editor_->setTitle(title_->text()); color_->setColor(scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()).seed[std::size_t(R)]); derived_->setText(tr("기준 색 변경을 따르는 색 · 직접 지정된 토큰은 유지합니다.")); for (int J = 0; J < I.tokenCount; ++J) Related.append(I.tokens[J]); hex_->setText(colorHex(color_->color().value_or(seedColor(R, Colors, effectiveSeeds())))); }
        derivedTable_->setRowCount(Related.size());
        for (int Row = 0; Row < Related.size(); ++Row) { const auto T = Related[Row]; auto* Name = new QTableWidgetItem(swatch(Colors[T]), tokenLabel(T)); derivedTable_->setItem(Row, 0, Name); derivedTable_->setItem(Row, 1, new QTableWidgetItem(tokenDisplayValue(T, Colors[T])));
            const auto Rule = scheme_.overridesFor(hc::theme::ProductDesign, variant())[indexOf(T)] ? tr("직접 지정 · 기준 색보다 우선") : tokenSource(T, hc::theme::ProductDesign).rule(variant());
            auto* RuleItem = new QTableWidgetItem(Rule + (Draft.adjusted.test(indexOf(T)) ? tr(" · 대비 보정됨") : QString())); RuleItem->setToolTip(RuleItem->text()); derivedTable_->setItem(Row, 2, RuleItem);
        }
        derivedTable_->resizeRowsToContents(); int TableHeight = derivedTable_->horizontalHeader()->height() + 4; for (int Row = 0; Row < Related.size(); ++Row) TableHeight += derivedTable_->rowHeight(Row); derivedTable_->setFixedHeight(TableHeight);
        const auto Scope = variant() == Variant::Light ? tr("라이트만") : tr("다크만");
        const bool AccentSeed = !all_->isChecked() && selected() == int(SeedRole::Accent);
        scope_->setText(Scope + tr(" · ") + (color_->color() ? tr("직접 지정") : AccentSeed ? seedState(SeedRole::Accent) : tr("자동")));
        scope_->setToolTip(AccentSeed ? tr("우선순위: 이 변형의 직접 지정 → Windows 강조색(사용 시) → 일반 · 모양의 공통 강조색 → 내장 색. 자동으로 되돌리면 공통 설정을 따릅니다.") : tr("토큰 직접 지정은 기준 색보다 우선합니다."));
        source_->setVisible(all_->isChecked() && roleOfToken(Token(selected())).has_value()); direct_->setEnabled(!color_->color().has_value());
        follow_->setVisible(!all_->isChecked() && (selected() == int(SeedRole::Sel) || selected() == int(SeedRole::InvSel)));
        const auto& S = scheme_.seeds.variantSeeds(hc::theme::ProductDesign, variant()); follow_->setChecked(selected() == int(SeedRole::Sel) ? S.selFollowsAccent : S.invSelFollowsAccent);
        editorForm_->setRowVisible(picker_, !all_->isChecked() && selected() == int(SeedRole::Accent)); picker_->setCurrent(seedColor(SeedRole::Accent, Colors, effectiveSeeds())); loading_ = false;
    }
    fm::style::ColorScheme scheme_; QJsonObject original_, library_, builtinDraft_; bool loading_ = false, supported_ = true;
    QComboBox *variant_, *presets_; QCheckBox *all_, *fix_, *follow_; QListWidget* list_; fm::ui::ColorPickButton* color_; fm::ui::AccentPicker* picker_;
    QFormLayout* editorForm_; QLineEdit* hex_; QLabel* scope_; QPushButton *direct_, *source_; QTableWidget* derivedTable_;
    QWidget *finder_; QGroupBox* editor_ = nullptr; QGroupBox* leftCard_ = nullptr; QGroupBox* contrastCard_ = nullptr; QTabBar* modeTabs_ = nullptr; QLineEdit* search_; QTreeWidget* tree_ = nullptr; std::array<QPushButton*, 3> filters_{}; int filterMode_ = 0; QSet<int> collapsed_;
    QTableWidget* contrastTable_; QWidget* individual_; QLabel *title_, *derived_, *contrast_, *status_; ThemePreview* preview_;
};
}
