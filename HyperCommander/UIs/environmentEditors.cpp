#include "stdafx.h"
#include "environmentEditors.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/system/environmentModel.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/EnvironmentInputs.h>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentEditors, "hc.environment.editors")
namespace hc::environment {
EnvironmentValueEditor::EnvironmentValueEditor(QWidget* Parent) : QWidget(Parent)
{
    setObjectName("hc.environment.valueEditor");
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    type_ = new fm::ui::ChoiceBox(this); type_->setObjectName("hc.environment.valueType");
    type_->addItems({"REG_SZ", "REG_EXPAND_SZ"}); Layout->addWidget(type_);
    pages_ = new QStackedWidget(this); Layout->addWidget(pages_, 1);
    text_ = new fm::ui::TextField(this); text_->setMaxLength(32766); text_->setMonospace(true); text_->setObjectName("hc.environment.text");
    folder_ = new fm::ui::PathEdit(this); folder_->setHistoryVisible(false); folder_->lineEdit()->setMaxLength(32766); folder_->setObjectName("hc.environment.folder");
    expanded_ = new fm::ui::ExpandableValueEdit(this); expanded_->setMaxLength(32766); expanded_->setObjectName("hc.environment.expanded");
    list_ = new hc::ui::PathListEditor(this); list_->setObjectName("hc.environment.pathList");
    tokens_ = new fm::ui::OrderedTokenEdit(this); tokens_->setObjectName("hc.environment.tokens");
    known_ = new fm::ui::ChoiceBox(this); known_->setEditable(true); known_->setObjectName("hc.environment.known");
    masked_ = new fm::ui::MaskedValueEdit(this); masked_->editor()->setMaxLength(32766); masked_->setObjectName("hc.environment.masked");
    for (QWidget* Page : std::initializer_list<QWidget*>{text_, folder_, expanded_, list_, tokens_, known_, masked_}) pages_->addWidget(Page);
    folderStatus_ = new fm::ui::Label({}, fm::ui::Label::Help, this); folderStatus_->setWordWrap(true);
    folderStatus_->setObjectName("hc.environment.folderStatus"); Layout->addWidget(folderStatus_); folderStatus_->hide();
    folderTimer_.setSingleShot(true); folderTimer_.setInterval(180);
    connect(&folderTimer_, &QTimer::timeout, this, &EnvironmentValueEditor::resolveFolder);
    message_ = new fm::ui::Label({}, fm::ui::Label::Help, this); message_->setWordWrap(true); Layout->addWidget(message_);
    auto* Actions = new QHBoxLayout; Layout->addLayout(Actions);
    save_ = new fm::ui::Button(tr("저장"), this); save_->setObjectName("hc.environment.save"); Actions->addWidget(save_);
    auto* DiscardButton = new fm::ui::Button(tr("초안 버리기"), this); DiscardButton->setObjectName("hc.environment.discard"); Actions->addWidget(DiscardButton); Actions->addStretch();
    connect(save_, &QPushButton::clicked, this, [this] { if (!busy_ && !readOnly_ && IsModified() && !ValidateValue(Draft())) emit SaveRequested(original_, Draft()); });
    connect(DiscardButton, &QPushButton::clicked, this, [this] { if (!busy_) { Discard(); emit CancelRequested(); } });
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(text_, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(folder_, &fm::ui::PathEdit::pathChanged, this, [this] { changed(); });
    connect(expanded_, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(expanded_, &fm::ui::ExpandableValueEdit::expansionRequested, this, &EnvironmentValueEditor::ExpansionRequested);
    connect(list_, &hc::ui::PathListEditor::ModifiedChanged, this, [this] { changed(); });
    connect(tokens_, &fm::ui::OrderedTokenEdit::valueChanged, this, [this] { changed(); });
    connect(known_, &QComboBox::currentTextChanged, this, [this] { changed(); });
    connect(masked_, &fm::ui::MaskedValueEdit::textChanged, this, [this] { changed(); });
    changed();
}
bool EnvironmentValueEditor::SetValue(const Value& Original, bool ReadOnly, bool Hide, const QStringList& KnownValues, bool FolderHint)
{
    if (busy_ || IsModified() || ValidateValue(Original)) return false;
    QScopedValueRollback Guard(loading_, true);
    ++folderGeneration_; folderTimer_.stop(); folderStatus_->clear();
    original_ = Original; loaded_ = true; readOnly_ = ReadOnly;
    // Clear inactive fields so changing selection cannot expose an earlier value.
    text_->clear(); folder_->setPath({}); expanded_->clear(); list_->SetValue({}); tokens_->setValue({}); masked_->setText({}); known_->clear();
    if (Hide || SensitiveName(Original.name_)) page_ = Masked;
    else if (SameName(Original.name_, L"PATHEXT")) page_ = Tokens;
    else if (EnvironmentModel::ListVariable(Original.name_)) page_ = List;
    else if (!KnownValues.empty()) { page_ = Known; known_->addItems(KnownValues); }
    else if (FolderHint || SameName(Original.name_, L"TEMP") || SameName(Original.name_, L"TMP") || SameName(Original.name_, L"JAVA_HOME") ||
        SameName(Original.name_, L"PYTHONHOME") || SameName(Original.name_, L"ANDROID_HOME") || SameName(Original.name_, L"CARGO_HOME") ||
        SameName(Original.name_, L"RUSTUP_HOME") || SameName(Original.name_, L"DOTNET_ROOT")) page_ = Folder;
    else if (Original.type_ == ValueType::ExpandString) page_ = Expanded;
    else page_ = Text;
    folderStatus_->setVisible(page_ == Folder);
    pages_->setCurrentIndex(page_); setDraft(QString::fromStdWString(Original.raw_));
    type_->setCurrentIndex(Original.type_ == ValueType::ExpandString ? 1 : 0);
    type_->setEnabled(!readOnly_); pages_->setEnabled(!readOnly_ || page_ == Masked); masked_->editor()->setReadOnly(readOnly_);
    message_->setText(readOnly_ ? tr("이 범위는 읽기 전용입니다. 원본을 따라가서 편집하세요.") : QString());
    save_->setEnabled(false); emit ModifiedChanged(false); return true;
}
void EnvironmentValueEditor::setDraft(const QString& Raw)
{
    switch (page_) {
    case Text: text_->setText(Raw); break; case Folder: folder_->setPath(Raw); break;
    case Expanded: expanded_->setText(Raw); break; case List: list_->SetValue(Raw); break;
    case Tokens: tokens_->setValue(Raw); break; case Known: known_->setCurrentText(Raw); break;
    case Masked: masked_->setText(Raw); break;
    }
}
Value EnvironmentValueEditor::Draft() const
{
    auto Result = original_; QString Raw;
    switch (page_) {
    case Text: Raw = text_->text(); break; case Folder: Raw = folder_->path(); break;
    case Expanded: Raw = expanded_->text(); break; case List: Raw = list_->Value(); break;
    case Tokens: Raw = tokens_->value(); break; case Known: Raw = known_->currentText(); break;
    case Masked: Raw = masked_->text(); break;
    }
    Result.raw_ = Raw.toStdWString(); Result.type_ = type_->currentIndex() == 1 ? ValueType::ExpandString : ValueType::String; return Result;
}
bool EnvironmentValueEditor::IsModified() const { return loaded_ && Draft() != original_; }
void EnvironmentValueEditor::changed()
{
    if (loading_) return;
    const bool Modified = IsModified();
    save_->setEnabled(loaded_ && Modified && !readOnly_ && !busy_ && !ValidateValue(Draft()));
    if (page_ == Folder && resolver_) { ++folderGeneration_; folderTimer_.start(); }
    emit ModifiedChanged(Modified);
}
void EnvironmentValueEditor::SetBusy(bool Busy) { busy_ = Busy; pages_->setEnabled(!Busy && (!readOnly_ || page_ == Masked)); type_->setEnabled(!Busy && !readOnly_); changed(); }
void EnvironmentValueEditor::SetVariableNames(const QStringList& Names) { expanded_->setVariableNames(Names); }
void EnvironmentValueEditor::SetResolver(hc::ui::PathResolver Resolver)
{
    resolver_ = Resolver; list_->SetResolver(std::move(Resolver));
    if (page_ == Folder) { ++folderGeneration_; folderTimer_.start(); }
}
void EnvironmentValueEditor::resolveFolder()
{
    if (page_ != Folder || !resolver_) return;
    const auto Generation = ++folderGeneration_; const auto Raw = folder_->path(); const auto Resolve = resolver_;
    auto* Watcher = new QFutureWatcher<hc::ui::PathResolution>(this);
    connect(Watcher, &QFutureWatcher<hc::ui::PathResolution>::finished, this, [this, Watcher, Generation] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (Generation != folderGeneration_ || page_ != Folder) return;
        const auto State = Result.status == hc::ui::PathResolution::Status::Exists ? tr("폴더 확인")
            : Result.status == hc::ui::PathResolution::Status::Missing ? tr("폴더 없음") : tr("확인하지 못함");
        folderStatus_->setText(State + " · " + Result.expanded + (Result.warning.isEmpty() ? QString() : "\n" + Result.warning));
    });
    Watcher->setFuture(QtConcurrent::run([Resolve, Raw] { return Resolve(Raw); }));
}
void EnvironmentValueEditor::Discard()
{
    if (!loaded_ || busy_) return;
    { QScopedValueRollback Guard(loading_, true); setDraft(QString::fromStdWString(original_.raw_)); type_->setCurrentIndex(original_.type_ == ValueType::ExpandString ? 1 : 0); }
    message_->clear(); changed();
}
void EnvironmentValueEditor::AcceptSaved(const Value& Saved) { original_ = Saved; busy_ = false; loaded_ = true; SetBusy(false); changed(); }
void EnvironmentValueEditor::FocusEditor() { pages_->currentWidget()->setFocus(Qt::ShortcutFocusReason); }
void EnvironmentValueEditor::ShowError(const QString& Error) { message_->setText(Error); }
}
