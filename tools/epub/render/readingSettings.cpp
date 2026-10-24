#include "stdafx.h"
#include "readingSettings.hpp"

/// Platform/framework headers
#include <QComboBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QColorDialog>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace hc::epub::poc
{
    ReadingSettings::ReadingSettings(const QString& Path, const QString& Book, const QString& Theme, QWidget* Parent)
        : QWidget(Parent), path_(Path), book_(Book)
    {
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
        auto* ScopeForm = new QFormLayout;
        scope_ = new QComboBox(this); scope_->setObjectName("scope");
        scope_->addItems({tr("이 책만"), tr("기본값 · 모든 책")}); ScopeForm->addRow(tr("적용 범위"), scope_); Layout->addLayout(ScopeForm);
        auto* Tabs = new QTabWidget(this); editors_ = Tabs; Layout->addWidget(Tabs);
        auto* Quick = new QWidget(Tabs); auto* Detail = new QWidget(Tabs);
        Tabs->addTab(Quick, tr("빠른 설정")); Tabs->addTab(Detail, tr("상세 설정"));
        addAppearance(Tabs);
        auto* QuickForm = new QFormLayout(Quick); auto* DetailForm = new QFormLayout(Detail);
        for(auto* Form : {QuickForm, DetailForm})
        { Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); }
        mode_ = new QComboBox; mode_->addItems({tr("사용자 설정"), tr("출판사 서식")}); addField(QuickForm, "override", tr("본문 서식"), mode_);
        theme_ = new QComboBox; theme_->addItems({tr("라이트"), tr("다크"), tr("세피아"), tr("연녹색"), tr("앱 테마 따름")}); addField(QuickForm, "theme", tr("배경"), theme_);
        font_ = new QComboBox;
        systemFonts_ = QFontDatabase::families();
        rebuildFonts();
        font_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); font_->setMinimumContentsLength(8);
        font_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); addField(QuickForm, "font", tr("글꼴"), font_);
        addNumber(QuickForm, "size", tr("글자 크기 · px"), 12, 40, 1);
        addNumber(QuickForm, "line", tr("행간 · 배수"), 1, 3, 0.1);
        addNumber(QuickForm, "after", tr("문단 뒤 · em"), 0, 3, 0.1);
        alignment_ = new QComboBox; alignment_->addItems({tr("왼쪽"), tr("가운데"), tr("오른쪽"), tr("양쪽")});
        addField(QuickForm, "align", tr("정렬"), alignment_);
        addNumber(DetailForm, "before", tr("문단 앞 · em"), 0, 3, 0.1);
        addNumber(DetailForm, "letter", tr("자간 · em"), -0.05, 0.2, 0.01);
        addNumber(DetailForm, "word", tr("단어 간격 · em"), 0, 0.8, 0.05);
        addNumber(DetailForm, "weight", tr("굵기"), 400, 700, 100);
        addNumber(DetailForm, "indent", tr("첫 줄 들여쓰기 · em"), 0, 3, 0.1);
        addNumber(DetailForm, "margin", tr("최소 좌우 여백 · px"), 8, 80, 4);
        addNumber(DetailForm, "width", tr("최대 본문 폭 · px"), 400, 1200, 40);
        auto* Presets = new QHBoxLayout;
        const QStringList Names{tr("표준"), tr("여유"), tr("촘촘")};
        for(int Index = 0; Index < Names.size(); ++Index)
        {
            auto* Button = new QPushButton(Names[Index], Quick); Presets->addWidget(Button);
            Button->setObjectName("preset" + QString::number(Index));
            connect(Button, &QPushButton::clicked, this, [this, Index] {
                draft_["override"] = true; draft_["line"] = Index == 1 ? 2.0 : Index == 2 ? 1.4 : 1.8;
                draft_["before"] = 0; draft_["after"] = Index == 1 ? 1.2 : Index == 2 ? 0.4 : 0.8;
                draft_["letter"] = Index == 1 ? 0.04 : 0; draft_["word"] = 0; draft_["indent"] = 0;
                refresh(); emit ProfileChanged(Profile());
            });
        }
        QuickForm->addRow(tr("간격 프리셋"), Presets);
        auto* Note = new QLabel(tr("줄 사이 간격은 행간, 문단 사이 간격은 문단 앞·뒤에서 조절합니다.\nem은 현재 글자 크기 기준입니다."), this);
        Note->setWordWrap(true); Layout->addWidget(Note);
        clear_ = new QPushButton(this); clear_->setObjectName("clear"); Layout->addWidget(clear_);
        auto* Actions = new QHBoxLayout;
        apply_ = new QPushButton(tr("적용"), this); apply_->setObjectName("apply");
        cancel_ = new QPushButton(tr("취소"), this); cancel_->setObjectName("cancel");
        reload_ = new QPushButton(tr("다시 불러오기"), this); reload_->setObjectName("reload");
        Actions->addWidget(apply_); Actions->addWidget(cancel_); Actions->addWidget(reload_); Layout->addLayout(Actions);
        status_ = new QLabel(this); status_->setObjectName("settingsStatus"); status_->setWordWrap(true); Layout->addWidget(status_);
        auto* StorageNote = new QLabel(path_.isEmpty() ? tr("검증 모드 · 파일 저장 없음")
            : tr("미리보기 후 적용하면 저장됩니다.\n책별 지정 값은 기본값보다 우선합니다.\nEPUB 원본은 변경하지 않습니다."), this);
        StorageNote->setWordWrap(true); Layout->addWidget(StorageNote);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this](int I) { edit("override", I == 0); });
        connect(theme_, &QComboBox::currentIndexChanged, this, [this](int I) { if(I >= 0) edit("theme", QStringList{"light", "dark", "sepia", "green", "system"}.at(I)); });
        connect(font_, &QComboBox::currentIndexChanged, this, [this](int I) {
            if(I >= 0 && font_->itemData(I).isValid()) edit("font", font_->itemData(I).toString());
        });
        connect(alignment_, &QComboBox::currentIndexChanged, this, [this](int I) { if(I >= 0) edit("align", QStringList{"left", "center", "right", "justify"}.at(I)); });
        connect(scope_, &QComboBox::currentIndexChanged, this, [this] {
            base_ = scope_->currentIndex() == 1 ? snapshot_.global : snapshot_.book; draft_ = base_;
            refresh(); emit ProfileChanged(Profile());
        });
        connect(clear_, &QPushButton::clicked, this, [this] { draft_ = {}; refresh(); emit ProfileChanged(Profile()); });
        connect(cancel_, &QPushButton::clicked, this, [this] { draft_ = base_; refresh(); emit ProfileChanged(Profile()); });
        connect(apply_, &QPushButton::clicked, this, &ReadingSettings::save);
        connect(reload_, &QPushButton::clicked, this, &ReadingSettings::reload);
        connect(&worker_, &QFutureWatcher<ReadingSnapshot>::finished, this, [this] {
            busy_ = false;
            const auto Result = worker_.result();
            if(Result.error.isEmpty()) { loadFailed_ = false; acceptSnapshot(Result); }
            else { refresh(); status_->setText(Result.error); }
        });
        if(path_.isEmpty())
        {
            snapshot_.global["theme"] = Theme == "dark" ? "dark" : "light";
            acceptSnapshot(snapshot_);
        }
        else reload();
    }
    void ReadingSettings::SetBookFonts(const QStringList& Families)
    {
        QStringList Accepted;
        for(const auto& Family : Families)
        {
            if(Family.isEmpty() || Family.size() > 128 || Family != Family.trimmed()) continue;
            bool Valid = true;
            for(const auto Ch : Family) if(Ch.category() == QChar::Other_Control) { Valid = false; break; }
            if(Valid && !Accepted.contains(Family)) Accepted.append(Family);
            if(Accepted.size() == 128) break;
        }
        Accepted.sort();
        if(Accepted == bookFonts_) return;
        bookFonts_ = Accepted;
        updating_ = true; rebuildFonts(); refresh();
    }
    void ReadingSettings::rebuildFonts()
    {
        font_->clear();
        for(const auto& Family : bookFonts_) font_->addItem(tr("%1 · 책 글꼴").arg(Family), Family);
        if(!bookFonts_.isEmpty()) font_->insertSeparator(font_->count());
        for(const auto& Family : systemFonts_) if(!bookFonts_.contains(Family)) font_->addItem(Family, Family);
        font_->setToolTip(tr("책 글꼴 목록은 화면 조건에 따라 달라집니다. 사용할 수 없거나 글자가 없으면 대체 글꼴로 표시됩니다."));
    }
    QJsonObject ReadingSettings::Profile() const
    { return scope_->currentIndex() == 1 ? ReadingStore::Resolve(draft_, {}) : ReadingStore::Resolve(snapshot_.global, draft_); }
    void ReadingSettings::addField(QFormLayout* Form, const QString& Key, const QString& Label, QWidget* Editor)
    {
        Editor->setObjectName(Key); Editor->setAccessibleName(Label);
        auto* Row = new QWidget; auto* Layout = new QHBoxLayout(Row); Layout->setContentsMargins(0, 0, 0, 0);
        Layout->addWidget(Editor, 1);
        auto* Reset = new QPushButton(QStringLiteral("↶"));
        Reset->setFixedWidth(30); Reset->setObjectName("reset_" + Key); Reset->setAccessibleName(Label + tr(" 상속값으로 복원"));
        Layout->addWidget(Reset); resets_[Key] = Reset; Form->addRow(Label, Row);
        connect(Reset, &QPushButton::clicked, this, [this, Key] { draft_.remove(Key); refresh(); emit ProfileChanged(Profile()); });
    }
    void ReadingSettings::addNumber(QFormLayout* Form, const QString& Key, const QString& Label, double Min, double Max, double Step)
    {
        auto* Box = new QDoubleSpinBox; Box->setRange(Min, Max); Box->setSingleStep(Step); Box->setDecimals(Step < 1 ? 2 : 0);
        Box->setKeyboardTracking(false); numbers_[Key] = Box; addField(Form, Key, Label, Box);
        connect(Box, &QDoubleSpinBox::valueChanged, this, [this, Key](double Value) { edit(Key, Value); });
    }
    void ReadingSettings::edit(const QString& Key, const QJsonValue& Value)
    {
        if(updating_ || busy_) return;
        draft_[Key] = Value; refresh(); emit ProfileChanged(Profile());
    }
    void ReadingSettings::refresh()
    {
        updating_ = true;
        const auto Values = Profile();
        mode_->setCurrentIndex(Values["override"].toBool() ? 0 : 1);
        theme_->setCurrentIndex(QStringList{"light", "dark", "sepia", "green", "system"}.indexOf(Values["theme"].toString()));
        const auto Family = Values["font"].toString();
        auto FontIndex = font_->findData(Family);
        if(FontIndex < 0) { font_->addItem(tr("%1 · 현재 조건에서 확인되지 않음").arg(Family), Family); FontIndex = font_->count() - 1; }
        font_->setCurrentIndex(FontIndex);
        alignment_->setCurrentIndex(QStringList{"left", "center", "right", "justify"}.indexOf(Values["align"].toString()));
        for(auto It = numbers_.begin(); It != numbers_.end(); ++It) It.value()->setValue(Values[It.key()].toDouble());
        for(auto It = resets_.begin(); It != resets_.end(); ++It)
        {
            It.value()->setEnabled(draft_.contains(It.key()));
            It.value()->setToolTip(scope_->currentIndex() == 1 ? tr("내장 기본값으로 복원") : tr("이 책의 지정을 해제하고 기본값 상속"));
        }
        background_->setCurrentIndex(background_->findData(Values["background"].toString()));
        backgroundColor_->setText(Values["backgroundColor"].toString().isEmpty()?tr("색상 선택…"):Values["backgroundColor"].toString());
        textColor_->setText(Values["textColor"].toString().isEmpty()?tr("테마 글자색"):Values["textColor"].toString());
        cssEnabled_->setChecked(Values["cssEnabled"].toBool()); cssForce_->setChecked(Values["cssForce"].toBool());
        if(css_->toPlainText()!=Values["customCss"].toString()) css_->setPlainText(Values["customCss"].toString());
        backgroundImport_->setEnabled(!path_.isEmpty());
        const bool Dirty = IsDirty();
        editors_->setEnabled(!busy_ && !loadFailed_); clear_->setEnabled(!busy_ && !loadFailed_ && !draft_.isEmpty());
        clear_->setText(scope_->currentIndex() == 1 ? tr("기본값 초기화") : tr("이 책의 모든 지정 해제"));
        apply_->setEnabled(!busy_ && !loadFailed_ && Dirty && CssValid()); cancel_->setEnabled(!busy_ && Dirty);
        reload_->setEnabled(!busy_ && !Dirty && !path_.isEmpty()); scope_->setEnabled(!busy_ && !Dirty && !loadFailed_);
        status_->setText(busy_ ? tr("설정을 처리하고 있습니다…") : Dirty ? tr("미리보기 · 아직 적용하지 않음")
            : scope_->currentIndex() == 1 ? tr("기본값 미리보기 · 책별 지정 제외") : tr("적용된 설정"));
        if(!CssValid()) status_->setText(tr("CSS 검사: %1").arg(cssError_.isEmpty()?tr("검사 중…"):cssError_));
        updating_ = false;
    }
    void ReadingSettings::acceptSnapshot(const ReadingSnapshot& Snapshot)
    {
        snapshot_ = Snapshot; base_ = scope_->currentIndex() == 1 ? Snapshot.global : Snapshot.book; draft_ = base_;
        refresh(); emit ProfileChanged(Profile());
    }
    void ReadingSettings::reload()
    {
        if(busy_ || IsDirty() || path_.isEmpty()) return;
        busy_ = true; loadFailed_ = true; refresh();
        worker_.setFuture(QtConcurrent::run([Path = path_, Book = book_] { return ReadingStore::Load(Path, Book); }));
    }
    void ReadingSettings::save()
    {
        if(busy_ || loadFailed_ || !IsDirty() || !CssValid()) return;
        if(path_.isEmpty())
        {
            auto Next = snapshot_; if(scope_->currentIndex() == 1) Next.global = draft_; else Next.book = draft_;
            acceptSnapshot(Next); return;
        }
        busy_ = true; refresh();
        worker_.setFuture(QtConcurrent::run([Path = path_, Book = book_, Global = scope_->currentIndex() == 1, Base = base_, Draft = draft_] {
            return ReadingStore::Save(Path, Book, Global, Base, Draft);
        }));
    }
}

