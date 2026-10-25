#pragma once
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
#include <fmstyle/ThemeManager.h>
#include "fileColorConditionRow.hpp"
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
        if (Selected && fm::style::ThemeManager::instance().design() == fm::style::Design::Standard && Index.column() == 0) Styled.font.setBold(true);
        const auto Effects = effects_(Index.row());
        Painter->setFont(Styled.font); const auto Rect = Option.rect.adjusted(6, 0, -6, 0);
        if (Effects["dim"].toBool()) Painter->setOpacity(Painter->opacity() * 0.55);
        Painter->drawText(Rect, Qt::AlignVCenter | (Index.column() == 1 ? Qt::AlignRight : Qt::AlignLeft), QFontMetrics(Styled.font).elidedText(Styled.text, Qt::ElideRight, Rect.width()));
        if (Option.state & QStyle::State_HasFocus) { QStyleOptionFocusRect Focus; Focus.rect = Option.rect.adjusted(1, 1, -1, -1); Focus.state = Option.state; Focus.backgroundColor = Colors.second; QApplication::style()->drawPrimitive(QStyle::PE_FrameFocusRect, &Focus, Painter); }
        const auto RowRect = table_->visualRect(Index.siblingAtColumn(0)).united(table_->visualRect(Index.siblingAtColumn(table_->columnCount() - 1)));
        Painter->setClipRect(Option.rect, Qt::IntersectClip);
        PaintFileRuleBorder(Painter, RowRect, resolve_(Index.row(), Option.state.testFlag(QStyle::State_Selected), true).first, Effects);
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
        auto* Layout = new QVBoxLayout(this);
        auto* Header = new fm::ui::DialogHeader(this); Header->setTitle(tr("파일 그룹 · 색상"));
        Header->setSubtitle(tr("핸들을 끌어 우선순위를 바꿉니다. 위 그룹부터 적용하며, 빈 색·글꼴은 기본 표시를 따릅니다.")); Layout->addWidget(Header);
        auto* Body = new QHBoxLayout; Layout->addLayout(Body, 1);
        auto* GroupsCard = new fm::ui::Card(this); GroupsCard->setObjectName("hc.rules.groupsCard");
        GroupsCard->setFixedWidth(280); Body->addWidget(GroupsCard);
        auto* Left = new QVBoxLayout(GroupsCard);
        Left->addWidget(new QLabel(tr("파일 그룹 · 위에서부터 적용"), GroupsCard));
        auto* Actions = new QHBoxLayout; Left->addLayout(Actions);
        const auto Button = [this](const QString& Text, QBoxLayout* Into, auto Action) {
            auto* Item = new fm::ui::Button(Text, this); Item->setCompact(true); Item->setAutoDefault(false); Into->addWidget(Item);
            connect(Item, &QPushButton::clicked, this, Action); return Item;
        };
        auto* NewGroup = Button(tr("추가"), Actions, [this] { addGroup(false); }); NewGroup->setAccessibleName(tr("새 그룹")); NewGroup->setObjectName("hc.rules.add");
        auto* AddShortcut = new QShortcut(QKeySequence("Alt+W"), this); connect(AddShortcut, &QShortcut::activated, this, [this] { addGroup(false); });
        NewGroup->setToolTip(tr("새 그룹 · Alt+W"));
        Button(tr("복제"), Actions, [this] { addGroup(true); })->setObjectName("hc.rules.copy");
        Button(tr("삭제"), Actions, [this] { auto Groups = document_["groups"].toArray(); const int Row = list_->currentRow(); if (Row < 0) return; Groups.removeAt(Row); document_["groups"] = Groups; refreshList(qMin(Row, int(Groups.size()) - 1)); })->setObjectName("hc.rules.delete");
        auto* Up = Button(tr("↑"), Actions, [this] { list_->MoveRow(list_->currentRow(), list_->currentRow() - 1); });
        Up->setObjectName("hc.rules.moveUp"); Up->setAccessibleName(tr("그룹 위로 이동")); Up->setToolTip(tr("위로 이동 · Alt+↑"));
        auto* Down = Button(tr("↓"), Actions, [this] { list_->MoveRow(list_->currentRow(), list_->currentRow() + 1); });
        Down->setObjectName("hc.rules.moveDown"); Down->setAccessibleName(tr("그룹 아래로 이동")); Down->setToolTip(tr("아래로 이동 · Alt+↓"));
        list_ = new fm::ui::ReorderListWidget(this); list_->setObjectName("hc.rules.groups"); list_->setAccessibleName(tr("파일 그룹 우선순위")); Left->addWidget(list_, 1);
        list_->setItemDelegate(new FileColorGroupDelegate(list_->itemDelegate(), list_)); list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        Left->addWidget(new QLabel(tr("여러 그룹에 해당할 때"), GroupsCard));
        mode_ = new QButtonGroup(this); mode_->setObjectName("hc.rules.mode");
        auto* First = new QRadioButton(tr("위 그룹 하나만"), GroupsCard); First->setObjectName("hc.rules.first"); mode_->addButton(First, 0); Left->addWidget(First);
        auto* Merge = new QRadioButton(tr("속성별로 합치기"), GroupsCard); Merge->setObjectName("hc.rules.merge"); mode_->addButton(Merge, 1); Left->addWidget(Merge);
        auto* PolicyHelp = new QLabel(tr("위 그룹 하나만: 처음 일치한 그룹을 적용합니다.\n합치기: 각 속성을 지정한 첫 그룹의 값을 사용합니다."), GroupsCard);
        PolicyHelp->setWordWrap(true); Left->addWidget(PolicyHelp);
        auto* Files = new QHBoxLayout;
        Button(tr("가져오기"), Files, [this] {
            const auto Path = QFileDialog::getOpenFileName(this, tr("파일 색상 규칙 가져오기"), {}, "JSON (*.json)"); if (Path.isEmpty()) return;
            QFile File(Path); if (!File.open(QIODevice::ReadOnly) || File.size() > 1024 * 1024) { error_->setText(tr("1MB 이하 JSON 파일을 읽을 수 없습니다.")); return; }
            const auto Doc = QJsonDocument::fromJson(File.readAll()).object(); const auto Error = settings::FileColorRules::Validate(Doc, false);
            if (!Error.isEmpty()) { error_->setText(Error); return; } Load(Doc);
        })->setObjectName("hc.rules.import");
        Button(tr("내보내기"), Files, [this] {
            if (!Validate()) return;
            const auto Path = QFileDialog::getSaveFileName(this, tr("파일 색상 규칙 내보내기"), {}, "JSON (*.json)"); if (Path.isEmpty()) return;
            if (QFileInfo(Path).absoluteFilePath() == QFileInfo(settings::AppSettings().FileName()).absoluteFilePath()) { error_->setText(tr("사용 중인 설정 파일은 적용/확인으로 저장하세요.")); return; }
            QSaveFile File(Path); File.setDirectWriteFallback(false); auto Bytes = QJsonDocument(document_).toJson(); Bytes.replace("\n", "\r\n");
            if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) error_->setText(tr("규칙을 저장하지 못했습니다."));
        })->setObjectName("hc.rules.export");
        auto* Scroll = new QScrollArea(this); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame); Body->addWidget(Scroll, 3);
        editor_ = new QWidget; Scroll->setWidget(editor_); auto* Right = new QVBoxLayout(editor_);
        Right->setContentsMargins(0, 0, 0, 0);
        const auto Card = [this, Right](const QString& Title, const QString& ObjectName) {
            auto* Widget = new fm::ui::Card(editor_); Widget->setObjectName(ObjectName); auto* Rows = new QVBoxLayout(Widget);
            auto* Heading = new QLabel(Title, Widget); auto Font = Heading->font(); Font.setBold(true); Heading->setFont(Font); Rows->addWidget(Heading); Right->addWidget(Widget); return Rows;
        };
        auto* Definition = Card(tr("그룹 정의"), "hc.rules.definitionCard");
        auto* Form = new QFormLayout; Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Definition->addLayout(Form);
        name_ = new QLineEdit(editor_); name_->setObjectName("hc.rules.name"); Form->addRow(tr("이름(&N)"), name_);
        join_ = new QButtonGroup(this);
        auto* JoinRow = new QWidget(editor_); auto* JoinButtons = new QHBoxLayout(JoinRow); JoinButtons->setContentsMargins(0, 0, 0, 0);
        for (int I = 0; I < 2; ++I) { auto* Choice = new QPushButton(I ? tr("모두 맞으면") : tr("하나라도 맞으면"), JoinRow); Choice->setCheckable(true); Choice->setAutoDefault(false); Choice->setObjectName(I ? "hc.rules.all" : "hc.rules.any"); join_->addButton(Choice, I); JoinButtons->addWidget(Choice); }
        Form->addRow(tr("다음 조건 중"), JoinRow);
        conditions_ = new QTableWidget(editor_); conditions_->setObjectName("hc.rules.conditions"); conditions_->setColumnCount(4); conditions_->verticalHeader()->hide();
        conditions_->setHorizontalHeaderLabels({tr("항목"), tr("비교 방법"), tr("값"), tr("삭제")});
        conditions_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); conditions_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed); conditions_->setColumnWidth(3, 40); conditions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        conditions_->setColumnWidth(0, qMax(105, QFontMetrics(conditions_->font()).horizontalAdvance(tr("파일명 정규화")) + 36)); conditions_->setColumnWidth(1, 110);
        conditions_->setSelectionBehavior(QAbstractItemView::SelectRows); Definition->addWidget(conditions_);
        auto* PatternHelp = new QLabel(tr("확장자는 마지막 . 뒤, 파일명은 확장자를 포함한 전체 이름입니다.\n와일드카드: 전체 일치(*, ?) · 정규표현식: 부분 일치(전체는 \\A…\\z)"), editor_);
        PatternHelp->setObjectName("hc.rules.patternHelp"); PatternHelp->setWordWrap(true);
        PatternHelp->setFixedHeight(PatternHelp->fontMetrics().lineSpacing() * 2 + 6); Definition->addWidget(PatternHelp);
        auto* ConditionButtons = new QHBoxLayout; Definition->addLayout(ConditionButtons);
        auto* AddCondition = Button(tr("조건 추가(&D)"), ConditionButtons, [this] { if (conditionRows_.size() >= 32) return; ensureVersion2(); appendCondition(settings::FileColorRules::NormalizeCondition({{"type", "extension"}, {"value", "txt"}}, 1)); storeForm(); });
        AddCondition->setObjectName("hc.rules.addCondition"); ConditionButtons->addStretch();
        conditions_->setAccessibleName(tr("그룹 조건")); conditions_->setToolTip(tr("확장자: txt, cpp · 무확장자: <none>\n파일명 패턴: CMakeLists.txt; *.cmake · 크기: 단위와 비교 방법 선택\n속성: 비트 값 또는 메뉴 선택 · 날짜: UTC 기준 · 확인되지 않은 날짜는 일치하지 않습니다."));
        auto* Appearance = Card(tr("색상 · 글꼴"), "hc.rules.appearanceCard");
        auto* Colors = new QGridLayout; Appearance->addLayout(Colors);
        Colors->addWidget(new QLabel(tr("라이트 테마"), editor_), 0, 1); Colors->addWidget(new QLabel(tr("다크 테마"), editor_), 0, 2);
        for (int I = 0; I < 4; ++I) {
            colors_[I] = new fm::ui::ColorPickButton(editor_); colors_[I]->setAllowNone(true); colors_[I]->setAutoDefault(false);
            colors_[I]->setObjectName("hc.rules.color." + QString::number(I));
            colors_[I]->setAccessibleName((I % 2 ? tr("다크 ") : tr("라이트 ")) + (I / 2 ? tr("배경색") : tr("글자색")));
            Colors->addWidget(colors_[I], I / 2 + 1, I % 2 + 1);
            connect(colors_[I], &fm::ui::ColorPickButton::colorChanged, this, [this] { storeForm(); });
        }
        Colors->addWidget(new QLabel(tr("글자색"), editor_), 1, 0); Colors->addWidget(new QLabel(tr("배경색"), editor_), 2, 0);
        auto* DefaultHelp = new QLabel(tr("지정하지 않은 색·글꼴은 아래 그룹 또는 목록 기본값을 따릅니다."), editor_); DefaultHelp->setWordWrap(true); Appearance->addWidget(DefaultHelp);
        autoDark_ = new QCheckBox(tr("라이트 색에서 다크 색 자동 계산"), editor_); autoDark_->setObjectName("hc.rules.autoDark"); Appearance->addWidget(autoDark_);
        autoDark_->setToolTip(tr("색조·채도·투명도를 유지하고 밝기를 다크용으로 조정합니다. 끄면 저장된 수동 다크 색으로 돌아갑니다."));
        connect(autoDark_, &QCheckBox::toggled, this, [this] { if (loading_) return; ensureVersion2(); storeForm(); refreshColors(); });
        for (int I = 0; I < 2; ++I) { contrast_[I] = new QLabel(editor_); contrast_[I]->setObjectName("hc.rules.contrast." + QString::number(I)); contrast_[I]->setWordWrap(true); Appearance->addWidget(contrast_[I]); }
        auto* FontRow = new QGridLayout; Appearance->addLayout(FontRow);
        const QStringList FontNames{tr("굵게"), tr("기울임"), tr("밑줄"), tr("취소선"), tr("흐려짐")};
        for (int I = 0; I < fontKeys_.size(); ++I) {
            fonts_[I] = new FileFontEffectButton(FontNames[I], editor_); fonts_[I]->setObjectName("hc.rules.font." + fontKeys_[I]); FontRow->addWidget(fonts_[I], I / 4, I % 4);
            connect(fonts_[I], &QPushButton::clicked, this, [this, I] { fonts_[I]->SetState((fonts_[I]->State() + 1) % 3); storeForm(); });
        }
        FontRow->addWidget(new QLabel(tr("행 테두리"), editor_), 1, 1);
        border_ = new QComboBox(editor_); border_->setObjectName("hc.rules.border"); border_->setAccessibleName(tr("파일 행 전체 테두리"));
        const QStringList BorderNames{tr("기본값"), tr("없음"), tr("실선"), tr("파선"), tr("점선"), tr("일점쇄선"), tr("이점쇄선")};
        const QStringList BorderKeys{"", "none", "solid", "dash", "dot", "dashDot", "dashDotDot"};
        for (int I = 0; I < BorderNames.size(); ++I) border_->addItem(BorderNames[I], BorderKeys[I]);
        border_->setToolTip(tr("파일 행 전체에 테두리를 표시합니다. 색상은 파일의 글자색을 따릅니다. 기본값은 아래 그룹을 따르고 없음은 테두리를 해제합니다."));
        FontRow->addWidget(border_, 1, 2, 1, 2); connect(border_, &QComboBox::currentIndexChanged, this, [this] { storeForm(); });
        auto* PreviewCard = Card(tr("미리보기"), "hc.rules.previewCard");
        previewTable_ = new QTableWidget(6, 3, editor_); previewTable_->setObjectName("hc.rules.previewTable"); previewTable_->setAccessibleName(tr("예제 파일과 적용 그룹"));
        previewTable_->setHorizontalHeaderLabels({tr("이름"), tr("크기"), tr("적용된 그룹")}); previewTable_->verticalHeader()->hide(); previewTable_->setShowGrid(false);
        previewTable_->setEditTriggers(QAbstractItemView::NoEditTriggers); previewTable_->setSelectionBehavior(QAbstractItemView::SelectRows); previewTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        previewTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); previewTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed); previewTable_->setColumnWidth(1, 78);
        previewTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive); previewTable_->setColumnWidth(2, 170);
        previewTable_->setItemDelegate(new FileRulePreviewDelegate([this](int Row, bool Selected, bool Name) { return recordPair(previewDark_->isChecked(), previewStyles_.value(Row), Selected, Name); },
            [this](int Row) { return previewStyles_.value(Row).font_; }, previewTable_));
        PreviewCard->addWidget(previewTable_);
        auto* PreviewControls = new QHBoxLayout; PreviewCard->addLayout(PreviewControls);
        previewDark_ = new QCheckBox(tr("다크"), editor_); previewDark_->setObjectName("hc.rules.previewDark"); previewDark_->setChecked(fm::style::isDarkVariant(fm::style::ThemeManager::instance().effectiveVariant())); PreviewControls->addWidget(previewDark_);
        auto* CustomToggle = new QToolButton(editor_); CustomToggle->setText(tr("직접 파일 조건 입력")); CustomToggle->setObjectName("hc.rules.customToggle"); CustomToggle->setCheckable(true); CustomToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); CustomToggle->setArrowType(Qt::RightArrow); PreviewControls->addWidget(CustomToggle); PreviewControls->addStretch();
        auto* SampleHelp = new QLabel(tr("예제 기준: 2026-01-15 12:00 UTC · 행을 선택하면 선택색을 확인할 수 있습니다."), editor_); SampleHelp->setWordWrap(true); PreviewCard->addWidget(SampleHelp);
        auto* Custom = new QWidget(editor_); auto* CustomLayout = new QVBoxLayout(Custom); CustomLayout->setContentsMargins(0, 0, 0, 0); PreviewCard->addWidget(Custom); Custom->hide();
        connect(CustomToggle, &QToolButton::toggled, this, [Custom, CustomToggle](bool Open) { Custom->setVisible(Open); CustomToggle->setArrowType(Open ? Qt::DownArrow : Qt::RightArrow); });
        auto* PreviewRow = new QHBoxLayout; CustomLayout->addLayout(PreviewRow);
        previewName_ = new QLineEdit("sample.txt", editor_); previewName_->setAccessibleName(tr("미리보기 파일 이름")); previewName_->setObjectName("hc.rules.sample"); PreviewRow->addWidget(previewName_);
        previewFolder_ = new QCheckBox(tr("폴더"), editor_); PreviewRow->addWidget(previewFolder_);
        previewSelected_ = new QCheckBox(tr("선택"), editor_); previewSelected_->setObjectName("hc.rules.sampleSelected"); PreviewRow->addWidget(previewSelected_);
        connect(previewSelected_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        auto* SampleForm = new QFormLayout; SampleForm->setRowWrapPolicy(QFormLayout::WrapLongRows); CustomLayout->addLayout(SampleForm);
        previewSize_ = new QLineEdit("0", editor_); previewSize_->setObjectName("hc.rules.sampleSize"); previewSize_->setMaxLength(20); SampleForm->addRow(tr("크기 · 바이트"), previewSize_);
        previewAttributes_ = new QLineEdit("0", editor_); previewAttributes_->setObjectName("hc.rules.sampleAttributes"); previewAttributes_->setMaxLength(10); SampleForm->addRow(tr("속성 · 비트 마스크"), previewAttributes_);
        auto* DateSample = new QWidget(editor_); auto* DateFields = new QHBoxLayout(DateSample); DateFields->setContentsMargins(0, 0, 0, 0);
        previewModifiedKnown_ = new QCheckBox(tr("확인됨"), DateSample); previewModifiedKnown_->setObjectName("hc.rules.sampleModifiedKnown"); DateFields->addWidget(previewModifiedKnown_);
        previewModified_ = new QDateTimeEdit(QDateTime::currentDateTimeUtc(), DateSample); previewModified_->setObjectName("hc.rules.sampleModified"); previewModified_->setTimeZone(QTimeZone::UTC); previewModified_->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'"); previewModified_->setEnabled(false); DateFields->addWidget(previewModified_, 1); SampleForm->addRow(tr("수정 시각"), DateSample);
        DateSample->setToolTip(tr("수정 시각을 확인할 수 없으면 날짜 조건에 일치하지 않습니다. 최근 기간은 현재 UTC 시각을 기준으로 판정합니다."));
        previewLink_ = new QComboBox(editor_); previewLink_->setObjectName("hc.rules.sampleLink"); previewLink_->addItems({tr("파일명·속성 사용"), tr("심볼릭 링크"), tr("정션"), tr("종류를 모르는 재분석 지점")}); SampleForm->addRow(tr("링크 종류"), previewLink_);
        connect(previewModifiedKnown_, &QCheckBox::toggled, this, [this](bool Known) { previewModified_->setEnabled(Known); updatePreview(); });
        connect(previewModified_, &QDateTimeEdit::dateTimeChanged, this, [this] { updatePreview(); });
        connect(previewLink_, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
        preview_ = new FileRulePreviewLabel(editor_); preview_->setTextFormat(Qt::PlainText); preview_->setAutoFillBackground(true); preview_->setMinimumHeight(40); CustomLayout->addWidget(preview_);
        owners_ = new QLabel(editor_); owners_->setTextFormat(Qt::PlainText); owners_->setWordWrap(true); CustomLayout->addWidget(owners_); Right->addStretch();
        error_ = new QLabel(this); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); Layout->addWidget(error_);
        Button(tr("기존 규칙 가져오기"), Files, [this] { ImportLegacy(); });
        auto* Examples = Button(tr("기본 예제 그룹 추가"), Files, [this] { addExamples(); }); Examples->setObjectName("hc.rules.addExamples"); Examples->setToolTip(tr("10가지 예제 중 아직 없는 그룹을 뒤에 추가합니다. 기존 그룹과 우선순위는 유지합니다."));
        Files->addStretch(); Layout->addLayout(Files);
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
        connect(previewDark_, &QCheckBox::toggled, this, [this] { updatePreview(); });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { const bool WasLoading = loading_; loading_ = true; updateGroupItems(); loading_ = WasLoading; updatePreview(); });
        previewTimer_.setSingleShot(true); previewTimer_.setInterval(150);
        connect(&previewTimer_, &QTimer::timeout, this, [this] { startPreview(); });
        Load(settings::FileColorRules::Empty());
    }
    void Load(const QJsonObject& Document) { document_ = Document.isEmpty() ? settings::FileColorRules::Empty() : Document; supported_ = settings::FileColorRules::Supported(document_); setEnabled(supported_); refreshList(0); if (!supported_) error_->setText(tr("다른 버전의 파일 색상 규칙은 원본 그대로 보존합니다.")); }
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
        auto* Row = new FileColorConditionRow(conditions_, Condition, document_["version"].toInt()); conditionRows_.append(Row);
        Row->onEdited_ = [this] { if (loading_) return; ensureVersion2(); storeForm(); };
        Row->onRemove_ = [this, Row] { if (loading_ || Row->Row() < 0) return; ensureVersion2(); conditions_->removeRow(Row->Row()); conditionRows_.removeAll(Row); Row->deleteLater(); storeForm(); };
    }
    void updateConditionHeight() {
        int Height = conditions_->horizontalHeader()->sizeHint().height() + 2 * conditions_->frameWidth();
        if (!conditions_->rowCount()) Height += conditions_->verticalHeader()->defaultSectionSize();
        for (int I = 0; I < qMin(conditions_->rowCount(), 3); ++I) Height += conditions_->rowHeight(I);
        conditions_->setFixedHeight(Height);
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
                const QMap<QString, QString> Comparisons{{"gt", ">"}, {"ge", "≥"}, {"lt", "<"}, {"le", "≤"}, {"eq", "="}, {"ne", "≠"}, {"any", tr("하나라도")}, {"all", tr("모두")}, {"within", tr("최근")}, {"before", tr("이전")}, {"after", tr("이후")}, {"notMatches", tr("불일치")}, {"notIn", tr("제외")}};
                QString Label = Types.value(typeKeys_.indexOf(C["type"].toString()));
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
            auto* Item = list_->item(Row); const auto Title = QString::number(Row + 1) + ". " + G["name"].toString() + (G["example"].toBool() ? tr(" · 예제") : QString{});
            Item->setText(Title + "\n" + list_->fontMetrics().elidedText(Summary, Qt::ElideRight, qMax(80, list_->viewport()->width() - 110)));
            Item->setToolTip(Title + "\n" + Summary); Item->setData(Qt::AccessibleTextRole, Title + " " + Summary);
            QPixmap Swatch(QSize(40, 32) * devicePixelRatioF()); Swatch.setDevicePixelRatio(devicePixelRatioF());
            const auto Colors = settings::FileColorRules::Colors(G, Dark); settings::FileRuleResult Style; Style.foreground_ = QColor(Colors["foreground"].toString()); Style.background_ = QColor(Colors["background"].toString()); Style.font_ = G["font"].toObject();
            const auto Pair = recordPair(Dark, Style, false); Swatch.fill(Pair.second);
            QPainter Painter(&Swatch); Painter.setPen(Pair.first);
            const auto Font = Style.Font(font());
            Painter.setFont(Font); if (Style.font_["dim"].toBool()) Painter.setOpacity(0.55);
            Painter.drawText(QRect(0, 0, 40, 32), Qt::AlignCenter, "Aa");
            PaintFileRuleBorder(&Painter, QRect(0, 0, 40, 32), Style.foreground_.isValid() ? Style.foreground_ : palette().color(QPalette::Text), Style.font_);
            Painter.end(); Item->setIcon(Swatch);
        }
        list_->setIconSize(QSize(40, 32));
    }
    void refreshList(int Row) {
        loading_ = true; list_->clear(); mode_->button(document_["mode"] == "merge" ? 1 : 0)->setChecked(true);
        for (const auto V : document_["groups"].toArray()) { auto* Item = new QListWidgetItem(V.toObject()["name"].toString(), list_); Item->setData(Qt::UserRole, V.toObject()["id"].toString()); Item->setCheckState(V.toObject()["enabled"].toBool(true) ? Qt::Checked : Qt::Unchecked); }
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
        updateConditionHeight();
        loading_ = false; updatePreview();
    }
    void storeForm() {
        if (loading_ || list_->currentRow() < 0) return;
        auto Groups = document_["groups"].toArray(); auto G = Groups[list_->currentRow()].toObject(); G["name"] = name_->text(); G["join"] = join_->checkedId() == 1 ? "all" : "any";
        QJsonArray Conditions; for (auto* Row : conditionRows_) Conditions.append(Row->Record()); G["conditions"] = Conditions;
        for (const auto C : Conditions) if (C.toObject().contains("syntax")) { document_["version"] = 3; break; }
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
        updateConditionHeight();
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
        const auto Geometry = RecordGeometry::make(Colors.isWatercolor(), false, Appearance);
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
    struct PreviewWork {
        QJsonObject document_; settings::FileRuleInput input_; QList<settings::FileRuleInput> samples_;
        quint64 generation_ = 0;
    };
    struct PreviewOutput {
        QString error_; settings::FileRuleResult custom_; QList<settings::FileRuleResult> samples_;
    };
    static PreviewOutput evaluatePreview(const PreviewWork& Work) {
        PreviewOutput Output; settings::FileColorRules Rules;
        Output.error_ = Rules.Load(Work.document_); if (!Output.error_.isEmpty()) return Output;
        settings::FileColorRules::EvaluationContext Context;
        for (const auto& Input : Work.samples_) {
            auto Style = Rules.Evaluate(Input, &Context);
            if (Output.error_.isEmpty()) Output.error_ = Style.error_;
            Output.samples_.append(std::move(Style));
        }
        Output.custom_ = Rules.Evaluate(Work.input_, &Context);
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
    void updatePreview() {
        if (!preview_ || !error_) return;
        ++previewGeneration_; pendingPreview_.reset(); previewTimer_.stop();
        updateContrast();
        bool SizeOk = false, AttributesOk = false; const auto Size = previewSize_->text().toULongLong(&SizeOk); const auto Attributes = previewAttributes_->text().toULongLong(&AttributesOk);
        if (!SizeOk || !AttributesOk || Attributes > 0xFFFFFFFFULL) {
            validationPending_ = false; validatedDocument_ = {}; error_->setText(tr("크기는 0 이상 정수, 속성은 0~4294967295 정수로 입력하세요.")); preview_->setText(tr("미리보기 입력을 확인하세요.")); return;
        }
        auto Work = std::make_shared<PreviewWork>(); Work->document_ = document_; Work->generation_ = previewGeneration_;
        auto& Input = Work->input_; Input = settings::FileRuleInput{previewName_->text(), quint32(Attributes) | (previewFolder_->isChecked() ? 0x10u : 0u), Size, previewDark_->isChecked()};
        if (previewModifiedKnown_->isChecked()) Input.modifiedUtcMs_ = previewModified_->dateTime().toMSecsSinceEpoch();
        if (previewLink_->currentIndex()) { Input.attributes_ |= 0x400; if (previewLink_->currentIndex() < 3) Input.reparseTag_ = previewLink_->currentIndex() == 1 ? 0xa000000cu : 0xa0000003u; }
        Work->samples_ = settings::FileColorExamples::Samples(); for (auto& Sample : Work->samples_) Sample.dark_ = Input.dark_;
        validationPending_ = true;
        if (settings::FileColorRules::HasRegex(document_)) {
            error_->setText(tr("조건을 검증하고 있습니다…"));
            pendingPreview_ = std::move(Work); previewTimer_.start();
        } else applyPreview(*Work, evaluatePreview(*Work));
    }
    void applyPreview(const PreviewWork& Work, const PreviewOutput& Output) {
        validationPending_ = false; validatedDocument_ = Work.document_; validationError_ = Output.error_;
        error_->setText(validationError_); previewStyles_ = Output.samples_; updateSampleTable();
        const auto& Result = Output.custom_;
        const auto Pair = recordPair(Work.input_.dark_, Result, previewSelected_->isChecked()); auto Palette = palette(); Palette.setColor(QPalette::WindowText, Pair.first); Palette.setColor(QPalette::Window, Pair.second);
        preview_->setPalette(Palette); preview_->setFont(Result.Font(recordScheme(Work.input_.dark_).FileList_Font)); preview_->setText(previewName_->text());
        preview_->setProperty("hc.fileEffects", QVariant::fromValue(Result.font_)); preview_->update();
        const QMap<QString, QString> EffectNames{{"dim", tr("흐려짐")}, {"border", tr("행 테두리")}};
        QStringList Owners; for (auto It = Result.owners_.begin(); It != Result.owners_.end(); ++It) Owners.append(EffectNames.value(It.key(), It.key()) + ": " + It.value());
        owners_->setText(tr("일치 그룹: %1\n%2\n크기·속성은 위 미리보기 입력을 사용합니다.").arg(Result.groups_.join(", "), Owners.join(" · ")));
    }

    void addExamples() {
        auto Groups = document_["groups"].toArray(); QSet<QString> Ids, Names;
        for (const auto Value : Groups) { Ids.insert(Value.toObject()["id"].toString()); Names.insert(Value.toObject()["name"].toString().toCaseFolded()); }
        auto Added = QJsonArray{};
        for (const auto Value : settings::FileColorExamples::Groups()) { auto G = Value.toObject(); if (Ids.contains(G["id"].toString()) || Groups.size() + Added.size() >= 256) continue;
            const auto Base = G["name"].toString(); auto Name = Base; int N = 2; while (Names.contains(Name.toCaseFolded())) Name = Base + " " + QString::number(N++);
            G["name"] = Name; Names.insert(Name.toCaseFolded()); Added.append(G);
        }
        if (Added.isEmpty()) return; ensureVersion2(); Groups = document_["groups"].toArray(); const int First = Groups.size();
        for (const auto Value : Added) Groups.append(Value); document_["groups"] = Groups; refreshList(First);
    }
    void updateSampleTable() {
        const auto Samples = settings::FileColorExamples::Samples();
        const auto BaseFont = recordScheme(previewDark_->isChecked()).FileList_Font;
        const QMap<QString, QString> Labels{{"foreground", tr("글자색")}, {"background", tr("배경색")}, {"bold", tr("굵게")}, {"italic", tr("기울임")}, {"underline", tr("밑줄")}, {"strike", tr("취소선")}, {"dim", tr("흐려짐")}, {"border", tr("행 테두리")}};
        int Height = previewTable_->horizontalHeader()->height() + 2 * previewTable_->frameWidth(); int SizeWidth = previewTable_->fontMetrics().horizontalAdvance(tr("크기")) + 20;
        for (int Row = 0; Row < Samples.size(); ++Row) { auto Input = Samples[Row]; Input.dark_ = previewDark_->isChecked(); const auto Style = previewStyles_.value(Row);
            QStringList Owners; for (auto It = Style.owners_.begin(); It != Style.owners_.end(); ++It) Owners.append(Labels.value(It.key(), It.key()) + ": " + It.value());
            const QStringList Values{Input.name_, QLocale().formattedDataSize(Input.size_, 1, QLocale::DataSizeTraditionalFormat), Style.groups_.isEmpty() ? tr("목록 기본값") : Style.groups_.join(" + ")};
            for (int Col = 0; Col < 3; ++Col) { auto* Item = previewTable_->item(Row, Col); if (!Item) { Item = new QTableWidgetItem; previewTable_->setItem(Row, Col, Item); }
                Item->setText(Values[Col]); Item->setFont(Style.Font(BaseFont)); Item->setToolTip(Values.join(" · ") + "\n" + Owners.join("\n")); Item->setData(Qt::AccessibleDescriptionRole, Owners.join(" · "));
            }
            const int Pitch = qMax(24, QFontMetrics(Style.Font(BaseFont)).height() + 8); previewTable_->setRowHeight(Row, Pitch);
            // Styles can clamp a section to a larger minimum than requested.
            // Size from the actual rows so the sixth example never clips.
            Height += previewTable_->rowHeight(Row);
            SizeWidth = qMax(SizeWidth, QFontMetrics(Style.Font(BaseFont)).horizontalAdvance(Values[1]) + 20);
        }
        previewTable_->setColumnWidth(1, SizeWidth); previewTable_->setFixedHeight(Height); previewTable_->viewport()->update();
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
    QLabel *preview_ = nullptr, *owners_ = nullptr, *error_ = nullptr;
    const QStringList typeKeys_{"extension", "pattern", "folder", "attributes", "size", "modified", "link", "normalization", "rtl"};
    const QStringList fontKeys_{"bold", "italic", "underline", "strike", "dim"};
};
} // namespace hc::ui
