#include "stdafx.h"
#include "environmentDialogs.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <fmwidgets/ContentDialog.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/EnvironmentInputs.h>
#include <fmwidgets/Label.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QPushButton>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentDialogs, "hc.ui.environmentDialogs")
namespace hc::environment {
std::optional<Value> AskEnvironmentValue(QWidget* Parent, Scope Source)
{
    fm::ui::ContentDialog Dialog(Parent); Dialog.setObjectName("hc.environment.newDialog"); Dialog.setTitle(QObject::tr("새 환경 변수"));
    Dialog.setPrimaryButtonText(QObject::tr("미리 보기")); Dialog.setCloseButtonText(QObject::tr("취소"));
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body);
    auto* Name = new fm::ui::TextField(Body); Name->setObjectName("hc.environment.newName"); Name->setPlaceholderText(QObject::tr("변수 이름")); Name->setMaxLength(32767);
    auto* Type = new fm::ui::ChoiceBox(Body); Type->setObjectName("hc.environment.newType");
    Type->addItem(QObject::tr("문자열 (REG_SZ)"), quint32(REG_SZ)); Type->addItem(QObject::tr("확장 문자열 (REG_EXPAND_SZ)"), quint32(REG_EXPAND_SZ));
    auto* Text = new fm::ui::MaskedValueEdit(Body); Text->setObjectName("hc.environment.newValue");
    auto* Hint = new fm::ui::Label(QObject::tr("새 값은 누르는 동안 확인할 수 있습니다. 저장 전에 변경 내용을 다시 확인합니다."), fm::ui::Label::Help, Body);
    Layout->addWidget(Name); Layout->addWidget(Type); Layout->addWidget(Text); Layout->addWidget(Hint); Dialog.setContentWidget(Body);
    auto Draft = [&] { return Value{Name->text().toStdWString(), Text->text().toStdWString(), ValueType(Type->currentData().toUInt()), Source}; };
    const auto Validate = [&] { Dialog.button(fm::ui::ContentDialog::PrimaryButton)->setEnabled(!ValidateValue(Draft())); };
    QObject::connect(Name, &QLineEdit::textChanged, &Dialog, Validate); Validate();
    QTimer::singleShot(0, Name, [Name] { Name->setFocus(); });
    if (Dialog.exec() != fm::ui::ContentDialog::Primary) return {};
    auto Result = Draft(); if (ValidateValue(Result)) return {}; return Result;
}
}
