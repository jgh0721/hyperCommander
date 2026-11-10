#include "stdafx.h"
#include "qtFlatProfileEditor.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "columnAutoSelection.hpp"
#include "shellFolderToken.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmwidgets/SegmentedControl.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVariantAnimation>

DECLARE_CMNLIBSV2_NAMESPACE

using namespace hc::columns;
using P = FlatPresentation;

namespace
{
    constexpr int LabelWidth = 150, IndentStep = 18;
    enum RuleColumn { RuleEnabled, RulePath, RuleResolved, RuleProfile };
    QString Escaped(const QString& Text) { return Text.toHtmlEscaped(); }
}

QtFlatProfileEditor::QtFlatProfileEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.flat.editor");
    auto* Outer = new QVBoxLayout(this); Outer->setContentsMargins(0, 0, 0, 0);
    pages_ = new QTabWidget(this); pages_->setObjectName("hc.flat.pages"); Outer->addWidget(pages_, 1);

    // 표시 방식: 단계형. 행은 미리 모두 만들고 값에 따라 보이기 · 고정 값만 바꾼다(포커스 · 접근성 이름 유지).
    auto* Display = new QWidget(pages_); auto* DisplayRows = new QVBoxLayout(Display);
    auto* Cascade = new QWidget(Display); auto* Grid = new QGridLayout(Cascade);
    Grid->setContentsMargins(4, 4, 4, 4); Grid->setVerticalSpacing(6); Grid->setColumnStretch(1, 1);
    view_ = segment({tr("목록"), tr("썸네일")}, "hc.flat.view");
    structure_ = segment({tr("없음"), tr("폴더별 그룹"), tr("열 값 그룹"), tr("트리")}, "hc.flat.structure");
    thumbStructure_ = segment({tr("없음"), tr("폴더 구획")}, "hc.flat.thumbStructure");
    location_ = segment({tr("이름 앞"), tr("별도 열"), tr("둘째 줄")}, "hc.flat.location");
    prefixLook_ = segment({tr("흐리게"), tr("이름과 같은 색")}, "hc.flat.prefixLook");
    prefixSort_ = segment({tr("경로 → 이름"), tr("파일 이름만")}, "hc.flat.prefixSort");
    columnPlace_ = segment({tr("이름 다음"), tr("마지막")}, "hc.flat.columnPlace");
    secondRight_ = segment({tr("수정한 날짜"), tr("크기"), tr("비움")}, "hc.flat.secondRight");
    groupTitle_ = segment({tr("상대 경로 전체"), tr("마지막 폴더 이름")}, "hc.flat.groupTitle");
    groupOrder_ = segment({tr("폴더 순서"), tr("최근 수정 먼저")}, "hc.flat.groupOrder");
    groupField_ = segment({tr("수정한 날짜"), tr("만든 날짜"), tr("확장자"), tr("형식")}, "hc.flat.groupField");
    groupMode_ = segment({tr("상대 날짜"), tr("날짜"), tr("월")}, "hc.flat.groupMode");
    groupDirection_ = segment({tr("최신 먼저"), tr("오래된 것 먼저")}, "hc.flat.groupDirection");
    expand_ = segment({tr("모두 펼침"), tr("첫 그룹만"), tr("모두 접기")}, "hc.flat.expand");
    treeTotal_ = segment({tr("하위 합계"), tr("비움")}, "hc.flat.treeTotal");
    treeExpand_ = segment({tr("모두"), tr("1단계만"), tr("모두 접기")}, "hc.flat.treeExpand");
    treeLines_ = segment({tr("표시"), tr("숨김")}, "hc.flat.treeLines");
    lines_ = segment({tr("한 줄"), tr("두 줄")}, "hc.flat.lines");
    tileSize_ = segment({"64", "96", "160", "256"}, "hc.flat.tileSize");
    tileChip_ = segment({tr("타일 아래 폴더 칩"), tr("표시 안 함")}, "hc.flat.tileChip");
    auto* Titles = new QWidget(Cascade); auto* TitleRow = new QHBoxLayout(Titles); TitleRow->setContentsMargins(0, 0, 0, 0);
    titleCount_ = new QCheckBox(tr("개수"), Titles); titleSize_ = new QCheckBox(tr("크기 합계"), Titles); titleLatest_ = new QCheckBox(tr("최근 수정"), Titles);
    for (auto* Check : {titleCount_, titleSize_, titleLatest_}) { TitleRow->addWidget(Check); connect(Check, &QCheckBox::toggled, this, [this] { store(); }); }
    TitleRow->addStretch();
    titleCount_->setObjectName("hc.flat.titleCount"); titleSize_->setObjectName("hc.flat.titleSize"); titleLatest_->setObjectName("hc.flat.titleLatest");
    rowView_ = &addRow(Grid, tr("목록 형태"), view_, 0, "hc.flat.row.view");
    rowStructure_ = &addRow(Grid, tr("묶음"), structure_, 1, "hc.flat.row.structure");
    rowLocation_ = &addRow(Grid, tr("경로 위치"), location_, 2, "hc.flat.row.location");
    rowPrefixLook_ = &addRow(Grid, tr("경로 모양"), prefixLook_, 3, "hc.flat.row.prefixLook");
    rowPrefixSort_ = &addRow(Grid, tr("이름 정렬"), prefixSort_, 3, "hc.flat.row.prefixSort");
    rowColumnPlace_ = &addRow(Grid, tr("열 위치"), columnPlace_, 3, "hc.flat.row.columnPlace");
    rowSecondRight_ = &addRow(Grid, tr("둘째 줄 오른쪽"), secondRight_, 3, "hc.flat.row.secondRight");
    rowGroupField_ = &addRow(Grid, tr("그룹 기준"), groupField_, 2, "hc.flat.row.groupField");
    rowGroupMode_ = &addRow(Grid, tr("방식"), groupMode_, 3, "hc.flat.row.groupMode");
    rowGroupDirection_ = &addRow(Grid, tr("순서"), groupDirection_, 3, "hc.flat.row.groupDirection");
    rowGroupTitle_ = &addRow(Grid, tr("그룹 제목"), groupTitle_, 2, "hc.flat.row.groupTitle");
    rowGroupOrder_ = &addRow(Grid, tr("그룹 순서"), groupOrder_, 2, "hc.flat.row.groupOrder");
    rowTitle_ = &addRow(Grid, tr("제목 정보"), Titles, 2, "hc.flat.row.title");
    rowExpand_ = &addRow(Grid, tr("처음 펼침"), expand_, 2, "hc.flat.row.expand");
    rowTreeTotal_ = &addRow(Grid, tr("폴더 행 크기"), treeTotal_, 2, "hc.flat.row.treeTotal");
    rowTreeExpand_ = &addRow(Grid, tr("처음 펼침"), treeExpand_, 2, "hc.flat.row.treeExpand");
    rowTreeLines_ = &addRow(Grid, tr("트리 선"), treeLines_, 2, "hc.flat.row.treeLines");
    rowLines_ = &addRow(Grid, tr("줄"), lines_, 2, "hc.flat.row.lines");
    rowTileSize_ = &addRow(Grid, tr("타일 크기"), tileSize_, 1, "hc.flat.row.tileSize");
    rowThumbStructure_ = &addRow(Grid, tr("묶음"), thumbStructure_, 1, "hc.flat.row.thumbStructure");
    rowTileChip_ = &addRow(Grid, tr("폴더 표시"), tileChip_, 2, "hc.flat.row.tileChip");
    DisplayRows->addWidget(Cascade);
    auto* Common = new QLabel(tr("경로 표기: 기준 폴더 기준 상대 경로. 긴 경로는 가운데를 줄입니다."), Display);
    Common->setWordWrap(true); Common->setEnabled(false); DisplayRows->addWidget(Common);
    auto* PreviewTitle = new QLabel(tr("미리보기"), Display); DisplayRows->addWidget(PreviewTitle);
    preview_ = new QLabel(Display); preview_->setObjectName("hc.flat.preview"); preview_->setTextFormat(Qt::RichText);
    preview_->setFrameShape(QFrame::StyledPanel); preview_->setAlignment(Qt::AlignLeft | Qt::AlignTop); preview_->setMargin(8);
    preview_->setMinimumHeight(170); preview_->setWordWrap(false); DisplayRows->addWidget(preview_);
    DisplayRows->addStretch();
    auto* DisplayScroll = new QScrollArea(pages_); DisplayScroll->setWidgetResizable(true); DisplayScroll->setFrameShape(QFrame::NoFrame); DisplayScroll->setWidget(Display);
    pages_->addTab(DisplayScroll, tr("표시 방식"));

    // 열 · 정렬: 열 출처와 정렬.
    auto* Columns = new QWidget(pages_); auto* ColumnForm = new QFormLayout(Columns);
    columnsFrom_ = segment({tr("평면 보기를 시작한 탭의 열 세트"), tr("이 세트의 열 구성")}, "hc.flat.columnsFrom");
    ColumnForm->addRow(tr("열 출처"), columnsFrom_);
    baseSet_ = new QComboBox(Columns); baseSet_->setObjectName("hc.flat.baseSet");
    ColumnForm->addRow(tr("열 구성 가져오기"), baseSet_);
    sortColumn_ = new QComboBox(Columns); sortColumn_->setObjectName("hc.flat.sortColumn");
    sortOrder_ = new QComboBox(Columns); sortOrder_->setObjectName("hc.flat.sortOrder"); sortOrder_->addItems({tr("오름차순"), tr("내림차순")});
    auto* Sorts = new QWidget(Columns); auto* SortRow = new QHBoxLayout(Sorts); SortRow->setContentsMargins(0, 0, 0, 0);
    SortRow->addWidget(sortColumn_, 1); SortRow->addWidget(sortOrder_);
    ColumnForm->addRow(tr("정렬"), Sorts);
    auto* ColumnNote = new QLabel(tr("시작한 탭의 열 세트를 쓰면 열 · 너비 · 정렬은 그 세트를 따르고, 이 세트는 표시 방식만 정합니다. "
        "이름 정렬의 경로 순 비교는 표시 방식의 \"이름 정렬\"에서 정합니다. 폴더는 파일보다 먼저입니다(경로 순이면 같은 폴더 안에서)."), Columns);
    ColumnNote->setWordWrap(true); ColumnForm->addRow(ColumnNote);
    connect(baseSet_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_ || draft_.columnsFrom_ != FlatProfile::ColumnsFrom::Self) return;
        if (const auto Base = DecodeProfile(normalLibrary_[baseSet_->currentData().toString()].toObject())) {
            const auto Sorts = draft_.columns_.sorts_; draft_.columns_ = *Base; draft_.columns_.id_ = QUuid::createUuidV5(draft_.id_, QStringLiteral("columns"));
            if (Sorts.isEmpty()) draft_.columns_.sorts_ = Base->sorts_;
            dirty_ = true; loadColumns(); updatePreview(); emit Changed();
        }
    });
    connect(columnsFrom_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this] { storeColumns(); });
    for (auto* Combo : {sortColumn_, sortOrder_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] { storeColumns(); });
    pages_->addTab(Columns, tr("열 · 정렬"));

    // 적용 조건: 기준 폴더 조건 → 세트. 처음 맞는 조건, 없으면 기본 세트.
    auto* Policy = new QWidget(pages_); auto* PolicyRows = new QVBoxLayout(Policy);
    selectionEnabled_ = new QCheckBox(tr("평면 보기를 시작할 때 기준 폴더 조건으로 세트 고르기"), Policy); selectionEnabled_->setObjectName("hc.flat.selectionEnabled");
    PolicyRows->addWidget(selectionEnabled_);
    rules_ = new QTableWidget(0, 4, Policy); rules_->setObjectName("hc.flat.rules");
    rules_->setHorizontalHeaderLabels({tr("사용"), tr("기준 폴더 조건"), tr("해석된 경로"), tr("평면 폴더 세트")});
    rules_->horizontalHeader()->setSectionResizeMode(RulePath, QHeaderView::Stretch);
    rules_->horizontalHeader()->setSectionResizeMode(RuleResolved, QHeaderView::Stretch);
    rules_->verticalHeader()->hide(); rules_->setSelectionBehavior(QAbstractItemView::SelectRows); rules_->setSelectionMode(QAbstractItemView::SingleSelection);
    PolicyRows->addWidget(rules_, 1);
    auto* RuleButtons = new QHBoxLayout; PolicyRows->addLayout(RuleButtons);
    const auto RuleButton = [&](const QString& Label, const char* Name, auto Handler) {
        auto* Button = new QPushButton(Label, Policy); Button->setObjectName(Name); RuleButtons->addWidget(Button);
        connect(Button, &QPushButton::clicked, this, Handler);
    };
    RuleButton(tr("추가"), "hc.flat.addRule", [this] {
        appendRule(QJsonObject{{"enabled", true}, {"path", currentLocation_.isEmpty() ? QStringLiteral("C:\\*") : QDir::toNativeSeparators(currentLocation_) + "\\*"}, {"profile", IdText(draft_.id_)}});
        rules_->setCurrentCell(rules_->rowCount() - 1, RulePath); storeRules();
    });
    RuleButton(tr("제거"), "hc.flat.removeRule", [this] { if (rules_->currentRow() >= 0) { rules_->removeRow(rules_->currentRow()); storeRules(); } });
    for (const int Direction : {-1, 1}) RuleButton(Direction < 0 ? tr("위로") : tr("아래로"), Direction < 0 ? "hc.flat.ruleUp" : "hc.flat.ruleDown", [this, Direction] {
        const int Row = rules_->currentRow(), Next = Row + Direction;
        if (Row < 0 || Next < 0 || Next >= rules_->rowCount()) return;
        auto Rules = selection_["rules"].toArray(); const auto Item = Rules[Row]; Rules[Row] = Rules[Next]; Rules[Next] = Item;
        selection_["rules"] = Rules; dirty_ = true; loadRules(); rules_->setCurrentCell(Next, RulePath); refreshVerdict(); emit Changed();
    });
    RuleButtons->addStretch();
    auto* Fallback = new QHBoxLayout; PolicyRows->addLayout(Fallback);
    Fallback->addWidget(new QLabel(tr("조건이 맞지 않을 때"), Policy));
    defaultProfile_ = new QComboBox(Policy); defaultProfile_->setObjectName("hc.flat.defaultProfile"); Fallback->addWidget(defaultProfile_, 1);
    verdict_ = new QLabel(Policy); verdict_->setObjectName("hc.flat.verdict"); verdict_->setWordWrap(true); PolicyRows->addWidget(verdict_);
    auto* PolicyNote = new QLabel(tr("위에서 처음 일치하는 조건을 적용합니다. 폴더 조건은 {GUID}, shell:Downloads, ::{CLSID}를 지원하고, 끝의 \\*는 하위 폴더를 포함합니다. "
        "판정은 평면 보기를 시작할 때 기준 폴더로 한 번 합니다(파일을 읽지 않음)."), Policy);
    PolicyNote->setWordWrap(true); PolicyRows->addWidget(PolicyNote);
    connect(selectionEnabled_, &QCheckBox::toggled, this, [this](bool Enabled) { if (loading_) return; selection_["enabled"] = Enabled; dirty_ = true; refreshVerdict(); emit Changed(); });
    connect(defaultProfile_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return; selection_["defaultProfile"] = defaultProfile_->currentData().toString(); dirty_ = true;
        { const QSignalBlocker Block(defaultSet_); defaultSet_->setChecked(defaultProfile_->currentData().toString() == IdText(draft_.id_)); }
        refreshVerdict(); emit Changed();
    });
    connect(rules_, &QTableWidget::itemChanged, this, [this] { if (!loading_) storeRules(); });
    pages_->addTab(Policy, tr("적용 조건"));

    defaultSet_ = new QCheckBox(tr("평면 보기 기본 세트로 사용 (조건이 맞지 않을 때)"), this); defaultSet_->setObjectName("hc.flat.defaultSet");
    Outer->addWidget(defaultSet_);
    connect(defaultSet_, &QCheckBox::toggled, this, [this](bool Checked) {
        if (loading_) return;
        const auto Id = Checked ? IdText(draft_.id_) : IdText(FlatBuiltInProfileId);
        selection_["defaultProfile"] = Id; dirty_ = true; loadRules(); refreshVerdict(); emit Changed();
    });
    error_ = new QLabel(this); error_->setObjectName("hc.flat.error"); error_->setWordWrap(true); Outer->addWidget(error_);

    for (auto* Control : {view_, structure_, thumbStructure_, location_, prefixLook_, prefixSort_, columnPlace_, secondRight_, groupTitle_, groupOrder_,
             groupField_, groupMode_, groupDirection_, expand_, treeTotal_, treeExpand_, treeLines_, lines_, tileSize_, tileChip_})
        connect(Control, &fm::ui::SegmentedControl::currentIndexChanged, this, [this] { store(); });
    Load({}, {}, {});
}

