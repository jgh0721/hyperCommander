#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include "UniqueLibs/settings/fileColorRules.hpp"
#include "UniqueLibs/fileRulePaint.hpp"
#include <QtWidgets>
#include <QCryptographicHash>
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/cmnTypeDefs.hpp"
#include <fmwidgets/Button.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/ReorderList.h>
#include <fmwidgets/SettingsWidgets.h>
#include <fmwidgets/DialogHeader.h>
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeManager.h>
#include <fmstyle/StylePaint.h>
#include "fileColorConditionRow.hpp"
#include "commonLibs/columns/columnQueryEngine.hpp"
#include "UniqueLibs/columnPluginFields.hpp"
#include "UniqueLibs/colorSchemeMgr.hpp"
#include <fmfilelist/RecordPainting.h>
#include "UniqueLibs/settings/fileColorExamples.hpp"
#include <functional>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

namespace hc::ui {
class FileRulePreviewDelegate final : public QStyledItemDelegate
{
public:
    using Resolver = std::function<QPair<QColor, QColor>(int, bool, bool)>;
    using Effects = std::function<QJsonObject(int)>;
    FileRulePreviewDelegate(Resolver Resolve, Effects Effect, QTableWidget* Parent)
        : QStyledItemDelegate(Parent), resolve_(std::move(Resolve)), effects_(std::move(Effect)), table_(Parent) {}
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        auto Styled = Option; initStyleOption(&Styled, Index); const bool Selected = Option.state & QStyle::State_Selected;
        const auto Colors = resolve_(Index.row(), Selected, Index.column() == 0);
        Painter->save(); Painter->fillRect(Option.rect, Colors.second); Painter->setPen(Colors.first);
        if (Selected && fm::style::ThemeManager::instance().design() != fm::style::Design::Watercolor && Index.column() == 0) Styled.font.setBold(true);
        const auto Effects = effects_(Index.row());
        Painter->setFont(Styled.font); const auto Rect = Option.rect.adjusted(6, 0, -6, 0);
        if (Effects["dim"].toBool()) Painter->setOpacity(Painter->opacity() * 0.55);
        Painter->drawText(Rect, Qt::AlignVCenter | (Index.column() == 1 ? Qt::AlignRight : Qt::AlignLeft), QFontMetrics(Styled.font).elidedText(Styled.text, Qt::ElideRight, Rect.width()));
        if (Option.state & QStyle::State_HasFocus) { QStyleOptionFocusRect Focus; Focus.rect = Option.rect.adjusted(1, 1, -1, -1); Focus.state = Option.state; Focus.backgroundColor = Colors.second; QApplication::style()->drawPrimitive(QStyle::PE_FrameFocusRect, &Focus, Painter); }
        const auto RowRect = table_->visualRect(Index.siblingAtColumn(0)).united(table_->visualRect(Index.siblingAtColumn(table_->columnCount() - 1)));
        Painter->setClipRect(Option.rect, Qt::IntersectClip);
        PaintFileRuleBorder(Painter, RowRect, resolve_(Index.row(), Option.state.testFlag(QStyle::State_Selected), true).first, Effects);
        // Rows waiting for field values carry the panel's left marker (design 6.4).
        PaintFileRulePending(Painter, RowRect, Effects);
        Painter->restore();
    }
private:
    Resolver resolve_;
    Effects effects_;
    QTableWidget* table_;
};
class FileRulePreviewLabel final : public QLabel
{
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter Painter(this); Painter.setFont(font()); Painter.setPen(palette().color(QPalette::WindowText));
        const auto Effects = property("hc.fileEffects").toJsonObject();
        if (Effects["dim"].toBool()) Painter.setOpacity(0.55);
        Painter.drawText(contentsRect().adjusted(4, 0, -4, 0), alignment(), text());
        PaintFileRuleBorder(&Painter, contentsRect(), palette().color(QPalette::WindowText), Effects);
        PaintFileRulePending(&Painter, contentsRect(), Effects);
    }
};
class FileFontEffectButton final : public QPushButton
{
public:
    FileFontEffectButton(const QString& Label, QWidget* Parent) : QPushButton(Parent), label_(Label) { setAutoDefault(false); setCheckable(true); SetState(0); }
    int State() const { return state_; }
    void SetState(int State) {
        state_ = State; const auto Value = State == 1 ? tr("켬") : State == 2 ? tr("끔") : tr("기본");
        setText(label_ + " · " + Value); setAccessibleName(text()); setChecked(State == 1);
        setToolTip(tr("%1: 기본 → 켬 → 끔 순서로 전환합니다. 기본은 아래 그룹 또는 목록 기본값을 따릅니다.").arg(label_));
    }
private:
    QString label_;
    int state_ = 0;
};
// Retain the shared reorder delegate's grip and native checkbox interaction,
// while sizing rows to the viewport so long names elide instead of scrolling.
class FileColorGroupDelegate final : public QStyledItemDelegate
{
public:
    FileColorGroupDelegate(QAbstractItemDelegate* Base, QObject* Parent) : QStyledItemDelegate(Parent), base_(Base) { base_->setParent(this); }
    QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override { return {0, base_->sizeHint(Option, Index).height()}; }
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override { base_->paint(Painter, Option, Index); }
    bool editorEvent(QEvent* Event, QAbstractItemModel* Model, const QStyleOptionViewItem& Option, const QModelIndex& Index) override { return base_->editorEvent(Event, Model, Option, Index); }
private:
    QAbstractItemDelegate* base_;
};

