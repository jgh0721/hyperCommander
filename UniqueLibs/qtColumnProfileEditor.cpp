#include "stdafx.h"
#include "qtColumnProfileEditor.hpp"
#include "cmnHCUtils.hpp"
#include <QScrollArea>
#include "columnPluginFields.hpp"
#include "commonLibs/columns/columnSort.hpp"

using namespace hc::columns;

namespace
{
    class ColumnChoice final : public QComboBox
    {
    public:
        using QComboBox::QComboBox;
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
    protected:
        QString textFromValue(double Value) const override { return locale().toString(Value, 'f', 0); }
    };
    QComboBox* Choice(QWidget* Parent, const char* Name, const QStringList& Items)
    {
        auto* Result = new ColumnChoice(Parent); Result->setObjectName(Name); Result->addItems(Items);
        Result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        Result->setMinimumContentsLength(12); Result->setMaxVisibleItems(16);
        return Result;
    }
}

QtColumnProfileEditor::QtColumnProfileEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.columns.editor");
    auto* Root = new QVBoxLayout(this); Root->setContentsMargins(0, 0, 0, 0);
    auto* ProfileBar = new QHBoxLayout;
    profiles_ = Choice(this, "hc.columns.profiles", {});
    profileTitle_ = new QLineEdit(this); profileTitle_->setObjectName("hc.columns.profileTitle");
    ProfileBar->addWidget(new QLabel(tr("프로필"))); ProfileBar->addWidget(profiles_, 1);
    ProfileBar->addWidget(profileTitle_, 1);
    auto* Copy = new QPushButton(tr("복제"), this); Copy->setObjectName("hc.columns.cloneProfile"); ProfileBar->addWidget(Copy);
    auto* Delete = new QPushButton(tr("삭제"), this); deleteProfile_ = Delete; ProfileBar->addWidget(Delete); Root->addLayout(ProfileBar);
    auto* Modes = new QHBoxLayout;
    layout_ = Choice(this, "hc.columns.layout", {tr("한 줄"), tr("두 줄: 이름 위"), tr("두 줄: 이름 아래")});
    scope_ = Choice(this, "hc.columns.scope", {tr("현재 탭"), tr("현재 탭 + 이 패널의 새 탭 기본값")});
    Modes->addWidget(new QLabel(tr("표시 모드"))); Modes->addWidget(layout_, 1);
    Modes->addWidget(new QLabel(tr("적용 범위"))); Modes->addWidget(scope_, 2); Root->addLayout(Modes);
    auto* Split = new QSplitter(this); Root->addWidget(Split, 3);
    auto* Left = new QWidget(Split); auto* LeftRows = new QVBoxLayout(Left); LeftRows->setContentsMargins(0, 0, 4, 0);
    columns_ = new QListWidget(Left); columns_->setObjectName("hc.columns.list"); LeftRows->addWidget(columns_, 1);
    auto* Actions = new QGridLayout; LeftRows->addLayout(Actions);
    const auto Button = [&](const QString& Label, const char* Name, int Row, int Col, auto Handler) {
        auto* Item = new QPushButton(Label, Left); Item->setObjectName(Name); Actions->addWidget(Item, Row, Col);
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
    Button(tr("기본값"), "hc.columns.reset", 1, 2, [this] { const auto Id = draft_.id_; const auto Title = draft_.title_; draft_ = DefaultProfile(); draft_.id_ = Id; draft_.title_ = Title; dirty_ = true; rebuildList(0); });
    auto* Details = new QTabWidget(Split); Details->setObjectName("hc.columns.details");
    const auto AddScrollableTab = [Details](QWidget* Page, const QString& Title) {
        auto* Scroll = new QScrollArea(Details); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        Scroll->setWidget(Page); Details->addTab(Scroll, Title);
    };
    auto* Content = new QWidget; auto* Form = new QFormLayout(Content);
    field_ = Choice(Content, "hc.columns.field", {});
    for (const auto& Field : PluginFields::Instance().Fields()) field_->addItem(Field.title_ + (Field.cost_ == EvaluationCost::Background ? tr(" (비동기)") : QString{}), Field.id_);
    field_->addItem(tr("복합 템플릿"), QString{});
    template_ = new QLineEdit(Content); template_->setObjectName("hc.columns.template"); template_->setPlaceholderText("${file.name} · ${file.size}");
    template_->setMaxLength(4096);
    auto* Insert = new QPushButton(tr("필드 삽입"), Content);
    auto* TemplateRow = new QHBoxLayout; TemplateRow->addWidget(template_, 1); TemplateRow->addWidget(Insert);
    connect(Insert, &QPushButton::clicked, this, [this, Insert] {
        QMenu Menu(this); for (const auto& Field : PluginFields::Instance().Fields()) { auto* Action = Menu.addAction(Field.title_); Action->setData(Field.id_); }
        if (const auto* Action = Menu.exec(Insert->mapToGlobal(QPoint(0, Insert->height())))) template_->insert("${" + Action->data().toString() + "}");
    });
    sortField_ = Choice(Content, "hc.columns.sortField", {tr("원래 값 / 템플릿 문자열")});
    for (const auto& Field : PluginFields::Instance().Fields()) sortField_->addItem(Field.title_, Field.id_);
    title_ = new QLineEdit(Content); title_->setObjectName("hc.columns.title");
    alignment_ = Choice(Content, "hc.columns.alignment", {tr("왼쪽"), tr("가운데"), tr("오른쪽")});
    primary_ = new QCheckBox(tr("주 이름 컬럼으로 사용"), Content); primary_->setObjectName("hc.columns.primary");
    sizeFormat_ = Choice(Content, "hc.columns.sizeFormat", {tr("바이트"), tr("자동 단위"), "KiB", "MiB", "GiB"});
    precision_ = new QSpinBox(Content); precision_->setObjectName("hc.columns.precision"); precision_->setRange(0, 6);
    dateFormat_ = Choice(Content, "hc.columns.dateFormat", {"yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd", "HH:mm:ss", "yyyy/MM/dd HH:mm"}); dateFormat_->setEditable(true);
    Form->addRow(tr("내용"), field_); Form->addRow(tr("템플릿"), TemplateRow); Form->addRow(tr("정렬 값"), sortField_); Form->addRow(tr("컬럼 제목"), title_); Form->addRow(tr("내용 정렬"), alignment_);
    Form->addRow(primary_); Form->addRow(tr("크기 형식"), sizeFormat_); Form->addRow(tr("소수 자릿수"), precision_); Form->addRow(tr("날짜 형식"), dateFormat_);
    auto* FormatNote = new QLabel(tr("같은 필드를 여러 컬럼에 추가해 서로 다른 형식으로 표시할 수 있습니다. 정렬은 원래 값을 기준으로 합니다."), Content);
    FormatNote->setWordWrap(true); Form->addRow(FormatNote); AddScrollableTab(Content, tr("내용·형식"));
    auto* Widths = new QWidget; auto* WidthForm = new QFormLayout(Widths);
    unit_ = Choice(Widths, "hc.columns.unit", {tr("픽셀"), tr("퍼센트")});
    widthMode_ = Choice(Widths, "hc.columns.widthMode", {tr("가변"), tr("고정")});
    const auto Number = [&](const char* Name, bool Optional) {
        auto* Result = new WidthNumber(Widths); Result->setObjectName(Name); Result->setAlignment(Qt::AlignCenter);
        Result->setDecimals(6); Result->setRange(Optional ? -1 : 0, 1000000); if (Optional) Result->setSpecialValueText(tr("미지정")); return Result;
    };
    width_ = Number("hc.columns.width", false); minimum_ = Number("hc.columns.minimum", true); maximum_ = Number("hc.columns.maximum", true);
    visible_ = new QCheckBox(tr("현재 모드에서 표시"), Widths); visible_->setObjectName("hc.columns.visible");
    WidthForm->addRow(tr("단위"), unit_); WidthForm->addRow(tr("모드"), widthMode_); WidthForm->addRow(tr("기준값"), width_);
    WidthForm->addRow(tr("최소값"), minimum_); WidthForm->addRow(tr("최대값"), maximum_); WidthForm->addRow(visible_);
    auto* WidthNote = new QLabel(tr("모드마다 너비와 표시 여부를 따로 저장합니다. 두 줄 모드의 주 이름은 별도 줄을 사용합니다."), Widths);
    WidthNote->setWordWrap(true); WidthForm->addRow(WidthNote); AddScrollableTab(Widths, tr("너비·표시"));
    Split->setStretchFactor(0, 2); Split->setStretchFactor(1, 3); Split->setChildrenCollapsible(false);
    auto* Sorting = new QHBoxLayout;
    sort_ = Choice(this, "hc.columns.sort", {}); sortOrder_ = Choice(this, "hc.columns.sortOrder", {tr("오름차순"), tr("내림차순")});
    Sorting->addWidget(new QLabel(tr("정렬 기준"))); Sorting->addWidget(sort_, 1); Sorting->addWidget(sortOrder_); Root->addLayout(Sorting);
    auto* AddSort = new QPushButton(tr("정렬 추가"), this); AddSort->setObjectName("hc.columns.addSort"); Sorting->addWidget(AddSort);
    auto* RemoveSort = new QPushButton(tr("정렬 제거"), this); Sorting->addWidget(RemoveSort);
    sortRules_ = new QListWidget(this); sortRules_->setObjectName("hc.columns.sortRules"); sortRules_->setMaximumHeight(80); Root->addWidget(sortRules_);
    auto* Priority = new QHBoxLayout; Root->addLayout(Priority); Priority->addStretch();
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
    Root->addWidget(new QLabel(tr("예제 미리 보기"), this));
    preview_ = new QTableWidget(this); preview_->setObjectName("hc.columns.preview"); preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_->viewport()->installEventFilter(this); preview_->verticalHeader()->hide(); preview_->setSelectionBehavior(QAbstractItemView::SelectRows); preview_->setMinimumHeight(130); Root->addWidget(preview_, 2);
    error_ = new QLabel(this); error_->setObjectName("hc.columns.error"); error_->setWordWrap(true); Root->addWidget(error_);
    connect(columns_, &QListWidget::currentRowChanged, this, &QtColumnProfileEditor::loadColumn);
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
    connect(profileTitle_, &QLineEdit::textEdited, this, [this](const QString& Text) { draft_.title_ = Text; const auto Index = profiles_->findData(IdText(draft_.id_)); if (Index >= 0) profiles_->setItemText(Index, Text); dirty_ = true; });
    connect(Copy, &QPushButton::clicked, this, &QtColumnProfileEditor::cloneProfile);
    connect(Delete, &QPushButton::clicked, this, [this] {
        library_.remove(IdText(draft_.id_)); draft_ = DecodeProfile(library_[IdText(BuiltInProfileId)].toObject()).value_or(DefaultProfile()); library_[IdText(draft_.id_)] = EncodeProfile(draft_); dirty_ = true; refreshProfiles(); rebuildList(0);
    });
    connect(profiles_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        const auto Next = DecodeProfile(library_[profiles_->currentData().toString()].toObject());
        if (!Next || !IsSupportedProfile(*Next)) return;
        if (Validate(draft_).isEmpty()) library_[IdText(draft_.id_)] = EncodeProfile(draft_);
        draft_ = *Next; dirty_ = true; refreshProfiles(); rebuildList(0);
    });
    auto* Reload = new QPushButton(tr("플러그인 필드 새로 읽기"), this); Reload->setObjectName("hc.columns.reloadProviders"); ProfileBar->addWidget(Reload);
    connect(Reload, &QPushButton::clicked, &PluginFields::Instance(), &PluginFields::Refresh);
    connect(&PluginFields::Instance(), &PluginFields::Changed, this, [this] {
        const QSignalBlocker One(field_), Two(sortField_); field_->clear(); sortField_->clear(); sortField_->addItem(tr("원래 값 / 템플릿 문자열"));
        for (const auto& Field : PluginFields::Instance().Fields()) { field_->addItem(Field.title_, Field.id_); sortField_->addItem(Field.title_, Field.id_); }
        field_->addItem(tr("복합 템플릿"), QString{}); loadColumn(row_);
        if (!PluginFields::Instance().Status().isEmpty()) error_->setText(PluginFields::Instance().Status());
    });
    Load(DefaultProfile(), {});
}