fm::ui::SegmentedControl* QtFlatProfileEditor::segment(const QStringList& Items, const char* Name)
{
    auto* Control = new fm::ui::SegmentedControl(Items, this); Control->setObjectName(Name);
    Control->setSegmentSize(fm::ui::SegmentedControl::Compact);
    return Control;
}

QtFlatProfileEditor::Row& QtFlatProfileEditor::addRow(QGridLayout* Grid, const QString& Label, QWidget* Control, int Level, const char* Name)
{
    auto* Line = new QWidget(Grid->parentWidget()); Line->setObjectName(Name); Line->setAutoFillBackground(true);
    auto* Layout = new QHBoxLayout(Line); Layout->setContentsMargins(4 + Level * IndentStep, 2, 4, 2);
    auto* Title = new QLabel(Label, Line); Title->setMinimumWidth(LabelWidth - Level * IndentStep); Title->setBuddy(Control);
    Layout->addWidget(Title); Control->setParent(Line); Layout->addWidget(Control);
    auto* Fixed = new QLabel(Line); Fixed->setEnabled(false); Fixed->hide(); Layout->addWidget(Fixed);
    Layout->addStretch();
    Control->setAccessibleName(Label);
    Grid->addWidget(Line, Grid->rowCount(), 0, 1, 2);
    rows_.push_back({Line, Fixed, Control, {}});
    return rows_.back();
}

