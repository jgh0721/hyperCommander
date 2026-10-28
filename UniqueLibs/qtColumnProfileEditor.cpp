#include "stdafx.h"
#include "columnAutoSelection.hpp"
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/Card.h>
#include <fmstyle/StyleProps.h>
#include "qtColumnProfileEditor.hpp"
#include "cmnHCUtils.hpp"
#include <QScrollArea>
#include <QFileIconProvider>
#include <QPainter>
#include "columnPluginFields.hpp"
#include "commonLibs/columns/luaColumns.hpp"
#include "nativeMaterializer.hpp"
#include "storageProviders.hpp"
#include "qtitanColumnAdapter.hpp"
#include "column_Defs.hpp"
#include <QtitanGrid.h>
#include "commonLibs/columns/columnSort.hpp"

using namespace hc::columns;

namespace
{
    // Keep the list's native selection, checkbox and keyboard behavior while
    // showing a compact summary of each column, including hidden columns.
    class ColumnListDelegate final : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
        {
            auto Size = QStyledItemDelegate::sizeHint(Option, Index);
            Size.setHeight(qMax(28, Option.fontMetrics.height() + 10));
            return Size;
        }
        void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
        {
            QStyleOptionViewItem Item(Option); initStyleOption(&Item, Index); Item.text.clear();
            const auto* Style = Item.widget ? Item.widget->style() : QApplication::style();
            Style->drawControl(QStyle::CE_ItemViewItem, &Item, Painter, Item.widget);
            Painter->save(); Painter->setClipRect(Option.rect);
            const auto Group = Option.state.testFlag(QStyle::State_Enabled) ? QPalette::Active : QPalette::Disabled;
            Painter->setPen(Option.palette.color(Group, Option.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
            const QStringList Texts{Index.data().toString(), Index.data(Qt::UserRole + 1).toString(),
                Index.data(Qt::UserRole + 2).toString(), Index.data(Qt::UserRole + 3).toString()};
            const QList<int> Weights{32, 34, 17, 17};
            const auto Check = Style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &Item, Item.widget);
            const int Padding = qMax(30, Check.right() - Option.rect.left() + 6);
            const int Available = qMax(0, Option.rect.width() - Padding - 6);
            int Offset = Padding;
            for (int Column = 0; Column < Texts.size(); ++Column) {
                const int Width = Available * Weights[Column] / 100;
                const QRect Rect(Option.rect.left() + Offset, Option.rect.top(), Width - 6, Option.rect.height());
                Painter->drawText(Rect, Qt::AlignLeft | Qt::AlignVCenter, Option.fontMetrics.elidedText(Texts[Column], Qt::ElideRight, qMax(0, Rect.width())));
                Offset += Width;
            }
            Painter->restore();
        }
    };
    class ColumnChoice final : public QComboBox
    {
    public:
        using QComboBox::QComboBox;
        QSize minimumSizeHint() const override
        {
            auto Size = QComboBox::minimumSizeHint(); Size.setWidth(75); return Size;
        }
    protected:
        void initStyleOption(QStyleOptionComboBox* Option) const override
        {
            QComboBox::initStyleOption(Option);
            if (objectName() == "hc.columns.unit" || objectName() == "hc.columns.widthMode") Option->textAlignment = Qt::AlignCenter;
        }
    };
    class WidthNumber final : public QDoubleSpinBox
    {
    public:
        using QDoubleSpinBox::QDoubleSpinBox;
        QSize minimumSizeHint() const override
        {
            auto Size = QDoubleSpinBox::minimumSizeHint(); Size.setWidth(60); return Size;
        }
    protected:
        QString textFromValue(double Value) const override { return locale().toString(Value, 'f', 0); }
    };
    QComboBox* Choice(QWidget* Parent, const char* Name, const QStringList& Items)
    {
        auto* Result = new ColumnChoice(Parent); Result->setObjectName(Name); Result->addItems(Items);
        Result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        Result->setMinimumContentsLength(12); Result->setMaxVisibleItems(16);
        Result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); Result->setMinimumWidth(75);
        return Result;
    }
}

