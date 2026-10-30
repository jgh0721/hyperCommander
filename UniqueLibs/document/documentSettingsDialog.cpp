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
#include <QTimer>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QFrame>
#include <QGroupBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QColorDialog>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QSet>
#include "text/TextLexerRegistry.h"
#include "text/ScintillaQtDirectBackend.h"
#include "text/ScintillaEditorSettings.h"
#include "core/ThemeManager.h"
#include <Scintilla.h>
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
    List->addItems({tr("공통"), tr("텍스트 뷰어"), tr("단축키"),tr("EPUB"),tr("마크다운")}); List->setFixedWidth(180);
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
    auto* Text = Page(tr("텍스트 뷰어"), tr("글꼴과 편집 화면, 들여쓰기·줄바꿈, 새 문서 저장과 외부 파일 변경 처리를 설정합니다."));
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
    auto* TextCommon = Pages->widget(1); Pages->removeWidget(TextCommon);
    auto* TextTabs = new QTabWidget(Pages); TextTabs->setObjectName("textViewerTabs");
    TextTabs->addTab(TextCommon, tr("공통")); TextTabs->addTab(createTextColorPage(), tr("색상"));
    Pages->insertWidget(1, TextTabs);
    Q_UNUSED(Actions);
    auto* ShortcutPage = Page(tr("단축키"), tr("명령이 동작하는 범위별로 단축키를 바꿉니다. 함께 동작할 수 있는 범위끼리만 충돌을 검사합니다."));
    auto* Diagnostics = new QLabel(this); Diagnostics->setObjectName("shortcutDiagnostics"); Diagnostics->setWordWrap(true); ShortcutPage->addWidget(Diagnostics);
    auto* FilterRow = new QHBoxLayout; FilterRow->setSpacing(8); ShortcutPage->addLayout(FilterRow);
    auto* Search = new QLineEdit(this); Search->setObjectName("shortcutSearch"); Search->setClearButtonEnabled(true); Search->setPlaceholderText(tr("모든 범위에서 명령 또는 단축키 검색…")); FilterRow->addWidget(Search, 1);
    auto* ChangedOnly = new QCheckBox(tr("변경한 항목만"), this); ChangedOnly->setObjectName("shortcutChangedOnly"); FilterRow->addWidget(ChangedOnly);
    // Scope list at the left, the selected scope's commands at the right.
    auto* ShortcutBody = new QHBoxLayout; ShortcutBody->setSpacing(12); ShortcutPage->addLayout(ShortcutBody);
    auto* Scopes = new QListWidget(this); Scopes->setObjectName("shortcutScopes"); Scopes->setAccessibleName(tr("단축키 범위")); Scopes->setFixedWidth(184); Scopes->setProperty(fm::style::props::kSoftSelection, true); Scopes->setFrameShape(QFrame::NoFrame);
    for (const auto& Item : DocumentShortcutRegistry::Scopes()) { auto* Entry = new QListWidgetItem(Scopes); Entry->setData(Qt::UserRole, Item.id_); Entry->setData(Qt::UserRole + 1, QString(Item.depth_ * 2, ' ') + Item.label_); Entry->setSizeHint(QSize(0, 28)); }
    ShortcutBody->addWidget(Scopes, 0, Qt::AlignTop);
    auto* Commands = new QGroupBox(this); Commands->setObjectName("shortcutCommands"); auto* Column = new QVBoxLayout(Commands); Column->setSpacing(4); ShortcutBody->addWidget(Commands, 1);
    auto* Header = new QHBoxLayout; auto* Title = new QLabel(Commands); Title->setObjectName("shortcutScopeTitle"); auto TitleFont = Title->font(); TitleFont.setWeight(QFont::DemiBold); TitleFont.setPointSizeF(TitleFont.pointSizeF() * 1.1); Title->setFont(TitleFont); Header->addWidget(Title, 1);
    auto* ScopeReset = new QPushButton(tr("이 범위 기본값으로"), Commands); ScopeReset->setObjectName("shortcutScopeReset"); fm::style::setSmall(ScopeReset); Header->addWidget(ScopeReset); Column->addLayout(Header);
    auto* Description = new QLabel(Commands); Description->setObjectName("shortcutScopeDescription"); Description->setWordWrap(true); Description->setForegroundRole(QPalette::PlaceholderText); Column->addWidget(Description);
    auto* Related = new QLabel(Commands); Related->setObjectName("shortcutScopeRelated"); Related->setWordWrap(true); Related->setForegroundRole(QPalette::PlaceholderText); Column->addWidget(Related);
    auto* Overlap = new QLabel(tr("검색한 키는 함께 동작하지 않는 범위에서만 겹치므로 충돌이 아닙니다."), Commands); Overlap->setObjectName("shortcutOverlapNote"); Overlap->setWordWrap(true); Overlap->hide(); Column->addWidget(Overlap);
    QList<QPair<DocumentShortcut, QWidget*>> Rows;
    // One block per command: name with its default key, the key field and its two resets, then any problem.
    for (const auto& Command : DocumentShortcutRegistry::Commands()) {
        auto* Block = new QWidget(Commands); Block->setObjectName("shortcutRow/" + Command.id_); auto* BlockLayout = new QVBoxLayout(Block); BlockLayout->setContentsMargins(0, 2, 0, 2); BlockLayout->setSpacing(2);
        auto* RowLayout = new QHBoxLayout; RowLayout->setSpacing(8); BlockLayout->addLayout(RowLayout);
        auto* Names = new QVBoxLayout; Names->setSpacing(0); RowLayout->addLayout(Names, 1);
        auto* Name = new QLabel(Command.label_, Block); Name->setWordWrap(true); Names->addWidget(Name);
        auto* Meta = new QLabel(tr("기본값 %1").arg(Command.key_.isEmpty() ? tr("없음") : QKeySequence(Command.key_).toString(QKeySequence::NativeText)), Block); Meta->setObjectName("shortcutMeta"); Meta->setProperty("scopeLabel", DocumentShortcutRegistry::ScopeLabel(Command.scope_));
        Meta->setForegroundRole(QPalette::PlaceholderText); Names->addWidget(Meta);
        auto* Field = new QKeySequenceEdit(Block); Field->setClearButtonEnabled(true); Field->setMaximumSequenceLength(2); Field->setMinimumWidth(140);
        const auto Key = "documentViews/shortcuts/bindings/" + Command.id_;
        Field->setObjectName(Key); Field->setProperty("defaultValue", Command.key_); Field->setProperty("shortcutScope", Command.scope_); Field->setAccessibleName(Command.label_); Name->setBuddy(Field);
        controls_[Key] = Field; RowLayout->addWidget(Field);
        auto* Reset = new QPushButton(tr("기본값"), Block); fm::style::setSmall(Reset); RowLayout->addWidget(Reset);
        auto* Previous = new QPushButton(tr("이전 값"), Block); fm::style::setSmall(Previous); RowLayout->addWidget(Previous);
        auto* Issue = new QHBoxLayout; auto* Problem = new QLabel(Block); Problem->setObjectName("shortcutIssue/" + Command.id_); Problem->setWordWrap(true); Problem->setTextFormat(Qt::PlainText); Problem->hide(); Issue->addWidget(Problem, 1);
        auto* Jump = new QPushButton(Block); Jump->setObjectName("shortcutIssueJump/" + Command.id_); Jump->setFlat(true); fm::style::setSmall(Jump); Jump->hide(); Issue->addWidget(Jump); BlockLayout->addLayout(Issue);
        connect(Reset, &QPushButton::clicked, this, [Field, Default = Command.key_] { Field->setKeySequence(QKeySequence(Default)); });
        connect(Previous, &QPushButton::clicked, this, [this, Field, Key] { Field->setKeySequence(QKeySequence(settings::UnifiedConfig::Value(initial_, Key.split('/')).toString())); });
        Column->addWidget(Block); Rows.append({Command, Block});
    }
    auto* Footnote = new QLabel(tr("빈 값은 단축키 해제입니다. 최대 두 번의 키 조합을 사용할 수 있습니다. 입력 범위의 이동·선택 기본키와 입력기 키, Esc · Tab · F6, 트리 · 목록의 Enter는 유지됩니다."), Commands); Footnote->setWordWrap(true); Footnote->setForegroundRole(QPalette::PlaceholderText); Column->addWidget(Footnote);
    auto Filter = [Search, Scopes, ChangedOnly, Rows, Title, Description, Related, Overlap, ScopeReset] {
        const auto Text = Search->text().trimmed(); const auto Selected = Scopes->currentItem() ? Scopes->currentItem()->data(Qt::UserRole).toString() : QStringLiteral("common");
        QMap<QString, int> Matches; QMap<QString, QStringList> Keys; int Visible = 0;
        for (const auto& Row : Rows) {
            auto* Field = Row.second->findChild<QKeySequenceEdit*>();
            const auto Current = Field->keySequence().toString(QKeySequence::NativeText);
            const bool Match = !Text.isEmpty() && (Row.first.label_ + " " + Row.first.id_ + " " + Current + " " + QKeySequence(Row.first.key_).toString(QKeySequence::NativeText)).contains(Text, Qt::CaseInsensitive);
            const bool Changed = !ChangedOnly->isChecked() || Field->keySequence() != QKeySequence(Row.first.key_);
            if (Match && Changed) { ++Matches[Row.first.scope_]; if (!Current.isEmpty()) Keys[Current].append(Row.first.scope_); }
            const bool Show = (Text.isEmpty() ? Row.first.scope_ == Selected : Match) && Changed; Row.second->setVisible(Show); Visible += Show;
            auto* Meta = Row.second->findChild<QLabel*>("shortcutMeta"); const auto Base = Meta->text().section(" · ", -1);
            Meta->setText(Text.isEmpty() ? Base : Meta->property("scopeLabel").toString() + " · " + Base);
        }
        bool Allowed = false; for (auto I = Keys.begin(); I != Keys.end(); ++I) for (const auto& A : I.value()) for (const auto& B : I.value()) Allowed |= A != B && !DocumentShortcutRegistry::CoActive(A, B);
        for (int Index = 0; Index < Scopes->count(); ++Index) {
            auto* Entry = Scopes->item(Index); const auto Id = Entry->data(Qt::UserRole).toString(); int Count = 0; for (const auto& Row : Rows) Count += Row.first.scope_ == Id;
            const int Problems = Entry->data(Qt::UserRole + 2).toInt();
            Entry->setText(Entry->data(Qt::UserRole + 1).toString() + QStringLiteral("  ") + (Text.isEmpty() ? QString::number(Count) : QString::number(Matches.value(Id))) + (Problems ? QObject::tr("  ⚠ %1").arg(Problems) : QString{}));
            Entry->setForeground(!Text.isEmpty() && !Matches.value(Id) ? Scopes->palette().color(QPalette::PlaceholderText) : Scopes->palette().color(QPalette::Text));
        }
        if (Text.isEmpty()) {
            for (const auto& Item : DocumentShortcutRegistry::Scopes()) if (Item.id_ == Selected) { Title->setText(Item.label_); Description->setText(Item.description_); }
            QStringList Names; for (const auto& Scope : DocumentShortcutRegistry::ConflictScopes(Selected)) Names.append(DocumentShortcutRegistry::ScopeLabel(Scope));
            Related->setText(QObject::tr("충돌 검사 대상: %1").arg(Names.join(" · ")));
        } else { Title->setText(QObject::tr("'%1' 검색 결과 %2건").arg(Text).arg(Visible)); Description->setText(QObject::tr("모든 범위")); Related->clear(); }
        Related->setVisible(Text.isEmpty()); ScopeReset->setVisible(Text.isEmpty()); Overlap->setVisible(!Text.isEmpty() && Allowed);
    };
    connect(Search, &QLineEdit::textChanged, this, Filter); connect(Scopes, &QListWidget::currentRowChanged, this, Filter); connect(ChangedOnly, &QCheckBox::toggled, this, Filter);
    for (const auto& Row : Rows) connect(Row.second->findChild<QKeySequenceEdit*>(), &QKeySequenceEdit::keySequenceChanged, this, Filter);
    connect(this, &DocumentSettingsDialog::ShortcutsValidated, this, Filter);
    connect(ScopeReset, &QPushButton::clicked, this, [Scopes, Rows] { const auto Selected = Scopes->currentItem() ? Scopes->currentItem()->data(Qt::UserRole).toString() : QString{};
        for (const auto& Row : Rows) if (Row.first.scope_ == Selected) Row.second->findChild<QKeySequenceEdit*>()->setKeySequence(QKeySequence(Row.first.key_)); });
    for (const auto& Row : Rows) connect(Row.second->findChild<QPushButton*>("shortcutIssueJump/" + Row.first.id_), &QPushButton::clicked, this, [Search, Scopes, Rows, Row] {
        const auto Other = Row.second->findChild<QPushButton*>("shortcutIssueJump/" + Row.first.id_)->property("otherId").toString();
        for (const auto& Target : Rows) if (Target.first.id_ == Other) { Search->clear(); for (int I = 0; I < Scopes->count(); ++I) if (Scopes->item(I)->data(Qt::UserRole) == Target.first.scope_) Scopes->setCurrentRow(I); Target.second->findChild<QKeySequenceEdit*>()->setFocus(); }
    });
    Scopes->setCurrentRow(0);
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
    auto* Markdown=Page(tr("마크다운"),tr("미리보기 글꼴과 문법, 개요 및 소스·본문 동기화를 설정합니다."));
    auto* MdFonts=Card(Markdown,tr("미리보기 글꼴 (.MD)"));
    const QString MdPrefix="documentViews/markdown/",FontPrefix=MdPrefix+"previewFonts/";
    Check(MdFonts,FontPrefix+"enabled",tr("글꼴 사용자 설정 사용"),false);
    Combo(MdFonts,FontPrefix+"mode",tr("글꼴 적용"),{tr("문서 글꼴 (없으면 웹뷰 기본)"),tr("웹뷰 기본"),tr("사용자 글꼴")},{"document","web","user"});
    auto MdSpin=[&](QFormLayout* Form,const QString& Key,const QString& Label,int Low,int High,int Default){auto* Field=new QSpinBox(this);Field->setRange(Low,High);Field->setObjectName(Key);Field->setProperty("defaultValue",Default);controls_[Key]=Field;Form->addRow(Label,Field);};
    for(const auto& Pair:{std::pair{"bodyFamily",tr("본문 글꼴")},std::pair{"codeFamily",tr("코드 글꼴")}}){auto* Field=new QFontComboBox(this);const QString Key=FontPrefix+Pair.first;Field->setObjectName(Key);Field->setProperty("defaultValue",QString(Pair.first)=="bodyFamily"?"맑은 고딕":"Consolas");Field->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);Field->setMinimumContentsLength(12);controls_[Key]=Field;MdFonts->addRow(Pair.second,Field);}
    MdSpin(MdFonts,FontPrefix+"bodySizePt",tr("본문 크기 (pt)"),8,72,11);MdSpin(MdFonts,FontPrefix+"codeSizePt",tr("코드 크기 (pt)"),8,72,10);
    auto* MdSpacing=new QDoubleSpinBox(this);MdSpacing->setRange(1.,3.);MdSpacing->setSingleStep(.1);MdSpacing->setDecimals(2);MdSpacing->setObjectName(FontPrefix+"lineHeight");MdSpacing->setProperty("defaultValue",1.7);controls_[MdSpacing->objectName()]=MdSpacing;MdFonts->addRow(tr("본문 행간"),MdSpacing);
    Check(MdFonts,FontPrefix+"headings",tr("제목에도 선택한 글꼴 적용"),true);
    Note(MdFonts,tr("미리보기에만 적용합니다. 문서 글꼴을 선택하거나 사용자 설정을 끄면 원래 문서 스타일로 돌아갑니다. 수식·아이콘 글꼴은 유지합니다."));
    auto* MdSample=new QLabel(this);MdSample->setObjectName("markdownFontSample");MdSample->setWordWrap(true);MdSample->setMargin(16);MdSample->setFrameShape(QFrame::StyledPanel);MdFonts->addRow(tr("적용 견본"),MdSample);
    auto UpdateFonts=[this,FontPrefix,MdSample]{
        const bool Enabled=qobject_cast<QCheckBox*>(controls_[FontPrefix+"enabled"])->isChecked();
        auto* Mode=qobject_cast<QComboBox*>(controls_[FontPrefix+"mode"]);Mode->setEnabled(Enabled);const bool Custom=Enabled&&Mode->currentData()=="user",Override=Enabled&&Mode->currentData()!="document";
        for(const auto& Key:{"bodyFamily","codeFamily","bodySizePt","codeSizePt"})controls_[FontPrefix+Key]->setEnabled(Custom);
        for(const auto& Key:{"lineHeight","headings"})controls_[FontPrefix+Key]->setEnabled(Override);
        const auto Family=qobject_cast<QFontComboBox*>(controls_[FontPrefix+"bodyFamily"])->currentFont().family().toHtmlEscaped();
        const auto CodeFamily=qobject_cast<QFontComboBox*>(controls_[FontPrefix+"codeFamily"])->currentFont().family().toHtmlEscaped();
        const int Size=qobject_cast<QSpinBox*>(controls_[FontPrefix+"bodySizePt"])->value(),CodeSize=qobject_cast<QSpinBox*>(controls_[FontPrefix+"codeSizePt"])->value();
        const auto Body=Custom?QString("font-family:'%1';font-size:%2pt;").arg(Family).arg(Size):QString();
        const auto Code=Custom?QString("font-family:'%1';font-size:%2pt;").arg(CodeFamily).arg(CodeSize):QString("font-family:Consolas;");
        const auto Heading=Custom&&qobject_cast<QCheckBox*>(controls_[FontPrefix+"headings"])->isChecked()?QString("font-family:'%1';").arg(Family):QString();
        const auto Spacing=Override?qobject_cast<QDoubleSpinBox*>(controls_[FontPrefix+"lineHeight"])->value():1.7;
        MdSample->setText(QString("<h3 style=\"%3\">문서에 맞는 읽기 환경</h3><p style=\"%1;line-height:%4%;\">미리보기 본문의 글꼴을 확인합니다.<br>행간 설정을 함께 확인합니다.</p><p style=\"%2\">const auto view = &quot;preview&quot;;</p><p style=\"font-family:Cambria\">E = mc<sup>2</sup></p>").arg(Body,Code,Heading).arg(qRound(Spacing*100)));
    };
    for(const auto& Key:{"enabled","headings"})connect(qobject_cast<QCheckBox*>(controls_[FontPrefix+Key]),&QCheckBox::toggled,this,UpdateFonts);
    for(const auto& Key:{"mode","bodyFamily","codeFamily"})connect(qobject_cast<QComboBox*>(controls_[FontPrefix+Key]),&QComboBox::currentIndexChanged,this,UpdateFonts);
    for(const auto& Key:{"bodySizePt","codeSizePt"})connect(qobject_cast<QSpinBox*>(controls_[FontPrefix+Key]),&QSpinBox::valueChanged,this,UpdateFonts);
    connect(MdSpacing,&QDoubleSpinBox::valueChanged,this,UpdateFonts);
    auto* MdView=Card(Markdown,tr("보기·개요"));
    Combo(MdView,MdPrefix+"viewerMode",tr("HyperViewer 기본 보기"),{tr("미리보기"),tr("소스")},{"preview","source"});
    Combo(MdView,MdPrefix+"editorMode",tr("HyperEditor 기본 보기"),{tr("나란히 보기"),tr("소스"),tr("미리보기")},{"split","source","preview"});
    Check(MdView,MdPrefix+"viewerOutline",tr("HyperViewer에서 개요 표시"),false);Check(MdView,MdPrefix+"editorOutline",tr("HyperEditor에서 개요 표시"),true);
    MdSpin(MdView,MdPrefix+"outlineDepth",tr("개요 최대 깊이"),1,6,6);
    Check(MdView,MdPrefix+"positionSync",tr("소스·본문 위치 동기화"),true);Check(MdView,MdPrefix+"scrollSync",tr("소스·본문 스크롤 동기화"),true);
    auto UpdateSync=[this,MdPrefix]{controls_[MdPrefix+"scrollSync"]->setEnabled(qobject_cast<QCheckBox*>(controls_[MdPrefix+"positionSync"])->isChecked());};
    connect(qobject_cast<QCheckBox*>(controls_[MdPrefix+"positionSync"]),&QCheckBox::toggled,this,UpdateSync);
    Note(MdView,tr("위치 동기화는 캐럿·본문 클릭 위치를, 스크롤 동기화는 화면 이동을 연결합니다. 위치 동기화를 끄면 스크롤 동기화는 비활성화되며 체크 상태를 유지합니다."));
    auto* MdRender=Card(Markdown,tr("문법·렌더링"));
    for(const auto& Pair:{std::pair{"math",tr("수식 표시 (KaTeX)")},std::pair{"inlineDollar",tr("인라인 $…$ 수식")},std::pair{"mermaid",tr("Mermaid 다이어그램")},std::pair{"codeHighlight",tr("코드 블록 구문강조")},std::pair{"autoPreview",tr("입력 후 미리보기 자동 갱신")}})Check(MdRender,MdPrefix+Pair.first,Pair.second,true);
    MdSpin(MdRender,MdPrefix+"debounceMs",tr("입력 후 갱신 지연 (ms)"),100,3000,250);Check(MdRender,MdPrefix+"remoteImages",tr("외부 이미지 자동 다운로드"),false);
    Note(MdRender,tr("수식·다이어그램 렌더러와 글꼴은 내장되어 오프라인에서도 표시합니다. 원문 HTML은 텍스트로 표시하며 스크립트는 실행하지 않습니다."));
    Markdown->addStretch();QTimer::singleShot(0,this,[UpdateFonts,UpdateSync]{UpdateFonts();UpdateSync();});
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
        if (List->currentRow() == 1 && findChild<QTabWidget*>("textViewerTabs")->currentIndex() == 1) {
            auto Mode = colorTheme_->currentData().toString(); auto Colors = textColors_[Mode].toObject();
            for (const auto& Key : Colors.keys()) Colors[Key] = QJsonValue::Null;
            const auto Legacy = legacyTextColors_[Mode].toObject();
            for (const auto& Key : Legacy.keys()) if (Key.startsWith("text.lexer.")) Colors[Key] = QJsonValue::Null;
            textColors_[Mode] = Colors; refreshTextColorRows(); return;
        }
        if(List->currentRow()==3){epubSettings_->ResetDraft();for(auto I=controls_.begin();I!=controls_.end();++I)if(I.key().startsWith("documentViews/epubEditor/")){if(auto* Field=qobject_cast<QLineEdit*>(I.value()))Field->clear();else if(auto* Check=qobject_cast<QCheckBox*>(I.value()))Check->setChecked(false);}return;}
        if(List->currentRow()==4){const auto Original=base_;base_=values();settings::UnifiedConfig::Set(base_,{"documentViews","markdown"},QJsonObject{});loadControls();base_=Original;return;}
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
QWidget* DocumentSettingsDialog::createTextColorPage()
{
    auto* Page = new QWidget(this); auto* Layout = new QVBoxLayout(Page);
    Layout->setContentsMargins(0, 12, 8, 0); Layout->setSpacing(8);
    auto* Form = new QFormLayout; Layout->addLayout(Form);
    colorTheme_ = new QComboBox(Page); colorTheme_->setObjectName("textColorTheme");
    colorTheme_->addItem(tr("밝은 테마"), "light"); colorTheme_->addItem(tr("어두운 테마"), "dark");
    colorTheme_->setCurrentIndex(ThemeManager::instance().currentTheme() == ThemeManager::Dark ? 1 : 0);
    Form->addRow(tr("테마"), colorTheme_);
    colorLanguage_ = new QComboBox(Page); colorLanguage_->setObjectName("textColorLanguage");
    colorLanguage_->addItem(tr("공통 색상"), QString{});
    const auto& Registry = TextLexerRegistry::instance();
    for (const auto& Name : Registry.displayNames()) if (Registry.lexerKeyForDisplayName(Name) != "none") colorLanguage_->addItem(Name, Name);
    colorLanguage_->setMaxVisibleItems(20); Form->addRow(tr("언어 / 렉서"), colorLanguage_);
    auto* Help = new QLabel(tr("테마별로 저장됩니다. 언어 색상을 비워 두면 공통 구문 색상을 따릅니다."), Page); Help->setWordWrap(true); Layout->addWidget(Help);
    colorTable_ = new QTableWidget(Page); colorTable_->setObjectName("textColorTable"); colorTable_->setColumnCount(2);
    colorTable_->setHorizontalHeaderLabels({tr("항목"), tr("색상")}); colorTable_->verticalHeader()->hide();
    colorTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    colorTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    colorTable_->setSelectionMode(QAbstractItemView::NoSelection); colorTable_->setMinimumHeight(180);
    Layout->addWidget(colorTable_, 1);
    auto* PreviewLabel = new QLabel(tr("미리보기"), Page); Layout->addWidget(PreviewLabel);
    colorPreview_ = new ScintillaQtDirectBackend(Page, Page); colorPreview_->widget()->setObjectName("textColorPreview");
    colorPreview_->setEditorFont(QFont("Consolas", 11));
    auto PreviewSettings = ScintillaEditorSettings::standard(); PreviewSettings.lineNumberMarginDigits = 2;
    PreviewSettings.showCodeFolding = false; colorPreview_->applySettings(PreviewSettings);
    colorPreview_->widget()->setMinimumHeight(120); colorPreview_->widget()->setMaximumHeight(170);
    colorPreview_->setReadOnly(true); Layout->addWidget(colorPreview_->widget());
    connect(colorTheme_, &QComboBox::currentIndexChanged, this, &DocumentSettingsDialog::refreshTextColorRows);
    connect(colorLanguage_, &QComboBox::currentIndexChanged, this, &DocumentSettingsDialog::refreshTextColorRows);
    return Page;
}