void QtFlatProfileEditor::highlight(Row& Target)
{
    // 새로 보이거나 값이 바뀐 하위 행을 잠깐 강조한다. 애니메이션을 끈 환경에서는 색만 잠깐 바꾼다.
    auto* Widget = Target.widget_;
    const auto Base = Widget->palette().color(QPalette::Window);
    const auto Accent = Widget->palette().color(QPalette::Highlight);
    auto Mixed = Accent; Mixed.setAlphaF(0.18);
    if (!QApplication::isEffectEnabled(Qt::UI_AnimateCombo)) {
        auto Palette = Widget->palette(); Palette.setColor(QPalette::Window, Mixed); Widget->setPalette(Palette);
        QTimer::singleShot(1000, Widget, [Widget] { Widget->setPalette(QPalette()); });
        return;
    }
    auto* Animation = new QVariantAnimation(Widget);
    Animation->setDuration(1000); Animation->setStartValue(Mixed); Animation->setEndValue(QColor(Base.red(), Base.green(), Base.blue(), 0));
    connect(Animation, &QVariantAnimation::valueChanged, Widget, [Widget](const QVariant& Value) {
        auto Palette = Widget->palette(); Palette.setColor(QPalette::Window, Value.value<QColor>()); Widget->setPalette(Palette);
    });
    connect(Animation, &QVariantAnimation::finished, Widget, [Widget] { Widget->setPalette(QPalette()); });
    Animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void QtFlatProfileEditor::Load(const QJsonObject& Library, const QJsonObject& Selection, const QJsonObject& NormalLibrary)
{
    library_ = WithBuiltInFlatProfiles(Library);
    selection_ = Selection.isEmpty() ? DefaultFlatSelection() : Selection;
    normalLibrary_ = WithBuiltInProfiles(NormalLibrary);
    dirty_ = false;
    draft_ = *DecodeFlatProfile(library_[IdText(FlatBuiltInProfileId)].toObject());
    loadColumns(); loadRules(); syncCascade(); refreshVerdict();
}

void QtFlatProfileEditor::SetNormalLibrary(const QJsonObject& Library)
{
    normalLibrary_ = WithBuiltInProfiles(Library); loadColumns();
}

void QtFlatProfileEditor::SetCurrentLocation(const QString& Path) { currentLocation_ = Path; refreshVerdict(); }

bool QtFlatProfileEditor::Select(const QUuid& Id)
{
    const auto Next = DecodeFlatProfile(library_[IdText(Id)].toObject());
    if (!Next) return false;
    if (const auto Error = ValidateFlatProfile(draft_); !Error.isEmpty() && draft_.id_ != Id) { error_->setText(Error); return false; }
    if (draft_.id_ != Id) library_[IdText(draft_.id_)] = EncodeFlatProfile(draft_);
    draft_ = *Next; error_->clear();
    loadColumns(); syncCascade();
    { const QScopedValueRollback Loading(loading_, true); defaultSet_->setChecked(selection_["defaultProfile"].toString() == IdText(draft_.id_)); }
    return true;
}

void QtFlatProfileEditor::SetTitle(const QString& Title)
{
    if (draft_.title_ == Title) return;
    draft_.title_ = Title; library_[IdText(draft_.id_)] = EncodeFlatProfile(draft_); dirty_ = true;
    loadRules(); emit Changed();
}

QUuid QtFlatProfileEditor::Add(const QUuid& From)
{
    library_[IdText(draft_.id_)] = EncodeFlatProfile(draft_);
    auto Source = DecodeFlatProfile(library_[IdText(From.isNull() ? FlatBuiltInProfileId : From)].toObject()).value_or(FlatDefaultProfile());
    Source.id_ = QUuid::createUuid(); Source.raw_ = {};
    Source.title_ = From.isNull() ? tr("새 평면 폴더 세트") : tr("%1 복사본").arg(Source.title_);
    library_[IdText(Source.id_)] = EncodeFlatProfile(Source); dirty_ = true;
    draft_ = Source; loadColumns(); loadRules(); syncCascade(); emit Changed();
    return Source.id_;
}

QUuid QtFlatProfileEditor::Remove()
{
    if (IsBuiltInFlatProfile(draft_.id_)) return draft_.id_;
    const auto Removed = IdText(draft_.id_);
    library_.remove(Removed);
    if (selection_["defaultProfile"].toString() == Removed) selection_["defaultProfile"] = IdText(FlatBuiltInProfileId);
    // 지운 세트를 쓰던 조건은 기본 제공 세트로 돌린다(조건 자체는 남긴다).
    auto Rules = selection_["rules"].toArray();
    for (int Index = 0; Index < Rules.size(); ++Index) {
        auto Rule = Rules[Index].toObject();
        if (Rule["profile"].toString() == Removed) { Rule["profile"] = IdText(FlatBuiltInProfileId); Rules[Index] = Rule; }
    }
    selection_["rules"] = Rules; dirty_ = true;
    draft_ = *DecodeFlatProfile(library_[IdText(FlatBuiltInProfileId)].toObject());
    loadColumns(); loadRules(); syncCascade(); refreshVerdict(); emit Changed();
    return draft_.id_;
}

QJsonObject QtFlatProfileEditor::Library() const
{
    auto Result = library_;
    if (const auto Encoded = EncodeFlatProfile(draft_); !Encoded.isEmpty()) Result[IdText(draft_.id_)] = Encoded;
    return Result;
}

QJsonObject QtFlatProfileEditor::Selection() const { return selection_; }

QVector<FlatProfile> QtFlatProfileEditor::Profiles() const
{
    QVector<FlatProfile> Result;
    const auto All = Library();
    for (auto It = All.begin(); It != All.end(); ++It) if (const auto Profile = DecodeFlatProfile(It.value().toObject())) Result.append(*Profile);
    std::stable_sort(Result.begin(), Result.end(), [](const auto& A, const auto& B) {
        if (IsBuiltInFlatProfile(A.id_) != IsBuiltInFlatProfile(B.id_)) return IsBuiltInFlatProfile(A.id_);
        return A.title_.localeAwareCompare(B.title_) < 0; });
    return Result;
}

QString QtFlatProfileEditor::Validate() const
{
    if (const auto Error = ValidateFlatProfile(draft_); !Error.isEmpty()) return Error;
    return ValidateFlatSelection(selection_, Library());
}

QString QtFlatProfileEditor::Summary(const QUuid& Id) const
{
    QStringList Parts;
    if (IsBuiltInFlatProfile(Id)) Parts.append(tr("기본 제공"));
    if (selection_["defaultProfile"].toString() == IdText(Id)) Parts.append(tr("평면 기본"));
    int Count = 0;
    if (selection_["enabled"].toBool(true)) for (const auto& Value : selection_["rules"].toArray()) {
        const auto Rule = Value.toObject();
        if (Rule["enabled"].toBool() && Rule["profile"].toString() == IdText(Id)) ++Count;
    }
    if (Count) Parts.append(tr("자동: 조건 %1개").arg(Count));
    if (Parts.isEmpty()) Parts.append(tr("수동"));
    return Parts.join(" · ");
}

void QtFlatProfileEditor::store()
{
    if (loading_) return;
    auto& Value = draft_.presentation_;
    Value.view_ = view_->currentIndex() == 1 ? P::View::Thumbnails : P::View::List;
    if (Value.view_ == P::View::Thumbnails) Value.structure_ = thumbStructure_->currentIndex() == 1 ? P::Structure::Folder : P::Structure::None;
    else Value.structure_ = static_cast<P::Structure>(std::clamp(structure_->currentIndex(), 0, 3));
    Value.location_ = static_cast<P::Location>(std::clamp(location_->currentIndex(), 0, 2));
    Value.prefixDim_ = prefixLook_->currentIndex() == 0; Value.prefixSortPath_ = prefixSort_->currentIndex() == 0;
    Value.columnLast_ = columnPlace_->currentIndex() == 1;
    Value.secondRight_ = static_cast<P::SecondRight>(std::clamp(secondRight_->currentIndex(), 0, 2));
    Value.groupTitleLeaf_ = groupTitle_->currentIndex() == 1; Value.groupOrderRecent_ = groupOrder_->currentIndex() == 1;
    static const QStringList Fields{"file.modified", "file.created", "file.extension", "file.type"};
    Value.groupField_ = Fields.value(groupField_->currentIndex(), "file.modified");
    static const QStringList Modes{"relativeDate", "date", "month"};
    Value.groupMode_ = IsGroupingDate(Value.groupField_) ? Modes.value(groupMode_->currentIndex(), "relativeDate") : QStringLiteral("value");
    // 날짜는 "최신 먼저"가 내림차순, 값은 "이름순"이 오름차순이다.
    Value.groupOrder_ = IsGroupingDate(Value.groupField_) ? (groupDirection_->currentIndex() == 0 ? Qt::DescendingOrder : Qt::AscendingOrder)
        : (groupDirection_->currentIndex() == 0 ? Qt::AscendingOrder : Qt::DescendingOrder);
    Value.expand_ = static_cast<ProfileGrouping::Expand>(std::clamp(expand_->currentIndex() == 1 ? 2 : expand_->currentIndex() == 2 ? 1 : 0, 0, 2));
    Value.titleCount_ = titleCount_->isChecked(); Value.titleSize_ = titleSize_->isChecked(); Value.titleLatest_ = titleLatest_->isChecked();
    Value.treeFolderTotal_ = treeTotal_->currentIndex() == 0; Value.treeLines_ = treeLines_->currentIndex() == 0;
    Value.treeExpandLevel_ = treeExpand_->currentIndex() == 0 ? -1 : treeExpand_->currentIndex() == 1 ? 1 : 0;
    Value.lines_ = lines_->currentIndex() == 1 ? 2 : 1;
    Value.tileSize_ = FlatTileSizes().value(tileSize_->currentIndex(), "96").toInt();
    Value.tileFolderChip_ = tileChip_->currentIndex() == 0;
    Value = NormalizeFlatPresentation(Value);
    dirty_ = true; syncCascade(); emit Changed();
}

void QtFlatProfileEditor::syncCascade()
{
    const QScopedValueRollback Loading(loading_, true);
    const auto Value = NormalizeFlatPresentation(draft_.presentation_);
    draft_.presentation_ = Value;
    const auto Rows = FlatRows(Value);
    const auto Set = [](fm::ui::SegmentedControl* Control, int Index) { const QSignalBlocker Block(Control); Control->setCurrentIndex(Index); };
    Set(view_, Value.view_ == P::View::Thumbnails ? 1 : 0);
    Set(structure_, int(Value.structure_)); Set(thumbStructure_, Value.structure_ == P::Structure::Folder ? 1 : 0);
    Set(location_, int(Value.location_)); Set(prefixLook_, Value.prefixDim_ ? 0 : 1); Set(prefixSort_, Value.prefixSortPath_ ? 0 : 1);
    Set(columnPlace_, Value.columnLast_ ? 1 : 0); Set(secondRight_, int(Value.secondRight_));
    Set(groupTitle_, Value.groupTitleLeaf_ ? 1 : 0); Set(groupOrder_, Value.groupOrderRecent_ ? 1 : 0);
    static const QStringList Fields{"file.modified", "file.created", "file.extension", "file.type"};
    Set(groupField_, std::max(0, int(Fields.indexOf(Value.groupField_))));
    static const QStringList Modes{"relativeDate", "date", "month"};
    Set(groupMode_, std::max(0, int(Modes.indexOf(Value.groupMode_))));
    const bool Date = IsGroupingDate(Value.groupField_);
    { const QSignalBlocker Block(groupDirection_);
      groupDirection_->setItems(Date ? QStringList{tr("최신 먼저"), tr("오래된 것 먼저")} : QStringList{tr("이름순"), tr("역순")});
      groupDirection_->setCurrentIndex(Date ? (Value.groupOrder_ == Qt::DescendingOrder ? 0 : 1) : (Value.groupOrder_ == Qt::AscendingOrder ? 0 : 1)); }
    Set(expand_, Value.expand_ == ProfileGrouping::Expand::All ? 0 : Value.expand_ == ProfileGrouping::Expand::First ? 1 : 2);
    { const QSignalBlocker A(titleCount_), B(titleSize_), C(titleLatest_);
      titleCount_->setChecked(Value.titleCount_); titleSize_->setChecked(Value.titleSize_); titleLatest_->setChecked(Value.titleLatest_); }
    Set(treeTotal_, Value.treeFolderTotal_ ? 0 : 1); Set(treeLines_, Value.treeLines_ ? 0 : 1);
    Set(treeExpand_, Value.treeExpandLevel_ < 0 ? 0 : Value.treeExpandLevel_ == 1 ? 1 : 2);
    Set(lines_, Value.lines_ == 2 ? 1 : 0); Set(tileSize_, std::max(0, int(FlatTileSizes().indexOf(QString::number(Value.tileSize_)))));
    Set(tileChip_, Value.tileFolderChip_ ? 0 : 1);
    // 행 보이기 · 고정 값. 새로 보이거나 값이 바뀐 행은 강조한다.
    const auto Show = [this](Row* Target, bool Visible, const QString& Fixed = {}) {
        const bool Was = Target->widget_->isVisibleTo(this);
        Target->widget_->setVisible(Visible);
        Target->fixed_->setVisible(Visible && !Fixed.isEmpty()); Target->fixed_->setText(Fixed);
        Target->control_->setVisible(Fixed.isEmpty());
        const auto Now = Fixed.isEmpty() ? QString::number(qobject_cast<fm::ui::SegmentedControl*>(Target->control_) ? qobject_cast<fm::ui::SegmentedControl*>(Target->control_)->currentIndex() : 0) : Fixed;
        if (Visible && isVisible() && (!Was || Target->value_ != Now)) highlight(*Target);
        Target->value_ = Now;
    };
    const bool List = Value.view_ == P::View::List;
    Show(rowView_, true);
    Show(rowStructure_, List);
    Show(rowLocation_, Rows.location_);
    Show(rowPrefixLook_, Rows.prefix_); Show(rowPrefixSort_, Rows.prefix_);
    Show(rowColumnPlace_, Rows.column_); Show(rowSecondRight_, Rows.secondLine_);
    Show(rowGroupField_, Rows.columnGroup_); Show(rowGroupMode_, Rows.columnGroup_ && Date); Show(rowGroupDirection_, Rows.columnGroup_);
    Show(rowGroupTitle_, Rows.groupTitle_); Show(rowGroupOrder_, Rows.folder_);
    Show(rowTitle_, Rows.groupTitle_ && List); Show(rowExpand_, Rows.groupTitle_);
    Show(rowTreeTotal_, Rows.tree_); Show(rowTreeExpand_, Rows.tree_); Show(rowTreeLines_, Rows.tree_);
    Show(rowLines_, List, Rows.linesFixed_ ? (Value.lines_ == 2 ? tr("두 줄 (둘째 줄 경로에서 고정)") : tr("한 줄 (트리에서 고정)")) : QString{});
    Show(rowTileSize_, !List); Show(rowThumbStructure_, !List); Show(rowTileChip_, Rows.tileChip_);
    // 트리(시안 E) · 썸네일(시안 F)은 지원이 확인된 범위에서만 고를 수 있다.
    if (auto* Tree = structure_->button(3)) Tree->setToolTip(tr("하위 폴더를 트리로 펼쳐 봅니다. 같은 부모 안에서 정렬됩니다."));
    updatePreview();
}

void QtFlatProfileEditor::loadColumns()
{
    const QScopedValueRollback Loading(loading_, true);
    const bool Self = draft_.columnsFrom_ == FlatProfile::ColumnsFrom::Self;
    columnsFrom_->setCurrentIndex(Self ? 1 : 0);
    baseSet_->clear();
    for (auto It = normalLibrary_.begin(); It != normalLibrary_.end(); ++It)
        if (const auto Profile = DecodeProfile(It.value().toObject())) baseSet_->addItem(Profile->title_, It.key());
    baseSet_->setCurrentIndex(-1); baseSet_->setEnabled(Self);
    sortColumn_->clear(); sortColumn_->addItem(tr("시작한 탭의 정렬"), QString{});
    if (Self) for (const auto& Column : draft_.columns_.columns_) sortColumn_->addItem(Column.title_, IdText(Column.id_));
    sortColumn_->setEnabled(Self); sortOrder_->setEnabled(Self);
    if (Self && !draft_.columns_.sorts_.isEmpty()) {
        sortColumn_->setCurrentIndex(std::max(0, sortColumn_->findData(IdText(draft_.columns_.sorts_.front().columnId_))));
        sortOrder_->setCurrentIndex(draft_.columns_.sorts_.front().order_ == Qt::DescendingOrder ? 1 : 0);
    }
}

void QtFlatProfileEditor::storeColumns()
{
    if (loading_) return;
    const bool Self = columnsFrom_->currentIndex() == 1;
    if (Self && draft_.columnsFrom_ != FlatProfile::ColumnsFrom::Self) {
        // 처음 이 세트의 열 구성을 쓰면 기본 일반 세트에서 시작한다.
        if (draft_.columns_.columns_.isEmpty()) { draft_.columns_ = DefaultProfile(); draft_.columns_.id_ = QUuid::createUuidV5(draft_.id_, QStringLiteral("columns")); }
    }
    draft_.columnsFrom_ = Self ? FlatProfile::ColumnsFrom::Self : FlatProfile::ColumnsFrom::Origin;
    if (Self) {
        const QUuid Column(sortColumn_->currentData().toString());
        if (!Column.isNull()) draft_.columns_.sorts_ = {{Column, sortOrder_->currentIndex() == 1 ? Qt::DescendingOrder : Qt::AscendingOrder}};
    }
    dirty_ = true; loadColumns(); updatePreview(); emit Changed();
}

void QtFlatProfileEditor::appendRule(const QJsonObject& Rule)
{
    const QScopedValueRollback Loading(loading_, true);
    const int Row = rules_->rowCount(); rules_->insertRow(Row);
    auto* Enabled = new QTableWidgetItem; Enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    Enabled->setCheckState(Rule["enabled"].toBool() ? Qt::Checked : Qt::Unchecked); rules_->setItem(Row, RuleEnabled, Enabled);
    rules_->setItem(Row, RulePath, new QTableWidgetItem(Rule["path"].toString()));
    auto* Resolved = new QTableWidgetItem; Resolved->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); rules_->setItem(Row, RuleResolved, Resolved);
    const auto Token = hc::shell::ParseFolderToken(Rule["path"].toString());
    Resolved->setText(Token.kind_ == hc::shell::FolderToken::Kind::None ? QString{} : Token.kind_ == hc::shell::FolderToken::Kind::Invalid
        ? tr("셸 폴더 형식 오류") : hc::shell::ResolveFolderToken(Token).path_);
    auto* Profile = new QComboBox(rules_);
    for (const auto& Item : Profiles()) Profile->addItem(Item.title_, IdText(Item.id_));
    Profile->setCurrentIndex(std::max(0, Profile->findData(Rule["profile"].toString())));
    rules_->setCellWidget(Row, RuleProfile, Profile);
    connect(Profile, &QComboBox::currentIndexChanged, this, [this] { if (!loading_) storeRules(); });
}