QtColumnProfileEditor::QtColumnProfileEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.columns.editor");
    installEventFilter(this);
    autoPolicy_ = EmptyAutoSelection();
    auto* Outer = new QVBoxLayout(this); Outer->setContentsMargins(0, 0, 0, 0);
    // The host has its own viewport. Let the splitters and inspector absorb a
    // smaller window instead of forcing the complete settings page to scroll.
    Outer->setSizeConstraint(QLayout::SetNoConstraint);
    auto* Header = new fm::ui::DialogHeader(this);
    Header->setTitle(tr("열 · 사용자 정의 열"));
    Header->setSubtitle(tr("열을 추가하고, 미리보기의 열 머리글을 선택해 표시 형식을 편집하세요."));
    Outer->addWidget(Header);
    auto* Body = new QHBoxLayout; Outer->addLayout(Body, 1);
    auto* ProfileCard = new fm::ui::Card(tr("열 세트"), this); ProfileCard->setMinimumWidth(150); ProfileCard->setMaximumWidth(200);
    auto* ProfileRows = new QVBoxLayout(ProfileCard); ProfileRows->setContentsMargins(8, 8, 8, 10); Body->addWidget(ProfileCard);
    Body->setSpacing(12);
    profileList_ = new QListWidget(ProfileCard); profileList_->setObjectName("hc.columns.profileList");
    profileList_->setTextElideMode(Qt::ElideRight); profileList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ProfileRows->addWidget(profileList_, 1);
    profileTitle_ = new QLineEdit(ProfileCard); profileTitle_->setObjectName("hc.columns.profileTitle");
    profileTitle_->setAccessibleName(tr("열 세트 이름")); ProfileRows->addWidget(profileTitle_);
    auto* ProfileActions = new QHBoxLayout; ProfileRows->addLayout(ProfileActions);
    auto* Copy = new QPushButton(tr("복제"), ProfileCard); Copy->setObjectName("hc.columns.cloneProfile"); ProfileActions->addWidget(Copy);
    auto* Delete = new QPushButton(tr("삭제"), ProfileCard); deleteProfile_ = Delete; ProfileActions->addWidget(Delete);
    pages_ = new QTabWidget(this); pages_->setObjectName("hc.columns.pages"); Body->addWidget(pages_, 1);
    const auto AddPage = [this](QWidget* Page, const QString& Title) {
        auto* Scroll = new QScrollArea(pages_); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        Scroll->setWidget(Page); pages_->addTab(Scroll, Title);
    };
    auto* Workspace = new QWidget(pages_); Workspace->setObjectName("hc.columns.workspace");
    auto* Root = new QVBoxLayout(Workspace); Root->setContentsMargins(6, 6, 6, 6);
    pages_->addTab(Workspace, tr("열 구성"));
    auto* Modes = new QHBoxLayout; Root->addLayout(Modes);
    layout_ = Choice(Workspace, "hc.columns.layout", {tr("한 줄"), tr("두 줄: 이름 위"), tr("두 줄: 이름 아래")});
    layout_->setMinimumWidth(150);
    Modes->addWidget(new QLabel(tr("표시 모드"), Workspace)); Modes->addWidget(layout_);
    auto* Guide = new QLabel(tr("열 선택 → 속성 편집 → 적용"), Workspace); Modes->addStretch(); Modes->addWidget(Guide);
    auto* Split = new QSplitter(Qt::Horizontal, Workspace); Split->setObjectName("hc.columns.workspaceSplit"); Root->addWidget(Split, 1);
    auto* Work = new QSplitter(Qt::Vertical, Split); Work->setObjectName("hc.columns.previewListSplit");
    auto* PreviewPanel = new QWidget(Work); auto* PreviewRows = new QVBoxLayout(PreviewPanel); PreviewRows->setContentsMargins(0, 0, 4, 0);
    auto* Left = new QWidget(Work); auto* LeftRows = new QVBoxLayout(Left); LeftRows->setContentsMargins(0, 4, 4, 0);
    auto* ListBar = new QHBoxLayout; LeftRows->addLayout(ListBar);
    auto* ListTitle = new QLabel(tr("열 구성 목록"), Left); ListBar->addWidget(ListTitle); ListBar->addStretch();
    auto* Actions = new QGridLayout; ListBar->addLayout(Actions);
    const auto Button = [&](const QString& Label, const char* Name, int Row, int Col, auto Handler) {
        auto* Item = new QPushButton(Label, Left); Item->setObjectName(Name); Actions->addWidget(Item, 0, Row * 3 + Col);
        if (Row == 1 && Col < 2) { Item->setText({}); Item->setIcon(style()->standardIcon(Col ? QStyle::SP_ArrowDown : QStyle::SP_ArrowUp)); Item->setToolTip(Label); Item->setAccessibleName(Label); }
        connect(Item, &QPushButton::clicked, this, Handler);
    };
    Button(tr("추가"), "hc.columns.add", 0, 0, [this] {
        ColumnDefinition Column; Column.fieldId_ = "file.size"; Column.title_ = tr("크기"); Column.alignment_ = Qt::AlignRight;
        Column.widths_.fill({hc::view::WidthUnit::Pixels, hc::view::WidthMode::Flexible, 120});
        draft_.columns_.append(Column); dirty_ = true; rebuildList(int(draft_.columns_.size()) - 1);
    });
    Button(tr("복제"), "hc.columns.duplicate", 0, 1, [this] {
        if (row_ < 0) return; auto Column = draft_.columns_[row_]; Column.id_ = QUuid::createUuid();
        draft_.columns_.insert(row_ + 1, Column); dirty_ = true; rebuildList(row_ + 1);
    });
    Button(tr("제거"), "hc.columns.remove", 0, 2, [this] {
        if (row_ < 0) return;
        if (draft_.columns_[row_].id_ == draft_.primaryNameId_) { error_->setText(tr("다른 이름 컬럼을 주 이름으로 지정한 후 제거하세요.")); return; }
        const auto Id = draft_.columns_[row_].id_; draft_.columns_.removeAt(row_);
        draft_.sorts_.erase(std::remove_if(draft_.sorts_.begin(), draft_.sorts_.end(), [&](const auto& Sort) { return Sort.columnId_ == Id; }), draft_.sorts_.end());
        dirty_ = true; rebuildList(std::min(row_, int(draft_.columns_.size()) - 1));
    });
    for (int Direction : {-1, 1}) Button(Direction < 0 ? tr("위로") : tr("아래로"), Direction < 0 ? "hc.columns.up" : "hc.columns.down", 1, Direction < 0 ? 0 : 1, [this, Direction] {
        const int Next = row_ + Direction; if (row_ < 0 || Next < 0 || Next >= draft_.columns_.size()) return;
        draft_.columns_.swapItemsAt(row_, Next); dirty_ = true; rebuildList(Next);
    });
    Button(tr("기본값"), "hc.columns.reset", 1, 2, [this] { const auto Id = draft_.id_; const auto Title = draft_.title_; draft_ = Id == ReferenceProfileId ? ReferenceProfile() : Id == ReferenceTwoLineProfileId ? ReferenceProfile(true) : DefaultProfile(); draft_.id_ = Id; draft_.title_ = Title; dirty_ = true; rebuildList(0); });
    auto* ListHeader = new QWidget(Left); auto* ListHeaderRows = new QHBoxLayout(ListHeader);
    ListHeaderRows->setContentsMargins(30, 2, 6, 2); ListHeaderRows->setSpacing(0);
    const QStringList Captions{tr("제목"), tr("필드"), tr("너비"), tr("글자 정렬")};
    const QList<int> Stretches{32, 34, 17, 17};
    for (int Index = 0; Index < Captions.size(); ++Index) ListHeaderRows->addWidget(new QLabel(Captions[Index], ListHeader), Stretches[Index]);
    LeftRows->addWidget(ListHeader);
    columns_ = new QListWidget(Left); columns_->setObjectName("hc.columns.list");
    columns_->setItemDelegate(new ColumnListDelegate(columns_)); columns_->setMinimumHeight(85);
    columns_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); LeftRows->addWidget(columns_, 1);
    auto* InspectorScroll = new QScrollArea(Split); InspectorScroll->setObjectName("hc.columns.inspectorScroll");
    InspectorScroll->setWidgetResizable(true); InspectorScroll->setFrameShape(QFrame::NoFrame); InspectorScroll->setMinimumWidth(300);
    auto* Inspector = new QWidget; Inspector->setObjectName("hc.columns.inspector"); InspectorScroll->setWidget(Inspector);
    auto* InspectorRows = new QVBoxLayout(Inspector); InspectorRows->setContentsMargins(8, 0, 2, 0);
    columnTitle_ = new QLabel(Inspector); columnTitle_->setObjectName("hc.columns.columnTitle");
    columnTitle_->setWordWrap(true);
    auto TitleFont = font(); TitleFont.setBold(true); columnTitle_->setFont(TitleFont); InspectorRows->addWidget(columnTitle_);
    auto* Content = new QWidget(Inspector); auto* Form = new QFormLayout(Content); contentForm_ = Form;
    Form->setContentsMargins(0, 0, 0, 0); Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    field_ = Choice(Content, "hc.columns.field", {});
    for (const auto& Field : availableFields()) field_->addItem(Field.title_, Field.id_);
    field_->addItem(tr("복합 템플릿"), QString{});
    title_ = new QLineEdit(Content); title_->setObjectName("hc.columns.title");
    alignment_ = Choice(Content, "hc.columns.alignment", {tr("왼쪽"), tr("가운데"), tr("오른쪽")});
    Form->addRow(tr("내용"), field_); Form->addRow(tr("제목"), title_); Form->addRow(tr("글자 정렬"), alignment_);
    templateRow_ = new QWidget(Content); auto* TemplateRow = new QHBoxLayout(templateRow_); TemplateRow->setContentsMargins(0, 0, 0, 0);
    template_ = new QLineEdit(templateRow_); template_->setObjectName("hc.columns.template"); template_->setPlaceholderText("${file.name} · ${file.size}"); template_->setMaxLength(4096);
    auto* Insert = new QPushButton(tr("필드 삽입"), templateRow_); TemplateRow->addWidget(template_, 1); TemplateRow->addWidget(Insert);
    Form->addRow(tr("템플릿"), templateRow_);
    connect(Insert, &QPushButton::clicked, this, [this, Insert] {
        QMenu Menu(this); for (const auto& Field : availableFields()) { auto* Action = Menu.addAction(Field.title_); Action->setData(Field.id_); }
        if (const auto* Action = Menu.exec(Insert->mapToGlobal(QPoint(0, Insert->height())))) template_->insert("${" + Action->data().toString() + "}");
    });
    primary_ = new QCheckBox(tr("주 이름 열로 사용"), Content); primary_->setObjectName("hc.columns.primary");
    splitNameExtension_ = new QCheckBox(tr("한 줄 보기에서 확장자 분리"), Content); splitNameExtension_->setObjectName("hc.columns.splitNameExtension");
    // Board: these two toggles are switches.
    fm::style::setSwitch(primary_); fm::style::setSwitch(splitNameExtension_);
    Form->addRow(primary_); Form->addRow(splitNameExtension_);
    connect(splitNameExtension_, &QCheckBox::toggled, this, [this](bool Checked) { if (!loading_) { draft_.splitNameExtension_ = Checked; dirty_ = true; updatePreview(); } });
    sizeFormat_ = Choice(Content, "hc.columns.sizeFormat", {tr("바이트"), tr("자동 단위"), "KiB", "MiB", "GiB"});
    precision_ = new QSpinBox(Content); precision_->setObjectName("hc.columns.precision"); precision_->setRange(0, 6);
    formatRow_ = new QWidget(Content); auto* Formats = new QHBoxLayout(formatRow_); Formats->setContentsMargins(0, 0, 0, 0);
    sizeFormat_->setMinimumContentsLength(5); Formats->addWidget(sizeFormat_, 1);
    precisionLabel_ = new QLabel(tr("소수"), formatRow_); Formats->addWidget(precisionLabel_); Formats->addWidget(precision_);
    precision_->setAccessibleName(tr("소수 자릿수"));
    grouping_ = new QCheckBox(tr("숫자 자릿수 구분"), Content); grouping_->setObjectName("hc.columns.grouping");
    dateFormat_ = Choice(Content, "hc.columns.dateFormat", {"yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd", "HH:mm:ss", "yyyy/MM/dd HH:mm"}); dateFormat_->setEditable(true);
    utc_ = new QCheckBox(tr("날짜·시간을 UTC로 표시"), Content); utc_->setObjectName("hc.columns.utc");
    Form->addRow(tr("표시 형식"), formatRow_);
    Form->addRow(tr("날짜 형식"), dateFormat_); Form->addRow(utc_); InspectorRows->addWidget(Content);
    auto* Widths = new QGroupBox(tr("너비"), Inspector); auto* WidthForm = new QFormLayout(Widths);
    WidthForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    unit_ = Choice(Widths, "hc.columns.unit", {tr("픽셀"), tr("퍼센트")});
    widthMode_ = Choice(Widths, "hc.columns.widthMode", {tr("가변"), tr("고정")});
    unit_->setMinimumContentsLength(4); widthMode_->setMinimumContentsLength(4);
    auto* WidthChoices = new QHBoxLayout; WidthChoices->addWidget(unit_); WidthChoices->addWidget(widthMode_); WidthForm->addRow(tr("단위·모드"), WidthChoices);
    const auto Number = [&](const char* Name, bool Optional) {
        auto* Result = new WidthNumber(Widths); Result->setObjectName(Name); Result->setAlignment(Qt::AlignCenter);
        Result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); Result->setMinimumWidth(60);
        Result->setDecimals(6); Result->setRange(Optional ? -1 : 0, 1000000); if (Optional) Result->setSpecialValueText(tr("미지정")); return Result;
    };
    width_ = Number("hc.columns.width", false); minimum_ = Number("hc.columns.minimum", true); maximum_ = Number("hc.columns.maximum", true);
    visible_ = new QCheckBox(tr("현재 모드에서 표시"), Widths); visible_->setObjectName("hc.columns.visible");
    auto* Bounds = new QHBoxLayout; Bounds->addWidget(minimum_); Bounds->addWidget(maximum_);
    minimum_->setAccessibleName(tr("최소 너비")); maximum_->setAccessibleName(tr("최대 너비"));
    minimum_->setToolTip(tr("최소 너비 · 미지정이면 하한을 지정하지 않습니다.")); maximum_->setToolTip(tr("최대 너비 · 미지정이면 상한을 지정하지 않습니다."));
    WidthForm->addRow(tr("기준값"), width_);
    InspectorRows->addWidget(Widths);
    auto* AdvancedToggle = new QToolButton(Inspector); AdvancedToggle->setObjectName("hc.columns.advancedToggle"); AdvancedToggle->setText(tr("고급 설정"));
    AdvancedToggle->setCheckable(true); AdvancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); AdvancedToggle->setArrowType(Qt::RightArrow); InspectorRows->addWidget(AdvancedToggle);
    auto* Advanced = new QWidget(Inspector); auto* AdvancedForm = new QFormLayout(Advanced); AdvancedForm->setContentsMargins(0, 0, 0, 0);
    fieldSearch_ = new QLineEdit(Advanced); fieldSearch_->setObjectName("hc.columns.search"); fieldSearch_->setPlaceholderText(tr("필드 이름 또는 식별자 검색"));
    fieldCategory_ = Choice(Advanced, "hc.columns.category", {tr("전체"), tr("내장"), "WDX", "DOpus", "Lua"});
    sortField_ = Choice(Advanced, "hc.columns.sortField", {tr("원래 값 / 템플릿 문자열")});
    for (const auto& Field : availableFields()) sortField_->addItem(Field.title_, Field.id_);
    plusSign_ = new QCheckBox(tr("양수에 + 표시"), Advanced); plusSign_->setObjectName("hc.columns.plusSign");
    providerDisplay_ = new QCheckBox(tr("플러그인 표시 문자열 사용"), Advanced); providerDisplay_->setObjectName("hc.columns.providerDisplay");
    fieldInfo_ = new QLabel(Advanced); fieldInfo_->setObjectName("hc.columns.fieldInfo"); fieldInfo_->setWordWrap(true);
    AdvancedForm->addRow(tr("필드 검색"), fieldSearch_); AdvancedForm->addRow(tr("공급자"), fieldCategory_); AdvancedForm->addRow(tr("정렬에 쓸 값"), sortField_);
    AdvancedForm->addRow(tr("최소·최대"), Bounds); AdvancedForm->addRow(visible_); AdvancedForm->addRow(grouping_);
    AdvancedForm->addRow(plusSign_); AdvancedForm->addRow(providerDisplay_); AdvancedForm->addRow(fieldInfo_); InspectorRows->addWidget(Advanced); Advanced->hide();
    connect(AdvancedToggle, &QToolButton::toggled, this, [Advanced, AdvancedToggle](bool Open) { Advanced->setVisible(Open); AdvancedToggle->setArrowType(Open ? Qt::DownArrow : Qt::RightArrow); });
    InspectorRows->addStretch();
    Split->setStretchFactor(0, 3); Split->setStretchFactor(1, 2); Split->setChildrenCollapsible(false); Split->setSizes({650, 340});
    Work->setStretchFactor(0, 3); Work->setStretchFactor(1, 2); Work->setChildrenCollapsible(false); Work->setSizes({300, 230});
    auto* SortingPage = new QWidget; auto* SortingLayout = new QVBoxLayout(SortingPage);
    auto* Sorting = new QHBoxLayout;
    sort_ = Choice(this, "hc.columns.sort", {}); sortOrder_ = Choice(this, "hc.columns.sortOrder", {tr("오름차순"), tr("내림차순")});
    Sorting->addWidget(new QLabel(tr("정렬 기준"))); Sorting->addWidget(sort_, 1); Sorting->addWidget(sortOrder_); SortingLayout->addLayout(Sorting);
    auto* AddSort = new QPushButton(tr("정렬 추가"), this); AddSort->setObjectName("hc.columns.addSort"); Sorting->addWidget(AddSort);
    auto* RemoveSort = new QPushButton(tr("정렬 제거"), this); Sorting->addWidget(RemoveSort);
    sortRules_ = new QListWidget(this); sortRules_->setObjectName("hc.columns.sortRules"); sortRules_->setMinimumHeight(100); SortingLayout->addWidget(sortRules_, 1);
    auto* Priority = new QHBoxLayout; SortingLayout->addLayout(Priority); Priority->addStretch();
    for (int Direction : {-1, 1}) {
        auto* Move = new QPushButton(Direction < 0 ? tr("우선순위 올림") : tr("우선순위 내림"), this);
        Move->setObjectName(Direction < 0 ? "hc.columns.sortUp" : "hc.columns.sortDown"); Priority->addWidget(Move);
        connect(Move, &QPushButton::clicked, this, [this, Direction] {
            const int Row = sortRules_->currentRow(), Next = Row + Direction;
            if (Row < 0 || Next < 0 || Next >= draft_.sorts_.size()) return;
            draft_.sorts_.swapItemsAt(Row, Next); dirty_ = true; refreshSorts(Next); updatePreview();
        });
    }
    connect(AddSort, &QPushButton::clicked, this, [this] {
        for (const auto& Column : draft_.columns_) if (std::none_of(draft_.sorts_.begin(), draft_.sorts_.end(), [&](const auto& Sort) { return Sort.columnId_ == Column.id_; })) {
            draft_.sorts_.append({Column.id_, Qt::AscendingOrder}); dirty_ = true; refreshSorts(int(draft_.sorts_.size()) - 1); updatePreview(); return;
        }
    });
    connect(RemoveSort, &QPushButton::clicked, this, [this] {
        const int Row = sortRules_->currentRow(); if (Row < 0) return; draft_.sorts_.removeAt(Row); dirty_ = true; refreshSorts(); updatePreview();
    });
    connect(sortRules_, &QListWidget::currentRowChanged, this, [this](int Row) {
        if (loading_ || Row < 0 || Row >= draft_.sorts_.size()) return;
        const QSignalBlocker One(sort_), Two(sortOrder_); sort_->setCurrentIndex(sort_->findData(IdText(draft_.sorts_[Row].columnId_))); sortOrder_->setCurrentIndex(draft_.sorts_[Row].order_ == Qt::DescendingOrder);
    });
    AddPage(SortingPage, tr("정렬"));
    auto* Policy = new fm::ui::Card(this); auto* PolicyRows = new QVBoxLayout(Policy);
    autoSelection_ = new QCheckBox(tr("열 세트 자동 적용 · 폴더 경로 또는 그룹 비율"), Policy); autoSelection_->setObjectName("hc.columns.autoEnabled"); PolicyRows->addWidget(autoSelection_);
    automaticTab_ = new QCheckBox(tr("적용할 탭에서 자동 선택 사용 · 수동 선택 해제"), Policy); automaticTab_->setObjectName("hc.columns.automaticTab"); PolicyRows->addWidget(automaticTab_);
    autoRules_ = new QTableWidget(Policy); autoRules_->setObjectName("hc.columns.autoRules"); autoRules_->setColumnCount(5); autoRules_->setHorizontalHeaderLabels({tr("사용"), tr("열 세트"), tr("폴더 경로 (* · ?)"), tr("파일 그룹"), tr("비율 이상")});
    autoRules_->setProperty("hc.cellControls", true); autoRules_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); autoRules_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); autoRules_->setMinimumHeight(100);  PolicyRows->addWidget(autoRules_, 1);
    auto* RuleActions = new QHBoxLayout; PolicyRows->addLayout(RuleActions);
    const auto RuleAction = [&](const QString& Text, const QString& Name, std::function<void()> Action) {
        auto* Button = new QPushButton(Text, Policy); Button->setObjectName(Name); Button->setAutoDefault(false); RuleActions->addWidget(Button); connect(Button, &QPushButton::clicked, this, Action);
    };
    RuleAction(tr("조건 추가"), "hc.columns.autoAdd", [this] { if (autoRules_->rowCount() < 128) { appendAutoRule({{"enabled", true}, {"profile", IdText(draft_.id_)}, {"path", "*"}, {"group", ""}, {"percent", 50}}); dirty_ = true; } });
    RuleAction(tr("조건 삭제"), "hc.columns.autoRemove", [this] { if (autoRules_->currentRow() >= 0) { autoRules_->removeRow(autoRules_->currentRow()); dirty_ = true; } });
    const auto Move = [this](int Direction) {
        const int Row = autoRules_->currentRow(), Next = Row + Direction; if (Row < 0 || Next < 0 || Next >= autoRules_->rowCount()) return;
        const auto Saved = AutoSelection(); auto Values = Saved["rules"].toArray(); const auto Current = Values[Row]; Values[Row] = Values[Next]; Values[Next] = Current;
        autoRules_->setRowCount(0); for (const auto& Value : Values) appendAutoRule(Value.toObject()); autoRules_->selectRow(Next); dirty_ = true;
    };
    RuleAction(tr("위로"), "hc.columns.autoUp", [Move] { Move(-1); }); RuleAction(tr("아래로"), "hc.columns.autoDown", [Move] { Move(1); }); RuleActions->addStretch();
    connect(autoSelection_, &QCheckBox::toggled, this, [this] { if (!loading_) dirty_ = true; });
    connect(automaticTab_, &QCheckBox::toggled, this, [this] { if (!loading_) dirty_ = true; });
    auto* PolicyNote = new QLabel(tr("위에서 처음 일치하는 조건을 적용합니다. 경로와 그룹을 함께 지정하면 둘 다 만족해야 합니다. 비율은 폴더 전체 파일 기준이며 하위 폴더는 제외합니다. 수동 선택한 탭은 자동 전환하지 않습니다."), Policy); PolicyNote->setWordWrap(true); PolicyRows->addWidget(PolicyNote); pages_->addTab(Policy, tr("자동 전환"));
    auto* Lua = new QWidget; auto* LuaForm = new QFormLayout(Lua); LuaForm->setRowWrapPolicy(QFormLayout::WrapAllRows); LuaForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    scripts_ = Choice(Lua, "hc.columns.scripts", {});
    auto* ScriptBar = new QHBoxLayout; ScriptBar->addWidget(scripts_, 1);
    auto* AddScript = new QPushButton(tr("추가"), Lua); AddScript->setObjectName("hc.columns.addScript"); ScriptBar->addWidget(AddScript);
    auto* RemoveScript = new QPushButton(tr("삭제"), Lua); ScriptBar->addWidget(RemoveScript); LuaForm->addRow(tr("Lua 정의"), ScriptBar);
    scriptTitle_ = new QLineEdit(Lua); scriptTitle_->setObjectName("hc.columns.scriptTitle");
    scriptEnabled_ = new QCheckBox(tr("계산 사용"), Lua); scriptEnabled_->setObjectName("hc.columns.scriptEnabled");
    dependencies_ = new QLineEdit(Lua); dependencies_->setObjectName("hc.columns.dependencies"); dependencies_->setPlaceholderText("file.extension, file.isDirectory");
    scriptOutputs_ = new QPlainTextEdit(Lua); scriptOutputs_->setObjectName("hc.columns.scriptOutputs"); scriptOutputs_->setMaximumHeight(90);
    scriptSource_ = new QPlainTextEdit(Lua); scriptSource_->setObjectName("hc.columns.scriptSource"); scriptSource_->setMinimumHeight(130);
    scriptSource_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    LuaForm->addRow(tr("제목"), scriptTitle_); LuaForm->addRow(scriptEnabled_); LuaForm->addRow(tr("의존 필드 (쉼표 구분)"), dependencies_);
    LuaForm->addRow(tr("출력: 이름 | 자료형 | 제목"), scriptOutputs_);
    auto* Types = new QLabel(tr("자료형: text, integer, unsigned, real, boolean, date, time, timestamp"), Lua); Types->setWordWrap(true); LuaForm->addRow(Types);
    LuaForm->addRow(tr("계산 함수"), scriptSource_);
    auto* Help = new QLabel(tr("return function(item) ... end 형식으로 작성합니다. item[\"file.size\"]처럼 선언한 의존 값을 읽고 {출력이름=값}을 반환합니다. 파일·명령 실행 API는 제공하지 않습니다. nil은 빈 값이며 큰 unsigned 정수는 문자열로 전달됩니다. 한 번의 계산으로 모든 출력을 반환합니다."), Lua);
    Help->setWordWrap(true); LuaForm->addRow(Help); AddPage(Lua, tr("계산 필드"));
    connect(AddScript, &QPushButton::clicked, this, [this] {
        ScriptDefinition Script; Script.title_ = tr("파일 분류"); Script.dependencies_ = {"file.extension", "file.isDirectory"};
        Script.outputs_ = {{"class", tr("분류"), FieldType::Text}, {"order", tr("순서"), FieldType::Integer}};
        Script.source_ = "return function(item)\n  local folder = item[\"file.isDirectory\"]\n  local ext = string.lower(item[\"file.extension\"] or \"\")\n  return {class = folder and \"폴더\" or (ext == \"iso\" and \"디스크 이미지\" or \"일반 파일\"), order = folder and 0 or 1}\nend";
        draft_.scripts_.append(Script); dirty_ = true; refreshScripts(int(draft_.scripts_.size()) - 1); refreshFields(); loadColumn(row_);
    });
    connect(RemoveScript, &QPushButton::clicked, this, [this] {
        const int Row = scripts_->currentIndex(); if (Row < 0) return; draft_.scripts_.removeAt(Row); dirty_ = true; refreshScripts(); refreshFields(); loadColumn(row_);
    });
    connect(scripts_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) loadScript(); });
    connect(scriptTitle_, &QLineEdit::textEdited, this, [this] { storeScript(); });
    connect(dependencies_, &QLineEdit::textEdited, this, [this] { storeScript(); });
    connect(scriptSource_, &QPlainTextEdit::textChanged, this, [this] { storeScript(); });
    connect(scriptOutputs_, &QPlainTextEdit::textChanged, this, [this] { storeScript(); });
    connect(scriptEnabled_, &QCheckBox::toggled, this, [this] { storeScript(); });
    auto* Samples = new QHBoxLayout; PreviewRows->addLayout(Samples);
    auto* PreviewTitle = new QLabel(tr("미리보기"), PreviewPanel); PreviewTitle->setFont(TitleFont); Samples->addWidget(PreviewTitle);
    sampleMode_ = Choice(PreviewPanel, "hc.columns.sampleMode", {tr("예제"), tr("현재 폴더 (최대 50개)"), tr("선택 항목"), tr("경로로 지정")}); Samples->addWidget(sampleMode_, 1);
    sampleMode_->setMinimumContentsLength(7);
    auto* Calculate = new QPushButton(tr("값 계산"), PreviewPanel); Calculate->setObjectName("hc.columns.calculate"); Samples->addWidget(Calculate);
    auto* Stop = new QPushButton(tr("중지"), PreviewPanel); Stop->setObjectName("hc.columns.stopPreview"); Samples->addWidget(Stop);
    auto* Options = new QToolButton(PreviewPanel); Options->setText(tr("옵션")); Options->setCheckable(true); Options->setObjectName("hc.columns.previewOptionsToggle"); Samples->addWidget(Options);
    auto* PreviewOptions = new QWidget(PreviewPanel); auto* OptionRows = new QHBoxLayout(PreviewOptions); OptionRows->setContentsMargins(0, 0, 0, 0);
    OptionRows->addWidget(new QLabel(tr("폭"), PreviewOptions)); previewWidth_ = new QSpinBox(PreviewOptions); previewWidth_->setObjectName("hc.columns.previewWidth"); previewWidth_->setRange(0, 8000); previewWidth_->setSpecialValueText(tr("창 너비")); OptionRows->addWidget(previewWidth_);
    extractSamples_ = new QCheckBox(tr("가상 파일을 임시 파일로 풀어 조회 (파일당 64 MiB 이하)"), PreviewOptions); extractSamples_->setObjectName("hc.columns.extractSamples"); OptionRows->addWidget(extractSamples_, 1);
    PreviewRows->addWidget(PreviewOptions); PreviewOptions->hide(); connect(Options, &QToolButton::toggled, PreviewOptions, &QWidget::setVisible);
    connect(previewWidth_, &QSpinBox::valueChanged, this, &QtColumnProfileEditor::layoutPreview);
    auto* ManualPanel = new QWidget(PreviewPanel); auto* Manual = new QHBoxLayout(ManualPanel); Manual->setContentsMargins(0, 0, 0, 0); PreviewRows->addWidget(ManualPanel);
    samplePath_ = new QLineEdit(ManualPanel); samplePath_->setObjectName("hc.columns.samplePath"); samplePath_->setPlaceholderText(tr("미리 볼 파일 또는 폴더의 전체 경로")); Manual->addWidget(samplePath_, 1);
    auto* ReadSample = new QPushButton(tr("표본 읽기"), ManualPanel); ReadSample->setObjectName("hc.columns.readSample"); Manual->addWidget(ReadSample); ManualPanel->hide();
    previewQueries_ = std::make_unique<QueryEngine>(QueryEngine::Loader{}, this); previewQueries_->Changed = [this](const auto&) { updatePreview(); };
    connect(Calculate, &QPushButton::clicked, this, &QtColumnProfileEditor::startPreview);
    connect(Stop, &QPushButton::clicked, this, [this] { stopPreview(); updatePreview(); });
    connect(ReadSample, &QPushButton::clicked, this, &QtColumnProfileEditor::loadManualSamples);
    connect(sampleMode_, &QComboBox::currentIndexChanged, this, [this, ManualPanel] { stopPreview(); ++manualGeneration_; ManualPanel->setVisible(sampleMode_->currentIndex() == 3); updatePreview(); });
    connect(extractSamples_, &QCheckBox::toggled, this, [this] { stopPreview(); updatePreview(); });
    preview_ = new QTableWidget(this); preview_->setObjectName("hc.columns.preview"); preview_->setEditTriggers(QAbstractItemView::NoEditTriggers); preview_->verticalHeader()->hide(); preview_->hide();
    previewGrid_ = new Qtitan::Grid(PreviewPanel); previewGrid_->setObjectName("hc.columns.previewGrid"); previewGrid_->setProperty("hc.fileGrid", true); previewGrid_->setViewType(Qtitan::Grid::BandedTableView);
    auto* PreviewView = previewGrid_->view<Qtitan::GridBandedTableView>(); PreviewView->options().setAutoCreateColumns(false); PreviewView->options().setGroupsHeader(false);
    PreviewView->options().setColumnMovingEnabled(false); PreviewView->options().setFieldChooserEnabled(false); PreviewView->options().setGridLines(Qtitan::LinesNone);
    // A sample table, not a file panel: no cursor or focus frame, the selected column is tinted instead.
    PreviewView->options().setFocusFrameEnabled(false); PreviewView->options().setShowFocusDecoration(false);
    PreviewView->setFileManagerMode(true); PreviewView->options().setModelDecoration(true); PreviewView->options().setRowAutoHeight(true); PreviewView->tableOptions().setColumnsQuickCustomization(false);
    PreviewView->bandedOptions().setBandsHeader(false); PreviewView->bandedOptions().setBandsQuickCustomization(false); PreviewView->addBand("Preview");
    previewAdapter_ = new QtitanColumnAdapter(this, previewGrid_, PreviewView, true);
    previewFrame_ = new QScrollArea(PreviewPanel); previewFrame_->setObjectName("hc.columns.previewFrame"); previewFrame_->setFrameShape(QFrame::NoFrame); previewFrame_->setWidget(previewGrid_); previewFrame_->setMinimumHeight(100); PreviewRows->addWidget(previewFrame_, 1);
    previewFrame_->viewport()->installEventFilter(this);
    // Defer selection until Qtitan finishes dispatching its event: loading a different
    // column rebuilds the preview and destroys the header that emitted the signal.
    const auto QueueColumn = [this](const QUuid& Id) {
        if (!updatingPreview_ && !Id.isNull()) QTimer::singleShot(0, this, [this, Id] { selectPreviewColumn(Id); });
    };
    connect(PreviewView, &Qtitan::GridViewBase::columnClicked, this, [QueueColumn](Qtitan::ColumnClickEventArgs* Args) { QueueColumn(Args->column()->property("hc.columnId").toUuid()); });
    connect(PreviewView, &Qtitan::GridViewBase::cellClicked, this, [this, QueueColumn](Qtitan::CellClickEventArgs* Args) {
        const auto Index = Args->cell().modelIndex(); if (!Index.isValid() || updatingPreview_) return;
        preview_->setCurrentCell(Index.row(), Index.column()); QueueColumn(previewColumnIds_.value(Index.column()));
    });
    connect(PreviewView, &Qtitan::GridViewBase::focusRowChanged, this, [this, PreviewView](int, int Row) {
        if (updatingPreview_ || Row < 0 || Row >= preview_->rowCount()) return;
        const auto Index = PreviewView->getRow(Row).modelIndex(std::max(0, preview_->currentColumn())); if (Index.isValid()) preview_->setCurrentCell(Index.row(), Index.column());
    });
    connect(PreviewView, &Qtitan::GridViewBase::focusColumnChanged, this, [this, PreviewView, QueueColumn](int, int Column) {
        if (updatingPreview_ || Column < 0 || Column >= previewColumnIds_.size()) return;
        QueueColumn(previewColumnIds_[Column]);
        const auto Index = PreviewView->focusedRow().modelIndex(Column); if (Index.isValid()) preview_->setCurrentCell(Index.row(), Index.column());
    });
    auto* PreviewStatus = new QHBoxLayout; PreviewRows->addLayout(PreviewStatus);
    sampleStatus_ = new QLabel(PreviewPanel); sampleStatus_->setObjectName("hc.columns.sampleStatus"); sampleStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); PreviewStatus->addWidget(sampleStatus_, 1);
    auto* DetailToggle = new QToolButton(PreviewPanel); DetailToggle->setObjectName("hc.columns.detailToggle"); DetailToggle->setText(tr("값 상세")); DetailToggle->setCheckable(true); DetailToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); DetailToggle->setArrowType(Qt::RightArrow); PreviewStatus->addWidget(DetailToggle);
    valueDetail_ = new QLabel(PreviewPanel); valueDetail_->setObjectName("hc.columns.valueDetail"); valueDetail_->setWordWrap(true); valueDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse); PreviewRows->addWidget(valueDetail_); valueDetail_->hide();
    connect(DetailToggle, &QToolButton::toggled, this, [this, DetailToggle](bool Open) { valueDetail_->setVisible(Open); DetailToggle->setArrowType(Open ? Qt::DownArrow : Qt::RightArrow); });
    connect(preview_, &QTableWidget::currentCellChanged, this, [this](int Row, int Column) { if (const auto* Cell = preview_->item(Row, Column)) valueDetail_->setText(Cell->toolTip()); });
    auto* Footer = new QHBoxLayout; Outer->addLayout(Footer);
    scope_ = Choice(this, "hc.columns.scope", {tr("현재 탭"), tr("현재 탭 + 이 패널의 새 탭 기본값"), tr("양쪽 패널의 현재 탭"), tr("이 패널의 새 탭 기본값만")});
    scope_->setMinimumWidth(210);
    Footer->addWidget(new QLabel(tr("적용 대상"), this)); Footer->addWidget(scope_);
    summary_ = new QLabel(this); summary_->setObjectName("hc.columns.summary"); summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Footer->addWidget(summary_, 1);
    error_ = new QLabel(this); error_->setObjectName("hc.columns.error"); error_->setWordWrap(true); Outer->addWidget(error_);
    connect(profileList_, &QListWidget::currentRowChanged, this, [this](int Row) {
        if (loading_ || Row < 0) return;
        const auto Next = DecodeProfile(library_[profileList_->item(Row)->data(Qt::UserRole).toString()].toObject());
        if (!Next || !IsSupportedProfile(*Next)) return;
        const auto Error = Validate(draft_);
        if (!Error.isEmpty()) {
            // Selecting another profile must not discard an invalid edit.
            const QSignalBlocker Blocker(profileList_);
            for (int Index = 0; Index < profileList_->count(); ++Index)
                if (profileList_->item(Index)->data(Qt::UserRole).toString() == IdText(draft_.id_)) {
                    profileList_->setCurrentRow(Index); break;
                }
            ValidateDraft();
            if (Error == "width") minimum_->setFocus(Qt::OtherFocusReason);
            return;
        }
        library_[IdText(draft_.id_)] = EncodeProfile(draft_);
        draft_ = *Next; automaticTab_->setChecked(false); dirty_ = true; refreshProfiles(); rebuildList(0);
    });
    connect(fieldSearch_, &QLineEdit::textChanged, this, [this] { refreshFields(); loadColumn(row_); });
    connect(fieldCategory_, &QComboBox::currentIndexChanged, this, [this] { refreshFields(); loadColumn(row_); });
    for (auto* Check : {grouping_, plusSign_, utc_, providerDisplay_}) connect(Check, &QCheckBox::toggled, this, [this] { storeColumn(); });
    connect(columns_, &QListWidget::currentRowChanged, this, &QtColumnProfileEditor::loadColumn);
    connect(columns_, &QListWidget::itemChanged, this, [this](QListWidgetItem* Item) {
        if (loading_) return;
        const int Row = columns_->row(Item);
        if (Row < 0 || Row >= draft_.columns_.size()) return;
        auto& Width = draft_.columns_[Row].widths_[int(draft_.mode_)];
        const bool Hidden = Item->checkState() != Qt::Checked;
        if (Width.hidden_ == Hidden) return;
        Width.hidden_ = Hidden; dirty_ = true;
        if (Row == row_) { const QSignalBlocker Block(visible_); visible_->setChecked(!Hidden); }
        updatePreview();
    });
    for (auto* Combo : {field_, alignment_, sizeFormat_, unit_, widthMode_, sortField_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] { storeColumn(); });
    connect(dateFormat_, &QComboBox::currentTextChanged, this, [this] { storeColumn(); });
    connect(title_, &QLineEdit::textEdited, this, [this] { storeColumn(); });
    connect(template_, &QLineEdit::textChanged, this, [this] { storeColumn(); });
    connect(precision_, &QSpinBox::valueChanged, this, [this] { storeColumn(); });
    for (auto* Number : {width_, minimum_, maximum_}) connect(Number, &QDoubleSpinBox::valueChanged, this, [this] { storeColumn(); });
    for (auto* Check : {primary_, visible_}) connect(Check, &QCheckBox::toggled, this, [this] { storeColumn(); });
    connect(layout_, &QComboBox::currentIndexChanged, this, [this](int Index) { if (loading_) return; draft_.mode_ = static_cast<LayoutMode>(Index); dirty_ = true; loadColumn(row_); });
    connect(scope_, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) dirty_ = true; });
    for (auto* Combo : {sort_, sortOrder_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        const auto Id = QUuid(sort_->currentData().toString()); const int Row = sortRules_->currentRow();
        if (Id.isNull()) { if (Row >= 0) draft_.sorts_.removeAt(Row); }
        else {
            for (int Index = 0; Index < draft_.sorts_.size(); ++Index) if (Index != Row && draft_.sorts_[Index].columnId_ == Id) { error_->setText(tr("이미 정렬 기준에 있습니다.")); return; }
            SortDefinition Sort{Id, sortOrder_->currentIndex() ? Qt::DescendingOrder : Qt::AscendingOrder};
            if (Row < 0) draft_.sorts_.append(Sort); else draft_.sorts_[Row] = Sort;
        }
        dirty_ = true; refreshSorts(std::max(0, Row)); updatePreview();
    });
    connect(profileTitle_, &QLineEdit::textEdited, this, [this](const QString& Text) {
        draft_.title_ = Text;
        if (auto* Item = profileList_->currentItem()) Item->setText(Text);
        dirty_ = true; updatePreview();
    });
    connect(Copy, &QPushButton::clicked, this, &QtColumnProfileEditor::cloneProfile);
    connect(Delete, &QPushButton::clicked, this, [this] {
        library_.remove(IdText(draft_.id_)); draft_ = DecodeProfile(library_[IdText(BuiltInProfileId)].toObject()).value_or(DefaultProfile()); library_[IdText(draft_.id_)] = EncodeProfile(draft_); dirty_ = true; refreshProfiles(); rebuildList(0);
    });
    auto* Reload = new QPushButton(tr("플러그인 필드 새로 읽기"), this); Reload->setObjectName("hc.columns.reloadProviders"); AdvancedForm->addRow(Reload);
    connect(Reload, &QPushButton::clicked, &PluginFields::Instance(), &PluginFields::Refresh);
    connect(&PluginFields::Instance(), &PluginFields::Changed, this, [this] {
        stopPreview(); previewQueries_->Reset(); refreshFields(); loadColumn(row_);
        if (!PluginFields::Instance().Status().isEmpty()) error_->setText(PluginFields::Instance().Status());
    });
    Load(DefaultProfile(), {});
}