void DocumentSettingsDialog::loadTextColors()
{
    textColors_ = settings::UnifiedConfig::Value(base_, {"documentViews", "textView", "colors"}).toObject();
    const auto Legacy = settings::UnifiedConfig::Decode(settings::UnifiedConfig::Value(base_, {"documentViews", "theme", "colorOverridesJson"})).toByteArray();
    legacyTextColors_ = QJsonDocument::fromJson(Legacy).object();
    refreshTextColorRows();
}

QColor DocumentSettingsDialog::textDraftColor(const QString& Key) const
{
    const auto Mode = colorTheme_->currentData().toString();
    const auto Colors = textColors_[Mode].toObject();
    const auto Value = Colors.value(Key);
    if (Value.isString() && QColor(Value.toString()).isValid()) return QColor(Value.toString());
    if (Value.isUndefined() && Key.startsWith("text.lexer.")) {
        const QColor Legacy(legacyTextColors_[Mode].toObject()[Key].toString()); if (Legacy.isValid()) return Legacy;
    }
    if (Key.startsWith("text.lexer.") && Key.count('.') > 2) {
        const auto* Profile = TextLexerRegistry::instance().profileForDisplayName(colorLanguage_->currentData().toString());
        if (Profile && Profile->languageId != Profile->lexerKey) {
            const auto SharedKey = QStringLiteral("text.lexer.%1.%2").arg(Profile->lexerKey, Key.section('.', -1));
            const auto Shared = Colors.value(SharedKey);
            if (Shared.isString() && QColor(Shared.toString()).isValid()) return QColor(Shared.toString());
            if (Shared.isUndefined()) { const QColor Legacy(legacyTextColors_[Mode].toObject()[SharedKey].toString()); if (Legacy.isValid()) return Legacy; }
        }
        return textDraftColor("text.lexer." + Key.section('.', -1));
    }
    return ThemeManager::defaultColors(Mode == "dark" ? ThemeManager::Dark : ThemeManager::Light).value(Key);
}