void QtColumnProfileEditor::Load(const ColumnProfile& Profile, const QJsonObject& Library, bool Writable)
{
    draft_ = Profile; library_ = Library; writable_ = Writable; dirty_ = false;
    { const QSignalBlocker Block(scope_); scope_->setCurrentIndex(0); }
    library_[IdText(draft_.id_)] = EncodeProfile(draft_);
    refreshProfiles(); rebuildList(0); setEnabled(Writable);
    if (!Writable) error_->setText(tr("지원하지 않는 컬럼 설정을 보존하고 있습니다. 편집할 수 없습니다."));
}

void QtColumnProfileEditor::refreshProfiles()
{
    loading_ = true; profiles_->clear();
    for (auto It = library_.begin(); It != library_.end(); ++It)
        if (const auto Profile = DecodeProfile(It.value().toObject()); Profile && IsSupportedProfile(*Profile)) profiles_->addItem(Profile->title_, It.key());
    profiles_->setCurrentIndex(profiles_->findData(IdText(draft_.id_))); loading_ = false;
}

void QtColumnProfileEditor::rebuildList(int Row)
{
    loading_ = true; columns_->clear(); sort_->clear(); sort_->addItem(tr("지정 안 함"), QString{});
    profileTitle_->setText(draft_.title_); layout_->setCurrentIndex(int(draft_.mode_));
    deleteProfile_->setEnabled(draft_.id_ != BuiltInProfileId);
    for (const auto& Column : draft_.columns_) {
        columns_->addItem(Column.title_); sort_->addItem(Column.title_, IdText(Column.id_));
    }
    if (!draft_.sorts_.isEmpty()) { sort_->setCurrentIndex(sort_->findData(IdText(draft_.sorts_.front().columnId_))); sortOrder_->setCurrentIndex(draft_.sorts_.front().order_ == Qt::DescendingOrder); }
    columns_->setCurrentRow(Row); loading_ = false; refreshSorts(); loadColumn(Row);
}