QVector<FieldDefinition> QtColumnProfileEditor::availableFields() const
{
    auto Fields = PluginFields::Instance().Fields(); Fields += contextFields_; Fields += ScriptFields(draft_); return Fields;
}
void QtColumnProfileEditor::SetContextFields(QVector<FieldDefinition> Fields)
{
    contextFields_.clear();
    for (const auto& Field : Fields) if (IsVfsColumn(Field.id_)) contextFields_.append(Field);
    const QSignalBlocker One(field_), Two(sortField_);
    field_->clear(); sortField_->clear(); sortField_->addItem(tr("원래 값 / 템플릿 문자열"));
    for (const auto& Field : availableFields()) { field_->addItem(Field.title_, Field.id_); sortField_->addItem(Field.title_, Field.id_); }
    field_->addItem(tr("복합 템플릿"), QString{});
}

void QtColumnProfileEditor::Load(const ColumnProfile& Profile, const QJsonObject& Library, bool Writable)
{
    draft_ = Profile; library_ = Library; writable_ = Writable; dirty_ = false;
    // New libraries offer Details only; saved reference/custom sets stay intact.
    if (!library_.contains(IdText(BuiltInProfileId))) library_[IdText(BuiltInProfileId)] = EncodeProfile(DefaultProfile());
    { const QSignalBlocker Block(scope_); scope_->setCurrentIndex(0); }
    library_[IdText(draft_.id_)] = EncodeProfile(draft_);
    refreshProfiles(); rebuildList(0); setEnabled(Writable);
    if (!Writable) error_->setText(tr("지원하지 않는 컬럼 설정을 보존하고 있습니다. 편집할 수 없습니다."));
}