void QtFlatProfileEditor::loadRules()
{
    {
        const QScopedValueRollback Loading(loading_, true);
        selectionEnabled_->setChecked(selection_["enabled"].toBool(true));
        rules_->setRowCount(0);
        defaultProfile_->clear();
        for (const auto& Item : Profiles()) defaultProfile_->addItem(Item.title_, IdText(Item.id_));
        defaultProfile_->setCurrentIndex(std::max(0, defaultProfile_->findData(selection_["defaultProfile"].toString())));
        defaultSet_->setChecked(selection_["defaultProfile"].toString() == IdText(draft_.id_));
    }
    for (const auto& Value : selection_["rules"].toArray()) appendRule(Value.toObject());
    refreshVerdict();
}

void QtFlatProfileEditor::storeRules()
{
    QJsonArray Rules;
    for (int Row = 0; Row < rules_->rowCount(); ++Row) {
        auto* Profile = qobject_cast<QComboBox*>(rules_->cellWidget(Row, RuleProfile));
        Rules.append(QJsonObject{{"enabled", rules_->item(Row, RuleEnabled)->checkState() == Qt::Checked},
            {"path", rules_->item(Row, RulePath)->text().trimmed()}, {"profile", Profile ? Profile->currentData().toString() : IdText(FlatBuiltInProfileId)}});
        const auto Token = hc::shell::ParseFolderToken(rules_->item(Row, RulePath)->text());
        const QScopedValueRollback Loading(loading_, true);
        rules_->item(Row, RuleResolved)->setText(Token.kind_ == hc::shell::FolderToken::Kind::None ? QString{}
            : Token.kind_ == hc::shell::FolderToken::Kind::Invalid ? tr("셸 폴더 형식 오류") : hc::shell::ResolveFolderToken(Token).path_);
    }
    selection_["rules"] = Rules; dirty_ = true; refreshVerdict(); emit Changed();
}