void QtColumnProfileEditor::loadColumn(int Row)
{
    if (loading_ || Row < 0 || Row >= draft_.columns_.size()) return;
    loading_ = true; row_ = Row; const auto& Column = draft_.columns_[Row]; const auto& Width = Column.widths_[int(draft_.mode_)];
    if (Column.template_.isEmpty() && field_->findData(Column.fieldId_) < 0) field_->addItem(tr("사용 불가 · %1").arg(Column.fieldId_), Column.fieldId_);
    if (!Column.sortField_.isEmpty() && sortField_->findData(Column.sortField_) < 0) sortField_->addItem(tr("사용 불가 · %1").arg(Column.sortField_), Column.sortField_);
    field_->setCurrentIndex(field_->findData(Column.template_.isEmpty() ? Column.fieldId_ : QString{})); title_->setText(Column.title_);
    template_->setText(Column.template_); template_->setEnabled(!Column.template_.isEmpty());
    sortField_->setCurrentIndex(std::max(0, sortField_->findData(Column.sortField_)));
    alignment_->setCurrentIndex(Column.alignment_ == Qt::AlignLeft ? 0 : Column.alignment_ == Qt::AlignRight ? 2 : 1);
    primary_->setChecked(Column.id_ == draft_.primaryNameId_); primary_->setEnabled(Column.fieldId_ == "file.name" || Column.fieldId_ == "file.stem");
    sizeFormat_->setCurrentIndex(int(Column.format_.size_)); precision_->setValue(Column.format_.precision_); dateFormat_->setCurrentText(Column.format_.datePattern_);
    sizeFormat_->setEnabled(Column.fieldId_ == "file.size" || Column.fieldId_ == "file.allocatedSize" || !Column.template_.isEmpty()); precision_->setEnabled(sizeFormat_->isEnabled());
    dateFormat_->setEnabled(Column.fieldId_ == "file.created" || Column.fieldId_ == "file.modified" || Column.fieldId_ == "file.accessed" || !Column.template_.isEmpty());
    unit_->setCurrentIndex(int(Width.unit_)); widthMode_->setCurrentIndex(int(Width.mode_)); width_->setValue(Width.value_);
    minimum_->setValue(Width.minimum_.value_or(-1)); maximum_->setValue(Width.maximum_.value_or(-1)); visible_->setChecked(!Width.hidden_);
    loading_ = false; updatePreview();
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
        const auto Fields = PluginFields::Instance().Fields();
        for (const auto& Old : Fields) if (Old.id_ == PreviousField && Column.title_ == Old.title_) {
            Column.title_ = field_->currentText(); break;
        }
    }
    Column.alignment_ = alignment_->currentIndex() == 0 ? Qt::AlignLeft : alignment_->currentIndex() == 2 ? Qt::AlignRight : Qt::AlignCenter;
    Column.format_ = {static_cast<SizeFormat>(sizeFormat_->currentIndex()), precision_->value(), dateFormat_->currentText()};
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
        for (auto& Sort : draft_.sorts_) if (Sort.columnId_ == Column.id_) Sort.order_ = PluginFields::Instance().Descending(Field) ? Qt::DescendingOrder : Qt::AscendingOrder;
        refreshSorts();
    }
    columns_->item(row_)->setText(Column.title_); dirty_ = true;
    const auto SortRow = sort_->findData(IdText(Column.id_)); if (SortRow >= 0) sort_->setItemText(SortRow, Column.title_);
    if (Validate(draft_).isEmpty()) error_->clear();
    if (FieldChanged || NewPrimary) loadColumn(row_); else updatePreview();
}