void QtColumnProfileEditor::refreshProfiles()
{
    const QScopedValueRollback Loading(loading_, true);
    profileList_->clear();
    for (auto It = library_.begin(); It != library_.end(); ++It)
        if (const auto Profile = DecodeProfile(It.value().toObject()); Profile && IsSupportedProfile(*Profile)) {
            auto* Item = new QListWidgetItem(Profile->title_, profileList_); Item->setData(Qt::UserRole, It.key());
            if (Profile->id_ == draft_.id_) profileList_->setCurrentItem(Item);
        }
}

void QtColumnProfileEditor::rebuildList(int Row)
{
    refreshScripts(); refreshFields();
    loading_ = true; columns_->clear(); sort_->clear(); sort_->addItem(tr("지정 안 함"), QString{});
    profileTitle_->setText(draft_.title_); layout_->setCurrentIndex(int(draft_.mode_));
    deleteProfile_->setEnabled(!IsBuiltInProfile(draft_.id_));
    for (const auto& Column : draft_.columns_) {
        columns_->addItem(Column.title_); refreshColumnItem(columns_->count() - 1);
        sort_->addItem(Column.title_, IdText(Column.id_));
    }
    if (!draft_.sorts_.isEmpty()) { sort_->setCurrentIndex(sort_->findData(IdText(draft_.sorts_.front().columnId_))); sortOrder_->setCurrentIndex(draft_.sorts_.front().order_ == Qt::DescendingOrder); }
    columns_->setCurrentRow(Row); loading_ = false; refreshSorts(); loadColumn(Row);
}