void QtFlatProfileEditor::refreshVerdict()
{
    if (!verdict_) return;
    if (const auto Error = ValidateFlatSelection(selection_, Library()); !Error.isEmpty()) { verdict_->setText(Error); return; }
    if (currentLocation_.isEmpty()) { verdict_->setText(tr("판정: 현재 위치가 없습니다.")); return; }
    QHash<QString, QString> Resolved;
    for (const auto& Value : selection_["rules"].toArray()) {
        const auto Token = hc::shell::ParseFolderToken(Value.toObject()["path"].toString());
        if (!Resolved.contains(Token.token_) && Token.kind_ != hc::shell::FolderToken::Kind::None && Token.kind_ != hc::shell::FolderToken::Kind::Invalid)
            Resolved[Token.token_] = hc::shell::ResolveFolderToken(Token).path_;
    }
    int Matched = -1;
    const auto Profile = SelectFlatProfile(selection_, Library(), QDir::fromNativeSeparators(currentLocation_),
        [&](const QString& Pattern, const QString& Path) { return MatchFolderCondition(QDir::fromNativeSeparators(Pattern), Path, Resolved); }, &Matched);
    verdict_->setText(tr("판정: %1 → %2 (%3)").arg(QDir::toNativeSeparators(currentLocation_), Profile.title_,
        Matched >= 0 ? tr("%1번 조건").arg(Matched + 1) : tr("기본 세트")));
}