bool QtColumnProfileEditor::ValidateDraft()
{
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

void QtColumnProfileEditor::updatePreview()
{
    if (!preview_) return;
    preview_->clearSpans(); preview_->clear();
    QVector<ColumnDefinition> Visible;
    for (const auto& Column : draft_.columns_) if (!Column.widths_[int(draft_.mode_)].hidden_ &&
        (draft_.mode_ == LayoutMode::Details || Column.id_ != draft_.primaryNameId_)) Visible.append(Column);
    preview_->setColumnCount(std::max(1, int(Visible.size())));
    QStringList Labels; for (const auto& Column : Visible) Labels.append(Column.title_); preview_->setHorizontalHeaderLabels(Labels);
    const bool Two = draft_.mode_ != LayoutMode::Details; preview_->setRowCount(Two ? 6 : 3);
    QVector<FileSnapshot> Samples(3);
    Samples[0].name_ = "archive.tar.gz"; Samples[0].size_ = 1536; Samples[0].attributes_ = 0x20;
    Samples[1].name_ = ".config"; Samples[1].size_ = 1048576; Samples[1].attributes_ = 0x2;
    Samples[2].name_ = tr("문서"); Samples[2].directory_ = true; Samples[2].attributes_ = 0x10;
    for (auto& File : Samples) { File.parentPath_ = "C:\\Projects"; File.created_ = File.modified_ = File.accessed_ = QDateTime(QDate(2026, 10, 2), QTime(10, 30)); }
    if (!draft_.sorts_.isEmpty()) {
        QVector<SortRow> Rows; QVector<Qt::SortOrder> Orders;
        for (const auto& Sort : draft_.sorts_) Orders.append(Sort.order_);
        for (const auto& File : Samples) {
            SortRow Row; Row.id_ = File.name_; Row.directory_ = File.directory_;
            for (const auto& Sort : draft_.sorts_) {
                const auto It = std::find_if(draft_.columns_.begin(), draft_.columns_.end(), [&](const auto& Col) { return Col.id_ == Sort.columnId_; });
                if (It == draft_.columns_.end()) { Row.values_.append(CellValue::Missing(ValueState::Empty)); continue; }
                Row.values_.append(!It->sortField_.isEmpty() ? Resolve(It->sortField_, File) : It->template_.isEmpty() ? Resolve(It->fieldId_, File)
                    : RenderTemplate(CompileTemplate(It->template_), It->format_, [&](const QString& Field) { return Resolve(Field, File); }));
            }
            Rows.append(Row);
        }
        const auto Ranks = SortRanks(Rows, Orders); auto Sorted = Samples;
        for (int Index = 0; Index < Samples.size(); ++Index) Sorted[Ranks[Index]] = Samples[Index]; Samples = Sorted;
    }
    for (int Row = 0; Row < Samples.size(); ++Row) {
        const auto& File = Samples[Row]; const int DetailRow = Two ? Row * 2 + (draft_.mode_ == LayoutMode::NameFirst ? 1 : 0) : Row;
        for (int Index = 0; Index < Visible.size(); ++Index) {
            const auto& Column = Visible[Index]; auto Value = Column.template_.isEmpty() ? Resolve(Column.fieldId_, File) : RenderTemplate(CompileTemplate(Column.template_), Column.format_, [&](const QString& Field) { return Resolve(Field, File); });
            auto Text = Value.State() == ValueState::OnDemand ? tr("<폴더>") : Format(Value, Column.format_);
            if (Column.fieldId_ == "file.attributes") Text = GetFormattedAttrText(quint32(File.attributes_), false);
            auto* Cell = new QTableWidgetItem(Text); Cell->setTextAlignment(Column.alignment_ | Qt::AlignVCenter); preview_->setItem(DetailRow, Index, Cell);
            const auto Width = hc::view::Resolve(Column.widths_[int(draft_.mode_)], preview_->viewport()->width());
            if (Width) preview_->setColumnWidth(Index, std::max(50, Width->width_));
        }
        if (Two) {
            const auto It = std::find_if(draft_.columns_.begin(), draft_.columns_.end(), [&](const auto& Column) { return Column.id_ == draft_.primaryNameId_; });
            const int NameRow = Row * 2 + (draft_.mode_ == LayoutMode::NameFirst ? 0 : 1);
            if (preview_->columnCount() > 1) preview_->setSpan(NameRow, 0, 1, preview_->columnCount());
            if (It != draft_.columns_.end()) {
                auto* Cell = new QTableWidgetItem(Format(Resolve(It->fieldId_, File), It->format_));
                Cell->setTextAlignment(It->alignment_ | Qt::AlignVCenter); preview_->setItem(NameRow, 0, Cell);
            }
        }
    }
    preview_->verticalHeader()->setDefaultSectionSize(std::max(28, fontMetrics().height() + 10));
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
bool QtColumnProfileEditor::NewTabDefault() const { return scope_->currentIndex() == 1; }
void QtColumnProfileEditor::resizeEvent(QResizeEvent* Event) { QWidget::resizeEvent(Event); updatePreview(); }
bool QtColumnProfileEditor::eventFilter(QObject* Watched, QEvent* Event)
{
    // Layout settles after the editor resize; measure the actual preview viewport.
    if (preview_ && Watched == preview_->viewport() && Event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &QtColumnProfileEditor::updatePreview);
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