void QtColumnProfileEditor::loadColumn(int Row)
{
    if (loading_ || Row < 0 || Row >= draft_.columns_.size()) return;
    loading_ = true; row_ = Row; const auto& Column = draft_.columns_[Row]; const auto& Width = Column.widths_[int(draft_.mode_)];
    if (Column.template_.isEmpty() && field_->findData(Column.fieldId_) < 0) {
        QString Label = tr("사용 불가 · %1").arg(Column.fieldId_);
        for (const auto& Definition : availableFields()) if (Definition.id_ == Column.fieldId_) Label = tr("현재 선택 · %1").arg(Definition.title_);
        field_->addItem(Label, Column.fieldId_);
    }
    if (!Column.sortField_.isEmpty() && sortField_->findData(Column.sortField_) < 0) sortField_->addItem(tr("사용 불가 · %1").arg(Column.sortField_), Column.sortField_);
    field_->setCurrentIndex(field_->findData(Column.template_.isEmpty() ? Column.fieldId_ : QString{})); title_->setText(Column.title_);
    template_->setText(Column.template_); template_->setEnabled(!Column.template_.isEmpty());
    sortField_->setCurrentIndex(std::max(0, sortField_->findData(Column.sortField_)));
    alignment_->setCurrentIndex(Column.alignment_ == Qt::AlignLeft ? 0 : Column.alignment_ == Qt::AlignRight ? 2 : 1);
    splitNameExtension_->setChecked(draft_.splitNameExtension_);
    primary_->setChecked(Column.id_ == draft_.primaryNameId_); primary_->setEnabled(Column.fieldId_ == "file.name" || Column.fieldId_ == "file.stem");
    sizeFormat_->setCurrentIndex(int(Column.format_.size_)); precision_->setValue(Column.format_.precision_); dateFormat_->setCurrentText(Column.format_.datePattern_);
    grouping_->setChecked(Column.format_.grouping_); plusSign_->setChecked(Column.format_.plusSign_); utc_->setChecked(Column.format_.utc_); providerDisplay_->setChecked(Column.format_.providerDisplay_);
    const auto Fields = availableFields(); const auto Field = std::find_if(Fields.begin(), Fields.end(), [&](const auto& Item) { return Item.id_ == Column.fieldId_; });
    const bool Composite = !Column.template_.isEmpty(); const bool Numeric = Field != Fields.end() && (Field->type_ == FieldType::Integer || Field->type_ == FieldType::UnsignedInteger || Field->type_ == FieldType::Real);
    const bool Date = Field != Fields.end() && (Field->type_ == FieldType::Date || Field->type_ == FieldType::Time || Field->type_ == FieldType::Timestamp);
    columnTitle_->setText(tr("%1 열 편집").arg(Column.title_));
    contentForm_->setRowVisible(templateRow_, Composite);
    contentForm_->setRowVisible(primary_, primary_->isEnabled());
    contentForm_->setRowVisible(splitNameExtension_, Column.id_ == draft_.primaryNameId_ && draft_.mode_ == LayoutMode::Details);
    contentForm_->setRowVisible(formatRow_, Composite || Numeric);
    sizeFormat_->setVisible(Composite || (Field != Fields.end() && Field->type_ == FieldType::UnsignedInteger));
    precision_->setVisible(Composite || Numeric); precisionLabel_->setVisible(Composite || Numeric); grouping_->setVisible(Composite || Numeric);
    contentForm_->setRowVisible(dateFormat_, Composite || Date); contentForm_->setRowVisible(utc_, Composite || Date);
    sizeFormat_->setEnabled(Composite || (Field != Fields.end() && Field->type_ == FieldType::UnsignedInteger)); precision_->setEnabled(Composite || Numeric);
    grouping_->setEnabled(Composite || Numeric); plusSign_->setEnabled(Composite || Numeric); dateFormat_->setEnabled(Composite || Date); utc_->setEnabled(Composite || Date);
    const QStringList Types{tr("문자열"), tr("정수"), tr("부호 없는 정수"), tr("실수"), tr("참/거짓"), tr("날짜"), tr("시간"), tr("날짜·시간")};
    fieldInfo_->setText(Field == Fields.end() ? (Composite ? tr("템플릿: 참조 필드의 형식을 조합합니다.") : tr("현재 위치에서 사용할 수 없는 필드입니다.")) :
        Field->id_ + " · " + Types.value(int(Field->type_)) + " · " + (Field->cost_ == EvaluationCost::Snapshot ? tr("목록 값: 즉시 표시") : tr("비동기 값: 값 계산을 눌러 조회")));
    unit_->setCurrentIndex(int(Width.unit_)); widthMode_->setCurrentIndex(int(Width.mode_)); width_->setValue(Width.value_);
    minimum_->setValue(Width.minimum_.value_or(-1)); maximum_->setValue(Width.maximum_.value_or(-1)); visible_->setChecked(!Width.hidden_);
    for (int Index = 0; Index < draft_.columns_.size(); ++Index) refreshColumnItem(Index);
    loading_ = false; updatePreview();
}

void QtColumnProfileEditor::refreshColumnItem(int Row)
{
    auto* Item = columns_->item(Row); if (!Item || Row < 0 || Row >= draft_.columns_.size()) return;
    const QSignalBlocker Block(columns_);
    const auto& Column = draft_.columns_[Row]; const auto& Width = Column.widths_[int(draft_.mode_)];
    Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
    Item->setText(Column.title_); Item->setCheckState(Width.hidden_ ? Qt::Unchecked : Qt::Checked);
    Item->setData(Qt::UserRole, Column.id_);
    Item->setData(Qt::UserRole + 1, Column.template_.isEmpty() ? Column.fieldId_ : tr("복합 템플릿"));
    const auto WidthText = QString::number(Width.value_, 'g', 6) + (Width.unit_ == hc::view::WidthUnit::Percent ? "%" : " px");
    Item->setData(Qt::UserRole + 2, WidthText);
    Item->setData(Qt::UserRole + 3, Column.alignment_ == Qt::AlignLeft ? tr("왼쪽") : Column.alignment_ == Qt::AlignRight ? tr("오른쪽") : tr("가운데"));
    Item->setToolTip(tr("%1\n%2\n너비: %3 · %4").arg(Column.title_, Column.fieldId_, WidthText, Width.mode_ == hc::view::WidthMode::Fixed ? tr("고정") : tr("가변")));
    Item->setData(Qt::AccessibleTextRole, tr("%1 · %2 · %3 · %4").arg(Column.title_, Column.fieldId_, WidthText, Width.hidden_ ? tr("숨김") : tr("표시")));
}

void QtColumnProfileEditor::selectPreviewColumn(const QUuid& Id)
{
    if (updatingPreview_ || loading_) return;
    // Model column zero remains the primary name regardless of visual order;
    // UUIDs also distinguish duplicate fields and survive moves and hiding.
    for (int Row = 0; Row < draft_.columns_.size(); ++Row)
        if (draft_.columns_[Row].id_ == Id) {
            if (columns_->currentRow() != Row) columns_->setCurrentRow(Row);
            return;
        }
}

