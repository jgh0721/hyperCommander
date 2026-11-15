#include "stdafx.h"
#include "registryPolicyEditor.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/Button.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/ReorderList.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QScopedValueRollback>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicyEditor, "hc.ui.registryPolicyEditor")
namespace hc::registry {
RegistryPolicyEditor::RegistryPolicyEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.registry.policyEditor");
    layout_ = new QVBoxLayout(this); layout_->setContentsMargins(0, 0, 0, 0);
    error_ = new fm::ui::Label({}, fm::ui::Label::Help, this); error_->setWordWrap(true); error_->setTextFormat(Qt::PlainText); layout_->addWidget(error_);
    auto* Actions = new QHBoxLayout; layout_->addLayout(Actions);
    save_ = new fm::ui::Button(tr("저장"), this); save_->setObjectName("hc.registry.policy.save"); Actions->addWidget(save_);
    cancel_ = new fm::ui::Button(tr("취소"), this); Actions->addWidget(cancel_); Actions->addStretch(); layout_->addStretch();
    connect(save_, &QPushButton::clicked, this, [this] { if (save_->isEnabled()) emit SaveRequested(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { if (!busy_) Discard(); emit CancelRequested(); });
    changed();
}
bool RegistryPolicyEditor::SetPolicy(const RegistryPolicyEntry& Entry)
{
    if (busy_ || IsModified()) return false;
    if (loaded_ && entry_.policy.id == Entry.policy.id && entry_.machine == Entry.machine
        && entry_.policy.definition == Entry.policy.definition && entry_.policy.presentation == Entry.policy.presentation
        && entry_.policy.strings == Entry.policy.strings && entry_.localError == Entry.localError
        && entry_.observation.state == Entry.observation.state && entry_.observation.options == Entry.observation.options) {
        entry_ = Entry; return true;
    }
    entry_ = Entry; loaded_ = true; rebuild(); return true;
}
void RegistryPolicyEditor::rebuild()
{
    const QScopedValueRollback Loading(loading_, true); readers_.clear(); delete form_;
    form_ = new QWidget(this); layout_->insertWidget(0, form_); auto* Form = new QVBoxLayout(form_); Form->setContentsMargins(0, 0, 0, 0);
    states_ = new fm::ui::SegmentedControl({tr("구성되지 않음"), tr("사용"), tr("사용 안 함")}, form_);
    states_->setObjectName("hc.registry.policy.state");
    const auto OriginalState = entry_.observation.state;
    states_->setCurrentIndex(OriginalState == PolicyState::NotConfigured ? 0 : OriginalState == PolicyState::Enabled ? 1 : OriginalState == PolicyState::Disabled ? 2 : -1);
    setFocusProxy(states_->button(qMax(0, states_->currentIndex())));
    Form->addWidget(states_); connect(states_, &fm::ui::SegmentedControl::currentIndexChanged, this, [this] { changed(); });
    auto* Context = new fm::ui::Label(tr("로컬 정책을 저장한 뒤 정책 새로 고침 완료와 실제 값을 검증합니다. 되돌리기도 이전 정책 파일을 적용합니다. 사용자 정책은 로컬 GPO의 사용자 범위에 적용되며 다른 정책 원본이 있으면 충돌할 수 있습니다."), fm::ui::Label::Help, form_);
    Context->setWordWrap(true); Form->addWidget(Context);
    options_ = new QWidget(form_); Form->addWidget(options_); auto* Fields = new QVBoxLayout(options_); Fields->setContentsMargins(0, 0, 0, 0);
    const auto* Elements = entry_.policy.definition.Child("elements");
    QList<AdmxNode> Ordered;
    if (Elements) {
        QSet<QString> Added;
        for (const auto& Control : entry_.policy.presentation.children) {
            const auto Id = Control.Attribute("refId");
            if (Id.isEmpty() && !Control.text.trimmed().isEmpty()) {
                auto Help = Control; Help.tag = "presentationHelp"; Ordered.append(Help);
            }
            for (const auto& Element : Elements->children) if (!Id.isEmpty() && Element.Attribute("id") == Id && !Added.contains(Id)) { Ordered.append(Element); Added.insert(Id); }
        }
        for (const auto& Element : Elements->children) if (!Added.contains(Element.Attribute("id"))) Ordered.append(Element);
    }
    const auto Defaults = RegistryPolicyDefinition::Defaults(entry_.policy);
    for (const auto& Element : Ordered) {
        if (Element.tag == "presentationHelp") {
            auto* Help = new fm::ui::Label(entry_.policy.Text(Element.text.trimmed()), fm::ui::Label::Help, options_);
            Help->setWordWrap(true); Help->setTextFormat(Qt::PlainText); Fields->addWidget(Help); continue;
        }
        const auto Id = Element.Attribute("id"), Label = RegistryPolicyDefinition::OptionLabel(entry_.policy, Element);
        const auto Initial = entry_.observation.options.value(Id, Defaults.value(Id));
        if (Element.tag == "boolean") {
            auto* Input = new fm::ui::WrapCheckBox(Label, options_); Input->setObjectName("hc.policy." + Id); Input->setChecked(Initial.toBool()); Fields->addWidget(Input);
            readers_[Id] = [Input] { return Input->isChecked(); };
            connect(Input, &QCheckBox::toggled, this, [this] { changed(); }); continue;
        }
        auto* Heading = new fm::ui::Label(Label, fm::ui::Label::Help, options_); Heading->setWordWrap(true); Heading->setTextFormat(Qt::PlainText); Fields->addWidget(Heading);
        if (Element.tag == "enum") {
            auto* Input = new fm::ui::ChoiceBox(options_); Input->setObjectName("hc.policy." + Id);
            for (const auto& Item : Element.children) if (Item.tag == "item") Input->addItem(entry_.policy.Text(Item.Attribute("displayName")));
            Input->setCurrentIndex(Initial.toInt()); Fields->addWidget(Input); readers_[Id] = [Input] { return Input->currentIndex(); };
            connect(Input, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        } else if (Element.tag == "decimal" || Element.tag == "longDecimal") {
            auto* Input = new fm::ui::UnsignedNumberEdit(options_); Input->setObjectName("hc.policy." + Id); Input->setBitWidth(Element.tag == "decimal" ? 32 : 64); Input->setRadix(10); Input->setDraft(Initial.toString());
            Fields->addWidget(Input); readers_[Id] = [Input]() -> QVariant { const auto Number = Input->value(); return Number ? QString::number(*Number) : Input->draft(); };
            auto* Bounds = new fm::ui::Label(tr("범위: %1–%2").arg(Element.Attribute("minValue", "0"), Element.Attribute("maxValue", "9999")), fm::ui::Label::Help, options_); Fields->addWidget(Bounds);
            connect(Input, &fm::ui::UnsignedNumberEdit::draftChanged, this, [this] { changed(); });
        } else if (Element.tag == "multiText") {
            auto* Input = new fm::ui::MultilineField(options_); Input->setObjectName("hc.policy." + Id); Input->setPlainText(Initial.toString()); Input->setMinimumHeight(90); Fields->addWidget(Input);
            readers_[Id] = [Input] { return Input->toPlainText(); }; connect(Input, &QPlainTextEdit::textChanged, this, [this] { changed(); });
        } else if (Element.tag == "list") {
            auto* List = new fm::ui::ReorderListWidget(options_); List->setObjectName("hc.policy." + Id); List->setMinimumHeight(100); List->setMaximumHeight(200); Fields->addWidget(List);
            const bool Explicit = Element.Attribute("explicitValue") == "true" || Element.Attribute("explicitValue") == "1";
            auto* Row = new QHBoxLayout; Fields->addLayout(Row);
            auto* Name = new fm::ui::TextField(options_); Name->setPlaceholderText(tr("이름")); Name->setVisible(Explicit); Row->addWidget(Name);
            auto* Data = new fm::ui::TextField(options_); Data->setPlaceholderText(tr("값")); Row->addWidget(Data);
            auto* Add = new fm::ui::Button(tr("추가"), options_); Row->addWidget(Add);
            auto* Remove = new fm::ui::Button(tr("삭제"), options_); Row->addWidget(Remove);
            const auto Display = [Explicit](const QVariantMap& Entry) { return Explicit ? Entry.value("name").toString() + " = " + Entry.value("data").toString() : Entry.value("data").toString(); };
            for (const auto& Value : Initial.toList()) { auto* Item = new QListWidgetItem(Display(Value.toMap()), List); Item->setData(Qt::UserRole, Value); }
            readers_[Id] = [List]() -> QVariant { QVariantList Result; for (int I = 0; I < List->count(); ++I) Result.append(List->item(I)->data(Qt::UserRole)); return Result; };
            connect(List, &QListWidget::currentRowChanged, this, [List, Name, Data, Remove](int Index) {
                const QSignalBlocker BlockName(Name), BlockData(Data); const auto* Item = List->item(Index); const auto Entry = Item ? Item->data(Qt::UserRole).toMap() : QVariantMap{};
                Name->setText(Entry.value("name").toString()); Data->setText(Entry.value("data").toString()); Name->setEnabled(Item); Data->setEnabled(Item); Remove->setEnabled(Item);
            });
            const auto Edited = [this, List, Name, Data, Display] {
                if (auto* Item = List->currentItem()) { const QVariantMap Entry{{"name", Name->text()}, {"data", Data->text()}}; Item->setData(Qt::UserRole, Entry); Item->setText(Display(Entry)); changed(); }
            };
            connect(Name, &QLineEdit::textEdited, this, Edited); connect(Data, &QLineEdit::textEdited, this, Edited);
            connect(Add, &QPushButton::clicked, this, [this, List] { if (List->count() >= 65536) return; auto* Item = new QListWidgetItem(QString(), List); Item->setData(Qt::UserRole, QVariantMap{{"name", ""}, {"data", ""}}); List->setCurrentItem(Item); changed(); });
            connect(Remove, &QPushButton::clicked, this, [this, List] { delete List->takeItem(List->currentRow()); changed(); });
            connect(List->model(), &QAbstractItemModel::rowsMoved, this, [this] { changed(); });
            if (List->count()) List->setCurrentRow(0); else { Name->setEnabled(false); Data->setEnabled(false); Remove->setEnabled(false); }
        } else {
            auto* Input = new fm::ui::TextField(options_); Input->setObjectName("hc.policy." + Id);
            // Keep out-of-schema existing data intact; validation rejects saving
            // it until explicitly corrected instead of truncating it on display.
            Input->setMaxLength(int(qMax<qsizetype>(Initial.toString().size(), qMin<qulonglong>(Element.Attribute("maxLength", "1023").toULongLong(), 16 * 1024 * 1024))));
            const AdmxNode* Control = nullptr;
            for (const auto& Item : entry_.policy.presentation.children) if (Item.Attribute("refId") == Id) { Control = &Item; break; }
            if (Control && Control->tag == "comboBox") {
                auto* Choices = new fm::ui::ChoiceBox(options_); Choices->setObjectName("hc.policy.choices." + Id);
                Choices->setEditable(true); Choices->setLineEdit(Input); Choices->setInsertPolicy(QComboBox::NoInsert);
                QStringList Suggestions;
                for (const auto& Item : Control->children) if (Item.tag == "suggestion") Suggestions.append(entry_.policy.Text(Item.text));
                if (Control->Attribute("noSort") != "true" && Control->Attribute("noSort") != "1") Suggestions.sort(Qt::CaseInsensitive);
                Choices->addItems(Suggestions); Fields->addWidget(Choices);
            } else Fields->addWidget(Input);
            Input->setText(Initial.toString());
            readers_[Id] = [Input] { return Input->text(); }; connect(Input, &QLineEdit::textChanged, this, [this] { changed(); });
        }
    }
    originalOptions_ = Options(); loading_ = false; changed();
}
PolicyState RegistryPolicyEditor::State() const
{
    if (!states_) return PolicyState::Unknown;
    switch (states_->currentIndex()) { case 0: return PolicyState::NotConfigured; case 1: return PolicyState::Enabled; case 2: return PolicyState::Disabled; default: return entry_.observation.state; }
}
QVariantMap RegistryPolicyEditor::Options() const { QVariantMap Result; for (auto I = readers_.cbegin(); I != readers_.cend(); ++I) Result[I.key()] = I.value()(); return Result; }
bool RegistryPolicyEditor::IsModified() const { return loaded_ && (State() != entry_.observation.state || Options() != originalOptions_); }
void RegistryPolicyEditor::SetBusy(bool Busy) { busy_ = Busy; changed(); }
void RegistryPolicyEditor::ShowError(const QString& Error) { error_->setText(Error); }
void RegistryPolicyEditor::Discard() { if (!busy_ && loaded_) rebuild(); }
void RegistryPolicyEditor::changed()
{
    if (loading_) return;
    const auto Program = loaded_ ? RegistryPolicyDefinition::Compile(entry_.policy, State(), Options()) : PolicyProgram{};
    const auto Error = entry_.localError ? tr("Registry.pol을 읽을 수 없어 편집할 수 없습니다 (%1).").arg(entry_.localError) : Program.error;
    error_->setText(Error); if (form_) form_->setEnabled(!busy_ && !entry_.localError && entry_.policy.unsupportedReason.isEmpty());
    if (options_) options_->setEnabled(State() == PolicyState::Enabled);
    save_->setEnabled(loaded_ && !busy_ && IsModified() && Error.isEmpty()); cancel_->setEnabled(loaded_);
    emit ModifiedChanged(IsModified());
}
} // namespace hc::registry