void DocumentSettingsDialog::refreshTextColorRows()
{
    if (!colorTable_) return;
    const auto Mode = colorTheme_->currentData().toString();
    const auto Language = colorLanguage_->currentData().toString();
    const auto* Profile = Language.isEmpty() ? nullptr : TextLexerRegistry::instance().profileForDisplayName(Language);
    QList<QPair<QString, QString>> Rows;
    if (!Profile) {
        for (const auto& Entry : ThemeManager::editableColorEntries())
            if (Entry.key.startsWith("text.") && Entry.key.count('.') <= 2) Rows.append({Entry.key, QString(Entry.label).remove("Text ").replace("편집기 배경", tr("본문 배경")).replace("Lexer ", tr("구문 · "))});
    } else {
        QSet<QString> Tokens; for (const auto& Token : Profile->styles) Tokens.insert(Token);
        for (const auto& Entry : ThemeManager::editableColorEntries()) if (Entry.key.startsWith("text.lexer.") && Entry.key.count('.') == 2) {
            const auto Token = Entry.key.section('.', -1);
            if (Tokens.isEmpty() || Tokens.contains(Token)) Rows.append({QStringLiteral("text.lexer.%1.%2").arg(Profile->languageId, Token), QString(Entry.label).remove("Lexer ")});
        }
    }
    // QTableWidget defers widget destruction; names must identify the current
    // fields even when the theme/language changes before the next event loop.
    for (auto* Edit : colorTable_->findChildren<QLineEdit*>()) Edit->setObjectName({});
    colorTable_->setRowCount(0); colorTable_->setRowCount(Rows.size());
    for (int Row = 0; Row < Rows.size(); ++Row) {
        const auto [Key, Label] = Rows[Row];
        auto* Item = new QTableWidgetItem(Label); Item->setFlags(Qt::ItemIsEnabled); colorTable_->setItem(Row, 0, Item);
        auto* Cell = new QWidget(colorTable_); auto* Line = new QHBoxLayout(Cell); Line->setContentsMargins(4, 2, 4, 2); Line->setSpacing(6);
        auto* Edit = new QLineEdit(Cell); Edit->setObjectName("textColor/" + Key); Edit->setAccessibleName(Label); Edit->setMaxLength(9); Edit->setClearButtonEnabled(true);
        const auto Value = textColors_[Mode].toObject().value(Key);
        if (Value.isString()) Edit->setText(Value.toString());
        else if (Value.isUndefined() && Key.startsWith("text.lexer.")) Edit->setText(legacyTextColors_[Mode].toObject()[Key].toString());
        Edit->setPlaceholderText(tr("기본값 %1").arg(textDraftColor(Key).name(QColor::HexRgb)));
        auto* Pick = new QPushButton(tr("선택…"), Cell); Pick->setAccessibleName(tr("%1 색상 선택").arg(Label));
        auto* Reset = new QPushButton(tr("기본값"), Cell); Reset->setAccessibleName(tr("%1 기본값").arg(Label));
        auto RefreshSwatch = [this, Key, Pick] {
            QPixmap Swatch(20, 16); Swatch.fill(textDraftColor(Key)); Pick->setIcon(QIcon(Swatch));
        };
        RefreshSwatch();
        connect(Edit, &QLineEdit::textChanged, this, [this, Edit, Key, Mode, RefreshSwatch] {
            auto Colors = textColors_[Mode].toObject(); const auto Value = Edit->text().trimmed();
            Colors[Key] = Value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(Value);
            textColors_[Mode] = Colors; RefreshSwatch(); refreshTextColorPreview();
        });
        connect(Pick, &QPushButton::clicked, this, [this, Key, Edit, Label] {
            const auto Color = QColorDialog::getColor(textDraftColor(Key), this, Label);
            if (Color.isValid()) Edit->setText(Color.name(QColor::HexRgb));
        });
        connect(Reset, &QPushButton::clicked, Edit, &QLineEdit::clear);
        Line->addWidget(Edit, 1); Line->addWidget(Pick); Line->addWidget(Reset);
        colorTable_->setCellWidget(Row, 1, Cell); colorTable_->setRowHeight(Row, qMax(36, Cell->sizeHint().height()));
    }
    refreshTextColorPreview();
}