void QtColumnProfileEditor::storeColumn()
{
    if (loading_ || row_ < 0) return;
    auto& Column = draft_.columns_[row_]; const auto Field = field_->currentData().toString();
    if (Column.id_ == draft_.primaryNameId_ && Field != "file.name" && Field != "file.stem") {
        const QSignalBlocker Block(field_); field_->setCurrentIndex(field_->findData(Column.fieldId_));
        error_->setText(tr("주 이름은 확장자 포함 이름 또는 확장자 제외 이름이어야 합니다.")); return;
    }
    const bool FieldChanged = Column.fieldId_ != Field;
    const auto PreviousField = Column.fieldId_;
    Column.fieldId_ = Field; Column.title_ = title_->text();
    Column.template_ = Field.isEmpty() ? (FieldChanged ? QString("${file.name} · ${file.size}") : template_->text()) : QString{};
    Column.sortField_ = sortField_->currentData().toString();
    if (FieldChanged) {
        const auto Fields = availableFields();
        for (const auto& Old : Fields) if (Old.id_ == PreviousField && Column.title_ == Old.title_) {
            Column.title_ = field_->currentText(); break;
        }
    }
    Column.alignment_ = alignment_->currentIndex() == 0 ? Qt::AlignLeft : alignment_->currentIndex() == 2 ? Qt::AlignRight : Qt::AlignCenter;
    Column.format_ = {static_cast<SizeFormat>(sizeFormat_->currentIndex()), precision_->value(), dateFormat_->currentText(), grouping_->isChecked(), plusSign_->isChecked(), utc_->isChecked(), providerDisplay_->isChecked()};
    const bool NewPrimary = primary_->isChecked() && draft_.primaryNameId_ != Column.id_ && (Field == "file.name" || Field == "file.stem");
    if (NewPrimary) draft_.primaryNameId_ = Column.id_;
    if (draft_.primaryNameId_ == Column.id_ && !primary_->isChecked()) {
        const QSignalBlocker Block(primary_); primary_->setChecked(true);
    }
    auto& Width = Column.widths_[int(draft_.mode_)];
    Width = {static_cast<hc::view::WidthUnit>(unit_->currentIndex()), static_cast<hc::view::WidthMode>(widthMode_->currentIndex()), width_->value()};
    if (minimum_->value() >= 0) Width.minimum_ = minimum_->value(); if (maximum_->value() >= 0) Width.maximum_ = maximum_->value();
    Width.hidden_ = !visible_->isChecked();
    if (NewPrimary) for (int Mode : {1, 2}) Column.widths_[Mode] = {hc::view::WidthUnit::Percent, hc::view::WidthMode::Flexible, 100};
    if (FieldChanged) {
        Column.format_ = {};
        for (const auto& Definition : availableFields()) if (Definition.id_ == Field && Definition.defaultAlignment_) Column.alignment_ = Definition.defaultAlignment_;
        for (auto& Sort : draft_.sorts_) if (Sort.columnId_ == Column.id_) Sort.order_ = PluginFields::Instance().Descending(Field) ? Qt::DescendingOrder : Qt::AscendingOrder;
        refreshSorts();
    }
    refreshColumnItem(row_); columnTitle_->setText(tr("%1 열 편집").arg(Column.title_)); dirty_ = true;
    const auto SortRow = sort_->findData(IdText(Column.id_)); if (SortRow >= 0) sort_->setItemText(SortRow, Column.title_);
    if (Validate(draft_).isEmpty()) error_->clear();
    if (FieldChanged || NewPrimary) loadColumn(row_); else updatePreview();
}

bool QtColumnProfileEditor::ValidateDraft()
{
    if (autoSelection_->isEnabled()) {
        const auto AutoError = ValidateAutoSelection(AutoSelection(), Library(), autoColors_);
        if (!AutoError.isEmpty()) { error_->setText(AutoError); return false; }
    }
    const auto ScriptError = ValidateScripts(draft_);
    if (!ScriptError.isEmpty()) { error_->setText(tr("Lua 정의·출력·의존 필드를 확인하세요: %1").arg(ScriptError)); return false; }
    for (const auto& Script : draft_.scripts_) {
        const auto Syntax = hc::script::CheckLuaSyntax(Script.source_.toStdString());
        if (!Syntax.HasValue()) { error_->setText(Script.title_ + ": " + QString::fromStdString(Syntax.Failure().code_)); return false; }
    }
    for (const auto& Column : draft_.columns_) {
        const auto Compiled = CompileTemplate(Column.template_);
        if (!Compiled.Valid()) { error_->setText(tr("%1 · %2번째 문자: %3").arg(Column.title_).arg(Compiled.errorOffset_ + 1).arg(Compiled.error_)); return false; }
    }
    if (!IsSupportedProfile(draft_)) { error_->setText(tr("필드 참조·정렬 값·너비 설정을 확인하세요.")); return false; }
    const auto Error = Validate(draft_);
    if (!Error.isEmpty()) {
        error_->setText(Error == "width" ? tr("너비를 확인하세요. 퍼센트는 0~100이며, 최소값은 최대값 이하이어야 합니다.")
            : Error == "primary-hidden" ? tr("두 줄 모드에서는 주 이름 컬럼을 표시해야 합니다.") : tr("이름 컬럼, 표시 여부 또는 형식 설정을 확인하세요.")); return false;
    }
    if (draft_.title_.trimmed().isEmpty()) { error_->setText(tr("프로필 이름을 입력하세요.")); return false; }
    error_->clear(); return writable_;
}