class FileColorRulesPage final : public QWidget
{
public:
    explicit FileColorRulesPage(QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName("hc.settings.fileColors");
        // Layout: 261104_file_color_page_layout_plan.rst (design A). The header keeps the example action,
        // the editor and the preview share one vertical splitter so the preview spans the page width.
        auto* Layout = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("파일 그룹 · 색상"));
        Header->setSubtitle(tr("핸들을 끌어 우선순위를 바꿉니다. 위 그룹부터 적용하며, 빈 색·글꼴은 기본 표시를 따릅니다."));
        auto* Heading = new QHBoxLayout; Heading->addWidget(Header, 1); Layout->addLayout(Heading);
        const auto Button = [this](const QString& Text, QBoxLayout* Into, auto Action) {
            auto* Item = new fm::ui::Button(Text, this); Item->setCompact(true); Item->setAutoDefault(false); Into->addWidget(Item);
            connect(Item, &QPushButton::clicked, this, Action); return Item;
        };
        auto* Files = new QHBoxLayout; Files->setSpacing(6); Heading->addLayout(Files);
        // No "add example groups" action: a first installation already registers the default groups.
        // Rarely used file actions and the rule-wide pending colours live in one menu (plan 3.3).
        more_ = new QToolButton(this); more_->setObjectName("hc.rules.more"); more_->setText(tr("⋯")); more_->setAccessibleName(tr("가져오기·내보내기 등 더 보기"));
        more_->setToolTip(tr("가져오기 · 내보내기 · 기존 규칙 가져오기 · 확인 중 항목 색")); more_->setPopupMode(QToolButton::InstantPopup); Files->addWidget(more_);
        auto* More = new QMenu(more_); more_->setMenu(More);
        auto* Import = More->addAction(tr("가져오기…")); Import->setObjectName("hc.rules.import");
        connect(Import, &QAction::triggered, this, [this] {
            const auto Path = QFileDialog::getOpenFileName(this, tr("파일 색상 규칙 가져오기"), {}, "JSON (*.json)"); if (Path.isEmpty()) return;
            QFile File(Path); if (!File.open(QIODevice::ReadOnly) || File.size() > 1024 * 1024) { error_->setText(tr("1MB 이하 JSON 파일을 읽을 수 없습니다.")); return; }
            const auto Doc = QJsonDocument::fromJson(File.readAll()).object(); const auto Error = settings::FileColorRules::Validate(Doc, false);
            if (!Error.isEmpty()) { error_->setText(Error); return; } Load(Doc);
        });
        auto* Export = More->addAction(tr("내보내기…")); Export->setObjectName("hc.rules.export");
        connect(Export, &QAction::triggered, this, [this] {
            if (!Validate()) return;
            const auto Path = QFileDialog::getSaveFileName(this, tr("파일 색상 규칙 내보내기"), {}, "JSON (*.json)"); if (Path.isEmpty()) return;
            if (QFileInfo(Path).absoluteFilePath() == QFileInfo(settings::AppSettings().FileName()).absoluteFilePath()) { error_->setText(tr("사용 중인 설정 파일은 적용/확인으로 저장하세요.")); return; }
            QSaveFile File(Path); File.setDirectWriteFallback(false); auto Bytes = QJsonDocument(document_).toJson(); Bytes.replace("\n", "\r\n");
            if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) error_->setText(tr("규칙을 저장하지 못했습니다."));
        });
        auto* Legacy = More->addAction(tr("기존 규칙 가져오기")); Legacy->setObjectName("hc.rules.legacy"); connect(Legacy, &QAction::triggered, this, [this] { ImportLegacy(); });
        More->addSeparator();
        auto* PendingAction = More->addAction(tr("확인 중 항목 색…")); PendingAction->setObjectName("hc.rules.pendingAction"); connect(PendingAction, &QAction::triggered, this, [this] { showPendingPopover(); });
        collapseAction_ = More->addAction(tr("미리보기 접기")); collapseAction_->setObjectName("hc.rules.collapseAction"); collapseAction_->setCheckable(true);
        connect(collapseAction_, &QAction::toggled, this, [this](bool On) { setPreviewCollapsed(On); });

        splitter_ = new QSplitter(Qt::Vertical, this); splitter_->setObjectName("hc.rules.splitter"); splitter_->setChildrenCollapsible(false); splitter_->setHandleWidth(10);
        splitter_->setAccessibleName(tr("편집과 미리보기 크기 조절")); Layout->addWidget(splitter_, 1);
        auto* Top = new QWidget(splitter_); Top->setObjectName("hc.rules.top"); Top->setMinimumHeight(200); splitter_->addWidget(Top);
        // The group list and the editor share a horizontal splitter; the list keeps its width when the window grows.
        auto* Body = new QHBoxLayout(Top); Body->setContentsMargins(0, 0, 0, 0); Body->setSpacing(0);
        bodySplitter_ = new QSplitter(Qt::Horizontal, Top); bodySplitter_->setObjectName("hc.rules.bodySplitter"); bodySplitter_->setChildrenCollapsible(false); bodySplitter_->setHandleWidth(12);
        bodySplitter_->setAccessibleName(tr("그룹 목록과 그룹 편집 크기 조절")); Body->addWidget(bodySplitter_);
        auto* GroupsCard = new fm::ui::Card(tr("그룹"), bodySplitter_); GroupsCard->setObjectName("hc.rules.groupsCard"); GroupsCard->setMeta(tr("위에서부터 적용"));
        GroupsCard->setMinimumWidth(200); bodySplitter_->addWidget(GroupsCard);
        connect(bodySplitter_, &QSplitter::splitterMoved, this, [this] { const bool WasLoading = loading_; loading_ = true; updateGroupItems(); loading_ = WasLoading; });
        auto* GroupRows = new QVBoxLayout(GroupsCard); GroupRows->setContentsMargins(8, 8, 8, 10);
        // Compact icon actions live in the card header band.
        auto* ActionBox = new QWidget(GroupsCard); auto* Actions = new QHBoxLayout(ActionBox); Actions->setContentsMargins(0, 0, 0, 0); Actions->setSpacing(4);
        auto* NewGroup = Button(tr("추가"), Actions, [this] { addGroup(false); }); NewGroup->setAccessibleName(tr("새 그룹")); NewGroup->setObjectName("hc.rules.add");
        auto* AddShortcut = new QShortcut(QKeySequence(hc::defaults::AuxiliaryShortcut("fileColorGroup.add")), this); connect(AddShortcut, &QShortcut::activated, this, [this] { addGroup(false); });
        NewGroup->setToolTip(tr("새 그룹 · Alt+W"));
        Button(tr("복제"), Actions, [this] { addGroup(true); })->setObjectName("hc.rules.copy");
        Button(tr("삭제"), Actions, [this] { auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow(); if (Row < 0) return; Groups.removeAt(Row); document_["groups"] = Groups; refreshList(qMin(Row, int(Groups.size()) - 1)); })->setObjectName("hc.rules.delete");
        auto* Up = Button(tr("↑"), Actions, [this] { list_->MoveRow(list_->currentRow(), list_->currentRow() - 1); });
        Up->setObjectName("hc.rules.moveUp"); Up->setAccessibleName(tr("그룹 위로 이동")); Up->setToolTip(tr("위로 이동 · Alt+↑"));
        auto* Down = Button(tr("↓"), Actions, [this] { list_->MoveRow(list_->currentRow(), list_->currentRow() + 1); });
        Down->setObjectName("hc.rules.moveDown"); Down->setAccessibleName(tr("그룹 아래로 이동")); Down->setToolTip(tr("아래로 이동 · Alt+↓"));
        for (auto* Item : ActionBox->findChildren<fm::ui::Button*>()) { Item->setMinimumWidth(0); fm::style::setSizeVariant(Item, fm::style::SizeVariant::Mini); }
        GroupsCard->setHeaderWidget(ActionBox);
        list_ = new fm::ui::ReorderListWidget(this); list_->setObjectName("hc.rules.groups"); list_->setAccessibleName(tr("파일 그룹 우선순위")); GroupRows->addWidget(list_, 1);
        list_->setItemDelegate(new FileColorGroupDelegate(list_->itemDelegate(), list_)); list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        GroupRows->addWidget(new QLabel(tr("여러 그룹에 해당할 때"), GroupsCard));
        mode_ = new QButtonGroup(this); mode_->setObjectName("hc.rules.mode");
        // Two lines: a narrow card cannot hold both labels side by side.
        auto* ModeRow = new QVBoxLayout; ModeRow->setSpacing(2); GroupRows->addLayout(ModeRow);
        auto* First = new QRadioButton(tr("위 그룹 하나만"), GroupsCard); First->setObjectName("hc.rules.first"); mode_->addButton(First, 0); ModeRow->addWidget(First);
        auto* Merge = new QRadioButton(tr("속성별로 합치기"), GroupsCard); Merge->setObjectName("hc.rules.merge"); mode_->addButton(Merge, 1); ModeRow->addWidget(Merge);
        First->setToolTip(tr("처음 일치한 그룹을 적용합니다.")); Merge->setToolTip(tr("각 속성을 지정한 첫 그룹의 값을 사용합니다."));

        // Group editor: name and join on one line, then the condition and appearance tabs.
        auto* EditorCard = new fm::ui::Card(bodySplitter_); EditorCard->setObjectName("hc.rules.editorCard"); bodySplitter_->addWidget(EditorCard);
        bodySplitter_->setStretchFactor(0, 0); bodySplitter_->setStretchFactor(1, 1);
        editor_ = EditorCard;
        auto* Editor = new QVBoxLayout(EditorCard); Editor->setContentsMargins(14, 12, 14, 10); Editor->setSpacing(8);
        auto* NameRow = new QHBoxLayout; NameRow->setSpacing(8); Editor->addLayout(NameRow);
        name_ = new QLineEdit(EditorCard); name_->setObjectName("hc.rules.name"); name_->setMinimumWidth(80);
        // Each tab scrolls on its own, so neither tab's content raises the page's minimum size (plan 3.2).
        const auto Scrolled = [](QWidget* Content, const char* Name) {
            auto* Area = new QScrollArea; Area->setObjectName(Name); Area->setWidgetResizable(true); Area->setFrameShape(QFrame::NoFrame);
            Area->setWidget(Content); Content->setAutoFillBackground(false); Area->viewport()->setAutoFillBackground(false); return Area;
        };
        auto* NameLabel = new QLabel(tr("이름(&N)"), EditorCard); NameLabel->setBuddy(name_); NameRow->addWidget(NameLabel); NameRow->addWidget(name_, 1);
        NameRow->addSpacing(6); NameRow->addWidget(new QLabel(tr("다음 조건 중"), EditorCard));
        join_ = new QButtonGroup(this);
        auto* JoinButtons = new QHBoxLayout; JoinButtons->setSpacing(0); NameRow->addLayout(JoinButtons); QList<QAbstractButton*> Joins;
        for (int I = 0; I < 2; ++I) { auto* Choice = new QPushButton(I ? tr("모두 일치") : tr("일부 일치"), EditorCard); Choice->setCheckable(true); Choice->setAutoDefault(false); Choice->setObjectName(I ? "hc.rules.all" : "hc.rules.any"); join_->addButton(Choice, I); JoinButtons->addWidget(Choice); Joins.append(Choice);
            Choice->setToolTip(I ? tr("모든 조건이 맞으면 이 그룹을 적용합니다.") : tr("조건 중 하나라도 맞으면 이 그룹을 적용합니다.")); }
        fm::style::setSegments(Joins);
        editorTabs_ = new QTabWidget(EditorCard); editorTabs_->setObjectName("hc.rules.editorTabs"); editorTabs_->setDocumentMode(true); Editor->addWidget(editorTabs_, 1);
        auto* ConditionsPage = new QWidget; ConditionsPage->setObjectName("hc.rules.conditionsPage"); editorTabs_->addTab(Scrolled(ConditionsPage, "hc.rules.conditionsScroll"), tr("조건"));
        auto* Definition = new QVBoxLayout(ConditionsPage); Definition->setContentsMargins(0, 10, 0, 0); Definition->setSpacing(8);
        conditions_ = new QTableWidget(ConditionsPage); conditions_->setObjectName("hc.rules.conditions"); conditions_->setColumnCount(4); conditions_->verticalHeader()->hide();
        conditions_->setHorizontalHeaderLabels({tr("항목"), tr("비교 방법"), tr("값"), QString{}});
        conditions_->horizontalHeaderItem(3)->setData(Qt::AccessibleTextRole, tr("삭제"));
        conditions_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
        conditions_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); conditions_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed); conditions_->setColumnWidth(3, 36); conditions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        conditions_->setColumnWidth(0, qMax(150, QFontMetrics(conditions_->font()).horizontalAdvance(tr("파일명 정규화")) + 52)); conditions_->setColumnWidth(1, 124);
        conditions_->setShowGrid(false); conditions_->setSelectionBehavior(QAbstractItemView::SelectRows); conditions_->setSelectionMode(QAbstractItemView::NoSelection);
        Definition->addWidget(conditions_);
        // "Add condition" sits at the top right of the tabs, beside the condition tab it acts on.
        // The tab widget sets a corner widget's bottom on the tab line; a 2 px bottom margin keeps the button off it.
        auto* Corner = new QWidget(editorTabs_); Corner->setObjectName("hc.rules.tabCorner");
        auto* CornerLayout = new QHBoxLayout(Corner); CornerLayout->setContentsMargins(0, 0, 0, 2); CornerLayout->setSpacing(0);
        auto* AddCondition = new fm::ui::Button(tr("조건 추가(&D)"), Corner); AddCondition->setObjectName("hc.rules.addCondition"); AddCondition->setCompact(true); AddCondition->setAutoDefault(false);
        fm::style::setSizeVariant(AddCondition, fm::style::SizeVariant::Small); CornerLayout->addWidget(AddCondition); editorTabs_->setCornerWidget(Corner, Qt::TopRightCorner);
        connect(AddCondition, &QPushButton::clicked, this, [this] {
            if (conditionRows_.size() >= 32) return; editorTabs_->setCurrentIndex(0); ensureVersion2();
            appendCondition(settings::FileColorRules::NormalizeCondition({{"type", "extension"}, {"value", "txt"}}, 1)); storeForm();
        });
        // Design 8 B: what the slow conditions of this group cost, and what is shown meanwhile (full text in the tooltip).
        cost_ = new QLabel(ConditionsPage); cost_->setObjectName("hc.rules.cost"); cost_->hide(); cost_->setWordWrap(true); Definition->addWidget(cost_);
        auto* PatternHelp = new QLabel(tr("확장자는 마지막 . 뒤, 파일명은 확장자를 포함한 전체 이름입니다.\n와일드카드: 전체 일치(*, ?) · 정규표현식: 부분 일치(전체는 \\A…\\z)"), ConditionsPage);
        PatternHelp->setObjectName("hc.rules.patternHelp"); PatternHelp->setWordWrap(true); Definition->addWidget(PatternHelp); Definition->addStretch();
        conditions_->setAccessibleName(tr("그룹 조건")); conditions_->setToolTip(tr("확장자: txt, cpp · 무확장자: <none>\n파일명 패턴: CMakeLists.txt; *.cmake · 크기: 단위와 비교 방법 선택\n속성: 비트 값 또는 메뉴 선택 · 날짜: UTC 기준 · 확인되지 않은 날짜는 일치하지 않습니다."));

        auto* AppearancePage = new QWidget; AppearancePage->setObjectName("hc.rules.appearancePage"); editorTabs_->addTab(Scrolled(AppearancePage, "hc.rules.appearanceScroll"), tr("색상 · 글꼴"));
        auto* AppearanceGrid = new QGridLayout(AppearancePage); AppearanceGrid->setContentsMargins(0, 10, 0, 0); AppearanceGrid->setHorizontalSpacing(14); AppearanceGrid->setVerticalSpacing(6);
        auto* ColorColumn = new QVBoxLayout; ColorColumn->setSpacing(6); auto* FontColumn = new QVBoxLayout; FontColumn->setSpacing(6);
        AppearanceGrid->addLayout(ColorColumn, 0, 0); AppearanceGrid->addLayout(FontColumn, 0, 1);
        // Both columns keep their natural width; a wider window only widens the empty column on the right.
        AppearanceGrid->setColumnStretch(2, 1); AppearanceGrid->setRowStretch(1, 1);
        auto* Colors = new QGridLayout; Colors->setHorizontalSpacing(8); Colors->setVerticalSpacing(6); ColorColumn->addLayout(Colors);
        Colors->addWidget(new QLabel(tr("라이트 테마"), AppearancePage), 0, 1, Qt::AlignHCenter); Colors->addWidget(new QLabel(tr("다크 테마"), AppearancePage), 0, 2, Qt::AlignHCenter);
        for (int I = 0; I < 4; ++I) {
            colors_[I] = new fm::ui::ColorPickButton(AppearancePage); colors_[I]->setAllowNone(true); colors_[I]->setAutoDefault(false);
            colors_[I]->setObjectName("hc.rules.color." + QString::number(I));
            colors_[I]->setAccessibleName((I % 2 ? tr("다크 ") : tr("라이트 ")) + (I / 2 ? tr("배경색") : tr("글자색")));
            fitColorButton(colors_[I]); Colors->addWidget(colors_[I], I / 2 + 1, I % 2 + 1);
            connect(colors_[I], &fm::ui::ColorPickButton::colorChanged, this, [this] { storeForm(); });
        }
        Colors->addWidget(new QLabel(tr("글자색"), AppearancePage), 1, 0); Colors->addWidget(new QLabel(tr("배경색"), AppearancePage), 2, 0);
        Colors->setColumnStretch(3, 1);
        autoDark_ = new QCheckBox(tr("라이트 색에서 다크 색 자동 계산"), AppearancePage); autoDark_->setObjectName("hc.rules.autoDark"); ColorColumn->addWidget(autoDark_);
        autoDark_->setToolTip(tr("색조·채도·투명도를 유지하고 밝기를 다크용으로 조정합니다. 끄면 저장된 수동 다크 색으로 돌아갑니다."));
        connect(autoDark_, &QCheckBox::toggled, this, [this] { if (loading_) return; ensureVersion2(); storeForm(); refreshColors(); });
        for (int I = 0; I < 2; ++I) { contrast_[I] = new QLabel(AppearancePage); contrast_[I]->setObjectName("hc.rules.contrast." + QString::number(I)); contrast_[I]->setWordWrap(true); ColorColumn->addWidget(contrast_[I]); }
        auto* FontRow = new QGridLayout; FontRow->setHorizontalSpacing(6); FontRow->setVerticalSpacing(6); FontColumn->addLayout(FontRow);
        const QStringList FontNames{tr("굵게"), tr("기울임"), tr("밑줄"), tr("취소선"), tr("흐려짐")};
        for (int I = 0; I < fontKeys_.size(); ++I) {
            fonts_[I] = new FileFontEffectButton(FontNames[I], AppearancePage); fonts_[I]->setObjectName("hc.rules.font." + fontKeys_[I]); FontRow->addWidget(fonts_[I], I / 2, I % 2);
            connect(fonts_[I], &QPushButton::clicked, this, [this, I] { fonts_[I]->SetState((fonts_[I]->State() + 1) % 3); storeForm(); });
        }
        // One width for the five effect buttons (their widest "· 기본" label), never stretched by the window.
        int FontWidth = 0; for (auto* Effect : fonts_) FontWidth = qMax(FontWidth, Effect->sizeHint().width());
        for (auto* Effect : fonts_) Effect->setFixedWidth(FontWidth);
        FontRow->setColumnStretch(2, 1);
        border_ = new QComboBox(AppearancePage); border_->setObjectName("hc.rules.border"); border_->setAccessibleName(tr("파일 행 전체 테두리"));
        const QStringList BorderNames{tr("기본값"), tr("없음"), tr("실선"), tr("파선"), tr("점선"), tr("일점쇄선"), tr("이점쇄선")};
        const QStringList BorderKeys{"", "none", "solid", "dash", "dot", "dashDot", "dashDotDot"};
        for (int I = 0; I < BorderNames.size(); ++I) border_->addItem(BorderNames[I], BorderKeys[I]);
        border_->setToolTip(tr("파일 행 전체에 테두리를 표시합니다. 색상은 파일의 글자색을 따릅니다. 기본값은 아래 그룹을 따르고 없음은 테두리를 해제합니다."));
        // Its own line under the effects, as wide as one effect button, so it never widens the button columns.
        border_->setFixedWidth(FontWidth);
        auto* BorderRow = new QHBoxLayout; BorderRow->addWidget(new QLabel(tr("행 테두리"), AppearancePage)); BorderRow->addWidget(border_); BorderRow->addStretch(); FontRow->addLayout(BorderRow, 3, 0, 1, 3);
        connect(border_, &QComboBox::currentIndexChanged, this, [this] { storeForm(); });
        auto* DefaultHelp = new QLabel(tr("지정하지 않은 색·글꼴은 아래 그룹 또는 목록 기본값을 따릅니다."), AppearancePage); DefaultHelp->setWordWrap(true); FontColumn->addWidget(DefaultHelp);
        ColorColumn->addStretch(); FontColumn->addStretch();

        // Preview: full page width under the splitter handle (plan 3.1).
        previewCard_ = new fm::ui::Card(tr("미리보기"), splitter_); previewCard_->setObjectName("hc.rules.previewCard"); previewCard_->setMinimumHeight(120); splitter_->addWidget(previewCard_);
        // The editor takes what the window gains; the preview keeps its rows (see applyInitialSplit).
        splitter_->setStretchFactor(0, 1); splitter_->setStretchFactor(1, 0);
        auto* PreviewTools = new QWidget(previewCard_); auto* Tools = new QHBoxLayout(PreviewTools); Tools->setContentsMargins(0, 0, 0, 0); Tools->setSpacing(6);
        source_ = new QButtonGroup(this); auto* SourceRow = new QHBoxLayout; SourceRow->setSpacing(0); Tools->addLayout(SourceRow); QList<QAbstractButton*> Sources;
        const QStringList SourceNames{tr("예제"), tr("현재 폴더"), tr("선택 항목")};
        for (int I = 0; I < SourceNames.size(); ++I) {
            auto* Choice = new QPushButton(SourceNames[I], PreviewTools); Choice->setCheckable(true); Choice->setAutoDefault(false); Choice->setObjectName("hc.rules.source." + QString::number(I));
            fm::style::setSizeVariant(Choice, fm::style::SizeVariant::Small); source_->addButton(Choice, I); SourceRow->addWidget(Choice); Sources.append(Choice);
        }
        fm::style::setSegments(Sources); source_->button(0)->setChecked(true);
        source_->button(1)->setToolTip(tr("설정 창을 연 패널의 현재 폴더에서 최대 50개를 씁니다.")); source_->button(2)->setToolTip(tr("설정 창을 연 패널에서 선택한 항목을 씁니다."));
        connect(source_, &QButtonGroup::idClicked, this, [this] { stopComputation(); updatePreview(); });
        // Every tool in the preview header is the same small button as the source segments, one height for all.
        const auto Tool = [PreviewTools](const QString& Text, const char* Name) {
            auto* Item = new fm::ui::Button(Text, PreviewTools); Item->setObjectName(Name); Item->setCompact(true); Item->setAutoDefault(false);
            fm::style::setSizeVariant(Item, fm::style::SizeVariant::Small); return Item;
        };
        compute_ = Tool(tr("값 계산"), "hc.rules.compute");
        compute_->setToolTip(tr("표본의 느린 필드·플러그인 필드 값을 읽습니다. 네트워크 위치는 「설정 → 파일 패널」의 범위를 따릅니다.")); Tools->addWidget(compute_);
        connect(compute_, &QPushButton::clicked, this, [this] { computeSampleFields(); });
        sampleStatus_ = new QLabel(PreviewTools); sampleStatus_->setObjectName("hc.rules.sampleStatus"); sampleStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Tools->addWidget(sampleStatus_, 1);
        pendingLegend_ = Tool(tr("확인 중"), "hc.rules.pendingLegend");
        pendingLegend_->setAccessibleName(tr("확인 중 항목 색")); pendingLegend_->setToolTip(tr("필드 값을 확인하기 전인 행의 색입니다. 눌러서 바꿉니다.")); Tools->addWidget(pendingLegend_);
        connect(pendingLegend_, &QPushButton::clicked, this, [this] { showPendingPopover(); });
        previewDark_ = new QCheckBox(tr("다크"), PreviewTools); previewDark_->setObjectName("hc.rules.previewDark"); previewDark_->setChecked(fm::style::isDarkVariant(fm::style::ThemeManager::instance().effectiveVariant())); Tools->addWidget(previewDark_);
        auto* CustomToggle = Tool(tr("직접 입력"), "hc.rules.customToggle"); CustomToggle->setCheckable(true);
        CustomToggle->setToolTip(tr("파일 이름·크기·필드 값을 직접 정해 결과를 확인합니다.")); Tools->addWidget(CustomToggle);
        previewCollapse_ = Tool(tr("접기"), "hc.rules.previewCollapse"); previewCollapse_->setCheckable(true);
        previewCollapse_->setAccessibleName(tr("미리보기 접기")); previewCollapse_->setToolTip(tr("미리보기 접기")); Tools->addWidget(previewCollapse_);
        connect(previewCollapse_, &QPushButton::toggled, this, [this](bool On) { setPreviewCollapsed(On); });
        const int ToolHeight = source_->button(0)->sizeHint().height();
        for (auto* Item : {compute_, pendingLegend_, CustomToggle, previewCollapse_}) Item->setFixedHeight(ToolHeight);
        previewCard_->setHeaderWidget(PreviewTools);
        auto* PreviewRows = new QVBoxLayout(previewCard_); PreviewRows->setContentsMargins(10, 8, 10, 10);
        previewBody_ = new QWidget(previewCard_); auto* BodyRow = new QHBoxLayout(previewBody_); BodyRow->setContentsMargins(0, 0, 0, 0); BodyRow->setSpacing(10); PreviewRows->addWidget(previewBody_, 1);
        previewTable_ = new QTableWidget(6, 3, previewBody_); previewTable_->setObjectName("hc.rules.previewTable"); previewTable_->setAccessibleName(tr("표본 파일과 적용 그룹"));
        previewTable_->setHorizontalHeaderLabels({tr("이름"), tr("크기"), tr("적용된 그룹")}); previewTable_->verticalHeader()->hide(); previewTable_->setShowGrid(false);
        previewTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
        previewTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); previewTable_->setSelectionBehavior(QAbstractItemView::SelectRows); previewTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        previewTable_->setToolTip(tr("행을 선택하면 선택색을 확인할 수 있습니다. 예제 기준: 2026-01-15 12:00 UTC"));
        previewTable_->setItemDelegate(new FileRulePreviewDelegate([this](int Row, bool Selected, bool Name) { return recordPair(previewDark_->isChecked(), previewStyles_.value(Row), Selected, Name); },
            [this](int Row) { return previewStyles_.value(Row).font_; }, previewTable_));
        BodyRow->addWidget(previewTable_, 1);
        // Custom sample drawer (plan 7): the form scrolls inside a fixed-width panel beside the table.
        customPanel_ = new QScrollArea(previewBody_); customPanel_->setObjectName("hc.rules.customPanel"); customPanel_->setWidgetResizable(true); customPanel_->setFrameShape(QFrame::NoFrame);
        customPanel_->setFixedWidth(300); customPanel_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); customPanel_->hide(); BodyRow->addWidget(customPanel_);
        auto* Custom = new QWidget; customPanel_->setWidget(Custom); auto* CustomLayout = new QVBoxLayout(Custom); CustomLayout->setContentsMargins(0, 0, 6, 0); CustomLayout->setSpacing(6);
        connect(CustomToggle, &QPushButton::toggled, this, [this](bool Open) { customPanel_->setVisible(Open); });
        auto* PreviewRow = new QHBoxLayout; CustomLayout->addLayout(PreviewRow);
        previewName_ = new QLineEdit("sample.txt", Custom); previewName_->setAccessibleName(tr("미리보기 파일 이름")); previewName_->setObjectName("hc.rules.sample"); PreviewRow->addWidget(previewName_, 1);
        previewFolder_ = new QCheckBox(tr("폴더"), Custom); PreviewRow->addWidget(previewFolder_);
        previewSelected_ = new QCheckBox(tr("선택"), Custom); previewSelected_->setObjectName("hc.rules.sampleSelected"); PreviewRow->addWidget(previewSelected_);
        connect(previewSelected_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        auto* SampleForm = new QFormLayout; SampleForm->setRowWrapPolicy(QFormLayout::WrapLongRows); SampleForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); CustomLayout->addLayout(SampleForm);
        previewSize_ = new QLineEdit("0", Custom); previewSize_->setObjectName("hc.rules.sampleSize"); previewSize_->setMaxLength(20); SampleForm->addRow(tr("크기 · 바이트"), previewSize_);
        previewAttributes_ = new QLineEdit("0", Custom); previewAttributes_->setObjectName("hc.rules.sampleAttributes"); previewAttributes_->setMaxLength(10); SampleForm->addRow(tr("속성 · 비트 마스크"), previewAttributes_);
        auto* DateSample = new QWidget(Custom); auto* DateFields = new QHBoxLayout(DateSample); DateFields->setContentsMargins(0, 0, 0, 0);
        previewModifiedKnown_ = new QCheckBox(tr("확인됨"), DateSample); previewModifiedKnown_->setObjectName("hc.rules.sampleModifiedKnown"); DateFields->addWidget(previewModifiedKnown_);
        previewModified_ = new QDateTimeEdit(QDateTime::currentDateTimeUtc(), DateSample); previewModified_->setObjectName("hc.rules.sampleModified"); previewModified_->setTimeZone(QTimeZone::UTC); previewModified_->setDisplayFormat("yyyy-MM-dd HH:mm 'UTC'"); previewModified_->setEnabled(false); DateFields->addWidget(previewModified_, 1); SampleForm->addRow(tr("수정 시각"), DateSample);
        DateSample->setToolTip(tr("수정 시각을 확인할 수 없으면 날짜 조건에 일치하지 않습니다. 최근 기간은 현재 UTC 시각을 기준으로 판정합니다."));
        previewLink_ = new QComboBox(Custom); previewLink_->setObjectName("hc.rules.sampleLink"); previewLink_->addItems({tr("파일명·속성 사용"), tr("심볼릭 링크"), tr("정션"), tr("종류를 모르는 재분석 지점")}); SampleForm->addRow(tr("링크 종류"), previewLink_);
        // Field sample values: one row per field the rules use (plan 7); the rows write the "field=value;" text.
        sampleFieldsBox_ = new QWidget(Custom); sampleFieldsBox_->setObjectName("hc.rules.sampleFieldRows");
        sampleFieldRows_ = new QGridLayout(sampleFieldsBox_); sampleFieldRows_->setContentsMargins(0, 4, 0, 0); sampleFieldRows_->setHorizontalSpacing(6); sampleFieldRows_->setVerticalSpacing(4);
        CustomLayout->addWidget(sampleFieldsBox_);
        previewFields_ = new QLineEdit(Custom); previewFields_->setObjectName("hc.rules.sampleFields"); previewFields_->setMaxLength(4096); previewFields_->hide();
        previewFields_->setToolTip(tr("필드 조건에 쓸 표본 값입니다. 필드=값을 ;로 구분합니다.\n적지 않은 필드는 확인 중, 값 자리에 -를 적으면 빈 값, !를 적으면 오류로 봅니다."));
        connect(previewFields_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
        connect(previewModifiedKnown_, &QCheckBox::toggled, this, [this](bool Known) { previewModified_->setEnabled(Known); updatePreview(); });
        connect(previewModified_, &QDateTimeEdit::dateTimeChanged, this, [this] { updatePreview(); });
        connect(previewLink_, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
        preview_ = new FileRulePreviewLabel(Custom); preview_->setTextFormat(Qt::PlainText); preview_->setAutoFillBackground(true); preview_->setMinimumHeight(34); CustomLayout->addWidget(preview_);
        owners_ = new QLabel(Custom); owners_->setTextFormat(Qt::PlainText); owners_->setWordWrap(true); CustomLayout->addWidget(owners_);
        CustomLayout->addStretch();

        // Design 6.4: rows whose field values are still being read. A popover of the page, built up front so
        // the settings search and accessibility see its controls.
        pendingPopover_ = new QFrame(this, Qt::Popup); pendingPopover_->setObjectName("hc.rules.pendingPopover"); fm::style::setCard(pendingPopover_);
        pendingPopover_->setAccessibleName(tr("확인 중 항목 색"));
        auto* PendingRows = new QVBoxLayout(pendingPopover_); PendingRows->setContentsMargins(14, 12, 14, 12); PendingRows->setSpacing(8);
        auto* PendingTitle = new QHBoxLayout; PendingRows->addLayout(PendingTitle);
        auto* PendingLabel = new QLabel(tr("확인 중 항목 색"), pendingPopover_); auto TitleFont = PendingLabel->font(); TitleFont.setBold(true); PendingLabel->setFont(TitleFont); PendingTitle->addWidget(PendingLabel, 1);
        auto* PendingReset = new QPushButton(tr("기본값으로"), pendingPopover_); PendingReset->setObjectName("hc.rules.pendingReset"); PendingReset->setAutoDefault(false);
        fm::style::setButtonRole(PendingReset, fm::style::ButtonRole::Link); PendingTitle->addWidget(PendingReset);
        auto* PendingGrid = new QGridLayout; PendingGrid->setHorizontalSpacing(8); PendingRows->addLayout(PendingGrid);
        PendingGrid->addWidget(new QLabel(tr("라이트 테마"), pendingPopover_), 0, 1, Qt::AlignHCenter); PendingGrid->addWidget(new QLabel(tr("다크 테마"), pendingPopover_), 0, 2, Qt::AlignHCenter);
        PendingGrid->addWidget(new QLabel(tr("배경색"), pendingPopover_), 1, 0); PendingGrid->addWidget(new QLabel(tr("표시선"), pendingPopover_), 2, 0);
        for (int I = 0; I < 4; ++I) {
            pending_[I] = new fm::ui::ColorPickButton(pendingPopover_); pending_[I]->setAllowNone(true); pending_[I]->setAutoDefault(false);
            pending_[I]->setObjectName("hc.rules.pending." + QString::number(I));
            pending_[I]->setAccessibleName((I % 2 ? tr("다크 ") : tr("라이트 ")) + (I / 2 ? tr("확인 중 표시선") : tr("확인 중 배경색")));
            fitColorButton(pending_[I]); PendingGrid->addWidget(pending_[I], I / 2 + 1, I % 2 + 1);
            connect(pending_[I], &fm::ui::ColorPickButton::colorChanged, this, [this] { storePending(); });
        }
        connect(PendingReset, &QPushButton::clicked, this, [this] { for (auto* Pick : pending_) { const QSignalBlocker Block(Pick); Pick->setColor(std::nullopt); } storePending(); });
        auto* PendingHelp = new QLabel(tr("필드 조건(ADS·이미지·버전·링크·플러그인 필드 등)의 값을 확인하기 전인 행에 씁니다. 모든 그룹에 공통이며, 비워 두면 기본색을 씁니다."), pendingPopover_);
        PendingHelp->setWordWrap(true); PendingHelp->setFixedWidth(300); PendingRows->addWidget(PendingHelp);
        error_ = new QLabel(this); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); Layout->addWidget(error_);
        previewEngine_ = std::make_unique<hc::columns::QueryEngine>(hc::columns::PluginFields::Instance().Loader(true), this);
        previewEngine_->Changed = [this](const QSet<QString>& Paths) { sampleValuesArrived(Paths); };
        setMinimumSize(580, 380);
        connect(list_->model(), &QAbstractItemModel::rowsAboutToBeMoved, this, [this] { loading_ = true; });
        connect(list_->model(), &QAbstractItemModel::rowsMoved, this, [this] {
            // Keep complete group records, including unknown fields. A row move
            // changes priority only; the selected group's editor follows its ID.
            QHash<QString, QJsonObject> Records;
            for (const auto Value : document_["groups"].toArray()) Records.insert(Value.toObject()["id"].toString(), Value.toObject());
            QJsonArray Groups;
            for (int Row = 0; Row < list_->count(); ++Row) Groups.append(Records.value(list_->item(Row)->data(Qt::UserRole).toString()));
            document_["groups"] = Groups; updateGroupItems(); loading_ = false; loadForm();
        });
        connect(list_, &QListWidget::currentRowChanged, this, [this] { loadForm(); });
        connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* Item) { if (loading_) return; auto Groups = document_["groups"].toArray(); auto G = Groups[list_->row(Item)].toObject(); G["enabled"] = Item->checkState() == Qt::Checked; Groups[list_->row(Item)] = G; document_["groups"] = Groups; updatePreview(); });
        connect(mode_, &QButtonGroup::idClicked, this, [this](int Index) { if (loading_) return; document_["mode"] = Index ? "merge" : "first"; updatePreview(); });
        connect(name_, &QLineEdit::textChanged, this, [this] { storeForm(); });
        connect(join_, &QButtonGroup::idClicked, this, [this] { storeForm(); });
        connect(previewSize_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
        connect(previewAttributes_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
        connect(previewName_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
        connect(previewFolder_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        connect(previewDark_, &QCheckBox::toggled, this, [this] { refreshPending(); updatePreview(); });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { const bool WasLoading = loading_; loading_ = true; updateGroupItems(); loading_ = WasLoading; refreshPending(); updatePreview(); });
        previewTimer_.setSingleShot(true); previewTimer_.setInterval(150);
        connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
        source_->button(1)->setEnabled(false); source_->button(2)->setEnabled(false);
        Load(settings::FileColorRules::Empty());
    }
    void Load(const QJsonObject& Document) { document_ = Document.isEmpty() ? settings::FileColorRules::Empty() : Document; supported_ = settings::FileColorRules::Supported(document_); setEnabled(supported_); refreshList(0); if (!supported_) error_->setText(tr("다른 버전의 파일 색상 규칙은 원본 그대로 보존합니다.")); }
    // Panel rows for the preview's "current folder" and "selected items" sources (layout plan L4).
    void SetSamples(QVector<settings::FileRuleSample> Folder, QVector<settings::FileRuleSample> Selection, const QString& FolderName) {
        stopComputation(); computed_.clear();
        folderSamples_ = std::move(Folder); selectionSamples_ = std::move(Selection); sampleFolder_ = FolderName;
        source_->button(1)->setEnabled(!folderSamples_.isEmpty()); source_->button(2)->setEnabled(!selectionSamples_.isEmpty());
        if (!source_->checkedButton() || !source_->checkedButton()->isEnabled()) source_->button(0)->setChecked(true);
        updatePreview();
    }
protected:
    void showEvent(QShowEvent* Event) override {
        QWidget::showEvent(Event);
        if (!splitApplied_) QTimer::singleShot(0, this, [this] { applyInitialSplit(); });
    }
    void hideEvent(QHideEvent* Event) override { stopComputation(); QWidget::hideEvent(Event); }
    // The first show may come before the layout has a height; the first real size settles the split.
    void resizeEvent(QResizeEvent* Event) override { QWidget::resizeEvent(Event); if (!splitApplied_ && isVisible()) QTimer::singleShot(0, this, [this] { applyInitialSplit(); }); }
public:
    void ImportLegacy() {
        const auto Files = settings::AppSettings().Read(settings::Section::FileSets).toArray();
        const auto Appearance = settings::AppSettings().Read(settings::Section::Appearance).toObject();
        const auto Schemes = Appearance["schemes"].toObject();
        auto Groups = document_["groups"].toArray(); QSet<QString> Ids, Names;
        for (const auto V : Groups) { Ids.insert(V.toObject()["id"].toString()); Names.insert(V.toObject()["name"].toString().toCaseFolded()); }
        if (!document_.contains("legacyBackup")) document_["legacyBackup"] = QJsonObject{{"fileSets", Files}, {"appearance", Appearance}};
        for (const auto V : Files) {
            const auto F = V.toObject(); const auto Name = F["name"].toString(); if (Name.isEmpty()) continue;
            const auto Id = "legacy." + QString::fromLatin1(QCryptographicHash::hash(Name.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
            if (Ids.contains(Id)) continue; const auto Flags = F["flags"].toInteger(); QJsonArray Conditions;
            const auto Add = [&](const QString& Type, const QString& Value) { Conditions.append(QJsonObject{{"type", Type}, {"value", Value}}); };
            bool Review = false;
            if (Flags & FILE_SET_NOR_EXT_FILTERS) { QStringList Extensions; for (const auto E : F["extensions"].toArray()) Extensions.append(E.toString().isEmpty() ? "<none>" : E.toString()); if (!Extensions.isEmpty()) Add("extension", Extensions.join(", ")); else Review = true; }
            else if (Flags & FILE_SET_NOR_FILTERS) { for (const auto P : F["filters"].toArray()) if (!P.toString().isEmpty()) Add("pattern", P.toString()); Review = true; }
            if (Flags & FILE_SET_EXP_ATTRIBUTE) Add("attributes", QString::number(F["attributes"].toInteger() & 0x4837));
            if (Flags & (FILE_SET_EXP_SIZE_EQUAL | FILE_SET_EXP_SIZE_LESS | FILE_SET_EXP_SIZE_MORE)) Review = true;
            if (Conditions.isEmpty()) Add("pattern", "*");
            QString Label = Name; int N = 1; while (Names.contains(Label.toCaseFolded())) Label = Name + tr(" (기존 %1)").arg(N++); Names.insert(Label.toCaseFolded());
            if (document_["version"].toInt() >= 2) { QJsonArray Upgraded; for (const auto C : Conditions) Upgraded.append(settings::FileColorRules::NormalizeCondition(C.toObject(), 1)); Conditions = Upgraded; }
            QJsonObject G{{"id", Id}, {"name", Label}, {"enabled", !Review}, {"join", "all"}, {"conditions", Conditions}, {"legacySource", F}};
            for (const auto& Mode : {QString("light"), QString("dark")}) {
                const auto Default = Mode == "dark" ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트");
                const auto Selected = Schemes[Appearance["selected"].toString()].toObject(); const auto Scheme = Selected["dark"].toBool() == (Mode == "dark") ? Selected : Schemes[Default].toObject();
                const auto Mapping = Scheme["fileSets"].toObject()[Name].toObject(); QJsonObject Colors;
                for (const auto& Key : {"foreground", "background"}) if (QColor(Mapping[Key].toString()).isValid()) Colors[Key] = Mapping[Key]; G[Mode] = Colors;
            }
            Groups.append(G); Ids.insert(Id);
        }
        document_["groups"] = Groups; refreshList(0);
        error_->setText(tr("기존 파일집합과 색 구성표 원본을 초안에 백업했습니다. 적용 전 순서와 조건을 확인하세요. 기존 엔진에서 미지원이던 파일명·크기 조건은 비활성으로 이관합니다."));
    }
    QJsonObject Document() const { return document_; }
    // Built-in fields plus the plugin fields of this installation (PluginFields::Fields()).
    void SetFieldCatalog(QVector<hc::columns::FieldDefinition> Catalog) {
        catalog_ = std::move(Catalog); const int Row = list_->currentRow();
        loading_ = true; updateGroupItems(); loading_ = false; list_->setCurrentRow(Row); loadForm();
    }
    bool Validate() {
        if (!supported_) return true;
        if (settings::FileColorRules::HasRegex(document_)) {
            if (validatedDocument_ != document_ || validationPending_) { error_->setText(tr("조건을 검증하고 있습니다. 잠시 후 적용하세요.")); return false; }
            error_->setText(validationError_); return validationError_.isEmpty();
        }
        const auto Error = settings::FileColorRules::Validate(document_); error_->setText(Error); return Error.isEmpty();
    }
    bool IsValidationPending() const { return validationPending_; }
private:
    void ensureVersion2() {
        if (document_["version"] == QJsonValue(1)) { document_ = settings::FileColorRules::Upgrade(document_); for (auto* Row : conditionRows_) Row->Upgrade(); }
    }
    void appendCondition(const QJsonObject& Condition) {
        auto* Row = new FileColorConditionRow(conditions_, Condition, document_["version"].toInt(), catalog_); conditionRows_.append(Row);
        Row->onEdited_ = [this] { if (loading_) return; ensureVersion2(); storeForm(); };
        Row->onRemove_ = [this, Row] { if (loading_ || Row->Row() < 0) return; ensureVersion2(); conditions_->removeRow(Row->Row()); conditionRows_.removeAll(Row); Row->deleteLater(); storeForm(); };
    }
    void updateConditionHeight() {
        int Height = conditions_->horizontalHeader()->sizeHint().height() + 2 * conditions_->frameWidth();
        if (!conditions_->rowCount()) Height += conditions_->verticalHeader()->defaultSectionSize();
        int First = Height + (conditions_->rowCount() ? conditions_->rowHeight(0) : 0);
        for (int I = 0; I < qMin(conditions_->rowCount(), 3); ++I) Height += conditions_->rowHeight(I);
        // Up to three rows when the editor has room; on a short screen it gives way down to one row and scrolls.
        conditions_->setMinimumHeight(First); conditions_->setMaximumHeight(Height);
    }
    void updateGroupItems() {
        const auto Groups = document_["groups"].toArray();
        const QStringList Types{tr("확장자"), tr("파일명"), tr("파일/폴더"), tr("속성"), tr("크기"), tr("수정 날짜"), tr("링크"), tr("파일명 정규화"), tr("RTL 문자")};
        const bool Dark = fm::style::isDarkVariant(fm::style::ThemeManager::instance().effectiveVariant());
        for (int Row = 0; Row < list_->count(); ++Row) {
            const auto G = Groups[Row].toObject(); QStringList Conditions;
            for (const auto Value : G["conditions"].toArray()) {
                const auto C = settings::FileColorRules::NormalizeCondition(Value.toObject(), document_["version"].toInt()); QString Text = C["value"].toString();
                if (C["type"] == "normalization") Text = C["value"] == "decomposed" ? tr("자소 분리 있음 (NFD·혼합)") : tr("분리 없음 (NFC)");
                if (C["type"] == "rtl") Text = C["op"] == "notContains" ? tr("비포함") : tr("포함");
                if (C["value"].isArray()) { QStringList Parts; for (const auto P : C["value"].toArray()) Parts.append(P.toString()); Text = Parts.join("; "); }
                const QMap<QString, QString> Comparisons{{"gt", ">"}, {"ge", "≥"}, {"lt", "<"}, {"le", "≤"}, {"eq", "="}, {"ne", "≠"}, {"any", tr("하나라도")}, {"all", tr("모두")}, {"within", tr("최근")}, {"before", tr("이전")}, {"after", tr("이후")}, {"notMatches", tr("불일치")}, {"notIn", tr("제외")},
                    {"between", tr("범위")}, {"exists", tr("값 있음")}, {"notExists", tr("값 없음")}};
                QString Label = Types.value(typeKeys_.indexOf(C["type"].toString()));
                if (C["type"] == "field") {
                    const auto Id = C["field"].toString();
                    const auto Known = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& F) { return F.id_ == Id; });
                    Label = Known != catalog_.end() ? Known->title_ : C["fieldTitle"].toString(Id);
                    if (C["op"] == "contains") Label += tr(" 포함"); else if (C["op"] == "notContains") Label += tr(" 포함 아님");
                    else if (C["op"] == "in") Label += tr(" 중 하나"); else if (C["op"] == "matches") Label += C["syntax"] == "regex" ? tr(" 정규식") : tr(" 와일드카드");
                    else if (C["op"] == "is") Text = C["value"] == "true" ? tr("예") : tr("아니요");
                    if (Known != catalog_.end() && C["op"] == "in") { QStringList Titles; const auto Values = C["value"].toArray();
                        for (const auto& Choice : Known->choices_) if (Values.contains(Choice.value_)) Titles.append(Choice.title_);
                        if (!Titles.isEmpty()) { Conditions.append(Label + ": " + Titles.join(", ")); continue; } }
                }
                if (C["type"] == "extension" || C["type"] == "pattern") {
                    const auto Syntax = settings::FileColorRules::Syntax(C, document_["version"].toInt());
                    if (Syntax == "regex") Label += tr(" 정규식");
                    else if (Syntax == "wildcard") Label += tr(" 와일드카드");
                    if (C["op"] == "contains") Label += tr(" 포함");
                    else if (C["op"] == "notContains") Label += tr(" 포함 아님");
                }
                if (Comparisons.contains(C["op"].toString())) Label += " " + Comparisons[C["op"].toString()];
                if (C["unit"].isString()) { const QMap<QString, QString> Units{{"hours", tr("시간")}, {"days", tr("일")}, {"minutes", tr("분")}}; Text += " " + Units.value(C["unit"].toString(), C["unit"].toString()); }
                Conditions.append(Label + ": " + Text);
            }
            const auto Summary = Conditions.join(G["join"] == "all" ? tr(" · 모두 · ") : tr(" · 또는 · "));
            const auto [Slow, Plugin] = fieldCosts(G);
            auto* Item = list_->item(Row); const auto Title = QString::number(Row + 1) + ". " + G["name"].toString() + (G["example"].toBool() ? tr(" · 예제") : QString{})
                + (Plugin ? tr(" · 플러그인") : Slow ? tr(" · 느림") : QString{});
            Item->setText(Title + "\n" + list_->fontMetrics().elidedText(Summary, Qt::ElideRight, qMax(80, list_->viewport()->width() - 96)));
            Item->setToolTip(Title + "\n" + Summary); Item->setData(Qt::AccessibleTextRole, Title + " " + Summary);
            QPixmap Swatch(QSize(32, 24) * devicePixelRatioF()); Swatch.setDevicePixelRatio(devicePixelRatioF());
            const auto Colors = settings::FileColorRules::Colors(G, Dark); settings::FileRuleResult Style; Style.foreground_ = QColor(Colors["foreground"].toString()); Style.background_ = QColor(Colors["background"].toString()); Style.font_ = G["font"].toObject();
            const auto Pair = recordPair(Dark, Style, false); Swatch.fill(Pair.second);
            QPainter Painter(&Swatch); Painter.setPen(Pair.first);
            const auto Font = Style.Font(font());
            Painter.setFont(Font); if (Style.font_["dim"].toBool()) Painter.setOpacity(0.55);
            Painter.drawText(QRect(0, 0, 32, 24), Qt::AlignCenter, "Aa");
            PaintFileRuleBorder(&Painter, QRect(0, 0, 32, 24), Style.foreground_.isValid() ? Style.foreground_ : palette().color(QPalette::Text), Style.font_);
            Painter.end(); Item->setIcon(Swatch);
        }
        list_->setIconSize(QSize(32, 24));
    }
    void refreshList(int Row) {
        loading_ = true; list_->clear(); mode_->button(document_["mode"] == "merge" ? 1 : 0)->setChecked(true);
        for (const auto V : document_["groups"].toArray()) { auto* Item = new QListWidgetItem(V.toObject()["name"].toString(), list_); Item->setData(Qt::UserRole, V.toObject()["id"].toString()); Item->setCheckState(V.toObject()["enabled"].toBool(hc::defaults::DefaultValue("fileColorGroup/enabled").toBool()) ? Qt::Checked : Qt::Unchecked); }
        updateGroupItems(); list_->setCurrentRow(Row); loading_ = false; loadForm();
    }
    void loadForm() {
        if (loading_) return; loading_ = true;
        const auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow(); const bool Valid = Row >= 0 && Row < Groups.size(); editor_->setEnabled(Valid);
        const auto G = Valid ? Groups[Row].toObject() : QJsonObject{}; name_->setText(G["name"].toString()); join_->button(G["join"] == "all" ? 1 : 0)->setChecked(true);
        qDeleteAll(conditionRows_); conditionRows_.clear(); conditions_->setRowCount(0); for (const auto C : G["conditions"].toArray()) appendCondition(C.toObject());
        autoDark_->setChecked(G["autoDark"].toBool()); refreshColors();
        for (int I = 0; I < fontKeys_.size(); ++I) fonts_[I]->SetState(G["font"].toObject().contains(fontKeys_[I]) ? G["font"].toObject()[fontKeys_[I]].toBool() ? 1 : 2 : 0);
        border_->setCurrentIndex(qMax(0, border_->findData(G["font"].toObject()["border"].toString())));
        updateConditionHeight(); refreshCost(); refreshPending();
        loading_ = false; updatePreview();
    }
    void storeForm() {
        if (loading_ || list_->currentRow() < 0) return;
        auto Groups = document_["groups"].toArray(); auto G = Groups[list_->currentRow()].toObject(); G["name"] = name_->text(); G["join"] = join_->checkedId() == 1 ? "all" : "any";
        QJsonArray Conditions; for (auto* Row : conditionRows_) Conditions.append(Row->Record()); G["conditions"] = Conditions;
        // v4 carries field conditions; once raised it stays (design 4.5). Never lower another group's need.
        if (document_["version"].toInt() < 3) for (const auto C : Conditions) if (C.toObject().contains("syntax")) { document_["version"] = 3; break; }
        for (const auto C : Conditions) if (C.toObject()["type"] == "field") { document_["version"] = 4; break; }
        auto Light = G["light"].toObject(), Dark = G["dark"].toObject(), Font = G["font"].toObject();
        for (int I = 0; I < 4; ++I) {
            if (I % 2 && (autoDark_->isChecked() || G["autoDark"].toBool())) continue;
            auto& Target = I % 2 ? Dark : Light; const auto* Key = I / 2 ? "background" : "foreground";
            const auto Color = colors_[I]->color();
            if (!Color) Target.remove(Key);
            else if (QColor(Target[Key].toString()) != *Color) Target[Key] = Color->name(Color->alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
        }
        for (int I = 0; I < fontKeys_.size(); ++I) { if (fonts_[I]->State()) Font[fontKeys_[I]] = fonts_[I]->State() == 1; else Font.remove(fontKeys_[I]); }
        if (border_->currentData().toString().isEmpty()) Font.remove("border"); else Font["border"] = border_->currentData().toString();
        if (autoDark_->isChecked() || G.contains("autoDark")) G["autoDark"] = autoDark_->isChecked();
        G["light"] = Light; G["dark"] = Dark; G["font"] = Font; Groups[list_->currentRow()] = G; document_["groups"] = Groups; refreshColors();
        loading_ = true; updateGroupItems(); loading_ = false;
        updateConditionHeight(); refreshCost();
        updatePreview();
    }
    // Conditions of a group by price (design 3): {slow built-in, plugin}.
    QPair<int, int> fieldCosts(const QJsonObject& Group) const {
        int Slow = 0, Plugin = 0;
        for (const auto Value : Group["conditions"].toArray()) {
            const auto C = Value.toObject(); if (C["type"] != "field") continue;
            const auto Id = C["field"].toString();
            const auto Known = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& F) { return F.id_ == Id; });
            const auto Cost = Known != catalog_.end() ? hc::columns::CostOf(*Known) : Id.startsWith("file.") ? hc::columns::ConditionCost::Slow : hc::columns::ConditionCost::Plugin;
            if (Cost == hc::columns::ConditionCost::Slow) ++Slow; else if (Cost == hc::columns::ConditionCost::Plugin) ++Plugin;
        }
        return {Slow, Plugin};
    }
    void refreshCost() {
        const auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow();
        const auto [Slow, Plugin] = Row >= 0 && Row < Groups.size() ? fieldCosts(Groups[Row].toObject()) : QPair<int, int>{};
        cost_->setVisible(Slow || Plugin); if (!Slow && !Plugin) return;
        QStringList Parts; if (Slow) Parts.append(tr("느린 조건 %1개").arg(Slow)); if (Plugin) Parts.append(tr("플러그인 조건 %1개").arg(Plugin));
        auto Text = Parts.join(" · ") + tr(" — 같은 그룹의 다른 조건으로 결정되지 않은 항목만, 화면에 보이는 것부터 값을 확인합니다. 확인 전에는 「확인 중 항목」 색으로 표시합니다.");
        if (Row < Groups.size() - 1) Text += tr(" 위쪽 그룹의 느린 조건은 아래 그룹의 표시도 늦춥니다.");
        if (Plugin) Text += tr(" 플러그인의 느린 필드는 상태 표시줄의 다시 조회로 계산합니다.");
        // One line beside "add condition"; the whole explanation is the tooltip (plan 3.3).
        cost_->setText(Parts.join(" · ") + tr(" · 걸러지지 않은 항목만, 보이는 것부터 확인")); cost_->setToolTip(Text);
    }
    // A colour button as wide as what it shows (swatch, hex code or "none", chevron) instead of its grid column.
    static void fitColorButton(fm::ui::ColorPickButton* Button) {
        QFont None = Button->font(); None.setPixelSize(13);
        const int Text = qMax(QFontMetrics(fm::style::monoFont(12.5)).horizontalAdvance(QStringLiteral("#FFFFFFFF")), QFontMetrics(None).horizontalAdvance(tr("지정 안 함")));
        Button->setFixedWidth(qMax(Button->minimumSizeHint().width(), 26 + 8 + Text + 30 + 4));
    }
    void refreshPending() {
        const auto Pending = document_.value("pending").toObject();
        for (int I = 0; I < 4; ++I) {
            const QSignalBlocker Block(pending_[I]);
            const QColor Color(Pending[I % 2 ? "dark" : "light"].toObject()[I / 2 ? "marker" : "background"].toString());
            pending_[I]->setColor(Color.isValid() ? std::optional<QColor>(Color) : std::nullopt);
        }
        // The legend shows the colour the preview draws: background with the left marker.
        const bool Dark = previewDark_->isChecked(); const qreal Ratio = devicePixelRatioF();
        QPixmap Swatch(QSize(14, 14) * Ratio); Swatch.setDevicePixelRatio(Ratio); Swatch.fill(Qt::transparent);
        QPainter Painter(&Swatch); Painter.fillRect(QRectF(0.5, 0.5, 13, 13), settings::FileColorRules::PendingColor(document_, Dark, "background"));
        Painter.fillRect(QRectF(0.5, 0.5, 3, 13), settings::FileColorRules::PendingColor(document_, Dark, "marker"));
        Painter.setPen(palette().color(QPalette::Mid)); Painter.drawRect(QRectF(0.5, 0.5, 13, 13)); Painter.end();
        pendingLegend_->setIcon(Swatch);
    }
    void showPendingPopover() {
        refreshPending(); pendingPopover_->adjustSize();
        QWidget* Anchor = pendingLegend_->isVisible() ? static_cast<QWidget*>(pendingLegend_) : static_cast<QWidget*>(more_);
        auto At = Anchor->mapToGlobal(QPoint(Anchor->width() - pendingPopover_->width(), Anchor->height() + 4));
        if (const auto* Screen = Anchor->screen()) {
            const auto Area = Screen->availableGeometry();
            At.setX(qBound(Area.left(), At.x(), Area.right() - pendingPopover_->width()));
            if (At.y() + pendingPopover_->height() > Area.bottom()) At.setY(Anchor->mapToGlobal(QPoint()).y() - pendingPopover_->height() - 4);
        }
        pendingPopover_->move(At); pendingPopover_->show(); pending_[0]->setFocus();
    }
    void setPreviewCollapsed(bool Collapsed) {
        { const QSignalBlocker Button(previewCollapse_), Action(collapseAction_); previewCollapse_->setChecked(Collapsed); collapseAction_->setChecked(Collapsed); }
        previewCollapse_->setText(Collapsed ? tr("펼치기") : tr("접기"));
        previewCollapse_->setToolTip(Collapsed ? tr("미리보기 펼치기") : tr("미리보기 접기")); previewCollapse_->setAccessibleName(previewCollapse_->toolTip());
        previewBody_->setVisible(!Collapsed);
        previewCard_->setMinimumHeight(Collapsed ? 0 : 120);
        previewCard_->setMaximumHeight(Collapsed ? previewCard_->minimumSizeHint().height() : QWIDGETSIZE_MAX);
    }
    void applyInitialSplit() {
        const int Total = splitter_->height() - splitter_->handleWidth(); if (splitApplied_ || Total <= 0) return; splitApplied_ = true;
        // The preview starts at about nine rows (the six examples with room to spare) and the editor takes the rest;
        // a short page keeps 170 for the preview (a few rows).
        const int TopMinimum = splitter_->widget(0)->minimumSizeHint().height();
        const int Row = previewTable_->rowCount() ? previewTable_->rowHeight(0) : qMax(24, QFontMetrics(recordScheme(previewDark_->isChecked()).FileList_Font).height() + 8);
        const int Chrome = qMax(0, previewCard_->height() - previewTable_->height());
        const int Rows = Chrome + previewTable_->horizontalHeader()->sizeHint().height() + 2 * previewTable_->frameWidth() + 9 * Row;
        const int Preview = qBound(previewCard_->minimumHeight(), Total >= 560 ? Rows : 170, qMax(previewCard_->minimumHeight(), Total - TopMinimum));
        splitter_->setSizes({Total - Preview, Preview});
        // The group list starts a little wider than its minimum; the editor takes the rest.
        const int Width = bodySplitter_->width() - bodySplitter_->handleWidth();
        if (Width > 0) { const int Groups = qBound(bodySplitter_->widget(0)->minimumWidth(), 270, qMax(200, Width - 420)); bodySplitter_->setSizes({Groups, Width - Groups}); }
        const bool WasLoading = loading_; loading_ = true; updateGroupItems(); loading_ = WasLoading;
    }
    void storePending() {
        if (loading_) return;
        QJsonObject Pending;
        for (int I = 0; I < 4; ++I) if (const auto Color = pending_[I]->color()) {
            auto Scheme = Pending[I % 2 ? "dark" : "light"].toObject(); Scheme[I / 2 ? "marker" : "background"] = Color->name(QColor::HexRgb);
            Pending[I % 2 ? "dark" : "light"] = Scheme;
        }
        if (Pending.isEmpty()) document_.remove("pending"); else document_["pending"] = Pending;
        updatePreview();
    }
    void refreshColors() {
        const auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow(); const auto G = Row >= 0 ? Groups[Row].toObject() : QJsonObject{};
        for (int I = 0; I < 4; ++I) { const QSignalBlocker Block(colors_[I]);
            const QColor Color(settings::FileColorRules::Colors(G, I % 2)[I / 2 ? "background" : "foreground"].toString());
            colors_[I]->setColor(Color.isValid() ? std::optional<QColor>(Color) : std::nullopt); colors_[I]->setEnabled(!(I % 2 && G["autoDark"].toBool()));
        }
    }
    void addGroup(bool Copy) {
        auto Groups = document_["groups"].toArray(); if (Groups.size() >= 256) return;
        QJsonObject G = Copy && list_->currentRow() >= 0 ? Groups[list_->currentRow()].toObject() : QJsonObject{{"enabled", true}, {"join", "any"}, {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "txt"}}}}};
        if (!Copy && document_["version"].toInt() >= 2) G["conditions"] = QJsonArray{settings::FileColorRules::NormalizeCondition({{"type", "extension"}, {"value", "txt"}}, 1)};
        G["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces); G["name"] = tr("새 그룹 %1").arg(G["id"].toString().left(8)); Groups.append(G); document_["groups"] = Groups; refreshList(Groups.size() - 1);
    }
    TyColorScheme recordScheme(bool Dark) const {
        auto* Manager = TyStColorSchemeMgr::GetInstance(); auto Scheme = Manager->GetColorScheme(Manager->GetCurrentColorScheme());
        if (Scheme.IsDarkMode != Dark) Scheme = Manager->GetColorScheme(Dark ? QStringLiteral("기본 다크") : QStringLiteral("기본 라이트"));
        return Scheme;
    }
    QPair<QColor, QColor> recordPair(bool Dark, const settings::FileRuleResult& Result, bool Selected, bool Name = true) const {
        using namespace fm::style; using namespace fm::filelist; using namespace fm::filelist::detail;
        const auto Scheme = recordScheme(Dark); auto Colors = ThemeManager::instance().colors(Dark ? Variant::Dark : Variant::Light);
        Colors.setColor(Token::Surface, Scheme.FileList_BGColor); Colors.setColor(Token::Fg, Scheme.FileList_FGColor);
        if (Scheme.explicitColors_.contains("listSelection")) { Colors.setColor(Token::Sel, Scheme.FileList_Selected); Colors.setColor(Token::InvSel, Scheme.FileList_Selected); }
        ListAppearance Appearance; Appearance.invertSelection = Scheme.IsInverseSelect;
        if (const auto* Inverse = window()->findChild<QCheckBox*>("hc.settings.inverseSelection")) Appearance.invertSelection = Inverse->isChecked();
        RecordState State; State.active = true; State.marked = Selected;
        const auto Geometry = RecordGeometry::make(Colors.design(), false, Appearance);
        QImage Image(32, 32, QImage::Format_ARGB32_Premultiplied); Image.fill(Colors[Token::Surface]);
        QPainter Painter(&Image); paintRecordBackground(&Painter, Image.rect(), Geometry, Appearance, State, false, Colors, Result.background_); Painter.end();
        const auto Background = Image.pixelColor(16, 16);
        const auto Text = textColors(Colors, Appearance, State, false, Geometry.separator);
        const auto Foreground = !Selected && Name && Result.foreground_.isValid() ? Result.foreground_ : Name ? Text.name : Text.meta;
        // Measure the same alpha composite that the painter displays.
        Painter.begin(&Image); Painter.fillRect(Image.rect(), Foreground); Painter.end();
        return {Image.pixelColor(16, 16), Background};
    }
    void updateContrast() {
        const auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow(); const auto G = Row >= 0 ? Groups[Row].toObject() : QJsonObject{};
        for (int I = 0; I < 2; ++I) {
            const auto Colors = settings::FileColorRules::Colors(G, I); settings::FileRuleResult Style;
            Style.foreground_ = QColor(Colors["foreground"].toString()); Style.background_ = QColor(Colors["background"].toString());
            Style.font_ = G["font"].toObject();
            const auto Normal = recordPair(I, Style, false), Selected = recordPair(I, Style, true);
            const auto Ratio = [&](const QPair<QColor, QColor>& Pair) {
                const double Alpha = Pair.first.alphaF() * (Style.font_["dim"].toBool() ? 0.55 : 1.0);
                const auto Shown = QColor::fromRgbF(Pair.first.redF() * Alpha + Pair.second.redF() * (1 - Alpha),
                    Pair.first.greenF() * Alpha + Pair.second.greenF() * (1 - Alpha), Pair.first.blueF() * Alpha + Pair.second.blueF() * (1 - Alpha));
                const double Value = fm::style::contrastRatio(Shown, Pair.second); return QString::number(Value, 'f', 1) + ":1 " + (Value >= 4.5 ? tr("충족") : tr("낮음"));
            };
            contrast_[I]->setText(tr("%1 · 일반 %2 · 선택 %3").arg(I ? tr("다크") : tr("라이트"), Ratio(Normal), Ratio(Selected)));
            contrast_[I]->setToolTip(tr("현재 그룹과 목록 기본색을 기준으로 계산합니다. 4.5:1 이상을 충족으로 표시합니다. 선택 상태는 목록의 선택색을 우선합니다. 수동 색은 자동 보정하지 않습니다."));
        }
    }
    // One preview row: the rule input and the field values known for it (by field id).
    struct PreviewRow { settings::FileRuleInput input_; QMap<QString, hc::columns::CellValue> values_; };
    struct PreviewWork {
        QJsonObject document_; settings::FileRuleInput input_; QList<PreviewRow> rows_;
        QMap<QString, QString> fields_; // Sample field values of the custom input, by field id.
        quint64 generation_ = 0;
    };
    // "-" empty, "!" error, anything else parsed with the condition's value type.
    static hc::columns::CellValue sampleValue(const QString& Text, hc::columns::FieldType Type) {
        using namespace hc::columns;
        if (Text == "-") return CellValue::Missing(ValueState::Empty);
        if (Text == "!") return CellValue::Missing(ValueState::Error, "sample");
        bool Ok = false;
        switch (Type) {
        case FieldType::UnsignedInteger: { const auto V = Text.toULongLong(&Ok); if (Ok) return CellValue::Ready(quint64(V)); break; }
        case FieldType::Integer: { const auto V = Text.toLongLong(&Ok); if (Ok) return CellValue::Ready(qint64(V)); break; }
        case FieldType::Real: { const auto V = Text.toDouble(&Ok); if (Ok) return CellValue::Ready(V); break; }
        case FieldType::Boolean: return CellValue::Ready(Text == "true" || Text == "1" || Text == QObject::tr("예"));
        case FieldType::Date: case FieldType::Timestamp: { const auto V = QDateTime::fromString(Text, Qt::ISODateWithMs); if (V.isValid()) return CellValue::Ready(V); break; }
        case FieldType::Time: { const auto V = QTime::fromString(Text, Qt::ISODate); if (V.isValid()) return CellValue::Ready(V); break; }
        case FieldType::Text: return CellValue::Ready(Text);
        }
        return CellValue::Missing(ValueState::Error, "sample-format");
    }
    struct PreviewOutput {
        QString error_; settings::FileRuleResult custom_; QList<settings::FileRuleResult> samples_;
        QStringList customErrors_; // Field ids whose custom sample value is an error (shown as the reason for a mismatch).
    };
    static PreviewOutput evaluatePreview(const PreviewWork& Work) {
        using namespace hc::columns;
        PreviewOutput Output; settings::FileColorRules Rules;
        Output.error_ = Rules.Load(Work.document_); if (!Output.error_.isEmpty()) return Output;
        settings::FileColorRules::EvaluationContext Context;
        const auto& Required = Rules.RequiredFields();
        for (const auto& Row : Work.rows_) {
            // Examples carry no field values (they show as being checked); panel samples carry what is known.
            auto Input = Row.input_; Input.fields_.clear();
            for (const auto& Field : Required) Input.fields_.append(Row.values_.value(Field, CellValue::Missing(ValueState::Pending)));
            auto Style = Rules.Evaluate(Input, &Context);
            if (Output.error_.isEmpty()) Output.error_ = Style.error_;
            Output.samples_.append(std::move(Style));
        }
        auto Custom = Work.input_;
        for (const auto& Field : Required) {
            QString Type;
            for (const auto G : Work.document_["groups"].toArray()) for (const auto C : G.toObject()["conditions"].toArray())
                if (C.toObject()["field"] == Field) Type = C.toObject()["valueType"].toString();
            const auto Value = Work.fields_.contains(Field) ? sampleValue(Work.fields_[Field], ParseFieldType(Type).value_or(FieldType::Text))
                : CellValue::Missing(ValueState::Pending);
            if (Value.State() == ValueState::Error) Output.customErrors_.append(Field);
            Custom.fields_.append(Value);
        }
        Output.custom_ = Rules.Evaluate(Custom, &Context);
        if (Output.error_.isEmpty()) Output.error_ = Output.custom_.error_;
        return Output;
    }
    void startPreview() {
        if (previewRunning_ || !pendingPreview_) return;
        const auto Work = std::exchange(pendingPreview_, {}); previewRunning_ = true;
        auto* Watcher = new QFutureWatcher<PreviewOutput>(this);
        connect(Watcher, &QFutureWatcher<PreviewOutput>::finished, this, [this, Watcher, Work] {
            auto Output = Watcher->future().takeResult(); Watcher->deleteLater(); previewRunning_ = false;
            if (Work->generation_ == previewGeneration_) applyPreview(*Work, Output);
            if (pendingPreview_ && !previewTimer_.isActive()) startPreview();
        });
        Watcher->setFuture(QtConcurrent::run([Work] { return evaluatePreview(*Work); }));
    }
    // Field ids the draft's field conditions use: the selected group's first, then the rest, each once.
    QStringList documentFields(bool CurrentOnly = false) const {
        QStringList Fields; const auto Groups = document_["groups"].toArray(); const int Current = list_->currentRow();
        const auto Collect = [&](const QJsonObject& G) { for (const auto C : G["conditions"].toArray()) if (C.toObject()["type"] == "field") { const auto Id = C.toObject()["field"].toString(); if (!Id.isEmpty() && !Fields.contains(Id)) Fields.append(Id); } };
        if (Current >= 0 && Current < Groups.size()) Collect(Groups[Current].toObject());
        if (!CurrentOnly) for (const auto G : Groups) Collect(G.toObject());
        return Fields;
    }
    const hc::columns::FieldDefinition* fieldDefinition(const QString& Id) const {
        const auto Known = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& F) { return F.id_ == Id; });
        return Known != catalog_.end() ? &*Known : nullptr;
    }
    QString fieldTitle(const QString& Id) const { const auto* Known = fieldDefinition(Id); return Known ? Known->title_ : Id; }
    const QVector<settings::FileRuleSample>& currentSamples() const {
        static const QVector<settings::FileRuleSample> None;
        const int Source = source_->checkedId(); return Source == 1 ? folderSamples_ : Source == 2 ? selectionSamples_ : None;
    }
    QString networkMode() const {
        return settings::AppSettings().Read(settings::Section::General).toObject()["filePanel"].toObject()["networkFieldConditions"]
            .toString(hc::defaults::DefaultValue("general/filePanel/networkFieldConditions").toString());
    }
    // Values a panel sample can have for a field without reading it now (plan 8): known, not applicable, or waiting.
    hc::columns::CellValue sampleFieldValue(const settings::FileRuleSample& Sample, const QString& Field, const QString& Mode) const {
        using namespace hc::columns;
        if (const auto Known = Sample.values_.constFind(Field); Known != Sample.values_.cend()) return *Known;
        if (const auto Path = computed_.constFind(Sample.path_); Path != computed_.cend()) if (const auto Read = Path->constFind(Field); Read != Path->cend()) return *Read;
        if (QueryFamily(Field).isEmpty()) return CellValue::Missing(ValueState::Pending);
        if (Sample.path_.isEmpty()) return CellValue::Missing(ValueState::Unsupported, "rule-field-location");
        if (Sample.directory_ && !QueriesFolders(Field)) return CellValue::Missing(ValueState::Empty);
        if (!NetworkFieldAllowed(Mode, Sample.network_, Field)) return CellValue::Missing(ValueState::Unsupported, "network-setting");
        return CellValue::Missing(ValueState::Pending);
    }
    void computeSampleFields() {
        using namespace hc::columns;
        const auto& Samples = currentSamples(); const auto Fields = documentFields(); const auto Mode = networkMode();
        previewEngine_->Cancel(); previewEngine_->Resume(); computing_ = true;
        for (const auto& Sample : Samples) for (const auto& Field : Fields) {
            if (QueryFamily(Field).isEmpty() || sampleFieldValue(Sample, Field, Mode).State() != ValueState::Pending) continue;
            const auto Value = previewEngine_->Read(Sample.path_, Field, true, false);
            if (Value.State() != ValueState::Pending && Value.State() != ValueState::OnDemand) computed_[Sample.path_][Field] = Value;
        }
        if (!previewEngine_->Pending()) computing_ = false;
        updatePreview();
    }
    void sampleValuesArrived(const QSet<QString>& Paths) {
        using namespace hc::columns;
        const auto Fields = documentFields();
        for (const auto& Path : Paths) for (const auto& Field : Fields) {
            if (QueryFamily(Field).isEmpty()) continue;
            const auto Value = previewEngine_->Read(Path, Field, false);
            if (Value.State() != ValueState::Pending && Value.State() != ValueState::OnDemand) computed_[Path][Field] = Value;
        }
        if (!previewEngine_->Pending()) computing_ = false;
        updatePreview();
    }
    void stopComputation() { if (previewEngine_) previewEngine_->Cancel(); computing_ = false; }
    void refreshSampleStatus(int Rows) {
        using namespace hc::columns;
        const int Source = source_->checkedId();
        if (Source == 0) { sampleStatus_->setText(tr("예제 %1개 · 기준 2026-01-15 12:00 UTC").arg(Rows)); compute_->setEnabled(false); sampleStatus_->setToolTip({}); return; }
        auto Text = Source == 1 ? tr("%1 · 표본 %2개").arg(sampleFolder_.isEmpty() ? tr("현재 폴더") : sampleFolder_).arg(Rows) : tr("선택 항목 %1개").arg(Rows);
        int Waiting = 0; const auto Mode = networkMode(); const auto Fields = documentFields();
        for (const auto& Sample : currentSamples()) for (const auto& Field : Fields) Waiting += !QueryFamily(Field).isEmpty() && sampleFieldValue(Sample, Field, Mode).State() == ValueState::Pending;
        if (computing_ && previewEngine_->Pending()) Text += tr(" · 계산 중 %1건").arg(previewEngine_->Pending());
        else if (Waiting) Text += tr(" · 값 필요 %1건").arg(Waiting);
        else if (!computed_.isEmpty()) Text += tr(" · 계산 완료");
        sampleStatus_->setText(Text); sampleStatus_->setToolTip(Text); compute_->setEnabled(Waiting && !(computing_ && previewEngine_->Pending()));
    }
    // Custom input rows (plan 7): one per field the rules use; they write the "field=value;" text the evaluation reads.
    struct SampleFieldRow { QString id_; QComboBox* state_ = nullptr; QComboBox* choice_ = nullptr; QLineEdit* value_ = nullptr; };
    QString sampleRowValue(const SampleFieldRow& Row) const { return Row.choice_ ? Row.choice_->currentData().toString() : Row.value_->text().trimmed(); }
    void rebuildSampleFieldRows() {
        const auto Ids = documentFields(); if (Ids == sampleRowIds_) return;
        QHash<QString, QPair<int, QString>> Previous; for (const auto& Row : sampleRows_) Previous.insert(Row.id_, {Row.state_->currentIndex(), sampleRowValue(Row)});
        for (auto* Child : sampleFieldsBox_->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) { sampleFieldRows_->removeWidget(Child); Child->hide(); Child->deleteLater(); }
        sampleRows_.clear(); sampleRowIds_ = Ids;
        if (Ids.isEmpty()) return;
        auto* Heading = new QLabel(tr("필드 표본 값"), sampleFieldsBox_); auto Bold = Heading->font(); Bold.setBold(true); Heading->setFont(Bold);
        Heading->setToolTip(previewFields_->toolTip()); sampleFieldRows_->addWidget(Heading, 0, 0, 1, 2);
        int Line = 1;
        for (const auto& Id : Ids) {
            SampleFieldRow Row; Row.id_ = Id; const auto* Known = fieldDefinition(Id);
            auto* Title = new QLabel(fieldTitle(Id), sampleFieldsBox_); Title->setToolTip(Id); sampleFieldRows_->addWidget(Title, Line, 0);
            Row.state_ = new QComboBox(sampleFieldsBox_); Row.state_->setObjectName("hc.rules.sampleState." + Id); Row.state_->setAccessibleName(fieldTitle(Id) + tr(" 값 상태"));
            Row.state_->addItems({tr("값 있음"), tr("빈 값"), tr("확인 중"), tr("오류")}); Row.state_->setCurrentIndex(2); sampleFieldRows_->addWidget(Row.state_, Line++, 1);
            if (Known && !Known->choices_.isEmpty()) {
                Row.choice_ = new QComboBox(sampleFieldsBox_); Row.choice_->setObjectName("hc.rules.sampleValue." + Id); Row.choice_->setAccessibleName(fieldTitle(Id));
                for (const auto& Choice : Known->choices_) Row.choice_->addItem(Choice.title_, Choice.value_);
                sampleFieldRows_->addWidget(Row.choice_, Line++, 0, 1, 2);
                connect(Row.choice_, &QComboBox::currentIndexChanged, this, [this] { writeSampleFields(); });
            } else {
                Row.value_ = new QLineEdit(sampleFieldsBox_); Row.value_->setObjectName("hc.rules.sampleValue." + Id); Row.value_->setAccessibleName(fieldTitle(Id));
                const QMap<hc::columns::FieldUnit, QString> Units{{hc::columns::FieldUnit::Bytes, tr("바이트")}, {hc::columns::FieldUnit::Pixels, tr("픽셀")}, {hc::columns::FieldUnit::Count, tr("개")}, {hc::columns::FieldUnit::Characters, tr("글자")}};
                Row.value_->setPlaceholderText(Known && Units.contains(Known->unit_) ? tr("값 · %1").arg(Units[Known->unit_]) : tr("값"));
                sampleFieldRows_->addWidget(Row.value_, Line++, 0, 1, 2);
                connect(Row.value_, &QLineEdit::textChanged, this, [this] { writeSampleFields(); });
            }
            QWidget* Editor = Row.choice_ ? static_cast<QWidget*>(Row.choice_) : static_cast<QWidget*>(Row.value_);
            if (const auto Old = Previous.constFind(Id); Old != Previous.cend()) {
                const QSignalBlocker BlockState(Row.state_), BlockEditor(Editor); Row.state_->setCurrentIndex(Old->first);
                if (Row.choice_) Row.choice_->setCurrentIndex(qMax(0, Row.choice_->findData(Old->second))); else Row.value_->setText(Old->second);
            }
            Editor->setEnabled(Row.state_->currentIndex() == 0);
            connect(Row.state_, &QComboBox::currentIndexChanged, this, [this, Editor](int State) { Editor->setEnabled(State == 0); writeSampleFields(); });
            sampleRows_.append(Row);
        }
    }
    void writeSampleFields() {
        QStringList Parts;
        for (const auto& Row : sampleRows_) {
            const int State = Row.state_->currentIndex(); if (State == 2) continue;
            const auto Value = State == 1 ? QString("-") : State == 3 ? QString("!") : sampleRowValue(Row);
            if (!Value.isEmpty()) Parts.append(Row.id_ + "=" + Value);
        }
        previewFields_->setText(Parts.join("; "));
    }
    void updatePreview() {
        if (!preview_ || !error_) return;
        ++previewGeneration_; pendingPreview_.reset(); previewTimer_.stop();
        updateContrast(); rebuildSampleFieldRows();
        bool SizeOk = false, AttributesOk = false; const auto Size = previewSize_->text().toULongLong(&SizeOk); const auto Attributes = previewAttributes_->text().toULongLong(&AttributesOk);
        if (!SizeOk || !AttributesOk || Attributes > 0xFFFFFFFFULL) {
            validationPending_ = false; validatedDocument_ = {}; error_->setText(tr("크기는 0 이상 정수, 속성은 0~4294967295 정수로 입력하세요.")); preview_->setText(tr("미리보기 입력을 확인하세요.")); return;
        }
        auto Work = std::make_shared<PreviewWork>(); Work->document_ = document_; Work->generation_ = previewGeneration_;
        auto& Input = Work->input_; Input = settings::FileRuleInput{previewName_->text(), quint32(Attributes) | (previewFolder_->isChecked() ? 0x10u : 0u), Size, previewDark_->isChecked()};
        if (previewModifiedKnown_->isChecked()) Input.modifiedUtcMs_ = previewModified_->dateTime().toMSecsSinceEpoch();
        if (previewLink_->currentIndex()) { Input.attributes_ |= 0x400; if (previewLink_->currentIndex() < 3) Input.reparseTag_ = previewLink_->currentIndex() == 1 ? 0xa000000cu : 0xa0000003u; }
        if (source_->checkedId() == 0) for (auto Sample : settings::FileColorExamples::Samples()) { Sample.dark_ = Input.dark_; Work->rows_.append({Sample, {}}); }
        else {
            const auto Mode = networkMode(); const auto Fields = documentFields(); const auto Now = QDateTime::currentMSecsSinceEpoch();
            for (const auto& Sample : currentSamples()) {
                PreviewRow Row{Sample.input_, {}}; Row.input_.dark_ = Input.dark_; Row.input_.referenceUtcMs_ = Now;
                for (const auto& Field : Fields) Row.values_.insert(Field, sampleFieldValue(Sample, Field, Mode));
                Work->rows_.append(std::move(Row));
            }
        }
        refreshSampleStatus(int(Work->rows_.size()));
        for (const auto& Pair : previewFields_->text().split(';', Qt::SkipEmptyParts)) {
            const auto At = Pair.indexOf('='); if (At <= 0) continue;
            Work->fields_.insert(Pair.left(At).trimmed(), Pair.mid(At + 1).trimmed());
        }
        validationPending_ = true;
        if (settings::FileColorRules::HasRegex(document_)) {
            error_->setText(tr("조건을 검증하고 있습니다…"));
            pendingPreview_ = std::move(Work); previewTimer_.start();
        } else applyPreview(*Work, evaluatePreview(*Work));
    }
    static QMap<QString, QString> effectNames() {
        return {{"foreground", tr("글자색")}, {"background", tr("배경색")}, {"bold", tr("굵게")}, {"italic", tr("기울임")}, {"underline", tr("밑줄")}, {"strike", tr("취소선")}, {"dim", tr("흐려짐")}, {"border", tr("행 테두리")}};
    }
    void applyPreview(const PreviewWork& Work, const PreviewOutput& Output) {
        validationPending_ = false; validatedDocument_ = Work.document_; validationError_ = Output.error_;
        error_->setText(validationError_); previewStyles_ = Output.samples_; updateSampleTable(Work);
        const auto& Result = Output.custom_;
        const auto Pair = recordPair(Work.input_.dark_, Result, previewSelected_->isChecked()); auto Palette = palette(); Palette.setColor(QPalette::WindowText, Pair.first); Palette.setColor(QPalette::Window, Pair.second);
        preview_->setPalette(Palette); preview_->setFont(Result.Font(recordScheme(Work.input_.dark_).FileList_Font)); preview_->setText(previewName_->text());
        preview_->setProperty("hc.fileEffects", QVariant::fromValue(Result.font_)); preview_->update();
        const auto EffectNames = effectNames();
        QStringList Owners; for (auto It = Result.owners_.begin(); It != Result.owners_.end(); ++It) Owners.append(EffectNames.value(It.key(), It.key()) + ": " + It.value());
        if (Result.pending_) owners_->setText(tr("확인 중: 필드 표본 값이 없는 필드 조건이 결과를 정합니다. 확인 중 항목 색으로 표시합니다."));
        else {
            auto Text = tr("일치 그룹: %1\n%2\n크기·속성은 위 미리보기 입력을 사용합니다.").arg(Result.groups_.join(", "), Owners.join(" · "));
            if (!Output.customErrors_.isEmpty()) { QStringList Titles; for (const auto& Id : Output.customErrors_) Titles.append(fieldTitle(Id)); Text += tr("\n오류 값은 조건에 일치하지 않는 것으로 봅니다: %1").arg(Titles.join(", ")); }
            owners_->setText(Text);
        }
    }

    // A field cell: the value, or why there is none. A row decided without the value shows a dash (plan 7).
    QString fieldCellText(const hc::columns::CellValue& Value, bool RowPending) const {
        using namespace hc::columns;
        switch (Value.State()) {
        case ValueState::Ready: return Value.Display().isEmpty() ? Format(Value) : Value.Display();
        case ValueState::Empty: return tr("없음");
        case ValueState::Unsupported: return Value.Error() == "network-setting" ? tr("계산 안 함") : tr("지원 안 함");
        case ValueState::Error: return tr("오류");
        default: return RowPending ? tr("확인 중") : tr("—");
        }
    }
    void updateSampleTable(const PreviewWork& Work) {
        // Columns (plan 7): name, size, the selected group's fields (two at most, else one combined column), group.
        const auto GroupFields = documentFields(true); const bool Combined = GroupFields.size() > 2;
        QStringList Headers{tr("이름"), tr("크기")};
        if (Combined) Headers.append(tr("필드 값")); else for (const auto& Id : GroupFields) Headers.append(fieldTitle(Id));
        Headers.append(tr("적용된 그룹")); const int Columns = int(Headers.size()), GroupColumn = Columns - 1;
        QStringList Current; for (int Col = 0; Col < previewTable_->columnCount(); ++Col) Current.append(previewTable_->horizontalHeaderItem(Col) ? previewTable_->horizontalHeaderItem(Col)->text() : QString{});
        if (Current != Headers) { previewTable_->setColumnCount(Columns); previewTable_->setHorizontalHeaderLabels(Headers); }
        // The columns always fill the table: name and group share the free width, size and field values fit their text.
        auto* Header = previewTable_->horizontalHeader(); Header->setSectionResizeMode(0, QHeaderView::Stretch); Header->setSectionResizeMode(1, QHeaderView::Fixed);
        for (int Col = 2; Col < Columns; ++Col) Header->setSectionResizeMode(Col, Col == GroupColumn ? QHeaderView::Stretch : QHeaderView::Fixed);
        // Field columns are measured with the row fonts the delegate draws, like the size column.
        QVector<int> FieldWidths; for (int Col = 2; Col < GroupColumn; ++Col) FieldWidths.append(Header->fontMetrics().horizontalAdvance(Headers[Col]) + 24);
        const auto BaseFont = recordScheme(previewDark_->isChecked()).FileList_Font;
        const auto Labels = effectNames();
        int SizeWidth = previewTable_->fontMetrics().horizontalAdvance(tr("크기")) + 20;
        previewTable_->setRowCount(int(Work.rows_.size()));
        for (int Row = 0; Row < Work.rows_.size(); ++Row) {
            const auto& Input = Work.rows_[Row].input_; const auto Style = previewStyles_.value(Row);
            QStringList Owners; for (auto It = Style.owners_.begin(); It != Style.owners_.end(); ++It) Owners.append(Labels.value(It.key(), It.key()) + ": " + It.value());
            QStringList Values{Input.name_, (Input.attributes_ & 0x10) ? tr("폴더") : QLocale().formattedDataSize(Input.size_, 1, QLocale::DataSizeTraditionalFormat)};
            QStringList FieldTexts; for (const auto& Id : GroupFields) FieldTexts.append(fieldCellText(Work.rows_[Row].values_.value(Id, hc::columns::CellValue::Missing(hc::columns::ValueState::Pending)), Style.pending_));
            if (Combined) { QStringList Parts; for (int I = 0; I < GroupFields.size(); ++I) Parts.append(fieldTitle(GroupFields[I]) + " " + FieldTexts[I]); Values.append(Parts.join(" · ")); } else Values += FieldTexts;
            Values.append(Style.pending_ ? tr("확인 중 · 필드 값 필요") : Style.groups_.isEmpty() ? tr("목록 기본값") : Style.groups_.join(" + "));
            for (int Col = 0; Col < Columns; ++Col) { auto* Item = previewTable_->item(Row, Col); if (!Item) { Item = new QTableWidgetItem; previewTable_->setItem(Row, Col, Item); }
                Item->setText(Values.value(Col)); Item->setFont(Style.Font(BaseFont)); Item->setToolTip(Values.join(" · ") + "\n" + Owners.join("\n")); Item->setData(Qt::AccessibleDescriptionRole, Owners.join(" · "));
            }
            const int Pitch = qMax(24, QFontMetrics(Style.Font(BaseFont)).height() + 8); previewTable_->setRowHeight(Row, Pitch);
            SizeWidth = qMax(SizeWidth, QFontMetrics(Style.Font(BaseFont)).horizontalAdvance(Values[1]) + 20);
            for (int I = 0; I < FieldWidths.size(); ++I) FieldWidths[I] = qMax(FieldWidths[I], QFontMetrics(Style.Font(BaseFont)).horizontalAdvance(Values.value(2 + I)) + 20);
        }
        previewTable_->setColumnWidth(1, SizeWidth);
        for (int I = 0; I < FieldWidths.size(); ++I) previewTable_->setColumnWidth(2 + I, qMin(FieldWidths[I], 260));
        previewTable_->viewport()->update();
    }
    QTimer previewTimer_;
    quint64 previewGeneration_ = 0;
    bool previewRunning_ = false, validationPending_ = false;
    std::shared_ptr<PreviewWork> pendingPreview_;
    QJsonObject validatedDocument_;
    QString validationError_;
    QJsonObject document_;
    bool loading_ = false, supported_ = true;
    QWidget* editor_ = nullptr;
    fm::ui::ReorderListWidget* list_ = nullptr;
    QLineEdit *name_ = nullptr, *previewName_ = nullptr, *previewSize_ = nullptr, *previewAttributes_ = nullptr;
    fm::ui::ColorPickButton* colors_[4]{};
    FileFontEffectButton* fonts_[5]{};
    QComboBox* border_ = nullptr;
    QComboBox* previewLink_ = nullptr;
    QCheckBox* autoDark_ = nullptr;
    QLabel* contrast_[2]{};
    QTableWidget* previewTable_ = nullptr;
    QList<settings::FileRuleResult> previewStyles_;
    QButtonGroup* join_ = nullptr;
    QButtonGroup* mode_ = nullptr;
    QTableWidget* conditions_ = nullptr;
    QList<FileColorConditionRow*> conditionRows_;
    QDateTimeEdit* previewModified_ = nullptr;
    QCheckBox* previewModifiedKnown_ = nullptr;
    QCheckBox *previewFolder_ = nullptr, *previewDark_ = nullptr, *previewSelected_ = nullptr;
    QLabel *preview_ = nullptr, *owners_ = nullptr, *error_ = nullptr, *cost_ = nullptr, *sampleStatus_ = nullptr;
    QLineEdit* previewFields_ = nullptr;
    fm::ui::ColorPickButton* pending_[4]{};
    // Layout (plan 3.1).
    QToolButton* more_ = nullptr;
    fm::ui::Button *pendingLegend_ = nullptr, *previewCollapse_ = nullptr;
    QSplitter* bodySplitter_ = nullptr;
    QAction* collapseAction_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTabWidget* editorTabs_ = nullptr;
    fm::ui::Card* previewCard_ = nullptr;
    QWidget *previewBody_ = nullptr, *sampleFieldsBox_ = nullptr;
    QScrollArea* customPanel_ = nullptr;
    QFrame* pendingPopover_ = nullptr;
    QGridLayout* sampleFieldRows_ = nullptr;
    QList<SampleFieldRow> sampleRows_;
    QStringList sampleRowIds_;
    bool splitApplied_ = false;
    // Sample sources and value computation (plan 8).
    QButtonGroup* source_ = nullptr;
    fm::ui::Button* compute_ = nullptr;
    QVector<settings::FileRuleSample> folderSamples_, selectionSamples_;
    QString sampleFolder_;
    QHash<QString, QMap<QString, hc::columns::CellValue>> computed_;
    bool computing_ = false;
    std::unique_ptr<hc::columns::QueryEngine> previewEngine_;
    QVector<hc::columns::FieldDefinition> catalog_ = hc::columns::BuiltInFields();
    const QStringList typeKeys_{"extension", "pattern", "folder", "attributes", "size", "modified", "link", "normalization", "rtl"};
    const QStringList fontKeys_{"bold", "italic", "underline", "strike", "dim"};
};
} // namespace hc::ui
