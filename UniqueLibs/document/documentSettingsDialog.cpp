#include "stdafx.h"
#include "documentSettingsDialog.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "documentShortcutRegistry.hpp"
#include "tools/epub/render/readingSettings.hpp"
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QScreen>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QFrame>
#include <QGroupBox>
#include <fmstyle/StyleProps.h>
#include <fmwidgets/DialogHeader.h>

namespace hc::document {
namespace {
// A labelled switch row: caption at the left edge, switch at the right edge (Paper Fluent board, as in the product settings).
QWidget* SwitchRow(QCheckBox* Check, QWidget* Parent)
{
    auto* Row = new QWidget(Parent); auto* Layout = new QHBoxLayout(Row); Layout->setContentsMargins(0, 0, 0, 0);
    auto* Caption = new QLabel(Check->text(), Row); Caption->setBuddy(Check); Caption->setWordWrap(true); Caption->setToolTip(Check->toolTip());
    Check->setAccessibleName(QString(Check->text()).remove('&')); Check->setText({}); fm::style::setSwitch(Check);
    Layout->addWidget(Caption, 1); Layout->addWidget(Check, 0, Qt::AlignRight | Qt::AlignVCenter);
    return Row;
}
}
DocumentSettingsDialog::DocumentSettingsDialog(QString Path, const QList<QAction*>& Actions, QWidget* Parent)
    : QDialog(Parent), path_(std::move(Path))
{
    setObjectName("documentSettingsDialog"); setWindowTitle(tr("설정"));
    const auto Screen = screen()->availableGeometry().size(); resize(qMin(900, Screen.width() - 40), qMin(680, Screen.height() - 60));
    // Same arrangement as the product settings (board Set-*): a soft-selection category column, a page heading,
    // titled cards with switch rows, and a footer band with the status at the left and the buttons at the right.
    fm::style::setDensity(this, fm::style::Density::Settings);
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(0);
    auto* Body = new QHBoxLayout; Body->setContentsMargins(12, 12, 4, 12); Body->setSpacing(12);
    auto* List = new QListWidget(this); List->setObjectName("settingsCategories");
    List->addItems({tr("공통"), tr("텍스트 편집기"), tr("단축키"),tr("EPUB")}); List->setFixedWidth(180);
    List->setProperty(fm::style::props::kSoftSelection, true); List->setFrameShape(QFrame::NoFrame);
    for (int Row = 0; Row < List->count(); ++Row) List->item(Row)->setSizeHint(QSize(0, 30));
    auto* Pages = new QStackedWidget(this); Body->addWidget(List); Body->addWidget(Pages, 1); Layout->addLayout(Body, 1);
    connect(List, &QListWidget::currentRowChanged, Pages, &QStackedWidget::setCurrentIndex); List->setCurrentRow(0);
    auto Page = [&](const QString& Title, const QString& Subtitle) {
        auto* Scroll = new QScrollArea(Pages); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        auto* Content = new QWidget(Scroll); auto* Column = new QVBoxLayout(Content); Column->setContentsMargins(0, 0, 8, 0); Column->setSpacing(12);
        auto* Header = new fm::ui::DialogHeader(Content); Header->setTitle(Title); Header->setSubtitle(Subtitle); Header->setSubtitleWrap(true); Column->addWidget(Header);
        Scroll->setWidget(Content); Pages->addWidget(Scroll); return Column;
    };
    auto Card = [](QVBoxLayout* Column, const QString& Title) {
        auto* Group = new QGroupBox(Title, Column->parentWidget()); auto* Form = new QFormLayout(Group);
        Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        Column->addWidget(Group); return Form;
    };
    auto Note = [this](QFormLayout* Form, const QString& Text) {
        auto* Label = new QLabel(Text, this); Label->setWordWrap(true); Form->addRow(Label);
    };
    auto Combo = [&](QFormLayout* Form, const QString& Key, const QString& Label, const QStringList& Labels, const QVariantList& Values) {
        auto* Field = new QComboBox(this); for (int Index = 0; Index < Labels.size(); ++Index) Field->addItem(Labels[Index], Values[Index]);
        Field->setObjectName(Key); controls_[Key] = Field; Form->addRow(Label, Field);
    };
    auto Check = [&](QFormLayout* Form, const QString& Key, const QString& Label, bool Default) {
        auto* Field = new QCheckBox(Label, this); Field->setProperty("defaultValue", Default); Field->setObjectName(Key);
        controls_[Key] = Field; Form->addRow(SwitchRow(Field, Form->parentWidget()));
    };
    auto Spin = [&](QFormLayout* Form, const QString& Key, const QString& Label, int Minimum, int Maximum, int Default) {
        auto* Field = new QSpinBox(this); Field->setRange(Minimum, Maximum); Field->setProperty("defaultValue", Default);
        Field->setObjectName("documentViews/textView/" + Key); controls_[Field->objectName()] = Field; Form->addRow(Label, Field);
    };
    // Font names must not widen the card past the page; the list still shows full names.
    auto FontField = [&](QFormLayout* Form, const QString& Key, const QString& Label) {
        auto* Field = new QFontComboBox(this); Field->setObjectName("documentViews/textView/" + Key);
        if (Key == "fontFamily") Field->setProperty("defaultValue", "D2Coding ligature");
        Field->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); Field->setMinimumContentsLength(12);
        controls_[Field->objectName()] = Field; Form->addRow(Label, Field);
    };
    auto* General = Page(tr("공통"), tr("테마와 메뉴·도구모음 표시, 창 동작과 설정 파일을 관리합니다."));
    auto* Display = Card(General, tr("화면"));
    Combo(Display, "appearance/documentWorkspace/themeMode", tr("밝기"), {tr("본체 따르기"), tr("시스템"), tr("밝게"), tr("어둡게")}, {"product", "system", "light", "dark"});
    Note(Display, tr("색과 강조색은 HyperCommander 설정을 따릅니다. 밝게·어둡게만 편집기에서 따로 고를 수 있습니다."));
    for (const auto& Key : {QString("mainMenuMode"), QString("mainToolbarMode")})
        Combo(Display, "appearance/documentWorkspace/" + Key, Key == "mainMenuMode" ? tr("메뉴 표시") : tr("도구모음 표시"), {tr("뷰 권장값"), tr("표시"), tr("숨기기")}, {"recommended", "show", "hide"});
    auto* WindowCard = Card(General, tr("창"));
    Check(WindowCard, "documentViews/common/closeViewerOnEscape", tr("ESC 키를 눌러 창 닫기 (HyperViewer 전용)"), true);
    auto* Files = Card(General, tr("설정 파일"));
    auto* Location = new QLabel(tr("설정 파일: %1").arg(path_), this); Location->setWordWrap(true); Location->setTextInteractionFlags(Qt::TextSelectableByMouse); Files->addRow(Location);
    auto* Import = new QPushButton(tr("기존 설정 가져오기…"), this); auto* ImportRow = new QHBoxLayout; ImportRow->addWidget(Import); ImportRow->addStretch(); Files->addRow(ImportRow);
    General->addStretch();
    connect(Import, &QPushButton::clicked, this, [this] {
        const auto Directory = QFileDialog::getExistingDirectory(this, tr("기존 설정 폴더 선택"), QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        if (Directory.isEmpty()) return;
        const auto Source = settings::UnifiedConfig::FileName(Directory); QJsonObject Profile; QString Error;
        if (!settings::UnifiedConfig::Load(Source, Profile, Error, false) || Profile.size() <= 1) { error_->setText(tr("가져올 설정을 읽을 수 없습니다.\n%1\n%2").arg(Source, Error)); return; }
        if (hc::ui::MessageDialog::question(this, tr("기존 설정 가져오기"), tr("%1의 설정을 현재 설정에 가져옵니다. 적용 또는 확인을 누르면 저장됩니다. 원본 파일은 유지됩니다.").arg(Directory)) != QMessageBox::Yes) return;
        imported_ = Profile; const auto Original = base_;
        for (auto It = Profile.begin(); It != Profile.end(); ++It) base_[It.key()] = It.value();
        loadControls(); base_ = Original; error_->clear();
    });
    auto* Text = Page(tr("텍스트 편집기"), tr("글꼴과 편집 화면, 들여쓰기·줄바꿈, 새 문서 저장과 외부 파일 변경 처리를 설정합니다."));
    auto* Fonts = Card(Text, tr("글꼴"));
    FontField(Fonts, "fontFamily", tr("글꼴"));
    Spin(Fonts, "fontSize", tr("글꼴 크기 (pt)"), 6, 72, 16);
    auto* Spacing = new QDoubleSpinBox(this); Spacing->setRange(1.0, 3.0); Spacing->setSingleStep(.1); Spacing->setDecimals(2); Spacing->setProperty("defaultValue", 1.1);
    Spacing->setObjectName("documentViews/textView/lineSpacing"); controls_[Spacing->objectName()] = Spacing; Fonts->addRow(tr("줄 간격"), Spacing);
    Combo(Fonts, "documentViews/textView/fontRendering", tr("글꼴 렌더링"), {tr("기본"), tr("안티앨리어싱 없음"), tr("회색조"), tr("ClearType")}, {0, 1, 2, 3});
    auto* View = Card(Text, tr("편집 화면"));
    const QList<QPair<QString, QString>> ViewChecks{{"showLineNumbers", tr("줄 번호")}, {"highlightCurrentLine", tr("현재 줄 강조")},
        {"braceHighlight", tr("괄호 강조")}, {"showCodeFolding", tr("코드 접기")}, {"showWhitespace", tr("공백·줄끝 표시")}, {"showIndentationGuides", tr("들여쓰기 안내선")}};
    for (const auto& Item : ViewChecks) Check(View, "documentViews/textView/" + Item.first, Item.second, Item.first != "showWhitespace");
    Combo(View, "documentViews/textView/indentGuideStyle", tr("들여쓰기 안내선 모드"), {tr("실제 들여쓰기"), tr("다음 줄 참조"), tr("앞뒤 줄 참조")}, {1, 2, 3});
    Combo(View, "documentViews/textView/changeHistoryMode", tr("변경 이력 표시"), {tr("끄기"), tr("여백 표시"), tr("본문 표시"), tr("모두")}, {0, 1, 2, 3});
    auto* Indent = Card(Text, tr("들여쓰기 · 줄바꿈"));
    Spin(Indent, "tabWidth", tr("탭 너비"), 1, 16, 4);
    Check(Indent, "documentViews/textView/autoIndent", tr("자동 들여쓰기"), true);
    Check(Indent, "documentViews/textView/useTabs", tr("들여쓰기에 탭 사용"), true);
    Combo(Indent, "documentViews/textView/wordWrapMode", tr("자동 줄바꿈"), {tr("없음"), tr("단어 단위"), tr("문자 단위"), tr("공백 단위")}, {0, 1, 2, 3});
    Combo(Indent, "documentViews/textView/wrapVisualFlags", tr("줄바꿈 이어짐 표시"),
        {tr("없음"), tr("줄 끝"), tr("줄 시작"), tr("줄 끝·시작"), tr("여백"), tr("줄 끝·여백"), tr("줄 시작·여백"), tr("모두")}, {0, 1, 2, 3, 4, 5, 6, 7});
    controls_["documentViews/textView/wrapVisualFlags"]->setProperty("defaultValue", 1);
    Combo(Indent, "documentViews/textView/wrapIndentMode", tr("이어지는 행 들여쓰기"),
        {tr("고정"), tr("원래 들여쓰기"), tr("한 단계 추가"), tr("두 단계 추가")}, {0, 1, 2, 3});
    controls_["documentViews/textView/wrapIndentMode"]->setProperty("defaultValue", 1);
    auto* Ruler = Card(Text, tr("눈금자"));
    Check(Ruler, "documentViews/textView/showRuler", tr("눈금자"), true);
    FontField(Ruler, "rulerFontFamily", tr("눈금자 글꼴"));
    Spin(Ruler, "rulerFontSize", tr("눈금자 글꼴 크기 (pt)"), 6, 36, 8);
    auto* Save = Card(Text, tr("새 문서 · 복구"));
    Combo(Save, "documentViews/textView/defaultLineEnding", tr("새 줄 기본 줄끝"), {"CRLF", "LF", "CR"}, {"CRLF", "LF", "CR"});
    Combo(Save, "documentViews/textView/saveEncoding", tr("새 문서 기본 인코딩"), {"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}, {"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"});
    Combo(Save, "documentViews/textView/saveBomMode", tr("새 문서 BOM"), {tr("인코딩 권장값"), tr("포함"), tr("제외")}, {0, 1, 2});
    controls_["documentViews/textView/saveBomMode"]->setProperty("defaultValue", 1);
    Check(Save, "documentViews/textView/recoveryEnabled", tr("미저장 문서의 주기적 복구 사본 생성"), true);
    Spin(Save, "largeFileMB", tr("대용량 안내 기준 (MiB)"), 1, 16, 1);
    Note(Save, tr("기존 파일의 인코딩·BOM·줄끝은 유지합니다. 수정 중인 문서는 다시 읽기 전에 확인합니다. 복구를 꺼도 F3 문서 교체 시 미저장 내용은 보호합니다."));
    auto* External = Card(Text, tr("외부 파일 변경"));
    Combo(External, "documentViews/textView/externalChangeAction", tr("외부 파일 변경"), {tr("감시 끄기"), tr("수정하지 않은 문서 자동 다시 읽기"), tr("항상 확인")}, {0, 1, 2});
    controls_["documentViews/textView/externalChangeAction"]->setProperty("defaultValue", 1);
    Combo(External, "documentViews/textView/externalChangeDetection", tr("변경 감지 방식"), {tr("OS 알림"), tr("주기적 확인")}, {0, 1});
    Spin(External, "externalChangePollSeconds", tr("확인 간격 (초, OS 알림 실패 시에도 사용)"), 1, 3600, 3);
    auto* Mask = Card(Text, tr("글자 마스크"));
    Check(Mask, "documentViews/textView/mask/enabled", tr("글자 마스크 보정"), false);
    auto MaskSpin = [&](const QString& Key, const QString& Label, double Maximum, double Default) {
        auto* Field = new QDoubleSpinBox(this); Field->setRange(.5, Maximum); Field->setSingleStep(.05); Field->setDecimals(2);
        Field->setProperty("defaultValue", Default); Field->setObjectName("documentViews/textView/mask/" + Key);
        controls_[Field->objectName()] = Field; Mask->addRow(Label, Field);
        auto* Enabled = qobject_cast<QCheckBox*>(controls_["documentViews/textView/mask/enabled"]);
        Field->setEnabled(false); connect(Enabled, &QCheckBox::toggled, Field, &QWidget::setEnabled);
    };
    MaskSpin("gamma", tr("글자 마스크 감마"), 3.0, 1.2); MaskSpin("strength", tr("글자 마스크 강도"), 2.0, 1.1);
    Text->addStretch();
    Q_UNUSED(Actions);
    auto* ShortcutPage = Page(tr("단축키"), tr("편집기 명령의 단축키를 찾아 바꿉니다. 분류와 변경 여부로 목록을 좁힐 수 있습니다."));
    auto* Diagnostics = new QLabel(this); Diagnostics->setObjectName("shortcutDiagnostics"); Diagnostics->setWordWrap(true); ShortcutPage->addWidget(Diagnostics);
    auto* FilterRow = new QHBoxLayout; FilterRow->setSpacing(8); ShortcutPage->addLayout(FilterRow);
    auto* Search = new QLineEdit(this); Search->setObjectName("shortcutSearch"); Search->setClearButtonEnabled(true); Search->setPlaceholderText(tr("명령 또는 단축키 검색…")); FilterRow->addWidget(Search, 1);
    auto* Scope = new QComboBox(this); Scope->addItem(tr("전체"), ""); Scope->addItem(tr("공통"), "common"); Scope->addItem(tr("텍스트·편집"), "text"); Scope->addItem(tr("EPUB"),"epub");
    auto* ScopeLabel = new QLabel(tr("분류"), this); ScopeLabel->setBuddy(Scope); FilterRow->addWidget(ScopeLabel); FilterRow->addWidget(Scope);
    auto* ChangedOnly = new QCheckBox(tr("변경한 항목만"), this); FilterRow->addWidget(ChangedOnly);
    auto* Shortcuts = Card(ShortcutPage, tr("명령"));
    QList<QPair<DocumentShortcut, QWidget*>> Rows;
    // One line per command: name with its scope and default key underneath, then the key field and its two resets.
    for (const auto& Command : DocumentShortcutRegistry::Commands()) {
        auto* Row = new QWidget(this); auto* RowLayout = new QHBoxLayout(Row); RowLayout->setContentsMargins(0, 2, 0, 2); RowLayout->setSpacing(8);
        auto* Names = new QVBoxLayout; Names->setSpacing(0); RowLayout->addLayout(Names, 1);
        auto* Name = new QLabel(Command.label_, Row); Name->setWordWrap(true); Names->addWidget(Name);
        auto* Meta = new QLabel(tr("%1 · 기본값 %2").arg(Command.scope_ == "common" ? tr("공통") : Command.scope_=="epub"?tr("EPUB"):tr("텍스트·편집"), Command.key_.isEmpty() ? tr("없음") : Command.key_), Row);
        Meta->setForegroundRole(QPalette::PlaceholderText); Names->addWidget(Meta);
        auto* Field = new QKeySequenceEdit(Row); Field->setClearButtonEnabled(true); Field->setMaximumSequenceLength(2); Field->setMinimumWidth(140);
        const auto Key = "documentViews/shortcuts/bindings/" + Command.id_;
        Field->setObjectName(Key); Field->setProperty("defaultValue", Command.key_); Field->setAccessibleName(Command.label_); Name->setBuddy(Field);
        controls_[Key] = Field; RowLayout->addWidget(Field);
        auto* Reset = new QPushButton(tr("기본값"), Row); fm::style::setSmall(Reset); RowLayout->addWidget(Reset);
        auto* Previous = new QPushButton(tr("이전 값"), Row); fm::style::setSmall(Previous); RowLayout->addWidget(Previous);
        connect(Reset, &QPushButton::clicked, this, [Field, Default = Command.key_] { Field->setKeySequence(QKeySequence(Default)); });
        connect(Previous, &QPushButton::clicked, this, [this, Field, Key] { Field->setKeySequence(QKeySequence(settings::UnifiedConfig::Value(initial_, Key.split('/')).toString())); });
        Shortcuts->addRow(Row); Rows.append({Command, Row});
    }
    auto Filter = [Search, Scope, ChangedOnly, Rows, Shortcuts] {
        for (const auto& Row : Rows) {
            auto* Field = Row.second->findChild<QKeySequenceEdit*>();
            const auto Current = Field->keySequence().toString(QKeySequence::NativeText);
            const bool Match = (Row.first.label_ + " " + Row.first.id_ + " " + Current + " " + Row.first.key_).contains(Search->text(), Qt::CaseInsensitive);
            const auto ScopeId=Scope->currentData().toString();
            const bool Category = ScopeId.isEmpty() || (ScopeId=="text"?(Row.first.scope_=="text" || Row.first.scope_=="edit"):Row.first.scope_==ScopeId);
            Shortcuts->setRowVisible(Row.second, Match && Category && (!ChangedOnly->isChecked() || Field->keySequence() != QKeySequence(Row.first.key_)));
        }
    };
    connect(Search, &QLineEdit::textChanged, this, Filter); connect(Scope, &QComboBox::currentIndexChanged, this, Filter); connect(ChangedOnly, &QCheckBox::toggled, this, Filter);
    for (const auto& Row : Rows) connect(Row.second->findChild<QKeySequenceEdit*>(), &QKeySequenceEdit::keySequenceChanged, this, Filter);
    Note(Shortcuts, tr("빈 값은 단축키 해제입니다. 최대 두 번의 키 조합을 사용할 수 있습니다. 이동·선택 기본키와 입력기 키는 유지됩니다."));
    ShortcutPage->addStretch();
    auto* EpubPage = Page(tr("EPUB"), tr("편집 초안 복구와 EPUBCheck 검사, 모든 책에 쓰는 읽기 기본값을 설정합니다."));
    auto* Drafts = Card(EpubPage, tr("편집 초안"));
    auto* EpubRecovery=new QCheckBox(tr("편집 초안 자동 복구 (텍스트 편집기와 공통 설정)"),this);EpubRecovery->setObjectName("epubRecoveryEnabled");Drafts->addRow(SwitchRow(EpubRecovery,Drafts->parentWidget()));
    auto* TextRecovery=qobject_cast<QCheckBox*>(controls_["documentViews/textView/recoveryEnabled"]);
    connect(TextRecovery,&QCheckBox::toggled,EpubRecovery,&QCheckBox::setChecked);connect(EpubRecovery,&QCheckBox::toggled,TextRecovery,&QCheckBox::setChecked);
    EpubRecovery->setChecked(TextRecovery->isChecked());
    auto* EpubForm = Card(EpubPage, tr("EPUBCheck 검사"));
    auto EpubTool=[&](const QString& Key,const QString& Label,const QString& Filter){
        auto* Row=new QWidget(this);auto* Fields=new QHBoxLayout(Row);Fields->setContentsMargins(0,0,0,0);
        auto* Field=new QLineEdit(Row);Field->setMaxLength(4096);Field->setObjectName("documentViews/epubEditor/"+Key);Field->setAccessibleName(Label);controls_[Field->objectName()]=Field;
        if(Key=="javaPath")Field->setPlaceholderText(tr("java.exe 경로 · 빈 값은 PATH에서 찾기"));
        auto* Browse=new QPushButton(tr("찾기…"),Row);Fields->addWidget(Field,1);Fields->addWidget(Browse);EpubForm->addRow(Label,Row);
        connect(Browse,&QPushButton::clicked,this,[this,Field,Label,Filter]{const auto File=QFileDialog::getOpenFileName(this,Label,Field->text(),Filter);if(!File.isEmpty())Field->setText(File);});
    };
    EpubTool("javaPath",tr("EPUBCheck Java 실행 파일"),tr("Java 실행 파일 (java.exe);;모든 파일 (*)"));
    EpubTool("epubcheckJar",tr("EPUBCheck JAR"),tr("EPUBCheck (epubcheck.jar);;JAR 파일 (*.jar)"));
    Check(EpubForm,"documentViews/epubEditor/requireEpubCheck",tr("EPUB 다른 이름 저장 전에 EPUBCheck 오류 없음 필수"),false);
    Note(EpubForm, tr("기본 검사는 패키지·XML·CSS 괄호·참조·이미지를 확인합니다. EPUBCheck는 선택한 외부 Java/JAR로 사본을 검사합니다. 경고는 저장을 허용하며, 필수 검사를 켜면 검사 실패·취소·오류가 있을 때 저장을 중단합니다."));
    auto* EpubCheckHelp=new QLabel(tr("<a href=\"https://www.w3.org/publishing/epubcheck/docs/installation/\">EPUBCheck 공식 설치 안내</a>"),this);EpubCheckHelp->setOpenExternalLinks(true);EpubForm->addRow(EpubCheckHelp);
    auto* Reading = Card(EpubPage, tr("읽기 기본값 · 모든 책"));
    epubSettings_=new hc::epub::poc::ReadingSettings(QDir(QFileInfo(path_).absolutePath()).filePath("epub-reading.json"),QString(64,'0'),"light",this);
    epubSettings_->setObjectName("epubGlobalSettings");epubSettings_->SetGlobalOnly();Reading->addRow(epubSettings_);
    EpubPage->addStretch();
    auto* Footer = new QFrame(this); Footer->setObjectName("settingsFooter"); Footer->setFrameShape(QFrame::StyledPanel); fm::style::setFooter(Footer);
    auto* FooterRow = new QHBoxLayout(Footer); FooterRow->setContentsMargins(16, 8, 16, 8); FooterRow->setSpacing(8); Layout->addWidget(Footer);
    error_ = new QLabel(Footer); error_->setObjectName("settingsError"); error_->setWordWrap(true); error_->setTextInteractionFlags(Qt::TextSelectableByMouse); FooterRow->addWidget(error_, 1);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, Footer); Buttons->setObjectName("settingsButtons"); FooterRow->addWidget(Buttons);
    Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인")); Buttons->button(QDialogButtonBox::Apply)->setText(tr("적용"));
    Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소")); Buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("기본값"));
    fm::style::setButtonRole(Buttons->button(QDialogButtonBox::Ok), fm::style::ButtonRole::Primary);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this] { if (Apply()) accept(); });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { Apply(); });
    connect(Buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this, List] {
        if(List->currentRow()==3){epubSettings_->ResetDraft();for(auto I=controls_.begin();I!=controls_.end();++I)if(I.key().startsWith("documentViews/epubEditor/")){if(auto* Field=qobject_cast<QLineEdit*>(I.value()))Field->clear();else if(auto* Check=qobject_cast<QCheckBox*>(I.value()))Check->setChecked(false);}return;}
        if (List->currentRow() == 2) { for (auto* Control : controls_) if (auto* Field = qobject_cast<QKeySequenceEdit*>(Control)) Field->setKeySequence(QKeySequence(Field->property("defaultValue").toString())); return; }
        imported_ = {}; const auto Original = base_; base_ = {{"schemaVersion", 2}}; loadControls(); base_ = Original;
    });
    QString Error; valid_ = settings::UnifiedConfig::Load(path_, base_, Error);
    if (!valid_) error_->setText(tr("설정을 읽을 수 없습니다.\n%1\n%2").arg(path_, Error));
    Buttons->button(QDialogButtonBox::Ok)->setEnabled(valid_); Buttons->button(QDialogButtonBox::Apply)->setEnabled(valid_);
    loadControls();
    initial_ = values();
    for (auto* Control : controls_) if (auto* Field = qobject_cast<QKeySequenceEdit*>(Control))
        connect(Field, &QKeySequenceEdit::keySequenceChanged, this, &DocumentSettingsDialog::validateShortcuts);
    validateShortcuts();
}
void DocumentSettingsDialog::loadControls()
{
    const auto Stored = settings::UnifiedConfig::Value(base_, {"documentViews", "shortcuts", "bindings"}).toObject();
    const auto Unknown = DocumentShortcutRegistry::Unknown(Stored);
    if (auto* Diagnostics = findChild<QLabel*>("shortcutDiagnostics"))
        Diagnostics->setText(DocumentShortcutRegistry::Validate(Stored) + (Unknown.isEmpty() ? QString{} : tr("\n지원하지 않는 항목은 원본에 보존됩니다: %1").arg(Unknown.join(", "))));
    auto Effective = base_;
    const auto Text = settings::UnifiedConfig::Value(Effective, {"documentViews", "textView"}).toObject();
    if (!Text.contains("wordWrapMode")) {
        const int Mode = Text.contains("wordWrap") ? (Text["wordWrap"].toBool() ? 1 : 0) : (Text.isEmpty() ? 2 : 0);
        settings::UnifiedConfig::Set(Effective, {"documentViews", "textView", "wordWrapMode"}, Mode);
    }
    settings::UnifiedConfig::Set(Effective, {"documentViews", "shortcuts", "bindings"}, DocumentShortcutRegistry::Bindings(settings::UnifiedConfig::Value(base_, {"documentViews", "shortcuts", "bindings"}).toObject()));
    for (auto It = controls_.begin(); It != controls_.end(); ++It) {
        const auto Raw = settings::UnifiedConfig::Value(Effective, It.key().split('/'));
        auto Value = Raw.isUndefined() ? It.value()->property("defaultValue") : settings::UnifiedConfig::Decode(Raw);
        if (auto* Field = qobject_cast<QFontComboBox*>(It.value())) Field->setCurrentFont(Value.toString().isEmpty() ? QFont("Consolas") : QFont(Value.toString()));
        else if (auto* Field = qobject_cast<QComboBox*>(It.value())) {
            if (!Value.isValid()) Value = It.key().endsWith("fontRendering") ? QVariant(2) : It.key().endsWith("changeHistoryMode") ? QVariant(3) : Field->itemData(0);
            const auto Index = Field->findData(Value); Field->setCurrentIndex(Index < 0 ? 0 : Index);
        } else if (auto* Field = qobject_cast<QSpinBox*>(It.value())) Field->setValue(Value.toInt());
        else if (auto* Field = qobject_cast<QDoubleSpinBox*>(It.value())) Field->setValue(Value.toDouble());
        else if (auto* Field = qobject_cast<QCheckBox*>(It.value())) Field->setChecked(Value.toBool());
        else if (auto* Field = qobject_cast<QLineEdit*>(It.value())) Field->setText(Value.toString());
        else if (auto* Field = qobject_cast<QKeySequenceEdit*>(It.value())) Field->setKeySequence(QKeySequence::fromString(Value.toString(), QKeySequence::PortableText));
    }
}
QJsonObject DocumentSettingsDialog::values() const
{
    auto Result = base_;
    for (auto It = controls_.begin(); It != controls_.end(); ++It) {
        QVariant Value;
        if (auto* Field = qobject_cast<QFontComboBox*>(It.value())) Value = Field->currentFont().family();
        else if (auto* Field = qobject_cast<QComboBox*>(It.value())) Value = Field->currentData();
        else if (auto* Field = qobject_cast<QSpinBox*>(It.value())) Value = Field->value();
        else if (auto* Field = qobject_cast<QDoubleSpinBox*>(It.value())) Value = Field->value();
        else if (auto* Field = qobject_cast<QCheckBox*>(It.value())) Value = Field->isChecked();
        else if (auto* Field = qobject_cast<QLineEdit*>(It.value())) Value = Field->text().trimmed();
        else if (auto* Field = qobject_cast<QKeySequenceEdit*>(It.value())) Value = Field->keySequence().toString(QKeySequence::PortableText);
        settings::UnifiedConfig::Set(Result, It.key().split('/'), settings::UnifiedConfig::Encode(Value));
    }
    settings::UnifiedConfig::Set(Result, {"documentViews", "shortcuts", "schemaVersion"}, 1);
    return Result;
}
void DocumentSettingsDialog::validateShortcuts()
{
    if (!valid_) return;
    const auto Problem = DocumentShortcutRegistry::Validate(settings::UnifiedConfig::Value(values(), {"documentViews", "shortcuts", "bindings"}).toObject());
    error_->setText(Problem);
    auto* Buttons = findChild<QDialogButtonBox*>("settingsButtons");
    Buttons->button(QDialogButtonBox::Ok)->setEnabled(Problem.isEmpty()); Buttons->button(QDialogButtonBox::Apply)->setEnabled(Problem.isEmpty());
}
bool DocumentSettingsDialog::Apply(bool ResolveConflicts)
{
    if (!valid_) return false;
    if(epubSettings_->IsBusy() && epubSettings_->IsDirty()){error_->setText(tr("EPUB 설정을 불러오고 있습니다."));return false;}
    auto Next = values();
    const auto EpubEditor=Next["documentViews"].toObject()["epubEditor"].toObject();
    if(EpubEditor["requireEpubCheck"].toBool()) {
        auto Java=EpubEditor["javaPath"].toString();if(Java.isEmpty())Java=QStandardPaths::findExecutable("java");const auto Jar=EpubEditor["epubcheckJar"].toString();
        if(!QFileInfo(Java).isAbsolute() || !QFileInfo(Java).isFile() || !QFileInfo(Jar).isAbsolute() || !QFileInfo(Jar).isFile() || QFileInfo(Jar).suffix().compare("jar",Qt::CaseInsensitive)!=0){error_->setText(tr("EPUBCheck 필수 검사에는 사용할 수 있는 Java 실행 파일과 EPUBCheck JAR 경로가 필요합니다."));return false;}
    }
    const auto Problem = DocumentShortcutRegistry::Validate(settings::UnifiedConfig::Value(Next, {"documentViews", "shortcuts", "bindings"}).toObject());
    if (!Problem.isEmpty()) { error_->setText(Problem); return false; }
    QJsonObject Saved; QString Error;
    auto Changed = base_;
    for (auto It = imported_.begin(); It != imported_.end(); ++It) Changed[It.key()] = It.value();
    Changed = settings::UnifiedConfig::Changed(initial_, Next, Changed);
    const QStringList WrapPath{"documentViews", "textView", "wordWrapMode"};
    const auto Mode = settings::UnifiedConfig::Value(Next, WrapPath);
    if (settings::UnifiedConfig::Value(base_, WrapPath).isUndefined() || Mode != settings::UnifiedConfig::Value(initial_, WrapPath)) {
        settings::UnifiedConfig::Set(Changed, WrapPath, Mode);
        if (Mode.toInt() != 0) settings::UnifiedConfig::Set(Changed, {"documentViews", "textView", "wordWrapLastMode"}, Mode);
        else {
            const auto Previous = settings::UnifiedConfig::Value(initial_, WrapPath).toInt();
            if (Previous != 0) settings::UnifiedConfig::Set(Changed, {"documentViews", "textView", "wordWrapLastMode"}, Previous);
        }
        settings::UnifiedConfig::Set(Changed, {"documentViews", "textView", "wordWrap"}, Mode.toInt() != 0);
    }
    for (const auto& Path : {QStringList{"appearance", "documentWorkspace"}, QStringList{"documentViews", "shortcuts"}}) {
        auto Object = settings::UnifiedConfig::Value(Changed, Path).toObject();
        if (!Object.isEmpty() && !Object.contains("schemaVersion")) { Object["schemaVersion"] = 1; settings::UnifiedConfig::Set(Changed, Path, Object); }
    }
    if (!settings::UnifiedConfig::Save(path_, base_, Changed, Saved, Error, ResolveConflicts)) {
        error_->setText(tr("설정을 저장하지 못했습니다.\n%1\n%2").arg(path_, Error));
        if (!ResolveConflicts && Error.startsWith("conflict:")) {
            hc::ui::MessageDialog Dialog(QMessageBox::Warning, tr("설정 변경 충돌"), tr("다른 창에서 같은 설정을 변경했습니다."), QMessageBox::Cancel, this);
            auto* Reload = Dialog.addButton(tr("다시 읽기"), QMessageBox::ResetRole);
            auto* Keep = Dialog.addButton(tr("현재 입력 적용"), QMessageBox::AcceptRole); Dialog.exec();
            if (Dialog.clickedButton() == Keep) return Apply(true);
            if (Dialog.clickedButton() == Reload) { valid_ = settings::UnifiedConfig::Load(path_, base_, Error); if (valid_) { imported_ = {}; loadControls(); initial_ = values(); error_->clear(); } }
        }
        return false;
    }
    base_ = Saved; imported_ = {}; loadControls(); initial_ = values();
    if(!epubSettings_->CommitGlobal(Error)){error_->setText(tr("일반 설정은 적용되었으나 EPUB 설정을 적용하지 못했습니다. 입력은 유지됩니다.\n%1").arg(Error));emit PreferencesSaved();return false;}
    error_->clear(); emit PreferencesSaved(); return true;
}
}
