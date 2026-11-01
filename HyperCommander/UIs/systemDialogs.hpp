#pragma once

// 프로세스·서비스 대화 상자: 끝내기 확인, 새 작업 실행, 프로세스 속성, 서비스 속성.

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QVector>

#include <functional>

#include "UIs/dialogShell.hpp"
#include "commonLibs/system/processActions.hpp"
#include "commonLibs/system/serviceControl.hpp"

struct ProcessRef;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QTreeWidget;
class QSpinBox;
class QRadioButton;
class QPlainTextEdit;

namespace hc::ui {

struct TerminateChoice
{
    bool accepted    = false;
    bool graceful    = true;
    bool allowWarned = false;
};

// 끝낼 대상과 경고(중요·보호·제품 구성 요소)를 보이고 확인을 받는다. Tree면 하위부터 끝낸다는 안내를 붙인다.
TerminateChoice ConfirmTerminate( QWidget* Parent, const QVector<ProcessRef>& Targets, bool Tree, bool Restart = false );

// 새 작업 실행(F7). 실행하면 true와 요청을 돌려준다.
bool PromptRun( QWidget* Parent, hc::system::RunRequest& Request, const QString& InitialDirectory );

// 프로세스 속성(F3). 모델의 정보를 바탕으로 상세 정보를 다시 읽어 보인다.
void ShowProcessProperties( QWidget* Parent, const ProcessRef& Process, const QStringList& Services );

// 서비스 속성(F3). 바꾼 값은 Apply로 넘긴다(로컬 시도 후 관리자 도우미). Apply가 오류 문구를 돌려주면 창을 닫지 않는다.
using ServiceApply = std::function<QString( const hc::system::ServiceConfigChange& Change, const QJsonObject& Json )>;
using ServiceControlRequest = std::function<void( const QString& Control )>;
void ShowServiceProperties( QWidget* Parent, const QString& Name, ServiceApply Apply, ServiceControlRequest Control );

} // namespace hc::ui