void QtFlatProfileEditor::updatePreview()
{
    if (!preview_) return;
    // 내장 예제(상대 경로를 가진 항목 몇 개)로 표시 방식을 보인다. 실제 적용은 같은 합성 규칙을 쓴다.
    struct Sample { QString folder_, name_, size_, date_, group_; };
    const QVector<Sample> Samples{{"", "readme.md", "2 KB", "2026-11-09", tr("어제")}, {"docs", "guide.pdf", "1.2 MB", "2026-11-10", tr("오늘")},
        {"docs\\images", "cover.png", "640 KB", "2026-10-02", tr("지난달")}, {"src", "main.cpp", "18 KB", "2026-11-10", tr("오늘")},
        {"src\\ui", "panel.cpp", "42 KB", "2026-11-03", tr("이번 주")}};
    const auto Value = NormalizeFlatPresentation(draft_.presentation_);
    const auto Dim = QStringLiteral("color:%1").arg(palette().color(QPalette::PlaceholderText).name());
    const auto Path = [&](const Sample& Item) { return Item.folder_.isEmpty() ? QString{} : Item.folder_ + "\\"; };
    QString Html = "<table cellspacing=0 cellpadding=2>";
    const auto Line = [&](const QString& Name, const QString& Extra = {}) {
        Html += "<tr><td style='padding-right:18px'>" + Name + "</td><td>" + Extra + "</td></tr>";
    };
    if (Value.view_ == P::View::Thumbnails) {
        Html += "<tr>";
        for (const auto& Item : Samples) {
            Html += "<td style='padding:4px 10px 4px 0' align=center>▣<br>" + Escaped(Item.name_);
            if (Value.structure_ == P::Structure::None && Value.tileFolderChip_ && !Item.folder_.isEmpty())
                Html += "<br><span style='" + Dim + "'>" + Escaped(Item.folder_) + "</span>";
            Html += "</td>";
        }
        Html += "</tr>";
        if (Value.structure_ == P::Structure::Folder) Html = "<b>" + tr("폴더 구획") + "</b> · " + Escaped(tr("docs, docs\\images, src …")) + "<br>" + Html;
    } else if (Value.structure_ == P::Structure::Tree) {
        Line("▾ docs"); Line("&nbsp;&nbsp;&nbsp;guide.pdf", Samples[1].size_); Line("&nbsp;&nbsp;&nbsp;▾ images"); Line("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;cover.png", Samples[2].size_);
        Line("▾ src"); Line("&nbsp;&nbsp;&nbsp;main.cpp", Samples[3].size_); Line("readme.md", Samples[0].size_);
    } else {
        // 예제는 경로 순(기준 폴더 → docs → docs\images → src → src\ui)으로 적혀 있다. "파일 이름만"이면 이름순으로 섞는다.
        auto Ordered = Samples;
        if (Value.structure_ == P::Structure::None && Value.location_ == P::Location::Prefix && !Value.prefixSortPath_)
            std::stable_sort(Ordered.begin(), Ordered.end(), [](const Sample& A, const Sample& B) { return A.name_.compare(B.name_, Qt::CaseInsensitive) < 0; });
        QString Group;
        for (const auto& Item : Ordered) {
            if (Value.structure_ == P::Structure::Folder || Value.structure_ == P::Structure::Column) {
                auto Title = Value.structure_ == P::Structure::Folder ? (Item.folder_.isEmpty() ? tr("기준 폴더") : Value.groupTitleLeaf_ ? Item.folder_.section('\\', -1) : Item.folder_)
                    : Value.groupField_ == "file.extension" ? Item.name_.section('.', -1).toUpper() : Item.group_;
                if (Title != Group) { Group = Title; Html += "<tr><td colspan=2><b>▾ " + Escaped(Title) + "</b>" + (Value.titleCount_ ? " · 1개" : "") + "</td></tr>"; }
            }
            QString Name = Escaped(Item.name_);
            if (Value.structure_ != P::Structure::Folder) {
                if (Value.location_ == P::Location::Prefix) Name = (Value.prefixDim_ ? "<span style='" + Dim + "'>" : "<span>") + Escaped(Path(Item)) + "</span>" + Name;
                else if (Value.location_ == P::Location::Column) {
                    const auto Folder = "<span>" + Escaped(Item.folder_) + "</span>";
                    Line(Value.columnLast_ ? Name : Name + "</td><td style='padding-right:18px'>" + Folder, (Value.columnLast_ ? Item.size_ + "</td><td>" + Folder : Item.size_));
                    continue;
                } else {
                    const auto Right = Value.secondRight_ == P::SecondRight::Modified ? Item.date_ : Value.secondRight_ == P::SecondRight::Size ? Item.size_ : QString{};
                    Line(Name); Line("<span style='" + Dim + "'>" + Escaped(Item.folder_) + "</span>", Right);
                    continue;
                }
            }
            Line(Name, Value.lines_ == 2 ? QString{} : Item.size_ + " &nbsp; " + Item.date_);
            if (Value.lines_ == 2) Line("<span style='" + Dim + "'>" + Item.size_ + " · " + Item.date_ + "</span>");
        }
    }
    Html += "</table>";
    preview_->setText(Html);
}