void DocumentSettingsDialog::refreshTextColorPreview()
{
    if (!colorPreview_) return;
    const auto Language = colorLanguage_->currentData().toString();
    const auto* Profile = Language.isEmpty() ? nullptr : TextLexerRegistry::instance().profileForDisplayName(Language);
    const auto PreviewLanguage = Profile ? Language : QStringLiteral("Batch");
    const auto* PreviewProfile = TextLexerRegistry::instance().profileForDisplayName(PreviewLanguage);
    const auto Lexer = PreviewProfile ? PreviewProfile->lexerKey : QString{};
    QString Sample = "// comment\nconst value = \"sample\";\nreturn value + 42;\n";
    if (Lexer == "batch") Sample = "@echo off\nrem Build script\nSET Tools=%~dp0Tools\nif exist \"%Tools%\" echo Ready\n";
    else if (Lexer == "python") Sample = "# comment\ndef example(value=42):\n    return \"sample\", value\n";
    else if (Lexer == "xml" || Lexer == "hypertext") Sample = "<!-- comment -->\n<book title=\"sample\">\n  <chapter>Text</chapter>\n</book>\n";
    else if (Lexer == "json") Sample = "{\n  \"name\": \"sample\",\n  \"count\": 42, \"ready\": true\n}\n";
    else if (Lexer == "yaml") Sample = "# comment\nname: sample\ncount: 42\nready: true\n";
    colorPreview_->setReadOnly(false); colorPreview_->setText(Sample); colorPreview_->applyLanguage(PreviewLanguage);
    const auto TextSettings = settings::UnifiedConfig::Value(base_, {"documentViews", "textView"}).toObject();
    colorPreview_->setEditorFont(QFont(TextSettings["fontFamily"].toString("Consolas"), qBound(9, TextSettings["fontSize"].toInt(11), 16)));
    const auto SciColor = [](const QColor& C) { return qintptr(C.red() | (C.green() << 8) | (C.blue() << 16)); };
    colorPreview_->Send(SCI_STYLESETFORE, STYLE_DEFAULT, SciColor(textDraftColor("text.foreground")));
    colorPreview_->Send(SCI_STYLESETBACK, STYLE_DEFAULT, SciColor(textDraftColor("text.background")));
    colorPreview_->Send(SCI_STYLECLEARALL);
    if (PreviewProfile) for (auto It = PreviewProfile->styles.begin(); It != PreviewProfile->styles.end(); ++It) {
        const auto Key = Profile ? QStringLiteral("text.lexer.%1.%2").arg(Profile->languageId, It.value()) : "text.lexer." + It.value();
        colorPreview_->Send(SCI_STYLESETFORE, It.key(), SciColor(textDraftColor(Key)));
    }
    colorPreview_->Send(SCI_STYLESETFORE, STYLE_LINENUMBER, SciColor(textDraftColor("text.marginForeground")));
    colorPreview_->Send(SCI_STYLESETBACK, STYLE_LINENUMBER, SciColor(textDraftColor("text.marginBackground")));
    colorPreview_->Send(SCI_STYLESETFORE, STYLE_INDENTGUIDE, SciColor(textDraftColor("text.indentGuide")));
    colorPreview_->Send(SCI_SETFOLDMARGINCOLOUR, 1, SciColor(textDraftColor("text.marginBackground")));
    colorPreview_->Send(SCI_SETFOLDMARGINHICOLOUR, 1, SciColor(textDraftColor("text.marginBackground")));
    for (int Element : {SC_ELEMENT_SELECTION_BACK, SC_ELEMENT_SELECTION_INACTIVE_BACK}) {
        const auto Color = textDraftColor("text.selection");
        colorPreview_->Send(SCI_SETELEMENTCOLOUR, Element, SciColor(Color) | (qintptr(Color.alpha()) << 24));
    }
    for (int Element : {SC_ELEMENT_SELECTION_TEXT, SC_ELEMENT_SELECTION_INACTIVE_TEXT})
        colorPreview_->Send(SCI_SETELEMENTCOLOUR, Element, SciColor(textDraftColor("text.selectionForeground")) | (qintptr(255) << 24));
    colorPreview_->Send(SCI_SETCARETFORE, SciColor(textDraftColor("text.caret")));
    colorPreview_->Send(SCI_SETCARETLINEBACK, SciColor(textDraftColor("text.currentLine")));
    colorPreview_->Send(SCI_COLOURISE, 0, -1); colorPreview_->setReadOnly(true);
}

