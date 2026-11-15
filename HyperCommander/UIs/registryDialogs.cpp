#include "stdafx.h"
#include "registryDialogs.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Label.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QPushButton>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryDialogs, "hc.ui.registryDialogs")
using namespace hc::registry;
namespace hc::ui {
std::optional<QString> AskRegistryName(QWidget* Parent, const QString& Title, const QString& Initial, bool Key)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.registry.nameDialog"); Dialog.setTitle(Title);
    Dialog.setPrimaryButtonText(QObject::tr("확인")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body); Layout->setContentsMargins(0, 0, 0, 0);
    auto* Field = new fm::ui::TextField(Body); Field->setObjectName("hc.registry.newName"); Field->setMaxLength(Key ? 255 : 16383); Field->setText(Initial);
    auto* Error = new fm::ui::Label({}, fm::ui::Label::Help, Body); Error->setWordWrap(true);
    Layout->addWidget(Field); Layout->addWidget(Error); Dialog.setContentWidget(Body);
    const auto Validate = [&] {
        const bool Valid = WinRegistryBackend::ValidName(Field->text(), Key);
        Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(Valid);
        Error->setText(Valid ? QString() : QObject::tr("사용할 수 없는 이름입니다. 키 이름은 비어 있거나 역슬래시를 포함할 수 없습니다."));
    };
    QObject::connect(Field, &QLineEdit::textChanged, &Dialog, Validate); Validate();
    QTimer::singleShot(0, Field, [Field] { Field->setFocus(); Field->selectAll(); });
    return Dialog.exec() == fm::ui::ContentDialog::Primary ? std::optional<QString>(Field->text()) : std::nullopt;
}
std::optional<RegistryValue> AskRegistryNewValue(QWidget* Parent)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.registry.newValueDialog"); Dialog.setTitle(QObject::tr("새 값"));
    Dialog.setPrimaryButtonText(QObject::tr("만들기")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body); Layout->setContentsMargins(0, 0, 0, 0);
    auto* Name = new fm::ui::TextField(Body); Name->setObjectName("hc.registry.newName"); Name->setMaxLength(16383); Name->setPlaceholderText(QObject::tr("이름 · 비워 두면 기본값"));
    auto* Type = new fm::ui::ChoiceBox(Body); Type->setObjectName("hc.registry.newType");
    for (const auto& Item : QList<QPair<QString, quint32>>{{QObject::tr("문자열 (REG_SZ)"), REG_SZ}, {QObject::tr("DWORD (32비트)"), REG_DWORD},
            {QObject::tr("QWORD (64비트)"), REG_QWORD}, {QObject::tr("다중 문자열 (REG_MULTI_SZ)"), REG_MULTI_SZ},
            {QObject::tr("확장 문자열 (REG_EXPAND_SZ)"), REG_EXPAND_SZ}, {QObject::tr("이진 (REG_BINARY)"), REG_BINARY}}) Type->addItem(Item.first, Item.second);
    Layout->addWidget(Name); Layout->addWidget(Type); Dialog.setContentWidget(Body);
    QObject::connect(Name, &QLineEdit::textChanged, &Dialog, [&] { Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(WinRegistryBackend::ValidName(Name->text(), false)); });
    QTimer::singleShot(0, Name, [Name] { Name->setFocus(); });
    if (Dialog.exec() != fm::ui::ContentDialog::Primary) return {};
    RegistryValue Result{Name->text(), Type->currentData().toUInt(), {}};
    if (Result.type == REG_DWORD || Result.type == REG_QWORD) Result.bytes = RegistryValueCodec::EncodeNumber(Result.type, 0).value;
    else if (Result.type != REG_BINARY) Result.bytes = RegistryValueCodec::EncodeText(Result.type, {}).value;
    return Result;
}
bool ConfirmRegistryDelete(QWidget* Parent, const QStringList& Names)
{
    if (Names.isEmpty()) return false;
    const auto Listed = Names.mid(0, 12).join('\n');
    const auto Text = QObject::tr("다음 %1개 항목을 삭제할까요? 키를 삭제하면 하위 키와 값도 삭제합니다.\n\n%2%3\n\n삭제 전에 registry-backup에 원본과 .reg 백업을 저장합니다.")
        .arg(Names.size()).arg(Listed, Names.size() > 12 ? QObject::tr("\n…") : QString());
    return fm::ui::ContentDialog::ask(Parent, QObject::tr("레지스트리 삭제"), Text, QObject::tr("삭제"), {}, QObject::tr("취소"), fm::ui::ContentDialog::CloseButton) == fm::ui::ContentDialog::Primary;
}
}