namespace {
    QString PreviewPath(const PreviewSample& Sample) { return Sample.path_.isEmpty() ? "sample:" + Sample.identity_ : Sample.path_; }
    QString StateText(ValueState State) {
        return QStringList{QObject::tr("완료"), QObject::tr("빈 값"), QObject::tr("계산 중"), QObject::tr("계산 대기"), QObject::tr("지원 안 함"), QObject::tr("오류")}.value(int(State));
    }
}
void QtColumnProfileEditor::SetSamples(QVector<PreviewSample> Current, QVector<PreviewSample> Selected)
{
    stopPreview(); previewQueries_->Reset(); currentSamples_ = std::move(Current); selectedSamples_ = std::move(Selected);
    previewSignature_.clear(); sampleMode_->setCurrentIndex(0); updatePreview();
}
QVector<PreviewSample> QtColumnProfileEditor::previewSamples() const
{
    if (sampleMode_->currentIndex() == 1) return currentSamples_;
    if (sampleMode_->currentIndex() == 2) return selectedSamples_;
    if (sampleMode_->currentIndex() == 3) return manualSamples_;
    QVector<FileSnapshot> Files(3);
    Files[0].name_ = "archive.tar.gz"; Files[0].size_ = 1536; Files[0].attributes_ = 0x20;
    Files[1].name_ = ".config"; Files[1].size_ = 1048576; Files[1].attributes_ = 2;
    Files[2].name_ = tr("문서"); Files[2].directory_ = true; Files[2].attributes_ = 0x10;
    QVector<PreviewSample> Samples;
    for (auto& File : Files) {
        File.parentPath_ = "C:\\Projects"; File.created_ = File.modified_ = File.accessed_ = QDateTime(QDate(2026, 10, 2), QTime(10, 30));
        PreviewSample Sample; Sample.identity_ = "example:" + File.name_; Sample.name_ = File.name_; Sample.directory_ = File.directory_;
        for (const auto& Field : BuiltInFields()) if (Field.cost_ == EvaluationCost::Snapshot) Sample.values_[Field.id_] = Resolve(Field.id_, File);
        Samples.append(Sample);
    }
    return Samples;
}
QueryEngine::Loader QtColumnProfileEditor::previewLoader() const
{
    const auto Base = PluginFields::Instance().Loader(true); const auto Samples = previewSamples(); const bool Extract = extractSamples_->isChecked();
    auto Loader = [Base, Samples, Extract](const QString& Path, const QString& Family, std::stop_token Stop) -> FieldValues {
        if (!Path.startsWith("sample:")) return Base(Path, Family, Stop);
        const auto Fail = [](ValueState State, const QString& Reason) { return FieldValues{{"__error", CellValue::Missing(State, Reason)}}; };
        const auto Sample = std::find_if(Samples.begin(), Samples.end(), [&](const auto& Item) { return PreviewPath(Item) == Path; });
        if (!Extract || Sample == Samples.end() || !Sample->source_ || Sample->directory_) return Fail(ValueState::Unsupported, "native-path-required");
        const auto Entry = Sample->source_->Describe();
        if (Entry.size_ > 64ULL * 1024 * 1024) return Fail(ValueState::Unsupported, "preview-extraction-size-limit");
        using namespace hc::storage;
        OperationContext Context; Context.stopToken_ = Stop; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        // Reuse the host's shared store quota. Different quotas on the same
        // pinned root conflict; this preview bounds each read and uses two workers.
        Context.maxReadBytes_ = 64ULL * 1024 * 1024;
        auto Store = MakeNativeMaterializer(QDir::tempPath().toStdString());
        if (!Store.HasValue()) return Fail(ValueState::Error, QString::fromStdString(Store.Failure().code_));
        MaterializationRequest Request{Sample->source_};
        if (const auto Packed = Entry.hints_.find("packedSize"); Packed != Entry.hints_.end()) {
            bool Valid = false; const auto Bytes = QString::fromStdString(Packed->second).toULongLong(&Valid);
            if (Valid) Request.compressedBytes_ = Bytes;
        }
        auto Lease = Store.Value()->Materialize(std::move(Request), Context).get();
        if (!Lease.HasValue()) return Fail(ValueState::Error, QString::fromStdString(Lease.Failure().code_));
        return Base(QString::fromStdString(Lease.Value()->LocalPath()), Family, Stop);
    };
    return WithLuaColumns(std::move(Loader), draft_, previewRetry_);
}
void QtColumnProfileEditor::stopPreview()
{
    ++manualGeneration_; manualStop_.request_stop(); calculating_ = false; previewComputed_ = false; if (previewQueries_) previewQueries_->Cancel();
}
void QtColumnProfileEditor::startPreview()
{
    if (!ValidateDraft()) return;
    updatePreview(); previewRetry_ = QUuid::createUuid().toString(); previewQueries_->SetLoader(previewLoader()); calculating_ = true; updatePreview();
}
void QtColumnProfileEditor::loadManualSamples()
{
    stopPreview(); const auto Path = QDir::toNativeSeparators(samplePath_->text().trimmed()); if (Path.isEmpty()) return;
    manualStop_ = std::stop_source{}; const auto Generation = ++manualGeneration_; sampleStatus_->setText(tr("표본 정보를 읽고 있습니다…"));
    auto* Watcher = new QFutureWatcher<PreviewSample>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        const auto Sample = Watcher->result(); Watcher->deleteLater(); if (Generation != manualGeneration_) return;
        manualSamples_ = {Sample}; previewQueries_->Reset(); previewSignature_.clear(); updatePreview();
    });
    Watcher->setFuture(QtConcurrent::run([Path, Stop = manualStop_.get_token()] {
        using namespace hc::storage;
        PreviewSample Sample; Sample.path_ = Path; Sample.identity_ = Path; Sample.name_ = Path;
        const auto Kind = Path.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local; auto Provider = MakeNativeReadProvider(Kind);
        OperationContext Context; Context.stopToken_ = Stop; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto Entry = Provider->Resolve({Kind, Path.toStdString()}, Context).get();
        if (!Entry.HasValue()) { Sample.values_["__error"] = CellValue::Missing(ValueState::Error, QString::fromStdString(Entry.Failure().code_)); return Sample; }
        Sample.name_ = QString::fromStdString(Entry.Value().name_); Sample.directory_ = Entry.Value().directory_;
        Sample.source_ = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
        QFileInfo Info(Path); FileSnapshot File; File.name_ = Sample.name_; File.directory_ = Sample.directory_; File.parentPath_ = Info.absolutePath();
        if (!File.directory_) File.size_ = Entry.Value().size_; File.created_ = Info.birthTime(); File.modified_ = Info.lastModified(); File.accessed_ = Info.lastRead();
        File.attributes_ = Entry.Value().attributes_;
        for (const auto& Field : BuiltInFields()) if (Field.cost_ == EvaluationCost::Snapshot) Sample.values_[Field.id_] = Resolve(Field.id_, File);
        return Sample;
    }));
}
CellValue QtColumnProfileEditor::readSample(const PreviewSample& Sample, const QString& Field)
{
    if (Field.startsWith("lua.")) {
        if (!ValidateScripts(draft_).isEmpty()) return CellValue::Missing(ValueState::Error, "invalid-script-dependency");
        const auto* Script = FindScript(draft_, Field); if (!Script || !Script->enabled_) return CellValue::Missing(ValueState::Unsupported, "script-disabled-or-missing");
        QJsonObject Values; std::optional<CellValue> Missing;
        for (const auto& Dependency : Script->dependencies_) {
            const auto Value = readSample(Sample, Dependency);
            if (Value.State() == ValueState::Ready) Values[Dependency] = EncodeScriptInput(Value);
            else if (Value.State() != ValueState::Empty && (!Missing || Value.State() == ValueState::Pending)) Missing = Value;
        }
        if (Missing) return *Missing;
        const auto Job = QJsonDocument(QJsonObject{{"identity", Sample.identity_}, {"values", Values}, {"source", QString::fromLatin1(previewSignature_.toHex())}}).toJson(QJsonDocument::Compact);
        if (Job.size() > 65536) return CellValue::Missing(ValueState::Error, "script-input-budget");
        const auto Value = previewQueries_->Read(QString::fromUtf8(Job), Field, calculating_);
        return !calculating_ && Value.State() == ValueState::Pending ? CellValue::Missing(ValueState::OnDemand) : Value;
    }
    if (!QueryFamily(Field).isEmpty()) {
        if (Sample.directory_ && Field != "file.linkTarget" && !IsPluginField(Field)) return CellValue::Missing(ValueState::Empty);
        const auto Value = previewQueries_->Read(PreviewPath(Sample), Field, calculating_);
        return !calculating_ && Value.State() == ValueState::Pending ? CellValue::Missing(ValueState::OnDemand) : Value;
    }
    return Sample.values_.value(Field, Sample.values_.value("__error", CellValue::Missing(ValueState::Unsupported)));
}
void QtColumnProfileEditor::updatePreview()
{
    if (!preview_ || !previewQueries_ || !previewGrid_ || updatingPreview_) return;
    const QScopedValueRollback Updating(updatingPreview_, true);
    // Invalid drafts keep the last valid preview, with Apply blocked by validation.
    if (!Validate(draft_).isEmpty()) return;
    auto* Native = previewGrid_->view<Qtitan::GridBandedTableView>(); Native->setModel(nullptr); Native->beginUpdate(); Native->removeColumns();
    auto Samples = previewSamples(); QJsonArray Inputs, Columns;
    for (const auto& Sample : Samples) Inputs.append(Sample.identity_);
    for (const auto& Column : draft_.columns_) Columns.append(QJsonArray{Column.fieldId_, Column.template_, Column.sortField_});
    const auto Signature = QCryptographicHash::hash(QJsonDocument(QJsonObject{{"inputs", Inputs}, {"columns", Columns}, {"scripts", EncodeProfile(draft_)["scripts"]},
        {"mode", sampleMode_->currentIndex()}, {"extract", extractSamples_->isChecked()}}).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (Signature != previewSignature_) { stopPreview(); previewSignature_ = Signature; previewQueries_->SetLoader(previewLoader(), false); previewQueries_->Cancel(); }
    const int SelectedRow = preview_->currentRow(); const QSignalBlocker Block(preview_);
    const auto SelectedId = row_ >= 0 && row_ < draft_.columns_.size() ? draft_.columns_[row_].id_ : QUuid{};
    preview_->clearSpans(); preview_->clear();
    const auto Binding = BindColumnProfile(draft_); QVector<ColumnDefinition> Visible;
    previewColumnIds_.clear();
    for (const auto& Column : Binding.VecColumns) { Visible.append(Column.definition_); previewColumnIds_.append(Column.definition_.id_); }
    preview_->setColumnCount(Visible.size()); QStringList Labels; for (const auto& Column : Visible) Labels.append(Column.title_); preview_->setHorizontalHeaderLabels(Labels);
    preview_->setRowCount(Samples.size());
    const auto ValueFor = [&](const PreviewSample& Sample, const ColumnDefinition& Column, bool Sort) {
        if (Sort && !Column.sortField_.isEmpty()) return readSample(Sample, Column.sortField_);
        return Column.template_.isEmpty() ? readSample(Sample, Sort ? Column.fieldId_ : DisplayField(draft_, Column)) : RenderTemplate(CompileTemplate(Column.template_), Column.format_, [&](const auto& Field) { return readSample(Sample, Field); });
    };
    if (!draft_.sorts_.isEmpty()) {
        QVector<SortRow> Rows; QVector<Qt::SortOrder> Orders; for (const auto& Sort : draft_.sorts_) Orders.append(Sort.order_);
        for (const auto& Sample : Samples) {
            SortRow Row; Row.id_ = Sample.identity_; Row.directory_ = Sample.directory_;
            for (const auto& Sort : draft_.sorts_) {
                const auto It = std::find_if(draft_.columns_.begin(), draft_.columns_.end(), [&](const auto& Column) { return Column.id_ == Sort.columnId_; });
                Row.values_.append(It == draft_.columns_.end() ? CellValue::Missing(ValueState::Empty) : ValueFor(Sample, *It, true));
            }
            Rows.append(Row);
        }
        const auto Ranks = SortRanks(Rows, Orders); auto Sorted = Samples; for (int Index = 0; Index < Samples.size(); ++Index) Sorted[Ranks[Index]] = Samples[Index]; Samples = Sorted;
    }
    const auto MakeCell = [&](const PreviewSample& Sample, const ColumnDefinition& Column) {
        const auto Value = ValueFor(Sample, Column, false), Sort = ValueFor(Sample, Column, true);
        QString Text = Value.State() == ValueState::Ready ? Format(Value, Column.format_) : Value.State() == ValueState::Empty ? QString{} : StateText(Value.State());
        if (Column.fieldId_ == "file.attributes" && Value.State() == ValueState::Ready) Text = GetFormattedAttrText(quint32(Value.Raw().toULongLong()), false);
        if (Column.fieldId_ == "file.size" && Value.State() == ValueState::OnDemand && Sample.directory_) Text = tr("<폴더>");
        auto* Cell = new QTableWidgetItem(Text); Cell->setTextAlignment(Column.alignment_ | Qt::AlignVCenter);
        Cell->setToolTip(tr("%1 · %2\n원래 값: %3\n표시: %4\n정렬 값: %5 · %6%7").arg(Sample.name_, StateText(Value.State()), Value.Raw().toString(), Text, Sort.Raw().toString(), StateText(Sort.State()), Value.Error().isEmpty() ? QString{} : "\n" + Value.Error()));
        return Cell;
    };
    const QFileIconProvider Icons;
    for (int Row = 0; Row < Samples.size(); ++Row) for (int Index = 0; Index < Visible.size(); ++Index) {
        auto* Cell = MakeCell(Samples[Row], Visible[Index]);
        if (Visible[Index].id_ == draft_.primaryNameId_) Cell->setIcon(Icons.icon(Samples[Row].directory_ ? QFileIconProvider::Folder : QFileIconProvider::File));
        preview_->setItem(Row, Index, Cell);
    }
    layoutPreview();
    previewGrid_->setPalette(palette()); previewGrid_->viewport()->setPalette(palette());
    auto& Appearance = Native->fileManagerOptions();
    Appearance.cursorColor = Appearance.selectedColor = palette().color(QPalette::Highlight);
    Appearance.cursorTextColor = Appearance.selectedTextColor = palette().color(QPalette::HighlightedText);
    Appearance.backgroundColor = palette().color(QPalette::Base);
    Native->options().setBackgroundColor(Appearance.backgroundColor);
    // The table was rebuilt while detached. Its indexes can be reused, so the
    // grid's decoration/font role cache must not retain the previous samples.
    Native->modelController()->clearRoleCache(); Native->setModel(preview_->model());
    for (const auto& Bound : Binding.VecColumns) {
        auto* Column = Native->addColumn(Bound.Index, Bound.definition_.title_); Column->setBandIndex(0); Column->setTextAlignment(Bound.definition_.alignment_);
        Column->setProperty("hc.columnId", Bound.definition_.id_);
        // The grid paints a column's decoration over its header after the title, so an opaque fill hides
        // the name. Only the selected column gets a translucent tint; the others keep the theme header.
        if (Bound.definition_.id_ == SelectedId) Column->setDecorationColor(palette().color(QPalette::Highlight));
        Column->setSortEnabled(false); Column->setFilterButtonVisible(false); Column->editorRepository()->setEditorActivationPolicy(Qtitan::GridEditor::ActivationPolicy());
    }
    for (int Position = 0; Position < draft_.columns_.size(); ++Position) for (const auto& Bound : Binding.VecColumns)
        if (Bound.definition_.id_ == draft_.columns_[Position].id_) Native->getColumnByModelColumn(Bound.Index)->setVisualIndex(Position);
    previewAdapter_->Bind(Binding); previewAdapter_->UseProfileWidths(); previewAdapter_->Apply(); Native->endUpdate();
    const int SelectedColumn = previewColumnIds_.indexOf(SelectedId);
    if (SelectedColumn >= 0 && row_ >= 0 && !draft_.columns_[row_].widths_[int(draft_.mode_)].hidden_) {
        Native->setFocusedColumnIndex(SelectedColumn);
        if (preview_->rowCount()) preview_->setCurrentCell(qBound(0, SelectedRow, preview_->rowCount() - 1), SelectedColumn);
    }
    if (const auto* Cell = preview_->currentItem()) valueDetail_->setText(Cell->toolTip()); else valueDetail_->clear();
    quint64 Bytes = 0; int Virtual = 0; for (const auto& Sample : Samples) if (Sample.path_.isEmpty() && Sample.source_ && !Sample.directory_) { ++Virtual; Bytes += Sample.source_->Describe().size_; }
    if (calculating_ && !previewQueries_->Pending()) { calculating_ = false; previewComputed_ = true; }
    findChild<QPushButton*>("hc.columns.stopPreview")->setVisible(calculating_);
    sampleStatus_->setText(tr("표본 %1개 · %2%3").arg(Samples.size()).arg(previewQueries_->Pending() ? tr("계산 중 %1건").arg(previewQueries_->Pending()) : previewComputed_ ? tr("계산 완료") : tr("목록 값 표시 · 느린 값은 ‘값 계산’으로 조회"))
        .arg(Virtual ? tr(" · 가상 파일 %1개, 최대 %2 MiB 읽기").arg(Virtual).arg(double(Bytes) / 1048576, 0, 'f', 2) : QString{}));
    int Count = 0; for (const auto& Column : draft_.columns_) if (!Column.widths_[int(draft_.mode_)].hidden_) ++Count;
    summary_->setText(tr("%1 · %2 · 표시 열 %3개").arg(draft_.title_, layout_->currentText()).arg(Count));
}

void QtColumnProfileEditor::cloneProfile()
{
    if (!ValidateDraft()) return;
    library_[IdText(draft_.id_)] = EncodeProfile(draft_);
    draft_.id_ = QUuid::createUuid(); draft_.title_ += tr(" 복사");
    for (auto& Column : draft_.columns_) {
        const auto Old = Column.id_; Column.id_ = QUuid::createUuid();
        if (draft_.primaryNameId_ == Old) draft_.primaryNameId_ = Column.id_;
        for (auto& Sort : draft_.sorts_) if (Sort.columnId_ == Old) Sort.columnId_ = Column.id_;
    }
    library_[IdText(draft_.id_)] = EncodeProfile(draft_); dirty_ = true; refreshProfiles(); rebuildList(0);
}

QJsonObject QtColumnProfileEditor::Library() const { auto Result = library_; Result[IdText(draft_.id_)] = EncodeProfile(draft_); return Result; }
bool QtColumnProfileEditor::NewTabDefault() const { return scope_->currentIndex() == 1 || scope_->currentIndex() == 3; }
void QtColumnProfileEditor::layoutPreview()
{
    if (!previewGrid_ || !previewFrame_) return;
    // Geometry changes must not rebuild the model or restart asynchronous values.
    previewGrid_->resize(previewWidth_->value() ? previewWidth_->value() : previewFrame_->viewport()->width(), std::max(130, previewFrame_->viewport()->height()));
    if (previewAdapter_) previewAdapter_->Request();
}
void QtColumnProfileEditor::resizeEvent(QResizeEvent* Event) { QWidget::resizeEvent(Event); layoutPreview(); }
bool QtColumnProfileEditor::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == this && previewGrid_ && (Event->type() == QEvent::PaletteChange || Event->type() == QEvent::FontChange || Event->type() == QEvent::StyleChange))
        QTimer::singleShot(0, this, &QtColumnProfileEditor::updatePreview);
    // Layout settles after the editor resize; measure the actual preview viewport.
    if (previewFrame_ && Watched == previewFrame_->viewport() && Event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &QtColumnProfileEditor::layoutPreview);
    return QWidget::eventFilter(Watched, Event);
}