void DocumentSettingsDialog::loadControls()
{
    loadTextColors();
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
    settings::UnifiedConfig::Set(Result, {"documentViews", "textView", "colors"}, textColors_);
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
    const auto Problems = DocumentShortcutRegistry::Issues(settings::UnifiedConfig::Value(values(), {"documentViews", "shortcuts", "bindings"}).toObject());
    // Each problem sits under its row; the footer only counts them and names the first.
    QMap<QString, DocumentShortcutIssue> ById; QMap<QString, int> PerScope;
    for (const auto& Item : Problems) if (!ById.contains(Item.id_)) { ById[Item.id_] = Item; ++PerScope[DocumentShortcutRegistry::ScopeOf(Item.id_)]; }
    for (const auto& Command : DocumentShortcutRegistry::Commands()) {
        auto* Label = findChild<QLabel*>("shortcutIssue/" + Command.id_); auto* Jump = findChild<QPushButton*>("shortcutIssueJump/" + Command.id_); if (!Label || !Jump) continue;
        const auto Item = ById.value(Command.id_); Label->setText(Item.message_.isEmpty() ? QString{} : QStringLiteral("⚠ ") + Item.message_); Label->setVisible(!Item.message_.isEmpty());
        Jump->setProperty("otherId", Item.other_); Jump->setVisible(!Item.other_.isEmpty());
        if (!Item.other_.isEmpty()) for (const auto& Other : DocumentShortcutRegistry::Commands()) if (Other.id_ == Item.other_) Jump->setText(tr("%1 보기").arg(Other.label_));
    }
    if (auto* Scopes = findChild<QListWidget*>("shortcutScopes")) for (int Index = 0; Index < Scopes->count(); ++Index) Scopes->item(Index)->setData(Qt::UserRole + 2, PerScope.value(Scopes->item(Index)->data(Qt::UserRole).toString()));
    if (Problems.isEmpty()) error_->clear();
    else { const auto& First = Problems.first(); QString Label; for (const auto& Command : DocumentShortcutRegistry::Commands()) if (Command.id_ == First.id_) Label = Command.label_;
        error_->setText(tr("단축키 문제 %1건 · %2 › %3").arg(ById.size()).arg(DocumentShortcutRegistry::ScopeLabel(DocumentShortcutRegistry::ScopeOf(First.id_)), Label)); error_->setToolTip(First.message_); }
    auto* Buttons = findChild<QDialogButtonBox*>("settingsButtons");
    Buttons->button(QDialogButtonBox::Ok)->setEnabled(Problems.isEmpty()); Buttons->button(QDialogButtonBox::Apply)->setEnabled(Problems.isEmpty());
    emit ShortcutsValidated();
}
bool DocumentSettingsDialog::Apply(bool ResolveConflicts)
{
    if (!valid_) return false;
    const QRegularExpression ColorPattern(QStringLiteral("^#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
    for (const auto& Mode : {QString("light"), QString("dark")}) {
        const auto Colors = textColors_[Mode].toObject();
        for (auto It = Colors.begin(); It != Colors.end(); ++It) if (!It.value().isNull() && !ColorPattern.match(It.value().toString()).hasMatch()) {
            error_->setText(tr("색상은 #RRGGBB 또는 #AARRGGBB 형식으로 입력하세요. 비워 두면 기본 색상을 사용합니다.")); return false;
        }
    }
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
