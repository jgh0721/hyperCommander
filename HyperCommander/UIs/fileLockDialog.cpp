#include "stdafx.h"
#include "fileLockDialog.hpp"

/// Project headers
#include "UniqueLibs/permissionService.hpp"
#include <fmwidgets/Banner.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/DialogCards.h>

/// Platform headers
#include <QTreeWidget>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QHeaderView>
#include <QLabel>
#include <QCheckBox>
#include <QClipboard>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QtConcurrent>

namespace hc::ui {
namespace {
QString referenceName(const QString& Kind) {
    if (Kind == "handle") return QObject::tr("파일 핸들 확인");
    if (Kind == "image") return QObject::tr("실행 이미지");
    if (Kind == "module") return QObject::tr("로드한 모듈");
    return QObject::tr("프로세스 확인");
}
QLabel* label(const QString& Text, QWidget* Parent) {
    auto* Value = new QLabel(Text, Parent); Value->setTextFormat(Qt::PlainText); Value->setWordWrap(true);
    Value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); return Value;
}
}
FileLockDialog::FileLockDialog(FileLockDialogRequest Request, QWidget* Parent, ClientFactory Factory)
    : DialogShell(tr("파일 사용 정보"), Parent), request_(std::move(Request)), factory_(std::move(Factory)) {
    setObjectName("hc.fileLocks.dialog");
    SetSummary(request_.source_); EnableCompactLayout(1000); setMinimumWidth(720); setMinimumHeight(580); resize(1000,740);
    if (!factory_) factory_ = [](const QStringList& Paths, bool Elevated) { return std::make_shared<filelocks::FileLockClient>(Paths, Elevated); };
    auto* Body = BodyLayout();
    auto* Context = new fm::ui::KeyValueCard(this);
    Context->addRow(tr("대상 파일"), QFileInfo(request_.source_).fileName(), fm::ui::KeyValueCard::Strong);
    Context->addRow(tr("현재 위치"), QDir::toNativeSeparators(QFileInfo(request_.source_).absolutePath()), fm::ui::KeyValueCard::Mono);
    if (!request_.destination_.isEmpty()) Context->addRow(tr("작업 대상"), QDir::toNativeSeparators(request_.destination_), fm::ui::KeyValueCard::Mono);
    Context->addRow(tr("작업 오류"), request_.reason_); Body->addWidget(Context);
    banner_ = new fm::ui::Banner(this); banner_->setObjectName("hc.fileLocks.banner"); Body->addWidget(banner_);
    auto* Toolbar = new QHBoxLayout;
    summary_ = label({},this); Toolbar->addWidget(summary_,1);
    refresh_ = new fm::ui::Button(tr("새로고침(&R)"),this); refresh_->setObjectName("hc.fileLocks.refresh"); Toolbar->addWidget(refresh_);
    elevate_ = new fm::ui::Button(tr("관리자 권한으로 다시 조회(&A)"),this); elevate_->setObjectName("hc.fileLocks.elevate");
    static_cast<fm::ui::Button*>(elevate_)->setGlyph(fm::ui::glyph::Shield); Toolbar->addWidget(elevate_);
    stop_ = new fm::ui::Button(tr("조회 취소"),this); stop_->setObjectName("hc.fileLocks.stop"); Toolbar->addWidget(stop_); Body->addLayout(Toolbar);
    table_ = new QTreeWidget(this); table_->setObjectName("hc.fileLocks.references");
    table_->setHeaderLabels({tr("프로그램 / 참조"),tr("PID"),tr("사용자"),tr("확인 범위"),tr("상태")});
    table_->setSelectionMode(QAbstractItemView::SingleSelection); table_->setAlternatingRowColors(true); table_->setMinimumHeight(145);
    table_->setColumnWidth(0,300); table_->setColumnWidth(1,80); table_->setColumnWidth(2,170); table_->setColumnWidth(3,150);
    table_->header()->setStretchLastSection(true); Body->addWidget(table_,1);
    details_ = new fm::ui::KeyValueCard(this); details_->setObjectName("hc.fileLocks.details"); Body->addWidget(details_);
    restriction_ = label(tr("사용 중이라는 정보만으로 삭제·이름 변경 차단 여부를 확정할 수 없습니다."),this);
    restriction_->setObjectName("hc.fileLocks.restriction"); Body->addWidget(restriction_);
    auto* Actions = new QHBoxLayout;
    auto* Explain = new fm::ui::Button(tr("직접 닫기 안내"),this); Actions->addWidget(Explain);
    close_ = new fm::ui::Button(tr("선택 핸들 강제 닫기…"),this); close_->setObjectName("hc.fileLocks.closeHandle"); Actions->addWidget(close_);
    terminate_ = new fm::ui::Button(tr("프로세스 종료…"),this); terminate_->setObjectName("hc.fileLocks.terminate"); Actions->addWidget(terminate_);
    auto* Copy = new fm::ui::Button(tr("경로 복사"),this); Actions->addWidget(Copy); Actions->addStretch(); Body->addLayout(Actions);
    AcceptButton()->setText(tr("다시 시도(&T)")); AcceptButton()->setObjectName("hc.fileLocks.retry");
    CancelButton()->setText(request_.canRetry_ ? tr("작업 취소(&C)") : tr("닫기(&C)"));
    CancelButton()->setObjectName("hc.fileLocks.cancel");
    auto* Footer = qobject_cast<QHBoxLayout*>(AcceptButton()->parentWidget()->layout());
    skip_ = new fm::ui::Button(tr("건너뛰기(&S)"),this); skip_->setObjectName("hc.fileLocks.skip");
    Footer->insertWidget(1,skip_); skip_->setVisible(request_.canSkip_);
    connect(skip_,&QPushButton::clicked,this,[this]{if(current()){reply_=Reply::Skip; done(2);}});
    connect(CancelButton(),&QPushButton::clicked,this,[this]{if(request_.canRetry_)reply_=Reply::CancelJob;});
    connect(refresh_,&QPushButton::clicked,this,[this]{query(elevated_);});
    connect(elevate_,&QPushButton::clicked,this,[this]{query(true);});
    connect(stop_,&QPushButton::clicked,this,[this]{if(client_)client_->Cancel();});
    connect(close_,&QPushButton::clicked,this,[this]{act("close");});
    connect(terminate_,&QPushButton::clicked,this,[this]{act("terminate");});
    connect(Explain,&QPushButton::clicked,this,[this]{QMessageBox::information(this,tr("직접 닫기 안내"),tr("해당 프로그램에서 파일을 저장하고 정상적으로 닫은 뒤 다시 시도를 선택하세요."));});
    connect(Copy,&QPushButton::clicked,this,[this]{
        auto* Item=table_->currentItem();const int Index=Item?Item->data(0,Qt::UserRole+1).toInt():-1;
        QApplication::clipboard()->setText(Index>=0&&Index<rows_.size()?rows_[Index].toObject()["path"].toString():request_.source_);
    });
    connect(table_,&QTreeWidget::currentItemChanged,this,[this]{updateDetails();});
    auto* Check = new QTimer(this); Check->setInterval(250);
    connect(Check,&QTimer::timeout,this,[this]{
        if (!requiresRefresh_ && !queried_.isNull() && queried_.msecsTo(QDateTime::currentDateTimeUtc()) >= 120000) {
            requiresRefresh_ = true; updateDetails();
            banner_->setTone(fm::ui::Banner::Warn); banner_->setText(tr("조회 유효 시간이 지났습니다. 강제 동작 전에 새로고침하세요."));
        }
        if(!current()) { if(client_)client_->Cancel(); AcceptButton()->setEnabled(false); skip_->setEnabled(false); close_->setEnabled(false); terminate_->setEnabled(false); }
    }); Check->start();
    QTimer::singleShot(0,this,[this]{query(false);});
}
FileLockDialog::~FileLockDialog() { if(client_)client_->Cancel(); }
bool FileLockDialog::current() const { return !request_.current_ || request_.current_(); }
void FileLockDialog::accept() {
    if (request_.canRetry_ && current() && !busy_) { reply_ = Reply::Retry; DialogShell::accept(); }
}
void FileLockDialog::setBusy(bool Busy) {
    busy_ = Busy; refresh_->setEnabled(!Busy && current()); elevate_->setEnabled(!Busy && current());
    stop_->setVisible(Busy); AcceptButton()->setEnabled(!Busy && request_.canRetry_ && current());
    skip_->setEnabled(!Busy && current()); updateDetails();
}
void FileLockDialog::query(bool Elevated) {
    if(busy_ || !current())return;
    if(client_)client_->Cancel();
    client_=factory_({request_.source_},Elevated); elevated_=Elevated; requiresRefresh_=true;
    banner_->setTone(fm::ui::Banner::Info); banner_->setText(tr("파일을 사용하는 프로그램을 확인하고 있습니다."));
    summary_->setText(tr("조회 중 · 파일 작업은 응답 대기 상태로 유지됩니다."));
    setBusy(true);
    auto Session=client_; auto* Watcher=new QFutureWatcher<QJsonObject>(this);
    connect(Watcher,&QFutureWatcher<QJsonObject>::finished,this,[this,Watcher,Session]{
        const auto Result=Watcher->result(); Watcher->deleteLater(); if(client_!=Session)return;
        rows_=Result["rows"].toArray(); generation_=Result["generation"].toString(); table_->clear();
        QHash<QString,QTreeWidgetItem*> Groups;
        for(int I=0;I<rows_.size();++I){
            const auto Row=rows_[I].toObject(); const auto Key=QString::number(Row["pid"].toInt())+':'+Row["created"].toString();
            auto* Group=Groups.value(Key);
            if(!Group){Group=new QTreeWidgetItem(table_,{Row["name"].toString(),QString::number(Row["pid"].toInt()),Row["user"].toString(),tr("프로세스"),tr("사용 중")});
                Group->setData(0,Qt::UserRole,-1);Group->setData(0,Qt::UserRole+1,I);Groups.insert(Key,Group);}
            auto* Leaf=new QTreeWidgetItem(Group,{QFileInfo(Row["path"].toString()).fileName(),{}, {},referenceName(Row["kind"].toString()),Row["kind"]=="handle"?"0x"+Row["handle"].toString():tr("상세 미확인")});
            Leaf->setData(0,Qt::UserRole,I);Leaf->setData(0,Qt::UserRole+1,I);Leaf->setToolTip(0,Row["path"].toString());Group->setExpanded(true);
        }
        requiresRefresh_=Result["outcome"]!="Completed";
        queried_ = QDateTime::currentDateTimeUtc();
        const bool Partial=Result["partial"].toBool();
        banner_->setTone(requiresRefresh_||Partial||rows_.isEmpty()?fm::ui::Banner::Warn:fm::ui::Banner::Info);
        banner_->setText(requiresRefresh_?tr("조회를 완료하지 못했습니다. 권한·세션 상태를 확인하거나 다시 조회하세요. 오류 %1").arg(Result["nativeCode"].toString())
            :rows_.isEmpty()?tr("현재 권한으로 파일을 사용하는 프로그램을 찾지 못했습니다. 사용 중이 아니라고 단정할 수 없습니다.")
            :Partial?tr("일부 프로세스의 권한 또는 조회 제한으로 확인된 항목만 표시합니다.")
            :tr("먼저 해당 프로그램에서 파일을 닫으세요. 목록만으로 실제 차단 여부를 확정할 수 없습니다."));
        summary_->setText(tr("프로그램 %1개 · 참조 %2개%3").arg(Groups.size()).arg(rows_.size()).arg(Partial?tr(" · 일부 결과"):QString()));
        if(table_->topLevelItemCount())table_->setCurrentItem(table_->topLevelItem(0));
        setBusy(false);
    });
    Watcher->setFuture(QtConcurrent::run([Session]{return Session->Query();}));
}
void FileLockDialog::updateDetails() {
    auto* Item=table_->currentItem(); const int Index=Item?Item->data(0,Qt::UserRole+1).toInt():-1;
    const auto Row=Index>=0&&Index<rows_.size()?rows_[Index].toObject():QJsonObject();
    details_->clear();
    if(!Row.isEmpty()){
        details_->addRow(tr("프로그램"),Row["name"].toString(),fm::ui::KeyValueCard::Strong);
        details_->addRow(tr("대상 파일"),Row["path"].toString(),fm::ui::KeyValueCard::Mono);
        if(Item->data(0,Qt::UserRole).toInt()>=0&&Row["kind"]=="handle")details_->addRow(tr("핸들"),"0x"+Row["handle"].toString(),fm::ui::KeyValueCard::Mono);
        details_->addRow(tr("차단 여부"),tr("미확인"));
    }
    const bool Valid=!busy_&&!requiresRefresh_&&current()&&!Row.isEmpty();
    close_->setEnabled(Valid&&Item->data(0,Qt::UserRole).toInt()>=0&&Row["closeAllowed"].toBool());
    terminate_->setEnabled(Valid&&Row["terminateAllowed"].toBool());
    restriction_->setText(!Row["restriction"].toString().isEmpty()?tr("자체·시스템·보호 프로세스 또는 확인되지 않은 프로세스에는 강제 동작을 제공하지 않습니다.")
        :Item&&Item->data(0,Qt::UserRole).toInt()<0?tr("프로세스 행에서는 핸들을 임의로 선택하지 않습니다. 하위 파일 핸들을 선택하면 개별 닫기를 확인할 수 있습니다.")
        :tr("사용 중인 참조입니다. 목록만으로 삭제·이름 변경 차단 여부를 확정할 수 없습니다."));
}
void FileLockDialog::act(const QString& Action) {
    if(busy_||requiresRefresh_||!current()||!table_->currentItem())return;
    auto* Item=table_->currentItem(); const int Index=Item->data(0,Qt::UserRole+1).toInt();
    if(Index<0||Index>=rows_.size())return;
    const auto Row=rows_[Index].toObject();
    if(Action=="close"&&(Item->data(0,Qt::UserRole).toInt()<0||!Row["closeAllowed"].toBool()))return;
    if(Action=="terminate"&&!Row["terminateAllowed"].toBool())return;
    DialogShell Confirm(Action=="close"?tr("선택한 핸들 강제 닫기"):tr("프로세스 종료"),this);
    Confirm.setObjectName("hc.fileLocks.confirm"); Confirm.EnableCompactLayout(720);
    Confirm.SetSummary(tr("%1 · PID %2").arg(Row["name"].toString()).arg(Row["pid"].toInt()));
    Confirm.SetMessage(Action=="close"?tr("프로그램은 계속 실행되지만 읽기·쓰기 오류나 저장하지 않은 내용의 손실이 발생할 수 있습니다. 이 동작은 되돌릴 수 없습니다.")
        :tr("해당 프로그램 전체를 종료합니다. 다른 파일의 작업과 저장하지 않은 내용도 손실될 수 있습니다."),true);
    Confirm.BodyLayout()->addWidget(label(Row["path"].toString(),&Confirm));
    if(Action=="close")Confirm.BodyLayout()->addWidget(label(tr("선택 핸들: 0x%1").arg(Row["handle"].toString()),&Confirm));
    auto* Consent=new QCheckBox(tr("저장하지 않은 내용이 손실될 수 있음을 확인했습니다."),&Confirm);Consent->setObjectName("hc.fileLocks.riskConsent");
    Confirm.BodyLayout()->addWidget(Consent);
    Confirm.AcceptButton()->setText(Action=="close"?tr("핸들 강제 닫기(&H)"):tr("프로세스 종료(&E)"));
    Confirm.AcceptButton()->setRole(fm::ui::Button::Danger);Confirm.AcceptButton()->setAutoDefault(false);Confirm.AcceptButton()->setDefault(false);Confirm.SetAcceptEnabled(false);
    Confirm.CancelButton()->setDefault(true);Confirm.CancelButton()->setFocus();
    connect(Consent,&QCheckBox::toggled,&Confirm,[&Confirm](bool Checked){Confirm.SetAcceptEnabled(Checked);});
    if(Confirm.exec()!=QDialog::Accepted||!Consent->isChecked()||!current())return;
    const auto RequestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!permissions::AppendHistory({{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"operation",Action=="close"?tr("선택 핸들 강제 닫기 요청"):tr("프로세스 종료 요청")},{"target",Row["path"]},
        {"pid",Row["pid"]},{"created",Row["created"]},{"requestId",RequestId},{"outcome","Pending"},{"code",1},{"detail",tr("요청 후 결과 확인 전 상태입니다. 완료 또는 원상복구를 의미하지 않습니다.")}})) {
        banner_->setTone(fm::ui::Banner::Warn);banner_->setText(tr("동작 기록을 저장하지 못하여 실행하지 않았습니다."));return;
    }
    setBusy(true); stop_->hide(); requiresRefresh_=true;
    banner_->setTone(fm::ui::Banner::Info);banner_->setText(tr("선택한 동작을 처리하고 있습니다. 같은 요청을 다시 보내지 않습니다."));
    auto Session=client_;const auto Generation=generation_;
    auto* Watcher=new QFutureWatcher<QJsonObject>(this);
    connect(Watcher,&QFutureWatcher<QJsonObject>::finished,this,[this,Watcher,Row,Action,Session,RequestId]{
        const auto Result=Watcher->result();Watcher->deleteLater();if(client_!=Session)return;
        const auto Outcome=Result["outcome"].toString();
        banner_->setTone(Outcome=="Completed"?fm::ui::Banner::Ok:fm::ui::Banner::Warn);
        banner_->setText(Outcome=="Completed"?tr("선택한 동작 처리를 완료했습니다. 원래 파일 작업은 아직 재개하지 않았습니다. 다시 시도하거나 새로고침하세요.")
            :Outcome=="Unknown"?tr("처리 결과를 확정하지 못했습니다. 같은 강제 명령을 다시 보내지 않습니다. 새로고침으로 상태를 확인하세요.")
            :Outcome=="TargetChanged"?tr("프로세스 또는 핸들 대상이 바뀌어 실행하지 않았습니다. 새로고침하세요.")
            :Outcome=="AlreadyGone"?tr("대상이 이미 종료되었거나 핸들이 없어졌습니다. 현재 상태를 새로고침하세요.")
            :tr("선택한 동작을 실행하지 못했습니다. 오류 %1. 새로고침으로 현재 상태를 확인하세요.").arg(Result["nativeCode"].toString()));
        permissions::AppendHistory({{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"operation",Action=="close"?tr("선택 핸들 강제 닫기"):tr("프로세스 종료")},{"target",Row["path"]},{"pid",Row["pid"]},
            {"created",Row["created"]},{"requestId",RequestId},{"outcome",Outcome},{"stage",Result["stage"]},{"code",Outcome=="Completed"?0:1},{"nativeCode",Result["nativeCode"]},{"detail",Outcome}});
        setBusy(false);
    });
    Watcher->setFuture(QtConcurrent::run([Session,Row,Generation,Action,RequestId]{return Session->Act(Row["capability"].toString(),Generation,Action,RequestId);}));
}
}