void QtColumnProfileEditor::refreshSorts(int Row)
{
    const QSignalBlocker Block(sortRules_); sortRules_->clear();
    for (int Index = 0; Index < draft_.sorts_.size(); ++Index) {
        const auto& Sort = draft_.sorts_[Index];
        const auto It = std::find_if(draft_.columns_.begin(), draft_.columns_.end(), [&](const auto& Col) { return Col.id_ == Sort.columnId_; });
        sortRules_->addItem(QString::number(Index + 1) + ". " + (It == draft_.columns_.end() ? QString{} : It->title_) + (Sort.order_ == Qt::AscendingOrder ? " ↑" : " ↓"));
    }
    sortRules_->setCurrentRow(std::min(Row, int(draft_.sorts_.size()) - 1));
    const QSignalBlocker One(sort_), Two(sortOrder_);
    if (sortRules_->currentRow() >= 0) {
        const auto& Sort = draft_.sorts_[sortRules_->currentRow()]; sort_->setCurrentIndex(sort_->findData(IdText(Sort.columnId_))); sortOrder_->setCurrentIndex(Sort.order_ == Qt::DescendingOrder);
    } else sort_->setCurrentIndex(0);
}

void QtColumnProfileEditor::refreshFields()
{
    const QSignalBlocker One(field_), Two(sortField_); field_->clear(); sortField_->clear(); sortField_->addItem(tr("원래 값 / 템플릿 문자열"));
    for (const auto& Field : availableFields()) {
        sortField_->addItem(Field.title_, Field.id_);
        if (fieldCategory_->currentIndex() && int(Field.provider_) != fieldCategory_->currentIndex() - 1) continue;
        if (!(Field.title_ + ' ' + Field.id_).contains(fieldSearch_->text(), Qt::CaseInsensitive)) continue;
        field_->addItem(Field.title_, Field.id_);
    }
    field_->addItem(tr("복합 템플릿"), QString{});
}
void QtColumnProfileEditor::refreshScripts(int Row)
{
    const QSignalBlocker Block(scripts_); scripts_->clear();
    for (const auto& Script : draft_.scripts_) scripts_->addItem(Script.title_);
    scripts_->setCurrentIndex(draft_.scripts_.isEmpty() ? -1 : std::clamp(Row, 0, int(draft_.scripts_.size()) - 1)); loadScript();
}
void QtColumnProfileEditor::loadScript()
{
    const bool Previous = loading_; loading_ = true; const int Row = scripts_->currentIndex();
    const auto Script = Row >= 0 ? draft_.scripts_[Row] : ScriptDefinition{};
    for (auto* Widget : QList<QWidget*>{scriptTitle_, dependencies_, scriptSource_, scriptOutputs_, scriptEnabled_}) Widget->setEnabled(Row >= 0);
    scriptTitle_->setText(Script.title_); dependencies_->setText(Script.dependencies_.join(", ")); scriptSource_->setPlainText(Script.source_); scriptEnabled_->setChecked(Script.enabled_);
    const QStringList Types{"text", "integer", "unsigned", "real", "boolean", "date", "time", "timestamp"}; QStringList Outputs;
    for (const auto& Output : Script.outputs_) Outputs.append(Output.name_ + " | " + Types.value(int(Output.type_)) + " | " + Output.title_);
    scriptOutputs_->setPlainText(Outputs.join('\n')); loading_ = Previous;
}
void QtColumnProfileEditor::storeScript()
{
    const int Row = scripts_->currentIndex(); if (loading_ || Row < 0) return;
    auto& Script = draft_.scripts_[Row]; Script.title_ = scriptTitle_->text(); Script.source_ = scriptSource_->toPlainText(); Script.enabled_ = scriptEnabled_->isChecked();
    Script.dependencies_ = dependencies_->text().split(',', Qt::SkipEmptyParts); for (auto& Dependency : Script.dependencies_) Dependency = Dependency.trimmed();
    Script.outputs_.clear(); const QStringList Types{"text", "integer", "unsigned", "real", "boolean", "date", "time", "timestamp"};
    for (const auto& Line : scriptOutputs_->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        const auto Parts = Line.split('|'); Script.outputs_.append({Parts.value(0).trimmed(), Parts.value(2).trimmed(), static_cast<FieldType>(Types.indexOf(Parts.value(1).trimmed()))});
    }
    scripts_->setItemText(Row, Script.title_); dirty_ = true; refreshFields(); loadColumn(row_);
}

QtColumnProfileEditor::~QtColumnProfileEditor()
{
    stopPreview(); delete previewAdapter_;
    if (previewGrid_) previewGrid_->view<Qtitan::GridBandedTableView>()->setModel(nullptr);
}

void QtColumnProfileEditor::LoadAutoSelection(const QJsonObject& Policy, const QJsonObject& Colors, bool AutomaticTab)
{
    const QScopedValueRollback Loading(loading_, true);
    autoPolicy_ = Policy.isEmpty() ? EmptyAutoSelection() : Policy; autoColors_ = Colors;
    const bool Supported = autoPolicy_["version"] == QJsonValue(1);
    autoSelection_->setEnabled(Supported); autoRules_->setEnabled(Supported); automaticTab_->setEnabled(Supported);
    for (const auto* Name : {"hc.columns.autoAdd", "hc.columns.autoRemove", "hc.columns.autoUp", "hc.columns.autoDown"}) findChild<QPushButton*>(Name)->setEnabled(Supported);
    autoSelection_->setChecked(autoPolicy_["enabled"].toBool()); automaticTab_->setChecked(AutomaticTab);
    autoRules_->setRowCount(0); for (const auto& Value : autoPolicy_["rules"].toArray()) appendAutoRule(Value.toObject());
}
QJsonObject QtColumnProfileEditor::AutoSelection() const
{
    if (!autoSelection_->isEnabled()) return autoPolicy_;
    auto Result = autoPolicy_; Result["version"] = 1; Result["enabled"] = autoSelection_->isChecked(); QJsonArray Rules;
    for (int Row = 0; Row < autoRules_->rowCount(); ++Row) {
        auto Rule = autoRules_->item(Row, 0)->data(Qt::UserRole).toJsonObject();
        Rule["enabled"] = autoRules_->item(Row, 0)->checkState() == Qt::Checked;
        Rule["profile"] = qobject_cast<QComboBox*>(autoRules_->cellWidget(Row, 1))->currentData().toString();
        Rule["path"] = qobject_cast<QLineEdit*>(autoRules_->cellWidget(Row, 2))->text().trimmed();
        Rule["group"] = qobject_cast<QComboBox*>(autoRules_->cellWidget(Row, 3))->currentData().toString();
        Rule["percent"] = qobject_cast<QSpinBox*>(autoRules_->cellWidget(Row, 4))->value(); Rules.append(Rule);
    }
    Result["rules"] = Rules; return Result;
}
void QtColumnProfileEditor::appendAutoRule(const QJsonObject& Rule)
{
    const int Row = autoRules_->rowCount(); autoRules_->insertRow(Row);
    auto* Enabled = new QTableWidgetItem; Enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable); Enabled->setCheckState(Rule["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked); Enabled->setData(Qt::UserRole, Rule); autoRules_->setItem(Row, 0, Enabled);
    auto* Profile = new QComboBox(autoRules_); autoRules_->setCellWidget(Row, 1, Profile);
    const auto Profiles = Library(); for (auto It = Profiles.begin(); It != Profiles.end(); ++It) if (const auto Value = DecodeProfile(It.value().toObject())) Profile->addItem(Value->title_, It.key());
    if (Profile->findData(Rule["profile"].toString()) < 0) Profile->addItem(tr("사용할 수 없는 열 세트"), Rule["profile"].toString()); Profile->setCurrentIndex(Profile->findData(Rule["profile"].toString()));
    auto* Path = new QLineEdit(Rule["path"].toString(), autoRules_); Path->setMaxLength(4096); Path->setAccessibleName(tr("폴더 경로 조건")); autoRules_->setCellWidget(Row, 2, Path);
    auto* Group = new QComboBox(autoRules_); Group->addItem(tr("사용 안 함"), ""); autoRules_->setCellWidget(Row, 3, Group);
    for (const auto& Value : autoColors_["groups"].toArray()) if (Value.toObject()["enabled"].toBool(true)) Group->addItem(Value.toObject()["name"].toString(), Value.toObject()["id"].toString());
    if (Group->findData(Rule["group"].toString()) < 0) Group->addItem(tr("사용할 수 없는 파일 그룹"), Rule["group"].toString()); Group->setCurrentIndex(Group->findData(Rule["group"].toString()));
    auto* Percent = new QSpinBox(autoRules_); Percent->setRange(1, 100); Percent->setSuffix("%"); Percent->setValue(Rule["percent"].toInt(50)); Percent->setAccessibleName(tr("파일 그룹 최소 비율")); autoRules_->setCellWidget(Row, 4, Percent);
    connect(Profile, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) dirty_ = true; }); connect(Path, &QLineEdit::textChanged, this, [this] { if (!loading_) dirty_ = true; }); connect(Group, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) dirty_ = true; }); connect(Percent, &QSpinBox::valueChanged, this, [this] { if (!loading_) dirty_ = true; });
    if (!autoRules_->property("hc.connected").toBool()) {
        autoRules_->setProperty("hc.connected", true); connect(autoRules_, &QTableWidget::itemChanged, this, [this] { if (!loading_) dirty_ = true; });
    }
}

void QtColumnProfileEditor::SetAutoColors(const QJsonObject& Colors)
{
    autoColors_ = Colors;
    for (int Row = 0; Row < autoRules_->rowCount(); ++Row) {
        auto* Group = qobject_cast<QComboBox*>(autoRules_->cellWidget(Row, 3));
        const auto Selected = Group->currentData().toString(); const QSignalBlocker Block(Group);
        Group->clear(); Group->addItem(tr("사용 안 함"), "");
        for (const auto& Value : Colors["groups"].toArray()) if (Value.toObject()["enabled"].toBool(true)) Group->addItem(Value.toObject()["name"].toString(), Value.toObject()["id"].toString());
        if (Group->findData(Selected) < 0) Group->addItem(tr("사용할 수 없는 파일 그룹"), Selected);
        Group->setCurrentIndex(Group->findData(Selected));
    }
}