namespace hc::epub::poc {
bool ReadingSettings::CssValid() const {
    const auto Value=Profile(); const auto Css=Value["customCss"].toString();
    return !Value["cssEnabled"].toBool() || Css.isEmpty() || (validatedCss_==Css && cssError_.isEmpty());
}
void ReadingSettings::SetCssValidation(const QString& Css,const QString& Error) {
    if(Css!=Profile()["customCss"].toString()) return;
    validatedCss_=Css; cssError_=Error; refresh();
}
void ReadingSettings::addAppearance(QTabWidget* Tabs) {
    auto* Panel=new QWidget(Tabs); auto* Form=new QFormLayout(Panel); Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    Tabs->addTab(Panel,tr("배경 · CSS"));
    background_=new QComboBox(Panel);
    const QStringList Names{tr("테마 배경"),tr("단색"),tr("종이 무늬"),tr("격자 무늬"),tr("사용자 이미지")};
    const QStringList Values{"theme","color","paper","grid","image"};
    for(int I=0;I<Values.size();++I) background_->addItem(Names[I],Values[I]);
    addField(Form,"background",tr("배경 종류"),background_);
    backgroundColor_=new QPushButton(Panel); addField(Form,"backgroundColor",tr("배경색"),backgroundColor_);
    textColor_=new QPushButton(Panel); addField(Form,"textColor",tr("글자색"),textColor_);
    backgroundImport_=new QPushButton(tr("이미지 선택…"),Panel); addField(Form,"backgroundAsset",tr("배경 이미지"),backgroundImport_);
    cssEnabled_=new QCheckBox(tr("사용자 CSS 사용"),Panel); addField(Form,"cssEnabled",tr("CSS 재정의"),cssEnabled_);
    cssForce_=new QCheckBox(tr("선택한 CSS를 우선 적용"),Panel); addField(Form,"cssForce",tr("적용 우선순위"),cssForce_);
    css_=new QPlainTextEdit(Panel); css_->setMinimumHeight(150); css_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont)); css_->setPlaceholderText("p { margin-block-end: 1em; text-align: justify; }"); addField(Form,"customCss",tr("사용자 CSS"),css_);
    auto* Hint=new QLabel(tr("문자·문단·이미지 폭·테두리의 CSS를 미리보기합니다. 외부 리소스와 동적 선택자는 지원하지 않습니다. 출판사 서식에서는 재정의를 적용하지 않습니다."),Panel); Hint->setWordWrap(true);Form->addRow(Hint);
    connect(background_,&QComboBox::currentIndexChanged,this,[this](int I){if(I>=0)edit("background",background_->itemData(I).toString());});
    connect(backgroundColor_,&QPushButton::clicked,this,[this]{const auto Color=QColorDialog::getColor(QColor(Profile()["backgroundColor"].toString()),this,tr("읽기 배경색"));if(Color.isValid())edit("backgroundColor",Color.name());});
    connect(textColor_,&QPushButton::clicked,this,[this]{const auto Color=QColorDialog::getColor(QColor(Profile()["textColor"].toString()),this,tr("읽기 글자색"));if(Color.isValid())edit("textColor",Color.name());});
    connect(backgroundImport_,&QPushButton::clicked,this,&ReadingSettings::importBackground);
    connect(cssEnabled_,&QCheckBox::toggled,this,[this](bool Value){edit("cssEnabled",Value);});
    connect(cssForce_,&QCheckBox::toggled,this,[this](bool Value){edit("cssForce",Value);});
    connect(css_,&QPlainTextEdit::textChanged,this,[this]{edit("customCss",css_->toPlainText());});
    connect(&assetWorker_,&QFutureWatcher<ReadingAsset>::finished,this,[this]{
        const auto Result=assetWorker_.result(); busy_=false;
        if(Result.error.isEmpty()) {draft_["backgroundAsset"]=Result.id;draft_["background"]="image";refresh();emit ProfileChanged(Profile());}
        else {refresh();status_->setText(Result.error);}
    });
}
void ReadingSettings::importBackground() {
    if(busy_ || path_.isEmpty())return;
    const auto Input=QFileDialog::getOpenFileName(this,tr("읽기 배경 이미지"),{},tr("이미지 (*.png *.jpg *.jpeg *.bmp)"));if(Input.isEmpty())return;
    busy_=true;refresh();assetWorker_.setFuture(QtConcurrent::run([Input,Path=path_]{return ReadingAssets::Import(Input,Path);}));
}
}

namespace hc::epub::poc {
bool ReadingSettings::Execute(const QString& Action,const QJsonObject& Patch,bool Global) {
    if(busy_ || loadFailed_)return false;
    if(Action=="preview") {
        if(scope_->currentIndex()!=(Global?1:0)) {if(IsDirty())return false;scope_->setCurrentIndex(Global?1:0);}
        auto Next=draft_;for(auto I=Patch.begin();I!=Patch.end();++I) {if(I.value().isNull())Next.remove(I.key());else Next[I.key()]=I.value();}
        if(!ReadingStore::Valid(Next))return false;
        draft_=Next;refresh();emit ProfileChanged(Profile());return true;
    }
    if(Action=="apply") {if(!CssValid())return false;if(IsDirty())save();return true;}
    if(Action=="cancel") {draft_=base_;refresh();emit ProfileChanged(Profile());return true;}
    if(Action=="reload") {if(IsDirty())return false;reload();return true;}
    return false;
}
}
