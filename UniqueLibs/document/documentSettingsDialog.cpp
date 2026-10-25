#include "documentSettingsDialog.hpp"
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

namespace hc::document {
DocumentSettingsDialog::DocumentSettingsDialog(QString Path, const QList<QAction*>& Actions, QWidget* Parent)
    : QDialog(Parent), path_(std::move(Path))
{
    setObjectName("documentSettingsDialog"); setWindowTitle(tr("설정"));
    const auto Screen = screen()->availableGeometry().size(); resize(qMin(900, Screen.width() - 40), qMin(680, Screen.height() - 60));
    auto* Layout = new QVBoxLayout(this); auto* Body = new QHBoxLayout;
    auto* List = new QListWidget(this); List->setObjectName("settingsCategories");
    List->addItems({tr("공통"), tr("텍스트 편집기"), tr("단축키"),tr("EPUB")}); List->setMaximumWidth(180);
    auto* Pages = new QStackedWidget(this); Body->addWidget(List); Body->addWidget(Pages, 1); Layout->addLayout(Body, 1);
    connect(List, &QListWidget::currentRowChanged, Pages, &QStackedWidget::setCurrentIndex); List->setCurrentRow(0);
    auto Page = [&] {
        auto* Scroll = new QScrollArea(Pages); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame);
        auto* Content = new QWidget(Scroll); auto* Form = new QFormLayout(Content); Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Scroll->setWidget(Content); Pages->addWidget(Scroll); return Form;
    };
    auto Combo = [&](QFormLayout* Form, const QString& Key, const QString& Label, const QStringList& Labels, const QVariantList& Values) {
        auto* Field = new QComboBox(this); for (int Index = 0; Index < Labels.size(); ++Index) Field->addItem(Labels[Index], Values[Index]);
        Field->setObjectName(Key); controls_[Key] = Field; Form->addRow(Label, Field);
    };
    auto Check = [&](QFormLayout* Form, const QString& Key, const QString& Label, bool Default) {
        auto* Field = new QCheckBox(Label, this); Field->setProperty("defaultValue", Default); Field->setObjectName(Key);
        controls_[Key] = Field; Form->addRow(Field);
    };
    auto* General = Page();
    Check(General, "documentViews/common/closeViewerOnEscape", tr("ESC 키를 눌러 창 닫기 (HyperViewer 전용)"), true);
    Combo(General, "appearance/documentWorkspace/themeMode", tr("테마"), {tr("시스템"), tr("밝게"), tr("어둡게")}, {"system", "light", "dark"});
    for (const auto& Key : {QString("mainMenuMode"), QString("mainToolbarMode")})
        Combo(General, "appearance/documentWorkspace/" + Key, Key == "mainMenuMode" ? tr("메뉴 표시") : tr("도구모음 표시"), {tr("뷰 권장값"), tr("표시"), tr("숨기기")}, {"recommended", "show", "hide"});
    auto* Location = new QLabel(tr("설정 파일: %1").arg(path_), this); Location->setWordWrap(true); Location->setTextInteractionFlags(Qt::TextSelectableByMouse); General->addRow(Location);
    auto* Import = new QPushButton(tr("기존 설정 가져오기…"), this); General->addRow(Import);
    connect(Import, &QPushButton::clicked, this, [this] {
        const auto Directory = QFileDialog::getExistingDirectory(this, tr("기존 설정 폴더 선택"), QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        if (Directory.isEmpty()) return;
        const auto Source = settings::UnifiedConfig::FileName(Directory); QJsonObject Profile; QString Error;
        if (!settings::UnifiedConfig::Load(Source, Profile, Error, false) || Profile.size() <= 1) { error_->setText(tr("가져올 설정을 읽을 수 없습니다.\n%1\n%2").arg(Source, Error)); return; }
        if (QMessageBox::question(this, tr("기존 설정 가져오기"), tr("%1의 설정을 현재 설정에 가져옵니다. 적용 또는 확인을 누르면 저장됩니다. 원본 파일은 유지됩니다.").arg(Directory)) != QMessageBox::Yes) return;
        imported_ = Profile; const auto Original = base_;
        for (auto It = Profile.begin(); It != Profile.end(); ++It) base_[It.key()] = It.value();
        loadControls(); base_ = Original; error_->clear();
    });
    auto* Text = Page();
    auto* Font = new QFontComboBox(this); Font->setObjectName("documentViews/textView/fontFamily");
    controls_[Font->objectName()] = Font; Text->addRow(tr("글꼴"), Font);
    auto Spin = [&](const QString& Key, const QString& Label, int Minimum, int Maximum, int Default) {
        auto* Field = new QSpinBox(this); Field->setRange(Minimum, Maximum); Field->setProperty("defaultValue", Default);
        Field->setObjectName("documentViews/textView/" + Key); controls_[Field->objectName()] = Field; Text->addRow(Label, Field);
    };
    Spin("fontSize", tr("글꼴 크기 (pt)"), 6, 72, 10); Spin("tabWidth", tr("탭 너비"), 1, 16, 4);
    auto* Spacing = new QDoubleSpinBox(this); Spacing->setRange(1.0, 3.0); Spacing->setSingleStep(.1); Spacing->setDecimals(2); Spacing->setProperty("defaultValue", 1.1);
    Spacing->setObjectName("documentViews/textView/lineSpacing"); controls_[Spacing->objectName()] = Spacing; Text->addRow(tr("줄 간격"), Spacing);
    const QList<QPair<QString, QString>> Checks{{"showLineNumbers", tr("줄 번호")}, {"showRuler", tr("눈금자")},
        {"highlightCurrentLine", tr("현재 줄 강조")}, {"showIndentationGuides", tr("들여쓰기 안내선")},
        {"autoIndent", tr("자동 들여쓰기")}, {"useTabs", tr("들여쓰기에 탭 사용")}, {"braceHighlight", tr("괄호 강조")},
        {"showCodeFolding", tr("코드 접기")}, {"showWhitespace", tr("공백·줄끝 표시")}};
    for (const auto& Item : Checks) Check(Text, "documentViews/textView/" + Item.first, Item.second, Item.first != "wordWrap" && Item.first != "showWhitespace");
    Combo(Text, "documentViews/textView/wordWrapMode", tr("자동 줄바꿈"), {tr("없음"), tr("단어 단위"), tr("문자 단위"), tr("공백 단위")}, {0, 1, 2, 3});
    Combo(Text, "documentViews/textView/wrapVisualFlags", tr("줄바꿈 이어짐 표시"),
        {tr("없음"), tr("줄 끝"), tr("줄 시작"), tr("줄 끝·시작"), tr("여백"), tr("줄 끝·여백"), tr("줄 시작·여백"), tr("모두")}, {0, 1, 2, 3, 4, 5, 6, 7});
    controls_["documentViews/textView/wrapVisualFlags"]->setProperty("defaultValue", 1);
    Combo(Text, "documentViews/textView/wrapIndentMode", tr("이어지는 행 들여쓰기"),
        {tr("고정"), tr("원래 들여쓰기"), tr("한 단계 추가"), tr("두 단계 추가")}, {0, 1, 2, 3});
    controls_["documentViews/textView/wrapIndentMode"]->setProperty("defaultValue", 1);
    auto* RulerFont = new QFontComboBox(this); RulerFont->setObjectName("documentViews/textView/rulerFontFamily");
    controls_[RulerFont->objectName()] = RulerFont; Text->addRow(tr("눈금자 글꼴"), RulerFont);
    Spin("rulerFontSize", tr("눈금자 글꼴 크기 (pt)"), 6, 36, 8);
    Combo(Text, "documentViews/textView/indentGuideStyle", tr("들여쓰기 안내선 모드"), {tr("실제 들여쓰기"), tr("다음 줄 참조"), tr("앞뒤 줄 참조")}, {1, 2, 3});
    Combo(Text, "documentViews/textView/fontRendering", tr("글꼴 렌더링"), {tr("기본"), tr("안티앨리어싱 없음"), tr("회색조"), tr("ClearType")}, {0, 1, 2, 3});
    Combo(Text, "documentViews/textView/changeHistoryMode", tr("변경 이력 표시"), {tr("끄기"), tr("여백 표시"), tr("본문 표시"), tr("모두")}, {0, 1, 2, 3});
    Combo(Text, "documentViews/textView/defaultLineEnding", tr("새 줄 기본 줄끝"), {"CRLF", "LF", "CR"}, {"CRLF", "LF", "CR"});
    Combo(Text, "documentViews/textView/saveEncoding", tr("새 문서 기본 인코딩"), {"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"}, {"UTF-8", "UTF-16LE", "UTF-16BE", "CP949"});
    Combo(Text, "documentViews/textView/saveBomMode", tr("새 문서 BOM"), {tr("인코딩 권장값"), tr("포함"), tr("제외")}, {0, 1, 2});
    controls_["documentViews/textView/saveBomMode"]->setProperty("defaultValue", 1);
    Check(Text, "documentViews/textView/recoveryEnabled", tr("미저장 문서의 주기적 복구 사본 생성"), true);
    Spin("largeFileMB", tr("대용량 안내 기준 (MiB)"), 1, 16, 1);
    Combo(Text, "documentViews/textView/externalChangeAction", tr("외부 파일 변경"), {tr("감시 끄기"), tr("수정하지 않은 문서 자동 다시 읽기"), tr("항상 확인")}, {0, 1, 2});
    controls_["documentViews/textView/externalChangeAction"]->setProperty("defaultValue", 1);
    Combo(Text, "documentViews/textView/externalChangeDetection", tr("변경 감지 방식"), {tr("OS 알림"), tr("주기적 확인")}, {0, 1});
    Spin("externalChangePollSeconds", tr("확인 간격 (초, OS 알림 실패 시에도 사용)"), 1, 3600, 3);
    auto* FilePolicyHint = new QLabel(tr("기존 파일의 인코딩·BOM·줄끝은 유지합니다. 수정 중인 문서는 다시 읽기 전에 확인합니다. 복구를 꺼도 F3 문서 교체 시 미저장 내용은 보호합니다."), this);
    FilePolicyHint->setWordWrap(true); Text->addRow(FilePolicyHint);
    Check(Text, "documentViews/textView/mask/enabled", tr("글자 마스크 보정"), false);
    auto MaskSpin = [&](const QString& Key, const QString& Label, double Maximum, double Default) {
        auto* Field = new QDoubleSpinBox(this); Field->setRange(.5, Maximum); Field->setSingleStep(.05); Field->setDecimals(2);
        Field->setProperty("defaultValue", Default); Field->setObjectName("documentViews/textView/mask/" + Key);
        controls_[Field->objectName()] = Field; Text->addRow(Label, Field);
        auto* Enabled = qobject_cast<QCheckBox*>(controls_["documentViews/textView/mask/enabled"]);
        Field->setEnabled(false); connect(Enabled, &QCheckBox::toggled, Field, &QWidget::setEnabled);
    };
    MaskSpin("gamma", tr("글자 마스크 감마"), 3.0, 1.2); MaskSpin("strength", tr("글자 마스크 강도"), 2.0, 1.1);
    Q_UNUSED(Actions);
    auto* Shortcuts = Page();
    auto* Diagnostics = new QLabel(this); Diagnostics->setObjectName("shortcutDiagnostics"); Diagnostics->setWordWrap(true); Shortcuts->addRow(Diagnostics);
    auto* Search = new QLineEdit(this); Search->setObjectName("shortcutSearch"); Search->setPlaceholderText(tr("명령 또는 단축키 검색…")); Shortcuts->addRow(Search);
    auto* Scope = new QComboBox(this); Scope->addItem(tr("전체"), ""); Scope->addItem(tr("공통"), "common"); Scope->addItem(tr("텍스트·편집"), "text"); Scope->addItem(tr("EPUB"),"epub");Shortcuts->addRow(tr("분류"), Scope);
    auto* ChangedOnly = new QCheckBox(tr("변경한 항목만"), this); Shortcuts->addRow(ChangedOnly);
    QList<QPair<DocumentShortcut, QWidget*>> Rows;
    for (const auto& Command : DocumentShortcutRegistry::Commands()) {
        auto* Row = new QWidget(this); auto* RowLayout = new QVBoxLayout(Row); RowLayout->setContentsMargins(0, 0, 0, 4);
        auto* Caption = new QLabel(tr("%1 · %2 · 기본값 %3").arg(Command.label_, Command.scope_ == "common" ? tr("공통") : Command.scope_=="epub"?tr("EPUB"):tr("텍스트·편집"), Command.key_.isEmpty() ? tr("없음") : Command.key_), Row); Caption->setWordWrap(true); RowLayout->addWidget(Caption);
        auto* Input = new QHBoxLayout; auto* Field = new QKeySequenceEdit(Row); Field->setClearButtonEnabled(true); Field->setMaximumSequenceLength(2);
        const auto Key = "documentViews/shortcuts/bindings/" + Command.id_;
        Field->setObjectName(Key); Field->setProperty("defaultValue", Command.key_); Field->setAccessibleName(Command.label_);
        controls_[Key] = Field; Input->addWidget(Field, 1);
        auto* Reset = new QPushButton(tr("기본값"), Row); Input->addWidget(Reset);
        auto* Previous = new QPushButton(tr("이전 값"), Row); Input->addWidget(Previous); RowLayout->addLayout(Input);
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
    auto* Hint = new QLabel(tr("빈 값은 단축키 해제입니다. 최대 두 번의 키 조합을 사용할 수 있습니다. 이동·선택 기본키와 입력기 키는 유지됩니다."), this); Hint->setWordWrap(true); Shortcuts->addRow(Hint);
    auto* EpubForm=Page();
    epubSettings_=new hc::epub::poc::ReadingSettings(QDir(QFileInfo(path_).absolutePath()).filePath("epub-reading.json"),QString(64,'0'),"light",this);
    epubSettings_->setObjectName("epubGlobalSettings");epubSettings_->SetGlobalOnly();EpubForm->addRow(epubSettings_);
    error_ = new QLabel(this); error_->setObjectName("settingsError"); error_->setWordWrap(true); error_->setTextInteractionFlags(Qt::TextSelectableByMouse); Layout->addWidget(error_);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, this); Buttons->setObjectName("settingsButtons"); Layout->addWidget(Buttons);
    Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인")); Buttons->button(QDialogButtonBox::Apply)->setText(tr("적용"));
    Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소")); Buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("기본값"));
    connect(Buttons, &QDialogButtonBox::accepted, this, [this] { if (Apply()) accept(); });
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { Apply(); });
    connect(Buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this, List] {
        if(List->currentRow()==3){epubSettings_->ResetDraft();return;}
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
            QMessageBox Dialog(QMessageBox::Warning, tr("설정 변경 충돌"), tr("다른 창에서 같은 설정을 변경했습니다."), QMessageBox::Cancel, this);
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
