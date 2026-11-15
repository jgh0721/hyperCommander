#include "stdafx.h"
#include "registryEditors.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/ContentStack.h>
#include <fmwidgets/HexBufferEditor.h>
#include <fmwidgets/DataInterpreter.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryEditors, "hc.ui.registryEditors")

namespace hc::registry {
RegistryValueEditor::RegistryValueEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.registry.valueEditor");
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    heading_ = new fm::ui::Label({}, fm::ui::Label::Help, this); Layout->addWidget(heading_);
    pages_ = new fm::ui::ContentStack(this); Layout->addWidget(pages_, 1);
    text_ = new fm::ui::TextField(this); text_->setObjectName("hc.registry.text"); text_->setMonospace(true); text_->setMaxLength(int(WinRegistryBackend::MaxValueBytes / 2));
    multi_ = new fm::ui::MultilineField(this); multi_->setObjectName("hc.registry.multi");
    number_ = new fm::ui::UnsignedNumberEdit(this); number_->setObjectName("hc.registry.number");
    auto* BinaryPage = new QWidget(this); auto* BinaryLayout = new QVBoxLayout(BinaryPage); BinaryLayout->setContentsMargins(0, 0, 0, 0);
    hex_ = new fm::ui::HexBufferEditor(BinaryPage); hex_->setObjectName("hc.registry.hex"); BinaryLayout->addWidget(hex_, 1);
    auto* Mode = new fm::ui::Label(tr("16진 숫자를 입력 · Ins: 삽입 / 덮어쓰기 · 현재 덮어쓰기"), fm::ui::Label::Help, BinaryPage); BinaryLayout->addWidget(Mode);
    connect(hex_, &fm::ui::HexBufferEditor::insertModeChanged, Mode, [Mode](bool Insert) { Mode->setText(Insert ? tr("16진 숫자를 입력 · Ins: 삽입 / 덮어쓰기 · 현재 삽입") : tr("16진 숫자를 입력 · Ins: 삽입 / 덮어쓰기 · 현재 덮어쓰기")); });
    interpreter_ = new fm::ui::DataInterpreter(BinaryPage); BinaryLayout->addWidget(interpreter_);
    path_ = new hc::ui::PathListEditor(this);
    // A stack gives its page the full viewport: wrap compact controls so they
    // retain their normal height instead of becoming a tall single-line field.
    const auto CompactPage = [this](QWidget* Input) {
        auto* Page = new QWidget(pages_); auto* Layout = new QVBoxLayout(Page);
        Layout->setContentsMargins(0, 0, 0, 0); Layout->addWidget(Input); Layout->addStretch();
        Page->setFocusProxy(Input);
        return Page;
    };
    for (QWidget* PageWidget : std::initializer_list<QWidget*>{CompactPage(text_), multi_, CompactPage(number_), BinaryPage, path_})
        pages_->addWidget(PageWidget);
    expanded_ = new fm::ui::Label({}, fm::ui::Label::Help, this); expanded_->setTextFormat(Qt::PlainText); expanded_->setWordWrap(true); Layout->addWidget(expanded_);
    warning_ = new fm::ui::Label({}, fm::ui::Label::Help, this); warning_->setTextFormat(Qt::PlainText); warning_->setWordWrap(true); Layout->addWidget(warning_);
    auto* Actions = new QHBoxLayout; Layout->addLayout(Actions);
    save_ = new fm::ui::Button(tr("저장"), this); save_->setObjectName("hc.registry.save");
    cancel_ = new fm::ui::Button(tr("취소"), this); cancel_->setObjectName("hc.registry.cancel");
    Actions->addWidget(save_); Actions->addWidget(cancel_); Actions->addStretch();
    connect(text_, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(multi_, &QPlainTextEdit::textChanged, this, [this] { changed(); });
    connect(number_, &fm::ui::UnsignedNumberEdit::draftChanged, this, [this] { changed(); });
    connect(hex_, &fm::ui::HexBufferEditor::dataChanged, this, [this] { interpreter_->setData(hex_->data(), hex_->cursorOffset()); changed(); });
    connect(hex_, &fm::ui::HexBufferEditor::cursorChanged, this, [this](qsizetype Offset) { interpreter_->setData(hex_->data(), Offset); });
    connect(hex_, &fm::ui::HexBufferEditor::editRejected, this, &RegistryValueEditor::ShowError);
    connect(path_, &hc::ui::PathListEditor::ModifiedChanged, this, [this] { changed(); });
    connect(save_, &QPushButton::clicked, this, [this] {
        const auto Value = Draft(); if (busy_ || !IsModified() || !Value) return;
        emit SaveRequested(original_, Value.value);
    });
    connect(cancel_, &QPushButton::clicked, this, [this] { if (!busy_) { Discard(); emit CancelRequested(); } });
    SetBusy(false);
}
bool RegistryValueEditor::SetValue(const RegistryValue& Value, bool PathValue)
{
    if (busy_ || IsModified() || Value.bytes.size() > fm::ui::HexBufferEditor::MaximumBytes) return false;
    const QScopedValueRollback Loading(loading_, true);
    original_ = Value; loaded_ = true; pathValue_ = PathValue;
    const auto TextValue = RegistryValueCodec::DecodeText(Value);
    const auto NumberValue = RegistryValueCodec::DecodeNumber(Value);
    QString Warning;
    if ((Value.type == REG_SZ || Value.type == REG_EXPAND_SZ || Value.type == REG_MULTI_SZ) && TextValue.lossless) {
        if (PathValue && Value.type != REG_MULTI_SZ && path_->SetValue(TextValue.text)) page_ = Path;
        else if (Value.type == REG_MULTI_SZ) { page_ = Multi; multi_->setPlainText(TextValue.text); }
        else { page_ = Text; text_->setText(TextValue.text); }
    } else if (NumberValue) {
        page_ = Number; number_->setBitWidth(Value.type == REG_QWORD ? 64 : 32); number_->setValue(NumberValue.value);
    } else {
        page_ = Binary; hex_->setData(Value.bytes); interpreter_->setData(Value.bytes);
        Warning = Value.Warning();
        if (Warning.isEmpty() && (Value.type == REG_SZ || Value.type == REG_EXPAND_SZ || Value.type == REG_MULTI_SZ)) Warning = TextValue.warning;
    }
    heading_->setText(tr("%1 · 형식 %2 · %3바이트").arg(Value.name.isEmpty() ? tr("(기본값)") : Value.name).arg(Value.type).arg(Value.bytes.size()));
    warning_->setText(Warning); warning_->setVisible(!Warning.isEmpty());
    expanded_->clear(); expanded_->setVisible(Value.type == REG_EXPAND_SZ && page_ == Text);
    pages_->setCurrentIndex(page_); save_->setEnabled(false); cancel_->setEnabled(true);
    emit ModifiedChanged(false);
    if (Value.type == REG_EXPAND_SZ && page_ == Text) emit ExpansionRequested(text_->text());
    return true;
}
RegistryResult<RegistryValue> RegistryValueEditor::Draft() const
{
    if (!loaded_) return {{}, ERROR_INVALID_STATE};
    RegistryResult<QByteArray> Encoded;
    switch (page_) {
    case Text: Encoded = RegistryValueCodec::EncodeText(original_.type, text_->text()); break;
    case Multi: Encoded = RegistryValueCodec::EncodeText(original_.type, multi_->toPlainText()); break;
    case Number: {
        const auto Value = number_->value(); if (!Value) return {{}, ERROR_INVALID_DATA};
        Encoded = RegistryValueCodec::EncodeNumber(original_.type, *Value); break;
    }
    case Binary: Encoded = {hex_->data()}; break;
    case Path: Encoded = RegistryValueCodec::EncodeText(original_.type, path_->Value()); break;
    }
    if (!Encoded) return {{}, Encoded.error};
    return {{original_.name, original_.type, Encoded.value}};
}
bool RegistryValueEditor::IsModified() const
{
    if (!loaded_ || loading_) return false;
    const auto Value = Draft(); return !Value || Value.value.bytes != original_.bytes;
}
void RegistryValueEditor::SetBusy(bool Busy)
{
    busy_ = Busy; pages_->setEnabled(!Busy); cancel_->setEnabled(loaded_ && !Busy);
    save_->setEnabled(!Busy && IsModified() && bool(Draft()));
}
void RegistryValueEditor::SetPathResolver(hc::ui::PathResolver Resolver) { path_->SetResolver(std::move(Resolver)); }
void RegistryValueEditor::SetExpandedText(const QString& Expanded, const QString& Warning)
{
    expanded_->setText(Warning.isEmpty() ? tr("→ %1").arg(Expanded) : Warning);
}
void RegistryValueEditor::Discard()
{
    if (busy_ || !loaded_) return;
    const auto Original = original_; loaded_ = false; SetValue(Original, pathValue_);
}
void RegistryValueEditor::AcceptSaved(const RegistryValue& Saved)
{
    busy_ = false; loaded_ = false; SetValue(Saved, pathValue_); SetBusy(false);
}
void RegistryValueEditor::ShowError(const QString& Error) { warning_->setText(Error); warning_->setVisible(!Error.isEmpty()); }
void RegistryValueEditor::FocusEditor()
{
    if (page_ == Number) number_->editor()->setFocus();
    else if (page_ == Binary) hex_->setFocus();
    else pages_->currentWidget()->setFocus();
}
void RegistryValueEditor::changed()
{
    if (loading_) return;
    save_->setEnabled(!busy_ && IsModified() && bool(Draft())); emit ModifiedChanged(IsModified());
    if (original_.type == REG_EXPAND_SZ && page_ == Text) emit ExpansionRequested(text_->text());
}
}
